-- A numeric_std `*` used as a CONCATENATION ELEMENT.
--
-- Companion to l3dcat.vhd: same failure mode (a concatenation element whose
-- Verilog self-determined width is not its VHDL width), different construct, and
-- this one is reachable in plain std_logic/numeric_std so it runs in the
-- --std=2008 suite.
--
-- VHDL: `a * b` on unsigned(7 downto 0) is SIXTEEN bits.
-- Verilog: `(a * b)` is self-determined max(8,8) = EIGHT bits, and a
-- concatenation does not resize its parts -- so `{st[23:16], (a*b)}` is 16 bits
-- wide, not 24, the product's high byte is dropped and everything to its left
-- shifts down.  vhdl2vlog cannot express the VHDL width here, so it must
-- DECLINE.  Without the decline the module installs and Y is silently wrong
-- (measured: accel Y=136746172 vs interpreter Y=1768636092).
library ieee; use ieee.std_logic_1164.all; use ieee.numeric_std.all;
entity mulcat is
  port (clk, rst_l : in std_logic; y : out std_logic_vector(31 downto 0));
end entity;
architecture rtl of mulcat is
  signal st  : unsigned(31 downto 0) := to_unsigned(1, 32);
  signal cnt : unsigned(31 downto 0) := (others => '0');
begin
  process (clk) is
    variable a, b : unsigned(7 downto 0);
    variable w    : std_logic_vector(23 downto 0);
  begin
    if rising_edge(clk) then
      if rst_l = '0' then
        st  <= to_unsigned(1, 32);
        cnt <= (others => '0');
      else
        a := st(7 downto 0);
        b := st(15 downto 8);
        -- 8-bit slice & 16-bit product = 24 bits in VHDL
        w   := std_logic_vector(st(23 downto 16)) & std_logic_vector(a * b);
        cnt <= cnt + unsigned(w);
        st  <= resize(st * 1103515245 + 12345, 32);
      end if;
    end if;
  end process;
  y <= std_logic_vector(cnt);
end architecture;
