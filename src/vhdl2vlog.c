//
//  vhdl2vlog.c — emit synthesizable Verilog from nvc's elaborated VHDL tree.
//
//  Used by `--accel`: gen_statemachine needs Verilog, but the elaborated design
//  may have come from VHDL (or SV via sv2ghdl). This walks the elaborated
//  T_BLOCK (mirroring dump.c) and writes a Verilog-2001 module so the existing
//  statemachine/cxxrtl path works for VHDL too. First cut: the synthesizable
//  subset (ports, signals, clocked/comb processes, concurrent assigns,
//  instances, operator expressions); unhandled nodes emit a /*?*/ marker so the
//  output is inspectable rather than crashing.
//
#include "util.h"
#include "tree.h"
#include "type.h"
#include "common.h"
#include "ident.h"
#include "vhdl2vlog.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <inttypes.h>

static void emit_expr(FILE *f, tree_t e);
static void emit_seq(FILE *f, tree_t s, int ind);
static bool emit_agg_general(FILE *f, tree_t e);
// Emit a statement list with the while->for counter-loop peephole. _range emits
// stmts [lo,hi); the plain form does the whole container.
static void emit_stmt_list(FILE *f, tree_t container, int ind);
static void emit_stmt_list_range(FILE *f, tree_t container, int lo, int hi, int ind);

// Best-effort fidelity gate: any construct we can't faithfully translate bumps
// this. If it's non-zero at the end, the emitted Verilog is NOT trustworthy and
// the caller must DECLINE to accelerate this leaf (it stays in the nvc sim).
// Emitting wrong-but-parseable Verilog would silently corrupt results — worse
// than not accelerating.
static int g_unhandled = 0;

static void tab(FILE *f, int n) { for (int i = 0; i < n; i++) fputc(' ', f); }

// VHDL identifiers are case-insensitive uppercase; keep them verbatim (Verilog
// is case-sensitive but consistent), just strip any library/path prefix.
// Identifier basename, lowercased + sanitized to a valid Verilog id. Lowercased
// so port/signal names line up with the accel bridge's pins (aj_lower) and the
// gen_statemachine field names; VHDL is case-insensitive so this can't collide.
// Rotating buffers so several vid() in one fprintf don't clobber each other.
static const char *vid(ident_t id)
{
   static char bufs[8][256];
   static unsigned which = 0;
   char *buf = bufs[which++ & 7];
   const char *s = istr(id);
   const char *dot = strrchr(s, '.');
   s = dot ? dot + 1 : s;
   int i = 0;
   for (; s[i] && i < 255; i++) {
      char c = tolower((unsigned char)s[i]);
      buf[i] = (isalnum((unsigned char)c) || c == '_') ? c : '_';
   }
   buf[i] = '\0';
   return buf;
}

// Module name as the accel scan names modules (model.c mod_lower): basename,
// lowercased + sanitized. Instance module-refs MUST use this so a whole-subtree
// flatten resolves child modules (which the accel scan emits under mod_lower).
const char *vhdl2vlog_modname(ident_t id)
{
   static char buf[256];
   const char *s = istr(id);
   const char *dot = strrchr(s, '.');
   s = dot ? dot + 1 : s;
   int i = 0;
   for (; s[i] && i < 255; i++) {
      char c = tolower((unsigned char)s[i]);
      buf[i] = (isalnum((unsigned char)c) || c == '_') ? c : '_';
   }
   buf[i] = '\0';
   return buf;
}

// Streaming FNV-1a, used to fold a block's variant signature into a short hash.
static void fnv_str(uint64_t *h, const char *s)
{
   for (; *s; s++) { *h ^= (unsigned char)*s; *h *= 0x100000001b3ULL; }
}
static void fnv_i64(uint64_t *h, int64_t v)
{
   char b[24]; snprintf(b, sizeof b, "%"PRIi64, v); fnv_str(h, b);
}

// Per-(entity, generic-actuals) Verilog module name for the whole-subtree accel.
// Two instances of the SAME generic entity but DIFFERENT generic actuals (e.g.
// sv_or n=2 vs n=4 -> different port widths AND loop bounds) are DISTINCT modules
// — deduping by entity name alone would instantiate the wrong-width module, which
// is SILENTLY WRONG for width-sensitive logic (a reduction over the wrong number
// of bits, a mis-sized shift, ...). The name = lowered entity + a hash of the
// generic actual values and the port widths/directions, computed from the block
// alone so it is IDENTICAL at the instantiation site (emit_stmt, child block in
// hand) and the definition site (emit_subtree_v / accel_install_subtree, same
// tree_t). A non-generic entity keeps its plain name (all its instances match).
// Mirrors elab.c's own variant key (elab_hash_vhdl_generic).
const char *vhdl2vlog_variant_name(ident_t entity_id, tree_t block)
{
   static char bufs[8][320];
   static unsigned which = 0;
   char *buf = bufs[which++ & 7];

   char base[256];
   snprintf(base, sizeof base, "%s", vhdl2vlog_modname(entity_id));

   // Escape hatch / A-B toggle: restore the old dedup-by-entity behaviour (which
   // collapses generic-width variants onto one module — silently wrong for
   // width-sensitive logic).
   const int ng = (block != NULL && tree_kind(block) == T_BLOCK
                   && !getenv("NVC_ACCEL_NO_VARIANT"))
                  ? tree_generics(block) : 0;
   if (ng == 0) { snprintf(buf, sizeof bufs[0], "%s", base); return buf; }

   uint64_t h = 0xcbf29ce484222325ULL;
   fnv_str(&h, base);

   // generic actuals: post-elaboration tree_genmaps is 1:1 positional with
   // tree_generics; the actual is tree_value(genmap). Branch on tree_class first
   // (a non-C_CONSTANT generic has no scalar value — key it by identity).
   for (int i = 0; i < ng; i++) {
      tree_t g = tree_generic(block, i);
      tree_t m = tree_genmap(block, i);
      fnv_str(&h, istr(tree_ident(g)));
      const class_t cls = tree_class(g);
      tree_t v = tree_value(m);
      if (cls != C_CONSTANT) {
         if (cls == C_TYPE) {
            type_t vt = tree_type(v);   // type_ident FATALs on an anon non-subtype
            if (type_has_ident(vt)) fnv_str(&h, istr(type_ident(vt)));
            else fnv_i64(&h, (int64_t)(intptr_t)vt);
         }
         else fnv_i64(&h, (int64_t)(intptr_t)tree_ref(v));   // pkg/func/proc
         continue;
      }
      int64_t iv; double dv; bool bv;
      if (folded_bool(v, &bv))            { fnv_str(&h, "b"); fnv_i64(&h, bv); }
      else if (folded_int(v, &iv))        { fnv_str(&h, "i"); fnv_i64(&h, iv); }
      else if (folded_real(v, &dv))       {
         // fold the EXACT IEEE-754 bits (like elab.c's FLOAT_BITS); a scaled int
         // would collide nearby reals and overflow for large magnitudes.
         union { double d; uint64_t u; } cvt = { .d = dv };
         fnv_str(&h, "r"); fnv_i64(&h, (int64_t)cvt.u);
      }
      else if (tree_kind(v) == T_REF
               && tree_kind(tree_ref(v)) == T_ENUM_LIT)
                                          { fnv_str(&h, "e"); fnv_i64(&h, tree_pos(tree_ref(v))); }
      else                                { fnv_str(&h, "o"); fnv_i64(&h, (int64_t)(intptr_t)v); }
   }

   // port widths + directions: the dominant manifestation of a generic, and the
   // thing a width-mismatched instantiation would corrupt. Guard type_width — it
   // FATALs on unconstrained / non-static bounds; fold the type identity for
   // those so distinct unconstrained/dynamic ports still differ.
   const int np = tree_ports(block);
   for (int i = 0; i < np; i++) {
      tree_t p = tree_port(block, i);
      type_t t = tree_type(p);
      fnv_str(&h, istr(tree_ident(p)));
      fnv_i64(&h, (int64_t)tree_subkind(p));
      if (type_is_unconstrained(t) || !type_const_bounds(t)) {
         fnv_str(&h, type_is_unconstrained(t) ? "U" : "D");
         if (type_has_ident(t)) fnv_str(&h, istr(type_ident(t)));
      }
      else fnv_i64(&h, (int64_t)type_width(t));
   }

   // full 64-bit hash in the name (and dedup key): truncating to 32 bits bought
   // nothing and left a (small but real) birthday-collision -> silent-merge channel.
   snprintf(buf, sizeof bufs[0], "%s_g%016"PRIx64, base, h);
   return buf;
}

