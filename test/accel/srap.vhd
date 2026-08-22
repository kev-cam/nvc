-- Arithmetic shift right + variable shift probe (OPERATOR forms: sra/srl/sll).
-- Exercises 'sra' (arithmetic shift right) and 'srl'/'sll' (logical) by a
-- RUNTIME amount n (including n >= width, n near width) on both narrow (32b)
-- and wide (100b) data, mixing the exact bits into Y.  Single clocked entity.
library ieee; use ieee.std_logic_1164.all; use ieee.numeric_std.all;
entity srap is
  port (clk, rst_l : in std_logic;
        y : out std_logic_vector(31 downto 0));
end entity;
architecture rtl of srap is
  signal acc    : signed(31 downto 0)  := (others => '0');
  signal wacc   : signed(99 downto 0)  := (others => '0');
  signal shamt  : integer range 0 to 255 := 0;   -- 0..127 runtime shift (headroom)
  signal nib    : signed(31 downto 0)  := (others => '0');
begin
  process (clk) is
    variable sr_narrow : signed(31 downto 0);
    variable sr_wide   : signed(99 downto 0);
    variable sl_narrow : signed(31 downto 0);
    variable mix       : signed(31 downto 0);
  begin
    if rising_edge(clk) then
      if rst_l = '0' then
        acc <= (others => '0'); wacc <= (others => '0');
        shamt <= 0; nib <= (others => '0');
      else
        -- ARITHMETIC shift right by a RUNTIME amount on NARROW signed data.
        -- shamt sweeps 0..127: covers n<width, n=width-1, n=width, n>width.
        sr_narrow := acc sra shamt;
        -- ARITHMETIC shift right on WIDE (100b) signed data, runtime amount.
        sr_wide   := wacc sra shamt;
        -- LOGICAL shift left by runtime amount (narrow).
        sl_narrow := acc sll shamt;
        -- fold the wide SRA result (low 32) into the mix.
        mix := sr_narrow xor sl_narrow xor sr_wide(31 downto 0) xor nib;
        acc   <= acc - to_signed(999983, 32) + mix;
        wacc  <= wacc + resize(mix, 100) - to_signed(7, 100);
        nib   <= mix sra 3;                        -- SRA by a CONSTANT 3
        if shamt >= 125 then shamt <= 0; else shamt <= shamt + 5; end if;
      end if;
    end if;
  end process;
  y <= std_logic_vector(acc);
end architecture;
