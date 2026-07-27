-- SLICE / CONCAT / REPLICATION / bit-reverse probe (signal-only, no subprograms
-- or process variables, mirroring wchurn's shape so it INSTALLS).
-- Everything is one combinational concat expression fed into a clocked register.
-- Stresses sig_expr multi-chunk concat (reordered non-aligned slices),
-- replication ((others=>bit)), and explicit bit-reverse.
library ieee; use ieee.std_logic_1164.all; use ieee.numeric_std.all;
entity slcat is
  port (clk, rst_l : in std_logic;
        din  : in  std_logic_vector(31 downto 0);
        y    : out std_logic_vector(31 downto 0));
end entity;
architecture rtl of slcat is
  signal acc : unsigned(31 downto 0) := (others => '0');
  signal x   : std_logic_vector(31 downto 0);
  -- reordered non-aligned slice concat, exactly 32 bits:
  --   sfb=x(20:11)[10b] | revlo=rev(x(15:0))(7:0)[8b] | sfc=x(30:28)[3b]
  --   | rep=(others=>x(23))[6b] | sfa=x(7:3)[5b]
  signal cat : std_logic_vector(31 downto 0);
begin
  x <= din xor std_logic_vector(acc);

  -- bit-reverse of x(15 downto 0) is written out explicitly as a 16-wide concat,
  -- then its low 8 bits go into the field concat. Reversed low-8 of x(15:0) is
  -- x(8),x(9),...,x(15) i.e. bits 8..15 in ascending order.
  cat <= x(20 downto 11)                              -- [31:22]
       & (x(8) & x(9) & x(10) & x(11)                 -- [21:14]  rev(x(15:0))(7:0)
          & x(12) & x(13) & x(14) & x(15))
       & x(30 downto 28)                              -- [13:11]
       & (x(23) & x(23) & x(23) & x(23) & x(23) & x(23))  -- [10:5] replication
       & x(7 downto 3);                               -- [4:0]

  process (clk) is begin
    if rising_edge(clk) then
      if rst_l = '0' then
        acc <= (others => '0');
      else
        acc <= unsigned(cat) + acc;
      end if;
    end if;
  end process;
  y <= std_logic_vector(acc);
end architecture;
