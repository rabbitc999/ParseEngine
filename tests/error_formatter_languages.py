"""生成并执行六个托管语言后端的错误提示回归；默认覆盖全部支持模式。"""
import argparse
import importlib.util
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "tmp/error-formatter-languages"
MODES = "lex,ll0,ll1,llk,lr0,slr,slrk,lalr,lalrk,lr1,lrk,ielr,dd,glr,earley,cyk"
EXE = ".exe" if os.name == "nt" else ""


def run(args, cwd=ROOT, env=None):
    result = subprocess.run([str(x) for x in args], cwd=cwd, env=env,
                            capture_output=True, text=True, encoding="utf-8", errors="replace")
    if result.returncode:
        raise RuntimeError(f"{args}\n{result.stdout}\n{result.stderr}")


CSHARP = r'''
using System;
using System.Collections.Generic;
class Program {
    static int Invoke(Parser p, string input) { INVOKE }
    static void Main() {
        foreach (string input in INPUTS) {
            var p = new Parser(); var other = new Parser();
            int status = Invoke(p, input);
            var original = p.lastError(); var originals = p.errors().ToArray();
            if (status >= 0 || original == null) throw new Exception("baseline");
            int calls = 0;
            p.setErrorFormatter((code, line, column, message) => {
                var expected = originals[calls];
                if (code != expected.code || line != expected.line || column != expected.column || message != expected.message)
                    throw new Exception("metadata");
                ++calls; return "custom";
            });
            if (Invoke(p, input) != status || calls != originals.Length || p.lastError().message != "custom") throw new Exception("callback");
            if (Invoke(other, input) != status || other.lastError().message != original.message || calls != originals.Length) throw new Exception("isolation");
            if (p.lastError().message != "custom" || calls != originals.Length) throw new Exception("query");
            if (Invoke(p, "a") != 0 || p.errors().Count != 0 || calls != originals.Length) throw new Exception("success");
            p.setErrorFormatter((code, line, column, message) => "");
            if (Invoke(p, input) != status || p.lastError().message != "") throw new Exception("empty");
            p.setErrorFormatter(null);
            if (Invoke(p, input) != status || p.lastError().message != original.message) throw new Exception("reset");
        }
    }
}
'''

GO = r'''
package main
func invoke(p *Parser, input string) int32 { INVOKE }
func main() {
    for _, input := range INPUTS {
        p, other := NewParser(), NewParser()
        status := invoke(p, input); original := *p.LastError(); originals := append([]*ParserError(nil), p.Errors()...)
        if status >= 0 { panic("baseline") }
        calls := 0
        p.SetErrorFormatter(func(code, line, column int32, message string) string {
            expected := originals[calls]
            if code != expected.Code || line != expected.Line || column != expected.Column || message != expected.Message { panic("metadata") }
            calls++; return "custom"
        })
        if invoke(p, input) != status || calls != len(originals) || p.LastError().Message != "custom" { panic("callback") }
        if invoke(other, input) != status || other.LastError().Message != original.Message || calls != len(originals) { panic("isolation") }
        if p.LastError().Message != "custom" || calls != len(originals) { panic("query") }
        if invoke(p, "a") != 0 || len(p.Errors()) != 0 || calls != len(originals) { panic("success") }
        p.SetErrorFormatter(func(code, line, column int32, message string) string { return "" })
        if invoke(p, input) != status || p.LastError().Message != "" { panic("empty") }
        p.SetErrorFormatter(nil)
        if invoke(p, input) != status || p.LastError().Message != original.Message { panic("reset") }
    }
}
'''

