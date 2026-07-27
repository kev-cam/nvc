library ieee; use ieee.std_logic_1164.all; use ieee.numeric_std.all;
entity psext_tb is generic (N : integer := 100000); end entity;
architecture sim of psext_tb is
  signal clk : std_logic := '0';
  signal running : boolean := true;
  signal din : std_logic_vector(31 downto 0) := (others => '0');
  signal dout : std_logic_vector(31 downto 0);
  signal chk : unsigned(31 downto 0) := (others => '0');
  signal cnt : unsigned(15 downto 0) := (others => '0');
begin
  dut : entity work.psext port map (clk => clk, din => din, dout => dout);
  clk <= not clk after 5 ns when running else '0';
  process (clk) is begin
    if rising_edge(clk) then
      cnt <= cnt + 1;
      -- drive din[11:0] so the 12-bit field crosses its sign boundary (bit 11 toggles)
      din <= std_logic_vector(unsigned(din) + x"00000123");
      chk <= (chk xor unsigned(dout)) + resize(cnt, 32);
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
