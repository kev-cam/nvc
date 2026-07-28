-- Testbench for l3dchain.  The checksum folds the RAW logic3d codes (0..7), not a
-- 2-state projection, so it distinguishes "converged to a certain value" from
-- "converged to an uncertain value" from "never converged" -- the uncertain
-- plane is exactly what is under test here.
library ieee; use ieee.std_logic_1164.all; use ieee.numeric_std.all;
library sv2vhdl; use sv2vhdl.logic3d_types_pkg.all;
use std.env.stop;

entity l3dchain_tb is end entity;

architecture tb of l3dchain_tb is
  signal clk     : std_logic := '0';
  signal seed    : logic3d_vector(31 downto 0) := (others => L3D_1);
  signal result  : logic3d_vector(31 downto 0);
  signal running : boolean := true;

  function codesum(v : logic3d_vector) return natural is
    variable a : natural := 0;
  begin
    for i in v'range loop
      a := (a * 5 + v(i)) mod 16777216;
    end loop;
    return a;
  end function;
begin
  dut : entity work.l3dchain port map (clk => clk, seed => seed, result => result);

  clkgen : process is
  begin
    while running loop
      wait for 5 ns;
      clk <= not clk;
    end loop;
    wait;
  end process;

  main : process is
    variable chk : natural := 0;
    variable mx  : natural := 0;
  begin
    wait for 1 ns;
    report "T0=" & integer'image(codesum(result));
    for c in 1 to 20 loop
      wait until rising_edge(clk);
      chk := (chk * 31 + codesum(result)) mod 16777216;
      for i in result'range loop
        if result(i) > mx then mx := result(i); end if;
      end loop;
    end loop;
    report "Y=" & integer'image(chk) & " MAXCODE=" & integer'image(mx);
    running <= false;
    wait for 20 ns;
    stop;
  end process;
end architecture;