static bool type_is_logic3d(type_t t);   // defined below

// bit width of a type as a Verilog range prefix ("[3:0] " or "" for 1-bit)
static void emit_range(FILE *f, type_t type)
{
   if (type_is_logic3d(type) && !type_is_array(type))
      return;   // scalar logic3d -> 1-bit value (bit0), no range
   if (type_is_array(type)) {
      const unsigned w = type_width(type);
      if (w > 1) fprintf(f, "[%u:0] ", w - 1);
   }
   else if (type_is_integer(type))
      fprintf(f, "[31:0] ");
   // std_logic / boolean / enum -> single bit, no range
}

// map a VHDL operator/function call to a Verilog operator
static const char *vlog_op(const char *fn)
{
   struct { const char *v; const char *o; } map[] = {
      {"\"+\"","+"}, {"\"-\"","-"}, {"\"*\"","*"},
      {"\"and\"","&"}, {"\"or\"","|"}, {"\"xor\"","^"}, {"\"nand\"","~&"},
      {"\"nor\"","~|"}, {"\"xnor\"","~^"}, {"\"not\"","~"},
      {"\"=\"","=="}, {"\"/=\"","!="}, {"\"<\"","<"}, {"\">\"",">"},
      {"\"<=\"","<="}, {"\">=\"",">="},
      {"\"sll\"","<<"}, {"\"srl\"",">>"},
      {NULL,NULL}
   };
   for (int i = 0; map[i].v; i++)
      if (strcmp(fn, map[i].v) == 0) return map[i].o;
   return NULL;
}

// basename of a (possibly library/package-qualified) identifier
static const char *id_base(const char *nm)
{
   const char *dot = strrchr(nm, '.');
   return dot ? dot + 1 : nm;
}

// Is this the sv2vhdl logic3d value type (scalar or array element)?  logic3d
// models a 3-state bit as natural 0..7 (bit0=value, bit1=driven, bit2=X); for
// --accel we keep ONLY bit0 -- which is exactly Verilator's 2-state value, the
// representation the reference passes these tests with.
static bool type_is_logic3d(type_t t)
{
   for (int i = 0; i < 8 && t != NULL; i++) {
      if (!strcasecmp(id_base(istr(type_ident(t))), "LOGIC3D")) return true;
      if (type_is_array(t)) { t = type_elem(t); continue; }
      if (type_kind(t) != T_SUBTYPE) break;
      t = type_base(t);
   }
   return false;
}

// True for an ieee.numeric_std `signed` (sub)type. Verilog erases VHDL
// signedness, so a signed relational/arithmetic op would otherwise emit as a
// bare unsigned operator and yosys builds the cell with A_SIGNED=0 (negatives
// compare/extend as raw bit patterns). Detecting it lets emit_expr wrap the
// operands in $signed so yosys sets A_SIGNED and the synth model sign-extends.
static bool type_is_signed(type_t t)
{
   for (int i = 0; i < 8 && t != NULL; i++) {
      if (!strcasecmp(id_base(istr(type_ident(t))), "SIGNED")) return true;
      if (type_kind(t) != T_SUBTYPE) break;
      t = type_base(t);
   }
   return false;
}

// Map an sv2vhdl logic3d package function to a Verilog operator on value bits.
// *kind: 0=binary "(a op b)", 1=unary-prefix "(op a)", 2=identity "a",
// 3=reduction "(op a)". Returns NULL if not a known logic3d op.
static const char *vlog_l3d_op(const char *fn, int *kind)
{
   const char *b = id_base(fn);
   static const struct { const char *n; const char *o; int k; } m[] = {
      {"l3d_and","&",0}, {"l3d_or","|",0}, {"l3d_xor","^",0},
      {"l3d_nand","~&",0}, {"l3d_nor","~|",0}, {"l3d_xnor","~^",0},
      {"l3d_not","~",1}, {"is_zero","~",1}, {"l3d_not_l","~",1},
      {"is_one","",2}, {"to_integer","",2}, {"l3d_to_unsigned","",2},
      {"unsigned_to_l3d","",2}, {"boolean_to_logic","",2}, {"to_logic3d","",2},
      {"unsigned_to_l3d_bit","",2}, {"l3d_to_bit","",2}, {"to_bit","",2},
      // width adjusters: Verilog's assignment context resizes -- emit the value
      {"resize","",2}, {"to_unsigned","",2}, {"to_signed","",2},
      {"reduce_or","|",3}, {"reduce_and","&",3}, {"reduce_xor","^",3},
      {NULL,NULL,0}
   };
   for (int i = 0; m[i].n; i++)
      if (!strcasecmp(b, m[i].n)) { *kind = m[i].k; return m[i].o; }
   return NULL;
}

static void emit_lit(FILE *f, tree_t e)
{
   int64_t i;
   if (folded_int(e, &i)) {
      // A folded logic3d constant (L3D_0=2, L3D_1=3, ...) carries its 2-state
      // value in bit0 -- emit that, not the raw 0..7 encoding.
      if (tree_has_type(e) && type_is_logic3d(tree_type(e)))
         fprintf(f, "1'b%d", (int)(i & 1));
      else
         fprintf(f, "%"PRIi64, i);
      return;
   }
   // std_logic enum literal '0'/'1' etc.
   const char *s = istr(tree_ident(e));
   if (strcmp(s, "'0'") == 0) fputs("1'b0", f);
   else if (strcmp(s, "'1'") == 0) fputs("1'b1", f);
   else { g_unhandled++; fprintf(f, "/*lit %s*/0", s); }
}

