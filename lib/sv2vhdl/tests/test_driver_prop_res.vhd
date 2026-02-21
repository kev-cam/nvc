-- Hand-written resolvers for test_driver_prop.vhd
-- One process per 'other signal, := deposit assignment.
-- Reads 'driver, writes 'other (only on endpoints that have it).
-- sv_assign endpoints: 'driver only, no 'other.
-- sv_tran endpoints: 'driver and 'other.

---------------------------------------------------------------------------
-- TEST 1: test_prop1 (sv_assign -> tran)
-- Net "left":  drv.q (driver-only) + t1.a (tran)
-- Net "right": t1.b (tran, single endpoint)
---------------------------------------------------------------------------
library ieee;
use ieee.std_logic_1164.all;
use work.logic3ds_pkg.all;

entity rn_prop1 is end entity;
architecture gen of rn_prop1 is
    alias drv_q  is << signal .resolved_test_prop1.dut.drv.q.driver : logic3ds >>;
    alias drv_a  is << signal .resolved_test_prop1.dut.t1.a.driver : logic3ds >>;
    alias oth_a  is << signal .resolved_test_prop1.dut.t1.a.other : logic3ds >>;
    alias oth_b  is << signal .resolved_test_prop1.dut.t1.b.other : logic3ds >>;
begin
    p_oth_a: process(drv_q) begin
        oth_a := drv_q;
    end process;

    p_oth_b: process(oth_b) begin
        oth_b := L3DS_Z;
    end process;
end architecture;

entity resolved_test_prop1 is end entity;
architecture wrap of resolved_test_prop1 is
begin
    dut: entity work.test_prop1;
    rn:  entity work.rn_prop1;
end architecture;

---------------------------------------------------------------------------
-- TEST 2: test_prop2 (sv_assign -> tran -> tran)
-- Net "n1": drv.q (driver-only) + t1.a (tran)
-- Net "n2": t1.b (tran) + t2.a (tran)  -> swap
-- Net "n3": t2.b (tran, single endpoint)
---------------------------------------------------------------------------
library ieee;
use ieee.std_logic_1164.all;
use work.logic3ds_pkg.all;

entity rn_prop2 is end entity;
architecture gen of rn_prop2 is
    -- Net n1
    alias drv_q   is << signal .resolved_test_prop2.dut.drv.q.driver : logic3ds >>;
    alias drv_1a  is << signal .resolved_test_prop2.dut.t1.a.driver : logic3ds >>;
    alias oth_1a  is << signal .resolved_test_prop2.dut.t1.a.other : logic3ds >>;
    -- Net n2
    alias drv_1b  is << signal .resolved_test_prop2.dut.t1.b.driver : logic3ds >>;
    alias oth_1b  is << signal .resolved_test_prop2.dut.t1.b.other : logic3ds >>;
    alias drv_2a  is << signal .resolved_test_prop2.dut.t2.a.driver : logic3ds >>;
    alias oth_2a  is << signal .resolved_test_prop2.dut.t2.a.other : logic3ds >>;
    -- Net n3
    alias oth_2b  is << signal .resolved_test_prop2.dut.t2.b.other : logic3ds >>;
begin
    -- Net n1: t1.a sees drv.q
    p_oth_1a: process(drv_q) begin
        oth_1a := drv_q;
    end process;

    -- Net n2: swap
    p_oth_1b: process(drv_2a) begin
        oth_1b := drv_2a;
    end process;

    p_oth_2a: process(drv_1b) begin
        oth_2a := drv_1b;
    end process;

    -- Net n3: single endpoint
    p_oth_2b: process(oth_2b) begin
        oth_2b := L3DS_Z;
    end process;
end architecture;

entity resolved_test_prop2 is end entity;
architecture wrap of resolved_test_prop2 is
begin
    dut: entity work.test_prop2;
    rn:  entity work.rn_prop2;
end architecture;

---------------------------------------------------------------------------
-- TEST 3: test_prop3 (bidirectional: sv_assign on each side of tran)
-- Net "left":  drv_a.q (driver-only) + t1.a (tran)
-- Net "right": drv_b.q (driver-only) + t1.b (tran)
---------------------------------------------------------------------------
library ieee;
use ieee.std_logic_1164.all;
use work.logic3ds_pkg.all;

entity rn_prop3 is end entity;
architecture gen of rn_prop3 is
    alias drv_aq is << signal .resolved_test_prop3.dut.drv_a.q.driver : logic3ds >>;
    alias drv_a  is << signal .resolved_test_prop3.dut.t1.a.driver : logic3ds >>;
    alias oth_a  is << signal .resolved_test_prop3.dut.t1.a.other : logic3ds >>;
    alias drv_bq is << signal .resolved_test_prop3.dut.drv_b.q.driver : logic3ds >>;
    alias drv_b  is << signal .resolved_test_prop3.dut.t1.b.driver : logic3ds >>;
    alias oth_b  is << signal .resolved_test_prop3.dut.t1.b.other : logic3ds >>;
begin
    -- Net "left": t1.a sees drv_a.q
    p_oth_a: process(drv_aq) begin
        oth_a := drv_aq;
    end process;

    -- Net "right": t1.b sees drv_b.q
    p_oth_b: process(drv_bq) begin
        oth_b := drv_bq;
    end process;
end architecture;

entity resolved_test_prop3 is end entity;
architecture wrap of resolved_test_prop3 is
begin
    dut: entity work.test_prop3;
    rn:  entity work.rn_prop3;
