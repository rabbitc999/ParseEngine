"""验证共享正则预算及 C/C++ 生成产物中的资源错误传播。"""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "tmp/regex-budget"
EXE = ".exe" if os.name == "nt" else ""


def run(args, input_text=None):
    result = subprocess.run([str(x) for x in args], cwd=ROOT, capture_output=True,
                            text=True, encoding="utf-8", errors="replace", input=input_text)
    if result.returncode:
        raise RuntimeError(f"{args} (退出码 {result.returncode})\n{result.stdout}\n{result.stderr}")
    return result


def check_generated_oom(target, lang):
    """强制生成运行时的分配器失败，检查错误码和回调收到的诊断。"""
    hook = target / "allocation_hook.h"
    hook.write_text('#include <stddef.h>\nvoid *BudgetMalloc(size_t bytes);\n', encoding="utf-8")
    if lang == "c":
        source = ('#include "lexer.h"\nvoid *BudgetMalloc(size_t bytes) { (void)bytes; return NULL; }\n'
                  'int main(void) { ParserLexer_T lexer; ParserToken_T token; '
                  'Parser_LexInit(&lexer, "aaaab"); '
                  'if (Parser_NextToken(&lexer, &token) != PARSER_ERR_REGEX_MEMORY) return 1; '
                  'return Parser_GetLastError()->code == -32 && '
                  'Parser_GetLastRegexResult()->status == PE_REGEX_NO_MEMORY ? 0 : 2; }\n')
        compiler, extension, standard = "gcc", ".c", "-std=c99"
    else:
        source = ('#include "lexer.hpp"\nvoid *BudgetMalloc(size_t) { return nullptr; }\n'
                  'int main() { Parser::Engine engine; int observed = 0; '
                  'engine.setErrorFormatter([&](int code, unsigned, unsigned, const Parser::ParserMemory::String &message) { '
                  'observed = code; return message; }); '
                  'Parser::ParserMemory::Vector<Parser::Token> tokens; '
                  'if (engine.tokenize("aaaab", &tokens) != Parser::Status::ErrRegexMemory) return 1; '
                  'return observed == -32 && engine.lastError()->code() == -32 && '
                  'engine.lastRegexResult().status == PE_REGEX_NO_MEMORY ? 0 : 2; }\n')
        compiler, extension, standard = "g++", ".cpp", "-std=c++17"
    driver = target / ("oom_check" + extension)
    driver.write_text(source, encoding="utf-8")
    sources = [x for x in target.glob("*" + extension)
               if x.name not in (driver.name, "main" + extension, "check" + extension)]
    binary = target / ("oom_check" + EXE)
    run([compiler, standard, "-DPE_REGEX_MALLOC=BudgetMalloc", "-include", hook,
         "-I", target, *sources, driver, "-lm", "-o", binary])
    run([binary])


C = r'''
#include "HEADER.h"
#include <string.h>
static int cancel(void *data) { return *(int *)data; }
static int32_t invoke(const char *text) { INVOKE }
int main(void) {
    PeRegexLimits limits = {0};
    int cancelled = 1;
    limits.maxSteps = 8;
    Parser_SetRegexLimits(&limits);
    if (invoke("aaaaaaaaaaaaaaaaaaaaac") != PARSER_ERR_REGEX_LIMIT) return 1;
    if (Parser_GetLastRegexResult()->status != PE_REGEX_LIMIT || Parser_GetLastRegexResult()->steps != 8) return 2;
    if (!Parser_GetLastError() || Parser_GetLastError()->code != PARSER_ERR_REGEX_LIMIT) return 3;
    if (Parser_GetLastError()->line != 1 || Parser_GetLastError()->column != 1) return 4;
    {
        PeRegexResult result = Parser_LexDfaMatchEx(&s_ParserRegexDfas[1], "aaaab", 5, NULL);
        if (result.status != PE_REGEX_MATCH || result.byteLength != 5) return 5;
        if (Parser_GetLastRegexResult()->status != PE_REGEX_LIMIT) return 6;
    }
    limits.maxSteps = MID_LIMIT; Parser_SetRegexLimits(&limits);
    if (invoke("aaaab aaaaaaaaaaaaaaaaaaaaac") != PARSER_ERR_REGEX_LIMIT || Parser_GetLastError()->column != 7 || Parser_GetLastError()->code != -30) return 11;
    Parser_SetRegexLimits(NULL);
    if (invoke("aaaab") != PARSER_OK) return 7;
    memset(&limits, 0, sizeof(limits)); limits.cancel = cancel; limits.userData = &cancelled;
    Parser_SetRegexLimits(&limits);
    if (invoke("aaaab") != PARSER_ERR_REGEX_CANCELLED || Parser_GetLastError()->code != PARSER_ERR_REGEX_CANCELLED) return 8;
    cancelled = 0;
    if (invoke("aaaab") != PARSER_OK) return 9;
    MEMORY
    return 0;
}
'''

