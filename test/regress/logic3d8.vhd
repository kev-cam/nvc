-- Test for 3D logic type: lookup table resolution with multiple drivers
-- Tests the iterative resolution: resolve(resolve(d1,d2), d3), etc.

package logic3d8_pkg is
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

    function encode(r : logic_3d_t) return natural;
    function decode(n : natural) return logic_3d_t;
    function resolved(s : logic_3d_vector) return logic_3d_t;
    subtype logic_3d_r is resolved logic_3d_t;
end package;

package body logic3d8_pkg is

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

    type resolve_table_t is array (0 to 7, 0 to 7) of natural;

    -- Resolution rules:
    -- - Strong 0 and Strong 1 conflict -> X
    -- - Strong beats Z
    -- - Z with Z stays Z
    constant RESOLVE_TABLE : resolve_table_t := (
        -- Second driver:  0    1    2    3    4    5    6    7
        --                000  001  010  011  100  101  110  111
        0 => (4, 4, 2, 3, 4, 4, 6, 6),  -- 000: treat as Z
        1 => (4, 4, 2, 3, 4, 4, 6, 6),  -- 001: treat as Z
        2 => (2, 2, 2, 6, 2, 2, 6, 6),  -- 010: L3D_0 (strong 0)
        3 => (3, 3, 6, 3, 3, 3, 6, 6),  -- 011: L3D_1 (strong 1)
        4 => (4, 4, 2, 3, 4, 4, 6, 6),  -- 100: L3D_Z (high impedance)
        5 => (4, 4, 2, 3, 4, 4, 6, 6),  -- 101: treat as Z
        6 => (6, 6, 6, 6, 6, 6, 6, 6),  -- 110: L3D_X (conflict propagates)
        7 => (6, 6, 6, 6, 6, 6, 6, 6)   -- 111: treat as X
    );

    function resolved(s : logic_3d_vector) return logic_3d_t is
        variable result : natural;
    begin
        if s'length = 0 then
            return L3D_Z;
        end if;

        result := encode(s(s'low));
        for i in s'low + 1 to s'high loop
            result := RESOLVE_TABLE(result, encode(s(i)));
        end loop;

        return decode(result);
    end function;

end package body;

-- Driver entities
library work;
use work.logic3d8_pkg.all;

entity logic3d8_drv is
    generic (drive_value : logic_3d_t := L3D_Z);
    port (q : out logic_3d_r);
end entity;

architecture behavioral of logic3d8_drv is
begin
    q <= drive_value;
end architecture;

-- Testbench
library work;
use work.logic3d8_pkg.all;

entity logic3d8 is
end entity;

architecture test of logic3d8 is
    signal s2 : logic_3d_r;  -- 2 drivers
    signal s3 : logic_3d_r;  -- 3 drivers
    signal s4 : logic_3d_r;  -- 4 drivers
begin

    -- Test with 2 drivers: Z and 1 -> should be 1
    drv2a: entity work.logic3d8_drv generic map (L3D_Z) port map (s2);
    drv2b: entity work.logic3d8_drv generic map (L3D_1) port map (s2);

    -- Test with 3 drivers: Z, Z, 0 -> should be 0
    drv3a: entity work.logic3d8_drv generic map (L3D_Z) port map (s3);
    drv3b: entity work.logic3d8_drv generic map (L3D_Z) port map (s3);
    drv3c: entity work.logic3d8_drv generic map (L3D_0) port map (s3);

    -- Test with 4 drivers: Z, 1, Z, 1 -> should be 1
    drv4a: entity work.logic3d8_drv generic map (L3D_Z) port map (s4);
    drv4b: entity work.logic3d8_drv generic map (L3D_1) port map (s4);
    drv4c: entity work.logic3d8_drv generic map (L3D_Z) port map (s4);
    drv4d: entity work.logic3d8_drv generic map (L3D_1) port map (s4);

    process
    begin
        wait for 1 ns;
        wait for 0 ns;

        -- Test 2 drivers: Z + 1 = 1
        report "2 drivers (Z, 1): s2 = (" &
            boolean'image(s2.value) & ", " &
            boolean'image(s2.strength) & ", " &
            boolean'image(s2.uncertain) & ")";
        assert s2 = L3D_1
            report "FAIL: 2 drivers Z+1 should give L3D_1"
            severity failure;

        -- Test 3 drivers: Z + Z + 0 = 0
        report "3 drivers (Z, Z, 0): s3 = (" &
            boolean'image(s3.value) & ", " &
            boolean'image(s3.strength) & ", " &
            boolean'image(s3.uncertain) & ")";
        assert s3 = L3D_0
            report "FAIL: 3 drivers Z+Z+0 should give L3D_0"
            severity failure;

        -- Test 4 drivers: Z + 1 + Z + 1 = 1
        report "4 drivers (Z, 1, Z, 1): s4 = (" &
            boolean'image(s4.value) & ", " &
            boolean'image(s4.strength) & ", " &
            boolean'image(s4.uncertain) & ")";
        assert s4 = L3D_1
            report "FAIL: 4 drivers Z+1+Z+1 should give L3D_1"
            severity failure;

        report "PASSED: Multiple driver resolution works!";
        wait;
    end process;

end architecture;
