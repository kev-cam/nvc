-- life_top.vhd
-- Conway's Game of Life grid using pipes
--
-- Pipes are unidirectional FIFOs: writes enqueue, reads dequeue.
-- Each cell pair uses two pipes (one per direction), matching the
-- C++ version where each direction has a separate pipe<int>.
--
-- The ONLY difference from a signal-based version is the keyword
-- 'pipe' instead of 'signal' in the connection declarations.
-- Change 'pipe' to 'signal' and the design still compiles —
-- just with last-value semantics instead of FIFO semantics.
--
-- Compare with C++:
--
--   template <int N,int M>
--   module top {
--       pipe<int> grid[N][M][8];         // one pipe per cell per direction
--       cell cells[N][M];
--       // ... wire each cell's in[d] to neighbor's out[opposite(d)] ...
--   };
--
-- In VHDL we use 8 named pipe arrays (one per direction) instead of
-- a 3D array.  Each pipe array carries state FROM the indexed cell
-- TO its neighbor in that direction.

library ieee;
use ieee.std_logic_1164.all;

entity life_top is
    generic (
        ROWS    : integer := 30;
        COLS    : integer := 60;
        MAX_GEN : integer := 30
    );
end entity;

architecture grid of life_top is

    -- Glider at (1,1):
    --   . 1 .      row 1: col 2
    --   . . 1      row 2: col 3
    --   1 1 1      row 3: cols 1,2,3
    function glider_init(r, c : integer) return std_logic is
    begin
        if (r = 1 and c = 2) or
           (r = 2 and c = 3) or
           (r = 3 and c = 1) or
           (r = 3 and c = 2) or
           (r = 3 and c = 3) then
            return '1';
        end if;
        return '0';
    end function;

    -- One pipe array per direction.
    -- p_north(r,c) carries cell(r,c)'s state northward to cell(r-1,c).
    type pipe_grid is array (0 to ROWS - 1, 0 to COLS - 1) of std_logic;

    ---------------------------------------------------------------------------
    -- Pipe declarations — change 'pipe' to 'signal' for synchronous mode.
    ---------------------------------------------------------------------------
    pipe p_n  : pipe_grid;   -- northward  (r,c) -> (r-1, c)
    pipe p_ne : pipe_grid;   -- NE         (r,c) -> (r-1, c+1)
    pipe p_e  : pipe_grid;   -- eastward   (r,c) -> (r,   c+1)
    pipe p_se : pipe_grid;   -- SE         (r,c) -> (r+1, c+1)
    pipe p_s  : pipe_grid;   -- southward  (r,c) -> (r+1, c)
    pipe p_sw : pipe_grid;   -- SW         (r,c) -> (r+1, c-1)
    pipe p_w  : pipe_grid;   -- westward   (r,c) -> (r,   c-1)
    pipe p_nw : pipe_grid;   -- NW         (r,c) -> (r-1, c-1)

begin

    gen_r: for r in 0 to ROWS - 1 generate
        gen_c: for c in 0 to COLS - 1 generate

            -- Wrapped neighbor indices (toroidal grid)
            constant rn : integer := (r - 1 + ROWS) mod ROWS;
            constant rs : integer := (r + 1)         mod ROWS;
            constant cw : integer := (c - 1 + COLS) mod COLS;
            constant ce : integer := (c + 1)         mod COLS;

        begin

            cell_inst: entity work.life_cell
                generic map (
                    INIT_ALIVE => glider_init(r, c),
                    MAX_GEN    => MAX_GEN
                )
                port map (
                    -- Outputs: cell (r,c) writes its state into these pipes
                    out_n  => p_n (r, c),
                    out_ne => p_ne(r, c),
                    out_e  => p_e (r, c),
                    out_se => p_se(r, c),
                    out_s  => p_s (r, c),
                    out_sw => p_sw(r, c),
                    out_w  => p_w (r, c),
                    out_nw => p_nw(r, c),

                    -- Inputs: cell (r,c) reads its neighbor's state
                    -- from the pipe the neighbor wrote into
                    in_n  => p_s (rn, c),    -- north neighbor wrote southward
                    in_ne => p_sw(rn, ce),   -- NE neighbor wrote southwestward
                    in_e  => p_w (r,  ce),   -- east neighbor wrote westward
                    in_se => p_nw(rs, ce),   -- SE neighbor wrote northwestward
                    in_s  => p_n (rs, c),    -- south neighbor wrote northward
                    in_sw => p_ne(rs, cw),   -- SW neighbor wrote northeastward
                    in_w  => p_e (r,  cw),   -- west neighbor wrote eastward
                    in_nw => p_se(rn, cw)    -- NW neighbor wrote southeastward
                );

        end generate;
    end generate;

end architecture;
