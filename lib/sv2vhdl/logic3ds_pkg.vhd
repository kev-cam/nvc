-- Strength-Aware 3D Logic Type Package (logic3ds)
-- 4-byte record companion to 3-bit logic3d for tran-level resolution.
-- Gate primitives use fast logic3d; tran networks promote to logic3ds
-- for strength-based resolution, then demote back for receivers.
--
-- Encoding: 4-byte record
--   Byte 0: value      natural range 0 to 255  (0=logic 0, 255=logic 1)
--   Byte 1: strength   l3ds_strength (0-127)     (power-of-2 bit encoding)
--   Byte 2: flags      l3ds_flags enum          (uncertainty + power state)
--   Byte 3: reserved   natural range 0 to 255   (zero for now)

library ieee;
use ieee.std_logic_1164.all;
use work.logic3d_types_pkg.all;

package logic3ds_pkg is

    ---------------------------------------------------------------------------
    -- Strength levels (IEEE 1800-2017 Section 7.8)
    -- Bit encoding: bit 0 = capacitive modifier, bits 1-4 = drive level.
    -- Capacitive variant of a level = level OR ST_CAP.
    -- Ordering: within same base level, active drive > capacitive charge.
    -- Between levels, higher base wins.  Use str_gt() for comparison.
    ---------------------------------------------------------------------------
    subtype l3ds_strength is natural range 0 to 31;

    -- Bit fields
    constant ST_CAP    : l3ds_strength := 1;   -- Capacitive modifier

    -- Active drive levels (bit 0 = 0)
    constant ST_HIGHZ  : l3ds_strength := 0;   -- No drive
    constant ST_WEAK   : l3ds_strength := 2;   -- Weak drive
    constant ST_PULL   : l3ds_strength := 4;   -- Pull drive
    constant ST_STRONG : l3ds_strength := 8;   -- Strong drive
    constant ST_SUPPLY : l3ds_strength := 16;  -- Supply drive

    -- Capacitive strengths (drive level OR ST_CAP)
    constant ST_SMALL  : l3ds_strength := 3;   -- ST_WEAK  + ST_CAP
    constant ST_MEDIUM : l3ds_strength := 5;   -- ST_PULL  + ST_CAP
    constant ST_LARGE  : l3ds_strength := 9;   -- ST_STRONG + ST_CAP

    ---------------------------------------------------------------------------
    -- Flags for uncertainty and power state
    ---------------------------------------------------------------------------
    type l3ds_flags is (
        FL_KNOWN,       -- 0: Value is determined
        FL_UNKNOWN,     -- 1: X - conflicting or indeterminate
        FL_UNDRIVEN,    -- 2: Z - no active driver
        FL_NOPOWER,     -- 3: Power supply missing
        FL_UNK_NOPOWER, -- 4: Unknown + no power
        FL_UDR_NOPOWER  -- 5: Undriven + no power
    );

    ---------------------------------------------------------------------------
    -- 4-byte strength-aware logic type
    ---------------------------------------------------------------------------
    type logic3ds is record
        value    : natural range 0 to 255;   -- 0=logic 0, 255=logic 1
        strength : l3ds_strength;             -- Drive strength
        flags    : l3ds_flags;                -- Uncertainty / power state
        reserved : natural range 0 to 255;   -- Zero (future use)
    end record;

    ---------------------------------------------------------------------------
    -- Vector type for resolution functions
    ---------------------------------------------------------------------------
    type logic3ds_vector is array (natural range <>) of logic3ds;

    ---------------------------------------------------------------------------
    -- Constants
    ---------------------------------------------------------------------------
    -- Undriven / unknown
    constant L3DS_Z : logic3ds := (value => 0,   strength => ST_HIGHZ,  flags => FL_UNDRIVEN, reserved => 0);
    constant L3DS_X : logic3ds := (value => 0,   strength => ST_STRONG, flags => FL_UNKNOWN,  reserved => 0);

    -- Strong drive
    constant L3DS_0     : logic3ds := (value => 0,   strength => ST_STRONG, flags => FL_KNOWN, reserved => 0);
    constant L3DS_1     : logic3ds := (value => 255, strength => ST_STRONG, flags => FL_KNOWN, reserved => 0);

    -- Pull drive
    constant L3DS_PULL0 : logic3ds := (value => 0,   strength => ST_PULL,   flags => FL_KNOWN, reserved => 0);
    constant L3DS_PULL1 : logic3ds := (value => 255, strength => ST_PULL,   flags => FL_KNOWN, reserved => 0);

    -- Weak drive
    constant L3DS_WEAK0 : logic3ds := (value => 0,   strength => ST_WEAK,   flags => FL_KNOWN, reserved => 0);
    constant L3DS_WEAK1 : logic3ds := (value => 255, strength => ST_WEAK,   flags => FL_KNOWN, reserved => 0);

    -- Supply drive
    constant L3DS_SU0   : logic3ds := (value => 0,   strength => ST_SUPPLY, flags => FL_KNOWN, reserved => 0);
    constant L3DS_SU1   : logic3ds := (value => 255, strength => ST_SUPPLY, flags => FL_KNOWN, reserved => 0);

    ---------------------------------------------------------------------------
    -- Strength comparison (respects capacitive < active at same base level)
    ---------------------------------------------------------------------------
    function str_gt(a, b : l3ds_strength) return boolean;
    function str_ge(a, b : l3ds_strength) return boolean;
    function is_capacitive(s : l3ds_strength) return boolean;

    ---------------------------------------------------------------------------
    -- Conversion functions
    ---------------------------------------------------------------------------
    function to_logic3ds(a : logic3d; str : l3ds_strength) return logic3ds;
    function to_logic3d(a : logic3ds) return logic3d;
    function to_std_logic(a : logic3ds) return std_logic;

    ---------------------------------------------------------------------------
    -- Resolution function (IEEE 1800-2017 Section 28.12)
    ---------------------------------------------------------------------------
    function l3ds_resolve(drivers : logic3ds_vector) return logic3ds;

    ---------------------------------------------------------------------------
    -- Query functions
    ---------------------------------------------------------------------------
    function is_logic_one(a : logic3ds) return boolean;
    function is_logic_zero(a : logic3ds) return boolean;
    function get_strength(a : logic3ds) return l3ds_strength;
    function is_driven(a : logic3ds) return boolean;
    function is_supply(a : logic3ds) return boolean;

    ---------------------------------------------------------------------------
    -- Builder functions
    ---------------------------------------------------------------------------
    function make_logic3ds(val : natural; str : l3ds_strength; fl : l3ds_flags) return logic3ds;
    function l3ds_drive(value : boolean; str : l3ds_strength) return logic3ds;

    ---------------------------------------------------------------------------
    -- Strength manipulation
    ---------------------------------------------------------------------------
    function weaken_strength(s : l3ds_strength) return l3ds_strength;
    function l3ds_weaken(a : logic3ds) return logic3ds;
    function l3ds_set_unknown(a : logic3ds) return logic3ds;

