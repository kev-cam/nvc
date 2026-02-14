-- Test: deposit (:=) to 'DRIVER attribute
-- This is the primary use case for switch-level modeling

library ieee;
use ieee.std_logic_1164.all;

entity test_deposit3 is
end entity;

architecture test of test_deposit3 is
    signal net_a_driver : std_logic_vector(0 to 1) := "ZZ";
    signal net_a_others : std_logic_vector(0 to 1) := "ZZ";
begin
    -- Simplified tran-like behavior: each endpoint deposits to its driver,
    -- reads from its others. Resolver swaps: others(0) = driver(1), etc.

    -- Simulated tran endpoint 0
    ep0: process
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
        -- Deposit '1' into driver(0)
        net_a_driver(0) := '1';
        check(net_a_driver(0), '1', "driver(0) deposited '1'");

        -- Simulate resolver swap: others(1) = driver(0)
        net_a_others(1) := net_a_driver(0);
        check(net_a_others(1), '1', "others(1) got driver(0)='1'");

        -- Simulate reverse: deposit into driver(1), resolver puts to others(0)
        net_a_driver(1) := '0';
        net_a_others(0) := net_a_driver(1);
        check(net_a_others(0), '0', "others(0) got driver(1)='0'");

        -- Chain of deposits (no deltas needed)
        net_a_driver(0) := 'X';
        net_a_others(1) := net_a_driver(0);
        net_a_driver(1) := net_a_others(1);
        check(net_a_driver(1), 'X', "chained deposit: X propagated");

        report "=== DEPOSIT3 TEST COMPLETE: " &
               integer'image(pass_count) & " passed, " &
               integer'image(fail_count) & " failed ===" severity note;

        if fail_count > 0 then
            report "SOME TESTS FAILED" severity failure;
        end if;

        wait;
    end process;
end architecture;
