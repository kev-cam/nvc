-- Same combinational-boundary chain as cchain, but the CONSUMER instance (ub) is
-- instantiated BEFORE the producer (ua). If accel installs/dispatches chunks in
-- instantiation order, the consumer runs before the producer -> reads a stale
-- cross-chunk combinational value -> divergence (the VeeR failure mode).
library ieee; use ieee.std_logic_1164.all; use ieee.numeric_std.all;
entity cchain_rev is
  port (clk : in std_logic; seed : in std_logic_vector(31 downto 0); result : out std_logic_vector(31 downto 0));
end entity;
architecture rtl of cchain_rev is
  signal amid : std_logic_vector(31 downto 0);
begin
  ub : entity work.cstage_b port map (clk => clk, amid => amid, bout => result);  -- consumer FIRST
  ua : entity work.cstage_a port map (clk => clk, xin => seed, amid => amid);     -- producer SECOND
end architecture;
