-- Test bench for resolver plugin
-- 4 test scenarios covering different resolution topologies
--
-- Signal naming convention: vector-based _driver/_others per net
--   signal <net>_driver : std_logic_vector(0 to N-1);
--   signal <net>_others : std_logic_vector(0 to N-1);
-- where N = number of switch endpoints on that net.

library ieee;
use ieee.std_logic_1164.all;

entity test_resolver_tb is
end entity test_resolver_tb;

architecture test of test_resolver_tb is

    ---------------------------------------------------------------------------
    -- Test 1: Simple bidirectional (two tran switches sharing net_a)
    --   tran0: a on net_a(0), b on net_b(0)
    --   tran1: a on net_a(1), b on net_c(0)
    --   Net A has 2 endpoints needing resolution
    ---------------------------------------------------------------------------
    signal t1_net_a_driver : std_logic_vector(0 to 1);
    signal t1_net_a_others : std_logic_vector(0 to 1);
    signal t1_net_b_driver : std_logic_vector(0 to 0);
    signal t1_net_b_others : std_logic_vector(0 to 0);
    signal t1_net_c_driver : std_logic_vector(0 to 0);
    signal t1_net_c_others : std_logic_vector(0 to 0);

    ---------------------------------------------------------------------------
    -- Test 2: Controlled bidirectional with pull gate
    --   tranif1_0: a on net_d(0), b on net_e(0)
    --   pullup on net_e (separate scalar, not part of vector)
    ---------------------------------------------------------------------------
    signal t2_net_d_driver : std_logic_vector(0 to 0);
    signal t2_net_d_others : std_logic_vector(0 to 0);
    signal t2_net_e_driver : std_logic_vector(0 to 0);
    signal t2_net_e_others : std_logic_vector(0 to 0);
    signal t2_ctrl : std_logic := '1';
    signal t2_pull_y : std_logic;

    ---------------------------------------------------------------------------
    -- Test 3: Star topology (three switches all meeting at net_f)
    --   tran_s0: a on net_f(0), b on net_g(0)
    --   tran_s1: a on net_f(1), b on net_h(0)
    --   tran_s2: a on net_f(2), b on net_i(0)
    --   Net F has 3 endpoints needing resolution
    ---------------------------------------------------------------------------
    signal t3_net_f_driver : std_logic_vector(0 to 2);
    signal t3_net_f_others : std_logic_vector(0 to 2);
    signal t3_net_g_driver : std_logic_vector(0 to 0);
    signal t3_net_g_others : std_logic_vector(0 to 0);
    signal t3_net_h_driver : std_logic_vector(0 to 0);
    signal t3_net_h_others : std_logic_vector(0 to 0);
    signal t3_net_i_driver : std_logic_vector(0 to 0);
    signal t3_net_i_others : std_logic_vector(0 to 0);

    ---------------------------------------------------------------------------
    -- Test 4: Cascaded chain (A <-> B <-> C)
    --   tran_c0: a on net_j(0), b on net_k(0)
    --   tran_c1: a on net_k(1), b on net_l(0)
    --   Net K has 2 endpoints needing resolution
    ---------------------------------------------------------------------------
    signal t4_net_j_driver : std_logic_vector(0 to 0);
    signal t4_net_j_others : std_logic_vector(0 to 0);
    signal t4_net_k_driver : std_logic_vector(0 to 1);
    signal t4_net_k_others : std_logic_vector(0 to 1);
    signal t4_net_l_driver : std_logic_vector(0 to 0);
    signal t4_net_l_others : std_logic_vector(0 to 0);

