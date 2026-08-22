library ieee; use ieee.std_logic_1164.all; use ieee.numeric_std.all;
entity cbp_tb is end entity;
architecture sim of cbp_tb is
  signal clk : std_logic := '0';
  signal src : unsigned(7 downto 0) := (others => '0');
  signal c1, c2, c3 : std_logic_vector(7 downto 0) := (others => '0');
  signal d, q : std_logic_vector(7 downto 0) := (others => '0');
begin
  dut: entity work.glitch port map (clk => clk, d => d, q => q);
  -- interp-side producer: flop + 3-stage comb delta chain into the
  -- chunk's input — d settles several deltas AFTER the posedge
  srcp: process (clk) is begin
    if rising_edge(clk) then src <= src + 1; end if;
  end process;
  c1 <= std_logic_vector(src);
  c2 <= c1;
  c3 <= c2;
  d  <= c3;
  stim: process is begin
    for i in 1 to 12 loop
      clk <= '0'; wait for 5 ns; clk <= '1'; wait for 5 ns;
    end loop;
    report "FINAL Y=" & integer'image(to_integer(unsigned(q)));
    wait;
  end process;
end architecture;
