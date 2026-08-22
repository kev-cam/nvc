-- SystemVerilog Three-State Gates
-- IEEE 1800 Section 28.6

---------------------------------------------------------------------------
-- BUFIF0: Three-state buffer with active-low control
---------------------------------------------------------------------------
library ieee;
use ieee.std_logic_1164.all;
use work.logic3d_types_pkg.all;

entity sv_bufif0 is
    port (
        y    : out logic3d;
        data : in  logic3d;
        ctrl : in  logic3d
    );
end entity sv_bufif0;

architecture behavioral of sv_bufif0 is
begin
    process (data, ctrl)
        variable d, c : logic3d;
        variable result : logic3d;
    begin
        d := data;
        c := ctrl;
        if not is_uncertain(c) and is_zero(c) then
            if is_uncertain(d) then result := L3D_X;
            elsif is_one(d) then    result := L3D_1;
            else                    result := L3D_0;
            end if;
        elsif not is_uncertain(c) and is_one(c) then
            result := L3D_Z;
        else
            if is_uncertain(d) then result := L3D_X;
            elsif is_one(d) then    result := L3D_H;
            else                    result := L3D_L;
            end if;
        end if;
        y <= result;
    end process;
end architecture behavioral;

---------------------------------------------------------------------------
-- BUFIF1: Three-state buffer with active-high control
---------------------------------------------------------------------------
library ieee;
use ieee.std_logic_1164.all;
use work.logic3d_types_pkg.all;

entity sv_bufif1 is
    port (
        y    : out logic3d;
        data : in  logic3d;
        ctrl : in  logic3d
    );
end entity sv_bufif1;

architecture behavioral of sv_bufif1 is
begin
    process (data, ctrl)
        variable d, c : logic3d;
        variable result : logic3d;
    begin
        d := data;
        c := ctrl;
        if not is_uncertain(c) and is_one(c) then
            if is_uncertain(d) then result := L3D_X;
            elsif is_one(d) then    result := L3D_1;
            else                    result := L3D_0;
            end if;
        elsif not is_uncertain(c) and is_zero(c) then
            result := L3D_Z;
        else
            if is_uncertain(d) then result := L3D_X;
            elsif is_one(d) then    result := L3D_H;
            else                    result := L3D_L;
            end if;
        end if;
        y <= result;
    end process;
end architecture behavioral;

---------------------------------------------------------------------------
-- NOTIF0: Three-state inverter with active-low control
---------------------------------------------------------------------------
library ieee;
use ieee.std_logic_1164.all;
use work.logic3d_types_pkg.all;

entity sv_notif0 is
    port (
        y    : out logic3d;
        data : in  logic3d;
        ctrl : in  logic3d
    );
end entity sv_notif0;

architecture behavioral of sv_notif0 is
begin
    process (data, ctrl)
        variable d, c : logic3d;
        variable result : logic3d;
    begin
        d := data;
        c := ctrl;
        if not is_uncertain(c) and is_zero(c) then
            if is_uncertain(d) then result := L3D_X;
            elsif is_one(d) then    result := L3D_0;
            else                    result := L3D_1;
            end if;
        elsif not is_uncertain(c) and is_one(c) then
            result := L3D_Z;
        else
            if is_uncertain(d) then result := L3D_X;
            elsif is_one(d) then    result := L3D_L;
            else                    result := L3D_H;
            end if;
        end if;
        y <= result;
    end process;
end architecture behavioral;

---------------------------------------------------------------------------
-- NOTIF1: Three-state inverter with active-high control
---------------------------------------------------------------------------
library ieee;
use ieee.std_logic_1164.all;
use work.logic3d_types_pkg.all;

entity sv_notif1 is
    port (
        y    : out logic3d;
        data : in  logic3d;
        ctrl : in  logic3d
    );
end entity sv_notif1;

architecture behavioral of sv_notif1 is
begin
    process (data, ctrl)
        variable d, c : logic3d;
        variable result : logic3d;
    begin
        d := data;
        c := ctrl;
        if not is_uncertain(c) and is_one(c) then
            if is_uncertain(d) then result := L3D_X;
            elsif is_one(d) then    result := L3D_0;
            else                    result := L3D_1;
            end if;
        elsif not is_uncertain(c) and is_zero(c) then
            result := L3D_Z;
        else
            if is_uncertain(d) then result := L3D_X;
            elsif is_one(d) then    result := L3D_L;
            else                    result := L3D_H;
            end if;
        end if;
        y <= result;
    end process;
end architecture behavioral;

---------------------------------------------------------------------------
-- STRENGTH BUFFER: a continuous assign with a drive-strength spec,
--   assign (STR1, STR0) y = data;
-- Generic strengths use the l3ds codes (ST_HIGHZ=0 / ST_WEAK=2 /
-- ST_PULL=4 / ST_STRONG=8 / ST_SUPPLY=16).  The driven value enters net
-- resolution through y'driver at the exact specified strength; the port
-- itself never drives (kernel net solver / generated resolver carries
-- the contribution, mirroring the sv_tran family).
---------------------------------------------------------------------------
library ieee;
use ieee.std_logic_1164.all;
use work.logic3d_types_pkg.all;
use work.logic3ds_pkg.all;

entity sv_strength_buf is
    generic (
        str1 : natural := 8;   -- strength when driving 1
        str0 : natural := 8    -- strength when driving 0
    );
    port (
        y    : inout logic3d;
        data : in    logic3d
    );
end entity sv_strength_buf;

architecture strength of sv_strength_buf is
begin
    process (data)
        variable v : logic3ds;
        variable smax : natural;
    begin
        if str1 > str0 then
            smax := str1;
        else
            smax := str0;
        end if;

        if data = L3D_Z then
            -- A released r-value contributes nothing: the assign's
            -- own strength applies to driven values only
            v := L3DS_Z;
        elsif is_uncertain(data) then
            v := (value => 0, strength => smax,
                  flags => FL_UNKNOWN, reserved => 0);
        elsif is_one(data) then
            if str1 = 0 then
                v := L3DS_Z;
            else
                v := (value => 255, strength => str1,
                      flags => FL_KNOWN, reserved => 0);
            end if;
        else
            if str0 = 0 then
                v := L3DS_Z;
            else
                v := (value => 0, strength => str0,
                      flags => FL_KNOWN, reserved => 0);
            end if;
        end if;

        y'driver := v;
    end process;
end architecture strength;
