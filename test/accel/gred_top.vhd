-- Instantiates the generic AND-reducer at TWO widths whose results diverge if
-- the two instances share one (wrong-width) module:
--   u_w8: W=8, a = x"07" (00000111)  -> real AND = 0  (upper bits clear)
--   u_w3: W=3, a = "111"             -> real AND = 1
-- Correct (each its own width):    y8=0, y3=1  -> acc += 1 / cycle.
-- Deduped to the 8-bit module:     y3 sees 00000111 -> AND=0 -> acc += 0.
-- Deduped to the 3-bit module:     y8 sees low 3 = 111 -> AND=1 -> acc += 2.
-- So a dedup bug shows as Y != 30 (either 0 or 60).
library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

entity gred_top is
  port (clk   : in  std_logic;
        rst_l : in  std_logic;
        y     : out std_logic_vector(7 downto 0));
end entity;

architecture rtl of gred_top is
  signal y8, y3 : std_logic;
  signal acc    : unsigned(7 downto 0) := (others => '0');
begin
  u_w8 : entity work.greduce
    generic map (W => 8)
    port map (a => x"07", y => y8);

  u_w3 : entity work.greduce
    generic map (W => 3)
    port map (a => "111", y => y3);

  process (clk) is
  begin
    if rising_edge(clk) then
      if rst_l = '0' then
        acc <= (others => '0');
      elsif (y8 = '1') and (y3 = '1') then
        acc <= acc + 2;
      elsif (y8 = '1') or (y3 = '1') then
        acc <= acc + 1;
      end if;
    end if;
  end process;

  y <= std_logic_vector(acc);
end architecture;
