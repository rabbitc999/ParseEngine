> This is the English edition. The Chinese edition is the default and is the one maintained first — see [中文文档](../README.md)。

# 07 Examples Guide

The target language of an example is determined by the manifest and the grammar constraints; for the full range of supported modes see [12 Support Matrix](../15-language-mode-support.md).

There are 46 grammars under `example/`, and generating + compiling each one is enough to catch most regressions — `make -C source test-smoke` runs this pass automatically according to
[`example/manifest.tsv`](../../example/manifest.tsv) (it exits non-zero if the output does not match
`example/<name>.expected`); **`tools/smoke.sh` is currently a stub that "forbids running smoke tests"**,
so this acceptance pass now has to be done by hand (`tools/gen_all.sh` regenerates everything + `tools/diff.sh` compares + a few are run manually).
They are also a "living document" of grammar style:
most examples use the rule `|` branch form. According to `manifest.tsv` coverage there are: C **37**,
C++17 **43**, Python **20**, C# **18**, Rust **17**, Go **16**,
**151** (grammar, language) combinations across the six complete backends; after adding the Java / JS DD, Earley, CYK
samples, the whole table has **155** combinations. GLR examples list C++17 / Python / C# / Rust / Go,
and Earley / CYK also cover Go; the remaining language restrictions and fixed inputs follow the manifest.

## 1. Expressions, Branches and LR Basics

| Grammar | Mode | Semantic value type | Key features |
| --- | --- | --- | --- |
| `or_branch.gram` | LALR | `%value_type int` | `\|` branch minimal example + semantic actions |
| `calc.gram` | LALR | `%value_type double` | precedence, the variable table in `%code ... %endcode`, `:=` assignment, `sqrt/pow/fmod` |
| `calc_prec.gram` | LALR | `%value_type double` | an ambiguous grammar + `%left/%right/%prec` resolution |
| `calc_branch.gram` | LALR | `%value_type double` | equivalent to `calc_prec`, but the candidates are written in a single `\|` rule |
| `logic.gram` | SLR | `%value_type int` | `%left/%right` precedence + `\|` branches (the only SLR example) |
| `lr0.gram` | LR(0) | `%value_type int` | a true LR(0) grammar (5 states, zero conflicts), with the complete item reduced on a whole line; ships with `%teach` + `%mermaid` |

## 2. LL and Tree Structures

| Grammar | Mode | Semantic value type | Key features |
| --- | --- | --- | --- |
| `pl0_ll1.gram` | LL(1) | fallback `int` | the LL(1) version of PL/0 (tail recursion eliminates left recursion) + `%tree` |
| `json.gram` | LL(1) | fallback `int` | `%tree` + `\|` branches, with optional parts using a tail chain and `%empty` |
| `ini.gram` | LL(1) | fallback `int` | `%tree` + `\|` branches + `%identifier` |
| `mini_lang.gram` | LL(1) | fallback `int` | `%tree` + `\|` branches, with statement/expression tail chains |
| `ll0_demo.gram` | (no `%mode`) | `%tree` | LL(0): no lookahead, each non-terminal has exactly one production |
| `llk_demo.gram` | (no `%mode`) | `%tree` | a grammar that is LL(2) rather than LL(1); `--mode 'LL(1)'` reports a conflict, `--mode 'LL(2)'` passes |

## 3. LR(1), LR(k) and Complete Languages

| Grammar | Mode | Semantic value type | Key features |
| --- | --- | --- | --- |
| `pl0.gram` | LALR | fallback `int` | left-recursive PL/0 + `%tree` |
| `ielr_demo.gram` | IELR | `%value_type int` | the classic non-LALR grammar (def/param_spec/return_spec), demonstrating IELR state splitting |
| `ielr_split.gram` | IELR | `%value_type int` | a non-LALR grammar that needs two independent splits |
| `lrk_demo.gram` | (no `%mode`) | `%tree` | LR(k) / LALR(k) comparison: the classic example that is not LALR(k) for any k |
| `c89_subset.gram` | LALR | fallback `int` | a C89 subset, `%tree`, the largest example |

