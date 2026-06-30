-- Identical registered stages chained. With per-instance accel each stage is its
-- own chunk; the stages are IDENTICAL so they hash to ONE cached .so -> tests
-- that per-chunk state is correctly separated (a shared static state_t would
-- corrupt) and is the one-synth/N-chunk in-place-rebuild case. dpipe4 also is a
-- DUT-heavier benchmark (4 stages of compute/cycle) for the multi-chunk speed-up.
library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

entity pstage is
  port (clk  : in  std_logic;
        din  : in  std_logic_vector(31 downto 0);
        dout : out std_logic_vector(31 downto 0));
end entity;

architecture rtl of pstage is
  signal acc : unsigned(31 downto 0) := (others => '0');
begin
  process (clk) is
  begin
    if rising_edge(clk) then
      acc <= acc xor ((unsigned(din(27 downto 0)) & unsigned(din(31 downto 28)))
                       + acc + 1);
    end if;
  end process;
  dout <= std_logic_vector(acc);
end architecture;

library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

entity dpipe is
  port (clk    : in  std_logic;
        seed   : in  std_logic_vector(31 downto 0);
        result : out std_logic_vector(31 downto 0));
end entity;

-- four IDENTICAL pstage instances, distinct 32-bit signals (no array slice)
architecture rtl of dpipe is
  signal s1, s2, s3 : std_logic_vector(31 downto 0);
begin
  u1 : entity work.pstage port map (clk => clk, din => seed, dout => s1);
  u2 : entity work.pstage port map (clk => clk, din => s1,   dout => s2);
  u3 : entity work.pstage port map (clk => clk, din => s2,   dout => s3);
  u4 : entity work.pstage port map (clk => clk, din => s3,   dout => result);
end architecture;
