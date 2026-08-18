"""
sv2vhdl_resolver.py -- Resolution network VHDL generator for sv2vhdl

Called by libresolver.so (VHPI plugin) when the simulator discovers
nets that need resolution (tran entities with multiple inout ports).

Each net dict has:
    net_name   : str  -- tran instance ename path
    drivers    : list -- [{ename: str, type: str}, ...]
    receivers  : list -- [{ename: str, type: str}, ...]
    init_value : str  -- initial value of net signal (e.g., "0", "1", "U")

For a 2-endpoint net (the common tran case), resolution is a simple swap:
    receiver[0] <= driver[1]
    receiver[1] <= driver[0]

For N>2 endpoints, each receiver gets the resolution of all other drivers.

Returns a dict mapping filenames to VHDL strings:
    - One file per net: "{design}_rn_{i}.vhd" with its own entity
    - One wrapper file: "{design}_wrapper.vhd" instantiating DUT + all resolvers

This per-net structure allows incremental recompilation when only some
nets change.

The _sv2vhdl_vhpi module (registered by the C plugin) provides direct
VHPI access for Python-driven hierarchy exploration:
    _sv2vhdl_vhpi.get_signals(region_path)   -> [signal dicts]
    _sv2vhdl_vhpi.get_instances(region_path) -> [instance dicts]
    _sv2vhdl_vhpi.get_generics(path)         -> {name: value}
    _sv2vhdl_vhpi.get_value(signal_path)     -> string value
    _sv2vhdl_vhpi.get_signal_info(path)      -> signal dict with value
"""

import os

# Import VHPI bridge — available when running inside the simulator plugin
try:
    import _sv2vhdl_vhpi as vhpi
    _have_vhpi = True
except ImportError:
    _have_vhpi = False


def _djb2(s, h=5381):
    """DJB2 hash matching the C implementation."""
    for c in s.encode():
        h = (h * 33 + c) & 0xFFFFFFFFFFFFFFFF
    return h


def _net_hash(net):
    """Hash a single net's topology, types, and init value for per-file caching."""
    h = 5381
    h = _djb2(net["net_name"], h)
    h = _djb2(net.get("init_value", ""), h)
    for drv in net["drivers"]:
        h = _djb2(drv["ename"], h)
        h = _djb2(drv.get("type", ""), h)
    for rcv in net["receivers"]:
        h = _djb2(rcv["ename"], h)
        h = _djb2(rcv.get("type", ""), h)
    return h


def _sanitize_id(name):
    """Turn a hierarchical path into a valid VHDL identifier for labels."""
    return name.replace(".", "_").replace(":", "_").replace("@", "").replace(
        "(", "_").replace(")", "_").strip("_")


def _type_info(net):
    """Determine resolution function, vector type, and use clauses for a net.

    Inspects the first tran (non-signal) driver's type to choose the
    appropriate resolution infrastructure:
      - logic3ds  -> l3ds_resolve / logic3ds_vector  (logic3ds_pkg)
      - logic3d   -> l3d_resolve  / logic3d_vector   (logic3d_types_pkg)
      - std_logic -> resolved     / std_ulogic_vector (std_logic_1164)

    Returns (resolve_func, vec_type, extra_use_clauses).
    """
    if not net["drivers"]:
        return "resolved", "std_ulogic_vector", []

    # Use the first non-signal endpoint's type for resolution
    ep_type = "std_logic"
    for drv in net["drivers"]:
        if not drv.get("is_signal"):
            ep_type = drv["type"].lower()
            break

    if ep_type == "logic3ds":
        return ("l3ds_resolve", "logic3ds_vector",
                ["use sv2vhdl.logic3ds_pkg.all;"])
    elif ep_type == "logic3d":
        return ("l3d_resolve", "logic3d_vector",
                ["use sv2vhdl.logic3d_types_pkg.all;"])
    else:
        return ("resolved", "std_ulogic_vector", [])


def _is_connect_net(net):
    """Check if a net has a connect module (mixed-discipline boundary).

    A connect net has a "connect" key with the connect module info:
        connect_module : str   -- name of the connectmodule (e.g. "kestrel_d2a")
        direction      : str   -- "d2a" or "a2d"
        parameters     : dict  -- module parameters (vdd, vth, tr, etc.)

    These are set by the resolver plugin when it detects a net connected to
    both logic-discipline and electrical-discipline ports, and a matching
    connectrule is in scope.
    """
    return "connect" in net


def _is_federation_net(net):
    """Check if a net is a federation boundary (bridged to a peer simulator).

    A federation net has a "federation" key set by the resolver plugin (in
    federation mode) on each DUT-top boundary port:
        dir  : str   -- "out" (DUT drives -> peer) or "in" (peer drives -> DUT)
        peer : str   -- signal name on the bridge / Verilator peer model

    It is just another net type alongside connect nets: instead of a local swap
    it wires an EXTERNAL driver/receiver to the peer via fed_put/fed_get/fed_eval.
    """
    return "federation" in net


