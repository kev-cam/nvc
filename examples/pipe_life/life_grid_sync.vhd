-- life_grid_sync.vhd
-- Game of Life grid — standard synchronous VHDL
--
-- Direct translation of:
--
--   template <int N,int M>
--   module top {
--       c_signal<bool> clk;
--       cell.arr[N][M](clk=&clk);
--       void post_bind() { /* wire neighbors, random init */ }
--       void draw() { /* VT100 output */ }
--       process sync { for(;;) { draw(); clk = !clk; wait(1); } } c;
--   };
--
-- Key mappings:
--   c_signal<bool> clk     ->  signal clk : std_logic
--   cell.arr[N][M]         ->  generate grid of life_cell_sync
--   memset(neigbor,0,...)  ->  padded state grid (border = 0, no NULL checks)
--   clk = !clk; wait(1)   ->  clk <= '1'; wait for 500 ns; clk <= '0'; ...
--   draw()                 ->  VT100 grid output via std.textio

library ieee;
use ieee.std_logic_1164.all;
use std.textio.all;

entity life_grid_sync is
    generic (
        ROWS    : integer := 30;
        COLS    : integer := 60;
        MAX_GEN : integer := 30
    );
end entity;

architecture sim of life_grid_sync is

    -- Call libc sleep() directly via VHPIDIRECT
    function c_sleep(seconds : integer) return integer;
    attribute foreign of c_sleep : function is "VHPIDIRECT sleep";
    function c_sleep(seconds : integer) return integer is
    begin return 0; end function;

    -- Glider pattern at (1,1)
    function init_state(r, c : integer) return integer is
    begin
        if (r = 1 and c = 2) or
           (r = 2 and c = 3) or
           (r = 3 and c = 1) or
           (r = 3 and c = 2) or
           (r = 3 and c = 3) then
            return 1;
        end if;
        return 0;
    end function;

    -- State grid with 1-cell border of zeros.
    -- Edge cells read from the border (always 0 = dead),
    -- replacing the C++ NULL pointer checks.
    type state_grid is array (integer range <>, integer range <>) of integer;
    signal states : state_grid(-1 to ROWS, -1 to COLS)
        := (others => (others => 0));

    signal clk : std_logic := '0';

begin

    -- Cell array: cell.arr[N][M](clk=&clk)
    gen_r: for r in 0 to ROWS - 1 generate
        gen_c: for c in 0 to COLS - 1 generate
        begin
            cell: entity work.life_cell_sync
                generic map (INIT_STATE => init_state(r, c))
                port map (
                    clk   => clk,
                    nb_n  => states(r - 1, c),
                    nb_ne => states(r - 1, c + 1),
                    nb_e  => states(r,     c + 1),
                    nb_se => states(r + 1, c + 1),
                    nb_s  => states(r + 1, c),
                    nb_sw => states(r + 1, c - 1),
                    nb_w  => states(r,     c - 1),
                    nb_nw => states(r - 1, c - 1),
                    state => states(r, c)
                );
        end generate;
    end generate;

    -- Clock + display: process sync { draw(); clk = !clk; wait(1); }
    p_sync: process
        variable L : line;
        variable s : integer;
        variable dummy : integer;

        procedure draw(gen : integer) is
        begin
            -- Cursor home
            write(L, character'val(27) & "[H");
            writeline(output, L);
            -- Grid
            for r in 0 to ROWS - 1 loop
                for c in 0 to COLS - 1 loop
                    s := states(r, c);
                    if s > 0 then
                        write(L, character'val(48 + s));
                    else
                        write(L, '.');
                    end if;
                end loop;
                writeline(output, L);
            end loop;
            -- Status line
            write(L, string'("gen "));
            write(L, gen);
            write(L, string'("   "));
            writeline(output, L);
        end procedure;

    begin
        wait for 0 ns;  -- let initial state propagate
        for gen in 0 to MAX_GEN - 1 loop
            draw(gen);
            dummy := c_sleep(1);
            clk <= '1'; wait for 500 ns;
            clk <= '0'; wait for 500 ns;
        end loop;
        draw(MAX_GEN);
        dummy := c_sleep(1);
        report "DONE";
        wait;
    end process;

end architecture;
