-- 3D Logic Gate Package with Lookup Table Implementation
-- All gates use 8x8 or 8-entry tables for O(1) evaluation

package logic3d_gates_pkg is
    -- 3-bit encoded type (0-7)
    subtype logic3d is natural range 0 to 7;

    -- Encoding: bit2=uncertain, bit1=strength, bit0=value
    -- 000 = invalid (treat as Z)
    -- 001 = invalid (treat as Z)
    -- 010 = strong 0 (L3D_0)
    -- 011 = strong 1 (L3D_1)
    -- 100 = high-Z (L3D_Z)
    -- 101 = invalid (treat as Z)
    -- 110 = unknown/conflict (L3D_X)
    -- 111 = invalid (treat as X)

    constant L3D_0 : logic3d := 2;  -- 010: strong 0
    constant L3D_1 : logic3d := 3;  -- 011: strong 1
    constant L3D_Z : logic3d := 4;  -- 100: high-Z
    constant L3D_X : logic3d := 6;  -- 110: unknown

    -- Gate lookup tables
    type lut1_t is array (0 to 7) of logic3d;
    type lut2_t is array (0 to 7, 0 to 7) of logic3d;

    -- NOT gate: inverts value, preserves strength/uncertain
    constant NOT_LUT : lut1_t := (
        4,  -- 000 -> Z
        4,  -- 001 -> Z
        3,  -- 010 (strong 0) -> 011 (strong 1)
        2,  -- 011 (strong 1) -> 010 (strong 0)
        4,  -- 100 (Z) -> Z
        4,  -- 101 -> Z
        6,  -- 110 (X) -> X
        6   -- 111 -> X
    );

    -- AND gate truth table
    -- 0 & anything = 0 (strong 0 dominates)
    -- 1 & 1 = 1
    -- 1 & X = X
    -- X & X = X
    -- Z & Z = Z
    constant AND_LUT : lut2_t := (
        --      0    1    2    3    4    5    6    7
        0 => (4, 4, 2, 2, 4, 4, 6, 6),  -- 000 (->Z)
        1 => (4, 4, 2, 2, 4, 4, 6, 6),  -- 001 (->Z)
        2 => (2, 2, 2, 2, 2, 2, 2, 2),  -- 010 (0): 0 & x = 0
        3 => (2, 2, 2, 3, 4, 4, 6, 6),  -- 011 (1): 1 & 0=0, 1&1=1, 1&Z=Z, 1&X=X
        4 => (4, 4, 2, 4, 4, 4, 6, 6),  -- 100 (Z): Z & 0=0, Z&1=Z, Z&Z=Z
        5 => (4, 4, 2, 4, 4, 4, 6, 6),  -- 101 (->Z)
        6 => (6, 6, 2, 6, 6, 6, 6, 6),  -- 110 (X): X & 0=0, X&x=X
        7 => (6, 6, 2, 6, 6, 6, 6, 6)   -- 111 (->X)
    );

    -- OR gate truth table
    -- 1 | anything = 1 (strong 1 dominates)
    -- 0 | 0 = 0
    -- 0 | X = X
    -- X | X = X
    -- Z | Z = Z
    constant OR_LUT : lut2_t := (
        --      0    1    2    3    4    5    6    7
        0 => (4, 4, 4, 3, 4, 4, 6, 6),  -- 000 (->Z)
        1 => (4, 4, 4, 3, 4, 4, 6, 6),  -- 001 (->Z)
        2 => (4, 4, 2, 3, 4, 4, 6, 6),  -- 010 (0): 0|0=0, 0|1=1, 0|Z=Z, 0|X=X
        3 => (3, 3, 3, 3, 3, 3, 3, 3),  -- 011 (1): 1 | x = 1
        4 => (4, 4, 4, 3, 4, 4, 6, 6),  -- 100 (Z): Z|0=Z, Z|1=1, Z|Z=Z
        5 => (4, 4, 4, 3, 4, 4, 6, 6),  -- 101 (->Z)
        6 => (6, 6, 6, 3, 6, 6, 6, 6),  -- 110 (X): X|1=1, X|x=X
        7 => (6, 6, 6, 3, 6, 6, 6, 6)   -- 111 (->X)
    );

    -- XOR gate truth table
    constant XOR_LUT : lut2_t := (
        --      0    1    2    3    4    5    6    7
        0 => (4, 4, 4, 4, 4, 4, 6, 6),  -- 000 (->Z)
        1 => (4, 4, 4, 4, 4, 4, 6, 6),  -- 001 (->Z)
        2 => (4, 4, 2, 3, 4, 4, 6, 6),  -- 010 (0): 0^0=0, 0^1=1, 0^Z=Z, 0^X=X
        3 => (4, 4, 3, 2, 4, 4, 6, 6),  -- 011 (1): 1^0=1, 1^1=0, 1^Z=Z, 1^X=X
        4 => (4, 4, 4, 4, 4, 4, 6, 6),  -- 100 (Z)
        5 => (4, 4, 4, 4, 4, 4, 6, 6),  -- 101 (->Z)
        6 => (6, 6, 6, 6, 6, 6, 6, 6),  -- 110 (X): X^x=X
        7 => (6, 6, 6, 6, 6, 6, 6, 6)   -- 111 (->X)
    );

    -- Resolution for multiple drivers
    constant RESOLVE_LUT : lut2_t := (
        --      0    1    2    3    4    5    6    7
        0 => (4, 4, 2, 3, 4, 4, 6, 6),  -- 000 (->Z)
        1 => (4, 4, 2, 3, 4, 4, 6, 6),  -- 001 (->Z)
        2 => (2, 2, 2, 6, 2, 2, 6, 6),  -- 010 (0): 0+0=0, 0+1=X, 0+Z=0
        3 => (3, 3, 6, 3, 3, 3, 6, 6),  -- 011 (1): 1+0=X, 1+1=1, 1+Z=1
        4 => (4, 4, 2, 3, 4, 4, 6, 6),  -- 100 (Z): Z+x=x
        5 => (4, 4, 2, 3, 4, 4, 6, 6),  -- 101 (->Z)
        6 => (6, 6, 6, 6, 6, 6, 6, 6),  -- 110 (X): X+x=X
        7 => (6, 6, 6, 6, 6, 6, 6, 6)   -- 111 (->X)
    );

    -- Gate functions (single table lookup each)
    function "not" (a : logic3d) return logic3d;
    function "and" (a, b : logic3d) return logic3d;
    function "or"  (a, b : logic3d) return logic3d;
    function "xor" (a, b : logic3d) return logic3d;

    -- Multi-input gates: chained lookups in single expression (no delta cycles)
    function and3(a, b, c : logic3d) return logic3d;
    function and4(a, b, c, d : logic3d) return logic3d;
    function or3(a, b, c : logic3d) return logic3d;
    function or4(a, b, c, d : logic3d) return logic3d;
    function xor3(a, b, c : logic3d) return logic3d;
    function xor4(a, b, c, d : logic3d) return logic3d;

    -- NAND/NOR: chained lookup + NOT in single expression
    function nand2(a, b : logic3d) return logic3d;
    function nand3(a, b, c : logic3d) return logic3d;
    function nor2(a, b : logic3d) return logic3d;
    function nor3(a, b, c : logic3d) return logic3d;

    -- MUX: sel=0 -> a, sel=1 -> b, sel=X/Z -> X
    function mux2(sel, a, b : logic3d) return logic3d;

    -- Utility
    function to_bit(a : logic3d) return bit;
    function is_strong(a : logic3d) return boolean;
    function is_one(a : logic3d) return boolean;
    function is_zero(a : logic3d) return boolean;

