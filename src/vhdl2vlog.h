#ifndef _VHDL2VLOG_H
#define _VHDL2VLOG_H
#include "prim.h"
// Emit synthesizable Verilog for the elaborated block `block` to `path`.
bool vhdl2vlog(tree_t block, const char *modname, const char *path);
#endif
