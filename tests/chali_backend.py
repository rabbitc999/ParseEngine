"""ChaLi 原生后端回归；--chali 指定可运行的 ChaLi 编译器。"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
WORK = ROOT / "build" / "chali-regression"


def run(args, *, cwd=WORK, input=None, ok=True):
    result = subprocess.run([str(a) for a in args], cwd=cwd, input=input,
                            text=True, encoding="utf-8", errors="replace",
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if (result.returncode == 0) != ok:
        raise AssertionError(f"{args}\n{result.stdout}")
    return result.stdout


def main():
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument("--chali", required=True)
    cli.add_argument("--generator", default=str(ROOT / "build/bin" / ("ParseEngine.exe" if os.name == "nt" else "ParseEngine")))
    args = cli.parse_args()
    WORK.mkdir(parents=True, exist_ok=True)
    generator = Path(args.generator).resolve()
    chali = Path(args.chali).resolve()

    def generate(text, name, mode, ok=True):
        grammar = WORK / (name + ".gram")
        grammar.write_text(text, encoding="utf-8")
        out = WORK / name
        output = run([generator, grammar, "--lang", "chali", "--mode", mode,
                      "--out-dir", out], ok=ok)
        return out, output

    cases = []

    def execute(text, name, mode, body):
        cases.append((text, name, mode, body))

    def run_case(text, name, mode, body):
        text += "\n%main\nvoid main() {\n" + body + '\nprintln("passed");\n}\n'
        out, _ = generate(text, name, mode)
        output = run([chali, "run", out / "parser.chl"])
        assert "passed" in output, output
        # 确定性产物，且自包含单文件。
        before = (out / "parser.chl").read_bytes()
        generate(text, name, mode)
        assert before == (out / "parser.chl").read_bytes()
        assert not (out / "parser.cpp").exists()

    lexicalOnly = """%lang chali
%mode lex
%token A "@"
%trivia keep "//[^\\n]*"
%postlex
token.text = "matched";
%endpostlex
"""
    execute(lexicalOnly, "lex", "lex", """
ParserLexResult result = Parser::tokenize("@ //note");
assert(result.ok && result.tokens.len() == 2 && result.comments.len() == 1);
assert(result.tokens[0].kind == Parser::tokenKind("A") && result.tokens[0].text == "matched");
assert(result.tokens[1].kind == Parser::tokenKind("END") && result.tokens[1].offset == 8);
assert(result.comments[0].text == "//note" && result.comments[0].offset == 2);
assert(!Parser::tokenize("@?").ok);
""")
    simple = """%lang chali
%token A "@"
%token B "+"
s := A B { $$ = 7; };
"""
    for mode in ("ll0", "lr0"):
        execute(simple, mode, mode, """
assert(Parser::runText("@+").value == 7);
assert(!Parser::runText("@").ok);
""")
    backtracking = """%lang chali
%token A "@"
%token B "+"
s := prefix B { $$ = $1; };
prefix := A B { $$ = 2; } | A { $$ = 1; };
"""
    execute(backtracking, "dd", "dd", """
assert(Parser::runText("@+").value == 1);
assert(Parser::runText("@++").value == 2);
assert(!Parser::runText("@").ok);
""")

    for mode in ("ll1", "slr", "lalr", "lr1", "ielr"):
        example = "chali_ll1" if mode == "ll1" else "chali_lr1"
        text = (ROOT / "example" / (example + ".gram")).read_text(encoding="utf-8")
        expected = 6 if mode == "ll1" else 7
        expression = "1+2+3" if mode == "ll1" else "1+(2*3)"
        execute(text, mode, mode, f'''
ParserResult result = Parser::runText(" {expression} //comment");
assert(result.ok && result.value == {expected});
assert(!Parser::runText("1+").ok);
assert(!Parser::runText("1 2").ok);
ParserResult bad = Parser::runText("1+@");
assert(!bad.ok && bad.offset == 2);
assert(!Parser::runText("").ok);
assert(Parser::tokenKind("unknown") == 0-1);
List<ParserToken> tokens;
assert(!Parser::runTokens(tokens).ok);
ParserToken number; number.kind = Parser::tokenKind("NUMBER"); number.value = 42;
ParserToken end; end.kind = Parser::tokenKind("END");
tokens.push(number); tokens.push(end);
assert(Parser::runTokens(tokens).ok && Parser::runTokens(tokens).value == 42);
tokens.push(end); assert(!Parser::runTokens(tokens).ok);
''')

    # 非 LALR(1) 文法：IELR 必须保留拆分后的两种归约上下文。
    split = '''%lang chali
%start s
%token A "a"
%token B "b"
%token C "c"
%token D "d"
%token E "e"
s := A x D { $$ = $2*10+1; } | B y D { $$ = $2*10+1; }
   | A y E { $$ = $2*10+2; } | B x E { $$ = $2*10+2; };
x := C { $$ = 1; };
y := C { $$ = 2; };
'''
    execute(split, "split", "ielr", '''
assert(Parser::runText("a c d").value == 11);
assert(Parser::runText("b c d").value == 21);
assert(Parser::runText("a c e").value == 22);
assert(Parser::runText("b c e").value == 12);
''')
    _, conflict = generate(split, "split-lalr", "lalr")
    assert "Warning" in conflict or "警告" in conflict, conflict

    # 空输入、树、声明顺序、最长匹配和 %identifier。
    lexical = '''%lang chali
%tree
%start s
%token IF "if"
%token EQ "="
%token EQEQ "=="
%identifier "[A-Za-z_][A-Za-z0-9_]*"
s := IF EQEQ VARIABLE | %empty;
'''
    for mode in ("ll1", "ielr"):
        execute(lexical, "lexical-" + mode, mode, '''
assert(Parser::runText("").ok);
ParserResult result = Parser::runText("if==iffy");
assert(result.ok && result.tree == "(s if == iffy)");
assert(!Parser::runText("iffy==if").ok);
''')

    # 自定义语义类型与原生用户代码、epilogue。
    custom = '''%lang chali
%value_type int64
%start s
s := %empty { $$ = nativeValue(); /* $99 留在注释中 */ };
%epilogue
int64 nativeValue() { String marker = "$1"; return 23; }
'''
    execute(custom, "custom", "ielr", 'assert(Parser::runText("").value == 23);')

    llk = """%lang chali
