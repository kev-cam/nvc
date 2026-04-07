# Reporting Bugs

This directory holds bug reproducers in progress. When you find a bug,
please package it as a self-contained gzipped tarball that unpacks into
a directory matching the pattern below.

## Format

```
YYYY-MM-DD-short-description/
├── README.md          # what's broken, how to reproduce, backtrace
├── *.vhd / *.sv       # minimal source files
├── tb_*.vhd           # testbench (if needed)
└── (optional) work/   # pre-elaborated library if needed
```

Pack with:

```bash
tar czf 2026-04-06-concurrent-assign-crash.tar.gz \
    2026-04-06-concurrent-assign-crash/
```

The archive should unpack with `tar xzf` straight into the `bugs/`
directory and be runnable from there with no further setup beyond what
the README documents.

## README contents

A bug README should include:

1. **Summary** — one-line description of the failure
2. **Reproduction** — exact command sequence (analyse, elaborate, run)
3. **Backtrace** — gdb output if it's a crash
4. **Analysis** — your best guess at the root cause
5. **Affected designs** — what reproduces it, what doesn't
6. **NVC version** — `nvc --version` output

See `test/regress/issue_async_xfer/README.md` for a worked example
(this bug was fixed in commit ef0c086de and the reproducer moved to
the regression suite).

## Lifecycle

1. **Drop the archive in `bugs/`** and unpack
2. **Investigate and fix** — work in the unpacked directory
3. **Once fixed**:
   - If the reproducer is small and self-contained, add a stripped-down
     version to `test/regress/` and update `test/regress/testlist.txt`
   - If it depends on external libraries (sv2vhdl, iverilog output, etc.),
     move the directory to `test/regress/issue_<name>/` with a README
     documenting how to recreate the environment
4. **Delete the original `bugs/` entry** once it lives in `test/regress/`

## Triage tips

- Build NVC with `--enable-debug` to keep asserts enabled
- Many crashes are NULL-pointer dereferences where an assert *would*
  have caught the issue if `NDEBUG` weren't set in release builds
- `gdb -ex run -ex bt --args nvc -r foo` is your friend
- For value-corruption bugs, try `--trace` to dump every signal event
