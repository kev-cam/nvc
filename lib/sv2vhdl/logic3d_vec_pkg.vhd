-- 3D Logic Vector Package (Generic)
-- Parameterized by width for arbitrary-size vectors

library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

package logic3d_vec_pkg is
    generic (WIDTH : positive);

    ---------------------------------------------------------------------------
    -- 3-field record for vectors of WIDTH bits
    ---------------------------------------------------------------------------
    subtype vec_t is unsigned(WIDTH-1 downto 0);

    type logic3d_vec is record
        value     : vec_t;   -- Value bits
        strength  : vec_t;   -- Strength bits (1=strong)
        uncertain : vec_t;   -- Uncertain bits (1=X/Z)
    end record;

    -- All-zeros, all-ones constants
    constant ALL_0 : vec_t := (others => '0');
    constant ALL_1 : vec_t := (others => '1');

    -- Common constants
    constant L3DV_0 : logic3d_vec := (value => ALL_0, strength => ALL_1, uncertain => ALL_0);
    constant L3DV_1 : logic3d_vec := (value => ALL_1, strength => ALL_1, uncertain => ALL_0);
    constant L3DV_Z : logic3d_vec := (value => ALL_0, strength => ALL_0, uncertain => ALL_1);
    constant L3DV_X : logic3d_vec := (value => ALL_0, strength => ALL_1, uncertain => ALL_1);

    ---------------------------------------------------------------------------
    -- Gate functions (bitwise operations)
    ---------------------------------------------------------------------------
    function l3dv_not(a : logic3d_vec) return logic3d_vec;
    function l3dv_and(a, b : logic3d_vec) return logic3d_vec;
    function l3dv_or(a, b : logic3d_vec) return logic3d_vec;
    function l3dv_xor(a, b : logic3d_vec) return logic3d_vec;

    ---------------------------------------------------------------------------
    -- Utilities
    ---------------------------------------------------------------------------
    function is_one(a : logic3d_vec; bit : natural) return boolean;
    function is_zero(a : logic3d_vec; bit : natural) return boolean;
    function is_strong(a : logic3d_vec; bit : natural) return boolean;
    function is_uncertain(a : logic3d_vec; bit : natural) return boolean;

    function make_vec(val, str, unc : vec_t) return logic3d_vec;

end package;

package body logic3d_vec_pkg is

    ---------------------------------------------------------------------------
    -- NOT: invert value, preserve strength and uncertain
    ---------------------------------------------------------------------------
    function l3dv_not(a : logic3d_vec) return logic3d_vec is
    begin
        return (
            value     => not a.value,
            strength  => a.strength,
            uncertain => a.uncertain
        );
    end function;

    ---------------------------------------------------------------------------
    -- AND: 0 dominates (masks X)
    ---------------------------------------------------------------------------
    function l3dv_and(a, b : logic3d_vec) return logic3d_vec is
        variable r : logic3d_vec;
        variable a_is_0, b_is_0 : vec_t;
    begin
        -- Strong 0: value=0, strength=1, uncertain=0
        a_is_0 := (not a.value) and a.strength and (not a.uncertain);
        b_is_0 := (not b.value) and b.strength and (not b.uncertain);

        r.value := a.value and b.value;
        r.strength := (a.strength and b.strength) or a_is_0 or b_is_0;
        r.uncertain := (a.uncertain or b.uncertain) and (not (a_is_0 or b_is_0));

        return r;
    end function;

    ---------------------------------------------------------------------------
    -- OR: 1 dominates (masks X)
    ---------------------------------------------------------------------------
    function l3dv_or(a, b : logic3d_vec) return logic3d_vec is
        variable r : logic3d_vec;
        variable a_is_1, b_is_1 : vec_t;
    begin
        -- Strong 1: value=1, strength=1, uncertain=0
        a_is_1 := a.value and a.strength and (not a.uncertain);
        b_is_1 := b.value and b.strength and (not b.uncertain);

        r.value := a.value or b.value;
        r.strength := (a.strength and b.strength) or a_is_1 or b_is_1;
        r.uncertain := (a.uncertain or b.uncertain) and (not (a_is_1 or b_is_1));

        return r;
    end function;

    ---------------------------------------------------------------------------
    -- XOR: X always propagates
    ---------------------------------------------------------------------------
    function l3dv_xor(a, b : logic3d_vec) return logic3d_vec is
    begin
        return (
            value     => a.value xor b.value,
            strength  => a.strength and b.strength,
            uncertain => a.uncertain or b.uncertain
        );
    end function;

    ---------------------------------------------------------------------------
    -- Utilities
    ---------------------------------------------------------------------------
    function is_one(a : logic3d_vec; bit : natural) return boolean is
    begin
        return a.value(bit) = '1';
    end function;

    function is_zero(a : logic3d_vec; bit : natural) return boolean is
    begin
        return a.value(bit) = '0';
    end function;

    function is_strong(a : logic3d_vec; bit : natural) return boolean is
    begin
        return a.strength(bit) = '1';
    end function;

    function is_uncertain(a : logic3d_vec; bit : natural) return boolean is
    begin
        return a.uncertain(bit) = '1';
    end function;

    function make_vec(val, str, unc : vec_t) return logic3d_vec is
    begin
        return (value => val, strength => str, uncertain => unc);
    end function;

end package body;