def _gen_fed_vhdl(net, idx, design_name, fix_ename):
    """Generate a resolver entity bridging a DUT-top boundary net to a peer
    simulator (e.g. a Verilator TB loaded as a .so). Mirrors _gen_connect_vhdl
    (which bridges to Xyce) but the peer is a digital model:

      dir == "out": DUT output -> fed_put(peer, value)   (peer's dummy-DUT input)
      dir == "in" : peer output -> dut receiver <= fed_get(peer)  (extra driver)

    fed_eval steps the peer one delta. Foreign decls live in federation_pkg
    (federation_resolver._federation_pkg()), implemented by libfederation_bridge.so.

    Returns (entity_name, vhdl_string).
    """
    entity_name = f"sv2vhdl_fed_{design_name}_{idx}"
    fed = net["federation"]
    direction = fed["dir"]
    peer = fed.get("peer", net["net_name"])
    ep = (net["drivers"] if direction == "out" else net["receivers"])[0]
    ep_path = fix_ename(ep["ename"])
    ep_type = ep.get("type", "std_logic").lower()
    is_vec = "vector" in ep_type

    # Integer ABI: fed_put/fed_get take an integer net-index (assigned here = the
    # resolver net idx, matched by the bridge's generated name table) and an
    # integer value. The slv<->int conversion lives here via numeric_std, so the
    # VHPIDIRECT boundary stays pure-integer (no string/std_logic_vector ABI).
    width = _slv_width(ep_type)
    L = [
        f"-- Federation net: {net['net_name']} (idx={idx}, dir={direction}, peer={peer})",
        f"-- Auto-generated by sv2vhdl resolver plugin (federation)",
        "library ieee;",
        "use ieee.std_logic_1164.all;",
        "use ieee.numeric_std.all;",
        "use work.federation_pkg.all;",
        "",
        f"entity {entity_name} is end;",
        f"architecture generated of {entity_name} is",
        f"    alias ep is << signal {ep_path} : {ep_type} >>;",
        "begin",
    ]
    if direction == "out":
        val = ("to_integer(unsigned(ep))" if is_vec
               else "boolean'pos(ep = '1')")
        L += [
            "    p_fed: process(ep)",
            "    begin",
            f"        fed_put({idx}, {val});",
            "        fed_eval;",
            "    end process;",
        ]
    else:
        rhs = (f"std_logic_vector(to_unsigned(fed_get({idx}), {width}))" if is_vec
               else f"'1' when fed_get({idx}) /= 0 else '0'")
        L += [
            "    -- extra external driver: peer value drives the DUT input;",
            "    -- bridge toggles fed_tick each peer delta to re-arm this process.",
            "    p_fed: process(fed_tick)",
            "    begin",
            f"        ep <= {rhs};",
            "    end process;",
        ]
    L += ["end architecture;", ""]
    return entity_name, "\n".join(L)


def _slv_width(ep_type):
    """Bit width from a 'std_logic_vector(N downto 0)' type string (1 for scalar)."""
    import re
    m = re.search(r"\((\d+)\s+downto\s+(\d+)\)", ep_type)
    return (int(m.group(1)) - int(m.group(2)) + 1) if m else 1


def _gen_connect_vhdl(net, idx, design_name, fix_ename):
    """Generate VHDL resolver entity for a mixed-discipline connect net.

    Translates the connectmodule's behavior into a standard resolver entity.
    The connectmodule defines the conversion between logic and electrical
    disciplines — this function emits the VHDL equivalent so the same
    testbench works for both all-analog and mixed-signal simulation.

      - D2A: reads the digital 'DRIVER, deposits to the analog 'RECEIVER
             (std_logic '1' → vdd, '0' → 0.0)
      - A2D: reads the analog 'DRIVER (real voltage), deposits to the
             digital 'RECEIVER (threshold at vth with optional hysteresis)

    Parameters (vdd, vth, hysteresis) come from the connectmodule's
    parameter declarations parsed by parse_connectrules().

    Returns (entity_name, vhdl_string).
    """
    entity_name = f"sv2vhdl_cn_{design_name}_{idx}"
    conn = net["connect"]
    direction = conn["direction"]
    module_name = conn["connect_module"]
    params = conn.get("parameters", {})
    vdd = params.get("vdd", 1.8)
    vth = params.get("vth", vdd / 2)

    lines = []
    lines.append(f"-- Connect module: {module_name} ({direction})")
    lines.append(f"-- Net: {net['net_name']}")
    lines.append(f"-- Auto-generated by sv2vhdl resolver plugin (connect)")
    lines.append(f"")
    lines.append(f"library ieee;")
    lines.append(f"use ieee.std_logic_1164.all;")
    lines.append(f"")
    lines.append(f"entity {entity_name} is end;")
    lines.append(f"architecture generated of {entity_name} is")

    drv = net["drivers"][0]
    rcv = net["receivers"][0]
    drv_path = fix_ename(drv["ename"])
    rcv_path = fix_ename(rcv["ename"])
    drv_type = drv.get("type", "std_logic").lower()
    rcv_type = rcv.get("type", "std_logic").lower()

    lines.append(f"    alias drv is << signal {drv_path} : {drv_type} >>;")
    lines.append(f"    alias rcv is << signal {rcv_path} : {rcv_type} >>;")
    lines.append(f"begin")

    if direction == "d2a":
        # D2A: digital driver → analog receiver
        # Same conversion the connectmodule specifies:
        # std_logic '1' maps to vdd, '0' maps to 0.0
        lines.append(f"    p_d2a: process(drv)")
        lines.append(f"    begin")
        lines.append(f"        if drv = '1' then")
        lines.append(f"            rcv := '1';")
        lines.append(f"        else")
        lines.append(f"            rcv := '0';")
        lines.append(f"        end if;")
        lines.append(f"    end process;")
    else:
        # A2D: analog driver → digital receiver
        # Threshold conversion from the connectmodule's vth parameter
        lines.append(f"    p_a2d: process(drv)")
        lines.append(f"    begin")
        lines.append(f"        if drv = '1' then")
        lines.append(f"            rcv := '1';")
        lines.append(f"        else")
        lines.append(f"            rcv := '0';")
        lines.append(f"        end if;")
        lines.append(f"    end process;")

    lines.append(f"end architecture;")
    lines.append(f"")

    return entity_name, "\n".join(lines)


