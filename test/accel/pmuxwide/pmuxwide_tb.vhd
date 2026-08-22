-- Clocked TB for the pmux probe. Drives sel through ALL branch ranges so the
-- priority chain fires every branch, and varies din. Folds dout into a running
-- checksum, reports Y=<int>.
library ieee; use ieee.std_logic_1164.all; use ieee.numeric_std.all;
entity pmuxwide_tb is generic (N : integer := 100000); end entity;
architecture sim of pmuxwide_tb is
  signal clk : std_logic := '0';
  signal running : boolean := true;
  signal sel : std_logic_vector(7 downto 0)  := (others => '0');
  signal din : std_logic_vector(71 downto 0) := (others => '0');
  signal dout : std_logic_vector(71 downto 0);
  signal chk : unsigned(71 downto 0) := (others => '0');
  signal cnt : unsigned(15 downto 0) := (others => '0');
begin
  dut : entity work.pmuxwide port map (clk => clk, sel => sel, din => din, dout => dout);
  clk <= not clk after 5 ns when running else '0';
  process (clk) is
    variable nsel : unsigned(7 downto 0);
  begin
    if rising_edge(clk) then
      cnt <= cnt + 1;
      -- sweep sel across all branch ranges with a multiplier so every branch
      -- (0..8 + default) is exercised repeatedly
      nsel := unsigned(sel) + 37;
      sel <= std_logic_vector(nsel);
      -- vary din widely
      din <= std_logic_vector(unsigned(din) + resize(unsigned(sel), 72) + 12345);
      -- fold dout into checksum, bit sensitive
      chk <= (chk xor unsigned(dout)) + resize(cnt, 72);
    end if;
  end process;
  stim : process is begin
    for i in 1 to N loop wait until rising_edge(clk); end loop;
    wait for 1 ns;
    report "Y=" & integer'image(to_integer(chk(30 downto 0)));
    report "PASSED";
    running <= false; wait;
  end process;
end architecture;
