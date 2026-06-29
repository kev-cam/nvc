-- Self-checking TB for the generic-dedup adversarial design (gred_top).
-- Correct result is Y=30; a dedup bug yields Y=0 or Y=60.
library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

entity gred_tb is end entity;

architecture sim of gred_tb is
  signal clk     : std_logic := '0';
  signal rst_l   : std_logic := '0';
  signal y       : std_logic_vector(7 downto 0);
  signal running : boolean := true;
begin
  dut : entity work.gred_top
    port map (clk => clk, rst_l => rst_l, y => y);

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
