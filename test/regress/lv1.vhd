entity lv1 is end entity;
architecture a of lv1 is
  signal s : integer := 0;          -- 'last_value read below → registered
  signal t : integer := 0;          -- never read → plane elided
  signal v : bit_vector(31 downto 0) := (others => '0');  -- vector, registered
begin
  process begin
    s <= 1; t <= 1; v <= x"0000_0001"; wait for 1 ns;
    s <= 2; t <= 2; v <= x"0000_0002"; wait for 1 ns;
    s <= 3; t <= 3; v <= x"0000_0003"; wait for 1 ns;
    assert s'last_value = 2 report "s'last_value wrong: " &
      integer'image(s'last_value) severity failure;
    assert v'last_value = x"0000_0002" report "v'last_value wrong"
      severity failure;
    assert t = 3 severity failure;
    report "LV1 PASS";
    wait;
  end process;
end architecture;
