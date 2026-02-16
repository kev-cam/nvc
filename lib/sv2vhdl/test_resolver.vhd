-- Test bench for resolver topologies
-- 4 test scenarios covering different resolution topologies
--
-- Tran entities use inout ports with 'driver/'others implicit signals.
-- Functional value propagation requires either:
--   1. The resolver plugin (--load=./libresolver.so) to connect 'driver/'others
--   2. Native 'others runtime support in NVC
-- Without these, this test verifies elaboration and entity instantiation.

library ieee;
use ieee.std_logic_1164.all;

entity test_resolver_tb is
end entity test_resolver_tb;

architecture test of test_resolver_tb is

    ---------------------------------------------------------------------------
    -- Test 1: Simple bidirectional (two tran switches sharing net_a)
    --   tran0: a=net_a, b=net_b
    --   tran1: a=net_a, b=net_c
    --   Net A has 2 switch endpoints needing resolution
    ---------------------------------------------------------------------------
    signal t1_net_a : std_logic;
    signal t1_net_b : std_logic;
    signal t1_net_c : std_logic;

    ---------------------------------------------------------------------------
    -- Test 2: Controlled bidirectional with pull gate
    --   tranif1_0: a=net_d, b=net_e, ctrl=t2_ctrl
    --   pullup on t2_pull_y (separate net)
    ---------------------------------------------------------------------------
    signal t2_net_d : std_logic;
    signal t2_net_e : std_logic;
    signal t2_ctrl  : std_logic := '1';
    signal t2_pull_y : std_logic;

    ---------------------------------------------------------------------------
    -- Test 3: Star topology (three switches all meeting at net_f)
    --   tran_s0: a=net_f, b=net_g
    --   tran_s1: a=net_f, b=net_h
    --   tran_s2: a=net_f, b=net_i
    --   Net F has 3 switch endpoints needing resolution
    ---------------------------------------------------------------------------
    signal t3_net_f : std_logic;
    signal t3_net_g : std_logic;
    signal t3_net_h : std_logic;
    signal t3_net_i : std_logic;

    ---------------------------------------------------------------------------
    -- Test 4: Cascaded chain (J <-> K <-> L)
    --   tran_c0: a=net_j, b=net_k
    --   tran_c1: a=net_k, b=net_l
    --   Net K has 2 switch endpoints needing resolution
    ---------------------------------------------------------------------------
    signal t4_net_j : std_logic;
    signal t4_net_k : std_logic;
    signal t4_net_l : std_logic;

begin

    ---------------------------------------------------------------------------
    -- Test 1: Two tran switches sharing net_a
    ---------------------------------------------------------------------------
    t1_tran0: entity work.sv_tran
        port map (a => t1_net_a, b => t1_net_b);

    t1_tran1: entity work.sv_tran
        port map (a => t1_net_a, b => t1_net_c);

    ---------------------------------------------------------------------------
    -- Test 2: Controlled switch + pull gate
    ---------------------------------------------------------------------------
    t2_trif0: entity work.sv_tranif1
        port map (a => t2_net_d, b => t2_net_e, ctrl => t2_ctrl);

    t2_pull0: entity work.sv_pullup
        port map (y => t2_pull_y);

    ---------------------------------------------------------------------------
    -- Test 3: Star topology
    ---------------------------------------------------------------------------
    t3_s0: entity work.sv_tran
        port map (a => t3_net_f, b => t3_net_g);

    t3_s1: entity work.sv_tran
        port map (a => t3_net_f, b => t3_net_h);

    t3_s2: entity work.sv_tran
        port map (a => t3_net_f, b => t3_net_i);

    ---------------------------------------------------------------------------
    -- Test 4: Cascaded chain
    ---------------------------------------------------------------------------
    t4_c0: entity work.sv_tran
        port map (a => t4_net_j, b => t4_net_k);

    t4_c1: entity work.sv_tran
        port map (a => t4_net_k, b => t4_net_l);

    ---------------------------------------------------------------------------
    -- Stimulus
    ---------------------------------------------------------------------------
    process
        variable pass_count : integer := 0;
        variable fail_count : integer := 0;

        procedure check(actual, expected : std_logic; msg : string) is
            type sl_str_t is array (std_ulogic) of string(1 to 1);
            constant tbl : sl_str_t := ("U","X","0","1","Z","W","L","H","-");
        begin
            if actual = expected then
                pass_count := pass_count + 1;
            else
                fail_count := fail_count + 1;
                report "FAIL: " & msg &
                       " -- expected " & tbl(expected) &
                       " got " & tbl(actual)
                    severity error;
            end if;
        end;

    begin
        -- Elaboration check: all entities instantiated with inout ports
        wait for 1 ns;

        report "=== Elaboration OK: 4 topologies instantiated ===";
        report "  T1: 2 tran sharing net_a (simple bidi)";
        report "  T2: tranif1 + pullup (controlled switch)";
        report "  T3: 3 tran sharing net_f (star topology)";
        report "  T4: 2 tran chain net_j-net_k-net_l (cascade)";

        -- Pullup is self-contained (no resolver needed)
        check(t2_pull_y, 'H', "pullup output");

        -- Toggle control signal to exercise tranif1 sensitivity list
        t2_ctrl <= '0';
        wait for 1 ns;
        t2_ctrl <= '1';
        wait for 1 ns;

        report "";
        report "========================================";
        report "  Results: " & integer'image(pass_count) & " passed, " &
               integer'image(fail_count) & " failed";
        report "  (Functional value propagation tests pending";
        report "   resolver plugin update for implicit signals)";
        report "========================================";

        if fail_count > 0 then
            report "TEST FAILED" severity failure;
        else
            report "ALL TESTS PASSED";
        end if;

        wait;
    end process;

end architecture test;
