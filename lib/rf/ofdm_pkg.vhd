-- OFDM Package
--
-- Behavioral 802.11 OFDM functions: FFT/IFFT, QAM mapping,
-- cyclic prefix, subcarrier mapping.
--
-- WiFi 802.11a/g parameters:
--   64-point FFT, 48 data + 4 pilot + 12 null subcarriers
--   20 MHz bandwidth, 312.5 kHz subcarrier spacing
--   16-sample cyclic prefix (800 ns guard interval)

library ieee;
use ieee.std_logic_1164.all;
use ieee.math_real.all;
use work.iq_pkg.all;

package ofdm_pkg is

    ---------------------------------------------------------------------------
    -- 802.11 OFDM constants
    ---------------------------------------------------------------------------
    constant OFDM_FFT_SIZE          : integer := 64;
    constant OFDM_DATA_CARRIERS     : integer := 48;
    constant OFDM_PILOT_CARRIERS    : integer := 4;
    constant OFDM_CP_LENGTH         : integer := 16;
    constant OFDM_BW                : real := 20.0e6;
    constant OFDM_SUBCARRIER_SPACING: real := 312.5e3;

    -- Pilot subcarrier indices (802.11a: -21, -7, 7, 21 -> mapped to 0-63)
    constant PILOT_IDX_0 : integer := 43;  -- subcarrier -21
    constant PILOT_IDX_1 : integer := 57;  -- subcarrier -7
    constant PILOT_IDX_2 : integer := 7;   -- subcarrier  7
    constant PILOT_IDX_3 : integer := 21;  -- subcarrier  21

    ---------------------------------------------------------------------------
    -- FFT / IFFT (behavioral, radix-2 Cooley-Tukey)
    ---------------------------------------------------------------------------

    -- Forward FFT: time domain -> frequency domain
    function fft(x : iq_array) return iq_array;

    -- Inverse FFT: frequency domain -> time domain
    function ifft(x : iq_array) return iq_array;

    ---------------------------------------------------------------------------
    -- QAM constellation mapping
    ---------------------------------------------------------------------------

    -- Map bits to QAM constellation point
    -- order: 2=BPSK, 4=QPSK, 16=16-QAM, 64=64-QAM
    function qam_map(bits : std_logic_vector; order : integer) return iq_sample;

    -- Hard-decision QAM demapping (per-order functions)
    function qam_demap_bpsk(s : iq_sample) return std_logic_vector;
    function qam_demap_qpsk(s : iq_sample) return std_logic_vector;
    function qam_demap_16qam(s : iq_sample) return std_logic_vector;

    -- Bits per symbol for given QAM order
    function qam_bits(order : integer) return integer;

    ---------------------------------------------------------------------------
    -- Cyclic prefix
    ---------------------------------------------------------------------------
    function add_cyclic_prefix(sym : iq_array; cp_len : integer) return iq_array;
    function remove_cyclic_prefix(sym : iq_array; cp_len : integer; fft_size : integer) return iq_array;

    ---------------------------------------------------------------------------
    -- OFDM symbol modulation / demodulation
    ---------------------------------------------------------------------------

    -- Modulate: 48 data symbols -> 64-pt IFFT -> 80-sample OFDM symbol
    function ofdm_modulate(data_syms : iq_array) return iq_array;

    -- Demodulate: 80-sample OFDM symbol -> CP removal -> FFT -> 48 data symbols
    function ofdm_demodulate(samples : iq_array) return iq_array;

end package;

