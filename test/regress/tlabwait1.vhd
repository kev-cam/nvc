-- A procedure that loops over calls to procedures that wait fills the TLAB
-- its process claimed: what overflows must still be there after the wait
-- (compiled code took it from the eval arena, which is reset at every
-- evaluation: the state of the suspended procedure was written over).

entity tlabwait1 is
end entity;

architecture test of tlabwait1 is
    signal clk  : bit;
    signal tock : natural;
begin

    p1: process is
        variable count : natural := 0;

        procedure pause (d : time) is
            constant t : time := now + d;
        begin
            while now < t loop
                wait for t - now;
            end loop;
        end procedure;

        function fill (n : natural; c : character) return string is
            variable s : string(1 to n) := (others => c);
        begin
            return s;
        end function;

        procedure tick (h : time := 5 ns) is
        begin
            pause(h);
            clk <= '1';
            -- A temporary: in the claimed TLAB while there is room, and
            -- after that where the state of TICK and PAUSE is too
            count := count + fill(1000 + count mod 7, 'x')'length / 1000 - 1;
            pause(h);
            clk <= '0';
            count := count + 1;
        end procedure;

        procedure run (n : natural) is
            constant start : time := now;
        begin
            for i in 1 to n loop
                tick;
                assert now = start + i * 10 ns
                    report "tick " & integer'image(i) & " ended at "
                    & time'image(now) severity failure;
            end loop;
        end procedure;
    begin
        for i in 1 to 300 loop          -- Enough calls to be compiled
            tick(1 ns);
        end loop;
        assert now = 600 ns;
        run(5000);
        assert count = 5300;
        assert now = 50600 ns;
        report "done";
        wait;
    end process;

    -- Evaluations in between, with something of their own to allocate
    p2: process (clk) is
        function image (n : natural) return string is
        begin
            return "tock " & integer'image(n);
        end function;
    begin
        if clk = '1' then
            assert image(tock)'length >= 6;
            tock <= tock + 1;
        end if;
    end process;

end architecture;
