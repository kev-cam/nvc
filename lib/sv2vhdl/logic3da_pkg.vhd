-- Thevenin-Equivalent Analog Signal Type Package (logic3da)
-- Mixed-signal companion to logic3d/logic3ds for analog simulation.
-- Represents signals as Thevenin-equivalent voltage-resistance pairs.
-- Resolution is parallel Thevenin combination:
--   V_out = sum(Vi/Ri) / sum(1/Ri)
--   R_out = 1 / sum(1/Ri)
--
-- Digital primitives use logic3d (3-bit fast) or logic3ds (strength-aware).
-- Analog components use logic3da for voltage-domain simulation.
-- Conversion functions bridge digital <-> analog domains.

library ieee;
use ieee.std_logic_1164.all;
use ieee.math_real.all;
use work.logic3d_types_pkg.all;
use work.logic3ds_pkg.all;

package logic3da_pkg is

    ---------------------------------------------------------------------------
    -- Flags enumeration
    -- AFL_ prefix avoids collision with l3ds_flags (FL_) when both are used
    ---------------------------------------------------------------------------
    type l3da_flags is (
        AFL_KNOWN,       -- 0: Normal driven, voltage/resistance valid
        AFL_UNKNOWN,     -- 1: X - conflicting or indeterminate
        AFL_UNDRIVEN,    -- 2: Z - no active driver (R = infinity)
        AFL_NOPOWER,     -- 3: Power supply missing
        AFL_UNK_NOPOWER, -- 4: Unknown + no power
        AFL_UDR_NOPOWER, -- 5: Undriven + no power
        AFL_PWL          -- 6: PWL marker - voltage/resistance are current
                         --    segment values of a piecewise-linear source
    );

    ---------------------------------------------------------------------------
    -- 3-field record: Thevenin voltage, resistance, and state flags
    ---------------------------------------------------------------------------
    type logic3da is record
        voltage    : real;       -- Thevenin equivalent voltage (volts)
        resistance : real;       -- Thevenin equivalent resistance (ohms)
        flags      : l3da_flags; -- State flags
    end record;

    ---------------------------------------------------------------------------
    -- Vector type for resolution functions
    ---------------------------------------------------------------------------
    type logic3da_vector is array (natural range <>) of logic3da;

    ---------------------------------------------------------------------------
    -- Reference voltage (nominal VDD for digital-analog boundary)
    ---------------------------------------------------------------------------
    constant VDD           : real := 1.0;   -- Default logic VDD (1.0V)
    constant VDD_THRESHOLD : real := 0.5;   -- V > this -> logic 1

    ---------------------------------------------------------------------------
    -- Resistance constants for digital strength levels
    -- Maps IEEE 1800 strength levels to physical resistance values.
    -- Lower R = stronger drive.  Higher R = weaker drive.
    ---------------------------------------------------------------------------
    constant R_SUPPLY : real := 0.0;      -- Ideal voltage source (0 ohm)
    constant R_STRONG : real := 100.0;    -- Strong drive (typical gate output)
    constant R_PULL   : real := 1.0e3;    -- Pull drive (1k ohm)
    constant R_WEAK   : real := 10.0e3;   -- Weak drive (10k ohm)
    constant R_HIGHZ  : real := 1.0e15;   -- High impedance (effectively inf)

    -- Threshold for "effectively infinite" resistance
    constant R_OPEN   : real := 1.0e12;   -- 1 teraohm

    -- Threshold for "effectively zero" resistance (ideal source)
    constant R_SHORT  : real := 1.0e-6;   -- 1 micro-ohm

    ---------------------------------------------------------------------------
    -- Common signal constants
    ---------------------------------------------------------------------------
    constant L3DA_Z     : logic3da := (voltage => 0.0, resistance => R_HIGHZ,  flags => AFL_UNDRIVEN);
    constant L3DA_X     : logic3da := (voltage => 0.0, resistance => R_STRONG, flags => AFL_UNKNOWN);
    constant L3DA_0     : logic3da := (voltage => 0.0, resistance => R_STRONG, flags => AFL_KNOWN);
    constant L3DA_1     : logic3da := (voltage => VDD, resistance => R_STRONG, flags => AFL_KNOWN);
    constant L3DA_PULL0 : logic3da := (voltage => 0.0, resistance => R_PULL,   flags => AFL_KNOWN);
    constant L3DA_PULL1 : logic3da := (voltage => VDD, resistance => R_PULL,   flags => AFL_KNOWN);
    constant L3DA_WEAK0 : logic3da := (voltage => 0.0, resistance => R_WEAK,   flags => AFL_KNOWN);
    constant L3DA_WEAK1 : logic3da := (voltage => VDD, resistance => R_WEAK,   flags => AFL_KNOWN);
    constant L3DA_SU0   : logic3da := (voltage => 0.0, resistance => R_SUPPLY, flags => AFL_KNOWN);
    constant L3DA_SU1   : logic3da := (voltage => VDD, resistance => R_SUPPLY, flags => AFL_KNOWN);

    ---------------------------------------------------------------------------
    -- Resolution function: Parallel Thevenin combination
    --
    -- For N drivers with voltages Vi and resistances Ri:
    --   V_out = sum(Vi/Ri) / sum(1/Ri)
    --   R_out = 1 / sum(1/Ri)
    --
    -- Edge cases:
    --   All drivers undriven (R >= R_OPEN)        -> L3DA_Z
    --   Single ideal source (R <= R_SHORT)        -> that source's V, R = 0
    --   Multiple ideal sources, same voltage      -> that V, R = 0
    --   Multiple ideal sources, different voltage -> AFL_UNKNOWN (contention)
    --   Any AFL_UNKNOWN driver with R < R_OPEN    -> propagates unknown
    --   Any AFL_PWL driver                        -> result is AFL_PWL
    ---------------------------------------------------------------------------
    function l3da_resolve(drivers : logic3da_vector) return logic3da;

    ---------------------------------------------------------------------------
    -- Resolved subtype: use for signals with multiple drivers
    ---------------------------------------------------------------------------
    subtype resolved_logic3da is l3da_resolve logic3da;

    ---------------------------------------------------------------------------
    -- Digital -> Analog conversions
    ---------------------------------------------------------------------------
    function to_logic3da(a : logic3d) return logic3da;
    function to_logic3da(a : logic3ds) return logic3da;

    ---------------------------------------------------------------------------
    -- Analog -> Digital conversions (lossy: continuous to discrete)
    ---------------------------------------------------------------------------
    function to_logic3d(a : logic3da) return logic3d;
    function to_logic3ds(a : logic3da) return logic3ds;
    function to_std_logic(a : logic3da) return std_logic;

    ---------------------------------------------------------------------------
    -- Strength-to-resistance bridge
    ---------------------------------------------------------------------------
    function strength_to_resistance(s : l3ds_strength) return real;
    function resistance_to_strength(r : real) return l3ds_strength;

    ---------------------------------------------------------------------------
    -- Query functions
    ---------------------------------------------------------------------------
    function is_driven(a : logic3da) return boolean;
    function is_unknown(a : logic3da) return boolean;
    function is_pwl(a : logic3da) return boolean;
    function is_ideal_source(a : logic3da) return boolean;
    function get_voltage(a : logic3da) return real;
    function get_resistance(a : logic3da) return real;

    ---------------------------------------------------------------------------
    -- Builder functions
    ---------------------------------------------------------------------------
    function make_logic3da(v : real; r : real; f : l3da_flags) return logic3da;
    function l3da_drive(v : real; r : real) return logic3da;
    function l3da_pwl(v : real; r : real) return logic3da;