end package;

package body logic3d_gates_pkg is

    function "not" (a : logic3d) return logic3d is
    begin
        return NOT_LUT(a);
    end function;

    function "and" (a, b : logic3d) return logic3d is
    begin
        return AND_LUT(a, b);
    end function;

    function "or" (a, b : logic3d) return logic3d is
    begin
        return OR_LUT(a, b);
    end function;

    function "xor" (a, b : logic3d) return logic3d is
    begin
        return XOR_LUT(a, b);
    end function;

    function to_bit(a : logic3d) return bit is
    begin
        if (a mod 2) = 1 then
            return '1';
        else
            return '0';
        end if;
    end function;

    function is_strong(a : logic3d) return boolean is
    begin
        return ((a / 2) mod 2) = 1 and ((a / 4) mod 2) = 0;
    end function;

    function is_one(a : logic3d) return boolean is
    begin
        return a = L3D_1;
    end function;

    function is_zero(a : logic3d) return boolean is
    begin
        return a = L3D_0;
    end function;

    -- Multi-input gates: all chained in single expression (no delta cycles)
    function and3(a, b, c : logic3d) return logic3d is
    begin
        return AND_LUT(AND_LUT(a, b), c);
    end function;

    function and4(a, b, c, d : logic3d) return logic3d is
    begin
        return AND_LUT(AND_LUT(AND_LUT(a, b), c), d);
    end function;

    function or3(a, b, c : logic3d) return logic3d is
    begin
        return OR_LUT(OR_LUT(a, b), c);
    end function;

    function or4(a, b, c, d : logic3d) return logic3d is
    begin
        return OR_LUT(OR_LUT(OR_LUT(a, b), c), d);
    end function;

    function xor3(a, b, c : logic3d) return logic3d is
    begin
        return XOR_LUT(XOR_LUT(a, b), c);
    end function;

    function xor4(a, b, c, d : logic3d) return logic3d is
    begin
        return XOR_LUT(XOR_LUT(XOR_LUT(a, b), c), d);
    end function;

    function nand2(a, b : logic3d) return logic3d is
    begin
        return NOT_LUT(AND_LUT(a, b));
    end function;

    function nand3(a, b, c : logic3d) return logic3d is
    begin
        return NOT_LUT(AND_LUT(AND_LUT(a, b), c));
    end function;

    function nor2(a, b : logic3d) return logic3d is
    begin
        return NOT_LUT(OR_LUT(a, b));
    end function;

    function nor3(a, b, c : logic3d) return logic3d is
    begin
        return NOT_LUT(OR_LUT(OR_LUT(a, b), c));
    end function;

    -- MUX: sel=0 -> a, sel=1 -> b, sel=X/Z -> X
    -- Implemented as: (sel AND b) OR (NOT sel AND a)
    function mux2(sel, a, b : logic3d) return logic3d is
    begin
        return OR_LUT(AND_LUT(sel, b), AND_LUT(NOT_LUT(sel), a));
    end function;

end package body;
