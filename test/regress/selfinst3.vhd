-- An architecture that instantiates itself: an expression actual names the
-- architecture's own signals, which every level declares.  And an instance
-- of another architecture of the entity with a block and a generate between
-- it and the port its actual names.

entity selfinst3_rec is
    generic ( depth : natural );
    port (
        d : in bit;
        q : out bit );
end entity;

architecture a of selfinst3_rec is
    signal s, t : bit;
    signal en   : bit := '1';
begin
    s <= d;

    g : if depth > 0 generate
        u : entity work.selfinst3_rec
            generic map ( depth => depth - 1 )
            port map ( d => en and s, q => t );
        q <= t;
    end generate;

    g0 : if depth = 0 generate
        q <= not s;
    end generate;
end architecture;

-------------------------------------------------------------------------------

entity selfinst3_sub is
    port (
        a, b : in bit;
        y, z : out bit );
end entity;

architecture leaf of selfinst3_sub is
begin
    y <= a;
    z <= b;
end architecture;

architecture wrap of selfinst3_sub is
    signal m1 : bit := '1';
begin
    blk : block is
        signal m2 : bit := '1';
    begin
        g : for k in 0 to 0 generate
            signal m3 : bit := '1';
        begin
            u : entity work.selfinst3_sub(leaf)
                port map (
                    a => a,
                    b => (m1 and m2 and m3) and b,
                    y => y,
                    z => z );
        end generate;
    end block;
end architecture;

-------------------------------------------------------------------------------

entity selfinst3 is
end entity;

architecture test of selfinst3 is
    signal d, q : bit;
    signal a, b, y, z : bit;
begin

    u1 : entity work.selfinst3_rec(a)
        generic map ( depth => 3 )
        port map ( d, q );

    u2 : entity work.selfinst3_sub(wrap)
        port map ( a, b, y, z );

    check : process is
    begin
        wait for 1 ns;
        assert q = '1';
        assert y = '0' and z = '0';
        d <= '1';
        b <= '1';
        wait for 1 ns;
        assert q = '0';
        assert y = '0' and z = '1';
        d <= '0';
        a <= '1';
        b <= '0';
        wait for 1 ns;
        assert q = '1';
        assert y = '1' and z = '0';
        wait;
    end process;

end architecture;
