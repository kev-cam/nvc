-- test_tran_chain2.vhd
-- Two-tran chain: sv_assign -> tran -> tran -> observe
--
-- Nets:
--   n1: assign.q + t1.a  (2 endpoints)
--   n2: t1.b + t2.a      (2 endpoints, both tran)
--   n3: t2.b             (1 endpoint)

---------------------------------------------------------------------------
-- DUT
---------------------------------------------------------------------------
library ieee;
use ieee.std_logic_1164.all;
use work.logic3ds_pkg.all;

entity sv_assign_tc2 is
    port (d : in std_logic; q : inout std_logic);
end entity;

architecture strength of sv_assign_tc2 is
begin
    process(d)
    begin
        q'driver := to_logic3ds(d, ST_STRONG);
    end process;
end architecture;

library ieee;
use ieee.std_logic_1164.all;

entity dut_tran_chain2 is
    port (a : in std_logic;
          obs1, obs2, obs3 : out std_logic);
end entity;

architecture test of dut_tran_chain2 is
    signal n1, n2, n3 : std_logic;
begin
    drv: entity work.sv_assign_tc2(strength)
        port map(d => a, q => n1);
    t1:  entity work.sv_tran(strength)
        port map(a => n1, b => n2);
    t2:  entity work.sv_tran(strength)
        port map(a => n2, b => n3);

    obs1 <= n1;
    obs2 <= n2;
    obs3 <= n3;
end architecture;

---------------------------------------------------------------------------
-- Resolver
---------------------------------------------------------------------------
library ieee;
use ieee.std_logic_1164.all;
use work.logic3ds_pkg.all;

entity rn_tran_chain2 is end entity;

architecture gen of rn_tran_chain2 is
    -- Net n1 drivers
    alias drv_assign_q is
        << signal .wrap_tran_chain2.dut.drv.q.driver : logic3ds >>;
    alias drv_t1_a is
        << signal .wrap_tran_chain2.dut.t1.a.driver : logic3ds >>;

    -- Net n2 drivers
    alias drv_t1_b is
        << signal .wrap_tran_chain2.dut.t1.b.driver : logic3ds >>;
    alias drv_t2_a is
        << signal .wrap_tran_chain2.dut.t2.a.driver : logic3ds >>;

    -- Net n3 drivers
    alias drv_t2_b is
        << signal .wrap_tran_chain2.dut.t2.b.driver : logic3ds >>;

    -- Tran 'other signals
    alias oth_t1_a is
        << signal .wrap_tran_chain2.dut.t1.a.other : logic3ds >>;
    alias oth_t1_b is
        << signal .wrap_tran_chain2.dut.t1.b.other : logic3ds >>;
    alias oth_t2_a is
        << signal .wrap_tran_chain2.dut.t2.a.other : logic3ds >>;
    alias oth_t2_b is
        << signal .wrap_tran_chain2.dut.t2.b.other : logic3ds >>;

    -- Receivers
    alias rcv_n1 is
        << signal .wrap_tran_chain2.dut.n1.receiver : std_logic >>;
    alias rcv_n2 is
        << signal .wrap_tran_chain2.dut.n2.receiver : std_logic >>;
    alias rcv_n3 is
        << signal .wrap_tran_chain2.dut.n3.receiver : std_logic >>;
begin
    -- p_tran: feed 'other to each tran endpoint
    p_tran: process(drv_assign_q, drv_t1_a, drv_t1_b, drv_t2_a, drv_t2_b)
    begin
        -- Net n1: assign.q + t1.a
        -- t1.a'other = assign.q'driver (only other driver on net n1)
        oth_t1_a := drv_assign_q;
        -- (assign doesn't read 'other)

        -- Net n2: t1.b + t2.a (both tran)
        -- t1.b'other = t2.a'driver
        -- t2.a'other = t1.b'driver
        oth_t1_b := drv_t2_a;
        oth_t2_a := drv_t1_b;

        -- Net n3: t2.b only
        -- t2.b'other = L3DS_Z (no other drivers)
        oth_t2_b := L3DS_Z;
    end process;

    -- p_receivers: deposit resolved net values
    p_receivers: process(drv_assign_q, drv_t1_a, drv_t1_b, drv_t2_a, drv_t2_b)
        variable v1, v2, v3 : std_logic;
    begin
        -- Net n1 = resolve(assign.q'driver, t1.a'driver)
        v1 := to_std_logic(l3ds_resolve(
            logic3ds_vector'(drv_assign_q, drv_t1_a)));
        -- Net n2 = resolve(t1.b'driver, t2.a'driver)
        v2 := to_std_logic(l3ds_resolve(
            logic3ds_vector'(drv_t1_b, drv_t2_a)));
        -- Net n3 = resolve(t2.b'driver) = just convert
        v3 := to_std_logic(drv_t2_b);

        report "RN: n1=" & std_logic'image(v1)
             & " n2=" & std_logic'image(v2)
             & " n3=" & std_logic'image(v3)
             & " | drv_t1_b=" & to_string(drv_t1_b)
             & " drv_t2_b=" & to_string(drv_t2_b);

        rcv_n1 := v1;
        rcv_n2 := v2;
        rcv_n3 := v3;
    end process;
end architecture;

---------------------------------------------------------------------------
-- Wrapper
---------------------------------------------------------------------------
library ieee;
use ieee.std_logic_1164.all;

entity wrap_tran_chain2 is end entity;

architecture w of wrap_tran_chain2 is
    signal a : std_logic := 'Z';
    signal obs1, obs2, obs3 : std_logic;
begin
    dut: entity work.dut_tran_chain2
        port map(a => a, obs1 => obs1, obs2 => obs2, obs3 => obs3);
    rn: entity work.rn_tran_chain2;

    process
    begin
        a <= '0';
        wait for 1 ns;
        report "T1: a=0: n1=" & std_logic'image(obs1)
             & " n2=" & std_logic'image(obs2)
             & " n3=" & std_logic'image(obs3);
        assert obs1 = '0' report "FAIL T1: n1" severity error;
        assert obs2 = '0' report "FAIL T1: n2" severity error;
        assert obs3 = '0' report "FAIL T1: n3" severity error;

        a <= '1';
        wait for 1 ns;
        report "T2: a=1: n1=" & std_logic'image(obs1)
             & " n2=" & std_logic'image(obs2)
             & " n3=" & std_logic'image(obs3);
        assert obs1 = '1' report "FAIL T2: n1" severity error;
        assert obs2 = '1' report "FAIL T2: n2" severity error;
        assert obs3 = '1' report "FAIL T2: n3" severity error;

        a <= 'Z';
        wait for 1 ns;
        report "T3: a=Z: n1=" & std_logic'image(obs1)
             & " n2=" & std_logic'image(obs2)
             & " n3=" & std_logic'image(obs3);
        assert obs1 = 'Z' report "FAIL T3: n1" severity error;
        assert obs2 = 'Z' report "FAIL T3: n2" severity error;
        assert obs3 = 'Z' report "FAIL T3: n3" severity error;

        report "ALL DONE";
        wait;
    end process;
end architecture;
