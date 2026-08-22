-- Drives gen_top with constant inputs for 30 clocks and reports the result.
-- The runner compares the reported Y from a non-accel run vs an --accel run;
-- they must match (accel must not change results).
library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

entity gen_tb is end entity;

architecture sim of gen_tb is
  signal clk     : std_logic := '0';
  signal rst_l   : std_logic := '0';
  signal a8      : std_logic_vector(7 downto 0) := x"01";
  signal a4      : std_logic_vector(3 downto 0) := x"1";
  signal y       : std_logic_vector(7 downto 0);
  signal running : boolean := true;
begin
  dut : entity work.gen_top
    port map (clk => clk, rst_l => rst_l, a8 => a8, a4 => a4, y => y);

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
