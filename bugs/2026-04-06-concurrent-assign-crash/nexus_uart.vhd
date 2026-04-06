-- This VHDL was converted from Verilog using the
-- Icarus Verilog VHDL Code Generator 13.0 (devel) (f647ae2cb)

library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;
library sv2vhdl;
use sv2vhdl.sv_display_pkg.all;
use sv2vhdl.sv_analog_pkg.all;

-- Generated from Verilog module nexus_uart (../../duts/nexus_uart/nexus_uart.sv:1)
--   ADDR_CTRL = 0
--   ADDR_FIFO_CTRL = 16
--   ADDR_INTR_ENABLE = 24
--   ADDR_INTR_STATE = 20
--   ADDR_INTR_TEST = 28
--   ADDR_RXDATA = 12
--   ADDR_STATUS = 4
--   ADDR_TXDATA = 8
--   FIFO_AW = 5
--   FIFO_DEPTH = 32
--   NUM_IRQS = 7
entity nexus_uart is
  port (
    alert_o : out std_logic;
    clk_i : in std_logic;
    intr_o : out unsigned(6 downto 0);
    rst_ni : in std_logic;
    tl_a_address_i : in unsigned(31 downto 0);
    tl_a_data_i : in unsigned(31 downto 0);
    tl_a_mask_i : in unsigned(3 downto 0);
    tl_a_opcode_i : in unsigned(2 downto 0);
    tl_a_ready_o : out std_logic;
    tl_a_size_i : in unsigned(1 downto 0);
    tl_a_source_i : in unsigned(7 downto 0);
    tl_a_valid_i : in std_logic;
    tl_d_data_o : out unsigned(31 downto 0);
    tl_d_error_o : out std_logic;
    tl_d_opcode_o : out unsigned(2 downto 0);
    tl_d_ready_i : in std_logic;
    tl_d_source_o : out unsigned(7 downto 0);
    tl_d_valid_o : out std_logic;
    uart_rx_i : in std_logic;
    uart_tx_o : out std_logic
  );
end entity; 