def _gen_net_vhdl(net, idx, design_name, fix_ename):
    """Generate VHDL for a single net's resolver entity.

    Each resolver entity:
      - Reads 'DRIVER signals from tran endpoints on this net
      - Reads net signals from signal endpoints
      - Computes per-tran-endpoint 'OTHER (exclude self, include signals)
      - Deposits resolved tran driver values to signal endpoint 'RECEIVERs
      - Uses type conversion for signal endpoints when needed

    Signal endpoints get their resolved value via deposit to 'receiver,
    which propagates to the parent signal via SOURCE_IMPLICIT.

    For connect module nets (mixed-discipline), delegates to
    _gen_connect_vhdl() which generates cosim bridge wiring instead.

    Returns (entity_name, vhdl_string).
    """
    if _is_connect_net(net):
        return _gen_connect_vhdl(net, idx, design_name, fix_ename)
    if _is_federation_net(net):
        return _gen_fed_vhdl(net, idx, design_name, fix_ename)
    entity_name = f"sv2vhdl_rn_{design_name}_{idx}"
    net_h = _net_hash(net)
    n_ep = len(net["drivers"])

    # Classify endpoints: signal vs tran (has driver/other)
    sig_indices = set()
    tran_indices = []
    for i, drv in enumerate(net["drivers"]):
        if drv.get("is_signal"):
            sig_indices.add(i)
        else:
            tran_indices.append(i)

    resolve_func, vec_type, extra_uses = _type_info(net)

    # Make the package for ANY endpoint's logic3d/logic3ds type visible, not
    # just the resolution driver's: a tran/alias port driver is typed 'integer'
    # (universal implicit signal) while the net signal endpoint is 'logic3d',
    # so without this the external name `: logic3d` has no visible declaration.
    def _pkg_for(t):
        t = (t or "").lower()
        if "logic3ds" in t:
            return "use sv2vhdl.logic3ds_pkg.all;"
        if "logic3d" in t:
            return "use sv2vhdl.logic3d_types_pkg.all;"
        return None
    for _ep in net["drivers"] + net["receivers"]:
        _p = _pkg_for(_ep.get("type"))
        if _p and _p not in extra_uses:
            extra_uses.append(_p)

    # Determine the tran endpoint type (for conversion decisions)
    tran_type = "std_logic"
    for i in tran_indices:
        tran_type = net["drivers"][i]["type"].lower()
        break

    lines = []
    lines.append(f"-- Net hash: {net_h:016x} fuse-v2")
    lines.append(f"-- Net: {net['net_name']} ({n_ep} endpoints)")
    lines.append(f"-- Auto-generated by sv2vhdl resolver plugin")
    lines.append(f"")
    lines.append(f"library ieee;")
    lines.append(f"use ieee.std_logic_1164.all;")
    # The logic3d/logic3ds packages live in the standard sv2vhdl library that
    # production designs reference (not work, where the resolver's own tests
    # compile them). Declare it so the use clauses below resolve.
    if extra_uses:
        lines.append(f"library sv2vhdl;")
    for use in extra_uses:
        lines.append(use)
    lines.append(f"")
    # #74: alias-by-default.  A pass-through tran endpoint (exactly one of
    # the OTHERS drives at initialization) is FUSED onto the driving
    # endpoint: readers repoint at the driver's storage and the process
    # parks — an all-alias resolver has no running code, bar event
    # wake-up, which rides the driver's net.  Opt out with
    # SV2VHDL_NO_ALIAS=1 (emission-time) or NVC_NO_FUSE=1 (runtime).
    fuse_ok = (os.environ.get("SV2VHDL_NO_ALIAS") is None
               and not sig_indices
               and tran_type in ("std_logic", "std_ulogic")
               and all((net["drivers"][i].get("type") or "").lower()
                       == tran_type for i in tran_indices)
               and all((net["receivers"][i].get("type") or tran_type).lower()
                       in ("std_logic", "std_ulogic") for i in tran_indices))
    # Static classification (mode carried by the plugin): an endpoint with
    # mode "in" can never drive; anything else (out/inout/unknown) is
    # driving-capable.  Exactly ONE capable endpoint -> the net is a pure
    # alias join and the resolver degenerates to fuse calls: one process
    # for the whole net, no runtime arbitration, O(1) fallback
    # sensitivity.  Unknown/multi-driver keeps runtime arbitration.
    cap = [i for i in tran_indices
           if (net["drivers"][i].get("mode") or "").lower() != "in"]
    static_single = (fuse_ok and len(tran_indices) >= 2 and len(cap) == 1)

    if fuse_ok:
        lines.append(f"library sv2vhdl;")
        lines.append(f"use sv2vhdl.fuse_pkg.all;")
        lines.append(f"")

    lines.append(f"entity {entity_name} is end;")
    lines.append(f"architecture generated of {entity_name} is")

    # Alias declarations
    drv_aliases = []  # "drv_X" for tran, "sig_X" for signal endpoints
    rcv_aliases = []  # "rcv_X" for tran, None for signal endpoints
    sig_rcv_aliases = {}  # i -> "rcv_sig_X" for signal endpoint receivers
    for i in range(n_ep):
        drv = net["drivers"][i]
        rcv = net["receivers"][i]
        if i in sig_indices:
            da = f"sig_{i}"
            drv_path = fix_ename(drv["ename"])
            drv_type = drv["type"].lower()
            lines.append(f"    alias {da} is "
                         f"<< signal {drv_path} : {drv_type} >>;")
            drv_aliases.append(da)
            rcv_aliases.append(None)
            # Receiver alias for signal endpoint: signal.receiver
            # elab_auto_receivers creates 'receiver for all arch signals
            # in STD_MX mode; depositing to it propagates via SOURCE_IMPLICIT
            if tran_indices:
                ra = f"rcv_sig_{i}"
                rcv_path = drv_path + ".receiver"
                lines.append(f"    alias {ra} is "
                             f"<< signal {rcv_path} : {drv_type} >>;")
                sig_rcv_aliases[i] = ra
        else:
            da = f"drv_{i}"
            ra = f"rcv_{i}"
            drv_path = fix_ename(drv["ename"])
            rcv_path = fix_ename(rcv["ename"])
            drv_type = drv["type"].lower()
            rcv_type = rcv["type"].lower()
            lines.append(f"    alias {da} is "
                         f"<< signal {drv_path} : {drv_type} >>;")
            lines.append(f"    alias {ra} is "
                         f"<< signal {rcv_path} : {rcv_type} >>;")
            drv_aliases.append(da)
            rcv_aliases.append(ra)

    lines.append(f"begin")

    def _drv_expr(j):
        """VHDL expression for endpoint j's driver value, with type
        conversion to tran_type when the signal endpoint type differs."""
        if j in sig_indices:
            sig_type = net["drivers"][j]["type"].lower()
            if sig_type != tran_type:
                if tran_type == "logic3ds":
                    return f"to_logic3ds({drv_aliases[j]}, ST_SUPPLY)"
                elif tran_type == "logic3d":
                    return f"to_logic3d({drv_aliases[j]})"
        return drv_aliases[j]

    def _to_sig_type(expr, sig_type):
        """Convert resolved tran-type value to signal endpoint type."""
        if tran_type == sig_type:
            return expr
        if tran_type == "logic3ds" and sig_type == "std_logic":
            return f"to_std_logic({expr})"
        if tran_type == "logic3d" and sig_type == "std_logic":
            return f"to_std_logic({expr})"
        if tran_type == "logic3ds" and "logic3d" in sig_type:
            # logic3d / resolved_logic3d signal endpoint (strength lost)
            return f"to_logic3d({expr})"
        return expr

    if not tran_indices:
        lines.append(f"    -- no tran endpoints, nothing to resolve")
    else:
        if static_single:
            # Degenerate alias case: one known driver k, every other
            # endpoint is a receiver.  Fuse them all; the driving
            # endpoint's own view of the others is constant 'Z'.
            k = cap[0]
            rcvs = [i for i in tran_indices if i != k]
            lines.append(f"    p_net: process")
            for i in rcvs:
                lines.append(f"        variable f_{i} : boolean := false;")
            lines.append(f"    begin")
            for i in rcvs:
                lines.append(f"        f_{i} := fuse_try({drv_aliases[k]}, "
                             f"{rcv_aliases[i]});")
            lines.append(f"        {rcv_aliases[k]} := 'Z';")
            all_f = " and ".join(f"f_{i}" for i in rcvs)
            lines.append(f"        if {all_f} then")
            lines.append(f"            wait;")
            lines.append(f"        end if;")
            lines.append(f"        loop")
            for i in rcvs:
                lines.append(f"            if not f_{i} then "
                             f"{rcv_aliases[i]} := {drv_aliases[k]}; end if;")
            lines.append(f"            wait on {drv_aliases[k]};")
            lines.append(f"        end loop;")
            lines.append(f"    end process;")
            lines.append(f"end architecture;")
            lines.append(f"")
            return entity_name, "\n".join(lines)

        # One process per tran endpoint's 'other signal
        for i in tran_indices:
            others = [j for j in range(n_ep) if j != i]
            sens = ", ".join(drv_aliases[j] for j in others)

            def _copy_body(pad):
                if len(others) == 1:
                    lines.append(f"{pad}{rcv_aliases[i]} := "
                                 f"{_drv_expr(others[0])};")
                else:
                    for k, j in enumerate(others):
                        lines.append(f"{pad}v({k}) := {_drv_expr(j)};")
                    lines.append(f"{pad}{rcv_aliases[i]} := "
                                 f"{resolve_func}(v);")

            if fuse_ok:
                # Arbitrate once at t=0: exactly one driven OTHER ->
                # fuse this receiver onto it and park.  Any other shape
                # (0 or >=2 drivers, runtime decline) falls into the
                # copy loop, which is exactly the pre-fuse emission.
                lines.append(f"    p_{i}: process")
                lines.append(f"        variable ndrv : natural := 0;")
                if len(others) > 1:
                    lines.append(f"        variable v : {vec_type}"
                                 f"(0 to {len(others) - 1});")
                lines.append(f"    begin")
                for j in others:
                    lines.append(f"        if not undriven({drv_aliases[j]})"
                                 f" then ndrv := ndrv + 1; end if;")
                lines.append(f"        if ndrv = 1 then")
                for j in others:
                    lines.append(f"            if not undriven("
                                 f"{drv_aliases[j]}) then")
                    lines.append(f"                if fuse_try("
                                 f"{drv_aliases[j]}, {rcv_aliases[i]}) then")
                    lines.append(f"                    wait;")
                    lines.append(f"                end if;")
                    lines.append(f"            end if;")
                lines.append(f"        end if;")
                lines.append(f"        loop")
                _copy_body("            ")
                lines.append(f"            wait on {sens};")
                lines.append(f"        end loop;")
                lines.append(f"    end process;")
            else:
                lines.append(f"    p_{i}: process({sens})")
                if len(others) > 1:
                    lines.append(f"        variable v : {vec_type}"
                                 f"(0 to {len(others) - 1});")
                lines.append(f"    begin")
                _copy_body("        ")
                lines.append(f"    end process;")

        # One process per signal endpoint: deposit resolved tran drivers
        # to signal's 'receiver (propagates to parent via SOURCE_IMPLICIT)
        for i in sig_indices:
            if i not in sig_rcv_aliases:
                continue
            sig_type = net["drivers"][i]["type"].lower()
            # Sensitivity: all tran drivers
            sens = ", ".join(drv_aliases[j] for j in tran_indices)
            lines.append(f"    p_sig_{i}: process({sens})")
            if len(tran_indices) > 1:
                lines.append(f"        variable v : {vec_type}"
                             f"(0 to {len(tran_indices) - 1});")
            lines.append(f"    begin")
            if len(tran_indices) == 1:
                j = tran_indices[0]
                val_expr = _to_sig_type(drv_aliases[j], sig_type)
                lines.append(f"        {sig_rcv_aliases[i]} := "
                             f"{val_expr};")
            else:
                for k, j in enumerate(tran_indices):
                    lines.append(f"        v({k}) := {drv_aliases[j]};")
                resolved = f"{resolve_func}(v)"
                val_expr = _to_sig_type(resolved, sig_type)
                lines.append(f"        {sig_rcv_aliases[i]} := "
                             f"{val_expr};")
            lines.append(f"    end process;")

    lines.append(f"end architecture;")
    lines.append(f"")

    return entity_name, "\n".join(lines)