RUST = r'''
mod parser; mod token;
use parser::Parser;
use std::cell::Cell;
use std::rc::Rc;
fn invoke(p: &mut Parser, input: &str) -> i32 { INVOKE }
fn main() {
    for input in INPUTS {
        let mut p = Parser::new(); let mut other = Parser::new();
        let status = invoke(&mut p, input);
        let original = p.last_error().unwrap().clone();
        assert!(status < 0);
        let expected = p.errors().clone(); let expected_count = expected.len(); let calls = Rc::new(Cell::new(0)); let captured = calls.clone();
        p.set_error_formatter(Some(Box::new(move |code, line, column, message| {
            let item = &expected[captured.get()];
            assert_eq!((code, line, column, message), (item.code, item.line, item.column, item.message.as_str()));
            captured.set(captured.get() + 1); "custom".to_string()
        })));
        assert_eq!(invoke(&mut p, input), status); assert_eq!(calls.get(), expected_count);
        assert_eq!(p.last_error().unwrap().message, "custom");
        assert_eq!(invoke(&mut other, input), status); assert_eq!(other.last_error().unwrap().message, original.message);
        assert_eq!(calls.get(), expected_count);
        assert_eq!(invoke(&mut p, "a"), 0); assert!(p.errors().is_empty()); assert_eq!(calls.get(), expected_count);
        p.set_error_formatter(Some(Box::new(|_, _, _, _| String::new())));
        assert_eq!(invoke(&mut p, input), status); assert!(p.last_error().unwrap().message.is_empty());
        p.set_error_formatter(None);
        assert_eq!(invoke(&mut p, input), status); assert_eq!(p.last_error().unwrap().message, original.message);
    }
}
'''

JAVA = r'''
public class Check {
    static void require(boolean ok) { if (!ok) throw new AssertionError(); }
    public static void main(String[] args) {
        for (String input : new String[]{"", "b"}) {
            Parser p = new Parser(input), other = new Parser(input);
            require(!p.parse()); String original = p.errorMessage(); int[] calls = {0};
            p.setErrorFormatter((code, line, column, message) -> {
                require(code < 0 && line >= 1 && column >= 1 && message.equals(original));
                calls[0]++; return "custom";
            });
            require(!p.parse() && calls[0] == 1 && p.errorMessage().equals("custom"));
            require(p.errorMessage().equals("custom") && calls[0] == 1);
            require(!other.parse() && other.errorMessage().equals(original));
            p.setErrorFormatter((code, line, column, message) -> "");
            require(!p.parse() && p.errorMessage().isEmpty());
            p.setErrorFormatter(null);
            require(!p.parse() && p.errorMessage().equals(original));
        }
        require(new Parser("a", (code, line, column, message) -> { throw new AssertionError("success"); }).parse());
        int[] calls = {0};
        try {
            new Parser("?", (code, line, column, message) -> {
                require(code == -29 && line == 1 && column == 1 && !message.isEmpty());
                calls[0]++; return "lex-custom";
            });
            throw new AssertionError("lex accepted");
        } catch (IllegalArgumentException error) {
            require(error.getMessage().equals("lex-custom") && calls[0] == 1);
        }
    }
}
'''

JS = r'''
import assert from "node:assert/strict";
import { Parser } from "./parser.ts";
for (const input of ["", "b"]) {
    const p = new Parser(input), other = new Parser(input);
    assert.equal(p.parse(), false); const original = p.errorMessage(); let calls = 0;
    p.setErrorFormatter((code, line, column, message) => {
        assert(code < 0); assert(line >= 1); assert(column >= 1); assert.equal(message, original);
        ++calls; return "custom";
    });
    assert.equal(p.parse(), false); assert.equal(calls, 1); assert.equal(p.errorMessage(), "custom");
    assert.equal(p.errorMessage(), "custom"); assert.equal(calls, 1);
    assert.equal(other.parse(), false); assert.equal(other.errorMessage(), original);
    p.setErrorFormatter(() => ""); assert.equal(p.parse(), false); assert.equal(p.errorMessage(), "");
    p.setErrorFormatter(null); assert.equal(p.parse(), false); assert.equal(p.errorMessage(), original);
}
assert.equal(new Parser("a", () => { throw new Error("success"); }).parse(), true);
let calls = 0;
assert.throws(() => new Parser("?", (code, line, column, message) => {
    assert.equal(code, -29); assert.equal(line, 1); assert.equal(column, 1); assert(message);
    ++calls; return "lex-custom";
}), { message: "lex-custom" });
assert.equal(calls, 1);
'''


