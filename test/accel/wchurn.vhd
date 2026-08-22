-- 100-bit datapath (>64) to exercise the __int128 wide-signal path end-to-end.
library ieee; use ieee.std_logic_1164.all; use ieee.numeric_std.all;
entity wchurn is
  port (clk, rst_l : in std_logic; y : out std_logic_vector(99 downto 0));
end entity;
architecture rtl of wchurn is
  signal acc : unsigned(99 downto 0) := (others => '0');
begin
  process (clk) is begin
    if rising_edge(clk) then
      if rst_l = '0' then acc <= (others => '0');
      else acc <= acc xor ((acc(89 downto 0) & acc(99 downto 90))
                            + to_unsigned(12345, 100) + (acc(31 downto 0) * to_unsigned(3,32)));
      end if;
    end if;
  end process;
  y <= std_logic_vector(acc);
end architecture;
