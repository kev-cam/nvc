-- Probe: a 128b register whose next value is a CONCAT of four driven 32b words
-- (destination scatter across 4 limbs), then each limb read back via a slice and
-- folded into acc (source gather). Mirrors dec's flush-cone shape: concat/repl
-- into a wide reg + wide-bitwise + slice readback. Accel must match interp.
library ieee; use ieee.std_logic_1164.all; use ieee.numeric_std.all;
entity wscat is
  port (clk, rst_l : in std_logic; y : out std_logic_vector(31 downto 0));
end entity;
architecture rtl of wscat is
  signal a,b,c,d : unsigned(31 downto 0) := (others=>'0');
  signal wreg    : std_logic_vector(127 downto 0) := (others=>'0');
  signal mask    : std_logic_vector(127 downto 0) := (others=>'0');
  signal acc     : unsigned(31 downto 0) := (others=>'0');
begin
  process (clk) is
    variable folded : std_logic_vector(127 downto 0);
  begin
    if rising_edge(clk) then
      if rst_l='0' then
        a<=(others=>'0');b<=(others=>'0');c<=(others=>'0');d<=(others=>'0');
        wreg<=(others=>'0');mask<=(others=>'0');acc<=(others=>'0');
      else
        a<=a+1; b<=b+3; c<=c+7; d<=d+13;
        -- concat scatter of four 32b words into a 128b reg
        wreg <= std_logic_vector(a) & std_logic_vector(b) & std_logic_vector(c) & std_logic_vector(d);
        -- replication into a wide mask, then wide bitwise AND (wplaceb/wslice cone)
        mask <= (others => a(0));
        folded := wreg and mask;
        -- gather each limb back via slices, fold into acc
        acc <= acc
             + unsigned(folded(31 downto 0))
             + unsigned(folded(63 downto 32))
             + unsigned(folded(95 downto 64))
             + unsigned(folded(127 downto 96));
      end if;
    end if;
  end process;
  y <= std_logic_vector(acc);
end architecture;
