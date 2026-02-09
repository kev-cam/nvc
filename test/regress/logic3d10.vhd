-- Test for multi-input 3D logic gates using lookup tables
-- All chained lookups complete in a single delta (no unnecessary delta cycles)

package logic3d10_pkg is
    subtype logic3d is natural range 0 to 7;

    constant L3D_0 : logic3d := 2;  -- 010: strong 0
    constant L3D_1 : logic3d := 3;  -- 011: strong 1
    constant L3D_Z : logic3d := 4;  -- 100: high-Z
    constant L3D_X : logic3d := 6;  -- 110: unknown

    type lut1_t is array (0 to 7) of logic3d;
    type lut2_t is array (0 to 7, 0 to 7) of logic3d;

    constant NOT_LUT : lut1_t := (4, 4, 3, 2, 4, 4, 6, 6);

    constant AND_LUT : lut2_t := (
        0 => (4, 4, 2, 2, 4, 4, 6, 6),
        1 => (4, 4, 2, 2, 4, 4, 6, 6),
        2 => (2, 2, 2, 2, 2, 2, 2, 2),
        3 => (2, 2, 2, 3, 4, 4, 6, 6),
        4 => (4, 4, 2, 4, 4, 4, 6, 6),
        5 => (4, 4, 2, 4, 4, 4, 6, 6),
        6 => (6, 6, 2, 6, 6, 6, 6, 6),
        7 => (6, 6, 2, 6, 6, 6, 6, 6)
    );

    constant OR_LUT : lut2_t := (
        0 => (4, 4, 4, 3, 4, 4, 6, 6),
        1 => (4, 4, 4, 3, 4, 4, 6, 6),
        2 => (4, 4, 2, 3, 4, 4, 6, 6),
        3 => (3, 3, 3, 3, 3, 3, 3, 3),
        4 => (4, 4, 4, 3, 4, 4, 6, 6),
        5 => (4, 4, 4, 3, 4, 4, 6, 6),
        6 => (6, 6, 6, 3, 6, 6, 6, 6),
        7 => (6, 6, 6, 3, 6, 6, 6, 6)
    );

    constant XOR_LUT : lut2_t := (
        0 => (4, 4, 4, 4, 4, 4, 6, 6),
        1 => (4, 4, 4, 4, 4, 4, 6, 6),
        2 => (4, 4, 2, 3, 4, 4, 6, 6),
        3 => (4, 4, 3, 2, 4, 4, 6, 6),
        4 => (4, 4, 4, 4, 4, 4, 6, 6),
        5 => (4, 4, 4, 4, 4, 4, 6, 6),
        6 => (6, 6, 6, 6, 6, 6, 6, 6),
        7 => (6, 6, 6, 6, 6, 6, 6, 6)
    );

    -- Multi-input gates: chained lookups in single expression
    function and3(a, b, c : logic3d) return logic3d;
    function and4(a, b, c, d : logic3d) return logic3d;
    function or3(a, b, c : logic3d) return logic3d;
    function xor3(a, b, c : logic3d) return logic3d;
    function nand2(a, b : logic3d) return logic3d;
    function nor2(a, b : logic3d) return logic3d;
    function mux2(sel, a, b : logic3d) return logic3d;
end package;

package body logic3d10_pkg is
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

    function xor3(a, b, c : logic3d) return logic3d is
    begin
        return XOR_LUT(XOR_LUT(a, b), c);
    end function;

    function nand2(a, b : logic3d) return logic3d is
    begin
        return NOT_LUT(AND_LUT(a, b));
    end function;

    function nor2(a, b : logic3d) return logic3d is
    begin
        return NOT_LUT(OR_LUT(a, b));
    end function;

    function mux2(sel, a, b : logic3d) return logic3d is
    begin
        return OR_LUT(AND_LUT(sel, b), AND_LUT(NOT_LUT(sel), a));
    end function;
