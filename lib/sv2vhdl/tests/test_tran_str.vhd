-- Translation of iverilog ivltests/tran.v
-- Tests bidirectional tran gate structural connectivity
--
-- Original Verilog structure:
--   Chain: a -> assign(supply) -> a1 -tran-> a2 -tran-> ... -tran-> a7
--   5x5 Grid: asymmetric-strength drivers on both sides of tran
--     Rows: str_one  = supply, strong, pull, weak, highz
--     Cols: str_zero = supply, strong, pull, weak, highz
--
-- Strength annotations on the assigns are metadata for the resolver
-- network; in this VHDL translation they are just concurrent assignments.
-- The tran entities handle bidirectional propagation internally.

library ieee;
use ieee.std_logic_1164.all;

entity test_tran_str is
end entity test_tran_str;

architecture test of test_tran_str is

    -- Control signals (like Verilog regs)
    signal a, b : std_logic := 'U';

    -- Chain signals: a1 through a7
    signal ac : std_logic_vector(1 to 7);

    -- Grid signals: 5x5 a-side and b-side, linearized [row*5+col]
    signal ag, bg : std_logic_vector(1 to 25);

    function sl_to_char(s : std_logic) return character is
    begin
        case s is
            when '0' => return '0';
            when '1' => return '1';
            when 'Z' => return 'Z';
            when 'X' => return 'X';
            when 'U' => return 'U';
            when 'W' => return 'W';
            when 'L' => return 'L';
            when 'H' => return 'H';
            when '-' => return '-';
        end case;
    end function;

begin

    ---------------------------------------------------------------------------
    -- Chain: assign (supply1, supply0) a1 = a;
    -- then tran t1(a1,a2); tran t2(a2,a3); ... tran t6(a6,a7);
    ---------------------------------------------------------------------------
    ac(1) <= a;

    gen_chain: for i in 1 to 6 generate
        tc: entity work.sv_tran(strength)
            port map(a => ac(i), b => ac(i+1));
    end generate gen_chain;

    ---------------------------------------------------------------------------
    -- 5x5 Grid: assign (row_str1, col_str0) aij = a, bij = b;
    --           tran tij(aij, bij);
    -- Strength annotations are metadata; drivers are simple assignments.
    -- Cell (5,5) has no driver (both sides undriven = highz).
    ---------------------------------------------------------------------------
    gen_row: for i in 1 to 5 generate
        gen_col: for j in 1 to 5 generate

            -- Drivers: present for all cells except (5,5)
            gen_drv: if i /= 5 or j /= 5 generate
                ag((i-1)*5+j) <= a;
                bg((i-1)*5+j) <= b;
            end generate gen_drv;

            -- Tran gate: always present
            t_grid: entity work.sv_tran(strength)
                port map(a => ag((i-1)*5+j), b => bg((i-1)*5+j));

        end generate gen_col;
    end generate gen_row;

    ---------------------------------------------------------------------------
    -- Stimulus: exercise all 16 combinations of a,b in {Z,X,0,1}
    ---------------------------------------------------------------------------
    process
        type stim_array is array(natural range <>) of std_logic;
        constant stimuli : stim_array(0 to 3) := ('Z', 'X', '0', '1');
    begin
        for ia in stimuli'range loop
            for ib in stimuli'range loop
                a <= stimuli(ia);
                b <= stimuli(ib);
                wait for 1 ns;

                report "a=" & sl_to_char(stimuli(ia)) &
                       " b=" & sl_to_char(stimuli(ib));

                -- Chain values
                report "  chain: " &
                    sl_to_char(ac(1)) & " " & sl_to_char(ac(2)) & " " &
                    sl_to_char(ac(3)) & " " & sl_to_char(ac(4)) & " " &
                    sl_to_char(ac(5)) & " " & sl_to_char(ac(6)) & " " &
                    sl_to_char(ac(7));

                -- Grid values (a-side b-side per cell)
                for i in 1 to 5 loop
                    report "  row " & integer'image(i) & ": " &
                        sl_to_char(ag((i-1)*5+1)) & sl_to_char(bg((i-1)*5+1)) & " " &
                        sl_to_char(ag((i-1)*5+2)) & sl_to_char(bg((i-1)*5+2)) & " " &
                        sl_to_char(ag((i-1)*5+3)) & sl_to_char(bg((i-1)*5+3)) & " " &
                        sl_to_char(ag((i-1)*5+4)) & sl_to_char(bg((i-1)*5+4)) & " " &
                        sl_to_char(ag((i-1)*5+5)) & sl_to_char(bg((i-1)*5+5));
                end loop;
            end loop;
        end loop;

        report "DONE";
        wait;
    end process;

end architecture test;
