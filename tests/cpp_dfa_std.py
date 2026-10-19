"""对照 C++ std/dfa 产物及 C/C++ 共用正则编译器。"""
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "tmp" / "cpp-dfa-std"
EXE = ".exe" if os.name == "nt" else ""


def run(command, **kwargs):
    return subprocess.run(command, cwd=ROOT, check=True, **kwargs)


def compare_lexer(generator, index, pattern, text, fast, extra=""):
    grammar = OUT / f"case{index}.gram"
    grammar.write_text(f'%mode lex\n%token regex WORD "{pattern}"\n{extra}', encoding="utf-8")
    results = []
    for backend in ("std", "dfa"):
        target = OUT / f"case{index}-{backend}"
        run([str(generator), str(grammar), "--lang", "cpp17", "--cpp-lexer", backend,
             "--out-dir", str(target)], stdout=subprocess.DEVNULL)
        source = target / "lexer.cpp"
        if backend == "dfa":
            assert ("kLexClassMap[256]" in source.read_text(encoding="utf-8")) == fast
        binary = target / ("check" + EXE)
        run(["g++", "-std=c++17", "-O0", str(source), str(target / "main.cpp"), "-o", str(binary)])
        result = subprocess.run([str(binary)], input=text.encode() if isinstance(text, str) else text, capture_output=True)
        results.append((result.returncode, result.stdout, result.stderr))
    assert results[0] == results[1], (pattern, text, results)


def compare_parser(generator):
    results = []
    for backend in ("std", "dfa"):
        target = OUT / f"parser-{backend}"
        run([str(generator), str(ROOT / "example" / "space_demo.gram"),
             "--lang", "cpp17", "--cpp-lexer", backend, "--out-dir", str(target)],
            stdout=subprocess.DEVNULL)
        binary = target / ("check" + EXE)
        run(["g++", "-std=c++17", "-O0", str(target / "parser.cpp"),
             str(target / "main.cpp"), "-o", str(binary)])
        outputs = []
        for text in (b"a b", b"ab", b"a  b", b"a\tb"):
            result = subprocess.run([str(binary)], input=text, capture_output=True)
            outputs.append((result.returncode, result.stdout, result.stderr))
        results.append(outputs)
    assert results[0] == results[1], results


def compare_c(generator):
    # 用真实 C99 产物验证共享编译器的上下限修复。
    for pattern, text, expected in [("a{2,3}", "aaaa", b'"aaa"'),
                                     ("a{2,}", "aaaa", b'"aaaa"')]:
        grammar = OUT / "c-repeat.gram"
        grammar.write_text(f'%mode lex\n%token regex WORD "{pattern}"\n', encoding="utf-8")
        target = OUT / ("c-bounded" if ",3" in pattern else "c-unbounded")
        run([str(generator), str(grammar), "--lang", "c", "--out-dir", str(target)],
            stdout=subprocess.DEVNULL)
        binary = target / ("check" + EXE)
        run(["gcc", "-std=c99", str(target / "lexer.c"), str(target / "parser_token.c"), str(target / "main.c"), "-lm", "-o", str(binary)])
        result = run([str(binary)], input=text.encode() if isinstance(text, str) else text, capture_output=True)
        word = next(line for line in result.stdout.splitlines() if line.startswith(b"WORD"))
        assert word.split()[1] == expected, (pattern, result.stdout)


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    unit = OUT / ("regex_dfa_std" + EXE)
    run(["g++", "-std=c++17", "-O1", "-Isource/include", "tests/regex_dfa_std.cpp",
         "source/src/regex_dfa.cpp", "source/src/regex_vm.cpp", "-o", str(unit)])
    run([str(unit)])
    cases = [
        ("[a-z]+", "abc xyz", True),
        ("a|ab", "ab", False),
        ("a+?", "aaa", False),
        ("a{2,3}", "aaaa", False),
        ("a{2,}", "aaaa", False),
        (".", "中文", False),
        ("a$", "ab", False),
        ("a(?=b)", "ab", False),
        ("[^a]+", "中文b", False),
        ("[a-z]+", "abc\u3000xyz", True),
    ]
    generator = ROOT / "build" / "bin" / ("ParseEngine" + EXE)
    for index, (pattern, text, fast) in enumerate(cases):
        compare_lexer(generator, index, pattern, text, fast)
    compare_lexer(generator, 10, "[b]+", "aa ccc ddd b", False,
                  '%identifier "a+?"\n%commit note "c+?"\n%trivia "d+?"\n')
    compare_lexer(generator, 11, "b", "aaa b", True, '%identifier "[a-z]+"\n')
    compare_lexer(generator, 12, "[a-z]+", b"\xe0\x81\xa1", True)
    compare_c(generator)
    compare_parser(generator)
    print("解析模式空白处理、共用正则编译器、13 组 C++ std/dfa 对照及 2 组 C99 产物回归通过")


if __name__ == "__main__":
    main()
