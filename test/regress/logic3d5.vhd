-- Test for 3D logic type: whole record port assignment
-- Tests assigning entire record through entity port
-- NOTE: This test exposes a bug where the first boolean field of a record
-- is corrupted when assigned through an entity port. GHDL passes this test.

package logic3d5_pkg is
    type logic_3d_t is record
        value    : boolean;
        strength : boolean;
        uncertain: boolean;
    end record logic_3d_t;

    type logic_3d_vector is array (natural range <>) of logic_3d_t;

    constant L3D_0 : logic_3d_t := (value => false, strength => true,  uncertain => false);
    constant L3D_1 : logic_3d_t := (value => true,  strength => true,  uncertain => false);
    constant L3D_Z : logic_3d_t := (value => false, strength => false, uncertain => true);

    function resolved (s : logic_3d_vector) return logic_3d_t;
    subtype logic_3d_r is resolved logic_3d_t;
end package;

package body logic3d5_pkg is
    function resolved (s : logic_3d_vector) return logic_3d_t is
        variable has_strong : boolean := false;
        variable strong_val : boolean := false;
    begin
        if s'length = 0 then
            return L3D_Z;
        end if;
        for i in s'range loop
            if not s(i).uncertain and s(i).strength then
                if has_strong then
                    if strong_val /= s(i).value then
                        return (value => false, strength => true, uncertain => true);  -- X
                    end if;
                else
                    has_strong := true;
                    strong_val := s(i).value;
                end if;
            end if;
        end loop;
        if has_strong then
            return (value => strong_val, strength => true, uncertain => false);
        end if;
        return L3D_Z;
    end function;
end package body;

-- Child entity that outputs L3D_1 as whole record
library work;
use work.logic3d5_pkg.all;

entity logic3d5_child is
    port (
        q : out logic_3d_r
    );
end entity;

architecture behavioral of logic3d5_child is
begin
    process
    begin
        q <= L3D_1;  -- Assign entire record
        wait;
    end process;
end architecture;

-- Parent testbench
library work;
use work.logic3d5_pkg.all;

entity logic3d5 is
end entity;

architecture test of logic3d5 is
    signal s : logic_3d_r;
begin

    uut: entity work.logic3d5_child port map (q => s);

    process
    begin
        wait for 1 ns;
        wait for 0 ns;  -- Delta cycle for signal update

        -- This is the exact bug scenario: first boolean field corrupted
        assert s.value = true
            report "BUG: s.value should be TRUE but is " & boolean'image(s.value)
            severity failure;

        assert s.strength = true
            report "BUG: s.strength should be TRUE but is " & boolean'image(s.strength)
            severity failure;

        assert s.uncertain = false
            report "BUG: s.uncertain should be FALSE but is " & boolean'image(s.uncertain)
            severity failure;

        assert s = L3D_1
            report "s should equal L3D_1"
            severity failure;

        wait;
    end process;

end architecture;
