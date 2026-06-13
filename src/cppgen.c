//
//  cppgen.c — emit portable C++ for the ldx RISC-V simulation array.
//
//  Each VHDL/SV process becomes a target-independent, stackless C++ function
//  on a per-instance context object, driven by a thin runtime HAL.  Instances
//  are emitted too (signal creation + context wiring) plus an ldx_elaborate()
//  that builds the design for a generic runtime.  Emitted code carries #line
//  back to the HDL; ldx tooling lays it out / pools / places it per target.
//
//  Ahead-of-time backend: `nvc -e --emit-cpp=DIR`.  Consumes MIR (every node
//  has a loc_t -> accurate #line).  No LLVM dependency.  Strings never reach
//  the array: each becomes a numeric id in strings.idx (mechanism TBD).
//

#include "util.h"
#include "array.h"
#include "common.h"
#include "diag.h"
#include "hash.h"
#include "ident.h"
#include "lib.h"
#include "lower.h"
#include "mir/mir-unit.h"
#include "mir/mir-node.h"
#include "option.h"
#include "phase.h"
#include "tree.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <limits.h>

typedef A(ident_t) unit_list_t;
typedef A(char *)  str_list_t;

static str_list_t g_strings = AINIT;
typedef A(char) kind_list_t;
static kind_list_t g_strkind = AINIT;   // 'L'=literal text, 'V'=value/image

static uint32_t cppgen_intern2(const char *s, char kind)
{
   for (unsigned i = 0; i < g_strings.count; i++)
      if (strcmp(g_strings.items[i], s) == 0)
         return i;
   APUSH(g_strings, xstrdup(s));
   APUSH(g_strkind, kind);
   return g_strings.count - 1;
}

static uint32_t cppgen_intern(const char *s)
{
   return cppgen_intern2(s, 'V');
}

// --- value-returning function calls: emit referenced VHDL functions inline ----
static char *cppgen_csym(ident_t name);       // defined below
static bool             g_infunc = false;     // true while lowering a function body
static mir_context_t   *g_mc = NULL;
static unit_registry_t *g_ur = NULL;
typedef A(ident_t) func_list_t;
static func_list_t g_funcs = AINIT;           // function idents to emit

static mir_unit_t *cppgen_get_func(ident_t id)
{
   mir_unit_t *fu = (g_mc != NULL) ? mir_get_unit(g_mc, id) : NULL;
   if (fu == NULL && g_ur != NULL) {
      (void)unit_registry_get(g_ur, id);
      fu = mir_get_unit(g_mc, id);
   }
   return fu;
}

// A user function we can emit inline: real function body, SCALAR result, and
// params matching the call's value args. (Array/string returns need the array
// op family — still stubbed.)
static mir_unit_t *cppgen_callable(ident_t callee, int nvalargs)
{
   mir_unit_t *fu = cppgen_get_func(callee);
   if (fu == NULL) return NULL;
   const int knd = mir_get_kind(fu);
   if (knd != MIR_UNIT_FUNCTION && knd != MIR_UNIT_PROCEDURE) return NULL;
   const mir_type_t rt = mir_get_result(fu);
   if (!mir_is_null(rt)) {                    // a value-returning fn
      switch (mir_get_class(fu, rt)) {
      case MIR_TYPE_INT: case MIR_TYPE_OFFSET: case MIR_TYPE_REAL:
      case MIR_TYPE_UARRAY:                   // a string/unconstrained-array result
         break;                               // (heap fat-pointer; reported via \001S)
      default:                                // record/carray-by-value returns are
         return NULL;                         // not modelled -> keep stubbing them
      }
   }                                          // else void (a non-suspending proc)
   if ((int)mir_count_params(fu) != nvalargs) return NULL;

   // params must be a single C value: scalar, or a pointer-passable composite
   // (uarray header / array data ptr / record ptr) passed as void*.
   for (int i = 0; i < nvalargs; i++) {
      switch (mir_get_class(fu, mir_get_type(fu, mir_get_param(fu, i)))) {
      case MIR_TYPE_INT: case MIR_TYPE_OFFSET: case MIR_TYPE_REAL:
      case MIR_TYPE_POINTER: case MIR_TYPE_ACCESS: case MIR_TYPE_UARRAY:
      case MIR_TYPE_CARRAY: case MIR_TYPE_RECORD: case MIR_TYPE_CONTEXT:
         break;     // CONTEXT = enclosing-scope ptr; passed as void*, often unused
      default:
         return NULL;
      }
   }

   // pure scalar function only: reject any signal/hal/context/upref op, which
   // can't be referenced from a standalone C++ function (would use hal / s).
   const int nb = mir_count_blocks(fu);
   for (int b = 0; b < nb; b++) {
      mir_block_t blk = mir_get_block(fu, b);
      const int nn = mir_count_nodes(fu, blk);
      for (int n = 0; n < nn; n++) {
         switch (mir_get_op(fu, mir_get_node(fu, blk, n))) {
         case MIR_OP_VAR_UPREF:    case MIR_OP_RESOLVED:
         case MIR_OP_DRIVE_SIGNAL: case MIR_OP_SCHED_WAVEFORM:
         case MIR_OP_SCHED_EVENT:  case MIR_OP_SCHED_PROCESS:
         case MIR_OP_CMP_TRIGGER:  case MIR_OP_ADD_TRIGGER:
         case MIR_OP_INIT_SIGNAL:
         case MIR_OP_WAIT:         case MIR_OP_PCALL:
         case MIR_OP_LINK_PACKAGE: case MIR_OP_LINK_VAR:
            // NB CONTEXT_UPREF is allowed: a recursive/nested call passes the
            // scope context around (stubbed to scratch) but never derefs it for
            // a variable -- that would be VAR_UPREF, which stays rejected.
            return NULL;
         default: break;
         }
      }
   }
   return fu;
}

static void cppgen_need_func(ident_t id)
{
   for (unsigned i = 0; i < g_funcs.count; i++)
      if (g_funcs.items[i] == id) return;
   APUSH(g_funcs, id);
}

// Unique C name for a function: cppgen_csym collapses overloaded functions
// (e.g. "=" for different types) to the same string, so disambiguate by the
// function's index in g_funcs. Caller frees.
static char *cppgen_func_sym(ident_t id)
{
   int idx = -1;
   for (unsigned i = 0; i < g_funcs.count; i++)
      if (g_funcs.items[i] == id) { idx = (int)i; break; }
   char *base LOCAL = cppgen_csym(id);
   return xasprintf("%s_fn%d", base, idx);
}

static void cppgen_scan_fcalls(mir_unit_t *mu)
{
   const int nblocks = mir_count_blocks(mu);
   for (int b = 0; b < nblocks; b++) {
      mir_block_t blk = mir_get_block(mu, b);
      const int nn = mir_count_nodes(mu, blk);
      for (int n = 0; n < nn; n++) {
         mir_value_t node = mir_get_node(mu, blk, n);
         if (mir_get_op(mu, node) != MIR_OP_FCALL) continue;
         const int na = mir_count_args(mu, node);
         ident_t callee = mir_get_name(mu, mir_get_arg(mu, node, 0));
         if (callee != NULL && cppgen_callable(callee, na - 1) != NULL)
            cppgen_need_func(callee);
      }
   }
}

typedef struct {
   mir_unit_t  *mu;
   mir_value_t *defs;
   bool        *isdef;
   bool        *used;
   int          maxid;
   int         *vmap;     // var id -> declaration index (for instances)
   int          vmapmax;
} cppgen_ctx_t;

static void cppgen_build_maps(cppgen_ctx_t *c)
{
   mir_unit_t *mu = c->mu;
   const int nblocks = mir_count_blocks(mu);
   c->maxid = 0;
   for (int b = 0; b < nblocks; b++) {
      mir_block_t blk = mir_get_block(mu, b);
      const int nn = mir_count_nodes(mu, blk);
      for (int n = 0; n < nn; n++) {
         mir_value_t node = mir_get_node(mu, blk, n);
         if ((int)node.id > c->maxid) c->maxid = node.id;
      }
   }
   c->defs  = xcalloc_array(c->maxid + 1, sizeof(mir_value_t));
   c->isdef = xcalloc_array(c->maxid + 1, sizeof(bool));
   c->used  = xcalloc_array(c->maxid + 1, sizeof(bool));
   for (int b = 0; b < nblocks; b++) {
      mir_block_t blk = mir_get_block(mu, b);
      const int nn = mir_count_nodes(mu, blk);
      for (int n = 0; n < nn; n++) {
         mir_value_t node = mir_get_node(mu, blk, n);
         c->defs[node.id] = node;
         c->isdef[node.id] = true;
         const int na = mir_count_args(mu, node);
         for (int a = 0; a < na; a++) {
            mir_value_t arg = mir_get_arg(mu, node, a);
            if (arg.tag == MIR_TAG_NODE && (int)arg.id <= c->maxid)
               c->used[arg.id] = true;
         }
      }
   }
}

