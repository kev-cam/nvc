-- Cross-chunk combinational RING in 3D-Logic (l3d), not std_logic.
--
-- WHY THIS EXISTS.  An earlier probe of the same shape was written in
-- std_logic, which answers a question we do not care about: std_logic's 'U' is
-- a true fixpoint under inversion (not 'U' = 'U'), so the ring settles in a
-- couple of deltas and the run completes.  Our engine is supposed to be running
-- 3D-Logic, where the encoding is value/driven/uncertain (bit0/bit1/bit2) and
-- the NOT semantics are "copy strength + certainty, FLIP THE VALUE BIT".
--
-- That difference is the whole point of this probe.  Under l3d:
--     not(L3D_U  = 7 = 111) = 110 = L3D_0X
--     not(L3D_0X = 6 = 110) = 111 = L3D_1X
-- Both operands stay uncertain, but the CODE alternates 6 <-> 7, and a logic3d
-- signal is a plain `natural range 0 to 7`, so a code change IS an event.  So
-- an l3d ring may well NOT have the fixpoint the std_logic version had --
-- keeping the uncertain plane is not by itself enough to make a loop converge,
-- because the value bit under uncertainty still carries a best-guess value and
-- inversion still flips it.
--
-- This probe is written to MEASURE that, not to confirm it.  Three outcomes are
-- meaningful and all are interesting:
--   * converges to a stable uncertain code  -> uncertain IS a fixpoint here
--   * oscillates to --stop-delta (default 10000, src/option.c:137) -> it is not
--   * interp and accel DIFFER -> the 2-state value plane changed termination
--
-- Each stage keeps a register so the subtree stays accel-eligible; the
-- registers never change value, so any delta activity is purely the ring.
-- Run per-instance (NVC_ACCEL_PER_INSTANCE=1) to get a real cross-chunk edge.

library ieee; use ieee.std_logic_1164.all;
library sv2vhdl; use sv2vhdl.logic3d_types_pkg.all;

entity lo_a is
  port (clk  : in  std_logic;
        ain  : in  logic3d_vector(31 downto 0);
        aout : out logic3d_vector(31 downto 0));
end entity;

architecture rtl of lo_a is
  signal r : logic3d_vector(31 downto 0) := (others => L3D_0);
begin
  process (clk) is
  begin
    if rising_edge(clk) then r <= r; end if;
  end process;
  aout <= l3d_xor(l3d_not(ain), r);
end architecture;

library ieee; use ieee.std_logic_1164.all;
library sv2vhdl; use sv2vhdl.logic3d_types_pkg.all;

entity lo_b is
  port (clk  : in  std_logic;
        bin  : in  logic3d_vector(31 downto 0);
        bout : out logic3d_vector(31 downto 0));
end entity;

architecture rtl of lo_b is
  signal r : logic3d_vector(31 downto 0) := (others => L3D_0);
begin
  process (clk) is
  begin
    if rising_edge(clk) then r <= r; end if;
  end process;
  bout <= l3d_xor(bin, r);
end architecture;

library ieee; use ieee.std_logic_1164.all;
library sv2vhdl; use sv2vhdl.logic3d_types_pkg.all;

entity l3dosc is
  port (clk    : in  std_logic;
        seed   : in  logic3d_vector(31 downto 0);
        result : out logic3d_vector(31 downto 0));
end entity;

architecture rtl of l3dosc is
  -- Start the ring UNCERTAIN.  Note a logic3d signal left to its own default
  -- would be 0 = L3D_0Z (undriven but CERTAIN 0), which is not what we want to
  -- probe -- unlike std_logic, whose default 'U' is already uncertain.
  signal a2b, b2a : logic3d_vector(31 downto 0) := (others => L3D_U);
begin
  ua : entity work.lo_a port map (clk => clk, ain => b2a, aout => a2b);
  ub : entity work.lo_b port map (clk => clk, bin => a2b, bout => b2a);
  result <= l3d_xor(a2b, seed);
end architecture;
