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

// defined below (shared with the direct-RTLIL walker's soundness guard)
static ident_t r2_multi_driver_array(tree_t block);

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

   // Same soundness veto as the direct-RTLIL walker: an array signal written
   // in disjoint elements by >1 driver renders as a `wire` with mixed
   // continuous + procedural drivers (invalid Verilog; yosys mis-resolves it),
   // so decline here too — the subtree then falls through to the interpreter
   // rather than to a wrong text-path model (mylex r22_ffirst).
   {
      ident_t bad = r2_multi_driver_array(block);
      if (bad != NULL) {
         char why[64];
         snprintf(why, sizeof why, "multi-driver-array %s", vid(bad));
         DECLINE(why);
         return false;
      }
   }

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
#include "diag.h"        // loc_t (census: source line of each decline)
#include <setjmp.h>      // census: a crashing process is tagged, not fatal
#include <signal.h>

static const gsm_rtlil_api_t *g_r2 = NULL;   // active builder api
static int  g_r2_tmp;                        // rx<N> expression temps
static int  g_r2_fail;                       // decline counter
static char g_r2_why[128];                   // first decline reason
static const char *g_r2_site = "?";          // breadcrumb for declines

// ---- census mode (NVC_ACCEL_RTLIL_CENSUS=1) --------------------------------
// A DRY walk against a null builder that does not stop at the first decline:
// every decline is streamed (module, process, source line, reason@site) and
// tallied per module, and the module is always declined afterwards (the
// caller takes the text path exactly as for a normal decline).  Diagnostic
// only -- it admits nothing; it exists to catalogue what the walker would
// need for a design that today declines on its first construct.
static bool        g_r2_census;
static const char *g_r2_modname = "?";
static int         g_r2_pidx = -1;
static tree_t      g_r2_cur;                 // statement being walked
static struct { char key[192]; int n; } g_r2_tally[512];
static int         g_r2_ntally;

static void r2_decline(const char *why)
{
   if (g_r2_fail++ == 0)
      snprintf(g_r2_why, sizeof g_r2_why, "%s@%s", why, g_r2_site);
   if (!g_r2_census)
      return;
   const loc_t *loc = g_r2_cur != NULL ? tree_loc(g_r2_cur) : NULL;
   notef("vhdl2rtlil-census: %s p%d L%u: %s@%s", g_r2_modname, g_r2_pidx,
         loc != NULL ? (unsigned)loc->first_line : 0u, why, g_r2_site);
   char key[192];
   snprintf(key, sizeof key, "%s@%s", why, g_r2_site);
   for (int i = 0; i < g_r2_ntally; i++)
      if (strcmp(g_r2_tally[i].key, key) == 0) {
         g_r2_tally[i].n++;
         return;
      }
   if (g_r2_ntally < (int)ARRAY_LEN(g_r2_tally)) {
      snprintf(g_r2_tally[g_r2_ntally].key, sizeof g_r2_tally[0].key,
               "%s", key);
      g_r2_tally[g_r2_ntally++].n = 1;
   }
}

#define R2_DECLINE(why) r2_decline(why)

// census: a walker crash inside one process (the fork child would die and
// the whole census with it) is caught, tagged CRASH(sig) on the statement
// being walked, and the walk goes on with the next process
static sigjmp_buf g_r2_jmp;
static volatile sig_atomic_t g_r2_jmp_armed;
static char g_r2_altstack[1 << 16];

static void r2_census_sig(int sig)
{
   if (g_r2_jmp_armed) {
      g_r2_jmp_armed = 0;
      siglongjmp(g_r2_jmp, sig);
   }
   signal(sig, SIG_DFL);
   raise(sig);
}

static void r2_census_arm_signals(void)
{
   stack_t ss = { .ss_sp = g_r2_altstack, .ss_size = sizeof g_r2_altstack };
   sigaltstack(&ss, NULL);
   struct sigaction sa;
   memset(&sa, 0, sizeof sa);
   sa.sa_handler = r2_census_sig;
   sa.sa_flags = SA_ONSTACK | SA_NODEFER;
   sigemptyset(&sa.sa_mask);
   sigaction(SIGSEGV, &sa, NULL);
   sigaction(SIGBUS, &sa, NULL);
   sigaction(SIGFPE, &sa, NULL);
   sigaction(SIGABRT, &sa, NULL);
}

// the null builder: every construction call succeeds and builds nothing
static int r2n_s(const char *a) { (void)a; return 0; }
static int r2n_ss(const char *a, const char *b) { (void)a; (void)b; return 0; }
static int r2n_v(void) { return 0; }
static int r2n_wire(const char *n, int w, int d, const char *i)
{ (void)n; (void)w; (void)d; (void)i; return 0; }
static int r2n_bin(const char *o, const char *n, const char *a,
                   const char *b, const char *y, int s)
{ (void)o; (void)n; (void)a; (void)b; (void)y; (void)s; return 0; }
static int r2n_un(const char *o, const char *n, const char *a,
                  const char *y, int s)
{ (void)o; (void)n; (void)a; (void)y; (void)s; return 0; }
static int r2n_mux(const char *n, const char *a, const char *b,
                   const char *s, const char *y)
{ (void)n; (void)a; (void)b; (void)s; (void)y; return 0; }
static int r2n_inst(const char *t, const char *n, const char *c)
{ (void)t; (void)n; (void)c; return 0; }
static int r2n_mem(const char *n, int w, int s)
{ (void)n; (void)w; (void)s; return 0; }
static int r2n_memrd(const char *n, const char *m, const char *a,
                     const char *d)
{ (void)n; (void)m; (void)a; (void)d; return 0; }
static int r2n_memwr(const char *m, const char *a, const char *d,
                     const char *e)
{ (void)m; (void)a; (void)d; (void)e; return 0; }
static unsigned long long r2n_hash(void) { return 0; }
static int r2n_synth(int n, const char *const *a) { (void)n; (void)a; return 1; }
static void r2n_abort(void) {}
static const gsm_rtlil_api_t g_r2_null_api = {
   .begin = r2n_s, .module = r2n_s, .wire = r2n_wire, .connect = r2n_ss,
   .cell_bin = r2n_bin, .cell_un = r2n_un, .cell_mux = r2n_mux,
   .cell_inst = r2n_inst, .proc = r2n_s, .sync = r2n_ss,
   .sync_assign = r2n_ss, .case_assign = r2n_ss, .case_assign_root = r2n_ss,
   .memory = r2n_mem, .memrd = r2n_memrd, .sync_memwr = r2n_memwr,
   .switch_begin = r2n_s, .case_begin = r2n_s, .case_end = r2n_v,
   .switch_end = r2n_v, .content_hash = r2n_hash, .synth = r2n_synth,
   .abort_session = r2n_abort
};

// Expression sigspec strings: names, selects, sized constants, temp wire
// names, or concats of those.  Binary literals spell one char per BIT, so
// this bounds the widest expressible constant/concat; wider declines.
#define R2_SPEC 4096

// memories qualified for direct construction (per module)
// user functions inlinable at call sites: straight-line pure bodies only
// (var-assigns to plain refs + nulls + one trailing valued return).  The
// table grows without a cap: tgt-vhdl emits every package function and
// every sv2v_cast_<N> helper the architecture uses as its own body (alu_top
// carries 21, exec_top 30), all straight-line.
static tree_t *g_r2_funcs;
static int     g_r2_nfuncs, g_r2_cfuncs;
static int     g_r2_inline_depth;
#define R2_INLINE_MAX 4   // package bodies call sv2v casts: to_fullPC ->
                          // sv2v_cast_32; the per-call substitution snapshot
                          // scopes each level's bindings, so nesting is safe

// the admitted user-function body called by `fn`, or NULL.  Consulted
// BEFORE the numeric_std name-pattern identities: a package function such
// as VX_gpu_pkg_inst_alu_SIGNED(op) = op(0) must inline, not pass its
// argument through as if it were numeric_std.SIGNED(x).
static tree_t r2_user_func(const char *fn);

static bool r2_func_inlinable(tree_t body, char *why, size_t wsz)
{
   const int n = tree_stmts(body);
   if (n < 1) {
      snprintf(why, wsz, "empty");
      return false;
   }
   for (int i = 0; i < n; i++) {
      tree_t st = tree_stmt(body, i);
      const tree_kind_t k = tree_kind(st);
      if (k == T_NULL)
         continue;
      if (k == T_RETURN) {
         if (i == n - 1 && tree_has_value(st))
            return true;
         snprintf(why, wsz, "ret@%d/%d", i, n);
         return false;
      }
      if (k != T_VAR_ASSIGN) {
         snprintf(why, wsz, "k%d@%d", (int)k, i);
         return false;
      }
      tree_t ft = tree_target(st);
      if (tree_kind(ft) == T_ARRAY_REF && tree_params(ft) == 1
          && tree_kind(tree_value(ft)) == T_REF)
         continue;   // per-bit build of a local: the inliner composes it
      if (tree_kind(ft) != T_REF) {
         snprintf(why, wsz, "tgt%d@%d", (int)tree_kind(ft), i);
         return false;
      }
   }
   snprintf(why, wsz, "noret");
   return false;
}

static tree_t r2_user_func(const char *fn)
{
   const char *cb = id_base(fn);
   for (int i = 0; i < g_r2_nfuncs; i++)
      if (strcasecmp(id_base(istr(tree_ident(g_r2_funcs[i]))), cb) == 0)
         return g_r2_funcs[i];
   return NULL;
}

typedef struct { ident_t id; char vname[80]; int width, size; } r2_mem_t;
static r2_mem_t g_r2_mems[16];
static int g_r2_nmems;

static r2_mem_t *r2_mem_of(ident_t id)
{
   for (int i = 0; i < g_r2_nmems; i++)
      if (g_r2_mems[i].id == id)
         return &g_r2_mems[i];
   return NULL;
}

// Decode an enum-literal ident to '0'/'1'/'x' (or 0 = not a bit literal).
// Two namings: character literals '0'/'1'/'L'/'H'/... and logic3d's NAMED
// literals L3D_0/L3D_1/L3D_X/...
static char r2_bit_of_tree(tree_t v);

static char r2_bit_of_ident(ident_t id)
{
   const char *n = istr(id);
   char c = 0;
   if (n[0] == '\'' && n[1] != '\0')
      c = n[1];
   else if (strncmp(n, "L3D_", 4) == 0)
      c = n[4];
   if (c == '0' || c == 'L') return '0';
   if (c == '1' || c == 'H') return '1';
   // the NAMED L3D_X has a value-plane form (0); a std_logic CHARACTER
   // metavalue ('U' 'X' 'W' '-' 'Z') has none and declines, as the text
   // path does (test/accel/l3dmv: a 'U' drive must not install)
   if (c == 'X' || c == 'U' || c == 'W' || c == '-' || c == 'Z')
      return n[0] == '\'' ? 0 : 'x';
   return 0;
}

// bit decode over a tree: refs (through named constants), literal encodings
static char r2_bit_of_tree(tree_t v)
{
   if (tree_kind(v) == T_REF) {
      const char c = r2_bit_of_ident(tree_has_ref(v)
                                     ? tree_ident(tree_ref(v))
                                     : tree_ident(v));
      if (c != 0)
         return c;
      if (tree_has_ref(v) && tree_kind(tree_ref(v)) == T_CONST_DECL
          && tree_has_value(tree_ref(v)))
         return r2_bit_of_tree(tree_value(tree_ref(v)));
      return 0;
   }
   int64_t bit;
   if (folded_int(v, &bit) && bit >= 0 && bit <= 7)
      return (bit & 1) ? '1' : '0';   // logic3d encoding: bit0 = value
   return 0;
}

// a std_logic CHARACTER metavalue literal ('X' 'Z' 'U' 'W' '-'): no
// value-plane form (the named L3D_X has one: 0)
static bool r2_char_meta(tree_t v)
{
   if (tree_kind(v) != T_REF)
      return false;
   const char *n = istr(tree_has_ref(v) ? tree_ident(tree_ref(v))
                                        : tree_ident(v));
   return n[0] == '\'' && n[1] != '\0' && n[2] == '\''
      && strchr("XZUW-", toupper((unsigned char)n[1])) != NULL;
}

static int r2_clog2(int n)
{
   int b = 0;
   while ((1 << b) < n)
      b++;
   return b > 0 ? b : 1;
}

// NBA-shadow aliases for the CURRENT process (tgt-vhdl idiom): the shadow
// variable v_nba_<r> is an alias for <r>'s hold temp — the pre-copy IS the
// root action, the commit IS the sync assign, both elided.
typedef struct { ident_t var; ident_t sig; tree_t sigdecl; bool committed;
                 bool wrote;   // any write through the alias so far: a read
                               // after that must see the WRITTEN value, and
                               // the signal only carries the pre-edge one
} r2_alias_t;
static r2_alias_t g_r2_alias[16];
static int        g_r2_nalias;

// straight-line process-local variables: pure expression SUBSTITUTION —
// a write records the value's sigspec, a read returns it (the read_verilog
// technique).  A write INSIDE a switch arm is scoped to that arm: it tags
// the entry with the case depth, and leaving the arm POISONS every entry
// written at that depth or deeper (spec/bits/ival cleared), so a later read
// outside the arm cannot pick up a branch-dependent value — it either
// resolves through the variable's promoted hold temp (r2_pvar_t) or
// declines.  This is read_verilog's subst_rvalue_map with the post-branch
// `$1` merge replaced by the pvar mux.
static bool r2_temp(int width, char *out, size_t sz);

typedef struct { ident_t var; char *spec;
                 bool has_ival; int64_t ival;
                 int wdepth;          // case depth of the last write (scope)
                 int sw;              // >0: rendered width of spec (known)
                 tree_t vtree;        // the value TREE behind spec, when a
                                      // whole write recorded it (idiom
                                      // recognisers look through it: the
                                      // OOB_WriteV index `l3d_index(e * K)`)
                 int bw;              // >0: var built PER-BIT (bits[] specs)
                 char *bits[128]; } r2_subst_t;
static r2_subst_t g_r2_subst[32];
static int        g_r2_nsubst;
static int        g_r2_case_depth;   // >0 while inside any switch case
// pv-wire name of a promoted latch var readable at this walk point (NULL
// if none / already written) — implemented after the pvar machinery
static const char *r2_pvar_read_pv(ident_t id);

// PATH-CONDITION stack: one 1-bit sigspec per open case level, kept in
// lockstep with g_r2_case_depth.  Powers VAR VERSIONING: a write to a
// process-local under branches becomes a module-level $mux(pathcond,
// value, prev-version) into a fresh wire — pure feed-forward SSA, no
// proc-action ordering involved, so read-after-write and dynamic
// part-writes to locals are all legal.
static char g_r2_conds[24][96];
static int  g_r2_nconds;

static bool r2_cond_push(const char *cs)
{
   if (g_r2_nconds >= 24 || strlen(cs) >= sizeof g_r2_conds[0])
      return false;
   snprintf(g_r2_conds[g_r2_nconds++], sizeof g_r2_conds[0], "%s", cs);
   return true;
}

static void r2_cond_pop(void)
{
   if (g_r2_nconds > 0)
      g_r2_nconds--;
}

static r2_subst_t *r2_subst_of(ident_t var)
{
   for (int i = 0; i < g_r2_nsubst; i++)
      if (g_r2_subst[i].var == var)
         return &g_r2_subst[i];
   return NULL;
}

static void r2_subst_clear_bits(r2_subst_t *e)
{
   for (int i = 0; i < e->bw && i < 128; i++)
      free(e->bits[i]);
   e->bw = 0;
}

// seed a bit table from a sized-literal spec ("N'dK" / "N'bBB.." / "N'hHH..")
static bool r2_bits_seed(char **bits, int w, const char *spec)
{
   const char *tick = spec ? strchr(spec, 39) : NULL;
   if (tick == NULL)
      return false;
   if (tick[1] == 'd') {
      if (w > 63)
         return false;
      const int64_t v = atoll(tick + 2);
      for (int i = 0; i < w; i++) {
         char b[8];
         snprintf(b, sizeof b, "1'd%d", (int)((v >> i) & 1));
         bits[i] = xstrdup(b);
      }
      return true;
   }
   if (tick[1] == 'b' || tick[1] == 'h') {
      const char *dig = tick + 2;
      const int nd = (int)strlen(dig);
      const int bpc = tick[1] == 'b' ? 1 : 4;
      if (nd * bpc < w)
         return false;
      for (int i = 0; i < w; i++) {
         // bit i counts from the LSB end = last digit backwards
         const int di = nd - 1 - i / bpc;
         int dv;
         const char c = dig[di];
         if (c >= '0' && c <= '9') dv = c - '0';
         else if (c >= 'a' && c <= 'f') dv = c - 'a' + 10;
         else if (c >= 'A' && c <= 'F') dv = c - 'A' + 10;
         else return false;
         char b[8];
         snprintf(b, sizeof b, "1'd%d", (dv >> (i % bpc)) & 1);
         bits[i] = xstrdup(b);
      }
      return true;
   }
   return false;
}

static void r2_subst_reset(void)
{
   for (int i = 0; i < g_r2_nsubst; i++) {
      free(g_r2_subst[i].spec);
      r2_subst_clear_bits(&g_r2_subst[i]);
   }
   g_r2_nsubst = 0;
}

static bool r2_subst_set(ident_t var, const char *spec)
{
   r2_subst_t *e = r2_subst_of(var);
   if (e == NULL) {
      if (g_r2_nsubst >= 32)
         return false;
      e = &g_r2_subst[g_r2_nsubst++];
      e->var = var;
      e->spec = NULL;
      e->bw = 0;
   }
   free(e->spec);
   r2_subst_clear_bits(e);
   e->spec = xstrdup(spec);
   e->has_ival = false;
   e->wdepth = g_r2_case_depth;
   e->sw = 0;
   e->vtree = NULL;
   return true;
}

// ... with the rendered width of the spec when the writer knows it (a
// slice/element read of the variable lands the spec on a wire of exactly
// that width — see r2_var_base)
static bool r2_subst_set_w(ident_t var, const char *spec, int w)
{
   if (!r2_subst_set(var, spec))
      return false;
   r2_subst_of(var)->sw = w > 0 ? w : 0;
   return true;
}

// ... and the value tree it was rendered from (whole writes only)
static bool r2_subst_set_wt(ident_t var, const char *spec, int w, tree_t v)
{
   if (!r2_subst_set_w(var, spec, w))
      return false;
   r2_subst_of(var)->vtree = v;
   return true;
}

// leaving a switch arm: every entry written inside it (at this depth or
// deeper) is branch-dependent from here on — clear it (see r2_subst_t)
static void r2_subst_poison_from(int depth)
{
   for (int i = 0; i < g_r2_nsubst; i++) {
      r2_subst_t *e = &g_r2_subst[i];
      if (e->wdepth < depth)
         continue;
      free(e->spec);
      e->spec = NULL;
      r2_subst_clear_bits(e);
      e->has_ival = false;
      e->wdepth = 0;
      e->vtree = NULL;
   }
}


// constant-valued substitution: spec doubles as the sigspec when the value
// is representable there; a NEGATIVE value keeps spec NULL (no sigspec form)
// but stays evaluable — the OOB-guard idiom computes negative indices whose
// USES are pruned by statically-false guards, never rendered
static bool r2_subst_set_int(ident_t var, int64_t v)
{
   r2_subst_t *e = r2_subst_of(var);
   if (e == NULL) {
      if (g_r2_nsubst >= 32)
         return false;
      e = &g_r2_subst[g_r2_nsubst++];
      e->var = var;
      e->spec = NULL;
      e->bw = 0;
   }
   free(e->spec);
   r2_subst_clear_bits(e);
   e->spec = NULL;
   if (v >= 0) {
      char b[32];
      snprintf(b, sizeof b, "32'd%lld", (long long)v);
      e->spec = xstrdup(b);
   }
   e->has_ival = true;
   e->ival = v;
   e->wdepth = g_r2_case_depth;
   e->sw = 0;
   e->vtree = NULL;
   return true;
}

static bool r2_subst_compose(const r2_subst_t *e, char *out, size_t sz)
{
   if (e->bw < 1)
      return false;
   size_t len = 0;
   out[len++] = '{';
   for (int i = e->bw - 1; i >= 0; i--) {
      if (e->bits[i] == NULL || len + strlen(e->bits[i]) + 3 >= sz)
         return false;
      if (i != e->bw - 1)
         out[len++] = ',';
      memcpy(out + len, e->bits[i], strlen(e->bits[i]));
      len += strlen(e->bits[i]);
   }
   out[len++] = '}';
   out[len] = '\0';
   return true;
}

// Walker-side constant interpreter: evaluate an expression to an integer
// under the current substitution environment.  Pure TRY — returns false on
// anything unknown, never declines.  Powers while-loop unrolling, static-if
// pruning, and arithmetic index/bound folding (the translated-SV counting
// loops compute indices like To_Integer(i)*992 + (To_Integer(j)-1)*32 that
// folded_int cannot see through).  logic3d const vectors decode by the
// value plane (bit0), MSB first, SIGN-EXTENDED by their width (the loop
// machinery compares with l3d_lt_s and friends — signed).
static int r2_width(tree_t e);

// constant-evaluable design functions (they run on the substitution
// snapshot machinery defined further down) and constant-driven signals
static int g_r2_ncfn;
static tree_t r2_cfn_of(const char *fn);
static bool r2_cfn_call(tree_t call, int64_t *out);
static int r2_cfn_ret_width(tree_t call);
typedef struct { ident_t id; int64_t v; } r2_csig_t;
static r2_csig_t g_r2_csig[64];
static int g_r2_ncsig;
static int g_r2_eval_depth;

