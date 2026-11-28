-- Conversions in the port map of an instance of another architecture of the
-- same entity: the actual inside the conversion names a port of the enclosing
-- architecture, which is the instance's port declaration too.

entity selfinst2_sub is
    port (
        a, b : in bit;
        y, z : out bit );
end entity;

architecture leaf of selfinst2_sub is
begin
    y <= a;
    z <= b;
end architecture;

architecture conv_in of selfinst2_sub is
    function inv (x : bit) return bit is
    begin
        return not x;
    end function;
begin
    u : entity work.selfinst2_sub(leaf)
        port map (
            a => inv(b),                -- A conversion of the outer port B
            b => a,
            y => y,
            z => z );
end architecture;

architecture conv_out of selfinst2_sub is
    function inv (x : bit) return bit is
    begin
        return not x;
    end function;
begin
    u : entity work.selfinst2_sub(leaf)
        port map (
            a => a,
            b => b,
            inv(y) => z,                -- The outer port Z from the inner Y
            z => y );
end architecture;

-------------------------------------------------------------------------------

entity selfinst2_num is
    port (
        i : in integer;
        r : in real;
        o : out integer );
end entity;

architecture leaf of selfinst2_num is
begin
    o <= i;
end architecture;

architecture wrap of selfinst2_num is
    signal r2 : real := 7.0;
begin
    u : entity work.selfinst2_num(leaf)
        port map (
            i => integer(r),            -- A type conversion of the outer R
            r => r2,
            o => o );
end architecture;

-------------------------------------------------------------------------------

entity selfinst2 is
end entity;

architecture test of selfinst2 is
    signal a, b, y1, z1, y2, z2 : bit;
    signal r : real := 3.0;
    signal o : integer;
begin

    u1 : entity work.selfinst2_sub(conv_in)
        port map ( a, b, y1, z1 );      -- Y1 = not B, Z1 = A

    u2 : entity work.selfinst2_sub(conv_out)
        port map ( a, b, y2, z2 );      -- Y2 = B, Z2 = not A

    u3 : entity work.selfinst2_num(wrap)
        port map ( 0, r, o );

    check : process is
    begin
        wait for 1 ns;
        assert y1 = '1' and z1 = '0';
        assert y2 = '0' and z2 = '1';
        assert o = 3;
        b <= '1';
        r <= 42.0;
        wait for 1 ns;
        assert y1 = '0' and z1 = '0';
        assert y2 = '1' and z2 = '1';
        assert o = 42;
        a <= '1';
        wait for 1 ns;
        assert y1 = '0' and z1 = '1';
        assert y2 = '1' and z2 = '0';
        b <= '0';
        wait for 1 ns;
        assert y1 = '1' and z1 = '1';
        assert y2 = '0' and z2 = '0';
        wait;
    end process;

end architecture;
