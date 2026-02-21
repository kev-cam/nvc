-- SystemVerilog Built-in Primitives Library
-- IEEE 1800-2017 Section 28: Gate-level and switch-level modeling
--
-- Component declarations for instantiation in translated designs.
-- All ports use std_logic/std_logic_vector for external compatibility.
-- Architectures use logic3d internally for enhanced X-propagation,
-- with _driver/_others implicit signal ports on bidirectional switches.

library ieee;
use ieee.std_logic_1164.all;
package sv_primitives is

    ---------------------------------------------------------------------------
    -- 28.4 MULTI-INPUT GATES
    ---------------------------------------------------------------------------
    component sv_and is
        generic (n : positive := 2);
        port (
            y : out std_logic;
            a : in  std_logic_vector(0 to n-1)
        );
    end component;

    component sv_nand is
        generic (n : positive := 2);
        port (
            y : out std_logic;
            a : in  std_logic_vector(0 to n-1)
        );
    end component;

    component sv_or is
        generic (n : positive := 2);
        port (
            y : out std_logic;
            a : in  std_logic_vector(0 to n-1)
        );
    end component;

    component sv_nor is
        generic (n : positive := 2);
        port (
            y : out std_logic;
            a : in  std_logic_vector(0 to n-1)
        );
    end component;

    component sv_xor is
        generic (n : positive := 2);
        port (
            y : out std_logic;
            a : in  std_logic_vector(0 to n-1)
        );
    end component;

    component sv_xnor is
        generic (n : positive := 2);
        port (
            y : out std_logic;
            a : in  std_logic_vector(0 to n-1)
        );
    end component;

    ---------------------------------------------------------------------------
    -- 28.5 MULTI-OUTPUT GATES
    ---------------------------------------------------------------------------
    component sv_buf is
        generic (n : positive := 1);
        port (
            y : out std_logic_vector(0 to n-1);
            a : in  std_logic
        );
    end component;

    component sv_not is
        generic (n : positive := 1);
        port (
            y : out std_logic_vector(0 to n-1);
            a : in  std_logic
        );
    end component;

    ---------------------------------------------------------------------------
    -- 28.6 THREE-STATE GATES
    ---------------------------------------------------------------------------
    component sv_bufif0 is
        port (
            y    : out std_logic;
            data : in  std_logic;
            ctrl : in  std_logic
        );
    end component;

    component sv_bufif1 is
        port (
            y    : out std_logic;
            data : in  std_logic;
            ctrl : in  std_logic
        );
    end component;

    component sv_notif0 is
        port (
            y    : out std_logic;
            data : in  std_logic;
            ctrl : in  std_logic
        );
    end component;

    component sv_notif1 is
        port (
            y    : out std_logic;
            data : in  std_logic;
            ctrl : in  std_logic
        );
    end component;

    ---------------------------------------------------------------------------
    -- 28.7 MOS SWITCHES
    ---------------------------------------------------------------------------
    component sv_nmos is
        port (
            y    : out std_logic;
            data : in  std_logic;
            gate : in  std_logic
        );
    end component;

    component sv_pmos is
        port (
            y    : out std_logic;
            data : in  std_logic;
            gate : in  std_logic
        );
    end component;

    component sv_rnmos is
        port (
            y    : out std_logic;
            data : in  std_logic;
            gate : in  std_logic
        );
    end component;

    component sv_rpmos is
        port (
            y    : out std_logic;
            data : in  std_logic;
            gate : in  std_logic
        );
    end component;

    ---------------------------------------------------------------------------
    -- 28.9 CMOS SWITCHES
    ---------------------------------------------------------------------------
    component sv_cmos is
        port (
            y     : out std_logic;
            data  : in  std_logic;
            ngate : in  std_logic;
            pgate : in  std_logic
        );
    end component;

    component sv_rcmos is
        port (
            y     : out std_logic;
            data  : in  std_logic;
            ngate : in  std_logic;
            pgate : in  std_logic
        );
    end component;

    ---------------------------------------------------------------------------
    -- 28.8 BIDIRECTIONAL SWITCHES
    -- Modeled using 'driver/'others implicit signals on inout ports.
    -- 'DRIVER = implicit signal for what this entity drives onto the net
    -- 'OTHERS = resolved value excluding this entity's driver
    -- At elaboration time, the resolver plugin discovers these implicit
    -- signals and constructs the appropriate resolution network.
    ---------------------------------------------------------------------------
    component sv_tran is
        port (
            a : inout std_logic;
            b : inout std_logic
        );
    end component;

    component sv_tranif0 is
        port (
            a    : inout std_logic;
            b    : inout std_logic;
            ctrl : in    std_logic
        );
    end component;

    component sv_tranif1 is
        port (
            a    : inout std_logic;
            b    : inout std_logic;
            ctrl : in    std_logic
        );
    end component;

    component sv_rtran is
        port (
            a : inout std_logic;
            b : inout std_logic
        );
    end component;

    component sv_rtranif0 is
        port (
            a    : inout std_logic;
            b    : inout std_logic;
            ctrl : in    std_logic
        );
    end component;

    component sv_rtranif1 is
        port (
            a    : inout std_logic;
            b    : inout std_logic;
            ctrl : in    std_logic
        );
    end component;

    ---------------------------------------------------------------------------
    -- 28.10 PULL GATES
    ---------------------------------------------------------------------------
    component sv_pullup is
        port (
            y : out std_logic
        );
    end component;

    component sv_pulldown is
        port (
            y : out std_logic
        );
    end component;

end package sv_primitives;
