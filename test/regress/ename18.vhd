entity ename18_sub is
end entity;

architecture test of ename18_sub is
    signal ac : bit_vector(0 to 3) := "0000";
    signal bd : bit_vector(7 downto 4) := "0000";
begin
    p1: process is
    begin
        wait for 5 ns;
        ac <= "1010";
        bd <= "1100";
        wait;
    end process;
end architecture;

entity ename18 is
end entity;

architecture test of ename18 is
begin
    uut: entity work.ename18_sub;

    p2: process is
    begin
        -- ascending range: 0 to 3
        assert << signal .ename18.uut.ac(0) : bit >> = '0';
        assert << signal .ename18.uut.ac(1) : bit >> = '0';
        assert << signal .ename18.uut.ac(2) : bit >> = '0';
        assert << signal .ename18.uut.ac(3) : bit >> = '0';

        -- descending range: 7 downto 4
        assert << signal .ename18.uut.bd(7) : bit >> = '0';
        assert << signal .ename18.uut.bd(4) : bit >> = '0';

        wait for 6 ns;

        -- ascending: ac = "1010" => ac(0)=1, ac(1)=0, ac(2)=1, ac(3)=0
        assert << signal .ename18.uut.ac(0) : bit >> = '1';
        assert << signal .ename18.uut.ac(1) : bit >> = '0';
        assert << signal .ename18.uut.ac(2) : bit >> = '1';
        assert << signal .ename18.uut.ac(3) : bit >> = '0';

        -- descending: bd = "1100" => bd(7)=1, bd(6)=1, bd(5)=0, bd(4)=0
        assert << signal .ename18.uut.bd(7) : bit >> = '1';
        assert << signal .ename18.uut.bd(6) : bit >> = '1';
        assert << signal .ename18.uut.bd(5) : bit >> = '0';
        assert << signal .ename18.uut.bd(4) : bit >> = '0';

        report "PASS";
        wait;
    end process;
end architecture;
