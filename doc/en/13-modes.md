> This is the English edition. The Chinese edition is the default and is the one maintained first — see [中文文档](../README.md)。

# 10 Generation Modes and Runtimes (User Manual)

This chapter was split out of [`README.md`](../../README.md): the 16 generation modes (LL family / LR family / GLR / Earley / CYK /
recursive descent `dd` / `lex`), teaching mode, the Mermaid state-machine diagrams and the lexical match order.

For each mode's formal definition, automaton construction and conflict handling see [`06-analysis.md`](../06-analysis.md);
for the API on the artifact side see [`08-generated-code.md`](../08-generated-code.md).
For the language-by-language mode and incremental support scope see [12 Modes Supported by Each Language and the Incremental API](../15-language-mode-support.md).

## LR(0) / LR(1) Canonical Automaton Modes

`%mode lr0` / `%mode lr1` (or the command line `--mode lr0` / `--mode lr1`) computes the two automata most often
compared against in textbooks, exactly as they are:

| Mode | Item set family | Reduction timing | Purpose |
| --- | --- | --- | --- |
| `lr0` | Canonical LR(0) item sets (`[A -> α . β]`) | A complete item reduces on **all** terminals, not looking at FOLLOW | Teaching the basic routine of LR parsing: closure, GOTO, and how to fill the ACTION/GOTO tables |
| `lr1` | Canonical LR(1) item sets (`[A -> α . β, a]`), **without merging** same-core states | Reduces only on the lookahead carried by the item | Explaining "where lookaheads come from, how many states merging saves, and at what cost" |

Like the other modes, these two **need no switch**: once `%mode lr0` / `--mode lr0` selects them, they
generate code that compiles and runs. They simply never abort generation due to unresolved conflicts — conflicts are written into the report's
`[未消解冲突]`, table entries follow the yacc default (shift/reduce takes shift, reduce/reduce takes the earlier reduction),
and a warning is printed on the terminal. So even an ambiguous expression grammar can fully expose the automaton and the conflicts themselves under
`--mode lr0`.

```sh
# 真正无冲突的 LR(0) 文法（example/lr0.gram 是 %mode lr0）：5 个状态、整行归约
./build/bin/ParseEngine --grammar-file example/lr0.gram --out-dir /tmp/lr0 \
    --dump-lr-automaton --dump-slr-tables
cd /tmp/lr0 && make && printf "a b" | ./parser_demo        # 3

# 同一份文法用 lr1：归约只出现在展望符上（END），表更小、状态更多
./build/bin/ParseEngine --grammar-file example/lr0.gram --out-dir /tmp/lr1 --mode lr1

# 非 LALR(1)、但 LR(1) 的文法：lr1 能干净生成
# （LALR 也能生成，但会打一条归约/归约冲突的大警告、按「更早的产生式」选一条）
./build/bin/ParseEngine --grammar-file example/ielr_split.gram --out-dir /tmp/split --mode lr1
```

These two modes' artifacts are **completely identical** to the other modes' (the split-file project, `Makefile` / `CMakeLists.txt`,
`%tree`, semantic actions and the LR runtime all reuse the same set);
the only difference is the source of the parse table and the report content. Under `--dump-lr-automaton`, `lr1` prints one item
per line in the textbook form (`[A -> α . β, a]`), while `lr0` prints LR(0) item set families like SLR.

For the algorithm and formal definition see [`06-analysis.md`](../06-analysis.md) §5.

## Lexical Mode (`%mode lex`)

`%mode lex` (or the command line `--mode lex`) **exports only the lexer**: the artifact has no parse stack, no parse table,
and no parse entry point; such a grammar may write not a single production (other modes must have rules).

```text
%mode lex

%identifier "[A-Za-z_][A-Za-z0-9_]*"
%token KEYWORD_IF "if"
%token regex FLOAT "[0-9]+\.[0-9]+"
%define PLUS "+"
```

All six target languages are supported (the same `--lang` / `%lang` rule), and the artifact contains only the lexer:

| Target | Artifacts | Lexer entry point |
| --- | --- | --- |
| C99 (`--lang c`) | `lexer.h` / `lexer.c` / `parser_token.h` / `parser_token.c` / `main.c` | `Parser_LexInit` + `Parser_NextToken` (`ParserToken_T`) |
| C++17 (`--lang cpp17`, default) | `lexer.hpp` / `lexer.cpp` / `main.cpp` | `Engine::tokenize` (`std::vector<Token>`) |
| Python (`--lang python`) | `parser.py` / `main.py` | `Parser().tokenize` (a `Token` list) |
| C# (`--lang csharp`) | `Parser.cs` / `Token.cs` / `Program.cs` / `parser.csproj` | `new Parser().tokenize(text, tokens)` (`List<Token>`) |
| Rust (`--lang rust`) | `Cargo.toml` + `src/parser.rs` / `src/token.rs` / `src/main.rs` | `Parser::new().tokenize(text)` (`Vec<Token>`) |
| Go (`--lang go`) | `go.mod` + `parser.go` / `token.go` / `main.go` | `NewParser().Tokenize(text)` (`[]Token`) |

