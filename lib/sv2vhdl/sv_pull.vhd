-- SystemVerilog Pull Gates
-- IEEE 1800 Section 28.10

---------------------------------------------------------------------------
-- PULLUP: Drives weak 1 (H)
---------------------------------------------------------------------------
library ieee;
use ieee.std_logic_1164.all;
use work.logic3d_types_pkg.all;

entity sv_pullup is
    port (
        y : out logic3d
    );
end entity sv_pullup;

architecture behavioral of sv_pullup is
begin
    y <= L3D_H;
end architecture behavioral;

---------------------------------------------------------------------------
-- PULLDOWN: Drives weak 0 (L)
---------------------------------------------------------------------------
library ieee;
use ieee.std_logic_1164.all;
use work.logic3d_types_pkg.all;

entity sv_pulldown is
    port (
        y : out logic3d
    );
end entity sv_pulldown;

architecture behavioral of sv_pulldown is
begin
    y <= L3D_L;
end architecture behavioral;
