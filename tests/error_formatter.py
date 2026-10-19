"""验证 C/C++ 生成运行时的自定义错误提示及恢复默认行为。"""
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "tmp" / "error-formatter"


def run(args):
    subprocess.run([str(arg) for arg in args], cwd=ROOT, check=True)


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    generator = ROOT / "build/bin" / ("ParseEngine.exe" if os.name == "nt" else "ParseEngine")
    for mode in ("lex", "ll1", "lalr", "dd", "glr", "earley", "cyk"):
        grammar = OUT / (mode + ".gram")
        grammar.write_text('%mode ' + mode + '\n%value_type int\n%token A "a"\n'
                           + ('' if mode == 'lex' else '%start input\ninput := A { $$ = 1; };\n'), encoding="utf-8")
        for lang in ("c", "cpp17"):
            if lang == "c" and mode in ("glr", "earley", "cyk"):
                continue
            target = OUT / (mode + "-" + lang)
            run([generator, grammar, "--lang", lang, "--out-dir", target])
            header = "lexer" if mode == "lex" else "parser"
            if lang == "c":
                source = r'''
#include "HEADER.h"
#include <string.h>
static int calls;
static void format(int32_t code, uint32_t line, uint32_t column,
    const char *message, char *output, uint32_t capacity, void *data) {
    if (code >= 0 || line != 1 || column != 1 || !message[0] || !data) return;
    ++calls; memset(output, 'X', capacity); /* 运行时必须补上终止零。 */
}
int main(void) {
    ParserValue_T value = 0;
    LEXDECL
    Parser_SetErrorFormatter(format, &calls);
    if (INVOKE >= 0 || calls != 1) return 1;
    if (strlen(Parser_GetLastError()->message) != sizeof(Parser_GetLastError()->message)-1 ||
        Parser_GetLastError()->message[sizeof(Parser_GetLastError()->message)-1] != 0 ||
        Parser_GetLastError()->message[0] != 'X') return 2;
    Parser_SetErrorFormatter(NULL, NULL);
    if (INVOKE >= 0 || calls != 1 || Parser_GetLastError()->message[0] == 'X') return 3;
    return 0;
}
'''.replace('HEADER', header)
                source = source.replace('LEXDECL', 'ParserLexer_T lexer; ParserToken_T token;' if mode == 'lex' else '')
                source = source.replace('INVOKE', '(Parser_LexInit(&lexer, "?"), Parser_NextToken(&lexer, &token))' if mode == 'lex' else 'Parser_RunText("?", &value)')
                compiler, ext, standard = 'gcc', '.c', '-std=c99'
            else:
                source = r'''
#include "HEADER.hpp"
int main() {
    Parser::Engine engine, other;
    Parser::ParserValue_T value = 0;
    Parser::ParserMemory::Vector<Parser::Token> tokens;
    int calls = 0;
    engine.setErrorFormatter([&](int code, unsigned line, unsigned column,
        const Parser::ParserMemory::String &message) {
        if (code < 0 && line == 1 && column == 1 && !message.empty()) ++calls;
        return Parser::ParserMemory::String("custom");
    });
    if (INVOKE == Parser::Status::Ok || calls != 1 || engine.lastError()->message() != "custom") return 1;
    if (OTHER_CALL == Parser::Status::Ok || other.lastError()->message() == "custom" || calls != 1) return 4;
    engine.setErrorFormatter({});
    if (INVOKE == Parser::Status::Ok || calls != 1 || engine.lastError()->message() == "custom") return 2;
    engine.setErrorFormatter([](int, unsigned, unsigned, const Parser::ParserMemory::String &) {
        return Parser::ParserMemory::String();
    });
    if (INVOKE == Parser::Status::Ok || !engine.lastError()->message().empty()) return 3;
    return 0;
}
'''.replace('HEADER', header).replace('INVOKE', 'engine.tokenize("?", &tokens)' if mode == 'lex' else 'engine.runText("?", &value)')
                compiler, ext, standard = 'g++', '.cpp', '-std=c++17'
            source = source.replace('OTHER_CALL', 'other.tokenize(\"?\", &tokens)' if mode == 'lex' else 'other.runText(\"?\", &value)')
            driver = target / ('check' + ext)
            source = source.replace('int main(void)', 'int main(int argc, char **argv)').replace('int main()', 'int main(int argc, char **argv)')
            source = source.replace('\"?\"', '(argc > 1 ? argv[1] : \"?\")')
            driver.write_text(source, encoding='utf-8')
            sources = [path for path in target.glob('*' + ext) if path.name not in ('main' + ext, driver.name)]
            binary = target / ('check.exe' if os.name == 'nt' else 'check')
            run([compiler, standard, '-I', target, *sources, driver, '-lm', '-o', binary])
            run([binary])
            if mode != "lex":
                run([binary, ""])
    print('自定义错误提示跨 C/C++ 词法及解析模式回归通过')


if __name__ == '__main__':
    main()
