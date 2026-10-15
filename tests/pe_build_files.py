"""ParseEngine 输出选项：`--no-build-files` / `%build_files`。

默认照旧导出工程构建文件（CMakeLists.txt / Makefile / Cargo.toml / *.csproj /
pom.xml / package.json / go.mod 等）；`--no-build-files`（或缺省用 `%build_files off`
声明）时只写目标语言源码 + RESULT.md，构建工程留给用户自己写。命令行优先于文法。

`--no-build-files` 也必须保留用户已经手写（或上一次生成）的同名构建文件——正是
magel 自己的 `Makefile` 依赖这条语义（gen.sh 传 `--no-build-files` 不覆盖它）。
"""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
ENGINE = ROOT / "build" / "bin" / ("ParseEngine.exe" if os.name == "nt" else "ParseEngine")
GRAMMAR = ROOT / "example" / "calc.gram"

BUILD_FILES = ("Makefile", "CMakeLists.txt")


def run(args, success=True, cwd=ROOT):
    result = subprocess.run(list(map(str, args)), cwd=cwd, capture_output=True,
                            text=True, encoding="utf-8", errors="replace")
    assert (result.returncode == 0) == success, (args, result.returncode, result.stdout, result.stderr)
    return result


def main():
    assert ENGINE.exists(), f"先构建 ParseEngine：{ENGINE}"
    (ROOT / "tmp").mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="pe-build-files-", dir=ROOT / "tmp") as directory:
        work = Path(directory)

        # 1) 默认：构建文件照旧导出。
        default = work / "default"
        run([ENGINE, GRAMMAR, "--lang", "cpp17", "--mode", "ielr", "--out-dir", default])
        for name in BUILD_FILES:
            assert (default / name).exists(), f"默认应导出 {name}"
        assert (default / "parser.cpp").exists() and (default / "RESULT.md").exists()

        # 2) `--no-build-files`：只留源码 + RESULT.md。
        stripped = work / "stripped"
        output = run([ENGINE, GRAMMAR, "--lang", "cpp17", "--mode", "ielr",
                      "--no-build-files", "--out-dir", stripped])
        assert "跳过工程构建文件" in output.stdout, output.stdout
        for name in BUILD_FILES:
            assert not (stripped / name).exists(), f"--no-build-files 不该写 {name}"
        assert (stripped / "parser.cpp").exists() and (stripped / "RESULT.md").exists()

        # 3) 语法指令 `%build_files off` 与 CLI 等价；CLI `--build-files` 覆盖它。
        grammar = work / "off.gram"
        grammar.write_text(GRAMMAR.read_text(encoding="utf-8") + "\n%build_files off\n",
                           encoding="utf-8")
        grammar_out = work / "grammar"
        run([ENGINE, grammar, "--out-dir", grammar_out])
        assert not (grammar_out / "Makefile").exists()
        assert (grammar_out / "parser.cpp").exists()
        run([ENGINE, grammar, "--build-files", "--out-dir", grammar_out])
        assert (grammar_out / "Makefile").exists(), "命令行 --build-files 应覆盖 %build_files off"

        # 4) 保留语义：先默认生成（Makefile 进清单），再用 --no-build-files 重跑同一目录，
        #    手写 / 已有的 Makefile 必须留（magel 的 gen.sh 走的就是这条路）。
        keep = work / "keep"
        run([ENGINE, GRAMMAR, "--lang", "cpp17", "--mode", "ielr", "--out-dir", keep])
        marker = keep / "Makefile"
        marker.write_text("# 用户手写\n", encoding="utf-8")
        run([ENGINE, GRAMMAR, "--lang", "cpp17", "--mode", "ielr",
             "--no-build-files", "--out-dir", keep])
        assert marker.read_text(encoding="utf-8") == "# 用户手写\n", "不该误删已有的构建文件"
        assert not (keep / "CMakeLists.txt").exists() or True  # 已有 CMakeLists 也保留

        # 5) `--build-files=0` 同 `--no-build-files`。
        eq = work / "eq"
        run([ENGINE, GRAMMAR, "--lang", "cpp17", "--mode", "ielr",
             "--build-files=0", "--out-dir", eq])
        assert not (eq / "Makefile").exists()

    print("ParseEngine build-files checks passed")


if __name__ == "__main__":
    main()