static void cppgen_free_maps(cppgen_ctx_t *c)
{
   free(c->defs); free(c->isdef); free(c->used);
   free(c->vmap);
}

static void cppgen_walk_hier(unit_list_t *units, hset_t *seen, tree_t block)
{
   assert(tree_kind(block) == T_BLOCK);

   tree_t hier = tree_decl(block, 0);
   assert(tree_kind(hier) == T_HIER);

   ident_t unit_name = tree_ident(hier), prefix = tree_ident2(hier);
   if (!hset_contains(seen, unit_name)) {
      APUSH(*units, unit_name);
      hset_insert(seen, unit_name);
   }

   const int nstmts = tree_stmts(block);
   // Register this block's OWN processes before descending into sub-instances,
   // so a parent's process runs ahead of its children's at time 0 (matches the
   // order nvc's scheduler uses, e.g. cover13: level_1 before nested level_2).
   for (int i = 0; i < nstmts; i++) {
      tree_t s = tree_stmt(block, i);
      if (tree_kind(s) == T_PROCESS) {
         ident_t sym = ident_prefix(prefix, tree_ident(s), '.');
         if (!hset_contains(seen, sym)) {
            APUSH(*units, sym);
            hset_insert(seen, sym);
         }
      }
   }
   for (int i = 0; i < nstmts; i++) {
      tree_t s = tree_stmt(block, i);
      if (tree_kind(s) == T_BLOCK)
         cppgen_walk_hier(units, seen, s);
   }
}

static char *cppgen_csym(ident_t name)
{
   char *s = xstrdup(istr(name));
   for (char *p = s; *p; p++) {
      if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z')
            || (*p >= '0' && *p <= '9')))
         *p = '_';
   }
   return s;
}

static const char *cppgen_ctype(mir_unit_t *mu, mir_type_t type)
{
   if (mir_is_null(type))
      return "int64_t";

   switch (mir_get_class(mu, type)) {
   case MIR_TYPE_INT:
      switch (mir_get_repr(mu, type)) {
      case MIR_REPR_U1:
      case MIR_REPR_U8:  return "uint8_t";
      case MIR_REPR_I8:  return "int8_t";
      case MIR_REPR_U16: return "uint16_t";
      case MIR_REPR_I16: return "int16_t";
      case MIR_REPR_U32: return "uint32_t";
      case MIR_REPR_I32: return "int32_t";
      case MIR_REPR_U64: return "uint64_t";
      default:           return "int64_t";
      }
   case MIR_TYPE_OFFSET:
      return "int64_t";
   case MIR_TYPE_REAL:
      return "double";
   case MIR_TYPE_POINTER:
   case MIR_TYPE_ACCESS:
   case MIR_TYPE_SIGNAL:
   case MIR_TYPE_CONTEXT:
   case MIR_TYPE_UARRAY:        // fat pointer, carried as an opaque address here
   case MIR_TYPE_CARRAY:        // a constrained-array value is a pointer to data
      return "void *";
   default:
      return "int64_t";
   }
}

// Byte size of a MIR type in our SELF-CONTAINED memory model (packed, no
// inter-field padding).  Only internal consistency matters: the emitted code
// both allocates and loads/stores with this layout; nothing is shared with nvc.
static int cppgen_sizeof(mir_unit_t *mu, mir_type_t t)
{
   if (mir_is_null(t)) return 8;
   switch (mir_get_class(mu, t)) {
   case MIR_TYPE_INT:
      switch (mir_get_repr(mu, t)) {
      case MIR_REPR_U1: case MIR_REPR_U8: case MIR_REPR_I8:  return 1;
      case MIR_REPR_U16: case MIR_REPR_I16:                  return 2;
      case MIR_REPR_U32: case MIR_REPR_I32:                  return 4;
      default:                                               return 8;
      }
   case MIR_TYPE_OFFSET:
   case MIR_TYPE_REAL:
   case MIR_TYPE_POINTER:
   case MIR_TYPE_ACCESS:
   case MIR_TYPE_SIGNAL:
   case MIR_TYPE_CONTEXT:
      return 8;
   case MIR_TYPE_CARRAY:
      return mir_get_size(mu, t) * cppgen_sizeof(mu, mir_get_elem(mu, t));
   case MIR_TYPE_RECORD: {
      size_t nf = 0;
      const mir_type_t *fs = mir_get_fields(mu, t, &nf);
      int sz = 0;
      for (size_t i = 0; i < nf; i++) sz += cppgen_sizeof(mu, fs[i]);
      return sz ? sz : 1;
   }
   case MIR_TYPE_UARRAY:                  // fat ptr: data + (left,length) per dim
      return 8 + 16 * mir_get_dims(mu, t);
   default:
      return 8;
   }
}

// A composite (record/constrained-array) type needs real inline byte storage,
// not a scalar slot.  Accessed only via ADDRESS_OF + RECORD_REF/ARRAY_REF +
// LOAD/STORE-through-pointer, never a direct scalar load.
static bool cppgen_is_composite(mir_unit_t *mu, mir_type_t t)
{
   if (mir_is_null(t)) return false;
   const int cls = mir_get_class(mu, t);
   return cls == MIR_TYPE_RECORD || cls == MIR_TYPE_CARRAY;
}

// Packed byte offset of field `idx` within a record type.
static int cppgen_field_offset(mir_unit_t *mu, mir_type_t rec, int idx)
{
   size_t nf = 0;
   const mir_type_t *fs = mir_get_fields(mu, rec, &nf);
   int off = 0;
   for (int i = 0; i < idx && i < (int)nf; i++) off += cppgen_sizeof(mu, fs[i]);
   return off;
}

static void cppgen_val(FILE *f, mir_unit_t *mu, mir_value_t v)
{
   switch (v.tag) {
   case MIR_TAG_NODE:  fprintf(f, "t%u", v.id); break;
   case MIR_TAG_VAR:   fprintf(f, g_infunc ? "v%u" : "s->v%u", v.id); break;
   case MIR_TAG_PARAM: fprintf(f, "p%u", v.id); break;
   case MIR_TAG_CONST:
      {
         int64_t c = 0;
         mir_get_const(mu, v, &c);
         fprintf(f, "%"PRIi64, c);
      }
      break;
   case MIR_TAG_ENUM:  fprintf(f, "%u", v.id); break;
   case MIR_TAG_BLOCK: fprintf(f, "BB%u", v.id); break;
   case MIR_TAG_NULL:  fprintf(f, "0"); break;
   default:            fprintf(f, "0 /*?tag%u*/", v.tag); break;
   }
}

static mir_value_t cppgen_trace_raw(cppgen_ctx_t *c, mir_value_t v)
{
   while (v.tag == MIR_TAG_NODE && v.id <= (unsigned)c->maxid && c->isdef[v.id]) {
      mir_value_t d = c->defs[v.id];
      const mir_op_t op = mir_get_op(c->mu, d);
      if (op == MIR_OP_UNWRAP || op == MIR_OP_CAST) {
         v = mir_get_arg(c->mu, d, 0);
         continue;
      }
      else if (op == MIR_OP_FCALL) {
         const int na = mir_count_args(c->mu, d);
         return na > 1 ? mir_get_arg(c->mu, d, na - 1) : MIR_NULL_VALUE;
      }
      break;
   }
   return MIR_NULL_VALUE;
}

// If v is (a pointer to) a constant character array — a string literal — return
// the literal text (caller frees); else NULL. Reports of literal strings then
// ship the host a string id, not a stubbed 0.
static char *cppgen_literal(cppgen_ctx_t *c, mir_value_t v)
{
   while (v.tag == MIR_TAG_NODE && v.id <= (unsigned)c->maxid && c->isdef[v.id]) {
      mir_value_t d = c->defs[v.id];
      const mir_op_t op = mir_get_op(c->mu, d);
      if (op == MIR_OP_ADDRESS_OF || op == MIR_OP_UNWRAP || op == MIR_OP_CAST) {
         v = mir_get_arg(c->mu, d, 0);
         continue;
      }
      if (op == MIR_OP_CONST_ARRAY) {
         const int n = mir_count_args(c->mu, d);
         char *s = xmalloc(n + 1);
         int len = 0;
         for (int i = 0; i < n; i++) {
            int64_t ch;
            if (mir_get_const(c->mu, mir_get_arg(c->mu, d, i), &ch)
                && ch >= 0 && ch <= 255)
               s[len++] = (char)ch;
            else { free(s); return NULL; }   // not a plain byte string
         }
         s[len] = '\0';
         return s;
      }
      break;
   }
   return NULL;
}

