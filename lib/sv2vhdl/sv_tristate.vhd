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
        y    : out std_logic;
        data : in  std_logic;
        ctrl : in  std_logic
    );
end entity sv_bufif0;

architecture behavioral of sv_bufif0 is
begin
    process (data, ctrl)
        variable d, c : logic3d;
        variable result : logic3d;
    begin
        d := to_logic3d(data);
        c := to_logic3d(ctrl);
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
        y <= to_std_logic(result);
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
        y    : out std_logic;
        data : in  std_logic;
        ctrl : in  std_logic
    );
end entity sv_bufif1;

architecture behavioral of sv_bufif1 is
begin
    process (data, ctrl)
        variable d, c : logic3d;
        variable result : logic3d;
    begin
        d := to_logic3d(data);
        c := to_logic3d(ctrl);
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
        y <= to_std_logic(result);
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
        y    : out std_logic;
        data : in  std_logic;
        ctrl : in  std_logic
    );
end entity sv_notif0;

architecture behavioral of sv_notif0 is
begin
    process (data, ctrl)
        variable d, c : logic3d;
        variable result : logic3d;
    begin
        d := to_logic3d(data);
        c := to_logic3d(ctrl);
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
        y <= to_std_logic(result);
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
        y    : out std_logic;
        data : in  std_logic;
        ctrl : in  std_logic
    );
end entity sv_notif1;

architecture behavioral of sv_notif1 is
begin
    process (data, ctrl)
        variable d, c : logic3d;
        variable result : logic3d;
    begin
        d := to_logic3d(data);
        c := to_logic3d(ctrl);
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
        y <= to_std_logic(result);
    end process;
end architecture behavioral;
