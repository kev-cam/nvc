library ieee; use ieee.std_logic_1164.all; use ieee.numeric_std.all;
use std.textio.all;
entity c3reg_dbg_tb is generic (N : integer := 8); end entity;
architecture sim of c3reg_dbg_tb is
  signal clk     : std_logic := '0';
  signal running : boolean := true;
  signal seed    : std_logic_vector(31 downto 0) := (others => '0');
  signal result  : std_logic_vector(31 downto 0);
  signal chk     : unsigned(31 downto 0) := (others => '0');
begin
  dut : entity work.c3reg port map (clk => clk, seed => seed, result => result);
  clk <= not clk after 5 ns when running else '0';
  process (clk) is begin
    if rising_edge(clk) then
      seed <= std_logic_vector(unsigned(seed) + 3);
      chk  <= chk xor unsigned(result);
    end if;
  end process;
  stim : process is
    variable l : line;
  begin
    for i in 1 to N loop
      wait until rising_edge(clk);
      wait for 1 ns;
      write(l, string'("cyc=")); write(l, i);
      write(l, string'(" seed=")); write(l, to_integer(unsigned(seed)));
      write(l, string'(" result=")); write(l, to_integer(unsigned(result)));
      write(l, string'(" chk=")); write(l, to_integer(chk));
      writeline(output, l);
    end loop;
    report "Y=" & integer'image(to_integer(chk(30 downto 0)));
    report "PASSED";
    running <= false; wait;
  end process;
end architecture;
