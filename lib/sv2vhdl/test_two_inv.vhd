-- Two-inverter multi-driver test
-- SV equivalent: not g1(y, a); not g2(y, b);
-- Two sv_not instances drive the same resolved std_logic net.
-- Expected resolution:
--   a=0 b=0 → not(0)=1, not(0)=1 → y='1'
--   a=1 b=1 → not(1)=0, not(1)=0 → y='0'
--   a=0 b=1 → not(0)=1, not(1)=0 → y='X'
--   a=1 b=0 → not(1)=0, not(0)=1 → y='X'

library ieee;
use ieee.std_logic_1164.all;

entity two_inv is
    port (
        a : in  std_logic;
        b : in  std_logic;
        y : out std_logic
    );
end entity two_inv;

architecture rtl of two_inv is
    signal y_net : std_logic;
begin
    inv1: entity work.sv_not
        generic map (n => 1)
        port map (y(0) => y_net, a => a);

    inv2: entity work.sv_not
        generic map (n => 1)
        port map (y(0) => y_net, a => b);

    y <= y_net;
end architecture rtl;

---------------------------------------------------------------------------
-- Testbench
---------------------------------------------------------------------------
library ieee;
use ieee.std_logic_1164.all;

entity test_two_inv is
end entity test_two_inv;

architecture test of test_two_inv is
    signal a, b, y : std_logic;
begin

    dut: entity work.two_inv
        port map (a => a, b => b, y => y);

    process
    begin
        -- Pattern 1: a=0 b=0 → y='1'
        a <= '0'; b <= '0';
        wait for 1 ns;
        report "a=0 b=0: y=" & std_logic'image(y);

        -- Pattern 2: a=1 b=1 → y='0'
        a <= '1'; b <= '1';
        wait for 1 ns;
        report "a=1 b=1: y=" & std_logic'image(y);

        -- Pattern 3: a=0 b=1 → y='X'
        a <= '0'; b <= '1';
        wait for 1 ns;
        report "a=0 b=1: y=" & std_logic'image(y);

        -- Pattern 4: a=1 b=0 → y='X'
        a <= '1'; b <= '0';
        wait for 1 ns;
        report "a=1 b=0: y=" & std_logic'image(y);

        report "DONE";
        wait;
    end process;

end architecture test;
