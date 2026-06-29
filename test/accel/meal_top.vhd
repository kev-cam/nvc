-- Mealy chunk: a registered counter PLUS a COMBINATIONAL boundary output that
-- depends on the input x in the SAME cycle (y = acc + x). The combinational path
-- x -> y must re-settle when x changes mid-cycle. A once-per-clock-edge bridge
-- (read x at the edge, eval once) leaves y stale; the comb-settling bridge
-- re-evaluates y whenever x changes, matching the interpreted reference.
library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

entity meal_top is
  port (clk, rst_l : in  std_logic;
        x          : in  std_logic_vector(7 downto 0);
        y          : out std_logic_vector(7 downto 0));
end entity;

architecture rtl of meal_top is
  signal acc : unsigned(7 downto 0) := (others => '0');
begin
  process (clk) is
  begin
    if rising_edge(clk) then
      if rst_l = '0' then acc <= (others => '0');
      else acc <= acc + 1; end if;
    end if;
  end process;

  -- combinational (Mealy) output: depends on x THIS cycle
  y <= std_logic_vector(acc + unsigned(x));
end architecture;
