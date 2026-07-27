-- X/Z accel-fallback test DUT (NVC_ACCEL_XDEMOTE).
--
-- Plain 2-state datapath, but its `d` input is captured only while `en` is
-- high. The testbench drives `d` to 'X' during an en-low window: nothing
-- captures the X, so the interpreted reference and the accelerated run agree
-- bit-for-bit — yet the accel bridge's boundary scan SEES the uncertain byte
-- and can ask to be handed back to the interpreter.
library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

entity xdemo is
  port (clk, rst_l, en : in std_logic;
        d              : in std_logic_vector(7 downto 0);
        y              : out std_logic_vector(31 downto 0));
end entity;

architecture rtl of xdemo is
  signal acc : unsigned(31 downto 0) := (others => '0');
  signal cnt : unsigned(31 downto 0) := (others => '0');
begin
  process (clk) is
  begin
    if rising_edge(clk) then
      if rst_l = '0' then
        acc <= (others => '0');
        cnt <= (others => '0');
      else
        cnt <= cnt + 1;
        if en = '1' then
          acc <= (acc(26 downto 0) & acc(31 downto 27)) + unsigned(d) + cnt;
        end if;
      end if;
    end if;
  end process;

  y <= std_logic_vector(acc xor cnt);
end architecture;