%lookahead 2
%token A "@"
%token B "+"
%token C "-"
s := A B { $$ = 1; } | A C { $$ = 2; } | A { $$ = 3; };
"""
    execute(llk, "llk", "llk", """
assert(Parser::runText("@+").value == 1);
assert(Parser::runText("@-").value == 2);
assert(Parser::runText("@").value == 3);
assert(!Parser::runText("@@").ok);
""")

    lrk = """%lang chali
%lookahead 2
%token A "+"
%token B "-"
%token C "@"
s := x A A { $$ = $1; } | y A B { $$ = $1; };
x := C { $$ = 4; };
y := C { $$ = 5; };
"""
    for mode in ("lrk", "slrk", "lalrk"):
        execute(lrk, mode, mode, """
assert(Parser::runText("@++").value == 4);
assert(Parser::runText("@+-").value == 5);
assert(!Parser::runText("@+").ok);
""")

    glr = """%lang chali
%token SUB "-"
%trivia keep "//[^\\n]*"
e := e SUB e { $$ = $1 - $3; } | NUMBER { $$ = $1; };
"""
    for mode in ("glr", "earley", "cyk"):
        execute(glr, mode, mode, """
ParserResult r = Parser::runText("8-3-2 // note");
assert(r.ok && !r.truncated && r.alternatives.len() == 2);
float64 a = r.alternatives[0].value;
float64 b = r.alternatives[1].value;
assert((a == 3 && b == 7) || (a == 7 && b == 3));
assert(r.value == a && r.tree == r.alternatives[0].tree);
assert(r.comments.len() == 1 && r.comments[0].offset == 6);
assert(!Parser::runText("8-").ok);
ParserResult many = Parser::runText("8-3-2-1-1-1");
assert(many.ok && !many.truncated && many.alternatives.len() == 42);
""")
    nullable = """%lang chali
s := a { $$ = $1; } | b { $$ = $1; };
a := %empty { $$ = 1; };
b := %empty { $$ = 2; };
"""
    for mode in ("glr", "earley", "cyk"):
        execute(nullable, mode + "-nullable", mode, """
ParserResult r = Parser::runText("");
assert(r.ok && r.alternatives.len() == 2);
""")
    generate("%lang chali\ns := s { $$ = $1; } | %empty { $$ = 1; };\n",
             "glr-cycle", "glr", ok=False)

    base = '%lang chali\n%start s\ns := NUMBER { $$ = $1; };\n'
    for name, extra in (("sync", '%sync NUMBER\n'), ("commit", '%commit Keep "abc"\n'),
                        ("regex", '%token regex NUMBER "(?i)[a-z]+"\n')):
        generate(extra + base, "reject-" + name, "ielr", ok=False)
    generate(base.replace("$1", "$2"), "bad-action", "ielr", ok=False)
    with ThreadPoolExecutor(max_workers=3) as pool:
        results = [pool.submit(run_case, *case) for case in cases]
        for result in results:
            result.result()
    print("ChaLi：十六种模式、k 元组、GLR 歧义、IELR 拆分、原生动作、词法、树、Token API 与拒绝路径全部通过")


if __name__ == "__main__":
    main()
