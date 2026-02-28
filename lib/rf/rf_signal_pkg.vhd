-- RF Signal Package (Jones vector representation)
--
-- Polarized RF E-field using Jones vector formalism.
-- Each field has two complex E-field components (Eh, Ev) plus frequency.
-- Resolution is coherent superposition (vector sum of E-fields).
--
-- Structurally identical to optical_field_pkg but for radio frequencies.
-- Eh = horizontal polarization, Ev = vertical polarization.

library ieee;
use ieee.std_logic_1164.all;
use ieee.math_real.all;

package rf_signal_pkg is

    -- Speed of light (m/s)
    constant SPEED_OF_LIGHT : real := 299792458.0;

    ---------------------------------------------------------------------------
    -- Polarized RF E-field (Jones vector + frequency)
    ---------------------------------------------------------------------------
    type rf_signal is record
        eh_re : real;    -- H-polarization E-field (real part)
        eh_im : real;    -- H-polarization E-field (imag part)
        ev_re : real;    -- V-polarization E-field (real part)
        ev_im : real;    -- V-polarization E-field (imag part)
        freq  : real;    -- center frequency in Hz (e.g. 2.4e9)
    end record;

    ---------------------------------------------------------------------------
    -- Vector type for resolution functions
    ---------------------------------------------------------------------------
    type rf_signal_vector is array (natural range <>) of rf_signal;

    ---------------------------------------------------------------------------
    -- Constants
    ---------------------------------------------------------------------------
    constant RF_ZERO : rf_signal := (
        eh_re => 0.0, eh_im => 0.0,
        ev_re => 0.0, ev_im => 0.0,
        freq => 2.4e9
    );

    constant RF_UNKNOWN : rf_signal := (
        eh_re => 0.0, eh_im => 0.0,
        ev_re => 0.0, ev_im => 0.0,
        freq => -1.0
    );

    -- Common WiFi frequencies
    constant FREQ_WIFI_2G4 : real := 2.4e9;     -- 2.4 GHz (802.11b/g/n)
    constant FREQ_WIFI_5G  : real := 5.8e9;     -- 5.8 GHz (802.11a/n/ac)
    constant FREQ_WIFI_6G  : real := 6.0e9;     -- 6 GHz (802.11ax)

    ---------------------------------------------------------------------------
    -- Resolution function: coherent superposition
    ---------------------------------------------------------------------------
    function rf_resolve(drivers : rf_signal_vector) return rf_signal;

    subtype resolved_rf_signal is rf_resolve rf_signal;

    ---------------------------------------------------------------------------
    -- Complex arithmetic helpers
    ---------------------------------------------------------------------------
    function cmul_re(a_re, a_im, b_re, b_im : real) return real;
    function cmul_im(a_re, a_im, b_re, b_im : real) return real;
    function cmag2(re, im : real) return real;

    ---------------------------------------------------------------------------
    -- Builder functions
    ---------------------------------------------------------------------------

    -- Build from power (watts), phase (radians), polarization angle (radians),
    -- and frequency. Pol angle: 0 = H-polarized, pi/2 = V-polarized.
    function make_rf_signal(
        power     : real;
        phase     : real;
        pol_angle : real;
        freq      : real
    ) return rf_signal;

    function make_h_polarized(
        power : real;
        phase : real;
        freq  : real
    ) return rf_signal;

    function make_v_polarized(
        power : real;
        phase : real;
        freq  : real
    ) return rf_signal;

    ---------------------------------------------------------------------------
    -- Query functions
    ---------------------------------------------------------------------------

    -- Total RF power: |Eh|^2 + |Ev|^2
    function rf_power(f : rf_signal) return real;

    -- Power in H polarization
    function power_h(f : rf_signal) return real;

    -- Power in V polarization
    function power_v(f : rf_signal) return real;

    -- Phase of H component
    function phase_h(f : rf_signal) return real;

    -- Phase of V component
    function phase_v(f : rf_signal) return real;

    -- Is the field essentially zero?
    function is_silent(f : rf_signal; threshold : real := 1.0e-30) return boolean;

    -- Is the field unknown?
    function is_rf_unknown(f : rf_signal) return boolean;

    ---------------------------------------------------------------------------
    -- Field operations
    ---------------------------------------------------------------------------

    function scale_rf(f : rf_signal; factor : real) return rf_signal;
    function shift_rf_phase(f : rf_signal; phi : real) return rf_signal;
    function add_rf(a, b : rf_signal) return rf_signal;

    ---------------------------------------------------------------------------
    -- Unit conversions
    ---------------------------------------------------------------------------

    function freq_to_wavelength(f : real) return real;
    function wavelength_to_freq(wl : real) return real;
    function dbm_to_watts(dbm : real) return real;
    function watts_to_dbm(watts : real) return real;
    function db_to_amplitude(db : real) return real;
    function db_to_linear(db : real) return real;

end package;

