-- Minimal reproducer for SEGV in sched_driver / async_transfer_signal
-- Concurrent assignments with combinational feedback through a clocked process
--
-- This is legal VHDL: the feedback loop breaks at the clock edge.
-- NVC crashes during the first delta cycle of simulation.

library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

entity crash_dut is
  port (
    clk_i   : in  std_logic;
    rst_ni  : in  std_logic;
    valid_i : in  std_logic;
    ready_o : out std_logic;
    data_i  : in  unsigned(31 downto 0);
    ack_o   : out std_logic
  );
end entity;

architecture rtl of crash_dut is
  signal pending    : std_logic;
  signal req_valid  : std_logic;
  signal ready_reg  : std_logic;

  function Bool_To_Logic(B : Boolean) return std_logic is
  begin
    if B then return '1'; else return '0'; end if;
  end function;
begin
  -- Concurrent assignments creating feedback path:
  --   ready_reg depends on pending
  --   req_valid depends on ready_reg
  --   pending (clocked) depends on req_valid
  ready_o   <= ready_reg;
  ready_reg <= Bool_To_Logic(pending = '0' or rst_ni = '0');
  req_valid <= valid_i and ready_reg;

  process (clk_i, rst_ni) is
  begin
    if rst_ni = '0' then
      pending <= '0';
      ack_o   <= '0';
    elsif rising_edge(clk_i) then
      if pending = '1' then
        pending <= '0';
        ack_o   <= '0';
      end if;
      if req_valid = '1' then
        pending <= '1';
        ack_o   <= '1';
      end if;
    end if;
  end process;
end architecture;

-- Testbench
library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

entity tb_crash is
end entity;

architecture test of tb_crash is
  signal clk   : std_logic := '0';
  signal rst_n  : std_logic := '0';
  signal valid  : std_logic := '0';
  signal ready  : std_logic;
  signal data   : unsigned(31 downto 0) := (others => '0');
  signal ack    : std_logic;
begin
  clk <= not clk after 5 ns;

  dut: entity work.crash_dut
    port map (
      clk_i => clk, rst_ni => rst_n,
      valid_i => valid, ready_o => ready,
      data_i => data, ack_o => ack
    );

  process
  begin
    rst_n <= '0';
    wait for 50 ns;
    rst_n <= '1';
    wait for 20 ns;
    valid <= '1';
    wait until rising_edge(clk);
    wait until rising_edge(clk);
    valid <= '0';
    wait for 50 ns;
    report "PASSED" severity note;
    std.env.finish;
  end process;
end architecture;