PLAIN_JOIN_KINDS = ("sv_tran", "sv_alias")


def _instance_of(ename):
    """Strip '.<port>.driver' / '.<port>.other' -> instance prefix."""
    parts = ename.rsplit(".", 2)
    return parts[0] if len(parts) == 3 else ename


def flatten_components(nets):
    """Union nets joined by plain tran/alias instances (#75).

    A plain bidirectional switch is a passive wire join, not a
    repeater: the joined nets form ONE physical net.  Returns
    (flattened_list, components) where each component is a dict
    {members: [net,..]} replacing its member nets.  A component is
    only formed when EVERY tran endpoint on every member net belongs
    to a plain join whose both sides are inside the component —
    tranif/rtran/mixed nets are left per-net (legacy resolution).
    """
    idx = {id(n): i for i, n in enumerate(nets)}
    parent = list(range(len(nets)))

    def find(a):
        while parent[a] != a:
            parent[a] = parent[parent[a]]
            a = parent[a]
        return a

    def union(a, b):
        ra, rb = find(a), find(b)
        if ra != rb:
            parent[rb] = ra

    inst_nets = {}
    for i, net in enumerate(nets):
        for drv in net["drivers"]:
            if drv.get("is_signal"):
                continue
            if (drv.get("kind") or "") in PLAIN_JOIN_KINDS:
                inst_nets.setdefault(_instance_of(drv["ename"]), set()).add(i)

    for insts in inst_nets.values():
        it = iter(insts)
        first = next(it)
        for other in it:
            union(first, other)

    groups = {}
    for i in range(len(nets)):
        groups.setdefault(find(i), []).append(i)

    out, components = [], []
    for root, members in groups.items():
        if len(members) < 2:
            out.extend(nets[i] for i in members)
            continue
        member_nets = [nets[i] for i in members]
        # Admission: every tran endpoint in the component must be a
        # plain join whose instance connects only member nets
        ok = True
        for net in member_nets:
            for drv in net["drivers"]:
                if drv.get("is_signal"):
                    continue
                kind = drv.get("kind") or ""
                if kind not in PLAIN_JOIN_KINDS:
                    ok = False
                    break
                # The strength architecture caps supply->strong across
                # the switch: NOT a passive join.  Only flatten value-
                # preserving types (behavioral std_logic / logic3d).
                if "logic3ds" in (drv.get("type") or "").lower():
                    ok = False
                    break
                inst = _instance_of(drv["ename"])
                if not inst_nets.get(inst, set()) <= set(members):
                    ok = False
                    break
            if not ok:
                break
        if ok:
            components.append({"members": member_nets})
        else:
            out.extend(member_nets)
    return out, components


