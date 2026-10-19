"""验证纯校验不写盘、严格兼容性及全部目标的逐规则报告。"""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
EXE = ".exe" if os.name == "nt" else ""
GENERATOR = ROOT / "build/bin" / ("ParseEngine" + EXE)


def run(args, ok=True, input_text=None):
    result = subprocess.run([str(x) for x in args], cwd=ROOT, capture_output=True,
                            encoding="utf-8", errors="replace", input=input_text)
    if (result.returncode == 0) != ok:
        raise AssertionError(f"{args}\n{result.stdout}\n{result.stderr}")
    return result.stdout + result.stderr


def main():
    (ROOT / "tmp").mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="regex-compatibility-", dir=ROOT / "tmp") as directory:
        base = Path(directory)
        unit = base / ("unit" + EXE)
        run(["g++", "-std=c++17", "-O1", "-Isource/include",
             "tests/regex_compatibility.cpp", "source/src/regex_compatibility.cpp",
             "source/src/regex_dfa.cpp", "source/src/regex_vm.cpp", "-o", unit])
        run([unit])
        grammar, out = base / "test.gram", base / "output"

        def write(pattern="[a-z]+", extra=""):
            # 各个关键字、词法规则和产生式均由完整文法声明。
            escaped = pattern.replace("\\", "\\\\").replace('"', '\\"')
            grammar.write_text('%tree\n%token regex WORD "' + escaped +
                               '"\n' + extra + '\nstart := WORD ;\n', encoding="utf-8")

        def invoke(lang="cpp17", *flags, ok=True):
            return run([GENERATOR, grammar, "--mode", "dd", "--lang", lang,
                        "--out-dir", out, *flags], ok)

        write()
        for lang in ("c", "cpp17", "python", "csharp", "rust", "go", "java", "js", "gram"):
            text = invoke(lang, "--check", "--strict")
            assert "token/WORD" in text and not out.exists()
        out.mkdir()
        sentinel = out / "user.txt"
        sentinel.write_bytes(b"keep me")
        invoke("cpp17", "--cpp-lexer", "dfa")
        report = (out / "RESULT.md").read_text(encoding="utf-8")
        assert "<code>cpp-dfa</code>" in report
        before = {p.name: p.read_bytes() for p in out.iterdir()}
        invoke("c", "--check")
        assert before == {p.name: p.read_bytes() for p in out.iterdir()}
        write(extra='%identifier "[A-Z]+"\n%trivia "#[^\\n]*"\n%commit note "@[a-z]+"')
        text = invoke("c", "--check")
        assert all(name in text for name in ("identifier/VARIABLE", "trivia/skip", "commit/note"))
        report_file = base / "analysis.txt"
        invoke("c", "--check", "--report", report_file)
        assert report_file.is_file()
        for flags in (("clear", "--check"), ("--mode", "clear", "--strict")):
            run([GENERATOR, *flags, "--out-dir", out], ok=False)
        assert before == {p.name: p.read_bytes() for p in out.iterdir()}
        # 文档中的块注释模式包含分组后的重复；漏写该量词不是 DFA 缺陷。
        pattern = r"\(\*([^*]|\*+[^*)])*\*+\)"
        grammar.write_text('%mode lex\n%token regex COMMENT "' +
                           pattern.replace("\\", "\\\\") + '"\n', encoding="utf-8")
        for lang, engine in (("c", "std"), ("cpp17", "std"), ("cpp17", "dfa"), ("cpp17", "vm")):
            target = base / (lang + "-" + engine)
            run([GENERATOR, grammar, "--lang", lang, "--cpp-lexer", engine, "--out-dir", target])
            suffix = ".c" if lang == "c" else ".cpp"
            binary = target / ("comment" + EXE)
            run(["gcc" if lang == "c" else "g++", "-std=c99" if lang == "c" else "-std=c++17",
                 *target.glob("*" + suffix), "-o", binary])
            for sample in ("(**)", "(* words *)", "(* a ** ) more *)", "(* 中文 *)"):
                assert "COMMENT" in run([binary], input_text=sample)
        write("a(?=b)", '%token regex OTHER "[a-z]+"')
        text = invoke("cpp17", "--check", "--cpp-lexer", "dfa")
        assert text.count("<code>std::regex</code>") == 2
        text = invoke("c", "--check")
        assert text.count("<code>ordered-vm</code>") == 2
        for strict in ("--strict", "--strict-regex"):
            invoke("cpp17", "--check", strict, ok=False)
        for lang in ("rust", "go"):
            text = invoke(lang, "--check", ok=False)
            assert "不支持" in text
        assert before == {p.name: p.read_bytes() for p in out.iterdir()}
        write("(?u)\\p{L}+")
        for lang in ("python", "js", "csharp", "go"):
            invoke(lang, "--check", ok=False)
        for lang in ("c", "cpp17"):
            assert "Unicode 17.0" in invoke(lang, "--check", "--strict")
        write("(?<=a)b")
        invoke("cpp17", "--check", ok=False)
        # 共享运行时不能识别的语法须打印不支持规则，正常返回失败。
        text = invoke("cpp17", "--check", "--cpp-lexer", "vm", ok=False)
        assert "token/WORD" in text
        assert before == {p.name: p.read_bytes() for p in out.iterdir()}
    print("正则兼容性、九种目标报告、严格模式和纯校验文件保护全部通过")


if __name__ == "__main__":
    main()