// General 1-D bit-vector aggregate: NAMED / RANGE / positional / OTHERS (mixed),
// each element ONE bit. Builds a W-slot array, fills it from the associations
// using the target array's own index range + direction, then emits
// {slot[0]..slot[W-1]} (slot[0] = leftmost = MSB, matching emit_range's [W-1:0]
// and the existing positional path). nvc places element values at offset
// off = is_downto ? (left-idx) : (idx-left) (lower_const_array_aggregate), and a
// positional assoc k -> off=k. Returns false (caller declines) for anything
// outside this shape: not 1-D, non-const bounds, multi-bit/array element,
// A_SLICE/A_CONCAT (multi-bit values), an index/range that won't fold, or an
// unfilled slot with no `others`.
static bool emit_agg_general(FILE *f, tree_t e)
{
   type_t at = tree_type(e);
   if (!type_is_array(at) || !type_const_bounds(at) || dimension_of(at) != 1)
      return false;
   type_t et = type_elem(at);
   if (type_is_array(et) || type_width(et) != 1)
      return false;
   tree_t r = range_of(at, 0);
   int64_t left, right;
   if (!folded_int(tree_left(r), &left) || !folded_int(tree_right(r), &right))
      return false;
   const bool is_downto = (tree_subkind(r) == RANGE_DOWNTO);
   const int W = (int)type_width(at);
   // self-consistent width cap (type_synth_ok's RAM guard only covers declared
   // object types, not an arbitrary aggregate's type): a huge aggregate would
   // xcalloc a multi-MB slot array and emit an enormous concat.
   if (W <= 0 || W > (1 << 16)) return false;

   tree_t *slot = xcalloc_array(W, sizeof(tree_t));   // NULL = unfilled
   tree_t others = NULL;
   bool have_others = false, bad = false;
   int pos = 0;                                       // positional counter, 0 = left
   const int n = tree_assocs(e);
   for (int i = 0; i < n && !bad; i++) {
      tree_t a = tree_assoc(e, i);
      switch (tree_subkind(a)) {
      case A_POS:
         if (pos < W) slot[pos] = tree_value(a); else bad = true;
         pos++;
         break;
      case A_NAMED: {
         int64_t idx;
         if (!folded_int(tree_name(a), &idx)) { bad = true; break; }
         const int64_t off = is_downto ? (left - idx) : (idx - left);
         if (off >= 0 && off < W) slot[(int)off] = tree_value(a); else bad = true;
         break;
      }
      case A_RANGE: {
         tree_t rr = tree_range(a, 0);
         int64_t rl, rh;
         if (!folded_int(tree_left(rr), &rl) || !folded_int(tree_right(rr), &rh)) {
            bad = true; break;
         }
         int64_t lo, hi; range_bounds(rr, &lo, &hi);
         for (int64_t j = lo; j <= hi && !bad; j++) {
            const int64_t off = is_downto ? (left - j) : (j - left);
            if (off >= 0 && off < W) slot[(int)off] = tree_value(a); else bad = true;
         }
         break;
      }
      case A_OTHERS:
         others = tree_value(a); have_others = true;
         break;
      default:                                        // A_SLICE / A_CONCAT
         bad = true; break;
      }
   }
   for (int k = 0; k < W && !bad; k++)
      if (slot[k] == NULL) { if (have_others) slot[k] = others; else bad = true; }

   if (!bad) {
      // Emit MSB-first so VHDL index N lands at Verilog bit N — matching the
      // read paths (T_ARRAY_REF emits sig[idx] with the raw index; emit_range
      // declares [W-1:0]). slot[0] is the LEFTMOST element: for DOWNTO that is
      // the MSB (emit slot[0] first), but for a TO/ascending vector the leftmost
      // is index `left` (the LOW index) -> it must go to the LSB, so emit slots
      // in reverse. Without this, a TO aggregate consumed by an indexed read is
      // silently bit-reversed.
      fputc('{', f);
      for (int b = 0; b < W; b++) {
         if (b) fputs(", ", f);
         emit_expr(f, slot[is_downto ? b : (W - 1 - b)]);
      }
      fputc('}', f);
   }
   free(slot);
   return !bad;
}

