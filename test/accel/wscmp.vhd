-- Wide (96b) SIGNED comparison with DRIVEN registers (sa goes negative), so the
-- signed-vs-unsigned difference is exercised without the undriven-init confound.
library ieee; use ieee.std_logic_1164.all; use ieee.numeric_std.all;
entity wscmp is
  port (clk, rst_l : in std_logic; y : out std_logic_vector(31 downto 0));
end entity;
architecture rtl of wscmp is
  signal sa, sb : signed(95 downto 0) := (others => '0');
  signal cnt    : unsigned(31 downto 0) := (others => '0');
begin
  process (clk) is begin
    if rising_edge(clk) then
      if rst_l = '0' then sa <= (others=>'0'); sb <= (others=>'0'); cnt <= (others=>'0');
      else
        sa <= sa - 3;                     -- drives sa negative
        sb <= sb + 1;
        if sa < sb then cnt <= cnt + 2;   -- SIGNED compare (true while sa<0<=sb)
        else            cnt <= cnt + 1; end if;
      end if;
    end if;
  end process;
  y <= std_logic_vector(cnt);
end architecture;
