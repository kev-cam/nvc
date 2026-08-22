library ieee; use ieee.std_logic_1164.all; use ieee.numeric_std.all;
entity mclk_tb is generic (N : integer := 100000); end entity;
architecture sim of mclk_tb is
  signal clk, clk2 : std_logic := '0';
  signal en : std_logic := '0';
  signal running : boolean := true;
  signal din : std_logic_vector(31 downto 0) := (others => '0');
  signal dout : std_logic_vector(31 downto 0);
  signal chk : unsigned(31 downto 0) := (others => '0');
begin
  dut : entity work.mclk port map (clk => clk, clk2 => clk2, din => din, dout => dout);
  clk  <= not clk after 5 ns when running else '0';
  clk2 <= clk and en;                       -- gated derived clock (lags clk by a delta)
  process (clk) is begin
    if rising_edge(clk) then
      en  <= not en;                        -- clk2 ticks every other cycle
      din <= std_logic_vector(unsigned(din) + 3);
      chk <= chk xor unsigned(dout);
    end if;
  end process;
  stim : process is begin
    for i in 1 to N loop wait until rising_edge(clk); end loop;
    wait for 1 ns;
    report "Y=" & integer'image(to_integer(chk(30 downto 0))); report "PASSED";
    running <= false; wait;
  end process;
end architecture;