// If v traces (through unwrap/cast) to a scalar 'image foreign call we can render,
// return a type code ('I' int, 'C' char, 'B' bool, 'T' time) and set *valout to
// the value arg; else 0 (enum/physical/user images -> caller stubs).
static char cppgen_image(cppgen_ctx_t *c, mir_value_t v, mir_value_t *valout)
{
   mir_unit_t *mu = c->mu;
   while (v.tag == MIR_TAG_NODE && v.id <= (unsigned)c->maxid && c->isdef[v.id]) {
      const mir_op_t o = mir_get_op(mu, c->defs[v.id]);
      if (o == MIR_OP_UNWRAP || o == MIR_OP_CAST) { v = mir_get_arg(mu, c->defs[v.id], 0); continue; }
      break;
   }
   if (v.tag != MIR_TAG_NODE || v.id > (unsigned)c->maxid || !c->isdef[v.id]
       || mir_get_op(mu, c->defs[v.id]) != MIR_OP_FCALL)
      return 0;
   mir_value_t fc = c->defs[v.id];
   ident_t cn = mir_get_name(mu, mir_get_arg(mu, fc, 0));
   const int fna = mir_count_args(mu, fc);
   if (cn == NULL || fna < 2) return 0;
   const char *s = istr(cn);
   if (strstr(s, "image") == NULL && strstr(s, "IMAGE") == NULL) return 0;
   char code = 0;
   if      (strstr(s, "CHARACTER")) code = 'C';
   else if (strstr(s, "BOOLEAN"))   code = 'B';
   else if (strstr(s, "TIME"))      code = 'T';
   else if (strstr(s, "INTEGER") || strstr(s, "NATURAL") || strstr(s, "POSITIVE")) code = 'I';
   if (code == 0) return 0;
   *valout = mir_get_arg(mu, fc, fna - 1);
   return code;
}

// Recognise a report message built by concatenation: an ALLOC buffer filled by
// ordered (array_ref offset -> copy src) pairs, each src a literal const-array
// or unwrap(fcall INTEGER'image(value)). Build a printf format ("text %lld") and
// return the single integer value in *valout. NULL if it isn't this shape (or has
// !=1 value / a non-integer image). Caller frees the result.
// Build a host format string (literal bytes + \001<typecode> per value) for a
// computed concat message, and fill vals[0..*nvals-1] with the value pieces in
// left-to-right order.  Caller frees the returned string.
#define CPPGEN_MAXVALS 16
static char *cppgen_format(cppgen_ctx_t *c, mir_value_t msg,
                           mir_value_t *vals, int *nvals)
{
   mir_unit_t *mu = c->mu;
   *nvals = 0;
   if (msg.tag != MIR_TAG_NODE || msg.id > (unsigned)c->maxid || !c->isdef[msg.id])
      return NULL;
   if (mir_get_op(mu, c->defs[msg.id]) != MIR_OP_ALLOC) return NULL;

   struct { int64_t off; mir_value_t src; } pc[32];
   int npc = 0;
   for (int id = 0; id <= c->maxid && npc < 32; id++) {
      if (!c->isdef[id] || mir_get_op(mu, c->defs[id]) != MIR_OP_COPY) continue;
      mir_value_t dest = mir_get_arg(mu, c->defs[id], 0);
      if (dest.tag != MIR_TAG_NODE || dest.id > (unsigned)c->maxid || !c->isdef[dest.id])
         continue;
      mir_value_t dd = c->defs[dest.id];
      if (mir_get_op(mu, dd) != MIR_OP_ARRAY_REF) continue;
      mir_value_t base = mir_get_arg(mu, dd, 0);
      if (base.tag != msg.tag || base.id != msg.id) continue;
      int64_t off = 0;
      mir_get_const(mu, mir_get_arg(mu, dd, 1), &off);
      pc[npc].off = off;
      pc[npc].src = mir_get_arg(mu, c->defs[id], 1);
      npc++;
   }
   if (npc == 0) return NULL;
   // pieces are collected in node-id order = concat (left-to-right) order; do NOT
   // sort by offset (offsets after a dynamic-length image piece are non-const).

   char fmt[512]; int fl = 0;
   for (int i = 0; i < npc; i++) {
      char *lit = cppgen_literal(c, pc[i].src);
      if (lit != NULL) {                            // literal piece -> verbatim
         for (const char *p = lit; *p && fl < 508; p++) fmt[fl++] = *p;
         free(lit);
         continue;
      }
      mir_value_t val;                              // value piece -> \x01<typecode>
      const char code = cppgen_image(c, pc[i].src, &val);
      if (code == 0 || *nvals >= CPPGEN_MAXVALS) return NULL;  // not renderable / too many
      vals[(*nvals)++] = val;
      if (fl < 507) { fmt[fl++] = '\x01'; fmt[fl++] = code; }
   }
   fmt[fl] = '\0';
   if (*nvals == 0) return NULL;   // pure-literal concat already handled elsewhere
   return xstrdup(fmt);
}

static bool cppgen_assign(FILE *f, cppgen_ctx_t *c, mir_value_t node)
{
   if (node.tag == MIR_TAG_NODE && node.id <= (unsigned)c->maxid
       && c->used[node.id]) {
      fprintf(f, "   t%u = ", node.id);
      return true;
   }
   fprintf(f, "   ");
   return false;
}

// Emit the temp declarations for used node results (no initialiser -> the
// resume goto cannot cross an initialisation).
static void cppgen_emit_temps(FILE *f, cppgen_ctx_t *c)
{
   for (int id = 0; id <= c->maxid; id++)
      if (c->isdef[id] && c->used[id]) {
         mir_type_t tt = mir_get_type(c->mu, c->defs[id]);
         // composite-typed temps (e.g. CONST_ARRAY) hold a pointer to the data
         const char *ct = cppgen_is_composite(c->mu, tt) ? "void *"
                                                         : cppgen_ctype(c->mu, tt);
         fprintf(f, "   %s t%d;\n", ct, id);
      }
}

// True if v is a RELIABLY-computed value (grounds in const/var/param/load/
// resolved through cmp/logic/arith), not a stubbed op (fcall/array/etc. -> 0).
// An assert is only emitted when its condition is reliable; otherwise a stubbed
// condition would read 0 and fire the assertion spuriously.
static bool cppgen_cond_ok(cppgen_ctx_t *c, mir_value_t v, int depth)
{
   if (depth > 24) return false;
   switch (v.tag) {
   case MIR_TAG_CONST: case MIR_TAG_ENUM: case MIR_TAG_PARAM: case MIR_TAG_VAR:
      return true;
   case MIR_TAG_NODE: break;
   default: return false;
   }
   if (v.id > (unsigned)c->maxid || !c->isdef[v.id]) return false;
   mir_value_t d = c->defs[v.id];
   const int na = mir_count_args(c->mu, d);
   switch (mir_get_op(c->mu, d)) {
   case MIR_OP_CONST: case MIR_OP_LOAD: case MIR_OP_RESOLVED:
      return true;
   case MIR_OP_NOT: case MIR_OP_NEG: case MIR_OP_CAST:
      return na > 0 && cppgen_cond_ok(c, mir_get_arg(c->mu, d, 0), depth + 1);
   case MIR_OP_CMP:
      return na > 2 && cppgen_cond_ok(c, mir_get_arg(c->mu, d, 1), depth + 1)
                    && cppgen_cond_ok(c, mir_get_arg(c->mu, d, 2), depth + 1);
   case MIR_OP_AND: case MIR_OP_OR:  case MIR_OP_XOR:
   case MIR_OP_ADD: case MIR_OP_SUB: case MIR_OP_MUL:
   case MIR_OP_DIV: case MIR_OP_REM: case MIR_OP_MOD:
      return na > 1 && cppgen_cond_ok(c, mir_get_arg(c->mu, d, 0), depth + 1)
                    && cppgen_cond_ok(c, mir_get_arg(c->mu, d, 1), depth + 1);
   case MIR_OP_SELECT:
      return na > 2 && cppgen_cond_ok(c, mir_get_arg(c->mu, d, 1), depth + 1)
                    && cppgen_cond_ok(c, mir_get_arg(c->mu, d, 2), depth + 1);
   default:
      return false;   // fcall/unwrap/array/etc -> value not trustworthy
   }
}

