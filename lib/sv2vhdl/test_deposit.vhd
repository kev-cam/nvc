-- Test blocking assignment (:=) to signals
-- Extension: signal_name := value deposits immediately (no delta cycle)

library ieee;
use ieee.std_logic_1164.all;

entity test_deposit is
end entity;

architecture test of test_deposit is
    signal a : std_logic := '0';
    signal b : std_logic := '0';
    signal c : std_logic_vector(3 downto 0) := "0000";
begin
    process
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
        -- Test 1: Basic blocking assignment to signal
        a := '1';
        -- Value should be immediately visible (no wait needed)
        check(a, '1', "T1: blocking assign a := '1'");

        -- Test 2: Second blocking assignment
        b := '1';
        check(b, '1', "T2: blocking assign b := '1'");

        -- Test 3: Blocking assignment overwrites
        a := '0';
        check(a, '0', "T3: blocking assign a := '0'");

        -- Test 4: Blocking assignment to vector
        c := "1010";
        assert c = "1010" report "FAIL: T4: blocking assign c := 1010"
            severity error;
        if c = "1010" then
            pass_count := pass_count + 1;
        else
            fail_count := fail_count + 1;
        end if;

        -- Test 5: Blocking assignment with expression
        a := b and '1';
        check(a, '1', "T5: blocking assign a := b and '1'");

        -- Test 6: Chain of blocking assignments (no delta delays)
        a := '0';
        b := a;
        check(b, '0', "T6: chain a:='0', b:=a -> b='0'");

        -- Summary
        report "=== DEPOSIT TEST COMPLETE: " &
               integer'image(pass_count) & " passed, " &
               integer'image(fail_count) & " failed ===" severity note;

        if fail_count > 0 then
            report "SOME TESTS FAILED" severity failure;
        end if;

        wait;
    end process;
end architecture;