end package body;

library work;
use work.logic3d10_pkg.all;

entity logic3d10 is
end entity;

architecture test of logic3d10 is
    signal a, b, c, d : logic3d := L3D_0;
    signal r_and3, r_and4, r_or3, r_xor3 : logic3d;
    signal r_nand2, r_nor2, r_mux : logic3d;
begin
    -- Concurrent assignments: all use chained lookups
    r_and3  <= and3(a, b, c);
    r_and4  <= and4(a, b, c, d);
    r_or3   <= or3(a, b, c);
    r_xor3  <= xor3(a, b, c);
    r_nand2 <= nand2(a, b);
    r_nor2  <= nor2(a, b);
    r_mux   <= mux2(a, b, c);

    process
    begin
        -- Test 1: All zeros
        a <= L3D_0; b <= L3D_0; c <= L3D_0; d <= L3D_0;
        wait for 0 ns; wait for 0 ns;
        assert r_and3 = L3D_0 report "and3(0,0,0) fail" severity failure;
        assert r_and4 = L3D_0 report "and4(0,0,0,0) fail" severity failure;
        assert r_or3 = L3D_0 report "or3(0,0,0) fail" severity failure;
        assert r_xor3 = L3D_0 report "xor3(0,0,0) fail" severity failure;
        assert r_nand2 = L3D_1 report "nand2(0,0) fail" severity failure;
        assert r_nor2 = L3D_1 report "nor2(0,0) fail" severity failure;

        -- Test 2: All ones
        a <= L3D_1; b <= L3D_1; c <= L3D_1; d <= L3D_1;
        wait for 0 ns; wait for 0 ns;
        assert r_and3 = L3D_1 report "and3(1,1,1) fail" severity failure;
        assert r_and4 = L3D_1 report "and4(1,1,1,1) fail" severity failure;
        assert r_or3 = L3D_1 report "or3(1,1,1) fail" severity failure;
        assert r_xor3 = L3D_1 report "xor3(1,1,1) fail" severity failure;
        assert r_nand2 = L3D_0 report "nand2(1,1) fail" severity failure;
        assert r_nor2 = L3D_0 report "nor2(1,1) fail" severity failure;

        -- Test 3: XOR parity
        a <= L3D_1; b <= L3D_0; c <= L3D_1;
        wait for 0 ns; wait for 0 ns;
        assert r_xor3 = L3D_0 report "xor3(1,0,1)=0 fail" severity failure;

        -- Test 4: MUX
        a <= L3D_0; b <= L3D_1; c <= L3D_0;  -- sel=0 -> b
        wait for 0 ns; wait for 0 ns;
        assert r_mux = L3D_1 report "mux2(0,1,0)=1 fail" severity failure;

        a <= L3D_1; b <= L3D_1; c <= L3D_0;  -- sel=1 -> c
        wait for 0 ns; wait for 0 ns;
        assert r_mux = L3D_0 report "mux2(1,1,0)=0 fail" severity failure;

        -- Test 5: X propagation
        a <= L3D_X; b <= L3D_1; c <= L3D_1;
        wait for 0 ns; wait for 0 ns;
        assert r_and3 = L3D_X report "and3(X,1,1)=X fail" severity failure;
        assert r_or3 = L3D_1 report "or3(X,1,1)=1 fail" severity failure;
        assert r_mux = L3D_X report "mux2(X,_,_)=X fail" severity failure;

        -- Test 6: Single 0 dominates AND
        a <= L3D_1; b <= L3D_0; c <= L3D_1; d <= L3D_1;
        wait for 0 ns; wait for 0 ns;
        assert r_and3 = L3D_0 report "and3(1,0,1)=0 fail" severity failure;
        assert r_and4 = L3D_0 report "and4(1,0,1,1)=0 fail" severity failure;

        report "PASSED: Multi-input 3D gates work correctly";
        wait;
    end process;
end architecture;
