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
    -- Stimulus
    ---------------------------------------------------------------------------
    process
    begin
        wait for 10 ns;
        t2_ctrl <= '0';
        wait for 10 ns;
        wait;
    end process;

end architecture test;
