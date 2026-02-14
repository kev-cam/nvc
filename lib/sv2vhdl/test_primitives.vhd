-- Comprehensive test bench for all 26 IEEE 1800-2017 sv_primitives
-- Tests truth tables, X propagation, strength handling, and weak inputs
--
-- Covers:
--   8 logic gates:    sv_and, sv_nand, sv_or, sv_nor, sv_xor, sv_xnor, sv_buf, sv_not
--   4 tristate gates: sv_bufif0, sv_bufif1, sv_notif0, sv_notif1
--   6 MOS switches:   sv_nmos, sv_pmos, sv_rnmos, sv_rpmos, sv_cmos, sv_rcmos
--   6 tran switches:  sv_tran, sv_tranif0, sv_tranif1, sv_rtran, sv_rtranif0, sv_rtranif1
--   2 pull gates:     sv_pullup, sv_pulldown

library ieee;
use ieee.std_logic_1164.all;

entity test_primitives is
end entity test_primitives;

architecture test of test_primitives is

    ---------------------------------------------------------------------------
    -- Logic gate signals (2-input, generic n=2)
    ---------------------------------------------------------------------------
    signal g_and_a, g_nand_a, g_or_a, g_nor_a,
           g_xor_a, g_xnor_a : std_logic_vector(0 to 1);
    signal g_and_y, g_nand_y, g_or_y, g_nor_y,
           g_xor_y, g_xnor_y : std_logic;

    -- buf/not: 1 input, n=1 output
    signal g_buf_a, g_not_a   : std_logic;
    signal g_buf_y, g_not_y   : std_logic_vector(0 to 0);

    ---------------------------------------------------------------------------
    -- Tristate gate signals
    ---------------------------------------------------------------------------
    signal ts_data, ts_ctrl : std_logic;
    signal ts_bufif0_y, ts_bufif1_y,
           ts_notif0_y, ts_notif1_y : std_logic;

    ---------------------------------------------------------------------------
    -- MOS switch signals
    ---------------------------------------------------------------------------
    signal m_data, m_gate : std_logic;
    signal m_nmos_y, m_pmos_y,
           m_rnmos_y, m_rpmos_y : std_logic;
    signal m_cmos_ngate, m_cmos_pgate : std_logic;
    signal m_cmos_y, m_rcmos_y : std_logic;

    ---------------------------------------------------------------------------
    -- Tran switch signals (standalone, no resolver needed)
    ---------------------------------------------------------------------------
    signal tr_ao, tr_bo  : std_logic;  -- _others inputs (driven by TB)
    signal tr_ctrl       : std_logic;

    -- Outputs from each tran variant
    signal tr_tran_ad, tr_tran_bd         : std_logic;
    signal tr_trif0_ad, tr_trif0_bd       : std_logic;
    signal tr_trif1_ad, tr_trif1_bd       : std_logic;
    signal tr_rtran_ad, tr_rtran_bd       : std_logic;
    signal tr_rtrif0_ad, tr_rtrif0_bd     : std_logic;
    signal tr_rtrif1_ad, tr_rtrif1_bd     : std_logic;

    ---------------------------------------------------------------------------
    -- Pull gate signals
    ---------------------------------------------------------------------------
    signal p_up_y, p_down_y : std_logic;