static void emit_expr(FILE *f, tree_t e)
{
   if (e == NULL) {   // e.g. a null/unaffected waveform value — decline, don't crash
      g_unhandled++; fputs("/*null*/0", f);
      return;
   }
   switch (tree_kind(e)) {
   case T_REF:
      {
         const char *nm = vid(tree_ident(e));
         const char *bn = id_base(istr(tree_ident(e)));
         // logic3d constants L3D_0/L3D_1/L3D_0X/... -> value bit (2-state)
         if (strncasecmp(bn, "L3D_", 4) == 0)
            fputs(bn[4] == '1' ? "1'b1" : "1'b0", f);
         else if (strcmp(bn, "'0'") == 0) fputs("1'b0", f);   // raw basename: vid() now sanitizes '0'->_0_
         else if (strcmp(bn, "'1'") == 0) fputs("1'b1", f);
         else {
            // Ports/signals/process-vars ARE declared in the emitted module, so a
            // bare name is correct. An architecture-level CONSTANT is NOT declared
            // -> a bare name is an undefined Verilog wire (silent wrong value, and
            // the chunk still installs). Inline its static value instead; decline
            // anything else non-declared (e.g. an unfolded generic ref).
            tree_t ref = tree_has_ref(e) ? tree_ref(e) : NULL;
            const tree_kind_t rk = ref != NULL ? tree_kind(ref) : T_REF;
            if (rk == T_CONST_DECL && tree_has_value(ref))
               emit_expr(f, tree_value(ref));
            else if (rk == T_CONST_DECL || rk == T_GENERIC_DECL) {
               g_unhandled++; fprintf(f, "/*ref %s*/0", bn);
            }
            else fputs(nm, f);
         }
      }
      break;
   case T_LITERAL:
      emit_lit(f, e);
      break;
   case T_STRING:
      {
         // std_logic_vector / bit_vector literal ("0010", x"07") -> Verilog sized
         // binary literal of the value bits, MSB-first (char 0 = leftmost). Each
         // char is a bit-like enum lit "'0'"/"'1'"/...; decline 2-state metavalues
         // (X/Z/U/W/-) rather than emit a wrong bit.
         const int n = tree_chars(e);
         if (n <= 0) { g_unhandled++; fputs("/*str0*/0", f); break; }
         fprintf(f, "%d'b", n);
         for (int i = 0; i < n; i++) {
            ident_t rune = tree_ident(tree_char(e, i));
            // guard the index-1 read on length too (ident_char asserts n<len):
            // a malformed/short rune declines rather than aborting the sim.
            const char c = (ident_len(rune) >= 2 && ident_char(rune, 0) == '\'')
                           ? ident_char(rune, 1) : '?';
            if (c == '1' || c == 'H') fputc('1', f);
            else if (c == '0' || c == 'L') fputc('0', f);
            else { g_unhandled++; fputc('0', f); }
         }
      }
      break;
   case T_FCALL:
      {
         const char *fn = istr(tree_ident(e));
         const int nparams = tree_params(e);
         // VHDL concatenation "&" -> Verilog {a, b}
         if (strcmp(fn, "\"&\"") == 0 && nparams == 2) {
            fputc('{', f);
            emit_expr(f, tree_value(tree_param(e, 0)));
            fputs(", ", f);
            emit_expr(f, tree_value(tree_param(e, 1)));
            fputc('}', f);
            break;
         }
         const char *op = vlog_op(fn);
         int l3dk = -1;
         const char *l3dop = (op == NULL) ? vlog_l3d_op(fn, &l3dk) : NULL;
         if (op != NULL && nparams == 2) {
            // If either operand is numeric_std `signed`, wrap both in $signed so
            // yosys builds a signed cell (arithmetic/compare sign-extends). Bitwise
            // ops are unaffected by $signed, so blanket-wrapping is safe.
            tree_t a0 = tree_value(tree_param(e, 0)), a1 = tree_value(tree_param(e, 1));
            const bool sgn =
               (tree_has_type(a0) && type_is_signed(tree_type(a0)))
               || (tree_has_type(a1) && type_is_signed(tree_type(a1)));
            fputc('(', f);
            if (sgn) fputs("$signed(", f);
            emit_expr(f, a0);
            if (sgn) fputc(')', f);
            fprintf(f, " %s ", op);
            if (sgn) fputs("$signed(", f);
            emit_expr(f, a1);
            if (sgn) fputc(')', f);
            fputc(')', f);
         }
         else if (op != NULL && nparams == 1) {
            const bool sgn = (tree_has_type(tree_value(tree_param(e, 0)))
                              && type_is_signed(tree_type(tree_value(tree_param(e, 0)))));
            fprintf(f, "%s(", op);
            if (sgn) fputs("$signed(", f);
            emit_expr(f, tree_value(tree_param(e, 0)));
            if (sgn) fputc(')', f);
            fputc(')', f);
         }
         else if (l3dop != NULL && l3dk == 0 && nparams == 2) {
            fputc('(', f);
            emit_expr(f, tree_value(tree_param(e, 0)));
            fprintf(f, " %s ", l3dop);
            emit_expr(f, tree_value(tree_param(e, 1)));
            fputc(')', f);
         }
         else if (l3dop != NULL && (l3dk == 1 || l3dk == 3) && nparams >= 1) {
            fprintf(f, "(%s", l3dop);
            emit_expr(f, tree_value(tree_param(e, 0)));
            fputc(')', f);
         }
         else if (l3dop != NULL && l3dk == 2 && nparams >= 1) {
            emit_expr(f, tree_value(tree_param(e, 0)));   // identity on value bit
         }
         else {
            // rising_edge / unhandled function: surface for inspection
            g_unhandled++;
            fprintf(f, "/*fn %s*/", vid(tree_ident(e)));
            if (nparams > 0) emit_expr(f, tree_value(tree_param(e, 0)));
         }
      }
      break;
   case T_TYPE_CONV:
   case T_QUALIFIED:
   case T_INERTIAL:   // inertial-delay waveform wrapper -> inner value
      emit_expr(f, tree_value(e));   // numeric/std_logic_vector casts: no-op in Verilog
      break;
   case T_ARRAY_REF:
      {
         tree_t base = tree_value(e);
         // Verilog cannot bit-select a LITERAL, and an inlined constant emits as
         // one (`8'b..[i]` is a syntax error). For a constant base use shift+mask
         // -- which is exactly what yosys lowers a bit-select to anyway, and it
         // unrolls cleanly when the index is a for-loop variable.
         const bool const_base = (tree_kind(base) == T_REF && tree_has_ref(base)
                                  && tree_kind(tree_ref(base)) == T_CONST_DECL);
         if (const_base && tree_params(e) == 1) {
            fputs("(((", f); emit_expr(f, base); fputs(") >> (", f);
            emit_expr(f, tree_value(tree_param(e, 0))); fputs(")) & 1'b1)", f);
         }
         else {
            emit_expr(f, base);
            if (tree_params(e) > 0) {
               fputc('[', f);
               emit_expr(f, tree_value(tree_param(e, 0)));
               fputc(']', f);
            }
         }
      }
      break;
   case T_ARRAY_SLICE:
      {
         tree_t r = tree_range(e, 0);
         emit_expr(f, tree_value(e));
         fputc('[', f);
         if (tree_subkind(r) == RANGE_TO) {   // ascending: Verilog wants [hi:lo]
            emit_expr(f, tree_right(r)); fputc(':', f); emit_expr(f, tree_left(r));
         } else {                             // downto: [left:right] = [hi:lo]
            emit_expr(f, tree_left(r)); fputc(':', f); emit_expr(f, tree_right(r));
         }
         fputc(']', f);
      }
      break;
   case T_AGGREGATE:
      {
         // logic3d_vector constant '(L3D_0, L3D_1, ...) -> Verilog binary
         // literal of the value bits (positional, MSB-first for downto).
         if (type_is_array(tree_type(e))) {   // logic3d_vector OR std_logic_vector etc.
            const int n = tree_assocs(e);
            // (others => <single-bit expr>) -> {W{(expr)}}. A logic3d_vector/
            // std_logic_vector element is one bit, so any scalar fill -- a
            // constant bit OR a signal/expression bit -- replicates across the
            // whole vector. (Was const-bit-only, which declined `(others => sig)`
            // -- the dominant aggregate in VeeR datapath modules.)
            if (n == 1 && tree_subkind(tree_assoc(e, 0)) == A_OTHERS) {
               const int w = type_width(tree_type(e));
               if (w > 0) {
                  fprintf(f, "{%d{", w);
                  emit_expr(f, tree_value(tree_assoc(e, 0)));
                  fputs("}}", f);
                  break;
               }
            }
            // all positional/concat -> Verilog concatenation {e0, e1, ...}. A_POS
            // elements are single bits; A_CONCAT pieces are multi-bit sub-arrays
            // (VHDL `a & b & c` elaborates to a concat aggregate — the dominant
            // declining form in VeeR) and Verilog {} concatenates them directly.
            // Each is emit_expr'd; MSB-first matches downto/source order.
            bool all_seq = (n > 0);
            for (int i = 0; i < n; i++) {
               const assoc_kind_t k = tree_subkind(tree_assoc(e, i));
               if (k != A_POS && k != A_CONCAT) { all_seq = false; break; }
            }
            if (all_seq) {
               fputc('{', f);
               for (int i = 0; i < n; i++) {
                  if (i) fputs(", ", f);
                  emit_expr(f, tree_value(tree_assoc(e, i)));
               }
               fputc('}', f);
               break;
            }
            // named / indexed / range / mixed-with-others (1-bit elements)
            if (emit_agg_general(f, e)) break;
         }
         // Not faithfully translated — decline rather than silently emit 0.
         g_unhandled++;
         if (getenv("GSM_LOG")) {
            const int nn = tree_assocs(e);
            tree_t a0 = nn > 0 ? tree_assoc(e, 0) : NULL;
            tree_t v0 = (a0 && tree_has_value(a0)) ? tree_value(a0) : NULL;
            fprintf(stderr, "AGG-DECLINE n=%d s0=%d vk0=%d l3d=%d\n", nn,
                    a0 ? tree_subkind(a0) : -1, v0 ? tree_kind(v0) : -1,
                    type_is_logic3d(tree_type(e)));
         }
         fputs("/*agg*/0", f);
      }
      break;
   default:
      g_unhandled++; fprintf(f, "/*?expr k=%d*/0", tree_kind(e));
      break;
   }
}

// Extract clock/reset edges from an if-condition: a single rising/falling_edge,
// or an OR of them (async-reset flop, translated from `always @(posedge clk or
// negedge rst)`). Fills sig[]/pe[] (pe = posedge), returns edge count or 0.
static int edges_of(tree_t test, tree_t *sig, bool *pe, int max)
{
   if (test == NULL || tree_kind(test) != T_FCALL || max < 1) return 0;
   const char *fn = id_base(istr(tree_ident(test)));
   if (!strcasecmp(fn, "RISING_EDGE") || !strcasecmp(fn, "FALLING_EDGE")) {
      if (tree_params(test) < 1) return 0;
      sig[0] = tree_value(tree_param(test, 0));
      pe[0]  = !strcasecmp(fn, "RISING_EDGE");
      return 1;
   }
   // OR of two edge tests (boolean "or" or logic3d l3d_or)
   if (!strcasecmp(fn, "\"or\"") || !strcasecmp(fn, "l3d_or")) {
      if (tree_params(test) < 2) return 0;
      int a = edges_of(tree_value(tree_param(test, 0)), sig, pe, max);
      if (a == 0) return 0;
      int b = edges_of(tree_value(tree_param(test, 1)), sig + a, pe + a, max - a);
      if (b == 0) return 0;
      return a + b;
   }
   return 0;
}

