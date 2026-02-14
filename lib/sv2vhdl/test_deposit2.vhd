-- Test: deposit (:=) vs signal assignment (<=) timing behavior
-- Deposit should update immediately; signal assignment waits for delta

library ieee;
use ieee.std_logic_1164.all;

entity test_deposit2 is
end entity;

architecture test of test_deposit2 is
    signal s1 : std_logic := '0';
    signal s2 : std_logic := '0';
    signal done : boolean := false;
begin

    -- Monitor process: watches for events on s1 and s2
    monitor: process
    begin
        wait until done;
        wait;
    end process;

    stim: process
        variable pass_count : integer := 0;
        variable fail_count : integer := 0;

        procedure check(actual, expected : std_logic; msg : string) is
        begin
            if actual = expected then
                pass_count := pass_count + 1;
            else
                report "FAIL: " & msg severity error;
                fail_count := fail_count + 1;
            end if;
        end;
    begin
        -- Test: deposit is visible immediately, no wait needed
        s1 := '1';
        -- Read s1 immediately - should be '1' because deposit is instant
        check(s1, '1', "deposit visible immediately");

        -- Compare with signal assignment:
        -- s2 <= '1' wouldn't be visible until after a delta
        s2 <= '1';
        -- s2 is still '0' here because <= is scheduled
        check(s2, '0', "signal assign not yet visible");

        wait for 0 ns; -- one delta

        -- Now s2 should be updated
        check(s2, '1', "signal assign visible after delta");

        -- Test: multiple deposits in sequence, each immediately visible
        s1 := 'X';
        check(s1, 'X', "multi-deposit 1");
        s1 := 'Z';
        check(s1, 'Z', "multi-deposit 2");
        s1 := '0';
        check(s1, '0', "multi-deposit 3");

        -- Test: deposit followed by signal assignment (different value)
        s1 := '1';
        check(s1, '1', "deposit before signal assign");
        s1 <= 'X';   -- driver changes from '0' to 'X'
        check(s1, '1', "signal assign pending, deposit still visible");
        wait for 0 ns;
        check(s1, 'X', "signal assign took effect after delta");

        -- Summary
        report "=== DEPOSIT2 TEST COMPLETE: " &
               integer'image(pass_count) & " passed, " &
               integer'image(fail_count) & " failed ===" severity note;

        if fail_count > 0 then
            report "SOME TESTS FAILED" severity failure;
        end if;

        done <= true;
        wait;
    end process;
end architecture;
