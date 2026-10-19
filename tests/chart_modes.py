"""End-to-end checks for generated Earley and CYK parsers.

Run from the repository root after building ParseEngine:
    python tests/chart_modes.py --langs python,cpp17,rust,csharp,go,java,js
"""

import argparse
import importlib.util
import itertools
import os
from pathlib import Path
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]
EXE = ".exe" if os.name == "nt" else ""
GENERATOR = ROOT / "build" / "bin" / ("ParseEngine" + EXE)


def command(args, *, cwd=ROOT, input_text=None, env=None):
    child_env = (env or os.environ).copy()
    child_env["PYTHONIOENCODING"] = "utf-8"
    result = subprocess.run(
        [str(arg) for arg in args], cwd=cwd, input=input_text, text=True,
        encoding="utf-8", errors="replace", capture_output=True, env=child_env,
        check=False,
    )
    if result.returncode != 0 and input_text is None:
        raise RuntimeError(f"{' '.join(map(str, args))}\n{result.stdout}\n{result.stderr}")
    return result


def build(grammar, mode, lang):
    out = ROOT / "tmp" / "chart-mode-tests" / f"{grammar}-{mode}-{lang}"
    out.mkdir(parents=True, exist_ok=True)
    command([GENERATOR, ROOT / "example" / f"{grammar}.gram",
             "--out-dir", out, "--lang", lang, "--mode", mode])
    if lang == "python":
        return [sys.executable, out / "main.py"], out
    if lang == "cpp17":
        command(["g++", "-std=c++17", "-O1", "parser.cpp", "main.cpp",
                 "-o", "demo" + EXE], cwd=out)
        return [out / ("demo" + EXE)], out
    if lang == "csharp":
        command(["dotnet", "build", "parser.csproj", "-v:q"], cwd=out)
        return ["dotnet", "run", "--no-build", "--project", out / "parser.csproj"], out
    if lang == "go":
        env = os.environ.copy()
        env["GOCACHE"] = str(ROOT / "tmp" / "chart-mode-tests" / "go-cache")
        executable = out / ("parser_demo" + EXE)
        command(["go", "build", "-o", executable, "."], cwd=out, env=env)
        return [executable], out
    if lang == "java":
        command(["javac", "-encoding", "UTF-8", "Parser.java"], cwd=out)
        return ["java", "-cp", out, "Parser"], out
    if lang == "js":
        command(["node", "--check", "parser.ts"], cwd=out)
        return ["node", out / "parser.ts"], out
    env = os.environ.copy()
    # 不同文法工程的同名二进制不能共享 target，否则未变源码会误运行旧产物。
    env["CARGO_TARGET_DIR"] = str(out / "target")
    command(["cargo", "build", "--offline"], cwd=out, env=env)
    return [Path(env["CARGO_TARGET_DIR"]) / "debug" / ("parser_demo" + EXE)], out


def check_output(run_args, cwd, text, expected, *, success=True, exact=None):
    result = command(run_args, cwd=cwd, input_text=text + "\n")
    output = result.stdout.replace("\r\n", "\n")
    if success and result.returncode != 0:
        raise AssertionError(f"{run_args} rejected {text!r}: {output}\n{result.stderr}")
    if not success and result.returncode == 0:
        raise AssertionError(f"{run_args} accepted invalid input {text!r}: {output}")
    if expected not in output:
        raise AssertionError(f"{run_args} on {text!r}: missing {expected!r}\n{output}")
    if exact is not None and output.strip() != exact.strip():
        raise AssertionError(f"{run_args} on {text!r}: output differs\n{output}")


def compare_chart_to_glr():
    modules = []
    for mode in ("glr", "earley", "cyk"):
        _, out = build("chart_nullable", mode, "python")
        spec = importlib.util.spec_from_file_location(f"chart_{mode}", out / "parser.py")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        modules.append(module)
    for length in range(5):
        for parts in itertools.product(("a", "b", "c"), repeat=length):
            text = " ".join(parts)
            outcomes = []
            for module in modules:
                engine = module.Parser()
                status = engine.run_glr_tree(text)
                outcomes.append((status == module.STATUS_OK,
                                 engine.glr_result_count()))
            if outcomes[1] != outcomes[2]:
                raise AssertionError(f"chart/GLR disagreement for {text!r}: {outcomes}")
            if outcomes[1][0] and outcomes[0] != outcomes[1]:
                raise AssertionError(f"result reconstruction disagrees for {text!r}: {outcomes}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--langs", default="python,cpp17,rust,csharp,go,java,js")
    args = parser.parse_args()
    langs = args.langs.split(",")
    if not GENERATOR.exists():
        parser.error(f"build the generator first: {GENERATOR}")
    english_expected = (ROOT / "example" / "english-clauses.expected").read_text(encoding="utf-8")
    nullable_expected = (ROOT / "example" / "chart_nullable.expected").read_text(encoding="utf-8")
    for mode in ("earley", "cyk"):
        for lang in langs:
            if lang not in ("python", "cpp17", "rust", "csharp", "go", "java", "js"):
                parser.error(f"unsupported language: {lang}")
            runner, cwd = build("english-clauses", mode, lang)
            if lang in ("java", "js"):
                check_output(runner, cwd, "the man saw the girl with the telescope",
                             "解析成功，共 1 个结果")
            else:
                check_output(runner, cwd, "the man saw the girl with the telescope",
                             "解析成功，共 2 个结果", exact=english_expected)
            runner, cwd = build("chart_nullable", mode, lang)
            check_output(runner, cwd, "a b c b b", "解析成功，共 1 个结果",
                         exact=nullable_expected)
            check_output(runner, cwd, "a a b c", "解析成功，共 1 个结果")
            check_output(runner, cwd, "", "解析成功，共 1 个结果")
            check_output(runner, cwd, "a b c c", "解析失败", success=False)
        if "cpp17" in langs:
            runner, cwd = build("glr_expr", mode, "cpp17")
            result = command(runner, cwd=cwd, input_text="2 + 3 * 4\n")
            output = result.stdout.replace("\r\n", "\n")
            if result.returncode != 0 or "结果[0]: 14" not in output or "结果[1]: 20" not in output:
                raise AssertionError(f"semantic actions failed for {mode}: {output}")
        if "csharp" in langs:
            runner, cwd = build("chart_value_csharp", mode, "csharp")
            value_expected = (ROOT / "example" / "chart_value_csharp.expected").read_text(encoding="utf-8")
            check_output(runner, cwd, "2 + 3 * 4", "结果[1]: 20", exact=value_expected)
        if "go" in langs:
            runner, cwd = build("chart_value_csharp", mode, "go")
            check_output(runner, cwd, "2 + 3 * 4", "结果[1]: 20")
    if "python" in langs:
        compare_chart_to_glr()
    if "csharp" in langs:
        runner, cwd = build("chart_value_csharp", "glr", "csharp")
        check_output(runner, cwd, "2 + 3 * 4", "结果[1]: 20")
        check_output(runner, cwd, "2 + + 3", "结果[0]: 5")
    print(f"Earley/CYK checks passed: {', '.join(langs)}")


if __name__ == "__main__":
    main()