// Emit `ldx_io_emit(...)` for a report/assert message: literal text, a concat
// format (\001<code> markers), an image-only value, or a raw value. Caller has
// already written any leading indentation/guard.
// A report/assert message is a VHDL string: msg = data pointer (already
// unwrapped by the front end), len = its length (may be null).  Render it as a
// literal, a concat format, a single 'image value, a computed-string (the raw
// bytes at msg/len), or a raw scalar -- in that priority.
static void cppgen_emit_report(FILE *f, cppgen_ctx_t *c, mir_value_t msg,
                               mir_value_t len)
{
   mir_unit_t *mu = c->mu;
   char *lit = mir_is_null(msg) ? NULL : cppgen_literal(c, msg);
   mir_value_t vals[CPPGEN_MAXVALS]; int nv = 0;
   char *fmt = NULL;
   mir_value_t ival = MIR_NULL_VALUE;
   char icode;
   if (lit != NULL) {                               // pure literal: no args
      const uint32_t id = cppgen_intern2(lit, 'L');
      fprintf(f, "ldx_io_emit(hal, %u, 0, 0);\n", id);
      free(lit);
   }
   else if (!mir_is_null(msg) && (fmt = cppgen_format(c, msg, vals, &nv)) != NULL) {
      const uint32_t id = cppgen_intern2(fmt, 'F');     // literal + N value pieces
      fprintf(f, "{ int64_t _a[] = {");
      for (int k = 0; k < nv; k++) {
         if (k) fprintf(f, ", ");
         fprintf(f, "(int64_t)("); cppgen_val(f, mu, vals[k]); fprintf(f, ")");
      }
      fprintf(f, "}; ldx_io_emit(hal, %u, _a, %d); }\n", id, nv);
      free(fmt);
   }
   else if (!mir_is_null(msg) && (icode = cppgen_image(c, msg, &ival)) != 0) {
      const char f2[3] = { '\x01', icode, '\0' };       // a single 'image value
      const uint32_t id = cppgen_intern2(f2, 'F');
      fprintf(f, "{ int64_t _a[] = {(int64_t)(");
      cppgen_val(f, mu, ival);
      fprintf(f, ")}; ldx_io_emit(hal, %u, _a, 1); }\n", id);
   }
   else if (!mir_is_null(msg) && !mir_is_null(len)) {
      // computed string (e.g. a user fn returning string): ship the data ptr +
      // length; the host prints the bytes (\001S consumes the two args).
      const char sfmt[3] = { '\x01', 'S', '\0' };
      const uint32_t id = cppgen_intern2(sfmt, 'F');
      fprintf(f, "{ int64_t _a[] = { (int64_t)(void*)(");
      cppgen_val(f, mu, msg);
      fprintf(f, "), (int64_t)(");
      cppgen_val(f, mu, len);
      fprintf(f, ") }; ldx_io_emit(hal, %u, _a, 2); }\n", id);
   }
   else {                                           // last resort: a raw scalar
      const char f2[3] = { '\x01', 'I', '\0' };
      const uint32_t id = cppgen_intern2(f2, 'F');
      mir_value_t raw = mir_is_null(msg) ? MIR_NULL_VALUE : cppgen_trace_raw(c, msg);
      fprintf(f, "{ int64_t _a[] = {(int64_t)(");
      if (mir_is_null(raw)) fprintf(f, "0"); else cppgen_val(f, mu, raw);
      fprintf(f, ")}; ldx_io_emit(hal, %u, _a, 1); }\n", id);
   }
}

// --------------------------------------------------------------------------
// Process body lowering
// Placeholder result for an UNLOWERED op.  A pointer-typed result must be a
// valid (zeroed) address so a later deref / array-ref / record-ref can't fault;
// scalars are just 0.  ldx_scratch() needs no hal, so this works in functions.
static void cppgen_stub_val(FILE *f, cppgen_ctx_t *c, mir_value_t node)
{
   mir_type_t t = mir_get_type(c->mu, node);
   if (!mir_is_null(t)) {
      switch (mir_get_class(c->mu, t)) {
      case MIR_TYPE_POINTER: case MIR_TYPE_ACCESS: case MIR_TYPE_UARRAY:
      case MIR_TYPE_CARRAY:  case MIR_TYPE_SIGNAL: case MIR_TYPE_CONTEXT:
      case MIR_TYPE_RECORD:
         fprintf(f, "ldx_scratch()");
         return;
      default: break;
      }
   }
   fprintf(f, "0");
}

