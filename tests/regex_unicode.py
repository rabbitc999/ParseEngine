"""验证 Unicode 正则与字节模式并存，以及 C99/C++17 的生成产物。"""
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "tmp" / "regex-unicode"
EXE = ".exe" if os.name == "nt" else ""


def run(args, **kwargs):
    return subprocess.run(args, cwd=ROOT, check=True, **kwargs)


def check_parser(generator):
    grammar = OUT / "parser.gram"
    grammar.write_text('%mode lalr\n%value_type int\n%start input\n'
                       '%token regex PAIR "(?iu)(K)-\\\\1"\n'
                       'input := PAIR { $$ = 42; };\n', encoding="utf-8")
    for backend in ("c", "std", "dfa"):
        lang = "c" if backend == "c" else "cpp17"
        target = OUT / ("parser-" + backend)
        args = [str(generator), str(grammar), "--lang", lang, "--out-dir", str(target)]
        if lang == "cpp17": args += ["--cpp-lexer", backend]
        run(args, stdout=subprocess.DEVNULL)
        driver = OUT / ("parser_check.c" if lang == "c" else "parser_check.cpp")
        if lang == "c":
            driver.write_text('#include "parser.h"\nint main(void) { ParserValue_T value = 0; '
                              'if (Parser_RunText("K-k", &value) != PARSER_OK || value != 42) return 1; '
                              'return Parser_RunText("K-A", &value) == PARSER_OK ? 2 : 0; }\n', encoding="utf-8")
            sources = sorted(path for path in target.glob("*.c") if path.name != "main.c")
            command = ["gcc", "-std=c99"]
        else:
            driver.write_text('#include "parser.hpp"\nint main() { Parser::Engine engine; '
                              'Parser::ParserValue_T value = 0; '
                              'if (engine.runText("K-k", &value) != Parser::Status::Ok || value != 42) return 1; '
                              'return engine.runText("K-A", &value) == Parser::Status::Ok ? 2 : 0; }\n', encoding="utf-8")
            sources = sorted(path for path in target.glob("*.cpp") if path.name != "main.cpp")
            command = ["g++", "-std=c++17"]
        binary = target / ("check" + EXE)
        run(command + ["-finput-charset=UTF-8", "-fexec-charset=UTF-8", "-I", str(target)] +
            [str(path) for path in sources] + [str(driver), "-lm", "-o", str(binary)])
        run([str(binary)])


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    binary = OUT / ("unicode_unit" + EXE)
    run(["g++", "-std=c++17", "-O1", "-Isource/include", "tests/regex_unicode.cpp",
         "source/src/regex_dfa.cpp", "source/src/regex_vm.cpp", "-o", str(binary)])
    run([str(binary)])
    generator = ROOT / "build" / "bin" / ("ParseEngine" + EXE)
    texts = ["Σ-ς;中文;١٢;🙂🙂;_变量1;「备注」;!abc", "　中文 ١٢;", "k-K;", "K-k;",
             "中文x;", "🙂;", "Σ-英;"]
    outputs = []
    for backend in ("c", "std", "dfa"):
        target = OUT / backend
        args = [str(generator), str(ROOT / "example" / "regex_unicode.gram"),
                "--lang", "c" if backend == "c" else "cpp17", "--out-dir", str(target), "--mermaid"]
        if backend != "c": args += ["--cpp-lexer", backend]
        run(args, stdout=subprocess.DEVNULL)
        binary = target / ("check" + EXE)
        if backend == "c":
            command = ["gcc", "-std=c99", "-Wall", "-Wextra", "-pedantic", "-I", str(target),
                       str(target / "lexer.c"), str(target / "parser_token.c"), "tests/regex_extended_c.c"]
        else:
            command = ["g++", "-std=c++17", "-I", str(target), str(target / "lexer.cpp"),
                       "tests/regex_extended_cpp.cpp"]
        run(command + ["-lm", "-o", str(binary)])
        results = [subprocess.run([str(binary)], input=text.encode("utf-8"), capture_output=True) for text in texts]
        assert all(result.returncode == 0 for result in results[:-1]), [(p.returncode, p.stdout) for p in results]
        assert results[-1].returncode == 1
        for bad in (b"\xed\xa0\x80", b"\xe0\x80\xaf", b"\xf0\x9f"):
            result = subprocess.run([str(binary)], input=bad, capture_output=True)
            assert result.returncode == 1
            results.append(result)
        outputs.append([(p.returncode, p.stdout) for p in results])
    assert outputs[0] == outputs[1] == outputs[2], outputs
    for expected in ("PAIR:Σ-ς:", "WORD:中文:", "NUMBER:١٢:", "EMOJI:🙂🙂:", "VARIABLE:_变量1:",
                     "commit:note:「备注」:", "BYTE:!abc:"):
        assert expected.encode() in outputs[0][0][1], expected
    # 自有 Unicode 模式的错误必须在生成期报出，而不是留给 std::regex 构造时失败。
    for index, pattern in enumerate([r"(?u)\p{Missing}", r"(?u)\b", r"(?u)["]):
        grammar = OUT / f"bad{index}.gram"
        grammar.write_text('%mode lex\n%token regex BAD "' + pattern.replace("\\", "\\\\") + '"\n', encoding="utf-8")
        for lang in ("c", "cpp17"):
            result = subprocess.run([str(generator), str(grammar), "--lang", lang,
                                     "--out-dir", str(OUT / f"bad{index}-{lang}")], capture_output=True)
            assert result.returncode != 0, (pattern, lang)
    check_parser(generator)
    print("Unicode C99/C++17 产物、混合字节规则、注释、trivia 和诊断对照通过")


if __name__ == "__main__":
    main()