## 4. Lexing and Text Containers

| Grammar | Mode | Semantic value type | Key features |
| --- | --- | --- | --- |
| `lex_demo.gram` | LALR | `%value_type double` | `%token regex`, `%identifier`, keywords, a user variable table |
| `lexer_only.gram` | `%mode lex` | none (lexer only) | it can be generated with no production at all; supported by all six targets |
| `commit_demo.gram` | SLR | fallback `int` (`%tree`) | the five forms of `%commit` / `%trivia`: block comment / line comment / default type / `skip` |
| `alias_demo.gram` | `%mode lex` | none (lexer only) | examples of aliases for tokens, regex tokens, `%identifier`, `%commit` and `%trivia` |
| `token_dict_demo.gram` | (no `%mode`) | `%union` (`%tree`) | `%token NAME include "csv"`: literal aliases go into a CSV, only appending and never creating; the example includes an `<ival>` member, unmatched rows that are not imported and noise in columns 3/4 |

## 5. Trees, Semantic Values and Whitespace Tokens

| Grammar | Mode | Semantic value type | Key features |
| --- | --- | --- | --- |
| `custom_tree.gram` | LALR | a user header (`%copy`) | `%tree` + `%copy` |
| `union_demo.gram` | LL(1) | `%union` | `%union` + `%type` / `%token <成员>`, giving each symbol its C type |
| `destructor_demo.gram` | LALR | `%union` | `%destructor <成员>`: calls cleanup code per member when error recovery pops the stack |
| `space_demo.gram` | IELR | fallback `int` (`%tree`) | whitespace / newline as tokens: `%token NL` / `%token SP "\b"` / `%space` |

## 6. Error Recovery

| Grammar | Mode | Semantic value type | Key features |
| --- | --- | --- | --- |
| `error_recovery.gram` | LALR | `%value_type double` | an `error` production + `yyerrok`, reporting multiple errors in one parse |
| `sync_recovery.gram` | LALR | `%value_type double` | `%sync SEMI` panic mode, popping the stack + dropping input and then continuing |
| `ll_follow_recovery.gram` | LL(1) | `%value_type double` | no recovery directive is written; recovery relies on the FOLLOW set |

## 7. Ambiguity and Backtracking (GLR / Recursive Descent)

| Grammar | Mode | Semantic value type | Key features |
| --- | --- | --- | --- |
| `glr_dangling_else.gram` | GLR | `%tree` | the classic dangling-else ambiguity: GSS lists both interpretations (cpp17 / python / csharp / rust / go, five targets) |
| `english-clauses.gram` | GLR | `%tree` | prepositional-phrase attachment and subordinate clauses in English syntax; `the man saw the girl with the telescope` yields two trees (cpp17 / python / csharp / rust / go, five targets) |
| `english-syntax.gram` | Earley / CYK | `%tree` | subject-verb-object, attributives, adverbials, object complements, imperatives, subjunctive conditionals and wish, relative/object/adverbial clauses; defaults to Earley, switch with `--mode cyk`; prepositional-phrase attachment keeps two trees (cpp17 / python / csharp / rust) |
| `glr_expr.gram` | GLR | fallback `int` | an ambiguous expression: multiple results from a single parse |
| `glr_recovery.gram` | GLR | fallback `int` | GLR dead-end recovery (swallowing tokens) |
| `chart_nullable.gram` | Earley / CYK | `%tree` | nullable prefixes, the empty sentence, unit rules, long right-hand sides and left recursion; defaults to Earley, switch with `--mode cyk` (cpp17 / python / csharp / rust) |
| `chart_value_csharp.gram` | Earley / CYK | `%value_type int` | C# semantic actions yield 14 and 20 on the two ambiguous paths |
| `dd_expr.gram` | recursive descent `dd` | `%tree` | right-recursive, no table construction: it tries each production in declaration order, rewinds the input position on failure, and keeps only the first matching path (**supported by all six targets**) |

