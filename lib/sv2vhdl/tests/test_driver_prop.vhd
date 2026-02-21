-- test_driver_prop.vhd
-- Tests 'driver -> resolver -> 'other propagation.
-- Hand-written resolvers, no plugin.
--
-- Resolver is a DAG: only reads 'driver, only writes 'other.
-- Every driver on a net must be an entity with inout ports
-- so it has 'driver/'other implicit signals.

---------------------------------------------------------------------------
-- sv_assign: models a continuous assignment (Verilog: assign q = d)
-- Deposits into q'driver for the resolver to read.
-- Relays d to q for the signal value.
---------------------------------------------------------------------------
library ieee;
use ieee.std_logic_1164.all;
use work.logic3ds_pkg.all;

entity sv_assign is
    port (
        d : in std_logic;
        q : inout std_logic
    );
end entity sv_assign;

architecture strength of sv_assign is
begin
    process(d)
    begin
        q'driver := to_logic3ds(d, ST_STRONG);
        q <= d;
    end process;
end architecture strength;

---------------------------------------------------------------------------
-- TEST 1: Pass-through
-- sv_assign -> tran -> observe
-- Net "left":  2 endpoints (drv.q, t1.a) -> swap
-- Net "right": 1 endpoint  (t1.b) -> Z
---------------------------------------------------------------------------
library ieee;
use ieee.std_logic_1164.all;

entity test_prop1 is end entity;

architecture test of test_prop1 is
    signal a : std_logic := 'Z';
    signal left, right : std_logic;
begin
    drv: entity work.sv_assign(strength)
        port map(d => a, q => left);
    t1: entity work.sv_tran(strength)
        port map(a => left, b => right);

    process
    begin
        a <= '0';
        wait for 1 ns;
        report "prop1 a=0: left=" & std_logic'image(left) &
               " right=" & std_logic'image(right);
        assert right = '0'
            report "FAIL prop1: right should be '0'" severity error;

        a <= '1';
        wait for 1 ns;
        report "prop1 a=1: left=" & std_logic'image(left) &
               " right=" & std_logic'image(right);
        assert right = '1'
            report "FAIL prop1: right should be '1'" severity error;

        report "test_prop1 DONE";
        wait;
    end process;
end architecture;

---------------------------------------------------------------------------
-- TEST 2: Chain of 2 trans
-- sv_assign -> tran -> tran -> observe
---------------------------------------------------------------------------
library ieee;
use ieee.std_logic_1164.all;

entity test_prop2 is end entity;

architecture test of test_prop2 is
    signal a : std_logic := 'Z';
    signal n1, n2, n3 : std_logic;
begin
    drv: entity work.sv_assign(strength)
        port map(d => a, q => n1);
    t1: entity work.sv_tran(strength)
        port map(a => n1, b => n2);
    t2: entity work.sv_tran(strength)
        port map(a => n2, b => n3);

    process
    begin
        a <= '0';
        wait for 1 ns;
        report "prop2 a=0: " & std_logic'image(n1) & " " &
               std_logic'image(n2) & " " & std_logic'image(n3);
        assert n3 = '0'
            report "FAIL prop2: n3 should be '0'" severity error;

        a <= '1';
        wait for 1 ns;
        report "prop2 a=1: " & std_logic'image(n1) & " " &
               std_logic'image(n2) & " " & std_logic'image(n3);
        assert n3 = '1'
            report "FAIL prop2: n3 should be '1'" severity error;

        report "test_prop2 DONE";
        wait;
    end process;
end architecture;

---------------------------------------------------------------------------
-- TEST 3: Bidirectional
-- sv_assign on each side of a tran, drive from either side
-- Net "left":  2 endpoints (drv_a.q, t1.a) -> swap
-- Net "right": 2 endpoints (drv_b.q, t1.b) -> swap
---------------------------------------------------------------------------
library ieee;
use ieee.std_logic_1164.all;

entity test_prop3 is end entity;

architecture test of test_prop3 is
    signal a, b : std_logic := 'Z';
    signal left, right : std_logic;
