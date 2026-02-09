-- Benchmark: 32-bit ripple-carry adder using 3D logic lookup tables
-- Runs many iterations to measure gate evaluation performance

library work;
use work.logic3d_gates_pkg.all;

entity logic3d_bench is
    generic (
        NUM_ITERATIONS : positive := 10000
    );
end entity;

architecture test of logic3d_bench is

    -- Full adder using lookup tables
    procedure full_adder(
        a, b, cin : in logic3d;
        sum, cout : out logic3d
    ) is
        variable p, g : logic3d;
    begin
        p := a xor b;
        g := a and b;
        sum := p xor cin;
        cout := g or (p and cin);
    end procedure;

    -- 32-bit ripple carry adder
    procedure add32(
        a, b : in logic3d;  -- Use single value, replicate for benchmark
        cin  : in logic3d;
        sum  : out logic3d;
        cout : out logic3d
    ) is
        variable s, c : logic3d;
        variable aa, bb : logic3d;
    begin
        c := cin;
        aa := a;
        bb := b;
        -- 32 full adders in series
        for i in 0 to 31 loop
            full_adder(aa, bb, c, s, c);
            -- Rotate inputs for variety
            aa := aa xor s;
            bb := bb xor c;
        end loop;
        sum := s;
        cout := c;
    end procedure;

begin

    process
        variable a, b, cin : logic3d;
        variable sum, cout : logic3d;
        variable start_time, end_time : time;
        variable count : natural := 0;
    begin
        report "Starting 3D logic benchmark: " & integer'image(NUM_ITERATIONS) & " iterations";
        report "Each iteration: 32-bit ripple adder (32 full adders, ~160 gate ops)";

        start_time := now;

        a := L3D_1;
        b := L3D_0;
        cin := L3D_0;

        for i in 1 to NUM_ITERATIONS loop
            add32(a, b, cin, sum, cout);
            -- Feed back to create data dependency
            cin := cout;
            if is_one(sum) then
                a := a xor L3D_1;
            else
                b := b xor L3D_1;
            end if;
            count := count + 1;
        end loop;

        end_time := now;

        report "Completed " & integer'image(count) & " iterations";
        report "Final sum=" & integer'image(sum) & " cout=" & integer'image(cout);
        report "Elapsed simulation time: " & time'image(end_time - start_time);

        -- Verify we got expected result (not X or Z)
        assert is_strong(sum)
            report "ERROR: sum is not strong (X or Z leaked)"
            severity failure;
        assert is_strong(cout)
            report "ERROR: cout is not strong (X or Z leaked)"
            severity failure;

        report "PASSED: Benchmark complete, no X/Z propagation";
        wait;
    end process;

end architecture;
