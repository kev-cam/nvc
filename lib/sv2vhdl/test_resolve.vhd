-- Test resolution functions and resolved subtypes for logic3d and logic3ds
-- Verifies:
--   1. l3d_resolve matches std_logic resolution semantics
--   2. resolved_logic3d works with multiple concurrent drivers
--   3. resolved_logic3ds works with multiple concurrent drivers
--   4. Lossless round-trip: std_logic -> logic3d -> std_logic

library ieee;
use ieee.std_logic_1164.all;
use work.logic3d_types_pkg.all;
use work.logic3ds_pkg.all;

entity test_resolve is
end entity;

architecture test of test_resolve is

    -- Test signals with resolved types (multiple drivers)
    signal r3d  : resolved_logic3d := L3D_Z;
    signal r3ds : resolved_logic3ds := L3DS_Z;

begin

    -- Driver 1 for r3d
    p1_3d: process
    begin
        r3d <= L3D_Z;
        wait for 1 ns;
        r3d <= L3D_0;   -- strong 0
        wait for 1 ns;
        r3d <= L3D_1;   -- strong 1
        wait for 1 ns;
        r3d <= L3D_L;   -- weak 0
        wait for 1 ns;
        r3d <= L3D_H;   -- weak 1
        wait for 1 ns;
        r3d <= L3D_Z;   -- release
        wait;
    end process;

    -- Driver 2 for r3d
    p2_3d: process
    begin
        r3d <= L3D_Z;
        wait for 1 ns;
        r3d <= L3D_Z;   -- Z: doesn't contribute
        wait for 1 ns;
        r3d <= L3D_0;   -- strong 0 vs strong 1 -> X
        wait for 1 ns;
        r3d <= L3D_H;   -- weak 1 vs weak 0 -> W
        wait for 1 ns;
        r3d <= L3D_0;   -- strong 0 vs weak 1 -> 0 wins
        wait for 1 ns;
        r3d <= L3D_Z;
        wait;
    end process;

    -- Driver 1 for r3ds
    p1_3ds: process
    begin
        r3ds <= L3DS_Z;
        wait for 1 ns;
        r3ds <= L3DS_0;       -- strong 0
        wait for 1 ns;
        r3ds <= L3DS_1;       -- strong 1 vs strong 0 -> X
        wait for 1 ns;
        r3ds <= L3DS_PULL0;   -- pull 0 vs weak 1 -> pull wins
        wait for 1 ns;
        r3ds <= L3DS_SU1;     -- supply 1 vs strong 0 -> supply wins
        wait for 1 ns;
        r3ds <= L3DS_Z;
        wait;
    end process;

    -- Driver 2 for r3ds
    p2_3ds: process
    begin
        r3ds <= L3DS_Z;
        wait for 1 ns;
        r3ds <= L3DS_Z;       -- Z: doesn't contribute
        wait for 1 ns;
        r3ds <= L3DS_0;       -- strong 0 vs strong 1
        wait for 1 ns;
        r3ds <= L3DS_WEAK1;   -- weak 1 vs pull 0
        wait for 1 ns;
        r3ds <= L3DS_0;       -- strong 0 vs supply 1
        wait for 1 ns;
        r3ds <= L3DS_Z;
        wait;
    end process;

    -- Checker process
    check: process
        -- Wait for drivers to settle (time step + delta cycle)
        procedure settle is begin wait for 0 ns; wait for 0 ns; end;
    begin
        -- t=0: both Z
        settle;
        assert r3d = L3D_Z
            report "t=0: expected Z, got " & to_char(r3d) severity failure;

        -- t=1ns: strong 0 + Z = strong 0
        wait for 1 ns; settle;
        assert r3d = L3D_0
            report "t=1ns: expected 0, got " & to_char(r3d) severity failure;
        assert r3ds = L3DS_0
            report "t=1ns 3ds: expected strong 0" severity failure;

        -- t=2ns: strong 1 + strong 0 = X (contention)
        wait for 1 ns; settle;
        assert r3d = L3D_X
            report "t=2ns: expected X, got " & to_char(r3d) severity failure;
        assert r3ds.flags = FL_UNKNOWN
            report "t=2ns 3ds: expected FL_UNKNOWN" severity failure;
        assert r3ds.strength = ST_STRONG
            report "t=2ns 3ds: expected ST_STRONG" severity failure;

        -- t=3ns: weak 0 + weak 1 = W (weak contention)
        wait for 1 ns; settle;
        assert r3d = L3D_W
            report "t=3ns: expected W, got " & to_char(r3d) severity failure;
        -- 3ds: pull 0 vs weak 1 -> pull wins (pull 0)
        assert r3ds.flags = FL_KNOWN
            report "t=3ns 3ds: expected FL_KNOWN" severity failure;
        assert r3ds.strength = ST_PULL
            report "t=3ns 3ds: expected ST_PULL" severity failure;
        assert r3ds.value = 0
            report "t=3ns 3ds: expected value=0" severity failure;

        -- t=4ns: weak 1 + strong 0 = strong 0 (strong wins)
        wait for 1 ns; settle;
        assert r3d = L3D_0
            report "t=4ns: expected 0, got " & to_char(r3d) severity failure;
        -- 3ds: supply 1 vs strong 0 -> supply wins
        assert r3ds.flags = FL_KNOWN
            report "t=4ns 3ds: expected FL_KNOWN" severity failure;
        assert r3ds.strength = ST_SUPPLY
            report "t=4ns 3ds: expected ST_SUPPLY" severity failure;
        assert r3ds.value = 255
            report "t=4ns 3ds: expected value=255 (logic 1)" severity failure;

        -- Test lossless round-trip: logic3d -> std_logic -> logic3d
        assert to_logic3d(to_std_logic(L3D_L)) = L3D_L report "round-trip L" severity failure;
        assert to_logic3d(to_std_logic(L3D_H)) = L3D_H report "round-trip H" severity failure;
        assert to_logic3d(to_std_logic(L3D_0)) = L3D_0 report "round-trip 0" severity failure;
        assert to_logic3d(to_std_logic(L3D_1)) = L3D_1 report "round-trip 1" severity failure;
        assert to_logic3d(to_std_logic(L3D_Z)) = L3D_Z report "round-trip Z" severity failure;
        assert to_logic3d(to_std_logic(L3D_W)) = L3D_W report "round-trip W" severity failure;
        assert to_logic3d(to_std_logic(L3D_X)) = L3D_X report "round-trip X" severity failure;
        assert to_logic3d(to_std_logic(L3D_U)) = L3D_U report "round-trip U" severity failure;

        -- Test std_logic -> logic3ds convenience
        assert to_logic3ds('0', ST_STRONG) = L3DS_0 report "std_logic->logic3ds '0'" severity failure;
        assert to_logic3ds('1', ST_STRONG) = L3DS_1 report "std_logic->logic3ds '1'" severity failure;
        assert to_logic3ds('Z', ST_STRONG) = L3DS_Z report "std_logic->logic3ds 'Z'" severity failure;

        -- Test functional l3d_resolve directly
        assert l3d_resolve((L3D_0, L3D_Z)) = L3D_0 report "resolve 0+Z" severity failure;
        assert l3d_resolve((L3D_0, L3D_1)) = L3D_X report "resolve 0+1" severity failure;
        assert l3d_resolve((L3D_L, L3D_H)) = L3D_W report "resolve L+H" severity failure;
        assert l3d_resolve((L3D_0, L3D_H)) = L3D_0 report "resolve 0+H" severity failure;
        assert l3d_resolve((L3D_Z, L3D_Z)) = L3D_Z report "resolve Z+Z" severity failure;

        report "ALL RESOLUTION TESTS PASSED" severity note;
        std.env.finish;
        wait;
    end process;

end architecture;
