library ieee; use ieee.std_logic_1164.all; use ieee.numeric_std.all;
library sv2vhdl; use sv2vhdl.logic3d_types_pkg.all;
use std.env.stop;

entity l3dwrap_tb is end entity;

architecture tb of l3dwrap_tb is
  signal clk  : std_logic := '0';
  signal sel  : logic3d_vector(1 downto 0)  := (others => L3D_0);
  signal din  : logic3d_vector(15 downto 0) := (others => L3D_0);
  signal qg, qb : logic3d_vector(15 downto 0);
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
  dutg : entity work.l3dwrap  port map (clk, din, qg);
  dutb : entity work.l3dwrapx port map (clk, sel, din, qb);

  clkgen : process is
  begin
    while running loop wait for 5 ns; clk <= not clk; end loop; wait;
  end process;

  main : process is
    variable chk : natural := 0;
  begin
    for k in 1 to 3 loop
      din <= mk(k * 21845, 16);          -- 0x5555, 0xAAAA, 0xFFFF
      sel <= mk(k, 2);
      wait until rising_edge(clk); wait for 1 ns;
      wait until rising_edge(clk); wait for 1 ns;
      report "G" & integer'image(k) & "=" & integer'image(val(qg))
           & " B" & integer'image(k) & "=" & integer'image(val(qb));
      chk := (chk * 3 + val(qg) + val(qb) * 7) mod 100000000;
    end loop;
    report "Y=" & integer'image(chk);
    running <= false;
    wait for 20 ns;
    stop;
  end process;
end architecture;
