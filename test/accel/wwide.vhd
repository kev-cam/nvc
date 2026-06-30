-- 160-bit datapath (>128, 5 limbs) to exercise the scalable 32-bit-limb wide
-- path: wide compare (wult), wide multiply (80b*80b=160b -> wmul), wide add,
-- wide xor, and concat/slice rotate. __int128 cannot represent this width.
library ieee; use ieee.std_logic_1164.all; use ieee.numeric_std.all;
entity wwide is
  port (clk, rst_l : in std_logic; y : out std_logic_vector(159 downto 0));
end entity;
architecture rtl of wwide is
  signal acc : unsigned(159 downto 0) := (others => '0');
begin
  process (clk) is begin
    if rising_edge(clk) then
      if rst_l = '0' then
        acc <= to_unsigned(1, 160);
      elsif acc > to_unsigned(100, 160) then          -- wide unsigned compare
        acc <= acc xor ((acc(149 downto 0) & acc(159 downto 150))   -- rotate (concat/slice)
                        + to_unsigned(12345, 160)                   -- wide add (const)
                        + (acc(79 downto 0) * acc(79 downto 0)));    -- 160b wide multiply
      else
        acc <= acc + to_unsigned(7, 160);
      end if;
    end if;
  end process;
  y <= std_logic_vector(acc);
end architecture;
