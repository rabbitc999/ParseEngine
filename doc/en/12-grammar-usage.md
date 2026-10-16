> This is the English edition. The Chinese edition is the default and is the one maintained first — see [中文文档](../README.md)。

# 09 Writing Grammars in `.gram` (User Manual)

This chapter was split out of [`README.md`](../../README.md): the `.gram` directive tables, semantic actions, semantic value types and user code injection, conflict resolution, error recovery, conditional compilation (`%if`), custom entry points (`%main` / `%epilogue`) and error location (`#line`).

For the specification and generation-time validation, see [`05-grammar.md`](../05-grammar.md); for the translation and template implementation, see [`07-codegen.md`](../07-codegen.md); for the API on the artifact side, see [`08-generated-code.md`](../08-generated-code.md).
Before choosing a target language and mode, first check [12 Modes Supported by Each Language and the Incremental API](../15-language-mode-support.md).

## Writing `.gram` Grammars

There are two ways to write a rule: **one per line** (write several lines with the same left-hand side), or yacc-style `|` branches (several branches of the same left-hand side written together). In the branch form each branch carries its own action, and the whole rule **must end with `;`**:

```text
initializer := assignment_expression        { $$ = $1; }
             | LBRACE initializer_list RBRACE      { $$ = $2; }
             | LBRACE initializer_list COMMA RBRACE { $$ = $2; }
             | LBRACE RBRACE                       { $$ = NULL; };
```

An action may be written as `{ ... }`, or written at the end of the line without braces. `%empty` denotes an empty production. The `//` and `/* */` comments in the file are skipped.

### Modes, Languages, and Where Code Goes

