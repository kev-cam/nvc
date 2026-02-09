-- Test for 3D logic type: resolution function
-- Tests resolved record type with multiple drivers from separate entities

package logic3d2_pkg is
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
    constant L3D_L : logic_3d_t := (value => false, strength => false, uncertain => false);
    constant L3D_H : logic_3d_t := (value => true,  strength => false, uncertain => false);
    constant L3D_W : logic_3d_t := (value => true,  strength => false, uncertain => true);

    -- Resolution function
    function resolved (s : logic_3d_vector) return logic_3d_t;
    subtype logic_3d_r is resolved logic_3d_t;
end package;

package body logic3d2_pkg is
    function resolved (s : logic_3d_vector) return logic_3d_t is
        variable result : logic_3d_t := L3D_Z;
        variable has_strong : boolean := false;
        variable strong_val : boolean := false;
        variable has_weak   : boolean := false;
        variable weak_val   : boolean := false;
        variable strong_conflict : boolean := false;
        variable weak_conflict   : boolean := false;
    begin
        if s'length = 0 then
            return L3D_Z;
        end if;

        for i in s'range loop
            if not s(i).uncertain then
                if s(i).strength then
                    if has_strong then
                        if strong_val /= s(i).value then
                            strong_conflict := true;
                        end if;
                    else
                        has_strong := true;
                        strong_val := s(i).value;
                    end if;
                else
                    if has_weak then
                        if weak_val /= s(i).value then
                            weak_conflict := true;
                        end if;
                    else
                        has_weak := true;
                        weak_val := s(i).value;
                    end if;
                end if;
            elsif s(i).strength then
                strong_conflict := true;
                has_strong := true;
            end if;
        end loop;

        if strong_conflict then
            return L3D_X;
        elsif has_strong then
            return (value => strong_val, strength => true, uncertain => false);
        elsif weak_conflict then
            return L3D_W;
        elsif has_weak then
            return (value => weak_val, strength => false, uncertain => false);
        else
            return L3D_Z;
        end if;
    end function resolved;
end package body;

-- Driver entity 1
library work;
use work.logic3d2_pkg.all;

entity logic3d2_drv1 is
    port (q : out logic_3d_r);
end entity;

architecture behavioral of logic3d2_drv1 is
begin
    process
    begin
        q <= L3D_Z;
        wait for 1 ns;
        q <= L3D_1;  -- Strong 1
        wait for 1 ns;
        q <= L3D_0;  -- Strong 0
        wait for 1 ns;
        q <= L3D_H;  -- Weak 1
        wait;
    end process;
end architecture;

-- Driver entity 2
library work;
use work.logic3d2_pkg.all;

entity logic3d2_drv2 is
    port (q : out logic_3d_r);
end entity;

architecture behavioral of logic3d2_drv2 is
begin
    process
    begin
        q <= L3D_Z;  -- Always Z so driver 1 dominates
        wait;
    end process;
end architecture;

-- Testbench
library work;
use work.logic3d2_pkg.all;

entity logic3d2 is
end entity;

architecture test of logic3d2 is
    signal s : logic_3d_r;
begin
    -- Two drivers on signal s
    drv1: entity work.logic3d2_drv1 port map (q => s);
    drv2: entity work.logic3d2_drv2 port map (q => s);

    -- Checker process
    process is
    begin
        -- Wait for drivers to initialize + delta cycle
        wait for 0 ns;
        wait for 0 ns;
        -- Both drivers are Z, result should be Z
        assert s.uncertain = true report "1: s should be uncertain (Z)";
        assert s.strength = false report "1: s should be weak (Z)";

        wait for 1 ns;
        wait for 0 ns;  -- Delta cycle for signal to update
        -- Driver 1 = L3D_1 (strong 1), Driver 2 = Z
        -- Strong beats Z, result should be 1
        assert s.value = true report "2: s.value should be true (1 vs Z)";
        assert s.strength = true report "2: s.strength should be true";
        assert s.uncertain = false report "2: s.uncertain should be false";

        wait for 1 ns;
        wait for 0 ns;
        -- Driver 1 = L3D_0 (strong 0), Driver 2 = Z
        assert s.value = false report "3: s.value should be false (0 vs Z)";
        assert s.strength = true report "3: s.strength should be true";
        assert s.uncertain = false report "3: s.uncertain should be false";

        wait for 1 ns;
        wait for 0 ns;
        -- Driver 1 = L3D_H (weak 1), Driver 2 = Z
        assert s.value = true report "4: s.value should be true (H vs Z)";
        assert s.strength = false report "4: s.strength should be false (weak)";
        assert s.uncertain = false report "4: s.uncertain should be false";

        wait;
    end process;

end architecture;
