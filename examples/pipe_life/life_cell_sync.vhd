-- life_cell_sync.vhd
-- Conway's Game of Life cell — standard synchronous VHDL
--
-- Direct translation of the C++ ParC cell:
--
--   module cell {
--       signal<int> *neigbor[8], state;
--       int ns;
--       c_signal<bool> *clk;
--       process p1 {
--         start:
--           for (;;) {
--               @(clk);
--               for (int i=ns=0; i < 8; i++)
--                   if (neigbor[i] && neigbor[i]->Value() > 0) ns++;
--               if (state.Value() > 0) {
--                   if (ns < 2 || ns > 3) ns = -ns;
--               } else {
--                   if (3 != ns) ns = -ns;
--               }
--               state @= ns;
--           }
--       } a;
--   };
--
-- Mapping:
--   signal<int>          ->  signal s : integer
--   @(clk)               ->  rising_edge(clk)
--   state @= ns          ->  s <= ns     (non-blocking signal assignment)
--   neigbor[i]->Value()  ->  nb_* input port
--   NULL pointer check   ->  port default := 0 + open in port map

library ieee;
use ieee.std_logic_1164.all;

entity life_cell_sync is
    generic (
        INIT_STATE : integer := 0
    );
    port (
        clk  : in  std_logic;
        nb_n : in  integer := 0;
        nb_ne: in  integer := 0;
        nb_e : in  integer := 0;
        nb_se: in  integer := 0;
        nb_s : in  integer := 0;
        nb_sw: in  integer := 0;
        nb_w : in  integer := 0;
        nb_nw: in  integer := 0;
        state: out integer
    );
end entity;

architecture behavioral of life_cell_sync is
    signal s : integer := INIT_STATE;
begin
    state <= s;

    process (clk)
        variable ns : integer;
    begin
        if rising_edge(clk) then
            -- Count alive neighbors (> 0 means alive)
            ns := 0;
            if nb_n  > 0 then ns := ns + 1; end if;
            if nb_ne > 0 then ns := ns + 1; end if;
            if nb_e  > 0 then ns := ns + 1; end if;
            if nb_se > 0 then ns := ns + 1; end if;
            if nb_s  > 0 then ns := ns + 1; end if;
            if nb_sw > 0 then ns := ns + 1; end if;
            if nb_w  > 0 then ns := ns + 1; end if;
            if nb_nw > 0 then ns := ns + 1; end if;

            -- Conway's rules: positive = alive, negative/zero = dead
            if s > 0 then
                if ns < 2 or ns > 3 then
                    ns := -ns;
                end if;
            else
                if ns /= 3 then
                    ns := -ns;
                end if;
            end if;

            s <= ns;
        end if;
    end process;
end architecture;