// detect a clocked process: a wrapping `if rising_edge(clk) [or falling_edge(rst)]`.
// Returns the wrapping cond (non-NULL = clocked), fills body_if + the edge list.
static tree_t clock_of(tree_t proc, tree_t *body_if, tree_t *sig, bool *pe, int *ne)
{
   const int n = tree_stmts(proc);
   for (int i = 0; i < n; i++) {
      tree_t s = tree_stmt(proc, i);
      if (tree_kind(s) != T_IF) continue;
      tree_t c = tree_cond(s, 0);
      tree_t test = tree_has_value(c) ? tree_value(c) : NULL;
      int k = edges_of(test, sig, pe, 8);
      if (k > 0) { *body_if = c; *ne = k; return c; }
   }
   return NULL;
}

static void emit_seq(FILE *f, tree_t s, int ind)
{
   switch (tree_kind(s)) {
   case T_SIGNAL_ASSIGN:
   case T_VAR_ASSIGN:
      {
         tab(f, ind);
         emit_expr(f, tree_target(s));
         // VHDL variables update immediately (Verilog blocking '='); signals are
         // scheduled (non-blocking '<='). A reduction-loop accumulator is a
         // variable and MUST be blocking or each iteration reads the stale value.
         fputs(tree_kind(s) == T_VAR_ASSIGN ? " = " : " <= ", f);
         if (tree_kind(s) == T_SIGNAL_ASSIGN && tree_waveforms(s) > 0)
            emit_expr(f, tree_value(tree_waveform(s, 0)));
         else
            emit_expr(f, tree_value(s));
         fputs(";\n", f);
      }
      break;
   case T_FOR:
      {
         // for i in <range> loop ... end loop;  ->  Verilog procedural for.
         // After elaboration the range is folded to explicit bounds (generics
         // resolved), so tree_left/tree_right are constants. The index variable
         // (tree_decl 0) is hoisted to a module-level `integer` by the caller.
         tree_t r = tree_range(s, 0);
         tree_t idecl = tree_decls(s) > 0 ? tree_decl(s, 0) : NULL;
         const char *iv = idecl ? vid(tree_ident(idecl)) : "i";
         const bool to = (tree_subkind(r) == RANGE_TO);
         if (tree_subkind(r) != RANGE_TO && tree_subkind(r) != RANGE_DOWNTO) {
            g_unhandled++; tab(f, ind);
            fprintf(f, "/*?for-range sk=%d*/\n", tree_subkind(r));
            break;
         }
         tab(f, ind);
         fprintf(f, "for (%s = ", iv); emit_expr(f, tree_left(r));
         fprintf(f, "; %s %s ", iv, to ? "<=" : ">="); emit_expr(f, tree_right(r));
         fprintf(f, "; %s = %s %s 1) begin\n", iv, iv, to ? "+" : "-");
         emit_stmt_list(f, s, ind + 2);
         tab(f, ind); fputs("end\n", f);
      }
      break;
   case T_IF:
      {
         const int nconds = tree_conds(s);
         for (int i = 0; i < nconds; i++) {
            tree_t c = tree_cond(s, i);
            tab(f, ind);
            if (i == 0) { fputs("if (", f); emit_expr(f, tree_value(c)); fputs(") begin\n", f); }
            else if (tree_has_value(c)) { fputs("else if (", f); emit_expr(f, tree_value(c)); fputs(") begin\n", f); }
            else fputs("else begin\n", f);
            emit_stmt_list(f, c, ind + 2);
            tab(f, ind); fputs("end\n", f);
         }
      }
      break;
   case T_CASE:
      {
         tab(f, ind); fputs("case (", f); emit_expr(f, tree_value(s)); fputs(")\n", f);
         const int nalt = tree_stmts(s);
         for (int i = 0; i < nalt; i++) {
            tree_t alt = tree_stmt(s, i);
            const int nch = tree_choices(alt);
            bool others = false; int nemit = 0;
            tab(f, ind + 2);
            for (int j = 0; j < nch; j++) {
               // A T_CHOICE is: a range (tree_ranges>0), a named value
               // (tree_has_name -> tree_name), or `others` (neither).
               tree_t c = tree_choice(alt, j);
               if (tree_ranges(c) > 0) {        // Verilog case has no range labels
                  g_unhandled++; fputs("/*?range-choice*/", f); continue;
               }
               if (!tree_has_name(c)) { others = true; continue; }   // others
               if (nemit++) fputs(", ", f);
               emit_expr(f, tree_name(c));
            }
            if (others && nemit == 0) fputs("default", f);
            else if (others) fputs(", default", f);
            fputs(": begin\n", f);
            emit_stmt_list(f, alt, ind + 4);
            tab(f, ind + 2); fputs("end\n", f);
         }
         tab(f, ind); fputs("endcase\n", f);
      }
      break;
   case T_WHILE:
      // yosys's read_verilog REJECTS procedural while-loops outright ("While
      // loops are only allowed in constant functions"), even bounded ones, so a
      // `while` emission hard-FAILS synthesis (it killed dec/dma/lsu/ifu chunks).
      // These are SV `for` loops sv2ghdl lowered to counter-controlled VHDL while
      // (`i:=init; while i<N loop ..; i:=i+1 end`); making them synthesizable
      // needs a while->Verilog-`for` peephole (init = preceding sibling assign,
      // increment = last body assign). Until then DECLINE so the module gracefully
      // stays interpreted instead of crashing the whole-subtree synth.
      g_unhandled++; tab(f, ind); fputs("/*?while-loop*/\n", f);
      break;
   case T_WAIT:
      break;   // process sensitivity — not emitted
   default:
      g_unhandled++; tab(f, ind); fprintf(f, "/*?seq k=%d*/\n", tree_kind(s));
      break;
   }
}

static void emit_process(FILE *f, tree_t p)
{
   tree_t body_if = NULL, sig[8];
   bool pe[8];
   int ne = 0;
   tree_t clk = clock_of(p, &body_if, sig, pe, &ne);
   if (clk != NULL) {
      fputs("  always @(", f);
      for (int i = 0; i < ne; i++) {
         if (i) fputs(" or ", f);
         fputs(pe[i] ? "posedge " : "negedge ", f);
         emit_expr(f, sig[i]);
      }
      fputs(") begin\n", f);
      emit_stmt_list(f, body_if, 4);
      fputs("  end\n", f);
   }
   else {
      // count non-wait statements; a lone signal-assign -> continuous assign
      // (so the target stays a wire, no `output reg` needed)
      tree_t only = NULL; int cnt = 0;
      const int nst = tree_stmts(p);
      for (int i = 0; i < nst; i++) {
         tree_t s = tree_stmt(p, i);
         if (tree_kind(s) == T_WAIT) continue;
         only = s; cnt++;
      }
      if (cnt == 1 && tree_kind(only) == T_SIGNAL_ASSIGN) {
         fputs("  assign ", f);
         emit_expr(f, tree_target(only));
         fputs(" = ", f);
         if (tree_waveforms(only) > 0) emit_expr(f, tree_value(tree_waveform(only, 0)));
         else emit_expr(f, tree_value(only));
         fputs(";\n", f);
      }
      else {
         fputs("  always @(*) begin\n", f);
         emit_stmt_list(f, p, 4);
         fputs("  end\n", f);
      }
   }
}

