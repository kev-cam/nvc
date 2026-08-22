library ieee; use ieee.std_logic_1164.all; use ieee.numeric_std.all;
entity q_ge64 is port (clk, rst_l : in std_logic; y : out std_logic_vector(31 downto 0)); end entity;
architecture rtl of q_ge64 is
  signal acc   : unsigned(31 downto 0)  := (others => '0');
  signal shamt : integer range 0 to 255 := 0;
begin
  process (clk) is variable mix : unsigned(31 downto 0); begin
    if rising_edge(clk) then
      if rst_l = '0' then acc <= (others=>'0'); shamt<=0;
      else
        mix := acc srl shamt;
        acc  <= acc + x"9E3779B9" + mix;
        if shamt >= 70 then shamt <= 0; else shamt <= shamt + 1; end if;
      end if;
    end if;
  end process;
  y <= std_logic_vector(acc);
end architecture;
