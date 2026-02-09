-- Test for 3D logic type: basic record operations
-- Tests record type with 3 boolean fields, constants, and basic operations

entity logic3d1 is
end entity;

architecture test of logic3d1 is

    -- 3D logic record type (unresolved)
    type logic_3d_t is record
        value    : boolean;   -- false=0, true=1
        strength : boolean;   -- false=weak, true=strong
        uncertain: boolean;   -- false=known, true=unknown
    end record logic_3d_t;

    -- Constants for common values
    constant L3D_0 : logic_3d_t := (value => false, strength => true,  uncertain => false);  -- '0'
    constant L3D_1 : logic_3d_t := (value => true,  strength => true,  uncertain => false);  -- '1'
    constant L3D_X : logic_3d_t := (value => false, strength => true,  uncertain => true);   -- 'X'
    constant L3D_Z : logic_3d_t := (value => false, strength => false, uncertain => true);   -- 'Z'
    constant L3D_L : logic_3d_t := (value => false, strength => false, uncertain => false);  -- 'L'
    constant L3D_H : logic_3d_t := (value => true,  strength => false, uncertain => false);  -- 'H'

begin

    process is
        variable v : logic_3d_t;
    begin
        -- Test constant values
        assert L3D_0.value = false report "L3D_0.value should be false";
        assert L3D_0.strength = true report "L3D_0.strength should be true";
        assert L3D_0.uncertain = false report "L3D_0.uncertain should be false";

        assert L3D_1.value = true report "L3D_1.value should be true";
        assert L3D_1.strength = true report "L3D_1.strength should be true";
        assert L3D_1.uncertain = false report "L3D_1.uncertain should be false";

        assert L3D_X.uncertain = true report "L3D_X.uncertain should be true";
        assert L3D_X.strength = true report "L3D_X.strength should be true";

        assert L3D_Z.uncertain = true report "L3D_Z.uncertain should be true";
        assert L3D_Z.strength = false report "L3D_Z.strength should be false";

        assert L3D_L.value = false report "L3D_L.value should be false";
        assert L3D_L.strength = false report "L3D_L.strength should be false";
        assert L3D_L.uncertain = false report "L3D_L.uncertain should be false";

        assert L3D_H.value = true report "L3D_H.value should be true";
        assert L3D_H.strength = false report "L3D_H.strength should be false";
        assert L3D_H.uncertain = false report "L3D_H.uncertain should be false";

        -- Test variable assignment
        v := L3D_1;
        assert v.value = true report "v.value should be true after L3D_1 assignment";
        assert v.strength = true report "v.strength should be true after L3D_1 assignment";
        assert v.uncertain = false report "v.uncertain should be false after L3D_1 assignment";

        -- Test field-by-field assignment
        v.value := false;
        assert v.value = false report "v.value should be false after field assignment";
        assert v.strength = true report "v.strength should remain true";

        -- Test record comparison
        v := L3D_0;
        assert v = L3D_0 report "v should equal L3D_0";
        assert v /= L3D_1 report "v should not equal L3D_1";

        -- Test aggregate assignment
        v := (value => true, strength => false, uncertain => false);
        assert v = L3D_H report "aggregate should equal L3D_H";

        wait;
    end process;

end architecture;