// --------------------------------------------------------------------------
static void cppgen_lower_node(FILE *f, cppgen_ctx_t *c, mir_value_t node)
{
   mir_unit_t *mu = c->mu;
   const mir_op_t op = mir_get_op(mu, node);
   const int na = mir_count_args(mu, node);

#define ARG(n) cppgen_val(f, mu, mir_get_arg(mu, node, n))

   switch (op) {
   case MIR_OP_VAR_UPREF:
      cppgen_assign(f, c, node);
      fprintf(f, "ldx_var_upref(s->__ctx, "); ARG(0); fprintf(f, ", ");
      ARG(2); fprintf(f, ");\n");
      break;

   case MIR_OP_LOAD:
      {
         mir_value_t a0 = mir_get_arg(mu, node, 0);
         const char *ct = cppgen_ctype(mu, mir_get_type(mu, node));
         cppgen_assign(f, c, node);
         if (a0.tag == MIR_TAG_VAR
             && !cppgen_is_composite(mu, mir_get_var_type(mu, a0)))
            cppgen_val(f, mu, a0);             // direct scalar var read
         else { fprintf(f, "*(%s*)(", ct); cppgen_val(f, mu, a0); fprintf(f, ")"); }
         fprintf(f, ";\n");
      }
      break;

   case MIR_OP_STORE:
      {
         mir_value_t dst = mir_get_arg(mu, node, 0);
         const char *ct = cppgen_ctype(mu, mir_get_type(mu, node));
         fprintf(f, "   ");
         if (dst.tag == MIR_TAG_VAR
             && !cppgen_is_composite(mu, mir_get_var_type(mu, dst))) {
            cppgen_val(f, mu, dst); fprintf(f, " = "); ARG(1); fprintf(f, ";\n");
         }
         else {
            fprintf(f, "*(%s*)(", ct); cppgen_val(f, mu, dst);
            fprintf(f, ") = ("); ARG(1); fprintf(f, ");\n");
         }
      }
      break;

   case MIR_OP_RESOLVED:
      cppgen_assign(f, c, node);
      fprintf(f, "ldx_resolved(hal, "); ARG(0); fprintf(f, ");\n");
      break;

   case MIR_OP_DRIVE_SIGNAL:
      fprintf(f, "   ldx_drive_signal(hal, "); ARG(0); fprintf(f, ", ");
      ARG(1); fprintf(f, ");\n");
      break;

   case MIR_OP_SCHED_WAVEFORM:
      fprintf(f, "   ldx_sched_waveform(hal, "); ARG(0); fprintf(f, ", ");
      ARG(1); fprintf(f, ", (int64_t)("); ARG(2); fprintf(f, "), ");
      ARG(3); fprintf(f, ", "); ARG(4); fprintf(f, ");\n");
      break;

   case MIR_OP_SCHED_EVENT:
      fprintf(f, "   ldx_sched_event(hal, "); ARG(0); fprintf(f, ", ");
      ARG(1); fprintf(f, ");\n");
      break;

   case MIR_OP_SCHED_PROCESS:
      fprintf(f, "   ldx_sched_process(hal, "); ARG(0); fprintf(f, ");\n");
      break;

   case MIR_OP_CMP_TRIGGER:
      cppgen_assign(f, c, node);
      fprintf(f, "ldx_cmp_trigger(hal, (void*)(");
      ARG(0); fprintf(f, "), (int64_t)("); ARG(1); fprintf(f, "));\n");
      break;

   case MIR_OP_ADD_TRIGGER:
      fprintf(f, "   ldx_add_trigger(hal, "); ARG(0); fprintf(f, ");\n");
      break;

   case MIR_OP_NOT:
      cppgen_assign(f, c, node); fprintf(f, "!("); ARG(0); fprintf(f, ");\n");
      break;

   case MIR_OP_ADD: case MIR_OP_TRAP_ADD:
      cppgen_assign(f, c, node);
      fprintf(f, "("); ARG(0); fprintf(f, ") + ("); ARG(1); fprintf(f, ");\n");
      break;
   case MIR_OP_SUB: case MIR_OP_TRAP_SUB:
      cppgen_assign(f, c, node);
      fprintf(f, "("); ARG(0); fprintf(f, ") - ("); ARG(1); fprintf(f, ");\n");
      break;
   case MIR_OP_MUL: case MIR_OP_TRAP_MUL:
      cppgen_assign(f, c, node);
      fprintf(f, "("); ARG(0); fprintf(f, ") * ("); ARG(1); fprintf(f, ");\n");
      break;
   case MIR_OP_DIV:
      cppgen_assign(f, c, node);
      fprintf(f, "("); ARG(0); fprintf(f, ") / ("); ARG(1); fprintf(f, ");\n");
      break;
   case MIR_OP_REM: case MIR_OP_MOD:
      cppgen_assign(f, c, node);
      fprintf(f, "("); ARG(0); fprintf(f, ") %% ("); ARG(1); fprintf(f, ");\n");
      break;
   case MIR_OP_AND:
      cppgen_assign(f, c, node);
      fprintf(f, "("); ARG(0); fprintf(f, ") & ("); ARG(1); fprintf(f, ");\n");
      break;
   case MIR_OP_OR:
      cppgen_assign(f, c, node);
      fprintf(f, "("); ARG(0); fprintf(f, ") | ("); ARG(1); fprintf(f, ");\n");
      break;
   case MIR_OP_XOR:
      cppgen_assign(f, c, node);
      fprintf(f, "("); ARG(0); fprintf(f, ") ^ ("); ARG(1); fprintf(f, ");\n");
      break;
   case MIR_OP_NEG:
      cppgen_assign(f, c, node); fprintf(f, "-("); ARG(0); fprintf(f, ");\n");
      break;

   case MIR_OP_CMP:
      {
         cppgen_assign(f, c, node);
         const char *o = "==";
         switch (mir_get_arg(mu, node, 0).id) {
            case MIR_CMP_EQ:  o = "=="; break;
            case MIR_CMP_NEQ: o = "!="; break;
            case MIR_CMP_LT:  o = "<";  break;
            case MIR_CMP_GT:  o = ">";  break;
            case MIR_CMP_LEQ: o = "<="; break;
            case MIR_CMP_GEQ: o = ">="; break;
         }
         fprintf(f, "("); ARG(1); fprintf(f, ") %s (", o); ARG(2); fprintf(f, ");\n");
      }
      break;
   case MIR_OP_SELECT:
      cppgen_assign(f, c, node);
      fprintf(f, "("); ARG(0); fprintf(f, ") ? (");
      ARG(1); fprintf(f, ") : ("); ARG(2); fprintf(f, ");\n");
      break;

   case MIR_OP_CONST:
      {
         int64_t cv = 0;
         mir_get_const(mu, node, &cv);
         cppgen_assign(f, c, node);
         fprintf(f, "%"PRIi64";\n", cv);
      }
      break;

   case MIR_OP_JUMP:
      fprintf(f, "   goto BB%u;\n", mir_get_arg(mu, node, 0).id);
      break;

   case MIR_OP_COND:
      fprintf(f, "   if ("); ARG(0);
      fprintf(f, ") goto BB%u; else goto BB%u;\n",
              mir_get_arg(mu, node, 1).id, mir_get_arg(mu, node, 2).id);
      break;

   case MIR_OP_CASE:
      // arg0=selector, arg1=default block, then (value,block) pairs
      for (int i = 2; i + 1 < na; i += 2) {
         fprintf(f, "   if ((");
         cppgen_val(f, mu, mir_get_arg(mu, node, 0));
         fprintf(f, ") == (");
         cppgen_val(f, mu, mir_get_arg(mu, node, i));
         fprintf(f, ")) goto BB%u;\n", mir_get_arg(mu, node, i + 1).id);
      }
      fprintf(f, "   goto BB%u;\n", mir_get_arg(mu, node, 1).id);
      break;

   case MIR_OP_WAIT:
      fprintf(f, "   s->__state = %u; return;\n", mir_get_arg(mu, node, 0).id);
      break;

   case MIR_OP_RETURN:
      if (g_infunc) {
         if (mir_is_null(mir_get_result(mu)))  fprintf(f, "   return;\n");  // void proc
         else if (na > 0) { fprintf(f, "   return "); ARG(0); fprintf(f, ";\n"); }
         else fprintf(f, "   return 0;\n");
      }
      else fprintf(f, "   s->__state = 1; return;\n");
      break;

   case MIR_OP_REPORT:                     // arg0=severity, arg1=msg ptr, arg2=length
      fprintf(f, "   ");                   // emitted fns take `hal` -> can report
      cppgen_emit_report(f, c, (na > 1) ? mir_get_arg(mu, node, 1) : MIR_NULL_VALUE,
                               (na > 2) ? mir_get_arg(mu, node, 2) : MIR_NULL_VALUE);
      break;

   case MIR_OP_ASSERT:
      if (na > 0 && !cppgen_cond_ok(c, mir_get_arg(mu, node, 0), 0)) break;  // unreliable cond
      {
         // assert <cond> [report <msg>] severity <sev>: when cond is FALSE emit
         // the message and, for severity >= ERROR, stop (matches nvc's default).
         mir_value_t msg = (na > 2) ? mir_get_arg(mu, node, 2) : MIR_NULL_VALUE;
         int64_t sev = 2;
         if (na > 1) mir_get_const(mu, mir_get_arg(mu, node, 1), &sev);
         fprintf(f, "   if (!(");
         cppgen_val(f, mu, mir_get_arg(mu, node, 0));
         fprintf(f, ")) {\n      ");
         if (!mir_is_null(msg))                      // arg2=msg ptr, arg3=length
            cppgen_emit_report(f, c, msg,
                               (na > 3) ? mir_get_arg(mu, node, 3) : MIR_NULL_VALUE);
         else                                       // nvc's default for a bare assert
            fprintf(f, "ldx_io_emit(hal, %u, 0, 0);\n",
                    cppgen_intern2("Assertion violation.", 'L'));
         if (sev >= 3)                               // only FAILURE aborts (nvc default)
            fprintf(f, "      ldx_fail(hal);\n");
         fprintf(f, "   }\n");
      }
      break;

   case MIR_OP_FCALL:
      {
         ident_t callee = (na > 0) ? mir_get_name(mu, mir_get_arg(mu, node, 0)) : NULL;
         mir_unit_t *fu = (callee != NULL) ? cppgen_callable(callee, na - 1) : NULL;
         if (fu != NULL) {
            cppgen_need_func(callee);
            char *cs LOCAL = cppgen_func_sym(callee);
            const int np = mir_count_params(fu);
            cppgen_assign(f, c, node);
            fprintf(f, "%s(hal", cs);             // hal first: lets fns report
            for (int i = 1; i < na; i++) {
               fprintf(f, ", ");
               mir_value_t arg = mir_get_arg(mu, node, i);
               if (i - 1 < np) {
                  mir_type_t pt = mir_get_type(fu, mir_get_param(fu, i-1));
                  // An inout/out scalar is a POINTER param taking a VAR by
                  // reference -> pass its address; a composite var is already a
                  // byte-buffer address.  Cast to the param type either way so a
                  // void*/int64/double mismatch is explicit, not a compile error.
                  const bool byref = mir_get_class(fu, pt) == MIR_TYPE_POINTER
                     && arg.tag == MIR_TAG_VAR
                     && !cppgen_is_composite(mu, mir_get_var_type(mu, arg));
                  fprintf(f, byref ? "(%s)&(" : "(%s)(", cppgen_ctype(fu, pt));
                  cppgen_val(f, mu, arg);
                  fprintf(f, ")");
               }
               else cppgen_val(f, mu, arg);
            }
            fprintf(f, ");\n");
         }
         else if (cppgen_assign(f, c, node)) {
            cppgen_stub_val(f, c, node); fprintf(f, "; // fcall -> host\n");
         }
         else fprintf(f, "; // fcall -> host\n");
      }
      break;

   case MIR_OP_ADDRESS_OF:
      {
         // Address of a storage location.  A scalar var needs &; a composite
         // var is already a byte buffer that decays to a pointer; anything else
         // (a CONST_ARRAY/ALLOC temp) is already a pointer value.
         mir_value_t a0 = mir_get_arg(mu, node, 0);
         cppgen_assign(f, c, node);
         if (a0.tag == MIR_TAG_VAR
             && !cppgen_is_composite(mu, mir_get_var_type(mu, a0))) {
            fprintf(f, "(void*)&("); cppgen_val(f, mu, a0); fprintf(f, ");\n");
         }
         else { fprintf(f, "(void*)("); cppgen_val(f, mu, a0); fprintf(f, ");\n"); }
      }
      break;

   case MIR_OP_ARRAY_REF:
      if (mir_is_signal(mu, node)) {       // signal slice: keep base (host maps)
         cppgen_assign(f, c, node); ARG(0); fprintf(f, ";\n");
      }
      else {
         const int scale = cppgen_sizeof(mu, mir_get_elem(mu, mir_get_type(mu, node)));
         cppgen_assign(f, c, node);
         fprintf(f, "(void*)((char*)("); ARG(0);
         fprintf(f, ") + (int64_t)("); ARG(1); fprintf(f, ") * %d);\n", scale);
      }
      break;

   case MIR_OP_RECORD_REF:
      {
         mir_value_t base = mir_get_arg(mu, node, 0);
         mir_type_t rec = mir_get_elem(mu, mir_get_type(mu, base));
         int64_t field = 0;
         mir_get_const(mu, mir_get_arg(mu, node, 1), &field);
         const int off = cppgen_field_offset(mu, rec, (int)field);
         cppgen_assign(f, c, node);
         fprintf(f, "(void*)((char*)("); ARG(0); fprintf(f, ") + %d);\n", off);
      }
      break;

   case MIR_OP_ALLOC:
      {
         mir_type_t pt = mir_get_pointer(mu, mir_get_type(mu, node));
         const int esz = cppgen_sizeof(mu, pt);
         cppgen_assign(f, c, node);
         fprintf(f, "ldx_alloc(hal, %d * (int64_t)(", esz); ARG(0); fprintf(f, "));\n");
      }
      break;

   case MIR_OP_NEW:
      {
         mir_type_t elem = mir_get_elem(mu, mir_get_type(mu, node));
         const int esz = cppgen_sizeof(mu, elem);
         cppgen_assign(f, c, node);
         fprintf(f, "ldx_alloc(hal, %d", esz);
         if (na > 0) { fprintf(f, " * (int64_t)("); ARG(0); fprintf(f, ")"); }
         fprintf(f, ");\n");
      }
      break;

   case MIR_OP_ALL:        // access deref: the value IS the pointer; LOAD reads it
      cppgen_assign(f, c, node); fprintf(f, "("); ARG(0); fprintf(f, ");\n");
      break;

   case MIR_OP_CAST:
      {
         // Scalar reinterpretation (int<->offset, int<->real, ->pointer).  A C
         // cast to the result type is exactly right and, crucially, keeps array
         // index expressions (offset = i - 'left, cast to OFFSET) faithful.
         const char *ct = cppgen_ctype(mu, mir_get_type(mu, node));
         cppgen_assign(f, c, node);
         fprintf(f, "(%s)(", ct); ARG(0); fprintf(f, ");\n");
      }
      break;

   case MIR_OP_COPY:
      {
         // memcpy(dest, src [, count]) of `count` (default 1) elements.  The
         // count can derive from a stubbed op (e.g. a uarray length) and come
         // out absurd; clamp so a bogus copy is skipped, not a glibc abort.
         const int esz = cppgen_sizeof(mu, mir_get_type(mu, node));
         fprintf(f, "   { int64_t _n = ");
         if (na > 2) { fprintf(f, "(int64_t)("); ARG(2); fprintf(f, ") * %d", esz); }
         else        fprintf(f, "%d", esz);
         fprintf(f, "; if (_n > 0 && _n <= (1<<20)) __builtin_memcpy((void*)(");
         ARG(0); fprintf(f, "), (void*)("); ARG(1); fprintf(f, "), _n); }\n");
      }
      break;

   case MIR_OP_SET:
      {
         // Fill `count` (arg2) elements at dest (arg0) with value (arg1):
         // lowers `(others => v)` array/slice initialisation.  Clamp count.
         const char *ct = cppgen_ctype(mu, mir_get_type(mu, node));
         fprintf(f, "   { %s *_d = (%s*)(", ct, ct); ARG(0);
         fprintf(f, "); int64_t _n = (int64_t)("); ARG(2);
         fprintf(f, "); if (_n < 0 || _n > (1<<20)) _n = 0; %s _v = (%s)(", ct, ct);
         ARG(1);
         fprintf(f, "); for (int64_t _i = 0; _i < _n; _i++) _d[_i] = _v; }\n");
      }
      break;

   case MIR_OP_CONST_ARRAY:
      {
         // Materialise a scalar-element constant array as a file-local static
         // and yield a pointer to it (gotos may legally skip a static init).
         mir_type_t et = mir_get_elem(mu, mir_get_type(mu, node));
         const int ec = mir_get_class(mu, et);
         if (ec == MIR_TYPE_INT || ec == MIR_TYPE_REAL || ec == MIR_TYPE_OFFSET) {
            fprintf(f, "   static const %s ca%u[] = {", cppgen_ctype(mu, et), node.id);
            for (int i = 0; i < na; i++) {
               if (i) fprintf(f, ", ");
               cppgen_val(f, mu, mir_get_arg(mu, node, i));
            }
            fprintf(f, "};\n");
            if (cppgen_assign(f, c, node)) fprintf(f, "(void*)ca%u;\n", node.id);
            else                           fprintf(f, ";\n");
         }
         else if (cppgen_assign(f, c, node)) fprintf(f, "0; // const array (composite elem)\n");
         else                               fprintf(f, "; // const array\n");
      }
      break;

   case MIR_OP_WRAP:
      {
         // Fat-pointer construction.  We model a uarray as a heap header
         //   int64[1 + 3*ndims] = { data_ptr, (left,len,dir) per dim }
         // so 'length/'left/'right/'range resolve at runtime.  Args: arg0=data,
         // then (left,right,dir) per dim; len = (dir==DOWNTO? l-r : r-l)+1.
         const int ndims = na / 3;
         cppgen_assign(f, c, node);
         fprintf(f, "({ int64_t *_h = (int64_t*)ldx_alloc(hal, %d); _h[0] = (int64_t)(",
                 (1 + 3 * (ndims < 1 ? 1 : ndims)) * 8);
         ARG(0); fprintf(f, ");");
         for (int d = 0; d < ndims; d++) {
            fprintf(f, " { int64_t _l=(int64_t)("); ARG(d*3 + 1);
            fprintf(f, "), _r=(int64_t)("); ARG(d*3 + 2);
            fprintf(f, "), _dir=(int64_t)("); ARG(d*3 + 3);
            fprintf(f, "); int64_t _len=(_dir==1?_l-_r:_r-_l)+1; if(_len<0)_len=0;");
            fprintf(f, " _h[%d]=_l; _h[%d]=_len; _h[%d]=_dir; }",
                    1 + d*3, 1 + d*3 + 1, 1 + d*3 + 2);
         }
         fprintf(f, " (void*)_h; });\n");
      }
      break;

   case MIR_OP_UNWRAP:
      // Extract the data pointer (header[0]).  A null/stub uarray (e.g. an
      // image fcall result) yields the zeroed scratch region, not a fault.
      cppgen_assign(f, c, node);
      fprintf(f, "({ void *_u=(void*)("); ARG(0);
      fprintf(f, "); int64_t _p=_u?((int64_t*)_u)[0]:0; _p?(void*)_p:ldx_scratch(); });\n");
      break;

   case MIR_OP_UARRAY_LEN:
   case MIR_OP_UARRAY_LEFT:
   case MIR_OP_UARRAY_DIR:
      {
         int64_t dim = 0;
         if (na > 1) mir_get_const(mu, mir_get_arg(mu, node, 1), &dim);
         const int base = 1 + (int)dim * 3;
         const int idx = (op == MIR_OP_UARRAY_LEFT) ? base
                       : (op == MIR_OP_UARRAY_DIR)  ? base + 2
                                                    : base + 1;   // LEN
         cppgen_assign(f, c, node);
         fprintf(f, "({ void *_u=(void*)("); ARG(0);
         fprintf(f, "); _u?((int64_t*)_u)[%d]:0; });\n", idx);
      }
      break;

   case MIR_OP_UARRAY_RIGHT:
      {
         int64_t dim = 0;
         if (na > 1) mir_get_const(mu, mir_get_arg(mu, node, 1), &dim);
         const int li = 1 + (int)dim*3, ni = li + 1, di = li + 2;
         cppgen_assign(f, c, node);
         fprintf(f, "({ int64_t *_h=(int64_t*)("); ARG(0);
         fprintf(f, "); _h?(_h[%d]==1?_h[%d]-_h[%d]+1:_h[%d]+_h[%d]-1):0; });\n",
                 di, li, ni, li, ni);
      }
      break;

   case MIR_OP_LOCUS:
      if (cppgen_assign(f, c, node)) {
         cppgen_stub_val(f, c, node); fprintf(f, "; // %s -> host\n", mir_op_string(op));
      }
      else fprintf(f, "; // %s -> host\n", mir_op_string(op));
      break;

   case MIR_OP_PCALL:
      {
         // Find the callee (LINKAGE) and value args (skip block + linkage).
         ident_t callee = NULL;
         mir_value_t vargs[8]; int nv = 0;
         for (int i = 0; i < na; i++) {
            mir_value_t a = mir_get_arg(mu, node, i);
            if (a.tag == MIR_TAG_LINKAGE) callee = mir_get_name(mu, a);
            else if (a.tag != MIR_TAG_BLOCK && nv < 8) vargs[nv++] = a;
         }
         const char *cn = callee ? istr(callee) : "";
         const bool isstop = strstr(cn, "STD.ENV.STOP") != NULL;
         const bool isfin  = strstr(cn, "STD.ENV.FINISH") != NULL;
         if (isstop || isfin) {
            // std.env.stop/finish: report "<STOP|FINISH> called[ with status N]"
            // then halt -- matches nvc's _std_env_stop (which aborts the sim).
            const char *word = isfin ? "FINISH" : "STOP";
            // the INTEGER overload carries a status value arg (the last one)
            const bool have_status = strstr(cn, "(I)") || strstr(cn, "INTEGER") || nv > 1;
            if (have_status && nv > 0) {
               char lbl[64]; snprintf(lbl, sizeof lbl, "%s called with status \001I", word);
               fprintf(f, "   { int64_t _a[] = {(int64_t)(");
               cppgen_val(f, mu, vargs[nv - 1]);
               fprintf(f, ")}; ldx_io_emit(hal, %u, _a, 1); }\n", cppgen_intern2(lbl, 'F'));
            }
            else {
               char lbl[32]; snprintf(lbl, sizeof lbl, "%s called", word);
               fprintf(f, "   ldx_io_emit(hal, %u, 0, 0);\n", cppgen_intern2(lbl, 'L'));
            }
            fprintf(f, "   ldx_fail(hal);\n");
         }
         break;
      }

   default:
      if (cppgen_assign(f, c, node)) {
         cppgen_stub_val(f, c, node); fprintf(f, "; // TODO %s\n", mir_op_string(op));
      }
      else fprintf(f, "; // TODO %s\n", mir_op_string(op));
      break;
   }
#undef ARG
}

