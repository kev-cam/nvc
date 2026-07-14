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

    constant HEXCHARS : string(1 to 16) := "0123456789abcdef";

    -- Map a single std_logic to its Verilog 4-state bit character.
    --   '0'/'L' -> '0'   '1'/'H' -> '1'   'Z' -> 'z'
    --   'X'/'U'/'W'/'-'  -> 'x'
    function sv_bit(b : std_logic) return character is
    begin
        case b is
            when '0' | 'L' => return '0';
            when '1' | 'H' => return '1';
            when 'Z'       => return 'z';
            when others    => return 'x';  -- X, U, W, -
        end case;
    end function;

    -- Classify a single std_logic as one of value(0/1), z, or x.
    --   returns 0 -> known, contributes value; 1 -> z; 2 -> x
    -- val_bit is the 0/1 value for known bits (else 0).
    procedure classify(b : std_logic; kind : out natural; val_bit : out natural) is
    begin
        val_bit := 0;
        case b is
            when '0' | 'L' => kind := 0;
            when '1' | 'H' => kind := 0; val_bit := 1;
            when 'Z'       => kind := 1;
            when others    => kind := 2;  -- X, U, W, -
        end case;
    end procedure;

    -- Render one grouped digit (hex: gwidth=4, oct: gwidth=3) per Verilog:
    --   all-known    -> radix digit (lowercase)
    --   all-x        -> 'x'      all-z -> 'z'
    --   any x (mixed)-> 'X'      else any z (mixed) -> 'Z'
    -- grp holds the group's bits with grp(0) = LSB of the group; gs = # bits.
    function sv_digit(grp : std_logic_vector; gs : natural) return character is
        variable nx, nz, val, k, v : natural := 0;
    begin
        nx := 0; nz := 0; val := 0;
        for i in 0 to gs - 1 loop
            classify(grp(i), k, v);
            case k is
                when 0      => val := val + v * (2 ** i);
                when 1      => nz := nz + 1;
                when others => nx := nx + 1;
            end case;
        end loop;
        if nx = 0 and nz = 0 then
            return HEXCHARS(val + 1);
        elsif nx = gs then
            return 'x';
        elsif nz = gs then
            return 'z';
        elsif nx > 0 then
            return 'X';
        else
            return 'Z';
        end if;
    end function;

    -- Common radix-grouped renderer (gwidth = 4 for hex, 3 for octal).
    function sv_radix(v : std_logic_vector; gwidth : natural) return string is
        constant n     : natural := v'length;
        variable vv    : std_logic_vector(n - 1 downto 0) := v;
        constant ndig  : natural := (n + gwidth - 1) / gwidth;
        variable result: string(1 to ndig);
        variable gs    : natural;
        variable grp   : std_logic_vector(gwidth - 1 downto 0);
    begin
        for g in 0 to ndig - 1 loop
            -- bits [g*gwidth .. min(g*gwidth+gwidth-1, n-1)]
            if (g + 1) * gwidth <= n then
                gs := gwidth;
            else
                gs := n - g * gwidth;
            end if;
            grp := (others => '0');
            for i in 0 to gs - 1 loop
                grp(i) := vv(g * gwidth + i);
            end loop;
            -- result(1) is the most-significant digit (group ndig-1)
            result(ndig - g) := sv_digit(grp, gs);
        end loop;
        return result;
    end function;

    function sv_hstr(v : std_logic_vector) return string is
    begin
        return sv_radix(v, 4);
    end function;

    function sv_ostr(v : std_logic_vector) return string is
    begin
        return sv_radix(v, 3);
    end function;

    function sv_bstr(v : std_logic_vector) return string is
        constant n     : natural := v'length;
        variable vv    : std_logic_vector(n - 1 downto 0) := v;
        variable result: string(1 to n);
    begin
        -- MSB (vv(n-1)) leftmost. Each bit -> Verilog 0/1/x/z.
        for i in 0 to n - 1 loop
            result(n - i) := sv_bit(vv(i));
        end loop;
        return result;
    end function;

    function sv_dstr(v : std_logic_vector) return string is
        constant n  : natural := v'length;
        variable vv : std_logic_vector(n - 1 downto 0) := v;
        variable u  : unsigned(n - 1 downto 0);
    begin
        -- Verilog %d prints "x" if any bit is unknown (x or z), else decimal.
        for i in 0 to n - 1 loop
            case vv(i) is
                when '0' | 'L' => u(i) := '0';
                when '1' | 'H' => u(i) := '1';
                when others    => return "x";
            end case;
        end loop;
        return integer'image(to_integer(u));
    end function;

end package body;
