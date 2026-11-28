-- An architecture that instantiates another architecture of its own entity.
-- An actual that is an expression is evaluated in a process of the instance,
-- where the formal's name hid the enclosing port of the same name: the
-- instance's port B was then driven from itself.

entity selfinst1_sub is
    port (
        a, b : in bit;
        y, z : out bit );
end entity;

architecture leaf of selfinst1_sub is
begin
    y <= a;
    z <= b;
end architecture;

architecture wrap of selfinst1_sub is
    signal mask : bit := '1';
begin
    u : entity work.selfinst1_sub(leaf)
        port map (
            a => a,                     -- A plain name
            b => mask and b,            -- An expression of the outer port B
            y => y,
            z => z );
end architecture;

-------------------------------------------------------------------------------

entity selfinst1 is
end entity;

architecture test of selfinst1 is
    signal a, b, y, z : bit;
begin

    dut : entity work.selfinst1_sub(wrap)
        port map ( a, b, y, z );

    check : process is
    begin
        wait for 1 ns;
        assert y = '0' and z = '0';
        a <= '1';
        wait for 1 ns;
        assert y = '1' and z = '0';
        b <= '1';
        wait for 1 ns;
        assert y = '1' and z = '1';
        a <= '0';
        wait for 1 ns;
        assert y = '0' and z = '1';
        b <= '0';
        wait for 1 ns;
        assert y = '0' and z = '0';
        wait;
    end process;

end architecture;
