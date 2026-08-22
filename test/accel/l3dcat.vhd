-- A scalar l3d_bit_read used as a CONCATENATION ELEMENT -- the one context in
-- Verilog (along with a replication operand) that does NOT resize its parts.
--
-- This is the register-file write-mask idiom sv2vhdl emits for every VeeR GPR
-- write: an N-wide mask built by concatenating N copies of ONE scalar bit, then
-- AND'ed with the write datum.  vhdl2vlog used to render l3d_bit_read as
-- `((a >> i) & 1'b1)`, whose Verilog SELF-DETERMINED width is width(a), not 1.
-- Each element then occupies width(a) concat slots instead of one, so bit k
-- lands at Verilog bit k*width(a) and the intended all-ones mask degenerates to
-- a comb.  Here width(v_wv) = 4 and the mask has 8 elements, so the surviving
-- 8-bit mask is 8'h11: writes came back as (data and 16#11#) -- 0xEE -> 0x00,
-- 0x55 -> 0x11, 0xFF -> 0x11.  In eh2_dec_gpr_ctl the base is 31 bits wide and
-- the same idiom yielded mask 32'h80000001.
--
-- Nothing else caught this: yosys reads the shredded Verilog with zero errors
-- (the surplus bits are undriven and get optimised away) and the synthesised
-- cell counts are unchanged, so the chunk installs and is silently wrong.
--
-- Deliberately tiny -- one 4x8 register bank -- so it synthesises in a second.
library ieee; use ieee.std_logic_1164.all; use ieee.numeric_std.all;
library sv2vhdl; use sv2vhdl.logic3d_types_pkg.all;

entity l3dcat is
  port (clk   : in  std_logic;
        wen   : in  logic3d;
        waddr : in  logic3d_vector(1 downto 0);
        wdata : in  logic3d_vector(7 downto 0);
        raddr : in  logic3d_vector(1 downto 0);
        rdata : out logic3d_vector(7 downto 0));
end entity;

architecture rtl of l3dcat is
  signal bank_in  : logic3d_vector(31 downto 0) := (others => L3D_0);
  signal bank_out : logic3d_vector(31 downto 0) := (others => L3D_0);
  signal rd_reg   : logic3d_vector(7 downto 0)  := (others => L3D_0);
begin
  -- WRITE: mask = {8 copies of a SCALAR bit} and wdata.
  wr : process (all) is
    variable v_wv  : logic3d_vector(3 downto 0);
    variable v_bin : logic3d_vector(31 downto 0);
    variable tmp   : logic3d_vector(7 downto 0);
    variable j     : integer;
  begin
    v_wv  := (others => L3D_0);
    v_bin := bank_out;
    j := 0;
    while j < 4 loop
      if l3d_eq1(waddr, to_l3d(j, 2)) then
        v_wv(j) := wen;
      else
        v_wv(j) := L3D_0;
      end if;
      j := j + 1;
    end loop;
    j := 0;
    while j < 4 loop
      tmp := l3d_and(l3d_bit_read(v_wv, l3d_index(to_l3d(j, 3), True))
                   & l3d_bit_read(v_wv, l3d_index(to_l3d(j, 3), True))
                   & l3d_bit_read(v_wv, l3d_index(to_l3d(j, 3), True))
                   & l3d_bit_read(v_wv, l3d_index(to_l3d(j, 3), True))
                   & l3d_bit_read(v_wv, l3d_index(to_l3d(j, 3), True))
                   & l3d_bit_read(v_wv, l3d_index(to_l3d(j, 3), True))
                   & l3d_bit_read(v_wv, l3d_index(to_l3d(j, 3), True))
                   & l3d_bit_read(v_wv, l3d_index(to_l3d(j, 3), True)), wdata);
      for p in 0 to 7 loop
        if l3d_eq1(l3d_bit_read(v_wv, j), L3D_1) then
          v_bin(j * 8 + p) := tmp(p);
        end if;
      end loop;
      j := j + 1;
    end loop;
    bank_in <= v_bin;
  end process;

  ff : process (clk) is
  begin
    if rising_edge(clk) then
      bank_out <= bank_in;
      rd_reg <= l3d_part_read(bank_out, l3d_index(to_l3d(raddr, 32), False) * 8, 8);
    end if;
  end process;

  rdata <= rd_reg;
end architecture;
