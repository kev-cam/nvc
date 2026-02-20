-- test_two_inv.vhd
-- Two inverters with outputs tied to the same net.
-- 'driver -> resolver -> 'receiver
-- No 'other (unidirectional drivers only).

library ieee;
use ieee.std_logic_1164.all;
use work.logic3ds_pkg.all;

-- Inverter: deposits strength-aware value into y'driver
entity sv_inv is
    port (
        a : in std_logic;
        y : out std_logic
    );
end entity;

architecture strength of sv_inv is
begin
    process(a)
    begin
        if a = '0' then
            y'driver <= L3DS_1 after 1 ps;
        elsif a = '1' then
            y'driver <= L3DS_0 after 1 ps;
        else
            y'driver <= L3DS_X after 1 ps;
        end if;
    end process;
end architecture;

-- DUT
library ieee;
use ieee.std_logic_1164.all;

entity test_two_inv is end entity;

architecture test of test_two_inv is
    signal a, b, y : std_logic;
begin
    inv1: entity work.sv_inv(strength) port map(a => a, y => y);
    inv2: entity work.sv_inv(strength) port map(a => b, y => y);

    process
    begin
        a <= '0'; b <= '0';
        wait for 1 ns;
        report "a=0 b=0: y=" & std_logic'image(y);

        a <= '1'; b <= '1';
        wait for 1 ns;
        report "a=1 b=1: y=" & std_logic'image(y);

        a <= '0'; b <= '1';
        wait for 1 ns;
        report "a=0 b=1: y=" & std_logic'image(y);

        report "DONE";
        wait;
    end process;

end architecture;

-- Resolver: reads 'driver signals, writes 'receiver
library ieee;
use ieee.std_logic_1164.all;
use work.logic3ds_pkg.all;

entity rn_two_inv is end entity;

architecture gen of rn_two_inv is
    alias drv_0 is << signal .resolved_two_inv.dut.inv1.y.driver : logic3ds >>;
    alias drv_1 is << signal .resolved_two_inv.dut.inv2.y.driver : logic3ds >>;
    alias rcv   is << signal .resolved_two_inv.dut.y.receiver : std_logic >>;
begin
    process(drv_0, drv_1)
        variable result : std_logic;
    begin
        result := to_std_logic(l3ds_resolve(logic3ds_vector'(drv_0, drv_1)));
        report "TRACE drv_0=(v=" & integer'image(drv_0.value) & ",s=" & integer'image(l3ds_strength'pos(drv_0.strength)) & ",f=" & integer'image(l3ds_flags'pos(drv_0.flags)) & ")"
             & " drv_1=(v=" & integer'image(drv_1.value) & ",s=" & integer'image(l3ds_strength'pos(drv_1.strength)) & ",f=" & integer'image(l3ds_flags'pos(drv_1.flags)) & ")"
             & " => rcv=" & std_logic'image(result);
        rcv := result;
    end process;
end architecture;

-- Wrapper
entity resolved_two_inv is end entity;

architecture wrap of resolved_two_inv is
begin
    dut: entity work.test_two_inv;
    rn:  entity work.rn_two_inv;
end architecture;
