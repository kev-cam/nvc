-- A timed wait after an event wait must not be cut short by events on
-- the signals the earlier wait named
entity wait32 is
end entity;

architecture test of wait32 is
    signal s : natural := 0;
    type rec is record
        a : natural;
        b : real;
    end record;
    signal r : rec := (0, 0.0);
begin

    stim: process is
    begin
        for i in 1 to 20 loop
            wait for 2 us;
            s <= s + 1;
            r <= (i, 0.0);
        end loop;
        wait;
    end process;

    p1: process is
    begin
        wait on s;
        assert now = 2 us;
        wait for 5 us;
        assert now = 7 us report "first timed wait cut short" severity failure;
        wait for 5 us;
        assert now = 12 us report "second timed wait cut short" severity failure;
        wait on s for 1 us;
        assert now = 12 us and s'event;
        wait on s for 1 us;
        assert now = 13 us and not s'event;
        wait;
    end process;

    p2: process is
    begin
        loop
            wait on r;
            exit when r.a >= 2;
        end loop;
        assert now = 4 us;
        wait for 5 us;
        assert now = 9 us report "timed wait after a loop cut short" severity failure;
        wait until r.a = 10;
        assert now = 20 us;
        wait for 3 us;
        assert now = 23 us report "timed wait after wait until cut short" severity failure;
        wait;
    end process;

end architecture;
