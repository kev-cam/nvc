#!/usr/bin/env python3
"""
arena_ctl.py -- federation controller for simulations that share arenas.

A model says which band its radio pins work in (the ARENA_BAND attribute of
the pin's 'driver type; the resolver plugin writes it into the generated
resolver as "-- arena band <band>: ...").  The controller decides which
logical arena each band is in, gets the arena a network (a named IPv6 VLAN,
from net-chat), runs the arena's rendezvous point on it, and launches the
simulations, telling each one the logical arenas of its bands:

    FED_NAME=<simulation>  FED_ARENAS=<band>=<arena>@<rendezvous>,...

The arena is not a simulation: once the simulations have met at the
rendezvous point they talk to each other directly (libarena).

    arena_ctl.py run federation.json [--logdir DIR] [--trace]
    arena_ctl.py rendezvous --arena NAME --listen HOST:PORT --expect a,b,...

federation.json:
    {
      "arenas": {
        "lab-2g4": {"vlan": "rf-lab", "listen": "[::1]:0"}
      },
      "bands": {"ism2g4": "lab-2g4"},
      "simulations": [
        {"name": "drone", "cwd": "/path", "cmd": ["nvc", "-r", "..."],
         "gen": "/path/gen"},                 # where its resolvers were generated
        {"name": "ctrl", ..., "bands": ["ism2g4"]}   # or say the bands directly
      ]
    }

How far apart in time the simulations run is not configured: where the
antennas' resolvers apply the physical delay of each path (the ARENA_DELAY
attribute of their type), each simulation runs ahead of another by the
slack between them, the shortest path delay between their antennas as they
are now (libarena); without delays they keep together delta cycle by delta
cycle.

An arena's "listen" address is where its rendezvous point runs; without one
the controller asks for the address of the arena's VLAN (vlan_address), and
without a VLAN it uses the loopback.  The simulations of an arena are
ranked in the order they are listed here: that is the order their endpoints
take in every resolution, so results do not depend on who starts first.
"""
import argparse
import glob
import json
import os
import re
import socket
import struct
import subprocess
import sys
import threading

M_JOIN, M_PEERS, M_ERROR = 1, 2, 3


# ---- the arena's network -------------------------------------------------

def vlan_address(vlan):
    """The address on `vlan` that the arena's rendezvous point binds.

    The IPv6 VLANs come from net-chat (chat_sessions.ipv6_vlan); net-mgr does
    not yet resolve a VLAN name to an address or prefix, so a VLAN's address
    is taken from NVC_ARENA_VLAN_<NAME> (e.g. NVC_ARENA_VLAN_RF_LAB=fd12::7)
    until it does, else the loopback."""
    if not vlan:
        return "::1"
    key = "NVC_ARENA_VLAN_" + re.sub(r"[^A-Za-z0-9]", "_", vlan).upper()
    return os.environ.get(key, "::1")


def _hostport(host, port):
    return "[%s]:%d" % (host, port) if ":" in host else "%s:%d" % (host, port)


def _split_hostport(hp):
    host, _, port = hp.rpartition(":")
    return host.strip("[]"), int(port)


# ---- the rendezvous point ------------------------------------------------

def _recv_all(s, n):
    b = b""
    while len(b) < n:
        k = s.recv(n - len(b))
        if not k:
            raise ConnectionError("closed")
        b += k
    return b


def _recv_msg(s):
    t, n = struct.unpack("<II", _recv_all(s, 8))
    return t, _recv_all(s, n) if n else b""


def _send_msg(s, t, body):
    s.sendall(struct.pack("<II", t, len(body)) + body)


