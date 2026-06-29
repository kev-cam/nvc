#ifndef _VHDL2VLOG_H
#define _VHDL2VLOG_H
#include "prim.h"
#include <stdio.h>
// Emit synthesizable Verilog for the elaborated block `block` to `path`.
bool vhdl2vlog(tree_t block, const char *modname, const char *path);
// Emit ONE module to an already-open file (for writing a whole subtree into one
// file). Returns true iff fully translated. Names child module references via
// vhdl2vlog_variant_name() (per-(entity,generics)) so a subtree flatten resolves
// and generic width-variants stay distinct.
bool vhdl2vlog_module(FILE *f, tree_t block, const char *modname);
// Canonical module name (basename, lowercased+sanitized) = model.c mod_lower.
const char *vhdl2vlog_modname(ident_t id);
// Per-(entity, generic-actuals) module name: distinguishes generic-instance
// variants (different widths/depths) so a whole-subtree flatten instantiates the
// correct-width module. Identical at the instantiation and definition sites
// (both pass the same elaborated block). Falls back to the plain modname when the
// block has no generics. `block` must be the elaborated T_BLOCK; `entity_id` its
// entity ident.
const char *vhdl2vlog_variant_name(ident_t entity_id, tree_t block);
#endif