package body ofdm_pkg is

    ---------------------------------------------------------------------------
    -- Bit-reversal permutation for FFT
    ---------------------------------------------------------------------------
    function bit_reverse(val : integer; bits : integer) return integer is
        variable result : integer := 0;
        variable v : integer := val;
    begin
        for i in 0 to bits - 1 loop
            result := result * 2 + (v mod 2);
            v := v / 2;
        end loop;
        return result;
    end function;

    -- Log2 for power-of-2 integers
    function log2_int(n : integer) return integer is
        variable v : integer := n;
        variable r : integer := 0;
    begin
        while v > 1 loop
            v := v / 2;
            r := r + 1;
        end loop;
        return r;
    end function;

    ---------------------------------------------------------------------------
    -- FFT: radix-2 decimation-in-time Cooley-Tukey
    ---------------------------------------------------------------------------
    function fft(x : iq_array) return iq_array is
        constant n : integer := x'length;
        constant log_n : integer := log2_int(n);
        variable a : iq_array(0 to n - 1);
        variable t, u, w : iq_sample;
        variable m, half_m, j, k : integer;
        variable angle : real;
    begin
        -- Bit-reversal reorder
        for i in 0 to n - 1 loop
            a(bit_reverse(i, log_n)) := x(x'low + i);
        end loop;

        -- Butterfly stages
        m := 2;
        while m <= n loop
            half_m := m / 2;
            for s in 0 to half_m - 1 loop
                angle := -2.0 * MATH_PI * real(s) / real(m);
                w := make_iq(1.0, angle);
                j := s;
                while j < n loop
                    k := j + half_m;
                    t := iq_mul(w, a(k));
                    u := a(j);
                    a(j) := iq_add(u, t);
                    a(k) := iq_sub(u, t);
                    j := j + m;
                end loop;
            end loop;
            m := m * 2;
        end loop;

        return a;
    end function;

    ---------------------------------------------------------------------------
    -- IFFT: conjugate-FFT-conjugate with 1/N scaling
    ---------------------------------------------------------------------------
    function ifft(x : iq_array) return iq_array is
        constant n : integer := x'length;
        variable conj_in : iq_array(0 to n - 1);
        variable fft_out : iq_array(0 to n - 1);
        variable result  : iq_array(0 to n - 1);
        variable scale   : real;
    begin
        scale := 1.0 / real(n);

        -- Conjugate input
        for i in 0 to n - 1 loop
            conj_in(i) := iq_conjugate(x(x'low + i));
        end loop;

        -- Forward FFT
        fft_out := fft(conj_in);

        -- Conjugate and scale
        for i in 0 to n - 1 loop
            result(i) := iq_scale(iq_conjugate(fft_out(i)), scale);
        end loop;

        return result;
    end function;

    ---------------------------------------------------------------------------
    -- QAM bits per symbol
    ---------------------------------------------------------------------------
    function qam_bits(order : integer) return integer is
    begin
        case order is
            when 2      => return 1;   -- BPSK
            when 4      => return 2;   -- QPSK
            when 16     => return 4;   -- 16-QAM
            when 64     => return 6;   -- 64-QAM
            when others => return 1;
        end case;
    end function;

    ---------------------------------------------------------------------------
    -- QAM mapping
    ---------------------------------------------------------------------------
    function qam_map(bits : std_logic_vector; order : integer) return iq_sample is
        variable result : iq_sample := IQ_ZERO;
        variable norm   : real;
        variable i_val, q_val : integer;
        variable nbits  : integer;
    begin
        case order is
            when 2 =>
                -- BPSK: 0 -> +1, 1 -> -1
                if bits(bits'low) = '1' then
                    result := make_iq_rect(-1.0, 0.0);
                else
                    result := make_iq_rect(1.0, 0.0);
                end if;

            when 4 =>
                -- QPSK: Gray-coded, normalized to unit power
                norm := 1.0 / sqrt(2.0);
                if bits(bits'low) = '0' then
                    result.i := norm;
                else
                    result.i := -norm;
                end if;
                if bits(bits'low + 1) = '0' then
                    result.q := norm;
                else
                    result.q := -norm;
                end if;

            when 16 =>
                -- 16-QAM: 2 bits I, 2 bits Q, values {-3,-1,+1,+3}
                norm := 1.0 / sqrt(10.0);
                -- I mapping
                if bits(bits'low) = '0' then
                    if bits(bits'low + 1) = '0' then
                        i_val := 1;
                    else
                        i_val := 3;
                    end if;
                else
                    if bits(bits'low + 1) = '0' then
                        i_val := -1;
                    else
                        i_val := -3;
                    end if;
                end if;
                -- Q mapping
                if bits(bits'low + 2) = '0' then
                    if bits(bits'low + 3) = '0' then
                        q_val := 1;
                    else
                        q_val := 3;
                    end if;
                else
                    if bits(bits'low + 3) = '0' then
                        q_val := -1;
                    else
                        q_val := -3;
                    end if;
                end if;
                result := make_iq_rect(real(i_val) * norm, real(q_val) * norm);

            when others =>
                -- Default BPSK
                if bits(bits'low) = '1' then
                    result := make_iq_rect(-1.0, 0.0);
                else
                    result := make_iq_rect(1.0, 0.0);
                end if;
        end case;

        return result;
    end function;

    ---------------------------------------------------------------------------
    -- QAM hard-decision demapping
    ---------------------------------------------------------------------------
    function qam_demap_bpsk(s : iq_sample) return std_logic_vector is
        variable result : std_logic_vector(0 downto 0);
    begin
        if s.i >= 0.0 then
            result(0) := '0';
        else
            result(0) := '1';
        end if;
        return result;
    end function;

    function qam_demap_qpsk(s : iq_sample) return std_logic_vector is
        variable result : std_logic_vector(1 downto 0);
    begin
        if s.i >= 0.0 then result(0) := '0';
        else result(0) := '1'; end if;
        if s.q >= 0.0 then result(1) := '0';
        else result(1) := '1'; end if;
        return result;
    end function;

    function qam_demap_16qam(s : iq_sample) return std_logic_vector is
        variable result : std_logic_vector(3 downto 0);
        variable norm : real := 1.0 / sqrt(10.0);
        variable i_scaled, q_scaled : real;
    begin
        i_scaled := s.i / norm;
        q_scaled := s.q / norm;
        if i_scaled >= 0.0 then result(0) := '0';
        else result(0) := '1'; end if;
        if abs(i_scaled) >= 2.0 then result(1) := '1';
        else result(1) := '0'; end if;
        if q_scaled >= 0.0 then result(2) := '0';
        else result(2) := '1'; end if;
        if abs(q_scaled) >= 2.0 then result(3) := '1';
        else result(3) := '0'; end if;
        return result;
    end function;

    ---------------------------------------------------------------------------
    -- Cyclic prefix
    ---------------------------------------------------------------------------
    function add_cyclic_prefix(sym : iq_array; cp_len : integer) return iq_array is
        constant n : integer := sym'length;
        variable result : iq_array(0 to n + cp_len - 1);
    begin
        -- CP = last cp_len samples of the symbol
        for i in 0 to cp_len - 1 loop
            result(i) := sym(sym'low + n - cp_len + i);
        end loop;
        -- Symbol data
        for i in 0 to n - 1 loop
            result(cp_len + i) := sym(sym'low + i);
        end loop;
        return result;
    end function;

    function remove_cyclic_prefix(sym : iq_array; cp_len : integer; fft_size : integer) return iq_array is
        variable result : iq_array(0 to fft_size - 1);
    begin
        for i in 0 to fft_size - 1 loop
            result(i) := sym(sym'low + cp_len + i);
        end loop;
        return result;
    end function;

    ---------------------------------------------------------------------------
    -- OFDM subcarrier mapping (802.11a)
    --
    -- 64 subcarriers indexed 0..63:
    --   0:     DC (null)
    --   1-6:   data (subcarriers +1 to +6)
    --   7:     pilot (+7)
    --   8-20:  data (+8 to +20)
    --   21:    pilot (+21)
    --   22-26: data (+22 to +26)
    --   27-37: null (guard band)
    --   38-42: data (-26 to -22)
    --   43:    pilot (-21)
    --   44-56: data (-20 to -8)
    --   57:    pilot (-7)
    --   58-63: data (-6 to -1)
    ---------------------------------------------------------------------------

    function is_data_subcarrier(idx : integer) return boolean is
    begin
        -- Null subcarriers: 0, 27-37
        -- Pilot subcarriers: 7, 21, 43, 57
        if idx = 0 then return false; end if;
        if idx >= 27 and idx <= 37 then return false; end if;
        if idx = 7 or idx = 21 or idx = 43 or idx = 57 then return false; end if;
        return true;
    end function;

    function ofdm_modulate(data_syms : iq_array) return iq_array is
        variable freq_domain : iq_array(0 to OFDM_FFT_SIZE - 1) :=
            (others => IQ_ZERO);
        variable time_domain : iq_array(0 to OFDM_FFT_SIZE - 1);
        variable d_idx : integer := 0;
    begin
        -- Map data symbols to subcarriers
        for i in 0 to OFDM_FFT_SIZE - 1 loop
            if is_data_subcarrier(i) then
                if d_idx <= data_syms'high - data_syms'low then
                    freq_domain(i) := data_syms(data_syms'low + d_idx);
                end if;
                d_idx := d_idx + 1;
            end if;
        end loop;

        -- Insert pilots (all +1 for simplicity)
        freq_domain(PILOT_IDX_0) := make_iq_rect(1.0, 0.0);
        freq_domain(PILOT_IDX_1) := make_iq_rect(1.0, 0.0);
        freq_domain(PILOT_IDX_2) := make_iq_rect(1.0, 0.0);
        freq_domain(PILOT_IDX_3) := make_iq_rect(1.0, 0.0);

        -- IFFT
        time_domain := ifft(freq_domain);

        -- Add cyclic prefix
        return add_cyclic_prefix(time_domain, OFDM_CP_LENGTH);
    end function;

    function ofdm_demodulate(samples : iq_array) return iq_array is
        variable freq_domain : iq_array(0 to OFDM_FFT_SIZE - 1);
        variable data_syms   : iq_array(0 to OFDM_DATA_CARRIERS - 1) :=
            (others => IQ_ZERO);
        variable time_domain : iq_array(0 to OFDM_FFT_SIZE - 1);
        variable d_idx : integer := 0;
    begin
        -- Remove cyclic prefix
        time_domain := remove_cyclic_prefix(samples, OFDM_CP_LENGTH, OFDM_FFT_SIZE);

        -- FFT
        freq_domain := fft(time_domain);

        -- Extract data subcarriers
        for i in 0 to OFDM_FFT_SIZE - 1 loop
            if is_data_subcarrier(i) then
                if d_idx <= data_syms'high then
                    data_syms(d_idx) := freq_domain(i);
                end if;
                d_idx := d_idx + 1;
            end if;
        end loop;

        return data_syms;
    end function;

end package body;
