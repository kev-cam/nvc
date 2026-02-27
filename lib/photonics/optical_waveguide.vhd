-- Optical Waveguide
--
-- Bidirectional waveguide with loss and phase accumulation.
-- Uses 'driver/'other implicit signals like sv_tran: each port
-- drives what it contributes to the net, and reads what all other
-- drivers provide via 'other.
--
-- The waveguide applies its Jones matrix (loss + phase) to the
-- signal passing through in each direction.

library ieee;
use ieee.std_logic_1164.all;
use ieee.math_real.all;
use work.optical_field_pkg.all;
use work.optical_matrix_pkg.all;

entity optical_waveguide is
    generic (
        length        : real := 1.0e-3;    -- meters (default 1 mm)
        loss_db_per_m : real := 2.0;        -- dB/m propagation loss
        neff          : real := 2.4         -- effective refractive index
    );
    port (
        a : inout std_logic;
        b : inout std_logic
    );
end entity optical_waveguide;

architecture behavioral of optical_waveguide is
begin
    process (a'other, b'other)
        variable m       : jones_matrix;
        variable a_in    : optical_field;
        variable b_in    : optical_field;
        variable wl      : real;
    begin
        a_in := a'other;
        b_in := b'other;

        -- Use wavelength from incoming field for phase computation
        -- Default to C-band if field is zero
        if not is_dark(a_in) then
            wl := a_in.wavelength;
        elsif not is_dark(b_in) then
            wl := b_in.wavelength;
        else
            wl := WL_C_BAND;
        end if;

        m := jones_waveguide(length, neff, wl, loss_db_per_m);

        -- Bidirectional: each direction applies same transfer matrix
        a'driver := jones_apply(m, b_in);
        b'driver := jones_apply(m, a_in);
    end process;
end architecture behavioral;
