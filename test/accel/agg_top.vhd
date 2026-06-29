-- Aggregate-translation regression for the whole-subtree --accel path.
-- Builds four 8-bit signals using DIFFERENT std_logic_vector aggregate forms,
-- each with CONSTANT element values so the result is deterministic:
--   s_named  <= (7 => '1', 0 => '1', others => '0');          -- 0x81 (named)
--   s_range  <= (7 downto 4 => '1', 3 downto 0 => '0');       -- 0xF0 (range)
--   s_posoth <= ('1','0','1', others => '0');                 -- 0xA0 (positional + others)
--   s_choice <= (1 | 3 | 5 => '1', others => '0');            -- 0x2A (choice list)
-- A registered accumulator (sync reset, rising_edge) adds 1 each cycle ONLY
-- when ALL FOUR aggregates equal their known constants. A correct translation
-- yields the gold Y; a bit-reversed / shifted / mis-indexed aggregate
-- translation breaks an equality so the increment stops -> a different Y.
library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

entity agg_top is
  port (clk, rst_l : in  std_logic;
        y          : out std_logic_vector(7 downto 0));
end entity;

architecture rtl of agg_top is
  signal s_named  : std_logic_vector(7 downto 0);
  signal s_range  : std_logic_vector(7 downto 0);
  signal s_posoth : std_logic_vector(7 downto 0);
  signal s_choice : std_logic_vector(7 downto 0);
  signal s_to     : std_logic_vector(0 to 7);   -- ASCENDING: index 0 is leftmost
  signal acc      : unsigned(7 downto 0) := (others => '0');
begin
  -- named choices                                  -- expect 0x81
  s_named  <= (7 => '1', 0 => '1', others => '0');
  -- range choices                                  -- expect 0xF0
  s_range  <= (7 downto 4 => '1', 3 downto 0 => '0');
  -- positional elements then 'others'              -- expect 0xA0
  s_posoth <= ('1', '0', '1', others => '0');
  -- choice (|) list                                -- expect 0x2A
  s_choice <= (1 | 3 | 5 => '1', others => '0');
  -- ascending (TO) vector: only element 0 set. Consumed by INDEXED reads below
  -- so a bit-reversed translation (which a literal compare can't catch) shows up:
  -- correct -> s_to(0)='1', s_to(7)='0'; reversed -> s_to(0)='0' and the count stops.
  s_to     <= (0 => '1', others => '0');

  process (clk) is
  begin
    if rising_edge(clk) then
      if rst_l = '0' then
        acc <= (others => '0');
      elsif s_named  = x"81" and s_range  = x"F0" and
            s_posoth = x"A0" and s_choice = x"2A" and
            s_to(0) = '1' and s_to(7) = '0' then
        acc <= acc + 1;
      end if;
    end if;
  end process;

  y <= std_logic_vector(acc);
end architecture;
