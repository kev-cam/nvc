-- Variable LOGICAL shift probe (srl/sll -> handled $shr/$shl).
-- Runtime shift amount (incl n>=width, n near width) on narrow (32b) and
-- wide (100b) data.  Single clocked entity so it INSTALLS.  This is the
-- HANDLED baseline: srl/sll map to Verilog >>/<< which the accel translates.
library ieee; use ieee.std_logic_1164.all; use ieee.numeric_std.all;
entity srlp is
  port (clk, rst_l : in std_logic;
        y : out std_logic_vector(31 downto 0));
end entity;
architecture rtl of srlp is
  signal acc   : unsigned(31 downto 0)  := (others => '0');
  signal wacc  : unsigned(99 downto 0)  := (others => '0');
  signal shamt : integer range 0 to 255 := 0;
begin
  process (clk) is
    variable sr_narrow : unsigned(31 downto 0);
    variable sr_wide   : unsigned(99 downto 0);
    variable sl_narrow : unsigned(31 downto 0);
    variable mix       : unsigned(31 downto 0);
  begin
    if rising_edge(clk) then
      if rst_l = '0' then
        acc <= (others => '0'); wacc <= (others => '0'); shamt <= 0;
      else
        sr_narrow := acc srl shamt;                    -- variable logical >> (narrow)
        sr_wide   := wacc srl shamt;                   -- variable logical >> (wide)
        sl_narrow := acc sll shamt;                    -- variable logical << (narrow)
        mix := sr_narrow xor sl_narrow xor sr_wide(31 downto 0);
        acc  <= acc + x"9E3779B9" + mix;
        wacc <= wacc + resize(mix, 100) + to_unsigned(7, 100);
        if shamt >= 125 then shamt <= 0; else shamt <= shamt + 5; end if;
      end if;
    end if;
  end process;
  y <= std_logic_vector(acc);
end architecture;