static void cppgen_process(FILE *f, mir_unit_t *mu, ident_t name, const char *csym)
{
   cppgen_ctx_t c = { .mu = mu };
   cppgen_build_maps(&c);

   fprintf(f, "// ===== process %s =====\n", istr(name));

   const int nvars = mir_count_vars(mu);
   fprintf(f, "typedef struct {\n   void   *__ctx;\n   int32_t __state;\n");
   for (int i = 0; i < nvars; i++) {
      mir_value_t var = mir_get_var(mu, i);
      ident_t vn = mir_get_name(mu, var);
      mir_type_t vt = mir_get_var_type(mu, var);
      if (cppgen_is_composite(mu, vt))   // record/array: real inline byte storage
         fprintf(f, "   unsigned char v%u[%d];", var.id, cppgen_sizeof(mu, vt));
      else
         fprintf(f, "   %s v%u;", cppgen_ctype(mu, vt), var.id);
      if (vn != NULL) fprintf(f, "   // %s", istr(vn));
      fprintf(f, "\n");
   }
   fprintf(f, "} %s_state_t;\n\n", csym);

   fprintf(f, "extern \"C\" void %s(%s_state_t *s, ldx_hal_t *hal, int32_t __resume)\n{\n",
           csym, csym);
   cppgen_emit_temps(f, &c);

   const int nblocks = mir_count_blocks(mu);
   mir_block_t b0 = mir_get_block(mu, 0);
   fprintf(f, "\n   if (__resume == 0) goto BB%u;\n   switch (s->__state) {\n", b0.id);
   bool *have = xcalloc_array(nblocks + 2, sizeof(bool));
   for (int b = 0; b < nblocks; b++) {
      mir_block_t blk = mir_get_block(mu, b);
      const int nn = mir_count_nodes(mu, blk);
      if (nn == 0) continue;
      mir_value_t last = mir_get_node(mu, blk, nn - 1);
      const mir_op_t lop = mir_get_op(mu, last);
      unsigned tgt = UINT_MAX;
      if (lop == MIR_OP_WAIT || lop == MIR_OP_PCALL) tgt = mir_get_arg(mu, last, 0).id;
      else if (lop == MIR_OP_RETURN)                 tgt = mir_get_block(mu, 1).id;
      if (tgt != UINT_MAX && tgt <= (unsigned)(nblocks + 1) && !have[tgt]) {
         fprintf(f, "   case %u: goto BB%u;\n", tgt, tgt);
         have[tgt] = true;
      }
   }
   fprintf(f, "   default: return;\n   }\n");
   free(have);

   for (int b = 0; b < nblocks; b++) {
      mir_block_t blk = mir_get_block(mu, b);
      fprintf(f, " BB%u:\n", blk.id);
      const int nn = mir_count_nodes(mu, blk);
      for (int n = 0; n < nn; n++) {
         mir_value_t node = mir_get_node(mu, blk, n);
         const loc_t *loc = mir_get_loc(mu, node);
         if (loc != NULL && loc->first_line != LINE_INVALID)
            fprintf(f, "#line %u \"%s\"\n", loc->first_line, loc_file_str(loc));
         cppgen_lower_node(f, &c, node);
      }
   }
   fprintf(f, "}\n\n");

   cppgen_free_maps(&c);
}

