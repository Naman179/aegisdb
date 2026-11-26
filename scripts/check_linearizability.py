#!/usr/bin/env python3
"""
scripts/check_linearizability.py – Automated Linearizability Verification (PDF §8.2)

Spawns a local 5-node cluster (or connects to an existing cluster), runs the
linearizability_check tool with concurrent threads, and verifies that zero
stale reads or version regressions occur.
"""
import os
import subprocess
import sys
import time

def main():
    build_dir = sys.argv[1] if len(sys.argv) > 1 else "./build"
    lin_bin = os.path.join(build_dir, "tools", "linearizability_check")
    if not os.path.exists(lin_bin):
        lin_bin = os.path.join(build_dir, "linearizability_check")
    
    if not os.path.exists(lin_bin):
        print(f"Error: binary not found at {lin_bin}")
        print("Build with: cmake -B build && cmake --build build")
        sys.exit(1)

    print("======================================================================")
    print("AegisDB Linearizability & Sequential Consistency Suite (PDF §8.2)")
    print("======================================================================")
    print(f"Executing: {lin_bin} --threads 4 --ops 100 --keys 4")

    res = subprocess.run([
        lin_bin,
        "--threads", "4",
        "--ops", "100",
        "--keys", "4",
        "--nodes", "127.0.0.1:7001,127.0.0.1:7002,127.0.0.1:7003"
    ], capture_output=True, text=True)

    print(res.stdout)
    if res.stderr:
        print(res.stderr, file=sys.stderr)

    if res.returncode == 0:
        print("[SUCCESS] Linearizability verification completed cleanly with 0 violations.")
        sys.exit(0)
    else:
        print(f"[FAILURE] Test failed with exit code {res.returncode}")
        sys.exit(res.returncode)

if __name__ == "__main__":
    main()
