library ieee; use ieee.std_logic_1164.all; use ieee.numeric_std.all;
entity oobrst_tb is end entity;
architecture sim of oobrst_tb is
  signal clk : std_logic := '0';
  signal rst : std_logic := '1';
  signal d, q : std_logic_vector(7 downto 0) := (others => '0');
  signal y : natural := 0;
begin
  dut: entity work.oobrst port map (clk => clk, rst => rst, d => d, q => q);
  stim: process is
    variable acc : natural := 0;
  begin
    for i in 0 to 3 loop clk <= '0'; wait for 5 ns; clk <= '1'; wait for 5 ns; end loop;
    rst <= '0';
    for i in 1 to 200 loop
      d <= std_logic_vector(to_unsigned(i mod 251 mod 256, 8));
      clk <= '0'; wait for 5 ns; clk <= '1'; wait for 5 ns;
      acc := (acc + to_integer(unsigned(q))) mod 1000000007;
    end loop;
    report "FINAL Y=" & integer'image(acc);
    wait;
  end process;
end architecture;
