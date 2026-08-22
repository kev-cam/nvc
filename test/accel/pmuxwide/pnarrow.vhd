-- Narrow ($pmux scalar path) probe with SIGNED / sign-extended branch values —
-- the dec-relevant pattern. A 32-bit priority-encoded chain where several
-- branches select SIGN-EXTENDED small signed immediates (a small negative
-- field widened to 32 bits) and a signed default. This exercises the scalar
-- $pmux codegen (sig_expr per branch) combined with signed resize/sign-extend,
-- which the wide-limb path does not cover.
library ieee; use ieee.std_logic_1164.all; use ieee.numeric_std.all;
entity pnarrow is
  port (clk  : in  std_logic;
        sel  : in  std_logic_vector(7 downto 0);
        din  : in  std_logic_vector(31 downto 0);
        dout : out std_logic_vector(31 downto 0));
end entity;
architecture rtl of pnarrow is
  signal acc : signed(31 downto 0) := (others => '0');
begin
  process (clk) is
    variable s   : unsigned(7 downto 0);
    variable d   : signed(31 downto 0);
    variable imm : signed(11 downto 0);   -- small signed immediate field
    variable v   : signed(31 downto 0);
  begin
    if rising_edge(clk) then
      s   := unsigned(sel);
      d   := signed(din);
      imm := signed(din(11 downto 0));    -- a 12-bit signed immediate
      -- Priority-encoded chain, overlapping conditions. Several branches
      -- SIGN-EXTEND a small signed value to 32 bits (resize on signed).
      if    s < 16   then v := resize(imm, 32);            -- sign-extend imm (branch 0)
      elsif s < 32   then v := d + resize(imm, 32);        -- signed add w/ sext imm
      elsif s < 48   then v := resize(imm, 32) - d;        -- sext then subtract
      elsif s < 64   then v := resize(signed(din(7 downto 0)), 32);  -- 8b signed sext
      elsif s < 96   then v := d - resize(imm, 32);
      elsif s < 128  then v := resize(signed(din(3 downto 0)), 32);  -- 4b signed sext
      elsif s < 160  then v := d + d;
      elsif s < 192  then v := resize(imm, 32) xor d;
      elsif s < 224  then v := -d;                          -- signed negate
      else                v := resize(signed(din(15 downto 0)), 32); -- 16b sext default
      end if;
      acc <= (acc xor v) + signed(resize(s, 32));
    end if;
  end process;
  dout <= std_logic_vector(acc);
end architecture;
