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
