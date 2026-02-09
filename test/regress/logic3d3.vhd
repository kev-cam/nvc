-- Test for 3D logic type: logical operators
-- Tests overloaded AND, OR, XOR, NOT operators

entity logic3d3 is
end entity;

architecture test of logic3d3 is

    type logic_3d_t is record
        value    : boolean;
        strength : boolean;
        uncertain: boolean;
    end record logic_3d_t;

    constant L3D_0 : logic_3d_t := (value => false, strength => true,  uncertain => false);
    constant L3D_1 : logic_3d_t := (value => true,  strength => true,  uncertain => false);
    constant L3D_X : logic_3d_t := (value => false, strength => true,  uncertain => true);
    constant L3D_L : logic_3d_t := (value => false, strength => false, uncertain => false);
    constant L3D_H : logic_3d_t := (value => true,  strength => false, uncertain => false);

    -- NOT operator
    function "not" (l : logic_3d_t) return logic_3d_t is
    begin
        if l.uncertain then
            return l;  -- not X = X
        else
            return (value => not l.value, strength => l.strength, uncertain => false);
        end if;
    end function "not";

    -- AND operator
    function "and" (l, r : logic_3d_t) return logic_3d_t is
    begin
        -- 0 AND anything = 0 (strong 0 dominates)
        if (not l.uncertain and not l.value and l.strength) or
           (not r.uncertain and not r.value and r.strength) then
            return L3D_0;
        end if;
        -- If either is unknown, result is unknown
        if l.uncertain or r.uncertain then
            return L3D_X;
        end if;
        -- Both known
        return (value    => l.value and r.value,
                strength => l.strength and r.strength,
                uncertain => false);
    end function "and";

    -- OR operator
    function "or" (l, r : logic_3d_t) return logic_3d_t is
    begin
        -- 1 OR anything = 1 (strong 1 dominates)
        if (not l.uncertain and l.value and l.strength) or
           (not r.uncertain and r.value and r.strength) then
            return L3D_1;
        end if;
        -- If either is unknown, result is unknown
        if l.uncertain or r.uncertain then
            return L3D_X;
        end if;
        -- Both known
        return (value    => l.value or r.value,
                strength => l.strength and r.strength,
                uncertain => false);
    end function "or";

    -- XOR operator
    function "xor" (l, r : logic_3d_t) return logic_3d_t is
    begin
        if l.uncertain or r.uncertain then
            return L3D_X;
        end if;
        return (value    => l.value xor r.value,
                strength => l.strength and r.strength,
                uncertain => false);
    end function "xor";

begin

    process is
        variable a, b, r : logic_3d_t;
    begin
        -- Test NOT operator
        r := not L3D_0;
        assert r.value = true report "NOT 0 should be 1";
        assert r.strength = true report "NOT 0 should be strong";

        r := not L3D_1;
        assert r.value = false report "NOT 1 should be 0";

        r := not L3D_X;
        assert r.uncertain = true report "NOT X should be X (uncertain)";

        r := not L3D_H;
        assert r.value = false report "NOT H should be L (value)";
        assert r.strength = false report "NOT H should be weak";

        -- Test AND operator
        r := L3D_1 and L3D_1;
        assert r.value = true report "1 AND 1 = 1";
        assert r.strength = true report "1 AND 1 should be strong";

        r := L3D_1 and L3D_0;
        assert r.value = false report "1 AND 0 = 0";

        r := L3D_0 and L3D_0;
        assert r.value = false report "0 AND 0 = 0";

        r := L3D_0 and L3D_X;
        assert r.value = false report "0 AND X = 0 (strong 0 dominates)";
        assert r.uncertain = false report "0 AND X should not be uncertain";

        r := L3D_1 and L3D_X;
        assert r.uncertain = true report "1 AND X = X";

        -- Test strength reduction
        r := L3D_1 and L3D_H;
        assert r.value = true report "1 AND H = H (value)";
        assert r.strength = false report "1 AND H should be weak";

        -- Test OR operator
        r := L3D_0 or L3D_0;
        assert r.value = false report "0 OR 0 = 0";

        r := L3D_0 or L3D_1;
        assert r.value = true report "0 OR 1 = 1";

        r := L3D_1 or L3D_1;
        assert r.value = true report "1 OR 1 = 1";

        r := L3D_1 or L3D_X;
        assert r.value = true report "1 OR X = 1 (strong 1 dominates)";
        assert r.uncertain = false report "1 OR X should not be uncertain";

        r := L3D_0 or L3D_X;
        assert r.uncertain = true report "0 OR X = X";

        -- Test XOR operator
        r := L3D_0 xor L3D_0;
        assert r.value = false report "0 XOR 0 = 0";

        r := L3D_0 xor L3D_1;
        assert r.value = true report "0 XOR 1 = 1";

        r := L3D_1 xor L3D_1;
        assert r.value = false report "1 XOR 1 = 0";

        r := L3D_0 xor L3D_X;
        assert r.uncertain = true report "0 XOR X = X";

        wait;
    end process;

end architecture;
