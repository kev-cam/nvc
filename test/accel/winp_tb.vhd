-- Drives winp's 256-bit input with a cheap per-cycle change: one element
-- toggles, which is enough to fail the bridge's raw memcmp and force a FULL
-- 256-element repack every cycle. The stimulus cost is therefore constant
-- across the A/B, and the delta measures the repack loop itself.
library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

entity winp_tb is
  generic (N : integer := 200000);
end entity;

architecture sim of winp_tb is
  signal clk, rst_l : std_logic := '0';
  signal d          : std_logic_vector(255 downto 0) := (others => '0');
  signal tog        : std_logic := '0';
  signal y          : std_logic_vector(31 downto 0);
  signal chk        : unsigned(31 downto 0) := (others => '0');
  signal running    : boolean := true;
begin
  dut : entity work.winp
    port map (clk => clk, rst_l => rst_l, d => d, y => y);

  clk <= not clk after 5 ns when running else '0';

  -- a fixed pattern with one toggling bit
  d <= x"0F1E2D3C4B5A69788796A5B4C3D2E1F0" & x"1122334455667788AABBCCDDEEFF0011"
       when tog = '0' else
       x"0F1E2D3C4B5A69788796A5B4C3D2E1F1" & x"1122334455667788AABBCCDDEEFF0011";

  process (clk) is
  begin
    if rising_edge(clk) then
      tog <= not tog;
      if rst_l = '1' then chk <= chk xor unsigned(y); end if;
    end if;
  end process;

  stim : process is
  begin
    rst_l <= '0';
    wait for 23 ns;
    rst_l <= '1';
    for i in 1 to N loop
      wait until rising_edge(clk);
    end loop;
    wait for 1 ns;
    report "Y=" & integer'image(to_integer(chk(30 downto 0)));
    report "PASSED";
    running <= false;
    wait;
  end process;
end architecture;
