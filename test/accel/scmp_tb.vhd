library ieee; use ieee.std_logic_1164.all; use ieee.numeric_std.all;
entity scmp_tb is generic (N : integer := 200000); end entity;
architecture sim of scmp_tb is
  signal clk, rst_l : std_logic := '0';
  signal y : std_logic_vector(31 downto 0);
  signal chk : unsigned(31 downto 0) := (others => '0');
  signal running : boolean := true;
begin
  dut : entity work.scmp port map (clk=>clk, rst_l=>rst_l, y=>y);
  clk <= not clk after 5 ns when running else '0';
  process (clk) is begin
    if rising_edge(clk) then
      if rst_l='1' then
        -- xor-fold the DUT output (bit-sensitive checksum)
        chk <= (chk xor unsigned(y)) + to_unsigned(40503, 32);
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
