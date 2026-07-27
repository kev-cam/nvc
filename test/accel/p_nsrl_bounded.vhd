library ieee; use ieee.std_logic_1164.all; use ieee.numeric_std.all;
entity p_nsrl_bounded is port (clk, rst_l : in std_logic; y : out std_logic_vector(31 downto 0)); end entity;
architecture rtl of p_nsrl_bounded is
  signal acc   : unsigned(31 downto 0)  := (others => '0');
  signal wacc  : unsigned(99 downto 0)  := (others => '0');
  signal shamt : integer range 0 to 255 := 0;
begin
  process (clk) is variable mix : unsigned(31 downto 0); variable wtmp : unsigned(99 downto 0); begin
    if rising_edge(clk) then
      if rst_l = '0' then acc <= (others=>'0'); wacc<=(others=>'0'); shamt<=0;
      else
        mix := acc srl (shamt mod 32);
        acc  <= acc + x"9E3779B9" + mix;
        wacc <= wacc + resize(mix,100) + to_unsigned(7,100);
        if shamt >= 125 then shamt <= 0; else shamt <= shamt + 5; end if;
      end if;
    end if;
  end process;
  y <= std_logic_vector(acc);
end architecture;
