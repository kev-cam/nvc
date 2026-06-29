library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

entity churn is
  port (clk, rst_l : in std_logic; y : out std_logic_vector(31 downto 0));
end entity;

architecture rtl of churn is
  signal s0, s1, s2, s3, s4, s5, s6, s7, s8, s9, s10, s11, s12, s13, s14, s15, s16, s17, s18, s19, s20, s21, s22, s23, s24, s25, s26, s27, s28, s29, s30, s31 : unsigned(31 downto 0) := (others => '0');
  signal cnt : unsigned(31 downto 0) := (others => '0');
begin
  process (clk) is
  begin
    if rising_edge(clk) then
      if rst_l = '0' then
        cnt <= (others => '0');
        s0 <= (others => '0');
        s1 <= (others => '0');
        s2 <= (others => '0');
        s3 <= (others => '0');
        s4 <= (others => '0');
        s5 <= (others => '0');
        s6 <= (others => '0');
        s7 <= (others => '0');
        s8 <= (others => '0');
        s9 <= (others => '0');
        s10 <= (others => '0');
        s11 <= (others => '0');
        s12 <= (others => '0');
        s13 <= (others => '0');
        s14 <= (others => '0');
        s15 <= (others => '0');
        s16 <= (others => '0');
        s17 <= (others => '0');
        s18 <= (others => '0');
        s19 <= (others => '0');
        s20 <= (others => '0');
        s21 <= (others => '0');
        s22 <= (others => '0');
        s23 <= (others => '0');
        s24 <= (others => '0');
        s25 <= (others => '0');
        s26 <= (others => '0');
        s27 <= (others => '0');
        s28 <= (others => '0');
        s29 <= (others => '0');
        s30 <= (others => '0');
        s31 <= (others => '0');
      else
        cnt <= cnt + 1;
        s0 <= s1 xor ((s0(24 downto 0) & s0(31 downto 25)) + cnt);
        s1 <= s2 xor ((s1(18 downto 0) & s1(31 downto 19)) + s8);
        s2 <= s3 xor ((s2(14 downto 0) & s2(31 downto 15)) + s9);
        s3 <= s4 xor ((s3(26 downto 0) & s3(31 downto 27)) + s10);
        s4 <= s5 xor ((s4(10 downto 0) & s4(31 downto 11)) + s11);
        s5 <= s6 xor ((s5(22 downto 0) & s5(31 downto 23)) + cnt);
        s6 <= s7 xor ((s6(28 downto 0) & s6(31 downto 29)) + s13);
        s7 <= s8 xor ((s7(20 downto 0) & s7(31 downto 21)) + s14);
        s8 <= s9 xor ((s8(12 downto 0) & s8(31 downto 13)) + s15);
        s9 <= s10 xor ((s9(6 downto 0) & s9(31 downto 7)) + s16);
        s10 <= s11 xor ((s10(30 downto 0) & s10(31 downto 31)) + cnt);
        s11 <= s12 xor ((s11(16 downto 0) & s11(31 downto 17)) + s18);
        s12 <= s13 xor ((s12(8 downto 0) & s12(31 downto 9)) + s19);
        s13 <= s14 xor ((s13(2 downto 0) & s13(31 downto 3)) + s20);
        s14 <= s15 xor ((s14(29 downto 0) & s14(31 downto 30)) + s21);
        s15 <= s16 xor ((s15(4 downto 0) & s15(31 downto 5)) + cnt);
        s16 <= s17 xor ((s16(24 downto 0) & s16(31 downto 25)) + s23);
        s17 <= s18 xor ((s17(18 downto 0) & s17(31 downto 19)) + s24);
        s18 <= s19 xor ((s18(14 downto 0) & s18(31 downto 15)) + s25);
        s19 <= s20 xor ((s19(26 downto 0) & s19(31 downto 27)) + s26);
        s20 <= s21 xor ((s20(10 downto 0) & s20(31 downto 11)) + cnt);
        s21 <= s22 xor ((s21(22 downto 0) & s21(31 downto 23)) + s28);
        s22 <= s23 xor ((s22(28 downto 0) & s22(31 downto 29)) + s29);
        s23 <= s24 xor ((s23(20 downto 0) & s23(31 downto 21)) + s30);
        s24 <= s25 xor ((s24(12 downto 0) & s24(31 downto 13)) + s31);
        s25 <= s26 xor ((s25(6 downto 0) & s25(31 downto 7)) + cnt);
        s26 <= s27 xor ((s26(30 downto 0) & s26(31 downto 31)) + s1);
        s27 <= s28 xor ((s27(16 downto 0) & s27(31 downto 17)) + s2);
        s28 <= s29 xor ((s28(8 downto 0) & s28(31 downto 9)) + s3);
        s29 <= s30 xor ((s29(2 downto 0) & s29(31 downto 3)) + s4);
        s30 <= s31 xor ((s30(29 downto 0) & s30(31 downto 30)) + cnt);
        s31 <= s0 xor ((s31(4 downto 0) & s31(31 downto 5)) + s6);
      end if;
    end if;
  end process;
  y <= std_logic_vector(s0 xor s1 xor s2 xor s3 xor s4 xor s5 xor s6 xor s7 xor s8 xor s9 xor s10 xor s11 xor s12 xor s13 xor s14 xor s15 xor s16 xor s17 xor s18 xor s19 xor s20 xor s21 xor s22 xor s23 xor s24 xor s25 xor s26 xor s27 xor s28 xor s29 xor s30 xor s31);
end architecture;
