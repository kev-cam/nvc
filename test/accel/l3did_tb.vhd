library ieee; use ieee.std_logic_1164.all; use ieee.numeric_std.all;
library sv2vhdl; use sv2vhdl.logic3d_types_pkg.all;
use std.env.stop;

entity l3did_tb is end entity;

architecture tb of l3did_tb is
  signal clk : std_logic := '0';
  signal sel : logic3d_vector(3 downto 0) := (others => L3D_0);
  signal d   : logic3d_vector(7 downto 0) := (others => L3D_0);
  signal q   : logic3d_vector(7 downto 0);
  signal running : boolean := true;

  function val(v : logic3d_vector) return natural is
    variable a : natural := 0;
  begin
    for i in v'high downto v'low loop
      a := a * 2 + (v(i) mod 2);
    end loop;
    return a;
  end function;

  function mk(n, w : natural) return logic3d_vector is
    variable r : logic3d_vector(w-1 downto 0);
    variable t : natural := n;
  begin
    for i in 0 to w-1 loop
      if (t mod 2) = 1 then r(i) := L3D_1; else r(i) := L3D_0; end if;
      t := t / 2;
    end loop;
    return r;
  end function;
begin
  dut : entity work.l3did port map (clk, sel, d, q);

  clkgen : process is
  begin
    while running loop wait for 5 ns; clk <= not clk; end loop; wait;
  end process;

  main : process is
    variable chk : natural := 0;
  begin
    -- Each case picks sel so that only sel(0) decides the mask, and d so that
    -- the shredded mask 8'h11 gives a DIFFERENT answer from the correct 8'hFF.
    --   sel=0001 -> u=1, bit0=1 -> mask FF -> q = d
    --   sel=1110 -> u=14, bit0=0 -> mask 00 -> q = 0
    -- Under the defect the mask is {u,u} = 8'h11 and 8'hEE respectively, so
    -- case 1 returns 0xEE and 0x00 -> 0x00, and case 2 returns 0x00 and 0xEE
    -- -> a NONZERO value where zero is correct. Both directions are covered.
    for c in 0 to 3 loop
      case c is
        when 0 => sel <= mk(1, 4);  d <= mk(238, 8);   -- expect 238
        when 1 => sel <= mk(14, 4); d <= mk(238, 8);   -- expect 0
        when 2 => sel <= mk(3, 4);  d <= mk(85, 8);    -- expect 85
        when others => sel <= mk(8, 4); d <= mk(255, 8); -- expect 0
      end case;
      wait until rising_edge(clk); wait for 1 ns;
      wait until rising_edge(clk); wait for 1 ns;
      report "C" & integer'image(c) & "=" & integer'image(val(q));
      -- rolling 24-bit accumulator: four 8-bit samples would overflow a signed
      -- 32-bit INTEGER on the last shift
      chk := (chk mod 65536) * 256 + val(q);
    end loop;
    report "Y=" & integer'image(chk);
    running <= false;
    wait for 20 ns;
    stop;
  end process;
end architecture;
