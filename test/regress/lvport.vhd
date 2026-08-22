entity lvsub is
  port ( p : in integer );
end entity;
architecture a of lvsub is
begin
  process (p) begin
    if p = 3 then
      assert p'last_value = 2 report "port 'last_value wrong: " &
        integer'image(p'last_value) severity failure;
      report "LVPORT PASS";
    end if;
  end process;
end architecture;

entity lvport is end entity;
architecture a of lvport is
  signal x : integer := 0;   -- outer driver; only the INNER unit reads 'last_value
begin
  u: entity work.lvsub port map ( p => x );
  process begin
    x <= 1; wait for 1 ns;
    x <= 2; wait for 1 ns;
    x <= 3; wait for 1 ns;
    wait;
  end process;
end architecture;