// Emit a Verilog port map "(.formal(actual), ...)" from an instance/block's
// params. T_INSTANCE and an elaborated child T_BLOCK carry the same param shape
// (P_NAMED -> .formal(actual), P_POS -> actual, T_OPEN value -> unconnected).
static void emit_portmap(FILE *f, tree_t s)
{
   const int nparams = tree_params(s);
   fputc('(', f);
   for (int i = 0; i < nparams; i++) {
      tree_t p = tree_param(s, i);
      if (i > 0) fputs(", ", f);
      if (tree_subkind(p) == P_NAMED) {
         fputc('.', f);
         emit_expr(f, tree_name(p));
         fputc('(', f);
      }
      tree_t v = tree_value(p);
      if (v != NULL && tree_kind(v) != T_OPEN)
         emit_expr(f, v);
      if (tree_subkind(p) == P_NAMED) fputc(')', f);
   }
   fputc(')', f);
}

static void emit_stmt(FILE *f, tree_t s)
{
   switch (tree_kind(s)) {
   case T_PROCESS:
      emit_process(f, s);
      break;
   case T_SIGNAL_ASSIGN:   // concurrent assign
      fputs("  assign ", f);
      emit_expr(f, tree_target(s));
      fputs(" = ", f);
      if (tree_waveforms(s) > 0) emit_expr(f, tree_value(tree_waveform(s, 0)));
      else emit_expr(f, tree_value(s));
      fputs(";\n", f);
      break;
   case T_INSTANCE:
      fprintf(f, "  %s %s ", vhdl2vlog_modname(tree_ident2(s)), vid(tree_ident(s)));
      emit_portmap(f, s);
      fputs(";\n", f);
      break;
   case T_BLOCK:
      {
         // An elaborated child instance: emit a Verilog module instantiation.
         // Module identity is on the block's first decl (a T_HIER marker) ->
         // tree_ref -> the architecture, whose entity names the child module
         // (the same name emit_subtree_v gives it). The port map params have the
         // same shape as T_INSTANCE. gen_statemachine then flattens. Generate /
         // VHDL-block scopes (ref not an arch) are declined (stay in nvc).
         tree_t hier = tree_decls(s) > 0 ? tree_decl(s, 0) : NULL;
         tree_t ref  = (hier != NULL && tree_kind(hier) == T_HIER)
                       ? tree_ref(hier) : NULL;
         if (ref == NULL || tree_kind(ref) != T_ARCH) {
            g_unhandled++;
            fprintf(f, "  /*?block k=%d*/\n", hier ? tree_kind(hier) : -1);
            break;
         }
         tree_t ent = tree_primary(ref);
         // Per-(entity,generics) variant name: emit_subtree_v names the child
         // module identically (same block), so generic-width variants don't
         // collide on one wrong-width module.
         fprintf(f, "  %s %s ", vhdl2vlog_variant_name(tree_ident(ent), s),
                 vid(tree_ident(s)));
         emit_portmap(f, s);
         fputs(";\n", f);
      }
      break;
   default:
      g_unhandled++; fprintf(f, "  /*?stmt k=%d*/\n", tree_kind(s));
      break;
   }
}

// recursively: does statement `s` (or any nested stmt) assign signal `name`?
static bool stmt_assigns(tree_t s, ident_t name)
{
   switch (tree_kind(s)) {
   case T_SIGNAL_ASSIGN:
   case T_VAR_ASSIGN:
      return tree_kind(tree_target(s)) == T_REF
         && tree_ident(tree_ref(tree_target(s))) == name;
   case T_IF:
      for (int i = 0; i < tree_conds(s); i++) {
         tree_t c = tree_cond(s, i);
         for (int j = 0; j < tree_stmts(c); j++)
            if (stmt_assigns(tree_stmt(c, j), name)) return true;
      }
      return false;
   case T_CASE:
      for (int i = 0; i < tree_stmts(s); i++)
         if (stmt_assigns(tree_stmt(s, i), name)) return true;
      return false;
   case T_PROCESS:
   case T_FOR:
   case T_WHILE:
   case T_BLOCK:
      for (int i = 0; i < tree_stmts(s); i++)
         if (stmt_assigns(tree_stmt(s, i), name)) return true;
      return false;
   default:
      return false;
   }
}

// ---- while->for counter-loop peephole --------------------------------------
// sv2ghdl lowers SystemVerilog `for` loops to counter-controlled VHDL while
// loops (`i:=init; while i<N loop ..; i:=i+1 end`). yosys's read_verilog REJECTS
// procedural while outright but UNROLLS a static for-loop, so we recognize that
// shape across a (init, while) statement pair and emit a Verilog for instead.

// Does expression `e` (recursively) reference the variable decl `Vd`?
static bool refs_var(tree_t e, tree_t Vd)
{
   if (e == NULL) return false;
   const tree_kind_t k = tree_kind(e);
   if (k == T_REF)
      return tree_has_ref(e) && tree_ref(e) == Vd;
   if (k == T_FCALL || k == T_ARRAY_REF || k == T_ARRAY_SLICE) {
      for (int i = 0; i < tree_params(e); i++)
         if (refs_var(tree_value(tree_param(e, i)), Vd)) return true;
   }
   if ((k == T_ARRAY_REF || k == T_ARRAY_SLICE || k == T_TYPE_CONV
        || k == T_QUALIFIED || k == T_INERTIAL) && tree_has_value(e))
      return refs_var(tree_value(e), Vd);
   return false;
}

// Is `e` a compile-time-constant (synthesis-static) expression? It must
// reference no signal/variable EXCEPT the loop var `Vallow` (NULL = none): only
// literals, enum lits, constants, and aggregates/conversions/operators over
// those. yosys can only unroll a for whose init/bound/step are static; folded_int
// is too strict (it can't see through the qualified logic3d conversions sv2ghdl
// wraps constants in, e.g. `integer'(logic3d_vector'(L3D_0,..))`), so use this.
static bool is_static_expr(tree_t e, tree_t Vallow)
{
   if (e == NULL) return true;
   switch (tree_kind(e)) {
   case T_LITERAL: case T_STRING:
      return true;
   case T_REF: {
      if (!tree_has_ref(e)) return false;
      tree_t r = tree_ref(e);
      if (r == Vallow) return true;
      const tree_kind_t rk = tree_kind(r);
      return rk == T_CONST_DECL || rk == T_ENUM_LIT;
   }
   case T_AGGREGATE:
      for (int i = 0; i < tree_assocs(e); i++)
         if (!is_static_expr(tree_value(tree_assoc(e, i)), Vallow)) return false;
      return true;
   case T_FCALL: case T_ARRAY_REF: case T_ARRAY_SLICE:
      for (int i = 0; i < tree_params(e); i++)
         if (!is_static_expr(tree_value(tree_param(e, i)), Vallow)) return false;
      if ((tree_kind(e) == T_ARRAY_REF || tree_kind(e) == T_ARRAY_SLICE)
          && tree_has_value(e))
         return is_static_expr(tree_value(e), Vallow);
      return true;
   case T_QUALIFIED: case T_TYPE_CONV: case T_INERTIAL:
      return tree_has_value(e) ? is_static_expr(tree_value(e), Vallow) : true;
   default:
      return false;
   }
}

