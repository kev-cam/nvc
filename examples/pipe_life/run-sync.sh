#!/bin/sh

nvc -L /usr/local/src/nvc-build/lib -a life_cell_sync.vhd life_grid_sync.vhd 
nvc -L /usr/local/src/nvc-build/lib -e life_grid_sync
clear
stdbuf -oL nvc -L /usr/local/src/nvc-build/lib -r life_grid_sync 
