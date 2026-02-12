-- 3D Logic Type Package
-- Provides the enum representation for single-bit signals
-- Vector versions would use ieee.numeric_std.unsigned for bitwise ops

-- Separating the handling of value, certainty and strength makes it easier
-- to convert between types than with the collapsed enum form (01XZ), logic
-- values should be consistent regardless of X (certainty state), converting
-- 1/0 to a Voltage and back doesn't require looking at the X/Z fields.

library ieee;
use ieee.std_logic_1164.all;

package logic3d_types_pkg is

    ---------------------------------------------------------------------------
    -- Enum version for single-bit signals (3-bit encoding)
    ---------------------------------------------------------------------------
    subtype logic3d is natural range 0 to 7;

    -- Encoding: bit2=uncertain, bit1=strength, bit0=value
    -- 000 = weak 0      (L3D_L)  std_logic 'L'
    -- 001 = weak 1      (L3D_H)  std_logic 'H'
    -- 010 = strong 0    (L3D_0)  std_logic '0'
    -- 011 = strong 1    (L3D_1)  std_logic '1'
    -- 100 = high-Z      (L3D_Z)  std_logic 'Z'
    -- 101 = weak unknown (L3D_W)  std_logic 'W'
    -- 110 = unknown      (L3D_X)  std_logic 'X'
    -- 111 = uninitialized (L3D_U) std_logic 'U'

    constant L3D_L : logic3d := 0;  -- 000: weak 0
    constant L3D_H : logic3d := 1;  -- 001: weak 1
    constant L3D_0 : logic3d := 2;  -- 010: strong 0
    constant L3D_1 : logic3d := 3;  -- 011: strong 1
    constant L3D_Z : logic3d := 4;  -- 100: high-Z
    constant L3D_W : logic3d := 5;  -- 101: weak unknown
    constant L3D_X : logic3d := 6;  -- 110: unknown
    constant L3D_U : logic3d := 7;  -- 111: uninitialized

    ---------------------------------------------------------------------------
    -- Lookup tables (8x8 for 2-input, 8 for 1-input)
    -- Gate outputs are always strong.
    -- Weak inputs (L,H) treated as known 0,1 for gate logic.
    -- Uncertain inputs (Z,W,X,U) propagate X, except where
    -- a dominating value (0 for AND, 1 for OR) forces the result.
    ---------------------------------------------------------------------------
    type lut1_t is array (0 to 7) of logic3d;
    type lut2_t is array (0 to 7, 0 to 7) of logic3d;

    --                        L  H  0  1  Z  W  X  U
    constant NOT_LUT : lut1_t := (3, 2, 3, 2, 6, 6, 6, 6);

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
    ---------------------------------------------------------------------------
    function to_std_logic(a : logic3d) return std_logic;
    function to_logic3d(s : std_logic) return logic3d;

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

end package;

package body logic3d_types_pkg is

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

end package body;
