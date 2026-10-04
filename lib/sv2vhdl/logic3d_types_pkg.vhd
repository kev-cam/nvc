-- 3D Logic Type Package
-- Provides the enum representation for single-bit signals
-- Vector versions would use ieee.numeric_std.unsigned for bitwise ops

-- Separating the handling of value, certainty and strength makes it easier
-- to convert between types than with the collapsed enum form (01XZ), logic
-- values should be consistent regardless of X (certainty state), converting
-- 1/0 to a Voltage and back doesn't require looking at the X/Z fields.

library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;
use std.textio.all;   -- $readmem support: text/line and their implicit file_open/endfile/deallocate

package logic3d_types_pkg is

    ---------------------------------------------------------------------------
    -- Enum version for single-bit signals (3-bit encoding)
    ---------------------------------------------------------------------------
    subtype logic3d is natural range 0 to 7;

    -- Encoding: bit2=uncertain, bit1=driven, bit0=value
    -- 000 = undriven 0   (L3D_L)  std_logic 'L'
    -- 001 = undriven 1   (L3D_H)  std_logic 'H'
    -- 010 = driven 0     (L3D_0)  std_logic '0'
    -- 011 = driven 1     (L3D_1)  std_logic '1'
    -- 100 = high-Z       (L3D_Z)  std_logic 'Z'
    -- 101 = uncertain 1  (L3D_W)  std_logic 'W'
    -- 110 = uncertain 0  (L3D_X)  std_logic 'X'
    -- 111 = uninitialized (L3D_U) std_logic 'U'

    -- Bit field constants for direct manipulation
    constant L3D_UNCERTAIN : natural := 4;  -- bit 2
    constant L3D_DRIVEN    : natural := 2;  -- bit 1
    constant L3D_VALUE     : natural := 1;  -- bit 0

    -- State constants
    -- Encoding: bit2 = uncertain, bit1 = driven, bit0 = value.
    -- The value bit propagates through gates regardless of certainty;
    -- certainty is metadata indicating "this value wasn't explicitly set".
    -- Naming: L3D_<value>{Z?}{X?}
    --   0/1 = value bit
    --   Z   = undriven (no driver — Verilog 'L'/'H' or pass-gate output)
    --   X   = uncertain (Verilog 'X' — value is suspect)
    constant L3D_0   : logic3d := 2;  -- 010: driven, certain 0
    constant L3D_1   : logic3d := 3;  -- 011: driven, certain 1
    constant L3D_0Z  : logic3d := 0;  -- 000: undriven, certain 0
    constant L3D_1Z  : logic3d := 1;  -- 001: undriven, certain 1
    constant L3D_0X  : logic3d := 6;  -- 110: driven, uncertain 0
    constant L3D_1X  : logic3d := 7;  -- 111: driven, uncertain 1
    constant L3D_0ZX : logic3d := 4;  -- 100: undriven, uncertain 0
    constant L3D_1ZX : logic3d := 5;  -- 101: undriven, uncertain 1
    -- Legacy aliases (std_logic-style names) for back-compat.
    constant L3D_L : logic3d := 0;  -- alias of L3D_0Z (undriven 0, 'L')
    constant L3D_H : logic3d := 1;  -- alias of L3D_1Z (undriven 1, 'H')
    constant L3D_Z : logic3d := 4;  -- alias of L3D_0ZX (high-Z)
    constant L3D_W : logic3d := 5;  -- alias of L3D_1ZX (weak X 1)
    constant L3D_X : logic3d := 6;  -- alias of L3D_0X (driven uncertain 0)
    constant L3D_U : logic3d := 7;  -- alias of L3D_1X (uninitialized)

    ---------------------------------------------------------------------------
    -- Vector type for resolution functions
    ---------------------------------------------------------------------------
    type logic3d_vector is array (natural range <>) of logic3d;

    ---------------------------------------------------------------------------
    -- Resolution function (matches std_logic resolution semantics)
    --
    -- Strong (driven) beats weak (undriven):
    --   0/1 override L/H on the same net
    -- Equal strength, same value: that value
    -- Equal strength, opposing values: uncertain at that strength
    --   (strong 0 vs strong 1 -> X; weak 0 vs weak 1 -> W)
    -- Z is identity (no contribution)
    -- U propagates unconditionally (uninitialized taints all)
    --
    -- The 8-value encoding fits NVC's memoization (tab2[8][8])
    -- for fast 1- and 2-driver paths.
    ---------------------------------------------------------------------------
    function l3d_resolve(drivers : logic3d_vector) return logic3d;

    ---------------------------------------------------------------------------
    -- Resolved subtype: use for signals with multiple drivers
    ---------------------------------------------------------------------------
    subtype resolved_logic3d is l3d_resolve logic3d;
    -- Element-resolved vector, for multi-bit nets/ports with several drivers.
    subtype resolved_logic3d_vector is (l3d_resolve) logic3d_vector;

    ---------------------------------------------------------------------------
    -- Lookup tables (8x8 for 2-input, 8 for 1-input)
    -- Gate outputs are always strong.
    -- Weak inputs (L,H) treated as known 0,1 for gate logic.
    -- Uncertain inputs (Z,W,X,U) propagate X, except where
    -- a dominating value (0 for AND, 1 for OR) forces the result.
    ---------------------------------------------------------------------------
    type lut1_t is array (0 to 7) of logic3d;
    type lut2_t is array (0 to 7, 0 to 7) of logic3d;

    -- NOT semantics: copy strength + certainty, flip the value bit.
    -- For CERTAIN inputs this is straightforward (NOT 0 = 1).
    -- For UNCERTAIN inputs we COULD flip the value bit, but iverilog's
    -- tgt-vhdl translates always_comb blocks with `<=` + `wait for 0 ns`
    -- between bit assignments, and includes self-driven signals in the
    -- wait list. With value-flip, the deltas oscillate forever on any
    -- self-referencing combinational block. Until iverilog uses VHDL
    -- variables for procedural locals, collapse uncertain NOT to L3D_0X.
    --                        L  H  0  1  Z  W  X  U
    --                        ↓  ↓  ↓  ↓  ↓  ↓  ↓  ↓
    --                        H  L  1  0  X  X  X  X
    -- VALUE-PRESERVING 4-state LUTs.  value and certainty are ORTHOGONAL: the
    -- value bit is always computed 2-state on the operand value bits (so it
    -- matches Verilog/Verilator), and the uncertain bit rides alongside (OR of
    -- inputs' uncertainty, with certain-0 dominating AND / certain-1 dominating
    -- OR).  This fixes the freeze where NOT(X) / 1 AND U dropped the value bit to
    -- 0: e.g. NOT(X=6, value 0) = U (7, value 1) not X (6), so a control signal
    -- never hangs the pipeline just because it is uncertain -- the value still
    -- propagates.  (Encoding: bit0=value, bit1=strong, bit2=uncertain;
    -- 2=strong0, 3=strong1, 6=X=strong-uncertain-0, 7=U=strong-uncertain-1.)
    constant NOT_LUT : lut1_t := (1, 0, 3, 2, 5, 4, 7, 6);

    constant AND_LUT : lut2_t := (
        0 => (2, 2, 2, 2, 2, 2, 2, 2),
        1 => (2, 3, 2, 3, 6, 7, 6, 7),
        2 => (2, 2, 2, 2, 2, 2, 2, 2),
        3 => (2, 3, 2, 3, 6, 7, 6, 7),
        4 => (2, 6, 2, 6, 6, 6, 6, 6),
        5 => (2, 7, 2, 7, 6, 7, 6, 7),
        6 => (2, 6, 2, 6, 6, 6, 6, 6),
        7 => (2, 7, 2, 7, 6, 7, 6, 7)
    );

    constant OR_LUT : lut2_t := (
        0 => (2, 3, 2, 3, 6, 7, 6, 7),
        1 => (3, 3, 3, 3, 3, 3, 3, 3),
        2 => (2, 3, 2, 3, 6, 7, 6, 7),
        3 => (3, 3, 3, 3, 3, 3, 3, 3),
        4 => (6, 3, 6, 3, 6, 7, 6, 7),
        5 => (7, 3, 7, 3, 7, 7, 7, 7),
        6 => (6, 3, 6, 3, 6, 7, 6, 7),
        7 => (7, 3, 7, 3, 7, 7, 7, 7)
    );

    constant XOR_LUT : lut2_t := (
        0 => (2, 3, 2, 3, 6, 7, 6, 7),
        1 => (3, 2, 3, 2, 7, 6, 7, 6),
        2 => (2, 3, 2, 3, 6, 7, 6, 7),
        3 => (3, 2, 3, 2, 7, 6, 7, 6),
        4 => (6, 7, 6, 7, 6, 7, 6, 7),
        5 => (7, 6, 7, 6, 7, 6, 7, 6),
        6 => (6, 7, 6, 7, 6, 7, 6, 7),
        7 => (7, 6, 7, 6, 7, 6, 7, 6)
    );

    constant NAND_LUT : lut2_t := (
        0 => (3, 3, 3, 3, 3, 3, 3, 3),
        1 => (3, 2, 3, 2, 7, 6, 7, 6),
        2 => (3, 3, 3, 3, 3, 3, 3, 3),
        3 => (3, 2, 3, 2, 7, 6, 7, 6),
        4 => (3, 7, 3, 7, 7, 7, 7, 7),
        5 => (3, 6, 3, 6, 7, 6, 7, 6),
        6 => (3, 7, 3, 7, 7, 7, 7, 7),
        7 => (3, 6, 3, 6, 7, 6, 7, 6)
    );

    constant NOR_LUT : lut2_t := (
        0 => (3, 2, 3, 2, 7, 6, 7, 6),
        1 => (2, 2, 2, 2, 2, 2, 2, 2),
        2 => (3, 2, 3, 2, 7, 6, 7, 6),
        3 => (2, 2, 2, 2, 2, 2, 2, 2),
        4 => (7, 2, 7, 2, 7, 6, 7, 6),
        5 => (6, 2, 6, 2, 6, 6, 6, 6),
        6 => (7, 2, 7, 2, 7, 6, 7, 6),
        7 => (6, 2, 6, 2, 6, 6, 6, 6)
    );

    constant XNOR_LUT : lut2_t := (
        0 => (3, 2, 3, 2, 7, 6, 7, 6),
        1 => (2, 3, 2, 3, 6, 7, 6, 7),
        2 => (3, 2, 3, 2, 7, 6, 7, 6),
        3 => (2, 3, 2, 3, 6, 7, 6, 7),
        4 => (7, 6, 7, 6, 7, 6, 7, 6),
        5 => (6, 7, 6, 7, 6, 7, 6, 7),
        6 => (7, 6, 7, 6, 7, 6, 7, 6),
        7 => (6, 7, 6, 7, 6, 7, 6, 7)
    );

    -- Weaken: clear strength bit, map strong unknown to weak unknown
    --                           L  H  0  1  Z  W  X  U
    constant WEAKEN_LUT : lut1_t := (0, 1, 0, 1, 4, 5, 5, 5);

    ---------------------------------------------------------------------------
    -- Resolution lookup (matches std_logic resolution table)
    -- Z is identity; U dominates; strong beats weak; contention -> X/W
    ---------------------------------------------------------------------------
    constant RESOLVE_LUT : lut2_t := (
    --                  L  H  0  1  Z  W  X  U
        0 => (0, 5, 2, 3, 0, 5, 6, 7),  -- L: weak 0
        1 => (5, 1, 2, 3, 1, 5, 6, 7),  -- H: weak 1
        2 => (2, 2, 2, 6, 2, 2, 6, 7),  -- 0: strong 0
        3 => (3, 3, 6, 3, 3, 3, 6, 7),  -- 1: strong 1
        4 => (0, 1, 2, 3, 4, 5, 6, 7),  -- Z: identity
        5 => (5, 5, 2, 3, 5, 5, 6, 7),  -- W: weak unknown
        6 => (6, 6, 6, 6, 6, 6, 6, 7),  -- X: strong unknown
        7 => (7, 7, 7, 7, 7, 7, 7, 7)   -- U: uninitialized (dominates)
    );

    ---------------------------------------------------------------------------
    -- Gate functions
    ---------------------------------------------------------------------------
    function l3d_not(a : logic3d) return logic3d;
    function l3d_and(a, b : logic3d) return logic3d;
    function l3d_or(a, b : logic3d) return logic3d;
    function l3d_xor(a, b : logic3d) return logic3d;
    function l3d_nand(a, b : logic3d) return logic3d;
    function l3d_nor(a, b : logic3d) return logic3d;
    function l3d_xnor(a, b : logic3d) return logic3d;
    function l3d_buf(a : logic3d) return logic3d;
    -- Re-strengthen: a plain assign drives its OWN (strong) strength;
    -- weak alphabet codes normalize to driven ones, Z stays Z
    --                  L  H  0  1  Z  W  X  U
    function l3d_strengthen(a : logic3d) return logic3d;
    -- Element-wise, for a vector (a variable copied from a weak net stores
    -- no strength; a vector continuous assignment drives its own)
    function l3d_strengthen(a : logic3d_vector) return logic3d_vector;
    function l3d_weaken(a : logic3d) return logic3d;
    function l3d_set_uncertain(a : logic3d) return logic3d;

    -- Multi-input (chained lookups, no delta cycles)
    -- Logical &&/|| truthiness: a vector's truth value is its value-plane
    -- OR-reduction; the scalar overload is the identity so the backend can
    -- wrap either operand kind and let overload resolution pick. (No
    -- return-type-overloaded l3d_and/l3d_or here: dual scalar/vector
    -- consumers like l3d_to_unsigned make such calls ambiguous.)
    function l3d_truthy(a : logic3d_vector) return logic3d;
    function l3d_truthy(a : logic3d) return logic3d;

    -- $isunknown: the explicit certainty observer -- L3D_1 when any bit has
    -- certainty 0. Reads never consult certainty; tests probe it via this.
    function l3d_isunknown(a : logic3d_vector) return logic3d;
    function l3d_isunknown(a : logic3d) return logic3d;

    -- Value-plane equality for == in boolean contexts, overloaded across
    -- the family so the operand's REAL type resolves (annotations lie for
    -- indexed elements: a 1-bit slice keeps the VECTOR type, and a memory
    -- word-select IS a vector despite the same annotation shape).
    function l3d_eq1(a, b : logic3d) return boolean;
    function l3d_eq1(a, b : logic3d_vector) return boolean;
    function l3d_eq1(a : logic3d_vector; b : logic3d) return boolean;
    function l3d_eq1(a : logic3d; b : logic3d_vector) return boolean;

    function l3d_and3(a, b, c : logic3d) return logic3d;
    function l3d_and4(a, b, c, d : logic3d) return logic3d;
    function l3d_or3(a, b, c : logic3d) return logic3d;
    function l3d_or4(a, b, c, d : logic3d) return logic3d;
    function l3d_xor3(a, b, c : logic3d) return logic3d;
    function l3d_nand3(a, b, c : logic3d) return logic3d;
    function l3d_nor3(a, b, c : logic3d) return logic3d;

    ---------------------------------------------------------------------------
    -- Conversion to/from std_logic
    --
    -- Both directions are lossless for signal values:
    --   to_logic3d:  9 std_logic values -> 8 logic3d values
    --                '-' maps to L3D_X (don't-care is not a signal value)
    --   to_std_logic: 8 logic3d values -> 8 distinct std_logic values
    --
    -- Lossless round-trip: to_logic3d(to_std_logic(x)) = x for all x
    -- This means std_logic drivers can be mixed with logic3d drivers
    -- by converting to logic3d first, then resolving with l3d_resolve.
    ---------------------------------------------------------------------------
    function to_std_logic(a : logic3d) return std_logic;  -- lossless
    function to_logic3d(s : std_logic) return logic3d;    -- lossless

    -- Certainty-preserving conversion to std_logic_vector: maps each logic3d
    -- bit via to_std_logic (0/1/L/H/Z/W/X/U), so x/z survive for $display
    -- radix formatting (unlike l3d_to_unsigned, which drops to value bits).
    -- Element order is preserved (a'range), keeping the MSB leftmost.
    function to_std_logic_vector(a : logic3d_vector) return std_logic_vector;
    function to_std_logic_vector(a : logic3d) return std_logic_vector;  -- 1-elem

    ---------------------------------------------------------------------------
    -- Utilities - check individual bits, independent of other attributes
    ---------------------------------------------------------------------------
    function to_char(a : logic3d) return character;
    function is_one(a : logic3d) return boolean;       -- bit 0 = 1
    function is_zero(a : logic3d) return boolean;      -- bit 0 = 0
    function is_strong(a : logic3d) return boolean;    -- bit 1 = 1
    function is_uncertain(a : logic3d) return boolean; -- bit 2 = 1
    function is_x(a : logic3d) return boolean;         -- uncertain and strong
    function is_z(a : logic3d) return boolean;         -- uncertain and not strong

    -- Edge detection for logic3d signals (matches std_logic rising_edge/falling_edge)
    function rising_edge(signal s : logic3d) return boolean;
    function falling_edge(signal s : logic3d) return boolean;

    ---------------------------------------------------------------------------
    -- Vector operations (element-wise)
    ---------------------------------------------------------------------------
    function l3d_not(a : logic3d_vector) return logic3d_vector;
    function l3d_and(a, b : logic3d_vector) return logic3d_vector;
    function l3d_or(a, b : logic3d_vector) return logic3d_vector;

    -- Mixed vector & scalar reducing to a scalar. iverilog emits these when a
    -- wider operand (e.g. an N-bit parameter literal) is combined with a 1-bit
    -- signal in a scalar (boolean / nonzero-test) context: Verilog sizes both
    -- to the wider width, applies the op, then the scalar lvalue keeps bit 0.
    function l3d_and(a : logic3d_vector; b : logic3d) return logic3d;
    -- logic3d AND std_logic: tgt-vhdl can emit a mixed-type AND (a logic3d bit
    -- ANDed with a Ternary_Logic/expr that resolved to std_logic). Lift the
    -- std_logic operand to logic3d so the bit-true LUT applies.
    function l3d_and(a : logic3d; b : std_logic) return logic3d;
    function l3d_xor(a, b : logic3d_vector) return logic3d_vector;
    function l3d_nand(a, b : logic3d_vector) return logic3d_vector;
    function l3d_nor(a, b : logic3d_vector) return logic3d_vector;
    function l3d_xnor(a, b : logic3d_vector) return logic3d_vector;

    -- Arithmetic: convert to/from unsigned for +, -, comparisons
    function l3d_to_unsigned(a : logic3d_vector) return unsigned;
    function l3d_to_unsigned(a : logic3d) return unsigned;        -- 1-bit
    -- Two's-complement reinterpretation (Verilog signed context).
    function l3d_to_signed(a : logic3d_vector) return signed;
    function to_integer(a : logic3d) return natural;              -- 0 or 1
    function unsigned_to_l3d(a : unsigned) return logic3d_vector;
    function "+"(a, b : logic3d_vector) return logic3d_vector;
    function "-"(a, b : logic3d_vector) return logic3d_vector;
    function "+"(a : logic3d_vector; b : natural) return logic3d_vector;

    -- Vector equality reducing to a 1-bit logic3d (Verilog '=='): iverilog emits
    -- a bare "=" on two logic3d_vectors but uses the result where a logic3d is
    -- expected (e.g. as an l3d_or operand). AND-reduce of bitwise XNOR, so X/Z
    -- propagate per Verilog's x-pessimistic equality. Coexists with the implicit
    -- boolean "=" (resolved by context / return type).
    function "="(a, b : logic3d_vector) return logic3d;

    -- Integer conversion
    function to_integer(a : logic3d_vector) return natural;

    -- Shift operators
    -- Shift-count value of a logic3d_vector: like to_integer but SATURATES at a
    -- huge sentinel when the value is too large to hold, instead of dropping the
    -- high bits (to_integer keeps only the low 31, so e.g. 2**64 wrongly reads 0
    -- -> a shift by 2**64 would leave the operand unchanged instead of clearing
    -- it). Any count past a vector's width clears/sign-fills it anyway.
    function l3d_shcount(a : logic3d_vector) return natural;
    function "sll"(a : logic3d_vector; n : natural) return logic3d_vector;
    function "srl"(a : logic3d_vector; n : natural) return logic3d_vector;
    function shift_left (a : logic3d_vector; n : natural) return logic3d_vector;
    function shift_right(a : logic3d_vector; n : natural) return logic3d_vector;
    -- Arithmetic right shift (Verilog >>> on a signed operand): sign-extends.
    function l3d_sra    (a : logic3d_vector; n : natural) return logic3d_vector;
    -- Sequential UDP evaluation. `rows` is every table row concatenated, each
    -- row (nin+2) chars: [current-state][in1..inN][next-state] (iverilog's
    -- ivl_udp_row order). Returns the next output given the current output and
    -- the current/previous inputs (previous = each input's 'last_value, so the
    -- one input that changed carries the edge). Mirrors vvp/udp.cc semantics:
    -- level rows first, then edge rows; '-' output holds; no match holds.
    function sv_udp_seq(rows : string; nin : natural; cur_out : std_logic;
                        prev_in : std_logic_vector;
                        cur_in : std_logic_vector) return std_logic;
    -- One-element std_logic_vector(0 to 0); lets the UDP input concatenation
    -- stay a vector even for a single-input primitive.
    function sl1(v : std_logic) return std_logic_vector;

    -- Signed relational + signed div/mod (Verilog signed context: both
    -- operands signed). Reinterpret the value bits as two's-complement.
    function l3d_lt_s(a, b : logic3d_vector) return boolean;
    function l3d_gt_s(a, b : logic3d_vector) return boolean;
    function l3d_le_s(a, b : logic3d_vector) return boolean;
    function l3d_ge_s(a, b : logic3d_vector) return boolean;
    function l3d_div_s(a, b : logic3d_vector) return logic3d_vector;
    -- Verilog's signed %: the remainder takes the DIVIDEND's sign (VHDL rem,
    -- not mod: -7 % 3 is -1); a % 0 is all x.
    function l3d_mod_s(a, b : logic3d_vector) return logic3d_vector;
    -- Sign-extending resize (Verilog signed widening): fill high bits w/ sign.
    function l3d_resize_s(a : logic3d_vector; new_size : natural) return logic3d_vector;

    -- Multiplication, division, modulus
    function "*"  (a, b : logic3d_vector) return logic3d_vector;
    function "/"  (a, b : logic3d_vector) return logic3d_vector;
    function "mod"(a, b : logic3d_vector) return logic3d_vector;
    function "rem"(a, b : logic3d_vector) return logic3d_vector;

    -- (Verilog $random(seed) / $urandom(seed): sv_math_pkg's sv_random_value,
    -- sv_urandom_value and sv_random_next, vvp's sequence.  The
    -- glibc-constant LCG sv_random(seed) that stood here returned the new
    -- seed as the value.)

    -- Resize (extend or truncate)
    function resize(a : logic3d_vector; new_size : natural) return logic3d_vector;

    -- Comparisons against integer (Verilog "if vec != 0" pattern)
    function "/="(a : logic3d_vector; b : integer) return boolean;
    function "="(a : logic3d_vector; b : integer) return boolean;

    -- Boolean⇄logic3d coercions (iverilog emits `bool and bool` where the
    -- surrounding context expects logic3d).
    function "and"(a, b : boolean) return logic3d;
    function "or"(a, b : boolean) return logic3d;
    function "xor"(a, b : boolean) return logic3d;
    function "not"(a : boolean) return logic3d;

    -- Concatenation overloads for mixed logic3d / logic3d_vector operands.
    -- Iverilog emits `(L3D_0,…) & l3d_and(bit, bit)` where the second operand
    -- is a single logic3d. VHDL doesn't auto-coerce.
    function "&"(a : logic3d_vector; b : logic3d) return logic3d_vector;
    function "&"(a : logic3d; b : logic3d_vector) return logic3d_vector;
    function "&"(a : logic3d; b : logic3d) return logic3d_vector;

    -- Same concatenations but returning std_logic_vector for contexts where
    -- the LHS or surrounding expression is typed as std_logic_vector (e.g.
    -- iverilog temporaries like `U_Tmp : std_logic_vector := a & b;`).
    -- NVC's overload resolution picks the matching return type from context.
    function "&"(a : logic3d_vector; b : logic3d_vector) return std_logic_vector;
    function "&"(a : logic3d_vector; b : logic3d) return std_logic_vector;
    function "&"(a : logic3d; b : logic3d_vector) return std_logic_vector;
    function "&"(a : logic3d; b : logic3d) return std_logic_vector;

    -- Comparisons between logic3d_vector operands.
    function "<" (a, b : logic3d_vector) return boolean;
    function "<="(a, b : logic3d_vector) return boolean;
    function ">" (a, b : logic3d_vector) return boolean;
    function ">="(a, b : logic3d_vector) return boolean;
    function "=" (a, b : logic3d_vector) return boolean;
    function "/="(a, b : logic3d_vector) return boolean;

    -- Reduce an N-bit unsigned to a single logic3d (the LSB). iverilog's
    -- to_std_logic path uses this in sv2vhdl mode to keep results in the
    -- logic3d family so comparisons against logic3d operands type-check.
    function unsigned_to_l3d_bit(a : unsigned) return logic3d;

    ---------------------------------------------------------------------------
    -- Verilog real <-> logic3d conversions. Accumulate through real (not
    -- integer) so vectors wider than 31 bits convert without overflow.
    ---------------------------------------------------------------------------
    function l3d_to_real(a : logic3d_vector) return real;   -- unsigned value
    function l3d_to_real(a : logic3d) return real;          -- single bit
    function l3d_to_real_s(a : logic3d_vector) return real; -- two's complement
    -- Verilog implicit real->vector: round to nearest (half away from zero),
    -- wrap into w bits, two's complement for negatives.
    function real_to_l3d(r : real; w : natural) return logic3d_vector;
    function real_to_l3d1(r : real) return logic3d;

    -- Verilog 4-state -> 2-state cast (bit/int/shortint targets): certain 1
    -- (strong or weak) stays 1, everything uncertain (X/Z/W/U) coerces to 0.
    function l3d_to_2state(a : logic3d_vector) return logic3d_vector;
    function l3d_to_2state(a : logic3d) return logic3d;

    -- Two's-complement negation in the logic3d family (any uncertain bit
    -- makes the result X, per Verilog arithmetic).
    function l3d_neg(a : logic3d_vector) return logic3d_vector;

    -- Index-vector to integer. 3D-logic doctrine: certainty never gates a
    -- read, so uncertain bits index by their value plane (all-X address is
    -- address 0), unlike Verilog's x-index-selects-nothing pessimism.
    function l3d_index(a : logic3d_vector; s : boolean) return integer;

    -- Bounds-safe part/bit reads: Verilog reads x for out-of-range bits
    -- (VHDL raises a bounds error). base may be any integer.
    function l3d_part_read(a : logic3d_vector; base : integer; w : natural)
        return logic3d_vector;
    function l3d_bit_read(a : logic3d_vector; idx : integer) return logic3d;

    -- Bring a value of whatever numeric kind into logic3d at width w. The
    -- backend's C++ type annotations can lie for logic3d expressions that
    -- passed through numeric-typed paths, so the SAME function name is
    -- overloaded for every kind and VHDL overload resolution picks the one
    -- matching the expression's REAL type (the l3d overload is a resize).
    function to_l3d(a : unsigned;       w : natural) return logic3d_vector;
    function to_l3d(a : signed;         w : natural) return logic3d_vector;
    function to_l3d(a : integer;        w : natural) return logic3d_vector;
    function to_l3d(a : logic3d_vector; w : natural) return logic3d_vector;
    -- NOTE: no logic3d-scalar overload -- logic3d is a subtype of natural, so
    -- it would be a homograph of the integer one (scalar widening is handled
    -- by the backend's cast path instead).

    -- Verilog ** on vectors: modular exponentiation at the base's width
    -- (square-and-multiply over the exponent's bits, truncating each step,
    -- so wide exponents neither overflow nor need integer conversion).
    function l3d_pow(b, e : logic3d_vector;
                     bs, es : boolean) return logic3d_vector;

    -- Gate-operator overloads: a design promoted from std_logic/bit to 3D-Logic
    -- keeps its `a and b` / `not a` syntax. logic3d is a plain natural subtype
    -- with no built-in logic operators, so these thin wrappers over the l3d_*
    -- gate primitives supply them (scalar + element-wise vector). Return-type
    -- overload resolution keeps the built-in boolean `and`/`or` (used by `=`
    -- comparison chains in conditions) unaffected.
    function "not" (a : logic3d) return logic3d;
    function "and" (a, b : logic3d) return logic3d;
    function "or"  (a, b : logic3d) return logic3d;
    function "xor" (a, b : logic3d) return logic3d;
    function "nand"(a, b : logic3d) return logic3d;
    function "nor" (a, b : logic3d) return logic3d;
    function "xnor"(a, b : logic3d) return logic3d;
    function "not" (a : logic3d_vector) return logic3d_vector;
    function "and" (a, b : logic3d_vector) return logic3d_vector;
    function "or"  (a, b : logic3d_vector) return logic3d_vector;
    function "xor" (a, b : logic3d_vector) return logic3d_vector;
    function "nand"(a, b : logic3d_vector) return logic3d_vector;
    function "nor" (a, b : logic3d_vector) return logic3d_vector;
    function "xnor"(a, b : logic3d_vector) return logic3d_vector;

    -- $readmemh / $readmemb / $writememh / $writememb support (emitted by
    -- iverilog tgt-vhdl), with vvp's rules and messages (vpi/sys_readmem.c,
    -- sys_readmem_lex.lex, sys_priv.c get_filename):
    --
    --   sv_readmem_load(<file>, <hex>, "$readmemh", "<file>:<line>",
    --       "<scope>.<memory>", <left>, <right>, <word width>, <kind>,
    --       <has start>, <start>, <start is real>,
    --       <has finish>, <finish>, <finish is real>);
    --   sv_write_buf(sv_memfile_msgs);         -- sv_display_pkg
    --   for i in 0 to sv_readmem_count - 1 loop
    --       mem(sv_readmem_addr(i) - <lowest address>) := sv_readmem_word(i, W);
    --   end loop;
    --
    -- sv_readmem_load checks the arguments, reads the whole file into a
    -- process-wide store and keeps the words that go into the memory, with
    -- their Verilog addresses: lowest address first unless start/finish say
    -- otherwise, @<hex> moves to an address (one outside [start, finish] is
    -- an error), the load stops at an invalid character or at a word past
    -- the range ("Too many words"), and a file with too few words and no
    -- @address gets "Not enough words".  A word's digits fill it from the
    -- right: x and z only where their digit is, the bits above the digits
    -- 0, excess digits dropped (one warning per call).  <left>/<right>: the
    -- memory's declared range [left:right]; <kind>: the vpi type of the
    -- file name argument, for the "not a valid string" message.  1-bit
    -- words: sv_readmem_bit(i).
    --
    --   sv_writemem_open(<file>, <hex>, "$writememh", <the same arguments
    --       from the location on, less the word width>);
    --   sv_write_buf(sv_memfile_msgs);
    --   for i in 0 to sv_writemem_count - 1 loop
    --       sv_writemem_word(<mem(sv_writemem_addr(i) - lowest) as %h or %b>);
    --   end loop;
    --   sv_writemem_close;
    --
    -- The messages are vvp's, "WARNING: <loc>: ..." / "ERROR: <loc>: ...",
    -- each line ending in LF; sv_memfile_msgs returns (and forgets) those of
    -- the last call, for the caller to print with the testbench's output.
    procedure sv_readmem_load(fname : string; hex : boolean; task : string;
                              loc : string; mem : string;
                              left_a : integer; right_a : integer;
                              wwid : positive; kind : string;
                              has_start : boolean; start_a : integer;
                              start_real : boolean;
                              has_stop : boolean; stop_a : integer;
                              stop_real : boolean);
    impure function sv_readmem_count return natural;
    impure function sv_readmem_addr(i : natural) return integer;
    impure function sv_readmem_word(i : natural; w : positive) return logic3d_vector;
    impure function sv_readmem_bit(i : natural) return logic3d;
    procedure sv_writemem_open(fname : string; hex : boolean; task : string;
                               loc : string; mem : string;
                               left_a : integer; right_a : integer;
                               kind : string;
                               has_start : boolean; start_a : integer;
                               start_real : boolean;
                               has_stop : boolean; stop_a : integer;
                               stop_real : boolean);
    impure function sv_writemem_count return natural;
    impure function sv_writemem_addr(i : natural) return integer;
    procedure sv_writemem_word(s : string);
    procedure sv_writemem_close;
    impure function sv_memfile_msgs return string;

    -- The file name of a file task: a relative one resolves against
    -- $SV2VHDL_FILE_DIR when that is set (vamos sets it to the directory
    -- ./simv was started from), else against the simulator's working
    -- directory.  Messages show the name as the testbench gave it.
    impure function sv_file_path(name : string) return string;
    -- vvp's get_filename check: "" for a valid file name, else its warning
    -- (one or two lines, each ending in LF).  kind: the vpi type of the
    -- argument ("vpiConstant", "vpiReg", ...).
    function sv_file_name_msg(name : string; kind : string; task : string;
                              loc : string) return string;
    -- vvp's vpiStringVal of a vector: 8-bit characters, most significant
    -- first; a bit counts as 1 only when it is 1; leading NULs are skipped,
    -- NULs after the first character become spaces.
    function sv_vec_str(v : logic3d_vector) return string;
    function sv_vec_str(v : logic3d) return string;
    -- vvp's vpiIntVal: the low 32 bits as a two's complement integer (x and
    -- z bits count as 0); is_signed: a narrower value is sign-extended.
    function sv_vec_int32(v : logic3d_vector; is_signed : boolean) return integer;
    function sv_vec_int32(v : logic3d; is_signed : boolean) return integer;

end package;

package body logic3d_types_pkg is

    ---------------------------------------------------------------------------
    -- Resolution function (pairwise fold over RESOLVE_LUT)
    -- Single driver: identity. Two drivers: single LUT lookup.
    -- N drivers: chain pairwise (associative and commutative).
    ---------------------------------------------------------------------------
    function l3d_resolve(drivers : logic3d_vector) return logic3d is
        variable result : logic3d := L3D_Z;
    begin
        if drivers'length = 1 then
            return drivers(drivers'low);
        end if;
        for i in drivers'range loop
            result := RESOLVE_LUT(result, drivers(i));
        end loop;
        return result;
    end function;

    ---------------------------------------------------------------------------
    -- Gate implementations (single table lookup)
    ---------------------------------------------------------------------------
    function l3d_not(a : logic3d) return logic3d is
    begin
        return NOT_LUT(a);
    end function;

    function l3d_and(a, b : logic3d) return logic3d is
    begin
        return AND_LUT(a, b);
    end function;

    function l3d_or(a, b : logic3d) return logic3d is
    begin
        return OR_LUT(a, b);
    end function;

    function l3d_xor(a, b : logic3d) return logic3d is
    begin
        return XOR_LUT(a, b);
    end function;

    function l3d_nand(a, b : logic3d) return logic3d is
    begin
        return NAND_LUT(a, b);
    end function;

    function l3d_nor(a, b : logic3d) return logic3d is
    begin
        return NOR_LUT(a, b);
    end function;

    function l3d_xnor(a, b : logic3d) return logic3d is
    begin
        return XNOR_LUT(a, b);
    end function;

    function l3d_buf(a : logic3d) return logic3d is
    begin
        return a;
    end function;

    function l3d_strengthen(a : logic3d) return logic3d is
        constant STRENGTHEN_LUT : lut1_t := (2, 3, 2, 3, 4, 6, 6, 7);
    begin
        return STRENGTHEN_LUT(a);
    end function;

    function l3d_strengthen(a : logic3d_vector) return logic3d_vector is
        variable r : logic3d_vector(a'range);
    begin
        for i in a'range loop
            r(i) := l3d_strengthen(a(i));
        end loop;
        return r;
    end function;

    function l3d_weaken(a : logic3d) return logic3d is
    begin
        return WEAKEN_LUT(a);
    end function;

    -- Set uncertain bit (mark undriven): preserves value/strength, sets bit 2
    function l3d_set_uncertain(a : logic3d) return logic3d is
    begin
        return (a mod 4) + 4;
    end function;

    ---------------------------------------------------------------------------
    -- Multi-input gates (chained, single expression, no delta cycles)
    ---------------------------------------------------------------------------
    function l3d_and3(a, b, c : logic3d) return logic3d is
    begin
        return AND_LUT(AND_LUT(a, b), c);
    end function;

    function l3d_and4(a, b, c, d : logic3d) return logic3d is
    begin
        return AND_LUT(AND_LUT(AND_LUT(a, b), c), d);
    end function;

    function l3d_or3(a, b, c : logic3d) return logic3d is
    begin
        return OR_LUT(OR_LUT(a, b), c);
    end function;

    function l3d_or4(a, b, c, d : logic3d) return logic3d is
    begin
        return OR_LUT(OR_LUT(OR_LUT(a, b), c), d);
    end function;

    function l3d_xor3(a, b, c : logic3d) return logic3d is
    begin
        return XOR_LUT(XOR_LUT(a, b), c);
    end function;

    function l3d_nand3(a, b, c : logic3d) return logic3d is
    begin
        return NOT_LUT(AND_LUT(AND_LUT(a, b), c));
    end function;

    function l3d_nor3(a, b, c : logic3d) return logic3d is
    begin
        return NOT_LUT(OR_LUT(OR_LUT(a, b), c));
    end function;

    ---------------------------------------------------------------------------
    -- Conversion to std_logic
    ---------------------------------------------------------------------------
    function to_std_logic(a : logic3d) return std_logic is
        type sl_lut_t is array(0 to 7) of std_logic;
        constant SL_LUT : sl_lut_t := ('L', 'H', '0', '1', 'Z', 'W', 'X', 'U');
    begin
        return SL_LUT(a);
    end function;

    -- logic3d_vector -> std_logic_vector (per-bit, certainty preserving)
    function to_std_logic_vector(a : logic3d_vector) return std_logic_vector is
        variable result : std_logic_vector(a'range);
    begin
        for i in a'range loop
            result(i) := to_std_logic(a(i));
        end loop;
        return result;
    end function;

    -- single logic3d -> 1-element std_logic_vector
    function to_std_logic_vector(a : logic3d) return std_logic_vector is
        variable result : std_logic_vector(0 downto 0);
    begin
        result(0) := to_std_logic(a);
        return result;
    end function;

    ---------------------------------------------------------------------------
    -- Conversion from std_logic
    ---------------------------------------------------------------------------
    function to_logic3d(s : std_logic) return logic3d is
    begin
        case s is
            when '0' => return L3D_0;
            when '1' => return L3D_1;
            when 'L' => return L3D_L;
            when 'H' => return L3D_H;
            when 'Z' => return L3D_Z;
            when 'W' => return L3D_W;
            when 'X' => return L3D_X;
            when 'U' => return L3D_U;
            when '-' => return L3D_X;
        end case;
    end function;

    ---------------------------------------------------------------------------
    -- Utilities - check individual bits, independent of other attributes
    -- Encoding: bit2=uncertain, bit1=strength, bit0=value
    ---------------------------------------------------------------------------
    function to_char(a : logic3d) return character is
        type char_lut_t is array(0 to 7) of character;
        constant CHAR_LUT : char_lut_t := ('L', 'H', '0', '1', 'Z', 'W', 'X', 'U');
    begin
        return CHAR_LUT(a);
    end function;

    function is_one(a : logic3d) return boolean is
    begin
        return (a mod 2) = 1;  -- bit 0 = value
    end function;

    function is_zero(a : logic3d) return boolean is
    begin
        return (a mod 2) = 0;  -- bit 0 = value
    end function;

    function is_strong(a : logic3d) return boolean is
    begin
        return ((a / 2) mod 2) = 1;  -- bit 1 = strength
    end function;

    function is_uncertain(a : logic3d) return boolean is
    begin
        return ((a / 4) mod 2) = 1;  -- bit 2 = uncertain
    end function;

    -- Convenience aliases
    function is_x(a : logic3d) return boolean is
    begin
        return is_uncertain(a) and is_strong(a);
    end function;

    function is_z(a : logic3d) return boolean is
    begin
        return is_uncertain(a) and not is_strong(a);
    end function;

    function rising_edge(signal s : logic3d) return boolean is
    begin
        return s'event and is_one(s) and not is_one(s'last_value);
    end function;

    function falling_edge(signal s : logic3d) return boolean is
    begin
        return s'event and is_zero(s) and not is_zero(s'last_value);
    end function;

    -- Vector overloads (element-wise)
    function l3d_not(a : logic3d_vector) return logic3d_vector is
        variable result : logic3d_vector(a'range);
    begin
        for i in a'range loop
            result(i) := NOT_LUT(a(i));
        end loop;
        return result;
    end function;

    -- Helper: align two vectors of possibly-different ranges to common
    -- 0-based positions and apply a bit-wise op via LUT lookup. Iverilog
    -- sometimes emits ops with mismatched index ranges (e.g. a is [19:16]
    -- and b is [3:0]) — both are 4 bits, just rebased differently.
    function l3d_and(a, b : logic3d_vector) return logic3d_vector is
        constant n : natural := minimum(a'length, b'length);
        variable result : logic3d_vector(a'range);
        variable ai, bi, ri : integer;
    begin
        -- Copy-free hot path: translated signals are always `downto`, so
        -- rebasing needs only index offsets, not the two defensive vector
        -- copies this used to make (they dominated the op's cost).
        if n < a'length then
            result := (others => L3D_0);
        end if;
        if (not a'ascending) and (not b'ascending) then
            for i in 0 to n-1 loop
                result(result'low + i) := AND_LUT(a(a'low + i), b(b'low + i));
            end loop;
        else
            for i in 0 to n-1 loop
               if a'ascending then ai := a'high - i; else ai := a'low + i; end if;
               if b'ascending then bi := b'high - i; else bi := b'low + i; end if;
               -- the WRITE side must honor the result direction too: for an
               -- ASCENDING result (mixed-direction operands, e.g. an
               -- unconstrained concat aggregate) logical bit i is element
               -- 'high - i; result'low put the LSB in the MSB slot and
               -- BIT-REVERSED the output (measured: {24'h0,a}^const gave
               -- 20a5a5a5 for a5a5a504 — the EH1a registered-LSU garbage)
               if result'ascending then ri := result'high - i; else ri := result'low + i; end if;
               result(ri) := AND_LUT(a(ai), b(bi));
            end loop;
        end if;
        return result;
    end function;

    -- (vec & scalar) reduced to the scalar lvalue keeps bit 0:
    --   (vec & scalar)[0] = vec[0] and scalar.
    -- Rebase to (length-1 downto 0) so index 0 is the LSB regardless of the
    -- literal's declared range, matching the vector-vector bodies above.
    function l3d_and(a : logic3d_vector; b : logic3d) return logic3d is
    begin
        return AND_LUT(a(a'right), b);   -- bit 0 is the rightmost element
    end function;

    function l3d_and(a : logic3d; b : std_logic) return logic3d is
    begin
        return AND_LUT(a, to_logic3d(b));
    end function;

    -- Verilog '==' on two vectors -> 1-bit result. AND-reduce of bitwise XNOR
    -- (X/Z propagate through the LUTs, matching x-pessimistic equality). Length
    -- mismatch compares the common low bits (operands are same-width in practice).
    function "="(a, b : logic3d_vector) return logic3d is
        constant n  : natural := minimum(a'length, b'length);
        variable eq  : boolean := true;
        variable unc : boolean := false;
        variable av, bv : logic3d;
    begin
        -- Value-plane equality; certainty is metadata. The old AND/XNOR LUT
        -- chain was x-pessimistic: one uncertain bit forced the RESULT's
        -- value to 0 ("not equal"), zeroing every address/tag/pointer match
        -- once boot-time uncertainty touched an operand (doctrine: certainty
        -- never gates a computation's value).
        -- Copy-free: index from the right (bit 0) directly; once ineq and
        -- uncertainty are both latched the outcome cannot change.
        for i in 0 to n-1 loop
            if a'ascending then av := a(a'high - i); else av := a(a'low + i); end if;
            if b'ascending then bv := b(b'high - i); else bv := b(b'low + i); end if;
            if is_one(av) /= is_one(bv) then eq := false; end if;
            if is_uncertain(av) or is_uncertain(bv) then unc := true; end if;
            if (not eq) and unc then exit; end if;
        end loop;
        if eq then
            if unc then return L3D_U; else return L3D_1; end if;
        else
            if unc then return L3D_X; else return L3D_0; end if;
        end if;
    end function;

    function l3d_or(a, b : logic3d_vector) return logic3d_vector is
        constant n : natural := minimum(a'length, b'length);
        variable result : logic3d_vector(a'range);
        variable ai, bi, ri : integer;
    begin
        -- Copy-free hot path: translated signals are always `downto`, so
        -- rebasing needs only index offsets, not the two defensive vector
        -- copies this used to make (they dominated the op's cost).
        if n < a'length then
            result := (others => L3D_0);
        end if;
        if (not a'ascending) and (not b'ascending) then
            for i in 0 to n-1 loop
                result(result'low + i) := OR_LUT(a(a'low + i), b(b'low + i));
            end loop;
        else
            for i in 0 to n-1 loop
               if a'ascending then ai := a'high - i; else ai := a'low + i; end if;
               if b'ascending then bi := b'high - i; else bi := b'low + i; end if;
               -- the WRITE side must honor the result direction too: for an
               -- ASCENDING result (mixed-direction operands, e.g. an
               -- unconstrained concat aggregate) logical bit i is element
               -- 'high - i; result'low put the LSB in the MSB slot and
               -- BIT-REVERSED the output (measured: {24'h0,a}^const gave
               -- 20a5a5a5 for a5a5a504 — the EH1a registered-LSU garbage)
               if result'ascending then ri := result'high - i; else ri := result'low + i; end if;
               result(ri) := OR_LUT(a(ai), b(bi));
            end loop;
        end if;
        return result;
    end function;

    function l3d_xor(a, b : logic3d_vector) return logic3d_vector is
        constant n : natural := minimum(a'length, b'length);
        variable result : logic3d_vector(a'range);
        variable ai, bi, ri : integer;
    begin
        -- Copy-free hot path: translated signals are always `downto`, so
        -- rebasing needs only index offsets, not the two defensive vector
        -- copies this used to make (they dominated the op's cost).
        if n < a'length then
            result := (others => L3D_0);
        end if;
        if (not a'ascending) and (not b'ascending) then
            for i in 0 to n-1 loop
                result(result'low + i) := XOR_LUT(a(a'low + i), b(b'low + i));
            end loop;
        else
            for i in 0 to n-1 loop
               if a'ascending then ai := a'high - i; else ai := a'low + i; end if;
               if b'ascending then bi := b'high - i; else bi := b'low + i; end if;
               -- the WRITE side must honor the result direction too: for an
               -- ASCENDING result (mixed-direction operands, e.g. an
               -- unconstrained concat aggregate) logical bit i is element
               -- 'high - i; result'low put the LSB in the MSB slot and
               -- BIT-REVERSED the output (measured: {24'h0,a}^const gave
               -- 20a5a5a5 for a5a5a504 — the EH1a registered-LSU garbage)
               if result'ascending then ri := result'high - i; else ri := result'low + i; end if;
               result(ri) := XOR_LUT(a(ai), b(bi));
            end loop;
        end if;
        return result;
    end function;

    function l3d_nand(a, b : logic3d_vector) return logic3d_vector is
        constant n : natural := minimum(a'length, b'length);
        variable result : logic3d_vector(a'range);
        variable ai, bi, ri : integer;
    begin
        -- Copy-free hot path: translated signals are always `downto`, so
        -- rebasing needs only index offsets, not the two defensive vector
        -- copies this used to make (they dominated the op's cost).
        if n < a'length then
            result := (others => L3D_0);
        end if;
        if (not a'ascending) and (not b'ascending) then
            for i in 0 to n-1 loop
                result(result'low + i) := NAND_LUT(a(a'low + i), b(b'low + i));
            end loop;
        else
            for i in 0 to n-1 loop
               if a'ascending then ai := a'high - i; else ai := a'low + i; end if;
               if b'ascending then bi := b'high - i; else bi := b'low + i; end if;
               -- the WRITE side must honor the result direction too: for an
               -- ASCENDING result (mixed-direction operands, e.g. an
               -- unconstrained concat aggregate) logical bit i is element
               -- 'high - i; result'low put the LSB in the MSB slot and
               -- BIT-REVERSED the output (measured: {24'h0,a}^const gave
               -- 20a5a5a5 for a5a5a504 — the EH1a registered-LSU garbage)
               if result'ascending then ri := result'high - i; else ri := result'low + i; end if;
               result(ri) := NAND_LUT(a(ai), b(bi));
            end loop;
        end if;
        return result;
    end function;

    function l3d_nor(a, b : logic3d_vector) return logic3d_vector is
        constant n : natural := minimum(a'length, b'length);
        variable result : logic3d_vector(a'range);
        variable ai, bi, ri : integer;
    begin
        -- Copy-free hot path: translated signals are always `downto`, so
        -- rebasing needs only index offsets, not the two defensive vector
        -- copies this used to make (they dominated the op's cost).
        if n < a'length then
            result := (others => L3D_0);
        end if;
        if (not a'ascending) and (not b'ascending) then
            for i in 0 to n-1 loop
                result(result'low + i) := NOR_LUT(a(a'low + i), b(b'low + i));
            end loop;
        else
            for i in 0 to n-1 loop
               if a'ascending then ai := a'high - i; else ai := a'low + i; end if;
               if b'ascending then bi := b'high - i; else bi := b'low + i; end if;
               -- the WRITE side must honor the result direction too: for an
               -- ASCENDING result (mixed-direction operands, e.g. an
               -- unconstrained concat aggregate) logical bit i is element
               -- 'high - i; result'low put the LSB in the MSB slot and
               -- BIT-REVERSED the output (measured: {24'h0,a}^const gave
               -- 20a5a5a5 for a5a5a504 — the EH1a registered-LSU garbage)
               if result'ascending then ri := result'high - i; else ri := result'low + i; end if;
               result(ri) := NOR_LUT(a(ai), b(bi));
            end loop;
        end if;
        return result;
    end function;

    function l3d_xnor(a, b : logic3d_vector) return logic3d_vector is
        constant n : natural := minimum(a'length, b'length);
        variable result : logic3d_vector(a'range);
        variable ai, bi, ri : integer;
    begin
        -- Copy-free hot path: translated signals are always `downto`, so
        -- rebasing needs only index offsets, not the two defensive vector
        -- copies this used to make (they dominated the op's cost).
        if n < a'length then
            result := (others => L3D_0);
        end if;
        if (not a'ascending) and (not b'ascending) then
            for i in 0 to n-1 loop
                result(result'low + i) := XNOR_LUT(a(a'low + i), b(b'low + i));
            end loop;
        else
            for i in 0 to n-1 loop
               if a'ascending then ai := a'high - i; else ai := a'low + i; end if;
               if b'ascending then bi := b'high - i; else bi := b'low + i; end if;
               -- the WRITE side must honor the result direction too: for an
               -- ASCENDING result (mixed-direction operands, e.g. an
               -- unconstrained concat aggregate) logical bit i is element
               -- 'high - i; result'low put the LSB in the MSB slot and
               -- BIT-REVERSED the output (measured: {24'h0,a}^const gave
               -- 20a5a5a5 for a5a5a504 — the EH1a registered-LSU garbage)
               if result'ascending then ri := result'high - i; else ri := result'low + i; end if;
               result(ri) := XNOR_LUT(a(ai), b(bi));
            end loop;
        end if;
        return result;
    end function;

    -- Convert logic3d_vector to unsigned (extract value bits)
    function l3d_to_unsigned(a : logic3d_vector) return unsigned is
        variable result : unsigned(a'range);
    begin
        for i in a'range loop
            if is_one(a(i)) then
                result(i) := '1';
            else
                result(i) := '0';
            end if;
        end loop;
        return result;
    end function;

    -- Convert single logic3d to 1-bit unsigned
    function l3d_to_signed(a : logic3d_vector) return signed is
    begin
        return signed(std_logic_vector(l3d_to_unsigned(a)));
    end function;

    function l3d_to_unsigned(a : logic3d) return unsigned is
        variable result : unsigned(0 downto 0);
    begin
        if is_one(a) then
            result(0) := '1';
        else
            result(0) := '0';
        end if;
        return result;
    end function;

    function to_integer(a : logic3d) return natural is
    begin
        if is_one(a) then return 1; else return 0; end if;
    end function;

    -- Convert unsigned to logic3d_vector (strong driven values)
    function unsigned_to_l3d(a : unsigned) return logic3d_vector is
        variable result : logic3d_vector(a'range);
    begin
        for i in a'range loop
            if a(i) = '1' then
                result(i) := L3D_1;
            else
                result(i) := L3D_0;
            end if;
        end loop;
        return result;
    end function;

    -- Fill a constrained unsigned (declared by the caller, so it lives in the
    -- call frame / tlab, not the escaping heap) from a logic3d_vector, with
    -- a'left as the MSB to match numeric_std. No allocation.
    procedure to_u(a : logic3d_vector; u : out unsigned) is
        variable k : natural := u'high;
    begin
        for i in a'range loop
            if is_one(a(i)) then u(k) := '1'; else u(k) := '0'; end if;
            if k > u'low then k := k - 1; end if;
        end loop;
    end procedure;

    function "+"(a, b : logic3d_vector) return logic3d_vector is
        variable ua : unsigned(a'length - 1 downto 0);
        variable ub : unsigned(b'length - 1 downto 0);
    begin
        to_u(a, ua); to_u(b, ub);
        return unsigned_to_l3d(ua + ub);
    end function;

    function "-"(a, b : logic3d_vector) return logic3d_vector is
        variable ua : unsigned(a'length - 1 downto 0);
        variable ub : unsigned(b'length - 1 downto 0);
    begin
        to_u(a, ua); to_u(b, ub);
        return unsigned_to_l3d(ua - ub);
    end function;

    function "+"(a : logic3d_vector; b : natural) return logic3d_vector is
        variable ua : unsigned(a'length - 1 downto 0);
    begin
        to_u(a, ua);
        return unsigned_to_l3d(ua + b);
    end function;

    -- LSB-first, low 31 bits only. A VHDL natural holds 31 value bits, so a
    -- wider operand (or an uninitialized/X index at time 0, e.g. a slice base
    -- before reset) must not overflow the accumulator: the old MSB-first
    -- result*2 traps the instant result reaches 2**31. Building from the LSB
    -- with 2**p (p<31) gives the same low-31-bit value with no overflow.
    function to_integer(a : logic3d_vector) return natural is
        variable result : natural := 0;
        variable p      : natural := 0;
    begin
        for i in a'reverse_range loop
            if p < 31 then
                -- Value-plane only: an uncertain bit still contributes its
                -- VALUE (doctrine: certainty is metadata and never gates a
                -- computation). The old return-0-on-uncertain guard silently
                -- redirected dynamic indexes to element 0 whenever init-X
                -- certainty rode along on a correct value (EH2 icache
                -- miss-buffer sibling lookup dropped wrap-fill beat pairs).
                -- is_one is value-plane, so the result stays in range.
                if is_one(a(i)) then result := result + 2**p; end if;
            end if;
            p := p + 1;
        end loop;
        return result;
    end function;

    function l3d_shcount(a : logic3d_vector) return natural is
        constant HUGE : natural := 2**20;   -- bigger than any real vector width
        variable result : natural := 0;
        variable p      : natural := 0;
    begin
        for i in a'reverse_range loop
            if is_uncertain(a(i)) then return 0; end if;  -- X count: as to_integer
            if is_one(a(i)) then
                if p >= 20 then return HUGE; end if;      -- overflow: saturate
                result := result + 2**p;
            end if;
            p := p + 1;
        end loop;
        return result;
    end function;

    function "sll"(a : logic3d_vector; n : natural) return logic3d_vector is
    begin
        return unsigned_to_l3d(l3d_to_unsigned(a) sll n);
    end function;

    function "srl"(a : logic3d_vector; n : natural) return logic3d_vector is
    begin
        return unsigned_to_l3d(l3d_to_unsigned(a) srl n);
    end function;

    function shift_left(a : logic3d_vector; n : natural) return logic3d_vector is
    begin
        return unsigned_to_l3d(ieee.numeric_std.shift_left(l3d_to_unsigned(a), n));
    end function;

    function shift_right(a : logic3d_vector; n : natural) return logic3d_vector is
    begin
        return unsigned_to_l3d(ieee.numeric_std.shift_right(l3d_to_unsigned(a), n));
    end function;

    -- Arithmetic right shift: reinterpret as signed so the sign bit fills in.
    function l3d_sra(a : logic3d_vector; n : natural) return logic3d_vector is
    begin
        return unsigned_to_l3d(unsigned(std_logic_vector(
            ieee.numeric_std.shift_right(
                signed(std_logic_vector(l3d_to_unsigned(a))), n))));
    end function;

    -- Signed relational operators (numeric_std signed compare sign-extends the
    -- shorter operand internally, so no pre-resize is needed).
    function l3d_lt_s(a, b : logic3d_vector) return boolean is
    begin
        return signed(std_logic_vector(l3d_to_unsigned(a)))
             < signed(std_logic_vector(l3d_to_unsigned(b)));
    end function;
    function l3d_gt_s(a, b : logic3d_vector) return boolean is
    begin
        return signed(std_logic_vector(l3d_to_unsigned(a)))
             > signed(std_logic_vector(l3d_to_unsigned(b)));
    end function;
    function l3d_le_s(a, b : logic3d_vector) return boolean is
    begin
        return signed(std_logic_vector(l3d_to_unsigned(a)))
             <= signed(std_logic_vector(l3d_to_unsigned(b)));
    end function;
    function l3d_ge_s(a, b : logic3d_vector) return boolean is
    begin
        return signed(std_logic_vector(l3d_to_unsigned(a)))
             >= signed(std_logic_vector(l3d_to_unsigned(b)));
    end function;

    -- Signed division / modulus. Verilog defines a/0, a%0 as all-x.
    function l3d_div_s(a, b : logic3d_vector) return logic3d_vector is
        variable allx : logic3d_vector(a'range) := (others => L3D_X);
    begin
        if l3d_to_unsigned(b) = 0 then return allx; end if;
        return unsigned_to_l3d(unsigned(std_logic_vector(ieee.numeric_std.resize(
            signed(std_logic_vector(l3d_to_unsigned(a)))
          / signed(std_logic_vector(l3d_to_unsigned(b))), a'length))));
    end function;
    -- Verilog's %: rem, the dividend's sign (VHDL mod takes the divisor's,
    -- -7 % 3 gave 2 where Verilog gives -1); as tgt-vhdl's Verilog_Rem_S.
    function l3d_mod_s(a, b : logic3d_vector) return logic3d_vector is
        variable allx : logic3d_vector(a'range) := (others => L3D_X);
    begin
        if l3d_to_unsigned(b) = 0 then return allx; end if;
        return unsigned_to_l3d(unsigned(std_logic_vector(ieee.numeric_std.resize(
            signed(std_logic_vector(l3d_to_unsigned(a)))
            rem signed(std_logic_vector(l3d_to_unsigned(b))), a'length))));
    end function;

    -- Sign-extending resize: reinterpret as signed so numeric_std.resize fills
    -- the new high bits with the sign bit (truncation keeps the low bits).
    function l3d_resize_s(a : logic3d_vector; new_size : natural) return logic3d_vector is
    begin
        return unsigned_to_l3d(unsigned(std_logic_vector(ieee.numeric_std.resize(
            signed(std_logic_vector(l3d_to_unsigned(a))), new_size))));
    end function;

    function "*"(a, b : logic3d_vector) return logic3d_vector is
        variable result : unsigned(a'length + b'length - 1 downto 0);
    begin
        result := l3d_to_unsigned(a) * l3d_to_unsigned(b);
        return unsigned_to_l3d(result(a'length - 1 downto 0));
    end function;

    -- Verilog defines a/0, a%0 (and rem) as x (all bits unknown). numeric_std
    -- happens to return all-X for "/" by zero but the DIVIDEND for "mod"/"rem"
    -- by zero, so guard the zero divisor explicitly and return all-x.
    function "/"(a, b : logic3d_vector) return logic3d_vector is
        variable allx : logic3d_vector(a'range) := (others => L3D_X);
    begin
        if l3d_to_unsigned(b) = 0 then return allx; end if;
        return unsigned_to_l3d(l3d_to_unsigned(a) / l3d_to_unsigned(b));
    end function;

    function "mod"(a, b : logic3d_vector) return logic3d_vector is
        variable allx : logic3d_vector(a'range) := (others => L3D_X);
    begin
        if l3d_to_unsigned(b) = 0 then return allx; end if;
        return unsigned_to_l3d(l3d_to_unsigned(a) mod l3d_to_unsigned(b));
    end function;

    function "rem"(a, b : logic3d_vector) return logic3d_vector is
        variable allx : logic3d_vector(a'range) := (others => L3D_X);
    begin
        if l3d_to_unsigned(b) = 0 then return allx; end if;
        return unsigned_to_l3d(l3d_to_unsigned(a) rem l3d_to_unsigned(b));
    end function;

    function resize(a : logic3d_vector; new_size : natural) return logic3d_vector is
        variable result : logic3d_vector(new_size - 1 downto 0) := (others => L3D_0);
    begin
        -- `a` may be a non-zero-based slice (e.g. ic_data(851 downto 568)), so
        -- index from a'low, not 0 -- a(new_size-1 downto 0) would be out of
        -- range. Truncate/extend LSB-aligned.
        if new_size <= a'length then
            result := a(a'low + new_size - 1 downto a'low);
        else
            result(a'length - 1 downto 0) := a;
        end if;
        return result;
    end function;

    function "/="(a : logic3d_vector; b : integer) return boolean is
    begin
        return to_integer(a) /= b;
    end function;

    function "="(a : logic3d_vector; b : integer) return boolean is
    begin
        return to_integer(a) = b;
    end function;

    function "and"(a, b : boolean) return logic3d is
    begin
        if a and b then return L3D_1; else return L3D_0; end if;
    end function;

    function "or"(a, b : boolean) return logic3d is
    begin
        if a or b then return L3D_1; else return L3D_0; end if;
    end function;

    function "xor"(a, b : boolean) return logic3d is
    begin
        if a xor b then return L3D_1; else return L3D_0; end if;
    end function;

    function "not"(a : boolean) return logic3d is
    begin
        if a then return L3D_0; else return L3D_1; end if;
    end function;

    function "&"(a : logic3d_vector; b : logic3d) return logic3d_vector is
        variable result : logic3d_vector(a'length downto 0);
    begin
        result(a'length downto 1) := a;
        result(0) := b;
        return result;
    end function;

    function "&"(a : logic3d; b : logic3d_vector) return logic3d_vector is
        variable result : logic3d_vector(b'length downto 0);
    begin
        result(b'length) := a;
        result(b'length - 1 downto 0) := b;
        return result;
    end function;

    function "&"(a : logic3d; b : logic3d) return logic3d_vector is
        variable result : logic3d_vector(1 downto 0);
    begin
        result(1) := a;
        result(0) := b;
        return result;
    end function;

    -- std_logic_vector-returning variants for contexts where the LHS is
    -- typed as std_logic_vector (iverilog temporaries created from $bits
    -- operands, or testbench wires declared with std_logic_vector).
    function "&"(a : logic3d_vector; b : logic3d_vector) return std_logic_vector is
        variable result : std_logic_vector(a'length + b'length - 1 downto 0);
        variable i : integer := result'high;
    begin
        for j in a'range loop result(i) := to_std_logic(a(j)); i := i - 1; end loop;
        for j in b'range loop result(i) := to_std_logic(b(j)); i := i - 1; end loop;
        return result;
    end function;

    function "&"(a : logic3d_vector; b : logic3d) return std_logic_vector is
        variable result : std_logic_vector(a'length downto 0);
        variable i : integer := result'high;
    begin
        for j in a'range loop result(i) := to_std_logic(a(j)); i := i - 1; end loop;
        result(0) := to_std_logic(b);
        return result;
    end function;

    function "&"(a : logic3d; b : logic3d_vector) return std_logic_vector is
        variable result : std_logic_vector(b'length downto 0);
        variable i : integer := result'high - 1;
    begin
        result(result'high) := to_std_logic(a);
        for j in b'range loop result(i) := to_std_logic(b(j)); i := i - 1; end loop;
        return result;
    end function;

    function "&"(a : logic3d; b : logic3d) return std_logic_vector is
        variable result : std_logic_vector(1 downto 0);
    begin
        result(1) := to_std_logic(a);
        result(0) := to_std_logic(b);
        return result;
    end function;

    function "<"(a, b : logic3d_vector) return boolean is
        variable ua : unsigned(a'length - 1 downto 0);
        variable ub : unsigned(b'length - 1 downto 0);
    begin
        to_u(a, ua); to_u(b, ub);
        return ua < ub;
    end function;

    function "<="(a, b : logic3d_vector) return boolean is
        variable ua : unsigned(a'length - 1 downto 0);
        variable ub : unsigned(b'length - 1 downto 0);
    begin
        to_u(a, ua); to_u(b, ub);
        return ua <= ub;
    end function;

    function ">"(a, b : logic3d_vector) return boolean is
        variable ua : unsigned(a'length - 1 downto 0);
        variable ub : unsigned(b'length - 1 downto 0);
    begin
        to_u(a, ua); to_u(b, ub);
        return ua > ub;
    end function;

    function ">="(a, b : logic3d_vector) return boolean is
        variable ua : unsigned(a'length - 1 downto 0);
        variable ub : unsigned(b'length - 1 downto 0);
    begin
        to_u(a, ua); to_u(b, ub);
        return ua >= ub;
    end function;

    function "="(a, b : logic3d_vector) return boolean is
        variable ua : unsigned(a'length - 1 downto 0);
        variable ub : unsigned(b'length - 1 downto 0);
    begin
        to_u(a, ua); to_u(b, ub);
        return ua = ub;
    end function;

    function "/="(a, b : logic3d_vector) return boolean is
        variable ua : unsigned(a'length - 1 downto 0);
        variable ub : unsigned(b'length - 1 downto 0);
    begin
        to_u(a, ua); to_u(b, ub);
        return ua /= ub;
    end function;

    function unsigned_to_l3d_bit(a : unsigned) return logic3d is
    begin
        if a(a'right) = '1' then return L3D_1; else return L3D_0; end if;
    end function;

    -- ---- Sequential UDP evaluation (mirrors vvp/udp.cc) --------------------
    function sl1(v : std_logic) return std_logic_vector is
        variable r : std_logic_vector(0 to 0);
    begin
        r(0) := v;
        return r;
    end function;

    -- Class of a std_logic value for UDP matching: '0', '1', or 'x'.
    function udp_cls(v : std_logic) return character is
    begin
        case v is
            when '0' | 'L' => return '0';
            when '1' | 'H' => return '1';
            when others    => return 'x';
        end case;
    end function;

    -- Does a UDP level char c cover value-class vc ('0'/'1'/'x')?
    function udp_level_covers(c : character; vc : character) return boolean is
    begin
        case c is
            when '0' => return vc = '0';
            when '1' => return vc = '1';
            when 'x' => return vc = 'x';
            when 'b' => return vc = '0' or vc = '1';
            when 'l' => return vc = '0' or vc = 'x';
            when 'h' => return vc = '1' or vc = 'x';
            when '?' => return true;
            when others => return false;   -- not a level char
        end case;
    end function;

    function udp_is_edge(c : character) return boolean is
    begin
        case c is
            when '0'|'1'|'x'|'b'|'l'|'h'|'?' => return false;
            when others => return true;
        end case;
    end function;

    -- Edge char: which classes the CURRENT value may be (vvp mask0/1/x).
    function udp_edge_cur(c : character; vc : character) return boolean is
    begin
        case c is
            when 'B' => return vc='0' or vc='1';   -- (x?)
            when 'f' => return vc='0';             -- (10)
            when 'F' => return vc='0';             -- (x0)
            when 'M' => return vc='x';             -- (1x)
            when 'N' => return vc='0' or vc='x';
            when 'P' => return vc='1' or vc='x';
            when 'q' => return vc='x';             -- (bx)
            when 'Q' => return vc='x';             -- (0x)
            when 'r' => return vc='1';             -- (01)
            when 'R' => return vc='1';             -- (x1)
            when '%' => return vc='x';             -- (?x)
            when '+' => return vc='1';             -- (?1)
            when '_' => return vc='0';             -- (?0)
            when others => return false;
        end case;
    end function;

    -- Edge char: which classes the PREVIOUS value may be (vvp edge_mask*).
    function udp_edge_prev(c : character; vc : character) return boolean is
    begin
        case c is
            when 'B' => return vc='x';
            when 'f' => return vc='1';
            when 'F' => return vc='x';
            when 'M' => return vc='1';
            when 'N' => return vc='1';
            when 'P' => return vc='0';
            when 'q' => return vc='0' or vc='1';
            when 'Q' => return vc='0';
            when 'r' => return vc='0';
            when 'R' => return vc='x';
            when '%' => return vc='0' or vc='1';
            when '+' => return vc='0' or vc='x';
            when '_' => return vc='1' or vc='x';
            when others => return false;
        end case;
    end function;

    -- Does edge char c match the transition prev->cur (both std_logic)?
    -- '*' is "any edge" (any value change); the rest use the per-char masks.
    function udp_edge_matches(c : character; cur : std_logic;
                              prev : std_logic) return boolean is
        constant cc : character := udp_cls(cur);
        constant pc : character := udp_cls(prev);
    begin
        if c = '*' then
            return cc /= pc;   -- any transition
        end if;
        return udp_edge_cur(c, cc) and udp_edge_prev(c, pc);
    end function;

    -- Map a UDP output char to std_logic ('-' = hold current output).
    function udp_out(c : character; cur_out : std_logic) return std_logic is
    begin
        case c is
            when '0' => return '0';
            when '1' => return '1';
            when 'x' => return 'X';
            when '-' => return cur_out;
            when others => return 'X';
        end case;
    end function;

    function sv_udp_seq(rows : string; nin : natural; cur_out : std_logic;
                        prev_in : std_logic_vector;
                        cur_in : std_logic_vector) return std_logic is
        constant rowlen : natural := nin + 2;
        constant nrows  : natural := rows'length / rowlen;
        constant coc    : character := udp_cls(cur_out);
        variable base   : natural;
        variable ok     : boolean;
        variable ecol   : integer;
    begin
        -- Pass 1: pure-level rows (no edge column), in table order.
        for r in 0 to nrows - 1 loop
            base := rows'left + r * rowlen;
            ok := true;
            for k in 0 to nin - 1 loop
                if udp_is_edge(rows(base + 1 + k)) then ok := false; end if;
            end loop;
            if ok and udp_level_covers(rows(base), coc) then
                ok := true;
                for k in 0 to nin - 1 loop
                    if not udp_level_covers(rows(base + 1 + k),
                                            udp_cls(cur_in(cur_in'low + k))) then
                        ok := false;
                    end if;
                end loop;
                if ok then
                    return udp_out(rows(base + nin + 1), cur_out);
                end if;
            end if;
        end loop;
        -- Pass 2: edge rows (single edge column) — needs a real transition.
        for r in 0 to nrows - 1 loop
            base := rows'left + r * rowlen;
            ecol := -1;
            for k in 0 to nin - 1 loop
                if udp_is_edge(rows(base + 1 + k)) then ecol := k; end if;
            end loop;
            if ecol >= 0 and udp_level_covers(rows(base), coc) then
                ok := udp_edge_matches(rows(base + 1 + ecol),
                                       cur_in(cur_in'low + ecol),
                                       prev_in(prev_in'low + ecol));
                if ok then
                    for k in 0 to nin - 1 loop
                        if k /= ecol and
                           not udp_level_covers(rows(base + 1 + k),
                                                udp_cls(cur_in(cur_in'low + k))) then
                            ok := false;
                        end if;
                    end loop;
                end if;
                if ok then
                    return udp_out(rows(base + nin + 1), cur_out);
                end if;
            end if;
        end loop;
        -- No match: hold current output.
        return cur_out;
    end function;

    ---------------------------------------------------------------------------
    -- Verilog real <-> logic3d conversions
    ---------------------------------------------------------------------------
    function l3d_to_real(a : logic3d_vector) return real is
        variable r : real := 0.0;
    begin
        for i in a'range loop      -- (msb downto lsb): MSB first
            r := r * 2.0;
            if is_one(a(i)) then
                r := r + 1.0;
            end if;
        end loop;
        return r;
    end function;

    function l3d_to_real(a : logic3d) return real is
    begin
        if is_one(a) then return 1.0; else return 0.0; end if;
    end function;

    function l3d_to_real_s(a : logic3d_vector) return real is
        variable r : real := l3d_to_real(a);
    begin
        if a'length > 0 and is_one(a(a'left)) then
            r := r - 2.0 ** a'length;
        end if;
        return r;
    end function;

    function real_to_l3d(r : real; w : natural) return logic3d_vector is
        constant m   : real := 2.0 ** w;
        variable x   : real;
        variable res : logic3d_vector(w - 1 downto 0) := (others => L3D_0);
    begin
        -- Round to nearest, half away from zero (Verilog conversion rule;
        -- math_real round() has exactly these semantics), then wrap the
        -- magnitude into w bits and two's-complement a negative value.
        x := ieee.math_real.round(abs(r));
        x := x - ieee.math_real.trunc(x / m) * m;   -- |value| mod 2**w
        if r < 0.0 and x /= 0.0 then
            x := m - x;
        end if;
        for i in 0 to w - 1 loop
            if x - ieee.math_real.trunc(x / 2.0) * 2.0 >= 1.0 then
                res(i) := L3D_1;
            end if;
            x := ieee.math_real.trunc(x / 2.0);
        end loop;
        return res;
    end function;

    function real_to_l3d1(r : real) return logic3d is
        variable v : logic3d_vector(0 downto 0);
    begin
        v := real_to_l3d(r, 1);
        return v(0);
    end function;

    function l3d_to_2state(a : logic3d) return logic3d is
    begin
        if is_one(a) and not is_uncertain(a) then
            return L3D_1;
        else
            return L3D_0;
        end if;
    end function;

    function l3d_to_2state(a : logic3d_vector) return logic3d_vector is
        variable res : logic3d_vector(a'range);
    begin
        for i in a'range loop
            res(i) := l3d_to_2state(a(i));
        end loop;
        return res;
    end function;

    function to_l3d(a : unsigned; w : natural) return logic3d_vector is
    begin
        return unsigned_to_l3d(resize(a, w));
    end function;

    function to_l3d(a : signed; w : natural) return logic3d_vector is
    begin
        return unsigned_to_l3d(unsigned(resize(a, w)));   -- sign-extends first
    end function;

    function to_l3d(a : integer; w : natural) return logic3d_vector is
    begin
        return unsigned_to_l3d(unsigned(to_signed(a, w)));
    end function;

    function to_l3d(a : logic3d_vector; w : natural) return logic3d_vector is
    begin
        if a'length = w then return a; end if;
        return resize(a, w);      -- pkg resize (x-preserving)
    end function;

    function l3d_index(a : logic3d_vector; s : boolean) return integer is
    begin
        -- Uncertain bits index by their value plane (3D-logic doctrine: reads
        -- use the 1/0 value, never the certainty, so an all-X address is
        -- address 0 rather than an X-propagating bounds failure).
        if s then
            return to_integer(l3d_to_signed(a));
        end if;
        return to_integer(l3d_to_unsigned(a));
    end function;

    function l3d_part_read(a : logic3d_vector; base : integer; w : natural)
        return logic3d_vector is
        variable r : logic3d_vector(w - 1 downto 0) := (others => L3D_X);
    begin
        -- Early bail keeps base + i from overflowing when base is the
        -- uncertain-index sentinel (integer'low) or similar extremes.
        if base > a'high or base < a'low - integer(w) then
            return r;
        end if;
        for i in 0 to w - 1 loop
            if base + i >= a'low and base + i <= a'high then
                r(i) := a(base + i);
            end if;
        end loop;
        return r;
    end function;

    function l3d_bit_read(a : logic3d_vector; idx : integer) return logic3d is
    begin
        if idx >= a'low and idx <= a'high then
            return a(idx);
        end if;
        return L3D_X;
    end function;

    function l3d_neg(a : logic3d_vector) return logic3d_vector is
    begin
        for i in a'range loop
            if is_uncertain(a(i)) then return (a'range => L3D_X); end if;
        end loop;
        return unsigned_to_l3d(unsigned(-signed(std_logic_vector(
                   l3d_to_unsigned(a)))));
    end function;

    function l3d_pow(b, e : logic3d_vector;
                     bs, es : boolean) return logic3d_vector is
        constant w   : natural := b'length;
        variable bu  : unsigned(w - 1 downto 0);
        variable acc : unsigned(w - 1 downto 0) := (others => '0');
        variable all_ones : boolean := true;
    begin
        -- Any uncertain bit makes the whole result X (Verilog **).
        for i in b'range loop
            if is_uncertain(b(i)) then return (b'range => L3D_X); end if;
        end loop;
        for i in e'range loop
            if is_uncertain(e(i)) then return (b'range => L3D_X); end if;
        end loop;

        bu := l3d_to_unsigned(b);

        if es and e'length > 0 and is_one(e(e'left)) then
            -- Negative exponent: 1**n=1; (-1)**n = +/-1 by parity (signed
            -- all-ones base); 0**-n = X (division by zero); else 0.
            for i in b'range loop
                if not is_one(b(i)) then all_ones := false; end if;
            end loop;
            if to_integer(bu) = 1 then
                acc := to_unsigned(1, w);
            elsif bs and all_ones then          -- base = -1
                if is_one(e(e'right)) then      -- odd exponent
                    return b;                    -- -1 (all ones)
                else
                    acc := to_unsigned(1, w);
                end if;
            elsif to_integer(bu) = 0 then
                return (b'range => L3D_X);
            end if;                              -- else acc stays 0
        else
            -- Square-and-multiply over exponent bits, MSB first, truncating
            -- to w bits each step (Verilog wraps at the result width).
            acc := to_unsigned(1, w);
            for i in e'range loop
                acc := resize(acc * acc, w);
                if is_one(e(i)) then
                    acc := resize(acc * bu, w);
                end if;
            end loop;
        end if;
        return unsigned_to_l3d(acc);
    end function;

    function l3d_truthy(a : logic3d_vector) return logic3d is
    begin
        for i in a'range loop
            if is_one(a(i)) then return L3D_1; end if;
        end loop;
        return L3D_0;
    end function;

    function l3d_truthy(a : logic3d) return logic3d is
    begin
        return a;
    end function;

    function l3d_isunknown(a : logic3d_vector) return logic3d is
    begin
        for i in a'range loop
            if is_uncertain(a(i)) then return L3D_1; end if;
        end loop;
        return L3D_0;
    end function;

    function l3d_isunknown(a : logic3d) return logic3d is
    begin
        if is_uncertain(a) then return L3D_1; end if;
        return L3D_0;
    end function;

    function l3d_eq1(a, b : logic3d) return boolean is
    begin
        return is_one(a) = is_one(b);
    end function;

    function l3d_eq1(a, b : logic3d_vector) return boolean is
    begin
        return a = b;   -- the value-plane boolean "=" above
    end function;

    function l3d_eq1(a : logic3d_vector; b : logic3d) return boolean is
        variable ua : unsigned(a'length - 1 downto 0);
    begin
        to_u(a, ua);
        return to_integer(ua) = to_integer(l3d_to_unsigned(b));
    end function;

    function l3d_eq1(a : logic3d; b : logic3d_vector) return boolean is
    begin
        return l3d_eq1(b, a);
    end function;

    -- Gate-operator overloads (thin wrappers over the l3d_* primitives).
    function "not" (a : logic3d) return logic3d is begin return l3d_not(a);     end function;
    function "and" (a, b : logic3d) return logic3d is begin return l3d_and(a, b);  end function;
    function "or"  (a, b : logic3d) return logic3d is begin return l3d_or(a, b);   end function;
    function "xor" (a, b : logic3d) return logic3d is begin return l3d_xor(a, b);  end function;
    function "nand"(a, b : logic3d) return logic3d is begin return l3d_nand(a, b); end function;
    function "nor" (a, b : logic3d) return logic3d is begin return l3d_nor(a, b);  end function;
    function "xnor"(a, b : logic3d) return logic3d is begin return l3d_xnor(a, b); end function;
    function "not" (a : logic3d_vector) return logic3d_vector is begin return l3d_not(a);     end function;
    function "and" (a, b : logic3d_vector) return logic3d_vector is begin return l3d_and(a, b);  end function;
    function "or"  (a, b : logic3d_vector) return logic3d_vector is begin return l3d_or(a, b);   end function;
    function "xor" (a, b : logic3d_vector) return logic3d_vector is begin return l3d_xor(a, b);  end function;
    function "nand"(a, b : logic3d_vector) return logic3d_vector is begin return l3d_nand(a, b); end function;
    function "nor" (a, b : logic3d_vector) return logic3d_vector is begin return l3d_nor(a, b);  end function;
    function "xnor"(a, b : logic3d_vector) return logic3d_vector is begin return l3d_xnor(a, b); end function;

    -- ---------------------------------------------------------------------
    -- $readmemh / $readmemb
    -- ---------------------------------------------------------------------
    type t_rm_lines is array (natural range <>) of std.textio.line;
    type t_rm_lines_ptr is access t_rm_lines;
    type t_rm_ints is array (natural range <>) of integer;
    type t_rm_ints_ptr is access t_rm_ints;

    type t_readmem is protected
        procedure clear(hex : boolean);
        procedure add(addr : integer; word : string);
        impure function count return natural;
        impure function addr(i : natural) return integer;
        impure function word(i : natural; w : positive) return logic3d_vector;
    end protected;

    type t_readmem is protected body
        variable words : t_rm_lines_ptr := null;
        variable addrs : t_rm_ints_ptr := null;
        variable n     : natural := 0;
        variable is_hex : boolean := true;

        procedure clear(hex : boolean) is
        begin
            if words /= null then
                for i in 0 to n - 1 loop
                    deallocate(words(i));
                end loop;
            end if;
            n := 0;
            is_hex := hex;
        end procedure;

        procedure add(addr : integer; word : string) is
            variable nw : t_rm_lines_ptr;
            variable na : t_rm_ints_ptr;
        begin
            if words = null then
                words := new t_rm_lines(0 to 255);
                addrs := new t_rm_ints(0 to 255);
            elsif n > words'high then
                nw := new t_rm_lines(0 to 2 * words'length - 1);
                na := new t_rm_ints(0 to 2 * addrs'length - 1);
                nw(0 to n - 1) := words(0 to n - 1);
                na(0 to n - 1) := addrs(0 to n - 1);
                deallocate(words); deallocate(addrs);
                words := nw; addrs := na;
            end if;
            words(n) := new string'(word);
            addrs(n) := addr;
            n := n + 1;
        end procedure;

        impure function count return natural is
        begin
            return n;
        end function;

        impure function addr(i : natural) return integer is
        begin
            return addrs(i);
        end function;

        -- vvp's make_hex_value / make_bin_value: the digits fill the word
        -- from the right, x and z only where their digit is; the bits above
        -- the digits are 0 and digits beyond the word are dropped.
        impure function word(i : natural; w : positive) return logic3d_vector is
            variable r    : logic3d_vector(w - 1 downto 0) := (others => L3D_0);
            constant s    : string(1 to words(i)'length) := words(i).all;
            variable bpd  : natural;
            variable bpos : natural := 0;
            variable v    : natural;
            variable c    : character;
        begin
            if is_hex then bpd := 4; else bpd := 1; end if;
            for k in s'high downto s'low loop
                exit when bpos >= w;
                c := s(k);
                for b in 0 to bpd - 1 loop
                    if bpos + b <= w - 1 then
                        case c is
                            when 'x' | 'X' => r(bpos + b) := L3D_X;
                            when 'z' | 'Z' => r(bpos + b) := L3D_Z;
                            when others =>
                                if c >= '0' and c <= '9' then
                                    v := character'pos(c) - character'pos('0');
                                elsif c >= 'a' and c <= 'f' then
                                    v := character'pos(c) - character'pos('a') + 10;
                                else
                                    v := character'pos(c) - character'pos('A') + 10;
                                end if;
                                if (v / 2 ** b) mod 2 = 1 then
                                    r(bpos + b) := L3D_1;
                                end if;
                        end case;
                    end if;
                end loop;
                bpos := bpos + bpd;
            end loop;
            return r;
        end function;
    end protected body;

    shared variable g_readmem : t_readmem;

    -- The messages of the last $readmem/$writemem call (sv_memfile_msgs)
    type t_msgs is protected
        procedure add(s : string);
        impure function take return string;
    end protected;

    type t_msgs is protected body
        variable buf   : std.textio.line := null;
        variable taken : boolean := false;     -- buf was returned by take

        procedure forget is
        begin
            if taken then
                deallocate(buf);
                taken := false;
            end if;
        end procedure;

        procedure add(s : string) is
            variable old : std.textio.line;
        begin
            forget;
            if buf = null then
                buf := new string'(s & LF);
            else
                old := buf;
                buf := new string'(old.all & s & LF);
                deallocate(old);
            end if;
        end procedure;

        impure function take return string is
        begin
            forget;
            if buf = null then
                return "";
            end if;
            taken := true;
            return buf.all;
        end function;
    end protected body;

    shared variable g_memfile_msgs : t_msgs;

    impure function sv_memfile_msgs return string is
    begin
        return g_memfile_msgs.take;
    end function;

    -- nvc's getenv (std.env.getenv is VHDL-2019's; the library is --std=2040)
    impure function sv_getenv(name : string) return string is
    begin
        return "";
    end function;
    attribute foreign of sv_getenv [string return string] : function is
        "INTERNAL _std_env_getenv";

    impure function sv_file_path(name : string) return string is
        constant dir : string := sv_getenv("SV2VHDL_FILE_DIR");
    begin
        if dir'length = 0 or name'length = 0 or name(name'left) = '/' then
            return name;
        end if;
        return dir & "/" & name;
    end function;

    -- C's isprint in the C locale
    function c_isprint(c : character) return boolean is
    begin
        return character'pos(c) >= 32 and character'pos(c) < 127;
    end function;

    -- the three octal digits of 0 <= n < 512
    function oct3(n : natural) return string is
    begin
        return character'val(48 + (n / 64) mod 8) & character'val(48 + (n / 8) mod 8)
            & character'val(48 + n mod 8);
    end function;

    -- vvp's as_escaped: a non-printable character as sprintf("\\%03o") of
    -- a signed char (one above 127 as its 32-bit two's complement)
    function c_escaped(s : string) return string is
        variable r : string(1 to 12 * s'length + 1);
        variable k : natural := 0;
        variable c : natural;
    begin
        for i in s'range loop
            c := character'pos(s(i));
            if c_isprint(s(i)) then
                k := k + 1;
                r(k) := s(i);
            elsif c < 128 then
                r(k + 1 to k + 4) := "\" & oct3(c);
                k := k + 4;
            else
                r(k + 1 to k + 12) := "\37777777" & oct3(c + 256);
                k := k + 12;
            end if;
        end loop;
        return r(1 to k);
    end function;

    function sv_file_name_msg(name : string; kind : string; task : string;
                              loc : string) return string is
        constant head : string := "WARNING: " & loc & ":";
        -- vvp formats that head in a 64-byte buffer
        function cut(s : string) return string is
        begin
            if s'length > 63 then
                return s(s'left to s'left + 62);
            end if;
            return s;
        end function;
        constant h63 : string := cut(head);
    begin
        if name'length = 0 then
            return head & " " & task & "'s file name argument (" & kind
                & ") is not a valid string." & LF;
        end if;
        for i in name'range loop
            if not c_isprint(name(i)) then
                return h63 & " " & task & "'s file name argument contains "
                    & "non-printable characters." & LF
                    & (1 to h63'length => ' ') & " """ & c_escaped(name) & """"
                    & LF;
            end if;
        end loop;
        return "";
    end function;

    -- a bit vvp's vpi values count as 1 (BIT4_1: a strong or weak 1)
    function vpi_one(b : logic3d) return boolean is
    begin
        return b = L3D_1 or b = L3D_H;
    end function;

    function sv_vec_str(v : logic3d_vector) return string is
        constant n   : natural := v'length;
        alias av     : logic3d_vector(n - 1 downto 0) is v;
        variable r   : string(1 to (n + 7) / 8);
        variable k   : natural := 0;
        variable c   : natural := 0;
    begin
        for i in n - 1 downto 0 loop
            c := c * 2;
            if vpi_one(av(i)) then
                c := c + 1;
            end if;
            if i mod 8 = 0 then
                if c /= 0 or k > 0 then
                    k := k + 1;
                    if c = 0 then
                        r(k) := ' ';
                    else
                        r(k) := character'val(c);
                    end if;
                end if;
                c := 0;
            end if;
        end loop;
        return r(1 to k);
    end function;

    function sv_vec_str(v : logic3d) return string is
    begin
        return sv_vec_str(logic3d_vector'(0 => v));
    end function;

    function sv_vec_int32(v : logic3d_vector; is_signed : boolean) return integer is
        constant n : natural := v'length;
        alias av   : logic3d_vector(n - 1 downto 0) is v;
        variable low : natural := 0;    -- bits 30 .. 0
        variable b31 : boolean;
    begin
        if n = 0 then
            return 0;
        end if;
        for i in 30 downto 0 loop
            low := low * 2;
            if i < n then
                if vpi_one(av(i)) then
                    low := low + 1;
                end if;
            elsif is_signed and vpi_one(av(n - 1)) then
                low := low + 1;                  -- sign extension
            end if;
        end loop;
        if n > 31 then
            b31 := vpi_one(av(31));
        else
            b31 := is_signed and vpi_one(av(n - 1));
        end if;
        if b31 then
            return -2147483647 - 1 + low;
        end if;
        return low;
    end function;

    function sv_vec_int32(v : logic3d; is_signed : boolean) return integer is
    begin
        return sv_vec_int32(logic3d_vector'(0 => v), is_signed);
    end function;

    function dec(i : integer) return string is
    begin
        return integer'image(i);
    end function;

    -- C's "%x" of an int (its 32 bits, no leading zeros); pad: "%08x"
    function hex_c(i : integer; pad : boolean := false) return string is
        constant HEXD : string(1 to 16) := "0123456789abcdef";
        variable u : unsigned(31 downto 0) := unsigned(to_signed(i, 32));
        variable r : string(1 to 8);
    begin
        for k in 0 to 7 loop
            r(8 - k) := HEXD(to_integer(u(4 * k + 3 downto 4 * k)) + 1);
        end loop;
        if pad then
            return r;
        end if;
        for k in 1 to 7 loop
            if r(k) /= '0' then
                return r(k to 8);
            end if;
        end loop;
        return r(8 to 8);
    end function;

    -- vvp's get_mem_params (after the file name) and process_params: the
    -- addresses of a $readmem/$writemem call, or ok = false (its messages
    -- are in g_memfile_msgs)
    procedure mem_params(fname : string; kind : string; task : string;
                         loc : string; mem : string;
                         left_a : integer; right_a : integer;
                         has_start : boolean; start_a : integer;
                         start_real : boolean;
                         has_stop : boolean; stop_a : integer;
                         stop_real : boolean;
                         start_v : out integer; stop_v : out integer;
                         incr : out integer; min_a : out integer;
                         max_a : out integer; ok : out boolean) is
        constant name_msg : string := sv_file_name_msg(fname, kind, task, loc);
        constant lo : integer := minimum(left_a, right_a);
        constant hi : integer := maximum(left_a, right_a);
        variable s, e : integer;
        constant range_txt : string := "'" & mem & "[" & dec(left_a) & ":"
            & dec(right_a) & "]'!";
    begin
        ok := false;
        start_v := 0; stop_v := 0; incr := 1; min_a := 0; max_a := 0;
        if name_msg'length > 0 then
            -- the message already ends in LF: add it without another
            g_memfile_msgs.add(name_msg(name_msg'left to name_msg'right - 1));
        end if;
        if has_start and start_real then
            g_memfile_msgs.add("WARNING: " & loc & ": " & task & "'s third "
                               & "argument (start address) is a real value.");
        end if;
        if has_start and has_stop and stop_real then
            g_memfile_msgs.add("WARNING: " & loc & ": " & task & "'s fourth "
                               & "argument (finish address) is a real value.");
        end if;
        if name_msg'length > 0 then
            return;
        end if;
        if not has_start then
            s := lo;
            e := hi;
            incr := 1;
        elsif not has_stop then
            s := start_a;
            e := hi;
            incr := 1;
        else
            s := start_a;
            e := stop_a;
            if s < e then
                incr := 1;
            else
                incr := -1;
            end if;
        end if;
        start_v := s;
        stop_v := e;
        min_a := minimum(s, e);
        max_a := maximum(s, e);
        if not has_stop and left_a > right_a then
            g_memfile_msgs.add("WARNING: " & loc & ": " & task & ": The behaviour "
                & "for reg[...] mem[N:0]; " & task & "(""..."", mem); changed in "
                & "the 1364-2005 standard. To avoid ambiguity, use mem[0:N] or "
                & "explicit range parameters " & task & "(""..."", mem, start, "
                & "stop);. Defaulting to 1364-2005 behavior.");
        end if;
        if s < lo or s > hi then
            g_memfile_msgs.add("ERROR: " & loc & ": " & task & ": Start address "
                & dec(s) & " is out of bounds for memory " & range_txt);
            return;
        end if;
        if e < lo or e > hi then
            g_memfile_msgs.add("ERROR: " & loc & ": " & task & ": Finish address "
                & dec(e) & " is out of bounds for memory " & range_txt);
            return;
        end if;
        ok := true;
    end procedure;

    function is_hexd(c : character) return boolean is
    begin
        return (c >= '0' and c <= '9') or (c >= 'a' and c <= 'f')
            or (c >= 'A' and c <= 'F');
    end function;

    -- the characters of a word token (sys_readmem_lex.lex)
    function is_wordc(c : character; hex : boolean) return boolean is
    begin
        if c = '_' or c = 'x' or c = 'X' or c = 'z' or c = 'Z' then
            return true;
        elsif hex then
            return is_hexd(c);
        end if;
        return c = '0' or c = '1';
    end function;

    -- @<hex>: sscanf("%x") into an unsigned int, read as an int (the low 32
    -- bits; a value beyond 64 bits saturates, as strtoul does)
    function addr_value(s : string) return integer is
        variable acc : unsigned(31 downto 0) := (others => '0');
        variable sig : natural := 0;     -- significant digits
        variable d   : natural;
    begin
        for i in s'range loop
            if s(i) >= '0' and s(i) <= '9' then
                d := character'pos(s(i)) - character'pos('0');
            elsif s(i) >= 'a' and s(i) <= 'f' then
                d := character'pos(s(i)) - character'pos('a') + 10;
            else
                d := character'pos(s(i)) - character'pos('A') + 10;
            end if;
            if sig > 0 or d /= 0 then
                sig := sig + 1;
            end if;
            acc := shift_left(acc, 4) + d;
        end loop;
        if sig > 16 then
            return -1;                   -- ULONG_MAX, its low 32 bits
        end if;
        return to_integer(signed(acc));
    end function;

    procedure sv_readmem_load(fname : string; hex : boolean; task : string;
                              loc : string; mem : string;
                              left_a : integer; right_a : integer;
                              wwid : positive; kind : string;
                              has_start : boolean; start_a : integer;
                              start_real : boolean;
                              has_stop : boolean; stop_a : integer;
                              stop_real : boolean) is
        file f : std.textio.text;
        variable st       : file_open_status;
        variable l        : std.textio.line;
        variable ok       : boolean;
        variable start_v, stop_v, incr, min_a, max_a : integer;
        variable addr     : integer;
        variable past     : boolean := false;   -- addr ran off the int range
        variable count    : integer;            -- words the range still wants
        variable i, j     : integer;
        variable c        : character;
        variable in_blk   : boolean := false;   -- inside /* ... */
        variable done     : boolean := false;   -- stopped by an error
        variable excess_said : boolean := false;
        variable ndig, need, extra : natural;
        variable tok      : std.textio.line;
        variable tn       : natural;
        variable a        : integer;
        -- "<loc>: <task>(<file>): ", the head of the messages about the file
        constant fhead    : string := loc & ": " & task & "(" & fname & "): ";
    begin
        g_readmem.clear(hex);
        mem_params(fname, kind, task, loc, mem, left_a, right_a, has_start,
                   start_a, start_real, has_stop, stop_a, stop_real,
                   start_v, stop_v, incr, min_a, max_a, ok);
        if not ok then
            return;
        end if;
        file_open(st, f, sv_file_path(fname), read_mode);
        if st /= open_ok then
            g_memfile_msgs.add("ERROR: " & loc & ": " & task & ": Unable to open "
                               & fname & " for reading.");
            return;
        end if;
        count := max_a - min_a + 1;
        addr := start_v;
        if hex then
            need := (wwid + 3) / 4;
        else
            need := wwid;
        end if;
        while not done and not endfile(f) loop
            std.textio.readline(f, l);
            if l = null then
                next;
            end if;
            i := l'low;
            while not done and i <= l'high loop
                c := l(i);
                if in_blk then
                    if c = '*' and i < l'high and l(i + 1) = '/' then
                        in_blk := false;
                        i := i + 2;
                    else
                        i := i + 1;
                    end if;
                elsif c = ' ' or c = HT or c = FF or c = CR then
                    i := i + 1;
                elsif c = '/' and i < l'high and l(i + 1) = '/' then
                    exit;                                   -- to the line's end
                elsif c = '/' and i < l'high and l(i + 1) = '*' then
                    in_blk := true;
                    i := i + 2;
                elsif c = '@' and i < l'high and is_hexd(l(i + 1)) then
                    j := i + 1;
                    while j <= l'high and is_hexd(l(j)) loop
                        j := j + 1;
                    end loop;
                    a := addr_value(l(i + 1 to j - 1));
                    i := j;
                    if a < min_a or a > max_a then
                        g_memfile_msgs.add("ERROR: " & fhead & "address (0x"
                            & hex_c(a) & ") is out of range [0x" & hex_c(start_v)
                            & ":0x" & hex_c(stop_v) & "]");
                        done := true;
                    else
                        addr := a;
                        past := false;
                        -- an address turns the "Not enough words" check off
                        count := 0;
                    end if;
                elsif is_wordc(c, hex) then
                    j := i;
                    ndig := 0;
                    while j <= l'high and is_wordc(l(j), hex) loop
                        if l(j) /= '_' then
                            ndig := ndig + 1;
                        end if;
                        j := j + 1;
                    end loop;
                    -- the digits, '_' taken out
                    tok := new string(1 to ndig);
                    tn := 0;
                    for k in i to j - 1 loop
                        if l(k) /= '_' then
                            tn := tn + 1;
                            tok(tn) := l(k);
                        end if;
                    end loop;
                    if ndig > need and not excess_said then
                        extra := ndig - need;
                        excess_said := true;
                        if hex then
                            g_memfile_msgs.add("WARNING: " & loc & ": Excess hex "
                                & "digits (" & dec(extra) & " of '" & l(i to j - 1)
                                & "') while reading " & dec(wwid) & "-bit words.");
                        else
                            g_memfile_msgs.add("WARNING: " & loc & ": Excess binary "
                                & "digits (" & dec(extra) & " of '" & l(i to j - 1)
                                & "') while reading " & dec(wwid) & "-bit words.");
                        end if;
                    end if;
                    i := j;
                    if not past and addr >= min_a and addr <= max_a then
                        g_readmem.add(addr, tok.all);
                        if count > 0 then
                            count := count - 1;
                        end if;
                        if (incr > 0 and addr = integer'high)
                            or (incr < 0 and addr = integer'low) then
                            past := true;
                        else
                            addr := addr + incr;
                        end if;
                    else
                        g_memfile_msgs.add("WARNING: " & fhead & "Too many words in "
                            & "the file for the requested range [" & dec(start_v)
                            & ":" & dec(stop_v) & "].");
                        done := true;
                    end if;
                    deallocate(tok);
                else
                    g_memfile_msgs.add("ERROR: " & fhead & "Invalid input character: "
                                       & c);
                    done := true;
                end if;
            end loop;
            deallocate(l);
        end loop;
        if not done and count > 0 then
            g_memfile_msgs.add("WARNING: " & fhead & "Not enough words in the file "
                & "for the requested range [" & dec(start_v) & ":" & dec(stop_v)
                & "].");
        end if;
        file_close(f);
    end procedure;

    impure function sv_readmem_count return natural is
    begin
        return g_readmem.count;
    end function;

    impure function sv_readmem_addr(i : natural) return integer is
    begin
        return g_readmem.addr(i);
    end function;

    impure function sv_readmem_word(i : natural; w : positive) return logic3d_vector is
    begin
        return g_readmem.word(i, w);
    end function;

    impure function sv_readmem_bit(i : natural) return logic3d is
        constant r : logic3d_vector(0 downto 0) := g_readmem.word(i, 1);
    begin
        return r(0);
    end function;

    -- $writemem: the file and the addresses it writes
    type t_writemem is protected
        impure function open_file(path : string) return boolean;
        procedure set_range(first : integer; step : integer; n : natural);
        impure function count return natural;
        impure function addr(i : natural) return integer;
        procedure put(s : string);
        procedure close_file;
    end protected;

    type t_writemem is protected body
        file wf : std.textio.text;
        variable active : boolean := false;
        variable first_a : integer := 0;
        variable step_a  : integer := 1;
        variable n_words : natural := 0;
        variable written : natural := 0;

        procedure close_file is
        begin
            if active then
                file_close(wf);
                active := false;
            end if;
            n_words := 0;
        end procedure;

        impure function open_file(path : string) return boolean is
            variable st : file_open_status;
        begin
            close_file;
            file_open(st, wf, path, write_mode);
            active := st = open_ok;
            written := 0;
            return active;
        end function;

        procedure set_range(first : integer; step : integer; n : natural) is
        begin
            first_a := first;
            step_a := step;
            n_words := n;
        end procedure;

        impure function count return natural is
        begin
            if not active then
                return 0;
            end if;
            return n_words;
        end function;

        impure function addr(i : natural) return integer is
        begin
            return first_a + step_a * i;
        end function;

        procedure put(s : string) is
        begin
            if active then
                if written mod 16 = 0 then
                    write(wf, "// 0x" & hex_c(written, true) & LF);
                end if;
                write(wf, s & LF);
                written := written + 1;
            end if;
        end procedure;
    end protected body;

    shared variable g_writemem : t_writemem;

    procedure sv_writemem_open(fname : string; hex : boolean; task : string;
                               loc : string; mem : string;
                               left_a : integer; right_a : integer;
                               kind : string;
                               has_start : boolean; start_a : integer;
                               start_real : boolean;
                               has_stop : boolean; stop_a : integer;
                               stop_real : boolean) is
        variable ok : boolean;
        variable start_v, stop_v, incr, min_a, max_a : integer;
    begin
        g_writemem.close_file;
        mem_params(fname, kind, task, loc, mem, left_a, right_a, has_start,
                   start_a, start_real, has_stop, stop_a, stop_real,
                   start_v, stop_v, incr, min_a, max_a, ok);
        if not ok then
            return;
        end if;
        if not g_writemem.open_file(sv_file_path(fname)) then
            g_memfile_msgs.add("ERROR: " & loc & ": " & task & ": Unable to open "
                               & fname & " for writing.");
            return;
        end if;
        g_writemem.set_range(start_v, incr, max_a - min_a + 1);
    end procedure;

    impure function sv_writemem_count return natural is
    begin
        return g_writemem.count;
    end function;

    impure function sv_writemem_addr(i : natural) return integer is
    begin
        return g_writemem.addr(i);
    end function;

    procedure sv_writemem_word(s : string) is
    begin
        g_writemem.put(s);
    end procedure;

    procedure sv_writemem_close is
    begin
        g_writemem.close_file;
    end procedure;

end package body;
