-- SystemVerilog Multi-input and Multi-output Gates
-- IEEE 1800 Sections 28.4 and 28.5
--
-- Ports use std_logic for external compatibility.
-- Architectures use logic3d LUT functions for enhanced X-propagation.

---------------------------------------------------------------------------
-- AND Gate: N-input AND
---------------------------------------------------------------------------
library ieee;
use ieee.std_logic_1164.all;
use work.logic3d_types_pkg.all;

entity sv_and is
    generic (n : positive := 2);
    port (
        y : out std_logic;
        a : in  std_logic_vector(0 to n-1)
    );
end entity sv_and;

architecture behavioral of sv_and is
begin
    process (a)
        variable result : logic3d;
    begin
        result := L3D_1;
        for i in a'range loop
            result := l3d_and(result, to_logic3d(a(i)));
        end loop;
        y <= to_std_logic(result);
    end process;
end architecture behavioral;

---------------------------------------------------------------------------
-- NAND Gate: N-input NAND
---------------------------------------------------------------------------
library ieee;
use ieee.std_logic_1164.all;
use work.logic3d_types_pkg.all;

entity sv_nand is
    generic (n : positive := 2);
    port (
        y : out std_logic;
        a : in  std_logic_vector(0 to n-1)
    );
end entity sv_nand;

architecture behavioral of sv_nand is
begin
    process (a)
        variable result : logic3d;
    begin
        result := L3D_1;
        for i in a'range loop
            result := l3d_and(result, to_logic3d(a(i)));
        end loop;
        y <= to_std_logic(l3d_not(result));
    end process;
end architecture behavioral;

---------------------------------------------------------------------------
-- OR Gate: N-input OR
---------------------------------------------------------------------------
library ieee;
use ieee.std_logic_1164.all;
use work.logic3d_types_pkg.all;

entity sv_or is
    generic (n : positive := 2);
    port (
        y : out std_logic;
        a : in  std_logic_vector(0 to n-1)
    );
end entity sv_or;

architecture behavioral of sv_or is
begin
    process (a)
        variable result : logic3d;
    begin
        result := L3D_0;
        for i in a'range loop
            result := l3d_or(result, to_logic3d(a(i)));
        end loop;
        y <= to_std_logic(result);
    end process;
end architecture behavioral;

---------------------------------------------------------------------------
-- NOR Gate: N-input NOR
---------------------------------------------------------------------------
library ieee;
use ieee.std_logic_1164.all;
use work.logic3d_types_pkg.all;

entity sv_nor is
    generic (n : positive := 2);
    port (
        y : out std_logic;
        a : in  std_logic_vector(0 to n-1)
    );
end entity sv_nor;

architecture behavioral of sv_nor is
begin
    process (a)
        variable result : logic3d;
    begin
        result := L3D_0;
        for i in a'range loop
            result := l3d_or(result, to_logic3d(a(i)));
        end loop;
        y <= to_std_logic(l3d_not(result));
    end process;
end architecture behavioral;

---------------------------------------------------------------------------
-- XOR Gate: N-input XOR (odd parity)
---------------------------------------------------------------------------
library ieee;
use ieee.std_logic_1164.all;
use work.logic3d_types_pkg.all;

entity sv_xor is
    generic (n : positive := 2);
    port (
        y : out std_logic;
        a : in  std_logic_vector(0 to n-1)
    );
end entity sv_xor;

architecture behavioral of sv_xor is
begin
    process (a)
        variable result : logic3d;
    begin
        result := L3D_0;
        for i in a'range loop
            result := l3d_xor(result, to_logic3d(a(i)));
        end loop;
        y <= to_std_logic(result);
    end process;
end architecture behavioral;

---------------------------------------------------------------------------
-- XNOR Gate: N-input XNOR (even parity)
---------------------------------------------------------------------------
library ieee;
use ieee.std_logic_1164.all;
use work.logic3d_types_pkg.all;

entity sv_xnor is
    generic (n : positive := 2);
    port (
        y : out std_logic;
        a : in  std_logic_vector(0 to n-1)
    );
end entity sv_xnor;

architecture behavioral of sv_xnor is
begin
    process (a)
        variable result : logic3d;
    begin
        result := L3D_0;
        for i in a'range loop
            result := l3d_xor(result, to_logic3d(a(i)));
        end loop;
        y <= to_std_logic(l3d_not(result));
    end process;
end architecture behavioral;

---------------------------------------------------------------------------
-- BUF: Buffer with N outputs (strengthens weak inputs)
---------------------------------------------------------------------------
library ieee;
use ieee.std_logic_1164.all;
use work.logic3d_types_pkg.all;

entity sv_buf is
    generic (n : positive := 1);
    port (
        y : out std_logic_vector(0 to n-1);
        a : in  std_logic
    );
end entity sv_buf;

architecture behavioral of sv_buf is
begin
    process (a)
        variable inp : logic3d;
        variable result : logic3d;
    begin
        inp := to_logic3d(a);
        if is_uncertain(inp) then
            result := L3D_X;
        elsif is_one(inp) then
            result := L3D_1;
        else
            result := L3D_0;
        end if;
        for i in y'range loop
            y(i) <= to_std_logic(result);
        end loop;
    end process;
end architecture behavioral;

---------------------------------------------------------------------------
-- NOT: Inverter with N outputs (inverts and strengthens)
---------------------------------------------------------------------------
library ieee;
use ieee.std_logic_1164.all;
use work.logic3d_types_pkg.all;

entity sv_not is
    generic (n : positive := 1);
    port (
        y : out std_logic_vector(0 to n-1);
        a : in  std_logic
    );
end entity sv_not;

architecture behavioral of sv_not is
begin
    process (a)
        variable inp : logic3d;
        variable result : logic3d;
    begin
        inp := to_logic3d(a);
        if is_uncertain(inp) then
            result := L3D_X;
        elsif is_one(inp) then
            result := L3D_0;
        else
            result := L3D_1;
        end if;
        for i in y'range loop
            y(i) <= to_std_logic(result);
        end loop;
    end process;
end architecture behavioral;
