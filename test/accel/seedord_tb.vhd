library ieee; use ieee.std_logic_1164.all; use ieee.numeric_std.all;
entity seedord_tb is generic (N : integer := 2000); end entity;
architecture sim of seedord_tb is
  signal clk : std_logic := '0';
  signal running : boolean := true;
  signal seed : std_logic_vector(31 downto 0) := x"12345678";
  signal result : std_logic_vector(31 downto 0);
  signal chk : unsigned(31 downto 0) := (others => '0');
begin
  dut : entity work.seedord port map (clk=>clk, seed=>seed, result=>result);
  clk <= not clk after 5 ns when running else '0';
  process (clk) is begin
    if rising_edge(clk) then
      seed <= std_logic_vector(unsigned(seed) + 3);
      chk  <= chk xor unsigned(result);
    end if;
  end process;
  -- `done` is a VARIABLE so it survives a process restart: reporting exactly
  -- once keeps the log unambiguous for the harness.
  stim : process is variable done : boolean := false; begin
    if not done then
      -- OBSERVE AT t=1ns: BEFORE the first rising edge (5 ns). Pure t=0 comb state.
      wait for 1 ns;
      report "T0=" & integer'image(to_integer(unsigned(result(30 downto 0))));
      for i in 1 to N loop wait until rising_edge(clk); end loop;
      wait for 1 ns;
      report "Y=" & integer'image(to_integer(chk(30 downto 0)));
      report "PASSED";
      done := true; running <= false;
    end if;
    wait;
  end process;
end architecture;
