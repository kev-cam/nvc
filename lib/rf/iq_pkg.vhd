-- IQ Sample Package
--
-- Complex baseband I/Q sample type for signal processing.
-- Used by OFDM modulator/demodulator and RF front-end components.

library ieee;
use ieee.std_logic_1164.all;
use ieee.math_real.all;
use work.rf_signal_pkg.all;

package iq_pkg is

    ---------------------------------------------------------------------------
    -- Complex I/Q sample
    ---------------------------------------------------------------------------
    type iq_sample is record
        i : real;    -- in-phase component
        q : real;    -- quadrature component
    end record;

    type iq_array is array (natural range <>) of iq_sample;

    ---------------------------------------------------------------------------
    -- Constants
    ---------------------------------------------------------------------------
    constant IQ_ZERO : iq_sample := (i => 0.0, q => 0.0);

    ---------------------------------------------------------------------------
    -- Builder functions
    ---------------------------------------------------------------------------
    function make_iq(mag : real; phase : real) return iq_sample;
    function make_iq_rect(i_val : real; q_val : real) return iq_sample;

    ---------------------------------------------------------------------------
    -- Query functions
    ---------------------------------------------------------------------------
    function iq_magnitude(s : iq_sample) return real;
    function iq_phase(s : iq_sample) return real;
    function iq_power(s : iq_sample) return real;

    ---------------------------------------------------------------------------
    -- Operations
    ---------------------------------------------------------------------------
    function iq_add(a, b : iq_sample) return iq_sample;
    function iq_sub(a, b : iq_sample) return iq_sample;
    function iq_mul(a, b : iq_sample) return iq_sample;
    function iq_scale(s : iq_sample; factor : real) return iq_sample;
    function iq_conjugate(s : iq_sample) return iq_sample;

    ---------------------------------------------------------------------------
    -- Domain conversions
    ---------------------------------------------------------------------------

    -- Extract scalar I/Q from rf_signal (takes H-pol component)
    function rf_to_iq(f : rf_signal) return iq_sample;

    -- Wrap I/Q as H-polarized rf_signal at given frequency
    function iq_to_rf(s : iq_sample; freq : real) return rf_signal;

end package;

package body iq_pkg is

    function make_iq(mag : real; phase : real) return iq_sample is
    begin
        return (i => mag * cos(phase), q => mag * sin(phase));
    end function;

    function make_iq_rect(i_val : real; q_val : real) return iq_sample is
    begin
        return (i => i_val, q => q_val);
    end function;

    function iq_magnitude(s : iq_sample) return real is
    begin
        return sqrt(s.i * s.i + s.q * s.q);
    end function;

    function iq_phase(s : iq_sample) return real is
    begin
        return arctan(s.q, s.i);
    end function;

    function iq_power(s : iq_sample) return real is
    begin
        return s.i * s.i + s.q * s.q;
    end function;

    function iq_add(a, b : iq_sample) return iq_sample is
    begin
        return (i => a.i + b.i, q => a.q + b.q);
    end function;

    function iq_sub(a, b : iq_sample) return iq_sample is
    begin
        return (i => a.i - b.i, q => a.q - b.q);
    end function;

    function iq_mul(a, b : iq_sample) return iq_sample is
    begin
        return (
            i => a.i * b.i - a.q * b.q,
            q => a.i * b.q + a.q * b.i
        );
    end function;

    function iq_scale(s : iq_sample; factor : real) return iq_sample is
    begin
        return (i => s.i * factor, q => s.q * factor);
    end function;

    function iq_conjugate(s : iq_sample) return iq_sample is
    begin
        return (i => s.i, q => -s.q);
    end function;

    function rf_to_iq(f : rf_signal) return iq_sample is
    begin
        return (i => f.eh_re, q => f.eh_im);
    end function;

    function iq_to_rf(s : iq_sample; freq : real) return rf_signal is
    begin
        return (
            eh_re => s.i, eh_im => s.q,
            ev_re => 0.0, ev_im => 0.0,
            freq => freq
        );
    end function;

end package body;
