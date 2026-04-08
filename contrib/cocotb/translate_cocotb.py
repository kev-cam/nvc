#!/usr/bin/env python3
"""Translate a CocoTB test file from async/await to plain sequential Python.

The transformed test calls blocking bridge functions instead of awaiting
triggers. No coroutines, no scheduler, no event loop — just sequential
function calls that suspend the simulator until the condition is met.

Transformations:
  async def test_foo(dut)        ->  def test_foo(dut)
  await Timer(10, "ns")          ->  _nvcb_wait_time(10000000)
  await RisingEdge(sig)          ->  _nvcb_wait_edge(sig, RISING)
  await FallingEdge(sig)         ->  _nvcb_wait_edge(sig, FALLING)
  cocotb.start_soon(Clock(...))  ->  _nvcb_start_clock(...)
  @cocotb.test()                 ->  no-op (test discovered via name)

Usage:
    python3 translate_cocotb.py input.py output.py
"""

import ast
import sys


_UNIT_SCALE = {
    "fs": 1,
    "ps": 1_000,
    "ns": 1_000_000,
    "us": 1_000_000_000,
    "ms": 1_000_000_000_000,
    "sec": 1_000_000_000_000_000,
    "step": 1_000,  # default to ps
}


def _const_int(node):
    """Try to extract an int from an AST constant node."""
    if isinstance(node, ast.Constant) and isinstance(node.value, (int, float)):
        return int(node.value)
    return None


def _const_str(node):
    if isinstance(node, ast.Constant) and isinstance(node.value, str):
        return node.value
    return None


# Global file index registry — file_id (int) -> absolute path.
# We choose file_ids that never end in 0 so the float repr doesn't lose
# digits when Nuitka writes the constant name.
_LOC_FILE_IDX = {}


def _next_file_id():
    """Pick the next file id that doesn't end in '0' (avoids ambiguity
    when the float repr drops trailing zeros)."""
    used = set(_LOC_FILE_IDX.values())
    candidate = 1
    while True:
        if candidate not in used and (candidate % 10) != 0:
            return candidate
        candidate += 1


def _register_file(path):
    if path not in _LOC_FILE_IDX:
        _LOC_FILE_IDX[path] = _next_file_id()
    return _LOC_FILE_IDX[path]


class LineSentinelInjector(ast.NodeTransformer):
    """Inject _nvcb_loc(<line>.<file_id>) calls before every statement.

    Encoding: float = line + (file_id / 10**ceil(log10(file_id+1))).
    Simpler: a string of <line>.<file_id> as a float, e.g.
    line 42 in file 1 -> 42.1, line 100 in file 13 -> 100.13.
    Since file_id never ends in 0, the float repr preserves all digits.

    These sentinels survive Nuitka compilation as recognisable constants
    that the post-processor finds and replaces with #line directives.
    """

    def __init__(self, source_file):
        self.file_id = _register_file(source_file)

    def visit_FunctionDef(self, node):
        node.body = self._inject(node.body)
        return self.generic_visit(node)

    def visit_AsyncFunctionDef(self, node):
        node.body = self._inject(node.body)
        return self.generic_visit(node)

    def _inject(self, body):
        out = []
        for stmt in body:
            if hasattr(stmt, 'lineno'):
                # Build float "<line>.<file_id>" via string parse so the
                # decimal places match the file_id digit count exactly.
                loc = float(f"{stmt.lineno}.{self.file_id}")
                sentinel = ast.Expr(value=ast.Call(
                    func=ast.Name(id="_nvcb_loc", ctx=ast.Load()),
                    args=[ast.Constant(value=loc)],
                    keywords=[],
                ))
                ast.copy_location(sentinel, stmt)
                ast.copy_location(sentinel.value, stmt)
                ast.copy_location(sentinel.value.func, stmt)
                ast.copy_location(sentinel.value.args[0], stmt)
                out.append(sentinel)
            out.append(stmt)
        return out


