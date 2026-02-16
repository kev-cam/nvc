-- SystemVerilog Bidirectional Switches
-- IEEE 1800 Section 28.8
--
-- Modeled using 'driver/'other implicit signals on inout ports.
-- 'DRIVER  = implicit signal for what this entity drives onto the net
-- 'OTHER   = implicit signal for the resolved value excluding this driver
-- 'DRIVER and 'OTHER are universal (untyped).
--
-- Two architecture variants per entity:
--   behavioral : uses logic3d (3-bit, no strength resolution)
--   strength   : uses logic3ds (4-byte, full IEEE strength resolution)
-- Same entity (std_logic ports); translator selects architecture.
--
-- logic3d encoding: bit2=uncertain, bit1=driven, bit0=value
-- Blocked:        x mod L3D_DRIVEN                    (keep value, clear driven)
-- Uncertain ctrl: (x mod L3D_DRIVEN) + L3D_UNCERTAIN  (keep value, mark uncertain)

---------------------------------------------------------------------------
-- TRAN: Always-on bidirectional switch
---------------------------------------------------------------------------
library ieee;
use ieee.std_logic_1164.all;

entity sv_tran is
    port (
        a : inout std_logic;
        b : inout std_logic
    );
end entity sv_tran;

architecture behavioral of sv_tran is
begin
    process (a'other, b'other)
    begin
        a'driver := b'other;
        b'driver := a'other;
    end process;
end architecture behavioral;

-- Strength-aware: pass logic3ds values through unchanged
library ieee;
use ieee.std_logic_1164.all;
use work.logic3ds_pkg.all;

architecture strength of sv_tran is
begin
    process (a'other, b'other)
        variable a_val, b_val : logic3ds;
    begin
        a_val := b'other;
        b_val := a'other;
        a'driver := a_val;
        b'driver := b_val;
    end process;
end architecture strength;

---------------------------------------------------------------------------
-- TRANIF0: Bidirectional switch, conducts when ctrl=0
---------------------------------------------------------------------------
library ieee;
use ieee.std_logic_1164.all;
use work.logic3d_types_pkg.all;

entity sv_tranif0 is
    port (
        a    : inout std_logic;
        b    : inout std_logic;
        ctrl : in    std_logic
    );
end entity sv_tranif0;

architecture behavioral of sv_tranif0 is
begin
    process (a'other, b'other, ctrl)
        variable c : logic3d;
        variable a_val, b_val : logic3d;
    begin
        c := to_logic3d(ctrl);
        a_val := b'other;
        b_val := a'other;

        if is_one(c) then
            a_val := a_val mod L3D_DRIVEN;
            b_val := b_val mod L3D_DRIVEN;
        elsif is_uncertain(c) then
            a_val := (a_val mod L3D_DRIVEN) + L3D_UNCERTAIN;
            b_val := (b_val mod L3D_DRIVEN) + L3D_UNCERTAIN;
        end if;

        a'driver := a_val;
        b'driver := b_val;
    end process;
end architecture behavioral;

-- Strength-aware tranif0
library ieee;
use ieee.std_logic_1164.all;
use work.logic3d_types_pkg.all;
use work.logic3ds_pkg.all;

architecture strength of sv_tranif0 is
begin
    process (a'other, b'other, ctrl)
        variable c : logic3d;
        variable a_val, b_val : logic3ds;
    begin
        c := to_logic3d(ctrl);
        a_val := b'other;
        b_val := a'other;

        if is_one(c) then
            -- ctrl=1: tranif0 blocks
            a_val := L3DS_Z;
            b_val := L3DS_Z;
        elsif is_uncertain(c) then
            a_val := l3ds_set_unknown(a_val);
            b_val := l3ds_set_unknown(b_val);
        end if;

        a'driver := a_val;
        b'driver := b_val;
    end process;
end architecture strength;

---------------------------------------------------------------------------
-- TRANIF1: Bidirectional switch, conducts when ctrl=1
---------------------------------------------------------------------------
library ieee;
use ieee.std_logic_1164.all;
use work.logic3d_types_pkg.all;

entity sv_tranif1 is
    port (
        a    : inout std_logic;
        b    : inout std_logic;
        ctrl : in    std_logic
    );
end entity sv_tranif1;

architecture behavioral of sv_tranif1 is
begin
    process (a'other, b'other, ctrl)
        variable c : logic3d;
        variable a_val, b_val : logic3d;
    begin
        c := to_logic3d(ctrl);
        a_val := b'other;
        b_val := a'other;

        if is_zero(c) then
            a_val := a_val mod L3D_DRIVEN;
            b_val := b_val mod L3D_DRIVEN;
        elsif is_uncertain(c) then
            a_val := (a_val mod L3D_DRIVEN) + L3D_UNCERTAIN;
            b_val := (b_val mod L3D_DRIVEN) + L3D_UNCERTAIN;
        end if;

        a'driver := a_val;
        b'driver := b_val;
    end process;
end architecture behavioral;

-- Strength-aware tranif1
library ieee;
use ieee.std_logic_1164.all;
use work.logic3d_types_pkg.all;
use work.logic3ds_pkg.all;

architecture strength of sv_tranif1 is
begin
    process (a'other, b'other, ctrl)
        variable c : logic3d;
        variable a_val, b_val : logic3ds;
    begin
        c := to_logic3d(ctrl);
        a_val := b'other;
        b_val := a'other;

        if is_zero(c) then
            -- ctrl=0: tranif1 blocks
            a_val := L3DS_Z;
            b_val := L3DS_Z;
        elsif is_uncertain(c) then
            a_val := l3ds_set_unknown(a_val);
            b_val := l3ds_set_unknown(b_val);
        end if;

        a'driver := a_val;
        b'driver := b_val;
    end process;
