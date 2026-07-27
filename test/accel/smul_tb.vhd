-- Clocked driver for the signed-multiply DUT. Feeds operands that sweep through
-- NEGATIVE values (two's-complement high bit set) so the signed product must
-- sign-extend into the 32b accumulator. The accumulator's full 32 bits are
-- folded (xor) into a checksum so the exact high bits are load-bearing:
-- a zero-extended (buggy) product produces a different Y than the signed gold.
library ieee; use ieee.std_logic_1164.all; use ieee.numeric_std.all;
entity smul_tb is generic (N : integer := 4000); end entity;
architecture sim of smul_tb is
  signal clk, rst_l : std_logic := '0';
  signal a, b : std_logic_vector(7 downto 0) := (others => '0');
  signal y    : std_logic_vector(31 downto 0);
  signal chk  : unsigned(31 downto 0) := (others => '0');
  signal running : boolean := true;
  signal ta, tb : signed(7 downto 0) := (others => '0');
begin
  dut : entity work.smul port map (clk=>clk, rst_l=>rst_l, a=>a, b=>b, y=>y);
  clk <= not clk after 5 ns when running else '0';
  a <= std_logic_vector(ta);
  b <= std_logic_vector(tb);
  process (clk) is begin
    if rising_edge(clk) then
      if rst_l = '1' then
        -- Sweep operands across the full signed range incl negatives.
        ta  <= ta - 13;         -- steps down through negative values
        tb  <= tb + 7;
        -- Fold ALL 32 bits of the signed accumulator into the checksum so the
        -- sign-extended high bits matter (a truncated/zero-extended product diverges).
        chk <= (chk xor unsigned(y)) + 1;
      end if;
    end if;
  end process;
  stim : process is begin
    rst_l <= '0'; wait for 23 ns; rst_l <= '1';
    for i in 1 to N loop wait until rising_edge(clk); end loop;
    wait for 1 ns;
    report "Y=" & integer'image(to_integer(chk(30 downto 0))); report "PASSED";
    running <= false; wait;
  end process;
end architecture;
