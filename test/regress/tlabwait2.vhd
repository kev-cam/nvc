-- A procedure with a local too big for a TLAB waits: the local was put in
-- the eval arena by the evaluation that claims the TLAB, and must be claimed
-- along with it (the arena is reset at every evaluation, and the next one to
-- overflow its TLAB wrote over the local).

entity tlabwait2 is
end entity;

architecture test of tlabwait2 is
    signal clk : bit;
begin

    p1: process is
        variable count : natural := 0;

        procedure hold (n : positive; c : character) is
            variable big : string(1 to n) := (others => c);
        begin
            wait for 1 ns;
            clk <= not clk;
            wait for 1 ns;
            for i in big'range loop
                assert big(i) = c
                    report "big(" & integer'image(i) & ") is " & big(i)
                    severity failure;
            end loop;
            count := count + 1;
        end procedure;
    begin
        for i in 1 to 300 loop          -- Enough calls to be compiled
            hold(10, 'a');
        end loop;
        for i in 1 to 20 loop
            hold(100000 + i, 'b');
            wait for 1 ns;              -- The TLAB is released here
        end loop;
        assert count = 320;
        report "done";
        wait;
    end process;

    -- An evaluation in between that fills its TLAB too
    p2: process (clk) is
        function fill (n : natural; c : character) return string is
            variable s : string(1 to n) := (others => c);
        begin
            return s;
        end function;
    begin
        assert fill(150000, 'z')'length = 150000;
    end process;

end architecture;
