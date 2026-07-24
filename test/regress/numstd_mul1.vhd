-- Differential test for the numeric_std multiply fast path: exercises widths
-- either side of the size<=64 boundary (size = la+lb), odd (non-multiple-of-8)
-- widths, and signed negatives including the most-negative value. Every
-- product is folded into a checksum so our-nvc and stock-nvc compare exactly.
library ieee; use ieee.std_logic_1164.all; use ieee.numeric_std.all;
use std.env.stop; use std.textio.all;
entity numstd_mul1 is end entity;
architecture t of numstd_mul1 is
  signal dummy : bit := '0';
begin
  process
    variable l : line;
    variable acc : unsigned(63 downto 0) := (others => '0');
    procedure fold(v : std_logic_vector) is begin
      for i in v'range loop
        acc := acc(62 downto 0) & acc(63);
        if v(i) = '1' then acc(0) := not acc(0); end if;
      end loop;
    end procedure;
    variable av, bv : natural;
  begin
    -- unsigned: la+lb sweeps from 2 to 80, crossing the 64-bit fast-path edge
    for la in 1 to 40 loop
      for lb in 1 to 40 loop
        for k in 0 to 2 loop
          av := (k * 2654435 + la * 97 + 1) mod 2**minimum(la, 30);
          bv := (k * 40503   + lb * 31 + 1) mod 2**minimum(lb, 30);
          fold(std_logic_vector(to_unsigned(av, la) * to_unsigned(bv, lb)));
        end loop;
      end loop;
    end loop;
    -- signed: negatives, mixed signs, and the most-negative value
    for la in 2 to 31 loop
      for lb in 2 to 31 loop
        for k in -2 to 2 loop
          fold(std_logic_vector(to_signed(k * la, la) * to_signed(-k * lb - 1, lb)));
        end loop;
        fold(std_logic_vector(to_signed(-(2**(la-1)), la) *
                              to_signed(-(2**(lb-1)), lb)));
        fold(std_logic_vector(to_signed(-(2**(la-1)), la) * to_signed(-1, lb)));
        fold(std_logic_vector(to_signed(2**(la-1)-1, la) *
                              to_signed(-(2**(lb-1)), lb)));
      end loop;
    end loop;
    -- Expected value is NOT self-baselined: stock nvc 1.22.0 and ghdl 5.0
    -- independently produce this same checksum for this stimulus.
    write(l, string'("numstd_mul1 CHK=")); hwrite(l, std_logic_vector(acc)); writeline(output, l);
    assert acc = x"BDD5F98B68C3727A"
      report "numeric_std multiply checksum mismatch" severity failure;
    stop;
  end process;
end architecture;
