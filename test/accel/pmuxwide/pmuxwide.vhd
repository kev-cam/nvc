-- Priority mux ($pmux) probe: a big priority-encoded if/elsif chain with many
-- branches selecting WIDE (72-bit) values and a WIDE default, driven by a
-- varying selector. Conditions OVERLAP (true priority encoding: several may be
-- true at once, the FIRST wins). yosys lowers this to a single $pmux whose S
-- port has one bit per branch and B is the concatenation of branch values;
-- A is the default. The 72-bit width forces the emit_wide_cell $pmux path
-- (wplace/emit_materialize per branch). The selected value flows into acc.
library ieee; use ieee.std_logic_1164.all; use ieee.numeric_std.all;
entity pmuxwide is
  port (clk  : in  std_logic;
        sel  : in  std_logic_vector(7 downto 0);   -- varying selector
        din  : in  std_logic_vector(71 downto 0);  -- wide varying data
        dout : out std_logic_vector(71 downto 0));
end entity;
architecture rtl of pmuxwide is
  signal acc : unsigned(71 downto 0) := (others => '0');

  -- wide constants selected by the priority chain
  constant K0 : unsigned(71 downto 0) := x"010203040506070809";
  constant K1 : unsigned(71 downto 0) := x"AABBCCDDEEFF001122";
  constant K2 : unsigned(71 downto 0) := x"DEADBEEFCAFEF00DBA";
  constant K3 : unsigned(71 downto 0) := x"0F1E2D3C4B5A697887";
begin
  process (clk) is
    variable v : unsigned(71 downto 0);
    variable s : unsigned(7 downto 0);
    variable d : unsigned(71 downto 0);
  begin
    if rising_edge(clk) then
      s := unsigned(sel);
      d := unsigned(din);
      -- Priority-encoded chain. Conditions overlap on purpose: e.g. sel<16
      -- also satisfies sel<64, etc. First matching branch wins (priority).
      -- WIDE default v := d + K0 (wide arithmetic feeds the pmux default).
      if    s < 16   then v := d xor K1;                 -- branch 0 (highest pri)
      elsif s < 32   then v := K2 + d;                   -- branch 1
      elsif s < 48   then v := K3 - d;                   -- branch 2
      elsif s < 64   then v := d xor K0 xor K2;          -- branch 3
      elsif s < 96   then v := K1 + K3;                  -- branch 4
      elsif s < 128  then v := d + d;                    -- branch 5
      elsif s < 160  then v := K2 xor K3;                -- branch 6
      elsif s < 192  then v := d - K1;                   -- branch 7
      elsif s < 224  then v := K0 xor d xor K1 xor K2;   -- branch 8
      else                v := d + K0;                   -- WIDE default
      end if;
      -- Mix selected value into the accumulator (bit-sensitive: xor + rotate-ish)
      acc <= (acc xor v) + unsigned(sel);
    end if;
  end process;
  dout <= std_logic_vector(acc);
end architecture;
