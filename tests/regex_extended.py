"""验证四类扩展正则及 C99 / C++17 std/dfa 生成产物。"""
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "tmp" / "regex-extended"
EXE = ".exe" if os.name == "nt" else ""


def run(args, **kwargs):
    return subprocess.run(args, cwd=ROOT, check=True, **kwargs)


def check_parser(generator):
    grammar = OUT / "parser.gram"
    grammar.write_text('%mode lalr\n%value_type int\n%start input\n'
                       '%token regex PAIR "(?i)(ab)\\\\1"\n'
                       'input := PAIR { $$ = 42; };\n', encoding="utf-8")
    for lang in ("c", "cpp17"):
        target = OUT / ("parser-" + lang)
        args = [str(generator), str(grammar), "--lang", lang, "--out-dir", str(target)]
        if lang == "cpp17":
            args += ["--cpp-lexer", "dfa"]
        run(args, stdout=subprocess.DEVNULL)
        driver = OUT / ("parser_check.c" if lang == "c" else "parser_check.cpp")
        if lang == "c":
            driver.write_text('#include "parser.h"\nint main(void) { ParserValue_T value = 0; '
                              'if (Parser_RunText("abAB", &value) != PARSER_OK || value != 42) return 1; '
                              'return Parser_RunText("abAC", &value) == PARSER_OK ? 2 : 0; }\n', encoding="utf-8")
            sources = sorted(path for path in target.glob("*.c") if path.name != "main.c")
            command = ["gcc", "-std=c99"]
        else:
            driver.write_text('#include "parser.hpp"\nint main() { Parser::Engine engine; '
                              'Parser::ParserValue_T value = 0; '
                              'if (engine.runText("abAB", &value) != Parser::Status::Ok || value != 42) return 1; '
                              'return engine.runText("abAC", &value) == Parser::Status::Ok ? 2 : 0; }\n', encoding="utf-8")
            sources = sorted(path for path in target.glob("*.cpp") if path.name != "main.cpp")
            command = ["g++", "-std=c++17"]
        binary = target / ("check" + EXE)
        run(command + ["-I", str(target)] + [str(path) for path in sources] +
            [str(driver), "-lm", "-o", str(binary)])
        run([str(binary)])


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    unit = OUT / ("vm_std" + EXE)
    run(["g++", "-std=c++17", "-O1", "-Isource/include", "tests/regex_vm_std.cpp",
         "source/src/regex_dfa.cpp", "source/src/regex_vm.cpp", "-o", str(unit)])
    run([str(unit)])
    generator = ROOT / "build" / "bin" / ("ParseEngine" + EXE)
    inputs = ["Ab-aB: foo; bar; note-NOTE skip;", "foo: foobar; foo;", "ABC-abc;",
              "note-NOTE\nfoo; skip;", "z;", "Ab-cd:"]
    outputs = []
    for backend in ("c", "std", "dfa"):
        target = OUT / backend
        args = [str(generator), str(ROOT / "example" / "regex_extended.gram"),
                "--lang", "c" if backend == "c" else "cpp17", "--out-dir", str(target), "--mermaid"]
        if backend != "c":
            args += ["--cpp-lexer", backend]
        run(args, stdout=subprocess.DEVNULL)
        binary = target / ("check" + EXE)
        if backend == "c":
            run(["gcc", "-std=c99", "-Wall", "-Wextra", "-pedantic", "-I", str(target),
                 str(target / "lexer.c"), str(target / "parser_token.c"),
                 "tests/regex_extended_c.c", "-lm", "-o", str(binary)])
            assert "PeRegexRun" in (target / "parser_token.c").read_text(encoding="utf-8")
        else:
            run(["g++", "-std=c++17", "-Wall", "-Wextra", "-I", str(target), str(target / "lexer.cpp"),
                 "tests/regex_extended_cpp.cpp", "-o", str(binary)])
        results = []
        for text in inputs:
            result = subprocess.run([str(binary), text], capture_output=True)
            results.append((result.returncode, result.stdout))
        assert all(status == 0 for status, _ in results[:-1]), results
        assert results[-1][0] == 1
        outputs.append(results)
    assert outputs[0] == outputs[1] == outputs[2], outputs
    first = outputs[0][0][1]
    assert b"PAIR:Ab-aB:1:1" in first and b"SAFE:foo:" in first
    assert b"commit:note:note-NOTE:" in first and b":skip:" not in first
    # 必须拒绝零宽规则和不存在的反向引用。
    for index, pattern in enumerate([r"\b", r"(?=a)", r"(a)\2", r"(?<=a)b"]):
        grammar = OUT / f"invalid{index}.gram"
        escaped = pattern.replace("\\", "\\\\")
        grammar.write_text(f'%mode lex\n%token regex BAD "{escaped}"\n', encoding="utf-8")
        result = subprocess.run([str(generator), str(grammar), "--lang", "c",
                                 "--out-dir", str(OUT / f"invalid{index}")], capture_output=True)
        assert result.returncode != 0, pattern
    check_parser(generator)
    print("四类扩展正则、C99/C++17 产物、注释收集、诊断及 Mermaid 回归通过")


if __name__ == "__main__":
    main()
