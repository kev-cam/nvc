#!/usr/bin/env python3
"""High-fanout resolver fixture (#74): 1 driving tran endpoint, N-1 readers.

Variant A: net routed through sv2vhdl_resolver.py generated resolver
           entities (today's copy/deposit machinery): N processes, each
           sensitive to the other N-1 drivers, resolving an (N-1)-vector
           per event -- the O(N^2) shape.
Variant B: leaves read the driver directly (zero-copy ideal = what the
           fuse/alias construct must reach automatically).

Usage: test_fanout_resolver.py [N] [CYCLES] [OUTDIR]
Then in OUTDIR (use -M 128m to analyse variant A at N>=256):
  nvc --work=wa -a fanout_dut.vhd fanout_dut_rn_0.vhd fanout_dut_wrapper.vhd
  nvc --work=wa -e resolved_fanout_dut && nvc --work=wa -r resolved_fanout_dut
  nvc --work=wb -a fanout_ideal.vhd && nvc --work=wb -e fanout_ideal
  nvc --work=wb -r fanout_ideal

Measured 2026-08-17 (instructions, medians of 3):
  N=16 /20k edges: A 3.31G  B 1.79G  (1.85x, 76k inst/edge resolver delta)
  N=64 /20k edges: A 6.54G  B 2.98G  (2.19x, 178k inst/edge)
  N=256/5k  edges: A 17.0G  B 4.7G   (3.6x,  2.46M inst/edge)
The alias/fuse acceptance bar: variant A after the transform must
approach variant B.
"""
import sys, os
N = int(sys.argv[1]) if len(sys.argv) > 1 else 64
CYCLES = int(sys.argv[2]) if len(sys.argv) > 2 else 200000
OUT = sys.argv[3] if len(sys.argv) > 3 else "/tmp/fan74"
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sv2vhdl_resolver as R

os.makedirs(OUT, exist_ok=True)

# ---------------- DUT (shared by both variants via generic) ------------
dut = []
dut.append("library ieee; use ieee.std_logic_1164.all;")
dut.append("entity fanout_dut is end;")
dut.append("architecture a of fanout_dut is")
dut.append("  signal src : std_logic := '0';   -- endpoint 0 drive value")
for i in range(N):
    dut.append(f"  signal drv{i} : std_logic := 'Z';")
    dut.append(f"  signal rcv{i} : std_logic := '0';")
dut.append("  type iv is array (0 to %d) of natural;" % (N-1))
dut.append("  signal counts : iv := (others => 0);")
dut.append("begin")
dut.append("  drv: process")
dut.append("  begin")
dut.append(f"    for c in 1 to {CYCLES} loop")
dut.append("      src <= not src; drv0 <= not src; wait for 1 ns;")
dut.append("    end loop;")
dut.append("    wait for 10 ns;")
dut.append("    report \"CHK \" & integer'image(counts(0)) & \" \" &")
dut.append(f"      integer'image(counts({N-1}));")
dut.append("    -- endpoint 0 sees only the OTHERS (all Z): its rcv never toggles")
dut.append("    assert counts(1) = counts(%d) report \"NONUNIFORM\" severity failure;" % (N-1))
dut.append("    assert counts(1) >= %d report \"TOO FEW EVENTS\" severity failure;" % CYCLES)
dut.append("    report \"FANOUT PASS\";")
dut.append("    wait;")
dut.append("  end process;")
for i in range(N):
    dut.append(f"  leaf{i}: process (rcv{i})")
    dut.append("  begin")
    dut.append(f"    counts({i}) <= counts({i}) + 1;")
    dut.append("  end process;")
dut.append("end architecture;")
open(f"{OUT}/fanout_dut.vhd", "w").write("\n".join(dut) + "\n")

# ---------------- Variant A: resolver-generated net --------------------
# One net: driver endpoint = src (signal), receiver endpoints = rcvI.
# Default carries static modes (endpoint 0 = inout driver, rest = in),
# exercising the degenerate one-alias emission.  FAN_RUNTIME=1 drops the
# modes so the generator falls back to runtime undriven() arbitration.
runtime_form = os.environ.get("FAN_RUNTIME") is not None
def _drv(i):
    d = {"ename": f".fanout_dut.drv{i}", "type": "std_logic"}
    if not runtime_form:
        d["mode"] = "inout" if i == 0 else "in"
    return d
net = {
    "net_name": "fan_net",
    "drivers":   [_drv(i) for i in range(N)],
    "receivers": [{"ename": f".fanout_dut.rcv{i}", "type": "std_logic"}
                  for i in range(N)],
}
files = R.resolve_net([net], "fanout_dut")
for fn, txt in (files or {}).items():
    open(f"{OUT}/{fn}", "w").write(txt)
print("generated:", ", ".join(sorted((files or {}).keys())))

# ---------------- Variant B: zero-copy ideal ---------------------------
dutb = [l.replace("fanout_dut", "fanout_ideal") for l in dut]
dutb = [l.replace("(rcv%d)" % i, "(src)") if ("leaf%d:" % i) in l else l
        for l in dutb for i in [0]]  # placeholder, fix below
# simpler: regenerate with leaves on src
dutb = []
dutb.append("library ieee; use ieee.std_logic_1164.all;")
dutb.append("entity fanout_ideal is end;")
dutb.append("architecture a of fanout_ideal is")
dutb.append("  signal src : std_logic := '0';")
dutb.append("  type iv is array (0 to %d) of natural;" % (N-1))
dutb.append("  signal counts : iv := (others => 0);")
dutb.append("begin")
dutb.append("  drv: process")
dutb.append("  begin")
dutb.append(f"    for c in 1 to {CYCLES} loop")
dutb.append("      src <= not src; wait for 1 ns;")
dutb.append("    end loop;")
dutb.append("    wait for 10 ns;")
dutb.append("    assert counts(0) = counts(%d) and counts(0) >= %d" % (N-1, CYCLES))
dutb.append("      report \"BAD counts\" severity failure;")
dutb.append("    report \"FANOUT PASS\";")
dutb.append("    wait;")
dutb.append("  end process;")
for i in range(N):
    dutb.append(f"  leaf{i}: process (src)")
    dutb.append("  begin")
    dutb.append(f"    counts({i}) <= counts({i}) + 1;")
    dutb.append("  end process;")
dutb.append("end architecture;")
open(f"{OUT}/fanout_ideal.vhd", "w").write("\n".join(dutb) + "\n")
print(f"N={N} cycles={CYCLES} -> {OUT}")