All six profiles come with `RESULT.md` (artifact list +
token table: fixed literals / regex / `%identifier`); the C / C++17 build artifacts are
`liblexer.a` + `lexer_demo`, C# is compiled directly with `dotnet build`, Rust with
`cargo build --offline`, and Go with `go build`.
**None of them generates a parser**: C has no `parser.h` / `parser.c` /
`parser_table.c`, C++17 has no `parser.hpp` / `parser.cpp`, Python's `parser.py` has
no parse table / parse stack / `run_text`, C#'s `Parser.cs` has
no parse table / parse stack / `runText`, and Go's `parser.go` has
no parse table / parse stack / `RunText` (only `Tokenize`).

`--lang gram` ([grammar export mode](../14-languages-and-products.md#文法导出--lang-gram)) also accepts
`--mode lex`, but that profile does not even generate a lexer: the artifact is the original `.gram` file + `RESULT.md`,
and the report lists a lexical section and the token table (fixed literals / `%token regex` / `%identifier`).

```sh
# C 产物
./build/bin/ParseEngine --grammar-file example/lexer_only.gram \
    --out-dir /tmp/lexer --lang c
make -C /tmp/lexer
printf 'a + 12 if 3.5 (x)' | /tmp/lexer/lexer_demo

# C++17 产物（缺省的 --lang 就是 cpp17）
./build/bin/ParseEngine --grammar-file example/lexer_only.gram \
    --out-dir /tmp/lexer17 --lang cpp17
make -C /tmp/lexer17
printf 'a + 12 if 3.5 (x)' | /tmp/lexer17/lexer_demo

# Python 产物（不用编译；文法里的 %code ... %endcode 要写 Python）
./build/bin/ParseEngine --grammar-file example/lexer_only.gram \
    --out-dir /tmp/lexerpy --lang python
printf 'a + 12 if 3.5 (x)' | python3 /tmp/lexerpy/main.py
```

```text
VARIABLE     "a"          1:1
PLUS         "+"          1:3
NUMBER       "12"         1:5
KEYWORD_IF   "if"         1:8
FLOAT        "3.5"        1:11
LPAREN       "("          1:15
VARIABLE     "x"          1:16
RPAREN       ")"          1:17
END          "END"        2:1
共 8 个 Token
```

Used as a library (what each of the three artifacts looks like):

```c
/* C99 */
#include "lexer.h"

ParserLexer_T lexer;
ParserToken_T token;
Parser_LexInit(&lexer, "a + 12");
while (Parser_NextToken(&lexer, &token) == PARSER_OK) {
    if (token.type == TOKEN_END) { break; }
    printf("%s\n", Parser_TokenName(token.type));
}
```

```cpp
// C++17
#include "lexer.hpp"

Engine engine;
std::vector<Token> tokens;
if (engine.tokenize("a + 12", &tokens) == Status::Ok) {
    for (const Token &token : tokens) {
        std::printf("%s\n", tokenName(token.type));
    }
}
```

```python
# Python
import parser

status, tokens = parser.Parser().tokenize("a + 12")
for token in tokens:
    print(parser.token_name(token.type), token.text)
```

A few conventions:

- **All six profiles support this**: C / C++17's token-name and lexeme interfaces correspond one to one (`Parser_TokenName`
  ↔ `tokenName`), likewise Python's `token_name`, C#'s
  `Token.tokenName`, Rust's `token_name` and Go's `TokenName`;
  the fine-grained error codes match across profiles
  (`-28` / `-29`).
  The token lexemes returned by C / C++17's `tokenize` / `Parser_NextToken` point to an internal buffer
  and are valid only **until the next token is fetched** (copy them yourself for long-term storage); on the Python / C# / Rust / Go
  side they are ordinary strings (no need to worry).
- **Parse-related forms are ignored**: productions, `%start`, `%tree`, `%sync`,
  `%destructor`, `%error_context` do not take part in the artifact, and generation prints a note "已忽略：…";
  `%left` / `%right` / `%nonassoc` still declare tokens by bison's rules.
- Lexical forms work as usual: `%token` (fixed literals / `regex`), `%define`,
  `%identifier`, `%space`, `%union` + `%token <member>`, `%code ... %endcode`, `%main`,
  `%epilogue`, `%copy`.
- Lexical errors reuse the fine-grained codes: `-29` an unrecognizable character, `-28` an unterminated string; the CLIs of all three artifacts
  print a one-line note and exit with code 1.
- Example: [`example/lexer_only.gram`](../../example/lexer_only.gram).

## Teaching Mode (Teaching Hints and Teaching Comparison)

**Teaching mode is a separate switch, independent of the generation mode**: write `%teach` in the grammar or add
`--teach` on the command line (`--no-teach` conversely turns off the grammar's `%teach`); default **off**. Once on:

1. A `[教学提示]` section appears in the report (whether the grammar is an X grammar, conflicts counted by kind) — even if this
   mode never aborts anyway, this section explains clearly "why it is not an X grammar".
2. A `[教学对照]` section appears in the report:
   - `lr0`: `LR(0) 状态数`, `LR(0) 表未消解冲突数`;
   - `slr`: `LR(0) 状态数`, `SLR(1) 表未消解冲突数`;
   - `lalr`: `规范 LR(1) 状态数`, `LALR 状态数`, `LALR 表未消解冲突数`;
   - `lr1`: `规范 LR(1) 状态数`, `LALR 同心合并后状态数` — the difference between the two numbers is the
     number of states saved by core merging;
   - `ielr`: the number of states before and after splitting, the number of state splits, `IELR 表未消解冲突数`;
3. It also changes `slr` / `lalr` / `ielr` to "report but do not abort": without `%teach` these three
   modes are strict, and an unresolved conflict is an immediate error that produces no code (see [Conflict Handling and Precedence](../12-grammar-usage.md#冲突处理与优先级)).

Under `ll1` (where there are no conflicts to talk about) `%teach` is ignored, with a note on the terminal. Teaching mode is only a
report-layer switch and does not change the artifact content; diagramming the automata is another switch, see [State Machine Diagrams (Mermaid)](#state-machine-diagrams-mermaid).

```sh
# slr：不开教学模式直接报冲突并中止（用二义表达式文法试）
# 加上 --teach 才只报不中止，并在报告里说明为什么不是 SLR(1)

# --no-teach 可以压掉文法里的 %teach（example/lr0.gram 自带 %teach）
./build/bin/ParseEngine --grammar-file example/lr0.gram --out-dir /tmp/lr0 --no-teach
```

## State Machine Diagrams (Mermaid)

`%mermaid` (or the command line `--mermaid` / `--no-mermaid`) draws the state machines computed at generation time as
[Mermaid](https://mermaid.js.org/) diagrams, written into the `mermaid/` directory alongside the artifacts; default **off**.
It is **completely independent of** teaching mode (`%teach`): you can get diagrams without `%teach`, and turning on `%teach`
does not automatically produce diagrams.

| File | Content |
| --- | --- |
| `mermaid/index.html` | Overview: all diagrams embedded on one page, with a legend and cross-links |
| `mermaid/grammar.html` | Grammar automaton: LR state = LR item set, edge = GOTO transition (terminals are shifts, non-terminals labeled `GOTO`); states colored by "shift / reduce / accept / unresolved conflict" |
| `mermaid/lexer.html` | Lexical DFA: one per `%token regex` / `%identifier`, double circle for accept states, character classes on the edges (compressed intervals like `a-z`) |
| `mermaid/*.mmd` | The Mermaid source of the two diagrams above (`%% ====` separates each diagram), can be pasted directly into Markdown or mermaid.live |

- **Which diagrams are drawn**: the grammar automaton is drawn for the whole LR family (`lr0` / `slr` / `lalr` / `lr1` / `ielr`);
  `ll1`'s parse table is not a state machine, so there is no grammar diagram. The lexical DFA is unrelated to the mode and is drawn
  whenever the grammar has `%token regex` / `%identifier`.
- **No diagrams, no directory**: with `ll1` and no regex token, no `mermaid/` appears.
- **Viewing**: just open `mermaid/index.html` in a browser (opening it locally via `file://` also works);
  the page loads Mermaid from a CDN, so the first open needs network access; without network, the
  "Mermaid source" collapsed block under each diagram and the `.mmd` files are still readable and copyable.
- **It does not change artifact behavior**: the state machine diagrams are just extra files, and artifacts such as
  `parser.c` / `Makefile` still `make` / run the demo as usual.
- **Just want a peek**: `--dump-mermaid` prints the same source to standard output in Markdown fenced format
  (redirecting with `> diagrams.md` gives you a Markdown with diagrams). It works in any mode;
  when there is no diagram to draw it prints a note and outputs nothing.
- **Omitted when the diagram is too large**: a real grammar's automaton may be too large to study (`example/c89_subset.gram`'s
  LALR automaton has 340 states and 3117 transitions, and its canonical LR(1) has 1691 states and
  16586 transitions); beyond 150 states / 400 edges / 120 KB the diagram contains only a note,
  and for the full item set families see the analysis report or `--dump-lr-automaton`.

```sh
# lr0 + 图示（example/lr0.gram 自带 %mode lr0 与 %mermaid）
./build/bin/ParseEngine --grammar-file example/lr0.gram --out-dir /tmp/lr0
ls /tmp/lr0/mermaid/          # index.html grammar.html grammar.mmd
xdg-open /tmp/lr0/mermaid/index.html     # macOS 用 open，Windows 用 start

# 词法 DFA（%token regex + %identifier）与文法图一起画
./build/bin/ParseEngine --grammar-file example/lex_demo.gram --out-dir /tmp/lex --mermaid

# 只需要源码：打印到标准输出，不用教学模式
./build/bin/ParseEngine --grammar-file example/calc.gram --out-dir /tmp/calc \
--mermaid --dump-mermaid > /tmp/calc_diagrams.md
```

## LR(k) / LALR(k) / SLR(k) Modes

When one lookahead is not enough (LALR(1) reports a conflict but a longer window could distinguish it), switch the
mode to `lrk` or `lalrk`, and specify k with `%lookahead N` / `--lookahead N`:

```sh
./build/bin/ParseEngine --grammar-file example/lrk_demo.gram \
    --mode lrk --lookahead 2 --report /tmp/lrk2.txt --out-dir /tmp/lrk2
```

- **k is 0..INT_MAX (no policy limit), default 2**; when k = 1, `lrk` / `lalrk` / `slrk`
  are equivalent to `lr1` / `lalr` / `slr` respectively. **A large k is at your own risk**: `FIRST_k` / `FOLLOW_k`
  and the number of columns all grow exponentially with k, and generation prints a note when k > 3; when the column encoding
  ($\sum_i \mathrm{Token号}_i\cdot(\text{Token 个数})^i$) does not fit in 64 bits, generation
  reports an error directly and gives the bound for this grammar.
- **Two-layer ACTION table**: a shift looks at only the next token (still a single-token column), while a reduction builds columns by the k-tuple
  lookahead; the runtime maintains a window of k tokens, first looking up the reduce columns and then the shift by the window's head.
  This makes the number of columns "the number of k-tuples actually used in the grammar", not `Token 数^k`.
- **The difference between `lrk` and `lalrk` is only whether same-core states are merged**: merging saves states but combines k-tuples
  from different contexts, thereby introducing the kind of reduce/reduce conflict seen in LALR(1) — **which lengthening k cannot remove**
  (`example/lrk_demo.gram` is not LALR(k) for any k). This boundary is more worth remembering than "bigger k is stronger":
  $\mathrm{LALR}(k)\subsetneq\mathrm{LR}(k)$.
- Conflict handling matches LR(1): `lrk` reports but does not abort, and `lalrk` is strict by default (`%teach` /
  `%expect` can let it pass); `%prec` is a single-token concept, and when k > 1 it is decided by the **first** token of the k-tuple.
- Error recovery (the `error` production / `%sync`) works as usual when k > 1: `error` sits at the head of the window and
  the following k-1 are still real tokens; `%sync`'s anchor scan is still single-token.
- `%lookahead` combined with `ll1` / other LR modes is a direct error (not silently ignored): top-down
  (LL(1)) needs no k, and the other LR modes have a fixed k = 1.
- **Incremental-friendly entry point**: the table-driven LR artifacts of C++17 / Rust (LR0 / SLR / LALR / LR1 /
  IELR / LR(k) / LALR(k) / SLR(k), excluding GLR / Earley / CYK) generate
  `IncrementalCache` and `runTextIncremental` / `run_text_incremental`. When the input text is
  exactly identical, it directly reuses the previous state, semantic results, error table and `%commit` fragments; when the text changes it first
  re-scans tokens, does a common-prefix match against the cached tokens, then restores the LR stack from the nearest
  `checkpointInterval` / `checkpoint_interval` stack snapshot within the common prefix and reruns only the suffix.
  If the restore path fails, it automatically falls back to a full parse and refreshes the token cache and checkpoints.
  This is still "checkpoint suffix rerun", not full LR incremental parsing: a complete solution would have to handle local repairs
  of the state stack, continuation of error-recovery state, side effects of semantic actions (needing purity constraints or disabled cache reuse) and
  CST patching.

**The mode name can be written any way you like.** Case does not matter, and you can write k directly in parentheses (the command-line `--mode`
and the grammar's `%mode` share the same set of forms):

```sh
--mode LL(1)      # 等价 ll1
--mode 'LL(2)'    # 等价 llk + --lookahead 2（shell 里带括号要加引号）
--mode LL(k)      # 等价 llk（k 走 %lookahead 或缺省 2）
--mode LALR       # 等价 lalr
--mode 'LALR(k)'  # 等价 lalrk
--mode LR(0)      # 等价 lr0
```

## LL(k) / LL(0) Modes

The top-down route: choose the production by k tokens (`Select_k(A→α) = FIRST_k(α·FOLLOW_k(A))`);
the object being decided is the grammar **after eliminating direct left recursion**, so a left-recursive grammar like
`calc` can also be generated under LL(k). When k = 1 it is byte-for-byte equivalent to `ll1` (only the mode name differs).

```sh
# LL(2) 而不是 LL(1) 的经典小例子
./build/bin/ParseEngine --grammar-file example/llk_demo.gram --mode 'LL(2)' \
    --out-dir /tmp/ll2 && make -C /tmp/ll2
printf 'a b c' | /tmp/ll2/parser_demo      # 解析成功
printf 'a b'   | /tmp/ll2/parser_demo      # 解析失败（还差一个 Token）

# LL(0)：不看 lookahead，每个非终结符只能有一条产生式
./build/bin/ParseEngine --grammar-file example/ll0_demo.gram --mode LL(0) \
    --out-dir /tmp/ll0 && make -C /tmp/ll0
```

| Form | Meaning |
| --- | --- |
| `%mode LL(0)` / `ll0` / `LL0` | LL(0), equivalent to `llk --lookahead 0` |
| `%mode LL(1)` / `ll1` / `LL1` | LL(1) (that is, the original `ll1`) |
| `%mode LL(k)` / `llk` / `LLK` | LL(k), with k given by `%lookahead` / `--lookahead` (default 2) |
| `%mode LL(2)` / `LL2` | LL(2), no need to write `--lookahead` |

The artifact's predictive parse table is built by columns: k = 1 is a dense token table (as before), k = 0 has only one column,
and k ≥ 2 has one column per "used k-tuple" (columns encoded and binary-searched, not `Token 数^k`). The runtime has an extra
k-token window, and **the public API and error-recovery semantics are unchanged**.

**Incremental entry point**: the LL artifacts of C++17 / Rust generate the same conservative incremental API as DD:
for C++17 it is `IncrementalCache` + `Engine::runTextIncremental(...)`, for Rust it is
`IncrementalCache` + `Parser::run_text_incremental(...)`. The current policy is to reuse the previous state, semantic
results, error table and `%commit` fragments when the input text is exactly identical, and to fully recompute and refresh the cache when the text changes.

Example grammar: [`example/lrk_demo.gram`](../../example/lrk_demo.gram). For the conflict comparison of the same
grammar (`lr0` 9 / `slr` 2 / `lalr` 2 / `lr1` 0 / `ielr` 0 / `lrk` 0 / `lalrk` 2) and
construction details see [`06-analysis.md`](../06-analysis.md) §2.12.

## IELR(1) Mode

`%mode ielr` (or the command line `--mode ielr`) splits states on demand on top of LALR: the generator first
builds LR(0) item sets and computes LALR lookaheads, and only when a state exhibits an unresolved
reduce/reduce conflict specific to LALR does it split that state by "the reduction items each LR(1) sub-state
enables on that token", then refine for GOTO consistency and rebuild the parse table. Therefore:

- **Power equal to canonical LR(1)**: every LR(1) grammar can be generated, including non-LALR grammars for which LALR(1) reports a
  reduce/reduce conflict (see `example/ielr_demo.gram`, `example/ielr_split.gram`;
  the latter needs two independent splits: LALR reports a reduce/reduce conflict while IELR generates and parses normally).
- **Scale close to LALR(1)**: only the truly conflicting states are split, the whole canonical LR(1) table is not expanded,
  and the number of states and the size of the ACTION/GOTO tables are of the same order as LALR, far smaller than canonical LR(1).
- **Zero cost for LALR grammars**: when there is no unresolved reduce/reduce conflict no splitting is done, and the output is
  exactly identical to LALR (`calc` / `pl0` / `c89_subset` can all be used to check whether the IELR and LALR parse tables
  are equivalent).
- **Reuses the whole LR machinery**: precedence / `%prec` / `%expect`, `%tree`,
  `$n` semantic actions, the LR runtime and the artifact templates are all shared with SLR/LALR.

The analysis report gains two sections, `[IELR 自动机]` (LALR state count / IELR state count after splitting / canonical
LR(1) state count / ACTION table entry count / GOTO transition count) and `[IELR 状态拆分]` (which state was split, triggered
by which kind of conflict on which token, and why); `RESULT.md` also has a corresponding "IELR(1) 状态拆分" subsection,
and `--dump-lr-automaton` / `--dump-slr-tables` support IELR too.

**When to use IELR(1)**: when the grammar is itself LALR(1), just use LALR; only when LALR
reports a reduce/reduce conflict and you do not want to rewrite the grammar, or want to confirm whether the problem lies in LALR's state
merging, switch to IELR(1). For algorithm details and the formal definition see
[`06-analysis.md`](../06-analysis.md).


## GLR (Graph-Structured Stack)

`%mode glr` (or `--mode glr`) keeps the **conflict candidates** in the ACTION table cells, and the runtime uses a
graph-structured stack (GSS) to explore several paths at the same input position together: the rule is "shift together, reduce
together", which lets an ambiguous grammar (such as `example/glr_dangling_else.gram`,
`example/english-clauses.gram` and `example/glr_expr.gram`) list **all interpretations**,
rather than picking one by precedence /
`%prec` as LR does.

- **Artifact side**: GLR is a runtime feature, usable only by profiles that generate the GSS runtime, currently
  **the five profiles C++17 / Python / C# / Rust / Go**; the other profiles writing `--mode glr` are blocked by a direct
  generation-time error, with no silent downgrade.
- **Result caching**: all five profiles C++17 / Python / C# / Rust / Go keep accepted paths in dynamic containers;
  a fixed result cap is no longer set by default.
- **Error recovery**: GSS multi-path exploration is used to preserve ambiguous interpretations; when all active paths fail to shift, the runtime
  can recover by deleting words, and `%sync` can serve as the boundary for swallowing tokens. GLR **does not support** the
  `error` production and `%destructor`.
- **Result management**: `%code ... %endcode` can register an ambiguity-resolution callback (`set_ambiguity_resolver`)
  that receives "the existing result / the candidate result" and decides whether to adopt it.
- **Incremental experiment**: C++17 / Rust already provide experimental caches; the current capabilities and limitations are as follows.
  The C++17 / Rust artifacts provide the experimental cache `GlrIncrementalCache`; the C++17 entry points are
  `runGlrIncremental(...)` / `runGlrTreeIncremental(...)`, and the Rust entry points are
  `run_glr_incremental(...)` / `run_glr_tree_incremental(...)`. It currently caches token spans,
  the token diff window, span indices of reduce results, old reduce entries affected by the change window, as well as the
  GSS size, reduce visit counts and a packed forest with stable IDs; in non-tree mode, when the input text is exactly identical,
  successful semantic results can be reused only if `cache.semanticReuse = GlrSemanticReusePolicy::Pure` is set explicitly
  (the Rust field is `semantic_reuse`); the default `Disabled` runs the actions every time while still collecting
  the structural cache. When the text changes the forest is locally patched, while the GSS control flow is still fully rerun.
  Rust's `GlrIncrementalCache::new()` / `default()`
  both use this default policy.
  `Pure` is a contract on the caller: semantic actions, lexical hooks and result copying must have no observable side effects, and results
  must not depend on undeclared mutable state or already-invalidated pointers. Actions such as file IO, process spawning, logging, counters and
  symbol-table writes should stay `Disabled`; the engine will not automatically infer purity, nor will it replay side effects.
  When the external semantic environment changes, increment `cache.semanticRevision` (Rust: `semantic_revision`)
  or call `cache.clear()`; using a cache across
  engines must satisfy the same semantic-environment contract. `clear()` keeps the policy and the current environment version.
  After switching from the disabled policy to `Pure`, it reparses first and cannot reuse semantic results from the disabled period.
  Semantic results are not reused when there is an ambiguity-resolution callback, when parsing fails, or in tree mode;
  `cache.reusedSemanticResults` (Rust: `reused_semantic_results`) indicates whether this run hit.
  Rust uses `clear_ambiguity_resolver()` to remove the ambiguity closure.
  `invalidatedReduceEntries` (Rust: `invalidated_reduce_entries`) points to the reduce entries of the old cache.
  The forest already implements transitive invalidation of parent dependencies, ID reuse for unchanged nodes, suffix-span remapping,
  candidate revalidation and unreachable-node cleanup; the number of invalidated edges is the number of child references of invalidated old nodes.
  Even when actions are declared pure, GLR may still run actions on several candidate branches in a single full
  parse, and this policy provides no transactional or exactly-once guarantee.
  In tree mode, what is locally reused is the packed forest's shared symbol nodes and reduction candidates, while the public `Tree`
  is still re-materialized. GSS checkpoint recovery, in-place CST patching and continuation of error-recovery state are not yet implemented.
  For new cache fields and metric definitions see [the Forest patching interface in 12](../15-language-mode-support.md#forest-修补接口).

## Earley and CYK Chart Parsing

`%mode earley` / `--mode earley` builds Earley item sets at each input position via the three steps predict, scan and complete,
directly handling left recursion, empty productions and ambiguous grammars. `%mode cyk` / `--mode cyk`,
**at generation time**, eliminates empty productions and unit rules from the grammar, replaces the terminals in long right-hand sides and binarizes long rules;
at runtime it fills the CYK table by input span. Whether the empty string is acceptable is recorded separately from the nullability of the original start symbol.

These two modes currently support **C++17 / Python / C# / Rust**. The chart is responsible for deciding whether the input can be
derived from the original grammar; on success it resets the lexer and reparses once more reusing GLR's GSS, so as to preserve
all parse trees and run the original productions' semantic actions. Hence the interface is the same as GLR (including `%tree`, the
multi-result API and Python's syntax-graph plotting), at the cost of one extra lexing and parsing. The CNF conversion serves only the decision and
**does not rewrite** the syntax tree the user sees, nor does it run actions for the synthesized rules.

These two modes inherit the limitations of GLR's result runtime: they do not support the `error` production and `%destructor`.
The chart decision strictly checks the entire input and does not do GLR's word-deletion recovery, so it does not support `%sync` either;
a lexical error returns directly, and a failed syntax decision returns `STATUS_ERR_SYNTAX`. You can use
[`example/chart_nullable.gram`](../../example/chart_nullable.gram) to check empty productions,
unit rules, long right-hand sides and left recursion; for the English ambiguity example you can use
`--mode earley` or `--mode cyk` to override the grammar's `%mode glr`, and both modes return two trees.

The C++17 / Rust artifacts of Earley/CYK also expose GLR's experimental incremental entry points, with the same limitations as GLR:
after the input changes they still rebuild the decision chart and GSS, but locally patch the shared forest; local chart updates are not yet implemented.
For the complete support matrix see [12](../15-language-mode-support.md).

## Recursive Descent (`%mode dd`)

`%mode dd` (or `--mode dd`) **builds no tables at generation time**: each non-terminal in the artifact corresponds to a piece of
parse logic that "tries its own productions in order", and on a failed match it rolls back the lexer position and the token index and line/column
together, then tries the next one. It suits cases where "the grammar is ambiguous but you want **the choice to be determined entirely by the order in which
the productions are written**".

```text
# example/dd_expr.gram（右递归 + 括号 / 加法 / 兜底三条产生式）
%mode dd
%tree
%start expr
%define LPAREN "("
%define RPAREN ")"
%define PLUS   "+"

expr := LPAREN expr RPAREN      # ① 看到 ( 才可能命中
      | NUMBER PLUS expr        # ② 1 + 2 这种加法
      | NUMBER;                 # ③ 兜底：单个数字
```

**Grammar requirements and trade-offs**:

- **No left recursion** (direct or indirect). Generation time finds and reports cycles that "still start with themselves
  after skipping the nullable prefix", with a rewrite suggestion — rewrite `expr := expr PLUS term` into
  a right recursion such as `expr := term PLUS expr`, or switch to `--mode lalr` / `ielr`.
- **The order in which productions are written is the order they are tried**: the one written earlier is tried first. An ambiguous grammar keeps only the result of
  **the 1st matching path** (the opposite of GLR's "list all interpretations"), so write the one you want most first.
- **Backtracking happens when "a production's right-hand side fails halfway"**: after a non-terminal has **already succeeded**
  and returned, the caller will not ask it to try another parse. So `%tree`'s output is deterministic for the same input,
  but it is not guaranteed to be exhaustive over all interpretations.
- **Unsupported directives** (direct generation-time errors, not silently ignored): the error production (recursive descent has no
  "pop to the nearest recovery point" action), `%destructor` (backtracking tries the same production many times,
  so there is no definite answer as to which values are discarded), and `yyerrok` / `yyclearin` in actions (no parse stack).
  `%sync` remains available: when the whole parse fails, use it as an anchor, jump past the next synchronization token and retry a segment
  (panic mode, with a budget cap).
- **Ignored directives**: `%left` / `%right` / `%nonassoc` / `%prec` and `%expect`
  have no place in recursive descent (no tables are built, and there is no "several actions in the same cell to pick one from" situation),
  and generation prints a note in the analysis report.

**Artifact side**: all six profiles are supported (C99 / C++17 / Python / C# / Rust / Go), and the artifact shape matches that language's other modes:

- Semantic actions' `$n` uses the same convention as LL's semantic frame (slot 0 is left empty), and `%tree` uses
  the same tree-node type as that profile's LL(1);
- A syntax error is reported by "**the farthest failure point**": backtracking does not flush "the expectations that
  failed during trial" into the error table; on the final failure it reports "which tokens the farthest-reaching attempt expected", e.g.
  `语法错误：期望 LPAREN / NUMBER 之一，实际是 END`;
- The parse of the start symbol additionally requires that "the end of input follows immediately after the match", so
  `1 + 2` does not drop trailing tokens because some earlier production matched first.
- **The incremental entry point of LL / DD / deterministic LR is provided only for C++17 / Rust artifacts**: C++17 generates
  `IncrementalCache` + `Engine::runTextIncremental(...)`, and Rust generates
  `IncrementalCache` + `Parser::run_text_incremental(...)`. LL / DD are conservatively invalidated:
  when the input text is exactly identical they reuse the previous state, semantic results, error table and `%commit` fragments; when the text changes they
  automatically fully recompute and refresh the cache. The deterministic LR modes additionally maintain a token cache and prefix-stack checkpoints,
  and after the text changes they restore from the nearest checkpoint within the common token prefix and rerun only the suffix; when the restore fails they
  fall back to a full parse. This is not full LR incremental parsing; a complete solution would have to handle local repairs of the state stack, continuation of error-recovery
  state, purity constraints on semantic-action side effects / a disabled policy, and CST patching.
  C++17 / Rust's GLR has the additional experimental API described above; the other languages do not generate an incremental API for now.

**When to use recursive descent**: when the grammar is itself ambiguous and you want "take the 1st in declaration order" as a
predictable choice (typically small DSLs that deliberately keep the ambiguity and disambiguate by order); to enumerate all interpretations or
do finer ambiguity resolution, use GLR.

## Plotting for Python Artifacts (`--svg` / `--dot`)

The Python profile's artifacts come with the ability to **draw SVG themselves** (using only the standard library, installing no third-party
module, with deterministic layout): **syntax-tree plotting** is provided only for `lalr` / `lr1` / `ielr`, and **syntax graphs** for `glr` / `earley` / `cyk`
(what the diagrams contain is below). The demo program recognizes `--svg <path>` / `--dot <path>`, and without the switch its output is
**byte-for-byte identical** to before; artifacts of modes outside the whitelist do not even have these methods (decided at generation time).

```sh
# LALR(1) 档：语法树（example/pl0.gram，输入 x := 1 + 2 * 3 .）
python main.py --svg tree.svg --dot tree.dot

# GLR 档：语法图（GSS + 各结果树）
python main.py --svg graph.svg --dot graph.dot
```

![Syntax tree of the LALR(1) profile (example/pl0.gram, input x := 1 + 2 * 3 .)](../../image/python-tree-example.svg)

![Syntax graph of the GLR profile: on the left the GSS (gold highlight = accepted nodes), below side by side the result trees of the two ambiguous paths](../../image/python-syntax-graph-example.svg)

| Capability | Applicable modes | Interface | What it draws |
| --- | --- | --- | --- |
| Syntax-tree plotting | `lalr` / `lr1` / `ielr` | `Tree.to_svg()` / `to_dot()` / `write_svg(path)` / `write_dot(path)` | A tidy tree layout: the first line in the box is the symbol name, the second the lexeme (leaf nodes); branches / leaves have different background colors |
| Syntax graph | `glr` / `earley` / `cyk` | `Parser.syntax_graph_svg()` / `syntax_graph_dot()` / `write_syntax_graph_svg(path)` / `write_syntax_graph_dot(path)` | On the left the GSS (node = state + node number; shift edges solid, reduce edges dashed with `·P<n>`, accepted nodes highlighted in gold), and below, side by side, the result trees of each ambiguous path (only with `%tree`) |

`--svg` / `--dot` **parse one extra time** (on the LR side via `parse_tree`, building only the tree and running no semantic actions), so a grammar without
`%tree` can still have its tree drawn; on the GLR side it directly uses the GSS and result left by that parse, **without reparsing**, and when parsing
fails what is drawn is a halfway graph — which is exactly what you want when investigating ambiguity / dead ends. With Graphviz installed (the Python
package + the `dot` command) a DOT can be rendered directly: `graphviz.Source(tree.to_dot()).render("tree")`.


## Lexing: Fixed Literals / Regex Tokens / Identifier Recognition

Before taking a token, whitespace is skipped first (space / `\t` / `\n` / `\v` / `\f` / `\r`, the set is decided by
`%space`, see the "Whitespace as a Token" section below). The match order after that is
(longest match first, and for equal length the order below):

1. A double-quoted string `"..."` (the lexical layer unescapes it; the value is `STRING`)
2. If the grammar writes `%token regex`: regex tokens do longest-match together with all fixed literals,
   and **for equal length the literal wins** (so keywords are not eaten by a regex); on a hit, return
3. The built-in number rules (integer / `1.2` float / `1e3` / suffix `u`, `d`), producing a numeric value
4. Fixed literals (`%define` / `%token`, longer first + keyword boundary checking);
   when no regex token is written, all literals match at this step
5. Identifiers: when `%identifier` is declared, scan with its regex (C99 looks up a DFA table; the other five profiles use
   the target language's regex library), then look up the keyword table; otherwise use the built-in C identifier rule (starting with a letter/underscore,
   then letters, digits and underscores), mapped to `VARIABLE`

When neither `%token regex` nor `%identifier` is written, step 2 does not exist, step 5 uses the built-in rule, and the
artifact is completely identical to when the directive is not used.

### Whitespace as a Token: `\b` and `%space`

To let newlines, tabs and spaces enter the grammar layer (line-separated or whitespace-separated DSLs), simply write the whitespace into a fixed
literal — once declared, those characters are no longer skipped:

```gram
%token NL "\n"      // 换行是真实 Token，可以直接写进产生式
%token SP "\b"      // \b = 一个空白字符：空格 / \t / \n 都命中（强制一个空白）
%space " \t"        // 取 Token 前只跳过空格与制表，换行交给 NL
```

| Form | Effect |
| --- | --- |
| `%token NAME "\n"` / `"\t"` / `" "` | The whitespace in the literal is no longer skipped; on a match it produces this token |
| `%token NAME "\b"` | `\b` wildcards any one whitespace character, and the lexeme is the real one in the input → **parse it / force whitespace** |
| `%space "whitespace set"` | Which whitespace to skip before taking a token; default `"\b"` (all whitespace), and `%space ""` skips none |

When the lexer encounters whitespace it first checks whether "a fixed literal starting with whitespace / `\b` can match at the current position":
on a hit it stops and hands over to lexical matching, otherwise it skips by `%space`'s set. So the whitespace-skipping logic and lexical behavior of an old grammar (with no
whitespace token declared) are unchanged. In a `%token regex` regex you must write `\s`; `\b` has this meaning only in a fixed
literal. Complete example: `example/space_demo.gram`; for the specification see
[`05-grammar.md`](../05-grammar.md).

### Overriding Built-in Tokens

`STRING` (double-quoted string), `NUMBER` (number) and `VARIABLE` (identifier) are three built-in
tokens, each with a default lexical rule. As soon as you declare a rule for a token of the same name with `%define NAME "..."`, `%token NAME "..."`
or `%token regex NAME "..."`, the corresponding built-in rule is removed and everything is
left to the user rule:

```gram
%token regex STRING "'[^']*'"            // 只认单引号字符串，不再接受 "..."
%token regex NUMBER "0[xX][0-9a-fA-F]+"  // 只认十六进制数字
%token regex VARIABLE "[$][A-Za-z_]+"    // 只认 $ 前缀的变量名
```

`%identifier "..."` is still the shortcut for "identifier regex → `VARIABLE` (keywords looked up separately)", and
declaring it likewise replaces the built-in C identifier rule. Note that `%identifier` itself is defining `VARIABLE`,
so it and `VARIABLE`'s lexical rule cannot be declared at the same time; a conflict is a direct error, so pick one.

### Regex Tokens

`%token NAME "..."` without `regex` supports only `|`-separated fixed literals (completely
equivalent to `%define`, with `\\|` meaning an ordinary vertical bar); adding `regex` before the name enables regex, and the whole string is interpreted as a regex:

```gram
%token EQ    "==|="                      // 不带 regex：只拆成字面量 "==" / "="
%token PIPE  "\|"                       // 字面量竖线
%token regex FLOAT "[0-9]+[.][0-9]+"    // 带 regex：整串按正则解释
%token regex DATE  "[0-9]{4}-[0-9]{2}-[0-9]{2}"
%identifier "[A-Za-z_][A-Za-z0-9_]*"
```

Supported syntax: literals, escapes, `.`, `[abc]`, `[a-z]`, `[^...]`, `(` `)`, `|`, `*`, `+`,
`?`, `{m}` `{m,}` `{m,n}` (≤32). The escapes `\* \. \[ \] \( \) \{ \} \| \^ \\ \/`
are restored to literals (writing `"\*"` or `"\\*"` both work), `\d \w \s` (uppercase for negation) are character-class shorthands, and
`\n \r \t \f \v \0` are control characters. A regex that can match the empty string is a generation-time error (the lexer cannot advance);
the `{m,n}` limit of 32 is a hard error only for **C99** — the other five profiles hand the regex as-is to the target language's regex library,
and exceeding the limit only prints a warning (see [`07-codegen.md`](../07-codegen.md) §13).

Regex tokens carry only text (`Parser_TreeGetText` / `pName`) and produce no numeric value — for a numeric value, keep using
the built-in `NUMBER`.

### Identifier Recognition Interface

After `%identifier` is declared, the generated `parser_token.h/.c` contains:

| Interface | Description |
| --- | --- |
| `Parser_TokenIdentifierLength(text)` | The longest identifier prefix length (through the generated DFA) |
| `Parser_TokenIsIdentifier(text)` | Whether the whole string is an identifier |
| `Parser_TokenKeyword(text)` | Keyword lookup; a non-keyword returns `TOKEN_INVALID` |
| `Parser_TokenName(type)` / `Parser_TokenText(token, buf, size)` | Token name and text |
| `Parser_LexDfaMatch(dfa, text)` | Generic DFA longest match (shared by the lexer and the two functions above) |