class CocotbToSync(ast.NodeTransformer):
    """Transform a CocoTB test module to sequential Python."""

    def __init__(self):
        self.test_funcs = []
        self.imports_added = False

    def visit_Module(self, node):
        # Visit all children first
        self.generic_visit(node)

        # Prepend our bridge imports (including _nvcb_loc sentinel)
        prelude = ast.parse(
            "import sys as _sys\n"
            "from cocotb.simulator import (\n"
            "    nvcb_wait_time as _nvcb_wait_time,\n"
            "    nvcb_wait_edge as _nvcb_wait_edge,\n"
            "    nvcb_start_clock as _nvcb_start_clock,\n"
            "    nvcb_stop_clock as _nvcb_stop_clock,\n"
            "    nvcb_loc as _nvcb_loc,\n"
            "    RISING as _RISING,\n"
            "    FALLING as _FALLING,\n"
            "    VALUE_CHANGE as _CHANGE,\n"
            ")\n"
            "_NVCB_TESTS = []\n"
        ).body
        node.body = prelude + node.body

        # Append a list of test functions at the bottom
        register = ast.parse(
            "_NVCB_TESTS = [" + ", ".join(self.test_funcs) + "]\n"
        ).body
        node.body.extend(register)

        return node

    def visit_AsyncFunctionDef(self, node):
        """Convert async def to def."""
        # Recurse first to process the body
        self.generic_visit(node)

        # Check if decorated with @cocotb.test()
        is_test = False
        new_decorators = []
        for dec in node.decorator_list:
            if self._is_cocotb_test_decorator(dec):
                is_test = True
                # Drop the decorator
            else:
                new_decorators.append(dec)

        sync = ast.FunctionDef(
            name=node.name,
            args=node.args,
            body=node.body,
            decorator_list=new_decorators,
            returns=node.returns,
            type_comment=node.type_comment,
        )
        ast.copy_location(sync, node)
        # Don't call fix_missing_locations here — it would propagate the
        # function's lineno down to children that haven't been given one
        # yet, breaking the source order. We call it once at the end of
        # translate() instead.

        if is_test:
            self.test_funcs.append(node.name)

        return sync

    def _is_cocotb_test_decorator(self, dec):
        """Check if decorator is @cocotb.test() or @cocotb.test."""
        if isinstance(dec, ast.Call):
            return self._is_cocotb_test_decorator(dec.func)
        if isinstance(dec, ast.Attribute):
            return (isinstance(dec.value, ast.Name)
                    and dec.value.id == "cocotb"
                    and dec.attr == "test")
        return False

    def visit_Await(self, node):
        """Replace await TRIGGER with a blocking call."""
        self.generic_visit(node)

        def with_loc(new_call):
            ast.copy_location(new_call, node)
            for arg in new_call.args:
                ast.copy_location(arg, node)
            ast.copy_location(new_call.func, node)
            return new_call

        value = node.value
        if isinstance(value, ast.Call):
            func_name = self._get_call_name(value)

            if func_name in ("Timer", "cocotb.triggers.Timer"):
                fs = self._timer_to_fs(value)
                return with_loc(ast.Call(
                    func=ast.Name(id="_nvcb_wait_time", ctx=ast.Load()),
                    args=[ast.Constant(value=fs)],
                    keywords=[],
                ))

            if func_name in ("RisingEdge", "cocotb.triggers.RisingEdge"):
                sig = value.args[0] if value.args else value.keywords[0].value
                return with_loc(ast.Call(
                    func=ast.Name(id="_nvcb_wait_edge", ctx=ast.Load()),
                    args=[
                        ast.Attribute(value=sig, attr="_handle", ctx=ast.Load()),
                        ast.Name(id="_RISING", ctx=ast.Load()),
                    ],
                    keywords=[],
                ))

            if func_name in ("FallingEdge", "cocotb.triggers.FallingEdge"):
                sig = value.args[0] if value.args else value.keywords[0].value
                return with_loc(ast.Call(
                    func=ast.Name(id="_nvcb_wait_edge", ctx=ast.Load()),
                    args=[
                        ast.Attribute(value=sig, attr="_handle", ctx=ast.Load()),
                        ast.Name(id="_FALLING", ctx=ast.Load()),
                    ],
                    keywords=[],
                ))

            if func_name in ("Edge", "ValueChange"):
                sig = value.args[0] if value.args else value.keywords[0].value
                return with_loc(ast.Call(
                    func=ast.Name(id="_nvcb_wait_edge", ctx=ast.Load()),
                    args=[
                        ast.Attribute(value=sig, attr="_handle", ctx=ast.Load()),
                        ast.Name(id="_CHANGE", ctx=ast.Load()),
                    ],
                    keywords=[],
                ))

        # Function call awaiting another async function (e.g. tl.reset())
        # Just call it directly — we transformed those to sync too
        return value

    def visit_Expr(self, node):
        """Detect cocotb.start_soon(Clock(...).start()) and replace."""
        self.generic_visit(node)
        if isinstance(node.value, ast.Call):
            call = node.value
            func_name = self._get_call_name(call)
            if func_name in ("cocotb.start_soon", "start_soon"):
                # arg is Clock(...).start() — extract the Clock(...)
                if call.args and isinstance(call.args[0], ast.Call):
                    inner = call.args[0]
                    inner_name = self._get_call_name(inner)
                    if inner_name == "start" and isinstance(inner.func, ast.Attribute):
                        clock_call = inner.func.value
                        if isinstance(clock_call, ast.Call):
                            clk_name = self._get_call_name(clock_call)
                            if clk_name == "Clock":
                                new_node = self._make_start_clock(clock_call)
                                if new_node is not None:
                                    # Preserve the original line number
                                    ast.copy_location(new_node, node)
                                    ast.copy_location(new_node.value, node)
                                return new_node
        return node

    def _make_start_clock(self, clock_call):
        """Build _nvcb_start_clock(sig._handle, period_fs)."""
        if not clock_call.args:
            return None

        sig = clock_call.args[0]
        period = clock_call.args[1] if len(clock_call.args) > 1 else None
        unit = "step"

        # Look for unit keyword
        for kw in clock_call.keywords:
            if kw.arg in ("unit", "units"):
                u = _const_str(kw.value)
                if u:
                    unit = u
        # Or third positional arg
        if len(clock_call.args) > 2:
            u = _const_str(clock_call.args[2])
            if u:
                unit = u

        period_val = _const_int(period) if period else 0
        period_fs = period_val * _UNIT_SCALE.get(unit, 1)

        return ast.Expr(value=ast.Call(
            func=ast.Name(id="_nvcb_start_clock", ctx=ast.Load()),
            args=[
                ast.Attribute(value=sig, attr="_handle", ctx=ast.Load()),
                ast.Constant(value=period_fs),
            ],
            keywords=[],
        ))

    def _timer_to_fs(self, call):
        """Convert Timer(N, "unit") to femtoseconds."""
        if not call.args:
            return 0
        n = _const_int(call.args[0]) or 0
        unit = "step"
        if len(call.args) > 1:
            u = _const_str(call.args[1])
            if u:
                unit = u
        for kw in call.keywords:
            if kw.arg in ("unit", "units"):
                u = _const_str(kw.value)
                if u:
                    unit = u
        return n * _UNIT_SCALE.get(unit, 1)

    def _get_call_name(self, call):
        """Get a dotted name from a Call node's func."""
        if isinstance(call.func, ast.Name):
            return call.func.id
        if isinstance(call.func, ast.Attribute):
            parts = []
            cur = call.func
            while isinstance(cur, ast.Attribute):
                parts.append(cur.attr)
                cur = cur.value
            if isinstance(cur, ast.Name):
                parts.append(cur.id)
            return ".".join(reversed(parts))
        return ""


