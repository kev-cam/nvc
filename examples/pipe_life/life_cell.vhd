-- life_cell.vhd
-- Conway's Game of Life cell — pipe-compatible, no STD_MX extensions
--
-- Bare reads and writes: no 'driver/'other needed.
-- Writes go into the front of the pipe FIFO, reads come off the back.
-- The entity code is identical whether ports are connected to pipes
-- or signals — only the top-level declaration keyword differs.
--
-- Compare with C++:
--
--   module cell {
--       pipe<int> in[8];
--       pipe<int> *out[8];
--       process p1 {
--         start:
--           for (;;) {
--               for (int i=ns=0; i < 8; i++) out[i]->write(alive);
--               int count = 0;
--               for (int i = 0; i < 8; i++) count += in[i].read();
--               alive = (count == 3) || (alive && count == 2);
--           }
--       } a;
--   };
--
-- Mapping:
--   pipe<int> in[8]       ->  in_n, in_ne, ... : in integer
--   pipe<int> *out[8]     ->  out_n, out_ne, ... : out integer
--   out[i]->write(alive)  ->  out_n <= nstate  (pipe: enqueue / signal: schedule)
--   in[i].read()          ->  in_n             (pipe: dequeue  / signal: read)

library ieee;
use ieee.std_logic_1164.all;

entity life_cell is
    generic (
        INIT_STATE : integer := 0;
        MAX_GEN    : integer := 10
    );
    port (
        -- Outputs: broadcast state to neighbors
        out_n  : out integer;
        out_ne : out integer;
        out_e  : out integer;
        out_se : out integer;
        out_s  : out integer;
        out_sw : out integer;
        out_w  : out integer;
        out_nw : out integer;
        -- Inputs: receive neighbor states
        in_n   : in  integer := 0;
        in_ne  : in  integer := 0;
        in_e   : in  integer := 0;
        in_se  : in  integer := 0;
        in_s   : in  integer := 0;
        in_sw  : in  integer := 0;
        in_w   : in  integer := 0;
        in_nw  : in  integer := 0
    );
end entity;

architecture behavioral of life_cell is
begin
    process
        variable nstate : integer := INIT_STATE;
        variable count  : integer;
    begin
        for gen in 0 to MAX_GEN - 1 loop
            -- Broadcast state to all neighbors
            -- (pipe: enqueues into each neighbor's FIFO)
            -- (signal: schedules update for next delta cycle)
            out_n  <= nstate;
            out_ne <= nstate;
            out_e  <= nstate;
            out_se <= nstate;
            out_s  <= nstate;
            out_sw <= nstate;
            out_w  <= nstate;
            out_nw <= nstate;

            -- Wait for all neighbors to have written.
            -- (pipe: reads below block until data arrives — this wait is optional)
            -- (signal: delta cycle propagates scheduled updates)
            wait for 0 ns;

            -- Count alive neighbors (> 0 means alive)
            -- (pipe: each read dequeues from neighbor's FIFO)
            -- (signal: reads current value)
            count := 0;
            if in_n  > 0 then count := count + 1; end if;
            if in_ne > 0 then count := count + 1; end if;
            if in_e  > 0 then count := count + 1; end if;
            if in_se > 0 then count := count + 1; end if;
            if in_s  > 0 then count := count + 1; end if;
            if in_sw > 0 then count := count + 1; end if;
            if in_w  > 0 then count := count + 1; end if;
            if in_nw > 0 then count := count + 1; end if;

            -- Conway's rules: positive = alive, negative/zero = dead
            if nstate > 0 then
                if count < 2 or count > 3 then
                    nstate := -count;
                else
                    nstate := count;
                end if;
            else
                if count = 3 then
                    nstate := count;
                else
                    nstate := -count;
                end if;
            end if;
        end loop;

        -- Final broadcast so display can show last generation
        out_n  <= nstate;
        out_ne <= nstate;
        out_e  <= nstate;
        out_se <= nstate;
        out_s  <= nstate;
        out_sw <= nstate;
        out_w  <= nstate;
        out_nw <= nstate;

        wait;
    end process;
end architecture;
