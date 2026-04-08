#!/usr/bin/env python3
"""Post-process Nuitka-generated C to add #line directives.

translate_cocotb.py injects _nvcb_loc(<line>.<file_id>) calls before
each Python statement. The float encoding (e.g. 42.1 = line 42 file 1)
appears in Nuitka's generated C as a recognisable constant name like
`const_tuple_float_42_1_tuple`.

This script scans the generated C for those references and inserts
`#line N "file.py"` directives so that the C code implementing each
Python statement is attributed to the original Python source line.
That lets gdb show Python source files when stepping, and lets users
set breakpoints on Python source lines.

Strategy:
- Find each `CALL_FUNCTION_*(... const_tuple_float_<line>_<file>_tuple)`.
- That call is wrapped in a Nuitka basic block starting with `{`.
- After the closing `}` of that block, insert a `#line N "file.py"`
  directive AND join the following block's body onto a single physical
  line so all of its C statements get attributed to the Python line.

Usage:
    python3 postprocess_c.py <build_dir> <locmap_file>
"""

import os
import re
import sys


# Match a call site that references our sentinel constant
FLOAT_REF_RE = re.compile(
    r'CALL_FUNCTION_\w+\([^)]*const_tuple_float_(\d+)_(\d+)_tuple'
)


def load_locmap(path):
    """Read translate_cocotb.py's file index registry."""
    idx_to_file = {}
    if not os.path.exists(path):
        return idx_to_file
    with open(path) as f:
        for line in f:
            line = line.rstrip('\n')
            if not line or '\t' not in line:
                continue
            idx_str, fpath = line.split('\t', 1)
            idx_to_file[int(idx_str)] = fpath
    return idx_to_file


def patch_file(c_file, idx_to_file):
    """Insert #line directives and merge Python-statement C blocks.

    For each call site referencing our sentinel:
      1. Find the closing `}` of the call's basic block.
      2. After that, find the next `{ ... }` block (the next Python
         statement's C code).
      3. Insert `#line N "file.py"` before the block.
      4. Join the block's body onto a single physical line so all of
         its C lines get attributed to the same Python source line.
    """
    with open(c_file) as f:
        lines = f.readlines()

    out = []
    i = 0
    n_patched = 0

    while i < len(lines):
        line = lines[i]
        m = FLOAT_REF_RE.search(line)
        if not m:
            out.append(line)
            i += 1
            continue

        try:
            lineno = int(m.group(1))
            file_id = int(m.group(2))
        except ValueError:
            out.append(line)
            i += 1
            continue

        fname = idx_to_file.get(file_id, f"<file{file_id}>")

        # Append the call line itself
        out.append(line)
        i += 1

        # Find closing `}` of this call's block
        depth = 0
        # Walk backwards to find the opening `{`... actually we know we're
        # already inside the block, so just walk forward to find matching `}`.
        # Count braces from here forward; the block we're in started before us.
        # Use 1 as initial depth (we're inside one block).
        depth = 1
        while i < len(lines) and depth > 0:
            cur = lines[i]
            depth += cur.count('{') - cur.count('}')
            out.append(cur)
            i += 1
            if depth <= 0:
                break

        # Now we should be at the start of the next block (or some other code).
        # Skip blank lines.
        while i < len(lines) and lines[i].strip() == '':
            out.append(lines[i])
            i += 1

        # If the next significant line is `{`, this is the next Python
        # statement's basic block. Insert #line directive and join.
        if i < len(lines) and lines[i].strip() == '{':
            # Insert #line directive
            out.append(f'#line {lineno} "{fname}"\n')
            # Append the `{` line
            out.append(lines[i])
            i += 1
            # Collect lines until matching `}`
            block_lines = []
            depth = 1
            while i < len(lines) and depth > 0:
                cur = lines[i]
                depth += cur.count('{') - cur.count('}')
                block_lines.append(cur)
                i += 1
                if depth <= 0:
                    break
            # Join into one physical line (strip newlines, separate with space).
            # Strip C++ "//" comments because joining onto one line would
            # cause them to swallow subsequent code (including labels).
            cleaned = []
            for l in block_lines:
                s = l.rstrip('\n').strip()
                if not s:
                    continue
                # Strip "//"-style comments (be careful of // inside strings,
                # but Nuitka generated code doesn't put // in strings).
                idx = s.find('//')
                if idx >= 0:
                    s = s[:idx].rstrip()
                if s:
                    cleaned.append(s)
            joined = ' '.join(cleaned)
            out.append(joined + '\n')
            n_patched += 1

    if n_patched > 0:
        with open(c_file, 'w') as f:
            f.writelines(out)

    return n_patched


def process(build_dir, locmap_file):
    if not os.path.isdir(build_dir):
        print(f"ERROR: not a directory: {build_dir}", file=sys.stderr)
        return 1

    idx_to_file = load_locmap(locmap_file)
    print(f"Loaded {len(idx_to_file)} file mappings from {locmap_file}")

    total = 0
    for fname in os.listdir(build_dir):
        if not fname.endswith('.c'):
            continue
        path = os.path.join(build_dir, fname)
        n = patch_file(path, idx_to_file)
        if n > 0:
            print(f"  {fname}: patched {n} call sites")
            total += n

    print(f"Total: {total} #line directives inserted")
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 3:
        print("Usage: postprocess_c.py <build_dir> <locmap_file>", file=sys.stderr)
        sys.exit(1)
    sys.exit(process(sys.argv[1], sys.argv[2]))
