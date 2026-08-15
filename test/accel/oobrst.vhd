library ieee; use ieee.std_logic_1164.all; use ieee.numeric_std.all;
entity oobrst is
  port (clk, rst : in std_logic;
        d : in std_logic_vector(7 downto 0);
        q : out std_logic_vector(7 downto 0));
end entity;
architecture rtl of oobrst is
  signal acc : unsigned(7 downto 0) := (others => '0');
begin
  process (clk) is begin
    if rising_edge(clk) then
      if rst = '1' then acc <= (others => '0');
      else acc <= acc + unsigned(d); end if;
    end if;
  end process;
  q <= std_logic_vector(acc);
end architecture;
