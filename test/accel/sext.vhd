-- Sign-extended mixed-width signed arithmetic:
--  a small signed 8-bit field (often negative) is resized (sign-extended) to 32
--  and ADDED into a signed 32-bit accumulator; a second small signed field is
--  resized and SUBTRACTED. This is exactly the "immediate sign-extend before add"
--  path an instruction-decode datapath uses. Single clocked entity -> installs.
library ieee; use ieee.std_logic_1164.all; use ieee.numeric_std.all;
entity sext is
  port (clk, rst_l : in std_logic;
        imm8  : in std_logic_vector(7 downto 0);   -- small signed immediate
        imm12 : in std_logic_vector(11 downto 0);  -- second small signed field
        y     : out std_logic_vector(31 downto 0));
end entity;
architecture rtl of sext is
  signal acc : signed(31 downto 0) := (others => '0');
begin
  process (clk) is begin
    if rising_edge(clk) then
      if rst_l = '0' then
        acc <= (others => '0');
      else
        -- sign-extend imm8 (8b) and imm12 (12b) to 32b, add one, subtract other
        acc <= acc + resize(signed(imm8), 32) - resize(signed(imm12), 32);
      end if;
    end if;
  end process;
  y <= std_logic_vector(acc);
end architecture;