// Emit a referenced VHDL function as a plain C++ function (scalar result; params
// p0.., locals v0..). decl_only -> just the prototype (forward declaration).
static void cppgen_function(FILE *f, mir_unit_t *mu, const char *csym, bool decl_only)
{
   cppgen_ctx_t c = { .mu = mu };
   cppgen_build_maps(&c);

   const int nparams = mir_count_params(mu);
   const mir_type_t res = mir_get_result(mu);
   const char *rct = mir_is_null(res) ? "void" : cppgen_ctype(mu, res);
   fprintf(f, "%s %s(ldx_hal_t *hal", rct, csym);
   for (int i = 0; i < nparams; i++) {
      mir_value_t p = mir_get_param(mu, i);
      fprintf(f, ", %s p%u", cppgen_ctype(mu, mir_get_type(mu, p)), p.id);
   }
   fprintf(f, ")");
   if (decl_only) { fprintf(f, ";\n"); cppgen_free_maps(&c); return; }

   fprintf(f, "\n{\n");
   const int nvars = mir_count_vars(mu);
   for (int i = 0; i < nvars; i++) {
      mir_value_t var = mir_get_var(mu, i);
      mir_type_t vt = mir_get_var_type(mu, var);
      if (cppgen_is_composite(mu, vt))
         fprintf(f, "   unsigned char v%u[%d];\n", var.id, cppgen_sizeof(mu, vt));
      else
         fprintf(f, "   %s v%u;\n", cppgen_ctype(mu, vt), var.id);
   }
   cppgen_emit_temps(f, &c);

   g_infunc = true;
   const int nblocks = mir_count_blocks(mu);
   for (int b = 0; b < nblocks; b++) {
      mir_block_t blk = mir_get_block(mu, b);
      fprintf(f, " BB%u: ;\n", blk.id);
      const int nn = mir_count_nodes(mu, blk);
      for (int n = 0; n < nn; n++) {
         mir_value_t node = mir_get_node(mu, blk, n);
         const loc_t *loc = mir_get_loc(mu, node);
         if (loc != NULL && loc->first_line != LINE_INVALID)
            fprintf(f, "#line %u \"%s\"\n", loc->first_line, loc_file_str(loc));
         cppgen_lower_node(f, &c, node);
      }
   }
   g_infunc = false;
   fprintf(f, mir_is_null(res) ? "   return;\n}\n\n" : "   return 0;\n}\n\n");
   cppgen_free_maps(&c);
}

// --------------------------------------------------------------------------
// Instance lowering: signal creation + context var wiring
// --------------------------------------------------------------------------
static void cppgen_instance(FILE *f, mir_unit_t *mu, ident_t name, const char *csym)
{
   cppgen_ctx_t c = { .mu = mu };
   cppgen_build_maps(&c);

   const int nvars = mir_count_vars(mu);
   // var id -> declaration index (slot in vars[])
   c.vmapmax = 0;
   for (int i = 0; i < nvars; i++) {
      mir_value_t v = mir_get_var(mu, i);
      if ((int)v.id > c.vmapmax) c.vmapmax = v.id;
   }
   c.vmap = xcalloc_array(c.vmapmax + 1, sizeof(int));
   for (int i = 0; i < nvars; i++)
      c.vmap[mir_get_var(mu, i).id] = i;

   fprintf(f, "// ===== instance %s =====\n", istr(name));
   fprintf(f, "struct %s_inst { void *parent; void *vars[%d]; };\n",
           csym, nvars > 0 ? nvars : 1);
   fprintf(f, "static void %s_init(%s_inst *self, void *parent, ldx_hal_t *hal)\n{\n",
           csym, csym);
   fprintf(f, "   self->parent = parent;\n");
   cppgen_emit_temps(f, &c);

   const int nblocks = mir_count_blocks(mu);
   for (int b = 0; b < nblocks; b++) {
      mir_block_t blk = mir_get_block(mu, b);
      const int nn = mir_count_nodes(mu, blk);
      for (int n = 0; n < nn; n++) {
         mir_value_t node = mir_get_node(mu, blk, n);
         const loc_t *loc = mir_get_loc(mu, node);
         if (loc != NULL && loc->first_line != LINE_INVALID)
            fprintf(f, "#line %u \"%s\"\n", loc->first_line, loc_file_str(loc));

         const mir_op_t op = mir_get_op(mu, node);
#define ARG(n) cppgen_val(f, mu, mir_get_arg(mu, node, n))
         switch (op) {
         case MIR_OP_INIT_SIGNAL:   // (count, size, value, flags, locus[, off])
            cppgen_assign(f, &c, node);
            fprintf(f, "ldx_init_signal(hal, "); ARG(0); fprintf(f, ", ");
            ARG(1); fprintf(f, ", (int64_t)("); ARG(2); fprintf(f, "), ");
            ARG(3); fprintf(f, ");\n");
            break;

         case MIR_OP_STORE:         // store handle into a context var slot
            {
               mir_value_t dst = mir_get_arg(mu, node, 0);
               if (dst.tag == MIR_TAG_VAR && (int)dst.id <= c.vmapmax) {
                  fprintf(f, "   self->vars[%d] = (void*)(", c.vmap[dst.id]);
                  ARG(1); fprintf(f, ");\n");
               }
               else fprintf(f, "   /* store non-var */\n");
            }
            break;

         case MIR_OP_VAR_UPREF:     // reach the parent instance's slot
            cppgen_assign(f, &c, node);
            fprintf(f, "ldx_var_upref(self->parent, "); ARG(0); fprintf(f, ", ");
            ARG(2); fprintf(f, ");\n");
            break;

         case MIR_OP_LOAD:
            cppgen_assign(f, &c, node);
            fprintf(f, "*(void**)("); ARG(0); fprintf(f, ");\n");
            break;

         case MIR_OP_MAP_SIGNAL:    // (src, dst, count): src drives dst
            fprintf(f, "   ldx_map_signal(hal, (void*)(");
            ARG(0); fprintf(f, "), (void*)("); ARG(1); fprintf(f, "));\n");
            break;

         case MIR_OP_CONST:
            {
               int64_t cv = 0;
               mir_get_const(mu, node, &cv);
               cppgen_assign(f, &c, node);
               fprintf(f, "%"PRIi64";\n", cv);
            }
            break;

         case MIR_OP_JUMP:
            fprintf(f, "   goto BB%u;\n", mir_get_arg(mu, node, 0).id);
            break;

         case MIR_OP_RETURN:
            fprintf(f, "   return;\n");
            break;

         case MIR_OP_ALIAS_SIGNAL:  // handled by the following store
         case MIR_OP_PACKAGE_INIT:
         case MIR_OP_LOCUS:
            if (cppgen_assign(f, &c, node)) fprintf(f, "0; // %s\n", mir_op_string(op));
            else                            fprintf(f, "; // %s\n", mir_op_string(op));
            break;

         default:
            if (cppgen_assign(f, &c, node)) fprintf(f, "0; // TODO %s\n", mir_op_string(op));
            else                            fprintf(f, "; // TODO %s\n", mir_op_string(op));
            break;
         }
#undef ARG
      }
   }
   fprintf(f, "}\n\n");

   cppgen_free_maps(&c);
}

