//
//  libfederation_bridge.cpp — runtime bridge for federated simulation.
//
//  Hosts a Verilator peer model (the testbench + dummy DUT, compiled by
//  Verilator) inside the nvc process, and exposes the foreign functions the
//  federation resolver (sv2vhdl_resolver._gen_fed_vhdl) calls:
//
//      fed_eval()                 -- step the peer model one delta (eval)
//      fed_put_int(name, val)     -- write a peer signal  (nvc DUT out -> peer)
//      fed_get_int(name)          -- read  a peer signal  (peer -> nvc DUT in)
//
//  Signals are addressed BY NAME via Verilator VPI (the dummy-DUT ports are
//  emitted `verilator public_flat_rw`), so the bridge is design-agnostic; it is
//  compiled and linked per design against the generated V<peer> model.
//
//  First runnable version uses an integer value interface (peer signals here are
//  <=64-bit); the std_logic_vector/string VHPIDIRECT ABI used by federation_pkg
//  is the next refinement (map fed_put/fed_get -> these via a thin shim).
//
//  Build (per design):
//      verilator --cc --vpi --build -Mdir vp <peer>.sv      # -> vp/V<peer>__ALL.a
//      g++ -shared -fPIC -I vp -I$(VERILATOR_ROOT)/include \
//          libfederation_bridge.cpp vp/V<peer>__ALL.a \
//          $(VERILATOR_ROOT)/include/verilated.cpp \
//          $(VERILATOR_ROOT)/include/verilated_vpi.cpp \
//          -o libfederation_bridge.so
//
#include "verilated.h"
#include "verilated_vpi.h"
#include FED_PEER_HEADER            // -DFED_PEER_HEADER='"Vfed_peer.h"' at build
#include <cstdint>
#include <cstdio>

#ifndef FED_PEER_CLASS
#define FED_PEER_CLASS Vfed_peer
#endif

static VerilatedContext *g_ctx = nullptr;
static FED_PEER_CLASS   *g_top = nullptr;

extern "C" void fed_init(void)
{
   if (g_top != nullptr) return;
   g_ctx = new VerilatedContext;
   g_top = new FED_PEER_CLASS(g_ctx);
   g_top->eval();                       // settle initial values
}

extern "C" void fed_eval(void)
{
   if (g_top == nullptr) fed_init();
   g_top->eval();
   VerilatedVpi::callValueCbs();        // fire VPI value-change callbacks
}

static vpiHandle handle_of(const char *name)
{
   vpiHandle h = vpi_handle_by_name((PLI_BYTE8 *)name, nullptr);
   if (h == nullptr)
      fprintf(stderr, "federation: no peer signal '%s'\n", name);
   return h;
}

extern "C" void fed_put_int(const char *name, uint64_t val)
{
   if (g_top == nullptr) fed_init();
   vpiHandle h = handle_of(name);
   if (h == nullptr) return;
   s_vpi_value v;
   v.format = vpiIntVal;
   v.value.integer = (PLI_INT32)val;
   vpi_put_value(h, &v, nullptr, vpiNoDelay);
}

extern "C" uint64_t fed_get_int(const char *name)
{
   if (g_top == nullptr) fed_init();
   vpiHandle h = handle_of(name);
   if (h == nullptr) return 0;
   s_vpi_value v;
   v.format = vpiIntVal;
   vpi_get_value(h, &v);
   return (uint64_t)(uint32_t)v.value.integer;
}

extern "C" void fed_final(void)
{
   if (g_top) { g_top->final(); delete g_top; g_top = nullptr; }
   if (g_ctx) { delete g_ctx; g_ctx = nullptr; }
}

// --------------------------------------------------------------------------
//  Integer-index ABI — exactly what federation_pkg declares:
//     procedure fed_put(idx : integer; val : integer)
//     impure function fed_get(idx : integer) return integer
//  fed_names[] (net-index -> peer leaf signal name) is generated per design by
//  federation_resolver.gen_names_header(); the bridge prefixes the peer
//  hierarchy path. Pure-integer boundary -> no string/std_logic_vector ABI.
// --------------------------------------------------------------------------
#ifdef FED_NAMES_HEADER
#include FED_NAMES_HEADER
#else
static const char *fed_names[] = { "din", "dout" };   // self-test default
static const int   fed_nnames  = 2;
#endif
#ifndef FED_PEER_PREFIX
#define FED_PEER_PREFIX "TOP.fed_peer."                // peer hierarchy prefix
#endif

static const char *full_name(int idx, char *buf, unsigned n)
{
   if (idx < 0 || idx >= fed_nnames) return nullptr;
   snprintf(buf, n, "%s%s", FED_PEER_PREFIX, fed_names[idx]);
   return buf;
}

extern "C" void fed_put(int idx, int val)
{
   char b[256];
   const char *nm = full_name(idx, b, sizeof b);
   if (nm) fed_put_int(nm, (uint64_t)(uint32_t)val);
}

extern "C" int fed_get(int idx)
{
   char b[256];
   const char *nm = full_name(idx, b, sizeof b);
   return nm ? (int)fed_get_int(nm) : 0;
}

#ifdef FED_TEST
// Mechanics self-test: drive a peer input, eval, read the peer output.
// Expects a peer with public signals "TOP.fed_peer.din" / ".dout" where
// dout = din + 1 (see the test peer).
int main(void)
{
   const char *IN  = "TOP.fed_peer.din";
   const char *OUT = "TOP.fed_peer.dout";
   int fail = 0;
   for (uint64_t x = 0; x < 8; x++) {
      fed_put_int(IN, x);
      fed_eval();
      uint64_t y = fed_get_int(OUT);
      printf("  put %s=%llu  -> eval -> get %s=%llu  (expect %llu) %s\n",
             IN, (unsigned long long)x, OUT, (unsigned long long)y,
             (unsigned long long)((x + 1) & 0xf),
             (y == ((x + 1) & 0xf)) ? "ok" : (fail = 1, "FAIL"));
   }
   // also exercise the integer-INDEX ABI (idx 0=din, 1=dout via fed_names[])
   printf("  -- integer-index ABI (fed_put/fed_get by net index) --\n");
   for (int x = 0; x < 4; x++) {
      fed_put(0, x);            // idx 0 -> din
      fed_eval();
      int y = fed_get(1);       // idx 1 -> dout
      printf("  fed_put(0,%d) -> eval -> fed_get(1)=%d (expect %d) %s\n",
             x, y, (x + 1) & 0xf, (y == ((x + 1) & 0xf)) ? "ok" : (fail = 1, "FAIL"));
   }
   fed_final();
   printf(fail ? "FED bridge mechanics: FAIL\n" : "FED bridge mechanics: PASS\n");
   return fail;
}
#endif
