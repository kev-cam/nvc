library ieee; use ieee.std_logic_1164.all; use ieee.numeric_std.all;
entity areset_tb is end entity;
architecture sim of areset_tb is
  signal clk:std_logic:='0'; signal arst:std_logic:='0'; signal en:std_logic:='0';
  signal running:boolean:=true; signal din:std_logic_vector(31 downto 0):=(others=>'0');
  signal y:std_logic_vector(31 downto 0); signal chk:unsigned(31 downto 0):=(others=>'0');
begin
  dut:entity work.areset port map(clk=>clk,arst=>arst,en=>en,din=>din,y=>y);
  clk<=not clk after 5 ns when running else '0';
  process(clk) is begin if rising_edge(clk) then en<=not en; din<=std_logic_vector(unsigned(din)+7); chk<=chk xor unsigned(y); end if; end process;
  stim:process is begin
    for i in 1 to 100000 loop wait until rising_edge(clk);
      if (i mod 64)=5 then arst<='1'; elsif (i mod 64)=7 then arst<='0'; end if;
    end loop; wait for 1 ns;
    report "Y=" & integer'image(to_integer(chk(30 downto 0))); report "PASSED"; running<=false; wait;
  end process;
end architecture;
