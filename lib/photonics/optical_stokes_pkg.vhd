-- Optical Stokes Package (incoherent/system layer)
--
-- Stokes parameters for partially polarized or depolarized light.
-- Used for system-level simulation where coherent phase information
-- is not needed or is averaged out.
--
-- Conversion functions bridge between Jones (coherent) and Stokes
-- (incoherent) representations.

library ieee;
use ieee.math_real.all;
use work.optical_field_pkg.all;

package optical_stokes_pkg is

    ---------------------------------------------------------------------------
    -- Stokes parameter vector + wavelength
    --
    -- S0 = total intensity = |Ex|^2 + |Ey|^2
    -- S1 = |Ex|^2 - |Ey|^2          (horizontal vs vertical)
    -- S2 = 2*Re(Ex*Ey*)              (+45 vs -45 degree)
    -- S3 = 2*Im(Ex*Ey*)              (right vs left circular)
    --
    -- For fully polarized light: S0^2 = S1^2 + S2^2 + S3^2
    -- For partially polarized:   S0^2 >= S1^2 + S2^2 + S3^2
    -- Degree of polarization:    DOP = sqrt(S1^2+S2^2+S3^2) / S0
    ---------------------------------------------------------------------------
    type optical_stokes is record
        s0         : real;   -- total intensity (always >= 0)
        s1         : real;   -- horizontal vs vertical preference
        s2         : real;   -- +45 vs -45 degree preference
        s3         : real;   -- right vs left circular preference
        wavelength : real;   -- meters
    end record;

    ---------------------------------------------------------------------------
    -- Vector type for resolution
    ---------------------------------------------------------------------------
    type optical_stokes_vector is array (natural range <>) of optical_stokes;

    ---------------------------------------------------------------------------
    -- Constants
    ---------------------------------------------------------------------------
    constant STOKES_ZERO : optical_stokes := (
        s0 => 0.0, s1 => 0.0, s2 => 0.0, s3 => 0.0,
        wavelength => 1.55e-6
    );

    constant STOKES_X : optical_stokes := (
        s0 => 0.0, s1 => 0.0, s2 => 0.0, s3 => 0.0,
        wavelength => -1.0
    );

    ---------------------------------------------------------------------------
    -- Resolution: incoherent power addition
    --
    -- For incoherent sources, Stokes parameters add linearly.
    -- This is correct when sources are mutually incoherent
    -- (different lasers, LEDs, amplified spontaneous emission).
    ---------------------------------------------------------------------------
    function stokes_resolve(drivers : optical_stokes_vector) return optical_stokes;

    ---------------------------------------------------------------------------
    -- Resolved subtype
    ---------------------------------------------------------------------------
    subtype resolved_optical_stokes is stokes_resolve optical_stokes;

    ---------------------------------------------------------------------------
    -- Conversion: Jones vector -> Stokes parameters
    -- This is lossless for fully polarized light.
    ---------------------------------------------------------------------------
    function to_stokes(f : optical_field) return optical_stokes;

    ---------------------------------------------------------------------------
    -- Conversion: Stokes -> Jones vector (lossy)
    -- Recovers a fully polarized Jones vector that matches S1,S2,S3.
    -- The unpolarized component (if any) is lost.
    -- Phase argument sets the absolute phase of the X component.
    ---------------------------------------------------------------------------
    function to_optical_field(
        s     : optical_stokes;
        phase : real := 0.0
    ) return optical_field;

    ---------------------------------------------------------------------------
    -- Query functions
    ---------------------------------------------------------------------------

    -- Total power
    function stokes_power(s : optical_stokes) return real;

    -- Degree of polarization (0.0 = unpolarized, 1.0 = fully polarized)
    function degree_of_polarization(s : optical_stokes) return real;

    -- Is unknown?
    function is_unknown(s : optical_stokes) return boolean;

    ---------------------------------------------------------------------------
    -- Operations
    ---------------------------------------------------------------------------

    -- Scale intensity
    function scale_stokes(s : optical_stokes; factor : real) return optical_stokes;

    -- Add two Stokes vectors (incoherent combination)
    function add_stokes(a, b : optical_stokes) return optical_stokes;

    -- Depolarize: reduce DOP toward zero while maintaining total power
    function depolarize(s : optical_stokes; factor : real) return optical_stokes;

end package;

