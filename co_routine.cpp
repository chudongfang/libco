/*
* Tencent is pleased to support the open source community by making Libco available.
* Copyright (C) 2014 THL A29 Limited, a Tencent company. All rights reserved.
*
* Licensed under the Apache License, Version 2.0 (the "License"); 
* you may not use this file except in compliance with the License. 
* You may obtain a copy of the License at
*
*	http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, 
* software distributed under the License is distributed on an "AS IS" BASIS, 
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. 
* See the License for the specific language governing permissions and 
* limitations under the License.
*/
#include "co_routine.h"
#include "co_routine_inner.h"
#include "co_epoll.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <string>
#include <map>
#include <poll.h>
#include <sys/time.h>
#include <errno.h>
#include <assert.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <limits.h>
extern "C"
{
	extern void coctx_swap( coctx_t *,coctx_t* ) asm("coctx_swap");
};
using namespace std;
stCoRoutine_t *GetCurrCo( stCoRoutineEnv_t *env );
struct stCoEpoll_t;
struct stCoRoutineEnv_t
{
	stCoRoutine_t *pCallStack[ 128 ]; //保存调用链
	int iCallStackSize;               //栈指针
	stCoEpoll_t *pEpoll;
	//for copy stack log lastco and nextco //共享栈
	stCoRoutine_t* pending_co;
	stCoRoutine_t* occupy_co;
};
//int socket(int domain, int type, int protocol);
void co_log_err( const char *fmt,... )
{
}
#if defined( __LIBCO_RDTSCP__) 
static unsigned long long counter(void)
{
	register uint32_t lo, hi;
	register unsigned long long o;
	__asm__ __volatile__ (
			"rdtscp" : "=a"(lo), "=d"(hi)::"%rcx"
			);
	o = hi;
	o <<= 32;
	return (o | lo);
}
static unsigned long long getCpuKhz()
{
	FILE *fp = fopen("/proc/cpuinfo","r");
	if(!fp) return 1;
	char buf[4096] = {0};
	fread(buf,1,sizeof(buf),fp);
	fclose(fp);
	char *lp = strstr(buf,"cpu MHz");
	if(!lp) return 1;
	lp += strlen("cpu MHz");
	while(*lp == ' ' || *lp == '\t' || *lp == ':')
	{
		++lp;
	}
	double mhz = atof(lp);
	unsigned long long u = (unsigned long long)(mhz * 1000);
	return u;
}
#endif
//得到当前时间
static unsigned long long GetTickMS()
{
#if defined( __LIBCO_RDTSCP__) 
	static uint32_t khz = getCpuKhz();
	return counter() / khz;
#else
	struct timeval now = { 0 };
	gettimeofday( &now,NULL );
	unsigned long long u = now.tv_sec;
	u *= 1000;
	u += now.tv_usec / 1000;
	return u;
#endif
}
static pid_t GetPid()
{
    static __thread pid_t pid = 0;
    static __thread pid_t tid = 0;
    if( !pid || !tid || pid != getpid() )
    {
        pid = getpid();
#if defined( __APPLE__ )
		tid = syscall( SYS_gettid );
		if( -1 == (long)tid )
		{
			tid = pid;
		}
#elif defined( __FreeBSD__ )
		syscall(SYS_thr_self, &tid);
		if( tid < 0 )
		{
			tid = pid;
		}
#else 
        tid = syscall( __NR_gettid );
#endif
    }
    return tid;
}
/*
static pid_t GetPid()
{
	char **p = (char**)pthread_self();
	return p ? *(pid_t*)(p + 18) : getpid();
}
*/
//----------------------------一些链表操作函数---------------------------------------------------
//从链表中移除节本点
//链表信息存储在该节点中
template <class T,class TLink>
void RemoveFromLink(T *ap)
{
	TLink *lst = ap->pLink;
	if(!lst) return ;
	assert( lst->head && lst->tail );
	if( ap == lst->head )
	{
		lst->head = ap->pNext;
		if(lst->head)
		{
			lst->head->pPrev = NULL;
		}
	}
	else
	{
		if(ap->pPrev)
		{
			ap->pPrev->pNext = ap->pNext;
		}
	}
	if( ap == lst->tail )
	{
		lst->tail = ap->pPrev;
		if(lst->tail)
		{
			lst->tail->pNext = NULL;
		}
	}
	else
	{
		ap->pNext->pPrev = ap->pPrev;
	}
	ap->pPrev = ap->pNext = NULL;
	ap->pLink = NULL;
}
//把ap 加到aplink链表中
template <class TNode,class TLink>
void inline AddTail(TLink*apLink,TNode *ap)
{
	if( ap->pLink )
	{
		return ;
	}
	if(apLink->tail)
	{
		apLink->tail->pNext = (TNode*)ap;
		ap->pNext = NULL;
		ap->pPrev = apLink->tail;
		apLink->tail = ap;
	}
	else
	{
		apLink->head = apLink->tail = ap;
		ap->pNext = ap->pPrev = NULL;
	}
	ap->pLink = apLink;
}
//删除链表头结点
template <class TNode,class TLink>
void inline PopHead( TLink*apLink )
{
	if( !apLink->head ) 
	{
		return ;
	}
	TNode *lp = apLink->head;
	if( apLink->head == apLink->tail )
	{
		apLink->head = apLink->tail = NULL;
	}
	else
	{
		apLink->head = apLink->head->pNext;
	}
	lp->pPrev = lp->pNext = NULL;
	lp->pLink = NULL;
	if( apLink->head )
	{
		apLink->head->pPrev = NULL;
	}
}
//把apOther加入到apLink中
template <class TNode,class TLink>
void inline Join( TLink*apLink,TLink *apOther )
{
	//printf("apOther %p\n",apOther);
	if( !apOther->head )
	{
		return ;
	}
	TNode *lp = apOther->head;
	while( lp )
	{
		lp->pLink = apLink;
		lp = lp->pNext;
	}
	lp = apOther->head;
	if(apLink->tail)
	{
		apLink->tail->pNext = (TNode*)lp;
		lp->pPrev = apLink->tail;
		apLink->tail = apOther->tail;
	}
	else
	{
		apLink->head = apOther->head;
		apLink->tail = apOther->tail;
	}
	apOther->head = apOther->tail = NULL;
}
/////////////////for copy stack //////////////////////////
stStackMem_t* co_alloc_stackmem(unsigned int stack_size)
{
	stStackMem_t* stack_mem = (stStackMem_t*)malloc(sizeof(stStackMem_t));
	stack_mem->occupy_co= NULL;
	stack_mem->stack_size = stack_size;
	stack_mem->stack_buffer = (char*)malloc(stack_size);
	stack_mem->stack_bp = stack_mem->stack_buffer + stack_size;
	return stack_mem;
}
stShareStack_t* co_alloc_sharestack(int count, int stack_size)
{
	stShareStack_t* share_stack = (stShareStack_t*)malloc(sizeof(stShareStack_t));
	share_stack->alloc_idx = 0;
	share_stack->stack_size = stack_size;
	//alloc stack array
	share_stack->count = count;
	stStackMem_t** stack_array = (stStackMem_t**)calloc(count, sizeof(stStackMem_t*));
	for (int i = 0; i < count; i++)
	{
		stack_array[i] = co_alloc_stackmem(stack_size);
	}
	share_stack->stack_array = stack_array;
	return share_stack;
}
static stStackMem_t* co_get_stackmem(stShareStack_t* share_stack)
{
	if (!share_stack)
	{
		return NULL;
	}
	int idx = share_stack->alloc_idx % share_stack->count;
	share_stack->alloc_idx++;
	return share_stack->stack_array[idx];
}
// ----------------------------------------------------------------------------
struct stTimeoutItemLink_t;
struct stTimeoutItem_t;
struct stCoEpoll_t
{
	int iEpollFd; //epoll 实例的文件描述符
	static const int _EPOLL_SIZE = 1024 * 10;//一次 epoll_wait 最多返回的就绪事件个数
	struct stTimeout_t *pTimeout;//时间轮(Timingwheel)定时器
	struct stTimeoutItemLink_t *pstTimeoutList;//该指针实际上是一个链表头。链表用于临时存放超时事件的 item
	struct stTimeoutItemLink_t *pstActiveList;//也是指向一个链表。该链表用于存放 epoll_wait 得到的就绪事件和定时器超时事件
	co_epoll_res *result;  //第二个参数的封装,即一次 epoll_wait 得到的结果集
};
//----------------------------------时间轮-------------------------
typedef void (*OnPreparePfn_t)( stTimeoutItem_t *,struct epoll_event &ev, stTimeoutItemLink_t *active );
typedef void (*OnProcessPfn_t)( stTimeoutItem_t *);
//双向链表块
//表示一个定时事件
struct stTimeoutItem_t
{
	enum
	{
		eMaxTimeout = 40 * 1000 //40s
	};
	stTimeoutItem_t *pPrev;//指向前一个节点
	stTimeoutItem_t *pNext;//指向后一个节点
	stTimeoutItemLink_t *pLink;//指向链表头节点和链表尾节点
    //定时事件到期时间
	unsigned long long ullExpireTime;
	OnPreparePfn_t pfnPrepare;
	OnProcessPfn_t pfnProcess;
	void *pArg; // routine  指向一个协程块 
	bool bTimeout; //是否超时
};
struct stTimeoutItemLink_t
{
	stTimeoutItem_t *head;
	stTimeoutItem_t *tail;
};
//封装一个双向链表
//表示一系列超时事件
struct stTimeout_t
{
    //超时事件的链表
	stTimeoutItemLink_t *pItems;
	//链表大小
    int iItemSize;
    //开始时间
	unsigned long long ullStart;
    //第一个放入的链表ID,其是环形的
    long long llStartIdx;
};
//创建一个固定大小的时间轮
stTimeout_t *AllocTimeout( int iSize )
{
	stTimeout_t *lp = (stTimeout_t*)calloc( 1,sizeof(stTimeout_t) );	
	lp->iItemSize = iSize;
	lp->pItems = (stTimeoutItemLink_t*)calloc( 1,sizeof(stTimeoutItemLink_t) * lp->iItemSize );
	lp->ullStart = GetTickMS();
	lp->llStartIdx = 0;
	return lp;
}
//释放一个时间轮
void FreeTimeout( stTimeout_t *apTimeout )
{
	free( apTimeout->pItems );
	free ( apTimeout );
}
//把定时事件加入时间轮
int AddTimeout( stTimeout_t *apTimeout,stTimeoutItem_t *apItem ,unsigned long long allNow )
{
    //如果时间轮为空,初始化其开始时间
	if( apTimeout->ullStart == 0 )
	{
		apTimeout->ullStart = allNow;
		apTimeout->llStartIdx = 0;
	}
	if( allNow < apTimeout->ullStart )
	{
		co_log_err("CO_ERR: AddTimeout line %d allNow %llu apTimeout->ullStart %llu",
					__LINE__,allNow,apTimeout->ullStart);
		return __LINE__;
	}
	if( apItem->ullExpireTime < allNow )
	{
		co_log_err("CO_ERR: AddTimeout line %d apItem->ullExpireTime %llu allNow %llu apTimeout->ullStart %llu",
					__LINE__,apItem->ullExpireTime,allNow,apTimeout->ullStart);
		return __LINE__;
	}
	unsigned long long diff = apItem->ullExpireTime - apTimeout->ullStart;
    //两者相差的时间
    //决定其放入时间轮的链表位置
	if( diff >= (unsigned long long)apTimeout->iItemSize )
	{
		diff = apTimeout->iItemSize - 1;
		co_log_err("CO_ERR: AddTimeout line %d diff %d",
					__LINE__,diff);
		//return __LINE__;
	}
    //加入时间轮
	AddTail( apTimeout->pItems + ( apTimeout->llStartIdx + diff ) % apTimeout->iItemSize , apItem );
	return 0;
}
inline void TakeAllTimeout( stTimeout_t *apTimeout,unsigned long long allNow,stTimeoutItemLink_t *apResult )
{
    //如果时间轮为空,初始化其开始时间
	if( apTimeout->ullStart == 0 )
	{
		apTimeout->ullStart = allNow;
		apTimeout->llStartIdx = 0;
	}
	if( allNow < apTimeout->ullStart )
	{
		return ;
	}
    //确定当前时间和事件轮开始事件的差值
	int cnt = allNow - apTimeout->ullStart + 1;
	if( cnt > apTimeout->iItemSize )
	{
		cnt = apTimeout->iItemSize;
	}
	if( cnt < 0 )
	{
		return;
	}
	for( int i = 0;i<cnt;i++)
	{
        //挨个访问时间轮位置,并把其中的事件全部加入活动时间中
		int idx = ( apTimeout->llStartIdx + i) % apTimeout->iItemSize;
		Join<stTimeoutItem_t,stTimeoutItemLink_t>( apResult,apTimeout->pItems + idx  );
	}
    //初始化时间轮的开始时间和其开始ID
	apTimeout->ullStart = allNow;
	apTimeout->llStartIdx += cnt - 1;
}
//-----------------------------------------------------------------
//执行协程中的函数
static int CoRoutineFunc( stCoRoutine_t *co,void * )
{
	if( co->pfn )
	{
		co->pfn( co->arg );
	}
	co->cEnd = 1;
	stCoRoutineEnv_t *env = co->env;
	co_yield_env( env );
	return 0;
}
//--------------------------------------------------------------------
//创建协程块
//对协程块进行一些初始化工作
struct stCoRoutine_t *co_create_env( stCoRoutineEnv_t * env, const stCoRoutineAttr_t* attr,
		pfn_co_routine_t pfn,void *arg )
{
	stCoRoutineAttr_t at;
	if( attr )
	{
		memcpy( &at,attr,sizeof(at) );
	}
	if( at.stack_size <= 0 )
	{
		at.stack_size = 128 * 1024;
	}
	else if( at.stack_size > 1024 * 1024 * 8 )
	{
		at.stack_size = 1024 * 1024 * 8;
	}
	if( at.stack_size & 0xFFF ) 
	{
		at.stack_size &= ~0xFFF;
		at.stack_size += 0x1000;
	}
	stCoRoutine_t *lp = (stCoRoutine_t*)malloc( sizeof(stCoRoutine_t) );
	memset( lp,0,(long)(sizeof(stCoRoutine_t))); 
	lp->env = env;
	lp->pfn = pfn;
	lp->arg = arg;
	stStackMem_t* stack_mem = NULL;
	if( at.share_stack )
	{
		stack_mem = co_get_stackmem( at.share_stack);
		at.stack_size = at.share_stack->stack_size;
	}
	else
	{
		stack_mem = co_alloc_stackmem(at.stack_size);
	}
	lp->stack_mem = stack_mem;
	lp->ctx.ss_sp = stack_mem->stack_buffer;
	lp->ctx.ss_size = at.stack_size;
	lp->cStart = 0;
	lp->cEnd = 0;
	lp->cIsMain = 0;
	lp->cEnableSysHook = 0;
	lp->cIsShareStack = at.share_stack != NULL;
	lp->save_size = 0;
	lp->save_buffer = NULL;
	return lp;
}
//创建协程块
int co_create( stCoRoutine_t **ppco,const stCoRoutineAttr_t *attr,pfn_co_routine_t pfn,void *arg )
{
	if( !co_get_curr_thread_env() ) 
	{
		co_init_curr_thread_env();
	}
	stCoRoutine_t *co = co_create_env( co_get_curr_thread_env(), attr, pfn,arg );
	*ppco = co;
	return 0;
}
//释放协程块
void co_free( stCoRoutine_t *co )
{
    if (!co->cIsShareStack) 
    {    
        free(co->stack_mem->stack_buffer);
        free(co->stack_mem);
    }   
    free( co );
}
//释放协程块
void co_release( stCoRoutine_t *co )
{
    co_free( co );
}
//切换两个协程执行
void co_swap(stCoRoutine_t* curr, stCoRoutine_t* pending_co);
//执行协程
void co_resume( stCoRoutine_t *co )
{
	stCoRoutineEnv_t *env = co->env;
	stCoRoutine_t *lpCurrRoutine = env->pCallStack[ env->iCallStackSize - 1 ]; //取当前协程块指针
	if( !co->cStart ) //协程首次执行
	{
		coctx_make( &co->ctx,(coctx_pfn_t)CoRoutineFunc,co,0 );
		co->cStart = 1;
	}
	env->pCallStack[ env->iCallStackSize++ ] = co;//将新协程块指针压入pCallStack栈中
	co_swap( lpCurrRoutine, co );                 //切换到co指向的新协程去执行
    //阻塞,并行执行
    //co_swap() 不会就此返回,而是要这次 resume 的 co 协程主动
    //yield 让出 CPU 时才会返回到 co_resume() 中来
}
//退出当前协程
void co_yield_env( stCoRoutineEnv_t *env )
{
	stCoRoutine_t *last = env->pCallStack[ env->iCallStackSize - 2 ]; //获取上一个协程块
	stCoRoutine_t *curr = env->pCallStack[ env->iCallStackSize - 1 ]; //获取当前协程块
	env->iCallStackSize--;
	co_swap( curr, last);
}
//退出当前线程中的协程
void co_yield_ct()
{
	co_yield_env( co_get_curr_thread_env() );
}
//退出当前协程
void co_yield( stCoRoutine_t *co )
{
	co_yield_env( co->env );
}
void save_stack_buffer(stCoRoutine_t* occupy_co)
{
	///copy out
	stStackMem_t* stack_mem = occupy_co->stack_mem;
	int len = stack_mem->stack_bp - occupy_co->stack_sp;
	if (occupy_co->save_buffer)
	{
		free(occupy_co->save_buffer), occupy_co->save_buffer = NULL;
	}
	occupy_co->save_buffer = (char*)malloc(len); //malloc buf;
	occupy_co->save_size = len;
	memcpy(occupy_co->save_buffer, occupy_co->stack_sp, len);
}
//交换执行两个协程
void co_swap(stCoRoutine_t* curr, stCoRoutine_t* pending_co)
{
 	stCoRoutineEnv_t* env = co_get_curr_thread_env();
	//get curr stack sp
	char c;
	curr->stack_sp= &c;
	if (!pending_co->cIsShareStack)
	{
		env->pending_co = NULL;
		env->occupy_co = NULL;
	}
	else 
	{
		env->pending_co = pending_co;
		//get last occupy co on the same stack mem
		stCoRoutine_t* occupy_co = pending_co->stack_mem->occupy_co;
		//set pending co to occupy thest stack mem;
		pending_co->stack_mem->occupy_co = pending_co;
		env->occupy_co = occupy_co;
		if (occupy_co && occupy_co != pending_co)
		{
			save_stack_buffer(occupy_co);
		}
	}
	//swap context
	coctx_swap(&(curr->ctx),&(pending_co->ctx) );
	//stack buffer may be overwrite, so get again;
	stCoRoutineEnv_t* curr_env = co_get_curr_thread_env();
	stCoRoutine_t* update_occupy_co =  curr_env->occupy_co;
	stCoRoutine_t* update_pending_co = curr_env->pending_co;
	if (update_occupy_co && update_pending_co && update_occupy_co != update_pending_co)
	{
		//resume stack buffer
		if (update_pending_co->save_buffer && update_pending_co->save_size > 0)
		{
			memcpy(update_pending_co->stack_sp, update_pending_co->save_buffer, update_pending_co->save_size);
		}
	}
}
//int poll(struct pollfd fds[], nfds_t nfds, int timeout);
// { fd,events,revents }
//继承定时事件结构体
//可以看做为一个定时事件
struct stPollItem_t ;
struct stPoll_t : public stTimeoutItem_t 
{
	struct pollfd *fds;
	nfds_t nfds; // typedef unsigned long int nfds_t;
	stPollItem_t *pPollItems;
	int iAllEventDetach;
	int iEpollFd;
	int iRaiseCnt;
};
struct stPollItem_t : public stTimeoutItem_t
{
	struct pollfd *pSelf;
	stPoll_t *pPoll;
	struct epoll_event stEvent;
};
/*
 *   EPOLLPRI 		POLLPRI    // There is urgent data to read.  
 *   EPOLLMSG 		POLLMSG
 *
 *   				POLLREMOVE
 *   				POLLRDHUP
 *   				POLLNVAL
 *
 * */
