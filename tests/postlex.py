"""Check postlex, placeholder terminals and retained comments across targets."""
import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor


EXE = ".exe" if os.name == "nt" else ""
HOOKS = {
    "c": "token->type = TOKEN_VIRTUAL;",
    "cpp17": "token.type = TokenType::TOKEN_VIRTUAL;",
    "python": "if token.text == '@':\n    token.type = TOKEN_VIRTUAL",
    "csharp": "token.type = Token.TOKEN_VIRTUAL;",
    "go": "token.Type = TOKEN_VIRTUAL",
    "rust": "token.kind = TOKEN_VIRTUAL;",
    "java": 'type = java.util.Arrays.asList(TOKEN_NAMES).indexOf("VIRTUAL");',
    "js": 'token.type = TOKEN_NAMES.indexOf("VIRTUAL");',
    "chali": 'token.kind = Parser::tokenKind("VIRTUAL");',
}


def run(args, cwd, text=None, ok=True):
    result = subprocess.run(
        [str(arg) for arg in args], cwd=cwd, input=text, capture_output=True,
        text=True, encoding="utf-8", errors="replace",
        env={**os.environ, "PYTHONIOENCODING": "utf-8"},
    )
    if (result.returncode == 0) != ok:
        raise AssertionError(f"{args} with input {text!r}\n{result.stdout}\n{result.stderr}")
    return result.stdout


def check(generator, workspace, lang, chali):
    source = workspace / f"{lang}.gram"
    source.write_text(
        '%tree\n%token RAW "@"\n%token VIRTUAL ""\n'
        '%trivia keep "//[^\\n]*"\n'
        '%trivia keep "/\\*([^*]|\\*+[^*/])*\\*+/"\n'
        '%if ENABLE_POSTLEX\n%postlex\n' + HOOKS[lang] +
        '\n%endpostlex\n%endif\nstart := VIRTUAL VIRTUAL;\n', encoding="utf-8",
    )
    mode = "dd" if lang in ("java", "js") else "ielr"
    target = workspace / lang
    run([generator, source, "--define", "ENABLE_POSTLEX", "--lang", lang,
         "--mode", mode, "--out-dir", target], workspace)
    binary = target / ("postlex" + EXE)
    if lang == "c":
        run(["gcc", "-std=c99", "parser.c", "parser_token.c", "parser_table.c", "main.c",
             "-lm", "-o", binary], target)
        command = [binary]
    elif lang == "cpp17":
        run(["g++", "-std=c++17", "parser.cpp", "main.cpp", "-o", binary], target)
        command = [binary]
    elif lang == "python":
        command = [sys.executable, "main.py"]
    elif lang == "csharp":
        run(["dotnet", "build", "parser.csproj", "-v:q"], target)
        command = ["dotnet", "run", "--no-build", "--project", "parser.csproj"]
    elif lang == "go":
        run(["go", "build", "-o", binary, "."], target)
        command = [binary]
    elif lang == "rust":
        command = ["cargo", "run", "--offline", "--quiet"]
    elif lang == "java":
        run(["javac", "-encoding", "UTF-8", "Parser.java"], target)
        command = ["java", "-cp", ".", "Parser"]
    elif lang == "js":
        command = ["node", "parser.ts"]
    elif chali:
        command = [chali, "run", "parser.chl"]
    else:
        generated = (target / "parser.chl").read_text(encoding="utf-8")
        assert HOOKS[lang] in generated and "tokens.push(Parser::postLex(token));" in generated
        assert "comment.offset = position;" in generated
        return "chali (generation only)"

    output = run(command, target, "@ /*注*/ @ //末\n")
    expected = "(start @ @)" if lang == "chali" else "解析成功"
    assert expected in output, (lang, output)
    for invalid in ("@\n", "?\n", "\n"):
        run(command, target, invalid, ok=False)
    check_comment_positions(lang, target)
    if lang == "python":
        check_python_modes(generator, source, workspace)
    return lang