begin
    drv_a: entity work.sv_assign(strength)
        port map(d => a, q => left);
    drv_b: entity work.sv_assign(strength)
        port map(d => b, q => right);
    t1: entity work.sv_tran(strength)
        port map(a => left, b => right);

    process
    begin
        -- Left to right
        a <= '0'; b <= 'Z';
        wait for 1 ns;
        report "prop3 L->R a=0 b=Z: left=" & std_logic'image(left) &
               " right=" & std_logic'image(right);
        assert right = '0'
            report "FAIL prop3 L->R: right should be '0'" severity error;

        -- Right to left
        a <= 'Z'; b <= '1';
        wait for 1 ns;
        report "prop3 R->L a=Z b=1: left=" & std_logic'image(left) &
               " right=" & std_logic'image(right);
        assert left = '1'
            report "FAIL prop3 R->L: left should be '1'" severity error;

        -- Conflict
        a <= '0'; b <= '1';
        wait for 1 ns;
        report "prop3 conflict a=0 b=1: left=" & std_logic'image(left) &
               " right=" & std_logic'image(right);
        assert left = 'X'
            report "FAIL prop3 conflict: left should be 'X'" severity error;
        assert right = 'X'
            report "FAIL prop3 conflict: right should be 'X'" severity error;

        -- Both Z
        a <= 'Z'; b <= 'Z';
        wait for 1 ns;
        report "prop3 both Z: left=" & std_logic'image(left) &
               " right=" & std_logic'image(right);

        report "test_prop3 DONE";
        wait;
    end process;
end architecture;

---------------------------------------------------------------------------
-- TEST 4: Chain of 4 trans with generate
---------------------------------------------------------------------------
library ieee;
use ieee.std_logic_1164.all;

entity test_prop4 is end entity;

architecture test of test_prop4 is
    signal a : std_logic := 'Z';
    signal n : std_logic_vector(1 to 5);
begin
    drv: entity work.sv_assign(strength)
        port map(d => a, q => n(1));
    gen: for i in 1 to 4 generate
        ti: entity work.sv_tran(strength)
            port map(a => n(i), b => n(i+1));
    end generate;

    process
    begin
        a <= '0';
        wait for 1 ns;
        report "prop4 a=0: " &
               std_logic'image(n(1)) & " " & std_logic'image(n(2)) & " " &
               std_logic'image(n(3)) & " " & std_logic'image(n(4)) & " " &
               std_logic'image(n(5));
        assert n(5) = '0'
            report "FAIL prop4: n(5) should be '0'" severity error;

        a <= '1';
        wait for 1 ns;
        report "prop4 a=1: " &
               std_logic'image(n(1)) & " " & std_logic'image(n(2)) & " " &
               std_logic'image(n(3)) & " " & std_logic'image(n(4)) & " " &
               std_logic'image(n(5));
        assert n(5) = '1'
            report "FAIL prop4: n(5) should be '1'" severity error;

        report "test_prop4 DONE";
        wait;
    end process;
end architecture;

---------------------------------------------------------------------------
-- TEST 5: Two sv_assigns tied to same net + tran observing
-- Net "net1": 3 endpoints (drv_a.q, drv_b.q, t1.a) -> N=3 resolution
-- Net "obs":  1 endpoint  (t1.b) -> Z
---------------------------------------------------------------------------
library ieee;
use ieee.std_logic_1164.all;

entity test_prop5 is end entity;

architecture test of test_prop5 is
    signal a, b : std_logic := 'Z';
    signal net1, obs : std_logic;
begin
    drv_a: entity work.sv_assign(strength)
        port map(d => a, q => net1);
    drv_b: entity work.sv_assign(strength)
        port map(d => b, q => net1);
    t1: entity work.sv_tran(strength)
        port map(a => net1, b => obs);

    process
    begin
        a <= '0'; b <= '0';
        wait for 1 ns;
        report "prop5 a=0 b=0: net1=" & std_logic'image(net1) &
               " obs=" & std_logic'image(obs);
        assert obs = '0'
            report "FAIL prop5: obs should be '0'" severity error;

        a <= '0'; b <= '1';
        wait for 1 ns;
        report "prop5 a=0 b=1: net1=" & std_logic'image(net1) &
               " obs=" & std_logic'image(obs);
        assert obs = 'X'
            report "FAIL prop5: obs should be 'X'" severity error;

        a <= '1'; b <= '1';
        wait for 1 ns;
        report "prop5 a=1 b=1: net1=" & std_logic'image(net1) &
               " obs=" & std_logic'image(obs);
        assert obs = '1'
            report "FAIL prop5: obs should be '1'" severity error;

        report "test_prop5 DONE";
        wait;
    end process;
end architecture;
