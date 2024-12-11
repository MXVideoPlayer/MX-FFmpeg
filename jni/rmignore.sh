#!/bin/bash 

list=$(find ./ -name ".gitignore")

echo $list

for one in $list
do
 echo $one
 rm $one
done