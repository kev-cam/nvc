-- arena_pkg.vhd -- a net resolved across an arena shared by several simulations
--
-- A type whose 'driver resolution is RESOLUTION="arena" and that names an
-- ARENA_BAND (see sv2vhdl_resolver.py) is resolved by a generated resolver
-- that also hears the endpoints of the other simulations in the same arena.
-- Which arena a band maps to is not part of the design: the federation
-- controller (arena_ctl.py) tells each simulation when it launches it
-- (FED_NAME, FED_ARENAS), and the simulations meet at the arena's rendezvous
-- point and then talk to each other directly.  A band that is not mapped is
-- resolved inside this simulation only.
--
-- Values cross as real vectors: the package of the resolved type provides
--     function fed_encode(x : T) return real_vector;
--     function fed_decode(v : real_vector; proto : T) return T;    -- v'low first
-- for the driver type and for each ARENA_PLACE type; integer and real are
-- below.
--
-- Load at runtime:
--   nvc --std=2040 -r --load=.../sv2vhdl/libarena.so <wrapper>

package arena_pkg is

    -- Join net `net` (idempotent; the generated resolver calls it first in
    -- every process): its band, how many of its endpoints are in this
    -- simulation, and the signal the bridge deposits when a remote endpoint's
    -- driver arrived ('path_name).
    -- delayed = 1: the resolver applies the physical delay of each path itself
    -- (ARENA_DELAY), reading the drivers' histories (arena_*_at below), and
    -- the simulations run apart by up to the slack between them (the shortest
    -- path delay, arena_slack).
    procedure arena_attach(net : integer; band : string; nlocal : integer; kick : string; delayed : integer);

    -- Local endpoint j's driver changed (sent to every other simulation in the
    -- arena at once), or its place field `what` (1, 2, ..: ARENA_PLACE order;
    -- moving is not an event).
    procedure arena_put(net, j : integer; v : real_vector);
    -- Local end j is port `pin` of instance `inst` (path, '.'-separated):
    -- libarena watches its 'driver and sends what is scheduled on it
    -- (p'driver <= v after d) at once, tagged with when it takes effect.
    procedure arena_watch(net, j : integer; inst, pin : string);
    procedure arena_place(net, j, what : integer; v : real_vector);

    -- The arena as it is now: every endpoint has a global index, the same in
    -- every simulation (simulations in rendezvous order, then each one's own
    -- order), so a resolution function sums in the same order everywhere.
    impure function arena_total(net : integer) return integer;
    impure function arena_global(net, j : integer) return integer;
    impure function arena_remotes(net : integer) return integer;
    impure function arena_remote_global(net, k : integer) return integer;
    -- what = 0: the driver; 1, 2, ..: place fields.  0 = nothing received yet.
    impure function arena_len(net, k, what : integer) return integer;
    procedure arena_get(net, k, what : integer; v : out real_vector);
    impure function arena_fetch(net, k, what : integer) return real_vector;

    -- Delayed resolution: the driver of end g (global index, local ends too)
    -- as it was emitted at time t (length 0: nothing emitted by then), and the
    -- first emission after t (-1 fs: none yet).
    -- arena_slack: the resolver of local end j found the path from end g to
    -- take d (the shortest from another simulation's ends is the slack to it:
    -- how far this simulation may run ahead of it).
    impure function arena_len_at(net, g : integer; t : time) return integer;
    procedure arena_get_at(net, g : integer; t : time; v : out real_vector);
    impure function arena_fetch_at(net, g : integer; t : time) return real_vector;
    impure function arena_next_after(net, g : integer; t : time) return time;
    procedure arena_slack(net, j, g : integer; d : time);

    function fed_encode(x : integer) return real_vector;
    function fed_decode(v : real_vector; proto : integer) return integer;
    function fed_encode(x : real) return real_vector;
    function fed_decode(v : real_vector; proto : real) return real;

end package;

package body arena_pkg is

    procedure arena_attach(net : integer; band : string; nlocal : integer; kick : string; delayed : integer) is begin end procedure;
    impure function arena_len_at(net, g : integer; t : time) return integer is begin end function;
    procedure arena_get_at(net, g : integer; t : time; v : out real_vector) is begin end procedure;
    impure function arena_next_after(net, g : integer; t : time) return time is begin end function;
    procedure arena_slack(net, j, g : integer; d : time) is begin end procedure;
    procedure arena_watch(net, j : integer; inst, pin : string) is begin end procedure;
    attribute foreign of arena_watch [integer, integer, string, string] : procedure is "VHPIDIRECT arena_watch";
    attribute foreign of arena_len_at [integer, integer, time return integer] : function is "VHPIDIRECT arena_len_at";
    attribute foreign of arena_get_at [integer, integer, time, real_vector] : procedure is "VHPIDIRECT arena_get_at";
    attribute foreign of arena_next_after [integer, integer, time return time] : function is "VHPIDIRECT arena_next_after";
    attribute foreign of arena_slack [integer, integer, integer, time] : procedure is "VHPIDIRECT arena_slack";

    impure function arena_fetch_at(net, g : integer; t : time) return real_vector is
        variable v : real_vector(0 to arena_len_at(net, g, t) - 1);
    begin
        if v'length > 0 then
            arena_get_at(net, g, t, v);
        end if;
        return v;
    end function;
    procedure arena_put(net, j : integer; v : real_vector) is begin end procedure;
    procedure arena_place(net, j, what : integer; v : real_vector) is begin end procedure;
    impure function arena_total(net : integer) return integer is begin end function;
    impure function arena_global(net, j : integer) return integer is begin end function;
    impure function arena_remotes(net : integer) return integer is begin end function;
    impure function arena_remote_global(net, k : integer) return integer is begin end function;
    impure function arena_len(net, k, what : integer) return integer is begin end function;
    procedure arena_get(net, k, what : integer; v : out real_vector) is begin end procedure;

    attribute foreign of arena_attach [integer, string, integer, string, integer] : procedure is "VHPIDIRECT arena_attach";
    attribute foreign of arena_put [integer, integer, real_vector] : procedure is "VHPIDIRECT arena_put";
    attribute foreign of arena_place [integer, integer, integer, real_vector] : procedure is "VHPIDIRECT arena_place";
    attribute foreign of arena_total [integer return integer] : function is "VHPIDIRECT arena_total";
    attribute foreign of arena_global [integer, integer return integer] : function is "VHPIDIRECT arena_global";
    attribute foreign of arena_remotes [integer return integer] : function is "VHPIDIRECT arena_remotes";
    attribute foreign of arena_remote_global [integer, integer return integer] : function is "VHPIDIRECT arena_remote_global";
    attribute foreign of arena_len [integer, integer, integer return integer] : function is "VHPIDIRECT arena_len";
    attribute foreign of arena_get [integer, integer, integer, real_vector] : procedure is "VHPIDIRECT arena_get";

    impure function arena_fetch(net, k, what : integer) return real_vector is
        variable v : real_vector(0 to arena_len(net, k, what) - 1);
    begin
        if v'length > 0 then
            arena_get(net, k, what, v);
        end if;
        return v;
    end function;

    function fed_encode(x : integer) return real_vector is
    begin
        return (0 => real(x));
    end function;

    function fed_decode(v : real_vector; proto : integer) return integer is
    begin
        return integer(v(v'low));
    end function;

    function fed_encode(x : real) return real_vector is
    begin
        return (0 => x);
    end function;

    function fed_decode(v : real_vector; proto : real) return real is
    begin
        return v(v'low);
    end function;

end package body;