static bool r2_eval_int(tree_t e, int64_t *out)
{
   if (folded_int(e, out))
      return true;
   switch (tree_kind(e)) {
   case T_QUALIFIED:
   case T_TYPE_CONV:
   case T_INERTIAL:
      return r2_eval_int(tree_value(e), out);
   case T_REF:
      {
         r2_subst_t *sb = r2_subst_of(tree_ident(e));
         if (sb != NULL && sb->has_ival) {
            *out = sb->ival;
            return true;
         }
         if (sb != NULL && sb->spec != NULL) {
            const char *tick = strchr(sb->spec, 39);
            if (tick != NULL && tick[1] == 'd') {
               *out = atoll(tick + 2);
               return true;
            }
         }
         if (tree_has_ref(e) && g_r2_eval_depth < 8) {
            // a design constant of integer or vector type evaluates
            // through its value (generate indices land in constant decls
            // and index the wire arrays); a signal whose ONLY driver is a
            // constant continuous assign IS that constant (tgt-vhdl
            // passes function actuals through such temps:
            // `tmp <= "0010"; y <= get_cnt_at_lev(tmp, ...)`)
            tree_t d = tree_ref(e);
            const tree_kind_t dk = tree_kind(d);
            type_t dt = tree_type(d);
            if ((dk == T_CONST_DECL || dk == T_GENERIC_DECL)
                && tree_has_value(d)
                && ((type_is_integer(dt) && !type_is_logic3d(dt))
                    || type_is_array(dt))) {
               g_r2_eval_depth++;
               const bool ok = r2_eval_int(tree_value(d), out);
               g_r2_eval_depth--;
               return ok;
            }
            if (dk == T_SIGNAL_DECL) {
               for (int i = 0; i < g_r2_ncsig; i++)
                  if (g_r2_csig[i].id == tree_ident(e)) {
                     *out = g_r2_csig[i].v;
                     return true;
                  }
            }
         }
         return false;
      }
   case T_STRING:
   case T_AGGREGATE:
      {
         // uniform (others => bit) with a constrained width evaluates
         if (tree_kind(e) == T_AGGREGATE && tree_assocs(e) == 1
             && tree_subkind(tree_assoc(e, 0)) == A_OTHERS) {
            const int w = r2_width(e);
            const char b = r2_bit_of_tree(tree_value(tree_assoc(e, 0)));
            if (w >= 1 && w <= 63 && b != 0) {
               uint64_t v = (b == '1') ? ((w == 63) ? ~0ULL >> 1
                                                    : (1ULL << w) - 1) : 0;
               int64_t sv = (int64_t)v;
               if (b == '1')
                  sv = -1;   // all-ones is -1 at any width, signed
               *out = sv;
               return true;
            }
            return false;
         }
         // constant logic3d / std_logic vector: value plane, MSB first.  A
         // literal wider than 63 bits evaluates when its high bits are all
         // zero (the 71-bit lane-stride constants of VX_pipe_register's
         // partial-reset loop): the value is then positive
         uint64_t v = 0;
         int n = 0;
         const bool str = tree_kind(e) == T_STRING;
         n = str ? tree_chars(e) : tree_assocs(e);
         if (n < 1 || n > 4096) return false;
         for (int i = 0; i < n; i++) {
            char b;
            if (str)
               b = r2_bit_of_tree(tree_char(e, i));
            else {
               tree_t a = tree_assoc(e, i);
               if (tree_subkind(a) != A_POS) return false;
               b = r2_bit_of_tree(tree_value(a));
            }
            if (b == 0) return false;
            if (n - i > 63 && b == '1')
               return false;   // set bit beyond the interpreter's range
            v = (v << 1) | (b == '1' ? 1 : 0);
         }
         int64_t sv = (int64_t)v;
         if (n < 64 && (v & (1ULL << (n - 1))))
            sv -= (int64_t)(1ULL << n);
         *out = sv;
         return true;
      }
   case T_FCALL:
      {
         const char *fn = istr(tree_ident(e));
         const char *base = id_base(fn);
         const int np = tree_params(e);
         int64_t a = 0, b = 0;
         if (g_r2_ncfn > 0 && r2_cfn_of(fn) != NULL)
            return r2_cfn_call(e, out);
         if (np == 2) {
            if (!r2_eval_int(tree_value(tree_param(e, 0)), &a)
                || !r2_eval_int(tree_value(tree_param(e, 1)), &b)) {
               // identity-with-width forms take (value, width): retry as unary
               if (strcasecmp(base, "resize") == 0
                   || strcasecmp(base, "l3d_resize_s") == 0
                   || strcasecmp(base, "l3d_index") == 0
                   || strcasecmp(base, "to_unsigned") == 0
                   || strcasecmp(base, "to_signed") == 0)
                  return r2_eval_int(tree_value(tree_param(e, 0)), out);
               return false;
            }
         }
         else if (np == 1) {
            if (!r2_eval_int(tree_value(tree_param(e, 0)), &a))
               return false;
         }
         else
            return false;
         if (np == 2) {
            // value-preserving width forms (2nd param is the width)
            if (strcasecmp(base, "resize") == 0
                || strcasecmp(base, "l3d_resize_s") == 0
                || strcasecmp(base, "l3d_index") == 0
                || strcasecmp(base, "to_unsigned") == 0
                || strcasecmp(base, "to_signed") == 0)
               { *out = a; return true; }
            if (strcasecmp(base, "l3d_lt_s") == 0) { *out = a < b;  return true; }
            if (strcasecmp(base, "l3d_le_s") == 0) { *out = a <= b; return true; }
            if (strcasecmp(base, "l3d_gt_s") == 0) { *out = a > b;  return true; }
            if (strcasecmp(base, "l3d_ge_s") == 0) { *out = a >= b; return true; }
            if (strcasecmp(base, "l3d_eq1") == 0)  { *out = a == b; return true; }
            if (strcasecmp(base, "l3d_ne1") == 0)  { *out = a != b; return true; }
            if (strcasecmp(base, "l3d_mod_s") == 0
                || strcmp(fn, "\"rem\"") == 0) { if (b == 0) return false;
                                                 *out = a % b; return true; }
            if (strcmp(fn, "\"mod\"") == 0) { if (b == 0) return false;
                                              int64_t m = a % b;
                                              if (m != 0 && ((m < 0) != (b < 0)))
                                                 m += b;
                                              *out = m; return true; }
            if (strcasecmp(base, "l3d_div_s") == 0) { if (b == 0) return false;
                                                      *out = a / b; return true; }
            if (strcasecmp(base, "l3d_sra") == 0)   { if (b < 0 || b > 62) return false;
                                                      *out = a >> b; return true; }
            if (strcmp(fn, "\"+\"") == 0)   { *out = a + b; return true; }
            if (strcmp(fn, "\"-\"") == 0)   { *out = a - b; return true; }
            if (strcmp(fn, "\"*\"") == 0)   { *out = a * b; return true; }
            if (strcmp(fn, "\"/\"") == 0)   { if (b == 0) return false;
                                            *out = a / b; return true; }
            if (strcmp(fn, "\"=\"") == 0)   { *out = a == b; return true; }
            if (strcmp(fn, "\"/=\"") == 0)  { *out = a != b; return true; }
            if (strcmp(fn, "\"<\"") == 0)   { *out = a < b;  return true; }
            if (strcmp(fn, "\"<=\"") == 0)  { *out = a <= b; return true; }
            if (strcmp(fn, "\">\"") == 0)   { *out = a > b;  return true; }
            if (strcmp(fn, "\">=\"") == 0)  { *out = a >= b; return true; }
            if (strcmp(fn, "\"and\"") == 0) { *out = (a != 0) && (b != 0); return true; }
            if (strcmp(fn, "\"or\"") == 0)  { *out = (a != 0) || (b != 0); return true; }
            return false;
         }
         // unary
         if (strcasecmp(base, "to_integer") == 0
             || strcasecmp(base, "l3d_shcount") == 0
             || strcasecmp(base, "boolean_to_logic") == 0
             || strcasecmp(base, "to_l3d") == 0
             || strcasecmp(base, "unsigned") == 0
             || strcasecmp(base, "signed") == 0
             || strcasecmp(base, "std_logic_vector") == 0)
            { *out = a; return true; }
         if (strcasecmp(base, "is_one") == 0)  { *out = a == 1; return true; }
         if (strcasecmp(base, "is_zero") == 0) { *out = a == 0; return true; }
         if (strcmp(fn, "\"-\"") == 0)   { *out = -a; return true; }
         if (strcmp(fn, "\"not\"") == 0) { *out = a == 0; return true; }
         return false;
      }
   default:
      return false;
   }
}

// Snapshot/restore of the whole substitution table, for user-function
// inlining: bindings and body-local writes must not leak into (or clobber)
// the calling process's substitutions — generated code reuses names like
// i/j across scopes.
typedef struct {
   int n;
   r2_subst_t e[32];   // specs are OWNED copies
} r2_subst_snap_t;

static void r2_subst_save(r2_subst_snap_t *sn)
{
   sn->n = g_r2_nsubst;
   for (int i = 0; i < g_r2_nsubst; i++) {
      sn->e[i] = g_r2_subst[i];
      sn->e[i].spec = g_r2_subst[i].spec ? xstrdup(g_r2_subst[i].spec) : NULL;
   }
}

static void r2_subst_restore(r2_subst_snap_t *sn)
{
   for (int i = 0; i < g_r2_nsubst; i++)
      free(g_r2_subst[i].spec);
   for (int i = 0; i < sn->n; i++)
      g_r2_subst[i] = sn->e[i];
   g_r2_nsubst = sn->n;
}

// ---- constant-evaluable design functions --------------------------------
// A body the inliner rejects (a while-loop, if/else, several returns) still
// evaluates when every actual is a constant: VX_csa_tree's get_cnt_at_lev
// walks the tree levels with a while-loop over generate constants.  The
// interpreter keeps its state in the substitution table (parameters and
// locals are constant-valued substitutions, r2_eval_int reads them) and
// snapshots the table around the call exactly like the inliner.
static tree_t g_r2_cfn[64];
static int g_r2_cfn_depth;

static tree_t r2_cfn_of(const char *fn)
{
   const char *cb = id_base(fn);
   for (int i = 0; i < g_r2_ncfn; i++)
      if (strcasecmp(id_base(istr(tree_ident(g_r2_cfn[i]))), cb) == 0)
         return g_r2_cfn[i];
   return NULL;
}

static bool r2_cfn_shape_list(tree_t list_of);

static bool r2_cfn_shape_stmt(tree_t s)
{
   switch (tree_kind(s)) {
   case T_NULL:
      return true;
   case T_RETURN:
      return tree_has_value(s);
   case T_VAR_ASSIGN:
      return tree_kind(tree_target(s)) == T_REF;
   case T_IF:
      for (int i = 0; i < tree_conds(s); i++)
         if (!r2_cfn_shape_list(tree_cond(s, i)))
            return false;
      return true;
   case T_WHILE:
      return tree_has_value(s) && r2_cfn_shape_list(s);
   default:
      return false;
   }
}

static bool r2_cfn_shape_list(tree_t list_of)
{
   for (int i = 0; i < tree_stmts(list_of); i++)
      if (!r2_cfn_shape_stmt(tree_stmt(list_of, i)))
         return false;
   return true;
}

static bool r2_cfn_admit(tree_t body)
{
   if (g_r2_ncfn >= 64 || tree_ports(body) > 8)
      return false;
   for (int i = 0; i < tree_decls(body); i++)
      if (tree_kind(tree_decl(body, i)) != T_VAR_DECL)
         return false;
   if (!r2_cfn_shape_list(body))
      return false;
   g_r2_cfn[g_r2_ncfn++] = body;
   return true;
}

static int r2_cfn_ret_width(tree_t call)
{
   tree_t fb = r2_cfn_of(istr(tree_ident(call)));
   if (fb == NULL)
      return -1;
   for (int i = tree_stmts(fb) - 1; i >= 0; i--) {
      tree_t s = tree_stmt(fb, i);
      if (tree_kind(s) == T_RETURN && tree_has_value(s))
         return r2_width(tree_value(s));
   }
   return -1;
}

// 0 = fell through, 1 = returned (*ret set), -1 = not evaluable
static int r2_cfn_exec_list(tree_t list_of, int64_t *ret);

static int r2_cfn_exec(tree_t s, int64_t *ret)
{
   switch (tree_kind(s)) {
   case T_NULL:
      return 0;
   case T_RETURN:
      return r2_eval_int(tree_value(s), ret) ? 1 : -1;
   case T_VAR_ASSIGN:
      {
         int64_t v;
         if (!r2_eval_int(tree_value(s), &v))
            return -1;
         return r2_subst_set_int(tree_ident(tree_target(s)), v) ? 0 : -1;
      }
   case T_IF:
      for (int i = 0; i < tree_conds(s); i++) {
         tree_t c = tree_cond(s, i);
         if (tree_has_value(c)) {
            int64_t cv;
            if (!r2_eval_int(tree_value(c), &cv))
               return -1;
            if (cv == 0)
               continue;
         }
         return r2_cfn_exec_list(c, ret);
      }
      return 0;
   case T_WHILE:
      for (int steps = 0; steps < 100000; steps++) {
         int64_t cv;
         if (!r2_eval_int(tree_value(s), &cv))
            return -1;
         if (cv == 0)
            return 0;
         const int r = r2_cfn_exec_list(s, ret);
         if (r != 0)
            return r;
      }
      return -1;
   default:
      return -1;
   }
}

static int r2_cfn_exec_list(tree_t list_of, int64_t *ret)
{
   for (int i = 0; i < tree_stmts(list_of); i++) {
      const int r = r2_cfn_exec(tree_stmt(list_of, i), ret);
      if (r != 0)
         return r;
   }
   return 0;
}

static bool r2_cfn_call(tree_t call, int64_t *out)
{
   tree_t fb = r2_cfn_of(istr(tree_ident(call)));
   const int np = tree_params(call);
   if (fb == NULL || np != tree_ports(fb) || g_r2_cfn_depth >= 16)
      return false;
   int64_t act[8];
   for (int i = 0; i < np; i++)
      if (!r2_eval_int(tree_value(tree_param(call, i)), &act[i]))
         return false;
   r2_subst_snap_t sn;
   r2_subst_save(&sn);
   g_r2_cfn_depth++;
   bool ok = true;
   for (int i = 0; ok && i < np; i++)
      ok = r2_subst_set_int(tree_ident(tree_port(fb, i)), act[i]);
   for (int i = 0; ok && i < tree_decls(fb); i++) {
      tree_t vd = tree_decl(fb, i);
      if (tree_has_value(vd)) {
         int64_t v;
         ok = r2_eval_int(tree_value(vd), &v)
            && r2_subst_set_int(tree_ident(vd), v);
      }
   }
   int64_t ret = 0;
   if (ok)
      ok = r2_cfn_exec_list(fb, &ret) == 1;
   g_r2_cfn_depth--;
   r2_subst_restore(&sn);
   if (ok)
      *out = ret;
   return ok;
}

static r2_alias_t *r2_alias_of(ident_t var)
{
   for (int i = 0; i < g_r2_nalias; i++)
      if (g_r2_alias[i].var == var)
         return &g_r2_alias[i];
   return NULL;
}

static int r2_width(tree_t e)
{
   const int ew = emitted_width(e, 0);
   if (ew > 0)
      return ew;
   type_t t = tree_type(e);
   // an INTEGER-valued operator result (index arithmetic `Idx + P`, an
   // integer function's return): the translator's `signed [31:0]`, not
   // type_width's scalar 1.  logic3d is an integer subtype but one bit.
   if (type_is_integer(t) && !type_is_logic3d(t))
      return 32;
   return type_const_bounds(t) ? (int)type_width(t) : -1;
}

