-- Two-clock design mimicking VeeR's gated-clock structure: ra on the main clk,
-- rb on a gated/derived clock clk2 (= clk & en, an INPUT generated combinationally
-- so it lags clk by a delta — the exact delta-race the bridge must edge-detect).
-- rb samples ra (cross-clock), so correct pre-edge sampling is load-bearing.
library ieee; use ieee.std_logic_1164.all; use ieee.numeric_std.all;
entity mclk is
  port (clk, clk2 : in std_logic;
        din  : in  std_logic_vector(31 downto 0);
        dout : out std_logic_vector(31 downto 0));
end entity;
architecture rtl of mclk is
  signal ra : unsigned(31 downto 0) := (others => '0');
  signal rb : unsigned(31 downto 0) := (others => '0');
begin
  process (clk)  is begin if rising_edge(clk)  then ra <= ra + unsigned(din) + 1; end if; end process;
  process (clk2) is begin if rising_edge(clk2) then rb <= (rb xor ra) + 7;       end if; end process;
  dout <= std_logic_vector(ra xor rb);
end architecture;
