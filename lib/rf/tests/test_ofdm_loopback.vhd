-- test_ofdm_loopback.vhd
-- OFDM modulate -> ideal channel (passthrough) -> demodulate
--
-- Tests the OFDM pure functions: IFFT/FFT, subcarrier mapping, cyclic prefix.
-- Generates QPSK symbols on 48 data subcarriers, modulates to an OFDM symbol,
-- then demodulates and verifies recovered symbols match input.
--
-- No arena resolution needed -- this is a pure function test using a process.

library ieee;
use ieee.std_logic_1164.all;
use ieee.math_real.all;
library rf;
use rf.iq_pkg.all;
use rf.ofdm_pkg.all;

entity test_ofdm_loopback is end entity;

architecture test of test_ofdm_loopback is
begin
    p_test: process
        variable norm      : real;
        variable tx_syms   : iq_array(0 to OFDM_DATA_CARRIERS - 1);
        variable tx_ofdm   : iq_array(0 to OFDM_FFT_SIZE + OFDM_CP_LENGTH - 1);
        variable rx_syms   : iq_array(0 to OFDM_DATA_CARRIERS - 1);
        variable err_i     : real;
        variable err_q     : real;
        variable max_err   : real;
        variable bits_tx   : std_logic_vector(1 downto 0);
        variable bits_rx   : std_logic_vector(1 downto 0);
        variable bit_errors: integer;
    begin
        norm := 1.0 / sqrt(2.0);

        -- Generate QPSK symbols using qam_map for consistency with demap
        for i in 0 to OFDM_DATA_CARRIERS - 1 loop
            case i mod 4 is
                when 0 =>     bits_tx := "00";
                when 1 =>     bits_tx := "01";
                when 2 =>     bits_tx := "10";
                when others => bits_tx := "11";
            end case;
            tx_syms(i) := qam_map(bits_tx, 4);
        end loop;

        -- OFDM modulate: subcarrier map + IFFT + cyclic prefix
        tx_ofdm := ofdm_modulate(tx_syms);

        -- Ideal channel: passthrough (no noise, no distortion)
        -- rx = tx

        -- OFDM demodulate: CP removal + FFT + subcarrier demap
        rx_syms := ofdm_demodulate(tx_ofdm);

        -- Check: compare TX and RX symbols
        max_err := 0.0;
        for i in 0 to OFDM_DATA_CARRIERS - 1 loop
            err_i := abs(rx_syms(i).i - tx_syms(i).i);
            err_q := abs(rx_syms(i).q - tx_syms(i).q);
            if err_i > max_err then max_err := err_i; end if;
            if err_q > max_err then max_err := err_q; end if;
        end loop;

        report "=== OFDM Loopback Test ===" severity note;
        report "Max IQ error: " & real'image(max_err) severity note;

        assert max_err < 1.0e-10
            report "FAIL: OFDM roundtrip error " & real'image(max_err) &
                   " exceeds tolerance"
            severity error;

        -- Also verify QAM demap produces correct bits
        bit_errors := 0;
        for i in 0 to OFDM_DATA_CARRIERS - 1 loop
            case i mod 4 is
                when 0 =>     bits_tx := "00";
                when 1 =>     bits_tx := "01";
                when 2 =>     bits_tx := "10";
                when others => bits_tx := "11";
            end case;
            bits_rx := qam_demap_qpsk(rx_syms(i));
            if bits_rx /= bits_tx then
                bit_errors := bit_errors + 1;
            end if;
        end loop;

        report "Bit errors: " & integer'image(bit_errors) &
               " / " & integer'image(OFDM_DATA_CARRIERS) severity note;

        assert bit_errors = 0
            report "FAIL: " & integer'image(bit_errors) & " bit errors"
            severity error;

        report "DONE" severity note;
        wait;
    end process;
end architecture;