class Rendezvous:
    """Where the simulations of one arena meet.  Once every expected
    simulation has joined it tells each of them all the others (address,
    number of endpoints), in rank order, and stays up to refuse late joiners
    (joining a running arena is not supported yet)."""

    def __init__(self, arena, host, port, expect, log=None):
        self.arena, self.expect, self.log = arena, list(expect), log or (lambda m: None)
        fam = socket.AF_INET6 if ":" in host else socket.AF_INET
        self.sock = socket.socket(fam, socket.SOCK_STREAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind((host, port))
        self.sock.listen(64)
        self.host, self.port = host, self.sock.getsockname()[1]
        self.joined = {}
        self.started = False
        self.thread = threading.Thread(target=self._serve, daemon=True)

    @property
    def address(self):
        return _hostport(self.host, self.port)

    def start(self):
        self.thread.start()
        return self

    def _serve(self):
        while True:
            try:
                c, addr = self.sock.accept()
            except OSError:
                return
            try:
                t, body = _recv_msg(c)
                name, arena, port, nlocal = body.decode().split("\n")
            except (ConnectionError, ValueError, struct.error):
                c.close()
                continue
            why = None
            if arena != self.arena:
                why = "this is the rendezvous of arena %s, not %s" % (self.arena, arena)
            elif self.started:
                why = "arena %s is running: joining a running arena is not supported yet" % self.arena
            elif name not in self.expect:
                why = "%s is not expected in arena %s" % (name, self.arena)
            elif name in self.joined:
                why = "%s already joined arena %s" % (name, self.arena)
            if why:
                self.log("rendezvous %s: refused %s: %s" % (self.arena, name, why))
                _send_msg(c, M_ERROR, why.encode())
                c.close()
                continue
            self.joined[name] = (c, addr[0], int(port), int(nlocal))
            self.log("rendezvous %s: %s joined from %s (%s endpoint(s))" % (self.arena, name, addr[0], nlocal))
            if all(n in self.joined for n in self.expect):
                self.started = True
                lines = ["peer %s %s %d %d" % (n, self.joined[n][1], self.joined[n][2], self.joined[n][3])
                         for n in self.expect]
                body = ("\n".join(lines) + "\n").encode()
                for n in self.expect:
                    _send_msg(self.joined[n][0], M_PEERS, body)
                    self.joined[n][0].close()
                self.log("rendezvous %s: started with %s" % (self.arena, ", ".join(self.expect)))

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


# ---- which bands a simulation uses -----------------------------------------

def bands_of(sim):
    """The bands of a simulation: given, or read from its generated
    resolvers."""
    if "bands" in sim:
        return list(sim["bands"])
    bands = []
    for f in sorted(glob.glob(os.path.join(sim.get("gen", ""), "*.vhd"))):
        with open(f) as fh:
            for line in fh:
                m = re.match(r"-- arena band (\S+):", line)
                if m and m.group(1) not in bands:
                    bands.append(m.group(1))
    return bands


# ---- running a federation --------------------------------------------------

def run(cfg, logdir=None, trace=False):
    log = lambda m: print("arena_ctl: " + m, file=sys.stderr, flush=True)
    bandmap = {b.lower(): a for b, a in cfg.get("bands", {}).items()}
    sims = cfg["simulations"]
    members = {a: [] for a in cfg.get("arenas", {})}
    for s in sims:
        for b in bands_of(s):
            a = bandmap.get(b.lower())
            if a is None:
                log("%s: band %s is not mapped to an arena: resolved inside %s only" % (s["name"], b, s["name"]))
                continue
            members.setdefault(a, [])
            if s["name"] not in members[a]:
                members[a].append(s["name"])
    rvs = {}
    for a, names in members.items():
        if not names:
            continue
        acfg = cfg.get("arenas", {}).get(a, {})
        if "listen" in acfg:
            host, port = _split_hostport(acfg["listen"])
        else:
            host, port = vlan_address(acfg.get("vlan")), 0
        rvs[a] = Rendezvous(a, host, port, names, log).start()
        log("arena %s%s: rendezvous at %s for %s" % (a, " (VLAN %s)" % acfg["vlan"] if acfg.get("vlan") else "",
                                                     rvs[a].address, ", ".join(names)))
    fed_arenas = ",".join("%s=%s@%s" % (b, a, rvs[a].address)
                          for b, a in cfg.get("bands", {}).items() if a in rvs)
    procs = []
    if logdir:
        os.makedirs(logdir, exist_ok=True)
    for s in sims:
        env = dict(os.environ, FED_NAME=s["name"], FED_ARENAS=fed_arenas)
        env.update(s.get("env", {}))
        if trace:
            env["FED_TRACE"] = "1"
        out = open(os.path.join(logdir, s["name"] + ".log"), "w") if logdir else None
        log("launch %s: FED_ARENAS=%s" % (s["name"], fed_arenas))
        procs.append((s, subprocess.Popen(s["cmd"], cwd=s.get("cwd"), env=env, stdout=out,
                                          stderr=subprocess.STDOUT if out else None), out))
    rc = 0
    for s, p, out in procs:
        r = p.wait()
        if out:
            out.close()
        log("%s exited %d" % (s["name"], r))
        rc = rc or r
    for r in rvs.values():
        r.close()
    return rc


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("run", help="run a federation")
    r.add_argument("config")
    r.add_argument("--logdir")
    r.add_argument("--trace", action="store_true")
    v = sub.add_parser("rendezvous", help="run one arena's rendezvous point only")
    v.add_argument("--arena", required=True)
    v.add_argument("--listen", required=True)
    v.add_argument("--expect", required=True, help="simulation names, in rank order")
    a = ap.parse_args(argv)
    if a.cmd == "run":
        with open(a.config) as fh:
            return run(json.load(fh), a.logdir, a.trace)
    host, port = _split_hostport(a.listen)
    rv = Rendezvous(a.arena, host, port, a.expect.split(","),
                    lambda m: print(m, file=sys.stderr, flush=True)).start()
    print(rv.address, flush=True)
    rv.thread.join()
    return 0


if __name__ == "__main__":
    sys.exit(main())
