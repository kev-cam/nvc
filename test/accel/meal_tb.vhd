-- Drives the Mealy chunk's input x from a clock-registered tb signal r, so x
-- changes a delta AFTER each clock edge (not at the edge the chunk reads). A
-- registered checksum accumulates y. With correct combinational settling the
-- accelerated chunk matches the interpreted gold; a once-per-edge bridge that
-- reads the stale x produces a different checksum.
library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

entity meal_tb is end entity;

architecture sim of meal_tb is
  signal clk, rst_l : std_logic := '0';
  signal x          : std_logic_vector(7 downto 0);
  signal y          : std_logic_vector(7 downto 0);
  signal r          : unsigned(7 downto 0) := (others => '0');
  signal chk        : unsigned(7 downto 0) := (others => '0');
  signal running    : boolean := true;
begin
  dut : entity work.meal_top
    port map (clk => clk, rst_l => rst_l, x => x, y => y);

  clk <= not clk after 5 ns when running else '0';

  -- r registered: a new value every cycle, so x = r changes mid-cycle.
  process (clk) is
  begin
    if rising_edge(clk) then
      if rst_l = '0' then r <= (others => '0');
      else r <= r + 7; end if;
    end if;
  end process;

  x <= std_logic_vector(r);   -- combinational input to the chunk

  process (clk) is
  begin
    if rising_edge(clk) then
      if rst_l = '1' then chk <= chk + unsigned(y); end if;
    end if;
  end process;

  stim : process is
  begin
    rst_l <= '0';
    wait for 23 ns;
    rst_l <= '1';
    for i in 1 to 30 loop
      wait until rising_edge(clk);
    end loop;
    wait for 1 ns;
    report "Y=" & integer'image(to_integer(chk));
    report "PASSED";
    running <= false;
    wait;
  end process;
end architecture;
