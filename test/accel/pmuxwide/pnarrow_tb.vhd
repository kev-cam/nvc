library ieee; use ieee.std_logic_1164.all; use ieee.numeric_std.all;
entity pnarrow_tb is generic (N : integer := 100000); end entity;
architecture sim of pnarrow_tb is
  signal clk : std_logic := '0';
  signal running : boolean := true;
  signal sel : std_logic_vector(7 downto 0)  := (others => '0');
  signal din : std_logic_vector(31 downto 0) := (others => '0');
  signal dout : std_logic_vector(31 downto 0);
  signal chk : unsigned(31 downto 0) := (others => '0');
  signal cnt : unsigned(15 downto 0) := (others => '0');
begin
  dut : entity work.pnarrow port map (clk => clk, sel => sel, din => din, dout => dout);
  clk <= not clk after 5 ns when running else '0';
  process (clk) is begin
    if rising_edge(clk) then
      cnt <= cnt + 1;
      sel <= std_logic_vector(unsigned(sel) + 37);
      -- drive din across sign boundaries (make top bits toggle so signed sext matters)
      din <= std_logic_vector(unsigned(din) + resize(unsigned(sel), 32) + x"9E3779B1");
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
