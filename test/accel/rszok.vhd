-- POSITIVE CONTROL for rszcat.vhd: `resize` used as a CONCATENATION ELEMENT in
-- the two shapes that ARE expressible in Verilog.  This must still translate and
-- INSTALL -- a guard that simply declined every resize inside {} would pass
-- rszcat while quietly taking the accelerator away from the single most common
-- construct sv2vhdl emits, and nothing else in the suite would notice.
--
--   resize(a, 16) with a 8 bits  -> WIDENING: emitted as `{8'b0, a}`, exactly 16
--                                   bits, so the concat is faithful.
--   resize(c, 16) with c 16 bits -> IDENTITY: emitted as bare `c`, and c really
--                                   is 16 bits, so the concat is faithful.
--
-- Both are 24-bit concatenations, same shape as rszcat's, and both feed a
-- churning accumulator so any width error moves Y on essentially every cycle.
library ieee; use ieee.std_logic_1164.all; use ieee.numeric_std.all;
entity rszok is
  port (clk, rst_l : in std_logic; y : out std_logic_vector(31 downto 0));
end entity;
architecture rtl of rszok is
  signal st  : unsigned(31 downto 0) := to_unsigned(1, 32);
  signal cnt : unsigned(31 downto 0) := (others => '0');
begin
  process (clk) is
    variable a    : unsigned(7 downto 0);
    variable c    : unsigned(15 downto 0);
    variable p, q : std_logic_vector(23 downto 0);
  begin
    if rising_edge(clk) then
      if rst_l = '0' then
        st  <= to_unsigned(1, 32);
        cnt <= (others => '0');
      else
        a := st(7 downto 0);
        c := st(23 downto 8);
        -- widening resize inside a concat: {8'b0, a} is exactly 16 bits
        p := std_logic_vector(st(31 downto 24)) & std_logic_vector(resize(a, 16));
        -- identity resize inside a concat: c already IS 16 bits
        q := std_logic_vector(st(31 downto 24)) & std_logic_vector(resize(c, 16));
        cnt <= cnt + unsigned(p) + unsigned(q);
        st  <= resize(st * 1103515245 + 12345, 32);
      end if;
    end if;
  end process;
  y <= std_logic_vector(cnt);
end architecture;
