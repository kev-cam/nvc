library ieee; use ieee.std_logic_1164.all; use ieee.numeric_std.all;
use std.env.stop;

entity l3dmv_tb is end entity;

architecture tb of l3dmv_tb is
  signal clk : std_logic := '0';
  signal sel : std_logic_vector(7 downto 0) := (others => '0');
  signal rm  : std_logic_vector(7 downto 0);
  signal rw  : std_logic_vector(7 downto 0);
  signal running : boolean := true;

  -- Fold a std_logic_vector to a natural WITHOUT resolving metavalues to a
  -- 2-state answer: anything that is not '0'/'1'/'L'/'H' contributes a distinct
  -- marker, so a run that silently turns 'U' into 0 does NOT produce the same
  -- checksum as one that keeps it uncertain. That distinction is the whole
  -- point of the test.
  function val(v : std_logic_vector) return natural is
    variable a : natural := 0;
    variable d : natural;
  begin
    for i in v'high downto v'low loop
      case v(i) is
        when '1' | 'H' => d := 1;
        when '0' | 'L' => d := 0;
        when others    => d := 2;      -- metavalue: neither 0 nor 1
      end case;
      a := (a mod 100000) * 3 + d;
    end loop;
    return a;
  end function;
begin
  dm : entity work.l3dmv_meta port map (clk, sel, rm);
  dw : entity work.l3dmv_weak port map (clk, sel, rw);

  clkgen : process is
  begin
    while running loop wait for 5 ns; clk <= not clk; end loop; wait;
  end process;

  main : process is
    variable chk : natural := 0;
  begin
    for c in 0 to 3 loop
      case c is
        when 0      => sel <= x"01";   -- meta arm on both DUTs
        when 1      => sel <= x"02";   -- weak-L arm on l3dmv_weak
        when 2      => sel <= x"EE";   -- passthrough
        when others => sel <= x"55";   -- passthrough
      end case;
      wait until rising_edge(clk); wait for 1 ns;
      wait until rising_edge(clk); wait for 1 ns;
      report "C" & integer'image(c)
           & " meta=" & integer'image(val(rm))
           & " weak=" & integer'image(val(rw));
      chk := (chk mod 100000) * 7 + val(rm) + val(rw);
    end loop;
    report "Y=" & integer'image(chk);
    running <= false;
    wait for 20 ns;
    stop;
  end process;
end architecture;
