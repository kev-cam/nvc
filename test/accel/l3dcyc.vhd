-- Cross-chunk combinational CYCLE in 3D-Logic (l3d), enable-gated.
--
-- Companion to l3dosc.  Same dependency shape -- ua and ub wired a->b AND b->a,
-- so the chunk dependency graph has a CYCLE and no topological order exists --
-- but here the loop is deliberately CUT at t=0 and only goes live later, so the
-- two halves of the question are separated:
--
--   * t=0 .. 4 edges : the loop input is l3d_and'ed with an enable that is
--     L3D_0.  In l3d, `0 and anything` is a certain 0 regardless of the other
--     operand's certainty, so the cycle is logically broken and BOTH the
--     interpreter and the 2-state accel value plane must reach the same
--     definite fixpoint.  Any divergence here is a real accel bug, not a
--     property of the loop.
--   * after 4 edges : enables go L3D_1, the loop is live with definite
--     operands, and l3d_or is monotone -- so it should still converge, in both
--     engines, in a bounded number of deltas (~logic depth, not thousands).
--
-- So this probe answers "does an accel chunk handle a dependency cycle at all",
-- while l3dosc answers "does uncertainty give a loop a fixpoint".  Written in
-- l3d rather than std_logic because that is the representation the engine is
-- actually supposed to be running -- a std_logic version tests std_logic's 'U'
-- fixpoint, which is not our semantics.
--
-- Run per-instance (NVC_ACCEL_PER_INSTANCE=1) to get a real cross-chunk edge;
-- whole-subtree mode folds both stages into one chunk and the cycle vanishes.

library ieee; use ieee.std_logic_1164.all; use ieee.numeric_std.all;
library sv2vhdl; use sv2vhdl.logic3d_types_pkg.all;

entity lc_a is
  port (clk  : in  std_logic;
        ain  : in  logic3d_vector(31 downto 0);
        aout : out logic3d_vector(31 downto 0));
end entity;

architecture rtl of lc_a is
  signal en  : logic3d := L3D_0;
  signal cnt : unsigned(3 downto 0) := (others => '0');
  signal ma  : logic3d_vector(31 downto 0) := (3 downto 0 => L3D_1, others => L3D_0);
  signal env : logic3d_vector(31 downto 0);
begin
  process (clk) is
  begin
    if rising_edge(clk) then
      cnt <= cnt + 1;
      if cnt = 3 then en <= L3D_1; end if;
      ma <= ma(30 downto 0) & ma(31);          -- rotate, keeps the chunk live
    end if;
  end process;
  env  <= (others => en);
  aout <= l3d_or(l3d_and(ain, env), ma);
end architecture;

library ieee; use ieee.std_logic_1164.all; use ieee.numeric_std.all;
library sv2vhdl; use sv2vhdl.logic3d_types_pkg.all;

entity lc_b is
  port (clk  : in  std_logic;
        bin  : in  logic3d_vector(31 downto 0);
        bout : out logic3d_vector(31 downto 0));
end entity;

architecture rtl of lc_b is
  signal en  : logic3d := L3D_0;
  signal cnt : unsigned(3 downto 0) := (others => '0');
  signal mb  : logic3d_vector(31 downto 0) := (23 downto 20 => L3D_1, others => L3D_0);
  signal env : logic3d_vector(31 downto 0);
begin
  process (clk) is
  begin
    if rising_edge(clk) then
      cnt <= cnt + 1;
      if cnt = 3 then en <= L3D_1; end if;
      mb <= mb(30 downto 0) & mb(31);
    end if;
  end process;
  env  <= (others => en);
  bout <= l3d_or(l3d_and(bin, env), mb);
end architecture;

library ieee; use ieee.std_logic_1164.all;
library sv2vhdl; use sv2vhdl.logic3d_types_pkg.all;

entity l3dcyc is
  port (clk    : in  std_logic;
        seed   : in  logic3d_vector(31 downto 0);
        result : out logic3d_vector(31 downto 0));
end entity;

architecture rtl of l3dcyc is
  signal a2b, b2a : logic3d_vector(31 downto 0);
begin
  ua : entity work.lc_a port map (clk => clk, ain => b2a, aout => a2b);
  ub : entity work.lc_b port map (clk => clk, bin => a2b, bout => b2a);
  result <= l3d_xor(l3d_xor(a2b, b2a), seed);
end architecture;
