-- Nothing here instantiates itself.  An expression actual takes its type from
-- the formal, and so from a generic of the instance: while its names are
-- looked up outside the instance (see selfinst1) the generic must still be
-- found inside.

entity selfinst4_sub is
    generic ( w : natural );
    port (
        v : in bit_vector(w - 1 downto 0);
        o : out bit_vector(w - 1 downto 0) );
end entity;

architecture rtl of selfinst4_sub is
begin
    o <= v;
end architecture;

-------------------------------------------------------------------------------

entity selfinst4 is
end entity;

architecture test of selfinst4 is
    component selfinst4_sub is
        generic ( w : natural );
        port (
            v : in bit_vector(w - 1 downto 0);
            o : out bit_vector(w - 1 downto 0) );
    end component;

    signal s      : bit;
    signal o4     : bit_vector(3 downto 0);
    signal o3     : bit_vector(2 downto 0);
begin

    u1 : entity work.selfinst4_sub
        generic map ( w => 4 )
        port map ( v => (others => s), o => o4 );

    u2 : component selfinst4_sub
        generic map ( w => 3 )
        port map ( v => (others => not s), o => o3 );

    check : process is
    begin
        wait for 1 ns;
        assert o4 = "0000";
        assert o3 = "111";
        s <= '1';
        wait for 1 ns;
        assert o4 = "1111";
        assert o3 = "000";
        wait;
    end process;

end architecture;