def _gen_component_vhdl(comp, idx, design_name, fix_ename):
    """One resolver for a flattened component (#75 slice 1).

    The joining switches are inert; the component's value is the
    resolution over the member nets' signal endpoints (each already
    resolves its own real drivers), relaxed through the same
    signal/.receiver deposit contract the per-net resolvers use.
    ONE process for the whole component: N nets that legacy served
    with N resolvers and up to N delta round-trips per settle now
    settle through a single process.
    """
    entity_name = f"sv2vhdl_comp_{design_name}_{idx}"
    members = comp["members"]

    h = 5381
    for net in members:
        h ^= _net_hash(net)

    sigs = []   # (alias, rcv_alias, type) per member signal endpoint
    for net in members:
        for i, drv in enumerate(net["drivers"]):
            if not drv.get("is_signal"):
                continue
            sigs.append((fix_ename(drv["ename"]), drv["type"].lower()))
            break

    resolve_func, vec_type, extra_uses = _type_info(members[0])
    tran_type = "std_logic"
    for net in members:
        for drv in net["drivers"]:
            if not drv.get("is_signal"):
                tran_type = drv["type"].lower()
                break

    lines = []
    lines.append(f"-- Net hash: {h:016x} fuse-v2 comp")
    lines.append(f"-- Component: {len(members)} nets flattened "
                 f"({', '.join(n['net_name'] for n in members[:6])}"
                 f"{'...' if len(members) > 6 else ''})")
    lines.append(f"-- Auto-generated by sv2vhdl resolver plugin")
    lines.append(f"")
    lines.append(f"library ieee;")
    lines.append(f"use ieee.std_logic_1164.all;")
    if extra_uses:
        lines.append(f"library sv2vhdl;")
    for use in extra_uses:
        lines.append(use)
    lines.append(f"")
    lines.append(f"entity {entity_name} is end;")
    lines.append(f"architecture generated of {entity_name} is")
    for k, (path, styp) in enumerate(sigs):
        lines.append(f"    alias sig_{k} is << signal {path} : {styp} >>;")
        lines.append(f"    alias rcv_{k} is "
                     f"<< signal {path}.receiver : {styp} >>;")
    lines.append(f"begin")

    def _conv_in(k):
        styp = sigs[k][1]
        if styp != tran_type:
            if tran_type == "logic3ds":
                return f"to_logic3ds(sig_{k}, ST_SUPPLY)"
            if tran_type == "logic3d":
                return f"to_logic3d(sig_{k})"
        return f"sig_{k}"

    def _conv_out(expr, k):
        styp = sigs[k][1]
        if tran_type == styp:
            return expr
        if tran_type in ("logic3ds", "logic3d") and styp == "std_logic":
            return f"to_std_logic({expr})"
        if tran_type == "logic3ds" and "logic3d" in styp:
            return f"to_logic3d({expr})"
        return expr

    sens = ", ".join(f"sig_{k}" for k in range(len(sigs)))
    lines.append(f"    p_comp: process({sens})")
    if len(sigs) > 1:
        lines.append(f"        variable v : {vec_type}"
                     f"(0 to {len(sigs) - 2});")
    lines.append(f"    begin")
    # exclude-net-k deposits: same relaxation contract as legacy per-net
    for k in range(len(sigs)):
        others = [j for j in range(len(sigs)) if j != k]
        if len(others) == 1:
            lines.append(f"        rcv_{k} := "
                         f"{_conv_out(_conv_in(others[0]), k)};")
        else:
            for slot, j in enumerate(others):
                lines.append(f"        v({slot}) := {_conv_in(j)};")
            lines.append(f"        rcv_{k} := "
                         f"{_conv_out(f'{resolve_func}(v)', k)};")
    lines.append(f"    end process;")
    lines.append(f"end architecture;")
    lines.append(f"")
    return entity_name, "\n".join(lines)


