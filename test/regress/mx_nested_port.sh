set -xe

pwd
which nvc

# Regression for STD_MX nested out-port propagation bug.
#
# When an inner entity's OUT port is bound to an outer entity's OUT port
# via direct port-map, then bound again to a top-level signal, the
# innermost driver must propagate all the way through.  In STD_MX mode
# (--std=2040, used for iverilog-generated VHDL with unresolved logic3d
# types), the `has_driver` check on SOURCE_PORT only walked one hop, so
# nested bindings silently dropped contributions and the top signal sat
# at its init value forever.

cat > test.vhd <<'EOF'
package l3d is
  subtype logic3d is integer range 0 to 7;
  constant L3D_0 : logic3d := 2;
  constant L3D_1 : logic3d := 3;
end package;

use work.l3d.all;
entity inner is port (Q : out logic3d); end entity;
architecture a of inner is begin
  Q <= L3D_1 after 1 ns, L3D_0 after 6 ns, L3D_1 after 11 ns;
end architecture;

use work.l3d.all;
entity outer is port (Q : out logic3d); end entity;
architecture a of outer is begin
  u : entity work.inner port map (Q => Q);
end architecture;

use work.l3d.all;
entity mx_nested_port is end entity;
architecture sim of mx_nested_port is
  signal q : logic3d := L3D_0;
begin
  u : entity work.outer port map (Q => q);
  check: process begin
    wait for 2 ns;
    assert q = L3D_1 report "at 2ns expected 3 got " & integer'image(q) severity failure;
    wait for 5 ns;
    assert q = L3D_0 report "at 7ns expected 2 got " & integer'image(q) severity failure;
    wait for 5 ns;
    assert q = L3D_1 report "at 12ns expected 3 got " & integer'image(q) severity failure;
    report "PASSED nested out-port propagation in STD_MX";
    wait;
  end process;
end architecture;
EOF

nvc --std=2040 -a test.vhd -e mx_nested_port -r 2>&1 | tee log.txt
grep -q "PASSED nested out-port propagation in STD_MX" log.txt
