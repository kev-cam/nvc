library ieee; use ieee.std_logic_1164.all; use ieee.numeric_std.all;
entity glitch_tb is end entity;
architecture sim of glitch_tb is
  signal clk : std_logic := '0';
  signal d, q : std_logic_vector(7 downto 0) := x"01";
begin
  dut: entity work.glitch port map (clk => clk, d => d, q => q);
  stim: process is
  begin
    for i in 1 to 50 loop
      clk <= '0'; wait for 5 ns;
      -- rise-fall-RISE within one instant every 8th cycle
      clk <= '1';
      if (i mod 8) = 0 then
        wait for 0 ns; clk <= '0'; wait for 0 ns; clk <= '1';
      end if;
      wait for 5 ns;
    end loop;
    report "FINAL Y=" & integer'image(to_integer(unsigned(q)));
    wait;
  end process;
end architecture;
