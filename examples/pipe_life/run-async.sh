#!/bin/bash
set -e
cd "$(dirname "$0")"
BUILD_DIR=`echo $PWD | sed s=nvc.*=nvc-build=`
NVC=nvc
PATH=$BUILD_DIR/bin:$PATH
which nvc 
which nvc >/dev/null 2>&1
if [ 0 != $? ] ; then
    echo No nvc in \$PATH
    exit
fi
rm -rf work
$NVC --std=2040 -L /usr/local/src/nvc-build/lib/ --work=work -a life_cell.vhd life_top.vhd
$NVC --std=2040 -L /usr/local/src/nvc-build/lib/ -e life_top
clear
$NVC -r life_top
