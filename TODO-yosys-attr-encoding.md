# Draft: RTLIL attribute encoding for the NVC mapping (§5 companion)

Companion to TODO-yosys-integration.md §5.  Written before any mapping
code, per the plan's ordering.  Grounded in the shipped
representations: l3d (3-bit: value / driven / uncertain), l3ds
(VALUE/STRENGTH/FLAGS record, strength 0-31 = base*2+cap, flags
KNOWN/UNKNOWN/UNDRIVEN/NOPOWER/…), and stitch nets (static endpoint
sets registered by path).

## The load-bearing observation

The differentiation (arena resolution, 9-value logic) already lives
OUTSIDE the accelerated cones **by construction**: today's admission
policy only accelerates cones whose nets are certainty-settled and
single-driver, and the l3d↔bit conversion happens at the chunk rim in
the bridge.  Multi-driver/strength/stitch nets stay interp-side where
the kernel solver owns them.  A direct-RTLIL builder should keep that
policy, which turns §5 from "encode nine values and resolution in
RTLIL" into two smaller jobs:

1. encode WHY a net is excluded, so the RTLIL builder can never
   admit it by accident, and
2. encode the rim contract, so the return path reconstructs the
   l3d planes exactly where they re-enter the interp world.

## Wire attributes (IdString → Const)

- `\nvc.res`      : resolution class.  Absent = plain single-driver
                    (admissible).  Values: `"stitch"` (kernel-solver
                    net; carries `\nvc.stitch_path` = the registered
                    hierarchical path), `"strength"` (l3ds ladder,
                    below-strong contributors), `"multi"` (multiple
                    same-strength drivers — error if admitted).
- `\nvc.planes`   : bitmask of OBSERVED l3d planes downstream
                    (1=value, 2=driven, 4=uncertain, 8=strength).
                    Plane elision carries through: planes=1 collapses
                    losslessly to RTLIL 2-state; any other mask makes
                    the wire rim-only (see below) or excluded.
- `\nvc.rim`      : present on wires crossing the accel boundary.
                    Const = the bridge contract id (byte layout the
                    bridge already uses for l3d↔bit at the rim), so
                    the C emission and the seed/back-fill machinery
                    (#43 ordered seed) key on the same contract.

## Nine-value policy

RTLIL x is only reached transiently INSIDE cones during opt -keepdc;
semantically meaningful U/W/L/H/- never enter RTLIL at all: a net
whose consumers observe the uncertain or strength plane is rim-only or
excluded (`\nvc.planes` ≠ 1).  L/H collapse to 0/1 ONLY when the
strength plane is unobserved (planes bit 8 clear) — same rule the
strength-arc classifier already applies.  So the "information loss"
goes NOWHERE: it is fenced at admission, exactly where the promise
("if it simulates OK, that's how it works when built") requires the
4-state interp to keep authority.

## Resolution (arena) policy

No resolution functions in RTLIL, agreed — and none needed: stitch
nets are static sets solved in the kernel (counts state machine); a
stitch wire in RTLIL exists only as a boundary INPUT to admitted
cones (`\nvc.res="stitch"` + `\nvc.rim`), never as something passes
may merge or re-drive.  `$tribuf` is not used; tristate sources are
classified by the same admission fence.

## Pass-safety

All `\nvc.*` attributes ride wires that are also ports of the
flattened top or rim cells, which yosys keeps; `opt_clean` does not
strip port attributes.  If a future pass set threatens them, the
fallback is a side-table keyed by wire name emitted alongside the
design instead of in-band attributes (the gsm cname() mangling rules
already give stable names).

## UPF cross-link (verified 2026-09-05 against current source)

`UPF_PLAN.md` (added by the user) leans directly on this §5 machinery.
Findings after verifying the shipped types — they REFRAME the plan's
central "certainty-plane corruption bit" question:

- **Power-missing is ALREADY a first-class distinguishable state.**
  `logic3da_pkg.l3da_flags` has `AFL_NOPOWER` / `AFL_UNK_NOPOWER`
  (corruption-X) / `AFL_UDR_NOPOWER`, with Thevenin driver resolution
  over them; `logic3ds` flags carry `NOPOWER` too.  The plan's headline
  demo (corruption-X ≠ reset-X ≠ uninit-X) is representable in shipped
  types TODAY — not something to invent.
- **The fast form has NO room, and that is the real boundary.**
  `logic3dw` certainty is a FULL 2-bit enum (00 certain / 01 W / 10 X /
  11 U, U-dominant) — verified in logic3dw_pkg.  No spare code.  Scalar
  `logic3d` is `natural range 0 to 7` (value + 2 kind bits) — also full.
- **Architecture fit is clean and already implied by this doc.** UPF
  corruption is a flags-plane property on domain CROSSINGS (receivers =
  rims).  The admission fence here already routes flags/strength/
  certainty-plane nets interp-side or rim-only (`\nvc.planes` ≠ 1);
  the interp-side resolution generator already owns the NOPOWER
  vocabulary.  So per-receiver power resolution drops into the EXISTING
  structure: `\nvc.planes` gains an observed-NOPOWER bit that fences the
  crossing rim-only, and the resolution generator sets the l3da/l3ds
  NOPOWER flag there.  The plan's "no inserted AND gates, zero area"
  holds because corruption never enters the accelerated cone.
- **The one open decision** (belongs with the certainty encoding, per
  both docs' "design before mapping" rule): does corruption need to
  ride the ACCELERATED l3dw path so large powered-DOWN regions still
  run at accel speed?  If yes → l3dw needs a 3rd kind-bit or a separate
  NOPOWER plane (breaks the zero-memory claim; scalar l3d must widen
  past 0..7 — invasive: interp, vhdl2vlog, bridge aj_bit_base, VHPI).
  If correctness-at-crossings suffices → free under today's fence, the
  accel path never sees it, powered domains stay full-speed 2-state.

Campaign items this doc already answers for `UPF_PLAN.md`: §9 IR tap
point = `vhdl2rtlil_module` (post-elab, pre-sim-lower); §4 text-vs-
linkage = the shipped direct `gsm_rtlil_*` builder (one RTLIL build →
write_verilog for P&R + gen_statemachine for sim = "same input, both
flows"); §5 "key functions on signature" = `gsm_rtlil_content_hash` +
the content-keyed .so tier; §5 "specialise, don't branch on supply" =
the double-bank / vtable-specialization substrate; §2 switch net =
one `cell_bin("and")`.  TENSION to hold: the accel fast path trends to
2-state value-plane (resolver elision, value-bit rim marshalling);
keeping the certainty plane alive through synthesis is the conscious
choice UPF requires — reconciled by the specialize-powered/unpowered
approach, not by carrying certainty on the hot path.
