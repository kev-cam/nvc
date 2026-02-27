-- Optical 2x2 Directional Coupler
--
-- Four-port coupler with coupling coefficient kappa.
-- Ports a1,a2 are on the input side; b1,b2 on the output side.
--
-- Transfer matrix (lossless):
--   b1 = sqrt(1-k) * a1 + j*sqrt(k) * a2
--   b2 = j*sqrt(k) * a1 + sqrt(1-k) * a2
--
-- Bidirectional: same matrix applies in reverse direction.
-- Each port uses 'driver/'other implicit signals for the resolver.

library ieee;
use ieee.std_logic_1164.all;
use ieee.math_real.all;
use work.optical_field_pkg.all;
use work.optical_matrix_pkg.all;

entity optical_coupler is
    generic (
        kappa : real := 0.5    -- coupling coefficient (0.0 to 1.0)
    );
    port (
        a1 : inout std_logic;
        a2 : inout std_logic;
        b1 : inout std_logic;
        b2 : inout std_logic
    );
end entity optical_coupler;

architecture behavioral of optical_coupler is
begin
    process (a1'other, a2'other, b1'other, b2'other)
        variable bar   : jones_matrix;
        variable cross : jones_matrix;
        variable a1_in, a2_in, b1_in, b2_in : optical_field;
    begin
        bar   := jones_coupler_bar(kappa);
        cross := jones_coupler_cross(kappa);

        a1_in := a1'other;
        a2_in := a2'other;
        b1_in := b1'other;
        b2_in := b2'other;

        -- Forward: a-side inputs -> b-side outputs
        -- b1 = bar * a1 + cross * a2
        b1'driver := add_fields(
            jones_apply(bar, a1_in),
            jones_apply(cross, a2_in)
        );
        -- b2 = cross * a1 + bar * a2
        b2'driver := add_fields(
            jones_apply(cross, a1_in),
            jones_apply(bar, a2_in)
        );

        -- Reverse: b-side inputs -> a-side outputs
        -- a1 = bar * b1 + cross * b2
        a1'driver := add_fields(
            jones_apply(bar, b1_in),
            jones_apply(cross, b2_in)
        );
        -- a2 = cross * b1 + bar * b2
        a2'driver := add_fields(
            jones_apply(cross, b1_in),
            jones_apply(bar, b2_in)
        );
    end process;
end architecture behavioral;
