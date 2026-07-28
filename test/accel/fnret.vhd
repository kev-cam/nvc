-- Design-declared VHDL FUNCTION, the shape emit_function is built for:
-- value parameters, a local variable, straight-line body, ONE trailing return.
-- This must still translate and INSTALL -- it is the control for fnearly.vhd,
-- which is the same design with the return moved out of tail position.
--
-- Neither shape had any coverage before: no design in test/accel declared a
-- VHDL function at all, so emit_function was never once exercised by the
-- regression (and the emit_function `name` use-after-clobber bug shipped).
library ieee; use ieee.std_logic_1164.all; use ieee.numeric_std.all;
entity fnret is
  port (clk, rst_l : in std_logic; y : out std_logic_vector(31 downto 0));
end entity;
architecture rtl of fnret is
  -- Straight-line, single trailing return -> translatable.
  -- The returned expression must be a CONSTRAINED object: emit_function takes
  -- the Verilog result width from the last return's value type, and the
  -- signature type (`std_logic_vector`) is unconstrained, so returning a
  -- conversion directly declines before the body is even looked at.
  function mix (a, b : unsigned(7 downto 0)) return std_logic_vector is
    variable s : unsigned(7 downto 0);
    variable r : std_logic_vector(7 downto 0);
  begin
    s := a xor b;
    r := std_logic_vector(s + 1);
    return r;
  end function;
  signal st  : unsigned(31 downto 0) := to_unsigned(1, 32);
  signal cnt : unsigned(31 downto 0) := (others => '0');
begin
  process (clk) is
    variable m : std_logic_vector(7 downto 0);
  begin
    if rising_edge(clk) then
      if rst_l = '0' then
        st  <= to_unsigned(1, 32);
        cnt <= (others => '0');
      else
        m   := mix(st(7 downto 0), st(15 downto 8));
        cnt <= cnt + unsigned(m);
        st  <= resize(st * 1103515245 + 12345, 32);
      end if;
    end if;
  end process;
  y <= std_logic_vector(cnt);
end architecture;