end architecture strength;

---------------------------------------------------------------------------
-- RTRAN: Resistive always-on bidirectional switch
---------------------------------------------------------------------------
library ieee;
use ieee.std_logic_1164.all;

entity sv_rtran is
    port (
        a : inout std_logic;
        b : inout std_logic
    );
end entity sv_rtran;

-- logic3d: equivalent to tran (no strength to reduce)
architecture behavioral of sv_rtran is
begin
    process (a'other, b'other)
    begin
        a'driver := b'other;
        b'driver := a'other;
    end process;
end architecture behavioral;

-- Strength-aware: reduce strength by one level
library ieee;
use ieee.std_logic_1164.all;
use work.logic3ds_pkg.all;

architecture strength of sv_rtran is
begin
    process (a'other, b'other)
        variable a_val, b_val : logic3ds;
    begin
        a_val := b'other;
        b_val := a'other;
        a_val := l3ds_weaken(a_val);
        b_val := l3ds_weaken(b_val);
        a'driver := a_val;
        b'driver := b_val;
    end process;
end architecture strength;

---------------------------------------------------------------------------
-- RTRANIF0: Resistive bidirectional switch, conducts when ctrl=0
---------------------------------------------------------------------------
library ieee;
use ieee.std_logic_1164.all;
use work.logic3d_types_pkg.all;

entity sv_rtranif0 is
    port (
        a    : inout std_logic;
        b    : inout std_logic;
        ctrl : in    std_logic
    );
end entity sv_rtranif0;

-- logic3d: equivalent to tranif0 (no strength to reduce)
architecture behavioral of sv_rtranif0 is
begin
    process (a'other, b'other, ctrl)
        variable c : logic3d;
        variable a_val, b_val : logic3d;
    begin
        c := to_logic3d(ctrl);
        a_val := b'other;
        b_val := a'other;

        if is_one(c) then
            a_val := a_val mod L3D_DRIVEN;
            b_val := b_val mod L3D_DRIVEN;
        elsif is_uncertain(c) then
            a_val := (a_val mod L3D_DRIVEN) + L3D_UNCERTAIN;
            b_val := (b_val mod L3D_DRIVEN) + L3D_UNCERTAIN;
        end if;

        a'driver := a_val;
        b'driver := b_val;
    end process;
end architecture behavioral;

-- Strength-aware rtranif0: weaken + control gating
library ieee;
use ieee.std_logic_1164.all;
use work.logic3d_types_pkg.all;
use work.logic3ds_pkg.all;

architecture strength of sv_rtranif0 is
begin
    process (a'other, b'other, ctrl)
        variable c : logic3d;
        variable a_val, b_val : logic3ds;
    begin
        c := to_logic3d(ctrl);
        a_val := b'other;
        b_val := a'other;
        a_val := l3ds_weaken(a_val);
        b_val := l3ds_weaken(b_val);

        if is_one(c) then
            -- ctrl=1: rtranif0 blocks
            a_val := L3DS_Z;
            b_val := L3DS_Z;
        elsif is_uncertain(c) then
            a_val := l3ds_set_unknown(a_val);
            b_val := l3ds_set_unknown(b_val);
        end if;

        a'driver := a_val;
        b'driver := b_val;
    end process;
end architecture strength;

---------------------------------------------------------------------------
-- RTRANIF1: Resistive bidirectional switch, conducts when ctrl=1
---------------------------------------------------------------------------
library ieee;
use ieee.std_logic_1164.all;
use work.logic3d_types_pkg.all;

entity sv_rtranif1 is
    port (
        a    : inout std_logic;
        b    : inout std_logic;
        ctrl : in    std_logic
    );
end entity sv_rtranif1;

-- logic3d: equivalent to tranif1 (no strength to reduce)
architecture behavioral of sv_rtranif1 is
begin
    process (a'other, b'other, ctrl)
        variable c : logic3d;
        variable a_val, b_val : logic3d;
    begin
        c := to_logic3d(ctrl);
        a_val := b'other;
        b_val := a'other;

        if is_zero(c) then
            a_val := a_val mod L3D_DRIVEN;
            b_val := b_val mod L3D_DRIVEN;
        elsif is_uncertain(c) then
            a_val := (a_val mod L3D_DRIVEN) + L3D_UNCERTAIN;
            b_val := (b_val mod L3D_DRIVEN) + L3D_UNCERTAIN;
        end if;

        a'driver := a_val;
        b'driver := b_val;
    end process;
end architecture behavioral;

-- Strength-aware rtranif1: weaken + control gating
library ieee;
use ieee.std_logic_1164.all;
use work.logic3d_types_pkg.all;
use work.logic3ds_pkg.all;

architecture strength of sv_rtranif1 is
begin
    process (a'other, b'other, ctrl)
        variable c : logic3d;
        variable a_val, b_val : logic3ds;
    begin
        c := to_logic3d(ctrl);
        a_val := b'other;
        b_val := a'other;
        a_val := l3ds_weaken(a_val);
        b_val := l3ds_weaken(b_val);

        if is_zero(c) then
            -- ctrl=0: rtranif1 blocks
            a_val := L3DS_Z;
            b_val := L3DS_Z;
        elsif is_uncertain(c) then
            a_val := l3ds_set_unknown(a_val);
            b_val := l3ds_set_unknown(b_val);
        end if;

        a'driver := a_val;
        b'driver := b_val;
    end process;
end architecture strength;
