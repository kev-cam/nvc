-- SIGNED narrow comparison probe: exercises $lt/$le/$gt/$ge with A_SIGNED on
-- narrow (<64b) operands of DIFFERING widths, including negatives. This mimics
-- an instruction-DECODE datapath: a small signed immediate field (sign-extended)
-- compared against a signed operand, with a signed select folded into Y.
--
-- The operands are SLICES of wider registers (field slicing), so sig_expr yields
-- a masked value that signed_expr must sign-extend via ((int64_t)(v<<k)>>k).
library ieee; use ieee.std_logic_1164.all; use ieee.numeric_std.all;
entity scmp is
  port (clk, rst_l : in std_logic;
        y : out std_logic_vector(31 downto 0));
end entity;
architecture rtl of scmp is
  -- state register holds several signed fields at various offsets/widths
  signal st  : std_logic_vector(63 downto 0) := (others => '0');
  signal acc : signed(31 downto 0) := (others => '0');
begin
  process (clk) is
    -- a : 12-bit signed immediate field (like a RISC-V I-imm), sign range -2048..2047
    variable a12 : signed(11 downto 0);
    -- b : 8-bit signed field
    variable b8  : signed(7 downto 0);
    -- c : 20-bit signed field (like a U/J imm slice)
    variable c20 : signed(19 downto 0);
    variable d16 : signed(15 downto 0);
    variable sel : signed(31 downto 0);
    variable bits : unsigned(3 downto 0);
  begin
    if rising_edge(clk) then
      if rst_l = '0' then
        st  <= (others => '0');
        acc <= (others => '0');
      else
        -- carve signed fields out of the state register (field slicing)
        a12 := signed(st(11 downto 0));
        b8  := signed(st(23 downto 16));
        c20 := signed(st(43 downto 24));
        d16 := signed(st(63 downto 48));

        -- accumulate boolean results of SIGNED narrow comparisons (differing widths)
        bits := (others => '0');
        -- signed(12b) < signed(8b): b8 sign-extends to compare against a12
        if a12 < resize(b8, 12) then bits(0) := '1'; end if;
        -- signed(20b) >= signed(16b)
        if c20 >= resize(d16, 20) then bits(1) := '1'; end if;
        -- signed(8b) <= signed(12b truncated) : cross-width le
        if b8 <= resize(a12(7 downto 0), 8) then bits(2) := '1'; end if;
        -- signed(16b) > signed(20b truncated)
        if d16 > resize(c20(15 downto 0), 16) then bits(3) := '1'; end if;

        -- a signed SELECT driven by a signed comparison: pick the larger (signed)
        if resize(a12, 32) < resize(c20, 32) then
          sel := resize(c20, 32);
        else
          sel := resize(a12, 32);
        end if;

        -- fold comparison bits + signed select into the accumulator
        acc <= acc + sel + signed(resize(bits, 32));

        -- churn the state register so fields cross zero (negatives) every cycle
        st <= std_logic_vector(resize(unsigned(st) * 1103515245 + 12345, 64));
      end if;
    end if;
  end process;
  y <= std_logic_vector(acc);
end architecture;
