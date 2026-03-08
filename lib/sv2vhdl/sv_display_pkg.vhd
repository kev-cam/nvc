-- sv_display_pkg.vhd -- Verilog $display format helper functions
--
-- Provides lowercase hex/octal/binary/decimal string conversion
-- matching Verilog $display conventions (lowercase hex digits).
--
-- Usage:
--   library sv2vhdl;
--   use sv2vhdl.sv_display_pkg.all;

library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

package sv_display_pkg is
    -- Lowercase hex string (Verilog %h/%x): "3ff" not "3FF"
    function sv_hstr(v : std_logic_vector) return string;
    -- Octal string (Verilog %o)
    function sv_ostr(v : std_logic_vector) return string;
    -- Binary string (Verilog %b)
    function sv_bstr(v : std_logic_vector) return string;
    -- Unsigned decimal string (Verilog %d)
    function sv_dstr(v : std_logic_vector) return string;
end package;

package body sv_display_pkg is

    function to_lower(c : character) return character is
    begin
        case c is
            when 'A' => return 'a';
            when 'B' => return 'b';
            when 'C' => return 'c';
            when 'D' => return 'd';
            when 'E' => return 'e';
            when 'F' => return 'f';
            when others => return c;
        end case;
    end function;

    function sv_hstr(v : std_logic_vector) return string is
        constant h : string := to_hstring(v);
        variable result : string(1 to h'length);
    begin
        for i in h'range loop
            result(i - h'low + 1) := to_lower(h(i));
        end loop;
        return result;
    end function;

    function sv_ostr(v : std_logic_vector) return string is
    begin
        return to_ostring(v);
    end function;

    function sv_bstr(v : std_logic_vector) return string is
    begin
        return to_bstring(v);
    end function;

    function sv_dstr(v : std_logic_vector) return string is
    begin
        return integer'image(to_integer(unsigned(v)));
    end function;

end package body;
