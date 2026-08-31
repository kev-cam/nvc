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
#include "hash.h"
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

// EVERY decline goes through here.  The COUNTER is what decides admission
// (vhdl2vlog_module returns g_unhandled == 0); the LOG is what makes a decline
// diagnosable, and until now most were not.  Ten of the 29 decline paths wrote
// nothing identifiable into the Verilog, and block_types_synth refuses a module
// before a single byte is emitted -- so its .v is empty AND the counter never
// moves.  Measured over the regression corpus, 42-54% of real declines were
// invisible: 412 of 991 declining designs emitted zero markers, and 52 of 97
// declined named subtrees were marker-free in every copy, including essentially
// the whole VeeR EH2 chunk set.  Ranking blockers off marker counts therefore
// under-weighted type-level declines about two-fold.
//
// PURELY DIAGNOSTIC: nothing here writes to `f`, so with GSM_LOG unset the
// emitted Verilog is byte-identical to before.  That is a checkable invariant --
// the whole-VeeR per-chunk md5 set must not move.
static void aj_decline(const char *why, const char *fn, int line)
{
   static int log = -1;
   g_unhandled++;
   if (log < 0) log = getenv("GSM_LOG") != NULL;
   if (log)
      fprintf(stderr, "vhdl2vlog: DECLINE %-30s (%s:%d)\n", why, fn, line);
}

#define DECLINE(why) aj_decline((why), __func__, __LINE__)


// "The expression about to be emitted is a CONCATENATION ELEMENT."
//
// Verilog resizes every operand to its context width in every context except
// two: a concatenation element and a replication operand.  Everywhere else an
// emitted expression that is wider or narrower than its VHDL type is harmless
// (the assignment or the operator resizes it); inside {} it silently shreds the
// surrounding vector -- which is exactly how the l3d_bit_read width defect
// destroyed every VeeR GPR write while yosys reported zero errors.
//
// Sensitivity stops at a SELF-DETERMINED operand position and flows through a
// CONTEXT-DETERMINED one (IEEE 1364-2005 Table 5-22).  emit_expr therefore
// CONSUMES the flag on entry -- each emission decides for itself -- and hands it
// back down only where an operand's own width can still escape into the result:
//   * verbatim pass-throughs   -- type conversion / qualified / inertial, and
//                                 the l3dk==2 IDENTITY emissions (resize,
//                                 to_l3d, unsigned_to_l3d, l3d_to_unsigned,
//                                 to_unsigned/to_signed, is_one, ...), which
//                                 print their operand and nothing else;
//   * both operands of + - * & | ^ ~& ~| ~^ and of unary ~ / -, and both arms
//     of ?:  -- the result is max(L,R) wide, so a too-WIDE operand poisons it;
//   * the LEFT operand of << / >>  -- the result is that operand's width.
// It must NOT flow into a self-determined position -- a relational operand
// (result is 1 bit), a reduction operand, a shift AMOUNT, an index, or a
// replication count -- because no width can escape from there.
//
// Getting this wrong in the "stops too early" direction is what made the
// original guard one node deep: the dominant sv2vhdl idiom is
// unsigned_to_l3d(Resize(l3d_to_unsigned(X), N)) (2539 `resize` and 370
// `unsigned_to_l3d_bit` calls in VeeR's design.vhd), so ANY width-wrong emission
// under one of those wrappers escaped every guard.
//
// Emissions whose Verilog SELF-DETERMINED width is not the VHDL type width test
// the consumed flag and bump g_unhandled, i.e. DECLINE, rather than emit a
// silently shredded vector.
static bool g_concat_elem = false;

// Emit one element of a concatenation/replication (width-sensitive context).
static void emit_concat_elem(FILE *f, tree_t e)
{
   const bool save = g_concat_elem;
   g_concat_elem = true;
   emit_expr(f, e);
   g_concat_elem = save;
}

// Emit a CONTEXT-DETERMINED operand, propagating the caller's (already
// consumed) width sensitivity into it.  `celem` false makes this a plain
// emit_expr, so it is safe to use unconditionally.
static void emit_ctx_operand(FILE *f, tree_t e, bool celem)
{
   const bool save = g_concat_elem;
   g_concat_elem = celem;
   emit_expr(f, e);
   g_concat_elem = save;
}

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
// The Verilog width emit_range() below gives a declaration of this type.
// MIRRORS emit_range case for case -- if one changes, change both, or the
// concat-element guard will disagree with what is actually emitted.  Returns
// -1 for "not statically known" (an unconstrained array), which callers must
// treat as a mismatch rather than as permission to pass through.
static int type_vlog_width(type_t type)
{
   if (type_is_logic3d(type) && !type_is_array(type))
      return 1;                    // scalar logic3d -> bit0 only
   if (type_is_array(type))
      return type_const_bounds(type) ? (int)type_width(type) : -1;
   if (type_is_integer(type))
      return 32;                   // VHDL Integer -> signed [31:0]
   return 1;                       // std_logic / boolean / enum -> single bit
}

static void emit_range(FILE *f, type_t type)
{
   if (type_is_logic3d(type) && !type_is_array(type))
      return;   // scalar logic3d -> 1-bit value (bit0), no range
   if (type_is_array(type)) {
      if (!type_const_bounds(type)) { DECLINE("unconstrained-array-range"); return; }  // unconstrained
      const unsigned w = type_width(type);
      if (w > 1) fprintf(f, "[%u:0] ", w - 1);
   }
   else if (type_is_integer(type))
      // VHDL Integer is SIGNED. Rendering it as a plain (unsigned) reg[31:0]
      // makes every comparison against a negative constant UNSIGNED in
      // Verilog (one unsigned operand poisons the op), so tgt-vhdl's
      // OOB_WriteV_Idx_N guards `idx >= -3` are constant-false and yosys
      // const-folds the whole guarded write network away (VeeR lsu
      // stbuf_numvld_any -> 8'h00 -> store-stall underflow at 695ns).
      fprintf(f, "signed [31:0] ");
   // std_logic / boolean / enum -> single bit, no range
}

// ---- memory-shaped signals -------------------------------------------------
// A signal of type array(N) of vector(W-1:0) whose EVERY module reference is a
// plain single-index T_ARRAY_REF is emitted as a true Verilog memory
// (`reg [W-1:0] name [0:N-1]`) instead of a flattened N*W-bit vector. yosys
// then lowers the accesses to word-based $memrd/$memwr, which the accel
// codegen turns into O(1) word reads/writes. Flattening instead yields
// whole-vector dynamic $shiftx barrel shifts — O(N*W) PER ACCESS (the VeeR
// icache 256x34 SRAM cost ~70% of total --accel sim time that way). Any other
// use (slice, whole-array read, port map) disqualifies the signal and it falls
// back to the flattened emission.

#define MAX_MEM_SIGS 64
static tree_t g_mem_sigs[MAX_MEM_SIGS];
static int    g_n_mem_sigs = 0;

// NBA-shadow memory pairs.  sv2vhdl's wake-shadow idiom (`v := sig; ...
// v(idx) := d; ...; sig <= v`) reads and writes the WHOLE array through a
// shadow variable, which breaks the refs==indexed qualification above even
// though every real access is single-index — the VeeR icache RAM primitives
// flattened to 8704-bit vectors this way, and every access became a
// whole-width barrel shift (272 limbs per access in the farm codegen).
// A qualified pair elides the copy and the writeback, rewrites each
// `v(idx) := d` into a direct NBA memory write `sig[idx] <= d`, and
// suppresses the shadow declaration.  Qualification demands exactly one
// copy and one writeback, no other whole references of either side, and
// no indexed READS of the shadow (writes only), so the elision cannot
// change read-after-write visibility.
#define MAX_MEM_SHADOWS 64
static tree_t g_shadow_var[MAX_MEM_SHADOWS];
static tree_t g_shadow_sigd[MAX_MEM_SHADOWS];
static int    g_n_shadows = 0;

static tree_t shadow_sig_of(tree_t vdecl)
{
   for (int i = 0; i < g_n_shadows; i++)
      if (g_shadow_var[i] == vdecl) return g_shadow_sigd[i];
   return NULL;
}

static bool sig_has_shadow(tree_t sdecl)
{
   for (int i = 0; i < g_n_shadows; i++)
      if (g_shadow_sigd[i] == sdecl) return true;
   return false;
}

typedef struct { tree_t sig; tree_t var; } shadow_find_t;

static void shadow_find_cb(tree_t t, void *ctx)
{
   shadow_find_t *sf = (shadow_find_t *)ctx;
   if (sf->var != NULL || tree_kind(t) != T_VAR_ASSIGN) return;
   tree_t tg = tree_target(t), v = tree_value(t);
   if (tree_kind(tg) != T_REF || !tree_has_ref(tg)) return;
   if (tree_kind(v) != T_REF || !tree_has_ref(v)) return;
   if (tree_ref(v) != sf->sig) return;
   if (tree_kind(tree_ref(tg)) != T_VAR_DECL) return;
   sf->var = tree_ref(tg);
}

typedef struct {
   tree_t sig, var;
   int ncopy, nwb, nwrite;          // v:=sig / sig<=v / v(idx):=e statements
   int v_ref, v_arr, s_ref, s_arr;  // raw reference counts
} shadow_scan_t;

static void shadow_scan_cb(tree_t t, void *ctx)
{
   shadow_scan_t *sc = (shadow_scan_t *)ctx;
   const tree_kind_t k = tree_kind(t);
   if (k == T_REF && tree_has_ref(t)) {
      if (tree_ref(t) == sc->var) sc->v_ref++;
      if (tree_ref(t) == sc->sig) sc->s_ref++;
   }
   else if (k == T_ARRAY_REF && tree_params(t) == 1) {
      tree_t base = tree_value(t);
      if (tree_kind(base) == T_REF && tree_has_ref(base)) {
         if (tree_ref(base) == sc->var) sc->v_arr++;
         if (tree_ref(base) == sc->sig) sc->s_arr++;
      }
   }
   else if (k == T_VAR_ASSIGN) {
      tree_t tg = tree_target(t), v = tree_value(t);
      if (tree_kind(tg) == T_REF && tree_has_ref(tg)
          && tree_ref(tg) == sc->var
          && tree_kind(v) == T_REF && tree_has_ref(v)
          && tree_ref(v) == sc->sig)
         sc->ncopy++;
      else if (tree_kind(tg) == T_ARRAY_REF && tree_params(tg) == 1) {
         tree_t base = tree_value(tg);
         if (tree_kind(base) == T_REF && tree_has_ref(base)
             && tree_ref(base) == sc->var)
            sc->nwrite++;
      }
   }
   else if (k == T_SIGNAL_ASSIGN) {
      tree_t tg = tree_target(t);
      if (tree_kind(tg) == T_REF && tree_has_ref(tg)
          && tree_ref(tg) == sc->sig && tree_waveforms(t) > 0
          && tree_has_value(tree_waveform(t, 0))) {
         tree_t v = tree_value(tree_waveform(t, 0));
         if (tree_kind(v) == T_REF && tree_has_ref(v)
             && tree_ref(v) == sc->var)
            sc->nwb++;
      }
   }
}

// Hoisted process-locals / loop indices whose name collides with a module
// signal or port get a __lp suffix; references follow by DECL IDENTITY (the
// T_REF resolves to the loop/var decl, so no name ambiguity). VeeR's
// ifu_compress_ctl has a 16-bit signal literally named `i` next to for-loops
// indexed by `i` — the collision made yosys reject the whole module
// ("Incompatible re-declaration of wire \i"), silently until now.
static hash_t *g_ren_map = NULL;   // var/index decl -> unique mangled suffix
static int     g_ren_ctr = 0;      // per-module suffix counter
static hset_t *g_sig_names = NULL;   // module signal+port idents
// True while emitting a COMBINATIONAL process body (always @(*)). SystemVerilog
// always_comb uses blocking '='; sv2ghdl represents it as VHDL signal '<=', and
// a naive '<=' in always @(*) is a non-blocking-in-comb anti-pattern — for a
// self-accumulating target (x |= t  ->  x <= x | t) yosys reads the external
// wire and drives it back => a combinational LOOP (which the ATPG levelizer and
// clean tooling reject). Emit '=' for signal assigns inside a comb process to
// recover the blocking semantics and break the loop.
static bool g_comb_proc = false;

// Design (non-package) function translation. g_func_set holds the names of the
// module's own functions so a call to one emits a Verilog function call (not the
// unhandled `/*fn*/` marker); g_func_ret_name is the current function's name, so
// a `return x` inside its body emits `<name> = x`.
static hset_t    *g_func_set = NULL;
static const char *g_func_ret_name = NULL;

static const char *ren_suffix(tree_t d)
{
   return (g_ren_map != NULL) ? (const char *)hash_get(g_ren_map, d) : NULL;
}

static bool ren_decl(tree_t d)
{
   return ren_suffix(d) != NULL;
}

static void ren_register(tree_t d, const char *sfx)
{
   if (g_ren_map == NULL) g_ren_map = hash_new(256);
   hash_put(g_ren_map, d, (void *)xstrdup(sfx));
}

static bool sig_is_mem(tree_t decl)
{
   for (int i = 0; i < g_n_mem_sigs; i++)
      if (g_mem_sigs[i] == decl) return true;
   return false;
}

// array(N>=2) of vector(2..64 bits), const bounds both levels
static bool mem_shape(type_t t, unsigned *nwords, unsigned *elemw)
{
   if (!type_is_array(t) || !type_const_bounds(t) || dimension_of(t) != 1)
      return false;
   type_t et = type_elem(t);
   if (type_is_integer(et) && !type_is_logic3d(et)) {
      // Array of constrained integers (ITC b12 RAM, b15 InstQueue): a real
      // Verilog memory of 32-bit words — the same signed [31:0] convention
      // every scalar integer in this translator uses, so indexed reads and
      // writes line up with integer expressions without any conversion.
      int64_t low, high;
      if (!folded_bounds(range_of(t, 0), &low, &high)) return false;
      const int64_t n = high - low + 1;
      if (n < 2 || n > 65536 || low != 0) return false;
      *nwords = (unsigned)n;
      *elemw  = 32;
      return true;
   }
   if (!type_is_array(et) || !type_const_bounds(et)) return false;
   const unsigned ew = type_width(et);
   if (ew < 2 || ew > 64) return false;   // >64: accel memory codegen declines
   const unsigned total = type_width(t);
   if (total < 2 * ew) return false;
   *nwords = total / ew;
   *elemw  = ew;
   return true;
}

typedef struct { tree_t decl; int refs; int indexed; int agg; } mem_scan_t;

static void mem_scan_cb(tree_t t, void *ctx)
{
   mem_scan_t *sc = (mem_scan_t *)ctx;
   const tree_kind_t k = tree_kind(t);
   if (k == T_VAR_ASSIGN || k == T_SIGNAL_ASSIGN) {
      // a whole-array := (positional aggregate) is representable (it expands
      // to per-word writes at emission) -- count it so it does not read as a
      // disqualifying bare ref (ITC b15's InstQueue reset)
      tree_t tg = tree_target(t);
      // T_SIGNAL_ASSIGN keeps its value inside a waveform — tree_value()
      // directly on it is a FATAL object lookup, and this scan runs as a
      // translatability PROBE, which must decline, never kill the sim (the
      // NBA-shadow commit `mem <= v_nba_mem` walked straight into this).
      tree_t val = NULL;
      if (k == T_VAR_ASSIGN)
         val = tree_value(t);
      else if (tree_waveforms(t) > 0 && tree_has_value(tree_waveform(t, 0)))
         val = tree_value(tree_waveform(t, 0));
      if (val != NULL && tree_kind(tg) == T_REF && tree_has_ref(tg)
          && tree_ref(tg) == sc->decl
          && tree_kind(val) == T_AGGREGATE) {
         bool allpos = true;
         for (int i = 0; i < tree_assocs(val); i++)
            if (tree_subkind(tree_assoc(val, i)) != A_POS)
               allpos = false;
         if (allpos) sc->agg++;
      }
   }
   if (k == T_REF) {
      if (tree_has_ref(t) && tree_ref(t) == sc->decl) sc->refs++;
   }
   else if (k == T_ARRAY_REF && tree_params(t) == 1) {
      tree_t base = tree_value(t);
      if (tree_kind(base) == T_REF && tree_has_ref(base)
          && tree_ref(base) == sc->decl)
         sc->indexed++;
   }
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
      // Integer division truncates toward zero in BOTH VHDL and (signed)
      // Verilog, and this translator declares every integer signed [31:0],
      // so "/" maps directly; numeric_std unsigned vectors likewise (plain
      // regs -> unsigned division).  "rem" is exactly Verilog "%" (result
      // sign follows the DIVIDEND in both).  VHDL "mod" is NOT "%" for
      // negative operands (sign follows the DIVISOR) -- it keeps only the
      // positive-power-of-2 mask special case and declines otherwise.
      {"\"/\"","/"}, {"\"rem\"","%"},
      {NULL,NULL}
   };
   for (int i = 0; map[i].v; i++)
      if (strcmp(fn, map[i].v) == 0) return map[i].o;
   return NULL;
}

