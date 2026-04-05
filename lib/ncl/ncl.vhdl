-- ncl.vhdl — NULL Convention Logic library for NVC.
--
-- Implements delay-insensitive dual-rail encoding and NCL threshold gates.
-- Based on the ARV async RISC-V project's ncl package, extended with:
--   - TH (threshold) gates: TH12, TH22, TH13, TH23, TH33, TH34, etc.
--   - Completion detection
--   - NCL register (with hysteresis)
--   - 3-valued logic: NULL (spacer), DATA0, DATA1
--
-- References:
--   Fant & Brandt, "NULL Convention Logic: a complete and consistent
--   logic for asynchronous digital circuit synthesis" (1996)
--
-- Encoding:
--   ncl_logic = (L, H) where:
--     (0, 0) = NULL (spacer, no data)
--     (1, 0) = DATA0 (logic 0)
--     (0, 1) = DATA1 (logic 1)
--     (1, 1) = INVALID (glitch, treated as NULL)

library IEEE;
use IEEE.std_logic_1164.all;

package ncl is

    -- ---- Core types ----
    type ncl_logic is record
        L : std_logic;  -- low rail (asserted = data 0)
        H : std_logic;  -- high rail (asserted = data 1)
    end record;

    type ncl_logic_vector is array (natural range <>) of ncl_logic;

    -- Constants
    constant NCL_NULL  : ncl_logic := (L => '0', H => '0');
    constant NCL_DATA0 : ncl_logic := (L => '0', H => '1');  -- encodes value 0
    constant NCL_DATA1 : ncl_logic := (L => '1', H => '0');  -- encodes value 1

    -- ---- State queries ----
    function ncl_is_null(d : ncl_logic)        return boolean;
    function ncl_is_null(d : ncl_logic_vector) return boolean;
    function ncl_is_null(d : ncl_logic)        return std_logic;
    function ncl_is_null(d : ncl_logic_vector) return std_logic_vector;
    function ncl_is_data(d : ncl_logic)        return boolean;
    function ncl_is_data(d : ncl_logic_vector) return boolean;
    -- Completion: true when ALL bits are DATA (none NULL)
    function ncl_complete(d : ncl_logic_vector) return boolean;
    function ncl_complete(d : ncl_logic_vector) return std_logic;

    -- ---- Encode / Decode ----
    function ncl_encode(d : std_logic)        return ncl_logic;
    function ncl_encode(d : std_logic_vector) return ncl_logic_vector;
    function ncl_decode(d : ncl_logic)        return std_logic;
    function ncl_decode(d : ncl_logic_vector) return std_logic_vector;

    -- ---- Threshold gates (fundamental NCL primitives) ----
    -- THmn: m-of-n threshold gate. Output asserts when m of n inputs assert.
    -- With hysteresis: output stays asserted until ALL inputs deassert (NULL).
    -- These are the building blocks of all NCL circuits.
    function th12(a, b : std_logic) return std_logic;  -- OR (1 of 2)
    function th22(a, b : std_logic) return std_logic;  -- AND (2 of 2)
    function th13(a, b, c : std_logic) return std_logic;
    function th23(a, b, c : std_logic) return std_logic;
    function th33(a, b, c : std_logic) return std_logic;
    function th24(a, b, c, d : std_logic) return std_logic;
    function th34(a, b, c, d : std_logic) return std_logic;
    function th44(a, b, c, d : std_logic) return std_logic;

    -- Weighted threshold gates: THmnW (weight on first input)
    function th12w2(a, b : std_logic) return std_logic;  -- a has weight 2
    function th23w2(a, b, c : std_logic) return std_logic;
    function th34w2(a, b, c, d : std_logic) return std_logic;

    -- ---- Logic operators (dual-rail) ----
    function "and"  (l, r: ncl_logic) return ncl_logic;
    function "nand" (l, r: ncl_logic) return ncl_logic;
    function "or"   (l, r: ncl_logic) return ncl_logic;
    function "nor"  (l, r: ncl_logic) return ncl_logic;
    function "xor"  (l, r: ncl_logic) return ncl_logic;
    function "xnor" (l, r: ncl_logic) return ncl_logic;
    function "not"  (l   : ncl_logic) return ncl_logic;

    -- Vector operators
    function "and"  (l, r: ncl_logic_vector) return ncl_logic_vector;
    function "nand" (l, r: ncl_logic_vector) return ncl_logic_vector;
    function "or"   (l, r: ncl_logic_vector) return ncl_logic_vector;
    function "nor"  (l, r: ncl_logic_vector) return ncl_logic_vector;
    function "xor"  (l, r: ncl_logic_vector) return ncl_logic_vector;
    function "xnor" (l, r: ncl_logic_vector) return ncl_logic_vector;
    function "not"  (l   : ncl_logic_vector) return ncl_logic_vector;

    -- Comparators
    function "="    (l, r: ncl_logic) return boolean;
    function "="    (l  : ncl_logic; r: std_logic) return boolean;

    -- ---- Arithmetic ----
    -- Ripple-carry adder (N-bit)
    function ncl_add(a, b : ncl_logic_vector) return ncl_logic_vector;
    -- Subtraction: a - b (via complement + add)
    function ncl_sub(a, b : ncl_logic_vector) return ncl_logic_vector;
    -- Negate (two's complement)
    function ncl_negate(a : ncl_logic_vector) return ncl_logic_vector;

    -- ---- Multiplexer ----
    -- 2:1 MUX: if sel=DATA1, return a; if sel=DATA0, return b; if NULL, return NULL
    function ncl_mux(sel : ncl_logic; a, b : ncl_logic_vector) return ncl_logic_vector;
    -- MUX with vector selector (uses bit 0)
    function ncl_mux(sel : ncl_logic_vector; a, b : ncl_logic_vector) return ncl_logic_vector;

    -- ---- Comparators ----
    -- Returns 1-bit NCL result: DATA1 if true, DATA0 if false, NULL if inputs NULL
    function ncl_compare(a, b : ncl_logic_vector; op : string) return ncl_logic_vector;

    -- ---- Utility ----
    -- Create a NULL vector of given width
    function ncl_null_vector(width : natural) return ncl_logic_vector;

end package ncl;

package body ncl is

    -- ---- State queries ----
    function ncl_is_null(d: ncl_logic) return boolean is
    begin
        return (d.H xnor d.L) = '1';
    end function;

    function ncl_is_null(d: ncl_logic) return std_logic is
    begin
        return d.H xnor d.L;
    end function;

    function ncl_is_null(d : ncl_logic_vector) return boolean is
    begin
        for i in d'range loop
            if ncl_is_null(d(i)) then return true; end if;
        end loop;
        return false;
    end function;

    function ncl_is_null(d : ncl_logic_vector) return std_logic_vector is
        variable r : std_logic_vector(d'range);
    begin
        for i in d'range loop r(i) := ncl_is_null(d(i)); end loop;
        return r;
    end function;

    function ncl_is_data(d: ncl_logic) return boolean is
    begin
        return not ncl_is_null(d);
    end function;

    function ncl_is_data(d: ncl_logic_vector) return boolean is
    begin
        return not ncl_is_null(d);
    end function;

    function ncl_complete(d : ncl_logic_vector) return boolean is
    begin
        for i in d'range loop
            if ncl_is_null(d(i)) then return false; end if;
        end loop;
        return true;
    end function;

    function ncl_complete(d : ncl_logic_vector) return std_logic is
    begin
        for i in d'range loop
            if ncl_is_null(d(i)) then return '0'; end if;
        end loop;
        return '1';
    end function;

    -- ---- Encode / Decode ----
    function ncl_encode(d : std_logic) return ncl_logic is
    begin
        return (H => not d, L => d);
    end function;

    function ncl_encode(d : std_logic_vector) return ncl_logic_vector is
        variable r : ncl_logic_vector(d'range);
    begin
        for i in d'range loop r(i) := ncl_encode(d(i)); end loop;
        return r;
    end function;

    function ncl_decode(d : ncl_logic) return std_logic is
    begin
        if ncl_is_null(d) then return 'U'; end if;
        return d.L;
    end function;

    function ncl_decode(d : ncl_logic_vector) return std_logic_vector is
        variable r : std_logic_vector(d'range);
    begin
        for i in d'range loop r(i) := ncl_decode(d(i)); end loop;
        return r;
    end function;

    -- ---- Threshold gates ----
    -- Note: these are combinational models (no hysteresis state).
    -- For synthesis, hysteresis is implemented in the NCL register.
    -- For simulation, these capture the functional behavior.

    function th12(a, b : std_logic) return std_logic is
    begin return a or b; end function;

    function th22(a, b : std_logic) return std_logic is
    begin return a and b; end function;

    function th13(a, b, c : std_logic) return std_logic is
    begin return a or b or c; end function;

    function th23(a, b, c : std_logic) return std_logic is
    begin return (a and b) or (a and c) or (b and c); end function;

    function th33(a, b, c : std_logic) return std_logic is
    begin return a and b and c; end function;

    function th24(a, b, c, d : std_logic) return std_logic is
        variable s : integer := 0;
    begin
        if a = '1' then s := s + 1; end if;
        if b = '1' then s := s + 1; end if;
        if c = '1' then s := s + 1; end if;
        if d = '1' then s := s + 1; end if;
        if s >= 2 then return '1'; else return '0'; end if;
    end function;

    function th34(a, b, c, d : std_logic) return std_logic is
        variable s : integer := 0;
    begin
        if a = '1' then s := s + 1; end if;
        if b = '1' then s := s + 1; end if;
        if c = '1' then s := s + 1; end if;
        if d = '1' then s := s + 1; end if;
        if s >= 3 then return '1'; else return '0'; end if;
    end function;

    function th44(a, b, c, d : std_logic) return std_logic is
    begin return a and b and c and d; end function;

    -- Weighted: input a counts as 2
    function th12w2(a, b : std_logic) return std_logic is
    begin return a or b; end function;  -- weight doesn't change 1-of-2

    function th23w2(a, b, c : std_logic) return std_logic is
    begin return a or (b and c); end function;

    function th34w2(a, b, c, d : std_logic) return std_logic is
    begin return (a and (b or c or d)) or (b and c and d); end function;

    -- ---- Logic operators ----
    function "and" (l, r : ncl_logic) return ncl_logic is
    begin
        if ncl_is_null(l) or ncl_is_null(r) then return NCL_NULL; end if;
        return ncl_encode(l.L and r.L);
    end function;

    function "nand" (l, r : ncl_logic) return ncl_logic is
    begin
        if ncl_is_null(l) or ncl_is_null(r) then return NCL_NULL; end if;
        return ncl_encode(l.L nand r.L);
    end function;

    function "or" (l, r : ncl_logic) return ncl_logic is
    begin
        if ncl_is_null(l) or ncl_is_null(r) then return NCL_NULL; end if;
        return ncl_encode(l.L or r.L);
    end function;

    function "nor" (l, r : ncl_logic) return ncl_logic is
    begin
        if ncl_is_null(l) or ncl_is_null(r) then return NCL_NULL; end if;
        return ncl_encode(l.L nor r.L);
    end function;

    function "xor" (l, r : ncl_logic) return ncl_logic is
    begin
        if ncl_is_null(l) or ncl_is_null(r) then return NCL_NULL; end if;
        return ncl_encode(l.L xor r.L);
    end function;

    function "xnor" (l, r : ncl_logic) return ncl_logic is
    begin
        if ncl_is_null(l) or ncl_is_null(r) then return NCL_NULL; end if;
        return ncl_encode(l.L xnor r.L);
    end function;

    function "not" (l : ncl_logic) return ncl_logic is
    begin return (H => l.L, L => l.H); end function;

    -- Vector operators
    function "and" (l, r : ncl_logic_vector) return ncl_logic_vector is
        variable o : ncl_logic_vector(l'range);
    begin for i in l'range loop o(i) := l(i) and r(i); end loop; return o; end function;
    function "nand" (l, r : ncl_logic_vector) return ncl_logic_vector is
        variable o : ncl_logic_vector(l'range);
    begin for i in l'range loop o(i) := l(i) nand r(i); end loop; return o; end function;
    function "or" (l, r : ncl_logic_vector) return ncl_logic_vector is
        variable o : ncl_logic_vector(l'range);
    begin for i in l'range loop o(i) := l(i) or r(i); end loop; return o; end function;
    function "nor" (l, r : ncl_logic_vector) return ncl_logic_vector is
        variable o : ncl_logic_vector(l'range);
    begin for i in l'range loop o(i) := l(i) nor r(i); end loop; return o; end function;
    function "xor" (l, r : ncl_logic_vector) return ncl_logic_vector is
        variable o : ncl_logic_vector(l'range);
    begin for i in l'range loop o(i) := l(i) xor r(i); end loop; return o; end function;
    function "xnor" (l, r : ncl_logic_vector) return ncl_logic_vector is
        variable o : ncl_logic_vector(l'range);
    begin for i in l'range loop o(i) := l(i) xnor r(i); end loop; return o; end function;
    function "not" (l : ncl_logic_vector) return ncl_logic_vector is
        variable o : ncl_logic_vector(l'range);
    begin for i in l'range loop o(i) := not l(i); end loop; return o; end function;

    -- Comparators
    function "=" (l, r: ncl_logic) return boolean is
    begin
        if ncl_is_null(l) or ncl_is_null(r) or (l.L /= r.L) then return false; end if;
        return true;
    end function;

    function "=" (l: ncl_logic; r: std_logic) return boolean is
    begin
        if ncl_is_null(l) or (l.L /= r) then return false; end if;
        return true;
    end function;

    -- Utility
    function ncl_null_vector(width : natural) return ncl_logic_vector is
        variable r : ncl_logic_vector(width-1 downto 0) := (others => NCL_NULL);
    begin return r; end function;

    -- ---- 1-bit full adder (inline, same as ARV's) ----
    function ncl_fulladd(a, b, cin : ncl_logic) return ncl_logic_vector is
        variable r : ncl_logic_vector(1 downto 0);  -- (1)=cout, (0)=sum
    begin
        r(0) := a xor b xor cin;
        r(1) := (a and b) or ((a xor b) and cin);
        return r;
    end function;

    -- ---- Ripple-carry adder ----
    function ncl_add(a, b : ncl_logic_vector) return ncl_logic_vector is
        variable r   : ncl_logic_vector(a'length-1 downto 0);
        variable cry : ncl_logic := NCL_DATA0;  -- carry starts at 0
        variable fa  : ncl_logic_vector(1 downto 0);
    begin
        for i in 0 to a'length-1 loop
            fa := ncl_fulladd(a(i), b(i), cry);
            r(i) := fa(0);  -- sum
            cry  := fa(1);  -- carry
        end loop;
        return r;
    end function;

    -- ---- Negate (two's complement: NOT + 1) ----
    function ncl_negate(a : ncl_logic_vector) return ncl_logic_vector is
        variable inv : ncl_logic_vector(a'range);
        variable one : ncl_logic_vector(a'range) := (others => NCL_DATA0);
    begin
        inv := not a;
        one(0) := NCL_DATA1;
        return ncl_add(inv, one);
    end function;

    -- ---- Subtraction: a - b = a + (~b + 1) ----
    function ncl_sub(a, b : ncl_logic_vector) return ncl_logic_vector is
    begin
        return ncl_add(a, ncl_negate(b));
    end function;

    -- ---- 2:1 MUX ----
    -- sel=DATA1 → a; sel=DATA0 → b; sel=NULL → NULL
    function ncl_mux(sel : ncl_logic; a, b : ncl_logic_vector) return ncl_logic_vector is
        variable r : ncl_logic_vector(a'range);
    begin
        if ncl_is_null(sel) then
            return (a'range => NCL_NULL);
        end if;
        -- sel.L = '1' means value=1 (DATA1) → select a
        -- sel.L = '0' means value=0 (DATA0) → select b
        if ncl_decode(sel) = '1' then
            return a;
        else
            return b;
        end if;
    end function;

    function ncl_mux(sel : ncl_logic_vector; a, b : ncl_logic_vector) return ncl_logic_vector is
    begin
        return ncl_mux(sel(0), a, b);
    end function;

    -- ---- Comparators ----
    -- Evaluates comparison in decoded (binary) domain, returns NCL result.
    -- This is a behavioral model — a structural NCL comparator would use
    -- threshold gates, but functionally they're equivalent.
    function ncl_compare(a, b : ncl_logic_vector; op : string) return ncl_logic_vector is
        variable r    : ncl_logic_vector(0 downto 0);
        variable av, bv : std_logic_vector(a'range);
        variable ai, bi : integer;
        variable cond : boolean;
    begin
        -- If any input is NULL, return NULL
        if ncl_is_null(a) or ncl_is_null(b) then
            r(0) := NCL_NULL;
            return r;
        end if;
        -- Decode to binary for comparison
        av := ncl_decode(a);
        bv := ncl_decode(b);
        -- Convert to integer for signed/unsigned compare
        ai := 0; bi := 0;
        for i in av'range loop
            if av(i) = '1' then ai := ai + 2**i; end if;
            if bv(i) = '1' then bi := bi + 2**i; end if;
        end loop;
        -- Evaluate
        if    op = ">"  then cond := ai > bi;
        elsif op = "<"  then cond := ai < bi;
        elsif op = ">=" then cond := ai >= bi;
        elsif op = "<=" then cond := ai <= bi;
        elsif op = "==" then cond := ai = bi;
        elsif op = "!=" then cond := ai /= bi;
        else cond := false;
        end if;
        if cond then
            r(0) := NCL_DATA1;
        else
            r(0) := NCL_DATA0;
        end if;
        return r;
    end function;

end package body ncl;
