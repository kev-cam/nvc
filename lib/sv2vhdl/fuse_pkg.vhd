-- #74: net-fusion primitives for generated resolvers.
--
-- The resolver generator emits, per pass-through endpoint, a one-shot
-- arbitration that fuses the receiver onto the single driving endpoint:
-- after a successful fuse the receiver's readers see the driver's
-- storage directly (zero per-event copies) and the process parks
-- forever.  An all-alias resolver therefore has no running code at all,
-- bar event wake-up, which rides the driver's net.
--
-- The bodies below are conservative fallbacks (always false) used when
-- JIT intrinsics are unavailable (AOT, OPT_JIT_INTRINSICS=0): callers
-- then keep today's copy-loop behaviour.  With intrinsics enabled the
-- JIT binds these to x_signal_undriven / x_fuse_signals in the runtime.
library ieee;
use ieee.std_logic_1164.all;

package fuse_pkg is
    impure function undriven(signal s : in std_ulogic) return boolean;
    impure function fuse_try(signal a : in std_ulogic;
                             signal b : in std_ulogic) return boolean;
end package;

package body fuse_pkg is

    impure function undriven(signal s : in std_ulogic) return boolean is
    begin
        return false;
    end function;

    impure function fuse_try(signal a : in std_ulogic;
                             signal b : in std_ulogic) return boolean is
    begin
        return false;
    end function;

end package body;
