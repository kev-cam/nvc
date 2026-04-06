library ieee; use ieee.std_logic_1164.all; use ieee.numeric_std.all;
library sv2vhdl; use sv2vhdl.sv_display_pkg.all; use sv2vhdl.sv_analog_pkg.all;
entity tb_nvc is end entity;
architecture test of tb_nvc is
  signal clk : std_logic := '0'; signal rst_n : std_logic := '0';
  signal tl_a_valid, tl_a_ready, tl_d_valid, tl_d_ready, tl_d_error : std_logic := '0';
  signal tl_a_opcode, tl_d_opcode : unsigned(2 downto 0) := "000";
  signal tl_a_address, tl_a_data, tl_d_data : unsigned(31 downto 0) := (others => '0');
  signal tl_a_mask : unsigned(3 downto 0) := "1111";
  signal tl_a_source, tl_d_source : unsigned(7 downto 0) := (others => '0');
  signal tl_a_size : unsigned(1 downto 0) := "10";
begin
  clk <= not clk after 5 ns;
  dut: entity work.nexus_uart port map (
    clk_i=>clk, rst_ni=>rst_n, tl_a_valid_i=>tl_a_valid, tl_a_ready_o=>tl_a_ready,
    tl_a_opcode_i=>tl_a_opcode, tl_a_address_i=>tl_a_address, tl_a_data_i=>tl_a_data,
    tl_a_mask_i=>tl_a_mask, tl_a_source_i=>tl_a_source, tl_a_size_i=>tl_a_size,
    tl_d_valid_o=>tl_d_valid, tl_d_ready_i=>tl_d_ready, tl_d_opcode_o=>tl_d_opcode,
    tl_d_data_o=>tl_d_data, tl_d_source_o=>tl_d_source, tl_d_error_o=>tl_d_error,
    uart_rx_i => '1');
  process begin
    rst_n <= '0'; wait for 50 ns; rst_n <= '1'; wait for 20 ns;
    -- Write CTRL register
    tl_d_ready<='1'; tl_a_valid<='1'; tl_a_opcode<="000"; tl_a_address<=x"00000000"; tl_a_data<=x"00000001";
    wait until rising_edge(clk); wait until rising_edge(clk); tl_a_valid<='0'; wait until rising_edge(clk); tl_d_ready<='0';
    -- Write reg at offset 4
    tl_d_ready<='1'; tl_a_valid<='1'; tl_a_opcode<="000"; tl_a_address<=x"00000004"; tl_a_data<=x"AAAAAAAA";
    wait until rising_edge(clk); wait until rising_edge(clk); tl_a_valid<='0'; wait until rising_edge(clk); tl_d_ready<='0';
    -- Read CTRL
    tl_d_ready<='1'; tl_a_valid<='1'; tl_a_opcode<="100"; tl_a_address<=x"00000000"; tl_a_data<=x"00000000";
    wait until rising_edge(clk); wait until rising_edge(clk); tl_a_valid<='0'; wait until rising_edge(clk); tl_d_ready<='0';
    -- Read reg at offset 4
    tl_d_ready<='1'; tl_a_valid<='1'; tl_a_opcode<="100"; tl_a_address<=x"00000004"; tl_a_data<=x"00000000";
    wait until rising_edge(clk); wait until rising_edge(clk); tl_a_valid<='0'; wait until rising_edge(clk); tl_d_ready<='0';
    wait for 200 ns; report "done" severity note; std.env.finish;
  end process;
end architecture;
