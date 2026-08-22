-- Like churn_tb but with M extra clk-only "sampler" processes, each a plain
-- process(clk)/rising_edge that xors y into its own accumulator. Every sampler
-- is clk-ONLY, so NVC_FAST_CLK pulls all M of them out of the proc queue and
-- runs them from the flat posedge table. Sweeping M shows the per-proc dispatch
-- saving (the whole point of the table) that a single-DUT testbench hides.
library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

entity churn_many_tb is
  generic (N : integer := 1000000; M : integer := 64);
end entity;

architecture sim of churn_many_tb is
  signal clk, rst_l : std_logic := '0';
  signal y          : std_logic_vector(31 downto 0);
  signal running    : boolean := true;
  type acc_t is array (0 to M-1) of unsigned(31 downto 0);
  signal mon : acc_t := (others => (others => '0'));
  signal chk : unsigned(31 downto 0) := (others => '0');
begin
  dut : entity work.churn
    port map (clk => clk, rst_l => rst_l, y => y);

  clk <= not clk after 5 ns when running else '0';

  -- M clk-only samplers (the fan-out the posedge table accelerates)
  gen : for i in 0 to M-1 generate
    process (clk) is
    begin
      if rising_edge(clk) then
        if rst_l = '1' then mon(i) <= mon(i) + unsigned(y) + to_unsigned(i + 1, 32); end if;
      end if;
    end process;
  end generate;

  -- fold the samplers into one checksum (clk-only as well)
  process (clk) is
    variable acc : unsigned(31 downto 0);
  begin
    if rising_edge(clk) then
      if rst_l = '1' then
        acc := (others => '0');
        for i in 0 to M-1 loop acc := acc xor mon(i); end loop;
        chk <= acc;
      end if;
    end if;
  end process;

  stim : process is
  begin
    rst_l <= '0';
    wait for 23 ns;
    rst_l <= '1';
    for i in 1 to N loop
      wait until rising_edge(clk);
    end loop;
    wait for 1 ns;
    report "Y=" & integer'image(to_integer(chk(30 downto 0)));
    report "PASSED";
    running <= false;
    wait;
  end process;
end architecture;
