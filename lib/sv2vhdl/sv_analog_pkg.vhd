-- sv_analog_pkg.vhd -- Verilog-AMS analog block support
--
-- Provides the sv_analog() procedure that carries Verilog-A block text
-- through the VHDL compilation pipeline as an opaque string payload.
-- Also declares discipline/nature attributes for analog signal metadata.
--
-- Usage:
--   library sv2vhdl;
--   use sv2vhdl.sv_analog_pkg.all;
--
-- Load at runtime:
--   nvc --std=2040 --load=.../libsv_analog.so -e ...
--

package sv_analog_pkg is

    -- Carry an analog block through VHDL as an opaque string.
    -- The string contains reconstructed Verilog-A source text.
    -- Phase 1: collected and printed at elaboration.
    -- Phase 2 (future): passed to ngSpice/OpenVAF for simulation.
    procedure sv_analog(block_text : in string);

    -- Discipline/nature attributes emitted by iverilog on analog signals
    attribute discipline : string;
    attribute va_nature_potential : string;
    attribute va_nature_flow : string;

end package sv_analog_pkg;

package body sv_analog_pkg is

    procedure sv_analog(block_text : in string) is
    begin
        -- Stub body; replaced by VHPIDIRECT at load time
    end procedure;
    attribute foreign of sv_analog [string] : procedure is
        "VHPIDIRECT sv_analog_eval";

end package body sv_analog_pkg;