end package;

package body logic3da_pkg is

    ---------------------------------------------------------------------------
    -- Resolution function
    ---------------------------------------------------------------------------
    function l3da_resolve(drivers : logic3da_vector) return logic3da is
        variable sum_g       : real := 0.0;     -- sum of conductances (1/Ri)
        variable sum_vg      : real := 0.0;     -- sum of Vi/Ri
        variable found_any   : boolean := false;
        variable has_unknown : boolean := false;
        variable has_pwl     : boolean := false;
        variable has_power   : boolean := false;
        variable ideal_v     : real := 0.0;
        variable ideal_count : natural := 0;
        variable ideal_agree : boolean := true;
        variable g           : real;
        variable v_out       : real;
        variable r_out       : real;
        variable fl          : l3da_flags;
    begin
        if drivers'length = 1 then
            return drivers(drivers'low);
        end if;

        for i in drivers'range loop
            -- Skip undriven drivers
            if drivers(i).resistance < R_OPEN and
               drivers(i).flags /= AFL_UNDRIVEN and
               drivers(i).flags /= AFL_UDR_NOPOWER then

                found_any := true;

                -- Track special flags
                if drivers(i).flags = AFL_UNKNOWN or
                   drivers(i).flags = AFL_UNK_NOPOWER then
                    has_unknown := true;
                end if;
                if drivers(i).flags = AFL_PWL then
                    has_pwl := true;
                end if;
                if drivers(i).flags = AFL_NOPOWER or
                   drivers(i).flags = AFL_UNK_NOPOWER or
                   drivers(i).flags = AFL_UDR_NOPOWER then
                    has_power := true;
                end if;

                -- Handle ideal voltage sources (R <= R_SHORT)
                if drivers(i).resistance <= R_SHORT then
                    ideal_count := ideal_count + 1;
                    if ideal_count = 1 then
                        ideal_v := drivers(i).voltage;
                    elsif abs(drivers(i).voltage - ideal_v) > 1.0e-9 then
                        ideal_agree := false;
                    end if;
                else
                    -- Normal resistive driver: accumulate conductance sums
                    g := 1.0 / drivers(i).resistance;
                    sum_g := sum_g + g;
                    sum_vg := sum_vg + drivers(i).voltage * g;
                end if;
            end if;
        end loop;

        -- No active drivers -> Z
        if not found_any then
            return L3DA_Z;
        end if;

        -- Unknown propagation
        if has_unknown then
            if ideal_count > 0 then
                return (voltage => 0.0, resistance => R_SUPPLY,
                        flags => AFL_UNKNOWN);
            elsif sum_g > 0.0 then
                return (voltage => 0.0, resistance => 1.0 / sum_g,
                        flags => AFL_UNKNOWN);
            else
                return (voltage => 0.0, resistance => R_STRONG,
                        flags => AFL_UNKNOWN);
            end if;
        end if;

        -- Ideal source contention (different voltages at R=0)
        if ideal_count > 1 and not ideal_agree then
            return (voltage => 0.0, resistance => R_SUPPLY,
                    flags => AFL_UNKNOWN);
        end if;

        -- Determine output flag
        if has_pwl then
            fl := AFL_PWL;
        elsif has_power then
            fl := AFL_NOPOWER;
        else
            fl := AFL_KNOWN;
        end if;

        -- Ideal source(s) dominate all resistive drivers
        if ideal_count > 0 then
            return (voltage => ideal_v, resistance => R_SUPPLY, flags => fl);
        end if;

        -- Standard Thevenin combination
        -- V_out = sum(Vi/Ri) / sum(1/Ri)
        -- R_out = 1 / sum(1/Ri)
        v_out := sum_vg / sum_g;
        r_out := 1.0 / sum_g;

        return (voltage => v_out, resistance => r_out, flags => fl);
    end function;

    ---------------------------------------------------------------------------
    -- Strength-to-resistance bridge
    ---------------------------------------------------------------------------
    function strength_to_resistance(s : l3ds_strength) return real is
    begin
        case s is
            when ST_SUPPLY => return R_SUPPLY;
            when ST_STRONG => return R_STRONG;
            when ST_LARGE  => return R_STRONG * 2.0;
            when ST_PULL   => return R_PULL;
            when ST_MEDIUM => return R_PULL * 2.0;
            when ST_WEAK   => return R_WEAK;
            when ST_SMALL  => return R_WEAK * 2.0;
            when ST_HIGHZ  => return R_HIGHZ;
            when others    => return R_HIGHZ;
        end case;
    end function;

    function resistance_to_strength(r : real) return l3ds_strength is
    begin
        if r <= R_SHORT then
            return ST_SUPPLY;
        elsif r <= R_STRONG * 5.0 then
            return ST_STRONG;
        elsif r <= R_PULL * 5.0 then
            return ST_PULL;
        elsif r <= R_WEAK * 5.0 then
            return ST_WEAK;
        else
            return ST_HIGHZ;
        end if;
    end function;

    ---------------------------------------------------------------------------
    -- Digital -> Analog: logic3d -> logic3da
    ---------------------------------------------------------------------------
    function to_logic3da(a : logic3d) return logic3da is
    begin
        case a is
            when L3D_0 => return L3DA_0;
            when L3D_1 => return L3DA_1;
            when L3D_L => return L3DA_WEAK0;
            when L3D_H => return L3DA_WEAK1;
            when L3D_Z => return L3DA_Z;
            when L3D_X => return L3DA_X;
            when L3D_W =>
                return (voltage => 0.0, resistance => R_WEAK,
                        flags => AFL_UNKNOWN);
            when L3D_U =>
                return (voltage => 0.0, resistance => R_HIGHZ,
                        flags => AFL_UNKNOWN);
            when others =>
                return (voltage => 0.0, resistance => R_HIGHZ,
                        flags => AFL_UNKNOWN);
        end case;
    end function;

    ---------------------------------------------------------------------------
    -- Digital -> Analog: logic3ds -> logic3da
    ---------------------------------------------------------------------------
    function to_logic3da(a : logic3ds) return logic3da is
        variable v : real;
        variable r : real;
        variable f : l3da_flags;
    begin
        -- Map value (0-255) to voltage (0.0 - VDD)
        v := VDD * real(a.value) / 255.0;

        -- Map strength to resistance
        r := strength_to_resistance(a.strength);

        -- Map flags (l3ds_flags -> l3da_flags)
        case a.flags is
            when FL_KNOWN       => f := AFL_KNOWN;
            when FL_UNKNOWN     => f := AFL_UNKNOWN;
            when FL_UNDRIVEN    => f := AFL_UNDRIVEN;
            when FL_NOPOWER     => f := AFL_NOPOWER;
            when FL_UNK_NOPOWER => f := AFL_UNK_NOPOWER;
            when FL_UDR_NOPOWER => f := AFL_UDR_NOPOWER;
        end case;

        return (voltage => v, resistance => r, flags => f);
    end function;

    ---------------------------------------------------------------------------
    -- Analog -> Digital: logic3da -> logic3d
    ---------------------------------------------------------------------------
    function to_logic3d(a : logic3da) return logic3d is
    begin
        case a.flags is
            when AFL_UNDRIVEN | AFL_UDR_NOPOWER =>
                return L3D_Z;
            when AFL_UNKNOWN | AFL_UNK_NOPOWER =>
                if a.resistance <= R_PULL then
                    return L3D_X;
                else
                    return L3D_W;
                end if;
            when AFL_KNOWN | AFL_PWL =>
                if a.resistance >= R_OPEN then
                    return L3D_Z;
                elsif a.resistance > R_PULL then
                    if a.voltage > VDD_THRESHOLD then
                        return L3D_H;
                    else
                        return L3D_L;
                    end if;
                else
                    if a.voltage > VDD_THRESHOLD then
                        return L3D_1;
                    else
                        return L3D_0;
                    end if;
                end if;
            when AFL_NOPOWER =>
                return L3D_X;
        end case;
    end function;

    ---------------------------------------------------------------------------
    -- Analog -> Digital: logic3da -> logic3ds
    ---------------------------------------------------------------------------
    function to_logic3ds(a : logic3da) return logic3ds is
        variable val : natural range 0 to 255;
        variable str : l3ds_strength;
        variable fl  : work.logic3ds_pkg.l3ds_flags;
    begin
        -- Map voltage to 8-bit value
        if a.voltage <= 0.0 then
            val := 0;
        elsif a.voltage >= VDD then
            val := 255;
        else
            val := natural(a.voltage / VDD * 255.0);
        end if;

        -- Map resistance to strength
        str := resistance_to_strength(a.resistance);

        -- Map flags
        case a.flags is
            when AFL_KNOWN       => fl := FL_KNOWN;
            when AFL_PWL         => fl := FL_KNOWN;
            when AFL_UNKNOWN     => fl := FL_UNKNOWN;
            when AFL_UNDRIVEN    => fl := FL_UNDRIVEN;
            when AFL_NOPOWER     => fl := FL_NOPOWER;
            when AFL_UNK_NOPOWER => fl := FL_UNK_NOPOWER;
            when AFL_UDR_NOPOWER => fl := FL_UDR_NOPOWER;
        end case;

        return (value => val, strength => str, flags => fl, reserved => 0);
    end function;

    ---------------------------------------------------------------------------
    -- Analog -> std_logic (via logic3d)
    ---------------------------------------------------------------------------
    function to_std_logic(a : logic3da) return std_logic is
    begin
        return work.logic3d_types_pkg.to_std_logic(to_logic3d(a));
    end function;

    ---------------------------------------------------------------------------
    -- Query functions
    ---------------------------------------------------------------------------
    function is_driven(a : logic3da) return boolean is
    begin
        return a.flags = AFL_KNOWN or a.flags = AFL_PWL;
    end function;

    function is_unknown(a : logic3da) return boolean is
    begin
        return a.flags = AFL_UNKNOWN or a.flags = AFL_UNK_NOPOWER;
    end function;

    function is_pwl(a : logic3da) return boolean is
    begin
        return a.flags = AFL_PWL;
    end function;

    function is_ideal_source(a : logic3da) return boolean is
    begin
        return a.resistance <= R_SHORT;
    end function;

    function get_voltage(a : logic3da) return real is
    begin
        return a.voltage;
    end function;

    function get_resistance(a : logic3da) return real is
    begin
        return a.resistance;
    end function;

    ---------------------------------------------------------------------------
    -- Builder functions
    ---------------------------------------------------------------------------
    function make_logic3da(v : real; r : real; f : l3da_flags) return logic3da is
    begin
        return (voltage => v, resistance => r, flags => f);
    end function;

    function l3da_drive(v : real; r : real) return logic3da is
    begin
        return (voltage => v, resistance => r, flags => AFL_KNOWN);
    end function;

    function l3da_pwl(v : real; r : real) return logic3da is
    begin
        return (voltage => v, resistance => r, flags => AFL_PWL);
    end function;

end package body;
