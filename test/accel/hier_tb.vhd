-- Drives the accumulator with x=1 for 30 clocks and reports the result.
-- The runner compares the reported Y from a non-accel run vs an --accel run;
-- they must match (accel must not change results).
library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

entity hier_tb is end entity;

architecture sim of hier_tb is
  signal clk     : std_logic := '0';
  signal rst_l   : std_logic := '0';
  signal x       : std_logic_vector(7 downto 0) := x"01";
  signal y       : std_logic_vector(7 downto 0);
  signal running : boolean := true;
begin
  dut : entity work.hier_top
    port map (clk => clk, rst_l => rst_l, x => x, y => y);

  clk <= not clk after 5 ns when running else '0';

  stim : process is
  begin
    rst_l <= '0';
    wait for 23 ns;
    rst_l <= '1';
    for i in 1 to 30 loop
      wait until rising_edge(clk);
    end loop;
    wait for 1 ns;
    report "Y=" & integer'image(to_integer(unsigned(y)));
    report "PASSED";
    running <= false;
    wait;
  end process;
end architecture;
