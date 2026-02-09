-- Test for 3D logic type: gate primitive simulation
-- Tests a simple AND gate using the 3D logic type

package logic3d6_pkg is
    type logic_3d_t is record
        value    : boolean;
        strength : boolean;
        uncertain: boolean;
    end record logic_3d_t;

    type logic_3d_vector is array (natural range <>) of logic_3d_t;

    constant L3D_0 : logic_3d_t := (value => false, strength => true,  uncertain => false);
    constant L3D_1 : logic_3d_t := (value => true,  strength => true,  uncertain => false);
    constant L3D_X : logic_3d_t := (value => false, strength => true,  uncertain => true);
    constant L3D_Z : logic_3d_t := (value => false, strength => false, uncertain => true);

    function resolved (s : logic_3d_vector) return logic_3d_t;
    subtype logic_3d_r is resolved logic_3d_t;

    function "and" (l, r : logic_3d_t) return logic_3d_t;
end package;

package body logic3d6_pkg is
    function resolved (s : logic_3d_vector) return logic_3d_t is
    begin
        if s'length = 0 then
            return L3D_Z;
        end if;
        return s(s'low);
    end function;

    function "and" (l, r : logic_3d_t) return logic_3d_t is
    begin
        if (not l.uncertain and not l.value and l.strength) or
           (not r.uncertain and not r.value and r.strength) then
            return L3D_0;
        end if;
        if l.uncertain or r.uncertain then
            return L3D_X;
        end if;
        return (value    => l.value and r.value,
                strength => l.strength and r.strength,
                uncertain => false);
    end function;
end package body;

-- AND gate entity
library work;
use work.logic3d6_pkg.all;

entity logic3d6_and is
    generic (n : positive := 2);
    port (
        y : out logic_3d_r;
        a : in  logic_3d_vector(0 to n-1)
    );
end entity;

architecture behavioral of logic3d6_and is
begin
    process (a)
        variable result : logic_3d_t := L3D_1;
    begin
        result := L3D_1;
        for i in a'range loop
            result := result and a(i);
        end loop;
        y <= result;
    end process;
end architecture;

-- Testbench
library work;
use work.logic3d6_pkg.all;

entity logic3d6 is
end entity;

architecture test of logic3d6 is
    signal inputs : logic_3d_vector(0 to 1);
    signal output : logic_3d_r;
begin

    uut: entity work.logic3d6_and
        generic map (n => 2)
        port map (y => output, a => inputs);

    process
    begin
        -- Test 1 AND 1 = 1
        inputs(0) <= L3D_1;
        inputs(1) <= L3D_1;
        wait for 1 ns;
        wait for 0 ns;  -- Delta cycle

        assert output.value = true
            report "1 AND 1: output.value should be TRUE"
            severity failure;
        assert output.strength = true
            report "1 AND 1: output.strength should be TRUE"
            severity failure;
        assert output.uncertain = false
            report "1 AND 1: output.uncertain should be FALSE"
            severity failure;

        -- Test 1 AND 0 = 0
        inputs(1) <= L3D_0;
        wait for 1 ns;
        wait for 0 ns;  -- Delta cycle

        assert output.value = false
            report "1 AND 0: output.value should be FALSE"
            severity failure;
        assert output.strength = true
            report "1 AND 0: output.strength should be TRUE"
            severity failure;

        -- Test 0 AND 0 = 0
        inputs(0) <= L3D_0;
        wait for 1 ns;
        wait for 0 ns;  -- Delta cycle

        assert output.value = false
            report "0 AND 0: output.value should be FALSE"
            severity failure;

        -- Test 0 AND X = 0 (strong 0 dominates)
        inputs(1) <= L3D_X;
        wait for 1 ns;
        wait for 0 ns;  -- Delta cycle

        assert output.value = false
            report "0 AND X: output.value should be FALSE"
            severity failure;
        assert output.uncertain = false
            report "0 AND X: output should not be uncertain"
            severity failure;

        -- Test 1 AND X = X
        inputs(0) <= L3D_1;
        wait for 1 ns;
        wait for 0 ns;  -- Delta cycle

        assert output.uncertain = true
            report "1 AND X: output should be uncertain"
            severity failure;

        wait;
    end process;

end architecture;