static uint32_t PollEvent2Epoll( short events )
{
	uint32_t e = 0;	
	if( events & POLLIN ) 	e |= EPOLLIN;
	if( events & POLLOUT )  e |= EPOLLOUT;
	if( events & POLLHUP ) 	e |= EPOLLHUP;
	if( events & POLLERR )	e |= EPOLLERR;
	if( events & POLLRDNORM ) e |= EPOLLRDNORM;
	if( events & POLLWRNORM ) e |= EPOLLWRNORM;
	return e;
}
static short EpollEvent2Poll( uint32_t events )
{
	short e = 0;	
	if( events & EPOLLIN ) 	e |= POLLIN;
	if( events & EPOLLOUT ) e |= POLLOUT;
	if( events & EPOLLHUP ) e |= POLLHUP;
	if( events & EPOLLERR ) e |= POLLERR;
	if( events & EPOLLRDNORM ) e |= POLLRDNORM;
	if( events & EPOLLWRNORM ) e |= POLLWRNORM;
	return e;
}
//全局变量,存储不同线程的的协程状态
static stCoRoutineEnv_t* g_arrCoEnvPerThread[ 204800 ] = { 0 };
//初始化当前线程的协程环境
void co_init_curr_thread_env()
{
	pid_t pid = GetPid();	
	g_arrCoEnvPerThread[ pid ] = (stCoRoutineEnv_t*)calloc( 1,sizeof(stCoRoutineEnv_t) );
	stCoRoutineEnv_t *env = g_arrCoEnvPerThread[ pid ];
	env->iCallStackSize = 0;
	struct stCoRoutine_t *self = co_create_env( env, NULL, NULL,NULL );
	self->cIsMain = 1;
	env->pending_co = NULL;
	env->occupy_co = NULL;
	coctx_init( &self->ctx );
	env->pCallStack[ env->iCallStackSize++ ] = self;
	stCoEpoll_t *ev = AllocEpoll();
	SetEpoll( env,ev );
}
//得到当前线程的协程环境
stCoRoutineEnv_t *co_get_curr_thread_env()
{
	return g_arrCoEnvPerThread[ GetPid() ];
}
//启动超时协程
void OnPollProcessEvent( stTimeoutItem_t * ap )
{
	stCoRoutine_t *co = (stCoRoutine_t*)ap->pArg;
	co_resume( co );
}
void OnPollPreparePfn( stTimeoutItem_t * ap,struct epoll_event &e,stTimeoutItemLink_t *active )
{

    //把epoll的返回状态转换程poll的返回状态
	stPollItem_t *lp = (stPollItem_t *)ap;
	lp->pSelf->revents = EpollEvent2Poll( e.events );


    //pPoll为定时器
    stPoll_t *pPoll = lp->pPoll;
	pPoll->iRaiseCnt++;
    
    //只要出现事件，就立马返回协程处理
	if( !pPoll->iAllEventDetach )
	{
		pPoll->iAllEventDetach = 1;
		RemoveFromLink<stTimeoutItem_t,stTimeoutItemLink_t>( pPoll );
		AddTail( active,pPoll );
	}
}
//EvenLoop为协程的调度器,类比与线程,线程调度由操作系统的内核实现
//而协程的调度则由eventloop实现
void co_eventloop( stCoEpoll_t *ctx,pfn_co_eventloop_t pfn,void *arg )
{
	if( !ctx->result )
	{
		ctx->result =  co_epoll_res_alloc( stCoEpoll_t::_EPOLL_SIZE );
	}
    //co_epoll_res epoll的返回结果
	co_epoll_res *result = ctx->result;
	for(;;)
	{
        //其相当与直接用epoll实现的定时器
        //一个1ms超时的短时间blocking调用
		int ret = co_epoll_wait( ctx->iEpollFd,result,stCoEpoll_t::_EPOLL_SIZE, 1 );//调用 epoll_wait() 等待 I/O 就绪事件

        //有活动的事件链表
		stTimeoutItemLink_t *active = (ctx->pstActiveList);

        //超时链表
        stTimeoutItemLink_t *timeout = (ctx->pstTimeoutList);
		memset( timeout,0,sizeof(stTimeoutItemLink_t) );

		for(int i=0;i<ret;i++)//处理就绪的文件描述符
		{
			stTimeoutItem_t *item = (stTimeoutItem_t*)result->events[i].data.ptr;
			if( item->pfnPrepare )
			{
				item->pfnPrepare( item,result->events[i],active );
			}
			else
			{
				AddTail( active,item );
			}
		}
		unsigned long long now = GetTickMS();
		TakeAllTimeout( ctx->pTimeout,now,timeout );//从时间轮上取出已超时的事件,放到 timeout 队列
        //遍历 timeout 队列,设置事件已超时标志
		stTimeoutItem_t *lp = timeout->head;
		while( lp )
		{
			//printf("raise timeout %p\n",lp);
			lp->bTimeout = true;
			lp = lp->pNext;
		}
        //将 timeout 队列中事件合并到 active 队列
		Join<stTimeoutItem_t,stTimeoutItemLink_t>( active,timeout );
        //遍历 active 队列,调用工作协程设置的 pfnProcess() 回调函数 resume
        //挂起的工作协程,处理对应的 I/O 或超时事件
		lp = active->head;
		while( lp )
		{
			PopHead<stTimeoutItem_t,stTimeoutItemLink_t>( active );
            if (lp->bTimeout && now < lp->ullExpireTime) 
			{
				int ret = AddTimeout(ctx->pTimeout, lp, now);
				if (!ret) 
				{
					lp->bTimeout = false;
					lp = active->head;
					continue;
				}
			}
			if( lp->pfnProcess )
			{
				lp->pfnProcess( lp );
			}
			lp = active->head;
		}
        //执行在EventLoop中执行的函数
		if( pfn )
		{
			if( -1 == pfn( arg ) )
			{
				break;
			}
		}
	}
}
//唤醒ap中的协程
void OnCoroutineEvent( stTimeoutItem_t * ap )
{
	stCoRoutine_t *co = (stCoRoutine_t*)ap->pArg;
	co_resume( co );
}
//创建epoll并分配对应的stCoEpoll结构体
stCoEpoll_t *AllocEpoll()
{
	stCoEpoll_t *ctx = (stCoEpoll_t*)calloc( 1,sizeof(stCoEpoll_t) );
	ctx->iEpollFd = co_epoll_create( stCoEpoll_t::_EPOLL_SIZE );
	ctx->pTimeout = AllocTimeout( 60 * 1000 );
	ctx->pstActiveList = (stTimeoutItemLink_t*)calloc( 1,sizeof(stTimeoutItemLink_t) );
	ctx->pstTimeoutList = (stTimeoutItemLink_t*)calloc( 1,sizeof(stTimeoutItemLink_t) );
	return ctx;
}
//删除epoll对应的节点
void FreeEpoll( stCoEpoll_t *ctx )
{
	if( ctx )
	{
		free( ctx->pstActiveList );
		free( ctx->pstTimeoutList );
		FreeTimeout( ctx->pTimeout );
		co_epoll_res_free( ctx->result );
	}
	free( ctx );
}
//得到当前执行的协程块
stCoRoutine_t *GetCurrCo( stCoRoutineEnv_t *env )
{
	return env->pCallStack[ env->iCallStackSize - 1 ];
}
//得到当前执行的协程块
stCoRoutine_t *GetCurrThreadCo( )
{
	stCoRoutineEnv_t *env = co_get_curr_thread_env();
	if( !env ) return 0;
	return GetCurrCo(env);
}
//这是一个非常重要的函数
//功能:
// 1.其把当前协程块唤醒事件加入时间轮,并进行定时,当其到达时间后便,唤醒本协程
// 2.其也提供了socket的监听功能,epoll
//其供co_poll/poll调用
//其内部调用epoll监听
typedef int (*poll_pfn_t)(struct pollfd fds[], nfds_t nfds, int timeout);
int co_poll_inner( stCoEpoll_t *ctx,struct pollfd fds[], nfds_t nfds, int timeout, poll_pfn_t pollfunc)
{
    if (timeout == 0)
	{
		return pollfunc(fds, nfds, timeout);
	}
    //timeout小于0时,其就相当与设置为无限等待
	if (timeout < 0)
	{
		timeout = INT_MAX;
	}


	int epfd = ctx->iEpollFd;
    //当前的协程块
	stCoRoutine_t* self = co_self();

	//1.struct change
	stPoll_t& arg = *((stPoll_t*)malloc(sizeof(stPoll_t)));
	memset( &arg,0,sizeof(arg) );
	arg.iEpollFd = epfd;
	arg.fds = (pollfd*)calloc(nfds, sizeof(pollfd));
	arg.nfds = nfds;
	
    stPollItem_t arr[2];
	if( nfds < sizeof(arr) / sizeof(arr[0]) && !self->cIsShareStack)
	{
		arg.pPollItems = arr;
	}	
	else
	{
		arg.pPollItems = (stPollItem_t*)malloc( nfds * sizeof( stPollItem_t ) );
	}
	memset( arg.pPollItems,0,nfds * sizeof(stPollItem_t) );
    
    //赋值执行函数,该函数会启动pArg表示的协程
	arg.pfnProcess = OnPollProcessEvent;
    //得到当前线程的协程环境
    arg.pArg = GetCurrCo( co_get_curr_thread_env() );
    
    
    //把要监听的socket加入epoll,让epoll监听	
    //2. add epoll
	for(nfds_t i=0;i<nfds;i++)
	{
		arg.pPollItems[i].pSelf = arg.fds + i;
		arg.pPollItems[i].pPoll = &arg;
		arg.pPollItems[i].pfnPrepare = OnPollPreparePfn;
		struct epoll_event &ev = arg.pPollItems[i].stEvent;
		if( fds[i].fd > -1 )
		{
			ev.data.ptr = arg.pPollItems + i;
			ev.events = PollEvent2Epoll( fds[i].events );
			int ret = co_epoll_ctl( epfd,EPOLL_CTL_ADD, fds[i].fd, &ev );
		    

            
            //目标fd不支持epoll 在非linux平台上运行
            if (ret < 0 && errno == EPERM && nfds == 1 && pollfunc != NULL)
			{
				if( arg.pPollItems != arr )
				{
					free( arg.pPollItems );
					arg.pPollItems = NULL;
				}
				free(arg.fds);
				free(&arg);
				return pollfunc(fds, nfds, timeout);
			}

		}
		//if fail,the timeout would work
	}


	//3.add timeout
	unsigned long long now = GetTickMS();
	arg.ullExpireTime = now + timeout;
    //这里arg会被强转成stTimeoutItem_t*类型,因为stPoll_t继承stTimeoutItem_t
    int ret = AddTimeout( ctx->pTimeout,&arg,now );
	int iRaiseCnt = 0;

	if( ret != 0 )
	{
		co_log_err("CO_ERR: AddTimeout ret %d now %lld timeout %d arg.ullExpireTime %lld",
				ret,now,timeout,arg.ullExpireTime);
		errno = EINVAL;
		iRaiseCnt = -1;
	}
    else
	{
        //成功加入时间轮后,其就会退出当期协程
       // printf("-----------------------------------------------\n");
       // printf("Poll 定时事件已经加入时间轮 , 将要退出本协程!\n");
       // printf("-----------------------------------------------\n\n\n");
		co_yield_env( co_get_curr_thread_env() );
		iRaiseCnt = arg.iRaiseCnt;
	}
    
    //在当前协程被重新唤醒后,说明定时已经到时,此时对epoll进行复原处理
    //清空epoll的状态
    {
		//clear epoll status and memory
		RemoveFromLink<stTimeoutItem_t,stTimeoutItemLink_t>( &arg );
		for(nfds_t i = 0;i < nfds;i++)
		{
			int fd = fds[i].fd;
			if( fd > -1 )
			{
				co_epoll_ctl( epfd,EPOLL_CTL_DEL,fd,&arg.pPollItems[i].stEvent );
			}
			fds[i].revents = arg.fds[i].revents;
            //返回相应事件revents
		}
		if( arg.pPollItems != arr )
		{
			free( arg.pPollItems );
			arg.pPollItems = NULL;
		}
		free(arg.fds);
		free(&arg);
	}
	return iRaiseCnt;
}




