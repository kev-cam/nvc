library ieee; use ieee.std_logic_1164.all; use ieee.numeric_std.all;
entity areset is port(clk,arst,en:in std_logic; din:in std_logic_vector(31 downto 0); y:out std_logic_vector(31 downto 0)); end entity;
architecture rtl of areset is
  signal r1 : unsigned(31 downto 0) := (others=>'0');   -- ZERO init (init path OK)
  signal r2 : unsigned(31 downto 0) := (others=>'0');
begin
  process(clk,arst) is begin
    if rising_edge(clk) then
      r1 <= r1 + unsigned(din) + 1;                       -- $adff datapath
      if en='1' then r2 <= (r2 xor unsigned(din)) + 3; end if;  -- $adffe datapath
    elsif arst='1' then
      r1 <= x"DEADBEEF";                                  -- NON-ZERO async reset
      r2 <= x"0BADF00D";
    end if;
  end process;
  y <= std_logic_vector(r1 xor r2);
end architecture;
