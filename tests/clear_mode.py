"""End-to-end checks for ParseEngine's generated-file cleanup mode.

Run from the repository root after building ParseEngine:
    python tests/clear_mode.py
"""

import os
from pathlib import Path
import shutil
import subprocess


ROOT = Path(__file__).resolve().parents[1]
EXE = ".exe" if os.name == "nt" else ""
GENERATOR = ROOT / "build" / "bin" / ("ParseEngine" + EXE)
WORK = ROOT / "tmp" / "clear-mode-tests"
MANIFEST = ".parseengine-manifest"


def command(args, *, succeeds=True):
    result = subprocess.run(
        [str(arg) for arg in args], cwd=ROOT, text=True, encoding="utf-8",
        errors="replace", capture_output=True, check=False,
    )
    if (result.returncode == 0) != succeeds:
        raise AssertionError(
            f"command returned {result.returncode}: {' '.join(map(str, args))}\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    return result


def generate(out, lang):
    command([
        GENERATOR, ROOT / "example" / "ll0_demo.gram",
        "--out-dir", out, "--lang", lang, "--mode", "ll0",
    ])


def assert_only_user_file(out):
    names = sorted(path.name for path in out.iterdir())
    if names != ["keep.txt"]:
        raise AssertionError(f"clear left unexpected entries: {names}")


def main():
    if not GENERATOR.exists():
        raise SystemExit(f"build the generator first: {GENERATOR}")
    shutil.rmtree(WORK, ignore_errors=True)
    WORK.mkdir(parents=True)
    try:
        out = WORK / "project"
        generate(out, "cpp17")
        if not (out / MANIFEST).is_file() or not (out / "parser.cpp").is_file():
            raise AssertionError("C++17 generation did not create files and manifest")
        (out / "keep.txt").write_text("user data\n", encoding="utf-8")

        generate(out, "python")
        if (out / "parser.cpp").exists():
            raise AssertionError("regeneration did not remove stale C++17 output")
        if not (out / "parser.py").is_file() or not (out / "keep.txt").is_file():
            raise AssertionError("regeneration removed a current or user-owned file")

        result = command([GENERATOR, "clear", "--out-dir", out])
        if "3" not in result.stdout:
            raise AssertionError(f"unexpected clear summary: {result.stdout!r}")
        assert_only_user_file(out)

        generate(out, "python")
        command([GENERATOR, "--mode", "clear", "--out-dir", out])
        assert_only_user_file(out)

        nested = WORK / "nested"
        generate(nested, "rust")
        (nested / "src" / "keep.txt").write_text("nested user data\n", encoding="utf-8")
        command([GENERATOR, "clear", "--out-dir", nested])
        remaining = sorted(
            path.relative_to(nested).as_posix()
            for path in nested.rglob("*") if path.is_file()
        )
        if remaining != ["src/keep.txt"]:
            raise AssertionError(f"clear mishandled a nested user file: {remaining}")

        command([GENERATOR, "clear", "--out-dir", WORK / "missing"])
        command([
            GENERATOR, ROOT / "example" / "ll0_demo.gram",
            "--mode", "clear", "--out-dir", out,
        ], succeeds=False)
        command([
            GENERATOR, "clear", "--mode", "ll1", "--out-dir", out,
        ], succeeds=False)

        victim = WORK / "victim.txt"
        victim.write_text("keep me\n", encoding="utf-8")
        bad = WORK / "bad"
        bad.mkdir()
        (bad / MANIFEST).write_text(
            "# ParseEngine generated files v1\n../victim.txt\n", encoding="utf-8",
        )
        command([GENERATOR, "clear", "--out-dir", bad], succeeds=False)
        if victim.read_text(encoding="utf-8") != "keep me\n":
            raise AssertionError("unsafe manifest escaped the output directory")
    finally:
        shutil.rmtree(WORK, ignore_errors=True)
    print("clear mode checks passed")


if __name__ == "__main__":
    main()
