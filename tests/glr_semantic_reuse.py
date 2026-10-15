"""Run after tools/lock.sh build: python tests/glr_semantic_reuse.py."""

import argparse
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
EXE = ".exe" if os.name == "nt" else ""
OUT = ROOT / "tmp" / "glr-semantic-reuse-tests"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--mode", choices=("glr", "earley", "cyk"), default="glr")
    args = parser.parse_args()
    base = OUT if args.mode == "glr" else OUT / args.mode
    for grammar, driver, out in [
        ("tests/glr_semantic_reuse.gram", "glr_semantic_reuse.cpp", base),
        ("example/chart_nullable.gram", "glr_forest_nullable.cpp", base / "nullable"),
    ]:
        out.mkdir(parents=True, exist_ok=True)
        subprocess.run([
            str(ROOT / "build" / "bin" / ("ParseEngine" + EXE)), str(ROOT / grammar),
            "--lang", "cpp17", "--mode", args.mode, "--out-dir", str(out),
        ], check=True, cwd=ROOT)
        binary = out / ("semantic_reuse_check" + EXE)
        subprocess.run([
            "g++", "-std=c++17", "-O1", "-I", str(out),
            str(out / "parser.cpp"), str(ROOT / "tests" / driver), "-o", str(binary),
        ], check=True, cwd=ROOT)
        subprocess.run([str(binary)], check=True, cwd=ROOT)


if __name__ == "__main__":
    main()
