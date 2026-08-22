-- Isolation: stage C ALONE as a single accel chunk. If it diverges standalone,
-- the c3reg failure is a per-chunk gen_statemachine codegen bug (the xor-const
-- pattern), NOT a multi-chunk evaluation bug.
library ieee; use ieee.std_logic_1164.all; use ieee.numeric_std.all;
entity c1c is
  port (clk : in std_logic; seed : in std_logic_vector(31 downto 0);
        result : out std_logic_vector(31 downto 0));
end entity;
architecture rtl of c1c is
  signal rc : unsigned(31 downto 0) := (others => '0');
begin
  process (clk) is begin
    if rising_edge(clk) then rc <= (rc + unsigned(seed)) xor x"5A5A5A5A"; end if;
  end process;
  result <= std_logic_vector(rc);
end architecture;