| Directive | Description |
| --- | --- |
| `%define NAME "literal"` | A fixed-string token; `\|` separates multiple candidates: `%define EQ "==\|="` (`%token NAME "literal"` is completely equivalent) |
| `%token A B C` | Declares tokens that have no fixed literal (produced externally by the lexical layer) |
| `%token NAME "a\|b"` | A fixed-string token; inside the string, only `\|` has a special meaning (separates candidates, and `\\\|` is an ordinary vertical bar) |
| `%space "whitespace"` | Which whitespace to skip before taking a token (default `"\b"` = all whitespace; `%space ""` = skip none). `\b` written in `%token` is a wildcard for "one whitespace character", while written in `%space` it stands for all whitespace |
| `%start name` | The start non-terminal (defaulting to the left-hand side of the first rule) |
| `%example "text"` | The example input used when generating documentation; at most once; supports string escapes such as `\\`, `\"`, `\n`, `\t`, while `\0` is rejected because it cannot be passed through the command line. The `RESULT.md` of the eight code backends uses it in the command-line and API examples; when undeclared, it displays `<符合文法的语言>` |
| `%mode ll1 \| ll0 \| llk \| lr0 \| slr \| slrk \| lalr \| lalrk \| lr1 \| lrk \| ielr \| glr \| earley \| cyk \| dd \| lex` | The generation mode; when it disagrees with the command-line `--mode`, the command line wins, and the default is `ielr`. `earley` / `cyk` support C++17, Python, C# and Rust artifacts; for a description of each mode see [Generation Modes and Runtimes](../13-modes.md) |
| `%lookahead N` (or written into the mode name `LL(2)`; command line `--lookahead N`) | The lookahead length k, 0..INT_MAX, default 2; it is meaningful only for `lrk` / `lalrk` / `slrk` / `llk` / `ll0` (writing it with any other mode is a direct error). For N = 1 these are equivalent to `lr1` / `lalr` / `slr` / `ll1` respectively. **There is no policy limit**, but a large k makes `FIRST_k` / `FOLLOW_k` and the number of columns grow exponentially (when the column encoding does not fit in 64 bits, generation reports an error and gives the bound for this grammar) |
| `%teach` | Turns on [teaching mode](../13-modes.md#教学模式教学提示与教学对照); equivalent to the command-line `--teach` (`--no-teach` overrides it in the opposite direction); default **off** |
| `%mermaid` | Generates the [Mermaid state machine diagrams](../13-modes.md#状态机图示mermaid); equivalent to the command-line `--mermaid` (`--no-mermaid` overrides it in the opposite direction); default **off**; independent of `%teach` |
| `%lang c \| cpp17 \| python \| csharp \| rust \| go \| gram` | The artifact language; when it disagrees with the command-line `--lang`, the command line wins, and when both are absent `cpp17` is used. `gram` generates no code: it only exports the original grammar file + the analysis report, see [`14-languages-and-products.md`](../14-languages-and-products.md#文法导出--lang-gram) |
| `%class NAME` | The parser class name of the Python artifacts (equivalent to `--class`, the command line takes precedence; default `Parser`); NAME matches `[A-Za-z_][A-Za-z0-9_]*` and may not be a Python keyword; valid only for `--lang python`, and pairing it with C / C++17 is an error |
| `%package NAME` | C#: `namespace NAME;` / Rust: the crate name (`package.name` of `Cargo.toml`) / Go: the module path (the `module` of `go.mod`, `::` normalized to `/`); NAME is a segmented identifier (joined by `.` or `::`, e.g. `MyApp.Compiler`); valid only for `--lang csharp` / `--lang rust` / `--lang go`, and under other target languages it notes and ignores it |
| `%import NAME` | C#: `using NAME;` / Rust: `use NAME;` / Go: `import ( … )` (`::` normalized to `/`, packages that duplicate the standard library are skipped); repeatable, and the last segment may be `*` (e.g. `System.Collections.*`); valid only for csharp / rust / go, and under other target languages it notes and ignores it |
| `%class_body` … `%endbody` | A section marker: each of the two markers occupies its own line, and the body between them is inserted **verbatim** into the parser class / implementation (C#: auxiliary methods / fields / inner classes; Rust: things inside `impl Parser { … }`; Go: pasted as a set of `func (p *Parser) …` methods); valid only for csharp / rust / go |
| `%namespace NAME` | The namespace name and file-name prefix of the C++17 artifacts (equivalent to `--namespace`, the command line takes precedence; when absent = namespace `Parser` and unprefixed file names); NAME matches `[A-Za-z_][A-Za-z0-9_]*` and may not be a C++ keyword; valid only for `--lang cpp17`, and pairing it with `--lang c` is an error |

### Semantic Values, Type Binding, and Cleanup

| Directive | Description |
| --- | --- |
| `%value_type type` | Declares the semantic value type (e.g. `%value_type double`, `%value_type struct Node *`); the generator appends `typedef … ParserValue_T;` |
| `%code ... %endcode` | A user C code block; may span lines and be written in several sections; concatenated verbatim into `parser_user.h` in order of appearance |
| `%code` … `%endcode` | A user code block: each of the two markers occupies its own line, and the content between them is concatenated verbatim into `parser_user.h`; may be written in several sections |
| `%code include "path"` | Same as above, but the body comes from an external file: the marker line writes `include "path"` directly (also accepts `include="path"`), and you do **not** write `%endcode`. `%epilogue` / `%main` / `%class_body` support it too; see [Putting Large User Code Blocks in External Files](#putting-large-user-code-blocks-in-external-files-include) |
| `%union { ... }` | The semantic value union (choose one of this and `%value_type`): the member declarations are pasted verbatim as `typedef union { ... } ParserValue_T;`, appended after the user code; see [`%union` + `%type`](#union--type-per-symbol-c-type-semantic-values) |
| `%type <member> a b` | Binds a `%union` member to a non-terminal (one directive may carry multiple symbols) |
| `%token <member> A B` | Binds a `%union` member to a token; the tag applies to all following symbols and can be combined with the `regex` / `"literal"` forms |
| `%token NAME include "wordlist.csv"` | **Appends** literal aliases from a CSV: only the second column of the rows whose first column equals `NAME` is taken and appended as a literal to the end of `NAME`'s alias list. It only appends aliases and creates no new token; the second column is always treated as a literal (never compiled as a regex); the header is not validated and only the first two columns are read; aliases must be unique; tokens carrying `<member>` can use it too. See [Maintaining a Literal Alias Table with CSV](#maintaining-a-literal-alias-table-with-csv) |
| `%destructor <member> { ... }` | Registers cleanup code for a `%union` member: it runs when the value is discarded (error-recovery pop / stack cleanup at parse failure), and `$$` in the fragment is that value; see [`%destructor`](#destructor-clean-up-by-member-when-a-semantic-value-is-discarded) |

### Recovery, Resource Limits, and the Remaining Directives

| Directive | Description |
| --- | --- |
| `%max_stack_depth N` | The analysis stack depth limit (`PARSER_MAX_STACK_DEPTH`, 1..65535, default 64) |
| `%max_errors N` | The maximum number of errors reported in one parse (`PARSER_MAX_ERRORS`, 1..4096, default 8) |
| `%error_recovery_tokens N` | The number of tokens to stay silent for after error recovery (`PARSER_ERROR_RECOVERY_TOKENS`, 0..4096, default 3; 0 = report the next error immediately after recovery) |
| `%sync TOKEN...` | Declares the synchronization anchor tokens for error recovery (see [Error Recovery](#error-recovery-error-productions--sync--ll-follow-sets)): for the LR family it is the fallback when an `error` production finds no recovery point, and for the LL family it is an extra token pushed in; the token must already be declared, and an undeclared one is an error |
| `%error_context` | Adds to each error record a piece of "the original text of the offending line + a `^` pointer" (`ParserError_T::context`, length `PARSER_ERROR_CONTEXT_LENGTH`, default 120). Equivalent to the compile-time `-DPARSER_ERROR_CONTEXT`; **off by default** (it has memory overhead, and only text modes such as `Parser_RunText` can obtain the source text) |
| `%empty` | Writing `%empty` on a production's right-hand side denotes an empty production (equivalent to writing nothing there, but more visible) |
| `%if` / `%elif` / `%else` / `%endif` | Conditional compilation: decides by `--define` which lines enter the artifacts, see [Conditional Compilation](#conditional-compilation-if) |
| `%epilogue` | A section marker: the body after it is pasted verbatim at the end of `parser.c` (`parser.cpp` for C++17, `parser.py` for Python); under Python it is first normalized by Python indentation rules, see [Custom Entry Points and Epilogue](#custom-entry-points-and-epilogue-main--epilogue) |
| `%main` | A section marker: the body after it replaces the built-in demo main in the artifact `main.c` (`main.cpp` for C++17, the demo body of `main.py` for Python); when there are several sections, use `%if` to ensure only one takes effect |
| `%tree` | Generates the generic syntax-tree API (supported by all modes except `%mode lex`; under `lex` it is ignored with a note) |
| `%left` / `%right` / `%nonassoc` | Declares token associativity and precedence; within a line it increases from left to right, and later lines are higher |
| `%prec TOKEN` | Written at the end of a production's right-hand side to override the source of that production's precedence |
| `%expect N` | Allows N unresolved shift/reduce conflicts to be kept |
| `%token regex NAME "regex"` | Defines a token with a regex (**C99** compiles it into a DFA at generation time and writes it into the artifact; C++17 / Python / C# / Rust / Go hand the regex to each language's own regex library as-is) |
| `%identifier "regex"` | Declares the identifier regex and generates the identifier-recognition and keyword-lookup interface |
| `%commit [type] "pattern"` / `%commit skip "pattern"` | Collects text that is "recognized but does not enter the grammar layer" into a container (elements `{line, type, text}`); when the type is omitted it is `default`, and `skip` means discard on match. Patterns are compiled as regexes and share the same longest-match order as `%token` (equal lengths by declaration order); see [`%commit`](#commit-collecting-text-that-is-recognized-but-does-not-enter-the-grammar-layer). **All six profiles support it** (C99 / C++17 / Python / C# / Rust / Go); for each profile's container type and ownership see [`08-generated-code.md`](../08-generated-code.md) |
| `%trivia "pattern"` | Discard on match: it enters neither the container nor the grammar layer. Completely equivalent to `%commit skip "pattern"` (the two spellings generate byte-for-byte identical lexical tables); using `%trivia` to express trivia is recommended; ordering and conflict rules are the same as `%commit` |
| `%copy [file\|dir] path` | Copies the library files / directories under the grammar directory into the artifact directory; default `file`, and the copied `.c`/`.h` enter `Makefile` / `CMakeLists.txt` |
| `%cpp_only` | Declares "this grammar can only generate C++17 artifacts": pairing it with `--lang c` / `--lang python` / `--lang csharp` / `--lang rust` / `--lang go` (or writing those in `%lang`) is a direct error; see [Grammars That Can Only Produce C++17](../14-languages-and-products.md#只能出-c17-的文法cpp_only) |

### Maintaining a Literal Alias Table with CSV

For cases such as keyword tables / directive tables / enum-name tables where "one token has hundreds of literals", put the alias list into a CSV and append it with a single `%token` directive (no need to pile up a long `"a\|b\|c"` in the `.gram`):

```gram
%token A include "kw.csv"                 /* A 的别名来自 kw.csv */
%token <ival> NUM include "numbers.csv"   /* 带成员的 Token 也能用 */
```

Semantics: open the CSV, take only the rows whose **first column** equals `A` (or `NUM`), and append each row's **second column** as a **literal** to the end of that token's alias list. Equivalent to hand-writing
`%token A "let|print|while"`.

The CSV looks like this (**the header can be anything and is not validated**; the 3rd column and beyond are always ignored):

```csv
token,word,note,extra
A,"let",关键字,噪音列
A,"print",关键字,噪音列
B,"这一行会被忽略",文法没导入 B,
NOT_TOKENS,"这一行也会被忽略",不在文法里,
```

- **It only appends aliases and creates no token**: `A` must already be declared, or be declared at the same time, by an ordinary `%token`;
  if the CSV's first column carries a name that does not exist in the grammar (such as `NOT_TOKENS` above), or a token
  name for which the grammar wrote no `include` (such as `B` above), **the whole row is silently ignored** with no error.
- **The second column is always treated as a literal**, never compiled into a regex — for a regex token keep using
  `%token regex NAME "..."`.
- **The header is not validated and only the first two columns are read**: the first row is not required to be a header; only columns 1 / 2 of each
  row are read, and column 3 and beyond are ignored.
- **Aliases must be unique**: a duplicate alias under the same token is silently deduplicated; the same alias used by
  different tokens is an error (the error points to the CSV row and to the place it was first assigned); an alias that collides with an already-declared
  token name is also an error. `%token regex` does not take part in this check.
- The path is resolved relative to **the directory of the `.gram` file that declares it**, and subdirectories are allowed; the same CSV
  referenced in several places is read only once.
- A `%token ... include` wrapped in a **false** `%if` branch **does not read the CSV** (conditional compilation runs before expansion).
- **Editors know this too**: `library/lsp/` and the VS Code extension that follows it
  treat `include` (and `alias`) as inline keywords of `%token` — it will not wrongly add
  a token called `include`, hovering over `include` shows a syntax note, and hovering over a token name tells you which CSV the aliases
  come from. But the LSP is a plain-text model and **does not open the CSV**: a missing CSV, an empty first column, and
  aliases conflicting across tokens are still reported by the generator at generation time.

Errors inside the CSV carry file, line and column (paths relative to the `.gram` directory):

```text
1.csv:5:2: 第 2 列（字面量）不能为空
1.csv:9:2: 别名 "x" 已被 token A 使用，不能重复分配给 token B（首次出现于 1.csv:5:2）
example/calc.gram:8:1: 找不到 CSV 文件 1.csv
```

A ready-made example: [`example/token_dict_demo.gram`](../../example/token_dict_demo.gram)
(`printf 'let num2 + num3 manual_b' | ./parser_demo` → prints the syntax tree;
`A(let)` and `NUM(num2)` are both aliases from the CSV, `B(manual_b)` is a hand-written literal;
the `B` / `NOT_A_TOKEN` rows not imported from the CSV do not enter the artifacts at all).

> A small pitfall: **the built-in `NUMBER` rule matches before "the remaining fixed literals"**, so a pure-digit string is eaten as
> `NUMBER` first. To use a pure number as an alias, first take over the built-in rule with something like `%token NUMBER ...`;
> that is why in the example the `NUM` aliases are written as `num2` / `num3`.

### `%commit`: Collecting Text That Is Recognized but Does Not Enter the Grammar Layer

If you want to keep text such as **comments** that is "recognized but does not take part in parsing" (for post-processing, documentation
generation, handing to an AI to analyze logs), use `%commit` to collect it into the parser's container:

```gram
%commit block "/\*([^*]|\*+[^*/])*\*+/"   /* 块注释，类型 block */
%commit line  "//[^\n]*"                  /* 行注释，类型 line */
%commit "0x[0-9a-fA-F]+"                  /* 类型缺省 = default */
%commit skip  "@[A-Za-z]+"                /* 匹配即丢，不进容器 */
%trivia       "#[A-Za-z]+"                /* 同上：%commit skip 的推荐写法 */

%start program
program := NUMBER "+" NUMBER;
```

Rules: the pattern is a regex (a form with no metacharacters is just a fixed literal; **only for the C99 target** do you need a DFA-friendly
form — the DFA does not support non-greedy, while the other five profiles' target regex libraries do, see
[`05-grammar.md`](../05-grammar.md) § "Known DFA Construction Pitfalls"); the matched text **does not enter the grammar layer
and is not treated as a token**,
but is appended to the container in order of appearance, with elements `{line, type, text}` — `line` is the 1-based
line number where the fragment starts, and `text` is the original text (including the literal itself; a multi-line fragment keeps its newlines). Matching
shares the same longest-match order with `%token` / the built-in comment rules: **longest match first, equal lengths by declaration
order**; the same pattern cannot be both a token and a `%commit` / `%trivia`.

How each language's artifact obtains the container (**all six profiles support this**: C99 / C++17 / Python / C# / Rust / Go;
here are C++17 and Python as two examples; Rust is `engine.commits()`, Go is `engine.Commits()`; for each profile's container and ownership details see
[`07-codegen.md`](../07-codegen.md) §13, and for the position-info API see
[`08-generated-code.md`](../08-generated-code.md) §1.5):

```cpp
// C++17（parser.hpp）
const std::vector<ParserCommit> &commits = engine.commits();
for (const ParserCommit &entry : commits) { /* entry.line / type / text */ }
```

```python
# Python（parser.py）
for entry in engine.commits:      # ParserCommit(line, type, text)
    print(entry.line, entry.type, entry.text)
```

`example/commit_demo.gram` puts the four forms together; the outputs generated by the six profiles (C / C++17 / Python /
C# / Rust / Go) are byte-identical (C# and Rust use
CRLF line endings on Windows).

**For "discard on match" use `%trivia`**: `%commit skip "<pattern>"` and
`%trivia "<pattern>"` are completely equivalent — both mean "the matched text enters neither the container nor the token layer",
sharing the same lexical declaration order (longest match first, equal lengths by earlier declaration) and the same check that "a pattern cannot duplicate
`%token`"; the lexical tables and artifacts generated by the two spellings are **byte-identical**; `%commit skip`
is kept only for compatibility. `example/commit_demo.gram` keeps one of each (`@tag` goes through
`%commit skip`, `#tag` goes through `%trivia`), and neither fragment enters the container.

#### A Known Regex Pitfall (Block Comments Hit It Most Often)

All of **C99**'s regexes (`%token regex` / `%commit` / `%trivia`) are compiled into a DFA by `source/src/regex_dfa.cpp`
and written into the artifact. One class of forms **compiles but never matches**: a negated class that excludes both `*` and
the terminating character, placed in a branch of `(A|B)*` — the `(* … *)` comment is the classic case
(**C++17 / Python / C# / Rust / Go are not affected by this**: they hand the regex to their own language's regex
library as-is):

```gram
%trivia "\(\*([^*]|\*+[^*)])\*+\)"     /* ✗ 永远认不出 (* ... *) */
%trivia "\(\*([^*]|\*+[^)]+)*\*+\)"    /* ✓ 等价、能用（推荐这一族写法） */
```

When you hit it there is no hint at all: the lexer just reports `(` as an "unrecognizable character". So always write block comments in the form
`\*+[^)]+` (the C-style `[^*/]` and `{ … }`'s `[^}]` are fine); if unsure, write the
regex alone into a small `%mode lex` grammar, feed one line of input and look at the token list — 5 seconds to verify.
For details and the backstory see [`05-grammar.md`](../05-grammar.md) § "Known DFA Construction Pitfalls"; a ready-made example is
`project/pl0-ir/pl0.gram`.

## Semantic Actions

| Form | Meaning |
| --- | --- |
| `$$` | The return value of this production |
| `$1` `$2` … | The return value of the n-th right-hand-side symbol |
| `$$.field = $3;` | When the semantic value is a struct, assign by field (`$$` can be an lvalue, `$n` cannot) |
| `int32(0)` / `float(1.5)` | Converted to the C casts `(int32_t)(0)` / `(float)(1.5)` |
| `$1.field` | Ordinary C member access; `$n.<scalar type name>` (such as `$1.int32`) is an error |
| `$1` / `$$` (when `%union` is written) | Write `$1 + $3` / `$$ = $1` as usual; the generator adds `.member` automatically according to the member bound to the symbol (a hand-written `$1.ival` passes through unchanged) |
| `sqrt($3)` / `pow($1,$3)` | Just the C functions of the same name; `#include <math.h>` yourself |

Arithmetic/logical operators (`+ - * / % && ||`, comparison, bitwise) are **emitted as C operators verbatim**; no type checking is done
at generation time, which is left to the C compiler. Forms outside the supported range are reported as errors at generation time, with a hint to implement them in C
yourself inside `%code` — for the list see the "Generation-time validation" section of [`05-grammar.md`](../05-grammar.md).

Every mode requires that "each production either writes an action or the grammar enables `%tree`"; otherwise it reports
"规则 X 缺少语义动作；请写动作或启用 %tree".

## Semantic Value Types and User Code Injection

### `%value_type`: Declare the Semantic Value Type in One Line

The easiest form is to write a single `%value_type` line at the top of the grammar; the generator appends
`typedef <type> ParserValue_T;` (after the `%code ... %endcode` user code, so the type can
refer to a struct defined in the user code):

```text
%value_type double                 /* → typedef double ParserValue_T; */
%value_type struct AstNode *       /* 也可以是指针 / 结构体等任意 C 类型 */
```

| Declaration form | Semantic value type | Applicability |
| --- | --- | --- |
| `%value_type <C type>` | All symbols share this one type | Simple and recommended; struct / pointer / scalar all work |
| `%union { ... }` + `%type <member>` | A union, binding a different member per symbol | When you want "one C type per symbol" like yacc/bison |
| Typedef it yourself in `%code ... %endcode` (or a header brought in by `%copy`) | The user takes over entirely | When you need the type definitions centralized in your own header |
| Write nothing (and there is no `%code ... %endcode`) | The generator appends `typedef int ParserValue_T;` | When you just want it to run first |

`%value_type` and `%union` cannot be written at the same time (both define `ParserValue_T`, a generation-time error);
when `%value_type` is written, the user code must not define `ParserValue_T` again.

For a comparison of the semantic value types of each profile's artifacts (C# must use the target language's own type name) see
[`14-languages-and-products.md`](../14-languages-and-products.md): for C#, when nothing is written it
is `object`; `%union` generates a class with member fields in C#
(members are written in each language's field syntax, and sections are still separated by `;`).

### `%code ... %endcode` Code Blocks

The content between `%code` and `%endcode` is collected **verbatim** (not parsed, not escaped, may span lines) and concatenated
into the generated `parser_user.h` in order of appearance. Both markers must be on their own line; a code block may be placed anywhere in the file without affecting the parsing of other
directives; when there is no code block and no `%value_type` / `%union`, `parser_user.h` is not generated.
Both the generated `parser.c` and `parser.h` include that header.

The old `%{{ ... }}%` syntax is disabled, and loading prompts you to switch to `%code ... %endcode`.

Under Python artifacts (`--lang python`) this body does not generate `parser_user.h`; it is inserted after the
`import`s in `parser.py` (indentation is normalized by Python rules, see
[Python Output](../14-languages-and-products.md#python-产物给文法做快速分析--lang-python)).

```text
%code
#include "mylib.h"

/* 辅助函数 / 宏 / 全局表都放这里；语义值类型用一行 %value_type 更省事 */
static double My_Clamp(double value) { ... }
%endcode
```

### `%union` + `%type`: Per-Symbol C Type Semantic Values

To get a semantic value with **one C type per symbol** like yacc/bison, use `%union` together with
`%type` / `%token <member>`:

```text
%union {
    int          ival;   /* 成员声明原样贴进产物，生成期不解析 C 类型 */
    double       dval;
    struct Node *node;   /* 可以引用 %code ... %endcode 里 struct / typedef 出来的类型 */
}
%code
/* 用户代码（排在联合体定义之前） */
%endcode

%token <ival> NUMBER
%type  <ival> expr term
expr := expr PLUS term { $$ = $1 + $3; }   /* → result.value.ival = values[1].ival + values[3].ival; */
      | term           { $$ = $1; }
      ;
```

The generator pastes the `%union` members verbatim as `typedef union { ... } ParserValue_T;` (appended after the
user code), and `$$` / `$n` in actions automatically get `.member` added according to the member bound to the symbol, so
forms like `$1 + $3` and `$$ = $1` need not change a single character. Generation time only does bracket balancing (find the outermost `}`,
split sections at top-level `;`) and member-name extraction; the full declarator parsing is left to the C compiler, so nested
`struct` / `union` / `enum` definitions, function-pointer members, arrays, bit-fields and `const` pointers can all be written;
a type mismatch is not blocked, and the C compiler raises the warning/error.

Terminals no longer need a hand-written `PARSER_USER_TOKEN_VALUE` either: when `%union` is written,
`ParserToken_T.value` itself is `ParserValue_T`, and the lexer, per `%token <ival> NUMBER`,
writes the numeric lexeme into `pToken->value.ival` (for double-quoted strings, identifiers and `%token regex` it writes the
matched text). You only need to define that macro when you want to take over the conversion yourself — for example converting a regex-matched numeric text into an `int` —
and then it takes precedence.

If `%token <member> NUMBER` is missing under `%union`, there is nowhere in the union to put the number: the generator
will not force a `pToken->value = …` that fails to compile, but writes no assignment — the semantic value of a numeric token
is always the zero value, and generation prints a `警告：` reminder (to use the numeric value, add that `%token`).

A pitfall: C++17 artifacts carry a compatibility layer inside the namespace, which occupies names such as `Parser_RunText` / `ParserError_T`;
the generator does not rewrite a single character of user code in `%code ... %endcode` and `%epilogue`,
so functions / types defined in `%code ... %endcode` **must not** start with `Parser_`, or they will collide with the compatibility layer's
declarations. Just change the prefix (e.g. `Demo_MakeNode`). The same applies to C99 artifacts:
`Parser_` is a prefix reserved by the generator.

**Design trade-off**: there are only two paths, "all symbols share the same `ParserValue_T`" and "`%union` splits
members by symbol"; the former is handled by a single `%value_type` line. When `%union` is not written, generation time does not touch
the member-expansion path at all — when changing generation time you can diff against an older version's artifacts to confirm that "no `%union`
written means the artifacts must not change".

For directive details see [`05-grammar.md`](../05-grammar.md) §2, for the translation rules see
[`07-codegen.md`](../07-codegen.md) §4.1, and for a complete example see
`example/union_demo.gram`.

### `%destructor`: Clean Up by Member When a Semantic Value Is Discarded

If a `%union` member is a type such as a raw pointer that **needs manual release**, use `%destructor`
to register cleanup code per member (the syntax matches bison):

```text
%union {
    char        *str;    /* 堆内存：丢弃时要 free */
    struct Node *node;   /* 堆节点：丢弃时要 Node_Free */
    int          ival;   /* 标量：没什么可清的 */
}

%type  <str>  list item
%token <node> VAR

%destructor <str>  { free($$); }
%destructor <node> { Node_Free($$); }
```

`$$` in the fragment refers to **the value being discarded**, expanded by member (in C artifacts it is `pValue->str`,
and likewise in C++17 artifacts). The fragment is collected verbatim as C statements and recognizes only `$$`; writing forms such as `$1` / `$line`
that need "a right-hand-side symbol" is a generation-time error — at the cleanup site there is no right-hand-side symbol to point to.

**When it is called** (the same rules as bison):

| Timing | Description |
| --- | --- |
| Error-recovery pop | The slots popped by `%sync` / `error` production recovery and by the LL FOLLOW-set recovery (abandoning one level of construct) |
| Failed-parse finalization | Before returning failure, discard every semantic value still on the stack, one by one |
| Successful parse | **Does not drain the stack**: the start symbol's result is handed back to the caller (pointers in the result are owned by the caller) |

**Who runs it**: the symbols bound to that member via `%type <member>` / `%token <member>`. A given member may have only
one `%destructor`; a member without a `%destructor` stays as before — C artifacts still call the
`PARSER_USER_DISCARD_VALUE` hook for every discarded value (compatible with the old form), while C++17 artifacts are RAII
semantic values by nature and have no such hook. The granularity is the **member**, not a specific symbol name. The LL family keeps semantic values in a
"semantic frame", and the runtime likewise records the symbol number per slot and looks up the same table when discarding.

**Restriction**: writing `%destructor` under `--lang python` is an immediate error (Python's semantic values are arbitrary
objects reclaimed by reference counting).

Complete example and self-check: [`example/destructor_demo.gram`](../../example/destructor_demo.gram)
(two counters respectively count "how many times heap memory was released" and "how many numeric values were discarded"; feed 4 groups of input and print the counts).

### The `%tree` Syntax Tree

`%tree` is completely decoupled from the semantic value type: the node type is `ParserTree_T`, which **stores only the symbol name and the
token lexeme**, not the semantic value.

```c
typedef struct ParserTree_T {
    const char *pName;                 /* 符号名（终结符 / 非终结符） */
    char *pText;                       /* 终结符词素；非终结符为 NULL */
    struct ParserTree_T **ppChildren;  /* 子节点数组 */
    uint32_t childCount;
    uint32_t childCapacity;
} ParserTree_T;

ParserTree_T *Parser_ParseTreeText(const char *pText);
uint32_t      Parser_TreeChildCount(const ParserTree_T *pNode);
const ParserTree_T *Parser_TreeGetChild(const ParserTree_T *pNode, uint32_t index);
const char   *Parser_TreeGetName(const ParserTree_T *pNode);
const char   *Parser_TreeGetText(const ParserTree_T *pNode);
void          Parser_TreePrint(const ParserTree_T *pRoot);
void          Parser_TreeFree(ParserTree_T *pRoot);
```

### `%copy` Copies Library Files

`%copy [file|dir] path` copies the library files / directories under the grammar directory into the artifact directory:

- `%copy file lib/value.h`, or the shorthand `%copy lib/value.h` (the default is `file`):
  copies by filename into the output directory, i.e. `out/value.h`;
- `%copy dir lib`: recursive copy, preserving the directory hierarchy, i.e. `out/lib/...`;
- Relative paths are based on the directory of the grammar file;
- The copied `.c` / `.h` are automatically added to the generated `Makefile`'s `LIB_SRCS` / `HDRS` and to
  `CMakeLists.txt`'s source list, and participate in compilation directly.

```text
%mode lalr
%tree
%start expr
%copy file lib/parser_value.h     /* 拷进产物目录并算进 HDRS */

%code
#include "parser_value.h"
%endcode
```

For a complete example see `example/custom_tree.gram`.

## Conflict Handling and Precedence

- **Default (no precedence directive written)**: when SLR(1) / LALR(1) encounters a **shift/reduce** conflict it throws an error
  at generation time, resolving nothing and producing no code; IELR(1) first tries to split the conflicting state, and only errors when it cannot.
- **After a precedence directive is written**: only shift/reduce conflicts are resolved — the shifted token's precedence comes from
  `%left/%right/%nonassoc`; the reducing production's precedence is taken from the token named by `%prec`, or, when not written, from
  the rightmost terminal of the right-hand side. The higher precedence wins; at the same level `%left` reduces, `%right` shifts, and
  `%nonassoc` is treated as an error (generation-time error).
- **A reduce/reduce conflict automatically picks one and prints a prominent large warning** (deterministic LR modes; bison does the same):
  first look at the two productions' precedences (`%prec` or the rightmost terminal of the right-hand side); **if they differ, the higher-precedence
  one wins** (this is the "user-recommended production"); if they are the same, take **the production appearing earlier in the grammar**.
  The chosen item goes into the ACTION table and the abandoned one is recorded under `[冲突消解]`, then generation prints a banner warning
  stating clearly "who was chosen, who was abandoned, and on what basis" (up to 8 listed, the rest summarized, to avoid flooding under LR(0)).
  IELR still first splits states by LR(1) context, and only takes this route when it still conflicts after splitting.
- **`lr0` / `lr1` are exceptions and never abort in any circumstance**: these two modes exist precisely to lay bare the "grammar is not strong
  enough" situation; unresolved conflicts are only written into the report's `[未消解冲突]`, and the table entries continue to be written with the yacc default
  (shift preferred) (see [LR(0) / LR(1) Canonical Automaton Modes](../13-modes.md#lr0--lr1-规范自动机模式)).
- **`slr` / `lalr` / `ielr` only let it pass when teaching mode is on (`%teach` / `--teach`)**: then they
  do not abort on unresolved conflicts, only record clearly in the report that "the grammar is not an X grammar" together with the conflict list, and continue generating table entries with
  shift preferred. Without `%teach` these three modes are strict: an unresolved conflict is an immediate error and
  produces no code.
- **`%expect N`**: lets it pass when the number of unresolved shift/reduce conflicts exactly equals N (prints a warning and writes the table with
  shift preferred), otherwise an error.
- Both the resolution results and the unresolved conflicts are written into the `[冲突消解]` / `[未消解冲突]` subsections of the analysis report,
  and with the split-file layout they are also written into `RESULT.md`.
- **LL(1) is unaffected**: the new directives parse but are ignored, and the artifacts are identical to when the directive is not used.

```gram
%mode lalr
%left  PLUS MINUS
%left  STAR SLASH
%right UMINUS
%right POW

expr := expr PLUS expr  { $$ = $1 + $3; }
expr := expr MINUS expr { $$ = $1 - $3; }
expr := expr STAR expr  { $$ = $1 * $3; }
expr := expr SLASH expr { $$ = $1 / $3; }
expr := expr POW expr   { $$ = pow($1, $3); }
expr := MINUS expr %prec UMINUS { $$ = -$2; }
expr := NUMBER { $$ = $1; }
```

## Error Recovery (Error Productions / `%sync` / LL FOLLOW Sets)

The default behavior is unchanged: a syntax error records a single `ParserError_T` and returns a negative error code. To make the parser **skip
the broken fragment and keep parsing** (reporting several errors in one parse), each of the three families has its own route:

| Form | Applicable modes | Recovery strategy |
| --- | --- | --- |
| `stmt := error SEMI { yyerrok; $$ = 0; }` | LR(0) / SLR(1) / LALR(1) / LR(1) / IELR(1) | Pop the stack to a state that can shift `error`, shift `error` as a pseudo-token, then continue with the same lookahead |
| `%sync SEMI RBRACE` | The five LR modes + the LL family | LR: pop the stack (first performing the pending reductions on it) to a state that can shift some synchronization token, discard input up to that token, then continue parsing; LL: an extra anchor |
| (nothing needs to be written) | LL(1) / LL(k) | **FOLLOW-set recovery**: on error, first check whether the top of the stack can take the current token, then whether it is a `%sync` anchor, then whether the top item "should end" (the current token is in its FOLLOW) or a deeper item can take it — if so, abandon the **construct** the top item belongs to (popping it together with the inner constructs already expanded inside it) and hand the token to the outer level; only when nobody recognizes it is the token discarded. LL(0) has no usable FOLLOW information and still stops at the first error |

- **`error` is a reserved word**: it may appear only on the right-hand side of a production; putting it on the left-hand side, or writing it into `%type` / `%token` /
  `%define` / `%start` / `%prec`, is a generation-time error. Its `$n` is a zero-initialized
  placeholder whose content is garbage — do not rely on it.
- **`yyerrok` / `yyclearin`** are written in an error rule's action (as their own statements): `yyerrok` clears
  the "currently recovering" state so later errors are reported as usual; `yyclearin` discards the current lookahead. Without
  `yyerrok`, no error is reported again for the next 3 tokens (`PARSER_ERROR_RECOVERY_TOKENS`) after recovery
  — consistent with bison's 3-token rule. Both are available only in the LR family, and error under LL(1).
- **When both are written the `error` production takes precedence**, and `%sync` is only the fallback when no `error` column action is found;
  under LR, with neither written it is the old behavior (report one error and return failure).
- **The LL FOLLOW-set recovery needs no `%sync`**: under LL, `%sync` is only an "extra anchor", and
  when the current token becomes one it recovers by "popping to the position that can take it"; the real default mechanism is the FOLLOW set
  — when the erroring level should end (the current token falls in its FOLLOW) that level is abandoned directly. So
  LL(1) / LL(k) can report several errors in one parse without writing `%sync`.
- **Mode restriction**: the `error` production is a generation-time error under LL(1) — top-down parsing has no "pop to the nearest
  recovery point" action.
- **A single parse reports at most `PARSER_MAX_ERRORS` (8) errors**: fetch them one by one with `Parser_GetErrorCount()` /
  `Parser_GetError(index)`; `Parser_GetLastError()` is still the last one. When errors were reported during recovery
  but the parse ultimately succeeds, the return value is still `PARSER_OK` and the result may be incomplete; the caller uses
  `Parser_GetErrorCount()` to decide whether to trust it (see [the API of the generated code](../14-languages-and-products.md#生成代码的-api)).
- **Capacity macros can be set in the grammar**: `%max_errors N` / `%error_recovery_tokens N` (and
  `%max_stack_depth N`) change the value that the artifact's `#ifndef` falls back to; a compile-time `-D` still takes precedence.
- **Semantic values dropped by popping are cleaned up by the user**: when `%union` is written, prefer `%destructor <member> { ... }`
  (it calls the corresponding fragment for the member bound to the symbol, see [`%destructor`](#destructor-clean-up-by-member-when-a-semantic-value-is-discarded));
  when `%union` is not written, or a member has no `%destructor` registered, C artifacts fall back to the
  `PARSER_USER_DISCARD_VALUE(pValue)` hook defined in `%code ... %endcode`, called once at runtime for each dropped value; with neither
  nothing is called (memory you allocated yourself will leak). C++17 artifacts rely on RAII and have no such hook. `%tree`'s
  tree nodes are released by the runtime itself.

```gram
%mode lalr
%sync SEMI                     /* 没写 error 产生式时的兜底锚点 */

stmt_list := stmt_list stmt { $$ = $2; } | stmt { $$ = $1; };

stmt := expr SEMI  { $$ = $1; }
      | error SEMI { yyerrok; $$ = 0; };   /* 坏语句整条丢掉，接着解析下一条 */
```

Three examples: [`example/error_recovery.gram`](../../example/error_recovery.gram) (`error` +
`yyerrok`; `1+2; 3; 4 5; 6 7; 8;` reports 2 errors and still parses up to 8),
[`example/sync_recovery.gram`](../../example/sync_recovery.gram) (only `%sync`; `1 + + 2 ; 3` hits the `;` anchor and continues),
[`example/ll_follow_recovery.gram`](../../example/ll_follow_recovery.gram) (LL(1)
FOLLOW-set recovery, with no recovery directive written at all; the broken statement is dropped via the FOLLOW set and parsing continues).

## Conditional Compilation (`%if`)

The same grammar produces different content per scenario: the conditions come from the command-line `--define NAME[=VALUE]`, and conditional compilation is
handled line by line **before parsing the grammar** — a line whose condition is false is dropped entirely (not even parsed), and a line whose condition is true is
kept verbatim.

```text
%if DEBUG
%token <ival> TRACE        /* 调试版才有的 Token */
%endif

%if LOG && !QUIET
%define BANG "!"
%else
/* 普通版一个都不加 */
%endif

factor := NUMBER {
    $$ = $1;
%if TRACE
    IfDemoTrace($1);       /* 动作片段也能用 %if 包 */
%endif
}
        ;
```

| Condition form | Meaning |
| --- | --- |
| `NAME` | The variable is true |
| `!NAME` | Negation |
| `A && B` | And |
| `A \|\| B` | Or |
| `( ... )` | Parenthesized grouping |

Precedence from low to high: `\|\|` < `&&` < `!` < parentheses < variable name. There is no notion of a "value", only true / false;
`==` / `!=` / arithmetic / function calls / string literals are all unsupported.

Values of `--define` (repeatable; for the same name **a later one overrides an earlier one**):

| Form | Truth |
| --- | --- |
| `--define NAME` / `--define NAME=1` / `=yes` / `=true` / `=on` / any other non-empty value | True |
| `--define NAME=0` / `=false` / `=no` / `=off` (case-insensitive), `NAME=`, or not passed | False |

Scope and conventions:

- **Grammar layer**: `%if` can wrap any grammar content (`%token` / `%define` / `%left` / `%start` /
  productions / a whole `%code ... %endcode` block); **action layer**: it can be written inside a semantic action `{ ... }`.
- **Unsupported**: using `%if` to decide the artifact list (e.g. wrapping `%copy` / `%tree`); referencing grammar
  symbols in a condition (e.g. "whether some token has been declared").
- The condition must be on the same line as `%if`; variable names are case-sensitive; an unpassed variable is false and does not error;
  `%code ... %endcode` code blocks are opaque to `%if` (to inject conditionally, wrap the whole block).
- Unpaired directives (unclosed / extra `%endif` / several `%else` / `%elif` after `%else`),
  and malformed condition expressions (unbalanced parentheses, operator missing an operand, illegal characters) are all generation-time errors,
  and **the line number points back to the original file** (lines dropped because their condition is false do not take up numbers).

Example [`example/if_demo.gram`](../../example/if_demo.gram): `--define LOG` turns on a `!` suffix
token and logging, `--define TRACE` turns on trace (controlled by `TRACE && !QUIET`),
and `--define LOG=off` is equivalent to not passing it.

## Custom Entry Points and Epilogue (`%main` / `%epilogue`)

There are four places to write user code, each with its own destination:

| What to put | Where | Why |
| --- | --- | --- |
| Declarations, `typedef`s, macros, `static inline` functions | `%code ... %endcode` → `parser_user.h` | This header is included by both `parser.c` / `parser_token.c`, so it may contain only things that are harmless when included repeatedly |
| Ordinary function definitions, `static` helper functions | `%epilogue` → end of `parser.c` (`parser.cpp` for C++17, `parser.py` for Python) | Appears only once; under C++17 it is defined outside the namespace (other compilation units can also link to it), a `using namespace` line is automatically added before the body, and the body is kept verbatim |
| `main` | `%main` → `main.c` (`main.cpp` for C++17, the demo body of `main.py` for Python) | Choose one of this and the built-in demo main, to avoid two `main`s |
| A separate compilation unit (its own `.c` / `.h`) | `%copy` | Copied into the artifact directory and into `Makefile` / `CMakeLists.txt` |

Counter-example: stuffing a non-`inline` function definition into `%code ... %endcode` — the header is included by several `.c` files, so at link time
it becomes several duplicate definitions. For a correct example see [`example/epilogue_demo.gram`](../../example/epilogue_demo.gram):
the declaration goes in `%code ... %endcode`, the implementation in `%epilogue`, and the semantic action calls it.

Under Python artifacts (`--lang python`) this table changes to: `%code ... %endcode` is inserted
after the `import`s in `parser.py`, `%epilogue` is pasted at the end of `parser.py`, and `%main` replaces
the demo body of `main.py` (all bodies are normalized by Python indentation rules).

```text
%code ... %endcode           /* 声明放这里 */

%epilogue             /* 函数定义贴到 parser.c 末尾 */
double MyHelper(double x) { return x * 2.0; }

%if PROD              /* 入口可以按场景切换 */
%main
int main(void) { ParserValue_T r; return Parser_RunText("1+2*3", &r); }
%endif
```

`%epilogue` / `%main` are both section markers that are **on their own line**, and the body is collected verbatim up to the next line whose "start is `%`"
or the end of the file. Key points:

### Putting Large User Code Blocks in External Files (include)

A section body can also be read from a file, leaving only the grammar in the `.gram`:

```text
%code include "demo_user.h"        /* 等价于 %code … %endcode 里贴上这个文件 */
%code include="demo_user.h"        /* 带等号的写法也认 */

%epilogue include "demo_epilogue.cpp"
%main     include "demo_main.cpp"
```

- `%code ... %endcode` and `%code include "…"` can be mixed, concatenated in order of appearance;
- Relative paths are based on **the directory of the grammar file** (the same rule set as `%copy`), and absolute paths are used as-is;
- An included file is **taken into the artifacts as-is**: it does not take part in `%if` conditional compilation, and it is not recursively parsed as a `.gram`;
  if the file cannot be opened, generation reports the resolved full path;
- Error locations still point to that marker line in the `.gram` (not to lines in the included file);
- For a complete example see `example/include_demo.gram` (all three sections `%code` / `%epilogue` / `%main` go through include,
  and the outputs of the c and cpp17 language backends are identical).

- `%epilogue` allows at most one, and **cannot define `main`** (it errors and points you to `%main`; the check is done after conditional compilation,
  so a `main` wrapped in `%if` is caught too);
- `%main` can have several sections (together with `%if` / `%elif` / `%else`), but **only one may take effect at a time**:
  0 sections → the built-in demo main, 1 section → it replaces the body of `main.c`, more than 1 → a generation-time error;
- **`%epilogue` and `%main` both call the generator functions by their C99 names**: under C++17 both sections are pasted
  **outside** the generated namespace (ordinary C++ functions, linkable by other compilation units), and before the body the generator inserts
  a line `using namespace <namespace>;` (default `Parser`, or whatever `%namespace` names),
  while the body itself is **kept verbatim, not rewritten by a single character**. So writing
  `Parser_RunText(...)` and `Parser_GetLastError()` on both sides works for `--lang c` and C++17 alike
  — C++17 artifacts carry a compatibility layer inside the namespace (`Parser_RunText` / `PARSER_OK` /
  `ParserError_T` and such names are all present, wrapping a process-level `Engine`, **not thread-safe**);
  `%epilogue` and `parser.cpp` belong to the same compilation unit and can also call the internal helpers there, while
  `%main` is a separate compilation unit and can use only the public API. New code is advised to write `Engine` /
  `engine.runText(...)` directly;
- To override `main` you can also go through `%copy file my_main.c`; do not use both routes at once (two `main`s fail to
  link).
- **Under Python artifacts these two sections work as before, except there is no `#line` and no namespace**: the `%epilogue`
  body is pasted at the end of `parser.py` (the same module as the parser class, so it can call its internal methods directly), and the
  `%main` body replaces the whole demo body of `main.py` (the built-in `_read_line()` / `main()` and the
  `if __name__` section are all included in the replacement, so you must write the entry-point call yourself; the header comment, `import parser`
  and the class-name alias `Parser` are kept). Both bodies are normalized by Python indentation rules (common indentation removed,
  leading tabs turned into 4 spaces), and nothing **outside the leading whitespace is changed at all**.

Two examples: [`example/epilogue_demo.gram`](../../example/epilogue_demo.gram) (`1+2*3` → 13;
the product is multiplied by 2 by the function in the epilogue) and [`example/main_demo.gram`](../../example/main_demo.gram)
(`--define PROD` switches to the production entry point; when not passed it falls back to the built-in demo main).

## Error Location (#line)

In the generated code, every part **coming from the `.gram`** is surrounded by a pair of `#line`s:

```c
        case 3u:
        {
#line 10 "example/calc.gram"          /* 下一行属于 .gram 第 10 行 */
            result.value = stack->values[stack->top - 1u + 0u];
#line 511 "parser.c"                  /* 行号还给自己 */
            break;
        }
```

So when `gcc -std=c99 -c parser.c` (for C++17 `g++ -std=c++17 -c parser.cpp`) reports an error you see the
`.gram`'s line number and original text, not the position in the generated file:

```text
example/calc.gram:10:28: error: 'NoSuchGlobal' undeclared (first use in this function)
   10 | expr := expr PLUS term { $$ = NoSuchGlobal; }
```

Coverage and conventions:

| Content | Destination | Forward `#line` points to |
| --- | --- | --- |
| Semantic actions | `parser.c` (`parser.cpp` for C++17) | The line with the action's `{` |
| `%code ... %endcode` | `parser_user.h` | The first line of the body |
| `%union` body | `parser_user.h` (inside `parser.hpp` for C++17) | The `%union` line |
| `%epilogue` body | End of `parser.c` (`parser.cpp` for C++17) | The line after `%epilogue` |
| `%main` body | `main.c` (`main.cpp` for C++17) | The line after `%main` |

- Line numbers are **original-file line numbers**: `%if` dropping whole lines does not make the line numbers drift (they are converted through the preprocessor layer's line-number mapping table);
- The line number of the restore line is filled back with the artifact's actual line number, so later errors do not shift as a whole;
- The `.gram` path uses the path you passed to `--grammar-file` (relative stays relative), and Windows backslashes
  are escaped as `\\`;
- **Purely generated content carries no `#line`**: not a single one in `Makefile` / `CMakeLists.txt` / `RESULT.md`
  (nor in `parser_table.c` / `parser_token.c` / `parser.h` of C99 artifacts; nor in the `parser.c` of a `%tree` example
  such as `pl0.gram` that has no semantic actions);
- **Python artifacts use a comment instead of `#line`**: Python has no `#line` directive (errors give only
  `parser.py` / `main.py`'s own line numbers), so the same positions become a one-line comment marking the origin —
  see "Line Markers in Python Artifacts" below;
- A multi-line action marks only its starting line; the following lines are incremented by the C compiler itself.

The comments in the example [`example/line_demo.gram`](../../example/line_demo.gram) describe "how to deliberately write it wrong and
what error you expect to see"; every item above can be verified against real `gcc` / `g++` error line numbers.

### Line Markers in Python Artifacts

With `--lang python` / `%lang python`, each position above becomes a one-line **comment** of the form
`# 语法文件 <grammar file> 行 <N>` (the line numbers it points to are **exactly the same** as C's `#line`, so the two kinds of
artifacts can be compared side by side):

```python
        if production == 3:
            # 语法文件 example/calc.gram 行 10
            result.value = values[0] + values[2]
            return STATUS_OK
```

| Content | Destination | Marker points to |
| --- | --- | --- |
| Semantic actions | `parser.py`'s `_apply_action` (for LL it is the expansion branch) | The line with the action's `{` |
| `%code ... %endcode` | Top of the `parser.py` module (after the imports) | The first line of the body |
| `%epilogue` | End of `parser.py` | The line after `%epilogue` |
| `%main` | Start of the `main.py` demo body | The line after `%main` |

A few conventions:

- The comment **still ends with a newline**, and its indentation follows the block it is in (inside an action it is 12 spaces), so it does not affect Python's
  indentation rules, and `python -m py_compile parser.py` passes as usual;
- The path uses the path you passed to `--grammar-file` (relative stays relative), **with no C-style escaping**
  (not needed in a comment), and Windows backslashes are written as-is;
- It is only **navigation for humans**: Python's traceback still reports `parser.py`'s own line numbers,
  which differs from C's `#line` — for an exact correspondence, follow the comment back to the `.gram`.
# Alias Example

`example/alias_demo.gram` demonstrates the `alias` forms of `%token`, `%token regex`, `%identifier`, `%commit` and `%trivia` in one place; it is a `%mode lex` grammar, and all six target languages are listed in `example/manifest.tsv`.

# CSV Alias Table Example

`example/token_dict_demo.gram` demonstrates `%token NAME include "csv"`: the alias lists live in
`example/token_dict_kw.csv` / `example/token_dict_num.csv` and are appended with a single directive.
It is a `%tree` grammar (columns `c,cpp17`); for details see [Maintaining a Literal Alias Table with CSV](#maintaining-a-literal-alias-table-with-csv) above.
