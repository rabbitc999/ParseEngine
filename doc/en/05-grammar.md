# 02 `.gram` Grammar Specification

> This is the English edition. The Chinese edition is the default and is the one maintained first — see [中文文档](../README.md)。

The available combinations of `%lang` and `%mode` are listed in [12 Modes Supported by Each Language](../15-language-mode-support.md);
the front end recognizing a mode does not mean every backend can generate it.

## 1. Lexical-Level Syntax

- The file is UTF-8 text, and a BOM is allowed; `//` line comments and `/* ... */` block comments are skipped.
- A directive starts with `%`, one per line; user code can only be written between `%code` and `%endcode`,
  and both markers must each be on their own line.
- A rule has two equivalent forms:

```gram
/* 写法一：一行一条，同名左部写多行 */
expr := expr PLUS term { $$ = $1 + $3; }
expr := term           { $$ = $1; }

/* 写法二：yacc 风格 | 分支，整条规则必须以 ; 结束 */
expr := expr PLUS term { $$ = $1 + $3; }
      | term           { $$ = $1; };
```

- An empty production is written as `%empty`, and may also be written inside a `|` branch: `opt := X | %empty;`.
- An action may be enclosed in braces `{ ... }`, or written at the end of the line without braces.

## 2. Directive Overview

### Modes, Languages, and Switches