// `e` is a binary op with one operand referencing Vd and the OTHER a folded
// integer constant. yosys can only unroll a for whose bound/step are compile-
// time constant (a non-constant bound is a HARD synth error), so this gate is
// what keeps the emitted for synthesizable.
static bool binop_const_operand(tree_t e, tree_t Vd)
{
   if (tree_kind(e) != T_FCALL || tree_params(e) != 2) return false;
   tree_t a = tree_value(tree_param(e, 0));
   tree_t b = tree_value(tree_param(e, 1));
   if (refs_var(a, Vd) && !refs_var(b, Vd)) return is_static_expr(b, NULL);
   if (refs_var(b, Vd) && !refs_var(a, Vd)) return is_static_expr(a, NULL);
   return false;
}

// Detect `V := init` (init_stmt) immediately followed by a counter-controlled
// `while V <relop> CONST loop ...body...; V := V <op> CONST end` (while_stmt),
// with init/bound/step all compile-time constant and V (a scalar variable)
// written ONLY by the final increment and no EXIT/NEXT/RETURN in the body.
static bool is_counter_while(tree_t init_stmt, tree_t while_stmt, tree_t *Vout,
                             tree_t *initv, tree_t *cond, tree_t *incv)
{
   if (init_stmt == NULL || while_stmt == NULL) return false;
   if (tree_kind(init_stmt) != T_VAR_ASSIGN) return false;
   tree_t lhs = tree_target(init_stmt);
   if (tree_kind(lhs) != T_REF || !tree_has_ref(lhs)) return false;
   tree_t Vd = tree_ref(lhs);
   if (tree_kind(Vd) != T_VAR_DECL) return false;
   if (tree_kind(while_stmt) != T_WHILE) return false;

   const int nb = tree_stmts(while_stmt);
   if (nb < 1) return false;
   for (int k = 0; k < nb; k++) {
      tree_t bs = tree_stmt(while_stmt, k);
      const tree_kind_t bk = tree_kind(bs);
      if (bk == T_EXIT || bk == T_NEXT || bk == T_RETURN) return false;
      if (k < nb - 1 && stmt_assigns(bs, tree_ident(Vd))) return false;
   }
   tree_t inc = tree_stmt(while_stmt, nb - 1);
   if (tree_kind(inc) != T_VAR_ASSIGN) return false;
   tree_t ilhs = tree_target(inc);
   if (tree_kind(ilhs) != T_REF || !tree_has_ref(ilhs) || tree_ref(ilhs) != Vd)
      return false;

   if (!is_static_expr(tree_value(init_stmt), NULL)) return false;   // init static
   tree_t c = tree_value(while_stmt);                                // condition
   if (tree_kind(c) != T_FCALL || vlog_op(istr(tree_ident(c))) == NULL) return false;
   if (!binop_const_operand(c, Vd)) return false;                   // V <relop> CONST
   tree_t iv = tree_value(inc);                                      // increment RHS
   if (!binop_const_operand(iv, Vd)) return false;                  // V <op> CONST

   *Vout = Vd; *initv = tree_value(init_stmt); *cond = c; *incv = iv;
   return true;
}

static void emit_for_from_while(FILE *f, tree_t V, tree_t initv, tree_t cond,
                                tree_t incv, tree_t while_stmt, int ind)
{
   const char *vn = vid(tree_ident(V));
   tab(f, ind);
   fprintf(f, "for (%s = ", vn); emit_expr(f, initv);
   fputs("; ", f);                emit_expr(f, cond);
   fprintf(f, "; %s = ", vn);     emit_expr(f, incv);
   fputs(") begin\n", f);
   // body minus the trailing increment; via the range emitter so a NESTED
   // counter loop inside is peepholed too.
   emit_stmt_list_range(f, while_stmt, 0, tree_stmts(while_stmt) - 1, ind + 2);
   tab(f, ind); fputs("end\n", f);
}

static void emit_stmt_list_range(FILE *f, tree_t container, int lo, int hi, int ind)
{
   for (int j = lo; j < hi; j++) {
      tree_t s  = tree_stmt(container, j);
      tree_t nx = (j + 1 < hi) ? tree_stmt(container, j + 1) : NULL;
      tree_t V, initv, cond, incv;
      if (nx != NULL && is_counter_while(s, nx, &V, &initv, &cond, &incv)) {
         emit_for_from_while(f, V, initv, cond, incv, nx, ind);
         j++;   // consume BOTH the init and the while
         continue;
      }
      emit_seq(f, s, ind);
   }
}

static void emit_stmt_list(FILE *f, tree_t container, int ind)
{
   emit_stmt_list_range(f, container, 0, tree_stmts(container), ind);
}

// A signal assigned inside any process must be declared `reg`; one driven only
// by a concurrent assignment stays a `wire`.
static bool is_reg(tree_t block, tree_t decl)
{
   ident_t name = tree_ident(decl);
   const int nstmts = tree_stmts(block);
   for (int i = 0; i < nstmts; i++) {
      tree_t s = tree_stmt(block, i);
      if (tree_kind(s) == T_PROCESS && stmt_assigns(s, name))
         return true;
   }
   return false;
}

// A standard 1-bit-like enumeration (std_logic family, bit, boolean): the only
// enums emit_range/emit_lit model correctly as a single Verilog bit.
static bool enum_is_bitlike(type_t t)
{
   for (int i = 0; i < 8 && t != NULL; i++) {
      const char *nm = istr(type_ident(t));
      const char *dot = strrchr(nm, '.');
      if (dot) nm = dot + 1;
      if (!strcasecmp(nm, "STD_ULOGIC") || !strcasecmp(nm, "STD_LOGIC")
          || !strcasecmp(nm, "BIT") || !strcasecmp(nm, "BOOLEAN"))
         return true;
      if (type_kind(t) != T_SUBTYPE)   // type_base is only valid for subtypes
         break;
      t = type_base(t);
   }
   return false;
}

// Conservative synthesizability test: vhdl2vlog only models integers, the
// std_logic/bit/boolean enums, and arrays thereof. Anything else (real,
// physical/time, file, access, record, protected, string/character arrays,
// user enums) would be silently mistranslated by emit_range into a plausible
// but wrong bit vector — so decline and leave the leaf in the nvc kernel.
static bool type_synth_ok(type_t t)
{
   if (type_is_array(t)) {
      // Decline large memory arrays: gen_statemachine would flatten them into
      // hundreds of thousands of flip-flops (hang/OOM). A RAM belongs in nvc's
      // native memory model, not an accelerated chunk -- leave it interpreted.
      // (type_width is the fully-flattened bit count across all dimensions.)
      if (type_width(t) > (1u << 16)) return false;
      type_t el = type_elem(t);
      // A bit-like enum element (std_logic_vector, bit_vector) is synthesizable
      // even though std_logic's literals are characters -- check that BEFORE the
      // character-array test, which would otherwise reject std_logic_vector as a
      // "string". (logic3d_vector is an integer array and was never affected.)
      if (type_is_enum(el) && enum_is_bitlike(el)) return true;
      if (type_is_array(el)) return type_synth_ok(el);   // multi-dim: recurse
      if (type_is_character_array(t)) return false;       // genuine strings
      return type_synth_ok(el);
   }
   if (type_is_integer(t)) return true;
   if (type_is_enum(t)) return enum_is_bitlike(t);
   return false;   // real, physical, file, access, record, protected, ...
}

