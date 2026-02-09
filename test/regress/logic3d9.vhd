-- Test for 3D logic type: conflict resolution (0 vs 1 -> X)

library work;
use work.logic3d8_pkg.all;

entity logic3d9 is
end entity;

architecture test of logic3d9 is
    signal s_conflict : logic_3d_r;  -- 0 vs 1 -> X
begin

    -- Conflicting drivers: 0 and 1 -> should be X
    drv_0: entity work.logic3d8_drv generic map (L3D_0) port map (s_conflict);
    drv_1: entity work.logic3d8_drv generic map (L3D_1) port map (s_conflict);

    process
    begin
        wait for 1 ns;
        wait for 0 ns;

        report "Conflict (0 vs 1): s = (" &
            boolean'image(s_conflict.value) & ", " &
            boolean'image(s_conflict.strength) & ", " &
            boolean'image(s_conflict.uncertain) & ")";

        assert s_conflict = L3D_X
            report "FAIL: 0 vs 1 conflict should give L3D_X"
            severity failure;

        report "PASSED: Conflict resolution works!";
        wait;
    end process;

end architecture;