// --------------------------------------------------------------------------
// Driver: emit instances, processes, and ldx_elaborate()
// --------------------------------------------------------------------------
typedef struct {
   ident_t          name;
   mir_unit_t      *mu;
   mir_unit_kind_t  kind;
   ident_t          parent;
   char            *csym;
} cppgen_unit_t;

void cppgen(tree_t top, unit_registry_t *ur, mir_context_t *mc,
            const char *outdir)
{
   assert(tree_kind(top) == T_ELAB);

   hset_t *seen = hset_new(64);
   unit_list_t units = AINIT;
   cppgen_walk_hier(&units, seen, tree_stmt(top, 0));

   const char *topname = istr(tree_ident(top));
   char *path LOCAL = xasprintf("%s/%s.cpp", outdir, topname);
   FILE *f = fopen(path, "w");
   if (f == NULL)
      fatal_errno("cannot create %s", path);

   fprintf(f, "// Generated by nvc cppgen for the ldx RISC-V array.\n");
   fprintf(f, "// Top: %s\n#include \"ldx_hal.h\"\n\n", topname);

   const bool dump = getenv("NVC_CPPGEN_DUMP") != NULL;

   // Resolve units (force lowering) and record kind/parent/csym.
   cppgen_unit_t *info = xcalloc_array(units.count, sizeof(cppgen_unit_t));
   int ninfo = 0;
   for (int i = 0; i < units.count; i++) {
      ident_t nm = units.items[i];
      mir_unit_t *mu = mir_get_unit(mc, nm);
      if (mu == NULL) { (void)unit_registry_get(ur, nm); mu = mir_get_unit(mc, nm); }
      if (mu == NULL) continue;
      const mir_unit_kind_t k = mir_get_kind(mu);
      if (k != MIR_UNIT_PROCESS && k != MIR_UNIT_INSTANCE) continue;
      if (dump) mir_dump(mu);
      info[ninfo++] = (cppgen_unit_t){ nm, mu, k, mir_get_parent(mu), cppgen_csym(nm) };
   }

   // Collect value-returning functions referenced by the design (transitive),
   // then emit them (forward decls first) so the processes can call them.
   g_mc = mc; g_ur = ur;
   for (int i = 0; i < ninfo; i++) cppgen_scan_fcalls(info[i].mu);
   for (unsigned i = 0; i < g_funcs.count; i++) {   // grows as functions are scanned
      mir_unit_t *fu = cppgen_get_func(g_funcs.items[i]);
      if (fu != NULL) cppgen_scan_fcalls(fu);
   }
   if (g_funcs.count > 0) fprintf(f, "// ===== functions =====\n");
   for (unsigned i = 0; i < g_funcs.count; i++) {
      mir_unit_t *fu = cppgen_get_func(g_funcs.items[i]);
      char *cs LOCAL = cppgen_func_sym(g_funcs.items[i]);
      if (fu != NULL) cppgen_function(f, fu, cs, true);
   }
   for (unsigned i = 0; i < g_funcs.count; i++) {
      mir_unit_t *fu = cppgen_get_func(g_funcs.items[i]);
      char *cs LOCAL = cppgen_func_sym(g_funcs.items[i]);
      if (fu != NULL) cppgen_function(f, fu, cs, false);
   }

   // Emit instance and process definitions.
   int nproc = 0, ninst = 0;
   for (int i = 0; i < ninfo; i++) {
      if (info[i].kind == MIR_UNIT_INSTANCE) {
         cppgen_instance(f, info[i].mu, info[i].name, info[i].csym);
         ninst++;
      }
      else {
         cppgen_process(f, info[i].mu, info[i].name, info[i].csym);
         nproc++;
      }
   }

   // ldx_elaborate(): build the design for the runtime.
   fprintf(f, "// ===== elaboration =====\n");
   for (int i = 0; i < ninfo; i++) {
      if (info[i].kind == MIR_UNIT_INSTANCE)
         fprintf(f, "static %s_inst I_%s;\n", info[i].csym, info[i].csym);
      else
         fprintf(f, "static %s_state_t P_%s;\n", info[i].csym, info[i].csym);
   }
   fprintf(f, "\nextern \"C\" void ldx_elaborate(ldx_hal_t *hal)\n{\n");
   // instance init (parent before child — walk order guarantees this)
   for (int i = 0; i < ninfo; i++) {
      if (info[i].kind != MIR_UNIT_INSTANCE) continue;
      const char *parent_vars = "nullptr";
      char buf[256];
      for (int j = 0; j < ninfo; j++) {
         if (info[j].kind == MIR_UNIT_INSTANCE && info[j].name == info[i].parent) {
            snprintf(buf, sizeof(buf), "I_%s.vars", info[j].csym);
            parent_vars = buf;
            break;
         }
      }
      fprintf(f, "   %s_init(&I_%s, %s, hal);\n", info[i].csym, info[i].csym, parent_vars);
   }
   // process context + registration
   for (int i = 0; i < ninfo; i++) {
      if (info[i].kind != MIR_UNIT_PROCESS) continue;
      const char *ctx = "nullptr";
      char buf[256];
      for (int j = 0; j < ninfo; j++) {
         if (info[j].kind == MIR_UNIT_INSTANCE && info[j].name == info[i].parent) {
            snprintf(buf, sizeof(buf), "I_%s.vars", info[j].csym);
            ctx = buf;
            break;
         }
      }
      fprintf(f, "   P_%s.__ctx = %s;\n", info[i].csym, ctx);
      fprintf(f, "   ldx_register_process((ldx_proc_fn)&%s, &P_%s, %s);\n",
              info[i].csym, info[i].csym, ctx);
   }
   fprintf(f, "}\n");

   // Host string table (no strings on the array: host-only). io_emit ships an id;
   // ldx_rt_host prints the literal text for 'L' ids, the value for 'V' ids.
   fprintf(f, "\n#ifndef __riscv\n");
   fprintf(f, "extern \"C\" const char *const ldx_strtab[] = {");
   for (unsigned i = 0; i < g_strings.count; i++) {
      fprintf(f, "%s\"", i ? "," : "");
      for (const char *p = g_strings.items[i]; *p; p++) {
         const unsigned char ch = (unsigned char)*p;
         if (ch == '"' || ch == '\\') fprintf(f, "\\%c", ch);
         else if (ch == '\n') fprintf(f, "\\n");
         else if (ch == '\t') fprintf(f, "\\t");
         else if (ch >= 32 && ch < 127) fputc(ch, f);
         else fprintf(f, "\\%03o", ch);   // octal (3 digits, not greedy like \x)
      }
      fprintf(f, "\"");
   }
   fprintf(f, "%s};\n", g_strings.count ? "" : "0");
   fprintf(f, "extern \"C\" const char ldx_strkind[] = {");
   for (unsigned i = 0; i < g_strkind.count; i++)
      fprintf(f, "%s'%c'", i ? "," : "", g_strkind.items[i]);
   fprintf(f, "%s};\n", g_strkind.count ? "" : "0");
   fprintf(f, "extern \"C\" const unsigned ldx_strtab_n = %u;\n", g_strings.count);
   fprintf(f, "#endif\n");

   fclose(f);

   char *spath LOCAL = xasprintf("%s/strings.idx", outdir);
   FILE *sf = fopen(spath, "w");
   if (sf != NULL) {
      for (unsigned i = 0; i < g_strings.count; i++)
         fprintf(sf, "%u\t%s\n", i, g_strings.items[i]);
      fclose(sf);
   }

   notef("cppgen: wrote %d instance(s) + %d process(es) to %s (%u strings)",
         ninst, nproc, path, g_strings.count);

   for (int i = 0; i < ninfo; i++) free(info[i].csym);
   free(info);
   hset_free(seen);
   ACLEAR(units);
}