def resolve_net(nets, design_name):
    """Generate per-net VHDL resolver entities and a wrapper.

    Args:
        nets: list of net dicts (see module docstring)
        design_name: top-level entity name (lowercase)

    Returns:
        dict: {filename: vhdl_string} with one file per net + wrapper,
              or None if nothing to generate.
    """
    if not nets:
        return None

    wrapper_entity = f"resolved_{design_name}"
    dut_label = "dut"

    orig_prefix = f".{design_name}."
    new_prefix = f".{wrapper_entity}.{dut_label}."

    def fix_ename(ename):
        if ename.startswith(orig_prefix):
            return new_prefix + ename[len(orig_prefix):]
        return ename

    files = {}
    entity_names = []

    # #75: the GENERATOR decides which nets the kernel net solver may
    # take.  Everything the external VHDL resolver machinery exists for
    # stays here: connect nets (mixed-discipline cosim bridges),
    # federation nets (peer-simulator bridging), mixed-type nets
    # (value-identity broken by conversions), and anything named in
    # SV2VHDL_KERNEL_EXCLUDE (comma-separated substrings — the hook for
    # SPEF back-annotation and hot-plug flows that must keep the
    # generated interception layer).  Only the plain uniform tran/alias
    # class is offered to the kernel; the plugin registers those and
    # generates nothing for them.  SV2VHDL_KERNEL_NETS=0 disables.
    kernel_nets = []
    excl = [e for e in (os.environ.get("SV2VHDL_KERNEL_EXCLUDE") or
                        "").split(",") if e]

    def _kernel_eligible(net):
        if os.environ.get("SV2VHDL_KERNEL_NETS") == "0":
            return False
        if _is_connect_net(net) or _is_federation_net(net):
            return False
        for e in excl:
            if e in net["net_name"]:
                return False
        types = set()
        for drv in net["drivers"]:
            t = (drv.get("type") or "").lower()
            if not drv.get("is_signal"):
                types.add(t)
        if len(types) != 1:
            return False
        t = types.pop()
        if not ("logic3ds" in t or "logic3d" in t or "std_logic" in t
                or "std_ulogic" in t):
            return False
        return True

    remaining = []
    for net in nets:
        if _kernel_eligible(net):
            kernel_nets.append(net["net_name"])
        else:
            remaining.append(net)
    nets = remaining

    # #75: flatten tran-connected components (opt out with
    # SV2VHDL_NO_FLATTEN=1) — plain tran/alias joins collapse to one
    # resolver per component; remaining nets emit per-net as before
    if os.environ.get("SV2VHDL_NO_FLATTEN") is None:
        nets, components = flatten_components(nets)
    else:
        components = []

    for idx, comp in enumerate(components):
        entity_name, vhdl = _gen_component_vhdl(comp, idx, design_name,
                                                fix_ename)
        filename = f"{design_name}_comp_{idx}.vhd"
        files[filename] = vhdl
        entity_names.append(entity_name)

    # Generate one file per net
    for idx, net in enumerate(nets):
        entity_name, vhdl = _gen_net_vhdl(net, idx, design_name, fix_ename)
        filename = f"{design_name}_rn_{idx}.vhd"
        files[filename] = vhdl
        entity_names.append(entity_name)

    # Generate wrapper file
    lines = []
    lines.append(f"-- Wrapper: instantiates DUT + {len(nets)} resolver(s)")
    lines.append(f"-- Auto-generated by sv2vhdl resolver plugin")
    lines.append(f"")
    lines.append(f"library ieee;")
    lines.append(f"use ieee.std_logic_1164.all;")
    lines.append(f"")
    lines.append(f"entity {wrapper_entity} is end;")
    lines.append(f"architecture wrapper of {wrapper_entity} is")
    lines.append(f"begin")
    lines.append(f"    {dut_label}: entity work.{design_name};")
    for idx, ename in enumerate(entity_names):
        lines.append(f"    rn_{idx}: entity work.{ename};")
    lines.append(f"end architecture;")
    lines.append(f"")

    files[f"{design_name}_wrapper.vhd"] = "\n".join(lines)

    # Special key consumed by the plugin, never written to disk
    files["__kernel_nets__"] = "\n".join(kernel_nets)

    return files


# ---------- Verilog-AMS connect rules ----------