end package;

package body logic3ds_pkg is

    ---------------------------------------------------------------------------
    -- Strength comparison: compare by base level (s/2), then capacitive
    -- is weaker than active drive at the same base.
    -- Ordering: HIGHZ < SMALL < WEAK < MEDIUM < PULL < LARGE < STRONG < SUPPLY
    ---------------------------------------------------------------------------
    function str_gt(a, b : l3ds_strength) return boolean is
        variable a_base : natural := a / 2;
        variable b_base : natural := b / 2;
    begin
        if a_base /= b_base then
            return a_base > b_base;
        else
            -- Same base: active drive (cap=0) beats capacitive (cap=1)
            return (a mod 2) < (b mod 2);
        end if;
    end function;

    function str_ge(a, b : l3ds_strength) return boolean is
    begin
        return a = b or str_gt(a, b);
    end function;

    function is_capacitive(s : l3ds_strength) return boolean is
    begin
        return (s mod 2) = 1;
    end function;

    ---------------------------------------------------------------------------
    -- Promote: logic3d -> logic3ds with given drive strength
    -- L/H retain ST_WEAK regardless of str (they encode weak drive)
    -- Z/U retain ST_HIGHZ (no active driver)
    ---------------------------------------------------------------------------
    function to_logic3ds(a : logic3d; str : l3ds_strength) return logic3ds is
    begin
        case a is
            when L3D_0 =>
                return (value => 0,   strength => str,      flags => FL_KNOWN,    reserved => 0);
            when L3D_1 =>
                return (value => 255, strength => str,      flags => FL_KNOWN,    reserved => 0);
            when L3D_L =>
                return (value => 0,   strength => ST_WEAK,  flags => FL_KNOWN,    reserved => 0);
            when L3D_H =>
                return (value => 255, strength => ST_WEAK,  flags => FL_KNOWN,    reserved => 0);
            when L3D_Z =>
                return (value => 0,   strength => ST_HIGHZ, flags => FL_UNDRIVEN, reserved => 0);
            when L3D_X =>
                return (value => 0,   strength => str,      flags => FL_UNKNOWN,  reserved => 0);
            when L3D_W =>
                return (value => 0,   strength => ST_WEAK,  flags => FL_UNKNOWN,  reserved => 0);
            when L3D_U =>
                return (value => 0,   strength => ST_HIGHZ, flags => FL_UNKNOWN,  reserved => 0);
            when others =>
                return (value => 0,   strength => ST_HIGHZ, flags => FL_UNKNOWN,  reserved => 0);
        end case;
    end function;

    ---------------------------------------------------------------------------
    -- Demote: logic3ds -> logic3d (threshold + strength mapping)
    -- Value threshold: >= 127 is logic 1, < 127 is logic 0
    -- Strength boundary: > ST_WEAK maps to strong (driven), <= ST_WEAK to weak
    ---------------------------------------------------------------------------
    function to_logic3d(a : logic3ds) return logic3d is
    begin
        case a.flags is
            when FL_UNDRIVEN | FL_UDR_NOPOWER =>
                return L3D_Z;
            when FL_UNKNOWN | FL_UNK_NOPOWER =>
                if str_gt(a.strength, ST_WEAK) then
                    return L3D_X;
                else
                    return L3D_W;
                end if;
            when FL_KNOWN =>
                if str_gt(a.strength, ST_WEAK) then
                    if a.value >= 127 then
                        return L3D_1;
                    else
                        return L3D_0;
                    end if;
                else
                    if a.value >= 127 then
                        return L3D_H;
                    else
                        return L3D_L;
                    end if;
                end if;
            when FL_NOPOWER =>
                return L3D_X;
        end case;
    end function;

    ---------------------------------------------------------------------------
    -- Convert to std_logic (via logic3d)
    ---------------------------------------------------------------------------
    function to_std_logic(a : logic3ds) return std_logic is
    begin
        return work.logic3d_types_pkg.to_std_logic(to_logic3d(a));
    end function;

    ---------------------------------------------------------------------------
    -- Strength resolution (IEEE 1800-2017 Section 28.12)
    --
    -- 1. Find strongest driver(s), ignoring HIGHZ
    -- 2. Single strongest driver -> its value wins
    -- 3. Multiple strongest, same value -> that value at that strength
    -- 4. Multiple strongest, opposing values -> X at that strength
    -- 5. No non-highz drivers -> Z
    -- 6. Propagate FL_NOPOWER if any supply driver has it
    ---------------------------------------------------------------------------
    function l3ds_resolve(drivers : logic3ds_vector) return logic3ds is
        variable best_str       : l3ds_strength := ST_HIGHZ;
        variable best_val       : natural range 0 to 255 := 0;
        variable best_flags     : l3ds_flags := FL_UNDRIVEN;
        variable contention     : boolean := false;
        variable has_power_issue : boolean := false;
        variable found_any      : boolean := false;
    begin
        for i in drivers'range loop
            -- Skip undriven / highz drivers
            if drivers(i).strength /= ST_HIGHZ and
               drivers(i).flags /= FL_UNDRIVEN and
               drivers(i).flags /= FL_UDR_NOPOWER then

                if not found_any or str_gt(drivers(i).strength, best_str) then
                    -- New strongest driver
                    best_str   := drivers(i).strength;
                    best_val   := drivers(i).value;
                    best_flags := drivers(i).flags;
                    contention := false;
                    found_any  := true;
                elsif drivers(i).strength = best_str then
                    -- Equal strength: check for contention
                    if drivers(i).flags = FL_UNKNOWN or best_flags = FL_UNKNOWN then
                        -- Either driver is X -> result is X
                        best_flags := FL_UNKNOWN;
                        contention := false;
                    elsif drivers(i).value /= best_val then
                        -- Opposing values at same strength -> contention
                        contention := true;
                    end if;
                end if;

                -- Track power issues from supply drivers
                if drivers(i).strength = ST_SUPPLY and
                   (drivers(i).flags = FL_NOPOWER or
                    drivers(i).flags = FL_UNK_NOPOWER or
                    drivers(i).flags = FL_UDR_NOPOWER) then
                    has_power_issue := true;
                end if;
            end if;
        end loop;

        -- No active drivers -> Z
        if not found_any then
            return L3DS_Z;
        end if;

        -- Contention at best strength -> X
        if contention then
            best_flags := FL_UNKNOWN;
        end if;

        -- Propagate power issue
        if has_power_issue then
            if best_flags = FL_UNKNOWN then
                best_flags := FL_UNK_NOPOWER;
            elsif best_flags = FL_UNDRIVEN then
                best_flags := FL_UDR_NOPOWER;
            else
                best_flags := FL_NOPOWER;
            end if;
        end if;

        return (value => best_val, strength => best_str,
                flags => best_flags, reserved => 0);
    end function;

    ---------------------------------------------------------------------------
    -- Query functions
    ---------------------------------------------------------------------------
    function is_logic_one(a : logic3ds) return boolean is
    begin
        return a.value >= 127;
    end function;

    function is_logic_zero(a : logic3ds) return boolean is
    begin
        return a.value < 127;
    end function;

    function get_strength(a : logic3ds) return l3ds_strength is
    begin
        return a.strength;
    end function;

    function is_driven(a : logic3ds) return boolean is
    begin
        return a.strength > ST_HIGHZ and a.flags = FL_KNOWN;
    end function;

    function is_supply(a : logic3ds) return boolean is
    begin
        return a.strength = ST_SUPPLY;
    end function;

    ---------------------------------------------------------------------------
    -- Builder functions
    ---------------------------------------------------------------------------
    function make_logic3ds(val : natural; str : l3ds_strength; fl : l3ds_flags) return logic3ds is
    begin
        return (value => val, strength => str, flags => fl, reserved => 0);
    end function;

    function l3ds_drive(value : boolean; str : l3ds_strength) return logic3ds is
    begin
        if value then
            return (value => 255, strength => str, flags => FL_KNOWN, reserved => 0);
        else
            return (value => 0,   strength => str, flags => FL_KNOWN, reserved => 0);
        end if;
    end function;

    ---------------------------------------------------------------------------
    -- Strength manipulation
    ---------------------------------------------------------------------------

    -- Reduce strength one drive level (for resistive tran gates)
    -- supply->strong, strong->pull, pull->weak, weak->small(weak|cap)
    -- Capacitive variants reduce similarly: large->medium, medium->small
    function weaken_strength(s : l3ds_strength) return l3ds_strength is
    begin
        case s is
            when ST_SUPPLY => return ST_STRONG;
            when ST_LARGE  => return ST_MEDIUM;   -- strong|cap -> pull|cap
            when ST_STRONG => return ST_PULL;
            when ST_MEDIUM => return ST_SMALL;     -- pull|cap -> weak|cap
            when ST_PULL   => return ST_WEAK;
            when ST_SMALL  => return ST_HIGHZ;     -- weak|cap -> highz
            when ST_WEAK   => return ST_SMALL;     -- weak -> weak|cap (small)
            when others    => return ST_HIGHZ;
        end case;
    end function;

    -- Weaken entire logic3ds signal (reduce strength, preserve value/flags)
    function l3ds_weaken(a : logic3ds) return logic3ds is
    begin
        return (value => a.value, strength => weaken_strength(a.strength),
                flags => a.flags, reserved => 0);
    end function;

    -- Mark signal as unknown (preserve value and strength)
    function l3ds_set_unknown(a : logic3ds) return logic3ds is
    begin
        return (value => a.value, strength => a.strength,
                flags => FL_UNKNOWN, reserved => 0);
    end function;

end package body;
