-- Signed multiply probe. Mirrors VeeR dec's sign-extended-immediate math:
-- a small SIGNED field (can be negative) multiplied by a signed multiplier,
-- the WIDER signed product mixed into a running accumulator.
--
-- Why this exposes the accel bug: gen_statemachine's $mul handler reads its
-- operands via sig_expr() as raw UNSIGNED bit-patterns and never sign-extends
-- (unlike $lt/$le/$gt/$ge which call signed_expr()). The product is masked to
-- the Y width. When Y is WIDER than the operands (a signed product that must
-- sign-extend into the high bits), a negative operand becomes a large positive
-- value -> the high/extended product bits diverge from gold.
library ieee; use ieee.std_logic_1164.all; use ieee.numeric_std.all;
entity smul is
  port (clk, rst_l : in std_logic;
        a  : in  std_logic_vector(7 downto 0);   -- signed 8b operand (can be negative)
        b  : in  std_logic_vector(7 downto 0);   -- signed 8b multiplier (can be negative)
        y  : out std_logic_vector(31 downto 0)); -- WIDE signed accumulator (sign-extension matters)
end entity;
architecture rtl of smul is
  signal acc : signed(31 downto 0) := (others => '0');
begin
  process (clk) is
    variable prod : signed(15 downto 0);   -- 8x8 signed product, 16 bits
  begin
    if rising_edge(clk) then
      if rst_l = '0' then
        acc <= (others => '0');
      else
        -- signed*signed, product sign-extended from 16b to the 32b accumulator.
        prod := signed(a) * signed(b);
        acc  <= acc + resize(prod, 32);
      end if;
    end if;
  end process;
  y <= std_logic_vector(acc);
end architecture;
