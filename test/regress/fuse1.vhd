-- Minimal hand fuse test: driver a, receiver b, fuse b onto a.
library ieee; use ieee.std_logic_1164.all;
library sv2vhdl; use sv2vhdl.fuse_pkg.all;

entity fuse1 is end;
architecture t of fuse1 is
  signal a : std_ulogic := '0';
  signal b : std_ulogic := 'Z';
  signal cnt : natural := 0;
begin
  drv: process begin
    for c in 1 to 10 loop
      a <= not a; wait for 1 ns;
    end loop;
    wait for 1 ns;
    -- fused: t0 run + 10 edges = 11.  Copy fallback adds the t0
    -- Z->0 copy delta = 12.  Both uniform in steady state.
    assert cnt = 11 or cnt = 12
      report "cnt=" & integer'image(cnt) severity failure;
    report "FUSE1 PASS cnt=" & integer'image(cnt);
    wait;
  end process;

  fuser: process begin
    if fuse_try(a, b) then
      wait;               -- fused: park forever, no code runs again
    end if;
    loop                  -- fallback: copy loop (intrinsics off)
      b <= a;
      wait on a;
    end loop;
  end process;

  leaf: process (b) begin
    cnt <= cnt + 1;
  end process;
end architecture;
