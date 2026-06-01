#!/usr/bin/env python3
"""
make_federated.py -- generate the artifacts to split a chunk (DUT) out as a
federated node, for general use. One chunk -> two consistent sides:

  PEER side (the rest-of-design / TB instantiates this in place of the chunk):
    dummy_<mod>.v       -- gen_dummy_dut: same ports, no logic; the ports are
                           the bridge tap points (inputs exposed, outputs driven
                           by public regs the bridge pokes from the real chunk).

  DUT side (the chunk's real logic, run as a federation node):
    --backend verilator : verilate the chunk RTL standalone -> a Verilator C++
                          model (eval()+port members). Works for Vortex chunks
                          (which Verilate but don't statemachine-compile). The
                          co-sim harness uses chunk->eval() exactly where the bet
                          harness used sm_eval().
    --backend nvc       : the chunk runs in nvc; federation_nets()/_gen_fed_vhdl
                          generate the resolvers, libfederation_bridge.so bridges.

  Shared (so the integer net-index is consistent across resolver/bridge/VPI):
    fed_names_<mod>.h   -- gen_names_header (bridge index->signal name)
    federation_pkg.vhd  -- the bridge foreign-function package (nvc side)

Usage:
  make_federated.py --module VX_alu_unit \
      --ports 'clk:in:1,reset:in:1,a:in:32,b:in:32,result:out:32,valid:out:1' \
      --rtl VX_alu_unit.sv ... -I rtl -I rtl/libs -D NUM_THREADS=4 \
      --backend verilator --outdir fed_out [--build]
"""
import argparse, os, subprocess, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import federation_resolver as F


def parse_ports(spec):
    ports = []
    for tok in spec.split(","):
        tok = tok.strip()
        if not tok:
            continue
        name, d, w = tok.split(":")
        assert d in ("in", "out"), f"port dir must be in/out: {tok}"
        ports.append({"name": name, "dir": d, "width": int(w)})
    return ports


def main():
    ap = argparse.ArgumentParser(description="generate federation artifacts for a chunk")
    ap.add_argument("--module", required=True, help="chunk top module name")
    ap.add_argument("--ports", required=True, help="comma list of name:in|out:width")
    ap.add_argument("--rtl", nargs="*", default=[], help="chunk RTL source files")
    ap.add_argument("-I", action="append", default=[], dest="incs", help="include dir")
    ap.add_argument("-D", action="append", default=[], dest="defs", help="define NAME=VAL")
    ap.add_argument("--backend", choices=["verilator", "nvc"], default="verilator")
    ap.add_argument("--outdir", default=".")
    ap.add_argument("--build", action="store_true", help="actually run the verilate build")
    a = ap.parse_args()

    ports = parse_ports(a.ports)
    os.makedirs(a.outdir, exist_ok=True)

    dummy = os.path.join(a.outdir, f"dummy_{a.module}.v")
    with open(dummy, "w") as f:
        f.write(F.gen_dummy_dut(a.module, ports) + "\n")
    hdr = os.path.join(a.outdir, f"fed_names_{a.module}.h")
    with open(hdr, "w") as f:
        f.write(F.gen_names_header(ports))
    pkg = os.path.join(a.outdir, "federation_pkg.vhd")
    with open(pkg, "w") as f:
        f.write(F.federation_pkg() + "\n")

    incs = [f"-I{d}" for d in a.incs]
    defs = [f"-D{d}" for d in a.defs]
    print(f"# federation artifacts for '{a.module}' ({len(ports)} boundary ports) -> {a.outdir}/")
    print(f"#   peer dummy DUT : {dummy}")
    print(f"#   bridge names   : {hdr}")
    print(f"#   federation pkg : {pkg}")

    if a.backend == "verilator":
        mdir = os.path.join(a.outdir, f"fed_{a.module}")
        cmd = (["verilator", "--cc", "--vpi", "--build", "-Wno-fatal",
                "-Mdir", mdir, "--top", a.module] + incs + defs + a.rtl)
        print("# federated DUT model (Verilator node) build:")
        print("  " + " ".join(cmd))
        print(f"#   -> {mdir}/V{a.module}__ALL.a   (host in the co-sim harness; chunk->eval())")
        if a.build:
            rc = subprocess.run(cmd).returncode
            print(f"# build rc={rc}; lib: "
                  + (f"{mdir}/V{a.module}__ALL.a" if os.path.exists(f"{mdir}/V{a.module}__ALL.a") else "NONE"))
    else:
        print("# nvc DUT: analyse the chunk, flag boundary via federation_nets(dut_path, ports),")
        print("#   resolve_net() emits the _gen_fed_vhdl resolvers + federation_pkg; build")
        print("#   libfederation_bridge.so (-DFED_NAMES_HEADER=fed_names_%s.h); nvc dlopens it." % a.module)


if __name__ == "__main__":
    main()
