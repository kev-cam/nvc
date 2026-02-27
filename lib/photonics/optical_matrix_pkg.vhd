-- Optical Matrix Package (Jones matrix formalism)
--
-- 2x2 complex Jones matrices for describing optical component transfer
-- functions. Each matrix operates on a Jones vector (optical_field)
-- to produce the output field.
--
-- Components are characterized by their Jones matrices:
--   output_field = jones_apply(component_matrix, input_field)
--
-- Cascaded components multiply matrices:
--   total = jones_multiply(M_last, jones_multiply(M_2, M_1))

library ieee;
use ieee.math_real.all;
use work.optical_field_pkg.all;

package optical_matrix_pkg is

    ---------------------------------------------------------------------------
    -- 2x2 complex Jones matrix
    --
    -- | m00  m01 |   operates on   | Ex |
    -- | m10  m11 |                  | Ey |
    ---------------------------------------------------------------------------
    type jones_matrix is record
        m00_re : real;  m00_im : real;
        m01_re : real;  m01_im : real;
        m10_re : real;  m10_im : real;
        m11_re : real;  m11_im : real;
    end record;

    ---------------------------------------------------------------------------
    -- Constants
    ---------------------------------------------------------------------------
    constant JONES_IDENTITY : jones_matrix := (
        m00_re => 1.0, m00_im => 0.0,
        m01_re => 0.0, m01_im => 0.0,
        m10_re => 0.0, m10_im => 0.0,
        m11_re => 1.0, m11_im => 0.0
    );

    constant JONES_ZERO : jones_matrix := (
        m00_re => 0.0, m00_im => 0.0,
        m01_re => 0.0, m01_im => 0.0,
        m10_re => 0.0, m10_im => 0.0,
        m11_re => 0.0, m11_im => 0.0
    );

    ---------------------------------------------------------------------------
    -- Core operations
    ---------------------------------------------------------------------------

    -- Apply Jones matrix to optical field: output = M * input
    -- Preserves wavelength from input field.
    function jones_apply(m : jones_matrix; f : optical_field) return optical_field;

    -- Multiply two Jones matrices: result = A * B
    function jones_multiply(a, b : jones_matrix) return jones_matrix;

    -- Scale matrix by a real factor
    function jones_scale(m : jones_matrix; factor : real) return jones_matrix;

    -- Scale matrix by a complex factor
    function jones_scale_complex(
        m : jones_matrix; re, im : real
    ) return jones_matrix;

    -- Transpose of a Jones matrix
    function jones_transpose(m : jones_matrix) return jones_matrix;

    -- Conjugate transpose (Hermitian adjoint) of a Jones matrix
    function jones_adjoint(m : jones_matrix) return jones_matrix;

    ---------------------------------------------------------------------------
    -- Predefined component matrices
    ---------------------------------------------------------------------------

    -- Polarization rotation by angle theta (radians)
    -- | cos(t)  -sin(t) |
    -- | sin(t)   cos(t) |
    function jones_rotation(theta : real) return jones_matrix;

    -- Phase delay (retarder): phase shift phi_x on X, phi_y on Y
    -- | exp(j*phi_x)     0       |
    -- |     0        exp(j*phi_y)|
    function jones_phase_delay(phi_x, phi_y : real) return jones_matrix;

    -- Uniform phase shift on both polarizations
    -- | exp(j*phi)    0     |
    -- |    0      exp(j*phi)|
    function jones_phase_shift(phi : real) return jones_matrix;

    -- Half-wave plate at angle theta
    -- Flips polarization about axis at angle theta
    function jones_half_wave_plate(theta : real) return jones_matrix;

    -- Quarter-wave plate at angle theta
    -- Converts linear to circular polarization (and vice versa)
    function jones_quarter_wave_plate(theta : real) return jones_matrix;

    -- Linear polarizer along angle theta from X axis
    function jones_linear_polarizer(theta : real) return jones_matrix;

    -- Horizontal polarizer (pass X, block Y)
    constant JONES_H_POLARIZER : jones_matrix := (
        m00_re => 1.0, m00_im => 0.0,
        m01_re => 0.0, m01_im => 0.0,
        m10_re => 0.0, m10_im => 0.0,
        m11_re => 0.0, m11_im => 0.0
    );

    -- Vertical polarizer (pass Y, block X)
    constant JONES_V_POLARIZER : jones_matrix := (
        m00_re => 0.0, m00_im => 0.0,
        m01_re => 0.0, m01_im => 0.0,
        m10_re => 0.0, m10_im => 0.0,
        m11_re => 1.0, m11_im => 0.0
    );

    -- Attenuation by factor (amplitude, not power)
    -- Power attenuation = factor^2
    function jones_attenuator(factor : real) return jones_matrix;

    -- Loss in dB (positive value = loss)
    function jones_loss_db(loss_db : real) return jones_matrix;

    ---------------------------------------------------------------------------
    -- 2x2 directional coupler matrix
    --
    -- A lossless 2x2 coupler with coupling coefficient kappa:
    --   | sqrt(1-k)    j*sqrt(k)  |
    --   | j*sqrt(k)    sqrt(1-k)  |
    --
    -- This returns the "bar" (through) and "cross" sub-matrices.
    -- For a full 4-port coupler, the resolver applies:
    --   b1 = bar * a1 + cross * a2
    --   b2 = cross * a1 + bar * a2
    ---------------------------------------------------------------------------
    function jones_coupler_bar(kappa : real) return jones_matrix;
    function jones_coupler_cross(kappa : real) return jones_matrix;

    ---------------------------------------------------------------------------
    -- Waveguide propagation matrix
    -- Accumulates phase (2*pi*neff*length/wavelength) and loss
    ---------------------------------------------------------------------------
    function jones_waveguide(
        length      : real;    -- meters
        neff        : real;    -- effective refractive index
        wavelength  : real;    -- meters
        loss_db_per_m : real   -- dB/m (positive = loss)
    ) return jones_matrix;