def translate(src_path, dst_path):
    import os
    with open(src_path) as f:
        src = f.read()
    tree = ast.parse(src)
    tree = CocotbToSync().visit(tree)
    # Inject line-tracking sentinels (after sync transform so line numbers
    # refer to the *original* source). Use the original src_path so gdb
    # can find the source file.
    src_abs = os.path.abspath(src_path)
    tree = LineSentinelInjector(src_abs).visit(tree)
    ast.fix_missing_locations(tree)
    out = ast.unparse(tree)
    with open(dst_path, "w") as f:
        f.write("# Auto-translated from " + src_path + " by translate_cocotb.py\n")
        f.write(out)
    print(f"  translated {src_path} -> {dst_path}")


def write_locmap(dst_dir):
    """Write the consolidated file index registry for postprocess_c.py.
    Call this AFTER translating all files in a build."""
    import os
    regfile = os.path.join(dst_dir, "nvcb.locmap")
    with open(regfile, "w") as f:
        for path, idx in sorted(_LOC_FILE_IDX.items(), key=lambda kv: kv[1]):
            f.write(f"{idx}\t{path}\n")
    return regfile


if __name__ == "__main__":
    if len(sys.argv) != 3:
        print("Usage: translate_cocotb.py input.py output.py", file=sys.stderr)
        sys.exit(1)
    translate(sys.argv[1], sys.argv[2])