end architecture;

---------------------------------------------------------------------------
-- TEST 4: test_prop4 (chain of 4 trans with generate)
-- Net n(1): drv.q (driver-only) + gen(1).ti.a (tran)
-- Net n(2): gen(1).ti.b + gen(2).ti.a  -> swap
-- Net n(3): gen(2).ti.b + gen(3).ti.a  -> swap
-- Net n(4): gen(3).ti.b + gen(4).ti.a  -> swap
-- Net n(5): gen(4).ti.b (single endpoint)
---------------------------------------------------------------------------
library ieee;
use ieee.std_logic_1164.all;
use work.logic3ds_pkg.all;

entity rn_prop4 is end entity;
architecture gen of rn_prop4 is
    -- Net n(1)
    alias drv_q    is << signal .resolved_test_prop4.dut.drv.q.driver : logic3ds >>;
    alias oth_1a   is << signal .resolved_test_prop4.dut.gen(1).ti.a.other : logic3ds >>;
    -- Net n(2)
    alias drv_1b   is << signal .resolved_test_prop4.dut.gen(1).ti.b.driver : logic3ds >>;
    alias oth_1b   is << signal .resolved_test_prop4.dut.gen(1).ti.b.other : logic3ds >>;
    alias drv_2a   is << signal .resolved_test_prop4.dut.gen(2).ti.a.driver : logic3ds >>;
    alias oth_2a   is << signal .resolved_test_prop4.dut.gen(2).ti.a.other : logic3ds >>;
    -- Net n(3)
    alias drv_2b   is << signal .resolved_test_prop4.dut.gen(2).ti.b.driver : logic3ds >>;
    alias oth_2b   is << signal .resolved_test_prop4.dut.gen(2).ti.b.other : logic3ds >>;
    alias drv_3a   is << signal .resolved_test_prop4.dut.gen(3).ti.a.driver : logic3ds >>;
    alias oth_3a   is << signal .resolved_test_prop4.dut.gen(3).ti.a.other : logic3ds >>;
    -- Net n(4)
    alias drv_3b   is << signal .resolved_test_prop4.dut.gen(3).ti.b.driver : logic3ds >>;
    alias oth_3b   is << signal .resolved_test_prop4.dut.gen(3).ti.b.other : logic3ds >>;
    alias drv_4a   is << signal .resolved_test_prop4.dut.gen(4).ti.a.driver : logic3ds >>;
    alias oth_4a   is << signal .resolved_test_prop4.dut.gen(4).ti.a.other : logic3ds >>;
    -- Net n(5)
    alias oth_4b   is << signal .resolved_test_prop4.dut.gen(4).ti.b.other : logic3ds >>;
begin
    -- Net n(1): gen(1).ti.a sees drv.q
    p_oth_1a: process(drv_q) begin
        oth_1a := drv_q;
    end process;

    -- Net n(2): swap
    p_oth_1b: process(drv_2a) begin
        oth_1b := drv_2a;
    end process;
    p_oth_2a: process(drv_1b) begin
        oth_2a := drv_1b;
    end process;

    -- Net n(3): swap
    p_oth_2b: process(drv_3a) begin
        oth_2b := drv_3a;
    end process;
    p_oth_3a: process(drv_2b) begin
        oth_3a := drv_2b;
    end process;

    -- Net n(4): swap
    p_oth_3b: process(drv_4a) begin
        oth_3b := drv_4a;
    end process;
    p_oth_4a: process(drv_3b) begin
        oth_4a := drv_3b;
    end process;

    -- Net n(5): single endpoint
    p_oth_4b: process(oth_4b) begin
        oth_4b := L3DS_Z;
    end process;
end architecture;

entity resolved_test_prop4 is end entity;
architecture wrap of resolved_test_prop4 is
begin
    dut: entity work.test_prop4;
    rn:  entity work.rn_prop4;
end architecture;

---------------------------------------------------------------------------
-- TEST 5: test_prop5 (two assigns tied + tran observer)
-- Net "net1": drv_a.q (driver-only) + drv_b.q (driver-only) + t1.a (tran)
-- Net "obs":  t1.b (single endpoint)
---------------------------------------------------------------------------
library ieee;
use ieee.std_logic_1164.all;
use work.logic3ds_pkg.all;

entity rn_prop5 is end entity;
architecture gen of rn_prop5 is
    alias drv_aq is << signal .resolved_test_prop5.dut.drv_a.q.driver : logic3ds >>;
    alias drv_bq is << signal .resolved_test_prop5.dut.drv_b.q.driver : logic3ds >>;
    alias oth_a  is << signal .resolved_test_prop5.dut.t1.a.other : logic3ds >>;
    alias oth_b  is << signal .resolved_test_prop5.dut.t1.b.other : logic3ds >>;
begin
    -- Net "net1": t1.a sees resolution of drv_a.q and drv_b.q
    p_oth_a: process(drv_aq, drv_bq) begin
        oth_a := l3ds_resolve(logic3ds_vector'(drv_aq, drv_bq));
    end process;

    -- Net "obs": single endpoint
    p_oth_b: process(oth_b) begin
        oth_b := L3DS_Z;
    end process;
end architecture;

entity resolved_test_prop5 is end entity;
architecture wrap of resolved_test_prop5 is
begin
    dut: entity work.test_prop5;
    rn:  entity work.rn_prop5;
end architecture;
