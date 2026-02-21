-- SystemVerilog Pull Gates
-- IEEE 1800 Section 28.10

---------------------------------------------------------------------------
-- PULLUP: Drives weak 1 (H)
---------------------------------------------------------------------------
library ieee;
use ieee.std_logic_1164.all;

entity sv_pullup is
    port (
        y : out std_logic
    );
end entity sv_pullup;

architecture behavioral of sv_pullup is
begin
    y <= 'H';
end architecture behavioral;

---------------------------------------------------------------------------
-- PULLDOWN: Drives weak 0 (L)
---------------------------------------------------------------------------
library ieee;
use ieee.std_logic_1164.all;

entity sv_pulldown is
    port (
        y : out std_logic
    );
end entity sv_pulldown;

architecture behavioral of sv_pulldown is
begin
    y <= 'L';
end architecture behavioral;
