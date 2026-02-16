-- Probe: can we access implicit signals via external names?
library ieee;
use ieee.std_logic_1164.all;
use work.logic3ds_pkg.all;

entity probe_dut is
end entity;

architecture test of probe_dut is
    signal n : std_logic;
begin
    t1: entity work.sv_tran(strength)
        port map(a => n, b => n);
end architecture;

-- Try different syntaxes for accessing implicit signals
entity probe_ext is end;
architecture test of probe_ext is
    -- Try dotted path (standard VHDL-2008)
    alias drv is << signal .probe_wrap.dut.t1.a'driver : logic3ds >>;
begin
end architecture;

entity probe_wrap is end;
architecture wrap of probe_wrap is
begin
    dut: entity work.probe_dut;
    ext: entity work.probe_ext;
end architecture;