-- Generated from Verilog module nexus_uart (../../duts/nexus_uart/nexus_uart.sv:1)
--   ADDR_CTRL = 0
--   ADDR_FIFO_CTRL = 16
--   ADDR_INTR_ENABLE = 24
--   ADDR_INTR_STATE = 20
--   ADDR_INTR_TEST = 28
--   ADDR_RXDATA = 12
--   ADDR_STATUS = 4
--   ADDR_TXDATA = 8
--   FIFO_AW = 5
--   FIFO_DEPTH = 32
--   NUM_IRQS = 7
architecture from_verilog of nexus_uart is
  signal intr_o_Reg : unsigned(6 downto 0);
  signal tl_a_ready_o_Reg : std_logic;
  signal tl_d_data_o_Reg : unsigned(31 downto 0);
  signal tl_d_error_o_Reg : std_logic;
  signal tl_d_opcode_o_Reg : unsigned(2 downto 0);
  signal tl_d_source_o_Reg : unsigned(7 downto 0);
  signal tl_d_valid_o_Reg : std_logic;
  signal uart_tx_o_Reg : std_logic;
  signal ctrl_baud_divisor : unsigned(15 downto 0);  -- Declared at ../../duts/nexus_uart/nexus_uart.sv:62
  signal ctrl_loopback_en : std_logic;  -- Declared at ../../duts/nexus_uart/nexus_uart.sv:65
  signal ctrl_parity_mode : unsigned(1 downto 0);  -- Declared at ../../duts/nexus_uart/nexus_uart.sv:63
  signal ctrl_reg : unsigned(31 downto 0);  -- Declared at ../../duts/nexus_uart/nexus_uart.sv:66
  signal ctrl_rx_enable : std_logic;  -- Declared at ../../duts/nexus_uart/nexus_uart.sv:61
  signal ctrl_stop_bits : std_logic;  -- Declared at ../../duts/nexus_uart/nexus_uart.sv:64
  signal ctrl_tx_enable : std_logic;  -- Declared at ../../duts/nexus_uart/nexus_uart.sv:60
  signal fifo_ctrl_rx_rst : std_logic;  -- Declared at ../../duts/nexus_uart/nexus_uart.sv:71
  signal fifo_ctrl_rx_watermark : unsigned(4 downto 0);  -- Declared at ../../duts/nexus_uart/nexus_uart.sv:69
  signal fifo_ctrl_tx_rst : std_logic;  -- Declared at ../../duts/nexus_uart/nexus_uart.sv:70
  signal fifo_ctrl_tx_watermark : unsigned(4 downto 0);  -- Declared at ../../duts/nexus_uart/nexus_uart.sv:68
  signal intr_enable : unsigned(6 downto 0);  -- Declared at ../../duts/nexus_uart/nexus_uart.sv:74
  signal intr_hw_set : unsigned(6 downto 0);  -- Declared at ../../duts/nexus_uart/nexus_uart.sv:75
  signal intr_state : unsigned(6 downto 0);  -- Declared at ../../duts/nexus_uart/nexus_uart.sv:73
  signal rx_active : std_logic;  -- Declared at ../../duts/nexus_uart/nexus_uart.sv:127
  signal rx_baud_cnt : unsigned(15 downto 0);  -- Declared at ../../duts/nexus_uart/nexus_uart.sv:111
  signal rx_bit_idx : unsigned(2 downto 0);  -- Declared at ../../duts/nexus_uart/nexus_uart.sv:114
  signal rx_expected_parity : std_logic;  -- Declared at ../../duts/nexus_uart/nexus_uart.sv:118
  signal rx_fifo_empty : std_logic;  -- Declared at ../../duts/nexus_uart/nexus_uart.sv:91
  signal rx_fifo_full : std_logic;  -- Declared at ../../duts/nexus_uart/nexus_uart.sv:92
  signal rx_fifo_level : unsigned(5 downto 0);  -- Declared at ../../duts/nexus_uart/nexus_uart.sv:93
  type rx_fifo_mem_Type is array (31 downto 0) of unsigned(7 downto 0);
  signal rx_fifo_mem : rx_fifo_mem_Type;  -- Declared at ../../duts/nexus_uart/nexus_uart.sv:88
  signal rx_fifo_pop : std_logic;  -- Declared at ../../duts/nexus_uart/nexus_uart.sv:95
  signal rx_fifo_push : std_logic;  -- Declared at ../../duts/nexus_uart/nexus_uart.sv:94
  signal rx_fifo_rdata : unsigned(7 downto 0);  -- Declared at ../../duts/nexus_uart/nexus_uart.sv:97
  signal rx_fifo_wdata : unsigned(7 downto 0);  -- Declared at ../../duts/nexus_uart/nexus_uart.sv:96
  signal rx_frame_err_sticky : std_logic;  -- Declared at ../../duts/nexus_uart/nexus_uart.sv:101
  signal rx_frame_error_det : std_logic;  -- Declared at ../../duts/nexus_uart/nexus_uart.sv:489
  signal rx_os_cnt : unsigned(3 downto 0);  -- Declared at ../../duts/nexus_uart/nexus_uart.sv:113
  signal rx_os_divisor : unsigned(15 downto 0);  -- Declared at ../../duts/nexus_uart/nexus_uart.sv:458
  signal rx_os_tick : std_logic;  -- Declared at ../../duts/nexus_uart/nexus_uart.sv:112
  signal rx_overflow_det : std_logic;  -- Declared at ../../duts/nexus_uart/nexus_uart.sv:490
  signal rx_overrun_sticky : std_logic;  -- Declared at ../../duts/nexus_uart/nexus_uart.sv:99
  signal rx_parity_calc : std_logic;  -- Declared at ../../duts/nexus_uart/nexus_uart.sv:116
  signal rx_parity_err_sticky : std_logic;  -- Declared at ../../duts/nexus_uart/nexus_uart.sv:100
  signal rx_parity_error_det : std_logic;  -- Declared at ../../duts/nexus_uart/nexus_uart.sv:488
  signal rx_push_data : unsigned(7 downto 0);  -- Declared at ../../duts/nexus_uart/nexus_uart.sv:487
  signal rx_push_valid : std_logic;  -- Declared at ../../duts/nexus_uart/nexus_uart.sv:486
  signal rx_rptr : unsigned(5 downto 0);  -- Declared at ../../duts/nexus_uart/nexus_uart.sv:90
  signal rx_serial_in : std_logic;  -- Declared at ../../duts/nexus_uart/nexus_uart.sv:120
  signal rx_serial_q : std_logic;  -- Declared at ../../duts/nexus_uart/nexus_uart.sv:121
  signal rx_serial_qq : std_logic;  -- Declared at ../../duts/nexus_uart/nexus_uart.sv:122
  signal rx_shift_reg : unsigned(7 downto 0);  -- Declared at ../../duts/nexus_uart/nexus_uart.sv:115
  signal rx_state : unsigned(3 downto 0);  -- Declared at ../../duts/nexus_uart/nexus_uart.sv:110
  signal rx_timeout_cnt : unsigned(31 downto 0);  -- Declared at ../../duts/nexus_uart/nexus_uart.sv:124
  signal rx_timeout_event : std_logic;  -- Declared at ../../duts/nexus_uart/nexus_uart.sv:125
  signal rx_timeout_thresh : unsigned(31 downto 0);  -- Declared at ../../duts/nexus_uart/nexus_uart.sv:126
  signal rx_wptr : unsigned(5 downto 0);  -- Declared at ../../duts/nexus_uart/nexus_uart.sv:89
  signal tl_req_addr : unsigned(31 downto 0);  -- Declared at ../../duts/nexus_uart/nexus_uart.sv:131
  signal tl_req_valid : std_logic;  -- Declared at ../../duts/nexus_uart/nexus_uart.sv:129
  signal tl_req_wdata : unsigned(31 downto 0);  -- Declared at ../../duts/nexus_uart/nexus_uart.sv:132
  signal tl_req_write : std_logic;  -- Declared at ../../duts/nexus_uart/nexus_uart.sv:130
  signal tl_rsp_error : std_logic;  -- Declared at ../../duts/nexus_uart/nexus_uart.sv:134
  signal tl_rsp_pending : std_logic;  -- Declared at ../../duts/nexus_uart/nexus_uart.sv:135
  signal tl_rsp_rdata : unsigned(31 downto 0);  -- Declared at ../../duts/nexus_uart/nexus_uart.sv:133
  signal tx_baud_cnt : unsigned(15 downto 0);  -- Declared at ../../duts/nexus_uart/nexus_uart.sv:104
  signal tx_baud_tick : std_logic;  -- Declared at ../../duts/nexus_uart/nexus_uart.sv:105
  signal tx_bit_idx : unsigned(2 downto 0);  -- Declared at ../../duts/nexus_uart/nexus_uart.sv:106
  signal tx_fifo_empty : std_logic;  -- Declared at ../../duts/nexus_uart/nexus_uart.sv:80
  signal tx_fifo_full : std_logic;  -- Declared at ../../duts/nexus_uart/nexus_uart.sv:81
  signal tx_fifo_level : unsigned(5 downto 0);  -- Declared at ../../duts/nexus_uart/nexus_uart.sv:82
  type tx_fifo_mem_Type is array (31 downto 0) of unsigned(7 downto 0);
  signal tx_fifo_mem : tx_fifo_mem_Type;  -- Declared at ../../duts/nexus_uart/nexus_uart.sv:77
  signal tx_fifo_pop : std_logic;  -- Declared at ../../duts/nexus_uart/nexus_uart.sv:84
  signal tx_fifo_pop_req : std_logic;  -- Declared at ../../duts/nexus_uart/nexus_uart.sv:346
  signal tx_fifo_push : std_logic;  -- Declared at ../../duts/nexus_uart/nexus_uart.sv:83
  signal tx_fifo_rdata : unsigned(7 downto 0);  -- Declared at ../../duts/nexus_uart/nexus_uart.sv:86
  signal tx_fifo_wdata : unsigned(7 downto 0);  -- Declared at ../../duts/nexus_uart/nexus_uart.sv:85
  signal tx_parity_bit : std_logic;  -- Declared at ../../duts/nexus_uart/nexus_uart.sv:108
  signal tx_rptr : unsigned(5 downto 0);  -- Declared at ../../duts/nexus_uart/nexus_uart.sv:79
  signal tx_shift_reg : unsigned(7 downto 0);  -- Declared at ../../duts/nexus_uart/nexus_uart.sv:107
  signal tx_state : unsigned(3 downto 0);  -- Declared at ../../duts/nexus_uart/nexus_uart.sv:103
  signal tx_wptr : unsigned(5 downto 0);  -- Declared at ../../duts/nexus_uart/nexus_uart.sv:78
  
  function Boolean_To_Logic(B : Boolean) return std_logic is
  begin
    if B then
      return '1';
    else
      return '0';
    end if;
  end function;
  
  function Reduce_XOR(X : std_logic_vector) return std_logic is
    variable R : std_logic := '0';
  begin
    for I in X'Range loop
      R := X(I) xor R;
    end loop;
    return R;
  end function;