| Directive | Effect |
| --- | --- |
| `%start NAME` | The start non-terminal (defaulting to the left-hand side of the first rule) |
| `%example "text"` | Declares one example input for this grammar; the generated `RESULT.md` uses it for the command-line, API, and diagram examples; may be declared at most once. Ordinary string escapes are supported (such as `\\`, `\"`, `\n`, `\t`), but `\0`, which cannot be passed through the command line, is rejected; when undeclared, it displays `<符合文法的语言>` |
| `%mode ll0 \| ll1 \| llk \| lr0 \| slr \| slrk \| lalr \| lalrk \| lr1 \| lrk \| ielr \| lex` | The generation mode; when it disagrees with the command-line `--mode`, the command line wins. **When neither is written, the default is `ielr` (IELR(1), which can cover all LR(1) grammars)**. `lr0` / `lr1` are the canonical LR(0) / LR(1) automaton modes, usable **without any switch**, except that they never abort due to unresolved conflicts. The four modes with k (`lrk` / `lalrk` / `slrk` / `llk`) pair with `%lookahead N`, and k can also be written directly into the mode name (`LL(2)`); see the "Where to Put k" section below. `lex` exports only the lexer (you may write not a single production, and the artifacts contain no parser); see §6 |
| `%lookahead N` (equivalent to command-line `--lookahead N`) | The lookahead length k: N ranges over **0..INT_MAX**, defaulting to **2** — **there is no policy limit, and a large k is at your own risk** (see "Values of k" below); it is meaningful only for `ll0` / `llk` / `lrk` / `lalrk` / `slrk` (N = 0 is meaningful only for the LL family, i.e. LL(0)), and writing it with any other mode is a direct error. For N = 1, these are equivalent to `ll1` / `ll1` / `lr1` / `lalr` / `slr` respectively |
| `%teach` (equivalent to command-line `--teach` / `--no-teach`) | Turns on teaching mode: the unresolved conflicts of `slr` / `lalr` / `ielr` are reported but do not abort, and the report gains `[教学提示]` / `[教学对照]`; the default is **off**, and `--no-teach` overrides `%teach` in the grammar; under `ll1` it is ignored with a note. It is independent of `%mermaid`. See [`06-analysis.md`](../06-analysis.md) §5 |
| `%mermaid` (equivalent to command-line `--mermaid` / `--no-mermaid`) | Generates [Mermaid state machine diagrams](https://mermaid.js.org/): an extra `mermaid/` directory appears in the artifacts (the grammar LR automaton + the lexical DFA); the default is **off**, and `--no-mermaid` overrides `%mermaid` in the grammar; it is unrelated to teaching mode. See [`07-codegen.md`](../07-codegen.md) |
| `%lang c \| cpp17 \| python \| csharp \| rust \| go \| gram` | The artifact language; when it disagrees with the command-line `--lang`, the command line wins, and when both are absent `cpp17` is used. `python` produces the compilation-free `parser.py` / `main.py` (see the `%class` section below), `csharp` produces `Parser.cs` / `Token.cs` / `Program.cs` / `parser.csproj`, `rust` produces the Cargo project `Cargo.toml` + `src/parser.rs` / `src/token.rs` / `src/main.rs`, and `go` produces the Go module `go.mod` + `parser.go` / `token.go` / `main.go` (all three are covered in the `%package` / `%import` / `%class_body` section below); `gram` is the **grammar export target**: it generates no code and only exports the original `.gram` file and the analysis report `RESULT.md` to `--out-dir` (see [`14-languages-and-products.md`](../14-languages-and-products.md#文法导出--lang-gram)) |
| `%namespace NAME` | The namespace name and file name prefix of the C++17 artifacts (command-line `--namespace` takes precedence). NAME matches `[A-Za-z_][A-Za-z0-9_]*`, may not be a C++ keyword, and does not support `::`; when absent, the default `Parser` + unprefixed file names are used |
| `%prefix NAME` | The public symbol prefix for C99 (command-line `--prefix` takes precedence); it may not start with `Parser` / `PARSER`; it takes effect only for `--lang c`, and for other languages it notes and ignores it |
| `%class NAME` | The parser class name of the Python artifacts (command-line `--class` takes precedence; defaults to `Parser`). NAME matches `[A-Za-z_][A-Za-z0-9_]*` and may not be a Python keyword; it is valid only for `--lang python`, and pairing it with C / C++17 is a direct error |
| `%package NAME` | C# artifacts: generates a file-level `namespace NAME;`; Rust artifacts: the crate name of `Cargo.toml` (`.` replaced with `_`); Go artifacts: the module path of `go.mod` (`::` normalized to `/`). NAME is a segmented identifier (`[A-Za-z_][A-Za-z0-9_]*` joined by `.` or `::`, e.g. `MyApp.Compiler`), written at most once; it is valid only for `--lang csharp` / `rust` / `go`, and under other target languages it notes and ignores it. See the "`%package` / `%import` / `%class_body`" section below |
| `%import NAME` | C# artifacts: generates `using NAME;` (always placed before `namespace`); Rust artifacts: `use NAME;`; Go artifacts: enters the `import ( … )` block (`::` is treated as a path separator normalized to `/`, packages that duplicate the standard library are skipped, and quoted forms are pasted verbatim). It is repeatable; NAME is a segmented identifier (joined by `.` or `::`), and the last segment may be `*` (e.g. `System.Collections.*`); it is valid only for csharp / rust / go, and elsewhere it notes and ignores it. See the "`%package` / `%import` / `%class_body`" section below |
| `%class_body` … `%endbody` | A section marker: each of the two markers occupies its own line, and the body between them is inserted **verbatim** into the parser class (C#: auxiliary methods / fields / inner classes), an `impl Parser { … }` (Rust), or a set of `func (p *Parser) …` methods after the parser code (Go). It is valid only for csharp / rust / go; elsewhere it notes and ignores it. See the "`%package` / `%import` / `%class_body`" section below |

### Semantic Values, Tokens, and Lexing

| Directive | Effect |
| --- | --- |
| `%value_type type` | Declares the semantic value type (e.g. `%value_type double`, `%value_type struct AstNode *`): the generator appends `typedef <type> ParserValue_T;` to `parser_user.h`, written at most once; choose one of this and `%union`. See §2.1 |
| `%union { ... }` | Declares the semantic value union: the member declarations are pasted **verbatim** into `typedef union { ... } ParserValue_T;` in the artifact `parser_user.h`, written at most once. See the next section and [`07-codegen.md`](../07-codegen.md) §4.1 |
| `%type <member> A B` | Binds a `%union` member to non-terminals (one directive may carry multiple symbols) |
| `%token <member> A B` / `%token <member> NAME "literal"` / `%token <member> regex NAME "regex"` | Binds a `%union` member to a token; the tag applies to **all** following symbols until the next tag appears |
| `%destructor <member> { ... }` | Registers cleanup code for a `%union` member: it runs when that member's value is discarded (error-recovery pop / stack cleanup at parse failure), and `$$` in the fragment is that value. See the "`%destructor`" section below |
| `%define NAME "literal"` | A fixed-string token; `\|` separates multiple candidates, and `\|` escapes to an ordinary vertical bar |
| `%space "whitespace"` | The set of whitespace characters skipped before taking a token (defaults to `"\b"`, i.e. all whitespace); here `\b` stands for all whitespace, and `%space ""` means no character at all is skipped. See the "Whitespace / Newline as Tokens" section below |
| `%token A B C` | Declares tokens that have no fixed literal |
| `%token A B C [alias "display name"]` | Declares tokens that have no fixed literal; the alias applies to the immediately preceding token declaration |
| `%token NAME "..." [alias "display name"]` | Equivalent to `%define` (without `regex` it only splits literals); `alias` changes only the display name, not the token identity |
| `%token NAME include "wordlist.csv" [alias "display name"]` | **Appends** literal aliases: for the rows of the CSV whose first column equals `NAME`, the second column is appended as a literal to the alias list of `NAME`. It only appends aliases and creates no new token; the second column is always treated as a **literal** (not compiled as a regex); it also works with `<member>`. See the "Maintaining a Literal Alias Table with CSV" section below |
| `%token regex NAME "regex" [alias "display name"]` | An optional display alias for a regex token; the regex matching rules and the token identity stay unchanged |
| `%token regex NAME "regex"` | Defines a token with a regex, compiled to a DFA at generation time |
| `%identifier "regex"` | Declares the identifier regex and generates the identifier-recognition and keyword-lookup interface |
| `%identifier "regex" [alias "display name"]` | Declares the identifier regex; the alias is displayed on the built-in `VARIABLE` name |
| `%commit [type] "pattern" [alias "display name"]` / `%commit skip "pattern"` | Collects text that is "recognized but does not enter the syntax layer" into a container (elements `{line, type, text}`); when the type is omitted it is `"default"`, and `skip` means discard on match and do not enter the container (**equivalent to `%trivia`; using `%trivia` is recommended**). `alias` is used only for the type table and diagnostic display, and does not change the `type` field. Patterns are compiled as DFA regexes (a form without metacharacters is just a fixed literal); they share the same longest-match order as `%token` / the built-in comment rules, and ties are broken by declaration order. See the "`%commit`" section below |
| `%trivia "pattern" [alias "display name"]` | Discard on match: it enters neither the container nor the syntax layer. It is **completely equivalent** to `%commit skip "pattern"` (the two forms generate byte-for-byte identical lexical tables), and the alias is recorded only in the type table; since the semantics are clearer, it is recommended for declaring trivia. Patterns and ordering rules are the same as `%commit` |

### Runtime Limits, Recovery, and User Code

| Directive | Effect |
| --- | --- |
| `%max_stack_depth N` | The analysis stack depth limit `PARSER_MAX_STACK_DEPTH`; N ranges 1..65535, default 64 (the stack index is a `uint16_t`, so larger values are pointless) |
| `%max_errors N` | The maximum number of errors retained per parse `PARSER_MAX_ERRORS`; N ranges 1..4096, default 8 |
| `%error_recovery_tokens N` | The number of tokens to stay silent for after error recovery `PARSER_ERROR_RECOVERY_TOKENS`; N ranges 0..4096, default 3; 0 means the next error is reported immediately after recovery |
| `%error_context` (equivalent to command-line `-DPARSER_ERROR_CONTEXT`) | Adds to each error record a piece of "the original text of the offending line + a `^` pointer" (`ParserError_T::context`, length `PARSER_ERROR_CONTEXT_LENGTH`, default 120). **Off by default** — this is pure memory overhead, and only text mode (`Parser_RunText`) can obtain the source text. See [`08-generated-code.md`](../08-generated-code.md) |
| `%epilogue` | A section marker: the body after it is pasted verbatim at the end of the generated `parser.c` (`parser.cpp` for C++17, `parser.py` for Python) |
| `%main` | A section marker: the body after it replaces the built-in demo main in the artifact `main.c` (`main.cpp` for C++17, the demo body of `main.py` for Python) |
| `%cpp_only` | Declares "this grammar can only generate C++17 artifacts": adding `--lang c` / `--lang python` / `--lang csharp` / `--lang rust` / `--lang go` on the command line then becomes a direct error (likewise if those are written in `%lang`). Use it to catch misuse early when the grammar depends on things only the C++17 artifacts provide (`Engine`, `std::string_view`, the compatibility layer) |
| `%tree` | Generates the CST interface; a pure `%tree` grammar may omit semantic actions |
| `%left` / `%right` / `%nonassoc` | Declares token associativity and precedence; the lower a line is, the higher the precedence |
| `%prec TOKEN` | Written at the end of a branch's right-hand side to override the source of the production's precedence |
| `%expect N` | Allows N unresolved shift/reduce conflicts to be kept |
| `%sync TOKEN...` | Declares the recovery anchor tokens for panic mode; the LR family falls back to it when an `error` production cannot find a recovery point; the LL family already recovers via the FOLLOW set, so it merely adds extra anchors (unused by `ll0`). See §3.2 |
| `%copy [file\|dir] path` | Copies a file/directory under the grammar directory into the artifact directory; `file` is the default |
| `%code` … `%endcode` | A user code block: `%code` and `%endcode` each occupy their own line, and the content between them is spliced verbatim into `parser_user.h`; multiple blocks may be written and are concatenated in order of appearance. The old `%{{ ... }}%` syntax is disabled |
| `%if` / `%elif` / `%else` / `%endif` | Conditional compilation (handled by the preprocessing layer, not a parser directive): the conditions come from the command-line `--define`, and whole lines whose condition is false are dropped. See the next section |

### `%value_type`: Declaring the Semantic Value Type in One Line

```text
%value_type double                 /* → typedef double ParserValue_T; */
%value_type struct AstNode *       /* 任意 C 类型：指针、结构体、联合体都行 */
%value_type uint32_t
```

The type is pasted into the artifacts verbatim (after the `%code` user code, so it can reference
structs defined in the user code); no parsing is done at generation time, and it is handed to the C compiler. Constraints:

- Written at most once; it cannot be written together with `%union` (both define `ParserValue_T`).
- When `%value_type` is written, `ParserValue_T` may not be defined again in `%code`.
- When neither is written and no `%code` is present, the generator provides
  `typedef int ParserValue_T;` as a fallback; if user code is present, the user supplies it and the generator no longer adds one
  — C99 and C++17 are consistent (the C++17 artifact's `parser_user.h` is included before the namespace is opened,
  so the user-supplied type is likewise visible inside the namespace).

### Writing `%union` / `%type` / `%token <member>`

```text
%union {
    int    ival;
    double dval;
    char  *sval;
    struct { int x; float y; } point;      /* 嵌套 struct 定义 */
    union  { int i; double d; } num;       /* 嵌套 union 定义 */
    int  (*callback)(int, char *);         /* 函数指针成员 */
    int    arr[16];                        /* 数组成员 */
    const char *name;
}

%type  <ival> expr term
%type  <sval> name
%token <ival> NUMBER
%token <sval> regex KEYWORD "[a-z]+"
```

At generation time, **C types are not parsed**; only three things are done, and the rest is handed to the C compiler:

1. **Balance the brackets to find the outermost `}`**: start from the first `{` after `%union` and simultaneously track
   the depths of the three bracket kinds `{}` / `[]` / `()`, so that the first `}` encountered when all three depths are 0 is
   the end marker of `%union` — hence the `}` in `struct { int x; } p;` is not mistaken for the end.
2. **Split member segments by `;` only at the top level** (likewise balanced over the three bracket kinds), so the
   `;` in `int (*fn)(int, char *);` does not split a segment. **A top-level comma is a direct error**: a segment may have
   only one declarator, so `int a, b;` must be split across two lines.
3. **Extract the member name from the tail of the segment**: first strip off, as needed, the trailing `[...]` (array),
   `(...)` (the parameter list of a function pointer), and `: bit width` (bit-field), then take a contiguous run of
   identifiers going backwards. If this fails, an error is reported together with the offending segment's original text,
   suggesting that you first give the type a name with `typedef`.

Member declarations can therefore be written freely: nested `struct` / `union` / `enum` definitions, function pointer members,
array members, bit-fields, and `const` pointers are all fine. Other rules:

- `%union` is allowed at most once, and members are stored in declaration order.
- Terminals and non-terminals **share one global "symbol name → member name" mapping** (yacc semantics:
  `%type` is global, and the same symbol cannot use different members in different productions).
- Duplicate member names, one symbol bound to two members, and `%type` / `%token <member>` referencing an undeclared
  member are all reported at generation time; when `%union` is written, the user `%code` may not define
  `ParserValue_T` again (this type is appended by the generator).
- To make all symbols share the same type, a one-line `%value_type` declaration suffices (see §2.1);
  `%union` is the advanced form for "one C type per symbol".
- `%union` may reference types defined in the user `%code` (the generator appends the union after the user code);
  see `example/union_demo.gram` for a complete example.
- When `%union` is written, `ParserToken_T.value` itself is also `ParserValue_T`, and the lexer writes the lexeme directly
  into the corresponding member according to `%token <member>` (numbers write the parsed numeric value, while double-quoted
  strings, identifiers, and `%token regex` write the matched text), so the terminal's semantic value usually does not need
  `PARSER_USER_TOKEN_VALUE` written again. For details, see [`07-codegen.md`](../07-codegen.md) §4.1 and
  [`08-generated-code.md`](../08-generated-code.md) §9.
- A terminal with an unbound member gets no semantic value written by the lexer (there is nowhere to put it in the union),
  leaving `value` at zero; when the built-in `NUMBER` is left unbound (`%union` is written but `%token <member> NUMBER` is
  not, while the grammar still uses `NUMBER`), the generator adds a `警告：` at generation time, and the artifact still compiles.

### `%destructor`: Registering Cleanup Code per Member

```text
%union {
    char        *str;
    struct Node *node;
    int          ival;
}
%type  <str>  list item
%token <node> VAR

%destructor <str>  { free($$); }
%destructor <node> { Node_Free($$); }
```

The memory for semantic values belongs to the user, and the runtime will not guess "which member needs freeing" on your behalf — `%destructor`
is how you tell it this information: it registers a piece of cleanup code **per `%union` member**, run when that member's value is discarded.

Syntax and validation:

- The fragment is C statements collected verbatim inside `{ ... }` (as with `%union`, only bracket balancing is done, without parsing
  C); only `$$` is recognized inside the fragment, expanding to "the value currently being discarded" (`pValue->member` in the C
  artifacts, likewise in the C++17 artifacts). Forms that need right-hand-side symbols, such as `$1` / `$line`, are errors at generation time.
- `%destructor` requires `%union` to be written first; `<member>` must be a member declared in `%union`;
  and only one entry may be written per member. All three violations are errors at generation time and point back to the original file line number.
- Writing `%destructor` under `%lang python` / `rust` / `go` is a direct error: Python's semantic values are reclaimed by reference
  counting, Rust's by ownership, and Go's by the GC, none of which has a "call cleanup code when discarded" layer. LL(0) /
  LL(1) / LL(k) and the LR family are all supported.
- Members without a `%destructor` keep their previous behavior: the C artifacts still call the
  `PARSER_USER_DISCARD_VALUE` hook for every discarded value (the old style is unaffected), and the C++17 artifacts have no such hook.

When it is called (the same rules as bison):

| Timing | Description |
| --- | --- |
| Error-recovery pop | The slots popped during `%sync` / `error` production recovery; for the LL family, the semantic frame slots that are popped |
| Parse-failure wrap-up | Before returning failure, every semantic value still on the stack is discarded one by one |
| Successful parse | **No stack cleanup**: the start symbol's result is handed back to the caller, and pointers inside the result belong to the caller |

The runtime calls `%destructor` only on slots that "really had a value written" (the LL semantic frame records which grammar
symbol each slot belongs to), so it never runs code such as `free` on uninitialized slots.

For a complete example (with a counter self-check) see `example/destructor_demo.gram`; for the artifact forms see
[`08-generated-code.md`](../08-generated-code.md) §9.1.

### `%if` / `%elif` / `%else` / `%endif`: Conditional Compilation

The same grammar produces different content depending on the scenario; the conditions come from the command-line `--define NAME[=VALUE]`, and any variable not passed
is false. Conditional compilation is processed line by line by the preprocessing layer **before the grammar is parsed**: whole lines whose condition is false are dropped, and lines whose condition
is true are kept verbatim (so dropped lines take up no original file line numbers).

```text
%if DEBUG
%token <ival> TRACE        /* 调试版才有的 Token */
%endif

%if LOG && !QUIET
%define BANG "!"
%elif TRACE
%define BANG "!"
%else
/* 普通版一个都不加 */
%endif
```

Conditional expressions (only these four operators and parentheses; there is no notion of "values", only true / false):

| Form | Meaning |
| --- | --- |
| `NAME` | The variable is true |
| `!NAME` | Negation |
| `A && B` | And |
| `A \|\| B` | Or |
| `( ... )` | Parenthesized grouping |

Precedence from low to high: `\|\|` < `&&` < `!` < parentheses < variable names. `==` / `!=` / `<` / arithmetic /
function calls / string literals are all **not supported** (`%if A == B` reports "条件表达式里有非法字符 '='").

Values of `--define` (repeatable; for the same name, **a later value overrides an earlier one**):

| Form | True/False |
| --- | --- |
| `--define NAME` | True (equivalent to `NAME=1`) |
| `--define NAME=1` / `=yes` / `=true` / `=on` / any other non-empty value | True |
| `--define NAME=0` / `=false` / `=no` / `=off` (case-insensitive) | False |
| `--define NAME=` (empty value), or simply not passing it | False |

Scope of effect:

- **Grammar layer**: `%if` can wrap any grammar content — `%token` / `%define` / `%left` /
  `%start` / productions / a whole `%code ... %endcode` block;
- **Action layer**: `%if` can be written inside a semantic action `{ ... }` to decide which lines of C code enter the artifact;
- **Not supported**: using `%if` to determine the artifact list (e.g. wrapping `%copy` / `%tree`), or referencing
  grammar symbols in a condition (e.g. "whether a certain token has been declared").

Edge cases (all of which are errors, and the line numbers point back to the **original file**):

- `%if` without `%endif` (including nested `%if` not paired), or `%endif` without a matching `%if`;
- `%elif` / `%else` appearing outside an `%if`, multiple `%else` in one `%if` block, or `%elif` appearing
  after `%else`;
- Conditional-expression syntax errors: unbalanced parentheses, an operator missing an operand, illegal characters;
- No condition after `%if`; extra content after `%else` / `%endif`;
- `%ifdef` / `%ifndef` are not supported; write `%if NAME` / `%if !NAME` instead.

A few more conventions:

- The condition must be written **on the same line** as `%if` (for multiple lines, use nested `%if`);
- A `%code ... %endcode` block is **opaque** to `%if` (the `%if` inside it is treated as ordinary code);
  to inject user code conditionally, wrap the whole `%code` block in `%if`;
- `%if` inside comments (`//`, `/* */`) and string literals is not treated as a directive;
- Variable names are **case-sensitive** (`DEBUG` and `debug` are two different variables); a variable that cannot be referenced is false and raises no error;
- The code that does not enter the artifact because its condition is false is also invisible to the C compiler on error — this is normal for "scenario-based trimming".

Example: `example/if_demo.gram`. If you only want to see the preprocessing result, add
`--preprocess-only` (it generates no artifacts and prints the processed text and the line-number mapping).

### `%code` … `%endcode`: User Code Blocks

```text
%code
typedef struct MyNode MyNode;
#define PARSER_USER_TOKEN_VALUE(pToken) ((ParserValue_T)((pToken)->value))
%endcode
```

`%code ... %endcode` is currently the only user code block syntax: the body goes into the artifact `parser_user.h`
(under C++17 it is `<prefix>_parser_user.h`), participating in the same validation (`%value_type` / `%union` and
the conflict check against "typing `ParserValue_T` yourself in user code") and likewise carrying `#line` (pointing forward to the first line of the body).

Rules:

- Both `%code` and `%endcode` must be **on their own line** (only whitespace is allowed around them); the body starts on the line after `%code`
  and ends at the `%endcode` line, collected **verbatim** (no parsing, no escaping, no brace wrapping — the `%code { … }`
  form is not supported: braces would become entangled with the braces in the user's C code);
- Multiple blocks may be written and are spliced into `parser_user.h` in order of appearance;
- A `%code` block is an **opaque region to the conditional compilation layer**: the `%if` / `%endif`
  inside it is just ordinary user code and is not treated as a directive;
- Error branches: `%code` missing `%endcode` (reports the `%code` line), or `%endcode` without a matching
  `%code` (reports the `%endcode` line).

Declarations, `typedef`s, macros, and `static inline` functions can be placed here; for non-`inline`
function definitions use `%epilogue` (`parser_user.h` is included by several `.c` files, and duplicate definitions will blow up at link time).
Example: `example/code_demo.gram`.

### `%namespace`: Namespace and File Name Prefix for C++17 Artifacts

```text
%namespace myapp
```

The namespace name and file name prefix of the C++17 artifacts (`--lang cpp17`) both use this name:

| Default (not written) | With `%namespace myapp` |
| --- | --- |
| `namespace Parser { ... }` | `namespace myapp { ... }` |
| `Parser::Engine::runText` / `Parser::Token` | `myapp::Engine::runText` / `myapp::Token` |
| `parser.hpp` / `parser.cpp` | `myapp_parser.hpp` / `myapp_parser.cpp` |
| `main.cpp` / `parser_user.h` | `myapp_main.cpp` / `myapp_parser_user.h` |
| `using namespace Parser;` in main.cpp | `using namespace myapp;` |

Rules:

- The name must match `[A-Za-z_][A-Za-z0-9_]*`, may not be a C++ keyword, and **multi-level names are not accepted**
  (`%namespace my::lang` is an error);
- The command-line `--namespace NAME` overrides `%namespace` (consistent with `--mode` overriding `%mode`, with no error);
- **An empty name means falling back to the default**: when `%namespace` is not written and `--namespace` is not passed (or an empty string is passed),
  the namespace is `Parser` and file names carry no prefix;
- `Makefile` / `CMakeLists.txt` / `RESULT.md` do not get the prefix (build scripts and documentation);
  files copied in by `%copy` keep their original names; the two target names `libparser.a` and `parser_demo` also stay unchanged;
- Header guards are generated from the prefix (`ALPHA_PARSER_H`), so the headers of two artifacts can be included at the same time;
  the `#include "…"` in the generated code and the `#line` recovery lines both use the prefixed names;
- **The C99 artifacts cannot use it**: `%namespace` is C++17-only (C artifacts have no namespace and their file names
  carry no prefix), so "writing `%namespace` or passing `--namespace`" together with `--lang c` is
  a direct error rather than a silent ignore; `--lang python` is likewise an error (to change the class name in Python, use `%class`).

Its purpose is multi-instance coexistence: when two generator artifacts are linked together under C++, the namespace and file names no longer
collide (for details and caveats, see the "Replacing the Namespace and File Name Prefix" section of
[`14-languages-and-products.md`](../14-languages-and-products.md)). Example:
`example/namespace_demo.gram`.

### `%class`: The Parser Class Name of the Python Artifacts (`--lang python`)

```text
%lang python
%class PyCalc
```

The Python artifacts contain only two files, `parser.py` / `main.py`: `parser.py` holds module-level constants
(status codes, Token, `Tree`, `ParserError`) plus an ordinary Python class whose name is taken from `%class`
(defaulting to `Parser`); `main.py` does `import parser` and then uses the alias `Parser = parser.<class name>`,
so the demo body does not need changing when the class name changes.

Rules:

- The name must match `[A-Za-z_][A-Za-z0-9_]*` and **may not be a Python keyword** (`%class class`
  reports "不能用 Python 关键字");
- The command-line `--class NAME` overrides `%class` (consistent with `--mode` overriding `%mode`, with no error),
  and an empty string is the same as not writing it (falling back to `Parser`);
- **Valid only for the Python artifacts**: the interface names of the C / C++17 artifacts are fixed (to change the namespace use
  `%namespace` / `--namespace`), so pairing it with `--lang c` / `--lang cpp17` is a direct error;
- The artifact file names are unrelated to the class name and are always `parser.py` / `main.py`.

Where user code lands in the Python artifacts (also "four places", as with C / C++17):

| What to put | Where to put it |
| --- | --- |
| Helper functions, constants, classes | `%code` … `%endcode`: inserted after the `import`s and before the generated constants in `parser.py`, concatenated in order of appearance |
| More helper functions | `%epilogue`: appended to the end of `parser.py` (in the same module as the parser class, so its internal methods can be called directly) |
| Your own entry point | `%main`: replaces the whole demo body of `main.py` (the header comment, `import parser`, and the class-name alias `Parser` are kept) |
| Standalone files | `%copy`: copied into the artifact directory as-is (in Python you must `import` them yourself) |

The body inserted into the Python module is **normalized** according to Python's indentation rules (done for `%code` /
`%epilogue` / `%main` alike): first the leading and trailing blank lines are removed, then the common indentation is stripped using the **minimal leading
whitespace** of all non-empty lines, and finally the leading tabs of the remaining lines are replaced with 4 spaces. So writing with the grammar's layout indentation will not turn into an
`IndentationError`, and no character other than leading whitespace is changed.

The Python artifacts **do not support** these C-only constructs, and writing them is a direct error at generation time (not a silent ignore):

- `%value_type` / `%union` / `%type <member>` / `%token <member>` — Python's semantic values are
  arbitrary objects, and there is no such thing as a "union member";
- `%namespace` / `--namespace` (see the previous section).

All other directives work: `%mode` (all sixteen are supported), `%start`, `%define` / `%token` /
`%token regex` / `%identifier`, `%tree`, `%sync`, `error` productions and
`yyerrok` / `yyclearin`, `%max_*`, `%expect`, `%left` /
`%right` / `%nonassoc` / `%prec`, `%teach`, `%mermaid`, `%copy`, `%if`,
`%main` / `%epilogue`, `%lookahead`. Example: `example/python_demo.gram`.

### `%package` / `%import` / `%class_body`: Exclusive to the C#, Rust, and Go Artifacts

These three directives take effect only for `--lang csharp` / `--lang rust` / `--lang go` (C# yields
`namespace NAME;` / `using NAME;` / members inside the class, Rust yields the crate name / `use NAME;` /
things inside `impl`, and Go yields the module path / the import block / a set of `func (p *Parser) …` methods);
under other target languages the generation stage emits a note "只对 --lang csharp / rust / go 有意义，已忽略",
and then generates as usual (it neither aborts nor changes behavior silently).

```text
%package MyApp.Compiler          /* C#: namespace MyApp.Compiler; */
%import System.Collections       /* C#: using System.Collections; */
%import System.Collections.*     /* 末段可以是 * */

%class_body                      /* 正文原样插进解析器类内部 */
    /** 辅助方法：把一个语义值翻倍（这里写目标语言自己的代码） */
    public double doubled()
    {
        result();                /* Parser.cs 里的公开入口 */
        return 0.0;
    }
%endbody
```

| Directive | Rules |
| --- | --- |
| `%package NAME` | NAME is a segmented identifier (`a`, `a.b.c`, `a::b`); an empty or invalid one is a direct error; it may be written only once per grammar. In the artifacts it is a **file-level** `namespace NAME;` (written after the `using`s) |
| `%import NAME` | Repeatable, generating `using NAME;` in order of appearance; NAME is a segmented identifier (joined by `.` or `::`), and the last segment may be `*`. The `using`s are always placed before the `namespace` declaration (a C# syntax requirement) |
| `%class_body` … `%endbody` | The same scheme as `%code` … `%endcode`: the two markers **each occupy their own line**, and the body between them is not parsed, not escaped, and not brace-balanced, being inserted verbatim into the parser class (for auxiliary methods / fields / inner classes); only one `%class_body` is allowed, and a missing `%endbody` or an extra `%endbody` is an error at generation time |

### `%lookahead` and k: LR(k) / LALR(k) / SLR(k) / LL(k) / LL(0)

```text
%mode lrk            /* 规范 LR(k)：不合并同心状态 */
%lookahead 2         /* k = 2；不写时缺省也是 2 */
```

**k can be written anywhere.** The mode name is case-insensitive, and k can also be written directly into the parentheses or appended after
(the command-line `--mode` and the grammar's `%mode` share the same set of forms; once k is written, `%lookahead` is no longer needed):

| Form | Equivalent to |
| --- | --- |
| `LL(0)` / `LL0` / `ll0` | LL(0): no lookahead |
| `LL(1)` / `LL1` / `ll1` | LL(1) |
| `LL(k)` / `LLK` / `llk` | LL(k), with k given by `%lookahead` / `--lookahead` (default 2) |
| `LL(2)` / `LL2` (likewise `LL3`) | LL(k) with k = 2 (/3) |
| `SLR` / `SLR(1)` / `slr` | SLR(1) |
| `SLR(k)` / `SLRK` / `slrk`, `SLR(2)` / `SLR2` | SLR(k), with k as above / hard-coded to 2 |
| `LALR` / `LALR(1)`, `LALR(k)` / `LALRK`, `LALR(2)` / `LALR2` | Likewise |
| `LR(0)` / `LR0`, `LR(1)` / `LR1`, `LR(k)` / `LRK`, `LR(2)` / `LR2` | Likewise (`LR(0)` is `lr0`, `LR(1)` is `lr1`) |
| `IELR` / `IELR(1)` / `ielr` | IELR(1) |

> A form with parentheses on the command line needs quoting in the shell: `--mode 'LL(2)'`.

**Values of k.** `%lookahead N` ranges over **0..INT_MAX**, defaulting to **2** — **there is no policy limit whatsoever**,
only one physical lower bound of "error out if the encoding does not fit":

- **N = 1**: `lrk` / `lalrk` / `slrk` / `llk` are equivalent to `lr1` / `lalr` / `slr` /
  `ll1` respectively (the analysis results and artifacts are identical; only the `[生成模式]` heading and the virtual start symbol name differ).
- **N = 0**: meaningful only for the LL family, namely **LL(0)** — it looks at no lookahead at all, so each
  non-terminal can have only one production.
- **No upper bound**: k can be written as large as you like (0..INT_MAX). But be aware of two costs:
  1. The set sizes of `FIRST_k` / `FOLLOW_k` and the number of k-tuple columns both **grow exponentially** with k —
     above k = 3, basically only teaching or toy grammars can hold up. For k > 3 the generation stage adds a reminder,
     but **will not stop you**;
  2. The column encoding is $\sum_i \mathrm{Token号}_i\cdot(\text{Token 个数})^i$, and when `Token 个数^k`
     does not fit in 64 bits the artifact computes incorrectly, so the generation stage **errors out and gives the largest k this grammar can use**
     (e.g. `--lookahead 1000000000` tells you "这个文法最多 k = 15"). This is a limit of the encoding
     width, not a policy upper bound we impose on k.
- The table size follows **the number of k-tuples actually used**, not `Token 数^k` (but the two items in the previous bullet still grow exponentially).
- Writing `%lookahead` with any other mode (including `ll1` / `lalr` …) is **a direct error**, not a silent ignore.
- The command-line `--lookahead N` overrides `%lookahead` (consistent with `--mode` overriding `%mode`).
- If k is written in the mode name and `%lookahead` is also written with a different value → error (writing it in just one place suffices).

**The four modes with k.** They come in pairs corresponding to the two routes, the "LR side" and the "LL side":

| Mode | Analysis | Where the lookahead comes from | Equivalent to at k = 1 |
| --- | --- | --- | --- |
| `lrk` | Canonical LR(k) item sets (no merging of like states) | The k-tuples carried by the items | `lr1` |
| `lalrk` | The same item sets + merging of like states | The k-tuple set after merging | `lalr` |
| `slrk` | LR(0) item sets | **FOLLOW_k (looking only at the left-hand side)** | `slr` |
| `llk` | Top-down predictive analysis (including left-recursion elimination) | **SELECT_k(A→α) = FIRST_k(α·FOLLOW_k(A))** | `ll1` |

`lrk` / `lalrk` / `slrk` are all bottom-up, deciding the reduction from the k-tuple lookahead (the three share one
table and runtime), while `llk` is top-down, deciding which production to expand from k tokens. **SLR(k) is strictly weaker than
LR(k)**: when reducing it looks only at the left-hand side's FOLLOW_k, not the context — on `example/lrk_demo.gram`,
SLR(1) / SLR(2) / SLR(3) all have 2 conflicts, while LR(2) has 0 (FOLLOW_k(X) and FOLLOW_k(Y)
happen to be equal, so lengthening k does not separate them).

**LL(0).** Zero lookahead symbols yield no choice information, so `M[A, ε]` can have only one production:
an LL(0) grammar is exactly "each non-terminal has exactly one production", and the whole language has just one string. It is
the starting point of the chain LL(1) ⊆ LL(2) ⊆ …, and can be written as `%mode LL(0)` / `%mode ll0` /
`%lookahead 0` (with the LL family); see `example/ll0_demo.gram` for an example.

When it is worth using: grammars where **LALR(1) has conflicts but LR(k) does not**. The classic example is
`example/lrk_demo.gram` (`S -> A X C | A Y D | B Y C | B X D; X -> E; Y -> E`):

| Mode | Unresolved conflicts |
| --- | --- |
| `%mode lr0` | 9 |
| `%mode slr` / `%mode lalr` | 2 |
| `%mode lr1` / `%mode ielr` / `%mode lrk` (k = 1/2/3) | **0** |
| `%mode lalrk` (k = 1/2/3) | 2 (this grammar is not LALR(k) for any k) |

The last row is worth remembering: **LALR(k) is strictly weaker than LR(k)**. The discrimination lost by merging like states
cannot be recovered by lengthening k — that information was gone the moment they were merged. To fix it, do not merge states (`lrk`), or let IELR
split them as needed (`ielr`); the top-down route can try `llk` (example `example/llk_demo.gram`: a
LL(2) rather than LL(1) grammar). For the analysis-layer construction see [`06-analysis.md`](../06-analysis.md)
§2.12 / §2.13, and for the extra table layer in the artifacts see [`07-codegen.md`](../07-codegen.md).

### Capacity Macros: `%max_stack_depth` / `%max_errors` / `%error_recovery_tokens`

The three capacity limits can be set either in the grammar or overridden at compile time with `-D`:

```text
%max_stack_depth 256        /* PARSER_MAX_STACK_DEPTH = 256 */
%max_errors 16              /* PARSER_MAX_ERRORS = 16 */
%error_recovery_tokens 0    /* PARSER_ERROR_RECOVERY_TOKENS = 0：恢复后立刻报下一个错 */
```

| Directive | Macro | Range | Default | Effect |
| --- | --- | --- | --- | --- |
| `%max_stack_depth N` | `PARSER_MAX_STACK_DEPTH` | 1..65535 | 64 | The analysis stack depth limit; hitting it reports `PARSER_ERR_STACK` (-20) |
| `%max_errors N` | `PARSER_MAX_ERRORS` | 1..4096 | 8 | The maximum number of errors retained per parse (see [`08-generated-code.md`](../08-generated-code.md) §2.2) |
| `%error_recovery_tokens N` | `PARSER_ERROR_RECOVERY_TOKENS` | 0..4096 | 3 | The number of tokens for "suppressing repeated errors" after error recovery (bison's 3-token rule); 0 = no suppression |

Conventions:

- The artifacts still use `#ifndef <macro> / #define <macro> <value>u / #endif`, so **a compile-time `-D` takes precedence**,
  and editing the template also works;
- Each directive may be written only once; the value must be an integer, and out-of-range values / non-numeric input / extra trailing content all
  error at generation time (with the original file line number);
- If any of them is written, the artifact's `RESULT.md` gains a "capacity macros" section listing the current values and sources of the three macros;
- When none is written, the original values in the template are used.

When none of the three is written, all three macros take their default values.

### `%epilogue`: Appending a Piece of User Code to the End of the Generated Code

```text
%code
/* 声明、宏、static inline 函数 */
%endcode

%epilogue
/* 普通函数定义写在这里：原样贴到 parser.c 末尾 */
double MyHelper(double x)
{
    return x * 2.0;
}
```

Rules:

- `%epilogue` must be **on its own line** (only whitespace is allowed around it), and the content after it is collected **verbatim** (no parsing,
  no escaping, no `%code` wrapping) until the next "line starting with `%`" or the end of the file;
- A grammar may have at most one `%epilogue`; writing a second one reports「%epilogue 只允许一个」;
- The insertion point is the **very end** of the generated `parser.c` (`parser.cpp` for C++17), after `parser_user.h`
  has already been included, so the types, macros, and function declarations declared in `%code` can be used directly;
- **Under the Python artifacts there is no `parser_user.h` and no `#line`** (Python has no such directive);
  the body is normalized by Python's indentation rules (stripping the common indentation, replacing leading tabs with 4 spaces) and then appended to
  the end of `parser.py`, preceded by a comment line `# 语法文件 <文法文件> 行 <N>` marking its
  position in the `.gram` (pointing to the same line as the C target's `#line`, see
  [`07-codegen.md`](../07-codegen.md) §4.2); the body is in the same module as the parser class, so the
  class's internal methods can be called directly;
- It adds no new artifact file — it is part of `parser.c`;
- **Under C++17 this code sits outside the namespace**: what you write is an ordinary C++ function, linkable from other compilation units
  by ordinary C++ linkage rules. Before the body the generator inserts a line `using namespace <namespace>;`
  (`Parser` by default, or that name when `%namespace` is used), and the body is **pasted as-is, with no rewriting whatsoever**;
  the C names `Parser_RunText(...)` / `Parser_GetLastError()` / `ParserError_T` can still be written there,
  relying on the compatibility entry layer included in the C++17 artifacts (see
  [`08-generated-code.md`](../08-generated-code.md) §1.2). New code can also use
  `Engine` directly: `engine.runText(text, &result)` / `engine.lastError()`.

`%epilogue` **may not define `main`**: the generator performs a simple lexical scan (looking for declarations such as `main(...)` / `int main(...)`
after ignoring whitespace and comments at the start of a line), and on a hit reports
「%epilogue 里不允许定义 main；请把 main 放到 %main 段里」. It prefers false positives over false negatives —
a miss would blow up at link time with two `main`s. The check is done **after** conditional compilation, so a `main` wrapped in `%if`
is still caught.

### `%main`: Replacing the Built-in Demo main with Your Own main

```text
%main
int main(void)
{
    ParserValue_T result;
    if (Parser_RunText("1+2*3", &result) != PARSER_OK) {
        return 1;
    }
    printf("%g\n", (double)result);
    return 0;
}
```

Rules:

- `%main` must be **on its own line**, and the body is collected verbatim until the next "line starting with `%`" or the end of the file;
- The body goes into the artifact's `main.c` (`main.cpp` for C++17), **replacing the body of the built-in demo main**,
  while the shell (`#include <stdio.h>` / `<string.h>` / `"parser.h"`) is unchanged and the artifact list is unchanged;
- In the C++17 artifact's `main.cpp`, the template inserts a line `using namespace <namespace>;` after the last `#include`,
  and the body is pasted as-is with no rewriting, so C names such as `Parser_RunText` (the compatibility layer) can still be written there,
  as can `Engine engine; engine.runText(...)` directly;
- Under the Python artifacts the body replaces the entire **demo body** of `main.py` (the built-in `_read_line()` /
  `main()` and `if __name__ == "__main__": sys.exit(main())` are all here), while the shell's
  header comment, `import sys` / `import parser`, and the class-name alias `Parser` are kept; the body is pasted as-is,
  normalized only by Python's indentation rules, so the entry call must be written yourself (run directly at the top level, or add your own
  `if __name__` block);
- `%main` may be written in multiple blocks (combined with `%if` to express branches), but **only one may take effect at a time**:

| Number of `%main` blocks in effect after conditional compilation | Result |
| --- | --- |
| 0 | The generator's built-in demo main is used |
| 1 | This block replaces the body of `main.c` |
| >1 | The generation stage errors with「多个 %main 同时生效」and points back to the original file line number of the second block |

- Both ways of combining with `%if` are supported (the preprocessing layer has already handled `%if`): writing `%if` inside a `%main` block
  affects only the content within the block; wrapping the whole `%main` in `%if` makes the entire block inactive when the condition is false;
- To override `main` you can also use `%copy file my_main.c` (see §7): `%main` suits short entry points,
  while `%copy` suits long entry points that need to include a pile of your own headers. **Do not use both at once**, or the two
  `main`s will fail to link.

### Whitespace / Newline as Tokens: `\b` and `%space`

By default the generative lexer skips whitespace (space / `\t` / `\n` / `\v` / `\f` / `\r`) before taking a token,
so to write a grammar by "line" or "whitespace-separated", you must first turn whitespace into tokens:

```gram
%token NL "\n"      /* 换行是真实 Token：可以在产生式里直接写 NL */
%token SP "\b"      /* \b = 一个空白字符（空格 / \t / \n / \v / \f / \r 都命中） */
%space " \t"        /* 取 Token 前只跳过空格与制表；换行归 NL 管 */
```

The three forms each cover one aspect:

| Form | Effect |
| --- | --- |
| `%token NAME "\n"` / `"\t"` / `" "` | When whitespace is written in a literal, those characters are **no longer skipped**: on a match this token is produced, and productions can be written for it |
| `%token NAME "\b"` | `\b` is a wildcard for "one whitespace character": space, tab, and newline all match, and the lexeme is the actual character in the input. It is both a **parsing target** and a "force one whitespace" |
| `%space "whitespace character set"` | Declares **which whitespace is skipped before taking a token**: the default is equivalent to `%space "\b"` (all whitespace), and `%space ""` means none at all is skipped |

Rule details:

- When whitespace is encountered before taking a token, the lexer first checks "whether a fixed literal starting with whitespace / `\b` can
  match at the current position": if so it stops and hands it to lexical matching to be used as a token; otherwise it skips according to
  the `%space` set. So **declaring a whitespace token takes effect automatically**, with no extra switch; for a grammar that declares no whitespace
  token the skipping logic is as before, and the lexical behavior is completely unchanged (the literal rule table merely gains two
  marker fields).
- `\b` has this meaning only in fixed literals (`%define` / `%token NAME "..."`).
  In a `%token regex` regex, write `\s` (the regex character-class escape); `\b` in a regex is still the ordinary
  character `b`.
- The `%space` argument may only be whitespace characters and `\b`, with `|` separating multiple candidates (the same form as `%define`);
  any other character, or writing it twice, is a direct error.
- When a fixed literal (including one with `\b`) matches, the lexeme is written into `pName` / the `pText` of `%tree`,
  but **no `%union` member is written** (consistent with other fixed literals); to get the value, read the lexeme yourself, or switch to
  `%token regex`.
- Whitespace that is not in the `%space` set and is not covered by a token makes the lexer report "词法解析失败";
  this is exactly how you express "this kind of whitespace must not appear".
- The built-in demo main feeds only **one line** of input (the trailing newline is consumed), so to try a newline token like `NL`
  you must write your own `%main` to read the entire standard input, or call the API directly:
  `Parser_RunText("a\nb\n", &result)` / `engine.runText("a\nb\n", &result)` /
  `engine.run_text("a\nb\n")`.

For a complete example see `example/space_demo.gram` (paired with [`10-examples.md`](../10-examples.md)).

### Maintaining a Literal Alias Table with CSV: `%token NAME include "..."`

For cases where "one token has a huge number of literals", such as keyword tables / directive tables / enum name tables, put the alias list into
a CSV and append it with a single `%token` directive:

```gram
%token A include "kw.csv"                 /* A 的别名来自 kw.csv */
%token <ival> NUM include "numbers.csv"   /* 带成员的 Token 也能用 */
```

There is just one semantics:

> Open the CSV and treat it as a two-column alias table; take only the rows whose **first column** equals `A` (or `NUM`),
> and append each row's **second column**, as a **literal**, to the end of that token's alias list.

It is equivalent to expanding each row into `%token A "…"` — the hand-written form for the same CSV and the same rows is
`%token A "let|print|while"` (this is how multiple literals of one token are written in a grammar).

**It only appends aliases; it does not declare tokens:**

- The token must already be, or simultaneously be, declared by an ordinary `%token`; this directive only adds aliases and creates no new token
  (`%token A include "…"` itself also counts as a declaration).
- If the CSV's first column contains a token name that **does not exist at all** in the grammar → the whole row is **completely ignored**, with no error,
  no creation, and no effect on other tokens.
- If the CSV's first column contains some token name but the grammar **does not** use `include` for that token
  → likewise ignored (this is the mechanism for "headers / unrelated rows are not validated").
- It does not change a token's category: `%token regex A "…"` and `%token A include "…"` can coexist,
  and the latter only appends literal aliases.
- A token with a member is allowed to use `include`: `%token <ival> NUM include "…"` only appends literal aliases for `NUM`,
  the member type `<ival>` is unchanged, and the token matched by the aliases is still `NUM`.

Many combinations are supported, and the results are merged in declaration order:

```gram
%token A "x"
%token A include "kw.csv"        /* 手写在前、CSV 在后 → 别名顺序 x, <kw.csv 的行…> */
%token A include "kw2.csv"       /* 同一 Token 引用多份 CSV */
```

```gram
%token A include "kw.csv"        /* 两个 Token 共享同一份 CSV，各取各的 */
%token B include "kw.csv"
```

**Alias uniqueness** (a hard rule):

| Situation | Result |
| --- | --- |
| Duplicate aliases under the same token | Silently deduplicated, keeping first-appearance order |
| The same literal used as an alias across different tokens | **Error**, naming both sources (the CSV row + the first assignment site) |
| An alias identical to a token name (e.g. the CSV appends the alias `"B"` to `A` while the grammar already has `%token B`) | **Error** |
| An alias versus a `%token regex` pattern | **No conflict**: regex tokens do not participate in literal alias uniqueness, and follow only the existing longest-match / declaration-order rules |

Uniqueness is enforced by a single full scan **at the expansion stage**: all `%token … include` in the grammar are collected first and then judged together,
so it does not depend on the order in which they are "expanded and seen".

**CSV format** (a minimal RFC 4180 style; **the header is ignored, not validated, and only the first two columns are read**):

- Only each row's **column 1** (the token name) and **column 2** (the literal) are read; column 3 onward is always ignored.
- The first row is not required to be a header, and its content is not validated; when the first row happens to be a header (such as `token,word`),
  its first column matches no imported token name and is **naturally ignored**.
- Empty lines are skipped; a leading `#` is treated as a comment and skipped.
- Column 2 must be written as a **double-quoted string literal** (the same form as `%token NAME "…"`): `""` is supported
  to mean one double quote, as are commas inside a field; **multi-line fields are not supported**.
- An empty column 1 / column 2 is an error; whitespace around columns 1 / 2 is **not automatically trimmed** (whitespace inside a
  literal is meaningful).
- CSV paths are resolved relative to **the directory of the `.gram` file that declares it**, and subdirectories are allowed.
- When the same CSV is referenced from several places within one `.gram`, the on-disk read result is cached by normalized path.

**Column 2 is always treated as a literal and is never parsed as a regex**: for regex tokens keep using
`%token regex NAME "…"`; this directive offers no regex channel, does not read a regex column from the CSV,
and you should not add a `regex` column to the CSV.

**Error format** (all carry file, line, and column):

```text
1.csv:5:2: 第 2 列（字面量）不能为空
1.csv:7:1: 第 1 列（Token 名）不能为空
1.csv:5:2: 别名 "x" 已被 token A 使用，不能重复分配给 token B（首次出现于 2.csv:3:2）
example/calc.gram:8:1: 找不到 CSV 文件 1.csv
```

Errors inside a CSV use a **path relative to the `.gram` directory**; errors in the `%token … include` line itself
(a missing file) use the grammar path + line and column. In the end all are printed by `main` as `错误：…`, with exit code 1.

**Relationship with other directives**:

- Expansion happens **after** `%if` conditional compilation and before `grammar_loader` officially parses — a
  `%token … include` wrapped in a false `%if` branch **reads no CSV and reports no missing file**.
- The alias declarations produced by expansion sit at the **original position** of that directive, ordered by CSV row.
- The export behavior of `--lang gram` is **unchanged**: it still exports the input `.gram` file as-is, and the expansion
  happens only in memory.
- **Not implemented**: `%commit include` / `%trivia include` / `%identifier include`,
  externalizing the whole token table as CSV, a regex column in the CSV, and the "create a new token" semantics for aliases.

Example: `example/token_dict_demo.gram` (including a token with an `<ival>` member, a comparison row not imported from the CSV,
and column-3 / column-4 noise).

### `%commit`: Collecting Recognized-but-Not-Parsed Text into a Container

`%commit` is the lexer layer's third channel: the matched text **enters neither syntactic analysis nor the token stream**,
but is collected into the parser's container by "line number + type + original text"; a typical use is to leave comments for post-processing
(documentation extraction, AI analysis logs, etc.). Three forms:

```gram
%commit block "/\*([^*]|\*+[^*/])*\*+/"   /* 块注释：类型 block */
%commit line  "//[^\n]*"                  /* 行注释：类型 line */
%commit "0x[0-9a-fA-F]+"                  /* 类型缺省 = "default" */
%commit skip  "@[A-Za-z]+"                /* 匹配即丢，不进容器 */
```

Grammar-layer parsing rules:

- The form is `%commit [type] "pattern"`: the type is an identifier (`[A-Za-z_][A-Za-z0-9_]*`), defaulting to the string `default`
  when omitted; `%commit skip "pattern"` is shorthand for "discard on match"
  (`skip` is a reserved type name, and `%commit skip` without a pattern is an error).
- The pattern must be a double-quoted literal and is executed by the selected regex engine. A form without metacharacters is a fixed literal.
  C supports DFA and the shared ordered runtime, including lazy matching, assertions and backreferences;
  C++ selects `std`, `dfa` or `vm`. Nullable patterns are rejected. See
  [regex compatibility](../21-regex-compatibility.md) for native-extension validation limits.
- `%commit` matching goes through the **same lexical match order** as `%token` (fixed literals / `%token regex`) and the built-in comment
  rules: longest match first, and for equal length by declaration order (earlier declarations win;
  within the same line, by writing order). **The same pattern cannot be both a token and a `%commit` /
  `%trivia`**: a duplicate of an already-declared fixed literal or regex is an error at generation time.
- Matching starts at the position "after skipping `%space` whitespace"; text hit by `%commit` is consumed
  (the lexical position advances), a `skip` fragment produces no container element, and the remaining fragments are appended in order of appearance.
- The line number is the 1-based line number of the fragment's **start position**; `text` is the matched original text (including the literal
  itself), and newlines inside a multi-line fragment are kept as-is. The column number / offset is not in a `%commit` element
  (for the syntax-layer position API see [`08-generated-code.md`](../08-generated-code.md)).
- `%commit` is a lexer-layer thing and does not change the grammar (`analysis_*` cannot see it at all);
  `%tree`, LL / LR, and `%mode lex` can all use it.
- Language coverage: all six backends support it (C99 `Parser_GetCommits()`, C++17 `Engine::commits()`,
  Python `engine.commits`, C# `Commits`, Rust `engine.commits()`,
  Go `engine.Commits()`). For the container's fields and
  ownership details see
  [`14-languages-and-products.md`](../14-languages-and-products.md).

For a complete example see `example/commit_demo.gram`.

#### Writing "Discard on Match" as `%trivia`

`%commit skip "<pattern>"` and `%trivia "<pattern>"` are two ways of writing the same rule: both mean
"the matched text enters neither the container nor the token stream". The only difference is grammar-layer readability —
`%commit skip` hides the "discard" in the type position of `%commit`, which is easily confused with "collect into the container",
so new grammars are advised to write:

```gram
%trivia "//[^\n]*"                     /* 行注释：匹配即丢 */
%trivia "/\*([^*]|\*+[^*/])*\*+/"      /* 块注释：匹配即丢 */
```

The two share one and the same "lexical declaration order", so they are fully consistent with the longest-match and
equal-length-earlier-declaration-wins rules of `%token` / `%commit`; a `%trivia` pattern likewise cannot duplicate a `%token`.
In `example/commit_demo.gram` one of each is kept (`%commit skip "@[A-Za-z]+"`
and `%trivia "#[A-Za-z]+"`); changing the `%trivia` line to `%commit skip` and regenerating
produces byte-for-byte identical artifacts and run results.

#### Repetition scope in block-comment regexes

For `(* ... *)` comments use:

```gram
%trivia "\(\*([^*]|\*+[^*)])*\*+\)"
```

The `*` after the group permits zero or more body fragments. Omitting it changes the accepted
language; that is not evidence of a negated-class or DFA construction bug. Empty bodies, ordinary
text, repeated stars and Chinese text are verified with C DFA, C++ standard regex and the shared
ordered runtime. Lazy expressions are also supported; C selects the ordered runtime for them.
See [the compatibility report](../21-regex-compatibility.md) for actual engine selection and guarantees.

### How the Four "Places to Write User Code" Are Divided

| What to put | Where to put it |
| --- | --- |
| Declarations, `typedef`s, macros, `static inline` functions | `%code ... %endcode` (goes into `parser_user.h`, included by `parser.c` / `parser_token.c` etc.) |
| Ordinary function definitions, `static` helper functions | `%epilogue` (goes into the end of `parser.c`) |
| `main` | `%main` (goes into `main.c`) |
| Standalone compilation units (your own `.c` / `.h`) | `%copy` (copied into the artifact directory and added to Makefile / CMakeLists) |

A counterexample: cramming a non-`inline` function definition into `%code` — `parser_user.h` is included by several `.c`
files, which at link time means multiple duplicate definitions. For positive examples see `example/epilogue_demo.gram` (declared in
`%code`, implemented in `%epilogue`, called from an action) and `example/main_demo.gram`.

## 3. Semantic Actions

| Form | Meaning |
| --- | --- |
| `$$` | The return value of this production |
| `$1` `$2` … | The value of the n-th symbol on the right-hand side |
| `$$.<field>` | Assigns by field when the semantic value is a struct (`$$` can be used as an lvalue) |
| `int32(0)` / `float(1.5)` | Turned into the C casts `(int32_t)(0)` / `(float)(1.5)` |
| `$1.<field>` | Ordinary C member access (`$n` cannot appear on the left of an assignment) |
| `sqrt($3)` / `pow($1,$3)` | Generated as a call to the C function of the same name; `#include <math.h>` yourself |
| `$1` `$3` (under `%union`) | Write the operation as usual; the generator automatically adds the member by symbol: if `$1` is `term` (`<ival>`), it expands to `values[1].ival` |
| `$$` (under `%union`) | Expands to `result.value.<member of the left-hand symbol>` |
| `$n.<member name>` | A hand-written member form passes through as-is |

Arithmetic/comparison/bitwise operations are generated as ordinary C operators; no C type checking is done at generation time (`$$ = $1` is emitted even
when the types do not match, leaving it to the C compiler). `$n = expression` (assigning to a right-hand-side symbol) is an error, whose message carries the
production number and the character position in the action. Any unsupported form in an action is an error at generation time; for the list see
§5 "Generation-Time Validation".

Under `%union` the semantic value is a union: a form such as `$$ = $1 + $3;` needs no change, and the generator adds `.member` according to the
member bound to each symbol.

LL(1) and the LR family (SLR/LALR/IELR) take `$n` values differently, translated per mode by `ActionTranslator`; see
[`07-codegen.md`](../07-codegen.md).

### 3.1 `error` Productions (yacc-Style Recovery Anchors)

`error` is a reserved pseudo-terminal, written on the **right-hand side** of a production just like an ordinary token:

```gram
%mode lalr

stmt_list := stmt_list stmt { $$ = $2; } | stmt { $$ = $1; };

stmt := expr SEMI  { $$ = $1; }
      | error SEMI { yyerrok; $$ = 0; };   /* 坏语句整条丢掉，接着解析下一条 */
```

When a syntax error is encountered at runtime, the stack is first popped to a state that can shift `error`, and `error` is shifted
as a pseudo-token (`errstatus = 3`) **without consuming the current lookahead**, returning to the main loop to continue
parsing with the same token. Shifting `error` pushes a zero-initialized `ParserValue_T`, so its `$n` is a garbage
placeholder value; do not rely on its contents.

Allowed / forbidden positions:

| Position | Result |
| --- | --- |
| Right-hand side of a production | Allowed |
| Left-hand side of a production (`error : ...`) | Error |
| An argument of `%type` / `%token` / `%token <member>` / `%define` / `%token regex` / `%start` / `%prec` | Error |
| A user-defined symbol name (a non-terminal or token named `error`) | Error (`error` is a reserved word) |

Mode restrictions:

- **The LR family** (`lr0` / `slr` / `lalr` / `lr1` / `ielr`) supports it; `error` is added to the terminal
  set and participates in item set construction and the ACTION table, which gains an extra `error` column (see
  [`06-analysis.md`](../06-analysis.md) §2.6).
- Writing an `error` production under **the LL family** (`ll0` / `ll1` / `llk`) is a generation-time error: top-down
  has no "pop the stack to the nearest recovery point" action, and its semantics are incompatible with yacc. Top-down has its own recovery
  means — **FOLLOW set recovery** (see §3.2) — which skips bad fragments without writing any directive; `%sync`
  merely adds an extra anchor.
- The FOLLOW set **does not contain `error`**: neither the `expect` set nor the LL(1) synchronization test propagates `error`
  as an ordinary terminal.

`yyerrok` / `yyclearin` are written in the action of an `error` rule and must be **a statement of their own**:

| Form | Meaning |
| --- | --- |
| `yyerrok;` | Clears the "currently recovering" state so that later errors are reported as usual |
| `yyclearin;` | Discards the current lookahead token (the next token is taken again by the main loop) |

The generated calls are `Parser_LrYyErrOk` / `Parser_LrYyClearIn` in the C99 artifacts, and the `Engine::Private` member
functions `yyErrOk()` / `yyClearIn()` in the C++17 artifacts. Writing either in an expression position is an error,
as is using them in LL(1) mode (only the LR family has the notion of an "error state"). When `yyerrok` is not written, no repeated errors are reported within
3 tokens (`PARSER_ERROR_RECOVERY_TOKENS`) after recovery, consistent with bison's 3-token rule.

### 3.2 Error Recovery in the Three Modes: `%sync` / FOLLOW Set / Token Swallowing

```gram
%mode lalr
%sync SEMI RBRACE        /* 恢复锚点，按声明序记录 */
```

#### 3.2.1 Bottom-Up (LR Family): `%sync` + `error` Productions

It first tries the `error` production layer; when no `error` column action is found, `%sync` serves as the fallback — pop the stack (before popping,
first finish the items on the stack awaiting reduction, a panic reduce — errors often occur in a half-reduced construct, e.g.
`1 + + 2` stuck at `expr PLUS . term`) to a state that can shift the sync token, discard input up to that token, shift it, and then continue
parsing.

#### 3.2.2 Top-Down (LL Family): FOLLOW Set Recovery

The LL family **recovers even without writing any recovery directive** — at runtime it automatically aligns in the following order (this is
"FOLLOW set recovery", consistent with ANTLR / the textbook panic mode):

1. The top-of-stack item **can take** the current token (a terminal is equal, or a non-terminal has a
   production in the prediction table of this column) → already aligned, and the main loop continues;
2. The current token is a user-declared `%sync` anchor → pop the stack to a position that can take it;
3. The top-of-stack item "should end" (**the current token is in its FOLLOW**), or an item deeper in the stack
   can take it → **abandon "the construct the top-of-stack item belongs to"**, popping it together with the inner constructs already expanded
   inside it (the inner frames clear their semantic values on popping, per the rules of §3.1), and hand the current token to the outer level;
4. No one recognizes it → discard this token and look at the next.

This is the top-down version of "recovery works even when neither layer is written": under LL, `%sync` is only an extra
anchor for step 2, and the real default means is the FOLLOW set in step 3. Popping and discarding input share a 256-step budget,
so pathological input will not drag the parse to death.

**LL(0) exception**: it has only the ε column and no information about "whom the current token belongs to", so FOLLOW recovery cannot be
judged, and it remains "stop at the first error" (this follows from the definition of top-down).


Constraints:

- The tokens in `%sync` must be already-declared tokens (`%define` / `%token` / `%token regex`);
  undeclared is an error.
- When both layers are written in the LR family, **the `error` production takes precedence**, and `%sync` serves only as the fallback when no
  `error` column action is found; only when both layers are absent does the LR family keep the old behavior (report one error, return a negative error code, no recovery).
- `%sync` does not participate in the analysis layer: it enters neither the terminal set nor the ACTION table, being only a runtime anchor
  set; moreover, in the analysis report (`--report`) and the artifact `RESULT.md` it is listed in a separate "sync tokens" section.
- Under the LL family there is no `error` production (see §3.1), and recovery relies on the FOLLOW set + (optional)
  `%sync`. `ll0` is the sole exception: it cannot even see "the current token", so neither route is available, and at
  runtime it keeps "stop at the first error" (writing `%sync` has no effect and is not an error at generation time).

## 4. Lexical Match Order

Before taking a token, whitespace is skipped according to `%space` (all whitespace by default; it is not skipped when a fixed
literal starting with whitespace / `\b` can match at the current position, see the "Whitespace / Newline as Tokens" section above). The
match order afterwards (longest match first, and equal length by order):

1. Double-quoted strings `"..."` → `STRING`
2. If `%token regex` is declared: regex tokens and fixed literals are matched together by longest match,
   with **a literal winning on equal length** (keywords are not eaten by regexes)
3. The built-in numeric rules (integers / `1.2` / `1e3` / the `u`, `d` suffixes)
4. The remaining fixed literals (longer preferred + keyword boundary check)
5. Identifiers: if `%identifier` is present use its DFA, otherwise use the built-in C identifier rules,
   hitting `VARIABLE`; when a keyword table is present, keywords are looked up first

The meaning of the keyword boundary check: the fixed literal `"b"` does not match when immediately followed by a letter/digit/underscore,
so `"bc"` is not split into `b` + `c`, and must be written as `"b c"` (see `example/or_branch.gram`).

After a fixed literal matches, the lexeme is written into `pToken->pName`, so `%tree` shows
`COLON(:)` rather than `COLON(COLON)`.

## 5. Generation-Time Validation (Excerpt)

- When `%mode` disagrees with the command-line `--mode`, the command line wins.
- Every production must have an action, or the entire grammar must enable `%tree`.
- `%start` must be a defined non-terminal.
- **Forms not supported in actions** (this is the **only** list; other documents just point here):
  the `%var` / `%var_max_count` directives, and in actions `[$n]` (dynamic variable reference),
  `def[...]` / `delete[...]` (variable definition and deletion), `$n.int32` (the built-in scalar type field), and
  `max(...)` (a built-in function) — writing any of them is an error at generation time, suggesting「请在 `%code` 里用 C
  自己实现」. The semantic value has only one kind, `custom` (`%value_type` / `%union`, see §2 and §3);
  for semantics such as a variable table, write it yourself in C in `%code` / `%epilogue`; see `example/calc.gram`.
- `%space` may be declared only once, and its argument may only be whitespace characters (space / `\t` / `\n` / `\v` / `\f` /
  `\r`) or `\b` (standing for all whitespace); any other character is a direct error; `%space ""` is a valid
  "skip not a single whitespace".
- `%token NAME include "csv"`: failing to open the CSV is an error (grammar path + line and column); an empty column 1 / 2,
  a column 2 that is not a double-quoted string, and an unclosed quote are all errors (CSV path + line and column); an alias duplicated across tokens,
  and an alias colliding with an already-declared token name, are also errors. Rows whose first column matches no imported token name are
  **silently ignored**, and the header **is not validated**. See "Maintaining a Literal Alias Table with CSV" above.
  - The semantic value runtime has only `custom`, which is also the default (not writing it equals `custom`); writing `%runtime`
    is a generation-time error suggesting you delete that line.
- `error` may appear only on the right-hand side of a production: appearing on the left-hand side, or being written into an
  argument of `%type` / `%token` / `%token regex` / `%define` / `%start` / `%prec`, is an error; being named the same as a user symbol
  is also an error.
  - The tokens in `%sync` must already be declared; an `error`
    production under `%mode ll1` is an error; `yyerrok` / `yyclearin` under
    `%mode ll1` are errors.
  - An `error` production under the LL family (`ll1` / `llk` / `ll0`) is an error, and likewise `yyerrok` /
    `yyclearin`; the lookahead length is meaningful only for `lrk` / `lalrk` / `slrk` / `llk` / `ll0`
    (writing it with any other mode is a direct error), and a conflict between k in the mode name and `%lookahead` is also an error.
  - **Reduce/reduce conflicts no longer abort generation**: the deterministic LR modes (`lr0` / `slr` / `lalr` /
    `lr1` / `ielr` / `lrk` / `lalrk` / `slrk`) automatically pick one by "the higher precedence wins, otherwise take the production that appears
    earlier in the grammar", and print a conspicuous large warning; for details see
    [`06-analysis.md` §10](../06-analysis.md).
- Conditional compilation: `%if` / `%elif` / `%else` / `%endif` must be paired, and conditional expressions may contain only
  `||` / `&&` / `!` / parentheses / variable names; errors in directives and conditions all carry the **original file** line number.
- At most one `%epilogue`; `main` may not be defined in `%epilogue` (it points you to `%main`);
  `%epilogue` may not appear inside a `%main` block (both extend to the end of the file, so `%epilogue` must be written first);
  more than one `%main` in effect after conditional compilation is an error.
- `%union` at most once, with no duplicate member names; the members referenced by `%type` / `%token <member>` must be
  in `%union`, one symbol cannot be bound to two members, and a top-level comma or a failure to extract a member name is an error.
- Target-language related: `%lang` accepts only `c` / `cpp17` / `python` / `csharp` / `rust` / `go` /
  `gram`; `%class` is valid only for
  `--lang python` (pairing it with C / C++17 is an error), and the name is validated against identifier rules + the Python keyword table;
  `%namespace` is valid only for C++17 (pairing it with C / Python is an error); `%cpp_only` paired with
  `--lang c` / `--lang python` / `--lang csharp` / `--lang rust` / `--lang go` is an error; the Python artifacts
  additionally reject `%value_type` /
  `%union` / `%type <member>` / `%token <member>` (see the `%class` section above); the Rust artifacts
  reject `%destructor` (relying on ownership reclamation).
- When `%union` is written, every symbol referenced in an action (the symbol corresponding to `$n`, the left-hand symbol of `$$`) must be
  bound to a member; `$n` may not appear on the left of an assignment; `%union` / `%type` / `%token <member>`
  are available only under the custom runtime (the only semantic value runtime).
- An out-of-range `$n` is an error: when the right-hand side has only k symbols, writing `$k+1` (or `$0`) reports at generation time
  `产生式 P? 的 $n 越界：这条产生式右部只有 k 个符号`, together with the line number of the action —
  without blocking it, the out-of-range position could not get a member suffix, and you would end up with an incomprehensible C type error.
- The symbols bound by `%type <member>` / `%token <member>` must really exist (a declared token or the left-hand side of some
  rule): a misspelled name reports at generation time `符号 X 没有定义` (with the directive line number), rather than
  silently treating it as "a useless binding".
- A regex may not match the empty string (all six backends error: no language's lexer could advance); the repetition
  limit of `{m,n}` in the shared compiler is 1000. C and C++ `vm` require shared compilation;
  native syntax may have different limits. See [compatibility validation](../21-regex-compatibility.md).
- LL(1) direct left recursion is eliminated automatically, while detected indirect left recursion is an error; the unresolved conflicts of SLR/LALR/IELR
  are handled by the `%expect` rules (IELR first tries to split the conflicting states and errors only if it cannot);

## 6. Lexer Mode (`%mode lex`)

`%mode lex` exports only the lexer: it builds no analysis stack / analysis table and has no parse entry point, so it **allows not a single
production** (other modes must have rules, otherwise they report「没有找到任何文法规则」).

- Lexer-related forms take effect as usual: `%token` (fixed literals / `regex`), `%define`,
  `%identifier`, `%space`, `%union` + `%token <member>`, `%code`, `%main`,
  `%epilogue`, `%copy`, `%if`.
- Parser-related forms are **ignored** (not an error): productions, `%start`, `%tree`,
  `%sync`, `%destructor`, `%error_context` — their corresponding generation-time validation is skipped as well; the entry point
  prints `提示：%mode lex 只导出词法器，已忽略：…`. `%left` / `%right` /
  `%nonassoc` still declare tokens per bison's rules.
- Lexer-related validation is as before: the members of `%token <member>` must be in `%union`, `%identifier`
  and `VARIABLE` cannot be declared at the same time, as well as an undefined symbol on the right-hand side and an out-of-range `$n` (see §5).
- All six target languages are supported, and the artifacts contain only the lexer:
  - C (`--lang c`): `lexer.h` / `lexer.c` / `parser_token.h` / `parser_token.c` /
    `main.c` (+ `parser_user.h` when `%code` is used), **not generating** `parser.h` /
    `parser.c` / `parser_table.c`; the entry points are `Parser_LexInit` + `Parser_NextToken`,
    and errors use `Parser_GetLastError()` / `Parser_GetErrorCount()` / `Parser_GetError(i)`;
  - C++17 (`--lang cpp17`, the default): `lexer.hpp` / `lexer.cpp` / `main.cpp`
    (+ the prefixed `parser_user.h` when `%code` is used), **not generating** `parser.hpp` /
    `parser.cpp`; the entry point is `Engine::tokenize(std::string_view, std::vector<Token> *)`,
    and errors use `engine.errors()` / `lastError()`;
  - Python (`--lang python`): `parser.py` (the lexer + `tokenize`) / `main.py`,
    with no analysis table / analysis stack / `run_text` in the module; the entry point is `Parser().tokenize(text)`,
    returning `(status code, token list)`.
  - C# (`--lang csharp`): `Parser.cs` / `Token.cs` / `Program.cs` /
    `parser.csproj`, with the entry point `new Parser().tokenize(text, tokens)` (`List<Token>`).
  - Rust (`--lang rust`): `Cargo.toml` + `src/parser.rs` / `src/token.rs` /
    `src/main.rs`, with no analysis table / analysis stack in the module; the entry point is
    `Parser::new().tokenize(text)` → `(i32, Vec<Token>)`, and errors use
    `engine.errors()` / `engine.last_error()`.
  All six backends ship a `RESULT.md`; C / C++17 additionally have `Makefile` / `CMakeLists.txt`,
  C# is compiled directly with `dotnet build`,
  Rust with `cargo build` (computation uses only the standard library, and lexical regex uses the official `regex`),
  and Python needs no compilation.
- The CLIs (C's `main.c` / C++17's `main.cpp` / Python's `main.py` / C#'s
  `Program.cs`)
  all "take tokens
  and determine their type": they read standard
  input to EOF and print `<type>  "<lexeme>"  <line>:<column>` for each,
  with the last line being the total; a lexical error prints a one-line message and ends with exit code 1 (`-29` an unrecognized character,
  `-28` an unterminated string).
- The token lexemes handed out by C / C++17 point into an internal reused buffer and are valid only until the next token is taken;
  on the Python / C# side they are ordinary strings (nothing to worry about).

Example: `example/lexer_only.gram` (only lexer declarations, no productions).

## 7. Related Implementation

- Loading and validation: `source/src/grammar_loader.cpp`
- Expansion of `%token NAME include "csv"`: `source/src/token_dict.cpp` (`TokenDict::expand`,
  run after `Preprocessor` and before `GrammarLoader`)
- Data model: `source/include/ParseEngine/grammar_types.hpp`
- Action translation: `source/src/action_translator.cpp`
- Regex compilation: `source/src/regex_dfa.cpp`
