"""ChaLi 原生后端回归；--chali 指定可运行的 ChaLi 编译器。"""
import argparse
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

    def execute(text, name, mode, body):
        text += "\n%main\nvoid main() {\n" + body + '\nprintln("passed");\n}\n'
        out, _ = generate(text, name, mode)
        output = run([chali, "run", out / "parser.chl"])
        assert "passed" in output, output
        # 确定性产物，且自包含单文件。
        before = (out / "parser.chl").read_bytes()
        generate(text, name, mode)
        assert before == (out / "parser.chl").read_bytes()
        assert not (out / "parser.cpp").exists()

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

    base = '%lang chali\n%start s\ns := NUMBER { $$ = $1; };\n'
    for mode in ("lex", "ll0", "llk", "lr0", "slrk", "lalrk", "lrk", "dd", "glr", "earley", "cyk"):
        _, output = generate(base, "unsupported-" + mode, mode, ok=False)
        assert "chali" in output, output
    for name, extra in (("sync", '%sync NUMBER\n'), ("commit", '%commit Keep "abc"\n'),
                        ("regex", '%token regex NUMBER "(?i)[a-z]+"\n')):
        generate(extra + base, "reject-" + name, "ielr", ok=False)
    generate(base.replace("$1", "$2"), "bad-action", "ielr", ok=False)
    print("ChaLi：五种模式、IELR 拆分、原生动作、词法、树、Token API 与拒绝路径全部通过")


if __name__ == "__main__":
    main()
