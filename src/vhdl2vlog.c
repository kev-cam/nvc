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
#include <string.h>
#include <ctype.h>
#include <inttypes.h>

static void emit_expr(FILE *f, tree_t e);
static void emit_seq(FILE *f, tree_t s, int ind);

// Best-effort fidelity gate: any construct we can't faithfully translate bumps
// this. If it's non-zero at the end, the emitted Verilog is NOT trustworthy and
// the caller must DECLINE to accelerate this leaf (it stays in the nvc sim).
// Emitting wrong-but-parseable Verilog would silently corrupt results — worse
// than not accelerating.
static int g_unhandled = 0;

static void tab(FILE *f, int n) { for (int i = 0; i < n; i++) fputc(' ', f); }

// VHDL identifiers are case-insensitive uppercase; keep them verbatim (Verilog
// is case-sensitive but consistent), just strip any library/path prefix.
static const char *vid(ident_t id)
{
   const char *s = istr(id);
   const char *dot = strrchr(s, '.');
   return dot ? dot + 1 : s;
}

// bit width of a type as a Verilog range prefix ("[3:0] " or "" for 1-bit)
static void emit_range(FILE *f, type_t type)
{
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

static void emit_lit(FILE *f, tree_t e)
{
   int64_t i;
   if (folded_int(e, &i)) { fprintf(f, "%"PRIi64, i); return; }
   // std_logic enum literal '0'/'1' etc.
   const char *s = istr(tree_ident(e));
   if (strcmp(s, "'0'") == 0) fputs("1'b0", f);
   else if (strcmp(s, "'1'") == 0) fputs("1'b1", f);
   else { g_unhandled++; fprintf(f, "/*lit %s*/0", s); }
}

static void emit_expr(FILE *f, tree_t e)
{
   switch (tree_kind(e)) {
   case T_REF:
      {
         const char *nm = vid(tree_ident(e));
         if (strcmp(nm, "'0'") == 0) fputs("1'b0", f);
         else if (strcmp(nm, "'1'") == 0) fputs("1'b1", f);
         else fputs(nm, f);
      }
      break;
   case T_LITERAL:
      emit_lit(f, e);
      break;
   case T_FCALL:
      {
         const char *fn = istr(tree_ident(e));
         const char *op = vlog_op(fn);
         const int nparams = tree_params(e);
         if (op != NULL && nparams == 2) {
            fputc('(', f);
            emit_expr(f, tree_value(tree_param(e, 0)));
            fprintf(f, " %s ", op);
            emit_expr(f, tree_value(tree_param(e, 1)));
            fputc(')', f);
         }
         else if (op != NULL && nparams == 1) {
            fprintf(f, "%s(", op);
            emit_expr(f, tree_value(tree_param(e, 0)));
            fputc(')', f);
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
      emit_expr(f, tree_value(e));   // numeric/std_logic_vector casts: no-op in Verilog
      break;
   case T_ARRAY_REF:
      emit_expr(f, tree_value(e));
      if (tree_params(e) > 0) {
         fputc('[', f);
         emit_expr(f, tree_value(tree_param(e, 0)));
         fputc(']', f);
      }
      break;
   case T_AGGREGATE:
      // Not faithfully translated — decline rather than silently emit 0.
      g_unhandled++; fputs("/*agg*/0", f);
      break;
   default:
      g_unhandled++; fprintf(f, "/*?expr k=%d*/0", tree_kind(e));
      break;
   }
}

// detect a clocked process: a single wrapping `if rising_edge(clk)` and return
// the clock ref; else NULL (treat as combinational).
static tree_t clock_of(tree_t proc, tree_t *body_if)
{
   // scan process stmts for `if rising_edge(clk)` (ignoring the sensitivity wait)
   const int n = tree_stmts(proc);
   for (int i = 0; i < n; i++) {
      tree_t s = tree_stmt(proc, i);
      if (tree_kind(s) != T_IF) continue;
      tree_t c = tree_cond(s, 0);
      tree_t test = tree_has_value(c) ? tree_value(c) : NULL;
      if (test == NULL || tree_kind(test) != T_FCALL) continue;
      const char *fn = vid(tree_ident(test));
      if ((strcasecmp(fn, "RISING_EDGE") == 0 || strcasecmp(fn, "FALLING_EDGE") == 0)
          && tree_params(test) >= 1) {
         *body_if = c;
         return tree_value(tree_param(test, 0));
      }
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
         fputs(" <= ", f);
         if (tree_kind(s) == T_SIGNAL_ASSIGN && tree_waveforms(s) > 0)
            emit_expr(f, tree_value(tree_waveform(s, 0)));
         else
            emit_expr(f, tree_value(s));
         fputs(";\n", f);
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
            const int nst = tree_stmts(c);
            for (int j = 0; j < nst; j++) emit_seq(f, tree_stmt(c, j), ind + 2);
            tab(f, ind); fputs("end\n", f);
         }
      }
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
   tree_t body_if = NULL;
   tree_t clk = clock_of(p, &body_if);
   if (clk != NULL) {
      fputs("  always @(posedge ", f);
      emit_expr(f, clk);
      fputs(") begin\n", f);
      const int nst = tree_stmts(body_if);
      for (int i = 0; i < nst; i++) emit_seq(f, tree_stmt(body_if, i), 4);
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
         for (int i = 0; i < nst; i++) emit_seq(f, tree_stmt(p, i), 4);
         fputs("  end\n", f);
      }
   }
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
      {
         fprintf(f, "  %s %s (", vid(tree_ident2(s)), vid(tree_ident(s)));
         const int nparams = tree_params(s);
         for (int i = 0; i < nparams; i++) {
            tree_t p = tree_param(s, i);
            if (i > 0) fputs(", ", f);
            if (tree_subkind(p) == P_NAMED) {
               fputc('.', f);
               emit_expr(f, tree_name(p));
               fputc('(', f);
            }
            emit_expr(f, tree_value(p));
            if (tree_subkind(p) == P_NAMED) fputc(')', f);
         }
         fputs(");\n", f);
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
      if (type_is_character_array(t)) return false;   // strings
      return type_synth_ok(type_elem(t));
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
      if (!type_synth_ok(tree_type(tree_port(block, i)))) return false;
   for (int i = 0; i < tree_decls(block); i++)
      if (!decl_type_synth(tree_decl(block, i))) return false;
   for (int i = 0; i < tree_stmts(block); i++) {
      tree_t s = tree_stmt(block, i);
      if (tree_kind(s) != T_PROCESS) continue;
      for (int j = 0; j < tree_decls(s); j++)
         if (!decl_type_synth(tree_decl(s, j))) return false;
   }
   return true;
}

bool vhdl2vlog(tree_t block, const char *modname, const char *path)
{
   g_unhandled = 0;

   // Decline non-synthesizable leaves up front (see block_types_synth): a wrong
   // but parseable model would silently corrupt results.
   if (!block_types_synth(block))
      return false;

   FILE *f = fopen(path, "w");
   if (f == NULL) { warnf("vhdl2vlog: cannot open %s", path); return false; }

   fprintf(f, "// auto-generated from nvc elaborated VHDL by vhdl2vlog\n");
   fprintf(f, "module %s (\n", modname);
   const int nports = tree_ports(block);
   for (int i = 0; i < nports; i++) {
      tree_t p = tree_port(block, i);
      const port_mode_t mode = tree_subkind(p);
      fprintf(f, "  %s ", mode == PORT_OUT || mode == PORT_INOUT ? "output" : "input");
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
   for (int i = 0; i < nstmts; i++)
      emit_stmt(f, tree_stmt(block, i));

   fputs("endmodule\n", f);
   fclose(f);
   return g_unhandled == 0;   // faithful translation only
}
