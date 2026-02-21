-- SystemVerilog MOS and CMOS Switches
-- IEEE 1800 Sections 28.7 and 28.9

---------------------------------------------------------------------------
-- NMOS: N-channel MOS, conducts when gate=1
---------------------------------------------------------------------------
library ieee;
use ieee.std_logic_1164.all;
use work.logic3d_types_pkg.all;

entity sv_nmos is
    port (
        y    : out std_logic;
        data : in  std_logic;
        gate : in  std_logic
    );
end entity sv_nmos;

architecture behavioral of sv_nmos is
begin
    process (data, gate)
        variable d, g : logic3d;
        variable result : logic3d;
    begin
        d := to_logic3d(data);
        g := to_logic3d(gate);
        if not is_uncertain(g) and is_one(g) then
            result := d;
        elsif not is_uncertain(g) and is_zero(g) then
            result := L3D_Z;
        else
            if is_uncertain(d) then result := L3D_X;
            elsif is_one(d) then    result := L3D_H;
            else                    result := L3D_L;
            end if;
        end if;
        y <= to_std_logic(result);
    end process;
end architecture behavioral;

---------------------------------------------------------------------------
-- PMOS: P-channel MOS, conducts when gate=0
---------------------------------------------------------------------------
library ieee;
use ieee.std_logic_1164.all;
use work.logic3d_types_pkg.all;

entity sv_pmos is
    port (
        y    : out std_logic;
        data : in  std_logic;
        gate : in  std_logic
    );
end entity sv_pmos;

architecture behavioral of sv_pmos is
begin
    process (data, gate)
        variable d, g : logic3d;
        variable result : logic3d;
    begin
        d := to_logic3d(data);
        g := to_logic3d(gate);
        if not is_uncertain(g) and is_zero(g) then
            result := d;
        elsif not is_uncertain(g) and is_one(g) then
            result := L3D_Z;
        else
            if is_uncertain(d) then result := L3D_X;
            elsif is_one(d) then    result := L3D_H;
            else                    result := L3D_L;
            end if;
        end if;
        y <= to_std_logic(result);
    end process;
end architecture behavioral;

---------------------------------------------------------------------------
-- RNMOS: Resistive N-channel MOS (strength reduction)
---------------------------------------------------------------------------
library ieee;
use ieee.std_logic_1164.all;
use work.logic3d_types_pkg.all;

entity sv_rnmos is
    port (
        y    : out std_logic;
        data : in  std_logic;
        gate : in  std_logic
    );
end entity sv_rnmos;

architecture behavioral of sv_rnmos is
begin
    process (data, gate)
        variable d, g : logic3d;
        variable result : logic3d;
    begin
        d := to_logic3d(data);
        g := to_logic3d(gate);
        if not is_uncertain(g) and is_one(g) then
            result := l3d_weaken(d);
        elsif not is_uncertain(g) and is_zero(g) then
            result := L3D_Z;
        else
            result := l3d_weaken(d);
        end if;
        y <= to_std_logic(result);
    end process;
end architecture behavioral;

---------------------------------------------------------------------------
-- RPMOS: Resistive P-channel MOS (strength reduction)
---------------------------------------------------------------------------
library ieee;
use ieee.std_logic_1164.all;
use work.logic3d_types_pkg.all;

entity sv_rpmos is
    port (
        y    : out std_logic;
        data : in  std_logic;
        gate : in  std_logic
    );
end entity sv_rpmos;

architecture behavioral of sv_rpmos is
begin
    process (data, gate)
        variable d, g : logic3d;
        variable result : logic3d;
    begin
        d := to_logic3d(data);
        g := to_logic3d(gate);
        if not is_uncertain(g) and is_zero(g) then
            result := l3d_weaken(d);
        elsif not is_uncertain(g) and is_one(g) then
            result := L3D_Z;
        else
            result := l3d_weaken(d);
        end if;
        y <= to_std_logic(result);
    end process;
end architecture behavioral;

---------------------------------------------------------------------------
-- CMOS: Complementary MOS (nmos + pmos in parallel)
---------------------------------------------------------------------------
library ieee;
use ieee.std_logic_1164.all;
use work.logic3d_types_pkg.all;

entity sv_cmos is
    port (
        y     : out std_logic;
        data  : in  std_logic;
        ngate : in  std_logic;
        pgate : in  std_logic
    );
end entity sv_cmos;

architecture behavioral of sv_cmos is
begin
    process (data, ngate, pgate)
        variable d, ng, pg : logic3d;
        variable n_cond, p_cond : boolean;
        variable result : logic3d;
    begin
        d  := to_logic3d(data);
        ng := to_logic3d(ngate);
        pg := to_logic3d(pgate);

        n_cond := not is_uncertain(ng) and is_one(ng);
        p_cond := not is_uncertain(pg) and is_zero(pg);

        if n_cond or p_cond then
            result := d;
        elsif is_uncertain(ng) or is_uncertain(pg) then
            if is_uncertain(d) then result := L3D_X;
            elsif is_one(d) then    result := L3D_H;
            else                    result := L3D_L;
            end if;
        else
            result := L3D_Z;
        end if;
        y <= to_std_logic(result);
    end process;
end architecture behavioral;

---------------------------------------------------------------------------
-- RCMOS: Resistive CMOS (strength reduction)
---------------------------------------------------------------------------
library ieee;
use ieee.std_logic_1164.all;
use work.logic3d_types_pkg.all;

entity sv_rcmos is
    port (
        y     : out std_logic;
        data  : in  std_logic;
        ngate : in  std_logic;
        pgate : in  std_logic
    );
end entity sv_rcmos;

architecture behavioral of sv_rcmos is
begin
    process (data, ngate, pgate)
        variable d, ng, pg : logic3d;
        variable n_cond, p_cond : boolean;
        variable result : logic3d;
    begin
        d  := to_logic3d(data);
        ng := to_logic3d(ngate);
        pg := to_logic3d(pgate);

        n_cond := not is_uncertain(ng) and is_one(ng);
        p_cond := not is_uncertain(pg) and is_zero(pg);

        if n_cond or p_cond or is_uncertain(ng) or is_uncertain(pg) then
            result := l3d_weaken(d);
        else
            result := L3D_Z;
        end if;
        y <= to_std_logic(result);
    end process;
end architecture behavioral;
