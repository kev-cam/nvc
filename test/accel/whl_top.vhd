-- Counter-controlled WHILE-loop pattern (the form sv2ghdl lowers an SV `for`
-- to: `i := init; while i < N loop ...; i := i + 1; end loop;`). This is the
-- shape vhdl2vlog currently DECLINES (T_WHILE -> /*?while-loop*/) so the whole
-- subtree stays interpreted instead of crashing yosys's read_verilog (which
-- rejects procedural while-loops). Used to validate a while->for peephole:
-- after the peephole the subtree should install and still MATCH this gold.
--
-- What it computes (observable):
--   pattern  = x"B7" = "10110111"  -> popcount = 6 set bits.
--   Each cycle the OUTER while iterates the 8 bits; for every set bit the INNER
--   while adds 1 (degenerate 1-iteration inner loop) so s = popcount = 6, then
--   acc <= acc + s. After reset is released the TB clocks N rising edges, so
--   Y = 6 * (#counted edges). With 30 post-reset edges -> Y = 180.
library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

entity whl_top is
  port (clk   : in  std_logic;
        rst_l : in  std_logic;
        y     : out std_logic_vector(7 downto 0));
end entity;

architecture rtl of whl_top is
  constant pattern : std_logic_vector(7 downto 0) := x"B7";  -- 6 ones
  signal   acc     : unsigned(7 downto 0) := (others => '0');
begin
  process (clk) is
    variable s : unsigned(7 downto 0);
    variable i : integer;
    variable j : integer;
  begin
    if rising_edge(clk) then
      if rst_l = '0' then
        acc <= (others => '0');
      else
        -- NESTED counter-controlled while-loops (mirrors dec_gpr_ctl): the
        -- outer scans the bit index, the inner is a degenerate add-once loop.
        s := (others => '0');
        i := 0;
        while i < 8 loop
          if pattern(i) = '1' then
            j := 0;
            while j < 1 loop          -- inner counter-controlled while
              s := s + 1;
              j := j + 1;
            end loop;
          end if;
          i := i + 1;                 -- outer counter increment
        end loop;
        acc <= acc + s;               -- += popcount(pattern) each cycle
      end if;
    end if;
  end process;

  y <= std_logic_vector(acc);
end architecture;
