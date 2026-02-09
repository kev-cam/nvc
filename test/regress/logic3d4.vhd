-- Test for 3D logic type: record port assignment
-- Tests record assignment through entity ports (the bug scenario)

entity logic3d4_child is
    port (
        q : out boolean;
        r : out boolean;
        s : out boolean
    );
end entity;

architecture behavioral of logic3d4_child is
begin
    process
    begin
        -- Assign all fields to true
        q <= true;
        r <= true;
        s <= false;
        wait;
    end process;
end architecture;

entity logic3d4 is
end entity;

architecture test of logic3d4 is

    type logic_3d_t is record
        value    : boolean;
        strength : boolean;
        uncertain: boolean;
    end record logic_3d_t;

    type logic_3d_vector is array (natural range <>) of logic_3d_t;

    constant L3D_1 : logic_3d_t := (value => true,  strength => true,  uncertain => false);
    constant L3D_Z : logic_3d_t := (value => false, strength => false, uncertain => true);

    function resolved (s : logic_3d_vector) return logic_3d_t is
    begin
        if s'length = 0 then
            return L3D_Z;
        end if;
        return s(s'low);
    end function resolved;

    subtype logic_3d_r is resolved logic_3d_t;

    signal sig : logic_3d_r;

begin

    -- Instantiate child with individual field connections
    uut: entity work.logic3d4_child
        port map (
            q => sig.value,
            r => sig.strength,
            s => sig.uncertain
        );

    process
    begin
        wait for 1 ns;

        -- Check that all fields were assigned correctly
        assert sig.value = true
            report "BUG: sig.value should be TRUE but is " & boolean'image(sig.value)
            severity failure;

        assert sig.strength = true
            report "BUG: sig.strength should be TRUE but is " & boolean'image(sig.strength)
            severity failure;

        assert sig.uncertain = false
            report "BUG: sig.uncertain should be FALSE but is " & boolean'image(sig.uncertain)
            severity failure;

        -- Verify it matches expected constant
        assert sig = L3D_1
            report "sig should equal L3D_1"
            severity failure;

        wait;
    end process;

end architecture;
