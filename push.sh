#!/bin/bash


#ls_date=`date +%Y-%m-%d`
ls_date="Add_notes"
git pull 
git add .
git commit -m ${ls_date}
git push origin
