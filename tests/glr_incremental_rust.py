"""Run after tools/lock.sh build: python tests/glr_incremental_rust.py."""

import argparse
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
EXE = ".exe" if os.name == "nt" else ""
OUT = ROOT / "tmp" / "glr-incremental-rust-tests"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--generate-only", action="store_true",
                        help="Generate the fixture for a toolchain in another environment")
    parser.add_argument("--mode", choices=("glr", "earley", "cyk"), default="glr")
    args = parser.parse_args()
    base = OUT if args.mode == "glr" else OUT / args.mode
    for grammar, driver, out in [
        ("tests/glr_incremental_rust.gram", "glr_incremental_rust.rs", base),
        ("example/chart_nullable.gram", "glr_forest_nullable.rs", base / "nullable"),
    ]:
        out.mkdir(parents=True, exist_ok=True)
        subprocess.run([
            str(ROOT / "build" / "bin" / ("ParseEngine" + EXE)), str(ROOT / grammar),
            "--lang", "rust", "--mode", args.mode, "--out-dir", str(out),
        ], check=True, cwd=ROOT)
        bin_dir = out / "src" / "bin"
        bin_dir.mkdir(exist_ok=True)
        shutil.copyfile(ROOT / "tests" / driver, bin_dir / "incremental_check.rs")
        shutil.copyfile(ROOT / "tests" / "glr_forest_check.rs", out / "src" / "forest_checks.rs")
        if not args.generate_only:
            subprocess.run(["cargo", "run", "--offline", "--quiet", "--bin", "incremental_check"],
                           check=True, cwd=out)


if __name__ == "__main__":
    main()
