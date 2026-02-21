-- Test: deposit to output port propagation
-- Verifies deposit on an out port propagates through port maps

library ieee;
use ieee.std_logic_1164.all;

entity deposit_inner is
    port (
        inp : in  std_logic;
        outp : out std_logic
    );
end entity;

architecture rtl of deposit_inner is
begin
    process (inp)
    begin
        outp := inp;  -- deposit to output port
    end process;
end architecture;

library ieee;
use ieee.std_logic_1164.all;

entity test_deposit_port is
end entity;

architecture test of test_deposit_port is
    signal drv : std_logic := '0';
    signal result : std_logic;
begin
    uut: entity work.deposit_inner
        port map (inp => drv, outp => result);

    process
    begin
        drv <= '1';
        wait for 1 ns;
        assert result = '1'
            report "FAIL: deposit to out port did not propagate"
            severity failure;
        report "PASS: deposit to out port propagated correctly"
            severity note;

        drv <= '0';
        wait for 1 ns;
        assert result = '0'
            report "FAIL: second deposit did not propagate"
            severity failure;
        report "PASS: second deposit propagated correctly"
            severity note;

        wait;
    end process;
end architecture;
