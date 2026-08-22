-- MINIMAL repro: sign-extension of a small signed immediate lost by accel.
-- One clocked entity. acc += sign_extend(din[11:0]) each cycle. Gold (VHDL
-- resize on signed) sign-extends; accel drops it (zero-extends).
library ieee; use ieee.std_logic_1164.all; use ieee.numeric_std.all;
entity psext is
  port (clk  : in  std_logic;
        din  : in  std_logic_vector(31 downto 0);
        dout : out std_logic_vector(31 downto 0));
end entity;
architecture rtl of psext is
  signal acc : signed(31 downto 0) := (others => '0');
begin
  process (clk) is
    variable imm : signed(11 downto 0);
  begin
    if rising_edge(clk) then
      imm := signed(din(11 downto 0));   -- 12-bit signed
      acc <= acc + resize(imm, 32);      -- sign-extend to 32b, accumulate
    end if;
  end process;
  dout <= std_logic_vector(acc);
end architecture;