begin
  intr_o <= intr_o_Reg;
  tl_a_ready_o <= tl_a_ready_o_Reg;
  tl_d_data_o <= tl_d_data_o_Reg;
  tl_d_error_o <= tl_d_error_o_Reg;
  tl_d_opcode_o <= tl_d_opcode_o_Reg;
  tl_d_source_o <= tl_d_source_o_Reg;
  tl_d_valid_o <= tl_d_valid_o_Reg;
  uart_tx_o <= uart_tx_o_Reg;
  tx_fifo_pop <= tx_fifo_pop_req;
  rx_fifo_push <= rx_push_valid;
  rx_fifo_wdata <= rx_push_data;
  alert_o <= '0';
  
  -- Generated from always process in nexus_uart (../../duts/nexus_uart/nexus_uart.sv:137)
  process (tl_a_valid_i, tl_a_ready_o_Reg, tl_a_opcode_i, tl_a_address_i, tl_a_data_i) is
  begin
    tl_req_valid <= tl_a_valid_i and tl_a_ready_o_Reg;
    tl_req_write <= Boolean_To_Logic((tl_a_opcode_i = "000") or (tl_a_opcode_i = "001"));
    tl_req_addr <= tl_a_address_i(2 + 29 downto 2) & "00";
    tl_req_wdata <= tl_a_data_i;
  end process;
  
  -- Generated from always process in nexus_uart (../../duts/nexus_uart/nexus_uart.sv:144)
  process (tl_rsp_pending, tl_d_ready_i) is
  begin
    tl_a_ready_o_Reg <= Boolean_To_Logic(((not tl_rsp_pending) = '1') or (tl_d_ready_i = '1'));
  end process;
  
  -- Generated from always process in nexus_uart (../../duts/nexus_uart/nexus_uart.sv:148)
  process (clk_i, rst_ni) is
  begin
    if (not rst_ni) = '1' then
      tl_rsp_pending <= '0';
      tl_d_valid_o_Reg <= '0';
      tl_d_opcode_o_Reg <= "000";
      tl_d_data_o_Reg <= X"00000000";
      tl_d_source_o_Reg <= X"00";
      tl_d_error_o_Reg <= '0';
    elsif rising_edge(clk_i) then
      if (tl_rsp_pending = '1') and (tl_d_ready_i = '1') then
        tl_rsp_pending <= '0';
        tl_d_valid_o_Reg <= '0';
      end if;
      if tl_req_valid = '1' then
        tl_rsp_pending <= '1';
        tl_d_valid_o_Reg <= '1';
        if tl_req_write = '1' then
          tl_d_opcode_o_Reg <= "000";
        else
          tl_d_opcode_o_Reg <= "001";
        end if;
        tl_d_data_o_Reg <= tl_rsp_rdata;
        tl_d_source_o_Reg <= tl_a_source_i;
        tl_d_error_o_Reg <= tl_rsp_error;
      end if;
    end if;
  end process;
  
  -- Generated from always process in nexus_uart (../../duts/nexus_uart/nexus_uart.sv:172)
  process (tl_req_valid, tl_req_write, tl_req_addr, tl_req_wdata, ctrl_reg, rx_frame_err_sticky, rx_parity_err_sticky, rx_overrun_sticky, rx_fifo_level, tx_fifo_level, rx_fifo_full, rx_fifo_empty, tx_fifo_full, tx_fifo_empty, rx_fifo_rdata, fifo_ctrl_rx_watermark, fifo_ctrl_tx_watermark, intr_state, intr_enable) is
  begin
    tl_rsp_rdata <= X"00000000";
    tl_rsp_error <= '0';
    tx_fifo_push <= '0';
    tx_fifo_wdata <= X"00";
    rx_fifo_pop <= '0';
    if tl_req_valid = '1' then
      if tl_req_write = '1' then
        case tl_req_addr is
          when X"00000000" =>
            null;
          when X"00000008" =>
            null;
          when X"00000010" =>
            null;
          when X"00000014" =>
            null;
          when X"00000018" =>
            null;
          when X"0000001c" =>
            null;
          when X"00000004" =>
            tl_rsp_error <= '1';
          when X"0000000c" =>
            tl_rsp_error <= '1';
          when others =>
            tl_rsp_error <= '1';
        end case;
        if tl_req_addr = X"00000008" then
          tx_fifo_push <= '1';
          tx_fifo_wdata <= tl_req_wdata(0 + 7 downto 0);
        end if;
      else
        case tl_req_addr is
          when X"00000000" =>
            tl_rsp_rdata <= ctrl_reg;
          when X"00000004" =>
            tl_rsp_rdata <= "0000000000000" & rx_frame_err_sticky & rx_parity_err_sticky & rx_overrun_sticky & rx_fifo_level & tx_fifo_level & rx_fifo_full & rx_fifo_empty & tx_fifo_full & tx_fifo_empty;
          when X"00000008" =>
            tl_rsp_rdata <= X"00000000";
            tl_rsp_error <= '1';
          when X"0000000c" =>
            tl_rsp_rdata <= X"000000" & rx_fifo_rdata;
            rx_fifo_pop <= not rx_fifo_empty;
          when X"00000010" =>
            tl_rsp_rdata <= X"00000" & "00" & fifo_ctrl_rx_watermark & fifo_ctrl_tx_watermark;
          when X"00000014" =>
            tl_rsp_rdata <= "0000000000000000000000000" & intr_state;
          when X"00000018" =>
            tl_rsp_rdata <= "0000000000000000000000000" & intr_enable;
          when X"0000001c" =>
            tl_rsp_rdata <= X"00000000";
          when others =>
            tl_rsp_error <= '1';
        end case;
      end if;
    end if;
  end process;
  
  -- Generated from always process in nexus_uart (../../duts/nexus_uart/nexus_uart.sv:218)
  process (clk_i, rst_ni) is
  begin
    if (not rst_ni) = '1' then
      ctrl_reg <= X"00000000";
      ctrl_tx_enable <= '0';
      ctrl_rx_enable <= '0';
      ctrl_baud_divisor <= X"0000";
      ctrl_parity_mode <= "00";
      ctrl_stop_bits <= '0';
      ctrl_loopback_en <= '0';
    elsif rising_edge(clk_i) then
      if ((tl_req_valid = '1') and (tl_req_write = '1')) and (tl_req_addr = X"00000000") then
        ctrl_reg <= tl_req_wdata;
        ctrl_tx_enable <= tl_req_wdata(0);
        ctrl_rx_enable <= tl_req_wdata(1);
        ctrl_baud_divisor <= tl_req_wdata(2 + 15 downto 2);
        ctrl_parity_mode <= tl_req_wdata(18 + 1 downto 18);
        ctrl_stop_bits <= tl_req_wdata(20);
        ctrl_loopback_en <= tl_req_wdata(21);
      end if;
    end if;
  end process;
  
  -- Generated from always process in nexus_uart (../../duts/nexus_uart/nexus_uart.sv:238)
  process (clk_i, rst_ni) is
  begin
    if (not rst_ni) = '1' then
      fifo_ctrl_tx_watermark <= "00001";
      fifo_ctrl_rx_watermark <= "00001";
      fifo_ctrl_tx_rst <= '0';
      fifo_ctrl_rx_rst <= '0';
    elsif rising_edge(clk_i) then
      if fifo_ctrl_tx_rst = '1' then
        fifo_ctrl_tx_rst <= '0';
      end if;
      if fifo_ctrl_rx_rst = '1' then
        fifo_ctrl_rx_rst <= '0';
      end if;
      if ((tl_req_valid = '1') and (tl_req_write = '1')) and (tl_req_addr = X"00000010") then
        fifo_ctrl_tx_watermark <= tl_req_wdata(0 + 4 downto 0);
        fifo_ctrl_rx_watermark <= tl_req_wdata(5 + 4 downto 5);
        fifo_ctrl_tx_rst <= tl_req_wdata(10);
        fifo_ctrl_rx_rst <= tl_req_wdata(11);
      end if;
    end if;
  end process;
  
  -- Generated from always process in nexus_uart (../../duts/nexus_uart/nexus_uart.sv:257)
  process (clk_i, rst_ni) is
  begin
    if (not rst_ni) = '1' then
      intr_state <= "0000000";
      intr_enable <= "0000000";
    elsif rising_edge(clk_i) then
      intr_state <= intr_state or intr_hw_set;
      if (tl_req_valid = '1') and (tl_req_write = '1') then
        if tl_req_addr = X"00000014" then
          intr_state <= (intr_state or intr_hw_set) and (not tl_req_wdata(0 + 6 downto 0));
        end if;
        if tl_req_addr = X"00000018" then
          intr_enable <= tl_req_wdata(0 + 6 downto 0);
        end if;
        if tl_req_addr = X"0000001c" then
          intr_state <= (intr_state or intr_hw_set) or tl_req_wdata(0 + 6 downto 0);
        end if;
      end if;
    end if;
  end process;
  
  -- Generated from always process in nexus_uart (../../duts/nexus_uart/nexus_uart.sv:278)
  process (intr_state, intr_enable) is
  begin
    intr_o_Reg <= intr_state and intr_enable;
  end process;
  
  -- Generated from always process in nexus_uart (../../duts/nexus_uart/nexus_uart.sv:282)
  process (tx_wptr, tx_rptr) is
  begin
    tx_fifo_empty <= Boolean_To_Logic(tx_wptr = tx_rptr);
    tx_fifo_full <= Boolean_To_Logic((tx_wptr(5) /= tx_rptr(5)) and (tx_wptr(0 + 4 downto 0) = tx_rptr(0 + 4 downto 0)));
    tx_fifo_level <= tx_wptr - tx_rptr;
  end process;
  
  -- Generated from always process in nexus_uart (../../duts/nexus_uart/nexus_uart.sv:289)
  process (clk_i, rst_ni) is
  begin
    if (not rst_ni) = '1' then
      tx_wptr <= "000000";
    elsif rising_edge(clk_i) then
      if fifo_ctrl_tx_rst = '1' then
        tx_wptr <= "000000";
      else
        if (tx_fifo_push = '1') and ((not tx_fifo_full) = '1') then
          tx_fifo_mem(To_Integer(Resize(tx_wptr(0 + 4 downto 0), 7))) <= tx_fifo_wdata;
          tx_wptr <= tx_wptr + "000001";
        end if;
      end if;
    end if;
  end process;
  
  -- Generated from always process in nexus_uart (../../duts/nexus_uart/nexus_uart.sv:300)
  process (clk_i, rst_ni) is
  begin
    if (not rst_ni) = '1' then
      tx_rptr <= "000000";
    elsif rising_edge(clk_i) then
      if fifo_ctrl_tx_rst = '1' then
        tx_rptr <= "000000";
      else
        if (tx_fifo_pop = '1') and ((not tx_fifo_empty) = '1') then
          tx_rptr <= tx_rptr + "000001";
        end if;
      end if;
    end if;
  end process;
  
  -- Generated from always process in nexus_uart (../../duts/nexus_uart/nexus_uart.sv:310)
  process (tx_rptr, tx_fifo_mem, tx_fifo_mem, tx_fifo_mem, tx_fifo_mem, tx_fifo_mem, tx_fifo_mem, tx_fifo_mem, tx_fifo_mem, tx_fifo_mem, tx_fifo_mem, tx_fifo_mem, tx_fifo_mem, tx_fifo_mem, tx_fifo_mem, tx_fifo_mem, tx_fifo_mem, tx_fifo_mem, tx_fifo_mem, tx_fifo_mem, tx_fifo_mem, tx_fifo_mem, tx_fifo_mem, tx_fifo_mem, tx_fifo_mem, tx_fifo_mem, tx_fifo_mem, tx_fifo_mem, tx_fifo_mem, tx_fifo_mem, tx_fifo_mem, tx_fifo_mem, tx_fifo_mem) is
  begin
    tx_fifo_rdata <= tx_fifo_mem(To_Integer(Resize(tx_rptr(0 + 4 downto 0), 7)));
  end process;
  
  -- Generated from always process in nexus_uart (../../duts/nexus_uart/nexus_uart.sv:314)
  process (rx_wptr, rx_rptr) is
  begin
    rx_fifo_empty <= Boolean_To_Logic(rx_wptr = rx_rptr);
    rx_fifo_full <= Boolean_To_Logic((rx_wptr(5) /= rx_rptr(5)) and (rx_wptr(0 + 4 downto 0) = rx_rptr(0 + 4 downto 0)));
    rx_fifo_level <= rx_wptr - rx_rptr;
  end process;
  
  -- Generated from always process in nexus_uart (../../duts/nexus_uart/nexus_uart.sv:321)
  process (clk_i, rst_ni) is
  begin
    if (not rst_ni) = '1' then
      rx_wptr <= "000000";
    elsif rising_edge(clk_i) then
      if fifo_ctrl_rx_rst = '1' then
        rx_wptr <= "000000";
      else
        if (rx_fifo_push = '1') and ((not rx_fifo_full) = '1') then
          rx_fifo_mem(To_Integer(Resize(rx_wptr(0 + 4 downto 0), 7))) <= rx_fifo_wdata;
          rx_wptr <= rx_wptr + "000001";
        end if;
      end if;
    end if;
  end process;
  
  -- Generated from always process in nexus_uart (../../duts/nexus_uart/nexus_uart.sv:332)
  process (clk_i, rst_ni) is
  begin
    if (not rst_ni) = '1' then
      rx_rptr <= "000000";
    elsif rising_edge(clk_i) then
      if fifo_ctrl_rx_rst = '1' then
        rx_rptr <= "000000";
      else
        if (rx_fifo_pop = '1') and ((not rx_fifo_empty) = '1') then
          rx_rptr <= rx_rptr + "000001";
        end if;
      end if;
    end if;
  end process;
  
  -- Generated from always process in nexus_uart (../../duts/nexus_uart/nexus_uart.sv:342)
  process (rx_rptr, rx_fifo_mem, rx_fifo_mem, rx_fifo_mem, rx_fifo_mem, rx_fifo_mem, rx_fifo_mem, rx_fifo_mem, rx_fifo_mem, rx_fifo_mem, rx_fifo_mem, rx_fifo_mem, rx_fifo_mem, rx_fifo_mem, rx_fifo_mem, rx_fifo_mem, rx_fifo_mem, rx_fifo_mem, rx_fifo_mem, rx_fifo_mem, rx_fifo_mem, rx_fifo_mem, rx_fifo_mem, rx_fifo_mem, rx_fifo_mem, rx_fifo_mem, rx_fifo_mem, rx_fifo_mem, rx_fifo_mem, rx_fifo_mem, rx_fifo_mem, rx_fifo_mem, rx_fifo_mem) is
  begin
    rx_fifo_rdata <= rx_fifo_mem(To_Integer(Resize(rx_rptr(0 + 4 downto 0), 7)));
  end process;
  
  -- Generated from always process in nexus_uart (../../duts/nexus_uart/nexus_uart.sv:349)
  process (tx_baud_cnt, ctrl_baud_divisor) is
  begin
    tx_baud_tick <= Boolean_To_Logic((tx_baud_cnt = ctrl_baud_divisor) and (ctrl_baud_divisor /= X"0000"));
  end process;
  
  -- Generated from always process in nexus_uart (../../duts/nexus_uart/nexus_uart.sv:353)
  process (clk_i, rst_ni) is
  begin
    if (not rst_ni) = '1' then
      tx_baud_cnt <= X"0000";
    elsif rising_edge(clk_i) then
      if (tx_state = X"0") or (tx_baud_tick = '1') then
        tx_baud_cnt <= X"0000";
      else
        tx_baud_cnt <= tx_baud_cnt + X"0001";
      end if;
    end if;
  end process;
  
  -- Generated from always process in nexus_uart (../../duts/nexus_uart/nexus_uart.sv:365)
  process (clk_i, rst_ni) is
  begin
    if (not rst_ni) = '1' then
      tx_state <= X"0";
      uart_tx_o_Reg <= '1';
      tx_bit_idx <= "000";
      tx_shift_reg <= X"00";
      tx_parity_bit <= '0';
      tx_fifo_pop_req <= '0';
    elsif rising_edge(clk_i) then
      tx_fifo_pop_req <= '0';
      case tx_state is
        when X"0" =>
          uart_tx_o_Reg <= '1';
          if (ctrl_tx_enable = '1') and ((not tx_fifo_empty) = '1') then
            tx_shift_reg <= tx_fifo_rdata;
            tx_fifo_pop_req <= '1';
            tx_state <= X"1";
            tx_bit_idx <= "000";
            if ctrl_parity_mode = "01" then
              tx_parity_bit <= Reduce_XOR(std_logic_vector(tx_fifo_rdata));
            else
              if ctrl_parity_mode = "10" then
                tx_parity_bit <= not Reduce_XOR(std_logic_vector(tx_fifo_rdata));
              else
                tx_parity_bit <= '0';
              end if;
            end if;
          end if;
        when X"1" =>
          uart_tx_o_Reg <= '0';
          if tx_baud_tick = '1' then
            tx_state <= X"2";
            tx_bit_idx <= "000";
          end if;
        when X"2" =>
          uart_tx_o_Reg <= tx_shift_reg(To_Integer(tx_bit_idx));
          if tx_baud_tick = '1' then
            if tx_bit_idx = "111" then
              if ctrl_parity_mode /= "00" then
                tx_state <= X"3";
              else
                tx_state <= X"4";
              end if;
            else
              tx_bit_idx <= tx_bit_idx + "001";
            end if;
          end if;
        when X"3" =>
          uart_tx_o_Reg <= tx_parity_bit;
          if tx_baud_tick = '1' then
            tx_state <= X"4";
          end if;
        when X"4" =>
          uart_tx_o_Reg <= '1';
          if tx_baud_tick = '1' then
            if ctrl_stop_bits = '1' then
              tx_state <= X"5";
            else
              tx_state <= X"0";
            end if;
          end if;
        when X"5" =>
          uart_tx_o_Reg <= '1';
          if tx_baud_tick = '1' then
            tx_state <= X"0";
          end if;
        when others =>
          tx_state <= X"0";
      end case;
    end if;
  end process;
  
  -- Generated from always process in nexus_uart (../../duts/nexus_uart/nexus_uart.sv:444)
  process (ctrl_loopback_en, uart_tx_o_Reg, uart_rx_i) is
  begin
    if ctrl_loopback_en = '1' then
      rx_serial_in <= uart_tx_o_Reg;
    else
      rx_serial_in <= uart_rx_i;
    end if;
  end process;
  
  -- Generated from always process in nexus_uart (../../duts/nexus_uart/nexus_uart.sv:448)
  process (clk_i, rst_ni) is
  begin
    if (not rst_ni) = '1' then
      rx_serial_q <= '1';
      rx_serial_qq <= '1';
    elsif rising_edge(clk_i) then
      rx_serial_q <= rx_serial_in;
      rx_serial_qq <= rx_serial_q;
    end if;
  end process;
  
  -- Generated from always process in nexus_uart (../../duts/nexus_uart/nexus_uart.sv:459)
  process (ctrl_baud_divisor) is
  begin
    rx_os_divisor <= X"0" & ctrl_baud_divisor(4 + 11 downto 4);
  end process;
  
  -- Generated from always process in nexus_uart (../../duts/nexus_uart/nexus_uart.sv:463)
  process (rx_baud_cnt, rx_os_divisor, ctrl_baud_divisor) is
  begin
    rx_os_tick <= Boolean_To_Logic((rx_baud_cnt = rx_os_divisor) and (ctrl_baud_divisor /= X"0000"));
  end process;
  
  -- Generated from always process in nexus_uart (../../duts/nexus_uart/nexus_uart.sv:467)
  process (ctrl_parity_mode, rx_parity_calc) is
  begin
    if ctrl_parity_mode = "01" then
      rx_expected_parity <= rx_parity_calc;
    else
      rx_expected_parity <= not rx_parity_calc;
    end if;
  end process;
  
  -- Generated from always process in nexus_uart (../../duts/nexus_uart/nexus_uart.sv:474)
  process (clk_i, rst_ni) is
  begin
    if (not rst_ni) = '1' then
      rx_baud_cnt <= X"0000";
    elsif rising_edge(clk_i) then
      if (rx_state = X"0") or (rx_os_tick = '1') then
        rx_baud_cnt <= X"0000";
      else
        rx_baud_cnt <= rx_baud_cnt + X"0001";
      end if;
    end if;
  end process;
  
  -- Generated from always process in nexus_uart (../../duts/nexus_uart/nexus_uart.sv:492)
  process (clk_i, rst_ni) is
  begin
    if (not rst_ni) = '1' then
      rx_state <= X"0";
      rx_os_cnt <= X"0";
      rx_bit_idx <= "000";
      rx_shift_reg <= X"00";
      rx_parity_calc <= '0';
      rx_push_valid <= '0';
      rx_push_data <= X"00";
      rx_parity_error_det <= '0';
      rx_frame_error_det <= '0';
      rx_overflow_det <= '0';
      rx_active <= '0';
    elsif rising_edge(clk_i) then
      rx_push_valid <= '0';
      rx_parity_error_det <= '0';
      rx_frame_error_det <= '0';
      rx_overflow_det <= '0';
      case rx_state is
        when X"0" =>
          rx_os_cnt <= X"0";
          rx_bit_idx <= "000";
          rx_active <= '0';
          if (ctrl_rx_enable = '1') and (rx_serial_qq = '0') then
            rx_state <= X"1";
            rx_os_cnt <= X"1";
            rx_active <= '1';
          end if;
        when X"1" =>
          if rx_os_tick = '1' then
            rx_os_cnt <= rx_os_cnt + X"1";
            if rx_os_cnt = X"8" then
              if rx_serial_qq = '0' then
                rx_state <= X"2";
                rx_os_cnt <= X"0";
                rx_bit_idx <= "000";
                rx_parity_calc <= '0';
              else
                rx_state <= X"0";
              end if;
            end if;
          end if;
        when X"2" =>
          if rx_os_tick = '1' then
            rx_os_cnt <= rx_os_cnt + X"1";
            if rx_os_cnt = X"f" then
              rx_os_cnt <= X"0";
            end if;
            if rx_os_cnt = X"8" then
              rx_shift_reg(To_Integer(rx_bit_idx)) <= rx_serial_qq;
              rx_parity_calc <= rx_parity_calc xor rx_serial_qq;
              if rx_bit_idx = "111" then
                if ctrl_parity_mode /= "00" then
                  rx_state <= X"3";
                else
                  rx_state <= X"4";
                end if;
                rx_os_cnt <= X"0";
              else
                rx_bit_idx <= rx_bit_idx + "001";
              end if;
            end if;
          end if;
        when X"3" =>
          if rx_os_tick = '1' then
            rx_os_cnt <= rx_os_cnt + X"1";
            if rx_os_cnt = X"f" then
              rx_os_cnt <= X"0";
            end if;
            if rx_os_cnt = X"8" then
              if rx_serial_qq /= rx_expected_parity then
                rx_parity_error_det <= '1';
              end if;
              rx_state <= X"4";
              rx_os_cnt <= X"0";
            end if;
          end if;
        when X"4" =>
          if rx_os_tick = '1' then
            rx_os_cnt <= rx_os_cnt + X"1";
            if rx_os_cnt = X"f" then
              rx_os_cnt <= X"0";
            end if;
            if rx_os_cnt = X"8" then
              if rx_serial_qq = '0' then
                rx_frame_error_det <= '1';
              end if;
              if ctrl_stop_bits = '1' then
                rx_state <= X"5";
                rx_os_cnt <= X"0";
              else
                if rx_fifo_full = '1' then
                  rx_overflow_det <= '1';
                else
                  rx_push_valid <= '1';
                  rx_push_data <= rx_shift_reg;
                end if;
                rx_state <= X"0";
              end if;
            end if;
          end if;
        when X"5" =>
          if rx_os_tick = '1' then
            rx_os_cnt <= rx_os_cnt + X"1";
            if rx_os_cnt = X"f" then
              rx_os_cnt <= X"0";
            end if;
            if rx_os_cnt = X"8" then
              if rx_serial_qq = '0' then
                rx_frame_error_det <= '1';
              end if;
              if rx_fifo_full = '1' then
                rx_overflow_det <= '1';
              else
                rx_push_valid <= '1';
                rx_push_data <= rx_shift_reg;
              end if;
              rx_state <= X"0";
            end if;
          end if;
        when others =>
          rx_state <= X"0";
      end case;
    end if;
  end process;
  
  -- Generated from always process in nexus_uart (../../duts/nexus_uart/nexus_uart.sv:632)
  process (clk_i, rst_ni) is
  begin
    if (not rst_ni) = '1' then
      rx_overrun_sticky <= '0';
      rx_parity_err_sticky <= '0';
      rx_frame_err_sticky <= '0';
    elsif rising_edge(clk_i) then
      if fifo_ctrl_rx_rst = '1' then
        rx_overrun_sticky <= '0';
        rx_parity_err_sticky <= '0';
        rx_frame_err_sticky <= '0';
      else
        if rx_overflow_det = '1' then
          rx_overrun_sticky <= '1';
        end if;
        if rx_parity_error_det = '1' then
          rx_parity_err_sticky <= '1';
        end if;
        if rx_frame_error_det = '1' then
          rx_frame_err_sticky <= '1';
        end if;
      end if;
    end if;
  end process;
  
  -- Generated from always process in nexus_uart (../../duts/nexus_uart/nexus_uart.sv:650)
  process (ctrl_baud_divisor) is
  begin
    rx_timeout_thresh <= Resize((X"0000" & ctrl_baud_divisor) * X"00000010", 32);
  end process;
  
  -- Generated from always process in nexus_uart (../../duts/nexus_uart/nexus_uart.sv:654)
  process (clk_i, rst_ni) is
  begin
    if (not rst_ni) = '1' then
      rx_timeout_cnt <= X"00000000";
      rx_timeout_event <= '0';
    elsif rising_edge(clk_i) then
      rx_timeout_event <= '0';
      if (rx_active = '1') or (rx_state /= X"0") then
        rx_timeout_cnt <= X"00000000";
      else
        if ((not rx_fifo_empty) = '1') and (ctrl_rx_enable = '1') then
          if rx_serial_qq = '1' then
            if rx_timeout_cnt < rx_timeout_thresh then
              rx_timeout_cnt <= rx_timeout_cnt + X"00000001";
            else
              rx_timeout_event <= '1';
            end if;
          else
            rx_timeout_cnt <= X"00000000";
          end if;
        else
          rx_timeout_cnt <= X"00000000";
        end if;
      end if;
    end if;
  end process;
  
  -- Generated from always process in nexus_uart (../../duts/nexus_uart/nexus_uart.sv:678)
  process (tx_fifo_level, fifo_ctrl_tx_watermark, ctrl_tx_enable, rx_fifo_level, fifo_ctrl_rx_watermark, ctrl_rx_enable, tx_fifo_empty, rx_overflow_det, rx_frame_error_det, rx_parity_error_det, rx_timeout_event) is
  begin
    intr_hw_set <= "0000000";
    if (tx_fifo_level <= ('0' & fifo_ctrl_tx_watermark)) and (ctrl_tx_enable = '1') then
      intr_hw_set(0) <= '1';
    end if;
    if (rx_fifo_level >= ('0' & fifo_ctrl_rx_watermark)) and (ctrl_rx_enable = '1') then
      intr_hw_set(1) <= '1';
    end if;
    if (tx_fifo_empty = '1') and (ctrl_tx_enable = '1') then
      intr_hw_set(2) <= '1';
    end if;
    if rx_overflow_det = '1' then
      intr_hw_set(3) <= '1';
    end if;
    if rx_frame_error_det = '1' then
      intr_hw_set(4) <= '1';
    end if;
    if rx_parity_error_det = '1' then
      intr_hw_set(5) <= '1';
    end if;
    if rx_timeout_event = '1' then
      intr_hw_set(6) <= '1';
    end if;
  end process;
end architecture;

