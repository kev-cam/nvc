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
--   pipe<int> in[8]       ->  in_n, in_ne, ... : in std_logic
--   pipe<int> *out[8]     ->  out_n, out_ne, ... : out std_logic
--   out[i]->write(alive)  ->  out_n <= alive   (pipe: enqueue / signal: schedule)
--   in[i].read()          ->  in_n             (pipe: dequeue  / signal: read)

library ieee;
use ieee.std_logic_1164.all;

entity life_cell is
    generic (
        INIT_ALIVE : std_logic := '0';
        MAX_GEN    : integer := 10
    );
    port (
        -- Outputs: broadcast state to neighbors
        out_n  : out std_logic;
        out_ne : out std_logic;
        out_e  : out std_logic;
        out_se : out std_logic;
        out_s  : out std_logic;
        out_sw : out std_logic;
        out_w  : out std_logic;
        out_nw : out std_logic;
        -- Inputs: receive neighbor states
        in_n   : in  std_logic := '0';
        in_ne  : in  std_logic := '0';
        in_e   : in  std_logic := '0';
        in_se  : in  std_logic := '0';
        in_s   : in  std_logic := '0';
        in_sw  : in  std_logic := '0';
        in_w   : in  std_logic := '0';
        in_nw  : in  std_logic := '0'
    );
end entity;

architecture behavioral of life_cell is
begin
    process
        variable alive : std_logic := INIT_ALIVE;
        variable count : integer;
    begin
        for gen in 0 to MAX_GEN - 1 loop
            -- Broadcast state to all neighbors
            -- (pipe: enqueues into each neighbor's FIFO)
            -- (signal: schedules update for next delta cycle)
            out_n  <= alive;
            out_ne <= alive;
            out_e  <= alive;
            out_se <= alive;
            out_s  <= alive;
            out_sw <= alive;
            out_w  <= alive;
            out_nw <= alive;

            -- Wait for all neighbors to have written.
            -- (pipe: reads below block until data arrives — this wait is optional)
            -- (signal: delta cycle propagates scheduled updates)
            wait for 0 ns;

            -- Count alive neighbors
            -- (pipe: each read dequeues from neighbor's FIFO)
            -- (signal: reads current value)
            count := 0;
            if in_n  = '1' then count := count + 1; end if;
            if in_ne = '1' then count := count + 1; end if;
            if in_e  = '1' then count := count + 1; end if;
            if in_se = '1' then count := count + 1; end if;
            if in_s  = '1' then count := count + 1; end if;
            if in_sw = '1' then count := count + 1; end if;
            if in_w  = '1' then count := count + 1; end if;
            if in_nw = '1' then count := count + 1; end if;

            -- Conway's rules
            if count = 3 or (count = 2 and alive = '1') then
                alive := '1';
            else
                alive := '0';
            end if;
        end loop;

        wait;
    end process;
end architecture;