`english-syntax.gram` is a controlled English fragment; it does not check subject-verb agreement, tense collocation or semantic selection.
Running `python tests/english_syntax.py --langs python,cpp17,rust` verifies affirmative/negative sentence forms,
syntax tree nodes and the two results of prepositional-phrase attachment; Python is also compared against GLR.

The order in which `dd_expr.gram`'s productions are written is the order they are tried (`LPAREN expr RPAREN` → `NUMBER PLUS expr`
→ `NUMBER`), and feeding `1 + 2` yields the right-associative tree `NUMBER(1) PLUS(+) expr(NUMBER(2))`;
a left-recursive grammar (like the one in `calc.gram`) is stopped at generation time with a rewriting suggestion.
For usage and limitations see "recursive descent (`%mode dd`)" in [`13-modes.md`](../13-modes.md).

## 8. User Code, Entry Points and Target Languages

| Grammar | Mode | Semantic value type | Key features |
| --- | --- | --- | --- |
| `if_demo.gram` | LALR | `%value_type double` | `%if` conditional compilation controlling `%token`, productions and action fragments |
| `epilogue_demo.gram` | LALR | `%value_type double` | `%epilogue` holds function definitions, called by semantic actions |
| `main_demo.gram` | LALR | `%value_type double` | `%main` replaces the built-in demo main |
| `line_demo.gram` | LALR | `%value_type double` | `#line` inserted in pairs, so errors point back to the `.gram` |
| `namespace_demo.gram` | LALR | `%value_type double` | `%namespace myapp` changes the C++17 namespace and file names |
| `prefix_demo.gram` | LALR | `%value_type int` | `%prefix Demo` changes the C99 public symbol prefix |
| `code_demo.gram` | LALR | `%value_type double` | a `%code ... %endcode` user code block |
| `include_demo.gram` | LALR | `%value_type double` | `%code include` / `%epilogue include` / `%main include`: the body lives in an external file, leaving only the grammar in the `.gram` |
| `python_demo.gram` | LALR | any Python object | `%class PyCalc` + Python actions, producing Python artifacts only |

To generate any one of them (C++17 artifacts by default; add `--lang c` to switch back to C99, add `--lang python`
for compile-free Python artifacts, add `--lang csharp` for C# artifacts that compile directly with
`dotnet build`, add `--lang rust` for a Cargo project built with `cargo build`, and add `--lang gram`
to get only the grammar and analysis report):

```sh
./build/bin/ParseEngine --grammar-file example/<名字>.gram --out-dir example/<名字>_out
cd example/<名字>_out && make      # g++ -std=c++17，产物是 parser.hpp / parser.cpp / main.cpp

# 或者用产物自带的 CMakeLists.txt（与 make 等价）
cmake -S . -B build && cmake --build build
```

For a `%tree` grammar with no semantic actions, `parser_demo` prints the CST directly; one with actions prints the
evaluation result.

## 9. The `|` Branch Form

These three are enough to learn the branch syntax:

- `example/or_branch.gram`: the minimal comparison, with three candidates each carrying an action;
  note that fixed literals have keyword boundary checking, so the two letter tokens `B` `C` must be written as `"b c"`.
- `example/calc_branch.gram`: `|` branches + `%prec`, behaving exactly like
  `example/calc_prec.gram`, useful for seeing how the two forms correspond.
- `example/logic.gram`: `expr := expr OR expr | expr AND expr | ...`
  written in one rule, resolving conflicts with `%left OR` / `%left AND` / `%right NOT`.

The branch form is fully equivalent to "one per line"; the whole rule must end with `;`, and each
branch carries its own action (a `%tree` grammar may omit all actions).

## 10. Finding Examples by Topic

