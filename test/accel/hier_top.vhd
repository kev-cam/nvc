-- 2-level hierarchy: an accumulator built from leaf_add + leaf_dff.
-- Elaborates to a T_BLOCK per child instance -> exercises the whole-subtree
-- accel path (the T_BLOCK -> Verilog instantiation/flatten that this tests).
--   reg_out_next = x + reg_out  ;  y = reg_out
-- With x held at 1, y counts clocks since reset.
library ieee;
use ieee.std_logic_1164.all;

entity hier_top is
  port (clk, rst_l : in  std_logic;
        x          : in  std_logic_vector(7 downto 0);
        y          : out std_logic_vector(7 downto 0));
end entity;

architecture rtl of hier_top is
  signal sum, reg_out : std_logic_vector(7 downto 0);
begin
  u_add : entity work.leaf_add
    port map (a => x, b => reg_out, s => sum);
  u_dff : entity work.leaf_dff
    port map (clk => clk, rst_l => rst_l, d => sum, q => reg_out);
  y <= reg_out;
end architecture;
