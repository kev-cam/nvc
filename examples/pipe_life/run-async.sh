#!/bin/bash
set -e
cd "$(dirname "$0")"
NVC=/usr/local/src/nvc-build/bin/nvc
rm -rf work
$NVC --std=2040 -L /usr/local/src/nvc-build/lib/ --work=work -a life_cell.vhd life_top.vhd
$NVC --std=2040 -L /usr/local/src/nvc-build/lib/ -e life_top
$NVC -r life_top
