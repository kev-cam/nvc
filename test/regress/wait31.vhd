-- A process that suspends on a dynamic `wait until` and then on a bare
-- `wait;` must never resume. The static-wait fast path promoted the process
-- after its first wait and kept the persistent registration on clk, so the
-- bare `wait;` woke on every clk event and the process re-ran from the top.
-- The clock generator (a static wait armed in the reset block) must keep
-- running.
entity wait31 is
end entity;

architecture test of wait31 is
    signal clk   : bit := '0';
    signal runs  : natural := 0;
    signal edges : natural := 0;
begin

    clk <= not clk after 2 ns;

    edges <= edges + 1 when clk'event and clk = '1';

    a: process is
    begin
        runs <= runs + 1;
        for i in 1 to 4 loop
            wait until clk = '1';
        end loop;
        wait;
    end process;

    check: process is
    begin
        wait for 40 ns;
        assert runs = 1
            report "process resumed after wait; ran " & natural'image(runs)
                   & " times" severity failure;
        assert edges = 10
            report "clock stopped: " & natural'image(edges) & " rising edges"
            severity failure;
        std.env.finish;
    end process;

end architecture;
