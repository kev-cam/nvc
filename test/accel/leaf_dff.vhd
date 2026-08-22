-- Clocked leaf with sync reset (the form vhdl2vlog's clock_of recognizes:
-- a wrapping `if rising_edge(clk)`).
library ieee;
use ieee.std_logic_1164.all;

entity leaf_dff is
  port (clk, rst_l : in  std_logic;
        d          : in  std_logic_vector(7 downto 0);
        q          : out std_logic_vector(7 downto 0));
end entity;

architecture rtl of leaf_dff is
begin
  process (clk) is
  begin
    if rising_edge(clk) then
      if rst_l = '0' then
        q <= (others => '0');
      else
        q <= d;
      end if;
    end if;
  end process;
end architecture;