int	co_poll( stCoEpoll_t *ctx,struct pollfd fds[], nfds_t nfds, int timeout_ms )
{
	return co_poll_inner(ctx, fds, nfds, timeout_ms, NULL);
}
void SetEpoll( stCoRoutineEnv_t *env,stCoEpoll_t *ev )
{
	env->pEpoll = ev;
}
stCoEpoll_t *co_get_epoll_ct()
{
	if( !co_get_curr_thread_env() )
	{
		co_init_curr_thread_env();
	}
	return co_get_curr_thread_env()->pEpoll;
}
struct stHookPThreadSpec_t
{
	stCoRoutine_t *co;
	void *value;
	enum 
	{
		size = 1024
	};
};
void *co_getspecific(pthread_key_t key)
{
	stCoRoutine_t *co = GetCurrThreadCo();
	if( !co || co->cIsMain )
	{
		return pthread_getspecific( key );
	}
	return co->aSpec[ key ].value;
}
int co_setspecific(pthread_key_t key, const void *value)
{
	stCoRoutine_t *co = GetCurrThreadCo();
	if( !co || co->cIsMain )
	{
		return pthread_setspecific( key,value );
	}
	co->aSpec[ key ].value = (void*)value;
	return 0;
}
void co_disable_hook_sys()
{
	stCoRoutine_t *co = GetCurrThreadCo();
	if( co )
	{
		co->cEnableSysHook = 0;
	}
}
bool co_is_enable_sys_hook()
{
	stCoRoutine_t *co = GetCurrThreadCo();
	return ( co && co->cEnableSysHook );
}
stCoRoutine_t *co_self()
{
	return GetCurrThreadCo();
}
//-----------------------------------------------------------------------
//co cond
//
//条件变量
//每个条件变量节点里面都包含一个stTimeoutItem_t
struct stCoCond_t;
struct stCoCondItem_t 
{
	stCoCondItem_t *pPrev;
	stCoCondItem_t *pNext;
	stCoCond_t *pLink;
	stTimeoutItem_t timeout; //一个定时事件
};
//表示条件变量的列表
struct stCoCond_t
{
	stCoCondItem_t *head;
	stCoCondItem_t *tail;
};
//唤醒当前的协程块
static void OnSignalProcessEvent( stTimeoutItem_t * ap )
{
	stCoRoutine_t *co = (stCoRoutine_t*)ap->pArg;
	co_resume( co );
}
stCoCondItem_t *co_cond_pop( stCoCond_t *link );
//把条件变量取出,并把其中的定时事件删除,移入活动事件中 
int co_cond_signal( stCoCond_t *si )
{
	stCoCondItem_t * sp = co_cond_pop( si );
	if( !sp ) 
	{
		return 0;
	}
    //把定时事件从时间轮中删除
	RemoveFromLink<stTimeoutItem_t,stTimeoutItemLink_t>( &sp->timeout );
    //直接加入到活动事件中
	AddTail( co_get_curr_thread_env()->pEpoll->pstActiveList,&sp->timeout );
    /*sleep(1);
    printf("co_cond_signal:退出当前协程\n");
    co_yield_ct();*/ 
	return 0;
}
int co_cond_broadcast( stCoCond_t *si )
{
	for(;;)
	{
		stCoCondItem_t * sp = co_cond_pop( si );
		if( !sp ) return 0;
		RemoveFromLink<stTimeoutItem_t,stTimeoutItemLink_t>( &sp->timeout );
		AddTail( co_get_curr_thread_env()->pEpoll->pstActiveList,&sp->timeout );
	}
	return 0;
}
//创建一个条件变量,
//并把定时事件加入时间轮
//退出当前协程
//其事件时间到时会返回当前协程
//在返回该协程时删除条件变量
int co_cond_timedwait( stCoCond_t *link,int ms )
{
    //创建一个条件变量
	stCoCondItem_t* psi = (stCoCondItem_t*)calloc(1, sizeof(stCoCondItem_t));
	psi->timeout.pArg = GetCurrThreadCo();  //保存当前的协程
	psi->timeout.pfnProcess = OnSignalProcessEvent;//协程函数,该函数会唤醒当前协程
	if( ms > 0 )
	{
        //设置超时时间
		unsigned long long now = GetTickMS();
		psi->timeout.ullExpireTime = now + ms;
        //把定时事件加入时间轮
		int ret = AddTimeout( co_get_curr_thread_env()->pEpoll->pTimeout,&psi->timeout,now );
		if( ret != 0 )
		{
			free(psi);
			return ret;
		}
	}
    //把条件变量加入到链表中
	AddTail( link, psi);
    printf("退出当前协程\n");
    //退出执行当前协程
	co_yield_ct();
    //---------------执行分割线----------------
    //当协程再次执行时,会移走条件变量
	RemoveFromLink<stCoCondItem_t,stCoCond_t>( psi );
	free(psi);
	return 0;
}
//创建一个条件变量
stCoCond_t *co_cond_alloc()
{
	return (stCoCond_t*)calloc( 1,sizeof(stCoCond_t) );
}
//释放一个条件变量
int co_cond_free( stCoCond_t * cc )
{
	free( cc );
	return 0;
}
//从条件变量中删除头节点并返回
stCoCondItem_t *co_cond_pop( stCoCond_t *link )
{
	stCoCondItem_t *p = link->head;
	if( p )
	{
		PopHead<stCoCondItem_t,stCoCond_t>( link );
	}
	return p;
}