begin

    ---------------------------------------------------------------------------
    -- Test 1: Two tran switches sharing net_a
    ---------------------------------------------------------------------------
    t1_tran0: entity work.sv_tran
        port map (
            a_others => t1_net_a_others(0),
            a_driver => t1_net_a_driver(0),
            b_others => t1_net_b_others(0),
            b_driver => t1_net_b_driver(0)
        );

    t1_tran1: entity work.sv_tran
        port map (
            a_others => t1_net_a_others(1),
            a_driver => t1_net_a_driver(1),
            b_others => t1_net_c_others(0),
            b_driver => t1_net_c_driver(0)
        );

    ---------------------------------------------------------------------------
    -- Test 2: Controlled switch + pull gate
    ---------------------------------------------------------------------------
    t2_trif0: entity work.sv_tranif1
        port map (
            a_others => t2_net_d_others(0),
            a_driver => t2_net_d_driver(0),
            b_others => t2_net_e_others(0),
            b_driver => t2_net_e_driver(0),
            ctrl     => t2_ctrl
        );

    t2_pull0: entity work.sv_pullup
        port map (y => t2_pull_y);

    ---------------------------------------------------------------------------
    -- Test 3: Star topology
    ---------------------------------------------------------------------------
    t3_s0: entity work.sv_tran
        port map (
            a_others => t3_net_f_others(0),
            a_driver => t3_net_f_driver(0),
            b_others => t3_net_g_others(0),
            b_driver => t3_net_g_driver(0)
        );

    t3_s1: entity work.sv_tran
        port map (
            a_others => t3_net_f_others(1),
            a_driver => t3_net_f_driver(1),
            b_others => t3_net_h_others(0),
            b_driver => t3_net_h_driver(0)
        );

    t3_s2: entity work.sv_tran
        port map (
            a_others => t3_net_f_others(2),
            a_driver => t3_net_f_driver(2),
            b_others => t3_net_i_others(0),
            b_driver => t3_net_i_driver(0)
        );

    ---------------------------------------------------------------------------
    -- Test 4: Cascaded chain (A <-> B <-> C)
    ---------------------------------------------------------------------------
    t4_c0: entity work.sv_tran
        port map (
            a_others => t4_net_j_others(0),
            a_driver => t4_net_j_driver(0),
            b_others => t4_net_k_others(0),
            b_driver => t4_net_k_driver(0)
        );

    t4_c1: entity work.sv_tran
        port map (
            a_others => t4_net_k_others(1),
            a_driver => t4_net_k_driver(1),
            b_others => t4_net_l_others(0),
            b_driver => t4_net_l_driver(0)
        );

    ---------------------------------------------------------------------------
    -- Stimulus and checking
    ---------------------------------------------------------------------------
    process
        variable pass_count : integer := 0;
        variable fail_count : integer := 0;

        -- Convert std_logic to readable string for reports
        function sl_str(s : std_logic) return string is
            type sl_str_t is array (std_ulogic) of string(1 to 1);
            constant tbl : sl_str_t := ("U", "X", "0", "1", "Z", "W", "L", "H", "-");
        begin
            return tbl(s);
        end;

        -- Check expected value, report on mismatch
        procedure check(actual, expected : std_logic; msg : string) is
        begin
            if actual = expected then
                pass_count := pass_count + 1;
            else
                fail_count := fail_count + 1;
                report "FAIL: " & msg &
                       " -- expected " & sl_str(expected) &
                       " got " & sl_str(actual)
                    severity error;
            end if;
        end;

    begin
        -----------------------------------------------------------------------
        -- Initialize: all leaf _others to 'Z' (not driving)
        -----------------------------------------------------------------------
        t1_net_b_others(0) <= 'Z';
        t1_net_c_others(0) <= 'Z';
        t2_net_d_others(0) <= 'Z';
        t2_net_e_others(0) <= 'Z';
        t3_net_g_others(0) <= 'Z';
        t3_net_h_others(0) <= 'Z';
        t3_net_i_others(0) <= 'Z';
        t4_net_j_others(0) <= 'Z';
        t4_net_l_others(0) <= 'Z';
        wait for 1 ns;

        -----------------------------------------------------------------------
        -- Test 1: Simple bidirectional (net_b <-> net_a <-> net_c)
        --   tran0: b=net_b, a=net_a(0)
        --   tran1: b=net_c, a=net_a(1)
        --   resolver swaps: oth_a(0)=drv_a(1), oth_a(1)=drv_a(0)
        -----------------------------------------------------------------------

        -- 1a: Drive '1' from B side, expect it at C side
        report "Test 1a: B='1', C='Z' -> C_driver should be '1'";
        t1_net_b_others(0) <= '1';
        t1_net_c_others(0) <= 'Z';
        wait for 1 ns;
        check(t1_net_c_driver(0), '1', "T1a: net_c_driver");

        -- 1b: Drive '0' from C side, expect it at B side
        report "Test 1b: B='Z', C='0' -> B_driver should be '0'";
        t1_net_b_others(0) <= 'Z';
        t1_net_c_others(0) <= '0';
        wait for 1 ns;
        check(t1_net_b_driver(0), '0', "T1b: net_b_driver");

        -- 1c: Both sides drive simultaneously -> each sees the other
        report "Test 1c: B='1', C='0' -> B_driver='0', C_driver='1'";
        t1_net_b_others(0) <= '1';
        t1_net_c_others(0) <= '0';
        wait for 1 ns;
        check(t1_net_c_driver(0), '1', "T1c: net_c_driver");
        check(t1_net_b_driver(0), '0', "T1c: net_b_driver");

        -- Reset
        t1_net_b_others(0) <= 'Z';
        t1_net_c_others(0) <= 'Z';
        wait for 1 ns;

        -----------------------------------------------------------------------
        -- Test 2: Controlled switch (tranif1: conducts when ctrl='1')
        --   Single-endpoint nets d and e (no resolver, TB is sole driver)
        -----------------------------------------------------------------------

        -- 2a: ctrl='1' (default), drive '1' from D -> expect at E
        report "Test 2a: ctrl='1', D='1' -> E_driver should be '1'";
        t2_net_d_others(0) <= '1';
        t2_net_e_others(0) <= 'Z';
        wait for 1 ns;
        check(t2_net_e_driver(0), '1', "T2a: net_e_driver (ctrl=1)");

        -- 2b: ctrl='0', switch should block -> expect 'Z' at E
        report "Test 2b: ctrl='0', D='1' -> E_driver should be 'Z'";
        t2_ctrl <= '0';
        wait for 1 ns;
        check(t2_net_e_driver(0), 'Z', "T2b: net_e_driver (ctrl=0)");

        -- 2c: Pullup check
        report "Test 2c: pullup -> t2_pull_y should be 'H'";
        check(t2_pull_y, 'H', "T2c: pullup output");

        -- Reset
        t2_ctrl <= '1';
        t2_net_d_others(0) <= 'Z';
        wait for 1 ns;

        -----------------------------------------------------------------------
        -- Test 3: Star topology (net_g/h/i <-> net_f via 3 tran)
        --   tran_s0: b=net_g, a=net_f(0)
        --   tran_s1: b=net_h, a=net_f(1)
        --   tran_s2: b=net_i, a=net_f(2)
        --   resolver: oth_f(i) = resolved(all drv_f except i)
        -----------------------------------------------------------------------

        -- 3a: Drive '1' from G, Z from H/I -> H and I should see '1'
        report "Test 3a: G='1', H='Z', I='Z' -> H_drv='1', I_drv='1'";
        t3_net_g_others(0) <= '1';
        t3_net_h_others(0) <= 'Z';
        t3_net_i_others(0) <= 'Z';
        wait for 1 ns;
        check(t3_net_h_driver(0), '1', "T3a: net_h_driver");
        check(t3_net_i_driver(0), '1', "T3a: net_i_driver");

        -- 3b: G='1', H='0' conflict -> I sees resolved('1','0')='X'
        --   oth_f(2) = resolved(drv_f(0) & drv_f(1)) = resolved('1' & '0') = 'X'
        --   oth_f(0) = resolved(drv_f(1) & drv_f(2)) = resolved('0' & 'Z') = '0'
        --   oth_f(1) = resolved(drv_f(0) & drv_f(2)) = resolved('1' & 'Z') = '1'
        report "Test 3b: G='1', H='0', I='Z' -> I_drv='X', G_drv='0', H_drv='1'";
        t3_net_g_others(0) <= '1';
        t3_net_h_others(0) <= '0';
        t3_net_i_others(0) <= 'Z';
        wait for 1 ns;
        check(t3_net_i_driver(0), 'X', "T3b: net_i_driver (conflict)");
        check(t3_net_g_driver(0), '0', "T3b: net_g_driver");
        check(t3_net_h_driver(0), '1', "T3b: net_h_driver");

        -- 3c: All 'Z' -> all drivers 'Z'
        report "Test 3c: G='Z', H='Z', I='Z' -> all _driver='Z'";
        t3_net_g_others(0) <= 'Z';
        t3_net_h_others(0) <= 'Z';
        t3_net_i_others(0) <= 'Z';
        wait for 1 ns;
        check(t3_net_g_driver(0), 'Z', "T3c: net_g_driver");
        check(t3_net_h_driver(0), 'Z', "T3c: net_h_driver");
        check(t3_net_i_driver(0), 'Z', "T3c: net_i_driver");

        -----------------------------------------------------------------------
        -- Test 4: Cascaded chain (net_j <-> net_k <-> net_l)
        --   tran_c0: a=net_j, b=net_k(0)
        --   tran_c1: a=net_k(1), b=net_l
        --   resolver swaps: oth_k(0)=drv_k(1), oth_k(1)=drv_k(0)
        -----------------------------------------------------------------------

        -- 4a: Drive '1' from J -> expect at L
        report "Test 4a: J='1', L='Z' -> L_driver should be '1'";
        t4_net_j_others(0) <= '1';
        t4_net_l_others(0) <= 'Z';
        wait for 1 ns;
        check(t4_net_l_driver(0), '1', "T4a: net_l_driver");

        -- 4b: Drive '0' from L -> expect at J
        report "Test 4b: J='Z', L='0' -> J_driver should be '0'";
        t4_net_j_others(0) <= 'Z';
        t4_net_l_others(0) <= '0';
        wait for 1 ns;
        check(t4_net_j_driver(0), '0', "T4b: net_j_driver");

        -----------------------------------------------------------------------
        -- Summary
        -----------------------------------------------------------------------
        report "";
        report "========================================";
        report "  Results: " & integer'image(pass_count) & " passed, " &
               integer'image(fail_count) & " failed";
        report "========================================";

        if fail_count > 0 then
            report "TEST FAILED" severity failure;
        else
            report "ALL TESTS PASSED";
        end if;

        wait;
    end process;

end architecture test;