- **Want to copy an LL(1) grammar structure**: `json.gram`, `ini.gram`, `mini_lang.gram` —
  their lists all use "tail chain + `%empty`" to avoid left recursion.
- **Want to resolve ambiguity with precedence**: `calc_prec.gram`, `calc_branch.gram`, `logic.gram`.
- **Want to use a syntax tree**: `pl0.gram` (`%tree`),
  `pl0_ll1.gram`, `custom_tree.gram`.
- **Want to use regex tokens / identifiers**: `lex_demo.gram`.
- **Want only a lexer (no parsing)**: `lexer_only.gram` — `%mode lex`, with not a single
  production in the grammar; C emits `lexer.h` / `lexer.c` / `parser_token.*` / `main.c`, C++17 emits
  `lexer.hpp` / `lexer.cpp` / `main.cpp`, and Python emits `parser.py` (lexer +
  `tokenize`) / `main.py`. `printf 'a + 12' | ./lexer_demo` (in Python,
  `python3 main.py`) prints each token's type and lexeme (`VARIABLE "a" 1:1`),
  reporting `-29` for an unrecognized character and `-28` for an unterminated string.
  Parsing-related constructs (productions, `%tree`, `%sync`, etc.) are ignored with a one-line notice.
- **Want to use `%copy` to copy an external semantic-value header**: `custom_tree.gram`, paired with
  `example/lib/parser_value.h` (`%copy file` copies it into the output artifact directory).
- **Want typed semantic values with `%union`**: `union_demo.gram` — the `%union` holds
  three kinds of members: `int` / `double` / struct pointer; `%type <ival> expr term`,
  `%token <ival> NUMBER`, and the actions still write `$$ = $1 + $3;`, with the generator adding members per symbol;
  the same grammar also works after switching to `%mode lalr`. The lexer writes the number into `pToken->value.ival`
  according to `%token <ival> NUMBER`, so this example does not need `PARSER_USER_TOKEN_VALUE`.
  When `%union` is not written, this member expansion does not participate at all.
- **Want to register "how to clean up when a semantic value is discarded"**: `destructor_demo.gram` — the
  `char *str` in `%union` is paired with `%destructor <str> { free($$); }` (`$$` is that member on discard),
  and `long num` with `%destructor <num> { ... }`; two counters count "how many times heap memory was freed" and
  "how many numeric values were discarded"; after feeding bad input (e.g. `a b )`), `strDiscarded=1` shows that the
  recovery path of popping the stack / failing out really did clear the value. A successful parse does not pop
  the stack (the result goes to the caller), so for `a b ; c ;` both counters are 0.
