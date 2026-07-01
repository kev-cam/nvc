-- Minimal repro: a SINGLE signed narrow comparison into Y.
-- a,b are signed 8-bit fields carved from a churning state register (so they go
-- negative). Y accumulates 1 each cycle that signed(a) < signed(b).
-- Gold counts true signed-lt; accel (vhdl2vlog drops `signed`) counts unsigned-lt.
library ieee; use ieee.std_logic_1164.all; use ieee.numeric_std.all;
entity scmin is
  port (clk, rst_l : in std_logic; y : out std_logic_vector(31 downto 0));
end entity;
architecture rtl of scmin is
  signal st  : unsigned(31 downto 0) := to_unsigned(1, 32);
  signal cnt : unsigned(31 downto 0) := (others => '0');
begin
  process (clk) is
    variable a, b : signed(7 downto 0);
  begin
    if rising_edge(clk) then
      if rst_l = '0' then
        st  <= to_unsigned(1, 32);
        cnt <= (others => '0');
      else
        a := signed(st(7 downto 0));
        b := signed(st(15 downto 8));
        if a < b then          -- SIGNED narrow comparison (the probe target)
          cnt <= cnt + 1;
        end if;
        st <= resize(st * 1103515245 + 12345, 32);
      end if;
    end if;
  end process;
  y <= std_logic_vector(cnt);
end architecture;