// logic3d relational FUNCTIONS (not operators): the sv2vhdl `<`/`<=`/... on a
// signed field lower to l3d_lt_s/le_s/... value-plane comparisons. Returns the
// Verilog operator and sets *sgn (signed variants wrap operands in $signed).
static const char *l3d_relop(const char *base, bool *sgn)
{
   static const struct { const char *n; const char *o; bool s; } m[] = {
      {"l3d_lt_s","<",true},  {"l3d_le_s","<=",true},
      {"l3d_gt_s",">",true},  {"l3d_ge_s",">=",true},
      {"l3d_lt","<",false},   {"l3d_le","<=",false},
      {"l3d_gt",">",false},   {"l3d_ge",">=",false},
      {NULL,NULL,false}
   };
   for (int i = 0; m[i].n; i++)
      if (!strcasecmp(base, m[i].n)) { *sgn = m[i].s; return m[i].o; }
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
   // numeric_std declares `subtype SIGNED is (resolved) UNRESOLVED_SIGNED`, and
   // every operator/function (`+`, `-`, resize, ...) returns UNRESOLVED_SIGNED.
   // So a declared `signed` signal reads as SIGNED but any expression RESULT
   // reads as UNRESOLVED_SIGNED -- both must count, or a chained signed
   // expression (acc + r8 - r12) loses sign-extension on the outer operator.
   for (int i = 0; i < 8 && t != NULL; i++) {
      const char *b = id_base(istr(type_ident(t)));
      if (!strcasecmp(b, "SIGNED") || !strcasecmp(b, "UNRESOLVED_SIGNED"))
         return true;
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
      // to_l3d(value, width): bring a numeric value into logic3d at width w.
      // Every overload (unsigned/signed/integer/logic3d_vector) is a resize on
      // the value plane, and kind 2 emits only param 0 so the trailing width
      // argument is dropped -- Verilog's assignment context does the resize,
      // exactly as for `resize` above.
      {"to_l3d","",2},
      {"reduce_or","|",3}, {"reduce_and","&",3}, {"reduce_xor","^",3},
      // value-plane equality (all overloads: l3d / l3d_vector)
      {"l3d_eq1","==",0}, {"l3d_ne1","!=",0},
      {NULL,NULL,0}
   };
   for (int i = 0; m[i].n; i++)
      if (!strcasecmp(b, m[i].n)) { *kind = m[i].k; return m[i].o; }
   return NULL;
}

// Does this Verilog operator take its width FROM its operands?  Equality and
// the relationals are exactly one bit whatever they compare, so their operands
// are sealed off; everything else vlog_op/vlog_l3d_op emits (+ - * & | ^ ~& ~|
// ~^ ~ << >>) is max(L,R) or L wide, i.e. its operands' widths escape.
static bool vlog_op_ctx_width(const char *o)
{
   return !(!strcmp(o, "==") || !strcmp(o, "!=") || !strcmp(o, "<")
            || !strcmp(o, ">") || !strcmp(o, "<=") || !strcmp(o, ">="));
}

// ---- Verilog SELF-DETERMINED width of what emit_expr() will actually PRINT ---
//
// Not the same thing as the VHDL type width: the l3dk==2 identities DROP their
// width argument and print their operand verbatim, so `resize(x, 32)` emits
// width(x) bits, not 32.  Everywhere but inside {} that is harmless (the
// enclosing assignment or operator resizes it); inside {} nothing resizes, so
// the concat-element guard has to know the real emitted width.
//
// Returns -1 for "not statically known", which the caller treats as a MISMATCH
// (decline).  Answering "I don't know" costs a chunk; answering wrongly ships a
// shredded vector, so every shape not resolved below returns -1.
//
// Descending the identity CHAIN is the point: sv2vhdl's dominant idiom is
//     unsigned_to_l3d(Resize(l3d_to_unsigned(X), N))
// in which every intermediate carries an UNCONSTRAINED numeric_std type, so the
// operand's own VHDL type width is unknown at every level and only walking down
// to X recovers the width that will really be emitted.
static int emitted_width(tree_t e, int depth)
{
   if (e == NULL || depth > 16) return -1;
   switch (tree_kind(e)) {
   case T_TYPE_CONV:
   case T_QUALIFIED:
   case T_INERTIAL:
      return emitted_width(tree_value(e), depth + 1);   // printed verbatim
   case T_STRING:
      {
         const int n = tree_chars(e);        // sized literal: `N'b...`
         return n > 0 ? n : -1;
      }
   case T_FCALL:
      {
         const char *fn = istr(tree_ident(e));
         int k = -1;
         // An operator whose result is ONE BIT WHATEVER ITS OPERANDS answers 1
         // here, not "unknown": equality and the relationals (vlog_op_ctx_width
         // encodes exactly that set, per IEEE 1364-2005 Table 5-22), the signed
         // logic3d relationals, and the unary reductions (l3dk==3).
         //
         // This is not a refinement for its own sake. -1 means "not statically
         // known", which the concat-element guard MUST treat as a mismatch, so
         // every unknown costs a whole chunk. MEASURED on VeeR: with these
         // returning -1, adding the one-parameter identity width check declined
         // 13 chunks INCLUDING eh2_dec, and every one of the 256 firings was
         // boolean_to_logic(nw=1, ew=-1) whose argument was a comparison --
         // l3d_eq1 or "=" -- i.e. already exactly one bit. The declines were
         // pure false positives from this function's ignorance, not real
         // width errors.
         const char *vop = vlog_op(fn);
         if (vop != NULL) return vlog_op_ctx_width(vop) ? -1 : 1;
         bool rsgn = false;
         if (l3d_relop(id_base(fn), &rsgn) != NULL) return 1;
         const char *lop = vlog_l3d_op(fn, &k);
         if (lop == NULL) return -1;
         if (k == 3) return 1;              // unary reduction -> one bit
         if (k != 2) return vlog_op_ctx_width(lop) ? -1 : 1;
         if (tree_params(e) < 1) return -1;
         // l3dk==2 identity: prints param 0 verbatim, except for the widening
         // zero-extend branch, which prints `{nw-aw'b0, param0}`.  Mirror that
         // branch's EXACT condition (see emit_expr) or the answer is fiction.
         tree_t a0 = tree_value(tree_param(e, 0));
         const int in = emitted_width(a0, depth + 1);
         const bool sgn = tree_has_type(e) && type_is_signed(tree_type(e));
         int64_t nwi = -1;
         int nw = -1, aw = -1;
         if (tree_params(e) >= 2
             && folded_int(tree_value(tree_param(e, 1)), &nwi))
            nw = (int)nwi;
         if (tree_has_type(a0) && type_is_array(tree_type(a0))
             && type_const_bounds(tree_type(a0)))
            aw = (int)type_width(tree_type(a0));
         if (!sgn && nw > 0 && aw > 0 && nw > aw)
            return in > 0 ? (nw - aw) + in : -1;    // {nw-aw'b0, a0}
         return in;                                  // $signed(a0) / bare a0
      }
   default:
      break;
   }
   if (!tree_has_type(e)) return -1;
   type_t t = tree_type(e);
   // Exactly emit_range()'s model -- that is what the module's own declarations
   // use, so it is the width every identifier reference really has.  logic3d is
   // an INTEGER subtype (natural range 0 to 7), so it must be tested first.
   if (type_is_logic3d(t) && !type_is_array(t)) return 1;
   if (type_is_array(t))
      return type_const_bounds(t) ? (int)type_width(t) : -1;
   if (type_is_integer(t)) return 32;                  // `signed [31:0]`
   if (type_is_enum(t)) return 1;                      // std_logic/bit/boolean
   return -1;                                          // real/physical/record/...
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
   // std_logic enum literal '0'/'1' etc. A literal with no ident (real, string,
   // physical, ...) is not a bit-enum lit -- mark unhandled (the leaf declines)
   // rather than fatal in tree_ident.
   if (!tree_has_ident(e)) { DECLINE("literal-without-ident"); fputs("/*lit?*/0", f); return; }
   const char *s = istr(tree_ident(e));
   if (strcmp(s, "'0'") == 0) fputs("1'b0", f);
   else if (strcmp(s, "'1'") == 0) fputs("1'b1", f);
   else { DECLINE("literal-unsupported"); fprintf(f, "/*lit %s*/0", s); }
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
         emit_concat_elem(f, slot[is_downto ? b : (W - 1 - b)]);
      }
      fputc('}', f);
   }
   free(slot);
   return !bad;
}

