# 11 Target Languages, Output Artifacts, and API (User Manual)

> This is the English edition. The Chinese edition is the default and is the one maintained first — see [中文文档](../README.md)。

This chapter was split out of [`README.md`](../README.md): the selection and artifact forms of the eight target languages (C99 / C++17 / Python / C# / Rust / Go / Java / JS-TS) and of the export profile `--lang gram` that produces "only the grammar and an analysis report"; the several language-related switches `%namespace` / `%cpp_only` / `%class`; the list of generated artifacts; and the API and memory management of the generated code.

Language backend implementations and templates are in [`07-codegen.md`](../07-codegen.md); the API details of each artifact profile are in [`08-generated-code.md`](../08-generated-code.md); artifacts of the lexical mode are in [`13-modes.md`](../13-modes.md). The unified matrix of the modes available per language and of the incremental API is in [12 Modes Supported by Each Language](15-language-mode-support.md).

## Target Languages (C99 / C++17 / Python / C# / Rust / Go / Java / JS-TS)

The same analysis result can produce projects in eight languages; the choice comes from three sources, in descending priority:

1. the command line `--lang {c,cpp17,python,csharp,rust,go,java,js,gram}`;
2. `%lang {c,cpp17,python,csharp,rust,go,java,js,gram}` in the grammar;
3. when neither is present, the default `cpp17`.

In other words `--lang` overrides `%lang` (the same policy as `--mode` overriding `%mode`), without an error. When `%mode` and `--mode` disagree, the command line likewise wins.

The C++17 artifact has one additional switch: `%namespace NAME` / `--namespace NAME` (command line takes priority) determines the namespace name and the file-name prefix; see [Replacing the Namespace and File-Name Prefix](#replacing-the-namespace-and-file-name-prefix-namespace----namespace) below.

The C99 artifact can use `%prefix NAME` / `--prefix NAME` (command line takes priority) to replace the public symbol prefix; the default is `Parser`, and it only affects C99. If omitted, you get the default `Parser` + `parser.*`.

### Forms of Each Artifact Profile

The C++17 artifact is **not** the C artifact stuffed into a `.cpp`; it is generated from a different set of templates (`source/templates/cpp17_*`) and a different backend (`source/src/code_gen_cpp17.cpp`): different files and a different API.

| | C99 artifacts (`--lang c`) | C++17 artifacts (default) |
| --- | --- | --- |
| Files | `parser.h` / `parser.c` / `parser_token.h` / `parser_token.c` / `parser_table.c` / `main.c` / `parser_user.h` | `parser.hpp` / `parser.cpp` / `main.cpp` / `parser_user.h` |
| Parse entry | `Parser_RunText(text, &result)` | `Engine engine; engine.runText(text, &result)` |
| Parse result | `Parser_Run(tokens, count, &result)` | `engine.run(tokens, &result)` |
| Errors | `Parser_GetLastError()` / `Parser_GetError(i)` | `engine.lastError()` / `engine.errors()` |
| Error structure | `ParserError_T` (C struct) | `class Error` (`toString()` directly emits "line N, column M: …") |
| Semantic value | `ParserValue_T` | Same name, but it must be a value-semantics type (POD or RAII) |
| Syntax tree | `ParserTree_T` + `Parser_TreePrint` / `Parser_TreeFree` | `class Tree` (held by `std::unique_ptr`, `tree->print()`) |
| Input | `const char *` (NUL-terminated) | `std::string_view` |
| Memory | Custom hooks: `PARSER_USER_DISCARD_VALUE` to release, `PARSER_MEMSET` / `PARSER_MEMCPY` to replace memset / memcpy; the tree goes through malloc / free | All standard library, **no custom allocator support** |
| Internal structure | Structs + array tables emitted by the generator | `class Engine` (d-pointer) inside a namespace + `inline constexpr` constants |

The third is `--lang python`: the artifacts are only `parser.py` / `main.py`; nothing is compiled and no `Makefile` is generated, the parser is an ordinary Python class (`%class` / `--class` renames the class), and the semantic value is an arbitrary Python object — see the detailed comparison in [Python Output](#python-output-quick-analysis-of-a-grammar---lang-python) below.

The fourth is `--lang csharp`: the artifacts are `Parser.cs` / `Token.cs` / `Program.cs` / `parser.csproj` — see the detailed comparison in [C# Output](#c-output---lang-csharp) below.

The fifth is `--lang rust`: the artifacts are a Cargo project (`Cargo.toml` + `src/parser.rs` / `src/token.rs` / `src/main.rs`); all computation uses the standard library and **lexer regexes use Rust's official `regex` crate** (a single `regex = "1"` in `Cargo.toml`; the first `cargo build` needs network access to fetch it), and you can `cargo build` — see the detailed comparison in [Rust Output](#rust-output---lang-rust) below.

The sixth is `--lang go`: the artifacts are a Go module (`go.mod` + `parser.go` / `token.go` / `main.go`); **only the standard library is used** (lexer regexes use the standard library's `regexp`), and `go build` / `go run .` just work — see the detailed comparison in [Go Output](#go-output---lang-go) below.

The seventh and eighth are the narrow DD/Earley/CYK backends. `--lang java` generates a self-contained `Parser.java`, compiled with `javac -encoding UTF-8 Parser.java`; `--lang js` generates `parser.ts` and `package.json`, with erasable TypeScript type annotations in the code, directly executable by Node.js 22+. Both profiles support `%tree`: DD backtracks by production declaration order; Earley performs predict, scan and complete and keeps the first derivation tree; CYK decides using a CNF span table and then recovers the first derivation tree against the original grammar. Semantic actions, `%value_type` / `%union`, `%sync`, `%commit`, `%copy` and user code sections are currently unsupported; other modes are explicitly rejected at generation time, and the full boundary is in the [support matrix](../15-language-mode-support.md).

The C / C++17 / Python profiles are **semantically** consistent: the same grammar and the same input produce byte-identical output (including error message text) (under Windows, C# writes platform CRLF line endings, normalized before comparison): among the grammar-like grammars in `example/`, 36 compile and run with identical results under both the C99 and C++17 targets (`namespace_demo.gram` produces C++17 only, `prefix_demo.gram` produces C99 only); those that avoid C-specific semantic-value syntax (such as `example/python_demo.gram`) also behave the same under Python; and for the lexer-mode examples (`example/lexer_only.gram`) all six profiles can produce a lexer (C#'s Token type names and line/column numbers match C; Rust / Go line/column numbers match C, with lexer regexes going through the official `regex` and the standard-library `regexp` respectively).

In `main.cpp`, a line `using namespace Parser;` is inserted after the last `#include`, so a demo program can also use the C names `Parser_RunText(...)` — the C++17 artifact carries a thin compatibility layer inside the namespace (the names `PARSER_OK` / `ParserError_T` / `Parser_RunText` / `Parser_GetLastError` are all present), wrapping a process-wide shared `Engine` (**not thread-safe**). New code is advised to use `Engine` directly.

User files are untouched: the `parser_user.h` generated by `%code ... %endcode`, and the `.h` / `.c` copied in by `%copy`, keep their original file names and original contents in both the C99 and C++17 artifacts; the `#include` in the C++17 artifact also points to the name `parser_user.h` (with `%namespace` it is `<prefix>_parser_user.h`). The `.c` files copied in by `%copy` are compiled separately by the artifact's `Makefile` / `CMakeLists.txt` with C rules in a C++17 project — C and C++ mixed together. The Python artifact has no `parser_user.h` (the body of `%code ... %endcode` is inserted into `parser.py`), and files copied in by `%copy` are still copied into the output directory verbatim.

### Replacing the Namespace and File-Name Prefix: `%namespace` / `--namespace`

The `Parser` and `parser.*` above are the defaults. Writing `%namespace myapp` (or the command line `--namespace myapp`, command line takes priority) replaces both the namespace name and the file-name prefix of the C++17 artifact:

| Default (no `%namespace`) | `%namespace myapp` |
| --- | --- |
| `namespace Parser { ... }` | `namespace myapp { ... }` |
| `Parser::Engine::runText` / `Parser::Token` | `myapp::Engine::runText` / `myapp::Token` |
| `parser.hpp` / `parser.cpp` | `myapp_parser.hpp` / `myapp_parser.cpp` |
| `main.cpp` / `parser_user.h` | `myapp_main.cpp` / `myapp_parser_user.h` |
| `using namespace Parser;` (main.cpp) | `using namespace myapp;` |

- The name must match `[A-Za-z_][A-Za-z0-9_]*`, must not be a C++ keyword, and does not support multiple levels (`my::lang` is an error);
- `Makefile` / `CMakeLists.txt` / `RESULT.md` are not prefixed (they are build scripts and documentation), files copied in by `%copy` keep their original names, and the two target names `libparser.a` and `parser_demo` stay unchanged;
- The header guard macro is generated from the artifact name (`MYAPP_PARSER_HPP`), so the headers of both artifacts can be included at the same time;
- **The C99 artifact cannot use it**: `%namespace` / `--namespace` together with `--lang c` is an immediate error (C artifacts have no namespace and their file names carry no prefix; silently ignoring it would only make people believe it took effect);
- An empty name (`--namespace ""`) or omission degrades to the default table above.

Coexistence of multiple instances (linking two parsers at the same time under C++) is the purpose of this feature:

```sh
ParseEngine --grammar-file a.gram --out-dir out-a --namespace alpha
ParseEngine --grammar-file b.gram --out-dir out-b --namespace beta
```

```cpp
#include "alpha_parser.hpp"     // namespace alpha
#include "beta_parser.hpp"      // namespace beta

alpha::Engine engineA;
engineA.runText("1+2*3", &a);   // 两个解析器各用各的，链接期不再撞符号
beta::Engine engineB;
engineB.runText("2+3*4", &b);
```

Two things to note: user code in `%code ... %endcode` and `%epilogue` **still lives in the global scope** (that is the user's own code, and the generator does not rewrite a single character of it); with multiple instances, write it as `static` / `inline` or add your own namespace. The `Parser_RunText` equivalent entry and the `PARSER_*` constants in the compatibility layer are in the same namespace, so including both artifacts at once does not conflict (each carries its own namespace).

Example [`example/namespace_demo.gram`](../../example/namespace_demo.gram): the `alpha` / `beta` artifacts can be included and linked at the same time, each producing its own results (header guards are generated from the prefix, otherwise the second one would be blocked by a guard macro of the same name).

### Grammars That Can Only Produce C++17: `%cpp_only`

When a grammar contains `%cpp_only`, writing `--lang c` on the command line (or `%lang c` in the grammar) is an immediate error, telling you that this grammar can only produce C++17 artifacts. It suits grammars such as "the actions call `Engine` / use `std::string_view`" that are bound never to compile as C: an early error beats making people guess the reason from C compiler errors.

### `%commit` / `%trivia`: Supported by All Six Profiles

`%commit` (collecting text like comments that is "recognized but not part of the syntax layer" into a container) is a **cross-language** capability; `%trivia` (the explicit form that is discarded on match, exactly equivalent to `%commit skip`) goes through the same lexer table; **all six profiles (C99 / C++17 / Python / C# / Rust / Go) support them**; match order, container contents (line number, type, original text) and output are byte-identical (C# / Rust write platform CRLF line endings on Windows):

| Target language | Container element | Parser exposure | Notes |
| --- | --- | --- | --- |
| C99 | `ParserCommit { int line; const char *type; char *text; }` | `Parser_GetCommits()` / `Parser_FreeCommits()` | `text` is heap memory, `type` points to a static string (do not free); who allocates and who frees is described in [`08-generated-code.md`](../08-generated-code.md) |
| C++17 | `ParserCommit { int line; std::string type; std::string text; }` | `Engine::commits()` | Memory managed by the standard library |
| Python | `ParserCommit` (namedtuple: `line` / `type` / `text`) | `engine.commits` | No compilation, usable directly as a library |
| C# | `Parser.Commit { int Line; string Type; string Text; }` | `Parser.Commits` | `Text` is a `string`, UTF-16 |
| Rust | `ParserCommit { line: i32, kind: String, text: String }` | `engine.commits()` | `kind` is the type name |
| Go | `ParserCommit { Line int32; Type string; Text string }` | `engine.Commits()` | `Type` is the type name; values are copied by value |

The location-info API is also present in all six profiles (C99 `Parser_CurrentLocation()`, C++17 `Engine::currentLocation()`, Python `engine.current_location()`, C# `CurrentLocation`, Rust `engine.current_location()`, Go `engine.CurrentLocation()`), and `offset` is always counted in UTF-8 bytes.

The same pattern cannot be both a `%token` and a `%commit` / `%trivia` (`%trivia` is equivalent to `%commit skip`, and the two spellings generate byte-identical lexer tables); matching shares a single longest-match order with `%token` / built-in comment rules (equal lengths resolved by declaration order). Usage and complete examples are in [`05-grammar.md`](../05-grammar.md) and [`12-grammar-usage.md`](../12-grammar-usage.md); the API details of what is generated are in [`08-generated-code.md`](../08-generated-code.md).

Token and commit types also support an optional display alias:

```gram
%token STRING "..." alias "字符串"
%commit block "/\\*...\\*/" alias "块注释"
```

The same rule applies to the literal-less `%token A B C alias "显示名"`, `%identifier "正则" alias "标识符"` and `%trivia "模式" alias "空白注释"`. The alias of `%identifier` corresponds to the built-in `VARIABLE`; the alias of `%trivia` only appears in the type table, because trivia itself never enters the commit container.

All six generators keep the original name and the alias. C99/C++17 use a two-column static name table, other languages use a name-to-alias dictionary; the alias only affects diagnostic display such as `token_name` (in the form `STRING(字符串)`), and does not change Token values, grammar symbols or `ParserCommit.type/kind`. When no alias is written, no parentheses are appended.

### Python Output: Quick Analysis of a Grammar (`--lang python`)

`--lang python` / `%lang python` produces a different artifact: only two files, **no compilation needed**, suited to quick verification of the "let me first see whether this grammar can even run" kind:

| | Python artifacts (`--lang python`) |
| --- | --- |
| Files | `parser.py` (lexer + analysis driver + public API) / `main.py` (demo entry) / `RESULT.md` |
| Class name | `%class NAME` / `--class NAME` (command line takes priority; default `Parser`) |
| Run | `python3 main.py` (the demo program reads one line from stdin), or `import parser` to use it as a library |
| Parse entry | `engine.run_text(text)` / `engine.run(tokens)` / `engine.parse_tree(text)` |
| Result | `engine.result` (an arbitrary Python object); read the failure reason from `engine.last_error` / `engine.errors` |
| Syntax tree | `class Tree` (`name` + `text` + `children`; `tree.print()` emits indented text) |
| User code | `%code ... %endcode` is inserted after the imports in `parser.py` (indentation normalized automatically); `%epilogue` is appended to the end of `parser.py`; `%main` replaces the demo body of `main.py` |
| Memory | Everything is managed by Python itself; no allocator / release hooks |

```sh
./build/bin/ParseEngine --grammar-file example/python_demo.gram --out-dir out --lang python
printf '1+2*3' | python3 out/main.py
```

#### Interfacing with AI / Scripts: Saving Context

The Python artifact has one more use — letting AI (terminal-equipped coding agents such as Codex or Claude Code) **run commands itself to get results**, instead of reading the grammar, artifacts or logs into context: `printf '...' | python3
main.py` returns only two or three lines; for a structured result use `python3 -c "import parser; e =
parser.Parser(); e.run_text(open('in.txt').read()); print(e.result, e.errors)"`,
which returns one line. Thus the "edit grammar → regenerate → look at the result" loop can spin many times within a very small context, without the AI having to read source code and long logs continuously, and without pasting the whole input / output into the conversation.

The artifact uses only the standard library and needs no compilation, so anywhere with `python3` (CI, containers, an AI sandbox) can run it directly — verifying a grammar does not require a working C toolchain first.

In Python the semantic value is an **arbitrary object**, and actions are written as plain Python expressions:

```text
expr := expr PLUS term { $$ = $1 + $3; }     # $1 -> values[0]，$$ -> result.value
      | term           { $$ = $1; };
```

The semantic value the lexer hands out: a string Token is a `str` with quotes removed and escapes resolved, a numeric Token is `int` / `float`, identifiers and fixed literals are the lexeme text (for example `+`), and the EOF and `error` pseudo-Tokens are `None`. When `result` is not `None`, the demo program prints one extra line `解析结果: <repr>`.

Helper functions / constants inside `%code ... %endcode` are inserted after the `import` in `parser.py` and before the generated constants, and can be called directly from actions (in `example/python_demo.gram` the actions call `py_value` defined by `%code ... %endcode`); the body is normalized according to Python's indentation rules — the **common indentation** of all non-blank lines is stripped and leading tabs are turned into 4 spaces, so writing it indented to match the grammar layout will not produce an `IndentationError`. `%epilogue` (appended to the end of `parser.py`) and `%main` (replacing the demo body of `main.py`) undergo the same normalization.

Because the semantic value is a Python object, the Python artifact **does not support** these C-specific spellings, and writing them is an immediate error (no silent ignoring):

- `%value_type`, `%union`, `%type <member>`, `%token <member>`;
- `%namespace` / `--namespace` (Python has no namespaces; rename the class with `%class` / `--class`).

All other directives work: `%mode` (ll1 / lr0 / slr / lalr / lr1 / ielr / lrk / lalrk), `%lookahead`, `%start`, `%define` / `%token` / `%token regex` / `%identifier`, `%tree`, `%sync`, `error` productions and `yyerrok` / `yyclearin`, `%max_*`, `%expect`, `%left` / `%right` / `%nonassoc` / `%prec`, `%teach`, `%mermaid`, `%copy`, `%if`, `%main` / `%epilogue`, `%code ... %endcode`. The same grammar (when it does not contain the C-specific spellings above) produces byte-identical output across the four artifacts for the same input, including error messages, error codes, line/column numbers and the tree text of `%tree` (C# writes platform CRLF line endings on Windows, normalized before comparison).

`%cpp_only` declares a grammar that "can only produce C++17", and pairing it with `--lang python` (or `--lang c`) is likewise an error.

### C# Output (`--lang csharp`)

C# is an artifact profile on a par with C99 / C++17 / Python / Rust / Go, with its own templates (`source/templates/csharp_*`) and backend (`source/src/code_gen_csharp.cpp`). **All generation modes are covered**: the LL family (ll0 / ll1 / llk) goes through one LL runtime, the LR family (lr0 / slr / lalr / lr1 / ielr / lrk / lalrk / slrk) goes through one LR runtime; `dd` goes through backtracking recursive descent, `glr` uses a `Dictionary`-indexed graph-structured stack, `earley` / `cyk` first decide with a chart and then rebuild the result with GLR; `lex` produces only the lexer.

```sh
# C#：dotnet build + dotnet run（目标框架 net8.0，只用标准库）
./build/bin/ParseEngine example/json.gram --out-dir /tmp/cs --lang csharp
cd /tmp/cs && dotnet build && printf '{"a": [1, 2]}' | dotnet run
```

| | C# artifacts |
| --- | --- |
| Files | `Parser.cs` / `Token.cs` / `Program.cs` / `parser.csproj` / `RESULT.md` |
| Entry | `new Parser().runText("...")` → `STATUS_*` |
| Errors | `errors()` / `lastError()` (`Parser.ParserError`) |
| Syntax tree | `Parser.Tree` (`print()`) |
| Ambiguity results | `runGlr(text)` / `runGlrTree(text)`; `glrResultCount()`, `glrResult(i)`, `glrTree(i)`; `setAmbiguityResolver(...)` can choose which results to keep |
| Semantic value | `%value_type` is filled in verbatim; `%union` → the top-level class `ParserValue`; neither → `object` |
| Error location | around each of the five "code sections coming from the `.gram`" a comment `// 在 <文法文件> 的第 <N> 行` is inserted (line numbers exactly match C's `#line`) |

Three directives that take effect only for csharp: `%package` (C#'s `namespace NAME;`), `%import` (C#'s `using NAME;`, repeatable), `%class_body` … `%endbody` (inserted verbatim inside the parser class, for helper methods / fields / nested classes). `%code ... %endcode`, `%epilogue`, `%main` work as usual, except the body must be written in **the target language's own code** (the generator does not rewrite a single character).

### Rust Output (`--lang rust`)

Rust has its own templates (`source/templates/rust_*`) and backend (`source/src/code_gen_rust.cpp`, `CodeGenRust`). **All generation modes are covered**: the LL family (ll0 / ll1 / llk) goes through one LL runtime, the LR family (lr0 / slr / lalr / lr1 / ielr / lrk / lalrk / slrk) goes through one LR runtime; `dd` goes through backtracking recursive descent, `glr` goes through GSS, `earley` / `cyk` decide with a chart and then rebuild the result with GLR; `lex` produces only the lexer.

```sh
# Rust：cargo build（标准库 + 官方的 regex crate；依赖进过本地缓存后可以加 --offline）
./build/bin/ParseEngine example/json.gram --out-dir /tmp/rs --lang rust
cd /tmp/rs && cargo build && printf '1+2*3' | ./target/debug/parser_demo
```

| | Rust artifacts |
| --- | --- |
| Files | `Cargo.toml` / `src/parser.rs` / `src/token.rs` / `src/main.rs` / `RESULT.md` |
| crate name | `package.name` in `Cargo.toml`, default `parser_demo` (`%package` changes it) |
| Parse entry | `Parser::new()` + `engine.run_text(&text)` → `STATUS_OK` |
| Errors | `engine.errors()` / `engine.last_error()` (`ParserError`); `status_name(status)` yields text |
| Syntax tree | `engine.parse_tree(&text)` → `Option<Tree>` (`Tree::print()` / `child(i)` / `child_count()`) |
| Location | `ParserLocation` / `engine.current_location()` (line / column start at 1, `offset` is a UTF-8 byte offset) |
| Semantic value | default `i64` (numeric Tokens give the integer, other Tokens give 0); `%union` → a `ParserValue` with `#[derive(Clone, Copy, Default, Debug)]` |
| Error location | same as the other profiles: around code sections coming from the `.gram`, a comment `// 在 <文法文件> 的第 <N> 行` is inserted |
| Lexer regex | Rust's **official `regex` crate** (one `regex = "1"` in `Cargo.toml`; the artifact has no DFA table, patterns are handed to it verbatim and each compiled once inside a `OnceLock`); `%identifier` / `%commit` / `%trivia` also go through `regex` |
| Memory | semantic values are copied by value, **no `%destructor` support** (reclaimed by ownership; writing it is an immediate error at generation time) |

**The semantic value must satisfy `Copy + Default + Debug`** (it is copied by value on the analysis stack): scalars (`i64` / `f64` / `bool` / `u8` …) satisfy this naturally; the `ParserValue` generated by `%union` brings the `derive` with it, as long as the member types satisfy these three too. This is the biggest difference between the Rust profile and the other profiles, and `RESULT.md` writes a dedicated paragraph about it.

Three directives that take effect for rust: `%package` (crate name), `%import` (`use NAME;`, repeatable), `%class_body` … `%endbody` (inserted verbatim into the parser implementation, for helper methods / fields / nested types). `%code ... %endcode` / `%epilogue` / `%main` work as usual, except the body must be written in **Rust's own code** (the generator does not rewrite a single character).

To use it only as a parsing library: drop `src/main.rs` and add `src/parser.rs` / `src/token.rs` as modules to your own crate (the `%code ... %endcode` items in the artifact are all `pub`, so `use parser::*;` picks them up).

### Go Output (`--lang go`)

Go has its own templates (`source/templates/go_*`) and backend (`source/src/code_gen_go.cpp`, `CodeGenGo`). **All 16 modes are supported**: the LL family (ll0 / ll1 / llk) goes through one LL runtime, the LR family (lr0 / slr / lalr / lr1 / ielr / lrk / lalrk / slrk) goes through one LR runtime, `dd` goes through backtracking recursive descent, `glr` goes through GSS, `earley` / `cyk` first decide with a chart and then rebuild all result trees against the original grammar via GSS and execute semantic actions, `lex` produces only the lexer; artifacts always go through the `library/template` template engine (the same approach as the Rust profile).

```sh
# Go：go build（只用标准库，没有第三方依赖）
./build/bin/ParseEngine example/json.gram --out-dir /tmp/go --lang go
cd /tmp/go && go build && printf '1+2*3' | go run .
```

| | Go artifacts |
| --- | --- |
| Files | `go.mod` / `parser.go` / `token.go` / `main.go` / `RESULT.md` (all three `.go` files are `package main`) |
| module path | `module` in `go.mod`, default `parserdemo` (`%package` changes it, `::` normalized to `/`) |
| Parse entry | `NewParser()` + `engine.RunText(text)` → `StatusOK` |
| Errors | `engine.Errors()` / `engine.LastError()` (`ParserError` implements `error`); `StatusName(status)` yields text |
| Syntax tree | `engine.ParseTree(text)` → `*Tree` (`tree.Print()` / `tree.Child(i)` / `tree.ChildCount()`) |
| Location | `ParserLocation` / `engine.CurrentLocation()` (line / column start at 1, `Offset` is a UTF-8 byte offset) |
| Semantic value | default `int64` (numeric Tokens give the integer, other Tokens give the zero value); `%union` → a `ParserValue` struct with member fields (members are written as in C declarations, e.g. `int64 ival;`, and the generator assembles them into Go field syntax) |
| Error location | same as the other profiles: around code sections coming from the `.gram`, a comment `// 在 <文法文件> 的第 <N> 行` is inserted |
| Lexer regex | Go's **standard-library `regexp`** (RE2; the artifact has no DFA table, patterns are handed to it verbatim and each compiled once by `regexp.MustCompile` at package-variable initialization); `%identifier` / `%commit` / `%trivia` also go through `regexp` |
| Memory | semantic values are copied by value, **no `%destructor` support** (reclaimed by the GC; writing it is an immediate error at generation time) |

The member syntax of `%union` matches C / C# (`int64 ival;`), whereas the Rust profile is the `ival: i32` style; semantic values are copied by value on the analysis stack, and scalars / structs / slices all work (for reference types like slices, what is copied is the reference).

Three directives that take effect for go: `%package` (module path), `%import` (goes into the `import ( … )` block, repeatable, with `::` as the path separator and packages duplicating the standard library skipped), `%class_body` … `%endbody` (appended verbatim after the parser code, written as a set of `func (p *Parser) …` methods — Go has no class layer). `%code ... %endcode` / `%epilogue` / `%main` work as usual, except the body must be written in **Go's own code**: the body of `%code ... %endcode` is inserted after the import block (**do not write `import` inside the body**), and the body of `%main` must supply its own `import` and `func main()` (the shell only provides the file-header comment and `package main`).

To use it only as a parsing library: drop `main.go` and add `parser.go` / `token.go` to your own package (they are all `package main`; just change them to your package name).

### Grammar Export (`--lang gram`)

The last profile is not a programming language but the export profile that gives **only the grammar and an analysis report, no code**: the output directory contains only the input `.gram` (exported byte for byte), its analysis report `<grammar name>.analysis.md` (conflict resolution / unresolved conflicts + the full analysis report) and `RESULT.md` (generation info + artifact list + how to use it). There is no parser, no lexer, and no build script.

```sh
ParseEngine example/pl0.gram --out-dir out --lang gram
ls out                          # pl0.gram  pl0.analysis.md  RESULT.md
head -n 30 out/pl0.analysis.md  # 冲突消解 / 未消解冲突 / 分析报告
```

| | Grammar export profile (`--lang gram`) |
| --- | --- |
| Files | `<original file name>.gram` (`grammar.gram` when no file is given) / `<grammar name>.analysis.md` / `RESULT.md` / files from `%copy` |
| Modes | All 16 modes can be exported; `glr` / `earley` / `cyk` export only the grammar and the corresponding analysis report, without generating a runtime. `--mode lex` reports as a lexical section + Token table; see the [support matrix](../15-language-mode-support.md) |
| Plotting | `gram` does not generate a Python runtime or `--svg` / `--dot` demo programs; for plotting of Python artifacts see [`10`](../13-modes.md) |
| Report | the analysis report is its own file, paired with the `.gram` (`pl0.gram` ↔ `pl0.analysis.md`), and its body is the same as what `--report FILE` writes |
| Inapplicable directives | `%namespace` / `%class` / `%package` / `%import` / `%class_body` / `%destructor` / `%cpp_only` are all reported with one line and ignored |

Purpose: to archive / commit / hand over "this grammar + the analysis result at the time" together; when the output directory contains only the generated source, confirming later "which grammar and which analysis this code came from" would require digging through the command line. Details are in [`07-codegen.md`](../07-codegen.md) §14 and [`08-generated-code.md`](../08-generated-code.md) §13.

### When to Use Which

- To link with a C project, or when the target platform only has a C compiler: `--lang c`.
- To use class semantics directly and mix with a C++ project: the default `cpp17` will do; the generated C++17 code uses only the standard library and C headers such as `<stdint.h>`, with no third-party dependencies.
- To quickly verify a grammar only (without installing a compiler or reading C types): `--lang python`, and `python3 main.py` runs it directly.
- To let AI / scripts run commands themselves to get parse results (saving context, without reading artifacts in): also `--lang python`; see [Interfacing with AI / Scripts](#interfacing-with-ai--scripts-saving-context) above.
- To integrate with a .NET project: `--lang csharp`; the artifact uses `dotnet build` + `dotnet run`.
- To integrate with a Rust project: `--lang rust`; the artifact is a Cargo project (the only dependency is the official `regex`), and `cargo build` will do (standard library only, the semantic value must be `Copy + Default + Debug`, no `%destructor`).
- To integrate with a Go project: `--lang go`; the artifact is a Go module, and `go build` / `go run .` will do (standard library only, semantic values copied by value, no `%destructor`).
- To keep only "a grammar + an analysis report" and no code at all (archiving, handover, writing a lab report): `--lang gram`; the artifact is the original `.gram` file and `RESULT.md`; switching `--mode` and exporting again gives another set of analyses (for example `--mode lalr --teach` to inspect conflicts).

## Generated Artifacts

The generator emits only split-file projects, written by default to `--out-dir` (default `out/`). The table below is the **C99** artifact (`--lang c`); the default **C++17** artifact is generated by another backend as `parser.hpp` / `parser.cpp` / `main.cpp` (plus a `parser_user.h` when `%code ... %endcode` is written), with the API and `Makefile` / `CMakeLists.txt` being the C++17 versions (see [Target Languages](#target-languages-c99--c17--python--c--rust--go--java--js-ts)); the **Python** artifact (`--lang python`) has only `parser.py` / `main.py` / `RESULT.md` (plus files from `%mermaid` / `%copy`), with no `Makefile` / `CMakeLists.txt`; the **C#** artifact (`--lang csharp`) is `Parser.cs` / `Token.cs` / `Program.cs` / `parser.csproj` / `RESULT.md` (see [C# Output](#c-output---lang-csharp)); the **Rust** artifact (`--lang rust`) is `Cargo.toml` + `src/parser.rs` / `src/token.rs` / `src/main.rs` / `RESULT.md` (see [Rust Output](#rust-output---lang-rust)). Files copied in by `%copy` stay as they are for every target:

| File | Content |
| --- | --- |
| `parser.h` | Public API: `Parser_Run` / `Parser_RunText` / tree / errors |
| `parser.c` | lexer, `ParserCtx_T`, semantic actions, LL/LR main loops, tree API |
| `parser_token.h` | Token enum, `ParserToken_T`, Token name interfaces, regex DFA and identifier interface declarations |
| `parser_token.c` | Token name table, text helpers, DFA tables and functions for `%token regex`/`%identifier` |
| `parser_user.h` | user code from `%code ... %endcode`, plus the `ParserValue_T` definition generated from `%value_type` / `%union`; not generated when none of the three is present |
| `parser_table.c` | prediction table / ACTION / GOTO / production table / tree metadata table |
| `main.c` | demo `main`: reads one line from stdin, then calls `Parser_RunText` |
| `Makefile` | build script: `make` produces `libparser.a` + `parser_demo`; also has `lib` / `demo` / `run` / `clean` |
| `CMakeLists.txt` | CMake build script (equivalent to `Makefile`): `cmake -S . -B build && cmake --build build` |
| `RESULT.md` | generation info, artifact list, build-and-run instructions, conflict resolution and unresolved conflicts (add `--full-report` for the full analysis report, see [`08-generated-code.md`](08-generated-code.md#17-resultmd-brief-by-default)) |
| `mermaid/*` | state-machine diagrams: `index.html` / `grammar.html` / `lexer.html` and `.mmd` sources; **generated only when `%mermaid` / `--mermaid` is on** (independent of teaching mode), see [State-Machine Diagrams](../13-modes.md#状态机图示mermaid) |

### Templates (`source/templates/`)

All generated content is split into templates for easy direct editing:

| Template | Role |
| --- | --- |
| `c99_parser_h_custom.h`, `c99_parser_c_custom.c`, `c99_custom_tree_api.h`, `c99_parser_prologue.c` | skeleton of parser.c / parser.h, the public API header and shared macro definitions (capacity macros + `PARSER_MEMSET` / `PARSER_MEMCPY` fallbacks) |
| `cpp17_parser.hpp`, `cpp17_parser.cpp`, `cpp17_main.cpp`, `cpp17_compat.hpp`, `cpp17_core.cpp` | skeleton of the C++17 artifact: public interface (`Engine` / `Tree` / `Error`), runtime assembly, the `main.cpp` shell and the C-name compatibility layer |
| `cpp17_tables.cpp`, `cpp17_helpers.cpp`, `cpp17_tree.cpp`, `cpp17_ll.cpp`, `cpp17_lr_*.cpp`, `cpp17_lexer*` | analysis tables, lexer, per-mode drivers and tree implementation of the C++17 artifact |
| `python_parser.py`, `python_engine.py`, `python_main.py` | skeleton of the Python artifact: module header / constant tables / `Token` / `Tree` / status codes, the parser class and `run_text` / `run` / `parse_tree`, the `main.py` shell |
| `python_lr*.py`, `python_ll.py`, `python_lexer*.py` | action dispatch, analysis tables, LR / LL drivers and lexer of the Python artifact |
| `python_demo_main.py` | demo `main` body for Python |
| `csharp_*` | C# artifact: parser / Token / demo entry / `parser.csproj` |
| `rust_*` | Rust artifact: `Cargo.toml` / parser / Token / demo entry (`src/*.rs`) |
| `go_*` | Go artifact: `go.mod` / parser / Token / demo entry (`parser.go` / `token.go` / `main.go`) |
| `c99_parser_token_h_custom.h`, `c99_parser_token.c`, `c99_token_impl_custom.c` | Token module |
| `c99_lexer.c`, `c99_lexer_*_block.c` | lexer (one section each for fixed literals / strings / numbers / regexes / identifiers) |
| `c99_custom_ll_*.c`, `c99_custom_lr_*.c`, `c99_lr_tree_header.c`, `c99_custom_tree_*.c` | analysis runtimes per mode and the syntax tree |
| `c99_parser_table.c` | analysis table skeleton |
| `c99_main_custom.c`, `c99_demo_main_custom.c` | demo program for the C artifact |
| `RESULT.md`, `result_run_section_{python,c_cpp17}.md`, `result_runtime_section_{custom,cpp17,python}.md`, `result_tree_section_{custom,cpp17,python}.md` | generation-result description and its individual sections; `RESULT.md` uses `{% include %}` to pull in the latter three groups per backend |
| `Makefile_custom`, `Makefile_custom_cpp17` | `Makefile` of the C99 / C++17 artifacts |
| `CMakeLists_custom.txt`, `CMakeLists_custom_cpp17.txt` | `CMakeLists.txt` of the C99 / C++17 artifacts |
| `c99_default_grammar.gram` | the default grammar used when there is no `--grammar-file` |

The C99 file names (`c99_*`) and artifact names **may collide** (`lexer.c` / `parser_token.c` / `parser_table.c` are both template names and artifact file names); when renaming, do not rename the artifact names along with them.

Templates **always go through `.tmpl`**: the format of the `library/template` engine (the `{{ variable }}` / `{% if %}` / `{% for %}` / `{% include %}` inside templates are rendered by the generator, and the generator only supplies "name → value" pairs). Editing a template only affects layout and the value names, not the generation logic. At present the **code templates of all six backends (`c99_*` / `cpp17_*` / `python_*` / `csharp_*` / `rust_*` / `go_*`) are all `.tmpl`** (the build scripts and the `RESULT.md` ones too), and `.template` is down to zero; `source/templates/` holds **384 files in total**: `c99_*` 50 / `cpp17_*` 68 / `python_*` 51 / `csharp_*` 49 / `rust_*` 53 / `go_*` 55 / `java_*` 2 / `js_*` 3, with the remaining 53 being build scripts (`Makefile_*` / `CMakeLists_*`), shared `code_gen_*` fragments, `RESULT.md` and `result_*.md` fragments, and `mermaid_*` (graph sources / pages / text of the state-machine diagrams).

## API of the Generated Code

```c
/* 解析入口 */
int32_t Parser_RunText(const char *pText, ParserValue_T *pResult);
int32_t Parser_Run(const ParserToken_T *pTokens, uint32_t count,
                   ParserValue_T *pResult);
const ParserError_T *Parser_GetLastError(void);

/* %tree 时生成 */
ParserTree_T *Parser_ParseTreeText(const char *pText);
uint32_t Parser_TreeChildCount(const ParserTree_T *pNode);
const ParserTree_T *Parser_TreeGetChild(const ParserTree_T *pNode,
                                        uint32_t index);
const char *Parser_TreeGetName(const ParserTree_T *pNode);
const char *Parser_TreeGetText(const ParserTree_T *pNode);
void Parser_TreePrint(const ParserTree_T *pRoot);
void Parser_TreeFree(ParserTree_T *pRoot);

```

The same thing in the C++17 artifact (the default target language) is a **completely different interface** — the Qt-style `Engine`:

```cpp
namespace Parser {

class Engine
{
public:
    Status runText(std::string_view text, ParserValue_T *result);
    Status run(const std::vector<Token> &tokens, ParserValue_T *result);
    const std::vector<Error> &errors() const;
    const Error *lastError() const;

    std::unique_ptr<Tree> parseTree(std::string_view text);   // %tree 时
};

const char *statusName(Status status);
const char *tokenName(TokenType type);
std::string tokenText(const Token &token);
} // namespace Parser
```

- `Status` is an `enum class` (`Ok` / `ErrStack` / `ErrSyntax` / `ErrLexer` …), with values corresponding one-to-one to the `PARSER_*` of the C artifact;
- `Error` is a class: `code()` / `line()` / `column()` / `message()` / `toString()`;
- `Tree` is a class: `name()` / `text()` / `childCount()` / `child(i)` / `print()`, owned via `std::unique_ptr`, with copying disabled;
- Compatibility layer: the C names `Parser_RunText` / `Parser_GetLastError` / `ParserError_T` / `PARSER_OK` also exist in the same namespace (for `%main` / `%epilogue`; underneath is a shared `Engine`, not thread-safe);
- `ParserValue_T` is determined by your `%value_type` / `%union` / `%code ... %endcode`, with the same name on both sides.

The complete API of the C# profile is in [`08-generated-code.md`](../08-generated-code.md) §12 (this section lists only the C99 and C++17 ones; the "C# Output" section above is a quick-reference table).

Error codes: `PARSER_OK 0`, `PARSER_ERR_STACK -20`, `PARSER_ERR_SYNTAX -21`, `PARSER_ERR_LEXER -22`, `PARSER_ERR_DUPLICATE -23`, `PARSER_ERR_UNDEFINED -24`, `PARSER_ERR_SYSTEM -25`. On a parse failure, `Parser_GetLastError()` gives the error code, line/column and description.

`Parser_ParseTreeText` for `%tree` only builds a CST and does not execute semantic actions, so it can coexist with semantic actions: `Parser_RunText` evaluates by actions, `Parser_ParseTreeText` produces the syntax tree.

## Memory Management

Semantic values are yours to manage: `ParserValue_T` is copied by value, and the runtime only moves it around on the analysis stack, without new or free. The memory pointed to by pointers inside it (strings, AST nodes, …) is allocated and released by your `%code ... %endcode`, usually paired with a custom cleanup function in the style of `Parser_TreeFree`.

Only the `%tree` syntax tree is allocated by the runtime: the nodes and child arrays of `ParserTree_T` go through `malloc` / `realloc` / `free`, and `Parser_TreeFree(pTree)` frees the whole tree at once; the ownership rules of `Parser_ParseTreeText` are written in the [API of the Generated Code](#api-of-the-generated-code) section.

To replace the tree allocation with a static pool / RTOS heap / an allocator with statistics, just change the `malloc` / `realloc` / `free` in `source/templates/c99_custom_tree_helpers.c.tmpl`, `c99_custom_lr_tree_body.c.tmpl` (all generated content lives in the templates).

**Zeroing and copying** in the generated code also leave hooks: every call of `memset` / `memcpy` goes through `PARSER_MEMSET(pDst, value, size)` / `PARSER_MEMCPY(pDst, pSrc, size)`, and the two macros have `#ifndef` fallbacks in `parser.h` to the `<string.h>` functions of the same name; `#define`-ing macros of the same name first in `%code ... %endcode` (or `-D` at compile time) switches to your own implementation — no-libc environments, versions with statistics / bounds checks, and RTOS-specific moves all work. The C++17 artifact has no such two macros: memory is always managed by the standard library, custom allocators are not supported, and memset / memcpy are not needed.

The C# profile has neither allocator hooks nor `memset` / `memcpy` hooks: semantic values and the syntax tree are reclaimed by the GC, and `%destructor` is an immediate error under this profile.

`parser_user.h` is included by multiple `.c` files; write helper functions that sit directly in the header as `static inline`, and it is advisable to put global state such as tables inside a function's `static` to avoid keeping one copy per translation unit.