package body rf_signal_pkg is

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

    ---------------------------------------------------------------------------
    -- Resolution: coherent superposition (vector sum)
    ---------------------------------------------------------------------------
    function rf_resolve(drivers : rf_signal_vector) return rf_signal is
        variable result : rf_signal := RF_ZERO;
        variable found_any : boolean := false;
    begin
        if drivers'length = 1 then
            return drivers(drivers'low);
        end if;

        for i in drivers'range loop
            if drivers(i).freq < 0.0 then
                return RF_UNKNOWN;
            end if;

            result.eh_re := result.eh_re + drivers(i).eh_re;
            result.eh_im := result.eh_im + drivers(i).eh_im;
            result.ev_re := result.ev_re + drivers(i).ev_re;
            result.ev_im := result.ev_im + drivers(i).ev_im;

            if not found_any and rf_power(drivers(i)) > 0.0 then
                result.freq := drivers(i).freq;
                found_any := true;
            end if;
        end loop;

        if not found_any then
            result.freq := RF_ZERO.freq;
        end if;

        return result;
    end function;

    ---------------------------------------------------------------------------
    -- Builders
    ---------------------------------------------------------------------------
    function make_rf_signal(
        power     : real;
        phase     : real;
        pol_angle : real;
        freq      : real
    ) return rf_signal is
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
            eh_re => amplitude * cos_pol * cos_ph,
            eh_im => amplitude * cos_pol * sin_ph,
            ev_re => amplitude * sin_pol * cos_ph,
            ev_im => amplitude * sin_pol * sin_ph,
            freq => freq
        );
    end function;

    function make_h_polarized(
        power : real;
        phase : real;
        freq  : real
    ) return rf_signal is
        variable amplitude : real;
    begin
        amplitude := sqrt(power);
        return (
            eh_re => amplitude * cos(phase),
            eh_im => amplitude * sin(phase),
            ev_re => 0.0, ev_im => 0.0,
            freq => freq
        );
    end function;

    function make_v_polarized(
        power : real;
        phase : real;
        freq  : real
    ) return rf_signal is
        variable amplitude : real;
    begin
        amplitude := sqrt(power);
        return (
            eh_re => 0.0, eh_im => 0.0,
            ev_re => amplitude * cos(phase),
            ev_im => amplitude * sin(phase),
            freq => freq
        );
    end function;

    ---------------------------------------------------------------------------
    -- Queries
    ---------------------------------------------------------------------------
    function rf_power(f : rf_signal) return real is
    begin
        return cmag2(f.eh_re, f.eh_im) + cmag2(f.ev_re, f.ev_im);
    end function;

    function power_h(f : rf_signal) return real is
    begin
        return cmag2(f.eh_re, f.eh_im);
    end function;

    function power_v(f : rf_signal) return real is
    begin
        return cmag2(f.ev_re, f.ev_im);
    end function;

    function phase_h(f : rf_signal) return real is
    begin
        return arctan(f.eh_im, f.eh_re);
    end function;

    function phase_v(f : rf_signal) return real is
    begin
        return arctan(f.ev_im, f.ev_re);
    end function;

    function is_silent(f : rf_signal; threshold : real := 1.0e-30) return boolean is
    begin
        return rf_power(f) < threshold;
    end function;

    function is_rf_unknown(f : rf_signal) return boolean is
    begin
        return f.freq < 0.0;
    end function;

    ---------------------------------------------------------------------------
    -- Field operations
    ---------------------------------------------------------------------------
    function scale_rf(f : rf_signal; factor : real) return rf_signal is
    begin
        return (
            eh_re => f.eh_re * factor,
            eh_im => f.eh_im * factor,
            ev_re => f.ev_re * factor,
            ev_im => f.ev_im * factor,
            freq => f.freq
        );
    end function;

    function shift_rf_phase(f : rf_signal; phi : real) return rf_signal is
        variable cos_phi : real := cos(phi);
        variable sin_phi : real := sin(phi);
    begin
        return (
            eh_re => cmul_re(f.eh_re, f.eh_im, cos_phi, sin_phi),
            eh_im => cmul_im(f.eh_re, f.eh_im, cos_phi, sin_phi),
            ev_re => cmul_re(f.ev_re, f.ev_im, cos_phi, sin_phi),
            ev_im => cmul_im(f.ev_re, f.ev_im, cos_phi, sin_phi),
            freq => f.freq
        );
    end function;

    function add_rf(a, b : rf_signal) return rf_signal is
    begin
        return (
            eh_re => a.eh_re + b.eh_re,
            eh_im => a.eh_im + b.eh_im,
            ev_re => a.ev_re + b.ev_re,
            ev_im => a.ev_im + b.ev_im,
            freq => a.freq
        );
    end function;

    ---------------------------------------------------------------------------
    -- Unit conversions
    ---------------------------------------------------------------------------
    function freq_to_wavelength(f : real) return real is
    begin
        return SPEED_OF_LIGHT / f;
    end function;

    function wavelength_to_freq(wl : real) return real is
    begin
        return SPEED_OF_LIGHT / wl;
    end function;

    function dbm_to_watts(dbm : real) return real is
    begin
        return 1.0e-3 * (10.0 ** (dbm / 10.0));
    end function;

    function watts_to_dbm(watts : real) return real is
    begin
        if watts <= 0.0 then
            return -100.0;
        end if;
        return 10.0 * log10(watts / 1.0e-3);
    end function;

    function db_to_amplitude(db : real) return real is
    begin
        return 10.0 ** (db / 20.0);
    end function;

    function db_to_linear(db : real) return real is
    begin
        return 10.0 ** (db / 10.0);
    end function;

end package body;