static void emit_expr(FILE *f, tree_t e)
{
   // consume the concat-element flag: this emission decides for itself, and its
   // operands (which the operator resizes) must not inherit it.
   const bool celem = g_concat_elem;
   g_concat_elem = false;

   if (e == NULL) {   // e.g. a null/unaffected waveform value — decline, don't crash
      DECLINE("null-literal"); fputs("/*null*/0", f);
      return;
   }
   switch (tree_kind(e)) {
   case T_REF:
      {
         if (tree_has_ref(e) && ren_decl(tree_ref(e))) {
            fprintf(f, "%s%s", vid(tree_ident(e)), ren_suffix(tree_ref(e)));
            break;
         }
         const char *nm = vid(tree_ident(e));
         const char *bn = id_base(istr(tree_ident(e)));
         // logic3d constants L3D_0/L3D_1/L3D_0X/... -> value bit (2-state)
         if (strncasecmp(bn, "L3D_", 4) == 0)
            fputs(bn[4] == '1' ? "1'b1" : "1'b0", f);
         else if (strcmp(bn, "'0'") == 0) fputs("1'b0", f);   // raw basename: vid() now sanitizes '0'->_0_
         else if (strcmp(bn, "'1'") == 0) fputs("1'b1", f);
         // The WEAK drives carry a definite value on the 2-state value plane,
         // so they are ordinary constants here.
         else if (strcmp(bn, "'L'") == 0) fputs("1'b0", f);
         else if (strcmp(bn, "'H'") == 0) fputs("1'b1", f);
         // BOOLEAN literals: without these, TRUE/FALSE reached the bare-name
         // fallthrough and emitted `true`/`false` -- identifiers declared
         // nowhere, which yosys reads as fresh undriven wires (constant 0)
         // and reports NOTHING: every `sig <= Pending` in ITC b15 silently
         // deasserted and the FSM walked a different path (b17 first
         // divergence at cycle 18).  The names are reserved words in VHDL,
         // so the basename alone identifies the standard literals.
         else if (strcasecmp(bn, "true") == 0)  fputs("1'b1", f);
         else if (strcasecmp(bn, "false") == 0) fputs("1'b0", f);
         // 'U' 'X' 'Z' 'W' '-' have NO value-plane representation.  Without
         // this they reached the bare-name fallthrough below, where vid()
         // renders them as _u_ / _x_ / _z_ / _w_ / _-_ -- identifiers that are
         // DECLARED NOWHERE in the emitted module.  yosys reads an undeclared
         // identifier as a fresh undriven wire and reports nothing, so the
         // chunk installs ACTIVE and the value is silently wrong.  MEASURED in
         // the artifact corpus: aj_mvvu_dut_subtree.v emits
         //     r <= {8{_u_}};
         // with _u_ declared zero times in the whole module.  Decline instead;
         // uncertainty is what the interpreter's logic3d planes are for, and
         // the runtime X-detect/demote path handles it correctly.
         else if (bn[0] == '\'' && bn[1] != '\0' && bn[2] == '\''
                  && bn[3] == '\0'
                  && strchr("UXZW-", toupper((unsigned char)bn[1])) != NULL) {
            // NVC_V2V_META0: accept the X->0 mapping instead of declining.
            // Sanctioned by the Verilator-match translation doctrine (2-state
            // Verilator reads X as 0) for whole-subtree V2V dumps; the accel
            // bridge keeps the decline so interp retains X fidelity there.
            static int meta0 = -1;
            if (meta0 < 0) meta0 = getenv("NVC_V2V_META0") != NULL;
            if (!meta0)
               DECLINE("std_logic-metavalue");
            fprintf(f, "/*meta %s*/0", bn);
         }
         else {
            // Ports/signals/process-vars ARE declared in the emitted module, so a
            // bare name is correct. An architecture-level CONSTANT is NOT declared
            // -> a bare name is an undefined Verilog wire (silent wrong value, and
            // the chunk still installs). Inline its static value instead; decline
            // anything else non-declared (e.g. an unfolded generic ref).
            tree_t ref = tree_has_ref(e) ? tree_ref(e) : NULL;
            const tree_kind_t rk = ref != NULL ? tree_kind(ref) : T_REF;
            if (rk == T_CONST_DECL && tree_has_value(ref))
               // inlined VERBATIM -> width sensitivity passes through
               emit_ctx_operand(f, tree_value(ref), celem);
            else if (rk == T_CONST_DECL || rk == T_GENERIC_DECL) {
               DECLINE("ref-not-declared"); fprintf(f, "/*ref %s*/0", bn);
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
         if (n <= 0) { DECLINE("empty-string-literal"); fputs("/*str0*/0", f); break; }
         fprintf(f, "%d'b", n);
         for (int i = 0; i < n; i++) {
            ident_t rune = tree_ident(tree_char(e, i));
            // guard the index-1 read on length too (ident_char asserts n<len):
            // a malformed/short rune declines rather than aborting the sim.
            const char c = (ident_len(rune) >= 2 && ident_char(rune, 0) == '\'')
                           ? ident_char(rune, 1) : '?';
            if (c == '1' || c == 'H') fputc('1', f);
            else if (c == '0' || c == 'L') fputc('0', f);
            else { DECLINE("string-char-unsupported"); fputc('0', f); }
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
            emit_concat_elem(f, tree_value(tree_param(e, 0)));
            fputs(", ", f);
            emit_concat_elem(f, tree_value(tree_param(e, 1)));
            fputc('}', f);
            break;
         }
         // sv2vhdl's Ternary_Unsigned/ternary_logic(T, X, Y) is a plain mux.
         if ((strcasecmp(vid(tree_ident(e)), "ternary_unsigned") == 0
              || strcasecmp(vid(tree_ident(e)), "ternary_logic") == 0)
             && nparams == 3) {
            // `?:` is max(L,R) wide: BOTH arms are context-determined, so a
            // width-wrong arm escapes into the concat.  The condition is
            // self-determined (only its truth matters) -- flag not passed.
            fputs("((", f);
            emit_expr(f, tree_value(tree_param(e, 0)));
            fputs(") ? (", f);
            emit_ctx_operand(f, tree_value(tree_param(e, 1)), celem);
            fputs(") : (", f);
            emit_ctx_operand(f, tree_value(tree_param(e, 2)), celem);
            fputs("))", f);
            break;
         }
         // l3d_bit_read(a, idx) -> value bit idx of a. Works for any base
         // expression and a variable index (mirrors the const-base T_ARRAY_REF
         // lowering).
         //
         // The obvious rendering, `((a >> idx) & 1'b1)`, has the right VALUE but
         // the WRONG WIDTH: Verilog gives a shift/bitwise expression the
         // SELF-DETERMINED width of its left operand, so it is width(a) bits,
         // not 1. Verilog has exactly two contexts that do NOT resize their
         // parts -- a concatenation element and a replication operand -- and
         // vhdl2vlog puts bit-reads into both (`&` -> {a,b}, aggregates,
         // (others=>X) -> {W{X}}). There each bit_read occupies width(a) slots
         // instead of one, so bit k of an N-element concat lands at Verilog bit
         // k*width(a) and the intended vector is silently shredded.
         //
         // sv2vhdl's register-file write masks are exactly that shape:
         // eh2_dec_gpr_ctl's `l3d_and({32 x l3d_bit_read(v_w0v,j)}, wd0)` with
         // width(v_w0v)=31 emitted a 992-bit concat whose low 32 bits are
         // 32'h80000001 -- only bits 0 and 31 of every GPR write were stored.
         // Trailing `!= 1'b0` makes the result SELF-DETERMINED 1 BIT WIDE (a
         // relational always is), which is what the VHDL scalar return type
         // says, without changing the value.
         if (strcasecmp(vid(tree_ident(e)), "l3d_bit_read") == 0
             && nparams == 2) {
            fputs("((((", f);
            emit_expr(f, tree_value(tree_param(e, 0)));
            fputs(") >> (", f);
            emit_expr(f, tree_value(tree_param(e, 1)));
            fputs(")) & 1'b1) != 1'b0)", f);
            break;
         }
         // logic3d relational functions (l3d_lt_s/le_s/gt_s/ge_s, unsigned too)
         // -> Verilog comparison; signed variants wrap operands in $signed.
         {
            bool rsgn = false;
            const char *rop = l3d_relop(id_base(istr(tree_ident(e))), &rsgn);
            if (rop != NULL && nparams == 2) {
               fputc('(', f);
               if (rsgn) fputs("$signed(", f);
               emit_expr(f, tree_value(tree_param(e, 0)));
               if (rsgn) fputc(')', f);
               fprintf(f, " %s ", rop);
               if (rsgn) fputs("$signed(", f);
               emit_expr(f, tree_value(tree_param(e, 1)));
               if (rsgn) fputc(')', f);
               fputc(')', f);
               break;
            }
         }
         // l3d_index(a, s) -> a as an integer index (value plane). s=true is a
         // SIGNED index (l3d_to_signed) -> $signed(a); s=false unsigned -> a.
         if (strcasecmp(vid(tree_ident(e)), "l3d_index") == 0 && nparams == 2) {
            int64_t sgn;
            const bool s = folded_int(tree_value(tree_param(e, 1)), &sgn) && sgn;
            if (s) fputs("$signed(", f);
            emit_expr(f, tree_value(tree_param(e, 0)));
            if (s) fputc(')', f);
            break;
         }
         // l3d_resize_s(a, w) -> signed resize on the value plane. Drop w and
         // wrap in $signed so the surrounding context sign-extends.
         if (strcasecmp(vid(tree_ident(e)), "l3d_resize_s") == 0
             && nparams == 2) {
            // the target width is DROPPED (the context resizes) -- so this is
            // width(a), not the requested width: unsafe inside a concatenation.
            if (celem) DECLINE("l3d_resize_s-in-concat");
            fputs("$signed(", f);
            emit_expr(f, tree_value(tree_param(e, 0)));
            fputc(')', f);
            break;
         }
         // l3d_part_read(a, base, w) -> w-bit value slice a[base +: w], LSB-
         // aligned. Emit ((a >> base) & mask(w)); w must fold to a constant
         // (numeric_std natural), else fall through to the unhandled marker so
         // the leaf is declined rather than mis-translated.
         if (strcasecmp(vid(tree_ident(e)), "l3d_part_read") == 0
             && nparams == 3) {
            int64_t w;
            tree_t a = tree_value(tree_param(e, 0));
            if (folded_int(tree_value(tree_param(e, 2)), &w) && w > 0) {
               if (w <= 64) {
                  // narrow: shift + mask (works for any base expression).
                  // Self-determined width is max(width(a), width(mask literal)),
                  // NOT w -- fine in a resizing context, fatal inside a
                  // concatenation, so decline there.
                  if (celem) DECLINE("narrow-shift-mask-in-concat");
                  fputs("(((", f);
                  emit_expr(f, a);
                  fputs(") >> (", f);
                  emit_expr(f, tree_value(tree_param(e, 1)));
                  // w==64: (1<<64)-1 overflows int64; emit the Verilog-sized
                  // all-ones mask directly (a C `0x..ull` literal is NOT valid
                  // Verilog -- yosys read it as `0` then a stray identifier,
                  // "unexpected TOK_ID", failing the whole ic_mem subtree synth).
                  if (w == 64) fputs(")) & 64'hffffffffffffffff)", f);
                  else fprintf(f, ")) & %lld)", (long long)((INT64_C(1) << w)-1));
                  break;
               }
               else if (tree_kind(a) == T_REF) {
                  // wide (>64b): Verilog indexed part-select on a net; yosys
                  // lowers it to $shiftx (wide dynamic select). The base may be
                  // a variable/expression; the width is the folded constant.
                  // Unlike the mask form above this is EXACTLY w bits wide --
                  // an indexed part-select is self-determined at its width --
                  // so it needs no concat-element guard.
                  emit_expr(f, a);
                  fputs("[(", f);
                  emit_expr(f, tree_value(tree_param(e, 1)));
                  fprintf(f, ") +: %lld]", (long long)w);
                  break;
               }
               // wide read of a non-net expression -> fall through to decline
            }
         }
         // sv2vhdl one-hot decoders: bit i of the result is (x == i), i.e. a
         // shifted 1. Works for any argument expression.
         if ((strcasecmp(vid(tree_ident(e)), "decode2_4") == 0
              || strcasecmp(vid(tree_ident(e)), "decode3_8") == 0)
             && nparams == 1) {
            const bool is38 = tolower((unsigned char)vid(tree_ident(e))[6]) == '3';
            fprintf(f, "(%s'b1 << (", is38 ? "8" : "4");
            emit_expr(f, tree_value(tree_param(e, 0)));
            fputs("))", f);
            break;
         }
         // sv2vhdl one-hot -> binary encoders (assume one-hot input, OR trees):
         //   f_Enc8to3(d):  {d4|d5|d6|d7, d2|d3|d6|d7, d1|d3|d5|d7}
         //   encode8_3(x):  {|x[7:4],     x7|x6|x3|x2, x7|x5|x3|x1}   (identical)
         // Need a simple ref for the bit-selects; else decline below.
         if ((strcasecmp(vid(tree_ident(e)), "f_enc8to3") == 0
              || strcasecmp(vid(tree_ident(e)), "encode8_3") == 0)
             && nparams == 1) {
            tree_t a0 = tree_value(tree_param(e, 0));
            if (tree_kind(a0) == T_REF) {
               const char *nm = vid(tree_ident(a0));
               fprintf(f, "{(%s[4]|%s[5]|%s[6]|%s[7]),"
                          "(%s[2]|%s[3]|%s[6]|%s[7]),"
                          "(%s[1]|%s[3]|%s[5]|%s[7])}",
                       nm, nm, nm, nm, nm, nm, nm, nm, nm, nm, nm, nm);
               break;
            }
         }
         // countones(x) -> explicit bit-sum (the subtree is read as plain .v,
         // so SystemVerilog's $countones isn't available). Only for a simple
         // signal ref of known width; anything else declines below.
         if (strcasecmp(vid(tree_ident(e)), "countones") == 0 && nparams == 1) {
            tree_t a0 = tree_value(tree_param(e, 0));
            if (tree_kind(a0) == T_REF && tree_has_type(a0)
                && type_is_array(tree_type(a0))
                && type_const_bounds(tree_type(a0))) {
               const int w = type_width(tree_type(a0));
               const char *nm = vid(tree_ident(a0));
               // Verilog self-determines the sum width from the operands — all
               // 1-bit here, so without a wide anchor the count TRUNCATES to
               // one bit (broke lsu_bus_buffer's num_valids -> bus-full logic).
               fputs("(8'd0", f);
               for (int b = 0; b < w; b++)
                  fprintf(f, " + %s[%d]", nm, b);
               fputc(')', f);
               break;
            }
         }
         // `a mod 2**k` (positive power-of-two constant modulus) -> `a & (2**k-1)`.
         // EXACT, not an approximation: VHDL's mod takes the sign of the RIGHT
         // operand, so with a positive modulus the result is always in [0, m),
         // which for m = 2**k is precisely the low k bits of a two's-complement
         // `a` — negative operands included. Verilog's `%` would be WRONG here
         // (it takes the sign of the LEFT operand), so `mod` is never mapped to
         // `%`; anything but a power-of-two constant modulus keeps declining.
         // (`reg3 mod 2**20`, `(count+1) mod 2**COD_COLOR` — the ITC'99 idiom
         // for "wrap to an N-bit field".)
         if (!strcmp(fn, "\"mod\"") && nparams == 2) {
            int64_t m;
            if (folded_int(tree_value(tree_param(e, 1)), &m)
                && m > 0 && m <= (INT64_C(1) << 30) && (m & (m - 1)) == 0) {
               // the unsized decimal mask makes this max(width(a), 32) wide,
               // not the k bits the VHDL type has -- unsafe inside a concat.
               if (celem) DECLINE("pow2-mod-mask-in-concat");
               fputc('(', f);
               emit_expr(f, tree_value(tree_param(e, 0)));
               fprintf(f, " & %"PRIi64")", m - 1);
               break;
            }
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
            // numeric_std `*` returns wa+wb bits in VHDL but only max(wa,wb)
            // self-determined in Verilog -- a concatenation element would be
            // truncated.  Every other operator in vlog_op is width-preserving
            // (+/- are max(wa,wb) in both; bitwise/shift keep the left width;
            // relationals are 1 bit in both).
            if (celem && strcmp(op, "*") == 0) DECLINE("multiply-in-concat");
            // ...and the operands are CONTEXT-determined for every one of those
            // width-preserving operators, so a too-WIDE operand still poisons
            // the result: max(L,R) is only right when L and R are.  $signed()
            // does not change a self-determined width, so it passes through too.
            // A shift AMOUNT is self-determined -- nothing escapes from there.
            const bool ctxw = vlog_op_ctx_width(op);
            const bool shft = (strcmp(op, "<<") == 0 || strcmp(op, ">>") == 0);
            fputc('(', f);
            if (sgn) fputs("$signed(", f);
            emit_ctx_operand(f, a0, celem && ctxw);
            if (sgn) fputc(')', f);
            fprintf(f, " %s ", op);
            if (sgn) fputs("$signed(", f);
            emit_ctx_operand(f, a1, celem && ctxw && !shft);
            if (sgn) fputc(')', f);
            fputc(')', f);
         }
         else if (op != NULL && nparams == 1) {
            const bool sgn = (tree_has_type(tree_value(tree_param(e, 0)))
                              && type_is_signed(tree_type(tree_value(tree_param(e, 0)))));
            fprintf(f, "%s(", op);
            if (sgn) fputs("$signed(", f);
            // unary `~` / `-`: result is the operand's width -> context-determined
            emit_ctx_operand(f, tree_value(tree_param(e, 0)), celem);
            if (sgn) fputc(')', f);
            fputc(')', f);
         }
         else if (l3dop != NULL && l3dk == 0 && nparams == 2) {
            // bitwise l3d_and/or/xor/... are max(L,R) wide (context-determined
            // operands); l3d_eq1/ne1 emit `==`/`!=` and are always 1 bit.
            const bool ctxw = vlog_op_ctx_width(l3dop);
            fputc('(', f);
            emit_ctx_operand(f, tree_value(tree_param(e, 0)), celem && ctxw);
            fprintf(f, " %s ", l3dop);
            emit_ctx_operand(f, tree_value(tree_param(e, 1)), celem && ctxw);
            fputc(')', f);
         }
         else if (l3dop != NULL && (l3dk == 1 || l3dk == 3) && nparams >= 1) {
            // kind 1 is `~a` -- as wide as its operand, so sensitivity passes
            // through.  Kind 3 is a REDUCTION (`|a`, `&a`, `^a`): always exactly
            // one bit, operand self-determined, nothing escapes.
            fprintf(f, "(%s", l3dop);
            emit_ctx_operand(f, tree_value(tree_param(e, 0)), celem && l3dk == 1);
            fputc(')', f);
         }
         else if (l3dop != NULL && l3dk == 2 && nparams >= 1) {
            // resize / to_signed / to_unsigned: the width arg is dropped and
            // Verilog's surrounding (assignment or operator) context does the
            // resize. Two cases need help beyond that:
            //  (1) a SIGNED result must be $signed-wrapped so the context
            //      sign-extends (Verilog zero-extends a bare unsigned value) --
            //      e.g. `sel := resize(c20,32)` as an assignment RHS, where the
            //      operator-level $signed wrapping never runs. Idempotent under
            //      an operator that also wraps.
            //  (2) a WIDENING UNSIGNED result must materialize its zero-extension
            //      rather than drop it: dropping is unsound when the value is
            //      later reinterpreted as signed (`signed(resize(bits,32))`) --
            //      the value's own MSB would wrongly become the sign bit.
            // l3d resize has a non-signed, non-array-numeric type so it stays
            // the bare identity below.
            tree_t a0 = tree_value(tree_param(e, 0));
            const bool sgn = tree_has_type(e) && type_is_signed(tree_type(e));
            // Target width: the resize/to_(un)signed size arg (param 1) folds to
            // a constant; the FCALL's own type is the unconstrained numeric_std
            // return type (no const bounds), so read the width from the arg.
            int64_t nwi = -1;
            int nw = -1, aw = -1;
            if (nparams >= 2 && folded_int(tree_value(tree_param(e, 1)), &nwi))
               nw = (int)nwi;
            // A ONE-PARAMETER identity has no width argument, so without this
            // nw stays -1, the `nw > 0` in ident_bad below is false, and the
            // concat-element width check CANNOT FIRE -- an unguarded tunnel
            // through the very guard this code exists to be.  Five of the
            // sixteen identities return a SCALAR while taking a vector
            // (unsigned_to_l3d_bit, l3d_to_bit, to_bit, is_one,
            // boolean_to_logic), so each occupies width(operand) concat slots
            // instead of one -- the same shredded-vector shape as the
            // l3d_bit_read defect f251009c0 fixed, reached through a different
            // door.  MEASURED with test/accel/l3did: eight
            // unsigned_to_l3d_bit(u) elements over a 4-bit u emitted as
            // `{{{{{{{u,u},u},u},u},u},u},u}` -- 32 bits into an 8-bit target,
            // ref Y=21760 vs accel Y=15602056, chunk installed, yosys silent.
            //
            // Take the width from the CALL'S OWN RESULT TYPE, which is what
            // the VHDL says the value is.
            if (nw < 0 && tree_has_type(e))
               nw = type_vlog_width(tree_type(e));
            if (tree_has_type(a0) && type_is_array(tree_type(a0))
                && type_const_bounds(tree_type(a0)))
               aw = (int)type_width(tree_type(a0));
            // IDENTITY EMISSION, i.e. a verbatim pass-through -- so it is
            // width-transparent in BOTH directions inside a concatenation:
            //  (a) its own result is width(a0)-as-EMITTED, not the requested nw;
            //  (b) whatever a0 emits lands straight in the concat, so a
            //      width-wrong expression NESTED under it must still be caught.
            // (b) is why these emissions have to hand the flag down exactly like
            // T_TYPE_CONV -- without that, resize/to_l3d/unsigned_to_l3d/... were
            // an unguarded tunnel through the guard, and they are the single most
            // common wrappers sv2vhdl emits.
            //
            // For (a) compare against emitted_width(a0), NOT the VHDL type width:
            // the operand of a resize is normally an unconstrained numeric_std
            // intermediate whose type width is unknown, while the width that will
            // really be printed is recoverable by descending the identity chain.
            // Unknown (-1) never equals nw, so it declines -- correct, because an
            // unknown width inside {} genuinely cannot be shown safe.  The check
            // applies only to the two IDENTITY branches: the zero-extend branch
            // below prints an exactly-nw-bit form, so testing it there would be a
            // false positive.
            const bool ident_bad =
               (celem && nw > 0 && emitted_width(a0, 0) != nw);
            if (sgn) {
               if (ident_bad) DECLINE("identity-width-in-concat");
               // $signed(x) keeps x's self-determined width -> transparent.
               fputs("$signed(", f);
               emit_ctx_operand(f, a0, celem);
               fputc(')', f);
            }
            else if (nw > 0 && aw > 0 && nw > aw) {
               // exact nw bits PROVIDED a0 really emits aw -- emit_concat_elem
               // makes a0 prove that for itself.
               fprintf(f, "{%d'b0, ", nw - aw);
               emit_concat_elem(f, a0);
               fputc('}', f);
            }
            else {
               if (ident_bad) DECLINE("identity-width-in-concat");
               emit_ctx_operand(f, a0, celem);
            }
         }
         else if (g_func_set != NULL
                  && hset_contains(g_func_set, ident_new(vid(tree_ident(e))))) {
            // call to a module-local design function -> Verilog function call
            fprintf(f, "%s(", vid(tree_ident(e)));
            for (int i = 0; i < nparams; i++) {
               if (i) fputs(", ", f);
               emit_expr(f, tree_value(tree_param(e, i)));
            }
            fputc(')', f);
         }
         else {
            // rising_edge / unhandled function: surface for inspection
            DECLINE("function-unhandled");
            fprintf(f, "/*fn %s*/", vid(tree_ident(e)));
            if (nparams > 0) emit_expr(f, tree_value(tree_param(e, 0)));
         }
      }
      break;
   case T_TYPE_CONV:
   case T_QUALIFIED:
   case T_INERTIAL:   // inertial-delay waveform wrapper -> inner value
      g_concat_elem = celem;         // emitted verbatim: width sensitivity passes through
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
            // `!= 1'b0` forces the SELF-DETERMINED width to 1 -- see the
            // l3d_bit_read case above; a bare shift+mask is as wide as the
            // inlined constant and corrupts any enclosing concatenation.
            fputs("((((", f); emit_expr(f, base); fputs(") >> (", f);
            emit_expr(f, tree_value(tree_param(e, 0)));
            fputs(")) & 1'b1) != 1'b0)", f);
         }
         else {
            // Element width: a 2-D array `array(..) of vector(W-1 downto 0)` is
            // flattened to a 1-D wire [N*W-1:0], so a scalar index selects a W-bit
            // WORD, not a bit. Emit a part-select scaled by W (`[(idx)*W +: W]`)
            // for multi-bit elements; a plain `[idx]` bit-select only for 1-bit
            // elements (logic3d_vector). Without the scaling a RAM `mem(adr)` read
            // /write touches ONE bit (the VeeR icache 256x34 SRAM corruption).
            // Memory-qualified signals are declared as true Verilog memories
            // (word-indexed), so their index is emitted UNscaled.
            const bool memsig = tree_kind(base) == T_REF && tree_has_ref(base)
               && sig_is_mem(tree_ref(base));
            int ew = 1;
            type_t et = tree_type(e);
            if (!memsig && type_is_array(et) && type_const_bounds(et))
               ew = type_width(et);
            emit_expr(f, base);
            if (tree_params(e) > 0) {
               if (ew > 1) {
                  fputs("[(", f);
                  emit_expr(f, tree_value(tree_param(e, 0)));
                  fprintf(f, ")*%d +: %d]", ew, ew);
               }
               else {
                  fputc('[', f);
                  emit_expr(f, tree_value(tree_param(e, 0)));
                  fputc(']', f);
               }
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
            // type_width fatals on a variable-bounded aggregate type (e.g. the
            // slice target of `s(k downto 1) <= (others => 1)` where k is a
            // variable) — guard so those fall through to the g_unhandled decline.
            if (n == 1 && tree_subkind(tree_assoc(e, 0)) == A_OTHERS
                && type_const_bounds(tree_type(e))) {
               const int w = type_width(tree_type(e));
               if (w > 0) {
                  fprintf(f, "{%d{", w);
                  emit_concat_elem(f, tree_value(tree_assoc(e, 0)));
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
                  emit_concat_elem(f, tree_value(tree_assoc(e, i)));
               }
               fputc('}', f);
               break;
            }
            // named / indexed / range / mixed-with-others (1-bit elements)
            if (emit_agg_general(f, e)) break;
         }
         // Not faithfully translated — decline rather than silently emit 0.
         DECLINE("aggregate-not-translatable");
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
      DECLINE("expression-unhandled"); fprintf(f, "/*?expr k=%d*/0", tree_kind(e));
      break;
   }
}

// Recognise a folded '0'/'1' literal of a bit-like enum type. The enum
// POSITION of '1' differs per type -- 1 in BIT, 3 in std_(u)logic and in
// logic3d (L3D_0=2, L3D_1=3) -- so resolve the position through the type's
// own literal list instead of hard-coding one encoding. *one = it is '1'.
static bool folded_bit(tree_t e, bool *one)
{
   int64_t lv;
   if (e == NULL || !folded_int(e, &lv) || lv < 0) return false;
   if (!tree_has_type(e)) return false;
   type_t b = type_base_recur(tree_type(e));
   if (type_kind(b) != T_ENUM) return false;
   if (lv >= type_enum_literals(b)) return false;
   tree_t lit = type_enum_literal(b, lv);
   if (!tree_has_ident(lit)) return false;
   const char *s = id_base(istr(tree_ident(lit)));
   if (!strcmp(s, "'0'") || !strcasecmp(s, "L3D_0")) { *one = false; return true; }
   if (!strcmp(s, "'1'") || !strcasecmp(s, "L3D_1")) { *one = true;  return true; }
   return false;
}

// Two expressions naming the SAME object (same resolved declaration). Used to
// require `clk'event and clk = '1'` -- `clk'event and rst = '1'` is not an edge.
static bool same_object(tree_t a, tree_t b)
{
   if (a == NULL || b == NULL) return false;
   if (tree_kind(a) != T_REF || tree_kind(b) != T_REF) return false;
   if (!tree_has_ref(a) || !tree_has_ref(b)) return false;
   return tree_ref(a) == tree_ref(b);
}

// `<sig>'EVENT` -> the prefix signal, else NULL.
static tree_t event_attr_of(tree_t e)
{
   if (e == NULL || tree_kind(e) != T_ATTR_REF) return NULL;
   if (tree_subkind(e) != ATTR_EVENT) return NULL;
   if (!tree_has_name(e)) return NULL;
   return tree_name(e);
}

// `<sig> = '1'` / `<sig> = '0'` -> the tested signal, *one = compared to '1'.
// Accepts the VHDL "=" operator and the logic3d value-plane equality.
static tree_t level_test_of(tree_t e, bool *one)
{
   if (e == NULL || tree_kind(e) != T_FCALL) return NULL;
   const char *fn = id_base(istr(tree_ident(e)));
   if (strcasecmp(fn, "\"=\"") && strcasecmp(fn, "l3d_eq1")
       && strcasecmp(fn, "l3d_eq")) return NULL;
   if (tree_params(e) < 2) return NULL;
   tree_t l = tree_value(tree_param(e, 0)), r = tree_value(tree_param(e, 1));
   if (folded_bit(r, one)) return l;    // sig = '1'
   if (folded_bit(l, one)) return r;    // '1' = sig
   return NULL;
}

// Extract clock/reset edges from an if-condition: a single rising/falling_edge,
// the VHDL-87 `clk'event and clk = '1'` idiom, or an OR of them (async-reset
// flop, translated from `always @(posedge clk or negedge rst)`). Fills
// sig[]/pe[] (pe = posedge), returns edge count or 0.
static int edges_of(tree_t test, tree_t *sig, bool *pe, int max)
{
   if (test == NULL || max < 1) return 0;
   if (tree_kind(test) != T_FCALL) return 0;
   const char *fn = id_base(istr(tree_ident(test)));
   if (!strcasecmp(fn, "RISING_EDGE") || !strcasecmp(fn, "FALLING_EDGE")) {
      if (tree_params(test) < 1) return 0;
      sig[0] = tree_value(tree_param(test, 0));
      pe[0]  = !strcasecmp(fn, "RISING_EDGE");
      return 1;
   }
   // The pre-`rising_edge` edge idiom: `clk'event and clk = '1'` (posedge) /
   // `... = '0'` (negedge) -- how every ITC'99 / VHDL-87-era design writes a
   // flop. Either operand order, and the conjunction may be the predefined
   // boolean "and" or the logic3d l3d_and (promoted sources). The 'EVENT
   // prefix and the level-tested signal MUST be the same object: `clk'event
   // and rst = '1'` is a level test qualified by a clock event, NOT an edge,
   // and must keep declining. Anything else about the shape -> decline.
   if (!strcasecmp(fn, "\"and\"") || !strcasecmp(fn, "l3d_and")) {
      if (tree_params(test) < 2) return 0;
      tree_t a = tree_value(tree_param(test, 0));
      tree_t b = tree_value(tree_param(test, 1));
      tree_t ev = event_attr_of(a), lvl = b;
      if (ev == NULL) { ev = event_attr_of(b); lvl = a; }
      if (ev != NULL) {
         bool one = false;
         tree_t lsig = level_test_of(lvl, &one);
         if (lsig != NULL && same_object(ev, lsig)) {
            sig[0] = ev;
            pe[0]  = one;
            return 1;
         }
         return 0;      // 'event present but not the edge shape -> decline
      }
      return 0;
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
// Returns the wrapping cond (non-NULL = clocked), fills body_if + the edge list
// and (if given) the enclosing T_IF so the caller can find an async-reset elsif.
static tree_t clock_of(tree_t proc, tree_t *body_if, tree_t *sig, bool *pe,
                       int *ne, tree_t *ifstmt)
{
   const int n = tree_stmts(proc);
   for (int i = 0; i < n; i++) {
      tree_t s = tree_stmt(proc, i);
      if (tree_kind(s) != T_IF) continue;
      // Scan ALL branches for the clock edge, not just the first: the standard
      // async-reset flop puts `if rst='1' then <reset> elsif rising_edge(clk)`,
      // so the clock is the SECOND condition. body_if is the edge branch (the
      // clocked body).
      const int nconds = tree_conds(s);
      for (int j = 0; j < nconds; j++) {
         tree_t c = tree_cond(s, j);
         tree_t test = tree_has_value(c) ? tree_value(c) : NULL;
         int k = edges_of(test, sig, pe, 8);
         if (k > 0) {
            *body_if = c; *ne = k;
            if (ifstmt != NULL) *ifstmt = s;
            return c;
         }
      }
   }
   return NULL;
}

// Detect an async-reset branch on a clocked process: a `rst = '1'|'0'` LEVEL
// test on a signal (in the process sensitivity), the way VHDL expresses an
// async-reset flop. Scans every branch EXCEPT the clock-edge one. Fills the
// reset signal + polarity (pe = active-high '1' -> posedge) and *before =
// whether the reset branch precedes the clock branch. In the STANDARD flop
// `if rst then <reset> elsif rising_edge(clk) then <sync>` the reset is FIRST
// (arst-priority) -> maps exactly to `always @(posedge clk or posedge rst)
// if (rst) <reset> else <sync>`. If the reset comes AFTER the clock (rising_edge
// first), the VHDL is clk-priority -- NOT a standard $adff -- and the caller
// declines rather than emit the wrong (arst-priority) form.
static tree_t areset_of(tree_t ifstmt, tree_t clockcond, tree_t *rsig,
                        bool *rpe, bool *before)
{
   const int nc = tree_conds(ifstmt);
   int clk_idx = nc;
   for (int i = 0; i < nc; i++)
      if (tree_cond(ifstmt, i) == clockcond) { clk_idx = i; break; }
   for (int i = 0; i < nc; i++) {
      tree_t c = tree_cond(ifstmt, i);
      if (c == clockcond || !tree_has_value(c)) continue;   // clock / bare else
      tree_t test = tree_value(c);
      // `rst = '0'|'1'` on any bit-like type. Was hard-coded to the std_logic
      // enum positions ('0'=2, '1'=3), so a BIT-typed reset ('0'=0, '1'=1) --
      // what every ITC'99 design uses -- was not recognised and the whole
      // reset branch was then silently DROPPED from the always block.
      bool one = false;
      tree_t rs = level_test_of(test, &one);
      if (rs == NULL) continue;
      *rsig   = rs;
      *rpe    = one;
      *before = (i < clk_idx);
      return c;
   }
   return NULL;
}

static void emit_seq(FILE *f, tree_t s, int ind)
{
   switch (tree_kind(s)) {
   case T_SIGNAL_ASSIGN:
   case T_VAR_ASSIGN:
      {
         // NBA-shadow memory pair rewrites (see g_shadow_var):
         //   v := sig      -> elided
         //   sig <= v      -> elided
         //   v(idx) := e   -> sig[idx] <= e   (direct NBA memory write)
         tree_t tg0 = tree_target(s);
         if (tree_kind(s) == T_VAR_ASSIGN) {
            if (tree_kind(tg0) == T_REF && tree_has_ref(tg0)
                && shadow_sig_of(tree_ref(tg0)) != NULL)
               break;                              // the whole-array copy
            if (tree_kind(tg0) == T_ARRAY_REF && tree_params(tg0) == 1) {
               tree_t base = tree_value(tg0);
               if (tree_kind(base) == T_REF && tree_has_ref(base)) {
                  tree_t sig = shadow_sig_of(tree_ref(base));
                  if (sig != NULL) {
                     tab(f, ind);
                     fprintf(f, "%s[", vid(tree_ident(sig)));
                     emit_expr(f, tree_value(tree_param(tg0, 0)));
                     fputs("] <= ", f);
                     emit_expr(f, tree_value(s));
                     fputs(";\n", f);
                     break;
                  }
               }
            }
         }
         else if (tree_kind(tg0) == T_REF && tree_has_ref(tg0)
                  && sig_has_shadow(tree_ref(tg0)))
            break;                                 // the whole-array writeback
         // Whole-array assignment to a memory-qualified decl: a memory has
         // no aggregate l-value in Verilog (the naive emission was
         // `instqueue = {0,...}` -- yosys-undefined).  A positional
         // CONSTANT aggregate (ITC b15's InstQueue reset) expands to one
         // word write per element, leftmost element = LEFT bound (VHDL
         // positional order); anything fancier declines loudly.
         if (tree_kind(tg0) == T_REF && tree_has_ref(tg0)
             && sig_is_mem(tree_ref(tg0))) {
            unsigned mnw, mew;
            // signal assigns keep the value in a waveform; tree_value(s)
            // directly is a FATAL object lookup (same class as mem_scan_cb)
            tree_t v0 = NULL;
            if (tree_kind(s) == T_VAR_ASSIGN)
               v0 = tree_value(s);
            else if (tree_waveforms(s) > 0
                     && tree_has_value(tree_waveform(s, 0)))
               v0 = tree_value(tree_waveform(s, 0));
            if (v0 == NULL) {
               DECLINE("memory-whole-array-assign");
               fprintf(f, "  /*?memagg*/\n");
               break;
            }
            if (mem_shape(tree_type(tree_ref(tg0)), &mnw, &mew)
                && tree_kind(v0) == T_AGGREGATE
                && (unsigned)tree_assocs(v0) == mnw) {
               bool allpos = true;
               for (int i = 0; i < tree_assocs(v0); i++)
                  if (tree_subkind(tree_assoc(v0, i)) != A_POS)
                     allpos = false;
               if (allpos) {
                  const bool nba = tree_kind(s) == T_SIGNAL_ASSIGN;
                  for (unsigned i = 0; i < mnw; i++) {
                     tab(f, ind);
                     fprintf(f, "%s[%u] %s ", vid(tree_ident(tg0)),
                             mnw - 1 - i, nba ? "<=" : "=");
                     emit_expr(f, tree_value(tree_assoc(v0, i)));
                     fputs(";\n", f);
                  }
                  break;
               }
            }
            DECLINE("memory-whole-array-assign");
            fprintf(f, "  /*?memagg*/\n");
            break;
         }
         tab(f, ind);
         emit_expr(f, tree_target(s));
         // VHDL variables update immediately (Verilog blocking '='); signals are
         // scheduled (non-blocking '<='). A reduction-loop accumulator is a
         // variable and MUST be blocking or each iteration reads the stale value.
         const bool blocking = tree_kind(s) == T_VAR_ASSIGN || g_comb_proc;
         fputs(blocking ? " = " : " <= ", f);
         if (tree_kind(s) == T_VAR_ASSIGN)
            emit_expr(f, tree_value(s));
         else if (tree_waveforms(s) > 0 && tree_has_value(tree_waveform(s, 0)))
            emit_expr(f, tree_value(tree_waveform(s, 0)));
         else { DECLINE("null-waveform"); fputs("0/*null-wave*/", f); }  // disconnect/null waveform — decline
         fputs(";\n", f);
      }
      break;
   case T_DEPOSIT:
      {
         // nvc's deposit statement (`target := value` on a SIGNAL): an
         // immediate in-place update — sv2vhdl uses it for Verilog BLOCKING
         // assignments to signals (e.g. accumulators in reduction loops).
         // Verilog equivalent: a blocking '='.
         tab(f, ind);
         emit_expr(f, tree_target(s));
         fputs(" = ", f);
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
         // Own storage, not vid()'s rotating static buffer and not a `static`:
         // `iv` is re-printed after emit_expr() on the loop bounds, and a call
         // in a bound would wrap vid()'s 8-slot ring and rename the loop
         // variable mid-header. (Same class of bug as emit_function's `name`.)
         // Sized to match emit_proc_locals' `mn[120]`, which emits the matching
         // module-level `integer` declaration -- a shorter buffer here would
         // truncate a long index name to a DIFFERENT string than the one
         // declared, leaving the for-header driving an undeclared identifier.
         char ivbuf[120] = "i";
         const char *iv = ivbuf;
         if (idecl != NULL)
            snprintf(ivbuf, sizeof ivbuf, "%s%s", vid(tree_ident(idecl)),
                     ren_decl(idecl) ? ren_suffix(idecl) : "");
         const bool to = (tree_subkind(r) == RANGE_TO);
         if (tree_subkind(r) != RANGE_TO && tree_subkind(r) != RANGE_DOWNTO) {
            DECLINE("for-range-subkind"); tab(f, ind);
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
                  DECLINE("range-choice"); fputs("/*?range-choice*/", f); continue;
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
      DECLINE("while-loop-nonconstant"); tab(f, ind); fputs("/*?while-loop*/\n", f);
      break;
   case T_LOOP:
      {
         // `loop <body> wait on <sens>; end loop` is a process-sensitivity loop
         // (the body runs once per wake) -- emit the body inline; the trailing
         // T_WAIT is skipped by its case. A loop with NO wait is a real
         // (bounded/exit) loop we don't translate yet -> decline.
         bool has_wait = false;
         for (int i = 0; i < tree_stmts(s); i++)
            if (tree_kind(tree_stmt(s, i)) == T_WAIT) { has_wait = true; break; }
         if (has_wait)
            emit_stmt_list(f, s, ind);
         else { DECLINE("loop-without-wait"); tab(f, ind); fputs("/*?loop-no-wait*/\n", f); }
      }
      break;
   case T_WAIT:
      break;   // process sensitivity — not emitted
   case T_RETURN:
      // Inside a function body: `return x` -> `<func> = x` (Verilog assigns the
      // result to the function name). Only the trailing return is reached in the
      // simple combinational functions handled here.
      if (g_func_ret_name != NULL && tree_has_value(s)) {
         tab(f, ind);
         fprintf(f, "%s = ", g_func_ret_name);
         emit_expr(f, tree_value(s));
         fputs(";\n", f);
      }
      else { DECLINE("return-not-tail"); tab(f, ind); fputs("/*?return*/\n", f); }
      break;
   default:
      DECLINE("sequential-stmt-unhandled"); tab(f, ind); fprintf(f, "/*?seq k=%d*/\n", tree_kind(s));
      break;
   }
}

// A function whose calls are already translated inline (a vlog_l3d_op table
// entry or a special emit_expr handler) must NOT also be emitted as a Verilog
// function: the definition is dead (calls never reach it) and its body may use
// constructs we don't translate (e.g. Reduce_OR's `for I in X'Range`).
static bool fn_is_builtin(const char *ident)
{
   int k;
   if (vlog_l3d_op(ident, &k) != NULL) return true;
   const char *b = id_base(ident);
   static const char *const sp[] = {
      "ternary_unsigned", "ternary_logic", "l3d_bit_read", "l3d_part_read",
      "l3d_index", "l3d_resize_s", NULL };
   for (int i = 0; sp[i]; i++)
      if (!strcasecmp(b, sp[i])) return true;
   return false;
}

// True if some `return` in this statement list is NOT in TAIL position.
//
// A Verilog function body has no early exit: emit_seq lowers `return x` to
// `<name> = x` and then FALLS THROUGH to whatever follows. That is faithful
// only for a return in tail position -- the last statement of the body, or the
// last statement of a branch that is itself in tail position. An early return
// followed by more statements emits an UNGUARDED assignment that a later one
// overwrites (last write wins), so
//     if c then return A; end if;
//     return B;
// computes B for every input, including c = true. Nothing else in emit_function
// catches this: the return-type scan below finds the trailing top-level return,
// so the function is accepted and silently ships the wrong value. `tail` says
// whether this container itself sits in tail position; a return anywhere inside
// a LOOP is never faithful (the loop would keep iterating), hence tail=false
// there.
static bool has_nontail_return(tree_t container, bool tail)
{
   const int n = tree_stmts(container);
   for (int i = 0; i < n; i++) {
      tree_t s = tree_stmt(container, i);
      const bool last = (i == n - 1);
      switch (tree_kind(s)) {
      case T_RETURN:
         if (!tail || !last) return true;
         break;
      case T_IF:
         {
            const int nc = tree_conds(s);
            for (int j = 0; j < nc; j++)
               if (has_nontail_return(tree_cond(s, j), tail && last)) return true;
         }
         break;
      case T_CASE:
         {
            const int na = tree_stmts(s);   // alternatives
            for (int j = 0; j < na; j++)
               if (has_nontail_return(tree_stmt(s, j), tail && last)) return true;
         }
         break;
      case T_FOR: case T_LOOP: case T_WHILE:
         if (has_nontail_return(s, false)) return true;
         break;
      default:
         break;
      }
   }
   return false;
}

// Emit a VHDL design function (T_FUNC_BODY) as a Verilog function. Handles the
// simple combinational shape sv2ghdl produces for SV helper functions: value
// parameters, local variables, straight-line body, a trailing `return`. Any
// body construct that can't translate bumps g_unhandled -> the whole module
// declines, so a partially-translated function never ships.
static void emit_function(FILE *f, tree_t fn)
{
   // COPY the name -- do NOT borrow vid()'s pointer. vid() returns one of 8
   // ROTATING static buffers, and the body emission below issues far more than
   // 8 vid() calls (emit_expr's T_FCALL dispatch chain alone burns up to 12 for
   // a single call, and every T_REF operand burns one). A borrowed pointer is
   // therefore overwritten with the last-emitted callee/operand identifier
   // before the trailing `return` prints g_func_ret_name, yielding e.g.
   // `l3d_and = pipe_to_thr_result;` instead of `pipe_to_thr = ...`.
   char name[256];
   snprintf(name, sizeof name, "%s", vid(tree_ident(fn)));

   // The signature return type is unconstrained (`logic3d_vector`), so take the
   // result width from the LAST return's (constrained) value type. If it isn't a
   // top-level return with a const-bounded type, decline the module.
   type_t rt = NULL;
   for (int i = tree_stmts(fn) - 1; i >= 0 && rt == NULL; i--) {
      tree_t s = tree_stmt(fn, i);
      if (tree_kind(s) == T_RETURN && tree_has_value(s)
          && tree_has_type(tree_value(s)))
         rt = tree_type(tree_value(s));
   }
   if (rt == NULL || (type_is_array(rt) && !type_const_bounds(rt))) {
      DECLINE("function-return-width");
      return;
   }

   // Verilog has no early `return` -- decline rather than mistranslate. See
   // has_nontail_return: without this an early return silently loses to the
   // trailing one.
   if (has_nontail_return(fn, true)) {
      DECLINE("function-early-return");
      return;
   }

   fputs("  function ", f);
   emit_range(f, rt);
   fprintf(f, "%s;\n", name);

   const int nports = tree_ports(fn);
   for (int i = 0; i < nports; i++) {
      tree_t p = tree_port(fn, i);
      fputs("    input ", f);
      emit_range(f, tree_type(p));
      fprintf(f, "%s;\n", vid(tree_ident(p)));
   }
   for (int i = 0; i < tree_decls(fn); i++) {
      tree_t d = tree_decl(fn, i);
      if (tree_kind(d) != T_VAR_DECL) continue;
      fputs("    reg ", f);
      emit_range(f, tree_type(d));
      fprintf(f, "%s;\n", vid(tree_ident(d)));
   }

   fputs("    begin\n", f);
   const char *save = g_func_ret_name;
   g_func_ret_name = name;
   emit_stmt_list(f, fn, 6);
   g_func_ret_name = save;
   fputs("    end\n  endfunction\n", f);
}

// A process whose body is `loop <stmts> wait on <sens>; end loop` is iverilog's
// rendering of an SV always block -- the loop just re-runs the body on each
// wake. Return the wrapping T_LOOP as the effective statement container (its
// trailing T_WAIT is skipped by emit_seq); otherwise return the process itself.
static tree_t proc_body(tree_t p)
{
   tree_t loop = NULL;
   const int n = tree_stmts(p);
   for (int i = 0; i < n; i++) {
      tree_t s = tree_stmt(p, i);
      if (tree_kind(s) == T_WAIT) continue;
      if (tree_kind(s) == T_LOOP && loop == NULL) loop = s;
      else return p;   // anything besides one bare loop -> not the pattern
   }
   return loop != NULL ? loop : p;
}

static void emit_process(FILE *f, tree_t p0)
{
   tree_t p = proc_body(p0);   // unwrap the process-sensitivity loop
   tree_t body_if = NULL, sig[8], ifstmt = NULL;
   bool pe[8];
   int ne = 0;
   tree_t clk = clock_of(p, &body_if, sig, pe, &ne, &ifstmt);
   if (clk != NULL) {
      tree_t rsig = NULL; bool rpe = false, rbefore = false;
      tree_t rcond = (ifstmt != NULL)
         ? areset_of(ifstmt, body_if, &rsig, &rpe, &rbefore) : NULL;
      if (rcond != NULL && !rbefore) {
         // Swapped `if clk elsif rst` form is clk-priority -- not a standard
         // $adff -- so decline rather than emit the wrong arst-priority form.
         DECLINE("reset-clk-priority");
         rcond = NULL;
      }
      fputs("  always @(", f);
      for (int i = 0; i < ne; i++) {
         if (i) fputs(" or ", f);
         fputs(pe[i] ? "posedge " : "negedge ", f);
         emit_expr(f, sig[i]);
      }
      if (rcond != NULL) {
         fputs(" or ", f);
         fputs(rpe ? "posedge " : "negedge ", f);
         emit_expr(f, rsig);
      }
      fputs(") begin\n", f);
      // The tgt-vhdl NBA idiom surrounds the edge-guard if with statements
      // that are part of the register semantics and MUST be kept:
      //     v_nba_<r> := <r>;              -- shadow pre-load  (before the if)
      //     if <edges> then ... end if;
      //     wait for 0 ns;                 -- delta delay (subsumed by `<=`)
      //     <r> <= v_nba_<r>;              -- THE COMMIT      (after the if)
      //     wait on <sens>;
      // Dropping the tail left every rvdff-family register never written --
      // yosys folded the whole flop chain to constant 0 (VeeR accel
      // divergence). Emit the pre-statements (variable assigns, blocking)
      // at the top of the always block and the post-statements at the
      // bottom (signal assigns emit as nonblocking `<=`, which reproduces
      // the wait-for-0 delta semantics); T_WAITs emit nothing.
      {
         const int np = tree_stmts(p);
         int ifidx = np;
         for (int i = 0; i < np; i++)
            if (tree_stmt(p, i) == ifstmt) { ifidx = i; break; }
         if (ifidx < np)
            emit_stmt_list_range(f, p, 0, ifidx, 4);
         if (rcond != NULL) {
            fputs("    if (", f);
            emit_expr(f, tree_value(rcond));
            fputs(") begin\n", f);
            emit_stmt_list(f, rcond, 6);
            fputs("    end else begin\n", f);
            emit_stmt_list(f, body_if, 6);
            fputs("    end\n", f);
         }
         else
            emit_stmt_list(f, body_if, 4);
         if (ifidx < np)
            emit_stmt_list_range(f, p, ifidx + 1, np, 4);
      }
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
         const bool save = g_comb_proc;
         g_comb_proc = true;          // signal assigns here are blocking '='
         emit_stmt_list(f, p, 4);
         g_comb_proc = save;
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
            // Component wrapper: look through to the bound arch and emit the
            // instance directly.  Positional actuals come from THIS block's
            // params (component-port order); vhdl2vlog_comp_inner guarantees
            // the entity's port order matches, and vhdl2vlog_module names/
            // emits the same inner block, so the flatten resolves.
            tree_t cin = vhdl2vlog_comp_inner(s);
            if (cin != NULL) {
               tree_t cih = tree_decl(cin, 0);
               tree_t cent = tree_primary(tree_ref(cih));
               fprintf(f, "  %s %s ",
                       vhdl2vlog_variant_name(tree_ident(cent), cin),
                       vid(tree_ident(s)));
               emit_portmap(f, s);
               fputs(";\n", f);
               break;
            }
            // The marker below prints tree_kind(hier), which is ALWAYS T_HIER,
            // so it cannot say WHICH construct declined -- component
            // instantiation, for-generate, if-generate, case-generate and a
            // plain block statement all land here.  The discriminating kind is
            // the REF's; it goes to the log rather than the marker so that
            // emission stays byte-identical.
            if (getenv("GSM_LOG"))
               fprintf(stderr, "vhdl2vlog: block-ref-not-arch ref_kind=%d (%s)\n",
                       ref != NULL ? (int)tree_kind(ref) : -1,
                       hier != NULL ? istr(tree_ident(hier)) : "?");
            DECLINE("block-ref-not-arch");
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
      DECLINE("concurrent-stmt-unhandled"); fprintf(f, "  /*?stmt k=%d*/\n", tree_kind(s));
      break;
   }
}

// recursively: does statement `s` (or any nested stmt) assign signal `name`?
static bool stmt_assigns(tree_t s, ident_t name)
{
   switch (tree_kind(s)) {
   case T_SIGNAL_ASSIGN:
   case T_VAR_ASSIGN:
      {
         // Peel indexed/slice/record prefixes: `ram(adr) <= d` assigns `ram`
         // just as much as `ram <= ...` does. Matching only bare T_REF left
         // such signals classified `wire` while being driven from an always
         // block (invalid Verilog yosys happened to tolerate) and blocked the
         // memory-shaped emission.
         tree_t tgt = tree_target(s);
         tree_kind_t tk = tree_kind(tgt);
         while (tk == T_ARRAY_REF || tk == T_ARRAY_SLICE || tk == T_RECORD_REF) {
            tgt = tree_value(tgt);
            tk = tree_kind(tgt);
         }
         return tk == T_REF && tree_has_ref(tgt)
            && tree_ident(tree_ref(tgt)) == name;
      }
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
   // The condition is a comparison: either a Verilog operator (id_base the
   // qualified ident, like emit_expr) OR a logic3d relational FUNCTION
   // (l3d_lt_s/... -- the sv2vhdl `<` on a signed field lowers to these).
   if (tree_kind(c) != T_FCALL) return false;
   {
      const char *cb = id_base(istr(tree_ident(c)));
      bool _s;
      if (vlog_op(cb) == NULL && l3d_relop(cb, &_s) == NULL) return false;
   }
   if (!binop_const_operand(c, Vd)) return false;                   // V <relop> CONST
   tree_t iv = tree_value(inc);                                      // increment RHS
   if (!binop_const_operand(iv, Vd)) return false;                  // V <op> CONST

   *Vout = Vd; *initv = tree_value(init_stmt); *cond = c; *incv = iv;
   return true;
}

static void emit_for_from_while(FILE *f, tree_t V, tree_t initv, tree_t cond,
                                tree_t incv, tree_t while_stmt, int ind)
{
   char vn[120];
   const char *sfx = ren_suffix(V);   // renamed hoisted vars: LHS must match refs
   snprintf(vn, sizeof vn, "%s%s", vid(tree_ident(V)), sfx ? sfx : "");
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
// by a concurrent assignment stays a `wire`. The per-decl stmt_assigns() walk
// was O(decls x stmts) — 13K signals x 9K statements in VeeR's dec made module
// emission the accel-install bottleneck (~50s/run in stmt_assigns). Instead
// collect every process-assigned base ident in ONE walk and look up in a set.
static hset_t *g_reg_set = NULL;   // idents assigned inside a process
static hset_t *g_conc_set = NULL;  // idents driven by a CONCURRENT assign

static void reg_scan_cb(tree_t t, void *ctx)
{
   const tree_kind_t k = tree_kind(t);
   if (k != T_SIGNAL_ASSIGN && k != T_VAR_ASSIGN && k != T_DEPOSIT)
      return;
   tree_t tgt = tree_target(t);
   tree_kind_t tk = tree_kind(tgt);
   while (tk == T_ARRAY_REF || tk == T_ARRAY_SLICE || tk == T_RECORD_REF) {
      tgt = tree_value(tgt);
      tk = tree_kind(tgt);
   }
   if (tk == T_REF && tree_has_ref(tgt))
      hset_insert((hset_t *)ctx, tree_ident(tree_ref(tgt)));
}

// Mirror emit_process's decision: a NON-clocked process whose only non-wait
// statement is a single T_SIGNAL_ASSIGN is emitted as a *continuous* `assign`
// (line ~1340), so its target must stay a wire -- NOT a reg. Return that lone
// assign (for conc-set scanning) or NULL if the process becomes an always block.
// Must match emit_process exactly: else a lone-assign target lands in g_reg_set,
// gets declared `reg = <init>`, and the continuous assign folds a driven flop
// input to a constant ("dffs.dout is driving constant bits" in yosys).
static tree_t proc_cont_assign_target(tree_t p0)
{
   tree_t p = proc_body(p0);
   tree_t body_if = NULL, sig[8], ifstmt = NULL;
   bool pe[8];
   int ne = 0;
   if (clock_of(p, &body_if, sig, pe, &ne, &ifstmt) != NULL)
      return NULL;   // clocked -> always block -> reg
   tree_t only = NULL; int cnt = 0;
   const int nst = tree_stmts(p);
   for (int i = 0; i < nst; i++) {
      tree_t s = tree_stmt(p, i);
      if (tree_kind(s) == T_WAIT) continue;
      only = s; cnt++;
   }
   return (cnt == 1 && tree_kind(only) == T_SIGNAL_ASSIGN) ? only : NULL;
}

// A signal wired to a child instance's OUTPUT (or inout/buffer) port is driven
// by that instance -- exactly like a concurrent assign. Record its base ident in
// `set` (g_conc_set) so (a) it is declared `wire`, not `reg`, and (b) the
// undriven-signal fallback does NOT add a second `assign <sig> = <init>`. Without
// this, an init'd signal fed by an instance output got both the instance driver
// AND `assign sig = 0`; yosys folded the flop/gate output to the constant
// ("Cell port ...dffs.dout is driving constant bits: N'0"), the dominant EH2
// whole-core synth failure. Modes come from the child T_BLOCK's ports (elaborated
// instance) or, for a bare T_INSTANCE, the referenced unit's ports.
static void scan_inst_outputs(tree_t s, hset_t *set)
{
   const tree_kind_t sk = tree_kind(s);
   tree_t unit = NULL;
   if (sk == T_BLOCK) unit = s;
   else if (sk == T_INSTANCE && tree_has_ref(s)) unit = tree_ref(s);
   if (unit == NULL) return;
   const int nports  = tree_ports(unit);
   const int nparams = tree_params(s);
   for (int i = 0; i < nparams; i++) {
      tree_t p = tree_param(s, i);
      tree_t formal = NULL;
      if (tree_subkind(p) == P_NAMED) {
         tree_t nm = tree_name(p);
         if (tree_kind(nm) == T_REF && tree_has_ref(nm)) formal = tree_ref(nm);
      }
      else if (i < nports)
         formal = tree_port(unit, i);
      if (formal == NULL || tree_kind(formal) != T_PORT_DECL) continue;
      const port_mode_t mode = tree_subkind(formal);
      if (mode != PORT_OUT && mode != PORT_INOUT && mode != PORT_BUFFER) continue;
      tree_t act = tree_value(p);
      if (act == NULL || tree_kind(act) == T_OPEN) continue;
      tree_kind_t ak = tree_kind(act);
      while (ak == T_ARRAY_REF || ak == T_ARRAY_SLICE || ak == T_RECORD_REF) {
         act = tree_value(act); ak = tree_kind(act);
      }
      if (ak == T_REF && tree_has_ref(act))
         hset_insert(set, tree_ident(tree_ref(act)));
   }
}

static void build_reg_set(tree_t block)
{
   if (g_reg_set != NULL) hset_free(g_reg_set);
   if (g_conc_set != NULL) hset_free(g_conc_set);
   g_reg_set  = hset_new(256);
   g_conc_set = hset_new(256);
   const int nstmts = tree_stmts(block);
   for (int i = 0; i < nstmts; i++) {
      tree_t s = tree_stmt(block, i);
      const tree_kind_t k = tree_kind(s);
      if (k == T_PROCESS) {
         tree_t ca = proc_cont_assign_target(s);
         if (ca != NULL)
            reg_scan_cb(ca, g_conc_set);   // lone signal-assign -> wire
         else
            tree_visit(s, reg_scan_cb, g_reg_set);
      }
      else if (k == T_SIGNAL_ASSIGN)   // concurrent assign
         reg_scan_cb(s, g_conc_set);
      else if (k == T_BLOCK || k == T_INSTANCE)   // instance output -> driven wire
         scan_inst_outputs(s, g_conc_set);
   }
}

// A signal with a CONCURRENT driver must be `wire`: emitting it `reg` (because
// some process ALSO touches a same-named signal, or a mis-scan) hands yosys the
// tolerated-but-treacherous assign-to-reg form. In ifu_mem_ctl that mangled
// `perr_state` (reg + assign from an instance-output slice) into cross-FSM
// aliasing — the netlist's iccm_ready grew a spurious miss_state_idle term
// that the RTL equation does not contain.
static bool is_reg(tree_t block, tree_t decl)
{
   (void)block;   // sets precomputed by build_reg_set()
   if (g_conc_set != NULL && hset_contains(g_conc_set, tree_ident(decl)))
      return false;
   return g_reg_set != NULL && hset_contains(g_reg_set, tree_ident(decl));
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
      // type_width (below) flattens every dimension via range_bounds/assume_int,
      // which FATALs on any non-static bound: an unconstrained array (range_of has
      // no range — e.g. an unconstrained bit_vector function result / deferred
      // constant), or a generic-/variable-bounded dimension or element (e.g. a
      // generic-package subtype `natural range G to 100`). type_const_bounds checks
      // every dimension AND element recursively with folded_bounds (no fatal); if
      // the flattened width isn't a compile-time constant, decline and stay in nvc.
      if (!type_const_bounds(t)) return false;
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
      // Array of a MULTI-BIT SCALAR — ITC'99 b12's
      //   type RAM is array (31 downto 0) of natural range 0 to 3
      // A declaration only becomes an indexed Verilog memory when mem_shape()
      // finds an ARRAY element; with a scalar element it falls through to the
      // flat `reg [W-1:0]` path, so every entry collapses to ONE bit and each
      // `memory(i)` silently reads the wrong value (b12: data_out truncated to
      // 0/1, the accelerated run's whole game state wrong with no marker
      // emitted). No representation exists -> reject the leaf rather than
      // mistranslate it.
      //
      // logic3d is EXEMPT and must stay so. `subtype logic3d is natural range
      // 0 to 7` is an integer subtype, so an unqualified integer-element test
      // rejects logic3d_vector — i.e. every sv2vhdl port in the design, which
      // takes ALL of VeeR out of --accel (measured: eh2_ifu_ifc_ctl declined at
      // its first vector port, EXU_FLUSH_PATH_FINAL, emitting 0 modules). The
      // one-bit-per-element collapse that is a silent bug for a natural RAM is
      // the DEFINED representation here: emit_range() renders scalar logic3d as
      // bit0 and a logic3d array as one bit per element, which is precisely the
      // 2-state value plane --accel is built on (see type_is_logic3d). So the
      // guard must key on "no bit-level representation exists", not on "the
      // element happens to be an integer".
      if (!type_is_logic3d(el) && (type_is_integer(el) || type_is_physical(el)))
         return false;
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
         unsigned mnw, mew;
         if (tree_kind(tree_decl(block, i)) == T_SIGNAL_DECL
             && mem_shape(tree_type(tree_decl(block, i)), &mnw, &mew))
            continue;   // representable as a Verilog memory
         if (getenv("GSM_LOG"))
            fprintf(stderr, "block_types_synth: decl %s (kind %d) rejected\n",
                    istr(tree_ident(tree_decl(block, i))),
                    tree_kind(tree_decl(block, i)));
         return false;
      }
   // This third loop USED TO BE A BARE `return false` with no GSM_LOG line,
   // while its two sibling loops above each logged.  That one asymmetry is why
   // ITC99 b12/b15 and up to 156 regression designs were undiagnosable: the
   // module is refused HERE, before vhdl2vlog_module writes a single byte, so
   // the .v is empty, g_unhandled never moves, and nothing says why.
   for (int i = 0; i < tree_stmts(block); i++) {
      tree_t s = tree_stmt(block, i);
      if (tree_kind(s) != T_PROCESS) continue;
      for (int j = 0; j < tree_decls(s); j++)
         if (!decl_type_synth(tree_decl(s, j))) {
            unsigned mnw, mew;
            if (tree_kind(tree_decl(s, j)) == T_VAR_DECL
                && mem_shape(tree_type(tree_decl(s, j)), &mnw, &mew))
               continue;   // representable as a Verilog memory (hoisted)
            if (getenv("GSM_LOG"))
               fprintf(stderr, "block_types_synth: process %s decl %s (kind %d) "
                       "rejected\n", istr(tree_ident(s)),
                       istr(tree_ident(tree_decl(s, j))),
                       tree_kind(tree_decl(s, j)));
            return false;
         }
   }
   return true;
}

// Record `nm` in the seen-set; return true if it was already present.
static bool local_seen(char seen[][64], int *nseen, const char *nm)
{
   for (int i = 0; i < *nseen; i++)
      if (!strcmp(seen[i], nm)) return true;
   if (*nseen < 8192) { strncpy(seen[*nseen], nm, 63); seen[*nseen][63] = '\0'; (*nseen)++; }
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
         if (shadow_sig_of(d) != NULL) continue;  // NBA-shadow of a memory: elided
         {  unsigned mnw, mew;
            if (!sig_is_mem(d) && mem_shape(tree_type(d), &mnw, &mew)
                && type_is_integer(type_elem(tree_type(d)))) {
               // memory-shaped but unqualified (slice/whole-array use the
               // expansion cannot represent): the flat path would collapse
               // each element to ONE BIT -- the silent-wrong class the old
               // type rejection existed to prevent.  Decline loudly.
               DECLINE("integer-array-unqualified");
               fprintf(f, "  /*?intmem %s*/\n", vid(tree_ident(d)));
               continue;
            }
            if (sig_is_mem(d) && mem_shape(tree_type(d), &mnw, &mew)) {
               // memory-qualified variable: hoist as a true Verilog memory;
               // integer elements keep the translator's signed convention
               const bool isint = type_is_integer(type_elem(tree_type(d)));
               fprintf(f, "  reg %s[%u:0] %s [0:%u];\n",
                       isint ? "signed " : "", mew - 1,
                       vid(tree_ident(d)), mnw - 1);
               local_seen(seen, nseen, vid(tree_ident(d)));
               continue;
            }
         }
         const char *nm = vid(tree_ident(d));
         // A variable whose name collides with a signal/port OR was already
         // hoisted from ANOTHER process gets a unique per-module suffix.
         // Sharing one module-scope reg between two always blocks is a
         // synthesis multi-driver bug: yosys merged ifu_mem_ctl's perr/miss
         // FSMs through the shared Verilog_Case_Ex temp, growing iccm_ready
         // a spurious miss-idle term.
         const bool sig_clash = g_sig_names != NULL
            && hset_contains(g_sig_names, ident_new(nm));
         if (sig_clash || local_seen(seen, nseen, nm)) {
            char sfx[24], mn[120];
            snprintf(sfx, sizeof sfx, "__pv%d", g_ren_ctr++);
            ren_register(d, sfx);
            snprintf(mn, sizeof mn, "%s%s", nm, sfx);
            local_seen(seen, nseen, mn);
            fputs("  reg ", f); emit_range(f, tree_type(d));
            fprintf(f, "%s;\n", mn);
            continue;
         }
         if (g_sig_names != NULL)   // vars join the collision domain too
            hset_insert(g_sig_names, ident_new(nm));
         fputs("  reg ", f); emit_range(f, tree_type(d)); fprintf(f, "%s;\n", nm);
      }
      for (int i = 0; i < tree_stmts(s); i++)
         emit_proc_locals(f, tree_stmt(s, i), seen, nseen);
      break;
   case T_FOR:
      if (tree_decls(s) > 0) {
         // Loop indices are purely local: mangle them UNCONDITIONALLY (__lp)
         // so they can never collide with a module signal/port/var of the
         // same name (VeeR's ifu_compress_ctl has a 16-bit signal `i` next
         // to i-indexed loops — the collision made yosys reject the module,
         // silently until the log_errfile fix). References follow by DECL
         // IDENTITY (ren_decl), so no name ambiguity.
         tree_t idc = tree_decl(s, 0);
         if (!ren_decl(idc)) {
            char sfx[24];
            snprintf(sfx, sizeof sfx, "__lp%d", g_ren_ctr++);
            ren_register(idc, sfx);
         }
         char mn[120];
         snprintf(mn, sizeof mn, "%s%s", vid(tree_ident(idc)), ren_suffix(idc));
         if (!local_seen(seen, nseen, mn))
            fprintf(f, "  integer %s;\n", mn);
      }
      for (int i = 0; i < tree_stmts(s); i++)
         emit_proc_locals(f, tree_stmt(s, i), seen, nseen);
      break;
   case T_LOOP:   // process-sensitivity loop (proc_body unwraps it) OR a bare
                  // loop: descend so for-indices nested inside get hoisted. Without
                  // this, a `for oob_p in ...` inside the sensitivity loop emitted a
                  // procedural for over an UNDECLARED index -> yosys "LHS of for-loop
                  // is not a register", failing ic_mem/ic_data whole-subtree synth.
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
tree_t vhdl2vlog_comp_inner(tree_t block)
{
   if (tree_kind(block) != T_BLOCK || tree_decls(block) == 0)
      return NULL;
   tree_t hier = tree_decl(block, 0);
   if (tree_kind(hier) != T_HIER)
      return NULL;
   tree_t comp = tree_ref(hier);
   if (comp == NULL || tree_kind(comp) != T_COMPONENT)
      return NULL;
   if (tree_stmts(block) != 1)
      return NULL;
   tree_t inner = tree_stmt(block, 0);
   if (tree_kind(inner) != T_BLOCK || tree_decls(inner) == 0)
      return NULL;
   tree_t ih = tree_decl(inner, 0);
   if (tree_kind(ih) != T_HIER)
      return NULL;
   tree_t ref = tree_ref(ih);
   if (ref == NULL || tree_kind(ref) != T_ARCH)
      return NULL;
   // Positional-connection soundness: instances emit POSITIONAL actuals in
   // the outer (component-port) order, and the module emits the entity's
   // ports — the two orders must correspond.  Default binding is by NAME,
   // so require the same names in the same order; anything else declines
   // back to the old behaviour.
   tree_t ent = tree_primary(ref);
   if (tree_ports(comp) != tree_ports(ent))
      return NULL;
   for (int i = 0; i < tree_ports(comp); i++)
      if (tree_ident(tree_port(comp, i)) != tree_ident(tree_port(ent, i)))
         return NULL;
   return inner;
}

bool vhdl2vlog_module(FILE *f, tree_t block, const char *modname)
{
   g_unhandled = 0;

   // A component wrapper block is transparent: translate the bound arch.
   // Without this the wrapper emitted as a module whose only statement was
   // an instance of ITSELF (ports+hoisted regs, no logic) and the arch body
   // was never emitted at all (ITC b17/b22, any component+configuration
   // design).
   {  tree_t inner = vhdl2vlog_comp_inner(block);
      if (inner != NULL)
         block = inner;
   }

   // Decline non-synthesizable modules up front (see block_types_synth): a wrong
   // but parseable model would silently corrupt results.
   if (!block_types_synth(block))
      return false;

   build_reg_set(block);   // one walk; is_reg() is then a set lookup

   // module-local design functions: register their (sanitized) names so calls
   // emit a Verilog function call rather than the unhandled marker.
   if (g_func_set != NULL) hset_free(g_func_set);
   g_func_set = hset_new(64);
   for (int i = 0; i < tree_decls(block); i++) {
      tree_t d = tree_decl(block, i);
      if (tree_kind(d) == T_FUNC_BODY
          && !fn_is_builtin(istr(tree_ident(d))))
         hset_insert(g_func_set, ident_new(vid(tree_ident(d))));
   }

   // module signal/port name set — hoisted locals colliding with these get
   // renamed (__lp) to keep yosys from rejecting the module
   if (g_sig_names != NULL) hset_free(g_sig_names);
   g_sig_names = hset_new(256);
   g_ren_map = NULL;   // dropped per module; translator process is short-lived
   g_ren_ctr = 0;
   for (int i = 0; i < tree_decls(block); i++) {
      tree_t d = tree_decl(block, i);
      if (tree_kind(d) == T_SIGNAL_DECL)
         hset_insert(g_sig_names, ident_new(vid(tree_ident(d))));
   }
   for (int i = 0; i < tree_ports(block); i++)
      hset_insert(g_sig_names, ident_new(vid(tree_ident(tree_port(block, i)))));

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

   // qualify memory-shaped signals: array-of-vector, process-driven, and every
   // reference a plain single-index (each such use contributes one T_REF and
   // one matching T_ARRAY_REF, so refs==indexed; a slice/whole-array/port-map
   // use adds an unmatched T_REF and disqualifies)
   const int ndecls = tree_decls(block);
   g_n_mem_sigs = 0;
   g_n_shadows = 0;
   for (int i = 0; i < ndecls && g_n_mem_sigs < MAX_MEM_SIGS; i++) {
      tree_t d = tree_decl(block, i);
      if (tree_kind(d) != T_SIGNAL_DECL) continue;
      unsigned nw, ew;
      if (!mem_shape(tree_type(d), &nw, &ew)) continue;
      if (!is_reg(block, d)) continue;   // memory must be process-driven (reg)
      mem_scan_t sc = { .decl = d, .refs = 0, .indexed = 0 };
      tree_visit(block, mem_scan_cb, &sc);
      if (sc.refs > 0 && sc.refs == sc.indexed + sc.agg) {
         g_mem_sigs[g_n_mem_sigs++] = d;
         continue;
      }
      // strict form failed: try the NBA-shadow idiom (see g_shadow_var)
      if (sc.refs > 0 && g_n_shadows < MAX_MEM_SHADOWS) {
         shadow_find_t sf = { .sig = d, .var = NULL };
         tree_visit(block, shadow_find_cb, &sf);
         if (sf.var == NULL) continue;
         shadow_scan_t ss = { .sig = d, .var = sf.var };
         tree_visit(block, shadow_scan_cb, &ss);
         if (ss.ncopy == 1 && ss.nwb == 1
             && ss.v_ref == 2 + ss.v_arr && ss.v_arr == ss.nwrite
             && ss.s_ref == 2 + ss.s_arr) {
            g_mem_sigs[g_n_mem_sigs++] = d;
            g_shadow_var[g_n_shadows] = sf.var;
            g_shadow_sigd[g_n_shadows] = d;
            g_n_shadows++;
         }
      }
   }

   // qualify memory-shaped PROCESS VARIABLES the same way (b15 InstQueue,
   // b12's RAM-as-variable): every reference inside the owning process a
   // plain single-index.  They join g_mem_sigs -- the set is keyed by tree
   // identity, so sig_is_mem() and the indexed-access emission just work;
   // emit_proc_locals hoists them as true Verilog memories.
   for (int si = 0; si < tree_stmts(block)
           && g_n_mem_sigs < MAX_MEM_SIGS; si++) {
      tree_t ps = tree_stmt(block, si);
      if (tree_kind(ps) != T_PROCESS) continue;
      for (int j = 0; j < tree_decls(ps)
              && g_n_mem_sigs < MAX_MEM_SIGS; j++) {
         tree_t vd = tree_decl(ps, j);
         if (tree_kind(vd) != T_VAR_DECL) continue;
         unsigned vnw, vew;
         if (!mem_shape(tree_type(vd), &vnw, &vew)) continue;
         mem_scan_t vsc = { .decl = vd, .refs = 0, .indexed = 0 };
         tree_visit(ps, mem_scan_cb, &vsc);
         if (vsc.refs > 0 && vsc.refs == vsc.indexed + vsc.agg)
            g_mem_sigs[g_n_mem_sigs++] = vd;
      }
   }

   // signal declarations (skip ports and hier markers)
   for (int i = 0; i < ndecls; i++) {
      tree_t d = tree_decl(block, i);
      if (tree_kind(d) != T_SIGNAL_DECL) continue;
      unsigned nw, ew;
      if (sig_is_mem(d) && mem_shape(tree_type(d), &nw, &ew)) {
         const bool isint = type_is_integer(type_elem(tree_type(d)));
         fprintf(f, "  reg %s[%u:0] %s [0:%u];\n", isint ? "signed " : "",
                 ew - 1, vid(tree_ident(d)), nw - 1);
         continue;
      }
      if (!sig_is_mem(d) && mem_shape(tree_type(d), &nw, &ew)
          && type_is_integer(type_elem(tree_type(d)))) {
         // see the hoist-path twin: never emit the one-bit collapse
         DECLINE("integer-array-unqualified");
         fprintf(f, "  /*?intmem %s*/\n", vid(tree_ident(d)));
         continue;
      }
      const bool is_r = is_reg(block, d);
      fprintf(f, "  %s ", is_r ? "reg" : "wire");
      emit_range(f, tree_type(d));
      fprintf(f, "%s", vid(tree_ident(d)));
      // A process-driven reg with a default value carries a POWER-ON init.
      // Emit it as a Verilog reg initializer so yosys records the wire `init`
      // attribute -- distinct from any async-reset value. Without it the accel
      // powers on at the reg's reset value (or 0) instead of its declared
      // default (e.g. `signal r := 0` with an async reset to X started at X).
      if (is_r && tree_has_value(d)) {
         fputs(" = ", f);
         emit_expr(f, tree_value(d));
      }
      fputs(";\n", f);

      // A signal with an initial value that is assigned NOWHERE (no process,
      // no concurrent statement) is effectively a constant. The bare `wire`
      // above leaves it undriven (x/0 in synthesis), dropping the value. Drive
      // it with a continuous assign of its initializer. (A reg or concurrent-
      // driven signal gets its value from that driver -- a second assign would
      // be a multi-driver conflict.)
      const bool driven =
         (g_reg_set  != NULL && hset_contains(g_reg_set,  tree_ident(d)))
         || (g_conc_set != NULL && hset_contains(g_conc_set, tree_ident(d)));
      if (!is_r && !driven && tree_has_value(d)) {
         fprintf(f, "  assign %s = ", vid(tree_ident(d)));
         emit_expr(f, tree_value(d));
         fputs(";\n", f);
      }
   }

   // design functions -> Verilog functions (before the processes that call them)
   for (int i = 0; i < tree_decls(block); i++) {
      tree_t d = tree_decl(block, i);
      if (tree_kind(d) == T_FUNC_BODY && !fn_is_builtin(istr(tree_ident(d))))
         emit_function(f, d);
   }

   const int nstmts = tree_stmts(block);

   // hoist process-local variables (-> reg) and for-loop indices (-> integer)
   static char seen[8192][64];   // static: 512KB, too big for the stack
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

// ===========================================================================
// vhdl2rtlil — the direct-RTLIL backend of the SAME tree walk
// (TODO-yosys-integration.md §1; subset — anything it cannot express
// declines the module and the caller falls back to the text path above.
// It shares this file's analysis: build_reg_set/is_reg, clock_of/areset_of,
// mem_shape/sig_is_mem, type_width/emitted_width, vid, comp_inner.)
// ===========================================================================
#include "gsm_rtlil.h"

static const gsm_rtlil_api_t *g_r2 = NULL;   // active builder api
static int  g_r2_tmp;                        // rx<N> expression temps
static int  g_r2_fail;                       // decline counter
static char g_r2_why[128];                   // first decline reason
static const char *g_r2_site = "?";          // breadcrumb for declines

#define R2_DECLINE(why) do {                                   \
      if (g_r2_fail++ == 0)                                    \
         snprintf(g_r2_why, sizeof g_r2_why, "%s@%s", (why),   \
                  g_r2_site);                                  \
   } while (0)

// Expression sigspec strings: names, selects, sized constants, temp wire
// names, or concats of those.  Binary literals spell one char per BIT, so
// this bounds the widest expressible constant/concat; wider declines.
#define R2_SPEC 4096

static int r2_width(tree_t e)
{
   const int ew = emitted_width(e, 0);
   if (ew > 0)
      return ew;
   type_t t = tree_type(e);
   return type_const_bounds(t) ? (int)type_width(t) : -1;
}

// Constant expression -> sized sigspec literal ("8'd42" / "4'b01xz").
static bool r2_const(tree_t e, char *out, size_t sz, int want_w)
{
   // strip conversion wrappers: constants arrive as
   // std_logic_vector(to_unsigned(<folded>, W)) and friends
   for (;;) {
      const tree_kind_t k = tree_kind(e);
      if (k == T_TYPE_CONV || k == T_QUALIFIED || k == T_INERTIAL) {
         e = tree_value(e);
         continue;
      }
      if (k == T_FCALL) {
         const char *fn = istr(tree_ident(e));
         if (tree_params(e) == 2
             && (strstr(fn, "TO_UNSIGNED") || strstr(fn, "to_unsigned")
                 || strstr(fn, "TO_SIGNED") || strstr(fn, "to_signed"))) {
            int64_t v, w;
            if (folded_int(tree_value(tree_param(e, 0)), &v)
                && folded_int(tree_value(tree_param(e, 1)), &w)
                && w > 0 && v >= 0) {
               snprintf(out, sz, "%lld'd%lld", (long long)w, (long long)v);
               return true;
            }
            return false;
         }
         if (tree_params(e) == 1 && vlog_op(fn) == NULL
             && (strstr(fn, "UNSIGNED") || strstr(fn, "SIGNED")
                 || strstr(fn, "STD_LOGIC_VECTOR")
                 || strstr(fn, "TO_STDLOGICVECTOR")
                 || strstr(fn, "unsigned")
                 || strstr(fn, "std_logic_vector"))) {
            e = tree_value(tree_param(e, 0));
            continue;
         }
      }
      break;
   }
   // scalar enum bits FIRST: folded_int on a std_logic ref returns the enum
   // POSITION, whose low bit coincidentally matches for 0/1/L/H but silently
   // aliases X and Z — decode the literal character instead
   if (tree_kind(e) == T_REF) {
      const ident_t id = tree_has_ref(e) ? tree_ident(tree_ref(e))
                                         : tree_ident(e);
      const char c = ident_char(id, 1);
      const char c0 = ident_char(id, 0);
      if (c0 == '\'') {
         if (c == '0' || c == 'L') { snprintf(out, sz, "1'b0"); return true; }
         if (c == '1' || c == 'H') { snprintf(out, sz, "1'b1"); return true; }
         return false;   // metavalue: same decline policy as the text path
      }
   }
   int64_t iv;
   if (folded_int(e, &iv)) {
      int w = want_w > 0 ? want_w : r2_width(e);
      if (w <= 0)
         w = 32;
      if (iv < 0) {   // two's complement within the width
         if (w > 63)
            return false;
         iv &= (((int64_t)1 << w) - 1);
      }
      snprintf(out, sz, "%d'd%lld", w, (long long)iv);
      return true;
   }
   if (tree_kind(e) == T_STRING) {
      const int n = tree_chars(e);
      if (n <= 0 || (size_t)n + 8 > sz)
         return false;
      char *p = out + snprintf(out, sz, "%d'b", n);
      for (int i = 0; i < n; i++) {
         const char c = ident_char(tree_ident(tree_ref(tree_char(e, i))), 1);
         switch (c) {
         case '0': case 'L': *p++ = '0'; break;
         case '1': case 'H': *p++ = '1'; break;
         default: return false;   // metavalues: same policy as the text path
         }
      }
      *p = '\0';
      return true;
   }
   if (tree_kind(e) == T_AGGREGATE && tree_assocs(e) > 1) {
      // all-positional aggregate of constant bits (how elaboration renders
      // hex/bit-string literals in some positions) -> sized binary literal
      const int n = tree_assocs(e);
      if ((size_t)n + 8 > sz)
         return false;
      char *p = out + snprintf(out, sz, "%d'b", n);
      for (int i = 0; i < n; i++) {
         tree_t a = tree_assoc(e, i);
         if (tree_subkind(a) != A_POS)
            return false;
         tree_t v = tree_value(a);
         // ident decode FIRST: folded_int on a std_logic ref returns the
         // ENUM POSITION ('0'=2, '1'=3), not the bit value
         char c = 0;
         if (tree_kind(v) == T_REF)
            c = ident_char(tree_has_ref(v) ? tree_ident(tree_ref(v))
                                           : tree_ident(v), 1);
         int64_t bit;
         if (c == '0' || c == 'L') *p++ = '0';
         else if (c == '1' || c == 'H') *p++ = '1';
         else if (c == 0 && folded_int(v, &bit) && (bit == 0 || bit == 1))
            *p++ = bit ? '1' : '0';
         else
            return false;
      }
      *p = '\0';
      return true;
   }
   if (tree_kind(e) == T_AGGREGATE && tree_assocs(e) == 1) {
      tree_t a = tree_assoc(e, 0);
      if (tree_subkind(a) == A_OTHERS) {
         int64_t bit;
         const int w = want_w > 0 ? want_w : r2_width(e);
         if (w <= 0)
            return false;
         tree_t v = tree_value(a);
         char cb, c = 0;
         if (tree_kind(v) == T_REF)
            c = ident_char(tree_has_ref(v) ? tree_ident(tree_ref(v))
                                           : tree_ident(v), 1);
         if (c == '0' || c == 'L') cb = '0';
         else if (c == '1' || c == 'H') cb = '1';
         else if (c == 0 && folded_int(v, &bit) && (bit == 0 || bit == 1))
            cb = bit ? '1' : '0';
         else {
            if (getenv("NVC_RTLIL_DEBUG") != NULL && tree_kind(v) == T_REF)
               fprintf(stderr, "r2 others-ref ident='%s' hasref=%d refid='%s'\n",
                       istr(tree_ident(v)), tree_has_ref(v),
                       tree_has_ref(v) ? istr(tree_ident(tree_ref(v))) : "-");
            return false;
         }
         if ((size_t)w + 8 > sz)
            return false;
         char *p = out + snprintf(out, sz, "%d'b", w);
         for (int i = 0; i < w; i++)
            *p++ = cb;
         *p = '\0';
         return true;
      }
   }
   return false;
}

static bool r2_expr(tree_t e, char *out, size_t sz);

// Make a fresh temp wire of `width` bits; returns its name in out.
static bool r2_temp(int width, char *out, size_t sz)
{
   if (width <= 0) {
      R2_DECLINE("temp-width");
      return false;
   }
   snprintf(out, sz, "rx%d", g_r2_tmp++);
   return g_r2->wire(out, width, 0, NULL) == 0;
}

// Map the text path's Verilog operator string to a builder cell op.
static const char *r2_binop(const char *vop)
{
   static const struct { const char *v, *b; } map[] = {
      {"+","add"}, {"-","sub"}, {"*","mul"}, {"/","div"}, {"%","mod"},
      {"&","and"}, {"|","or"}, {"^","xor"}, {"~^","xnor"},
      {"==","eq"}, {"!=","ne"}, {"<","lt"}, {">","gt"},
      {"<=","le"}, {">=","ge"}, {"<<","shl"}, {">>","shr"},
      {NULL, NULL}
   };
   for (int i = 0; map[i].v != NULL; i++)
      if (strcmp(map[i].v, vop) == 0)
         return map[i].b;
   return NULL;
}

static bool r2_is_onebit_op(const char *b)
{
   return strcmp(b, "eq") == 0 || strcmp(b, "ne") == 0
      || strcmp(b, "lt") == 0 || strcmp(b, "gt") == 0
      || strcmp(b, "le") == 0 || strcmp(b, "ge") == 0;
}

static bool r2_expr(tree_t e, char *out, size_t sz)
{
   if (g_r2_fail > 0)
      return false;

   switch (tree_kind(e)) {
   case T_REF:
      {
         tree_t d = tree_has_ref(e) ? tree_ref(e) : NULL;
         if (d != NULL && tree_kind(d) == T_CONST_DECL) {
            if (tree_has_value(d) && r2_const(tree_value(d), out, sz, -1))
               return true;
            if (tree_has_value(d))
               return r2_expr(tree_value(d), out, sz);
            R2_DECLINE("const-ref");
            return false;
         }
         if (r2_const(e, out, sz, -1))   // enum literals ('0','1',true,...)
            return true;
         if (d != NULL && (tree_kind(d) == T_SIGNAL_DECL
                           || tree_kind(d) == T_PORT_DECL)) {
            snprintf(out, sz, "%s", vid(tree_ident(e)));
            return true;
         }
         R2_DECLINE("ref");
         return false;
      }

   case T_LITERAL:
   case T_STRING:
   case T_AGGREGATE:
      if (r2_const(e, out, sz, -1))
         return true;
      if (tree_kind(e) == T_AGGREGATE && tree_assocs(e) > 0) {
         // elaboration folds `&` chains into concat-aggregates: build the
         // verilog-order concat {e0, e1, ...} (first assoc = MSB end)
         bool concat = true;
         const int n = tree_assocs(e);
         for (int i = 0; i < n; i++) {
            const assoc_kind_t sk = tree_subkind(tree_assoc(e, i));
            if (sk != A_CONCAT && sk != A_POS) {
               concat = false;
               break;
            }
         }
         if (concat) {
            size_t len = 0;
            out[len++] = '{';
            for (int i = 0; i < n; i++) {
               char el[R2_SPEC];
               if (!r2_expr(tree_value(tree_assoc(e, i)), el, sizeof el))
                  return false;
               const size_t need = strlen(el) + 2;
               if (len + need + 2 >= sz) {
                  R2_DECLINE("concat-size");
                  return false;
               }
               if (i > 0)
                  out[len++] = ',';
               memcpy(out + len, el, strlen(el));
               len += strlen(el);
            }
            out[len++] = '}';
            out[len] = '\0';
            return true;
         }
      }
      {
         char why[80];
         if (tree_kind(e) == T_STRING && tree_chars(e) > 0)
            snprintf(why, sizeof why, "literal(str n=%d c0=%c)",
                     tree_chars(e),
                     ident_char(tree_ident(tree_ref(tree_char(e, 0))), 1));
         else if (tree_kind(e) == T_AGGREGATE && tree_assocs(e) > 0)
            snprintf(why, sizeof why, "literal(agg n=%d sk0=%d vk0=%d)",
                     tree_assocs(e),
                     (int)tree_subkind(tree_assoc(e, 0)),
                     (int)tree_kind(tree_value(tree_assoc(e, 0))));
         else
            snprintf(why, sizeof why, "literal(k=%d w=%d)",
                     (int)tree_kind(e), r2_width(e));
         R2_DECLINE(why);
      }
      return false;

   case T_TYPE_CONV:
   case T_QUALIFIED:
   case T_INERTIAL:
      return r2_expr(tree_value(e), out, sz);

   case T_ARRAY_SLICE:
      {
         tree_t base = tree_value(e);
         if (tree_kind(base) != T_REF) {
            R2_DECLINE("slice-base");
            return false;
         }
         tree_t r = tree_range(e, 0);
         int64_t left, right;
         if (!folded_int(tree_left(r), &left)
             || !folded_int(tree_right(r), &right)) {
            R2_DECLINE("slice-bounds");
            return false;
         }
         const int64_t hi = left > right ? left : right;
         const int64_t lo = left > right ? right : left;
         snprintf(out, sz, "%s[%lld:%lld]", vid(tree_ident(base)),
                  (long long)hi, (long long)lo);
         return true;
      }

   case T_ARRAY_REF:
      {
         tree_t base = tree_value(e);
         int64_t idx;
         if (tree_kind(base) != T_REF || tree_params(e) != 1
             || !folded_int(tree_value(tree_param(e, 0)), &idx)) {
            R2_DECLINE("array-ref");
            return false;
         }
         snprintf(out, sz, "%s[%lld]", vid(tree_ident(base)),
                  (long long)idx);
         return true;
      }

   case T_FCALL:
      {
         const char *fn = istr(tree_ident(e));
         const int np = tree_params(e);

         // transparent numeric_std/library identities (same set the text
         // path prints verbatim)
         if (np >= 1 && (strstr(fn, "UNSIGNED") || strstr(fn, "SIGNED")
                         || strstr(fn, "STD_LOGIC_VECTOR")
                         || strstr(fn, "TO_STDLOGICVECTOR")
                         || strstr(fn, "unsigned")
                         || strstr(fn, "std_logic_vector"))
             && vlog_op(fn) == NULL && np == 1)
            return r2_expr(tree_value(tree_param(e, 0)), out, sz);

         // to_unsigned(<const>, <width>) -> sized literal
         if (np == 2 && (strstr(fn, "TO_UNSIGNED") || strstr(fn, "to_unsigned"))) {
            int64_t v, w;
            if (folded_int(tree_value(tree_param(e, 0)), &v)
                && folded_int(tree_value(tree_param(e, 1)), &w)
                && w > 0 && v >= 0) {
               snprintf(out, sz, "%lld'd%lld", (long long)w, (long long)v);
               return true;
            }
            R2_DECLINE("to_unsigned");
            return false;
         }

         const char *vop = vlog_op(fn);
         if (vop != NULL && np == 2) {
            if (strcmp(vop, "&") == 0 && !type_is_array(tree_type(e))) {
               // scalar AND — fall through to cell path below
            }
            // VHDL "&" on arrays is CONCATENATION
            if (strcmp(vop, "&") == 0 && type_is_array(tree_type(e))
                && !type_is_array(tree_type(tree_value(tree_param(e, 0))))) {
               // 1-bit & vector or similar odd shapes: decline for now
            }
         }
         if (fn[0] == '"' && strcmp(fn, "\"&\"") == 0) {
            // concatenation: {a, b}
            char a[R2_SPEC], b[R2_SPEC];
            if (!r2_expr(tree_value(tree_param(e, 0)), a, sizeof a)
                || !r2_expr(tree_value(tree_param(e, 1)), b, sizeof b))
               return false;
            snprintf(out, sz, "{%s,%s}", a, b);
            return true;
         }
         if (vop != NULL && np == 2) {
            const char *bop = r2_binop(vop);
            if (bop == NULL) {
               R2_DECLINE("binop");
               return false;
            }
            tree_t ea = tree_value(tree_param(e, 0));
            tree_t eb = tree_value(tree_param(e, 1));
            char a[R2_SPEC], b[R2_SPEC];
            if (!r2_expr(ea, a, sizeof a) || !r2_expr(eb, b, sizeof b))
               return false;
            int w = r2_is_onebit_op(bop) ? 1 : r2_width(e);
            if (w <= 0)
               w = r2_width(ea);   // operator return types are unconstrained;
            if (w <= 0)            // same-width operands carry the real width
               w = r2_width(eb);
            char y[R2_SPEC], cn[R2_SPEC + 8];
            if (!r2_temp(w, y, sizeof y))
               return false;
            const int sg = type_is_signed(tree_type(ea))
               || type_is_signed(tree_type(eb));
            snprintf(cn, sizeof cn, "c%s", y);
            if (g_r2->cell_bin(bop, cn, a, b, y, sg) != 0) {
               R2_DECLINE("cell_bin");
               return false;
            }
            snprintf(out, sz, "%s", y);
            return true;
         }
         if (vop != NULL && np == 1) {
            const char *uop = strcmp(vop, "~") == 0 ? "not"
               : strcmp(vop, "-") == 0 ? "neg" : NULL;
            if (uop == NULL) {
               R2_DECLINE("unop");
               return false;
            }
            char a[R2_SPEC];
            tree_t ea = tree_value(tree_param(e, 0));
            if (!r2_expr(ea, a, sizeof a))
               return false;
            int w = r2_width(e);
            if (w <= 0)
               w = r2_width(ea);
            char y[R2_SPEC], cn[R2_SPEC + 8];
            if (!r2_temp(w, y, sizeof y))
               return false;
            snprintf(cn, sizeof cn, "c%s", y);
            if (g_r2->cell_un(uop, cn, a, y, 0) != 0) {
               R2_DECLINE("cell_un");
               return false;
            }
            snprintf(out, sz, "%s", y);
            return true;
         }
         R2_DECLINE("fcall");
         return false;
      }

   default:
      R2_DECLINE("expr-kind");
      return false;
   }
}

// A 1-bit condition sigspec for `if`/ternary tests.
static bool r2_cond(tree_t e, char *out, size_t sz)
{
   return r2_expr(e, out, sz);
}

// ---- process bodies as decision trees --------------------------------------

typedef struct {
   ident_t name;        // target signal ident
   char    g0[80];      // hold-temp wire
   char    spec[80];    // target sigspec (vid)
   int     width;
} r2_target_t;

typedef struct {
   r2_target_t t[64];
   int         n;
} r2_targets_t;

static r2_target_t *r2_target(r2_targets_t *ts, ident_t id)
{
   for (int i = 0; i < ts->n; i++)
      if (ts->t[i].name == id)
         return &ts->t[i];
   return NULL;
}

static void r2_collect_cb(tree_t t, void *ctx)
{
   r2_targets_t *ts = (r2_targets_t *)ctx;
   if (tree_kind(t) != T_SIGNAL_ASSIGN)
      return;
   tree_t tg = tree_target(t);
   if (tree_kind(tg) != T_REF || !tree_has_ref(tg))
      return;
   if (ts->n >= 64)
      return;
   ident_t id = tree_ident(tg);
   if (r2_target(ts, id) != NULL)
      return;
   r2_target_t *n = &ts->t[ts->n++];
   n->name = id;
   snprintf(n->spec, sizeof n->spec, "%s", vid(id));
   snprintf(n->g0, sizeof n->g0, "g0_%s", vid(id));
   type_t ty = tree_type(tree_ref(tg));
   n->width = type_const_bounds(ty) ? (int)type_width(ty) : -1;
}

static bool r2_seq(tree_t list_of, r2_targets_t *ts);

static bool r2_seq_one(tree_t s, r2_targets_t *ts)
{
   switch (tree_kind(s)) {
   case T_WAIT:
      return true;
   case T_SIGNAL_ASSIGN:
      {
         tree_t tg = tree_target(s);
         if (tree_kind(tg) != T_REF || !tree_has_ref(tg)) {
            R2_DECLINE("assign-target");
            return false;
         }
         r2_target_t *t = r2_target(ts, tree_ident(tg));
         if (t == NULL) {
            R2_DECLINE("target-miss");
            return false;
         }
         if (tree_waveforms(s) < 1 || !tree_has_value(tree_waveform(s, 0))) {
            R2_DECLINE("null-wave");
            return false;
         }
         // constants first WITH the target's width for context (an
         // others-aggregate has no self-width)
         char v[R2_SPEC];
         tree_t val = tree_value(tree_waveform(s, 0));
         g_r2_site = "seq-assign";
         if (!r2_const(val, v, sizeof v, t->width)
             && !r2_expr(val, v, sizeof v))
            return false;
         return g_r2->case_assign(t->g0, v) == 0;
      }
   case T_IF:
      {
         // conds chain: nvc T_IF has conditions with values; mirror the text
         // path's if/else-if chain as nested switches
         const int nc = tree_conds(s);
         int depth = 0;
         bool ok = true;
         for (int i = 0; i < nc && ok; i++) {
            tree_t c = tree_cond(s, i);
            if (tree_has_value(c)) {
               char cs[R2_SPEC];
               g_r2_site = "if-cond";
               if (!r2_cond(tree_value(c), cs, sizeof cs)) { ok = false; break; }
               if (g_r2->switch_begin(cs) != 0
                   || g_r2->case_begin("1'b1") != 0) { ok = false; break; }
               ok = r2_seq(c, ts);
               if (!ok) break;
               if (g_r2->case_end() != 0
                   || g_r2->case_begin(NULL) != 0) { ok = false; break; }
               depth++;
            }
            else {
               // else arm: statements into the current default case
               ok = r2_seq(c, ts);
            }
         }
         for (int i = 0; i < depth; i++) {
            if (g_r2->case_end() != 0 || g_r2->switch_end() != 0)
               ok = false;
         }
         return ok && g_r2_fail == 0;
      }
   case T_CASE:
      {
         char sw[R2_SPEC];
         tree_t cv = tree_value(s);
         g_r2_site = "case-value";
         if (!r2_expr(cv, sw, sizeof sw))
            return false;
         const int cw = r2_width(cv);
         if (g_r2->switch_begin(sw) != 0)
            return false;
         const int nalt = tree_stmts(s);
         bool ok = true;
         for (int i = 0; i < nalt && ok; i++) {
            tree_t alt = tree_stmt(s, i);
            const int nch = tree_choices(alt);
            char cmp[R2_SPEC];
            size_t cl = 0;
            bool others = false;
            cmp[0] = '\0';
            for (int j = 0; j < nch && ok; j++) {
               tree_t c = tree_choice(alt, j);
               if (tree_ranges(c) > 0) {
                  R2_DECLINE("range-choice");
                  ok = false;
                  break;
               }
               if (!tree_has_name(c)) {   // `others`
                  others = true;
                  continue;
               }
               char one[R2_SPEC];
               g_r2_site = "case-choice";
               if (!r2_const(tree_name(c), one, sizeof one, cw)
                   && !r2_expr(tree_name(c), one, sizeof one)) {
                  ok = false;
                  break;
               }
               cl += snprintf(cmp + cl, sizeof cmp - cl, "%s%s",
                              cl > 0 ? ";" : "", one);
               if (cl >= sizeof cmp - 1) {
                  R2_DECLINE("choice-size");
                  ok = false;
                  break;
               }
            }
            if (!ok)
               break;
            // an alternative that mixes values with `others` is just the
            // default from the builder's perspective (matches text path's
            // "<v>, default:")
            const char *cc = (others || cl == 0) ? NULL : cmp;
            if (g_r2->case_begin(cc) != 0) { ok = false; break; }
            ok = r2_seq(alt, ts);
            if (g_r2->case_end() != 0)
               ok = false;
         }
         if (g_r2->switch_end() != 0)
            ok = false;
         return ok && g_r2_fail == 0;
      }
   default:
      R2_DECLINE("stmt-kind");
      return false;
   }
}

static bool r2_seq(tree_t list_of, r2_targets_t *ts)
{
   const int n = tree_stmts(list_of);
   for (int i = 0; i < n; i++)
      if (!r2_seq_one(tree_stmt(list_of, i), ts))
         return false;
   return true;
}

static bool r2_process(tree_t p0, int pidx)
{
   tree_t p = proc_body(p0);
   tree_t body_if = NULL, sig[8], ifstmt = NULL;
   bool pe[8];
   int ne = 0;
   tree_t clk = clock_of(p, &body_if, sig, pe, &ne, &ifstmt);
   (void)clk;

   if (clk == NULL) {
      // The lone-signal-assign process is a CONTINUOUS assign (the same
      // conversion the text path makes, keeping the target a wire).
      tree_t only = NULL;
      int cnt = 0;
      const int nst = tree_stmts(p);
      for (int i = 0; i < nst; i++) {
         tree_t s = tree_stmt(p, i);
         if (tree_kind(s) == T_WAIT)
            continue;
         only = s;
         cnt++;
      }
      if (cnt == 1 && tree_kind(only) == T_SIGNAL_ASSIGN) {
         tree_t tg = tree_target(only);
         if (tree_kind(tg) != T_REF || !tree_has_ref(tg)
             || tree_waveforms(only) < 1
             || !tree_has_value(tree_waveform(only, 0))) {
            R2_DECLINE("cont-assign");
            return false;
         }
         type_t tty = tree_type(tree_ref(tg));
         const int tw = type_const_bounds(tty) ? (int)type_width(tty) : -1;
         char v[R2_SPEC];
         tree_t val = tree_value(tree_waveform(only, 0));
         if (!r2_const(val, v, sizeof v, tw)
             && !r2_expr(val, v, sizeof v))
            return false;
         return g_r2->connect(vid(tree_ident(tg)), v) == 0;
      }
      // general comb process: decision tree + `always` sync.  The root
      // action g0 = target mirrors read_verilog's $0 self-init: complete
      // assignment optimizes the self arm away; incomplete assignment
      // infers the same latch the text path would (and gsm declines it).
      r2_targets_t cts = { .n = 0 };
      tree_visit(p, r2_collect_cb, &cts);
      if (cts.n == 0) {
         R2_DECLINE("comb-empty");
         return false;
      }
      char cpn[64];
      snprintf(cpn, sizeof cpn, "p%d", pidx);
      if (g_r2->proc(cpn) != 0)
         return false;
      for (int i = 0; i < cts.n; i++) {
         if (cts.t[i].width <= 0) {
            R2_DECLINE("target-width");
            return false;
         }
         if (g_r2->wire(cts.t[i].g0, cts.t[i].width, 0, NULL) != 0
             || g_r2->case_assign(cts.t[i].g0, cts.t[i].spec) != 0)
            return false;
      }
      bool cok = r2_seq(p, &cts);
      if (cok)
         cok = g_r2->sync("always", NULL) == 0;
      for (int i = 0; cok && i < cts.n; i++)
         cok = g_r2->sync_assign(cts.t[i].spec, cts.t[i].g0) == 0;
      return cok && g_r2_fail == 0;
   }
   if (ne != 1) {
      R2_DECLINE("multi-edge");
      return false;
   }
   tree_t rsig = NULL; bool rpe = false, rbefore = false;
   tree_t rcond = (ifstmt != NULL)
      ? areset_of(ifstmt, body_if, &rsig, &rpe, &rbefore) : NULL;
   if (rcond != NULL && !rbefore) {
      R2_DECLINE("reset-clk-priority");
      return false;
   }
   // the tgt-vhdl NBA idiom (statements around the edge-if) is not in the
   // subset yet — require the edge-if to be the only non-wait statement
   {
      const int np = tree_stmts(p);
      for (int i = 0; i < np; i++) {
         tree_t s = tree_stmt(p, i);
         if (s != ifstmt && tree_kind(s) != T_WAIT) {
            R2_DECLINE("proc-extra-stmt");
            return false;
         }
      }
   }

   r2_targets_t ts = { .n = 0 };
   tree_visit(p, r2_collect_cb, &ts);
   if (ts.n == 0 || g_r2_fail > 0)
      return g_r2_fail == 0;

   char pn[64];
   snprintf(pn, sizeof pn, "p%d", pidx);
   if (g_r2->proc(pn) != 0)
      return false;

   for (int i = 0; i < ts.n; i++) {
      if (ts.t[i].width <= 0) {
         R2_DECLINE("target-width");
         return false;
      }
      if (g_r2->wire(ts.t[i].g0, ts.t[i].width, 0, NULL) != 0
          || g_r2->case_assign(ts.t[i].g0, ts.t[i].spec) != 0)
         return false;
   }

   bool ok;
   if (rcond != NULL) {
      // async-reset form: level sync carries the reset values; the body
      // branch builds the tree.  Reset branch must be const assigns.
      ok = r2_seq(body_if, &ts);
      if (ok) {
         char es[R2_SPEC];
         ok = r2_expr(sig[0], es, sizeof es)
            && g_r2->sync(pe[0] ? "posedge" : "negedge", es) == 0;
         for (int i = 0; ok && i < ts.n; i++)
            ok = g_r2->sync_assign(ts.t[i].spec, ts.t[i].g0) == 0;
      }
      if (ok) {
         char rs[R2_SPEC];
         ok = r2_expr(rsig, rs, sizeof rs)
            && g_r2->sync(rpe ? "level1" : "level0", rs) == 0;
      }
      if (ok) {
         const int nr = tree_stmts(rcond);
         for (int i = 0; ok && i < nr; i++) {
            tree_t s = tree_stmt(rcond, i);
            if (tree_kind(s) != T_SIGNAL_ASSIGN) { ok = false; break; }
            tree_t tg = tree_target(s);
            r2_target_t *t = (tree_kind(tg) == T_REF && tree_has_ref(tg))
               ? r2_target(&ts, tree_ident(tg)) : NULL;
            char v[R2_SPEC];
            if (t == NULL || tree_waveforms(s) < 1
                || !tree_has_value(tree_waveform(s, 0))
                || !r2_const(tree_value(tree_waveform(s, 0)), v, sizeof v,
                             t->width)) {
               R2_DECLINE("arst-value");
               ok = false;
               break;
            }
            ok = g_r2->sync_assign(t->spec, v) == 0;
         }
      }
   }
   else {
      ok = r2_seq(body_if, &ts);
      if (ok) {
         char es[R2_SPEC];
         ok = r2_expr(sig[0], es, sizeof es)
            && g_r2->sync(pe[0] ? "posedge" : "negedge", es) == 0;
         for (int i = 0; ok && i < ts.n; i++)
            ok = g_r2->sync_assign(ts.t[i].spec, ts.t[i].g0) == 0;
      }
   }
   return ok && g_r2_fail == 0;
}

// ---- the module walk -------------------------------------------------------

bool vhdl2rtlil_module(const void *api_, tree_t block, const char *modname)
{
   const gsm_rtlil_api_t *api = (const gsm_rtlil_api_t *)api_;
   g_r2 = api;
   g_r2_tmp = 0;
   g_r2_fail = 0;
   g_r2_why[0] = '\0';

   {  tree_t inner = vhdl2vlog_comp_inner(block);
      if (inner != NULL)
         block = inner;
   }
   if (!block_types_synth(block))
      return false;

   build_reg_set(block);

   // subset guards: no design functions, no hoisted process variables,
   // no memory-shaped signals
   const int ndecls = tree_decls(block);
   for (int i = 0; i < ndecls; i++) {
      tree_t d = tree_decl(block, i);
      if (tree_kind(d) == T_FUNC_BODY && !fn_is_builtin(istr(tree_ident(d)))) {
         R2_DECLINE("function");
         return false;
      }
      if (tree_kind(d) == T_SIGNAL_DECL) {
         unsigned nw, ew;
         if (mem_shape(tree_type(d), &nw, &ew)) {
            R2_DECLINE("memory");
            return false;
         }
      }
   }

   if (api->module(modname) != 0)
      return false;

   // ports
   const int nports = tree_ports(block);
   for (int i = 0; i < nports; i++) {
      tree_t pt = tree_port(block, i);
      const port_mode_t mode = tree_subkind(pt);
      const int dir = (mode == PORT_OUT || mode == PORT_INOUT
                       || mode == PORT_BUFFER) ? 2 : 1;
      type_t ty = tree_type(pt);
      if (!type_const_bounds(ty)) {
         R2_DECLINE("port-width");
         return false;
      }
      if (api->wire(vid(tree_ident(pt)), (int)type_width(ty), dir, NULL) != 0)
         return false;
   }

   // signals (with reg power-on init); undriven-initialized wires become
   // connects of their initializer
   for (int i = 0; i < ndecls; i++) {
      tree_t d = tree_decl(block, i);
      if (tree_kind(d) != T_SIGNAL_DECL)
         continue;
      type_t ty = tree_type(d);
      if (!type_const_bounds(ty)) {
         R2_DECLINE("sig-width");
         return false;
      }
      const int w = (int)type_width(ty);
      const bool is_r = is_reg(block, d);
      char init[R2_SPEC];
      const char *initbits = NULL;
      char bits[R2_SPEC];
      if (is_r && tree_has_value(d)) {
         if (!r2_const(tree_value(d), init, sizeof init, w)) {
            R2_DECLINE("reg-init");
            return false;
         }
         const char *tick = strchr(init, '\'');
         if (tick != NULL && tick[1] == 'b')
            initbits = tick + 2;
         else if (tick != NULL && tick[1] == 'd') {
            // decimal init: render as binary MSB-first
            long long v = atoll(tick + 2);
            if (w > 63) { R2_DECLINE("init-wide"); return false; }
            for (int b = 0; b < w; b++)
               bits[b] = ((v >> (w - 1 - b)) & 1) ? '1' : '0';
            bits[w] = '\0';
            initbits = bits;
         }
      }
      if (api->wire(vid(tree_ident(d)), w, 0, initbits) != 0)
         return false;

      const bool driven =
         (g_reg_set  != NULL && hset_contains(g_reg_set,  tree_ident(d)))
         || (g_conc_set != NULL && hset_contains(g_conc_set, tree_ident(d)));
      if (!is_r && !driven && tree_has_value(d)) {
         char v[R2_SPEC];
         if (!r2_const(tree_value(d), v, sizeof v, w)) {
            R2_DECLINE("wire-init");
            return false;
         }
         if (api->connect(vid(tree_ident(d)), v) != 0)
            return false;
      }
   }

   // concurrent statements
   const int nstmts = tree_stmts(block);
   int pidx = 0;
   for (int i = 0; i < nstmts && g_r2_fail == 0; i++) {
      tree_t s = tree_stmt(block, i);
      switch (tree_kind(s)) {
      case T_SIGNAL_ASSIGN:
         {
            tree_t tg = tree_target(s);
            if (tree_kind(tg) != T_REF || !tree_has_ref(tg)
                || tree_waveforms(s) < 1
                || !tree_has_value(tree_waveform(s, 0))) {
               R2_DECLINE("conc-assign");
               break;
            }
            type_t tty = tree_type(tree_ref(tg));
            const int tw = type_const_bounds(tty) ? (int)type_width(tty) : -1;
            char v[R2_SPEC];
            tree_t val = tree_value(tree_waveform(s, 0));
            if (!r2_const(val, v, sizeof v, tw)
                && !r2_expr(val, v, sizeof v))
               break;
            if (api->connect(vid(tree_ident(tg)), v) != 0)
               R2_DECLINE("connect");
            break;
         }
      case T_PROCESS:
         if (!r2_process(s, pidx++))
            R2_DECLINE("process");
         break;
      case T_BLOCK:
         {
            // elaborated child instance — named actuals only (v1)
            tree_t hier = tree_decls(s) > 0 ? tree_decl(s, 0) : NULL;
            tree_t ref  = (hier != NULL && tree_kind(hier) == T_HIER)
                          ? tree_ref(hier) : NULL;
            if (ref == NULL || tree_kind(ref) != T_ARCH) {
               R2_DECLINE("block-ref");
               break;
            }
            tree_t ent = tree_primary(ref);
            char conns[4096];
            size_t cl = 0;
            conns[0] = '\0';
            const int nparams = tree_params(s);
            bool ok = true;
            for (int j = 0; j < nparams && ok; j++) {
               tree_t pp = tree_param(s, j);
               const char *formal = NULL;
               if (tree_subkind(pp) == P_NAMED) {
                  tree_t nm = tree_name(pp);
                  if (tree_kind(nm) != T_REF) {
                     R2_DECLINE("portmap-name");
                     ok = false;
                     break;
                  }
                  formal = vid(tree_ident(nm));
               }
               else {
                  // positional (elaboration normalizes to this): the formal
                  // is the child entity's port at this position
                  if (j >= tree_ports(ent)) {
                     R2_DECLINE("portmap-pos");
                     ok = false;
                     break;
                  }
                  formal = vid(tree_ident(tree_port(ent, j)));
               }
               tree_t v = tree_value(pp);
               if (v == NULL || tree_kind(v) == T_OPEN)
                  continue;
               char av[R2_SPEC];
               if (!r2_expr(v, av, sizeof av)) {
                  ok = false;
                  break;
               }
               cl += snprintf(conns + cl, sizeof conns - cl, "%s%s=%s",
                              cl > 0 ? "," : "", formal, av);
               if (cl >= sizeof conns - 1) {
                  R2_DECLINE("portmap-size");
                  ok = false;
                  break;
               }
            }
            if (ok && api->cell_inst(
                   vhdl2vlog_variant_name(tree_ident(ent), s),
                   vid(tree_ident(s)), conns) != 0)
               R2_DECLINE("cell_inst");
            break;
         }
      default:
         R2_DECLINE("conc-kind");
         break;
      }
   }

   if (g_r2_fail > 0) {
      warnf("vhdl2rtlil: '%s' declined (%s) — using the text path",
            modname, g_r2_why);
      return false;
   }
   return true;
}
