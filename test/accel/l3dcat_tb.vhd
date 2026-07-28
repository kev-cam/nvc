library ieee; use ieee.std_logic_1164.all; use ieee.numeric_std.all;
library sv2vhdl; use sv2vhdl.logic3d_types_pkg.all;
use std.env.stop;

entity l3dcat_tb is end entity;

architecture tb of l3dcat_tb is
  signal clk   : std_logic := '0';
  signal wen   : logic3d := L3D_0;
  signal waddr : logic3d_vector(1 downto 0) := (others => L3D_0);
  signal wdata : logic3d_vector(7 downto 0) := (others => L3D_0);
  signal raddr : logic3d_vector(1 downto 0) := (others => L3D_0);
  signal rdata : logic3d_vector(7 downto 0);
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
  dut : entity work.l3dcat port map (clk, wen, waddr, wdata, raddr, rdata);

  clkgen : process is
  begin
    while running loop wait for 5 ns; clk <= not clk; end loop; wait;
  end process;

  main : process is
    variable chk : natural := 0;
  begin
    -- write 0xEE to reg1, 0x55 to reg2, 0xFF to reg3
    for a in 1 to 3 loop
      wen <= L3D_1; waddr <= mk(a, 2);
      if a = 1 then wdata <= mk(238, 8);
      elsif a = 2 then wdata <= mk(85, 8);
      else wdata <= mk(255, 8); end if;
      wait until rising_edge(clk);
      wait for 1 ns;
    end loop;
    wen <= L3D_0;
    wait until rising_edge(clk); wait for 1 ns;
    -- read them back
    for a in 1 to 3 loop
      raddr <= mk(a, 2);
      wait until rising_edge(clk); wait for 1 ns;
      wait until rising_edge(clk); wait for 1 ns;
      report "RD" & integer'image(a) & "=" & integer'image(val(rdata));
      chk := chk * 256 + val(rdata);
    end loop;
    report "Y=" & integer'image(chk);
    running <= false;
    wait for 20 ns;
    stop;
  end process;
end architecture;
