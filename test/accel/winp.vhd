-- Wide-INPUT accel benchmark. wchurn/wwide are wide on the OUTPUT side (their
-- only inputs are clk and rst_l, 1 bit each), so they cannot measure what the
-- boundary input scan costs. This DUT takes 256 input bits and folds all of
-- them into its state, so every scan that sees a change repacks 256 elements —
-- the loop the X/Z detection lives in.
library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

entity winp is
  port (clk, rst_l : in std_logic;
        d          : in std_logic_vector(255 downto 0);
        y          : out std_logic_vector(31 downto 0));
end entity;

architecture rtl of winp is
  signal acc : unsigned(31 downto 0) := (others => '0');
  signal cnt : unsigned(31 downto 0) := (others => '0');
begin
  process (clk) is
    variable f : unsigned(31 downto 0);
  begin
    if rising_edge(clk) then
      if rst_l = '0' then
        acc <= (others => '0');
        cnt <= (others => '0');
      else
        f := unsigned(d(31 downto 0))    xor unsigned(d(63 downto 32))
         xor unsigned(d(95 downto 64))   xor unsigned(d(127 downto 96))
         xor unsigned(d(159 downto 128)) xor unsigned(d(191 downto 160))
         xor unsigned(d(223 downto 192)) xor unsigned(d(255 downto 224));
        cnt <= cnt + 1;
        acc <= (acc(26 downto 0) & acc(31 downto 27)) + f + cnt;
      end if;
    end if;
  end process;

  y <= std_logic_vector(acc xor cnt);
end architecture;