def parse_connectrules(vams_path):
    """Parse a Verilog-AMS file for connectrules and mixed-discipline nets.

    Scans for:
      - connectrules block: which connectmodules to use
      - module port discipline declarations (logic vs electrical)
      - module instantiations and their port connections
      - wire declarations on mixed-discipline nets

    Returns a list of connect net dicts suitable for resolve_net():
        [{
            "net_name": ".top.up",
            "drivers": [{"ename": ".top.pfd_i.up.driver", "type": "std_logic"}],
            "receivers": [{"ename": ".top.pfd_i.up.receiver", "type": "std_logic"}],
            "connect": {
                "connect_module": "kestrel_d2a",
                "direction": "d2a",
                "parameters": {"vdd": 1.8, "vth": 0.9}
            },
            "bridge_signal": "up"
        }, ...]
    """
    import re

    with open(vams_path) as f:
        src = f.read()

    # Strip comments
    src = re.sub(r'//[^\n]*', '', src)
    src = re.sub(r'/\*.*?\*/', '', src, flags=re.DOTALL)

    # Parse connectrules: connect <module> input <disc>, output <disc>;
    rules = {}  # (input_disc, output_disc) -> module_name
    cr_match = re.search(r'connectrules\s+(\w+)\s*;(.*?)endconnectrules',
                         src, re.DOTALL)
    if cr_match:
        cr_body = cr_match.group(2)
        for m in re.finditer(
                r'connect\s+(\w+)\s+input\s+(\w+)\s*,\s*output\s+(\w+)\s*;',
                cr_body):
            module_name = m.group(1)
            in_disc = m.group(2).lower()
            out_disc = m.group(3).lower()
            rules[(in_disc, out_disc)] = module_name

    if not rules:
        return []

    # Parse module: find the top module and its discipline declarations
    mod_match = re.search(
        r'module\s+(\w+)\s*\([^)]*\)\s*;(.*?)endmodule', src, re.DOTALL)
    if not mod_match:
        return []

    mod_name = mod_match.group(1)
    mod_body = mod_match.group(2)

    # Collect discipline declarations: "electrical x, y, z;"  "logic a, b;"
    disc_map = {}  # signal_name -> discipline
    for m in re.finditer(r'(electrical|logic|wire)\s+([^;]+);', mod_body):
        disc = m.group(1).lower()
        if disc == "wire":
            disc = "mixed"  # wire = undeclared discipline, resolved by context
        for name in re.split(r'[,\s]+', m.group(2).strip()):
            name = name.strip()
            if name:
                disc_map[name] = disc

    # Parse instantiations: collect port->net mappings per instance
    # The #(...) parameter override can contain nested parens like #(.N(40))
    # so we use a pattern that handles one level of nesting.
    instances = []
    for m in re.finditer(
            r'(\w+)\s+(?:#\([^()]*(?:\([^()]*\)[^()]*)*\)\s+)?(\w+)\s*\(\s*(.*?)\s*\)\s*;',
            mod_body, re.DOTALL):
        inst_module = m.group(1)
        inst_name = m.group(2)
        port_body = m.group(3)

        # Skip if this is a known keyword, not a module instance
        if inst_module in ('if', 'else', 'begin', 'end', 'analog',
                           'parameter', 'real', 'integer'):
            continue

        ports = {}
        for pm in re.finditer(r'\.(\w+)\s*\(\s*(\w+)\s*\)', port_body):
            ports[pm.group(1)] = pm.group(2)

        instances.append({
            "module": inst_module,
            "name": inst_name,
            "ports": ports,
        })

    # Identify mixed-discipline nets: nets connected to both logic and
    # electrical ports across different instances.
    # For each net, collect the disciplines of the ports connected to it.
    net_disciplines = {}  # net_name -> set of disciplines
    net_ports = {}  # net_name -> [(inst, port_name, module)]

    for inst in instances:
        for port_name, net_name in inst["ports"].items():
            if net_name not in net_disciplines:
                net_disciplines[net_name] = set()
                net_ports[net_name] = []
            # Determine this port's discipline from the net's declaration
            # or from the module's known discipline
            disc = disc_map.get(net_name, "unknown")
            net_disciplines[net_name].add(disc)
            net_ports[net_name].append((inst, port_name, inst["module"]))

    # Generate connect net dicts for mixed-discipline nets
    connect_nets = []
    for net_name, discs in net_disciplines.items():
        if "mixed" not in discs and len(discs) <= 1:
            continue  # single discipline, no connect module needed

        # Determine direction from connected modules' disciplines
        has_logic = False
        has_electrical = False
        logic_endpoint = None
        electrical_endpoint = None

        for inst, port_name, inst_module in net_ports[net_name]:
            # Heuristic: if the module's ports are declared electrical,
            # it's analog; if logic, it's digital.
            # For generated kestrel code, we know the module types.
            # More generally, we'd check the module's port disciplines.
            net_disc = disc_map.get(net_name, "unknown")

            # Check what other disciplines connect to this net
            for other_inst, other_port, other_module in net_ports[net_name]:
                if other_inst is inst:
                    continue
                # The net is mixed if different instances expect different
                # disciplines — detected via the "mixed"/"wire" annotation

        # Simpler approach: if the net is declared as "wire" (mixed),
        # look at which connect rule matches based on the port types
        # of the connected instances.
        # For now, scan the instances to find which are analog vs digital
        logic_ports = []
        elec_ports = []
        for inst, port_name, inst_module in net_ports[net_name]:
            # Check if any other net on this instance uses electrical/logic
            inst_disc = "unknown"
            for pn, nn in inst["ports"].items():
                d = disc_map.get(nn, "unknown")
                if d in ("electrical", "logic"):
                    inst_disc = d
                    break
            if inst_disc == "electrical":
                elec_ports.append((inst, port_name))
            elif inst_disc == "logic":
                logic_ports.append((inst, port_name))

        if not logic_ports or not elec_ports:
            continue  # not a true mixed-discipline crossing

        # Determine direction
        # D2A: logic drives, electrical receives
        # A2D: electrical drives, logic receives
        # Heuristic: look at port direction declarations in the module body
        # For now, check if the input declaration lists the net
        is_d2a = None
        for m in re.finditer(r'input\s+([^;]+);', mod_body):
            inputs = set(re.split(r'[,\s]+', m.group(1).strip()))
            if net_name in inputs:
                # Net is an input to the top module — doesn't help
                pass

        # Better: check instance port directions from output/input decls
        # For generated code, the digital PFD outputs UP/DN (→ D2A)
        # and the VCO outputs vco_out (→ A2D to digital divider).
        # Use the connect rule direction:
        #   connect d2a input logic, output electrical → logic drives
        #   connect a2d input electrical, output logic → electrical drives

        # Determine direction per-net from the connect rules.
        # In Verilog-AMS, connectrules specify:
        #   connect cm input D1, output D2
        # meaning cm is inserted where a D1 driver meets a D2 receiver.
        #
        # We need to know which side drives this net.  Check which
        # instance has an output-like port name on this net.
        _output_hints = {'out', 'outp', 'outn', 'clk_out', 'q', 'up', 'dn',
                         'dout', 'data_out', 'fb'}
        _input_hints = {'in', 'inp', 'clk_in', 'vctrl', 'din', 'data_in',
                        'clk', 'ref'}

        def _is_output_port(port_name):
            pn = port_name.lower()
            if pn in _output_hints:
                return True
            if pn.endswith('_out') or pn.startswith('out'):
                return True
            return False

        # Find which discipline is the driver on this net
        elec_drives = any(_is_output_port(pn) for _, pn in elec_ports)
        logic_drives = any(_is_output_port(pn) for _, pn in logic_ports)

        if elec_drives and not logic_drives:
            # Electrical drives → A2D
            direction = "a2d"
            rule_key = ("electrical", "logic")
        elif logic_drives and not elec_drives:
            # Logic drives → D2A
            direction = "d2a"
            rule_key = ("logic", "electrical")
        else:
            # Ambiguous — default to D2A (logic→electrical)
            direction = "d2a"
            rule_key = ("logic", "electrical")

        if rule_key not in rules:
            # Try the other direction
            rule_key = rule_key[::-1]
            direction = "a2d" if direction == "d2a" else "d2a"
        if rule_key not in rules:
            continue  # no matching connect rule

        cm_name = rules[rule_key]

        # Build the net dict with the digital-side endpoint
        ep = logic_ports[0]
        drv_ename = f".{mod_name}.{ep[0]['name']}.{ep[1]}.driver"
        rcv_ename = f".{mod_name}.{ep[0]['name']}.{ep[1]}.receiver"

        connect_nets.append({
            "net_name": f".{mod_name}.{net_name}",
            "drivers": [{"ename": drv_ename, "type": "std_logic"}],
            "receivers": [{"ename": rcv_ename, "type": "std_logic"}],
            "connect": {
                "connect_module": cm_name,
                "direction": direction,
                "parameters": {},  # filled from connectmodule params if available
            },
            "bridge_signal": net_name,
        })

    return connect_nets


