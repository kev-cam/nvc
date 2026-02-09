-- Test for 3D logic type: lookup table resolution
-- Uses atomic record resolution via encoding to avoid per-field update bug

package logic3d7_pkg is
    type logic_3d_t is record
        value    : boolean;
        strength : boolean;
        uncertain: boolean;
    end record logic_3d_t;

    type logic_3d_vector is array (natural range <>) of logic_3d_t;

    -- Encoded values (3 bits: uncertain:strength:value)
    constant L3D_0 : logic_3d_t := (value => false, strength => true,  uncertain => false);  -- 010 = 2
    constant L3D_1 : logic_3d_t := (value => true,  strength => true,  uncertain => false);  -- 011 = 3
    constant L3D_Z : logic_3d_t := (value => false, strength => false, uncertain => true);   -- 100 = 4
    constant L3D_X : logic_3d_t := (value => false, strength => true,  uncertain => true);   -- 110 = 6

    -- Encode record to 3-bit integer
    function encode(r : logic_3d_t) return natural;

    -- Decode 3-bit integer to record
    function decode(n : natural) return logic_3d_t;

    -- Resolution function using lookup table
    function resolved(s : logic_3d_vector) return logic_3d_t;
    subtype logic_3d_r is resolved logic_3d_t;
end package;

package body logic3d7_pkg is

    function encode(r : logic_3d_t) return natural is
        variable result : natural := 0;
    begin
        if r.value then result := result + 1; end if;
        if r.strength then result := result + 2; end if;
        if r.uncertain then result := result + 4; end if;
        return result;
    end function;

    function decode(n : natural) return logic_3d_t is
    begin
        return (
            value     => (n mod 2) = 1,
            strength  => ((n / 2) mod 2) = 1,
            uncertain => ((n / 4) mod 2) = 1
        );
    end function;

    -- 2-driver resolution lookup table (8x8 = 64 entries)
    -- Index: [driver1_encoded][driver2_encoded] -> resolved_encoded
    type resolve_table_t is array (0 to 7, 0 to 7) of natural;

    -- Resolution rules:
    -- - Strong drivers (strength=true, uncertain=false) dominate
    -- - Conflicting strong drivers -> X
    -- - Weak drivers combine if no strong driver
    -- - Z (no driver) is default
    constant RESOLVE_TABLE : resolve_table_t := (
        -- Second driver:  0    1    2    3    4    5    6    7
        --                000  001  010  011  100  101  110  111
        0 => (4, 4, 2, 3, 4, 4, 6, 6),  -- 000: invalid (no strength, no uncertain)
        1 => (4, 4, 2, 3, 4, 4, 6, 6),  -- 001: invalid
        2 => (2, 2, 2, 6, 2, 2, 6, 6),  -- 010: L3D_0 (strong 0)
        3 => (3, 3, 6, 3, 3, 3, 6, 6),  -- 011: L3D_1 (strong 1)
        4 => (4, 4, 2, 3, 4, 4, 6, 6),  -- 100: L3D_Z (high impedance)
        5 => (4, 4, 2, 3, 4, 4, 6, 6),  -- 101: invalid
        6 => (6, 6, 6, 6, 6, 6, 6, 6),  -- 110: L3D_X (unknown/conflict)
        7 => (6, 6, 6, 6, 6, 6, 6, 6)   -- 111: invalid
    );

    function resolved(s : logic_3d_vector) return logic_3d_t is
        variable result : natural;
    begin
        if s'length = 0 then
            return L3D_Z;
        end if;

        -- Start with first driver
        result := encode(s(s'low));

        -- Resolve with each subsequent driver using lookup table
        for i in s'low + 1 to s'high loop
            result := RESOLVE_TABLE(result, encode(s(i)));
        end loop;

        return decode(result);
    end function;

end package body;

-- Child entity that outputs L3D_1 as whole record
library work;
use work.logic3d7_pkg.all;

entity logic3d7_child is
    port (
        q : out logic_3d_r
    );
end entity;

architecture behavioral of logic3d7_child is
begin
    process
    begin
        q <= L3D_1;  -- Assign entire record
        wait;
    end process;
end architecture;

-- Parent testbench
library work;
use work.logic3d7_pkg.all;

entity logic3d7 is
end entity;

architecture test of logic3d7 is
    signal s : logic_3d_r;
begin

    uut: entity work.logic3d7_child port map (q => s);

    process
    begin
        wait for 1 ns;
        wait for 0 ns;  -- Delta cycle for signal update

        report "s.value = " & boolean'image(s.value);
        report "s.strength = " & boolean'image(s.strength);
        report "s.uncertain = " & boolean'image(s.uncertain);

        -- This is the exact bug scenario: first boolean field corrupted
        assert s.value = true
            report "BUG: s.value should be TRUE but is " & boolean'image(s.value)
            severity failure;

        assert s.strength = true
            report "BUG: s.strength should be TRUE but is " & boolean'image(s.strength)
            severity failure;

        assert s.uncertain = false
            report "BUG: s.uncertain should be FALSE but is " & boolean'image(s.uncertain)
            severity failure;

        assert s = L3D_1
            report "s should equal L3D_1"
            severity failure;

        report "PASSED: Lookup table resolution works!";
        wait;
    end process;

end architecture;
