-- Test for multi-input 3D logic gates with no delta cycles
-- All chained lookups should complete in a single delta

library work;
use work.logic3d_gates_pkg.all;

entity logic3d_multigate is
end entity;

architecture test of logic3d_multigate is
    signal a, b, c, d : logic3d := L3D_0;
    signal r_and3, r_and4 : logic3d;
    signal r_or3, r_or4 : logic3d;
    signal r_xor3, r_xor4 : logic3d;
    signal r_nand2, r_nand3 : logic3d;
    signal r_nor2, r_nor3 : logic3d;
    signal r_mux : logic3d;
begin

    -- All outputs computed in single delta (chained lookups, no intermediate signals)
    r_and3  <= and3(a, b, c);
    r_and4  <= and4(a, b, c, d);
    r_or3   <= or3(a, b, c);
    r_or4   <= or4(a, b, c, d);
    r_xor3  <= xor3(a, b, c);
    r_xor4  <= xor4(a, b, c, d);
    r_nand2 <= nand2(a, b);
    r_nand3 <= nand3(a, b, c);
    r_nor2  <= nor2(a, b);
    r_nor3  <= nor3(a, b, c);
    r_mux   <= mux2(a, b, c);  -- sel=a, inputs b,c

    process
        variable delta_count : natural := 0;
    begin
        -- Test 1: All zeros
        a <= L3D_0; b <= L3D_0; c <= L3D_0; d <= L3D_0;
        wait for 0 ns;  -- One delta
        delta_count := delta_count + 1;
        wait for 0 ns;  -- Second delta to let signals propagate
        delta_count := delta_count + 1;

        report "Test 1: All zeros";
        assert r_and3 = L3D_0 report "and3(0,0,0) should be 0" severity failure;
        assert r_and4 = L3D_0 report "and4(0,0,0,0) should be 0" severity failure;
        assert r_or3 = L3D_0 report "or3(0,0,0) should be 0" severity failure;
        assert r_or4 = L3D_0 report "or4(0,0,0,0) should be 0" severity failure;
        assert r_xor3 = L3D_0 report "xor3(0,0,0) should be 0" severity failure;
        assert r_xor4 = L3D_0 report "xor4(0,0,0,0) should be 0" severity failure;
        assert r_nand2 = L3D_1 report "nand2(0,0) should be 1" severity failure;
        assert r_nand3 = L3D_1 report "nand3(0,0,0) should be 1" severity failure;
        assert r_nor2 = L3D_1 report "nor2(0,0) should be 1" severity failure;
        assert r_nor3 = L3D_1 report "nor3(0,0,0) should be 1" severity failure;
        assert r_mux = L3D_0 report "mux2(0,0,0) with sel=0 should be 0" severity failure;

        -- Test 2: All ones
        a <= L3D_1; b <= L3D_1; c <= L3D_1; d <= L3D_1;
        wait for 0 ns;
        wait for 0 ns;

        report "Test 2: All ones";
        assert r_and3 = L3D_1 report "and3(1,1,1) should be 1" severity failure;
        assert r_and4 = L3D_1 report "and4(1,1,1,1) should be 1" severity failure;
        assert r_or3 = L3D_1 report "or3(1,1,1) should be 1" severity failure;
        assert r_or4 = L3D_1 report "or4(1,1,1,1) should be 1" severity failure;
        assert r_xor3 = L3D_1 report "xor3(1,1,1) should be 1" severity failure;
        assert r_xor4 = L3D_0 report "xor4(1,1,1,1) should be 0" severity failure;
        assert r_nand2 = L3D_0 report "nand2(1,1) should be 0" severity failure;
        assert r_nand3 = L3D_0 report "nand3(1,1,1) should be 0" severity failure;
        assert r_nor2 = L3D_0 report "nor2(1,1) should be 0" severity failure;
        assert r_nor3 = L3D_0 report "nor3(1,1,1) should be 0" severity failure;
        assert r_mux = L3D_1 report "mux2(1,1,1) with sel=1 should be 1" severity failure;

        -- Test 3: Mixed values for XOR parity
        a <= L3D_1; b <= L3D_0; c <= L3D_1; d <= L3D_0;
        wait for 0 ns;
        wait for 0 ns;

        report "Test 3: XOR parity check (1,0,1,0)";
        assert r_xor3 = L3D_0 report "xor3(1,0,1) should be 0" severity failure;
        assert r_xor4 = L3D_0 report "xor4(1,0,1,0) should be 0" severity failure;

        -- Test 4: MUX selection
        a <= L3D_0; b <= L3D_1; c <= L3D_0;  -- sel=0, a=1, b=0 -> result=1
        wait for 0 ns;
        wait for 0 ns;

        report "Test 4: MUX sel=0 should select input b";
        assert r_mux = L3D_1 report "mux2(0,1,0) should select b=1" severity failure;

        a <= L3D_1; b <= L3D_1; c <= L3D_0;  -- sel=1, a=1, b=0 -> result=0
        wait for 0 ns;
        wait for 0 ns;

        report "Test 5: MUX sel=1 should select input c";
        assert r_mux = L3D_0 report "mux2(1,1,0) should select c=0" severity failure;

        -- Test 5: X propagation
        a <= L3D_X; b <= L3D_1; c <= L3D_1; d <= L3D_1;
        wait for 0 ns;
        wait for 0 ns;

        report "Test 6: X propagation";
        assert r_and3 = L3D_X report "and3(X,1,1) should be X" severity failure;
        assert r_or3 = L3D_1 report "or3(X,1,1) should be 1 (1 dominates)" severity failure;
        assert r_xor3 = L3D_X report "xor3(X,1,1) should be X" severity failure;
        assert r_mux = L3D_X report "mux2(X,1,1) should be X" severity failure;

        -- Test 6: Single 0 in AND chain
        a <= L3D_1; b <= L3D_0; c <= L3D_1; d <= L3D_1;
        wait for 0 ns;
        wait for 0 ns;

        report "Test 7: Single 0 in AND";
        assert r_and3 = L3D_0 report "and3(1,0,1) should be 0" severity failure;
        assert r_and4 = L3D_0 report "and4(1,0,1,1) should be 0" severity failure;

        report "PASSED: All multi-input gate tests passed!";
        wait;
    end process;

end architecture;
