-- Optical Field Package (Jones vector representation)
--
-- Coherent polarized optical field using Jones vector formalism.
-- Each field has two complex E-field components (Ex, Ey) plus wavelength.
-- Resolution is coherent superposition (vector sum of E-fields).
--
-- This is the photonics equivalent of logic3ds_pkg: the fundamental
-- signal type for optical nets with per-receiver resolution.

library ieee;
use ieee.std_logic_1164.all;
use ieee.math_real.all;

package optical_field_pkg is

    ---------------------------------------------------------------------------
    -- Polarized optical field (Jones vector + wavelength)
    ---------------------------------------------------------------------------
    type optical_field is record
        ex_re      : real;   -- E-field X polarization (real part)
        ex_im      : real;   -- E-field X polarization (imag part)
        ey_re      : real;   -- E-field Y polarization (real part)
        ey_im      : real;   -- E-field Y polarization (imag part)
        wavelength : real;   -- meters (e.g. 1.55e-6 for C-band)
    end record;

    ---------------------------------------------------------------------------
    -- Vector type for resolution functions
    ---------------------------------------------------------------------------
    type optical_field_vector is array (natural range <>) of optical_field;

    ---------------------------------------------------------------------------
    -- Constants
    ---------------------------------------------------------------------------
    -- No light (zero field)
    constant OPTICAL_ZERO : optical_field := (
        ex_re => 0.0, ex_im => 0.0,
        ey_re => 0.0, ey_im => 0.0,
        wavelength => 1.55e-6
    );

    -- Unknown/uninitialized (NaN-like sentinel)
    -- Use negative wavelength as marker since physical wavelength is positive
    constant OPTICAL_X : optical_field := (
        ex_re => 0.0, ex_im => 0.0,
        ey_re => 0.0, ey_im => 0.0,
        wavelength => -1.0
    );

    -- Common telecom wavelengths
    constant WL_C_BAND : real := 1.55e-6;    -- 1550 nm
    constant WL_O_BAND : real := 1.31e-6;    -- 1310 nm
    constant WL_850NM  : real := 850.0e-9;   -- 850 nm (multimode)

    ---------------------------------------------------------------------------
    -- Resolution function: coherent superposition
    --
    -- Multiple drivers on the same optical net interfere coherently.
    -- Result is the vector sum of all E-fields.
    -- Wavelength from first non-zero driver (all should match on a net).
    ---------------------------------------------------------------------------
    function optical_resolve(drivers : optical_field_vector) return optical_field;

    ---------------------------------------------------------------------------
    -- Resolved subtype: for signals with multiple optical drivers
    ---------------------------------------------------------------------------
    subtype resolved_optical_field is optical_resolve optical_field;

    ---------------------------------------------------------------------------
    -- Complex arithmetic helpers (no VHDL complex type)
    -- (a_re + j*a_im) * (b_re + j*b_im)
    ---------------------------------------------------------------------------
    function cmul_re(a_re, a_im, b_re, b_im : real) return real;
    function cmul_im(a_re, a_im, b_re, b_im : real) return real;

    -- Complex magnitude squared: |a|^2 = re^2 + im^2
    function cmag2(re, im : real) return real;

    -- Complex addition
    function cadd_re(a_re, b_re : real) return real;
    function cadd_im(a_im, b_im : real) return real;

    ---------------------------------------------------------------------------
    -- Builder functions
    ---------------------------------------------------------------------------

    -- Build from power (watts), phase (radians), polarization angle (radians),
    -- and wavelength. Polarization angle: 0 = X-polarized, pi/2 = Y-polarized.
    function make_optical_field(
        power     : real;
        phase     : real;
        pol_angle : real;
        wavelength : real
    ) return optical_field;

    -- Build X-polarized field from power and phase
    function make_x_polarized(
        power      : real;
        phase      : real;
        wavelength : real
    ) return optical_field;

    -- Build Y-polarized field from power and phase
    function make_y_polarized(
        power      : real;
        phase      : real;
        wavelength : real
    ) return optical_field;

    ---------------------------------------------------------------------------
    -- Query functions
    ---------------------------------------------------------------------------

    -- Total optical power: |Ex|^2 + |Ey|^2
    function optical_power(f : optical_field) return real;

    -- Power in X polarization: |Ex|^2
    function power_x(f : optical_field) return real;

    -- Power in Y polarization: |Ey|^2
    function power_y(f : optical_field) return real;

    -- Phase of X component: atan2(ex_im, ex_re)
    function phase_x(f : optical_field) return real;

    -- Phase of Y component: atan2(ey_im, ey_re)
    function phase_y(f : optical_field) return real;

    -- Is the field essentially zero? (power below threshold)
    function is_dark(f : optical_field; threshold : real := 1.0e-30) return boolean;

    -- Is the field unknown/uninitialized?
    function is_unknown(f : optical_field) return boolean;

    ---------------------------------------------------------------------------
    -- Field operations
    ---------------------------------------------------------------------------

    -- Scale field amplitude by a real factor (power scales by factor^2)
    function scale_field(f : optical_field; factor : real) return optical_field;

    -- Apply phase shift to both polarizations
    function shift_phase(f : optical_field; phi : real) return optical_field;

    -- Add two fields (coherent superposition)
    function add_fields(a, b : optical_field) return optical_field;

    -- Convert power in dBm to watts
    function dbm_to_watts(dbm : real) return real;

    -- Convert power in watts to dBm
    function watts_to_dbm(watts : real) return real;

    -- Convert dB loss to linear amplitude factor
    function db_to_amplitude(db : real) return real;