begin

    ---------------------------------------------------------------------------
    -- Logic gate instantiations
    ---------------------------------------------------------------------------
    u_and:  entity work.sv_and  generic map(n=>2) port map(y=>g_and_y,  a=>g_and_a);
    u_nand: entity work.sv_nand generic map(n=>2) port map(y=>g_nand_y, a=>g_nand_a);
    u_or:   entity work.sv_or   generic map(n=>2) port map(y=>g_or_y,   a=>g_or_a);
    u_nor:  entity work.sv_nor  generic map(n=>2) port map(y=>g_nor_y,  a=>g_nor_a);
    u_xor:  entity work.sv_xor  generic map(n=>2) port map(y=>g_xor_y,  a=>g_xor_a);
    u_xnor: entity work.sv_xnor generic map(n=>2) port map(y=>g_xnor_y, a=>g_xnor_a);
    u_buf:  entity work.sv_buf  generic map(n=>1) port map(y=>g_buf_y,  a=>g_buf_a);
    u_not:  entity work.sv_not  generic map(n=>1) port map(y=>g_not_y,  a=>g_not_a);

    ---------------------------------------------------------------------------
    -- Tristate gate instantiations (share data and ctrl signals)
    ---------------------------------------------------------------------------
    u_bufif0: entity work.sv_bufif0 port map(y=>ts_bufif0_y, data=>ts_data, ctrl=>ts_ctrl);
    u_bufif1: entity work.sv_bufif1 port map(y=>ts_bufif1_y, data=>ts_data, ctrl=>ts_ctrl);
    u_notif0: entity work.sv_notif0 port map(y=>ts_notif0_y, data=>ts_data, ctrl=>ts_ctrl);
    u_notif1: entity work.sv_notif1 port map(y=>ts_notif1_y, data=>ts_data, ctrl=>ts_ctrl);

    ---------------------------------------------------------------------------
    -- MOS switch instantiations (share data and gate signals)
    ---------------------------------------------------------------------------
    u_nmos:  entity work.sv_nmos  port map(y=>m_nmos_y,  data=>m_data, gate=>m_gate);
    u_pmos:  entity work.sv_pmos  port map(y=>m_pmos_y,  data=>m_data, gate=>m_gate);
    u_rnmos: entity work.sv_rnmos port map(y=>m_rnmos_y, data=>m_data, gate=>m_gate);
    u_rpmos: entity work.sv_rpmos port map(y=>m_rpmos_y, data=>m_data, gate=>m_gate);
    u_cmos:  entity work.sv_cmos  port map(y=>m_cmos_y,  data=>m_data, ngate=>m_cmos_ngate, pgate=>m_cmos_pgate);
    u_rcmos: entity work.sv_rcmos port map(y=>m_rcmos_y, data=>m_data, ngate=>m_cmos_ngate, pgate=>m_cmos_pgate);

    ---------------------------------------------------------------------------
    -- Tran switch instantiations (each gets its own outputs)
    ---------------------------------------------------------------------------
    u_tran:    entity work.sv_tran    port map(a_others=>tr_ao, a_driver=>tr_tran_ad,  b_others=>tr_bo, b_driver=>tr_tran_bd);
    u_tranif0: entity work.sv_tranif0 port map(a_others=>tr_ao, a_driver=>tr_trif0_ad, b_others=>tr_bo, b_driver=>tr_trif0_bd, ctrl=>tr_ctrl);
    u_tranif1: entity work.sv_tranif1 port map(a_others=>tr_ao, a_driver=>tr_trif1_ad, b_others=>tr_bo, b_driver=>tr_trif1_bd, ctrl=>tr_ctrl);
    u_rtran:   entity work.sv_rtran   port map(a_others=>tr_ao, a_driver=>tr_rtran_ad, b_others=>tr_bo, b_driver=>tr_rtran_bd);
    u_rtranif0:entity work.sv_rtranif0 port map(a_others=>tr_ao, a_driver=>tr_rtrif0_ad, b_others=>tr_bo, b_driver=>tr_rtrif0_bd, ctrl=>tr_ctrl);
    u_rtranif1:entity work.sv_rtranif1 port map(a_others=>tr_ao, a_driver=>tr_rtrif1_ad, b_others=>tr_bo, b_driver=>tr_rtrif1_bd, ctrl=>tr_ctrl);

    ---------------------------------------------------------------------------
    -- Pull gate instantiations
    ---------------------------------------------------------------------------
    u_pullup:   entity work.sv_pullup   port map(y => p_up_y);
    u_pulldown: entity work.sv_pulldown port map(y => p_down_y);

    ---------------------------------------------------------------------------
    -- Test process
    ---------------------------------------------------------------------------
    process
        variable pass_count : integer := 0;
        variable fail_count : integer := 0;

        function sl(s : std_logic) return string is
            type t is array (std_ulogic) of string(1 to 1);
            constant tbl : t := ("U","X","0","1","Z","W","L","H","-");
        begin return tbl(s); end;

        procedure check(actual, expected : std_logic; msg : string) is
        begin
            if actual = expected then
                pass_count := pass_count + 1;
            else
                fail_count := fail_count + 1;
                report "FAIL: " & msg & " -- expected " & sl(expected) & " got " & sl(actual)
                    severity error;
            end if;
        end;

    begin

        -----------------------------------------------------------------------
        -- PULL GATES (static, test immediately)
        -----------------------------------------------------------------------
        wait for 1 ns;
        report "=== Pull Gates ===";
        check(p_up_y,   'H', "pullup");
        check(p_down_y, 'L', "pulldown");

        -----------------------------------------------------------------------
        -- LOGIC GATES: Full truth table + X propagation
        -----------------------------------------------------------------------
        report "=== Logic Gates: AND ===";
        g_and_a <= "00"; wait for 1 ns; check(g_and_y, '0', "AND 00");
        g_and_a <= "01"; wait for 1 ns; check(g_and_y, '0', "AND 01");
        g_and_a <= "10"; wait for 1 ns; check(g_and_y, '0', "AND 10");
        g_and_a <= "11"; wait for 1 ns; check(g_and_y, '1', "AND 11");
        g_and_a <= "X1"; wait for 1 ns; check(g_and_y, 'X', "AND X1");
        g_and_a <= "X0"; wait for 1 ns; check(g_and_y, '0', "AND X0 (0 dominates)");
        g_and_a <= "L1"; wait for 1 ns; check(g_and_y, '0', "AND L1 (weak 0)");
        g_and_a <= "H1"; wait for 1 ns; check(g_and_y, '1', "AND H1 (weak 1)");

        report "=== Logic Gates: NAND ===";
        g_nand_a <= "00"; wait for 1 ns; check(g_nand_y, '1', "NAND 00");
        g_nand_a <= "01"; wait for 1 ns; check(g_nand_y, '1', "NAND 01");
        g_nand_a <= "10"; wait for 1 ns; check(g_nand_y, '1', "NAND 10");
        g_nand_a <= "11"; wait for 1 ns; check(g_nand_y, '0', "NAND 11");
        g_nand_a <= "X1"; wait for 1 ns; check(g_nand_y, 'X', "NAND X1");
        g_nand_a <= "X0"; wait for 1 ns; check(g_nand_y, '1', "NAND X0 (0 dominates)");

        report "=== Logic Gates: OR ===";
        g_or_a <= "00"; wait for 1 ns; check(g_or_y, '0', "OR 00");
        g_or_a <= "01"; wait for 1 ns; check(g_or_y, '1', "OR 01");
        g_or_a <= "10"; wait for 1 ns; check(g_or_y, '1', "OR 10");
        g_or_a <= "11"; wait for 1 ns; check(g_or_y, '1', "OR 11");
        g_or_a <= "X0"; wait for 1 ns; check(g_or_y, 'X', "OR X0");
        g_or_a <= "X1"; wait for 1 ns; check(g_or_y, '1', "OR X1 (1 dominates)");

        report "=== Logic Gates: NOR ===";
        g_nor_a <= "00"; wait for 1 ns; check(g_nor_y, '1', "NOR 00");
        g_nor_a <= "01"; wait for 1 ns; check(g_nor_y, '0', "NOR 01");
        g_nor_a <= "10"; wait for 1 ns; check(g_nor_y, '0', "NOR 10");
        g_nor_a <= "11"; wait for 1 ns; check(g_nor_y, '0', "NOR 11");
        g_nor_a <= "X0"; wait for 1 ns; check(g_nor_y, 'X', "NOR X0");
        g_nor_a <= "X1"; wait for 1 ns; check(g_nor_y, '0', "NOR X1 (1 dominates)");

        report "=== Logic Gates: XOR ===";
        g_xor_a <= "00"; wait for 1 ns; check(g_xor_y, '0', "XOR 00");
        g_xor_a <= "01"; wait for 1 ns; check(g_xor_y, '1', "XOR 01");
        g_xor_a <= "10"; wait for 1 ns; check(g_xor_y, '1', "XOR 10");
        g_xor_a <= "11"; wait for 1 ns; check(g_xor_y, '0', "XOR 11");
        g_xor_a <= "X0"; wait for 1 ns; check(g_xor_y, 'X', "XOR X0 (no domination)");
        g_xor_a <= "X1"; wait for 1 ns; check(g_xor_y, 'X', "XOR X1 (no domination)");

        report "=== Logic Gates: XNOR ===";
        g_xnor_a <= "00"; wait for 1 ns; check(g_xnor_y, '1', "XNOR 00");
        g_xnor_a <= "01"; wait for 1 ns; check(g_xnor_y, '0', "XNOR 01");
        g_xnor_a <= "10"; wait for 1 ns; check(g_xnor_y, '0', "XNOR 10");
        g_xnor_a <= "11"; wait for 1 ns; check(g_xnor_y, '1', "XNOR 11");
        g_xnor_a <= "X0"; wait for 1 ns; check(g_xnor_y, 'X', "XNOR X0");
        g_xnor_a <= "X1"; wait for 1 ns; check(g_xnor_y, 'X', "XNOR X1");

        report "=== Logic Gates: BUF ===";
        g_buf_a <= '0'; wait for 1 ns; check(g_buf_y(0), '0', "BUF 0");
        g_buf_a <= '1'; wait for 1 ns; check(g_buf_y(0), '1', "BUF 1");
        g_buf_a <= 'X'; wait for 1 ns; check(g_buf_y(0), 'X', "BUF X");
        g_buf_a <= 'L'; wait for 1 ns; check(g_buf_y(0), '0', "BUF L (strengthen to 0)");
        g_buf_a <= 'H'; wait for 1 ns; check(g_buf_y(0), '1', "BUF H (strengthen to 1)");
        g_buf_a <= 'Z'; wait for 1 ns; check(g_buf_y(0), 'X', "BUF Z (unknown)");

        report "=== Logic Gates: NOT ===";
        g_not_a <= '0'; wait for 1 ns; check(g_not_y(0), '1', "NOT 0");
        g_not_a <= '1'; wait for 1 ns; check(g_not_y(0), '0', "NOT 1");
        g_not_a <= 'X'; wait for 1 ns; check(g_not_y(0), 'X', "NOT X");
        g_not_a <= 'L'; wait for 1 ns; check(g_not_y(0), '1', "NOT L (weak 0 -> 1)");
        g_not_a <= 'H'; wait for 1 ns; check(g_not_y(0), '0', "NOT H (weak 1 -> 0)");
        g_not_a <= 'Z'; wait for 1 ns; check(g_not_y(0), 'X', "NOT Z (unknown)");

        -----------------------------------------------------------------------
        -- TRISTATE GATES
        -- bufif0/notif0: active-low ctrl (ctrl=0 enables)
        -- bufif1/notif1: active-high ctrl (ctrl=1 enables)
        -- When ctrl=X: output is weakened (H/L instead of 1/0)
        -----------------------------------------------------------------------
        report "=== Tristate: enabled ===";
        ts_data <= '1'; ts_ctrl <= '0'; wait for 1 ns;
        check(ts_bufif0_y, '1', "bufif0 data=1 ctrl=0 (enabled)");
        check(ts_notif0_y, '0', "notif0 data=1 ctrl=0 (enabled, inverts)");
        check(ts_bufif1_y, 'Z', "bufif1 data=1 ctrl=0 (disabled)");
        check(ts_notif1_y, 'Z', "notif1 data=1 ctrl=0 (disabled)");

        ts_data <= '1'; ts_ctrl <= '1'; wait for 1 ns;
        check(ts_bufif0_y, 'Z', "bufif0 data=1 ctrl=1 (disabled)");
        check(ts_notif0_y, 'Z', "notif0 data=1 ctrl=1 (disabled)");
        check(ts_bufif1_y, '1', "bufif1 data=1 ctrl=1 (enabled)");
        check(ts_notif1_y, '0', "notif1 data=1 ctrl=1 (enabled, inverts)");

        ts_data <= '0'; ts_ctrl <= '0'; wait for 1 ns;
        check(ts_bufif0_y, '0', "bufif0 data=0 ctrl=0 (enabled)");
        check(ts_notif0_y, '1', "notif0 data=0 ctrl=0 (enabled, inverts)");

        ts_data <= '0'; ts_ctrl <= '1'; wait for 1 ns;
        check(ts_bufif1_y, '0', "bufif1 data=0 ctrl=1 (enabled)");
        check(ts_notif1_y, '1', "notif1 data=0 ctrl=1 (enabled, inverts)");

        report "=== Tristate: X control (weakened output) ===";
        ts_data <= '1'; ts_ctrl <= 'X'; wait for 1 ns;
        check(ts_bufif0_y, 'H', "bufif0 data=1 ctrl=X (weak 1)");
        check(ts_bufif1_y, 'H', "bufif1 data=1 ctrl=X (weak 1)");
        check(ts_notif0_y, 'L', "notif0 data=1 ctrl=X (weak inverted)");
        check(ts_notif1_y, 'L', "notif1 data=1 ctrl=X (weak inverted)");

        ts_data <= '0'; ts_ctrl <= 'X'; wait for 1 ns;
        check(ts_bufif0_y, 'L', "bufif0 data=0 ctrl=X (weak 0)");
        check(ts_bufif1_y, 'L', "bufif1 data=0 ctrl=X (weak 0)");
        check(ts_notif0_y, 'H', "notif0 data=0 ctrl=X (weak inverted)");
        check(ts_notif1_y, 'H', "notif1 data=0 ctrl=X (weak inverted)");

        ts_data <= 'X'; ts_ctrl <= '0'; wait for 1 ns;
        check(ts_bufif0_y, 'X', "bufif0 data=X ctrl=0 (X passes)");
        check(ts_notif0_y, 'X', "notif0 data=X ctrl=0 (X passes)");

        -----------------------------------------------------------------------
        -- MOS SWITCHES
        -- nmos: gate=1 conducts, gate=0 blocks
        -- pmos: gate=0 conducts, gate=1 blocks
        -- rnmos/rpmos: same polarity but always weaken output
        -----------------------------------------------------------------------
        report "=== MOS: NMOS/PMOS conducting ===";
        m_data <= '1'; m_gate <= '1'; wait for 1 ns;
        check(m_nmos_y,  '1', "nmos data=1 gate=1 (on, pass)");
        check(m_pmos_y,  'Z', "pmos data=1 gate=1 (off)");
        check(m_rnmos_y, 'H', "rnmos data=1 gate=1 (on, weakened)");
        check(m_rpmos_y, 'Z', "rpmos data=1 gate=1 (off)");

        m_data <= '1'; m_gate <= '0'; wait for 1 ns;
        check(m_nmos_y,  'Z', "nmos data=1 gate=0 (off)");
        check(m_pmos_y,  '1', "pmos data=1 gate=0 (on, pass)");
        check(m_rnmos_y, 'Z', "rnmos data=1 gate=0 (off)");
        check(m_rpmos_y, 'H', "rpmos data=1 gate=0 (on, weakened)");

        m_data <= '0'; m_gate <= '1'; wait for 1 ns;
        check(m_nmos_y,  '0', "nmos data=0 gate=1 (on, pass 0)");
        check(m_rnmos_y, 'L', "rnmos data=0 gate=1 (on, weakened 0)");

        m_data <= '0'; m_gate <= '0'; wait for 1 ns;
        check(m_pmos_y,  '0', "pmos data=0 gate=0 (on, pass 0)");
        check(m_rpmos_y, 'L', "rpmos data=0 gate=0 (on, weakened 0)");

        report "=== MOS: X gate (weakened) ===";
        m_data <= '1'; m_gate <= 'X'; wait for 1 ns;
        check(m_nmos_y,  'H', "nmos data=1 gate=X (weak 1)");
        check(m_pmos_y,  'H', "pmos data=1 gate=X (weak 1)");
        check(m_rnmos_y, 'H', "rnmos data=1 gate=X (weak 1)");
        check(m_rpmos_y, 'H', "rpmos data=1 gate=X (weak 1)");

        m_data <= '0'; m_gate <= 'X'; wait for 1 ns;
        check(m_nmos_y,  'L', "nmos data=0 gate=X (weak 0)");
        check(m_pmos_y,  'L', "pmos data=0 gate=X (weak 0)");

        report "=== MOS: CMOS/RCMOS ===";
        -- CMOS: conducts when ngate=1 OR pgate=0
        m_data <= '1'; m_cmos_ngate <= '1'; m_cmos_pgate <= '0'; wait for 1 ns;
        check(m_cmos_y,  '1', "cmos data=1 ng=1,pg=0 (both on)");
        check(m_rcmos_y, 'H', "rcmos data=1 ng=1,pg=0 (weak)");

        m_data <= '1'; m_cmos_ngate <= '0'; m_cmos_pgate <= '1'; wait for 1 ns;
        check(m_cmos_y,  'Z', "cmos data=1 ng=0,pg=1 (both off)");
        check(m_rcmos_y, 'Z', "rcmos data=1 ng=0,pg=1 (both off)");

        m_data <= '1'; m_cmos_ngate <= '1'; m_cmos_pgate <= '1'; wait for 1 ns;
        check(m_cmos_y,  '1', "cmos data=1 ng=1,pg=1 (nmos on)");

        m_data <= '1'; m_cmos_ngate <= '0'; m_cmos_pgate <= '0'; wait for 1 ns;
        check(m_cmos_y,  '1', "cmos data=1 ng=0,pg=0 (pmos on)");

        m_data <= '1'; m_cmos_ngate <= 'X'; m_cmos_pgate <= '1'; wait for 1 ns;
        check(m_cmos_y,  'H', "cmos data=1 ng=X,pg=1 (uncertain, weak)");

        m_data <= '0'; m_cmos_ngate <= '1'; m_cmos_pgate <= '0'; wait for 1 ns;
        check(m_cmos_y,  '0', "cmos data=0 ng=1,pg=0 (pass 0)");
        check(m_rcmos_y, 'L', "rcmos data=0 ng=1,pg=0 (weak 0)");

        -----------------------------------------------------------------------
        -- TRAN SWITCHES (standalone, each driven with same inputs)
        -- tran: always pass, a_driver<=b_others, b_driver<=a_others
        -- tranif0: ctrl=0 conducts, ctrl=1 blocks
        -- tranif1: ctrl=1 conducts, ctrl=0 blocks
        -- rtran: always pass, weakened
        -- rtranif0/1: controlled, always weakened when conducting
        -----------------------------------------------------------------------
        report "=== Tran: conducting (ctrl=0 for if0, ctrl=1 for if1) ===";
        tr_bo <= '1'; tr_ao <= 'Z'; tr_ctrl <= '0'; wait for 1 ns;
        check(tr_tran_ad,   '1', "tran a_drv (b=1 passes)");
        check(tr_tran_bd,   'Z', "tran b_drv (a=Z passes)");
        check(tr_trif0_ad,  '1', "tranif0 a_drv ctrl=0 (conducts)");
        check(tr_trif0_bd,  'Z', "tranif0 b_drv ctrl=0");
        check(tr_trif1_ad,  'Z', "tranif1 a_drv ctrl=0 (blocked)");
        check(tr_trif1_bd,  'Z', "tranif1 b_drv ctrl=0 (blocked)");
        check(tr_rtran_ad,  'H', "rtran a_drv (weakened 1)");
        check(tr_rtran_bd,  'Z', "rtran b_drv (weakened Z stays Z)");
        check(tr_rtrif0_ad, 'H', "rtranif0 a_drv ctrl=0 (weak)");
        check(tr_rtrif0_bd, 'Z', "rtranif0 b_drv ctrl=0 (weak Z)");
        check(tr_rtrif1_ad, 'Z', "rtranif1 a_drv ctrl=0 (blocked)");
        check(tr_rtrif1_bd, 'Z', "rtranif1 b_drv ctrl=0 (blocked)");

        tr_ctrl <= '1'; wait for 1 ns;
        check(tr_trif0_ad,  'Z', "tranif0 a_drv ctrl=1 (blocked)");
        check(tr_trif0_bd,  'Z', "tranif0 b_drv ctrl=1 (blocked)");
        check(tr_trif1_ad,  '1', "tranif1 a_drv ctrl=1 (conducts)");
        check(tr_trif1_bd,  'Z', "tranif1 b_drv ctrl=1");
        check(tr_rtrif0_ad, 'Z', "rtranif0 a_drv ctrl=1 (blocked)");
        check(tr_rtrif0_bd, 'Z', "rtranif0 b_drv ctrl=1 (blocked)");
        check(tr_rtrif1_ad, 'H', "rtranif1 a_drv ctrl=1 (weak)");
        check(tr_rtrif1_bd, 'Z', "rtranif1 b_drv ctrl=1 (weak Z)");

        report "=== Tran: X control (weakened for controlled, pass for always-on) ===";
        tr_bo <= '1'; tr_ao <= '0'; tr_ctrl <= 'X'; wait for 1 ns;
        check(tr_tran_ad,   '1', "tran a_drv (always on, b=1)");
        check(tr_tran_bd,   '0', "tran b_drv (always on, a=0)");
        check(tr_trif0_ad,  'H', "tranif0 a_drv ctrl=X (weak 1)");
        check(tr_trif0_bd,  'L', "tranif0 b_drv ctrl=X (weak 0)");
        check(tr_trif1_ad,  'H', "tranif1 a_drv ctrl=X (weak 1)");
        check(tr_trif1_bd,  'L', "tranif1 b_drv ctrl=X (weak 0)");
        check(tr_rtran_ad,  'H', "rtran a_drv (weak 1)");
        check(tr_rtran_bd,  'L', "rtran b_drv (weak 0)");
        check(tr_rtrif0_ad, 'H', "rtranif0 a_drv ctrl=X (weak 1)");
        check(tr_rtrif0_bd, 'L', "rtranif0 b_drv ctrl=X (weak 0)");
        check(tr_rtrif1_ad, 'H', "rtranif1 a_drv ctrl=X (weak 1)");
        check(tr_rtrif1_bd, 'L', "rtranif1 b_drv ctrl=X (weak 0)");

        report "=== Tran: pass 0 from b side ===";
        tr_bo <= '0'; tr_ao <= 'Z'; tr_ctrl <= '1'; wait for 1 ns;
        check(tr_tran_ad,   '0', "tran a_drv (b=0)");
        check(tr_trif1_ad,  '0', "tranif1 a_drv ctrl=1 (pass 0)");
        check(tr_rtran_ad,  'L', "rtran a_drv (weak 0)");
        check(tr_rtrif1_ad, 'L', "rtranif1 a_drv ctrl=1 (weak 0)");

        -----------------------------------------------------------------------
        -- Summary
        -----------------------------------------------------------------------
        report "";
        report "========================================";
        report "  Results: " & integer'image(pass_count) & " passed, " &
               integer'image(fail_count) & " failed";
        report "========================================";

        if fail_count > 0 then
            report "TEST FAILED" severity failure;
        else
            report "ALL TESTS PASSED";
        end if;

        wait;
    end process;

end architecture test;