- **Want to see an IELR / non-LALR grammar**: `ielr_demo.gram` (Bison's def/param_spec example)
  and `ielr_split.gram` (two independent conflicts, needing two state splits); switching either to `%mode lalr`
  produces a **big warning** about reduce/reduce conflicts (the deterministic LR mode automatically picks the "earlier production" to keep
  generating), while switching to `%mode ielr` truly resolves them through splitting and produces no warning.
- **Want to see the LR(0) / canonical LR(1) automaton**: `lr0.gram` is a true LR(0) grammar (`%mode lr0`),
  good for explaining closures, GOTO and whole-line reduction alongside `--dump-lr-automaton --dump-slr-tables`;
  to see the state-count gap between LR(1) and LALR, run any example with `--mode lr1` (the biggest gap is
  `c89_subset.gram`: 1691 → 340), and to see "the consequences of merging vs. not merging" use
  `ielr_split.gram` (`--mode lr1` gives 26 → 24; among LALR's 24 states there are two
  reduce/reduce conflicts).
- **Want to see state-machine diagrams**: `lr0.gram` ships with `%mermaid` (equivalent to the command-line `--mermaid`);
  after generating, open `mermaid/index.html` in the output artifacts with a browser to see the LR state-machine diagram;
  adding `--mermaid` to `lex_demo.gram` also produces three lexical DFAs for FLOAT / DATE / identifier,
  while an LL(1) grammar like `ini.gram` only has a lexical diagram. The command-line `--dump-mermaid`
  prints the source to standard output without any other switch. Diagrams are unrelated to the teaching mode, see
  [`06-analysis.md`](../06-analysis.md) §5.4.
- **Want to see error recovery**: `error_recovery.gram` uses `stmt := error SEMI { yyerrok; }` to
  drop the whole bad statement (`1+2; 3; 4 5; 6 7; 8;` → still parses through to 8 after reporting 2 errors);
  `sync_recovery.gram` writes no error production and only uses `%sync SEMI` for panic mode
  (`1 + + 2 ; 3` → reduces to a statement boundary, hits the anchor and continues parsing to 3);
  `ll_follow_recovery.gram` is the top-down side — **with no recovery directive at all**, it
  automatically uses the FOLLOW set to recover on an LL(1) error (`1 + + 2 ; 3 ;` continues after reporting 1 error);
  the results of all three can be checked entry by entry with `Parser_GetErrorCount()` / `Parser_GetError(index)`.
  For the syntax and semantics see [`05-grammar.md`](../05-grammar.md) §3.1 / §3.2.
- **Want to see a large-scale grammar**: `c89_subset.gram`.
- **Want to see conditional compilation**: `if_demo.gram` — the same grammar produces different content
  with `--define LOG` (an extra `!` suffix
  token + logging), `--define TRACE` (trace output, controlled by `TRACE && !QUIET`) and
  `--define LOG=off` (equivalent to not passing it); conditions are combinations of `||` / `&&` / `!` and
  parentheses; `--preprocess-only` lets you see the preprocessing layer's result on its own.
- **Want to write helper functions / your own main**: `epilogue_demo.gram` (declarations in `%code ... %endcode`,
  function definitions in `%epilogue`, called from actions; `1+2*3` → 13) and `main_demo.gram`
  (`--define PROD` uses the production entry point; without it, it falls back to the built-in demo main). To include a bunch of your own
  headers use `%copy`, and do not stuff function definitions into `%code ... %endcode` (they would be defined twice).
- **Want to verify that "errors point back to the `.gram`"**: `line_demo.gram` — the comment tells you to change a certain line to
  `return NoSuchGlobal;` and then compile with `gcc -std=c99 -c parser.c` (or `g++ -std=c++17 -c
  parser.cpp`), and you will see the error point to the `.gram` line number and echo that source line.
  All five places — actions / `%union` / `%code ... %endcode` / `%epilogue` / `%main` — can be checked this way.
- **Want to link two parsers at the same time**: `namespace_demo.gram` — the same grammar generated twice with `--namespace alpha`
  and `--namespace beta`; the two headers can be included together, and `alpha::Engine` and
  `beta::Engine` do not conflict (this also lets you check the file-name prefix and header guard macro; `%namespace`
  with `--lang c` reports an error).
- **Want to quickly see whether a grammar runs**: `python_demo.gram` — `--lang python` generates
  `parser.py` / `main.py`, no compiler needed, and `printf '1+2*3' | python3 main.py` gives the result
  directly; the class name is changed with `%class` / `--class`. Any grammar without C-specific semantic-value
  conventions (e.g. `json.gram` / `pl0_ll1.gram`) can be run this way as a cross-check
  against the output of the C / C++17 artifacts.
- **Want whitespace to reach the grammar layer**: `space_demo.gram` — `NL` (newline) and `SP` (a single whitespace character)
  are both real tokens, and `%space` decides which whitespace to skip before fetching a token. The single-line input
  `printf 'a b' | ./parser_demo` shows `SP( )`; to see the newline token you have to feed multi-line text
  (the built-in demo main reads only one line, so write `%main` or call `Parser_RunText` /
  `engine.runText` / `engine.run_text` directly). For the rule details see the
  "whitespace / newline as tokens" section of [`05-grammar.md`](../05-grammar.md).

## 11. Actual Run Samples

```sh
# 带语义动作
printf "X:=3*4; X/2" | ./parser_demo          # calc：6
printf "-2+3" | ./parser_demo                 # calc_prec：1
printf "2^3^2" | ./parser_demo                # calc_prec：512
printf "-2^2" | ./parser_demo                 # calc_branch：-4
printf "true && !false || 0" | ./parser_demo  # logic：1
printf "begin x := 1.5 end" | ./parser_demo   # lex_demo：99.5
printf "a b" | ./parser_demo                  # lr0：3（真 LR(0) 文法）

# %tree，打印 CST
printf "a + b * c" | ./parser_demo                            # custom_tree
printf "x := 1 + 2 * 3 ." | ./parser_demo                     # pl0 / pl0_ll1
printf '{"a": [1, 2, true]}' | ./parser_demo                  # json
printf "[server] host = localhost port = 8080" | ./parser_demo # ini
printf "let a = 1 + 2 * 3; print a;" | ./parser_demo          # mini_lang
printf "int main(void) { return 0; }" | ./parser_demo         # c89_subset
printf "int a : int ," | ./parser_demo                       # ielr_demo：0（非 LALR 文法也能解析）
printf "a c d" | ./parser_demo                              # ielr_split：11（两处冲突任一分支都正确）

# %union：解析成功后语义值在 result.ival 里
printf "1 + 2 * 3" | ./parser_demo    # union_demo：解析成功（退出码 0）

# 错误恢复
printf "1+2; 3; 4 5; 6 7; 8;" | ./parser_demo  # error_recovery：8（报 2 条错后继续）
printf "1 + + 2 ; 3" | ./parser_demo            # sync_recovery：3（%sync 命中 ; 锚点）
printf "1 + + 2 ; 3 ;" | ./parser_demo          # ll_follow_recovery：0（第一条语句被放弃，后续照常解析）

# 条件编译（生成时加 --define 才有差异）
printf "1+2*3" | ./parser_demo                  # if_demo 默认：7
printf "1+2!" | ./parser_demo                   # if_demo + --define LOG：3，并打印 [if_demo] number = …

# 辅助函数与入口
printf "1+2*3" | ./parser_demo                  # epilogue_demo：13（乘积被 epilogue 里的函数乘了 2）
printf "1+2*3" | ./parser_demo                    # main_demo 默认：回落内建 demo main -> 7
./parser_demo                          # main_demo + --define PROD：生产入口，打印 prod: 7

# 词法层容器（%commit / %trivia）
printf "/* c1 */ 1+2 0x1F @tag #tag // c2" | ./parser_demo
# commit_demo：commits: 3（@tag 与 #tag 都被丢掉）+ location: line=1 column=34 offset=33
```

The demo program reads one line from standard input: each of the lines above can be written as
`printf '<输入>' | ./parser_demo` (parsing starts on reading a newline or EOF);
the `%main` entry point of `main_demo` parses `argv` itself and runs according to its own usage.

## 12. Adding Your Own Example

1. Create `example/<名字>.gram`; the `%mode` on the first line determines the mode;
2. If you need extra C state (variable table, buffers…), define it yourself in `%code ... %endcode`;
3. Try generating with `./build/bin/ParseEngine --grammar-file example/<名字>.gram --out-dir /tmp/<名字>`;
4. Add a line `名字<TAB>实测支持的档<TAB>固定输入` to [`example/manifest.tsv`](../../example/manifest.tsv), and run
   `tools/smoke.sh capture` once (**the script is currently a stub**, so you first have to let it through; or generate + compile + feed
   input by hand as in [`09-build-and-test.md`](../09-build-and-test.md) §4) to produce `example/<名字>.expected`;
5. `make -C source test-smoke` should turn green (**the script is currently a stub**, so before letting it through verify with the manual approach in §4);
   a `.gram` under `example/` that is missing from the manifest reports FAIL.