// Constant expression -> sized sigspec literal ("8'd42" / "4'b01xz").
static bool r2_const(tree_t e, char *out, size_t sz, int want_w)
{
   if (tree_kind(e) == T_FCALL && g_r2_ncfn > 0
       && r2_cfn_of(istr(tree_ident(e))) != NULL) {
      // a constant-evaluable design function with constant actuals
      int64_t cv;
      int w = want_w > 0 ? want_w : r2_width(e);
      if (w < 1)
         w = r2_cfn_ret_width(e);
      if (w >= 1 && w <= 63 && r2_cfn_call(e, &cv)) {
         const uint64_t m = (w == 63) ? (~0ULL >> 1) : ((1ULL << w) - 1);
         snprintf(out, sz, "%d'd%llu", w,
                  (unsigned long long)((uint64_t)cv & m));
         return true;
      }
      return false;
   }
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
             && r2_user_func(fn) == NULL
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
   // a std_logic CHARACTER metavalue ('U' 'X' 'W' '-' 'Z') has no
   // value-plane form: never let folded_int's enum POSITION stand in for it
   // (test/accel/l3dmv: a 'U' drive must decline, not install as 0)
   if (tree_kind(e) == T_REF && r2_char_meta(e))
      return false;
   // scalar enum bits FIRST: folded_int on a std_logic ref returns the enum
   // POSITION, whose low bit coincidentally matches for 0/1/L/H but silently
   // aliases X and Z — decode the literal character instead
   if (tree_kind(e) == T_REF) {
      const ident_t id = tree_has_ref(e) ? tree_ident(tree_ref(e))
                                         : tree_ident(e);
      const char b = r2_bit_of_ident(id);
      if (b == '0') { snprintf(out, sz, "1'b0"); return true; }
      if (b == '1') { snprintf(out, sz, "1'b1"); return true; }
      if (b == 'x') {
         // a logic3d NAMED metavalue (L3D_X in `v := L3D_X`, the casez
         // default arm) is 0 on the value plane — what the text path
         // prints for every L3D_ literal but L3D_1; a std_logic CHARACTER
         // metavalue ('X', 'Z') has no value-plane form and declines
         // there, so it declines here too
         if (strncmp(istr(id), "L3D_", 4) == 0) {
            snprintf(out, sz, "1'b0");
            return true;
         }
         return false;
      }
   }
   int64_t iv;
   if (folded_int(e, &iv)) {
      // a SCALAR logic3d literal is the natural 0..7 ENCODING (bit0 =
      // value plane): elaboration folds the package constant L3D_0 into
      // the literal 2, which must render as the value bit, never as
      // `1'd2` (VX_fcvt_unit's `L3D_0 & fclass(4)`, first hit by the
      // walker in the FPU's exponent unpack)
      {
         type_t lt = tree_type(e);
         if (lt != NULL && !type_is_array(lt) && type_is_logic3d(lt)
             && iv >= 0 && iv <= 7) {
            snprintf(out, sz, "1'b%d", (int)(iv & 1));
            return true;
         }
      }
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
         const char c =
            r2_bit_of_ident(tree_ident(tree_ref(tree_char(e, i))));
         switch (c) {
         case '0': *p++ = '0'; break;
         case '1': *p++ = '1'; break;
         case 'x': *p++ = '0'; break;   // metavalue: text path renders 0
         default: return false;
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
         const char c = r2_bit_of_tree(v);
         if (c == '0') *p++ = '0';
         else if (c == '1') *p++ = '1';
         else if (c == 'x')
            *p++ = '0';   // metavalue: text path renders 0
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
         // nested uniform aggregate (others => (others => b)): an
         // array-of-vector wire's power-on fill
         while (tree_kind(v) == T_AGGREGATE && tree_assocs(v) == 1
                && tree_subkind(tree_assoc(v, 0)) == A_OTHERS)
            v = tree_value(tree_assoc(v, 0));
         char cb;
         const char c = r2_bit_of_tree(v);
         if (c == '0') cb = '0';
         else if (c == '1') cb = '1';
         else if (c == 'x')
            cb = '0';   // metavalue init: the text path renders {N{1'b0}}
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
// width for a cell result: the expression's own width, falling back to
// the widest operand when the result type is unconstrained (operator FCALL
// results carry no bounds — verilog self-determination is the text-path
// semantics being mirrored)
static int r2_width_or_operands(tree_t e)
{
   const int w = r2_width(e);
   if (w > 0)
      return w;
   if (tree_kind(e) == T_FCALL) {
      // concatenation: the operands' widths ADD (an unconstrained `a & b
      // & L3D_1` chain — the serial divider's shift-in operands)
      const int np = tree_params(e);
      if (np == 2 && strcmp(istr(tree_ident(e)), "\"&\"") == 0) {
         const int wa = r2_width_or_operands(tree_value(tree_param(e, 0)));
         const int wb = r2_width_or_operands(tree_value(tree_param(e, 1)));
         return (wa > 0 && wb > 0) ? wa + wb : -1;
      }
      // recurse: chains of unconstrained operator results (l3d_or of
      // l3d_and of resize of ...) carry their width arbitrarily deep
      int mx = -1;
      for (int i = 0; i < np; i++) {
         const int pw = r2_width_or_operands(tree_value(tree_param(e, i)));
         if (pw > mx)
            mx = pw;
      }
      return mx;
   }
   if (tree_kind(e) == T_QUALIFIED || tree_kind(e) == T_TYPE_CONV)
      return r2_width_or_operands(tree_value(e));
   return w;
}

static bool r2_temp(int width, char *out, size_t sz)
{
   if (width <= 0) {
      char why[32];
      snprintf(why, sizeof why, "temp-width %d", width);
      R2_DECLINE(why);
      return false;
   }
   snprintf(out, sz, "rx%d", g_r2_tmp++);
   return g_r2->wire(out, width, 0, NULL) == 0;
}

// The declared width of a local (variable / bound formal) referenced by
// `e`: a constrained array's width, 32 for an integer (the translator's
// `signed [31:0]`), 1 for a scalar bit; -1 when unknown (unconstrained
// formal)
static int r2_local_width(tree_t e)
{
   tree_t d = tree_has_ref(e) ? tree_ref(e) : NULL;
   if (d == NULL || (tree_kind(d) != T_VAR_DECL && tree_kind(d) != T_PARAM_DECL))
      return 32;
   type_t ty = tree_type(d);
   if (type_is_array(ty))
      return type_const_bounds(ty) ? (int)type_width(ty) : -1;
   if (type_is_integer(ty) && !type_is_logic3d(ty))
      return 32;
   return 1;
}

// The width of the sigspec r2_expr RENDERS for `e` — which differs from the
// VHDL type width wherever the walker prints an identity's operand verbatim
// (l3d_index / to_integer / resize drop their conversion, so an Integer
// index computed from a 2-bit field renders 2 bits wide) and follows a
// substituted variable to the value it was rendered from.  Used where the
// width must be exact: a switch signal and its compare constants.
static int r2_rendered_width(tree_t e)
{
   for (int guard = 0; guard < 32; guard++) {
      const tree_kind_t k = tree_kind(e);
      if (k == T_TYPE_CONV || k == T_QUALIFIED || k == T_INERTIAL) {
         e = tree_value(e);
         continue;
      }
      if (k == T_REF) {
         r2_subst_t *sb = r2_subst_of(tree_ident(e));
         if (sb != NULL && sb->vtree != NULL) {
            e = sb->vtree;
            continue;
         }
         if (sb != NULL && sb->has_ival) {
            const int lw = r2_local_width(e);   // as the T_REF read renders
            return (lw >= 1 && lw <= 63) ? lw : 32;
         }
         if (sb != NULL && sb->sw > 0)
            return sb->sw;
         if (sb != NULL && sb->bw > 0)
            return sb->bw;
         return r2_width(e);
      }
      if (k == T_FCALL && tree_params(e) >= 1) {
         const char *fn = istr(tree_ident(e));
         const char *base = id_base(fn);
         int lk = -1;
         const char *lop = vlog_l3d_op(fn, &lk);
         if (tree_params(e) == 2 && strcasecmp(base, "l3d_resize_s") == 0) {
            // rendered at its target width (sign-extended / truncated)
            int64_t nw;
            if (folded_int(tree_value(tree_param(e, 1)), &nw)
                || r2_eval_int(tree_value(tree_param(e, 1)), &nw))
               return nw > 0 ? (int)nw : -1;
            return -1;
         }
         const bool ident =
            (lop != NULL && lk == 2)
            || strcasecmp(base, "l3d_index") == 0
            || strcasecmp(base, "l3d_shcount") == 0
            || (tree_params(e) == 1 && vlog_op(fn) == NULL
                && r2_user_func(fn) == NULL
                && (strstr(fn, "UNSIGNED") || strstr(fn, "SIGNED")
                    || strstr(fn, "STD_LOGIC_VECTOR")
                    || strstr(fn, "TO_STDLOGICVECTOR")
                    || strstr(fn, "TO_INTEGER")
                    || strstr(fn, "unsigned") || strstr(fn, "to_integer")
                    || strstr(fn, "std_logic_vector")));
         if (ident) {
            e = tree_value(tree_param(e, 0));
            continue;
         }
      }
      break;
   }
   return r2_width_or_operands(e);
}

// The shape of a dynamic index expression, looking through conversions and
// substituted variables: signed (`l3d_index(x, True)`, a SIGNED operand),
// and the constant STRIDE when it is a product `x * K` that cannot wrap
// (the rendered width of x plus K's bits fits the product) — every value
// the index takes is then a multiple of K, so only those positions need an
// arm (read_verilog's `[expr * K +: K]` case reduction).
static void r2_index_shape(tree_t e, int *stride, bool *sgn)
{
   *stride = 1;
   *sgn = false;
   for (int guard = 0; guard < 32; guard++) {
      const tree_kind_t k = tree_kind(e);
      if (k == T_TYPE_CONV || k == T_QUALIFIED || k == T_INERTIAL) {
         e = tree_value(e);
         continue;
      }
      if (k == T_REF) {
         r2_subst_t *sb = r2_subst_of(tree_ident(e));
         if (sb != NULL && sb->vtree != NULL) {
            e = sb->vtree;
            continue;
         }
         if (tree_has_ref(e) && type_is_signed(tree_type(e)))
            *sgn = true;
         break;
      }
      if (k != T_FCALL)
         break;
      const char *fn = istr(tree_ident(e));
      const char *base = id_base(fn);
      const int np = tree_params(e);
      if (np == 2 && strcasecmp(base, "l3d_index") == 0) {
         bool b;
         int64_t iv;
         tree_t p1 = tree_value(tree_param(e, 1));
         if (folded_bool(p1, &b))
            *sgn = b;
         else if (r2_eval_int(p1, &iv))
            *sgn = iv != 0;
         e = tree_value(tree_param(e, 0));
         continue;
      }
      if (np == 2 && strcmp(fn, "\"*\"") == 0) {
         tree_t p0 = tree_value(tree_param(e, 0));
         tree_t p1 = tree_value(tree_param(e, 1));
         int64_t kv;
         tree_t o = NULL;
         if (r2_eval_int(p1, &kv))
            o = p0;
         else if (r2_eval_int(p0, &kv))
            o = p1;
         if (o != NULL && kv > 0 && kv <= 4096) {
            const int ow = r2_rendered_width(o), pw = r2_rendered_width(e);
            if (ow > 0 && pw > 0 && ow + r2_clog2((int)kv + 1) <= pw)
               *stride = (int)kv;
         }
         if (type_is_signed(tree_type(p0)) || type_is_signed(tree_type(p1)))
            *sgn = true;
         break;
      }
      {
         int lk = -1;
         const char *lop = vlog_l3d_op(fn, &lk);
         const bool ident = np >= 1
            && ((lop != NULL && lk == 2)
                || strcasecmp(base, "l3d_resize_s") == 0
                || strcasecmp(base, "l3d_shcount") == 0
                || (np == 1 && vlog_op(fn) == NULL && r2_user_func(fn) == NULL
                    && (strstr(fn, "UNSIGNED") || strstr(fn, "SIGNED")
                        || strstr(fn, "TO_INTEGER")
                        || strstr(fn, "unsigned")
                        || strstr(fn, "to_integer"))));
         if (!ident)
            break;
         if (strcasecmp(base, "l3d_resize_s") == 0
             || (strstr(fn, "SIGNED") && !strstr(fn, "UNSIGNED")))
            *sgn = true;
         e = tree_value(tree_param(e, 0));
      }
   }
}

// Dynamic-index WRITE lowering, shared by the single-bit write `t(i) <= b`
// and the OOB_WriteV part-select idiom `t(i*W +: W) <= v`: a switch on the
// index with one arm per reachable position v (a multiple of `stride`,
// below 2**iw — a signed index is never negative inside an arm), each a
// constant slice action on the hold temp
//    g0[v + n - 1 : v] = src[n - 1 : 0]      (n = min(W, width - v))
// — read_verilog's own lowering of a dynamic lvalue range (simplify.cc
// replaces it by a case over the positions).  Being case actions, any
// number of such writes to one target compose in statement order, in any
// arms; an out-of-range index matches no arm and writes nothing, which is
// what the translated OOB guards do.  `src` is a plain wire name when
// W > 1 (sliced in the partial arms); any 1-bit sigspec when W == 1.
static bool r2_index_switch(tree_t idx, const char *g0, int width, int W,
                            const char *src, int stride, bool sgn)
{
   char is[R2_SPEC];
   if (!r2_expr(idx, is, sizeof is))
      return false;
   const int iw = r2_rendered_width(idx);
   if (iw <= 0 || iw > 4096) {
      R2_DECLINE("dyn-idx-width");
      return false;
   }
   // the switch signal at exactly iw bits (a concat / cell temp lands on a
   // wire; the compare constants are sized to it)
   char sw[64];
   if (!r2_temp(iw, sw, sizeof sw) || g_r2->connect(sw, is) != 0) {
      R2_DECLINE("dyn-idx-land");
      return false;
   }
   int64_t vmax = width - 1;
   const int ub = sgn ? iw - 1 : iw;
   if (ub < 62 && (((int64_t)1 << ub) - 1) < vmax)
      vmax = ((int64_t)1 << ub) - 1;
   if (stride < 1)
      stride = 1;
   if (vmax / stride + 1 > 1024) {
      R2_DECLINE("dyn-arms");
      return false;
   }
   if (g_r2->switch_begin(sw) != 0) {
      R2_DECLINE("api-switch");
      return false;
   }
   for (int64_t v = 0; v <= vmax; v += stride) {
      char cmp[48], lhs[128], rhs[R2_SPEC];
      snprintf(cmp, sizeof cmp, "%d'd%lld", iw, (long long)v);
      const int n = (W < width - (int)v) ? W : width - (int)v;
      if (n == 1)
         snprintf(lhs, sizeof lhs, "%s[%lld]", g0, (long long)v);
      else
         snprintf(lhs, sizeof lhs, "%s[%lld:%lld]", g0,
                  (long long)(v + n - 1), (long long)v);
      if (n == W)
         snprintf(rhs, sizeof rhs, "%s", src);
      else if (n == 1)
         snprintf(rhs, sizeof rhs, "%s[0]", src);
      else
         snprintf(rhs, sizeof rhs, "%s[%d:0]", src, n - 1);
      if (g_r2->case_begin(cmp) != 0 || g_r2->case_assign(lhs, rhs) != 0
          || g_r2->case_end() != 0) {
         R2_DECLINE("api-case-assign");
         return false;
      }
   }
   if (g_r2->switch_end() != 0) {
      R2_DECLINE("api-switch");
      return false;
   }
   return true;
}

// AND-chain of the open path conditions; false = stack too deep/none
// representable.  Empty stack yields out[0] == '\0' (unconditional).
static bool r2_path_cond(char *out, size_t sz)
{
   if (g_r2_nconds == 0) {
      out[0] = '\0';
      return true;
   }
   char cur[96];
   snprintf(cur, sizeof cur, "%s", g_r2_conds[0]);
   for (int i = 1; i < g_r2_nconds; i++) {
      char t[64], cn[80];
      if (!r2_temp(1, t, sizeof t))
         return false;
      snprintf(cn, sizeof cn, "c%s", t);
      if (g_r2->cell_bin("and", cn, cur, g_r2_conds[i], t, 0) != 0)
         return false;
      snprintf(cur, sizeof cur, "%s", t);
   }
   snprintf(out, sz, "%s", cur);
   return true;
}

// Static width of a rendered sigspec, -1 if underivable: bare wires are
// unknowable (the caller must know), but "w[h:l]"/"w[i]" forms, sized
// literals, and concats of those all classify.
static int r2_spec_width(const char *sp)
{
   if (sp == NULL || sp[0] == '\0')
      return -1;
   if (sp[0] == '{') {
      int total = 0;
      const char *q = sp + 1;
      while (*q && *q != '}') {
         char el[256];
         int d2 = 0;
         size_t n = 0;
         while (*q && n + 1 < sizeof el) {
            if (*q == '{') d2++;
            else if (*q == '}') { if (d2 == 0) break; d2--; }
            else if (*q == ',' && d2 == 0) break;
            el[n++] = *q++;
         }
         el[n] = '\0';
         const int ew = r2_spec_width(el);
         if (ew < 0)
            return -1;
         total += ew;
         if (*q == ',')
            q++;
      }
      return total > 0 ? total : -1;
   }
   if (isdigit((unsigned char)sp[0])) {
      const char *tick = strchr(sp, 39);
      return tick != NULL ? atoi(sp) : -1;
   }
   const char *br = strchr(sp, '[');
   if (br != NULL) {
      int h2, l2;
      if (sscanf(br, "[%d:%d]", &h2, &l2) == 2)
         return h2 - l2 + 1;
      if (sscanf(br, "[%d]", &h2) == 1)
         return 1;
      return -1;
   }
   return -1;   // bare wire: width not in the string
}

// VERSIONED write to a process-local: new wire vN+1 =
// $mux(pathcond, newvalue, vN) at module level.  `whole` spans the var;
// otherwise [hi:lo] of the previous version is replaced (composed via
// concat).  The subst entry's spec becomes the new wire (bare name, so
// element reads index it directly).  Requires an existing whole-value
// spec (a read-before-first-write local is latch state — pvar handles
// those).
static bool r2_var_write(ident_t vi, int w, const char *value,
                         bool whole, int hi, int lo)
{
   r2_subst_t *e = r2_subst_of(vi);
   if (e == NULL || e->spec == NULL || w < 1 || w > 4000) {
      R2_DECLINE("var-version-base");
      return false;
   }
   // depth at which the value BEING MERGED WITH is valid: a mux write below
   // (cond ? new : prev) is valid at PREV's scope, so its version escapes the
   // arm-exit poison and survives to the enclosing scope (SSA if-merge)
   const int prev_wd = e->wdepth;
   // slicing needs a WIRE base: materialize literal/compose specs once
   bool bare = true;
   for (const char *q = e->spec; *q && bare; q++)
      if (!isalnum((unsigned char)*q) && *q != '_' && *q != '$')
         bare = false;
   if (!bare) {
      // connect throws (and poisons the session) on width mismatch —
      // materialize only when the spec's width derives statically AND
      // matches the declaration
      // an integer substitution renders as 32'dK regardless of the decl
      // width — re-render at the declaration width when the value fits
      char lit[48];
      const char *msrc = e->spec;
      int64_t mival = e->has_ival ? e->ival : -1;
      // a single-bit var holding an l3d enum POSITION (folded_int on enum
      // literals yields positions: '0'=2, '1'=3, weak 6/7): value plane
      if (w == 1 && mival >= 2 && mival <= 7)
         mival &= 1;
      if (e->has_ival && mival >= 0
          && (w >= 63 || mival < ((int64_t)1 << w))) {
         snprintf(lit, sizeof lit, "%d'd%lld", w, (long long)mival);
         msrc = lit;
      }
      else if (r2_spec_width(e->spec) != w) {
         char why[96];
         snprintf(why, sizeof why, "var-version-spec w%d '%.24s'",
                  w, e->spec);
         R2_DECLINE(why);
         return false;
      }
      char mt[64];
      if (!r2_temp(w, mt, sizeof mt) || g_r2->connect(mt, msrc) != 0) {
         R2_DECLINE("var-version-mat");
         return false;
      }
      if (!r2_subst_set(vi, mt))
         return false;
      e = r2_subst_of(vi);
   }
   char nv[R2_SPEC];
   if (whole)
      snprintf(nv, sizeof nv, "%s", value);
   else {
      // compose: {prev[w-1:hi+1], value, prev[lo-1:0]}
      size_t off = 0;
      nv[off++] = '{';
      if (hi < w - 1)
         off += snprintf(nv + off, sizeof nv - off, "%s[%d:%d],",
                         e->spec, w - 1, hi + 1);
      off += snprintf(nv + off, sizeof nv - off, "%s", value);
      if (lo > 0)
         off += snprintf(nv + off, sizeof nv - off, ",%s[%d:0]",
                         e->spec, lo - 1);
      if (off + 2 >= sizeof nv) {
         R2_DECLINE("var-version-size");
         return false;
      }
      nv[off++] = '}';
      nv[off] = '\0';
   }
   char pc[96];
   if (!r2_path_cond(pc, sizeof pc)) {
      R2_DECLINE("var-version-cond");
      return false;
   }
   char t[64];
   if (!r2_temp(w, t, sizeof t))
      return false;
   bool ok;
   if (pc[0] == '\0')
      ok = g_r2->connect(t, nv) == 0;
   else {
      char cn[80];
      snprintf(cn, sizeof cn, "c%s", t);
      ok = g_r2->cell_mux(cn, e->spec, nv, pc, t) == 0;
   }
   if (!ok) {
      R2_DECLINE("var-version-emit");
      return false;
   }
   if (!r2_subst_set(vi, t))
      return false;
   // a MERGE write (t = mux(pathcond, new, prev)) is valid where prev was, so
   // stamp it at prev's depth instead of this arm's — r2_subst_poison_from then
   // keeps it live past the arm, giving a correct read-after-branch of a
   // branch-written local (SSA if-merge: loop-carried found-flags, if/elsif/else
   // merges).  A straight-line write (pathcond empty) keeps the current depth.
   if (pc[0] != '\0')
      r2_subst_of(vi)->wdepth = prev_wd;
   return true;
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

// hold temp of a promoted process variable (defined with the process-body
// machinery below); NULL when the variable is not promoted
static const char *r2_pvar_g0(ident_t id);
static int g_r2_tree_seq;   // top-level switch counter (a "tree")

// Re-size a sized literal spec ("N'dV" / "N'b...") to `w` bits, value
// plane preserved (truncate high / zero-extend); false for any other spec.
static bool r2_lit_resize(const char *spec, int w, char *out, size_t sz)
{
   const char *tick = strchr(spec, 39);
   if (tick == NULL || w < 1)
      return false;
   if (tick[1] == 'd') {
      if (w > 63)
         return false;
      const uint64_t v = strtoull(tick + 2, NULL, 10);
      snprintf(out, sz, "%d'd%llu", w,
               (unsigned long long)(v & ((1ULL << w) - 1)));
      return true;
   }
   if (tick[1] == 'b') {
      const char *dig = tick + 2;
      const int nd = (int)strlen(dig);
      if ((size_t)w + 8 > sz)
         return false;
      char *p = out + snprintf(out, sz, "%d'b", w);
      for (int i = w - 1; i >= 0; i--)
         *p++ = i < nd ? dig[nd - 1 - i] : '0';
      *p = '\0';
      return true;
   }
   return false;
}

// Structural equality of two expression trees (refs, literals, calls,
// element selects, conversions) — enough to recognise the same index
// expression on both bounds of a dynamic part-select.
static bool r2_tree_same(tree_t a, tree_t b)
{
   if (a == b)
      return true;
   if (a == NULL || b == NULL || tree_kind(a) != tree_kind(b))
      return false;
   switch (tree_kind(a)) {
   case T_REF:
      return tree_ident(a) == tree_ident(b);
   case T_LITERAL:
      {
         int64_t x, y;
         return folded_int(a, &x) && folded_int(b, &y) && x == y;
      }
   case T_FCALL:
   case T_ARRAY_REF:
      {
         if (tree_kind(a) == T_FCALL
             ? tree_ident(a) != tree_ident(b)
             : !r2_tree_same(tree_value(a), tree_value(b)))
            return false;
         const int np = tree_params(a);
         if (np != tree_params(b))
            return false;
         for (int i = 0; i < np; i++)
            if (!r2_tree_same(tree_value(tree_param(a, i)),
                              tree_value(tree_param(b, i))))
               return false;
         return true;
      }
   case T_TYPE_CONV:
   case T_QUALIFIED:
      return r2_tree_same(tree_value(a), tree_value(b));
   default:
      return false;
   }
}

// A dynamic slice range `b + K downto b` (or `b to b + K`): tgt-vhdl's
// rendering of the indexed part-select `[b +: K+1]` once the scaled index
// sits in a temporary.  Yields the base-index expression and K.
static bool r2_dyn_slice(tree_t r, tree_t *lo_e, int64_t *k)
{
   const bool to = tree_subkind(r) == RANGE_TO;
   tree_t hi = to ? tree_right(r) : tree_left(r);
   tree_t lo = to ? tree_left(r) : tree_right(r);
   if (tree_kind(hi) != T_FCALL || tree_params(hi) != 2
       || strcmp(istr(tree_ident(hi)), "\"+\"") != 0)
      return false;
   tree_t p0 = tree_value(tree_param(hi, 0));
   tree_t p1 = tree_value(tree_param(hi, 1));
   *lo_e = lo;
   if ((folded_int(p1, k) || r2_eval_int(p1, k)) && r2_tree_same(p0, lo))
      return *k >= 0;
   if ((folded_int(p0, k) || r2_eval_int(p0, k)) && r2_tree_same(p1, lo))
      return *k >= 0;
   return false;
}

// ---- array-of-vector WIRE selects ---------------------------------------
// A non-memory array signal/port is ONE flat wire of type_width bits laid
// out as NVC flattens it (leftmost element highest).  A constant selection
// chain s(i)(j downto k) / s(i)(b) / s(a downto b) resolves to a bit range
// of the root wire; a dynamic OUTERMOST select over a constant word
// materialises the word and reuses the plain-vector lowerings (shr + [0],
// shr + [k:0]).

static bool r2_sel_index(tree_t ie, int64_t *v)
{
   return folded_int(ie, v) || r2_eval_int(ie, v);
}

static bool r2_sel_range(tree_t e, tree_t *root, int64_t *off, int64_t *w)
{
   tree_t chain[8];
   int n = 0;
   tree_t t = e;
   while (tree_kind(t) == T_ARRAY_REF || tree_kind(t) == T_ARRAY_SLICE) {
      if (n >= 8)
         return false;
      chain[n++] = t;
      t = tree_value(t);
   }
   if (n == 0 || tree_kind(t) != T_REF || !tree_has_ref(t))
      return false;
   tree_t d = tree_ref(t);
   if (tree_kind(d) != T_SIGNAL_DECL && tree_kind(d) != T_PORT_DECL)
      return false;
   type_t ty = tree_type(d);
   if (!type_const_bounds(ty))
      return false;
   int64_t o = 0, width = type_width(ty);
   for (int i = n - 1; i >= 0; i--) {
      tree_t s = chain[i];
      if (!type_is_array(ty) || dimension_of(ty) != 1)
         return false;
      int64_t low, high;
      if (!folded_bounds(range_of(ty, 0), &low, &high))
         return false;
      type_t et = type_elem(ty);
      const int64_t ew = type_is_array(et)
         ? (type_const_bounds(et) ? (int64_t)type_width(et) : -1) : 1;
      if (ew < 1)
         return false;
      const bool desc = direction_of(ty, 0) == RANGE_DOWNTO;
      if (tree_kind(s) == T_ARRAY_REF) {
         int64_t idx;
         if (tree_params(s) != 1
             || !r2_sel_index(tree_value(tree_param(s, 0)), &idx)
             || idx < low || idx > high)
            return false;
         o += (desc ? idx - low : high - idx) * ew;
         width = ew;
         ty = et;
      }
      else {
         tree_t r = tree_range(s, 0);
         int64_t left, right;
         if (!r2_sel_index(tree_left(r), &left)
             || !r2_sel_index(tree_right(r), &right))
            return false;
         const int64_t shi = left > right ? left : right;
         const int64_t slo = left > right ? right : left;
         if (slo < low || shi > high)
            return false;
         o += (desc ? slo - low : high - shi) * ew;
         width = (shi - slo + 1) * ew;
         if (i > 0)
            return false;   // a select INSIDE a slice: not needed yet
      }
   }
   *root = t;
   *off = o;
   *w = width;
   return true;
}

// Does this select belong to the flat-wire-array lowering?  Nested chains
// rooted at a signal/port, or an element select of an array-of-vector
// signal/port; memories keep their $memrd/$memwr paths, locals theirs.
static bool r2_sel_nested(tree_t e)
{
   const tree_kind_t k = tree_kind(e);
   if (k != T_ARRAY_REF && k != T_ARRAY_SLICE)
      return false;
   tree_t b = tree_value(e);
   const tree_kind_t bk = tree_kind(b);
   if (bk == T_ARRAY_REF || bk == T_ARRAY_SLICE) {
      tree_t r = b;
      while (tree_kind(r) == T_ARRAY_REF || tree_kind(r) == T_ARRAY_SLICE)
         r = tree_value(r);
      if (tree_kind(r) != T_REF || !tree_has_ref(r))
         return false;
      const tree_kind_t dk = tree_kind(tree_ref(r));
      return (dk == T_SIGNAL_DECL || dk == T_PORT_DECL)
         && r2_mem_of(tree_ident(r)) == NULL;
   }
   if (bk != T_REF || !tree_has_ref(b))
      return false;
   tree_t d = tree_ref(b);
   if ((tree_kind(d) != T_SIGNAL_DECL && tree_kind(d) != T_PORT_DECL)
       || r2_mem_of(tree_ident(b)) != NULL)
      return false;
   type_t ty = tree_type(d);
   return type_is_array(ty) && type_const_bounds(ty)
      && type_is_array(type_elem(ty));
}

static bool r2_sel_chain_expr(tree_t e, char *out, size_t sz)
{
   tree_t root;
   int64_t off, w;
   if (r2_sel_range(e, &root, &off, &w)) {
      if (w == 1)
         snprintf(out, sz, "%s[%lld]", vid(tree_ident(root)),
                  (long long)off);
      else
         snprintf(out, sz, "%s[%lld:%lld]", vid(tree_ident(root)),
                  (long long)(off + w - 1), (long long)off);
      return true;
   }
   // dynamic outermost select over a constant word
   tree_t base = tree_value(e);
   if (!r2_sel_range(base, &root, &off, &w) || w < 1 || w > 4000) {
      R2_DECLINE("sel-chain");
      return false;
   }
   char wt[R2_SPEC], ws[R2_SPEC];
   if (!r2_temp((int)w, wt, sizeof wt))
      return false;
   snprintf(ws, sizeof ws, "%s[%lld:%lld]", vid(tree_ident(root)),
            (long long)(off + w - 1), (long long)off);
   if (g_r2->connect(wt, ws) != 0) {
      R2_DECLINE("sel-word");
      return false;
   }
   char is[R2_SPEC], dt[R2_SPEC], cn[R2_SPEC + 8];
   int64_t k = 0;
   if (tree_kind(e) == T_ARRAY_REF) {
      if (tree_params(e) != 1
          || !r2_expr(tree_value(tree_param(e, 0)), is, sizeof is))
         return false;
   }
   else {
      tree_t lo_e = NULL;
      if (!r2_dyn_slice(tree_range(e, 0), &lo_e, &k) || k >= w) {
         R2_DECLINE("sel-dyn-slice");
         return false;
      }
      if (!r2_expr(lo_e, is, sizeof is))
         return false;
   }
   if (!r2_temp((int)w, dt, sizeof dt))
      return false;
   snprintf(cn, sizeof cn, "c%s", dt);
   if (g_r2->cell_bin("shr", cn, wt, is, dt, 0) != 0) {
      R2_DECLINE("sel-shr");
      return false;
   }
   if (tree_kind(e) == T_ARRAY_REF)
      snprintf(out, sz, "%s[0]", dt);
   else
      snprintf(out, sz, "%s[%lld:0]", dt, (long long)k);
   return true;
}

// Memory-shaped signal used as a WIRE ARRAY: every indexed reference has
// a constant index (generate constants, the literal word numbers of a
// carry-save tree).  It stays one flat wire; element selects are ranges.
typedef struct { tree_t decl; int idx, cidx; } r2_warr_scan_t;

static void r2_warr_scan_cb(tree_t t, void *ctx)
{
   r2_warr_scan_t *sc = (r2_warr_scan_t *)ctx;
   if (tree_kind(t) != T_ARRAY_REF)
      return;
   tree_t b = tree_value(t);
   if (tree_kind(b) != T_REF || !tree_has_ref(b) || tree_ref(b) != sc->decl)
      return;
   sc->idx++;
   int64_t v;
   if (tree_params(t) == 1 && r2_sel_index(tree_value(tree_param(t, 0)), &v))
      sc->cidx++;
}

static bool r2_wire_array(tree_t block, tree_t d)
{
   r2_warr_scan_t sc = { .decl = d, .idx = 0, .cidx = 0 };
   for (int si = 0; si < tree_stmts(block); si++)
      tree_visit(tree_stmt(block, si), r2_warr_scan_cb, &sc);
   return sc.idx > 0 && sc.idx == sc.cidx;
}

// VHDL "&" concatenation.  A left-nested chain ((a & b) & c) & ... is
// flattened ITERATIVELY: the partial-product rows of the wallace
// multiplier chain 700 operands, and one r2_expr frame per operand (tens
// of KB of sigspec buffers each) overflowed the stack.  A rendered chain
// wider than one sigspec buffer lands in temp wires chunk by chunk.
// The DECLARED width of an expression where its rendering is
// self-determined: a width-taking conversion's width argument, a scalar
// logic3d's 1, a constrained type's width; -1 when unknown
static int r2_decl_width(tree_t e)
{
   if (tree_kind(e) == T_FCALL && tree_params(e) == 2) {
      const char *b = id_base(istr(tree_ident(e)));
      if (strcasecmp(b, "to_l3d") == 0 || strcasecmp(b, "resize") == 0
          || strcasecmp(b, "to_unsigned") == 0
          || strcasecmp(b, "to_signed") == 0
          || strcasecmp(b, "l3d_resize_s") == 0) {
         int64_t nw;
         tree_t we = tree_value(tree_param(e, 1));
         if ((folded_int(we, &nw) || r2_eval_int(we, &nw)) && nw >= 1
             && nw <= 4096)
            return (int)nw;
         return -1;
      }
   }
   if (tree_kind(e) == T_QUALIFIED || tree_kind(e) == T_TYPE_CONV)
      return r2_decl_width(tree_value(e));
   type_t t = tree_type(e);
   if (type_is_array(t))
      return type_const_bounds(t) ? (int)type_width(t) : -1;
   return type_is_logic3d(t) ? 1 : -1;   // type_is_logic3d is true of the
                                          // vector's element type as well
}

// a non-constant aggregate whose every association is positional/concat
// (elaboration folds `a & b & c` into one): a concatenation
static bool r2_agg_is_concat(tree_t t)
{
   const int n = tree_assocs(t);
   for (int i = 0; i < n; i++) {
      const assoc_kind_t sk = tree_subkind(tree_assoc(t, i));
      if (sk != A_CONCAT && sk != A_POS)
         return false;
   }
   char tmp[R2_SPEC];
   return !r2_const(t, tmp, sizeof tmp, -1);   // constants stay literals
}

static bool r2_concat_chain(tree_t e, char *out, size_t sz)
{
   int cap = 64, n = 0, scap = 64, sn = 0;
   tree_t *leaf = xmalloc_array(cap, sizeof(tree_t));
   tree_t *stk = xmalloc_array(scap, sizeof(tree_t));
   stk[sn++] = e;
   while (sn > 0) {
      tree_t t = stk[--sn];
      if (tree_kind(t) == T_AGGREGATE && tree_assocs(t) > 0
          && r2_agg_is_concat(t)) {
         // elaboration's folded `&` chain: first assoc = MSB end
         const int na = tree_assocs(t);
         while (sn + na > scap) {
            scap *= 2;
            stk = xrealloc_array(stk, scap, sizeof(tree_t));
         }
         for (int i = na - 1; i >= 0; i--)
            stk[sn++] = tree_value(tree_assoc(t, i));
         continue;
      }
      if (tree_kind(t) == T_FCALL && tree_params(t) == 2
          && strcmp(istr(tree_ident(t)), "\"&\"") == 0) {
         if (sn + 2 > scap) {
            scap *= 2;
            stk = xrealloc_array(stk, scap, sizeof(tree_t));
         }
         stk[sn++] = tree_value(tree_param(t, 1));   // popped second
         stk[sn++] = tree_value(tree_param(t, 0));
         continue;
      }
      if (n == cap) {
         cap *= 2;
         leaf = xrealloc_array(leaf, cap, sizeof(tree_t));
      }
      leaf[n++] = t;
   }
   free(stk);

   char **ls = xmalloc_array(n, sizeof(char *));
   int *lw = xmalloc_array(n, sizeof(int));
   bool ok = true;
   size_t total = 0;
   for (int i = 0; i < n; i++)
      ls[i] = NULL;
   for (int i = 0; ok && i < n; i++) {
      char buf[R2_SPEC];
      ok = r2_expr(leaf[i], buf, sizeof buf);
      if (ok) {
         lw[i] = r2_width_or_operands(leaf[i]);
         if (lw[i] < 1)
            lw[i] = r2_spec_width(buf);
         if (lw[i] < 1 && !type_is_array(tree_type(leaf[i]))
             && type_is_logic3d(tree_type(leaf[i])))
            lw[i] = 1;   // a scalar local (the multiplier's and-terms)
         // a concat element is SELF-determined: an identity conversion
         // renders its operand verbatim, so `to_l3d(x, 8)` or
         // `unsigned_to_l3d_bit(u)` would contribute width(x) bits here.
         // Land it at its declared width (the builder fits: zero-extend
         // or low bits -- test/accel/l3did, l3dwrap)
         const int dw = r2_decl_width(leaf[i]);
         if (dw >= 1 && dw != lw[i]) {
            char t[64];
            if (!r2_temp(dw, t, sizeof t) || g_r2->connect(t, buf) != 0) {
               ok = false;
               break;
            }
            snprintf(buf, sizeof buf, "%s", t);
            lw[i] = dw;
         }
         ls[i] = xstrdup(buf);
         total += strlen(buf) + 1;
      }
   }
   if (ok && total + 2 <= sz) {
      char *p = out;
      *p++ = '{';
      for (int i = 0; i < n; i++) {
         if (i > 0)
            *p++ = ',';
         const size_t l = strlen(ls[i]);
         memcpy(p, ls[i], l);
         p += l;
      }
      *p++ = '}';
      *p = '\0';
   }
   else if (ok) {
      // every chunk's concat fits one buffer; its width is the sum of the
      // leaf widths, which must all be known
      char chunks[R2_SPEC];
      size_t cl = 0;
      chunks[cl++] = '{';
      int i = 0;
      while (ok && i < n) {
         char cb[R2_SPEC];
         size_t bl = 0;
         int cw = 0, j = i;
         cb[bl++] = '{';
         while (j < n && bl + strlen(ls[j]) + 3 < sizeof cb - 8) {
            if (lw[j] < 1) {
               ok = false;
               break;
            }
            if (j > i)
               cb[bl++] = ',';
            const size_t l = strlen(ls[j]);
            memcpy(cb + bl, ls[j], l);
            bl += l;
            cw += lw[j];
            j++;
         }
         if (!ok || j == i) {
            ok = false;
            break;
         }
         cb[bl++] = '}';
         cb[bl] = '\0';
         char t[R2_SPEC];
         if (!r2_temp(cw, t, sizeof t) || g_r2->connect(t, cb) != 0) {
            ok = false;
            break;
         }
         const size_t tl = strlen(t);
         if (cl + tl + 3 >= sizeof chunks) {
            ok = false;
            break;
         }
         if (cl > 1)
            chunks[cl++] = ',';
         memcpy(chunks + cl, t, tl);
         cl += tl;
         i = j;
      }
      if (ok) {
         chunks[cl++] = '}';
         chunks[cl] = '\0';
         snprintf(out, sz, "%s", chunks);
      }
      else
         R2_DECLINE("concat-chain");
   }
   for (int i = 0; i < n; i++)
      free(ls[i]);
   free(ls);
   free(lw);
   free(leaf);
   return ok;
}

static bool r2_expr_1(tree_t e, char *out, size_t sz);

// depth guard: one r2_expr frame carries several sigspec buffers (tens of
// KB); a pathological nesting declines instead of overflowing the stack
static int g_r2_expr_depth;

static bool r2_expr(tree_t e, char *out, size_t sz)
{
   if (g_r2_expr_depth >= 200) {
      R2_DECLINE("expr-depth");
      return false;
   }
   g_r2_expr_depth++;
   const bool ok = r2_expr_1(e, out, sz);
   g_r2_expr_depth--;
   return ok;
}

// Serve a CONSTANT-index element read of a process-local variable from its
// live SSA substitution version — the same source a whole-variable read
// consults.  The !have_idx block in r2_expr_1's T_ARRAY_REF case already does
// this for loop-substituted indices; this is the folded-constant counterpart,
// so `v(1)` AFTER a write to `v` resolves instead of declining "var-elem".
// Returns true and fills `out` when served.  Returns false with *hard=false
// when the variable has NO live version (genuine read-before-write: the caller
// MUST fall through to the promote-on-read latch path).  Returns false with
// *hard=true after a DECLINE that must NOT fall through: a fall-through would
// call r2_pvar_read_or_promote, whose ts->npv++ side effect creates a spurious
// pvar that then poisons the NEXT statement's per-bit write gate (the
// var-assign@var-part cascade).  Raw idx is the bit index, correct only for a
// downto range and a 1-bit value-plane element (both hold for std_logic_vector
// and logic3d_vector under --accel); anything else declines rather than risk a
// silent wrong bit.
static bool r2_subst_elem_read(tree_t base, int64_t idx,
                               char *out, size_t sz, bool *hard)
{
   *hard = false;
   if (!tree_has_ref(base))
      return false;
   r2_subst_t *sb = r2_subst_of(tree_ident(base));
   if (sb == NULL)
      return false;   // never written: genuine read-before-write -> promote/latch
   if (sb->spec == NULL && sb->bw == 0) {
      // WRITTEN then poisoned (a branch merge the walker cannot represent as a
      // flat version, e.g. a partial write under an `if` with no covering
      // else): the promote-on-read fallback would install a PERSISTENT pv wire
      // that is never seeded with the earlier writes -> installs silently WRONG
      // (the pre-existing branch-var pv defect).  Decline to the text path,
      // which lowers the same shape correctly.
      *hard = true; R2_DECLINE("var-elem-poison"); return false;
   }
   // flat-wire element offset computed EXACTLY as r2_sel_range does for
   // signal/port selects, so a non-zero lower bound maps correctly (bit
   // off = (idx - low) for a downto vector) and an ascending range is
   // rejected rather than mirror-indexed.  Element width in the --accel value
   // plane is 1 bit for a scalar-element vector (std_logic_vector AND
   // logic3d_vector); >1 only for a 2-D array-of-vector local.
   type_t vt = tree_type(tree_ref(base));
   if (!type_is_array(vt) || dimension_of(vt) != 1 || !type_const_bounds(vt)) {
      *hard = true; R2_DECLINE("var-elem-ty"); return false;
   }
   int64_t low, high;
   if (!folded_bounds(range_of(vt, 0), &low, &high)) {
      *hard = true; R2_DECLINE("var-elem-bounds"); return false;
   }
   if (direction_of(vt, 0) != RANGE_DOWNTO) {
      // ascending (`to`) locals: the write and text paths mis-index them, so
      // keep the walker off the shape rather than risk a mirror-bit read
      *hard = true; R2_DECLINE("var-elem-to"); return false;
   }
   if (idx < low || idx > high) {
      *hard = true; R2_DECLINE("var-elem-oob"); return false;
   }
   const int64_t bidx = idx - low;   // 0-based element position in the flat wire
   type_t et = type_elem(vt);
   const int64_t ew = type_is_array(et)
      ? (type_const_bounds(et) ? (int64_t)type_width(et) : -1) : 1;
   if (ew < 1) { *hard = true; R2_DECLINE("var-elem-ew"); return false; }
   // per-bit build: bits[] hold one value-bit per element (scalar element)
   if (sb->bw > 0) {
      if (ew != 1 || bidx >= sb->bw) {
         *hard = true; R2_DECLINE("var-elem-bitw"); return false;
      }
      if (sb->bits[bidx] == NULL) {
         *hard = true; R2_DECLINE("var-elem-bit"); return false;
      }
      snprintf(out, sz, "%s", sb->bits[bidx]);
      return true;
   }
   // whole/versioned spec: index it.  A bare wire name indexes directly;
   // otherwise only a sized literal whose prefix width equals the decl width
   // may be landed on a temp (a width-mismatched connect throws inside the gsm
   // child and poisons it).
   bool bare = sb->spec[0] != '\0';
   for (const char *q = sb->spec; *q && bare; q++)
      if (!isalnum((unsigned char)*q) && *q != '_' && *q != '$')
         bare = false;
   const char *S = sb->spec;
   char t2[64];
   if (!bare) {
      const int bw2 = (int)type_width(vt);
      const bool litw = isdigit((unsigned char)sb->spec[0])
         && atoi(sb->spec) == bw2;
      if (!(litw && bw2 >= 1 && bw2 <= 4000)) {
         *hard = true; R2_DECLINE("var-elem-spec"); return false;
      }
      if (!(r2_temp(bw2, t2, sizeof t2) && g_r2->connect(t2, sb->spec) == 0)) {
         *hard = true; R2_DECLINE("var-elem-connect"); return false;
      }
      S = t2;
   }
   if (ew == 1)
      snprintf(out, sz, "%s[%lld]", S, (long long)bidx);
   else
      snprintf(out, sz, "%s[%lld:%lld]", S,
               (long long)(bidx * ew + ew - 1), (long long)(bidx * ew));
   return true;
}

static bool r2_expr_1(tree_t e, char *out, size_t sz)
{
   if (g_r2_fail > 0 && !g_r2_census)
      return false;

   switch (tree_kind(e)) {
   case T_REF:
      {
         if (r2_char_meta(e)) {
            R2_DECLINE("metavalue");
            return false;
         }
         {  r2_subst_t *sb = r2_subst_of(tree_ident(e));
            if (sb != NULL && sb->has_ival) {
               // a constant-valued local renders at the VARIABLE's (or
               // bound formal's) declared width when it is known — two's
               // complement, so a negative value (sv2v_cast_5(all-ones),
               // which the const interpreter sign-extends) has a sigspec
               // form; an integer variable stays at the translator's 32
               const int w = r2_local_width(e);
               if (w >= 1 && w <= 63) {
                  const uint64_t m = (w == 63) ? (~0ULL >> 1)
                                              : ((1ULL << w) - 1);
                  snprintf(out, sz, "%d'd%llu", w,
                           (unsigned long long)((uint64_t)sb->ival & m));
                  return true;
               }
            }
            if (sb != NULL && sb->spec != NULL) {
               snprintf(out, sz, "%s", sb->spec);
               return true;
            }
            if (sb != NULL && sb->bw > 0) {
               if (r2_subst_compose(sb, out, sz))
                  return true;
               R2_DECLINE("bits-partial");
               return false;
            }
         }
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
         if (d != NULL && tree_kind(d) == T_VAR_DECL) {
            // an NBA-shadow alias read BEFORE any write is the signal's
            // pre-edge value — exactly what the pre-copy bound it to
            r2_alias_t *al = r2_alias_of(tree_ident(e));
            if (al != NULL && !al->wrote) {
               snprintf(out, sz, "%s", vid(al->sig));
               return true;
            }
            // a PROMOTED latch var read before any write this activation:
            // the persistent pv wire IS the activation-start value
            {
               const char *pv2 = r2_pvar_read_pv(tree_ident(e));
               if (pv2 != NULL) {
                  snprintf(out, sz, "%s", pv2);
                  return true;
               }
            }
            // otherwise: no wire exists for a local — rendering the bare
            // name poisons the gsm session; decline cleanly
            char why[96];
            snprintf(why, sizeof why, "var-read %s",
                     istr(tree_ident(e)));
            if (getenv("NVC_RTLIL_DEBUG") != NULL) {
               fprintf(stderr, "r2 var-read '%s' decl '%s' depth %d subst:",
                       istr(tree_ident(e)), istr(tree_ident(d)),
                       g_r2_inline_depth);
               for (int i = 0; i < g_r2_nsubst; i++)
                  fprintf(stderr, " %s=%s", istr(g_r2_subst[i].var),
                          g_r2_subst[i].spec ? g_r2_subst[i].spec : "-");
               fputc('\n', stderr);
            }
            R2_DECLINE(why);
            return false;
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
         if (concat)   // same flattener as `&`: elements at declared width
            return r2_concat_chain(e, out, sz);
         if (tree_assocs(e) == 1
             && tree_subkind(tree_assoc(e, 0)) == A_OTHERS) {
            // (others => x) with NON-constant x: replication.  The sigspec
            // grammar has no {n{x}} form, so expand to a literal concat of
            // total/element copies (multi-bit elements replicate whole).
            const int w = r2_width(e);
            tree_t v0 = tree_value(tree_assoc(e, 0));
            const int wel = r2_width(v0);
            char el[R2_SPEC];
            if (w > 0 && wel >= 1 && w % wel == 0 && w / wel <= 256
                && r2_expr(v0, el, sizeof el)) {
               const int reps = w / wel;
               const size_t elen = strlen(el);
               if ((elen + 1) * (size_t)reps + 3 < sz) {
                  size_t len = 0;
                  out[len++] = '{';
                  for (int i = 0; i < reps; i++) {
                     if (i > 0)
                        out[len++] = ',';
                     memcpy(out + len, el, elen);
                     len += elen;
                  }
                  out[len++] = '}';
                  out[len] = '\0';
                  return true;
               }
            }
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
         if (r2_sel_nested(e))
            return r2_sel_chain_expr(e, out, sz);
         if (tree_kind(base) != T_REF) {
            R2_DECLINE("slice-base");
            return false;
         }
         tree_t r = tree_range(e, 0);
         int64_t left, right;
         if ((!folded_int(tree_left(r), &left)
              && !r2_eval_int(tree_left(r), &left))
             || (!folded_int(tree_right(r), &right)
                 && !r2_eval_int(tree_right(r), &right))) {
            // DYNAMIC part-select READ: `sig(x + K downto x)` — the bounds
            // are dynamic but their span is constant.  Recognize left as
            // right + K structurally (render both sub-expressions and
            // compare the sigspecs) and lower to shr + a low slice, the
            // l3d_part_read lowering for direct slicing.
            tree_t lt = tree_left(r), rt = tree_right(r);
            int64_t span = -1;
            char rs2[R2_SPEC];
            if (tree_kind(lt) == T_FCALL && tree_params(lt) == 2
                && strcmp(istr(tree_ident(lt)), "\"+\"") == 0
                && (folded_int(tree_value(tree_param(lt, 1)), &span)
                    || r2_eval_int(tree_value(tree_param(lt, 1)), &span))
                && span > 0 && span < 4000) {
               char ls2[R2_SPEC];
               if (r2_expr(tree_value(tree_param(lt, 0)), ls2, sizeof ls2)
                   && r2_expr(rt, rs2, sizeof rs2)
                   && strcmp(ls2, rs2) == 0) {
                  tree_t bd = tree_has_ref(base) ? tree_ref(base) : NULL;
                  type_t bt = bd != NULL ? tree_type(bd) : NULL;
                  const int bw = (bt != NULL && type_const_bounds(bt))
                     ? (int)type_width(bt) : -1;
                  if (bw >= 1 && bw <= 4000
                      && (tree_kind(bd) == T_SIGNAL_DECL
                          || tree_kind(bd) == T_PORT_DECL)) {
                     char bspec[R2_SPEC], dt[R2_SPEC], cn[R2_SPEC + 8];
                     if (!r2_expr(base, bspec, sizeof bspec))
                        return false;
                     if (!r2_temp(bw, dt, sizeof dt))
                        return false;
                     snprintf(cn, sizeof cn, "c%s", dt);
                     if (g_r2->cell_bin("shr", cn, bspec, rs2, dt, 0) != 0) {
                        R2_DECLINE("part-shr");
                        return false;
                     }
                     snprintf(out, sz, "%s[%lld:0]", dt, (long long)span);
                     return true;
                  }
               }
            }
            // origin structural x+K did not match: r2_dyn_slice fallback
            // dynamic part-select `a(b + K downto b)` (the lane selects
            // `alu_in1(To_Integer(t) + 31 downto To_Integer(t))`): the
            // l3d_part_read lowering (a >> b)[K:0] — what yosys makes of
            // the text path's `a[b+K:b]`
            tree_t lo_e = NULL;
            int64_t k = -1;
            if (r2_dyn_slice(r, &lo_e, &k)) {
               char bs[R2_SPEC], ls[R2_SPEC];
               if (!r2_expr(base, bs, sizeof bs)
                   || !r2_expr(lo_e, ls, sizeof ls))
                  return false;
               const int bw = r2_width_or_operands(base);
               if (bw <= 0 || k >= bw) {
                  R2_DECLINE("dyn-slice-width");
                  return false;
               }
               char t[R2_SPEC], cn[R2_SPEC + 8];
               if (!r2_temp(bw, t, sizeof t))
                  return false;
               snprintf(cn, sizeof cn, "c%s", t);
               if (g_r2->cell_bin("shr", cn, bs, ls, t, 0) != 0) {
                  R2_DECLINE("dyn-slice-shr");
                  return false;
               }
               snprintf(out, sz, "%s[%lld:0]", t, (long long)k);
               return true;
            }
            char why[48];
            snprintf(why, sizeof why, "slice-bounds k%d/k%d",
                     (int)tree_kind(tree_left(r)),
                     (int)tree_kind(tree_right(r)));
            R2_DECLINE(why);
            return false;
         }
         if (left < 0 || right < 0) {
            R2_DECLINE("slice-neg");
            return false;
         }
         const int64_t hi = left > right ? left : right;
         const int64_t lo = left > right ? right : left;
         {
            // resolve the base like any reference: a substituted local
            // slices its CURRENT VERSION wire; an unresolved local has no
            // wire and must decline (bare names poison the session)
            r2_subst_t *sb2 = r2_subst_of(tree_ident(base));
            if (sb2 != NULL && sb2->spec != NULL) {
               bool bare2 = true;
               for (const char *q = sb2->spec; *q && bare2; q++)
                  if (!isalnum((unsigned char)*q) && *q != '_' && *q != '$')
                     bare2 = false;
               if (!bare2) {
                  R2_DECLINE("slice-subst");
                  return false;
               }
               snprintf(out, sz, "%s[%lld:%lld]", sb2->spec,
                        (long long)hi, (long long)lo);
               return true;
            }
            if (tree_has_ref(base)
                && tree_kind(tree_ref(base)) == T_VAR_DECL) {
               char why[96];
               snprintf(why, sizeof why, "var-slice %s",
                        istr(tree_ident(base)));
               R2_DECLINE(why);
               return false;
            }
         }
         snprintf(out, sz, "%s[%lld:%lld]", vid(tree_ident(base)),
                  (long long)hi, (long long)lo);
         return true;
      }

   case T_ARRAY_REF:
      {
         tree_t base = tree_value(e);
         int64_t idx;
         if (r2_sel_nested(e))
            return r2_sel_chain_expr(e, out, sz);
         if (tree_kind(base) == T_REF && tree_params(e) == 1) {
            r2_mem_t *mm = r2_mem_of(tree_ident(base));
            if (mm != NULL) {
               // memory read: an async $memrd port with the index as ADDR
               char as[R2_SPEC];
               if (!r2_expr(tree_value(tree_param(e, 0)), as, sizeof as))
                  return false;
               char dt[R2_SPEC], cn[R2_SPEC + 8];
               if (!r2_temp(mm->width, dt, sizeof dt))
                  return false;
               snprintf(cn, sizeof cn, "m%s", dt);
               if (g_r2->memrd(cn, mm->vname, as, dt) != 0) {
                  R2_DECLINE("memrd");
                  return false;
               }
               snprintf(out, sz, "%s", dt);
               return true;
            }
         }
         bool have_idx = tree_kind(base) == T_REF && tree_params(e) == 1
            && folded_int(tree_value(tree_param(e, 0)), &idx);
         if (!have_idx && tree_kind(base) == T_REF && tree_params(e) == 1) {
            int64_t bidx;
            r2_subst_t *sb = r2_subst_of(tree_ident(base));
            if (sb != NULL && sb->bw > 0
                && r2_eval_int(tree_value(tree_param(e, 0)), &bidx)
                && bidx >= 0 && bidx < sb->bw) {
               if (sb->bits[bidx] == NULL) {
                  R2_DECLINE("bits-unset");
                  return false;
               }
               snprintf(out, sz, "%s", sb->bits[bidx]);
               return true;
            }
            // element read of a WHOLE-substituted var: index the spec — a
            // bare wire name directly, anything else via a landed temp
            if (sb != NULL && sb->spec != NULL
                && r2_eval_int(tree_value(tree_param(e, 0)), &bidx)
                && bidx >= 0) {
               bool bare = sb->spec[0] != '\0';
               for (const char *q = sb->spec; *q && bare; q++)
                  if (!isalnum((unsigned char)*q) && *q != '_' && *q != '$')
                     bare = false;
               if (bare) {
                  snprintf(out, sz, "%s[%lld]", sb->spec, (long long)bidx);
                  return true;
               }
               tree_t bd = tree_has_ref(base) ? tree_ref(base) : NULL;
               type_t bt = bd != NULL ? tree_type(bd) : NULL;
               const int bw2 = (bt != NULL && type_const_bounds(bt))
                  ? (int)type_width(bt) : -1;
               // temp-connect needs EQUAL widths (RTLIL connections throw
               // on mismatch and poison the session): only a sized literal
               // whose prefix width matches the decl is provably safe here
               const bool litw = isdigit((unsigned char)sb->spec[0])
                  && atoi(sb->spec) == bw2;
               if (litw && bw2 >= 1 && bw2 <= 4000 && bidx < bw2) {
                  char t2[64];
                  if (r2_temp(bw2, t2, sizeof t2)
                      && g_r2->connect(t2, sb->spec) == 0) {
                     snprintf(out, sz, "%s[%lld]", t2, (long long)bidx);
                     return true;
                  }
                  R2_DECLINE("spec-elem-connect");
                  return false;
               }
               R2_DECLINE("spec-elem");
               return false;
            }
         }
         if (!have_idx && tree_kind(base) == T_REF && tree_params(e) == 1) {
            // arithmetic over substituted loop indices evaluates directly
            int64_t ev;
            if (r2_eval_int(tree_value(tree_param(e, 0)), &ev) && ev >= 0) {
               idx = ev;
               have_idx = true;
            }
         }
         if (!have_idx && tree_kind(base) == T_REF && tree_params(e) == 1) {
            // a substituted loop index folds through its sigspec ("N'dK")
            char is[R2_SPEC];
            if (r2_expr(tree_value(tree_param(e, 0)), is, sizeof is)) {
               const char *tick = strchr(is, 39);
               if (tick != NULL && tick[1] == 'd') {
                  idx = atoll(tick + 2);
                  have_idx = true;
               }
            }
         }
         if (!have_idx && tree_kind(base) == T_REF && tree_params(e) == 1) {
            // DYNAMIC single-bit read of a plain vector: shr + [0], the
            // same lowering l3d_bit_read gets (the base is not a memory —
            // that path returned above)
            tree_t bd = tree_has_ref(base) ? tree_ref(base) : NULL;
            type_t bt = bd != NULL ? tree_type(bd) : NULL;
            const int bw = (bt != NULL && type_const_bounds(bt))
               ? (int)type_width(bt) : -1;
            char is2[R2_SPEC];
            if (bw >= 1 && bw <= 4000
                && (tree_kind(bd) == T_SIGNAL_DECL
                    || tree_kind(bd) == T_PORT_DECL)
                && r2_expr(tree_value(tree_param(e, 0)), is2, sizeof is2)) {
               char dt[R2_SPEC], cn[R2_SPEC + 8], bspec[R2_SPEC];
               if (!r2_expr(base, bspec, sizeof bspec))
                  return false;
               if (!r2_temp(bw, dt, sizeof dt))
                  return false;
               snprintf(cn, sizeof cn, "c%s", dt);
               if (g_r2->cell_bin("shr", cn, bspec, is2, dt, 0) != 0)
                  return false;
               snprintf(out, sz, "%s[0]", dt);
               return true;
            }
         }
         if (!have_idx) {
            R2_DECLINE("array-ref");
            return false;
         }
         {
            // resolve the BASE like any reference: an NBA-shadow alias
            // reads its signal; an unresolved local variable has NO wire
            // (the bare name would poison the gsm session)
            ident_t bi = tree_ident(base);
            r2_alias_t *al = r2_alias_of(bi);
            if (al != NULL && !al->wrote)
               bi = al->sig;   // pure pre-copy so far: the signal IS it
            else if (al != NULL) {
               char why[96];
               snprintf(why, sizeof why, "alias-raw %s", istr(bi));
               R2_DECLINE(why);
               return false;
            }
            else if (tree_has_ref(base)
                     && tree_kind(tree_ref(base)) == T_VAR_DECL) {
               // a written local's element read is served from its live SSA
               // version (the folded-constant mirror of the !have_idx block
               // above); only a GENUINE read-before-write falls through to the
               // promote-on-read latch path below
               {
                  bool hard = false;
                  if (r2_subst_elem_read(base, idx, out, sz, &hard))
                     return true;
                  if (hard)
                     return false;
               }
               // read-before-any-write of a local: promote to latch state
               // and read the persistent pv wire (activation-start value)
               const char *pv3 = NULL;
               {
                  extern const char *r2_pvar_read_or_promote(tree_t);
                  pv3 = r2_pvar_read_or_promote(tree_ref(base));
               }
               if (pv3 != NULL) {
                  snprintf(out, sz, "%s[%lld]", pv3, (long long)idx);
                  return true;
               }
               {
                  tree_t bd2 = tree_ref(base);
                  type_t bt2 = tree_type(bd2);
                  char why[112];
                  snprintf(why, sizeof why, "var-elem %s cb%d w%d",
                           istr(bi), (int)type_const_bounds(bt2),
                           type_const_bounds(bt2) ? (int)type_width(bt2)
                                                  : -1);
                  R2_DECLINE(why);
               }
               return false;
            }
            snprintf(out, sz, "%s[%lld]", vid(bi), (long long)idx);
         }
         return true;
      }

   case T_FCALL:
      {
         const char *fn = istr(tree_ident(e));
         const int np = tree_params(e);

         // transparent numeric_std/library identities (same set the text
         // path prints verbatim); never a user body that happens to carry
         // one of the names (VX_gpu_pkg_inst_alu_signed)
         if (np >= 1 && (strstr(fn, "UNSIGNED") || strstr(fn, "SIGNED")
                         || strstr(fn, "STD_LOGIC_VECTOR")
                         || strstr(fn, "TO_STDLOGICVECTOR")
                         || strstr(fn, "TO_INTEGER")
                         || strstr(fn, "unsigned")
                         || strstr(fn, "to_integer")
                         || strstr(fn, "std_logic_vector"))
             && vlog_op(fn) == NULL && np == 1 && r2_user_func(fn) == NULL)
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

         // l3d vocabulary — the text path's own table (kind 0 binary,
         // 1 unary, 2 identity, 3 reduction)
         {
            int lk = -1;
            const char *lop = vlog_l3d_op(fn, &lk);
            const char *base = id_base(fn);
            // l3d_shcount(a) is to_integer on the value plane (its 2**20
            // saturation only matters for counts past any operand width,
            // where $shl/$shr already yield zero)
            // l3d_resize_s(a, N): SIGNED resize on the value plane — the
            // text path's `$signed(a)` in an N-bit context.  Widening is a
            // $pos cell with A signed (sign extension: the LSU's LB/LH
            // response `sv2v_cast_32_signed(l3d_resize_s(rsp_data8, 32))`,
            // the multiplier's 33 -> 66-bit operands); narrowing takes the
            // low bits; equal width is the operand.  An identity here would
            // let the builder ZERO-extend the assignment.
            if (lop == NULL && np == 2 && strcasecmp(base, "l3d_resize_s") == 0) {
               int64_t nw;
               tree_t ea = tree_value(tree_param(e, 0));
               char a[R2_SPEC];
               if (!r2_expr(ea, a, sizeof a))
                  return false;
               const int aw = r2_rendered_width(ea);
               if (!folded_int(tree_value(tree_param(e, 1)), &nw)
                   && !r2_eval_int(tree_value(tree_param(e, 1)), &nw)) {
                  R2_DECLINE("resize_s-width");
                  return false;
               }
               if (aw <= 0 || nw <= 0) {
                  R2_DECLINE("resize_s-operand");
                  return false;
               }
               if (nw == aw) {
                  snprintf(out, sz, "%s", a);
                  return true;
               }
               char y[R2_SPEC], cn[R2_SPEC + 8];
               if (nw > aw) {
                  if (!r2_temp((int)nw, y, sizeof y))
                     return false;
                  snprintf(cn, sizeof cn, "c%s", y);
                  if (g_r2->cell_un("pos", cn, a, y, 1) != 0) {
                     R2_DECLINE("resize_s-sext");
                     return false;
                  }
                  snprintf(out, sz, "%s", y);
                  return true;
               }
               // narrowing: land and take the low bits
               if (!r2_temp(aw, y, sizeof y) || g_r2->connect(y, a) != 0) {
                  R2_DECLINE("resize_s-land");
                  return false;
               }
               if (nw == 1)
                  snprintf(out, sz, "%s[0]", y);
               else
                  snprintf(out, sz, "%s[%lld:0]", y, (long long)(nw - 1));
               return true;
            }
            if (lop == NULL && np >= 1
                && (strcasecmp(base, "l3d_index") == 0
                    || strcasecmp(base, "l3d_shcount") == 0))
               return r2_expr(tree_value(tree_param(e, 0)), out, sz);
            if (lop != NULL && lk == 2 && np >= 1)
               return r2_expr(tree_value(tree_param(e, 0)), out, sz);
            if (lop != NULL && lk == 1 && np == 1) {
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
               if (g_r2->cell_un("not", cn, a, y, 0) != 0) {
                  R2_DECLINE("l3d-not");
                  return false;
               }
               snprintf(out, sz, "%s", y);
               return true;
            }
            if (lop != NULL && lk == 3 && np == 1) {
               const char *rop = strcmp(lop, "|") == 0 ? "reduce_or"
                  : strcmp(lop, "&") == 0 ? "reduce_and" : "reduce_xor";
               char a[R2_SPEC];
               if (!r2_expr(tree_value(tree_param(e, 0)), a, sizeof a))
                  return false;
               char y[R2_SPEC], cn[R2_SPEC + 8];
               if (!r2_temp(1, y, sizeof y))
                  return false;
               snprintf(cn, sizeof cn, "c%s", y);
               if (g_r2->cell_un(rop, cn, a, y, 0) != 0) {
                  R2_DECLINE("l3d-reduce");
                  return false;
               }
               snprintf(out, sz, "%s", y);
               return true;
            }
            if (lop != NULL && lk == 0 && np == 2) {
               const char *bop = strcmp(lop, "&") == 0 ? "and"
                  : strcmp(lop, "|") == 0 ? "or"
                  : strcmp(lop, "^") == 0 ? "xor"
                  : strcmp(lop, "~^") == 0 ? "xnor"
                  : strcmp(lop, "==") == 0 ? "eq"
                  : strcmp(lop, "!=") == 0 ? "ne" : NULL;
               if (bop == NULL) {
                  R2_DECLINE("l3d-binop");
                  return false;
               }
               tree_t ea = tree_value(tree_param(e, 0));
               tree_t eb = tree_value(tree_param(e, 1));
               char a[R2_SPEC], b[R2_SPEC];
               if (!r2_expr(ea, a, sizeof a) || !r2_expr(eb, b, sizeof b))
                  return false;
               int w = r2_is_onebit_op(bop) ? 1 : r2_width(e);
               if (w <= 0)
                  w = r2_width_or_operands(ea);
               if (w <= 0)
                  w = r2_width_or_operands(eb);
               char y[R2_SPEC], cn[R2_SPEC + 8];
               if (!r2_temp(w, y, sizeof y))
                  return false;
               snprintf(cn, sizeof cn, "c%s", y);
               if (g_r2->cell_bin(bop, cn, a, b, y, 0) != 0) {
                  R2_DECLINE("l3d-bin");
                  return false;
               }
               snprintf(out, sz, "%s", y);
               return true;
            }
            // signed relationals (l3d_lt_s family)
            if (np == 2 && strncasecmp(base, "l3d_", 4) == 0) {
               const char *rel = strcasecmp(base, "l3d_lt_s") == 0 ? "lt"
                  : strcasecmp(base, "l3d_gt_s") == 0 ? "gt"
                  : strcasecmp(base, "l3d_le_s") == 0 ? "le"
                  : strcasecmp(base, "l3d_ge_s") == 0 ? "ge" : NULL;
               if (rel != NULL) {
                  char a[R2_SPEC], b[R2_SPEC];
                  if (!r2_expr(tree_value(tree_param(e, 0)), a, sizeof a)
                      || !r2_expr(tree_value(tree_param(e, 1)), b, sizeof b))
                     return false;
                  char y[R2_SPEC], cn[R2_SPEC + 8];
                  if (!r2_temp(1, y, sizeof y))
                     return false;
                  snprintf(cn, sizeof cn, "c%s", y);
                  if (g_r2->cell_bin(rel, cn, a, b, y, 1) != 0) {
                     R2_DECLINE("l3d-rel");
                     return false;
                  }
                  snprintf(out, sz, "%s", y);
                  return true;
               }
            }
            // l3d_bit_read(a, i): (a >> i)[0]
            if (np == 2 && strcasecmp(base, "l3d_bit_read") == 0) {
               char a[R2_SPEC], b[R2_SPEC];
               tree_t ea = tree_value(tree_param(e, 0));
               if (!r2_expr(ea, a, sizeof a)
                   || !r2_expr(tree_value(tree_param(e, 1)), b, sizeof b))
                  return false;
               // the operand may be an unconstrained operator result
               // (`a + b` of two resized lane indices): take the width
               // from the operand chain, as the binop path does
               const int aw = r2_width_or_operands(ea);
               if (aw <= 0) {
                  R2_DECLINE("bitread-width");
                  return false;
               }
               char t[R2_SPEC], cn[R2_SPEC + 8];
               if (!r2_temp(aw, t, sizeof t))
                  return false;
               snprintf(cn, sizeof cn, "c%s", t);
               if (g_r2->cell_bin("shr", cn, a, b, t, 0) != 0) {
                  R2_DECLINE("bitread-shr");
                  return false;
               }
               snprintf(out, sz, "%s[0]", t);
               return true;
            }
            // l3d_sra(a, n): arithmetic shift right — $sshr with A signed
            // (the text path's `$signed(a) >>> n`); the count stays
            // unsigned (addSshr sets B_SIGNED=false)
            if (np == 2 && strcasecmp(base, "l3d_sra") == 0) {
               char a[R2_SPEC], b[R2_SPEC];
               tree_t ea = tree_value(tree_param(e, 0));
               if (!r2_expr(ea, a, sizeof a)
                   || !r2_expr(tree_value(tree_param(e, 1)), b, sizeof b))
                  return false;
               const int aw = r2_width_or_operands(ea);
               if (aw <= 0) {
                  R2_DECLINE("sra-width");
                  return false;
               }
               char y[R2_SPEC], cn[R2_SPEC + 8];
               if (!r2_temp(aw, y, sizeof y))
                  return false;
               snprintf(cn, sizeof cn, "c%s", y);
               if (g_r2->cell_bin("sshr", cn, a, b, y, 1) != 0) {
                  R2_DECLINE("l3d-sra");
                  return false;
               }
               snprintf(out, sz, "%s", y);
               return true;
            }
            // l3d_part_read(a, b, W): (a >> b)[W-1:0]
            if (np >= 3 && strcasecmp(base, "l3d_part_read") == 0) {
               int64_t pw;
               if (!folded_int(tree_value(tree_param(e, 2)), &pw) || pw <= 0) {
                  R2_DECLINE("partread-w");
                  return false;
               }
               char a[R2_SPEC], b[R2_SPEC];
               tree_t ea = tree_value(tree_param(e, 0));
               if (!r2_expr(ea, a, sizeof a)
                   || !r2_expr(tree_value(tree_param(e, 1)), b, sizeof b))
                  return false;
               const int aw = r2_width(ea);
               if (aw <= 0 || pw > aw) {
                  R2_DECLINE("partread-width");
                  return false;
               }
               char t[R2_SPEC], cn[R2_SPEC + 8];
               if (!r2_temp(aw, t, sizeof t))
                  return false;
               snprintf(cn, sizeof cn, "c%s", t);
               if (g_r2->cell_bin("shr", cn, a, b, t, 0) != 0) {
                  R2_DECLINE("partread-shr");
                  return false;
               }
               snprintf(out, sz, "%s[%d:0]", t, (int)pw - 1);
               return true;
            }
            // ternary_*(T, X, Y) -> mux: T ? X : Y
            if (np == 3 && strncasecmp(base, "ternary_", 8) == 0) {
               char t[R2_SPEC], x[R2_SPEC], yv[R2_SPEC];
               if (!r2_expr(tree_value(tree_param(e, 0)), t, sizeof t)
                   || !r2_expr(tree_value(tree_param(e, 1)), x, sizeof x)
                   || !r2_expr(tree_value(tree_param(e, 2)), yv, sizeof yv))
                  return false;
               int w = r2_width(e);
               if (w <= 0)   // unconstrained arms: concat / operator chains
                  w = r2_width_or_operands(tree_value(tree_param(e, 1)));
               if (w <= 0)
                  w = r2_width_or_operands(tree_value(tree_param(e, 2)));
               char y[R2_SPEC], cn[R2_SPEC + 8];
               if (!r2_temp(w, y, sizeof y))
                  return false;
               snprintf(cn, sizeof cn, "c%s", y);
               if (g_r2->cell_mux(cn, yv, x, t, y) != 0) {
                  R2_DECLINE("ternary");
                  return false;
               }
               snprintf(out, sz, "%s", y);
               return true;
            }
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
         if (fn[0] == '"' && strcmp(fn, "\"&\"") == 0)
            return r2_concat_chain(e, out, sz);   // concatenation {a, b, ..}
         if (vop != NULL && np == 2) {
            const char *bop = r2_binop(vop);
            if (bop == NULL) {
               char why[48];
               snprintf(why, sizeof why, "binop %s", vop);
               R2_DECLINE(why);
               return false;
            }
            tree_t ea = tree_value(tree_param(e, 0));
            tree_t eb = tree_value(tree_param(e, 1));
            // `x = 'Z'` / `x /= 'Z'` against a std_logic CHARACTER
            // metavalue: tgt-vhdl's casez expansion `((sel(0) = 'Z') or
            // (sel(0) = '1')) and ...` tests a 2-state selector, which is
            // never Z — the test is a constant (the text path declines the
            // literal outright)
            if ((strcmp(bop, "eq") == 0 || strcmp(bop, "ne") == 0)
                && (r2_char_meta(ea) || r2_char_meta(eb))) {
               snprintf(out, sz, "1'b%d", strcmp(bop, "ne") == 0);
               return true;
            }
            char a[R2_SPEC], b[R2_SPEC];
            if (!r2_expr(ea, a, sizeof a) || !r2_expr(eb, b, sizeof b))
               return false;
            int w = r2_is_onebit_op(bop) ? 1 : r2_width(e);
            if (w <= 0) {
               // operator returns are unconstrained: Verilog's context
               // width — the WIDER operand for + - * & | ^ (a 1-bit lane
               // index times a 32-bit constant is a 32-bit product), the
               // left operand for shifts
               const bool shift = strcmp(bop, "shl") == 0
                  || strcmp(bop, "shr") == 0;
               w = r2_width_or_operands(ea);
               const int wb = shift ? -1 : r2_width_or_operands(eb);
               if (wb > w)
                  w = wb;
            }
            char y[R2_SPEC], cn[R2_SPEC + 8];
            if (!r2_temp(w, y, sizeof y))
               return false;
            // numeric_std SIGNED, or VHDL INTEGER: the translator declares
            // every integer `signed [31:0]`, and the OOB range guards
            // compare an index against NEGATIVE bounds (`Idx >= -113`)
            const int sg = type_is_signed(tree_type(ea))
               || type_is_signed(tree_type(eb))
               || type_is_integer(tree_type(ea))
               || type_is_integer(tree_type(eb));
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
               char why[48];
               snprintf(why, sizeof why, "unop %s", vop);
               R2_DECLINE(why);
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
         if (g_r2_ncfn > 0 && r2_cfn_of(fn) != NULL) {
            // constant-evaluable design function: fold the call
            if (r2_const(e, out, sz, -1))
               return true;
            R2_DECLINE("cfn-eval");
            return false;
         }
         // user-function inlining: bind params as substitutions, walk the
         // straight-line body, render the return value.  The whole table
         // is snapshotted around the call — generated code reuses local
         // names (i, j) across scopes, and bindings must not leak.
         if (g_r2_inline_depth < R2_INLINE_MAX) {
            tree_t fb = r2_user_func(fn);
            if (fb != NULL && tree_ports(fb) == np && np <= 8) {
               // every actual renders in the CALLER's environment before
               // any formal is bound: a formal may share its name with a
               // caller local that a later actual reads (nested inlining
               // reuses `inp`/`pc` across the package bodies)
               struct { bool isint; int64_t iv; char *spec; int w; } act[8];
               bool iok = true;
               for (int i = 0; i < np; i++) {
                  act[i].spec = NULL;
                  act[i].isint = false;
                  act[i].w = -1;
               }
               for (int i = 0; iok && i < np; i++) {
                  tree_t av = tree_value(tree_param(e, i));
                  if (r2_eval_int(av, &act[i].iv))
                     act[i].isint = true;
                  else {
                     char as[R2_SPEC];
                     iok = r2_expr(av, as, sizeof as);
                     if (iok) {
                        act[i].spec = xstrdup(as);
                        // the rendered width: a slice/element read of the
                        // formal lands the spec on a wire of that width
                        act[i].w = r2_width_or_operands(av);
                     }
                  }
                  if (getenv("NVC_RTLIL_DEBUG") != NULL)
                     fprintf(stderr, "r2 inline %s actual %d: kind %d isint %d iv %lld spec %s w %d\n",
                             id_base(fn), i, (int)tree_kind(av), act[i].isint,
                             (long long)act[i].iv, act[i].spec ? act[i].spec : "-", act[i].w);
               }
               r2_subst_snap_t sn;
               r2_subst_save(&sn);
               g_r2_inline_depth++;
               for (int i = 0; iok && i < np; i++) {
                  ident_t pi = tree_ident(tree_port(fb, i));
                  iok = act[i].isint ? r2_subst_set_int(pi, act[i].iv)
                                     : r2_subst_set_w(pi, act[i].spec,
                                                      act[i].w);
               }
               for (int i = 0; i < np; i++)
                  free(act[i].spec);
               // locals written PER-BIT compose into a concat on first
               // whole read (f_Enc8to3 builds its 3-bit result that way)
               struct { ident_t var; int w; char *b[64]; } bm[4];
               int nbm = 0;
               const int nfs = tree_stmts(fb);
               for (int i = 0; iok && i < nfs; i++) {
                  tree_t st = tree_stmt(fb, i);
                  if (tree_kind(st) == T_NULL)
                     continue;
                  tree_t vv = NULL, ft = NULL;
                  if (tree_kind(st) == T_RETURN)
                     vv = tree_value(st);
                  else {
                     vv = tree_value(st);
                     ft = tree_target(st);
                  }
                  // compose a bit-mapped var read {b[w-1],...,b[0]}
                  char comp[R2_SPEC];
                  const char *vspec = NULL;
                  if (tree_kind(vv) == T_REF) {
                     for (int m = 0; m < nbm; m++)
                        if (bm[m].var == tree_ident(vv)) {
                           size_t cl = 0;
                           comp[cl++] = '{';
                           for (int bi = bm[m].w - 1; bi >= 0; bi--) {
                              if (bm[m].b[bi] == NULL
                                  || cl + strlen(bm[m].b[bi]) + 3 >= sizeof comp)
                                 { iok = false; break; }
                              if (bi != bm[m].w - 1)
                                 comp[cl++] = ',';
                              memcpy(comp + cl, bm[m].b[bi],
                                     strlen(bm[m].b[bi]));
                              cl += strlen(bm[m].b[bi]);
                           }
                           comp[cl++] = '}';
                           comp[cl] = '\0';
                           vspec = comp;
                           break;
                        }
                  }
                  if (!iok)
                     break;
                  if (tree_kind(st) == T_RETURN) {
                     if (vspec != NULL) {
                        snprintf(out, sz, "%s", vspec);
                        iok = true;
                     }
                     else
                        iok = r2_expr(vv, out, sz);
                     break;
                  }
                  if (ft != NULL && tree_kind(ft) == T_ARRAY_REF) {
                     // per-bit write: record the bit's spec
                     int64_t bidx;
                     ident_t bv = tree_ident(tree_value(ft));
                     if (!r2_eval_int(tree_value(tree_param(ft, 0)), &bidx)
                         || bidx < 0 || bidx >= 64) {
                        iok = false;
                        break;
                     }
                     int m;
                     for (m = 0; m < nbm; m++)
                        if (bm[m].var == bv)
                           break;
                     if (m == nbm) {
                        if (nbm >= 4) { iok = false; break; }
                        tree_t bd = tree_ref(tree_value(ft));
                        type_t bt = tree_type(bd);
                        const int bw = type_const_bounds(bt)
                           ? (int)type_width(bt) : -1;
                        if (bw < 1 || bw > 64) { iok = false; break; }
                        bm[nbm].var = bv;
                        bm[nbm].w = bw;
                        memset(bm[nbm].b, 0, sizeof bm[nbm].b);
                        nbm++;
                     }
                     if (bidx >= bm[m].w) { iok = false; break; }
                     char bs[R2_SPEC];
                     int64_t bcv;
                     if (r2_eval_int(vv, &bcv))
                        snprintf(bs, sizeof bs, "1'd%d", bcv ? 1 : 0);
                     else if (!r2_expr(vv, bs, sizeof bs)) {
                        iok = false;
                        break;
                     }
                     free(bm[m].b[bidx]);
                     bm[m].b[bidx] = xstrdup(bs);
                     continue;
                  }
                  // whole var-assign
                  ident_t ti = tree_ident(ft);
                  int64_t cv;
                  if (vspec != NULL)
                     iok = r2_subst_set(ti, vspec);
                  else if (r2_eval_int(vv, &cv))
                     iok = r2_subst_set_int(ti, cv);
                  else {
                     char vs[R2_SPEC];
                     iok = r2_expr(vv, vs, sizeof vs)
                        && r2_subst_set(ti, vs);
                  }
               }
               for (int m = 0; m < nbm; m++)
                  for (int bi = 0; bi < 64; bi++)
                     free(bm[m].b[bi]);
               g_r2_inline_depth--;
               r2_subst_restore(&sn);
               if (iok)
                  return true;
               return false;
            }
         }
         {
            char why[96];
            snprintf(why, sizeof why, "fcall:%s d%d np%d", id_base(fn),
                     g_r2_inline_depth, np);
            R2_DECLINE(why);
         }
         return false;
      }

   default:
      {
         char why[48];
         snprintf(why, sizeof why, "expr-kind %d", (int)tree_kind(e));
         R2_DECLINE(why);
      }
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

// backing store for the per-process target table: the giant decode/tlu
// control modules carry thousands of tmp_ivl deposit targets — far past
// any sane stack array (this is ~700KB, static in the fork child)
#define R2_MAX_TARGETS 4096
static r2_target_t g_r2_tgt[R2_MAX_TARGETS];

typedef struct { char memid[80]; char *addr; char *data; char en[32]; }
   r2_memwr_t;

// promoted persistent variables (VHDL process vars keep their value across
// activations — a branch-written one is LATCH state): pv wire + hold temp,
// committed on the process sync; proc then infers the $dlatch exactly as
// read_verilog would
typedef struct { ident_t var; char pv[80]; char g0[80]; int width;
                 bool persistent;
                 bool wrote;   // a write happened this walk: a later read
                               // must see it — pv (activation-start) reads
                               // are only sound BEFORE any write
} r2_pvar_t;

typedef struct {
   r2_target_t *t;        // points at g_r2_tgt (walker is child-single-
                          // threaded; one process walks at a time)
   int         pidx;      // owning process index: scopes g0/pv wire names
                          // (two processes driving one signal must not
                          // collide on wire g0_<sig>)
   bool        capped;    // collection hit the cap: target-miss is a LIE
   int         n;
   r2_memwr_t  mw[64];   // pending memory writes, flushed after the edge
   int         nmw;      //  sync exists (addr/data are heap copies)
   int         nsites;   // enable-temp counter
   r2_pvar_t   pv[16];   // promoted persistent variables (latch state)
   int         npv;
   ident_t     dynwr[16];   // targets already dyn-composed this process
   int         ndynwr;      //  (the compose reads the PRE value = the sig)
   bool        comb;     // process kind: only comb processes may promote
} r2_targets_t;

static r2_pvar_t *r2_pvar_of(r2_targets_t *ts, ident_t id)
{
   for (int i = 0; i < ts->npv; i++)
      if (ts->pv[i].var == id)
         return &ts->pv[i];
   return NULL;
}

// the process being walked (its promoted variables are visible to the
// expression walker through r2_pvar_g0); NULL outside r2_process
static r2_targets_t *g_r2_pvts;

static const char *r2_pvar_g0(ident_t id)
{
   r2_pvar_t *pe = g_r2_pvts != NULL ? r2_pvar_of(g_r2_pvts, id) : NULL;
   return pe != NULL ? pe->g0 : NULL;
}

// Promote a branch-written process variable to latch state: a persistent
// pv wire, a hold temp rooted at pv (or at the last straight-line value,
// which makes it a plain temp, not a latch).
static r2_pvar_t *r2_pvar_promote(r2_targets_t *ts, tree_t vdecl)
{
   const ident_t id = tree_ident(vdecl);
   r2_pvar_t *e = r2_pvar_of(ts, id);
   if (e != NULL)
      return e;
   if (ts->npv >= 16)
      return NULL;
   type_t ty = tree_type(vdecl);
   if (!type_const_bounds(ty))
      return NULL;
   const int w = (int)type_width(ty);
   e = &ts->pv[ts->npv];
   e->var = id;
   e->width = w;
   snprintf(e->pv, sizeof e->pv, "pv%d_%s", ts->pidx, vid(id));
   snprintf(e->g0, sizeof e->g0, "g0pv%d_%s", ts->pidx, vid(id));
   r2_subst_t *sb = r2_subst_of(id);
   if (g_r2->wire(e->g0, w, 0, NULL) != 0)
      return NULL;
   if (sb != NULL && sb->spec != NULL) {
      // written straight-line earlier this activation: hold base is that
      // value — a plain tree temp, nothing persists
      e->persistent = false;
      e->wrote = false;
      if (g_r2->case_assign_root(e->g0, sb->spec) != 0)
         return NULL;
   }
   else {
      e->persistent = true;
      e->wrote = false;
      if (g_r2->wire(e->pv, w, 0, NULL) != 0
          || g_r2->case_assign_root(e->g0, e->pv) != 0)
         return NULL;
   }
   ts->npv++;
   return e;
}

   // current process targets (pvar reads)

// promote-on-READ: a local read before any write carries the previous
// activation's value (latch semantics) — ensure a pv/hold pair exists and
// return the pv wire, or NULL when promotion is unavailable (clocked
// process, capacity, unconstrained width, already written this walk).
const char *r2_pvar_read_or_promote(tree_t vdecl)
{
   if (g_r2_pvts == NULL)
      return NULL;
   r2_pvar_t *pe = r2_pvar_of(g_r2_pvts, tree_ident(vdecl));
   if (pe == NULL) {
      pe = r2_pvar_promote(g_r2_pvts, vdecl);
      // seed the substitution so later writes VERSION from the pv wire
      // and the end-of-walk commit (sync_assign pv <= final version)
      // closes the register loop
      if (pe != NULL && pe->persistent)
         r2_subst_set(tree_ident(vdecl), pe->pv);
   }
   if (pe != NULL && pe->persistent && !pe->wrote)
      return pe->pv;
   return NULL;
}

static const char *r2_pvar_read_pv(ident_t id)
{
   if (g_r2_pvts == NULL)
      return NULL;
   r2_pvar_t *pe = r2_pvar_of(g_r2_pvts, id);
   if (pe != NULL && pe->persistent && !pe->wrote)
      return pe->pv;
   return NULL;
}

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
   if (tree_kind(t) != T_SIGNAL_ASSIGN && tree_kind(t) != T_DEPOSIT)
      return;
   tree_t tg = tree_target(t);
   // a slice/indexed target contributes its BASE signal (the hold temp
   // covers the whole vector; the branch assigns into a slice of it)
   while (tree_kind(tg) == T_ARRAY_SLICE || tree_kind(tg) == T_ARRAY_REF)
      tg = tree_value(tg);
   if (tree_kind(tg) != T_REF || !tree_has_ref(tg))
      return;
   if (r2_mem_of(tree_ident(tg)) != NULL)   // memories write via memwr
      return;
   if (ts->n >= R2_MAX_TARGETS) {
      ts->capped = true;
      return;
   }
   ident_t id = tree_ident(tg);
   if (r2_target(ts, id) != NULL)
      return;
   r2_target_t *n = &ts->t[ts->n++];
   n->name = id;
   snprintf(n->spec, sizeof n->spec, "%s", vid(id));
   snprintf(n->g0, sizeof n->g0, "g0p%d_%s", ts->pidx, vid(id));
   type_t ty = tree_type(tree_ref(tg));
   n->width = type_const_bounds(ty) ? (int)type_width(ty) : -1;
}

static bool r2_seq(tree_t list_of, r2_targets_t *ts);

// tgt-vhdl's OOB_WriteV idiom — the dynamic part-select write
// `t[i*W +: W] = v` rendered as
//    Tmp := v;  Idx := l3d_index(i * W);
//    if (Idx >= -(W-1)) and (Idx <= hi) then
//       for P in 0 to W-1 loop
//          if ((Idx + P) >= 0) and ((Idx + P) <= hi) then
//             t(Idx + P) <= Tmp(P);
//          end if;
//       end loop;
//    end if;
// The FOR is matched here (the outer guard and any enclosing counting
// loop are the ordinary paths: statically pruned when Idx folds, a switch
// otherwise).  A constant Idx is ONE slice action on the hold temp; a
// dynamic one is an index switch with a slice action per reachable
// position (stride W when Idx is a product by W).  Unrolled instead, the W
// guarded copies would be W dynamic single-bit writes — W index switches of
// up to `width` arms each.  Not the idiom (any structural mismatch, or a
// guard bound that is not the target's top index): `handled` stays false
// and the generic unroll takes it.
static bool r2_oob_for(tree_t s, r2_targets_t *ts, bool *handled)
{
   *handled = false;
   tree_t r = tree_range(s, 0);
   tree_t pdecl = tree_decls(s) > 0 ? tree_decl(s, 0) : NULL;
   int64_t left, right;
   if (pdecl == NULL || tree_subkind(r) != RANGE_TO
       || !folded_int(tree_left(r), &left)
       || !folded_int(tree_right(r), &right)
       || left != 0 || right < 0 || right > 4095)
      return true;
   const int W = (int)right + 1;
   const ident_t P = tree_ident(pdecl);
   // exactly one statement: if <guard> then <copy> end if
   tree_t ifs = NULL;
   for (int i = 0; i < tree_stmts(s); i++) {
      tree_t st = tree_stmt(s, i);
      if (tree_kind(st) == T_NULL)
         continue;
      if (ifs != NULL)
         return true;
      ifs = st;
   }
   if (ifs == NULL || tree_kind(ifs) != T_IF || tree_conds(ifs) != 1
       || !tree_has_value(tree_cond(ifs, 0)))
      return true;
   tree_t c = tree_cond(ifs, 0), copy = NULL;
   for (int i = 0; i < tree_stmts(c); i++) {
      tree_t st = tree_stmt(c, i);
      if (tree_kind(st) == T_NULL)
         continue;
      if (copy != NULL)
         return true;
      copy = st;
   }
   if (copy == NULL || (tree_kind(copy) != T_SIGNAL_ASSIGN
                        && tree_kind(copy) != T_VAR_ASSIGN))
      return true;
   tree_t tg = tree_target(copy), val = NULL;
   if (tree_kind(copy) == T_SIGNAL_ASSIGN) {
      if (tree_waveforms(copy) == 1 && tree_has_value(tree_waveform(copy, 0)))
         val = tree_value(tree_waveform(copy, 0));
   }
   else
      val = tree_value(copy);
   if (val == NULL || tree_kind(tg) != T_ARRAY_REF || tree_params(tg) != 1
       || tree_kind(tree_value(tg)) != T_REF
       || !tree_has_ref(tree_value(tg))
       || tree_kind(val) != T_ARRAY_REF || tree_params(val) != 1
       || tree_kind(tree_value(val)) != T_REF)
      return true;
   // copy: t(A + P) <= Tmp(P)
   tree_t ix = tree_value(tree_param(tg, 0));
   tree_t sx = tree_value(tree_param(val, 0));
   if (tree_kind(sx) != T_REF || tree_ident(sx) != P
       || tree_kind(ix) != T_FCALL || tree_params(ix) != 2
       || strcmp(istr(tree_ident(ix)), "\"+\"") != 0)
      return true;
   tree_t i0 = tree_value(tree_param(ix, 0));
   tree_t i1 = tree_value(tree_param(ix, 1));
   tree_t A;
   if (tree_kind(i1) == T_REF && tree_ident(i1) == P)
      A = i0;
   else if (tree_kind(i0) == T_REF && tree_ident(i0) == P)
      A = i1;
   else
      return true;
   // guard: ((A + P) >= 0) and ((A + P) <= hi)
   tree_t g = tree_value(c);
   if (tree_kind(g) != T_FCALL || tree_params(g) != 2
       || strcmp(istr(tree_ident(g)), "\"and\"") != 0)
      return true;
   tree_t ge = tree_value(tree_param(g, 0));
   tree_t le = tree_value(tree_param(g, 1));
   int64_t glo, ghi;
   if (tree_kind(ge) != T_FCALL || tree_params(ge) != 2
       || strcmp(istr(tree_ident(ge)), "\">=\"") != 0
       || !r2_tree_same(tree_value(tree_param(ge, 0)), ix)
       || !folded_int(tree_value(tree_param(ge, 1)), &glo) || glo != 0
       || tree_kind(le) != T_FCALL || tree_params(le) != 2
       || strcmp(istr(tree_ident(le)), "\"<=\"") != 0
       || !r2_tree_same(tree_value(tree_param(le, 0)), ix)
       || !folded_int(tree_value(tree_param(le, 1)), &ghi))
      return true;
   // target: a signal (or its NBA shadow) whose hold temp spans the guard
   ident_t ti = tree_ident(tree_value(tg));
   r2_alias_t *al = r2_alias_of(ti);
   if (al != NULL)
      ti = al->sig;
   r2_target_t *t = r2_target(ts, ti);
   if (t == NULL || t->width <= 0 || ghi != t->width - 1
       || r2_mem_of(ti) != NULL)
      return true;
   if (al != NULL)
      al->wrote = true;
   *handled = true;
   g_r2_cur = copy;
   // the source vector, landed at W bits (the copies read Tmp(0..W-1)); a
   // constant-valued Tmp (`:= (others => X)`, value plane 0) is a sized
   // literal at W bits
   char ss[R2_SPEC], st[64];
   g_r2_site = "oob-src";
   {
      r2_subst_t *sb = r2_subst_of(tree_ident(tree_value(val)));
      if (sb != NULL && sb->has_ival && sb->ival >= 0 && W <= 62)
         snprintf(ss, sizeof ss, "%d'd%lld", W,
                  (long long)(sb->ival & (((int64_t)1 << W) - 1)));
      else if (sb != NULL && sb->has_ival && sb->ival == 0)
         snprintf(ss, sizeof ss, "%d'd0", W);
      else if (!r2_expr(tree_value(val), ss, sizeof ss))
         return false;
   }
   if (!r2_temp(W, st, sizeof st) || g_r2->connect(st, ss) != 0) {
      R2_DECLINE("oob-src-land");
      return false;
   }
   int64_t cv;
   if (r2_eval_int(A, &cv)) {
      // constant position: one slice action, clamped to the vector
      const int64_t pl = cv < 0 ? -cv : 0;
      const int64_t ph = (cv + W - 1 > t->width - 1) ? t->width - 1 - cv
                                                     : W - 1;
      if (pl > ph)
         return true;   // entirely out of range: nothing written
      char lhs[128], rhs[96];
      if (ph == pl)
         snprintf(lhs, sizeof lhs, "%s[%lld]", t->g0, (long long)(cv + pl));
      else
         snprintf(lhs, sizeof lhs, "%s[%lld:%lld]", t->g0,
                  (long long)(cv + ph), (long long)(cv + pl));
      if (pl == 0 && ph == W - 1)
         snprintf(rhs, sizeof rhs, "%s", st);
      else if (ph == pl)
         snprintf(rhs, sizeof rhs, "%s[%lld]", st, (long long)pl);
      else
         snprintf(rhs, sizeof rhs, "%s[%lld:%lld]", st, (long long)ph,
                  (long long)pl);
      if (g_r2->case_assign(lhs, rhs) != 0) {
         R2_DECLINE("api-case-assign");
         return false;
      }
      return true;
   }
   int stride;
   bool sgn;
   r2_index_shape(A, &stride, &sgn);
   g_r2_site = "oob-idx";
   return r2_index_switch(A, t->g0, t->width, W, st, stride, sgn);
}

static bool r2_seq_one(tree_t s, r2_targets_t *ts)
{
   g_r2_cur = s;
   switch (tree_kind(s)) {
   case T_WAIT:
   case T_NULL:
      return true;
   case T_SIGNAL_ASSIGN:
   case T_DEPOSIT:
   case T_VAR_ASSIGN:
      {
         tree_t tg = tree_target(s);
         // NBA-shadow variables route to the shadowed SIGNAL's hold temp;
         // any other variable target declines
         if (tree_kind(s) == T_VAR_ASSIGN) {
            tree_t vb = tg;
            while (tree_kind(vb) == T_ARRAY_SLICE
                   || tree_kind(vb) == T_ARRAY_REF)
               vb = tree_value(vb);
            r2_alias_t *al = (tree_kind(vb) == T_REF)
               ? r2_alias_of(tree_ident(vb)) : NULL;
            if (al == NULL
                && (tree_kind(tg) == T_ARRAY_REF
                    || tree_kind(tg) == T_ARRAY_SLICE)
                && tree_kind(tree_value(tg)) == T_REF
                && tree_has_ref(tree_value(tg))
                && tree_kind(tree_ref(tree_value(tg))) == T_VAR_DECL
                && r2_pvar_of(ts, tree_ident(tree_value(tg))) == NULL
                && r2_mem_of(tree_ident(tree_value(tg))) == NULL) {
               // per-bit / per-slice build of a straight-line local vector
               int64_t blo = -1, bhi = -1;
               tree_t bd = tree_ref(tree_value(tg));
               type_t bt = tree_type(bd);
               const int bw = type_const_bounds(bt)
                  ? (int)type_width(bt) : -1;
               g_r2_site = "bit-build";
               bool brange = false;
               if (tree_kind(tg) == T_ARRAY_REF && tree_params(tg) == 1) {
                  if (r2_eval_int(tree_value(tree_param(tg, 0)), &blo)) {
                     bhi = blo;
                     brange = true;
                  }
               }
               else if (tree_kind(tg) == T_ARRAY_SLICE) {
                  tree_t r = tree_range(tg, 0);
                  int64_t l2, r2v;
                  if ((folded_int(tree_left(r), &l2)
                       || r2_eval_int(tree_left(r), &l2))
                      && (folded_int(tree_right(r), &r2v)
                          || r2_eval_int(tree_right(r), &r2v))) {
                     bhi = l2 > r2v ? l2 : r2v;
                     blo = l2 > r2v ? r2v : l2;
                     brange = true;
                  }
               }
               if (bw >= 1 && bw <= 4000 && brange
                   && blo >= 0 && bhi < bw) {
                  // VERSIONED partial write (any depth): a fresh wire takes
                  // $mux(pathcond, composed, prev-version)
                  char bs[R2_SPEC];
                  ident_t bvi = tree_ident(tree_value(tg));
                  g_r2_site = "var-part";
                  if (!r2_const(tree_value(s), bs, sizeof bs,
                                (int)(bhi - blo + 1))
                      && !r2_expr(tree_value(s), bs, sizeof bs))
                     return false;
                  return r2_var_write(bvi, bw, bs, false, (int)bhi,
                                      (int)blo);
               }
               {
                  char why[64];
                  snprintf(why, sizeof why, "bit-build w%d r%d", bw,
                           (int)brange);
                  R2_DECLINE(why);
                  return false;
               }
            }
            if (al == NULL) {
               // straight-line local variable: pure substitution (whole
               // target, outside any switch scope, not yet promoted)
               if (tree_kind(tg) == T_REF && g_r2_case_depth == 0
                   && r2_pvar_of(ts, tree_ident(tg)) == NULL) {
                  int64_t cv;
                  if (r2_eval_int(tree_value(s), &cv)) {
                     // constant: store the VALUE — sigspec renders can use
                     // it and the const interpreter can keep computing with
                     // it (loop induction, index arithmetic)
                     if (!r2_subst_set_int(tree_ident(tg), cv)) {
                        R2_DECLINE("subst-count");
                        return false;
                     }
                     return true;
                  }
                  char vs[R2_SPEC];
                  g_r2_site = "var-subst";
                  if (!r2_expr(tree_value(s), vs, sizeof vs))
                     return false;
                  if (!r2_subst_set(tree_ident(tg), vs)) {
                     R2_DECLINE("subst-count");
                     return false;
                  }
                  return true;
               }
               // branch-written variable WITH a current value: VERSION it
               // (feed-forward mux, correct read-after-write)
               if (tree_kind(tg) == T_REF && tree_has_ref(tg)
                   && tree_kind(tree_ref(tg)) == T_VAR_DECL
                   && g_r2_case_depth > 0) {
                  r2_subst_t *sv = r2_subst_of(tree_ident(tg));
                  type_t vt2 = tree_type(tree_ref(tg));
                  const int vw2 = type_const_bounds(vt2)
                     ? (int)type_width(vt2) : -1;
                  if (sv != NULL && sv->spec != NULL
                      && vw2 >= 1 && vw2 <= 4000) {
                     char vs2[R2_SPEC];
                     g_r2_site = "var-vers";
                     if (!r2_const(tree_value(s), vs2, sizeof vs2, vw2)
                         && !r2_expr(tree_value(s), vs2, sizeof vs2))
                        return false;
                     return r2_var_write(tree_ident(tg), vw2, vs2, true,
                                         0, 0);
                  }
               }
               // branch-written variable with NO prior value: LATCH state
               // (VHDL process vars persist across activations) — promote
               // to a pv/hold pair
               if (tree_kind(tg) == T_REF && tree_has_ref(tg)
                   && tree_kind(tree_ref(tg)) == T_VAR_DECL) {
                  r2_pvar_t *pe = r2_pvar_promote(ts, tree_ref(tg));
                  if (pe == NULL) {
                     R2_DECLINE("var-promote");
                     return false;
                  }
                  char vs[R2_SPEC];
                  g_r2_site = "pvar-assign";
                  pe->wrote = true;
                  tree_t pval = tree_value(s);
                  if (!r2_const(pval, vs, sizeof vs, pe->width)
                      && !r2_expr(pval, vs, sizeof vs))
                     return false;
                  return g_r2->case_assign(pe->g0, vs) == 0;
               }
               {
                  char why[48];
                  snprintf(why, sizeof why, "var-assign k%d d%d",
                           (int)tree_kind(tg), g_r2_case_depth);
                  R2_DECLINE(why);
               }
               return false;
            }
            // rewrite the BASE ident by aliasing: the slice/index structure
            // stays, target lookup below uses the signal's name
         }
         // memory writes: only the ENABLE threads the decision tree —
         // addr/data are unconditional comb, gated by EN at the port
         r2_mem_t *mm = NULL;
         {
            tree_t mb = tg;
            if (tree_kind(mb) == T_ARRAY_REF)
               mb = tree_value(mb);
            if (tree_kind(mb) == T_REF) {
               ident_t mi = tree_ident(mb);
               r2_alias_t *al = r2_alias_of(mi);
               if (al != NULL) {
                  mi = al->sig;
                  al->wrote = true;
               }
               mm = r2_mem_of(mi);
            }
         }
         if (mm != NULL) {
            tree_t val = NULL;
            if (tree_kind(s) == T_SIGNAL_ASSIGN) {
               if (tree_waveforms(s) < 1
                   || !tree_has_value(tree_waveform(s, 0))) {
                  R2_DECLINE("mem-null-wave");
                  return false;
               }
               val = tree_value(tree_waveform(s, 0));
            }
            else
               val = tree_value(s);   // deposit / shadow-var write
            if (ts->nsites >= 64 || ts->nmw >= 64) {
               R2_DECLINE("mem-sites");
               return false;
            }
            char en[32];
            snprintf(en, sizeof en, "g0m%d", ts->nsites++);
            if (g_r2->wire(en, 1, 0, NULL) != 0
                || g_r2->case_assign_root(en, "1'b0") != 0
                || g_r2->case_assign(en, "1'b1") != 0)
               return false;
            if (tree_kind(tg) == T_ARRAY_REF) {
               // indexed write
               char as[R2_SPEC], ds[R2_SPEC];
               g_r2_site = "memwr-addr";
               if (!r2_expr(tree_value(tree_param(tg, 0)), as, sizeof as))
                  return false;
               g_r2_site = "memwr-data";
               if (!r2_const(val, ds, sizeof ds, mm->width)
                   && !r2_expr(val, ds, sizeof ds))
                  return false;
               r2_memwr_t *w = &ts->mw[ts->nmw++];
               snprintf(w->memid, sizeof w->memid, "%s", mm->vname);
               w->addr = xstrdup(as);
               w->data = xstrdup(ds);
               snprintf(w->en, sizeof w->en, "%s", en);
               return true;
            }
            // whole-array positional aggregate: element i -> word
            // (size-1-i), the text path's (validated) convention
            if (tree_kind(val) != T_AGGREGATE
                || tree_assocs(val) != mm->size) {
               R2_DECLINE("mem-agg");
               return false;
            }
            const int ab = r2_clog2(mm->size);
            for (int i = 0; i < mm->size; i++) {
               tree_t a = tree_assoc(val, i);
               if (tree_subkind(a) != A_POS) {
                  R2_DECLINE("mem-agg-pos");
                  return false;
               }
               char as[64], ds[R2_SPEC];
               snprintf(as, sizeof as, "%d'd%d", ab, mm->size - 1 - i);
               g_r2_site = "memagg-data";
               if (!r2_const(tree_value(a), ds, sizeof ds, mm->width)
                   && !r2_expr(tree_value(a), ds, sizeof ds))
                  return false;
               if (ts->nmw >= 64) {
                  R2_DECLINE("mem-sites");
                  return false;
               }
               r2_memwr_t *w = &ts->mw[ts->nmw++];
               snprintf(w->memid, sizeof w->memid, "%s", mm->vname);
               w->addr = xstrdup(as);
               w->data = xstrdup(ds);
               snprintf(w->en, sizeof w->en, "%s", en);
            }
            return true;
         }
         // constant slice / bit target: assign into the hold temp's range
         int64_t hi = -1, lo = -1;
         if (r2_sel_nested(tg)) {
            // element / nested select of an array-of-vector wire: the
            // flat bit range of the root signal (r17 wire arrays)
            tree_t root;
            int64_t off, w;
            if (!r2_sel_range(tg, &root, &off, &w)) {
               R2_DECLINE("target-sel-chain");
               return false;
            }
            hi = off + w - 1;
            lo = off;
            tg = root;
         }
         else if (tree_kind(tg) == T_ARRAY_SLICE) {
            tree_t r = tree_range(tg, 0);
            int64_t left, right;
            if ((!folded_int(tree_left(r), &left)
                 && !r2_eval_int(tree_left(r), &left))
                || (!folded_int(tree_right(r), &right)
                    && !r2_eval_int(tree_right(r), &right))
                || left < 0 || right < 0) {
               R2_DECLINE("target-slice-bounds");
               return false;
            }
            hi = left > right ? left : right;
            lo = left > right ? right : left;
            tg = tree_value(tg);
         }
         else if (tree_kind(tg) == T_ARRAY_REF) {
            int64_t idx;
            if (tree_params(tg) == 1
                && (folded_int(tree_value(tree_param(tg, 0)), &idx)
                    || r2_eval_int(tree_value(tree_param(tg, 0)), &idx))
                && idx >= 0) {
               hi = lo = idx;
               tg = tree_value(tg);
            }
            else if (tree_params(tg) == 1) {
               // DYNAMIC single-bit write: lower to a masked whole-target
               // compose — g0 = (g0 & ~(1<<i)) | (bit<<i) — ordinary cells,
               // so the assignment threads decision trees like any other
               tree_t tb = tree_value(tg);
               ident_t bi = (tree_kind(tb) == T_REF && tree_has_ref(tb))
                  ? tree_ident(tb) : NULL;
               if (bi != NULL) {
                  r2_alias_t *al = r2_alias_of(bi);
                  if (al != NULL) {
                     bi = al->sig;
                     al->wrote = true;
                  }
               }
               r2_target_t *t = bi != NULL ? r2_target(ts, bi) : NULL;
               if (t == NULL || t->width <= 0 || t->width > 4000) {
                  R2_DECLINE("dyn-target");
                  return false;
               }
               // the compose reads the PRE-activation value (the signal):
               // a second dynamic write to the same target would clobber
               // the first — decline (the NBA sites are single-write)
               for (int dw = 0; dw < ts->ndynwr; dw++)
                  if (ts->dynwr[dw] == t->name) {
                     R2_DECLINE("dyn-multi");
                     return false;
                  }
               if (ts->ndynwr >= 16) {
                  R2_DECLINE("dyn-count");
                  return false;
               }
               ts->dynwr[ts->ndynwr++] = t->name;
               char is[R2_SPEC];
               g_r2_site = "dyn-idx";
               if (!r2_expr(tree_value(tree_param(tg, 0)), is, sizeof is))
                  return false;
               tree_t dval;
               if (tree_kind(s) == T_SIGNAL_ASSIGN) {
                  if (tree_waveforms(s) < 1
                      || !tree_has_value(tree_waveform(s, 0))) {
                     R2_DECLINE("dyn-wave");
                     return false;
                  }
                  dval = tree_value(tree_waveform(s, 0));
               }
               else
                  dval = tree_value(s);
               char vs[R2_SPEC];
               g_r2_site = "dyn-val";
               if (!r2_const(dval, vs, sizeof vs, 1)
                   && !r2_expr(dval, vs, sizeof vs))
                  return false;
               const int w = t->width;
               char one[64], m1[R2_SPEC], m2[R2_SPEC], m3[R2_SPEC],
                  m4[R2_SPEC], m5[R2_SPEC], cn[R2_SPEC + 8];
               snprintf(one, sizeof one, "%d'd1", w);
               // m1 = 1 << idx
               if (!r2_temp(w, m1, sizeof m1)) return false;
               snprintf(cn, sizeof cn, "c%s", m1);
               if (g_r2->cell_bin("shl", cn, one, is, m1, 0) != 0)
                  return false;
               // m2 = ~m1
               if (!r2_temp(w, m2, sizeof m2)) return false;
               snprintf(cn, sizeof cn, "c%s", m2);
               if (g_r2->cell_un("not", cn, m1, m2, 0) != 0)
                  return false;
               // m3 = <pre-value> & m2 — the SIGNAL, not g0: reading the
               // post-mux g0 wire from module-level cells would be a
               // combinational loop
               if (!r2_temp(w, m3, sizeof m3)) return false;
               snprintf(cn, sizeof cn, "c%s", m3);
               if (g_r2->cell_bin("and", cn, t->spec, m2, m3, 0) != 0)
                  return false;
               // m4 = value << idx  (value zero-extends to width w)
               if (!r2_temp(w, m4, sizeof m4)) return false;
               snprintf(cn, sizeof cn, "c%s", m4);
               if (g_r2->cell_bin("shl", cn, vs, is, m4, 0) != 0)
                  return false;
               // m5 = m3 | m4
               if (!r2_temp(w, m5, sizeof m5)) return false;
               snprintf(cn, sizeof cn, "c%s", m5);
               if (g_r2->cell_bin("or", cn, m3, m4, m5, 0) != 0)
                  return false;
               return g_r2->case_assign(t->g0, m5) == 0;
            }
            else {
               R2_DECLINE("target-index");
               return false;
            }
         }
         if (tree_kind(tg) != T_REF || !tree_has_ref(tg)) {
            R2_DECLINE("assign-target");
            return false;
         }
         ident_t ti = tree_ident(tg);
         { r2_alias_t *al = r2_alias_of(ti);
           if (al != NULL) { ti = al->sig; al->wrote = true; } }
         r2_target_t *t = r2_target(ts, ti);
         if (t == NULL) {
            char why[96];
            snprintf(why, sizeof why, "%s %s",
                     ts->capped ? "target-cap" : "target-miss", istr(ti));
            R2_DECLINE(why);
            return false;
         }
         char lhs[96];
         int vw = t->width;
         if (hi >= 0) {
            if (hi >= t->width) {
               R2_DECLINE("target-range");
               return false;
            }
            snprintf(lhs, sizeof lhs, "%s[%lld:%lld]", t->g0,
                     (long long)hi, (long long)lo);
            vw = (int)(hi - lo + 1);
         }
         else
            snprintf(lhs, sizeof lhs, "%s", t->g0);
         tree_t val;
         if (tree_kind(s) == T_SIGNAL_ASSIGN) {
            if (tree_waveforms(s) < 1
                || !tree_has_value(tree_waveform(s, 0))) {
               R2_DECLINE("null-wave");
               return false;
            }
            val = tree_value(tree_waveform(s, 0));
         }
         else
            val = tree_value(s);   // deposit / shadow-var write
         // trailing copy `sig <= var` of a PROMOTED variable: the signal's
         // sync source becomes the var's hold temp (the post-tree muxed
         // value — a root action here would read the PRE-branch value,
         // since case actions evaluate before switches)
         if (hi < 0 && tree_kind(val) == T_REF && g_r2_case_depth == 0) {
            r2_pvar_t *pe = r2_pvar_of(ts, tree_ident(val));
            if (pe != NULL) {
               snprintf(t->g0, sizeof t->g0, "%s", pe->g0);
               return true;
            }
         }
         // constants first WITH the target's width for context (an
         // others-aggregate has no self-width)
         char v[R2_SPEC];
         g_r2_site = "seq-assign";
         if (!r2_const(val, v, sizeof v, vw)
             && !r2_expr(val, v, sizeof v))
            return false;
         if (hi >= 0 && vw > 0) {
            // slice target: RTLIL assigns need equal widths, but the value
            // may render wider (unconstrained operator chains, whole-var
            // reads).  Land it on a temp and take the low slice.
            const int rw = r2_width_or_operands(val);
            if (rw > vw) {
               char ct[64];
               if (!r2_temp(rw, ct, sizeof ct)
                   || g_r2->connect(ct, v) != 0) {
                  R2_DECLINE("coerce");
                  return false;
               }
               if (vw == 1)
                  snprintf(v, sizeof v, "%s[0]", ct);
               else
                  snprintf(v, sizeof v, "%s[%d:0]", ct, vw - 1);
            }
         }
         if (g_r2->case_assign(lhs, v) != 0) {
            R2_DECLINE("api-case-assign");
            return false;
         }
         return true;
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
               // statically-constant condition (OOB guards around computed
               // indices, unrolled-loop residue): prune — a true arm walks
               // inline in the CURRENT case scope and ends the chain, a
               // false arm vanishes.  This keeps case depth unchanged, so
               // var substitution stays live through the pruned branch.
               int64_t scv;
               if (r2_eval_int(tree_value(c), &scv)) {
                  if (scv != 0) {
                     ok = r2_seq(c, ts);
                     break;
                  }
                  continue;
               }
               char cs[R2_SPEC];
               g_r2_site = "if-cond";
               if (!r2_cond(tree_value(c), cs, sizeof cs)) { ok = false; break; }
               if (g_r2->switch_begin(cs) != 0
                   || g_r2->case_begin("1'b1") != 0) { ok = false; break; }
               g_r2_case_depth++;
               if (!r2_cond_push(cs)) { ok = false; break; }
               ok = r2_seq(c, ts);
               r2_subst_poison_from(g_r2_case_depth);   // arm scope ends
               if (!ok) break;
               if (g_r2->case_end() != 0
                   || g_r2->case_begin(NULL) != 0) { ok = false; break; }
               // entering the default arm: path condition flips to !cs
               // (subsequent elsifs nest inside this default)
               r2_cond_pop();
               {
                  char nt[64], ncn[80];
                  if (!r2_temp(1, nt, sizeof nt)) { ok = false; break; }
                  snprintf(ncn, sizeof ncn, "c%s", nt);
                  if (g_r2->cell_un("not", ncn, cs, nt, 0) != 0
                      || !r2_cond_push(nt)) { ok = false; break; }
               }
               depth++;
            }
            else {
               // else arm: statements into the current default case
               ok = r2_seq(c, ts);
               r2_subst_poison_from(g_r2_case_depth);
            }
         }
         // the implicit default arms' scope ends too: a value seeded
         // there (r2_arm_defaults) must not survive to depth d0
         if (depth > 0)
            r2_subst_poison_from(g_r2_case_depth - depth + 1);
         for (int i = 0; i < depth; i++) {
            if (g_r2->case_end() != 0 || g_r2->switch_end() != 0)
               ok = false;
            g_r2_case_depth--;
            r2_cond_pop();
         }
         return ok && (g_r2_fail == 0 || g_r2_census);
      }
   case T_WHILE:
      {
         // counting-while unroll: the translated-SV loops are all
         // `i := K; while l3d_lt_s(i, N) loop ... i := i + 1; end` — the
         // condition and induction evaluate under the substitution env
         // (var-subst stores constants as integers), so interpret the loop
         // at walk time.  yosys read_verilog cannot take these at all
         // (procedural while is rejected), so this is walker-only ground.
         // Inside a switch arm the induction variable is an arm-scoped
         // substitution (VX_pipe_register's reset/enable loops).
         if (!tree_has_value(s)) {
            R2_DECLINE("while-cond");
            return false;
         }
         for (int iter = 0; ; iter++) {
            if (iter > 4096) {
               R2_DECLINE("while-size");
               return false;
            }
            int64_t cv;
            if (!r2_eval_int(tree_value(s), &cv)) {
               R2_DECLINE("while-eval");
               return false;
            }
            if (cv == 0)
               break;
            if (!r2_seq(s, ts))
               return false;
         }
         return true;
      }
   case T_LOOP:
      {
         // process-sensitivity loop (`<init>; loop <body>; wait on ...; end
         // loop`): the TEXT path emits the body INLINE and drops the waits
         // (yosys sees a flat always body) — mirror that exactly.  A bare
         // loop without a wait is a real infinite loop: decline.
         const int n = tree_stmts(s);
         bool haswait = false;
         for (int i = 0; i < n; i++)
            if (tree_kind(tree_stmt(s, i)) == T_WAIT)
               haswait = true;
         if (!haswait) {
            R2_DECLINE("loop-nowait");
            return false;
         }
         for (int i = 0; i < n; i++) {
            tree_t st = tree_stmt(s, i);
            if (tree_kind(st) == T_WAIT)
               continue;
            if (!r2_seq_one(st, ts))
               return false;
         }
         return true;
      }
   case T_FOR:
      {
         // the OOB_WriteV part-select write idiom lowers as a whole
         {
            bool handled;
            if (!r2_oob_for(s, ts, &handled))
               return false;
            if (handled)
               return true;
         }
         // constant-range unroll: the index becomes a substituted constant
         // per iteration (post-elaboration bounds are folded)
         tree_t r = tree_range(s, 0);
         tree_t idecl = tree_decls(s) > 0 ? tree_decl(s, 0) : NULL;
         int64_t left, right;
         if (idecl == NULL
             || !folded_int(tree_left(r), &left)
             || !folded_int(tree_right(r), &right)) {
            R2_DECLINE("for-range");
            return false;
         }
         const int64_t lo = left < right ? left : right;
         const int64_t hi = left < right ? right : left;
         if (hi - lo > 4096) {
            R2_DECLINE("for-size");
            return false;
         }
         const int step = left <= right ? 1 : -1;
         for (int64_t iv = left; ; iv += step) {
            char cs[32];
            snprintf(cs, sizeof cs, "32'd%lld", (long long)(iv < 0 ? 0 : iv));
            if (iv < 0) {
               R2_DECLINE("for-neg");
               return false;
            }
            if (!r2_subst_set(tree_ident(idecl), cs)) {
               R2_DECLINE("subst-count");
               return false;
            }
            if (!r2_seq(s, ts))
               return false;
            if (iv == right)
               break;
         }
         return true;
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
         char anyprev[96] = "1'b0";   // OR of previous arms' conditions
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
            g_r2_case_depth++;
            // path condition for this arm: OR of eq(sw, choice) — the
            // default arm is NOT(any previous arm's condition)
            {
               char armc[96];
               if (cc == NULL) {
                  char nt[64], ncn[80];
                  if (!r2_temp(1, nt, sizeof nt)) { ok = false; }
                  else {
                     snprintf(ncn, sizeof ncn, "c%s", nt);
                     if (g_r2->cell_un("not", ncn, anyprev, nt, 0) != 0)
                        ok = false;
                     else
                        snprintf(armc, sizeof armc, "%s", nt);
                  }
               }
               else {
                  armc[0] = '\0';
                  char one2[R2_SPEC];
                  snprintf(one2, sizeof one2, "%s", cmp);
                  for (char *tk = strtok(one2, ";"); ok && tk != NULL;
                       tk = strtok(NULL, ";")) {
                     char eqw[64], eqn[80];
                     if (!r2_temp(1, eqw, sizeof eqw)) { ok = false; break; }
                     snprintf(eqn, sizeof eqn, "c%s", eqw);
                     if (g_r2->cell_bin("eq", eqn, sw, tk, eqw, 0) != 0) {
                        ok = false;
                        break;
                     }
                     if (armc[0] == '\0')
                        snprintf(armc, sizeof armc, "%s", eqw);
                     else {
                        char orw[64], orn[80];
                        if (!r2_temp(1, orw, sizeof orw)) { ok = false; break; }
                        snprintf(orn, sizeof orn, "c%s", orw);
                        if (g_r2->cell_bin("or", orn, armc, eqw, orw, 0)
                            != 0) { ok = false; break; }
                        snprintf(armc, sizeof armc, "%s", orw);
                     }
                  }
                  if (ok) {
                     // fold into the running any-previous-arm condition
                     char orw[64], orn[80];
                     if (!r2_temp(1, orw, sizeof orw)) ok = false;
                     else {
                        snprintf(orn, sizeof orn, "c%s", orw);
                        if (g_r2->cell_bin("or", orn, anyprev, armc, orw, 0)
                            != 0) ok = false;
                        else snprintf(anyprev, sizeof anyprev, "%s", orw);
                     }
                  }
               }
               if (ok && !r2_cond_push(armc)) ok = false;
            }
            if (ok)
               ok = r2_seq(alt, ts);
            r2_cond_pop();
            g_r2_case_depth--;
            if (g_r2->case_end() != 0)
               ok = false;
         }
         if (g_r2->switch_end() != 0)
            ok = false;
         return ok && (g_r2_fail == 0 || g_r2_census);
      }
   default:
      {
         char why[48];
         snprintf(why, sizeof why, "stmt-kind %d", (int)tree_kind(s));
         R2_DECLINE(why);
      }
      return false;
   }
}

// does a process body write a memory (directly or through its NBA shadow)?
typedef struct { int n; } r2_memw_scan_t;

static void r2_memw_scan_cb(tree_t t, void *ctx)
{
   r2_memw_scan_t *sc = (r2_memw_scan_t *)ctx;
   const tree_kind_t k = tree_kind(t);
   if (k != T_VAR_ASSIGN && k != T_SIGNAL_ASSIGN)
      return;
   tree_t tg = tree_target(t);
   if (tree_kind(tg) == T_ARRAY_REF)
      tg = tree_value(tg);
   if (tree_kind(tg) != T_REF)
      return;
   ident_t id = tree_ident(tg);
   r2_alias_t *al = r2_alias_of(id);
   if (al != NULL)
      id = al->sig;
   if (r2_mem_of(id) != NULL)
      sc->n++;
}

static bool r2_seq(tree_t list_of, r2_targets_t *ts)
{
   const int n = tree_stmts(list_of);
   bool ok = true;
   for (int i = 0; i < n; i++) {
      const int before = g_r2_fail;
      tree_t st = tree_stmt(list_of, i);
      if (!r2_seq_one(st, ts)) {
         if (!g_r2_census)
            return false;
         ok = false;   // census: keep walking the remaining statements
         if (g_r2_fail == before) {   // a bare `return false` path
            char why[48];
            g_r2_cur = st;
            snprintf(why, sizeof why, "silent-stmt k%d", (int)tree_kind(st));
            R2_DECLINE(why);
         }
      }
   }
   return ok;
}

// flush pending memory writes onto the (just created) edge sync
static bool r2_flush_memwr(r2_targets_t *ts)
{
   bool ok = true;
   for (int i = 0; i < ts->nmw; i++) {
      if (ok && g_r2->sync_memwr(ts->mw[i].memid, ts->mw[i].addr,
                                 ts->mw[i].data, ts->mw[i].en) != 0) {
         R2_DECLINE("sync-memwr");
         ok = false;
      }
      free(ts->mw[i].addr);
      free(ts->mw[i].data);
   }
   ts->nmw = 0;
   return ok;
}

// tgt-vhdl's NBA delta guard: `if <boolvar> then <boolvar> := False; else
// wait for 0 ns; end if;` — every arm holds only writes to the guard
// variable, waits and nulls
static bool r2_nba_init_guard(tree_t s)
{
   const int nc = tree_conds(s);
   if (nc < 1 || !tree_has_value(tree_cond(s, 0)))
      return false;
   tree_t cv = tree_value(tree_cond(s, 0));
   if (tree_kind(cv) != T_REF || !tree_has_ref(cv)
       || tree_kind(tree_ref(cv)) != T_VAR_DECL)
      return false;
   const ident_t gv = tree_ident(cv);
   for (int i = 0; i < nc; i++) {
      tree_t c = tree_cond(s, i);
      if (i > 0 && tree_has_value(c))
         return false;
      const int n = tree_stmts(c);
      for (int j = 0; j < n; j++) {
         tree_t st = tree_stmt(c, j);
         const tree_kind_t k = tree_kind(st);
         if (k == T_WAIT || k == T_NULL)
            continue;
         if (k == T_VAR_ASSIGN && tree_kind(tree_target(st)) == T_REF
             && tree_ident(tree_target(st)) == gv)
            continue;
         return false;
      }
   }
   return true;
}

static bool r2_process(tree_t p0, int pidx)
{
   tree_t p = proc_body(p0);
   tree_t body_if = NULL, sig[8], ifstmt = NULL;
   g_r2_pidx = pidx;
   g_r2_cur = p0;
   g_r2_expr_depth = 0;
   if (g_r2_census) {
      const loc_t *loc = tree_loc(p0);
      notef("vhdl2rtlil-census: %s p%d begin L%u", g_r2_modname, pidx,
            loc != NULL ? (unsigned)loc->first_line : 0u);
   }
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
         if (tree_waveforms(only) < 1
             || !tree_has_value(tree_waveform(only, 0))) {
            R2_DECLINE("cont-assign");
            return false;
         }
         char lhs[R2_SPEC];
         int tw = -1;
         if (tree_kind(tg) == T_REF && tree_has_ref(tg)) {
            type_t tty = tree_type(tree_ref(tg));
            tw = type_const_bounds(tty) ? (int)type_width(tty) : -1;
            snprintf(lhs, sizeof lhs, "%s", vid(tree_ident(tg)));
         }
         else if (r2_sel_nested(tg)) {
            tree_t root;
            int64_t off, w;
            if (!r2_sel_range(tg, &root, &off, &w)) {
               R2_DECLINE("cont-sel-chain");
               return false;
            }
            snprintf(lhs, sizeof lhs, "%s[%lld:%lld]",
                     vid(tree_ident(root)), (long long)(off + w - 1),
                     (long long)off);
            tw = (int)w;
         }
         else if ((tree_kind(tg) == T_ARRAY_SLICE
                   || tree_kind(tg) == T_ARRAY_REF)
                  && tree_kind(tree_value(tg)) == T_REF
                  && r2_mem_of(tree_ident(tree_value(tg))) == NULL) {
            // slice / const-indexed target: RTLIL connections take any
            // sigspec LHS, so render the target as its sigspec.  Memory
            // bases are excluded (r2_expr would build a $memrd — a READ).
            int64_t dummy;
            if (tree_kind(tg) == T_ARRAY_REF
                && (tree_params(tg) != 1
                    || !folded_int(tree_value(tree_param(tg, 0)), &dummy))) {
               R2_DECLINE("cont-assign-dynidx");
               return false;
            }
            g_r2_site = "cont-lhs";
            if (!r2_expr(tg, lhs, sizeof lhs))
               return false;
            tw = r2_width(tg);
         }
         else {
            R2_DECLINE("cont-assign-target");
            return false;
         }
         g_r2_site = "cont-rhs";
         char v[R2_SPEC];
         tree_t val = tree_value(tree_waveform(only, 0));
         if (!r2_const(val, v, sizeof v, tw)
             && !r2_expr(val, v, sizeof v))
            return false;
         return g_r2->connect(lhs, v) == 0;
      }
      // general comb process: decision tree + `always` sync.  The root
      // action g0 = target mirrors read_verilog's $0 self-init: complete
      // assignment optimizes the self arm away; incomplete assignment
      // infers the same latch the text path would (and gsm declines it).
      r2_subst_reset();
      g_r2_case_depth = 0;
      r2_targets_t cts = { .n = 0, .comb = true, .t = g_r2_tgt,
                           .pidx = pidx };
      g_r2_pvts = &cts;
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
      if (cok && cts.nmw > 0) {
         R2_DECLINE("comb-memwr");
         cok = false;
      }
      if (cok)
         cok = g_r2->sync("always", NULL) == 0;
      for (int i = 0; cok && i < cts.n; i++)
         cok = g_r2->sync_assign(cts.t[i].spec, cts.t[i].g0) == 0;
      for (int i = 0; cok && i < cts.npv; i++)
         if (cts.pv[i].persistent) {   // latch state: commit the hold value
            // versioned flow: the current subst spec IS the final value;
            // the g0 hold covers the pre-versioning branch-tree flow
            r2_subst_t *sbp = r2_subst_of(cts.pv[i].var);
            const char *src = (sbp != NULL && sbp->spec != NULL)
               ? sbp->spec : cts.pv[i].g0;
            cok = g_r2->sync_assign(cts.pv[i].pv, src) == 0;
         }
      return cok && g_r2_fail == 0;
   }
   // (rcond/rsig are derived below from ifstmt; the sensitivity may carry
   //  the async reset as a SECOND edge — resolved after areset_of runs)
   tree_t rsig = NULL; bool rpe = false, rbefore = false;
   tree_t rcond = (ifstmt != NULL)
      ? areset_of(ifstmt, body_if, &rsig, &rpe, &rbefore) : NULL;
   if (rcond != NULL && !rbefore) {
      R2_DECLINE("reset-clk-priority");
      return false;
   }
   // pick the clock among the sensitivity edges: with an async reset the
   // translated form lists BOTH `falling_edge(rst) or rising_edge(clk)`;
   // the reset's edge is subsumed by the ST0/ST1 level sync
   int clk_i = 0;
   bool both_edges = false;   // read_verilog encoding: emit BOTH edge syncs
   if (ne == 2) {
      if (rcond == NULL) {
         // the reset-if NESTS INSIDE the two-edge guard (rvdff shape):
         // mirror read_verilog — both edge syncs carry the actions and
         // yosys's proc_arst recognizes the inner reset switch.  An
         // unmatched shape becomes a contained proc_dff error -> rc 2 ->
         // the text path retries (which meets the same fate) — safe.
         both_edges = true;
      }
      else if (rsig == NULL || tree_kind(rsig) != T_REF) {
         R2_DECLINE("multi-edge");
         return false;
      }
      if (both_edges)
         ;   // no clock/reset split — both syncs emitted below
      else if (tree_kind(sig[0]) == T_REF
          && tree_ident(sig[0]) == tree_ident(rsig))
         clk_i = 1;
      else if (tree_kind(sig[1]) == T_REF
               && tree_ident(sig[1]) == tree_ident(rsig))
         clk_i = 0;
      else {
         R2_DECLINE("multi-edge-rst");
         return false;
      }
   }
   else if (ne != 1) {
      R2_DECLINE("multi-edge");
      return false;
   }
   // tgt-vhdl NBA idiom: shadow pre-copies before the edge-if, commits
   // after it.  The pre-copy IS the g0 root action and the commit IS the
   // sync assign, so both elide into the existing hold-temp machinery;
   // the shadow var becomes an alias for the signal inside the tree.
   g_r2_nalias = 0;
   r2_subst_reset();
   g_r2_case_depth = 0;
   tree_t extra[8];   // merged same-edge blocks: further `if rising_edge`
   int nextra = 0;
   {
      const int np = tree_stmts(p);
      int ifidx = np;
      for (int i = 0; i < np; i++)
         if (tree_stmt(p, i) == ifstmt) { ifidx = i; break; }
      for (int i = 0; i < np; i++) {
         tree_t s = tree_stmt(p, i);
         if (s == ifstmt || tree_kind(s) == T_WAIT)
            continue;
         if (i > ifidx && tree_kind(s) == T_IF) {
            // the tgt-vhdl delta guard `if nba_init_run then nba_init_run
            // := False; else wait for 0 ns; end if;` between the edge-if
            // and the commit: no register semantics (the text path emits
            // it and yosys drops the dead variable) — skip it
            if (r2_nba_init_guard(s))
               continue;
            // a merged same-edge always block: a second `if <same edge>`
            // with no other arm walks under the one sync after the first
            tree_t xsig[8];
            bool xpe[8];
            if (rcond == NULL && ne == 1 && nextra < 8
                && tree_conds(s) == 1 && tree_has_value(tree_cond(s, 0))
                && edges_of(tree_value(tree_cond(s, 0)), xsig, xpe, 8) == 1
                && tree_kind(xsig[0]) == T_REF && tree_kind(sig[0]) == T_REF
                && tree_ident(xsig[0]) == tree_ident(sig[0])
                && xpe[0] == pe[0]) {
               extra[nextra++] = tree_cond(s, 0);
               continue;
            }
            R2_DECLINE("proc-extra-if");
            return false;
         }
         if (i < ifidx && tree_kind(s) == T_VAR_ASSIGN) {
            // pre-copy  v_nba_r := r
            tree_t vt = tree_target(s), vv = tree_value(s);
            if (tree_kind(vt) == T_REF && tree_kind(vv) == T_REF
                && tree_has_ref(vv) && g_r2_nalias < 16) {
               r2_alias_t *al = &g_r2_alias[g_r2_nalias++];
               al->var = tree_ident(vt);
               al->sig = tree_ident(vv);
               al->sigdecl = tree_ref(vv);
               al->committed = false;
               al->wrote = false;
               continue;
            }
            R2_DECLINE("nba-pre");
            return false;
         }
         if (i > ifidx && tree_kind(s) == T_SIGNAL_ASSIGN) {
            // commit  r <= v_nba_r
            tree_t st = tree_target(s);
            tree_t sv = (tree_waveforms(s) >= 1
                         && tree_has_value(tree_waveform(s, 0)))
               ? tree_value(tree_waveform(s, 0)) : NULL;
            if (st != NULL && sv != NULL && tree_kind(st) == T_REF
                && tree_kind(sv) == T_REF) {
               r2_alias_t *al = r2_alias_of(tree_ident(sv));
               if (al != NULL && al->sig == tree_ident(st)) {
                  al->committed = true;
                  continue;
               }
            }
            R2_DECLINE("nba-post");
            return false;
         }
         R2_DECLINE("proc-extra-stmt");
         return false;
      }
      for (int i = 0; i < g_r2_nalias; i++)
         if (!g_r2_alias[i].committed) {
            R2_DECLINE("nba-pair");
            return false;
         }
   }

   r2_targets_t ts = { .n = 0, .t = g_r2_tgt, .pidx = pidx };
   g_r2_pvts = &ts;
   tree_visit(p, r2_collect_cb, &ts);
   // aliased signals are targets even when the body writes only the shadow
   for (int i = 0; i < g_r2_nalias; i++) {
      if (r2_target(&ts, g_r2_alias[i].sig) != NULL
          || r2_mem_of(g_r2_alias[i].sig) != NULL)
         continue;
      if (ts.n >= 64) { R2_DECLINE("targets"); return false; }
      r2_target_t *n = &ts.t[ts.n++];
      n->name = g_r2_alias[i].sig;
      snprintf(n->spec, sizeof n->spec, "%s", vid(n->name));
      snprintf(n->g0, sizeof n->g0, "g0_%s", vid(n->name));
      type_t ty = tree_type(g_r2_alias[i].sigdecl);
      n->width = type_const_bounds(ty) ? (int)type_width(ty) : -1;
   }
   // a process with no register target still owns its MEMORY writes (a
   // RAM's write port: VX_dp_ram writes only the shadow of the array);
   // without them there is nothing to build
   r2_memw_scan_t mws = { .n = 0 };
   tree_visit(p, r2_memw_scan_cb, &mws);
   if ((ts.n == 0 && mws.n == 0) || (g_r2_fail > 0 && !g_r2_census))
      return g_r2_fail == 0 || g_r2_census;

   char pn[64];
   snprintf(pn, sizeof pn, "p%d", pidx);
   if (g_r2->proc(pn) != 0) {
      R2_DECLINE("api-proc");
      return false;
   }

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
         ok = r2_expr(sig[clk_i], es, sizeof es)
            && g_r2->sync(pe[clk_i] ? "posedge" : "negedge", es) == 0;
         for (int i = 0; ok && i < ts.n; i++)
            ok = g_r2->sync_assign(ts.t[i].spec, ts.t[i].g0) == 0;
         if (ok)
            ok = r2_flush_memwr(&ts);
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
            tree_t val = NULL;
            tree_t tg = NULL;
            if (tree_kind(s) == T_SIGNAL_ASSIGN
                && tree_waveforms(s) >= 1
                && tree_has_value(tree_waveform(s, 0))) {
               tg = tree_target(s);
               val = tree_value(tree_waveform(s, 0));
            }
            else if (tree_kind(s) == T_VAR_ASSIGN) {
               tg = tree_target(s);
               val = tree_value(s);
            }
            else { ok = false; break; }
            ident_t ti2 = (tg != NULL && tree_kind(tg) == T_REF)
               ? tree_ident(tg) : NULL;
            if (ti2 != NULL) {
               r2_alias_t *al = r2_alias_of(ti2);
               if (al != NULL)
                  ti2 = al->sig;
            }
            r2_target_t *t = ti2 != NULL ? r2_target(&ts, ti2) : NULL;
            char v[R2_SPEC];
            if (t == NULL || val == NULL
                || !r2_const(val, v, sizeof v, t->width)) {
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
      // merged same-edge always blocks: their bodies follow under the one
      // sync, in statement order (same targets, same hold temps)
      for (int i = 0; ok && i < nextra; i++)
         ok = r2_seq(extra[i], &ts);
      if (ok && both_edges && ts.nmw > 0) {
         R2_DECLINE("both-edges-memwr");
         ok = false;
      }
      if (ok) {
         const int e0 = both_edges ? 0 : clk_i;
         const int e1 = both_edges ? 1 : clk_i;
         for (int e = e0; ok && e <= e1; e++) {
            char es[R2_SPEC];
            ok = r2_expr(sig[e], es, sizeof es)
               && g_r2->sync(pe[e] ? "posedge" : "negedge", es) == 0;
            for (int i = 0; ok && i < ts.n; i++)
               ok = g_r2->sync_assign(ts.t[i].spec, ts.t[i].g0) == 0;
            // promoted register-vars (read-before-write locals in clocked
            // processes): commit the pv wire from the final version
            for (int i = 0; ok && i < ts.npv; i++)
               if (ts.pv[i].persistent) {
                  r2_subst_t *sbp = r2_subst_of(ts.pv[i].var);
                  const char *src = (sbp != NULL && sbp->spec != NULL)
                     ? sbp->spec : ts.pv[i].g0;
                  ok = g_r2->sync_assign(ts.pv[i].pv, src) == 0;
               }
            if (ok && e == e0)
               ok = r2_flush_memwr(&ts);
         }
      }
   }
   return ok && (g_r2_fail == 0 || g_r2_census);
}

// ---- the module walk -------------------------------------------------------

typedef struct { ident_t id; int n; } r2_wcount_t;

// references to a signal from sensitivity lists: a `process (a, b)` is
// lowered to a trailing `wait on a, b;`, so the triggers hang off the
// T_WAIT (a T_PROCESS keeps them only in the unlowered form)
typedef struct { tree_t decl; int n; } r2_sens_t;

static void r2_sens_cb(tree_t t, void *ctx)
{
   r2_sens_t *sc = (r2_sens_t *)ctx;
   if (tree_kind(t) != T_PROCESS && tree_kind(t) != T_WAIT)
      return;
   const int nt = tree_triggers(t);
   for (int i = 0; i < nt; i++) {
      tree_t r = tree_trigger(t, i);
      if (tree_kind(r) == T_REF && tree_has_ref(r) && tree_ref(r) == sc->decl)
         sc->n++;
   }
}

static void r2_wcount_cb(tree_t t, void *ctx)
{
   r2_wcount_t *wc = (r2_wcount_t *)ctx;
   const tree_kind_t k = tree_kind(t);
   if (k != T_SIGNAL_ASSIGN && k != T_DEPOSIT && k != T_VAR_ASSIGN)
      return;
   tree_t tg = tree_target(t);
   while (tree_kind(tg) == T_ARRAY_SLICE || tree_kind(tg) == T_ARRAY_REF
          || tree_kind(tg) == T_RECORD_REF)
      tg = tree_value(tg);
   if (tree_kind(tg) == T_REF && tree_ident(tg) == wc->id)
      wc->n++;
}

// SOUNDNESS guard: an array/vector signal written by >1 distinct driver
// (process / concurrent assign) where at least one write is PARTIAL
// (indexed/slice/field) mis-composes.  Each driver is collected with its OWN
// whole-array hold temp (g0p<pidx>_<sig>, see r2_collect_cb) and commits the
// WHOLE wire, so the disjoint element writes CONTEND across drivers instead
// of composing — r2_sel_nested lowers each write to a correct bit-range
// WITHIN a process, but the per-process whole-wire commit still collides.
// It installs but is silently WRONG (mylex r22_ffirst: d_n and s_n each have
// 7 element-drivers).  A SINGLE driver writing many disjoint elements
// (mylex r31_shiftvec: one clocked shift over sr(0..3)) composes fine — do
// NOT decline that.  So the discriminator is: an array base written PARTIALLY
// somewhere AND written by more than one distinct driver process.  Detect and
// decline to the text path (soundness first); installing it correctly
// (per-slice cross-process commit) is a later coverage gap.
typedef struct { ident_t *ids; int n, cap; bool capped; } r2_pset_t;

static void r2_partial_target_cb(tree_t t, void *ctx)
{
   r2_pset_t *ps = (r2_pset_t *)ctx;
   const tree_kind_t k = tree_kind(t);
   if (k != T_SIGNAL_ASSIGN && k != T_DEPOSIT)
      return;                            // signal drivers only (not variables)
   tree_t tg = tree_target(t);
   bool partial = false;
   while (tree_kind(tg) == T_ARRAY_SLICE || tree_kind(tg) == T_ARRAY_REF
          || tree_kind(tg) == T_RECORD_REF) {
      partial = true;
      tg = tree_value(tg);
   }
   if (!partial || tree_kind(tg) != T_REF || !tree_has_ref(tg))
      return;
   ident_t id = tree_ident(tg);
   for (int i = 0; i < ps->n; i++)
      if (ps->ids[i] == id)
         return;
   if (ps->n == ps->cap) {
      if (ps->cap >= 4096) { ps->capped = true; return; }
      ps->cap = ps->cap ? ps->cap * 2 : 16;
      ps->ids = xrealloc_array(ps->ids, ps->cap, sizeof(ident_t));
   }
   ps->ids[ps->n++] = id;
}

// per-process counts, per candidate signal: partial WRITES (assigns whose
// target base is the signal) and total partial REFS (every indexed/slice/field
// reference to it — targets and reads alike).  Because tree_visit also lands on
// each assign target's own array-ref node, a single-index target contributes
// one ref that exactly offsets its write, so refs > writes iff the process
// partial-READS the signal in a value position.
typedef struct { ident_t *ids; int *refs; int *writes; int n; } r2_wr_scan_t;

static void r2_wr_scan_cb(tree_t t, void *ctx)
{
   r2_wr_scan_t *w = (r2_wr_scan_t *)ctx;
   const tree_kind_t k = tree_kind(t);
   if (k == T_ARRAY_REF || k == T_ARRAY_SLICE || k == T_RECORD_REF) {
      tree_t b = t;
      while (tree_kind(b) == T_ARRAY_SLICE || tree_kind(b) == T_ARRAY_REF
             || tree_kind(b) == T_RECORD_REF)
         b = tree_value(b);
      if (tree_kind(b) == T_REF && tree_has_ref(b)) {
         ident_t id = tree_ident(b);
         for (int j = 0; j < w->n; j++)
            if (w->ids[j] == id) { w->refs[j]++; break; }
      }
      return;
   }
   if (k == T_SIGNAL_ASSIGN || k == T_DEPOSIT) {
      tree_t tg = tree_target(t);
      bool partial = false;
      while (tree_kind(tg) == T_ARRAY_SLICE || tree_kind(tg) == T_ARRAY_REF
             || tree_kind(tg) == T_RECORD_REF) { partial = true; tg = tree_value(tg); }
      if (partial && tree_kind(tg) == T_REF && tree_has_ref(tg)) {
         ident_t id = tree_ident(tg);
         for (int j = 0; j < w->n; j++)
            if (w->ids[j] == id) { w->writes[j]++; break; }
      }
   }
}

// returns the id of an array signal that is partial-written by >1 distinct
// driver AND partial-READ by at least one of those writing drivers (a
// combinational chain THROUGH the array across drivers) — the shape that the
// per-process whole-array hold temp mis-composes.  Disjoint slices with no
// cross-driver read (mylex r4_slice_arm: two halves, read only in a separate
// non-writing process) install correctly and are NOT declined; a single driver
// writing many elements (r31_shiftvec) has only one writer and is NOT declined.
static ident_t r2_multi_driver_array(tree_t block)
{
   r2_pset_t ps = { .ids = NULL, .n = 0, .cap = 0, .capped = false };
   tree_visit(block, r2_partial_target_cb, &ps);
   if (ps.capped)                        // pathological: keep today's behavior
      warnf("vhdl2rtlil: multi-driver guard skipped (>4096 sliced signals)");
   if (ps.n == 0 || ps.capped) {
      free(ps.ids);
      return NULL;
   }
   int  *nwriter   = xcalloc_array(ps.n, sizeof(int));
   bool *crossread = xcalloc_array(ps.n, sizeof(bool));
   const int nst = tree_stmts(block);
   for (int i = 0; i < nst; i++) {
      tree_t st = tree_stmt(block, i);
      if (tree_kind(st) != T_PROCESS)
         continue;
      int *refs   = xcalloc_array(ps.n, sizeof(int));
      int *writes = xcalloc_array(ps.n, sizeof(int));
      r2_wr_scan_t w = { .ids = ps.ids, .refs = refs, .writes = writes,
                         .n = ps.n };
      tree_visit(st, r2_wr_scan_cb, &w);
      for (int j = 0; j < ps.n; j++)
         if (writes[j] > 0) {
            nwriter[j]++;
            if (refs[j] > writes[j])       // a value-position partial read
               crossread[j] = true;
         }
      free(refs);
      free(writes);
   }
   ident_t bad = NULL;
   for (int j = 0; j < ps.n; j++)
      if (nwriter[j] > 1 && crossread[j]) { bad = ps.ids[j]; break; }
   free(nwriter);
   free(crossread);
   free(ps.ids);
   return bad;
}

bool vhdl2rtlil_module(const void *api_, tree_t block, const char *modname)
{
   const gsm_rtlil_api_t *api = (const gsm_rtlil_api_t *)api_;
   g_r2_census = getenv("NVC_ACCEL_RTLIL_CENSUS") != NULL;
   if (g_r2_census)
      api = &g_r2_null_api;   // dry walk: build nothing, decline at the end
   g_r2 = api;
   g_r2_modname = modname;
   g_r2_pidx = -1;
   g_r2_cur = NULL;
   g_r2_site = "?";
   g_r2_ntally = 0;
   g_r2_tmp = 0;
   g_r2_fail = 0;
   g_r2_nmems = 0;
   g_r2_why[0] = '\0';
   if (g_r2_census) {
      notef("vhdl2rtlil-census: begin %s", modname);
      r2_census_arm_signals();
   }

   {  tree_t inner = vhdl2vlog_comp_inner(block);
      if (inner != NULL)
         block = inner;
   }
   if (!block_types_synth(block))
      return false;

   build_reg_set(block);

   // constant-driven signals: a lone `s <= <constant>` concurrent assign
   // whose target has no other writer anywhere in the block
   g_r2_ncsig = 0;
   g_r2_ncfn = 0;
   g_r2_expr_depth = 0;
   r2_subst_reset();
   {
      const int nst = tree_stmts(block);
      for (int i = 0; i < nst && g_r2_ncsig < 64; i++) {
         tree_t p0 = tree_stmt(block, i);
         if (tree_kind(p0) != T_PROCESS)
            continue;
         tree_t p = proc_body(p0);
         tree_t only = NULL;
         int cnt = 0;
         for (int j = 0; j < tree_stmts(p); j++) {
            tree_t s = tree_stmt(p, j);
            if (tree_kind(s) == T_WAIT)
               continue;
            only = s;
            cnt++;
         }
         if (cnt != 1 || tree_kind(only) != T_SIGNAL_ASSIGN
             || tree_waveforms(only) != 1
             || !tree_has_value(tree_waveform(only, 0)))
            continue;
         tree_t tg = tree_target(only);
         if (tree_kind(tg) != T_REF || !tree_has_ref(tg)
             || tree_kind(tree_ref(tg)) != T_SIGNAL_DECL)
            continue;
         int64_t v;
         if (!r2_eval_int(tree_value(tree_waveform(only, 0)), &v))
            continue;
         r2_csig_t *c = &g_r2_csig[g_r2_ncsig++];
         c->id = tree_ident(tg);
         c->v = v;
      }
      for (int i = 0; i < g_r2_ncsig; i++) {
         r2_wcount_t wc = { .id = g_r2_csig[i].id, .n = 0 };
         tree_visit(block, r2_wcount_cb, &wc);
         if (wc.n != 1)
            g_r2_csig[i--] = g_r2_csig[--g_r2_ncsig];
      }
   }

   // subset guards: no design functions, no hoisted process variables,
   // no memory-shaped signals
   const int ndecls = tree_decls(block);
   g_r2_nfuncs = 0;
   g_r2_inline_depth = 0;
   for (int i = 0; i < ndecls; i++) {
      tree_t d = tree_decl(block, i);
      if (tree_kind(d) == T_FUNC_BODY && !fn_is_builtin(istr(tree_ident(d)))) {
         // a straight-line pure body INLINES at its call sites; anything
         // else still declines the module
         char fw[32] = "";
         if (r2_func_inlinable(d, fw, sizeof fw)) {
            if (g_r2_nfuncs == g_r2_cfuncs) {
               g_r2_cfuncs = g_r2_cfuncs ? g_r2_cfuncs * 2 : 32;
               g_r2_funcs = xrealloc_array(g_r2_funcs, g_r2_cfuncs,
                                           sizeof(tree_t));
            }
            g_r2_funcs[g_r2_nfuncs++] = d;
            continue;
         }
         if (r2_cfn_admit(d))
            continue;   // evaluates at call sites with constant actuals
         {
            char why[96];
            snprintf(why, sizeof why, "function %s %s",
                     id_base(istr(tree_ident(d))), fw);
            R2_DECLINE(why);
         }
         if (g_r2_census)
            continue;   // the call sites will report the uninlined calls
         goto declined;
      }
   }

   // soundness: disjoint element writes to one array signal from >1 driver
   // contend through the per-process whole-array hold temp (installs-wrong)
   {
      ident_t bad = r2_multi_driver_array(block);
      if (bad != NULL) {
         char why[96];
         snprintf(why, sizeof why, "multi-driver-array %s", vid(bad));
         R2_DECLINE(why);
         if (!g_r2_census)
            goto declined;
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
         goto declined;
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
      unsigned mnw, mew;
      if (mem_shape(ty, &mnw, &mew) && !r2_wire_array(block, d)) {
         // memory-shaped: qualify by USAGE exactly as the text path does
         // (every reference indexed or a whole-array positional aggregate)
         if (type_is_integer(type_elem(ty))) {
            R2_DECLINE("int-mem");
            goto declined;
         }
         if (tree_has_value(d)) {
            // the TEXT path drops memory initializers outright (emits the
            // bare reg array) — match it for a uniform (others => ...)
            // aggregate; real per-element contents still decline
            tree_t iv = tree_value(d);
            if (tree_kind(iv) == T_AGGREGATE && tree_assocs(iv) == 1
                && tree_subkind(tree_assoc(iv, 0)) == A_OTHERS)
               ;   // uniform power-on fill: drop, as text does
            else {
               R2_DECLINE("mem-init");
               goto declined;
            }
         }
         mem_scan_t sc = { .decl = d, .refs = 0, .indexed = 0, .agg = 0 };
         for (int si = 0; si < tree_stmts(block); si++)
            tree_visit(tree_stmt(block, si), mem_scan_cb, &sc);
         // a comb process reading the memory names it in its sensitivity
         // list (`process (raddr, ram)`): that reference is neither an
         // access nor a write — discount it from both censuses
         // (VX_dp_ram, the last Tier B decline)
         r2_sens_t sens = { .decl = d, .n = 0 };
         tree_visit(block, r2_sens_cb, &sens);
         sc.refs -= sens.n;
         if (sc.refs == 0 || sc.refs != sc.indexed + sc.agg) {
            // strict form failed: qualify the NBA-shadow idiom on the SAME
            // census the text path uses (the walker's alias machinery then
            // handles the bind/commit and routes shadow writes to memwr)
            bool shadow_ok = false;
            if (sc.refs > 0) {
               shadow_find_t sf = { .sig = d, .var = NULL };
               tree_visit(block, shadow_find_cb, &sf);
               if (getenv("NVC_RTLIL_DEBUG") != NULL)
                  fprintf(stderr, "r2 shadow-find %s: var %s\n",
                          istr(tree_ident(d)),
                          sf.var ? istr(tree_ident(sf.var)) : "(none)");
               if (sf.var != NULL) {
                  shadow_scan_t ss = { .sig = d, .var = sf.var };
                  tree_visit(block, shadow_scan_cb, &ss);
                  shadow_ok = (ss.ncopy == 1 && ss.nwb == 1
                               && ss.v_ref == 2 + ss.v_arr
                               && ss.v_arr == ss.nwrite
                               && ss.s_ref == 2 + ss.s_arr + sens.n);
                  if (getenv("NVC_RTLIL_DEBUG") != NULL)
                     fprintf(stderr, "r2 shadow %s: copy %d wb %d nwrite %d "
                             "v_ref %d v_arr %d s_ref %d s_arr %d | strict "
                             "refs %d indexed %d agg %d | sens %d\n",
                             istr(tree_ident(d)), ss.ncopy, ss.nwb,
                             ss.nwrite, ss.v_ref, ss.v_arr, ss.s_ref,
                             ss.s_arr, sc.refs, sc.indexed, sc.agg, sens.n);
               }
            }
            if (!shadow_ok) {
               R2_DECLINE("mem-usage");
               goto declined;
            }
         }
         if (g_r2_nmems >= 16) {
            R2_DECLINE("mem-count");
            goto declined;
         }
         r2_mem_t *mm = &g_r2_mems[g_r2_nmems++];
         mm->id = tree_ident(d);
         snprintf(mm->vname, sizeof mm->vname, "%s", vid(tree_ident(d)));
         mm->width = (int)mew;
         mm->size = (int)mnw;
         if (api->memory(mm->vname, mm->width, mm->size) != 0) {
            R2_DECLINE("memory");
            goto declined;
         }
         continue;   // a Memory, not a wire
      }
      if (!type_const_bounds(ty)) {
         R2_DECLINE("sig-width");
         goto declined;
      }
      const int w = (int)type_width(ty);
      const bool is_r = is_reg(block, d);
      char init[R2_SPEC];
      const char *initbits = NULL;
      char bits[R2_SPEC];
      if (is_r && tree_has_value(d)) {
         if (!r2_const(tree_value(d), init, sizeof init, w)) {
            char why[96];
            tree_t iv0 = tree_value(d);
            if (tree_kind(iv0) == T_AGGREGATE && tree_assocs(iv0) > 0) {
               tree_t a0 = tree_assoc(iv0, 0);
               snprintf(why, sizeof why,
                        "reg-init(agg n=%d sk=%d vk=%d)",
                        tree_assocs(iv0), (int)tree_subkind(a0),
                        (int)tree_kind(tree_value(a0)));
            }
            else
               snprintf(why, sizeof why, "reg-init(k=%d w=%d)",
                        (int)tree_kind(iv0), w);
            R2_DECLINE(why);
            goto declined;
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
            goto declined;
         }
         if (api->connect(vid(tree_ident(d)), v) != 0)
            return false;
      }
   }

   // concurrent statements
   const int nstmts = tree_stmts(block);
   int pidx = 0;
   for (int i = 0; i < nstmts && (g_r2_fail == 0 || g_r2_census); i++) {
      tree_t s = tree_stmt(block, i);
      g_r2_cur = s;
      g_r2_site = "?";
      g_r2_pvts = NULL;   // no process scope for concurrent expressions
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
         {
            const int before = g_r2_fail;
            bool pok;
            if (g_r2_census) {
               int sig;
               if ((sig = sigsetjmp(g_r2_jmp, 1)) == 0) {
                  g_r2_jmp_armed = 1;
                  pok = r2_process(s, pidx);
                  g_r2_jmp_armed = 0;
               }
               else {
                  char why[48];
                  snprintf(why, sizeof why, "CRASH(sig%d)", sig);
                  R2_DECLINE(why);   // g_r2_cur = the statement being walked
                  g_r2_case_depth = 0;
                  g_r2_inline_depth = 0;
                  g_r2_nalias = 0;
                  pok = false;
               }
               pidx++;
            }
            else
               pok = r2_process(s, pidx++);
            if (!pok) {
               g_r2_cur = s;
               R2_DECLINE(g_r2_fail == before ? "process-silent" : "process");
            }
         }
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
            char conns[16384];
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
         {
            char why[48];
            snprintf(why, sizeof why, "conc-kind %d", (int)tree_kind(s));
            R2_DECLINE(why);
         }
         break;
      }
   }

 declined:
   if (g_r2_census) {
      int total = 0;
      for (int i = 0; i < g_r2_ntally; i++) {
         total += g_r2_tally[i].n;
         notef("vhdl2rtlil-census: tally %s: %5d x %s", modname,
               g_r2_tally[i].n, g_r2_tally[i].key);
      }
      notef("vhdl2rtlil-census: module %s: %d decline(s), %d distinct, "
            "%d process(es) — census only, using the text path",
            modname, total, g_r2_ntally, pidx);
      return false;
   }
   if (g_r2_fail > 0) {
      if (getenv("NVC_ACCEL_RTLIL_PROBE") == NULL)
         warnf("vhdl2rtlil: '%s' declined (%s) — using the text path",
               modname, g_r2_why);
      return false;
   }
   return true;
}

// The null builder for a dry walk outside census mode: rt/model.c probes
// a subtree the TEXT emitter declined through it (in a fork child) before
// admitting the subtree on the walker alone.
const void *vhdl2rtlil_null_api(void)
{
   return &g_r2_null_api;
}