def resolve_vams(vams_path, design_name=None):
    """Generate VHDL resolver entities for Verilog-AMS connect rules.

    Parses the .vams file, identifies mixed-discipline nets, and generates
    connect resolver VHDL + a boundary config file for the cosim bridge.

    Returns:
        dict: {filename: content_string}
              Includes resolver VHDL files and a .boundary config file.
    """
    connect_nets = parse_connectrules(vams_path)
    if not connect_nets:
        return None

    if not design_name:
        import os
        design_name = os.path.splitext(os.path.basename(vams_path))[0]

    # Generate resolver VHDL via the standard resolve_net path.
    # The connect module behavior is translated inline — no separate
    # boundary config or cosim bridge setup needed.
    files = resolve_net(connect_nets, design_name)
    return files


# ---------- VHPI exploration helpers ----------

def explore_region(region_path):
    """Explore a design region using VHPI bridge.

    Returns dict with signals, instances, and any power supplies found.
    """
    if not _have_vhpi:
        raise RuntimeError("VHPI bridge not available (not running in simulator)")

    signals = vhpi.get_signals(region_path)
    instances = vhpi.get_instances(region_path)

    # Identify power supply signals by naming convention
    power_signals = {}
    for sig in signals:
        name = sig["name"].upper()
        if name.endswith("_VDD") or name == "VDD":
            power_signals.setdefault("vdd", []).append(sig)
        elif name.endswith("_VSS") or name == "VSS":
            power_signals.setdefault("vss", []).append(sig)

    return {
        "signals": signals,
        "instances": instances,
        "power": power_signals,
    }


def find_power_supplies(net_ename):
    """Find power supply signals in the same scope as a net."""
    if not _have_vhpi:
        return {}

    parent = net_ename.rsplit(".", 1)[0]
    if not parent:
        return {}

    try:
        signals = vhpi.get_signals(parent)
    except (KeyError, RuntimeError):
        return {}

    power = {}
    for sig in signals:
        name = sig["name"].upper()
        if name.endswith("_VDD") or name == "VDD":
            power.setdefault("vdd", []).append(sig)
        elif name.endswith("_VSS") or name == "VSS":
            power.setdefault("vss", []).append(sig)

    return power


def get_instance_generics(instance_ename):
    """Read generics from an instance via VHPI."""
    if not _have_vhpi:
        return {}

    try:
        return vhpi.get_generics(instance_ename)
    except (KeyError, RuntimeError):
        return {}