def check_comment_positions(lang, target):
    """Check exact text and byte positions through generated runtime APIs."""
    if lang == "python":
        run([sys.executable, "-c", """
from parser import Parser
p = Parser()
assert p.run_text("@ //é\\n /*注*/ @ //末") == 0
assert [(c.text, c.offset, c.line, c.column) for c in p.commits] == [
    ("//é", 2, 1, 3), ("/*注*/", 8, 2, 2), ("//末", 18, 2, 10)
]
assert p.run_text("@@") == 0 and not p.commits
"""], target)
    elif lang == "js":
        (target / "verify.ts").write_text("""
import assert from "node:assert/strict";
import {Parser} from "./parser.ts";
const parser = new Parser("@ //é\\n /*注*/ @ //末");
assert(parser.parse());
assert.deepEqual(parser.comments().map(c => [c.text, c.offset]), [
    ["//é", 2], ["/*注*/", 8], ["//末", 18],
]);
""", encoding="utf-8")
        run(["node", "verify.ts"], target)
    elif lang == "java":
        (target / "Verify.java").write_text("""
class Verify {
    public static void main(String[] args) {
        Parser parser = new Parser("@ //é\\n /*注*/ @ //末", null);
        if (!parser.parse()) throw new AssertionError();
        var comments = parser.comments();
        if (comments.size() != 3) throw new AssertionError();
        String[] texts = {"//é", "/*注*/", "//末"};
        int[] offsets = {2, 8, 18};
        for (int i = 0; i < texts.length; ++i) {
            if (!comments.get(i).text.equals(texts[i]) || comments.get(i).offset != offsets[i])
                throw new AssertionError(comments.get(i).text + " at " + comments.get(i).offset);
        }
    }
}
""", encoding="utf-8")
        run(["javac", "-encoding", "UTF-8", "Verify.java"], target)
        run(["java", "-cp", ".", "Verify"], target)


def check_python_modes(generator, source, workspace):
    grammar = source.read_text(encoding="utf-8")
    fixture = workspace / "python-modes.gram"
    for mode, production, checks in (
        ("lex", "", """
status, tokens = p.tokenize("@ /*注*/ @ //末")
assert status == 0
assert [t.type for t in tokens] == [TOKEN_VIRTUAL, TOKEN_VIRTUAL, TOKEN_END]
"""),
        ("dd", 'start := VIRTUAL VIRTUAL RAW | VIRTUAL VIRTUAL;', """
assert p.run_text("@ /*注*/ @ //末") == 0
"""),
    ):
        fixture.write_text(
            f"%mode {mode}\n" + grammar.split("start :=")[0] + production + "\n",
            encoding="utf-8",
        )
        target = workspace / f"python-{mode}"
        run([generator, fixture, "--define", "ENABLE_POSTLEX", "--lang", "python",
             "--mode", mode, "--out-dir", target], workspace)
        run([sys.executable, "-c", """
from parser import Parser, TOKEN_VIRTUAL, TOKEN_END
p = Parser()
""" + checks + """
assert [(c.type, c.text, c.offset) for c in p.commits] == [
    ("comment", "/*注*/", 2), ("comment", "//末", 12)
]
"""], target)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--generator", type=Path, required=True)
    parser.add_argument("--langs", default=",".join(HOOKS))
    parser.add_argument("--chali", type=Path)
    args = parser.parse_args()
    generator = args.generator.resolve()
    chali = args.chali.resolve() if args.chali else None
    with tempfile.TemporaryDirectory(prefix="parseengine-postlex-") as tmp:
        workspace = Path(tmp)
        with ThreadPoolExecutor(max_workers=4) as pool:
            checks = [pool.submit(check, generator, workspace, lang, chali)
                      for lang in args.langs.split(",")]
            print("Passed: " + ", ".join(result.result() for result in checks))


if __name__ == "__main__":
    main()