// Only value-bearing decl kinds carry a type item; calling tree_type/
// tree_has_type on others (hierarchy markers, attr specs, ...) asserts.
static bool decl_type_synth(tree_t d)
{
   switch (tree_kind(d)) {
   case T_SIGNAL_DECL:
   case T_VAR_DECL:
   case T_CONST_DECL:
   case T_PORT_DECL:
      return type_synth_ok(tree_type(d));
   default:
      return true;   // no synthesis-relevant value type
   }
}

// Reject the whole leaf if any port, signal, or process variable uses a
// non-synthesizable type — the silent-mistranslation class that g_unhandled
// (statement-level) does not catch.
static bool block_types_synth(tree_t block)
{
   for (int i = 0; i < tree_ports(block); i++)
      if (!type_synth_ok(tree_type(tree_port(block, i)))) {
         if (getenv("GSM_LOG"))
            fprintf(stderr, "block_types_synth: port %s type %s rejected\n",
                    istr(tree_ident(tree_port(block, i))),
                    istr(type_ident(tree_type(tree_port(block, i)))));
         return false;
      }
   for (int i = 0; i < tree_decls(block); i++)
      if (!decl_type_synth(tree_decl(block, i))) {
         if (getenv("GSM_LOG"))
            fprintf(stderr, "block_types_synth: decl %s (kind %d) rejected\n",
                    istr(tree_ident(tree_decl(block, i))),
                    tree_kind(tree_decl(block, i)));
         return false;
      }
   for (int i = 0; i < tree_stmts(block); i++) {
      tree_t s = tree_stmt(block, i);
      if (tree_kind(s) != T_PROCESS) continue;
      for (int j = 0; j < tree_decls(s); j++)
         if (!decl_type_synth(tree_decl(s, j))) return false;
   }
   return true;
}

// Record `nm` in the seen-set; return true if it was already present.
static bool local_seen(char seen[][64], int *nseen, const char *nm)
{
   for (int i = 0; i < *nseen; i++)
      if (!strcmp(seen[i], nm)) return true;
   if (*nseen < 256) { strncpy(seen[*nseen], nm, 63); seen[*nseen][63] = '\0'; (*nseen)++; }
   return false;
}

// Verilog has no process-local storage, so a process's local variables (-> reg)
// and for-loop indices (-> integer) must be hoisted to module scope. Walk the
// statement tree (mirroring stmt_assigns) and emit each once, deduped by name.
static void emit_proc_locals(FILE *f, tree_t s, char seen[][64], int *nseen)
{
   switch (tree_kind(s)) {
   case T_PROCESS:
      for (int i = 0; i < tree_decls(s); i++) {
         tree_t d = tree_decl(s, i);
         if (tree_kind(d) != T_VAR_DECL) continue;
         const char *nm = vid(tree_ident(d));
         if (local_seen(seen, nseen, nm)) continue;
         fputs("  reg ", f); emit_range(f, tree_type(d)); fprintf(f, "%s;\n", nm);
      }
      for (int i = 0; i < tree_stmts(s); i++)
         emit_proc_locals(f, tree_stmt(s, i), seen, nseen);
      break;
   case T_FOR:
      if (tree_decls(s) > 0) {
         const char *nm = vid(tree_ident(tree_decl(s, 0)));
         if (!local_seen(seen, nseen, nm)) fprintf(f, "  integer %s;\n", nm);
      }
      for (int i = 0; i < tree_stmts(s); i++)
         emit_proc_locals(f, tree_stmt(s, i), seen, nseen);
      break;
   case T_WHILE:
   case T_BLOCK:
   case T_CASE:
      for (int i = 0; i < tree_stmts(s); i++)
         emit_proc_locals(f, tree_stmt(s, i), seen, nseen);
      break;
   case T_IF:
      for (int i = 0; i < tree_conds(s); i++) {
         tree_t c = tree_cond(s, i);
         for (int j = 0; j < tree_stmts(c); j++)
            emit_proc_locals(f, tree_stmt(c, j), seen, nseen);
      }
      break;
   default:
      break;
   }
}

// Emit ONE module to an already-open file. Returns true iff fully + faithfully
// translated. Used both for a single leaf (via vhdl2vlog) and for each module of
// a whole subtree written into one file (whole-chunk accel, gen_statemachine
// flattens the top). Child instantiations it emits (emit_stmt T_BLOCK) reference
// each child module by vhdl2vlog_variant_name(entity, child-block) — the same
// per-(entity,generics) name emit_subtree_v gives the child's definition, so
// width-variants stay distinct and the flatten resolves. (vid() = instance LABEL
// only, not the module reference.)
bool vhdl2vlog_module(FILE *f, tree_t block, const char *modname)
{
   g_unhandled = 0;

   // Decline non-synthesizable modules up front (see block_types_synth): a wrong
   // but parseable model would silently corrupt results.
   if (!block_types_synth(block))
      return false;

   fprintf(f, "// auto-generated from nvc elaborated VHDL by vhdl2vlog\n");
   fprintf(f, "module %s (\n", modname);
   const int nports = tree_ports(block);
   for (int i = 0; i < nports; i++) {
      tree_t p = tree_port(block, i);
      const port_mode_t mode = tree_subkind(p);
      // A VHDL `buffer` port is an OUTPUT that is also readable internally (VeeR
      // uses it for registered outputs that feed back). Emit it as `output` (a
      // Verilog output is internally readable too) — NOT `input`. Mislabeling it
      // `input` makes the net both a port-input and a flop Q in the accel
      // codegen, colliding into one C identifier (redefinition compile failure).
      fprintf(f, "  %s ",
              mode == PORT_OUT || mode == PORT_INOUT || mode == PORT_BUFFER
              ? "output" : "input");
      emit_range(f, tree_type(p));
      fprintf(f, "%s%s\n", vid(tree_ident(p)), i + 1 < nports ? "," : "");
   }
   fputs(");\n", f);

   // signal declarations (skip ports and hier markers)
   const int ndecls = tree_decls(block);
   for (int i = 0; i < ndecls; i++) {
      tree_t d = tree_decl(block, i);
      if (tree_kind(d) != T_SIGNAL_DECL) continue;
      fprintf(f, "  %s ", is_reg(block, d) ? "reg" : "wire");
      emit_range(f, tree_type(d));
      fprintf(f, "%s;\n", vid(tree_ident(d)));
   }

   const int nstmts = tree_stmts(block);

   // hoist process-local variables (-> reg) and for-loop indices (-> integer)
   char seen[256][64];
   int nseen = 0;
   for (int i = 0; i < nstmts; i++)
      emit_proc_locals(f, tree_stmt(block, i), seen, &nseen);

   for (int i = 0; i < nstmts; i++)
      emit_stmt(f, tree_stmt(block, i));

   fputs("endmodule\n", f);
   return g_unhandled == 0;   // faithful translation only
}

bool vhdl2vlog(tree_t block, const char *modname, const char *path)
{
   FILE *f = fopen(path, "w");
   if (f == NULL) { warnf("vhdl2vlog: cannot open %s", path); return false; }
   const bool ok = vhdl2vlog_module(f, block, modname);
   fclose(f);
   return ok;
}