package body optical_stokes_pkg is

    ---------------------------------------------------------------------------
    -- Resolution: sum Stokes vectors
    ---------------------------------------------------------------------------
    function stokes_resolve(drivers : optical_stokes_vector) return optical_stokes is
        variable result : optical_stokes := STOKES_ZERO;
        variable found_any : boolean := false;
    begin
        if drivers'length = 1 then
            return drivers(drivers'low);
        end if;

        for i in drivers'range loop
            if drivers(i).wavelength < 0.0 then
                return STOKES_X;
            end if;

            result.s0 := result.s0 + drivers(i).s0;
            result.s1 := result.s1 + drivers(i).s1;
            result.s2 := result.s2 + drivers(i).s2;
            result.s3 := result.s3 + drivers(i).s3;

            if not found_any and drivers(i).s0 > 0.0 then
                result.wavelength := drivers(i).wavelength;
                found_any := true;
            end if;
        end loop;

        if not found_any then
            result.wavelength := STOKES_ZERO.wavelength;
        end if;

        return result;
    end function;

    ---------------------------------------------------------------------------
    -- Jones -> Stokes conversion
    ---------------------------------------------------------------------------
    function to_stokes(f : optical_field) return optical_stokes is
        variable px   : real;  -- |Ex|^2
        variable py   : real;  -- |Ey|^2
        variable re_xy : real; -- Re(Ex * Ey*)
        variable im_xy : real; -- Im(Ex * Ey*)
    begin
        if is_unknown(f) then
            return STOKES_X;
        end if;

        px := cmag2(f.ex_re, f.ex_im);
        py := cmag2(f.ey_re, f.ey_im);

        -- Ex * Ey* = (ex_re + j*ex_im) * (ey_re - j*ey_im)
        re_xy := f.ex_re * f.ey_re + f.ex_im * f.ey_im;
        im_xy := f.ex_im * f.ey_re - f.ex_re * f.ey_im;

        return (
            s0 => px + py,
            s1 => px - py,
            s2 => 2.0 * re_xy,
            s3 => 2.0 * im_xy,
            wavelength => f.wavelength
        );
    end function;

    ---------------------------------------------------------------------------
    -- Stokes -> Jones conversion (lossy for partially polarized light)
    ---------------------------------------------------------------------------
    function to_optical_field(
        s     : optical_stokes;
        phase : real := 0.0
    ) return optical_field is
        variable p_pol : real;   -- polarized power
        variable px    : real;   -- X power
        variable py    : real;   -- Y power
        variable ax    : real;   -- X amplitude
        variable ay    : real;   -- Y amplitude
        variable delta : real;   -- phase difference Y-X
        variable dop   : real;
    begin
        if s.wavelength < 0.0 then
            return OPTICAL_X;
        end if;

        if s.s0 <= 0.0 then
            return (
                ex_re => 0.0, ex_im => 0.0,
                ey_re => 0.0, ey_im => 0.0,
                wavelength => s.wavelength
            );
        end if;

        -- Polarized fraction only
        p_pol := sqrt(s.s1 * s.s1 + s.s2 * s.s2 + s.s3 * s.s3);
        if p_pol > s.s0 then
            p_pol := s.s0;  -- clamp (numerical noise)
        end if;

        -- Recover polarization from S1, S2, S3
        px := (p_pol + s.s1) / 2.0;
        py := (p_pol - s.s1) / 2.0;

        if px < 0.0 then px := 0.0; end if;
        if py < 0.0 then py := 0.0; end if;

        ax := sqrt(px);
        ay := sqrt(py);

        -- Phase difference from S2 and S3
        if ax > 0.0 and ay > 0.0 then
            delta := arctan(s.s3, s.s2);
        else
            delta := 0.0;
        end if;

        return (
            ex_re => ax * cos(phase),
            ex_im => ax * sin(phase),
            ey_re => ay * cos(phase + delta),
            ey_im => ay * sin(phase + delta),
            wavelength => s.wavelength
        );
    end function;

    ---------------------------------------------------------------------------
    -- Queries
    ---------------------------------------------------------------------------
    function stokes_power(s : optical_stokes) return real is
    begin
        return s.s0;
    end function;

    function degree_of_polarization(s : optical_stokes) return real is
        variable p_pol : real;
    begin
        if s.s0 <= 0.0 then
            return 0.0;
        end if;
        p_pol := sqrt(s.s1 * s.s1 + s.s2 * s.s2 + s.s3 * s.s3);
        return p_pol / s.s0;
    end function;

    function is_unknown(s : optical_stokes) return boolean is
    begin
        return s.wavelength < 0.0;
    end function;

    ---------------------------------------------------------------------------
    -- Operations
    ---------------------------------------------------------------------------
    function scale_stokes(s : optical_stokes; factor : real) return optical_stokes is
    begin
        return (
            s0 => s.s0 * factor,
            s1 => s.s1 * factor,
            s2 => s.s2 * factor,
            s3 => s.s3 * factor,
            wavelength => s.wavelength
        );
    end function;

    function add_stokes(a, b : optical_stokes) return optical_stokes is
    begin
        return (
            s0 => a.s0 + b.s0,
            s1 => a.s1 + b.s1,
            s2 => a.s2 + b.s2,
            s3 => a.s3 + b.s3,
            wavelength => a.wavelength
        );
    end function;

    function depolarize(s : optical_stokes; factor : real) return optical_stokes is
        -- factor = 1.0: no change; factor = 0.0: fully depolarized
    begin
        return (
            s0 => s.s0,
            s1 => s.s1 * factor,
            s2 => s.s2 * factor,
            s3 => s.s3 * factor,
            wavelength => s.wavelength
        );
    end function;

end package body;
