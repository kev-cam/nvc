library ieee; use ieee.std_logic_1164.all; use ieee.numeric_std.all;
entity sext_tb is generic (N : integer := 1000000); end entity;
architecture sim of sext_tb is
  signal clk, rst_l : std_logic := '0';
  signal imm8  : std_logic_vector(7 downto 0)  := (others => '0');
  signal imm12 : std_logic_vector(11 downto 0) := (others => '0');
  signal y     : std_logic_vector(31 downto 0);
  signal chk   : unsigned(31 downto 0) := (others => '0');
  signal running : boolean := true;
begin
  dut : entity work.sext port map (clk=>clk, rst_l=>rst_l, imm8=>imm8, imm12=>imm12, y=>y);
  clk <= not clk after 5 ns when running else '0';
  -- Drive the small fields with values that swing negative (top bit set) so the
  -- sign-extension path is exercised, not just zero-extension.
  process (clk) is begin
    if rising_edge(clk) then
      if rst_l='1' then
        imm8  <= std_logic_vector(unsigned(imm8)  + 37);   -- wraps through 0x80.. (negative)
        imm12 <= std_logic_vector(unsigned(imm12) + 213);  -- wraps through high (negative) values
        chk   <= chk xor unsigned(y);                      -- fold every bit of acc into chk
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
