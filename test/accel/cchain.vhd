-- Repro for the VeeR chunk-boundary failure: two clocked chunks coupled by a
-- COMBINATIONAL boundary signal. cstage_a drives `amid` combinationally from its
-- own register + input; cstage_b samples `amid` at the clock edge. Under
-- per-instance accel these become TWO separate native chunks exchanging `amid`
-- across a native<->native bridge each cycle — the case VeeR has and the toys
-- (twochunk = one model; pipe = registered boundary) do NOT. If chunk dispatch
-- is not dependency-ordered (or cross-chunk combinational NBA is mishandled),
-- the accel result diverges from the interpreted gold.
library ieee; use ieee.std_logic_1164.all; use ieee.numeric_std.all;
entity cstage_a is
  port (clk  : in  std_logic;
        xin  : in  std_logic_vector(31 downto 0);
        amid : out std_logic_vector(31 downto 0));
end entity;
architecture rtl of cstage_a is
  signal ra : unsigned(31 downto 0) := (others => '0');
begin
  process (clk) is begin
    if rising_edge(clk) then ra <= ra + unsigned(xin) + 1; end if;
  end process;
  amid <= std_logic_vector(ra xor unsigned(xin));   -- COMBINATIONAL cross-chunk output
end architecture;

library ieee; use ieee.std_logic_1164.all; use ieee.numeric_std.all;
entity cstage_b is
  port (clk  : in  std_logic;
        amid : in  std_logic_vector(31 downto 0);
        bout : out std_logic_vector(31 downto 0));
end entity;
architecture rtl of cstage_b is
  signal rb : unsigned(31 downto 0) := (others => '0');
begin
  process (clk) is begin
    if rising_edge(clk) then rb <= (rb xor unsigned(amid)) + 1; end if;  -- samples amid at edge
  end process;
  bout <= std_logic_vector(rb);
end architecture;

library ieee; use ieee.std_logic_1164.all; use ieee.numeric_std.all;
entity cchain is
  port (clk    : in  std_logic;
        seed   : in  std_logic_vector(31 downto 0);
        result : out std_logic_vector(31 downto 0));
end entity;
architecture rtl of cchain is
  signal amid : std_logic_vector(31 downto 0);
begin
  ua : entity work.cstage_a port map (clk => clk, xin => seed, amid => amid);
  ub : entity work.cstage_b port map (clk => clk, amid => amid, bout => result);
end architecture;
