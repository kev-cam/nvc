-- test_deposit_rcv.vhd
-- Minimal test: deposit to 'receiver propagates to parent signal

library ieee;
use ieee.std_logic_1164.all;

-- DUT: just a signal
entity dut_deposit_rcv is end entity;

architecture arch of dut_deposit_rcv is
    signal y : std_logic;
begin
    process
    begin
        wait for 0 ps;  -- delta 1: deposit propagation
        wait for 0 ps;  -- delta 2: driving update applied
        report "t=0+2d: y=" & std_logic'image(y);
        assert y = '1' report "FAIL: expected y='1'" severity error;

        wait for 1 ns;
        wait for 0 ps;  -- propagation delta
        report "t=1ns+1d: y=" & std_logic'image(y);
        assert y = '0' report "FAIL: expected y='0'" severity error;

        wait for 1 ns;
        wait for 0 ps;  -- propagation delta
        report "t=2ns+1d: y=" & std_logic'image(y);
        assert y = 'X' report "FAIL: expected y='X'" severity error;

        report "PASS";
        wait;
    end process;
end architecture;

-- Resolver deposits into y'receiver via external names
library ieee;
use ieee.std_logic_1164.all;

entity rn_deposit_rcv is end entity;

architecture gen of rn_deposit_rcv is
    alias rcv is << signal .test_deposit_rcv.dut.y.receiver : std_logic >>;
begin
    process
    begin
        rcv := '1';
        report "deposited '1' to rcv";
        wait for 1 ns;

        rcv := '0';
        report "deposited '0' to rcv";
        wait for 1 ns;

        rcv := 'X';
        report "deposited 'X' to rcv";
        wait;
    end process;
end architecture;

-- Top: instantiates DUT + resolver side by side
entity test_deposit_rcv is end entity;

architecture wrap of test_deposit_rcv is
begin
    dut: entity work.dut_deposit_rcv;
    rn:  entity work.rn_deposit_rcv;
end architecture;
