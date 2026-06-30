-- Two-stage pipeline for the multi-chunk / both-banked accel path: stage A's
-- registered output (a2b) feeds stage B's input. Each stage is individually
-- accel-installable (registered std_logic boundary, synthesizable), so the
-- accel layer should install TWO chunks and the A->B signal is the chunk-to-
-- chunk (both-banked) boundary. Gold (interpreted) is the bit-identical oracle.
library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

entity stagea is
  port (clk  : in  std_logic;
        din  : in  std_logic_vector(31 downto 0);
        dout : out std_logic_vector(31 downto 0));
end entity;

architecture rtl of stagea is
  signal acc : unsigned(31 downto 0) := (others => '0');
begin
  process (clk) is
  begin
    if rising_edge(clk) then
      acc <= acc + unsigned(din) + 1;
    end if;
  end process;
  dout <= std_logic_vector(acc);
end architecture;

library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

entity stageb is
  port (clk  : in  std_logic;
        din  : in  std_logic_vector(31 downto 0);
        dout : out std_logic_vector(31 downto 0));
end entity;

architecture rtl of stageb is
  signal acc : unsigned(31 downto 0) := (others => '0');
begin
  process (clk) is
  begin
    if rising_edge(clk) then
      acc <= acc xor ((unsigned(din(27 downto 0)) & unsigned(din(31 downto 28)))
                       + acc);
    end if;
  end process;
  dout <= std_logic_vector(acc);
end architecture;

library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

entity twochunk is
  port (clk    : in  std_logic;
        seed   : in  std_logic_vector(31 downto 0);
        result : out std_logic_vector(31 downto 0));
end entity;

architecture rtl of twochunk is
  signal a2b : std_logic_vector(31 downto 0);   -- the chunk-to-chunk boundary
begin
  ua : entity work.stagea port map (clk => clk, din => seed, dout => a2b);
  ub : entity work.stageb port map (clk => clk, din => a2b, dout => result);
end architecture;