end package;

package body optical_field_pkg is

    ---------------------------------------------------------------------------
    -- Complex arithmetic
    ---------------------------------------------------------------------------
    function cmul_re(a_re, a_im, b_re, b_im : real) return real is
    begin
        return a_re * b_re - a_im * b_im;
    end function;

    function cmul_im(a_re, a_im, b_re, b_im : real) return real is
    begin
        return a_re * b_im + a_im * b_re;
    end function;

    function cmag2(re, im : real) return real is
    begin
        return re * re + im * im;
    end function;

    function cadd_re(a_re, b_re : real) return real is
    begin
        return a_re + b_re;
    end function;

    function cadd_im(a_im, b_im : real) return real is
    begin
        return a_im + b_im;
    end function;

    ---------------------------------------------------------------------------
    -- Resolution: coherent superposition (vector sum)
    --
    -- For a single driver: identity.
    -- For multiple drivers: sum all E-field components.
    -- Unknown fields (negative wavelength) propagate.
    -- Zero fields (OPTICAL_ZERO) are identity for addition.
    ---------------------------------------------------------------------------
    function optical_resolve(drivers : optical_field_vector) return optical_field is
        variable result : optical_field := OPTICAL_ZERO;
        variable found_any : boolean := false;
    begin
        if drivers'length = 1 then
            return drivers(drivers'low);
        end if;

        for i in drivers'range loop
            -- Propagate unknown
            if drivers(i).wavelength < 0.0 then
                return OPTICAL_X;
            end if;

            result.ex_re := result.ex_re + drivers(i).ex_re;
            result.ex_im := result.ex_im + drivers(i).ex_im;
            result.ey_re := result.ey_re + drivers(i).ey_re;
            result.ey_im := result.ey_im + drivers(i).ey_im;

            -- Take wavelength from first non-zero driver
            if not found_any and optical_power(drivers(i)) > 0.0 then
                result.wavelength := drivers(i).wavelength;
                found_any := true;
            end if;
        end loop;

        if not found_any then
            result.wavelength := OPTICAL_ZERO.wavelength;
        end if;

        return result;
    end function;

    ---------------------------------------------------------------------------
    -- Builders
    ---------------------------------------------------------------------------
    function make_optical_field(
        power     : real;
        phase     : real;
        pol_angle : real;
        wavelength : real
    ) return optical_field is
        variable amplitude : real;
        variable cos_pol   : real;
        variable sin_pol   : real;
        variable cos_ph    : real;
        variable sin_ph    : real;
    begin
        amplitude := sqrt(power);
        cos_pol := cos(pol_angle);
        sin_pol := sin(pol_angle);
        cos_ph  := cos(phase);
        sin_ph  := sin(phase);
        return (
            ex_re => amplitude * cos_pol * cos_ph,
            ex_im => amplitude * cos_pol * sin_ph,
            ey_re => amplitude * sin_pol * cos_ph,
            ey_im => amplitude * sin_pol * sin_ph,
            wavelength => wavelength
        );
    end function;

    function make_x_polarized(
        power      : real;
        phase      : real;
        wavelength : real
    ) return optical_field is
        variable amplitude : real;
    begin
        amplitude := sqrt(power);
        return (
            ex_re => amplitude * cos(phase),
            ex_im => amplitude * sin(phase),
            ey_re => 0.0,
            ey_im => 0.0,
            wavelength => wavelength
        );
    end function;

    function make_y_polarized(
        power      : real;
        phase      : real;
        wavelength : real
    ) return optical_field is
        variable amplitude : real;
    begin
        amplitude := sqrt(power);
        return (
            ex_re => 0.0,
            ex_im => 0.0,
            ey_re => amplitude * cos(phase),
            ey_im => amplitude * sin(phase),
            wavelength => wavelength
        );
    end function;

    ---------------------------------------------------------------------------
    -- Queries
    ---------------------------------------------------------------------------
    function optical_power(f : optical_field) return real is
    begin
        return cmag2(f.ex_re, f.ex_im) + cmag2(f.ey_re, f.ey_im);
    end function;

    function power_x(f : optical_field) return real is
    begin
        return cmag2(f.ex_re, f.ex_im);
    end function;

    function power_y(f : optical_field) return real is
    begin
        return cmag2(f.ey_re, f.ey_im);
    end function;

    function phase_x(f : optical_field) return real is
    begin
        return arctan(f.ex_im, f.ex_re);
    end function;

    function phase_y(f : optical_field) return real is
    begin
        return arctan(f.ey_im, f.ey_re);
    end function;

    function is_dark(f : optical_field; threshold : real := 1.0e-30) return boolean is
    begin
        return optical_power(f) < threshold;
    end function;

    function is_unknown(f : optical_field) return boolean is
    begin
        return f.wavelength < 0.0;
    end function;

    ---------------------------------------------------------------------------
    -- Field operations
    ---------------------------------------------------------------------------
    function scale_field(f : optical_field; factor : real) return optical_field is
    begin
        return (
            ex_re => f.ex_re * factor,
            ex_im => f.ex_im * factor,
            ey_re => f.ey_re * factor,
            ey_im => f.ey_im * factor,
            wavelength => f.wavelength
        );
    end function;

    function shift_phase(f : optical_field; phi : real) return optical_field is
        variable cos_phi : real := cos(phi);
        variable sin_phi : real := sin(phi);
    begin
        return (
            ex_re => cmul_re(f.ex_re, f.ex_im, cos_phi, sin_phi),
            ex_im => cmul_im(f.ex_re, f.ex_im, cos_phi, sin_phi),
            ey_re => cmul_re(f.ey_re, f.ey_im, cos_phi, sin_phi),
            ey_im => cmul_im(f.ey_re, f.ey_im, cos_phi, sin_phi),
            wavelength => f.wavelength
        );
    end function;

    function add_fields(a, b : optical_field) return optical_field is
    begin
        return (
            ex_re => a.ex_re + b.ex_re,
            ex_im => a.ex_im + b.ex_im,
            ey_re => a.ey_re + b.ey_re,
            ey_im => a.ey_im + b.ey_im,
            wavelength => a.wavelength
        );
    end function;

    ---------------------------------------------------------------------------
    -- Unit conversions
    ---------------------------------------------------------------------------
    function dbm_to_watts(dbm : real) return real is
    begin
        return 1.0e-3 * (10.0 ** (dbm / 10.0));
    end function;

    function watts_to_dbm(watts : real) return real is
    begin
        if watts <= 0.0 then
            return -100.0;  -- floor value
        end if;
        return 10.0 * log10(watts / 1.0e-3);
    end function;

    function db_to_amplitude(db : real) return real is
    begin
        return 10.0 ** (db / 20.0);
    end function;

end package body;
