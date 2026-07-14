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
    function l3d_weaken(a : logic3d) return logic3d;
    function l3d_set_uncertain(a : logic3d) return logic3d;

    -- Multi-input (chained lookups, no delta cycles)
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
    function l3d_mod_s(a, b : logic3d_vector) return logic3d_vector;
    -- Sign-extending resize (Verilog signed widening): fill high bits w/ sign.
    function l3d_resize_s(a : logic3d_vector; new_size : natural) return logic3d_vector;

    -- Multiplication, division, modulus
    function "*"  (a, b : logic3d_vector) return logic3d_vector;
    function "/"  (a, b : logic3d_vector) return logic3d_vector;
    function "mod"(a, b : logic3d_vector) return logic3d_vector;
    function "rem"(a, b : logic3d_vector) return logic3d_vector;

    -- Verilog $random(seed): one deterministic step of a seeded RNG. A pure
    -- function (no inout) returning the next value; tgt-vhdl assigns it to the
    -- target AND re-applies it to the seed, so it composes with any lvalue and
    -- with a signal- or variable-class seed.
    function sv_random(seed : logic3d_vector) return logic3d_vector;

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
        variable aa : logic3d_vector(a'length-1 downto 0) := a;
        variable bb : logic3d_vector(b'length-1 downto 0) := b;
        constant n : natural := minimum(a'length, b'length);
        variable result : logic3d_vector(a'range);
    begin
        result := (others => L3D_0);
        for i in 0 to n-1 loop
            result(result'low + i) := AND_LUT(aa(i), bb(i));
        end loop;
        return result;
    end function;

    -- (vec & scalar) reduced to the scalar lvalue keeps bit 0:
    --   (vec & scalar)[0] = vec[0] and scalar.
    -- Rebase to (length-1 downto 0) so index 0 is the LSB regardless of the
    -- literal's declared range, matching the vector-vector bodies above.
    function l3d_and(a : logic3d_vector; b : logic3d) return logic3d is
        variable aa : logic3d_vector(a'length-1 downto 0) := a;
    begin
        return AND_LUT(aa(0), b);
    end function;

    function l3d_and(a : logic3d; b : std_logic) return logic3d is
    begin
        return AND_LUT(a, to_logic3d(b));
    end function;

    -- Verilog '==' on two vectors -> 1-bit result. AND-reduce of bitwise XNOR
    -- (X/Z propagate through the LUTs, matching x-pessimistic equality). Length
    -- mismatch compares the common low bits (operands are same-width in practice).
    function "="(a, b : logic3d_vector) return logic3d is
        variable aa : logic3d_vector(a'length-1 downto 0) := a;
        variable bb : logic3d_vector(b'length-1 downto 0) := b;
        constant n  : natural := minimum(a'length, b'length);
        variable r  : logic3d := L3D_1;
    begin
        for i in 0 to n-1 loop
            r := AND_LUT(r, XNOR_LUT(aa(i), bb(i)));
        end loop;
        return r;
    end function;

    function l3d_or(a, b : logic3d_vector) return logic3d_vector is
        variable aa : logic3d_vector(a'length-1 downto 0) := a;
        variable bb : logic3d_vector(b'length-1 downto 0) := b;
        constant n : natural := minimum(a'length, b'length);
        variable result : logic3d_vector(a'range);
    begin
        result := (others => L3D_0);
        for i in 0 to n-1 loop
            result(result'low + i) := OR_LUT(aa(i), bb(i));
        end loop;
        return result;
    end function;

    function l3d_xor(a, b : logic3d_vector) return logic3d_vector is
        variable aa : logic3d_vector(a'length-1 downto 0) := a;
        variable bb : logic3d_vector(b'length-1 downto 0) := b;
        constant n : natural := minimum(a'length, b'length);
        variable result : logic3d_vector(a'range);
    begin
        result := (others => L3D_0);
        for i in 0 to n-1 loop
            result(result'low + i) := XOR_LUT(aa(i), bb(i));
        end loop;
        return result;
    end function;

    function l3d_nand(a, b : logic3d_vector) return logic3d_vector is
        variable aa : logic3d_vector(a'length-1 downto 0) := a;
        variable bb : logic3d_vector(b'length-1 downto 0) := b;
        constant n : natural := minimum(a'length, b'length);
        variable result : logic3d_vector(a'range);
    begin
        result := (others => L3D_0);
        for i in 0 to n-1 loop
            result(result'low + i) := NAND_LUT(aa(i), bb(i));
        end loop;
        return result;
    end function;

    function l3d_nor(a, b : logic3d_vector) return logic3d_vector is
        variable aa : logic3d_vector(a'length-1 downto 0) := a;
        variable bb : logic3d_vector(b'length-1 downto 0) := b;
        constant n : natural := minimum(a'length, b'length);
        variable result : logic3d_vector(a'range);
    begin
        result := (others => L3D_0);
        for i in 0 to n-1 loop
            result(result'low + i) := NOR_LUT(aa(i), bb(i));
        end loop;
        return result;
    end function;

    function l3d_xnor(a, b : logic3d_vector) return logic3d_vector is
        variable aa : logic3d_vector(a'length-1 downto 0) := a;
        variable bb : logic3d_vector(b'length-1 downto 0) := b;
        constant n : natural := minimum(a'length, b'length);
        variable result : logic3d_vector(a'range);
    begin
        result := (others => L3D_0);
        for i in 0 to n-1 loop
            result(result'low + i) := XNOR_LUT(aa(i), bb(i));
        end loop;
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
                -- An uncertain (X/Z) bit in the low 31 makes the index value
                -- undefined; return a safe in-range 0 instead of trapping on an
                -- out-of-range slice (Verilog yields X for an X index). Almost
                -- always a time-0 / pre-reset artifact before index regs settle.
                if is_uncertain(a(i)) then return 0; end if;
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
    function l3d_mod_s(a, b : logic3d_vector) return logic3d_vector is
        variable allx : logic3d_vector(a'range) := (others => L3D_X);
    begin
        if l3d_to_unsigned(b) = 0 then return allx; end if;
        return unsigned_to_l3d(unsigned(std_logic_vector(ieee.numeric_std.resize(
            signed(std_logic_vector(l3d_to_unsigned(a)))
            mod signed(std_logic_vector(l3d_to_unsigned(b))), a'length))));
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

    function sv_random(seed : logic3d_vector) return logic3d_vector is
        variable s : unsigned(31 downto 0);
    begin
        -- Deterministic 32-bit LCG (glibc constants). Verilog $random's exact
        -- sequence is not required: the self-checking tests verify reproducibility
        -- for a given seed and a non-zero seed advance. seed=0 -> 12345.
        s := resize(l3d_to_unsigned(seed), 32);
        s := resize(s * to_unsigned(1103515245, 32), 32) + to_unsigned(12345, 32);
        return unsigned_to_l3d(resize(s, seed'length));
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

end package body;
