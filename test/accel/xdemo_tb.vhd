-- Positive test for the accel X/Z fallback.
--
--   * cycles 1..XAT-1 : en='1', d=x"5A"  -- ordinary 2-state traffic
--   * XAT..XAT+2      : en='0', d='X'    -- an UNCERTAIN byte crosses the
--                                           accel boundary; nothing samples it
--   * XAT+3..N        : en='1', d=x"5A"  -- traffic resumes
--
-- Y is therefore identical under the interpreter, under accel, and under
-- accel-then-demote: the run must continue correctly whichever engine finishes
-- it. Run it three ways and diff Y:
--   NVC_ACCEL=0                              -> interpreted reference
--   NVC_ACCEL_JIT=1                          -> accel, detection only
--   NVC_ACCEL_JIT=1 NVC_ACCEL_XDEMOTE=1      -> accel until the X, then interp
library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

entity xdemo_tb is
  -- XVAL selects which uncertain value crosses the boundary; override at
  -- elaboration with -gXVAL=Z to prove the detector is not X-specific.
  generic (N : integer := 20000; XAT : integer := 2000;
           XVAL : std_logic := 'X');
end entity;

architecture sim of xdemo_tb is
  signal clk, rst_l, en : std_logic := '0';
  signal d              : std_logic_vector(7 downto 0) := x"5A";
  signal y              : std_logic_vector(31 downto 0);
  signal chk            : unsigned(31 downto 0) := (others => '0');
  signal running        : boolean := true;
begin
  dut : entity work.xdemo
    port map (clk => clk, rst_l => rst_l, en => en, d => d, y => y);

  clk <= not clk after 5 ns when running else '0';

  process (clk) is
  begin
    if rising_edge(clk) then
      if rst_l = '1' then chk <= chk xor unsigned(y); end if;
    end if;
  end process;

  stim : process is
  begin
    rst_l <= '0';
    en    <= '1';
    wait for 23 ns;
    rst_l <= '1';
    for i in 1 to N loop
      wait until rising_edge(clk);
      if i = XAT then
        -- drive the boundary uncertain, with capture disabled
        en <= '0';
        d  <= (others => XVAL);
        report "XDRIVE: d <= '" & std_logic'image(XVAL)
             & "' at cycle " & integer'image(i);
      elsif i = XAT + 3 then
        d  <= x"5A";
        en <= '1';
      end if;
    end loop;
    wait for 1 ns;
    report "Y=" & integer'image(to_integer(chk(30 downto 0)));
    report "PASSED";
    running <= false;
    wait;
  end process;
end architecture;