CPP = r'''
#include "HEADER.hpp"
static int cancel(void *data) { return *static_cast<int *>(data); }
static Parser::Status invoke(Parser::Engine &engine, const char *text) { INVOKE }
int main() {
    Parser::Engine engine;
    Parser::RegexLimits limits{};
    limits.maxSteps = 8;
    if (!Parser::kRegexBudgetSupported || !engine.setRegexLimits(limits)) return 1;
    if (invoke(engine, "aaaaaaaaaaaaaaaaaaaaac") != Parser::Status::ErrRegexLimit) return 2;
    if (engine.lastRegexResult().status != PE_REGEX_LIMIT || engine.lastRegexResult().steps != 8) return 3;
    if (!engine.lastError() || engine.lastError()->code() != -30) return 4;
    if (engine.lastError()->line() != 1 || engine.lastError()->column() != 1) return 5;
    limits.maxSteps = 100; engine.setRegexLimits(limits);
    if (invoke(engine, "aaaab aaaaaaaaaaaaaaaaaaaaac") != Parser::Status::ErrRegexLimit || engine.lastError()->column() != 7 || engine.lastError()->code() != -30) return 10;
    if (!engine.setRegexLimits({}) || invoke(engine, "aaaab") != Parser::Status::Ok) return 6;
    int cancelled = 1;
    limits = {}; limits.cancel = cancel; limits.userData = &cancelled;
    engine.setRegexLimits(limits);
    if (invoke(engine, "aaaab") != Parser::Status::ErrRegexCancelled || engine.lastError()->code() != -31) return 7;
    cancelled = 0;
    if (invoke(engine, "aaaab") != Parser::Status::Ok) return 8;
    limits = {}; limits.maxMemory = 1; engine.setRegexLimits(limits);
    if (invoke(engine, "aaaab") != Parser::Status::ErrRegexLimit) return 9;
    INCREMENTAL
    return 0;
}
'''


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    unit = OUT / ("unit" + EXE)
    run(["g++", "-std=c++17", "-O1", "-Isource/include", "tests/regex_budget.cpp",
         "source/src/regex_dfa.cpp", "source/src/regex_vm.cpp", "-o", unit])
    print(run([unit]).stdout, end="", flush=True)
    generator = ROOT / ("build/bin/ParseEngine" + EXE)
    cases = [("lex", "dfa"), ("lex", "vm"), ("lex", "unicode"), ("ll1", "vm"), ("lalr", "vm"),
             ("dd", "vm"), ("glr", "vm"), ("earley", "vm"), ("cyk", "vm")]
    for mode, engine in cases:
        grammar = OUT / f"{mode}-{engine}.gram"
        pattern = "a+b" if engine == "dfa" else "(?=a)(a|aa)+b"
        short = "a"
        if engine == "unicode":
            pattern = "(?u)" + pattern
            short = "(?u)a"
        grammar.write_text(f'%mode {mode}\n%token regex SHORT "{short}"\n%token regex WORD "{pattern}"\n'
                           + ("" if mode == "lex" else "%tree\n%start input\ninput := WORD;\n"), encoding="utf-8")
        for lang in ("c", "cpp17"):
            if lang == "c" and mode in ("glr", "earley", "cyk"):
                continue
            if lang == "cpp17" and engine == "dfa":
                continue
            target = OUT / f"{lang}-{mode}-{engine}"
            args = [generator, grammar, "--lang", lang, "--out-dir", target]
            if lang == "cpp17":
                args += ["--cpp-lexer", "std" if engine == "unicode" else "vm"]
            run(args)
            header = "lexer" if mode == "lex" else "parser"
            if lang == "c":
                invoke = ('ParserLexer_T lexer; ParserToken_T token; Parser_LexInit(&lexer, text); '
                          'for (;;) { int32_t status = Parser_NextToken(&lexer, &token); if (status != PARSER_OK || token.type == TOKEN_END) return status; }' if mode == "lex" else
                          'ParserValue_T result; return Parser_RunText(text, &result);')
                memory = ('' if engine == "dfa" else 'memset(&limits, 0, sizeof(limits)); limits.maxMemory = 1; '
                          'Parser_SetRegexLimits(&limits); if (invoke("aaaab") != PARSER_ERR_REGEX_LIMIT) return 10;')
                source = C.replace("INVOKE", invoke).replace("MEMORY", memory).replace("MID_LIMIT", "8" if engine == "dfa" else "100")
                compiler, extension, standard = "gcc", ".c", "-std=c99"
            else:
                invoke = ('Parser::ParserMemory::Vector<Parser::Token> tokens; return engine.tokenize(text, &tokens);'
                          if mode == "lex" else 'return engine.runText(text, nullptr);')
                source = CPP.replace("INVOKE", invoke)
                incremental = ""
                if mode != "lex":
                    graph = mode in ("glr", "earley", "cyk")
                    cache = "GlrIncrementalCache" if graph else "IncrementalCache"
                    call = ('engine.runGlrIncremental("aaaab", cache)' if graph else
                            'engine.runTextIncremental("aaaab", nullptr, cache)')
                    incremental = (f'Parser::{cache} cache; ' +
                        ('cache.semanticReuse = Parser::GlrSemanticReusePolicy::Pure; ' if graph else '') +
                        'limits = {}; limits.maxSteps = 8; engine.setRegexLimits(limits); ' +
                        f'if ({call} != Parser::Status::ErrRegexLimit || cache.valid) return 20; ' +
                        'engine.setRegexLimits({}); ' +
                        f'if ({call} != Parser::Status::Ok || !cache.valid) return 21; ' +
                        'limits.maxSteps = 100; engine.setRegexLimits(limits); ' +
                        'if (invoke(engine, "aaaaaaaaaaaaaaaaaaaaac") != Parser::Status::ErrRegexLimit) return 22; ' +
                        f'if ({call} != Parser::Status::Ok) return 23; ')
                source = source.replace("INCREMENTAL", incremental)
                compiler, extension, standard = "g++", ".cpp", "-std=c++17"
            driver = target / ("check" + extension)
            driver.write_text(source.replace("HEADER", header), encoding="utf-8")
            sources = [x for x in target.glob("*" + extension)
                       if x.name not in (driver.name, "main" + extension, "oom_check" + extension)]
            binary = target / ("check" + EXE)
            run([compiler, standard, "-I", target, *sources, driver, "-lm", "-o", binary])
            run([binary])
            if mode == "lex" and engine == "vm":
                check_generated_oom(target, lang)
            print(f"{lang}/{mode}/{engine} 预算及取消状态传播通过", flush=True)
    target = OUT / "cpp17-lex-vm"
    run(["g++", "-std=c++17", "-fno-exceptions", "-I", target,
         target / "lexer.cpp", target / "check.cpp", "-o", target / ("without-exceptions" + EXE)])
    run([target / ("without-exceptions" + EXE)])
    for engine in ("std", "dfa"):
        target = OUT / ("unsupported-" + engine)
        run([generator, OUT / "lex-dfa.gram", "--lang", "cpp17", "--cpp-lexer", engine, "--out-dir", target])
        driver = target / "check.cpp"
        driver.write_text('#include "lexer.hpp"\nint main() { Parser::Engine p; '
                          'return Parser::kRegexBudgetSupported || p.setRegexLimits({}) ? 1 : 0; }\n', encoding="utf-8")
        binary = target / ("check" + EXE)
        run(["g++", "-std=c++17", "-I", target, target / "lexer.cpp", driver, "-o", binary])
        run([binary])
    with tempfile.TemporaryDirectory(dir=OUT) as temporary:
        work = Path(temporary)
        grammar = work / "unsupported.gram"
        grammar.write_text('%mode lex\n%cpp_lexer vm\n%token regex LONG "a{1001}"\n', encoding="utf-8")
        result = subprocess.run([str(generator), str(grammar), "--lang", "cpp17", "--out-dir", str(work / "rejected")],
                                cwd=ROOT, capture_output=True)
        assert result.returncode == 1 and not (work / "rejected").exists()
        run([generator, grammar, "--lang", "cpp17", "--cpp-lexer", "std", "--out-dir", work / "native"])
        grammar.write_text('%mode lex\n%token regex BAD "(?u)\\\\p{Missing}"\n', encoding="utf-8")
        result = subprocess.run([str(generator), str(grammar), "--lang", "cpp17", "--out-dir", str(work / "bad-unicode")],
                                cwd=ROOT, capture_output=True)
        assert result.returncode == 1 and not (work / "bad-unicode").exists()
    target = OUT / "example"
    run([generator, ROOT / "example/regex_budget.gram", "--out-dir", target])
    binary = target / ("demo" + EXE)
    run(["g++", "-std=c++17", "-I", target, target / "lexer.cpp", target / "main.cpp", "-o", binary])
    result = run([binary], "word-word;")
    assert "PAIR" in result.stdout and "word-word" in result.stdout
    print("正则资源限制及生成产物回归全部通过", flush=True)


if __name__ == "__main__":
    main()