end package;

package body optical_matrix_pkg is

    ---------------------------------------------------------------------------
    -- Core operations
    ---------------------------------------------------------------------------
    function jones_apply(m : jones_matrix; f : optical_field) return optical_field is
        variable ox_re, ox_im : real;
        variable oy_re, oy_im : real;
    begin
        if is_unknown(f) then
            return OPTICAL_X;
        end if;

        -- out_x = m00 * ex + m01 * ey
        ox_re := cmul_re(m.m00_re, m.m00_im, f.ex_re, f.ex_im)
               + cmul_re(m.m01_re, m.m01_im, f.ey_re, f.ey_im);
        ox_im := cmul_im(m.m00_re, m.m00_im, f.ex_re, f.ex_im)
               + cmul_im(m.m01_re, m.m01_im, f.ey_re, f.ey_im);

        -- out_y = m10 * ex + m11 * ey
        oy_re := cmul_re(m.m10_re, m.m10_im, f.ex_re, f.ex_im)
               + cmul_re(m.m11_re, m.m11_im, f.ey_re, f.ey_im);
        oy_im := cmul_im(m.m10_re, m.m10_im, f.ex_re, f.ex_im)
               + cmul_im(m.m11_re, m.m11_im, f.ey_re, f.ey_im);

        return (
            ex_re => ox_re, ex_im => ox_im,
            ey_re => oy_re, ey_im => oy_im,
            wavelength => f.wavelength
        );
    end function;

    function jones_multiply(a, b : jones_matrix) return jones_matrix is
        variable r : jones_matrix;
    begin
        -- r(0,0) = a(0,0)*b(0,0) + a(0,1)*b(1,0)
        r.m00_re := cmul_re(a.m00_re, a.m00_im, b.m00_re, b.m00_im)
                  + cmul_re(a.m01_re, a.m01_im, b.m10_re, b.m10_im);
        r.m00_im := cmul_im(a.m00_re, a.m00_im, b.m00_re, b.m00_im)
                  + cmul_im(a.m01_re, a.m01_im, b.m10_re, b.m10_im);

        -- r(0,1) = a(0,0)*b(0,1) + a(0,1)*b(1,1)
        r.m01_re := cmul_re(a.m00_re, a.m00_im, b.m01_re, b.m01_im)
                  + cmul_re(a.m01_re, a.m01_im, b.m11_re, b.m11_im);
        r.m01_im := cmul_im(a.m00_re, a.m00_im, b.m01_re, b.m01_im)
                  + cmul_im(a.m01_re, a.m01_im, b.m11_re, b.m11_im);

        -- r(1,0) = a(1,0)*b(0,0) + a(1,1)*b(1,0)
        r.m10_re := cmul_re(a.m10_re, a.m10_im, b.m00_re, b.m00_im)
                  + cmul_re(a.m11_re, a.m11_im, b.m10_re, b.m10_im);
        r.m10_im := cmul_im(a.m10_re, a.m10_im, b.m00_re, b.m00_im)
                  + cmul_im(a.m11_re, a.m11_im, b.m10_re, b.m10_im);

        -- r(1,1) = a(1,0)*b(0,1) + a(1,1)*b(1,1)
        r.m11_re := cmul_re(a.m10_re, a.m10_im, b.m01_re, b.m01_im)
                  + cmul_re(a.m11_re, a.m11_im, b.m11_re, b.m11_im);
        r.m11_im := cmul_im(a.m10_re, a.m10_im, b.m01_re, b.m01_im)
                  + cmul_im(a.m11_re, a.m11_im, b.m11_re, b.m11_im);

        return r;
    end function;

    function jones_scale(m : jones_matrix; factor : real) return jones_matrix is
    begin
        return (
            m00_re => m.m00_re * factor, m00_im => m.m00_im * factor,
            m01_re => m.m01_re * factor, m01_im => m.m01_im * factor,
            m10_re => m.m10_re * factor, m10_im => m.m10_im * factor,
            m11_re => m.m11_re * factor, m11_im => m.m11_im * factor
        );
    end function;

    function jones_scale_complex(
        m : jones_matrix; re, im : real
    ) return jones_matrix is
    begin
        return (
            m00_re => cmul_re(m.m00_re, m.m00_im, re, im),
            m00_im => cmul_im(m.m00_re, m.m00_im, re, im),
            m01_re => cmul_re(m.m01_re, m.m01_im, re, im),
            m01_im => cmul_im(m.m01_re, m.m01_im, re, im),
            m10_re => cmul_re(m.m10_re, m.m10_im, re, im),
            m10_im => cmul_im(m.m10_re, m.m10_im, re, im),
            m11_re => cmul_re(m.m11_re, m.m11_im, re, im),
            m11_im => cmul_im(m.m11_re, m.m11_im, re, im)
        );
    end function;

    function jones_transpose(m : jones_matrix) return jones_matrix is
    begin
        return (
            m00_re => m.m00_re, m00_im => m.m00_im,
            m01_re => m.m10_re, m01_im => m.m10_im,
            m10_re => m.m01_re, m10_im => m.m01_im,
            m11_re => m.m11_re, m11_im => m.m11_im
        );
    end function;

    function jones_adjoint(m : jones_matrix) return jones_matrix is
    begin
        return (
            m00_re =>  m.m00_re, m00_im => -m.m00_im,
            m01_re =>  m.m10_re, m01_im => -m.m10_im,
            m10_re =>  m.m01_re, m10_im => -m.m01_im,
            m11_re =>  m.m11_re, m11_im => -m.m11_im
        );
    end function;

    ---------------------------------------------------------------------------
    -- Predefined matrices
    ---------------------------------------------------------------------------
    function jones_rotation(theta : real) return jones_matrix is
        variable c : real := cos(theta);
        variable s : real := sin(theta);
    begin
        return (
            m00_re =>  c, m00_im => 0.0,
            m01_re => -s, m01_im => 0.0,
            m10_re =>  s, m10_im => 0.0,
            m11_re =>  c, m11_im => 0.0
        );
    end function;

    function jones_phase_delay(phi_x, phi_y : real) return jones_matrix is
    begin
        return (
            m00_re => cos(phi_x), m00_im => sin(phi_x),
            m01_re => 0.0,        m01_im => 0.0,
            m10_re => 0.0,        m10_im => 0.0,
            m11_re => cos(phi_y), m11_im => sin(phi_y)
        );
    end function;

    function jones_phase_shift(phi : real) return jones_matrix is
    begin
        return jones_phase_delay(phi, phi);
    end function;

    function jones_half_wave_plate(theta : real) return jones_matrix is
        variable c2 : real := cos(2.0 * theta);
        variable s2 : real := sin(2.0 * theta);
    begin
        -- HWP = j * [[cos(2t), sin(2t)], [sin(2t), -cos(2t)]]
        -- (global j phase doesn't affect power, but matters for interference)
        return (
            m00_re => 0.0, m00_im => c2,
            m01_re => 0.0, m01_im => s2,
            m10_re => 0.0, m10_im => s2,
            m11_re => 0.0, m11_im => -c2
        );
    end function;

    function jones_quarter_wave_plate(theta : real) return jones_matrix is
        variable c  : real := cos(theta);
        variable s  : real := sin(theta);
        variable c2 : real := c * c;
        variable s2 : real := s * s;
        variable cs : real := c * s;
        variable inv_sqrt2 : real := 1.0 / sqrt(2.0);
    begin
        -- QWP at angle theta:
        -- (1/sqrt(2)) * [[1+j*cos(2t), j*sin(2t)], [j*sin(2t), 1-j*cos(2t)]]
        return (
            m00_re => inv_sqrt2,
            m00_im => inv_sqrt2 * (c2 - s2),
            m01_re => 0.0,
            m01_im => inv_sqrt2 * 2.0 * cs,
            m10_re => 0.0,
            m10_im => inv_sqrt2 * 2.0 * cs,
            m11_re => inv_sqrt2,
            m11_im => -inv_sqrt2 * (c2 - s2)
        );
    end function;

    function jones_linear_polarizer(theta : real) return jones_matrix is
        variable c : real := cos(theta);
        variable s : real := sin(theta);
    begin
        return (
            m00_re => c * c, m00_im => 0.0,
            m01_re => c * s, m01_im => 0.0,
            m10_re => c * s, m10_im => 0.0,
            m11_re => s * s, m11_im => 0.0
        );
    end function;

    function jones_attenuator(factor : real) return jones_matrix is
    begin
        return jones_scale(JONES_IDENTITY, factor);
    end function;

    function jones_loss_db(loss_db : real) return jones_matrix is
    begin
        return jones_attenuator(db_to_amplitude(-loss_db));
    end function;

    ---------------------------------------------------------------------------
    -- Coupler matrices
    ---------------------------------------------------------------------------
    function jones_coupler_bar(kappa : real) return jones_matrix is
        variable t : real := sqrt(1.0 - kappa);
    begin
        return jones_scale(JONES_IDENTITY, t);
    end function;

    function jones_coupler_cross(kappa : real) return jones_matrix is
        variable k_amp : real := sqrt(kappa);
    begin
        -- Cross coupling has j phase shift (lossless condition)
        return jones_scale_complex(JONES_IDENTITY, 0.0, k_amp);
    end function;

    ---------------------------------------------------------------------------
    -- Waveguide propagation
    ---------------------------------------------------------------------------
    function jones_waveguide(
        length      : real;
        neff        : real;
        wavelength  : real;
        loss_db_per_m : real
    ) return jones_matrix is
        variable phi  : real;
        variable loss : real;
    begin
        phi  := 2.0 * MATH_PI * neff * length / wavelength;
        loss := db_to_amplitude(-loss_db_per_m * length);
        return jones_scale(jones_phase_shift(phi), loss);
    end function;

end package body;
