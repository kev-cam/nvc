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
    constant NOT_LUT : lut1_t := (1, 0, 3, 2, 6, 6, 6, 6);

    constant AND_LUT : lut2_t := (
    --                  L  H  0  1  Z  W  X  U
        0 => (2, 2, 2, 2, 2, 2, 2, 2),  -- L: 0 AND x = 0
        1 => (2, 3, 2, 3, 6, 6, 6, 6),  -- H: 1 AND x
        2 => (2, 2, 2, 2, 2, 2, 2, 2),  -- 0: 0 AND x = 0
        3 => (2, 3, 2, 3, 6, 6, 6, 6),  -- 1: 1 AND x
        4 => (2, 6, 2, 6, 6, 6, 6, 6),  -- Z: X AND x (0 dominates)
        5 => (2, 6, 2, 6, 6, 6, 6, 6),  -- W: X AND x (0 dominates)
        6 => (2, 6, 2, 6, 6, 6, 6, 6),  -- X: X AND x (0 dominates)
        7 => (2, 6, 2, 6, 6, 6, 6, 6)   -- U: X AND x (0 dominates)
    );

    constant OR_LUT : lut2_t := (
    --                  L  H  0  1  Z  W  X  U
        0 => (2, 3, 2, 3, 6, 6, 6, 6),  -- L: 0 OR x
        1 => (3, 3, 3, 3, 3, 3, 3, 3),  -- H: 1 OR x = 1
        2 => (2, 3, 2, 3, 6, 6, 6, 6),  -- 0: 0 OR x
        3 => (3, 3, 3, 3, 3, 3, 3, 3),  -- 1: 1 OR x = 1
        4 => (6, 3, 6, 3, 6, 6, 6, 6),  -- Z: X OR x (1 dominates)
        5 => (6, 3, 6, 3, 6, 6, 6, 6),  -- W: X OR x (1 dominates)
        6 => (6, 3, 6, 3, 6, 6, 6, 6),  -- X: X OR x (1 dominates)
        7 => (6, 3, 6, 3, 6, 6, 6, 6)   -- U: X OR x (1 dominates)
    );

    constant XOR_LUT : lut2_t := (
    --                  L  H  0  1  Z  W  X  U
        0 => (2, 3, 2, 3, 6, 6, 6, 6),  -- L: 0 XOR x
        1 => (3, 2, 3, 2, 6, 6, 6, 6),  -- H: 1 XOR x
        2 => (2, 3, 2, 3, 6, 6, 6, 6),  -- 0: 0 XOR x
        3 => (3, 2, 3, 2, 6, 6, 6, 6),  -- 1: 1 XOR x
        4 => (6, 6, 6, 6, 6, 6, 6, 6),  -- Z: X XOR x = X
        5 => (6, 6, 6, 6, 6, 6, 6, 6),  -- W: X XOR x = X
        6 => (6, 6, 6, 6, 6, 6, 6, 6),  -- X: X XOR x = X
        7 => (6, 6, 6, 6, 6, 6, 6, 6)   -- U: X XOR x = X
    );

    constant NAND_LUT : lut2_t := (
    --                  L  H  0  1  Z  W  X  U
        0 => (3, 3, 3, 3, 3, 3, 3, 3),  -- L: NOT(0 AND x) = 1
        1 => (3, 2, 3, 2, 6, 6, 6, 6),  -- H: NOT(1 AND x)
        2 => (3, 3, 3, 3, 3, 3, 3, 3),  -- 0: NOT(0 AND x) = 1
        3 => (3, 2, 3, 2, 6, 6, 6, 6),  -- 1: NOT(1 AND x)
        4 => (3, 6, 3, 6, 6, 6, 6, 6),  -- Z: NOT(X AND x)
        5 => (3, 6, 3, 6, 6, 6, 6, 6),  -- W
        6 => (3, 6, 3, 6, 6, 6, 6, 6),  -- X
        7 => (3, 6, 3, 6, 6, 6, 6, 6)   -- U
    );

    constant NOR_LUT : lut2_t := (
    --                  L  H  0  1  Z  W  X  U
        0 => (3, 2, 3, 2, 6, 6, 6, 6),  -- L: NOT(0 OR x)
        1 => (2, 2, 2, 2, 2, 2, 2, 2),  -- H: NOT(1 OR x) = 0
        2 => (3, 2, 3, 2, 6, 6, 6, 6),  -- 0: NOT(0 OR x)
        3 => (2, 2, 2, 2, 2, 2, 2, 2),  -- 1: NOT(1 OR x) = 0
        4 => (6, 2, 6, 2, 6, 6, 6, 6),  -- Z: NOT(X OR x)
        5 => (6, 2, 6, 2, 6, 6, 6, 6),  -- W
        6 => (6, 2, 6, 2, 6, 6, 6, 6),  -- X
        7 => (6, 2, 6, 2, 6, 6, 6, 6)   -- U
    );

    constant XNOR_LUT : lut2_t := (
    --                  L  H  0  1  Z  W  X  U
        0 => (3, 2, 3, 2, 6, 6, 6, 6),  -- L: NOT(0 XOR x)
        1 => (2, 3, 2, 3, 6, 6, 6, 6),  -- H: NOT(1 XOR x)
        2 => (3, 2, 3, 2, 6, 6, 6, 6),  -- 0: NOT(0 XOR x)
        3 => (2, 3, 2, 3, 6, 6, 6, 6),  -- 1: NOT(1 XOR x)
        4 => (6, 6, 6, 6, 6, 6, 6, 6),  -- Z: X
        5 => (6, 6, 6, 6, 6, 6, 6, 6),  -- W: X
        6 => (6, 6, 6, 6, 6, 6, 6, 6),  -- X: X
        7 => (6, 6, 6, 6, 6, 6, 6, 6)   -- U: X
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
    function "sll"(a : logic3d_vector; n : natural) return logic3d_vector;
    function "srl"(a : logic3d_vector; n : natural) return logic3d_vector;
    function shift_left (a : logic3d_vector; n : natural) return logic3d_vector;
    function shift_right(a : logic3d_vector; n : natural) return logic3d_vector;

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

end package body;