def python_check(target, mode):
    spec = importlib.util.spec_from_file_location("formatter_" + mode, target / "parser.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    def invoke(p, text):
        return p.tokenize(text)[0] if mode == "lex" else p.run_text(text)
    for text in (["?"] if mode == "lex" else ["?", "", "b"]):
        p, other = module.Parser(), module.Parser()
        status = invoke(p, text)
        original = p.last_error
        originals = list(p.errors)
        calls = []
        def formatter(code, line, column, message):
            expected = originals[len(calls)]
            assert (code, line, column, message) == (expected.code, expected.line, expected.column, expected.message)
            calls.append(1)
            return "custom"
        p.set_error_formatter(formatter)
        assert status < 0 and invoke(p, text) == status
        assert p.last_error.message == "custom" and len(calls) == len(originals)
        assert invoke(other, text) == status and other.last_error.message == original.message
        assert len(calls) == len(originals)
        for actual, expected in zip(p.errors, originals):
            assert all(getattr(actual, key) == getattr(expected, key)
                       for key in module.ParserError.__slots__ if key != "message")
        assert invoke(p, "a") == 0 and not p.errors and len(calls) == len(originals)
        p.set_error_formatter(lambda *_: "")
        assert invoke(p, text) == status and p.last_error.message == ""
        p.set_error_formatter(None)
        assert invoke(p, text) == status and p.last_error.message == original.message


def check(lang, mode, grammar):
    target = OUT / (lang + "-" + mode)
    run([ROOT / ("build/bin/ParseEngine" + EXE), grammar, "--lang", lang, "--out-dir", target])
    if lang == "python":
        python_check(target, mode)
        return
    inputs = '["?"]' if mode == "lex" else '["?", "", "b"]'
    if lang == "csharp":
        invoke = 'return p.tokenize(input, new List<Token>());' if mode == 'lex' else 'return p.runText(input);'
        (target / "Program.cs").write_text(CSHARP.replace('INVOKE', invoke).replace('INPUTS', 'new string[]' + inputs.replace('[', '{').replace(']', '}')), encoding='utf-8')
        run(['dotnet', 'build', 'parser.csproj', '-v:q'], target)
        run(['dotnet', 'run', '--no-build', '--project', 'parser.csproj'], target)
    elif lang == "go":
        invoke = 'status, _ := p.Tokenize(input); return status' if mode == 'lex' else 'return p.RunText(input)'
        (target / 'main.go').write_text(GO.replace('INVOKE', invoke).replace('INPUTS', '[]string' + inputs.replace('[', '{').replace(']', '}')), encoding='utf-8')
        env = os.environ.copy()
        env['GOCACHE'] = str(OUT / 'go-cache')
        run(['go', 'build', '-o', 'check' + EXE, '.'], target, env)
        run([target / ('check' + EXE)], target)
    elif lang == "rust":
        invoke = 'p.tokenize(input).0' if mode == 'lex' else 'p.run_text(input)'
        (target / 'src/main.rs').write_text(RUST.replace('INVOKE', invoke).replace('INPUTS', inputs), encoding='utf-8')
        env = os.environ.copy()
        env['CARGO_TARGET_DIR'] = str(OUT / 'rust-target')
        run(['cargo', 'run', '--offline', '--quiet'], target, env)
    elif lang == "java":
        (target / 'Check.java').write_text(JAVA, encoding='utf-8')
        run(['javac', '-encoding', 'UTF-8', 'Parser.java', 'Check.java'], target)
        run(['java', '-cp', target, 'Check'], target)
    else:
        (target / 'check.ts').write_text(JS, encoding='utf-8')
        run(['node', 'check.ts'], target)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--langs', default='python,csharp,rust,go,java,js')
    parser.add_argument('--modes', default=MODES)
    args = parser.parse_args()
    OUT.mkdir(parents=True, exist_ok=True)
    count = 0
    for mode in args.modes.split(','):
        grammar = OUT / (mode + '.gram')
        grammar.write_text('%mode ' + mode + '\n%token A "a"\n%token B "b"\n' +
                           ('' if mode == 'lex' else '%tree\n%start input\ninput := A;\n'), encoding='utf-8')
        for lang in args.langs.split(','):
            if lang in ('java', 'js') and mode not in ('dd', 'earley', 'cyk'):
                continue
            check(lang, mode, grammar)
            count += 1
            print(f'{lang}/{mode} 自定义错误提示通过', flush=True)
    print(f'{count} 组生成工程全部通过', flush=True)


if __name__ == '__main__':
    main()
