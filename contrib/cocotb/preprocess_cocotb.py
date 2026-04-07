#!/usr/bin/env python3
"""Preprocess CocoTB package for NVC direct bridge compilation.

Copies the cocotb package, replaces the simulator C extension with our
Python shim (nvc_simulator.py), and copies test files into a build directory.

Usage:
    python3 preprocess_cocotb.py <test_file> [extra_files...] -o <build_dir>
"""

import argparse
import os
import shutil
import site
import sys


def find_cocotb_package():
    """Find the installed cocotb package directory."""
    try:
        import cocotb
        return os.path.dirname(cocotb.__file__)
    except ImportError:
        # Search common locations
        for base in site.getsitepackages() + [site.getusersitepackages()]:
            p = os.path.join(base, 'cocotb')
            if os.path.isdir(p):
                return p
    return None


def preprocess(test_files, build_dir, bridge_dir):
    cocotb_src = find_cocotb_package()
    if not cocotb_src:
        print("ERROR: cocotb package not found", file=sys.stderr)
        sys.exit(1)

    print(f"CocoTB source: {cocotb_src}")
    print(f"Build dir: {build_dir}")

    os.makedirs(build_dir, exist_ok=True)

    # 1. Copy cocotb package (Python files only, skip .so and __pycache__)
    dst_cocotb = os.path.join(build_dir, 'cocotb')
    if os.path.exists(dst_cocotb):
        shutil.rmtree(dst_cocotb)

    def ignore_binaries(directory, files):
        ignored = []
        for f in files:
            if f == '__pycache__':
                ignored.append(f)
            elif f.endswith('.so') or f.endswith('.pyd'):
                ignored.append(f)
            elif f == 'libs':
                ignored.append(f)
        return ignored

    shutil.copytree(cocotb_src, dst_cocotb, ignore=ignore_binaries)
    print(f"  Copied cocotb package (Python files only)")

    # 2. Replace simulator module with our shim
    shim_src = os.path.join(bridge_dir, 'nvc_simulator.py')
    shim_dst = os.path.join(dst_cocotb, 'simulator.py')
    shutil.copy2(shim_src, shim_dst)
    print(f"  Installed nvc_simulator.py as cocotb/simulator.py")

    # 3. Copy test files
    for tf in test_files:
        dst = os.path.join(build_dir, os.path.basename(tf))
        shutil.copy2(tf, dst)
        print(f"  Copied {tf}")

    # 4. Create empty __init__.py if needed for test module discovery
    init_file = os.path.join(build_dir, '__init__.py')
    if not os.path.exists(init_file):
        with open(init_file, 'w') as f:
            pass

    print("Preprocessing complete.")
    return dst_cocotb


def main():
    parser = argparse.ArgumentParser(description='Preprocess CocoTB for NVC bridge')
    parser.add_argument('test_files', nargs='+', help='Test files to include')
    parser.add_argument('-o', '--output', required=True, help='Build output directory')
    args = parser.parse_args()

    bridge_dir = os.path.dirname(os.path.abspath(__file__))
    preprocess(args.test_files, args.output, bridge_dir)


if __name__ == '__main__':
    main()
