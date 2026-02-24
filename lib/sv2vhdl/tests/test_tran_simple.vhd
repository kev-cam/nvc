-- test_tran_simple.vhd
-- Simplest possible tran test with hand-written resolver.
-- Tests: sv_assign -> tran -> observe
--
-- Topology:
--   signal a drives sv_assign.d (input)
--   sv_assign.q connects to net "left"  (assign driver endpoint)
--   sv_tran.a connects to net "left"    (tran endpoint)
--   sv_tran.b connects to net "right"   (tran endpoint)
--
-- Net "left" has 2 endpoints: assign.q (driver), tran.a (bidirectional)
-- Net "right" has 1 endpoint: tran.b (bidirectional)
--
-- Expected: left follows assign driver, right follows left through tran.

---------------------------------------------------------------------------
-- DUT: assign + tran
---------------------------------------------------------------------------
library ieee;
use ieee.std_logic_1164.all;
use work.logic3ds_pkg.all;

-- sv_assign entity (inline for simplicity)
entity sv_assign_ts is
    port (d : in std_logic; q : inout std_logic);
end entity;

architecture strength of sv_assign_ts is
begin
    process(d)
    begin
        q'driver := to_logic3ds(d, ST_STRONG);
    end process;
end architecture;

-- DUT
library ieee;
use ieee.std_logic_1164.all;

entity dut_tran_simple is
    port (a : in std_logic;
          obs_left, obs_right : out std_logic);
end entity;

architecture test of dut_tran_simple is
    signal left, right : std_logic;
begin
    drv: entity work.sv_assign_ts(strength)
        port map(d => a, q => left);
    t1:  entity work.sv_tran(strength)
        port map(a => left, b => right);

    obs_left  <= left;
    obs_right <= right;
end architecture;

---------------------------------------------------------------------------
-- Resolver: reads 'driver, deposits to 'receiver
-- Net "left":  assign.q'driver + tran.a'driver -> resolve -> each 'receiver
-- Net "right": tran.b'driver -> deposit -> 'receiver
---------------------------------------------------------------------------
library ieee;
use ieee.std_logic_1164.all;
use work.logic3ds_pkg.all;

entity rn_tran_simple is end entity;

architecture gen of rn_tran_simple is
    -- Net "left" drivers
    alias drv_assign_q is
        << signal .wrap_tran_simple.dut.drv.q.driver : logic3ds >>;
    alias drv_tran_a is
        << signal .wrap_tran_simple.dut.t1.a.driver : logic3ds >>;

    -- Net "right" drivers
    alias drv_tran_b is
        << signal .wrap_tran_simple.dut.t1.b.driver : logic3ds >>;

    -- Net "left" others (tran endpoints)
    alias oth_tran_a is
        << signal .wrap_tran_simple.dut.t1.a.other : logic3ds >>;

    -- Net "right" others (tran endpoints)
    alias oth_tran_b is
        << signal .wrap_tran_simple.dut.t1.b.other : logic3ds >>;

    -- Receivers for depositing resolved values
    alias rcv_left is
        << signal .wrap_tran_simple.dut.left.receiver : std_logic >>;
    alias rcv_right is
        << signal .wrap_tran_simple.dut.right.receiver : std_logic >>;
begin
    -- p_assign: for assign-type endpoints, forward the resolved tran
    -- cross-drive value into their 'other
    p_assign: process(drv_tran_a)
    begin
        -- Net "left": assign.q is the only non-tran driver.
        -- The tran's cross-drive (drv_tran_a) becomes assign.q's 'other.
        -- But sv_assign doesn't read 'other, so this is just informational.
        -- (Nothing to do for assign endpoints)
    end process;

    -- p_tran: for tran endpoints, compute the value they should see
    -- from all OTHER drivers on the same net
    p_tran: process(drv_assign_q, drv_tran_a, drv_tran_b)
        variable resolved_left, resolved_right : logic3ds;
    begin
        -- Net "left" has 2 endpoints: assign.q and tran.a
        -- tran.a's 'other = resolve(all other drivers on net "left")
        --                  = assign.q'driver (only other driver)
        oth_tran_a := drv_assign_q;

        -- Net "right" has 1 endpoint: tran.b
        -- tran.b's 'other = resolve(all other drivers on net "right")
        --                  = L3DS_Z (no other drivers)
        oth_tran_b := L3DS_Z;
    end process;

    -- p_receivers: deposit resolved values to signal receivers
    p_receivers: process(drv_assign_q, drv_tran_a, drv_tran_b)
        variable left_val, right_val : std_logic;
    begin
        -- Net "left" = resolve(assign.q'driver, tran.a'driver)
        left_val := to_std_logic(l3ds_resolve(
            logic3ds_vector'(drv_assign_q, drv_tran_a)));

        -- Net "right" = resolve(tran.b'driver)
        -- Only one driver, so just convert
        right_val := to_std_logic(drv_tran_b);

        report "RN: drv_assign_q=" & to_string(drv_assign_q)
             & " drv_tran_a=" & to_string(drv_tran_a)
             & " drv_tran_b=" & to_string(drv_tran_b)
             & " -> left=" & std_logic'image(left_val)
             & " right=" & std_logic'image(right_val);

        rcv_left  := left_val;
        rcv_right := right_val;
    end process;
end architecture;

---------------------------------------------------------------------------
-- Wrapper
---------------------------------------------------------------------------
library ieee;
use ieee.std_logic_1164.all;

entity wrap_tran_simple is end entity;

architecture w of wrap_tran_simple is
    signal a : std_logic := 'Z';
    signal obs_left, obs_right : std_logic;
begin
    dut: entity work.dut_tran_simple
        port map(a => a, obs_left => obs_left, obs_right => obs_right);
    rn: entity work.rn_tran_simple;

    process
    begin
        -- Test 1: drive '0'
        a <= '0';
        wait for 1 ns;
        report "T1: a=0: left=" & std_logic'image(obs_left)
             & " right=" & std_logic'image(obs_right);
        assert obs_left = '0'
            report "FAIL T1: left should be '0'" severity error;
        assert obs_right = '0'
            report "FAIL T1: right should be '0'" severity error;

        -- Test 2: drive '1'
        a <= '1';
        wait for 1 ns;
        report "T2: a=1: left=" & std_logic'image(obs_left)
             & " right=" & std_logic'image(obs_right);
        assert obs_left = '1'
            report "FAIL T2: left should be '1'" severity error;
        assert obs_right = '1'
            report "FAIL T2: right should be '1'" severity error;

        -- Test 3: drive 'Z'
        a <= 'Z';
        wait for 1 ns;
        report "T3: a=Z: left=" & std_logic'image(obs_left)
             & " right=" & std_logic'image(obs_right);
        assert obs_left = 'Z'
            report "FAIL T3: left should be 'Z'" severity error;
        assert obs_right = 'Z'
            report "FAIL T3: right should be 'Z'" severity error;

        report "ALL DONE";
        wait;
    end process;
end architecture;
