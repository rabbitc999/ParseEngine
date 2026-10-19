> This is the English edition. The Chinese edition is the default and is the one maintained first — see [中文文档](../README.md)。

# 05 Generated Output Artifacts and Generated Code API

Mode and language combinations are listed together in [12 Support Matrix](../15-language-mode-support.md). The
incremental API is provided only for C++17 / Rust: the LL/DD same-text cache, the deterministic LR prefix checkpoint,
and the GLR-family experimental cache are three different capabilities. Earley/CYK share the GLR experimental entry
point, which does not mean an incremental chart algorithm has been implemented; the purity strategy currently applies
only to the GLR family.

## 1. Product File List

C99 artifacts (`--lang c`) default to 10 files (for the C++17 artifact list see §1.1, Python artifacts see
§10, C# artifacts see §12, Rust artifacts see §14, Go artifacts see §15):

| File | Contents |
| --- | --- |
| `parser.h` | Public API: `Parser_Run*`, trees, errors |
| `parser.c` | Lexer, parse main loop, semantic actions, tree implementation; when `%epilogue` is present this user code is appended at the end |
| `parser_token.h` | Token enum, `ParserToken_T`, token text interface, lexical DFA declarations |
| `parser_token.c` | Token name table, text helpers, regex DFA and identifier interface implementations |
| `parser_user.h` | User `%code ... %endcode` code + `ParserValue_T` generated from `%value_type` / `%union`; not generated when none of the three is present |
| `parser_table.c` | Parse table / ACTION / GOTO / production table |
| `main.c` | Entry point: by default the generator's built-in demo main; when `%main` is present (and only one block takes effect after conditional compilation) it is replaced with the user body |
| `Makefile` | `make` → `libparser.a` + `parser_demo` |
| `CMakeLists.txt` | CMake targets equivalent to the `Makefile` (`parser` + `parser_demo`) |
| `RESULT.md` | Generation info, product file list, conflict resolution and unresolved conflicts (the full analysis report is not written by default, see §1.7) |

Files from `%copy` are appended to the output directory, and their `.c` / `.h` files are also counted into the
`Makefile` and `CMakeLists.txt`.

Where the two sections land in the artifacts:

| Section | Landing point | Notes |
| --- | --- | --- |
| `%epilogue` | End of `parser.c` (for C++17 the end of `parser.cpp`, outside the namespace, with a `using namespace` line added before the body; for Python the end of `parser.py`; for Rust the end of `src/parser.rs`; for Go the end of `parser.go`) | No new file; declarations from `parser_user.h` are directly usable, and functions produced by the generator can be called by their C99 names (C++17 relies on the compatibility layer, see §1.2; Python shares the module with the parser class) |
| `%main` | The body of `main.c` (for C++17 `main.cpp`; for Python the demo body of `main.py`; for C# a member of the entry class in `Program.cs`; for Rust the demo body of `src/main.rs`; for Go the demo body of `main.go`) | The shell (`stdio` / `string` / `parser.h`; for Python a header comment + `import parser` + class-name alias; for C# a file header comment + `namespace` + entry class declaration) remains a template; the product file list is unchanged |

### 1.1 Target Languages: C++17 (default) / C99 / Python / C# / Rust / Go

The default target is C++17, whose artifacts are produced by a **separate backend**
(`source/src/code_gen_cpp17.cpp` + `source/templates/cpp17_*`) rather than by rewriting the C artifacts as text:

| File | Contents |
| --- | --- |
| `parser.hpp` | Public interface: `Status` / `Token` / `Error` / `Tree` / `Engine` inside the namespace, plus `ParserValue_T` (the `%union` body also lives here) |
| `parser.cpp` | Runtime: parse tables, lexical DFA, lexer, LR/LL drivers, public API implementation |
| `main.cpp` | Entry point: the built-in demo main (reads one line from standard input) or the `%main` body |
| `parser_user.h` | User code from `%code ... %endcode`; not generated when absent |

When `%namespace myapp` is written (or the command line uses `--namespace myapp`), the file names get a prefix:
`myapp_parser.hpp` / `myapp_parser.cpp` / `myapp_main.cpp` / `myapp_parser_user.h`, and the namespace becomes `myapp`
as well; `Makefile` / `CMakeLists.txt` / `RESULT.md` get no prefix, and files from `%copy` keep their original names.
**C99 artifacts cannot use `%namespace`** (combining it with `--lang c` reports an error).

The relationship between C++17 artifacts and C99 artifacts is not "a one-to-one rename"; the APIs are completely
different (see §1.2); Python artifacts are a third interface (see §10). Features such as `%tree` / `%sync` / error
recovery exist for all three targets, and their semantics (including error message text) are aligned item by item —
only the way they are written differs (`%value_type` / `%union` are the C / C++17-only semantic value model, while
Python uses arbitrary objects).

Add `--lang c` (or write `%lang c` in the grammar) to get the C99 form described in the remaining sections of this
document. When a grammar depends on C++17-only things you can write `%cpp_only` in the grammar: combining that with
`--lang c` reports an error immediately.

`--lang python` (or `%lang python`) is the third one: `source/src/code_gen_python.cpp` +
`source/templates/python_*` generate two files, `parser.py` / `main.py`; nothing is compiled and no
`Makefile` is generated, and semantic values are arbitrary Python objects — see §10 for the API. The body of
`%code ... %endcode` is normalized and inserted after the imports in `parser.py`; the C-only forms
`%value_type` / `%union` / `%namespace` are **not supported** (writing them reports an error at generation time), and
the class name is changed with `%class` / `--class`.

`--lang csharp` is the fourth one: `source/src/code_gen_csharp.cpp` + `source/templates/csharp_*` generate
`Parser.cs` / `Token.cs` / `Program.cs` / `parser.csproj` — see §12 for the API. For this target the
semantic value is a three-way choice: "`%value_type` copied as-is / `%union` → a `ParserValue` class with member
fields / neither → `object`"; the three directives `%package` / `%import` / `%class_body`
take effect for this target. Error locations do not use `#line`; instead there are five paired comment lines (§12.3).

`--lang rust` is the fifth one: `source/src/code_gen_rust.cpp` + `source/templates/rust_*` generate
a Cargo project (`Cargo.toml` + `src/parser.rs` / `src/token.rs` / `src/main.rs`) — for the API,
the product file list, and the hard constraint that "the semantic value must satisfy `Copy + Default + Debug`" see
[`14-languages-and-products.md`](../14-languages-and-products.md#rust-产物--lang-rust).
`%package` (crate name) / `%import` (`use`) / `%class_body` likewise take effect,
and **`%destructor` is not supported** (ownership takes care of reclamation; writing it reports an error at
generation time); the lexical regexes use Rust's
**official `regex` crate** (one `regex = "1"` entry in `Cargo.toml`, `use regex::Regex;` at the top of
`src/parser.rs`), and there is no DFA table in the artifacts — see §14.4.

`--lang go` is the sixth one: `source/src/code_gen_go.cpp` + `source/templates/go_*` generate a
Go module (`go.mod` + `parser.go` / `token.go` / `main.go`) — for the API, the product file list, and
the convention that "`%union` members are written like C declarations" see §15.
`%package` (module path) / `%import` (import block) / `%class_body` (a set of
`func (p *Parser) …` methods) likewise take effect, and **`%destructor` is not supported** (the GC reclaims; writing
it reports an error at generation time); the lexical regexes use Go's **standard-library `regexp`** (no third-party
dependency), and there is no DFA table in the artifacts — see §15.5.

### 1.2 C++17 Public API (Qt style)

One class holds the entire parse state: `Parser::Engine` (d-pointer, non-copyable), with methods in camelCase:

```cpp
#include "parser.hpp"

int main()
{
    Parser::Engine engine;
    ParserValue_T result = ParserValue_T{};
    const Parser::Status status = engine.runText("1+2*3", &result);
    if (status != Parser::Status::Ok)
    {
        const Parser::Error *error = engine.lastError();   // 可为 nullptr
        std::printf("解析失败: %s\n", error->toString().c_str());
        return 1;
    }
    for (const Parser::Error &error : engine.errors())     // 一次解析可报多条
    {
        std::printf("%s\n", error.toString().c_str());
    }
    return 0;
}
```

| Member | Purpose |
| --- | --- |
| `Status runText(std::string_view text, ParserValue_T *result)` | Parse text |
| `Status run(const std::vector<Token> &tokens, ParserValue_T *result)` | Parse an already tokenized token sequence |
| `const std::vector<Error> &errors() const` | Errors accumulated in this parse (possibly several per parse) |
| `const Error *lastError() const` | The most recent error; `nullptr` when there is none |
| `ParserLocation currentLocation() const` | Location of the current lookahead (line / column 1-based, `offset` is a UTF-8 byte offset); see §1.5 |
| `std::unique_ptr<Tree> parseTree(std::string_view text)` | `%tree` grammars: build the tree only, without running semantic actions |

The namespace also contains a few free functions and types:

- `enum class Status { Ok, ErrStack, ErrSyntax, ErrLexer, ... }`, with `statusName(Status)`
  returning the name; the numeric values match the `PARSER_*` of the C artifacts;
- `struct Token`: `type` / `value` / `text` (`std::string_view`) / `name` / `pName` /
  `line` / `column` / `offset`; `name` / `pName` are kept only for compatibility with `%code ... %endcode`
  written against the C artifacts;
- `tokenName(TokenType)` / `tokenText(const Token &)`;
- `class Error`: `code()` / `line()` / `column()` / `message()` / `toString()`;
- `class Tree` (`%tree`): `name()` / `text()` / `location()` / `childCount()` /
  `child(i)` / `addChild(unique_ptr)` / `toString()` / `print(FILE * = stdout)`;
  parent-child ownership is managed by `std::unique_ptr` and copying is disabled (write your own clone if you need a
  copy); `location()` is described in §1.5.

**Compatibility layer**: the same namespace also provides a set of same-named entry points for the C artifact API,
for code written in C style inside `%main` / `%epilogue` —

```cpp
Parser_RunText("1+2*3", &result);        // = Parser::Parser_CompatEngine().runText(...)
Parser_GetLastError();                   // 返回 ParserError_T*
PARSER_OK / PARSER_ERR_* / PARSER_MAX_*  // 兼容常量（inline constexpr）
```

Underneath it is a process-wide shared `Engine` (`Parser_CompatEngine()`), so it is **not thread-safe**;
new code should use `Engine` directly. See [`07-codegen.md`](../07-codegen.md) §8.1 for details.

### 1.3 Memory: No Custom Allocator

Memory for C++17 artifacts is entirely handed to the standard library: `std::string` / `std::string_view` /
`std::vector` / `std::deque` / `std::unordered_map` / `std::unique_ptr`,
the parse stack grows on demand (`std::vector`), and the syntax tree is released when it is destroyed. **Custom
allocators are not supported**, and there is no global hook like the C artifacts' `PARSER_USER_DISCARD_VALUE` — to
control memory, either make the semantic value type adhere to RAII (`std::string` / `std::unique_ptr` and the like),
or register cleanup code per member with `%union`'s `%destructor <member>` (see §9.1), which is the form shared by
C and C++17.

### 1.4 `#line`: Errors Point Back to `.gram`

In the artifacts, every piece of **code that comes from `.gram`** carries a pair of `#line` directives:

```c
#line 10 "example/calc.gram"
    result.value = stack->values[...];
#line 511 "parser.c"
```

So when `gcc -std=c99 -c parser.c` reports an error it points straight at the `.gram` line number and echoes that
source line (the same holds for `g++ -std=c++17 -c parser.cpp` under C++17, where the restoring lines point at
`parser.cpp` / `main.cpp`). The positions covered are: semantic actions (`parser.c`), `%code ... %endcode` and
`%union` (`parser_user.h`), `%epilogue` (the end of `parser.c`), and `%main` (`main.c`).
Purely generated content — `parser_table.c` / `parser_token.c` / `parser.h` / `Makefile` /
`CMakeLists.txt` / `RESULT.md` — contains not a single `#line`. See
[`07-codegen.md`](../07-codegen.md) §4.2 for details.

**Python artifacts have no `#line`** (Python has no such directive either); instead a comment line
`# 语法文件 <grammar file> 行 <N>` is pasted before each user code position, pointing at the same line numbers as
the C `#line`:

```python
        if production == 3:
            # 语法文件 example/calc.gram 行 10
            result.value = values[0] + values[2]
            return STATUS_OK
```

It is navigation for humans only (a Python traceback still reports `parser.py`'s own line numbers), the indentation
follows the enclosing block, and `python -m py_compile parser.py` passes as usual. See §10.5 and
[`07-codegen.md`](../07-codegen.md) §4.2.

### 1.5 Location API: `ParserLocation` / `currentLocation()`

The line and column numbers in error messages are now also **structured data**: the parser exposes the location of
the current lookahead, and syntax tree nodes carry the start of the text they cover, so IDEs / ASTs / error reports no
longer have to parse the message text. All six targets (C99 / C++17 / Python / C# / Rust / Go) support this:

| Target language | Location type | Parser API | Token fields | Tree node fields |
| --- | --- | --- | --- | --- |
| C99 | `ParserLocation { int line; int column; int offset; }` | `Parser_CurrentLocation()` | `ParserToken_T::line / column / offset` | `Parser_TreeGetLocation(node)` |
| C++17 | `ParserLocation { int line; int column; int offset; }` | `Engine::currentLocation()` | `Token::line / column / offset` | `Tree::location()` |
| Python | `ParserLocation` (namedtuple: `line` / `column` / `offset`) | `engine.current_location()` | `token.line / column / offset` | `Tree.location` |
| C# | `Parser.Location` (`readonly struct`) | `engine.CurrentLocation` | `Token.line / column / offset` | `Tree.Location` |
| Rust | `ParserLocation { line / column / offset }` (`u32`) | `engine.current_location()` | `Token.line / column / offset` | `Tree::location()` |

Conventions (uniform across the six targets):

- **Line and column are both 1-based**; `offset` is a **UTF-8 byte offset** counted from the start of the input
  (0-based). For non-ASCII input, whether "column" is a character count (Python) or a byte count (C++17) follows each
  lexer's existing counting method, while `offset` is always computed in UTF-8 bytes — use `offset` to align on byte
  positions.
- The location refers to **the start of the current lookahead token**: it comes from the same source as `$line` /
  `$column` inside actions (those two pseudo-variables now simply read `currentLocation()`; the semantics of
  `$token` / `$code` / `$message` are unchanged). On a lexical error, `currentLocation()` is the error position, and
  it agrees with the line and column in `error.line()` / `error.column()` / `toString()`.
- In Token-array mode (C++17 `run(tokens)`), `offset` is determined by the tokens the caller supplied: if it is
  unset it is 0, and `line` / `column` likewise use the values the caller filled in.
- **Syntax tree nodes**: a leaf node is the start of that token; a branch node takes the first child that has a
  location (that is, the start of the text the node covers; it is computed on demand, so building the parent before
  filling in the children also works); for an empty node (such as a branch reduced from `%empty`) all three fields are
  0, meaning "unknown".
- **C99 ownership**: `ParserCommit::text` is allocated with `malloc`, so after reading its contents you must call
  `Parser_FreeCommits()` (the start of the next `Parser_RunText` / `Parser_Run` also
  releases the previous one automatically); `ParserCommit::type` points at a static string — **do not free it**.
  The C99 artifacts of `%mode lex` correspond to `Parser_LexGetCommits(&lexer)` /
  `Parser_LexFreeCommits(&lexer)` (`Parser_LexInit` also clears them automatically).

```cpp
// C++17：打印根节点的位置
Parser::Engine engine;
std::unique_ptr<Parser::Tree> tree = engine.parseTree("1+2*3");
if (tree != nullptr)
{
    const Parser::ParserLocation &at = tree->location();
    std::printf("根节点: 第 %d 行第 %d 列（偏移 %d）\n",
                at.line, at.column, at.offset);
}
// 当前 lookahead（语义动作里 $line / $column 读的就是它）：
const Parser::ParserLocation where = engine.currentLocation();
```

```python
# Python：同样的位置 API（snake_case）
engine = parser.Parser()
tree = engine.parse_tree("1+2*3")
if tree is not None:
    at = tree.location          # ParserLocation(line, column, offset)
    print("根节点: 第 %d 行第 %d 列（偏移 %d）" % (at.line, at.column, at.offset))
at = engine.current_location()  # 当前 lookahead
```

The acceptance cross-check: parse the same input successfully first and print the location of some node, then feed a
counterexample that fails at **the same location**; the line and column in the error message must be exactly equal to
`currentLocation()` / `current_location()`. `example/json.gram` is a ready-made `%tree` example (generate it with
`--lang cpp17` / `--lang python` and read node locations with the snippets above).

### 1.6 Error Message Format (uniform across the six targets)

The description of a syntax error is no longer just a vague statement such as "the parse table has no matching
entry": **it says what is expected here and what was actually found**. All six targets
(C99 / C++17 / Python / C# / Rust / Go) are covered, and for the same grammar + same input,
**the `message`, error code, line and column of the six artifacts are identical word for word**:

| Situation | `message` | Example |
| --- | --- | --- |
| Expected set non-empty (≥ 2) | `语法错误：期望 A / B 之一，实际是 C` | `语法错误：期望 PLUS / MINUS / RPAREN 之一，实际是 DOT` |
| Expected set non-empty (only 1) | `语法错误：期望 A，实际是 C` | `语法错误：期望 ASSIGN，实际是 VARIABLE` |
| Expected set empty (LL family) | `语法错误：该状态下无可用产生式` | LL(0) / no usable column at all in the table |
| Expected set empty (LR family) | `语法错误：该状态下无可用动作` | The ACTION row has no shift/reduce at all |

Rules and sources:

- The **expected set** is the "terminals that have an action on this row" computed at **generation time**: for the LL
  family it is the column set of that non-terminal in the parse table (`expectedTokensByRow`), and for the LR family
  it is every terminal in that state's row of the ACTION table that can shift or reduce.
- The message lists at most **4** expected tokens (in terminal declaration order); beyond that it is summarized with
  `等 N 个之一` (`N` being the total number expected on that row); the struct / object still keeps up to
  `PARSER_MAX_EXPECTED` / `_MAX_EXPECTED` / `MAX_EXPECTED` (8) entries plus the `expectedTotal` count, for the caller to
  lay out itself.
- When the expected set is empty, it falls back to "no usable production / action in this state" (written into an
  artifact constant per mode at generation time: C `PARSER_EXPECTED_FALLBACK`, C++17 `kExpectedFallback`, Python / C# /
  Rust / Go `_EXPECTED_FALLBACK` / `sParserExpectedFallback`).
- The **error code** keeps using the fine-grained codes: when the expected set is non-empty, EOF reports
  `PARSER_ERR_UNEXPECTED_EOF` / `Status::ErrUnexpectedEof` / `STATUS_ERR_UNEXPECTED_EOF` (-27) and everything else is
  `..._ERR_EXPECTED` / `ErrExpected` / `STATUS_ERR_EXPECTED` (-26); when the expected set is empty it falls back to the
  general -21 (`ErrSyntax`).
- The **full diagnostic** is assembled by `ParserError_T::message` / `Error::toString()` / `str(error)` /
  `error.Display`, uniformly formatted as `第 N 行第 M 列：<message>` (line / column 1-based, from the same source as
  the location API in §1.5).
- Besides `message`, the error struct also carries `tokenType` / `tokenText` (the token actually seen),
  `state` (the LR state number; for LL the symbol number on top of the stack), and `expected` / `expectedCount` /
  `expectedTotal` (the expected set and total described above).
- **Lexical errors** do not take the "expected / actual" form: an unterminated string is
  `Status::ErrLexUnterminated` (-28) + "字符串没有闭合", an unrecognized character is
  `ErrLexChar` (-29) + "认不出来的字符", and any other lexical failure is `ErrLexer` (-22) +
  "词法解析失败".
- The `message` of C99 artifacts is a fixed-length field (`PARSER_ERROR_MESSAGE_MAX`, 192 bytes by default) and is
  truncated to the field capacity; the strings of the other five targets are not truncated. Typical messages (with 4
  expected tokens) are never truncated at the default value.

A live comparison (`example/pl0.gram`, `%mode lalr` + `%tree`, the same wording across the six targets):

```text
$ printf 'x := (1 + 2 .' | ./parser_demo
解析失败: code=-26 line=1 column=13 message=语法错误：期望 PLUS / MINUS / RPAREN 之一，实际是 DOT

$ printf 'x := 1 , 2 .' | ./parser_demo
解析失败: code=-26 line=1 column=8 message=语法错误：期望 END / THEN / DO / PLUS 等 16 个之一，实际是 COMMA

$ printf 'begn x := 1 .' | ./parser_demo
解析失败: code=-26 line=1 column=6 message=语法错误：期望 ASSIGN，实际是 VARIABLE
```

### 1.7 `RESULT.md`: brief by default

`RESULT.md` is the single reference document in the artifact directory. Its sections always appear in
this order:

```text
# 生成结果
## 生成信息           source grammar / mode / target language / %tree / %teach / %mermaid /
                      precedence directives / %expect / %token regex / %identifier
(language and compatibility notes)   target-specific notes, %sync, capacity macros,
                      %epilogue / %main blocks, LL(k) lookahead, IELR state splitting,
                      Mermaid diagrams
## 产物文件           which files were written this time
(optional sections)  public API examples, location info and %commit, build and run,
                      runtime and memory, syntax tree, chart modes — only when the
                      corresponding feature is used
## 冲突消解           every conflict removed by %left / %right / %prec
## 未消解冲突         shift/reduce and reduce/reduce conflicts that could not be removed
```

**The trailing `## 文法分析报告` section is not written by default.** Its body enumerates the final
productions / FIRST / FOLLOW tables / automata / conflict details, which easily run to hundreds of lines
and are only useful when debugging a grammar — not something to pin into every artifact directory.
There are two ways to get it, and both yield **the very same text**:

| Way | Effect |
| --- | --- |
| `--full-report` | pastes the whole report into a ```` ```text ```` block at the end of `RESULT.md` |
| `--report FILE` | saves the report as a separate UTF-8 file (word for word identical to what `--full-report` pastes) |

```sh
ParseEngine example/calc.gram --out-dir out --lang c              # brief RESULT.md
ParseEngine example/calc.gram --out-dir out --full-report         # RESULT.md carries the full report
ParseEngine example/calc.gram --out-dir out --report calc-report.txt   # report as its own file
```

The report content follows `--mode` (generating again with another mode produces a different analysis:
productions / FIRST / FOLLOW / automata / conflicts all differ). Of the six code backends, the
`RESULT.md` of C99 / C++17 / Python comes from the template and that of C# / Rust / Go from a separate
implementation, yet `--full-report` appends this section in all six. The `<grammar>.analysis.md`
exported by `--lang gram` **is itself the report** (see §13) and is unaffected by this switch; the two
narrow backends `java` / `js` write no `RESULT.md` at all.

## 2. Parse Entry Points (C99 output artifacts)

This section through §9 describe the API of **C99 artifacts** (`--lang c`); C++17 artifacts are a different
interface (`Engine` / `Tree` / `Error`, see §1.2), and only `ParserValue_T` and the numeric error codes agree
between the two.

```c
int32_t Parser_Run(const ParserToken_T *pTokens, uint32_t count,
                   ParserValue_T *pResult);
int32_t Parser_RunText(const char *pText, ParserValue_T *pResult);
const ParserError_T *Parser_GetLastError(void);
uint32_t             Parser_GetErrorCount(void);
const ParserError_T *Parser_GetError(uint32_t index);
```

`ParserValue_T` is supplied by the user (a POD): the common case is a single line such as `%value_type double`,
but you can also use `%union` or typedef it yourself inside `%code ... %endcode`; when none of these is present the
generator adds `typedef int ParserValue_T;` (when `%code ... %endcode` is written the generator no longer adds the
default typedef, and `parser_user.h` is included before the namespace is opened, so the type the user provides is also
visible inside the namespace). The `PARSER_USER_TOKEN_VALUE(pToken)` macro converts a token into a semantic value (for
example, taking `pToken->value` for a numeric token). Success is judged by the return value: `PARSER_OK` means
success, and the result is written into `pResult`.

### 2.1 Error Recovery API (`error` productions / `%sync` / FOLLOW sets for LL)

The upper bound on the number of errors accumulated in one parse is `PARSER_MAX_ERRORS` (8 by default):

```c
uint32_t             Parser_GetErrorCount(void);            /* 本次解析已报的条数 */
const ParserError_T *Parser_GetError(uint32_t index);       /* 第 index 条，越界返回 NULL */
const ParserError_T *Parser_GetLastError(void);             /* 仍然是最后一条（兼容旧代码） */
```

**Fields of `ParserError_T`** (fields that exist only with `%error_context` are marked separately):

| Field | Meaning |
| --- | --- |
| `code` / `line` / `column` / `message` | Error code, line and column, and a one-sentence explanation (the explanation contains "expecting… actual…") |
| `tokenType` / `tokenText` | Type and lexeme of the token actually seen (the lexeme is a copy, so it is safe from being overwritten by the next token) |
| `state` | LR state number (for the LL family, the symbol number on top of the stack), for debugging |
| `expected[PARSER_MAX_EXPECTED]` (8 by default) / `expectedCount` / `expectedTotal` | Token types expected at this position; `Total` is the true count (possibly more than `Count`) |
| `context` (only with `%error_context`) | The original text of the offending line + a line with a `^` marker |

Besides `-21` (no action in the table and it cannot say what is expected), the error codes are further broken down
into `-26` (the expected set is non-empty but a different token arrived) and `-27` (the input ended early); `-28` /
`-29` are reserved for the two lexical situations "unterminated string/comment" and "unrecognized character". C++17
uses `Status::ErrExpected` / `ErrUnexpectedEof` / `ErrLexUnterminated` / `ErrLexChar`, with the four accessors
`expected()` / `tokenText()` / `state()` / `tokenType()` on `Error`; Python uses
`e.expected` / `e.token_text` / `e.state` / `e.token_type`.

**Actions can read the error scene**: `$line` / `$column` (line and column of the current lookahead), `$token`
(the lexeme of the current lookahead), `$code` / `$message` (code and description of the most recent error, **only
meaningful inside error productions**; writing them elsewhere reports an error at generation time). A typical use is
to wrap printf in a macro inside `%code ... %endcode` (string literals cannot be written inside actions), and then
call it in the error rule:

```gram
%code
#define LOG_ERR(cd, l, c, m, t) printf("%d:%d %s (%s)\n", (l), (c), (m), (t))
%endcode
stmt := error SEMI { LOG_ERR($code, $line, $column, $message, $token); yyerrok; $$ = 0; }
      ;
```

The error table is a **global** `static` array (the same one as `Parser_GetLastError`), cleared at the start of each
`Parser_RunText` / `Parser_Run`; once `PARSER_MAX_ERRORS` entries are written, new errors are discarded and the parser
stops recovering. Both macros use `#ifndef` fallbacks: you can set the defaults in the grammar with
`%max_errors` / `%error_recovery_tokens` (see
[`05-grammar.md` §2](../05-grammar.md#容量宏max_stack_depth--max_errors--error_recovery_tokens)), or override them at
compile time with `-D` (`-D` takes precedence):

| Macro | Default | Grammar directive | Meaning |
| --- | --- | --- | --- |
| `PARSER_MAX_ERRORS` | 8 | `%max_errors N` (1..4096) | Upper bound on the number of errors kept in one parse |
| `PARSER_ERROR_RECOVERY_TOKENS` | 3 | `%error_recovery_tokens N` (0..4096) | Number of tokens that "suppress repeated reporting" after recovery (bison's 3-token rule): it decrements on each successful shift of a real token, and once it reaches 0 the next error is reported again; 0 = no suppression |
| `PARSER_LOOKAHEAD_K` | 1 | `%lookahead N` (0..INT_MAX) or written into the mode name (`LL(2)`) | Lookahead length for the k-aware modes (`lrk` / `lalrk` / `slrk` / `llk` / `ll0`); when k > 1 the runtime maintains a window of k tokens and looks up k-tuple columns, while other modes are always 1 (empty window, single-level table). LL(0) is 0. **There is no policy upper bound on k**, but the column encoding is 64-bit, so if `Token count^k` does not fit, generation reports an error |
| `PARSER_LOOKAHEAD_COLUMN_COUNT` | 1 | (determined by the analysis result, not a directive) | Number of k-tuple columns: on the LR side it is the number of reduce-level columns, on the LL side the number of parse table columns; meaningless when k ≤ 1 (1 is used only to keep the array legal) |

The parse stack depth limit `PARSER_MAX_STACK_DEPTH` (64 by default) works the same way and can be set in the grammar
with `%max_stack_depth N` (1..65535); when the limit is hit, `Parser_Run*` returns `PARSER_ERR_STACK` (-20).

Return value conventions (both cases under a custom signature apply):

| Situation | Return value | `pResult` | `Parser_GetErrorCount()` |
| --- | --- | --- | --- |
| No errors | `PARSER_OK` | valid | 0 |
| Errors were reported but parsing finally succeeded after recovery | `PARSER_OK` | valid, **the result may be incomplete** | > 0 |
| Errors reached `PARSER_MAX_ERRORS`, or recovery was impossible | a negative error code (`PARSER_ERR_SYNTAX`) | undefined, do not read | the number reported |

In other words, only the caller sees that "recovery happened" by looking at `Parser_GetErrorCount()`.

The three targets (C99 / C++17 / Python) share the same recovery strategy, with one path per mode:

- **LR family**: `error` productions take priority, with `%sync` as the fallback;
- **LL family**: **there is recovery even without writing any directive** — at runtime it aligns in four steps: whether
  the top of the stack can take the current token → whether it is a `%sync` anchor → whether the item on top of the
  stack should end here (the current token is in its FOLLOW set) or an item deeper down can take it → discard the token.
  Step 3 is precisely FOLLOW-set recovery;
  for how to write it see [`05-grammar.md` §3.1 / §3.2](../05-grammar.md#31-error-产生式yacc-风格的恢复锚点);
  the three runnable examples are `example/error_recovery.gram`, `example/sync_recovery.gram` and
  `example/ll_follow_recovery.gram`.

**Artifact shape for LR(k) / LALR(k) (`%mode lrk` / `%mode lalrk`).** When k > 1 the ACTION table is split into two
levels and three extra tables appear in the artifacts (C99's `s_ParserReduce` / `s_ParserLookaheadCode` /
`s_ParserLookaheadColumn`, C++17's `kReduce` / `kLookaheadCode` /
`kLookaheadColumn`, Python's `_REDUCE` / `_LOOKAHEAD_CODE` / `_LOOKAHEAD_COLUMN`):

| Table | Contents |
| --- | --- |
| First level `s_ParserAction` / `kAction` / `_ACTION` | Shifts only (the same table as before) |
| Second level `s_ParserReduce` / … | Reduce / accept columns built from k-tuple lookaheads |
| Column index | Column encoding = $\sum_i \mathrm{Token号}_i\cdot(\text{Token 个数})^i$ (the low-order term is the head of the window); columns are looked up by binary search in ascending encoding order |

At runtime each step forms a window from "current + the following k-1 tokens": it first looks up the second level by
the window, and if that hits, it uses that reduce / accept; otherwise it looks up the first-level shift by the head of
the window. The window is the same under `%tree`, in Token-array mode, and in text-stream mode (text-stream mode
pre-fetches k-1 extra tokens); `yyclearin` and "discard lookahead" during error recovery merely shift the whole window
one position to the left, with semantics identical to k = 1. The only added capacity is
`PARSER_LOOKAHEAD_K`: **the artifact API, the return value conventions, and the error table are all unchanged**.

**LL side (`%mode llk` / `ll0`).** There is only one parse table, and the meaning of its columns is the same k-tuple
as the LR second level: k = 1 is a dense token table (byte-for-byte identical to LL(1)), k = 0 has only one column (no
lookahead), and k ≥ 2 builds columns from the k-tuples that are used. The artifacts likewise gain
`s_ParserLookaheadCode` / `s_ParserLookaheadColumn` (C99),
`kLookaheadCode` / `kLookaheadColumn` (C++17), `_LOOKAHEAD_CODE` /
`_LOOKAHEAD_COLUMN` (Python). **The LL public API and return value conventions are completely unchanged**,
and the error recovery of `Parser_GetErrorCount()` works just the same.

## 3. Syntax Tree API (`%tree`)

```c
typedef struct ParserTree_T {
    const char *pName;                 /* 符号名 */
    char *pText;                       /* 终结符词素；非终结符为 NULL */
    struct ParserTree_T **ppChildren;  /* 子节点数组 */
    uint32_t childCount;
    uint32_t childCapacity;
} ParserTree_T;

ParserTree_T *Parser_ParseTreeText(const char *pText);
uint32_t Parser_TreeChildCount(const ParserTree_T *pNode);
const ParserTree_T *Parser_TreeGetChild(const ParserTree_T *pNode, uint32_t index);
const char *Parser_TreeGetName(const ParserTree_T *pNode);
const char *Parser_TreeGetText(const ParserTree_T *pNode);
void Parser_TreePrint(const ParserTree_T *pRoot);
void Parser_TreeFree(ParserTree_T *pRoot);
```

`Parser_ParseTreeText` builds only the CST and does not execute semantic actions, so it can coexist with semantic
actions: `Parser_RunText` evaluates, and `Parser_ParseTreeText` produces the tree.

## 4. Token and Lexical API

```c
const char *Parser_TokenName(ParserTokenType_T type);
void Parser_TokenText(const ParserToken_T *pToken, char *pBuffer, uint32_t size);

/* 声明了 %identifier 时额外生成 */
uint32_t Parser_TokenIdentifierLength(const char *text);
uint8_t  Parser_TokenIsIdentifier(const char *text);
ParserTokenType_T Parser_TokenKeyword(const char *text);

/* 通用 DFA 最长匹配（Lexer 与上面两个函数共用） */
uint32_t Parser_LexDfaMatch(const ParserLexDfa_T *pDfa, const char *pText);
```

The value order of `Parser_TokenText` is: `pName` (lexeme) → `name` (numeric / string lexeme)
→ token enum name. So a fixed literal is displayed as `COLON(:)`, an identifier as
`VARIABLE(x)`, and a number as its numeric value.

### Token / commit display aliases

A declaration may end with `alias "display name"`, for example:

```gram
%token STRING "..." alias "字符串"
%commit block "/\\*...\\*/" alias "块注释"
```

The alias is used only in name tables and diagnostics; it does not change token values, grammar symbols, or the
original `type`/`kind` in commit elements. C99/C++17 generate two-column static tables (original name, alias), while
Python / C# / Rust / Go generate a mapping from name to alias (for Go it is `TOKEN_ALIASES`, and when `TokenName`
hits it displays `NAME(alias)`); diagnostics such as `token_name` display `STRING(字符串)` when an alias exists, and
only `STRING` when there is none. `%define NAME "..." alias "..."`
follows the same rule as `%token`.

## 5. Error Codes

| Code | Name | Meaning |
| --- | --- | --- |
| 0 | `PARSER_OK` | Success |
| -20 | `PARSER_ERR_STACK` | Stack / frame overflow |
| -21 | `PARSER_ERR_SYNTAX` | Syntax error (including no match in the LL table and no match in the SLR/LALR/IELR tables) |
| -22 | `PARSER_ERR_LEXER` | Lexical error |
| -23 | `PARSER_ERR_DUPLICATE` | Duplicate definition |
| -24 | `PARSER_ERR_UNDEFINED` | Undefined |
| -25 | `PARSER_ERR_SYSTEM` | System / memory error |
| -26 | `PARSER_ERR_EXPECTED` | Several tokens were expected but a different one arrived (fine-grained code, see §1.6; the other targets have spellings such as `ErrExpected` / `STATUS_ERR_EXPECTED`) |
| -27 | `PARSER_ERR_UNEXPECTED_EOF` | Input ended early (`ErrUnexpectedEof` / `STATUS_ERR_UNEXPECTED_EOF`): used when the expected set is non-empty, more specific than the general -21 |
| -28 | `PARSER_ERR_LEX_UNTERMINATED` | Lexical error: string not closed (`ErrLexUnterminated`) |
| -29 | `PARSER_ERR_LEX_CHAR` | Lexical error: unrecognized character (`ErrLexChar`) |

`Parser_GetLastError()` returns a read-only `ParserError_T{code, line, column,
message}`; the demo program prints it as
`解析失败: code=-21 line=1 column=5 message=...`.

Error codes do not change with the target language: on the C# side these are same-named constants
`STATUS_ERR_EXPECTED` / `STATUS_ERR_UNEXPECTED_EOF` / `STATUS_ERR_LEX_UNTERMINATED` /
`STATUS_ERR_LEX_CHAR`, and Python uses
`STATUS_ERR_*` — the values correspond one-to-one with the table above, see §1.6.

The semantics of the error codes do not change because of error recovery: when something was recovered but parsing
ultimately succeeded, the return value is still `PARSER_OK`, and `PARSER_ERR_SYNTAX` stands for both "recoverable
syntax error" and "unrecoverable syntax error" — to tell them apart, look at `Parser_GetErrorCount()` (see §2.2).

## 6. Memory: Allocator and `memset` / `memcpy` Hooks (C99 output artifacts only)

C++17 artifacts do not have this section: all memory is managed by the standard library (see §1.3), and custom
allocators are not supported. Neither do they generate `PARSER_MEMSET` / `PARSER_MEMCPY` — `std::string` / `std::vector` /
`std::unique_ptr` handle zeroing and copying themselves, and there are no libc calls to replace.

In C99 artifacts, only the syntax tree of `%tree` is allocated by the runtime: the nodes and child arrays of
`ParserTree_T` go through `malloc` / `realloc` / `free`, and `Parser_TreeFree` releases them in bulk. To switch to your
own memory pool (a static pool / an RTOS heap / an allocator with statistics), change
`source/templates/c99_custom_tree_helpers.c.tmpl`, `c99_custom_lr_tree_body.c.tmpl`,
and these three call sites, then regenerate.

Memory for semantic values is entirely the user's responsibility: `ParserValue_T` is copied by value, and the runtime
only moves it around on the parse stack — it neither allocates nor frees. For fields pointing at heap memory (strings,
AST nodes, …) allocate and release them yourself inside
`%code ... %endcode`; `parser_user.h` is included by several `.c` files, so helper functions written in the header
should use `static inline` and global state should live in a function-local `static`.

**Values dropped when error recovery pops the stack** deserve special care: both `error` production and `%sync`
recovery pop a number of stack slots, and the `ParserValue_T` in those slots will never be used again. The runtime
does not know the lifetime of user semantic values, so there are two available approaches: when `%union` is present,
use `%destructor <member> { ... }` to register cleanup code per member (the preferred one; supported by both C and
C++17 artifacts, see §9.1); otherwise C artifacts can also define the global hook below:

```c
#define PARSER_USER_DISCARD_VALUE(pValue)   /* pValue 是 ParserValue_T * */
```

When defined, it is called once for each discarded value; when not defined, nothing is called (that is, memory you
`malloc`ed yourself **will leak**). Define it inside `%code ... %endcode`, for example:

```c
#define PARSER_USER_DISCARD_VALUE(pValue) Ast_Free((AstNode *)*(pValue))
```

Members for which `%destructor` was written no longer go through this hook (the dispatch table hits, so it is not
called), while members without one still do. The `ParserTree_T` of `%tree` does not go through this macro: tree nodes
are allocated by the runtime itself, which knows how to free them.

### 6.1 `PARSER_MEMSET` / `PARSER_MEMCPY`: Replacing memset / memcpy in the generated code

Every zeroing and copy operation in the generated code is written as a macro call, and the macros themselves live in
the `#ifndef` fallback section of `parser.h`
(the `__SEC_DEFINES__` section of `source/templates/c99_parser_prologue.c.tmpl`):

```c
#ifndef PARSER_MEMSET
#include <string.h>
#define PARSER_MEMSET(pDst, value, size) memset((pDst), (value), (size))
#endif
#ifndef PARSER_MEMCPY
#include <string.h>
#define PARSER_MEMCPY(pDst, pSrc, size) memcpy((pDst), (pSrc), (size))
#endif
```

`parser.h` expands `parser_user.h` first and then this fallback section, so writing a same-named `#define` inside
`%code ... %endcode` already takes effect (no duplicate-definition clash), and `-D PARSER_MEMSET=...` at compile time
also takes precedence. A typical use:

```c
/* 带统计的清零：数一数运行时到底清了多少字节 */
#define PARSER_MEMSET(pDst, value, size) \
    (g_MemsetBytes += (size), memset((pDst), (value), (size)))
```

When the semantic value type is something like an array of structs, these two macros are the only exit for bulk
memory operations in the generated code, so switching them to `memcpy_psram` or the like moves everything over at
once. The macro arguments are evaluated only once and substituted positionally, so a normal call such as
`PARSER_MEMSET(&value, 0, sizeof(value))` with an identically named identifier does not go wrong.

## 7. Generated Makefile / CMakeLists.txt

| Target | Purpose |
| --- | --- |
| `make` / `make all` | Compile `libparser.a` and `parser_demo` |
| `make lib` | Compile the static library only |
| `make demo` | Compile the demo program only |
| `make run` | Compile and run (ARGS is passed to main as argv; the built-in demo main reads standard input) |
| `make clean` | Delete `.o`, `.a`, `parser_demo` |

C++17 artifacts use the same set of targets (`libparser.a` + `parser_demo`), except that the sources are
`parser.cpp` / `main.cpp` and it uses `g++ -std=c++17`; you can also skip the Makefile:

```sh
gcc -std=c99 -Wall -Wextra -O2 *.c -lm -o parser_demo            # --lang c
g++ -std=c++17 -Wall -Wextra -O2 parser.cpp main.cpp -lm \
    -o parser_demo                                               # 默认（cpp17）
```

The same directory also has an equivalent `CMakeLists.txt` that does not depend on the Makefile:

```sh
cmake -S . -B build
cmake --build build
printf '<文本>' | ./build/parser_demo
```

It defines two targets, the `parser` static library and `parser_demo`; `.c` files brought in by `%copy` are appended
to the library's source list; on Windows it does not link `libm`, while other platforms link `-lm` just like the
Makefile. When building with MSVC it adds `/utf-8` to both targets (artifact sources are UTF-8, and without this
switch MSVC decodes them using the GBK code page, which corrupts Chinese string literals at compile time).

## 8. Demo Program Behavior

- `printf '<文本>' | ./parser_demo`: reads one line from standard input (up to a newline or EOF; a trailing
  newline / `\r` is not part of the content) and then starts parsing; `%tree` grammars print the syntax tree, and
  grammars with semantic actions print the parse result (`PARSER_USER_DEMO_PRINT_VALUE`).
  Redirection such as `./parser_demo < 输入.txt` works the same way, taking only the first line of the file.
- When an empty line is read it still enters parsing — empty input is usually a syntax error (exit code 1), and the
  "print usage only" branch no longer exists.

## 9. Artifact Shape for `custom` + `%union`

When `%union` is written, `parser_user.h` gains a union (the member declarations come from the grammar as-is and are
appended after the user's `%code ... %endcode` code):

```c
/* 语义值类型：由文法的 %union 生成，成员声明原样来自文法 */
typedef union {
    int          ival;
    double       dval;
    struct Node *node;
} ParserValue_T;
```

Actions still write `$1 + $3` / `$$ = $1`, and the generator expands them into member accesses according to the
member each symbol is bound to:

```c
/* LL(1)：$1 / $3 是 term（<ival>），$$ 是 expr（<ival>） */
result.value.ival = values[1].ival + values[3].ival;

/* LALR(1)：$n 走状态栈槽位，成员访问跟在槽位后面 */
result.value.ival = stack->values[stack->top - 3u + 0u].ival +
                    stack->values[stack->top - 3u + 2u].ival;

/* 换一个成员：$$ 是 <dval>、$1 是 <ival>，隐式转换交给 C 编译器 */
result.value.dval = values[1].ival / 2.0;
```

Because it is a union, the demo program takes the value by member: with `ParserValue_T result;` you read the result
as `result.ival` (the member name is exactly the one written in the grammar).

### The Token value is `ParserValue_T`

When `%union` is written, `ParserToken_T.value` also changes from `int32_t` to `ParserValue_T`
(`parser_token.h` includes `parser_user.h` under a guard, sharing `PARSER_USER_HEADER_INCLUDED` with `parser.h` so
that it is expanded only once):

```c
#ifndef PARSER_USER_HEADER_INCLUDED
#define PARSER_USER_HEADER_INCLUDED 1
#include "parser_user.h"
#endif

typedef struct __ParserToken_T
{
    ParserTokenType_T type;
    ParserValue_T value;   /* 没有 %union 时是 int32_t value; */
    const char *name;
    const char *pName;
    uint32_t line;
    uint32_t column;
} ParserToken_T;
```

The lexer writes the lexeme straight into the corresponding member according to `%token <member>`, so terminal
semantic values no longer need hand-written conversion macros:

| Token source | Value written into the member | Generated code shape |
| --- | --- | --- |
| Numeric literal (`%token <ival> NUMBER`) | The numeric value parsed by the lexer | `pToken->value.ival = (value);` |
| Double-quoted string (`%token <sval> STRING`) | The unescaped text | `pToken->value.sval = pCtx->lexeme;` |
| The member bound to an identifier (including `%identifier`) | The matched text | `pToken->value.sval = pCtx->lexeme;` |
| `%token <sval> regex KEYWORD "[a-z]+"` | The matched text | `case TOKEN_KEYWORD: pToken->value.sval = pCtx->lexeme; break;` |
| Fixed literal (`%define PLUS "+"`) | Nothing (there is no lexeme value, so it stays 0) | none |
| A token with an unbound member under `%union` | Nothing (there is no such member in the union, so it stays 0) | none |

`Parser_UserTokenValue()` is simplified along with it: when `PARSER_USER_TOKEN_VALUE` is not defined it directly
`return pToken->value;` (without `%union` it is still a zero value produced by memset). To take over yourself, define
`PARSER_USER_TOKEN_VALUE` as before, which takes precedence.

Two things to keep in mind:

- What is written in is the address of `pCtx->lexeme`, which is a buffer the lexer reuses, so reading the next token
  overwrites it; to keep it long term, `malloc` + `memcpy` yourself.
- Numeric tokens are best bound to a numeric member, and regex and identifier tokens to a text member (the lexer only
  has text on hand). A wrong binding is not blocked at generation time and is reported by the C compiler as a warning —
  for example `%token <ival> regex NUM "[0-9]+"` generates
  `pToken->value.ival = pCtx->lexeme;`, so convert in the action if you want a number.
- When writing `%union`, do not forget to bind members for the built-in tokens you use: without a binding there is no
  target member, and the lexer writes no semantic value (numeric tokens are therefore always zero). In C artifacts
  those `pToken->value = …` lines are deleted wholesale — assigning an `int` to the union does not compile; generation
  prints a `警告：` notice about it.

See `example/union_demo.gram` for a complete example: after generating and compiling (both the C99 and C++17 artifacts),
feeding `1 + 2 * 3` / `(1 + 2) * 3` / `2 * 3 + 4 * 5` yields 7 / 9 / 26 respectively.

### 9.1 `%destructor`: Which Code Runs When a Semantic Value Is Discarded

When `%destructor <member> { ... }` is written, the artifacts gain a static function that dispatches by symbol
(for C artifacts it is in `parser.c`, for C++17 artifacts in the unnamed namespace of `parser.cpp`):

```c
/* C 产物：统一符号号 → %union 成员 → 该成员的清理片段 */
static uint8_t Parser_DiscardValue(ParserValue_T *pValue, uint16_t symbol)
{
    (void)pValue;
    switch (symbol)
    {
    /* %destructor <str>：list、item */
    case 10u:
    case 11u:
    {
        free(pValue->str);          /* 文法里的 free($$) 展开成这里 */
        break;
    }
    default:
        return 0u;                  /* 没绑 %destructor：调用方退回全局钩子 */
    }
    return 1u;
}
```

Symbol numbers are **unified**: a terminal is its token number (0 .. terminal count-1), and a non-terminal is
"terminal count + non-terminal number" — the two ranges do not overlap, so from this number alone on the stack you know
which grammar symbol it is and hence which member it is bound to. Symbols bound to the same member are merged into one
group of `case`s.

Call sites:

| Backend | Where the semantic values live | Discard points |
| --- | --- | --- |
| C (LR family) | `ParserLRStack_T.values[]` + `symbols[]` | `Parser_LRDiscardTop` (pop during error recovery), `Parser_LRDiscardStack` (clear the stack on failure) |
| C (LL family) | `ParserSemanticFrame_T.values[]` + per-slot symbol | `Parser_LLDiscardTop` (pop a frame during recovery), `Parser_ReleaseContext` (on failure) |
| C++17 (LR family) | `LrStack::values` + `symbols` | `LrStack::discardTop` / `discardAll` (the RAII guard of `runLoop`) |
| C++17 (LL family) | `LlFrame::values` + per-slot symbol | `discardFrameValues` (`llDiscardTop` / `runLoop` on failure) |

Two conventions:

- **Members with `%destructor` go through it and no longer call the global hook**: the C artifacts'
  `PARSER_USER_DISCARD_VALUE` is called only when `Parser_DiscardValue` returns 0 (that symbol has no
  `%destructor` bound); the artifacts of a grammar without `%union` are byte-for-byte identical to before (neither the
  dispatch function nor the stack-clearing function is generated). C++17 has no such hook, and unbound symbols do
  nothing.
- **Only slots that really had a value written are cleaned**: LL semantic frames record the symbol number per slot
  (`symbols[]`), and a slot that never had a value written keeps `PARSER_INVALID_INDEX`, so code such as `free` is not
  executed on it; C++17 frames additionally have a `resultDone` flag recording "whether the action ran". The
  accumulator slot rewritten for left-recursion does not record a symbol — that ownership has already been handed to
  the action under the identity "the previous round's result", and recording it would clean up twice.

For a counting self-check see `example/destructor_demo.gram`: two `%destructor`s each add a counter, and feeding 4
groups of input prints "how many times cleanup ran", with identical output from the C and C++17 artifacts.

## 10. Python Output Artifacts (`--lang python`)

Python artifacts consist of only two files, `parser.py` / `main.py` (plus the `mermaid/*` of `%mermaid`
and files copied in by `%copy`, and `RESULT.md`); they **need no compilation** and run as soon as `python3` is
installed. The purpose is quick analysis of a grammar: for the same grammar and same input the error codes, line and
column numbers, error messages, and `%tree` tree text are byte-for-byte aligned with the C / C++17 artifacts, giving
the shortest feedback loop when editing a grammar.

Another use is as an interface for AI / scripts: `printf '...' | python3 main.py` returns only a few lines, and
`python3 -c "import parser; e = parser.Parser(); e.run_text(...); print(e.result, e.errors)"`
returns a single line — a coding agent can check a grammar from that without pulling artifacts or long logs into its
context (see [`07-codegen.md`](../07-codegen.md) §9 for details).

| File | Contents |
| --- | --- |
| `parser.py` | `import` + user code (`%code ... %endcode`, see §10.5) + module-level constants (status codes, `Token`, `Tree`, `ParserError`) + lexical tables / parse tables + the parser class; when `%epilogue` is present another block of user code is appended at the end |
| `main.py` | Demo entry point: header comment + `import parser` + class-name alias + the built-in demo body; when `%main` is present the whole body is replaced |
| `RESULT.md` | Generation info, product file list, run instructions, conflict resolution and unresolved conflicts (the full analysis report is not written by default, see §1.7) |

### 10.1 Module-Level Constants and Functions

```python
import parser

parser.STATUS_OK            # 0；解析成功
parser.STATUS_ERR_STACK     # -20；分析栈溢出（%max_stack_depth）
parser.STATUS_ERR_SYNTAX    # -21；语法错误
parser.STATUS_ERR_LEXER     # -22；词法错误
parser.STATUS_ERR_DUPLICATE # -23
parser.STATUS_ERR_UNDEFINED # -24
parser.STATUS_ERR_SYSTEM    # -25

parser.status_name(status)  # "Ok" / "ErrSyntax" / …；未知返回 "Unknown"
parser.TOKEN_*              # Token 类型枚举（数值与 C / C++17 一致）
parser.token_name(type_id)  # Token 类型名；越界返回 None
parser.token_text(token)    # 显示文本：优先词素，没有词素退回类型名
```

- `class Token`: the six fields `type` / `text` / `value` / `line` / `column` / `offset`
  (`offset` is a UTF-8 byte offset);
- `class Tree` (`%tree` grammars): `name` / `text` / `location` / `children`, with the methods
  `child_count` (a property) / `child(i)` / `add_child(node)` / `clone()` /
  `to_string()` / `print(stream=None)`; `location` is a `ParserLocation` (see §1.5);
- `class ParserError`: `code` / `line` / `column` / `message`,
  and `str(error)` directly yields "第 N 行第 M 列：…" (consistent with the format of the C artifacts'
  `Parser_SetError`).

### 10.2 The Parser Class

The class name is determined by `%class` / `--class` (`Parser` by default); `main.py` aliases it with
`Parser = parser.<class name>`, so the demo body uniformly writes `Parser`. The class's read-only attributes:

| Member | Meaning |
| --- | --- |
| `run_text(text)` | Parse a piece of text; returns a status code and writes the semantic result into `result` (`None` on failure) |
| `run(tokens)` | Parse an already tokenized `Token` sequence |
| `parse_tree(text)` | Build the syntax tree only, without running semantic actions; returns the tree root, or `None` on failure (the reason is in `errors`) |
| `current_location()` | Location of the current lookahead (`ParserLocation`, line / column 1-based + UTF-8 byte offset); see §1.5 |
| `result` | The semantic value of the most recent parse (an arbitrary Python object, where `$$` lands) |
| `errors` | Errors accumulated in this parse (a list of `ParserError`; there may be several when recovery kicks in) |
| `last_error` | The most recent error; `None` when there is none |
| `max_stack_depth` / `max_errors` / `error_recovery_tokens` / `value_slots` | The actual values of `%max_*` and the internal slot count (read-only; see the "容量宏" section of `RESULT.md` too) |

```python
import parser

engine = parser.Parser()
status = engine.run_text("1+2*3")
if status == parser.STATUS_OK:
    print(engine.result)                 # 7
else:
    error = engine.last_error
    print(error.code, error.line, error.column, error.message)
```

For `%tree` grammars:

```python
tree = parser.Parser().parse_tree("1+2*3")
if tree is not None:
    tree.print()                         # 与 C 产物 Parser_TreePrint 的文本一致
```

### 10.3 Semantic Actions and Lexical Semantic Values

Inside an action, `$$` writes `result.value` and `$1` reads `values[0]` (`$n` is simply the semantic value of the
n-th symbol on the right-hand side), expressions are translated according to Python syntax (write `and` / `or` / `not`
instead of `&&` / `||` / `!`, and do not put a semicolon at the end of a statement); `yyerrok` / `yyclearin` become
`self._yy_err_ok()` / `self._yy_clearin()`. The bodies of `%code ... %endcode` (as well as `%epilogue` /
`%main`) are inserted into the module and indentation-normalized (see §10.5). Because semantic values are Python
objects, there are **no** `%value_type` / `%union` / `%token <member>` here (writing them reports an error at
generation time).

The semantic values the lexer hands out: a string token is a `str` with the quotes stripped and escapes resolved, a
numeric token is an `int` / `float`, identifiers and fixed literals are the lexeme text, and the EOF and `error`
pseudo-tokens are `None`. The character tests in `parser.py` (`_is_digit` / `_is_alpha` / `_is_space` …) follow ASCII
semantics, aligned with `isdigit` / `isalpha` of the C target, so lexical boundaries agree across the targets.

### 10.4 Lexical: Fixed Literals + Python Regexes

The lexer of Python artifacts **does not use the generator's built-in DFA**; instead the regexes go as they are to the
standard library's `re` module — `re.compile` runs once when `parser.py` is loaded, and lexical matching calls
`match()` at the current position:

| Declaration in the grammar | Table in `parser.py` |
| --- | --- |
| `%token regex NAME "正则"` | `_REGEX_RULES = [(编译好的正则, TOKEN_NAME, 声明序), …]` |
| `%identifier "正则"` | `_IDENTIFIER_RE` (`None` when absent, falling back to the built-in C-style identifier) |
| `%commit` / `%trivia "模式"` | `_COMMIT_RES = [(编译好的正则, 类型名, 是否 skip, 声明序), …]` |

The match order is the same as the other targets: built-in strings → fixed literals and the regexes above together,
**taking the longest**; on equal length, literals win, then declaration order; fragments matched by `%commit` do not
enter the grammar level.

Across targets, distinguish matching order from character domains:

- The C/C++ shared runtime supports `\b` word boundaries. Byte mode uses ASCII word characters; `(?u)` uses fixed Unicode 17.0 semantics. Native libraries may use different character domains and Unicode versions.
- Ordinary C DFA rules take the longest accepting prefix. Ordered runtimes and most native libraries follow branch and greedy/lazy order inside one rule; the lexer then compares lengths and declaration order across rules. Patterns such as `a|ab` are not automatically equivalent across engines.
- C++ `dfa` optimizes only a proven ASCII subset, otherwise all byte rules fall back to the standard library. `vm` uses shared execution throughout, and explicit Unicode rules always use the shared path.

See [regex compatibility](../21-regex-compatibility.md) for each rule's engine, unverified native semantics and rejection reasons.
`--check` validates generation in memory; `--strict-regex` rejects native rules without proven shared equivalence.
C and C++ `vm` require successful shared compilation. Unrecognized native extensions may warn and proceed, but known unsupported capabilities are rejected.
The shared bounded-repeat limit is 1000; nullable patterns are rejected.

### 10.5 Demo Program Behavior

`python3 main.py` behaves the same as the C / C++17 `parser_demo`: it reads one line from standard input (up to a
newline or EOF; a trailing newline / `\r` is not part of the content) and then parses; `%tree` grammars print the
syntax tree, and when `result` is not `None` (which usually means semantic actions are present) one more line
`解析结果: <repr>` is printed. Exit code 0 means parsing succeeded and 1 means it failed. When `%main` is present this
whole block is replaced by the user body (the shell's `import parser` and the class-name alias `Parser` are kept).

Example: `example/python_demo.gram` (`%class PyCalc` + LALR + Python actions).

### 10.6 Where User Code Is Inserted: `%code` / `%epilogue` / `%main`

| Section in the grammar | Landing point | Notes |
| --- | --- | --- |
| `%code` … `%endcode` | In `parser.py`, after `import sys` and before the status code constants | Several blocks may be written and are concatenated in order of appearance (a blank line between blocks); the functions / constants defined there can be called directly from actions |
| `%epilogue` | End of `parser.py` | In the same module as the parser class, so methods of the class can be called |
| `%main` | The demo body of `main.py` | The shell's docstring, `import sys` / `import parser` and `Parser = parser.<class name>` are kept, and you must write the entry call yourself |
| `%copy` | Copied into the output directory as-is | In Python you must `import` it yourself (no automatic renaming / rewriting of contents) |

All four bodies are **normalized** according to Python indentation rules: trailing `\r` is dropped, leading and
trailing blank lines are removed, the common indentation is stripped according to the smallest leading whitespace
across all non-empty lines, and remaining leading tabs are converted to 4 spaces. So even if you indent the code to
match the grammar layout (indenting the `%code` block by 4 spaces overall, writing the `%main` body indented), it will
not become an `IndentationError`; nothing other than leading whitespace is changed. When there is no user code this
section is not inserted.

Each body is additionally preceded by a **line marker** comment `# 语法文件 <grammar file> 行 <N>` (Python has no
`#line`, so a comment is the only option), indicating which line of the `.gram` this code comes from; the line numbers
pointed at match the C target's `#line` exactly (actions → the line where the action starts, `%code` / `%epilogue` /
`%main` → the first line of the body).

See `example/python_demo.gram` for an example (the action calls `py_value` defined in
`%code ... %endcode`).

## 11. Artifact Shape for Lexical Mode (`%mode lex`)

`%mode lex` is supported for all six target languages, and the artifacts contain **no parser** in any of them:

| Target | Files | Contents |
| --- | --- | --- |
| C | `lexer.h` | Token enum (`ParserTokenType_T`) / `ParserToken_T` / `ParserError_T` / `PARSER_*` macros + the lexer entry point |
| C | `lexer.c` | Error table + lexer + `Parser_LexInit` / `Parser_NextToken` |
| C | `parser_token.h` / `parser_token.c` | `Parser_TokenName` / `Parser_TokenText` and the lexical DFA table |
| C | `main.c` | CLI: read standard input to EOF and print the token type of each token |
| C++17 | `lexer.hpp` / `lexer.cpp` | The same public surface: `TokenType` / `Token` / `Error` / `Engine::tokenize` |
| C++17 | `main.cpp` | The same CLI (`liblexer.a` + `lexer_demo`) |
| Python | `parser.py` / `main.py` | A lexer class + `tokenize(text)` (returning `(status code, token list)`) / the same CLI |
| C# | `Parser.cs` / `Token.cs` / `Program.cs` | As at left (`List<Token>`) |
| Rust | `src/parser.rs` / `src/token.rs` / `src/main.rs` | Lexer + `engine.tokenize(text)` → `(i32, Vec<Token>)` / the same CLI |
| Six targets | `Makefile` / `CMakeLists.txt` / `parser.csproj` / `Cargo.toml` | C / C++17 use Makefile / CMake (`liblexer.a` + `lexer_demo`), C# uses `dotnet build`, Rust uses `cargo build` (with the official `regex` dependency), Python needs none |
| Six targets | `RESULT.md` | Product file list + token table |
| C / C++17 | `parser_user.h` | Generated only when there is `%code ... %endcode` / `%union` |

**No parser is generated**: C has no `parser.h` / `parser.c` / `parser_table.c` (hence no
`Parser_Run` / `Parser_RunText`), C++17 has no `parser.hpp` / `parser.cpp`,
Python's `parser.py` has no parse tables / parse stack / `run_text`, C#'s
`Parser.cs` has no parse tables / parse stack / `runText`, and the Rust / Go `parser` module has
only the lexer and `tokenize` / `Tokenize`.

Public API (`lexer.h`):

```c
typedef struct __ParserLexer_T
{
    const char *pText;
    uint32_t textPosition, line, column;
    uint8_t textMode;
    char lexeme[PARSER_MAX_STRING_LENGTH];
    ParserToken_T current;
} ParserLexer_T;

void Parser_LexInit(ParserLexer_T *pLexer, const char *pText);
int32_t Parser_NextToken(ParserLexer_T *pLexer, ParserToken_T *pToken);
const ParserError_T *Parser_GetLastError(void);
uint32_t Parser_GetErrorCount(void);
const ParserError_T *Parser_GetError(uint32_t index);
```

`Parser_NextToken` returns `PARSER_OK` with `pToken->type == TOKEN_END` once it reaches the end; a lexical
error returns a negative fine-grained code (`-29` unrecognized character / `-28` string not closed), with the
explanation and line and column in the error table. The struct fields are "generator-internal + caller read-only", and
the caller only has to allocate a `ParserLexer_T` on the stack and hand it to `Parser_LexInit`.

The CLI (built-in demo main, which `%main` can replace wholesale) reads standard input to EOF and prints one token per
line:

```text
VARIABLE     "a"          1:1
PLUS         "+"          1:3
NUMBER       "12"         1:5
END          "END"        2:1
共 3 个 Token
```

The last line is the total token count; on a lexical error it prints
`词法错误: code=… line=… column=… message=…` and exits with code 1. Example: `example/lexer_only.gram`.

C++17's public surface shares the names and style of the parse artifacts, except that `runText` / `run` become
`tokenize`:

```cpp
Engine engine;                      // 命名空间内（默认 Parser::）
std::vector<Token> tokens;
if (engine.tokenize("a + 12", &tokens) == Status::Ok) {
    for (const Token &token : tokens) {
        std::printf("%s\n", tokenName(token.type));
    }
}
```

The lexemes of the tokens handed out point at a stable buffer inside the engine (copied into
`std::deque<std::string>` by `tokenize`) and are valid only until the next `tokenize`.

On the Python side it is just the lexer class in the module:

```python
import parser

status, tokens = parser.Parser().tokenize("a + 12")
for token in tokens:
    print(parser.token_name(token.type), token.text, token.line, token.column)
```

The module has no `Tree` / `parse_tree` / `run_text`; errors live in `lexer.errors` (a list of `ParserError`) and
`last_error`, with fine-grained codes consistent with C / C++17.

## 12. C# Output Artifacts (`--lang csharp`)

Artifacts: `Parser.cs` / `Token.cs` / `Program.cs` / `parser.csproj` / `RESULT.md`.
Lexical mode uses the same files, except that `Parser.cs` contains only the lexer and `tokenize`.

### 12.1 Public API

In the public API, type names start with a capital letter and method names match C++17 / Python:

```sh
dotnet build
printf '1+2*3' | dotnet run
```

```csharp
Parser engine = new Parser();
int status = engine.runText("1+2*3");
if (status == Parser.STATUS_OK) {
    System.Console.WriteLine(engine.result());
} else {
    System.Console.WriteLine(engine.lastError());
}

Parser.Tree tree = engine.parseTree("1+2*3");     // %tree 时
int glrStatus = engine.runGlrTree("2+3*4");       // glr / earley / cyk + %tree
for (int i = 0; i < engine.glrResultCount(); i++)
    engine.glrTree(i).print();
List<Token> tokens = new List<Token>();
engine.tokenize("a + 12", tokens);                // %mode lex 时
```

- Type names: `Parser` / `Token` / `Parser.Tree` / `Parser.ParserError`; the entry class is called
  `Program` following C# convention (C# does not allow a member name to equal the class name, so the name `Main.cs` is
  not viable);
- `Parser.ParserError`: `code` / `line` / `column` / `message` (`ToString()` yields
  "第 N 行第 M 列：…") / `tokenType` / `tokenText` / `state` / `expected`;
- `Parser.Tree`: fields `name` / `text` / `children`, methods `childCount()` /
  `child(i)` / `addChild(node)` / `print()`;
- `Token`: `type` / `text` / `value` / `line` / `column` / `offset`, plus the static
  `Token.tokenName(type)` / `Token.tokenText(token)`;
- In semantic actions, `yyerrok` / `yyclearin` are translated to `yyErrOk()` / `yyClearIn()`, and
  `$line` / `$column` / `$token` / `$code` / `$message` are translated to
  `currentLine()` / `currentColumn()` / `currentTokenText()` /
  `lastErrorCode()` / `lastErrorMessage()`;
- `%package NAME` generates a file-level `namespace NAME;`, and `%import NAME` generates
  `using NAME;` (`using` is always placed before the namespace, which C# syntax requires);
- The target framework is `net8.0` (the generated `parser.csproj` can be edited directly; the template-side value is
  written in `source/templates/csharp_csproj.xml.tmpl`, and the template is purely static with no placeholders), using
  only the standard library.
- `glr` uses `Dictionary<int, int>` to index the graph-structured stack by input position and state; `earley` / `cyk`
  first decide whether the original grammar / its CNF can accept, then rebuild all semantic results or syntax trees of
  the original productions.
  `runGlr` / `runGlrTree` return a status code, and `glrResultCount` with `glrResult(i)` / `glrTree(i)`
  read the individual paths; `error` productions and `%destructor` are not supported, and the chart modes do not
  support `%sync` either.

### 12.2 Lexical: Fixed Literals + .NET Regexes

The lexer of C# artifacts **does not use the generator's built-in DFA**; instead the regexes go as they are to .NET's
`System.Text.RegularExpressions.Regex` — each is constructed once in a static field of `Parser.cs`
(`using System.Text.RegularExpressions;` is added automatically by the generator whenever "the artifacts contain a
lexer"):

| Declaration in the grammar | Table in `Parser.cs` |
| --- | --- |
| `%token regex NAME "正则"` | `_REGEX_RE_LIST` (together with `_REGEX_TYPES` / `_REGEX_ORDERS`) |
| `%identifier "正则"` | `_IDENTIFIER_RE` (`null` when absent, falling back to the built-in identifier rule) |
| `%commit` / `%trivia "模式"` | `_COMMIT_RE_LIST` (together with `_COMMIT_TYPES` / `_COMMIT_SKIPS` / `_COMMIT_ORDERS`) |

The match order is the same as the other targets: built-in strings → fixed literals and the regexes above together,
**taking the longest**; on equal length, literals win, then declaration order; fragments matched by `%commit` do not
enter the grammar level.

Two implementation details:

- `Regex.Match(text, start)` **searches forward from start** and is not anchored at start, so `_lexRegexMatch()` also
  requires `Match.Index == start` for the semantics to match the original DFA;
- Longest matching relies on `Regex`'s greedy semantics (the same convention as Python artifacts), so the two points
  in §10.4 — "`\b` means different things" and "regexes with branches of different lengths take the match in branch
  order" — apply to C# as well (Rust is the same, see §14.4). The DFA construction pitfall recorded in
  `05-grammar.md` — "`(A|B)*` plus a negated class never matches" — **does not exist in C# artifacts either**.

Generation-time validation follows the same convention as the Python target (see the last paragraph of §10.4): when the
grammar is loaded it is checked with `source/src/regex_dfa.cpp`, and **only C99 treats a failure as a hard error**; C#
artifacts hand the regex as-is to .NET's `Regex`, so when the built-in compiler rejects it only a warning is printed
and generation proceeds as usual. The DFA produced by validation is no longer written into C# artifacts.

### 12.3 Line Markers in C# Artifacts

C# has no portable `#line` (`#line` can only change line numbers, not point at another file), so a comment line is
inserted before and after each "code segment coming from the `.gram`":

```csharp
            // 在 example/calc.gram 的第 10 行
            result.value = values[0] + values[2];
            // 继续 Parser.cs 的正文
```

| Content | Landing point | Forward comment points at | Versus the C `#line` |
| --- | --- | --- | --- |
| Semantic action | The action branch of `Parser.cs` | The line of the action's `{` | Line numbers identical |
| `%code ... %endcode` | The file-level user code area | The first line of the body | Identical |
| `%class_body` body | Inside the parser class | The line after `%class_body` | Identical |
| `%epilogue` body | The end of the artifact file | The line after `%epilogue` | Identical |
| `%main` body | `Program.cs` | The line after `%main` | Identical |

The backward comment is `// 继续 <artifact file> 的正文`; the comment line still ends with a newline and its indentation
follows the enclosing block, so it does not affect compilation. It is navigation for humans only: C# compile errors
still report the artifact's own line numbers.

## 13. Grammar Export (`--lang gram`)

This target generates no code and has no callable API: the output directory contains only the original grammar file,
its analysis report, and `RESULT.md`.

| File | Contents |
| --- | --- |
| `<original file name>.gram` | The `.gram` that was the input, exported **byte for byte** (`%if` not expanded, comments and blank lines all present); when no file is given it is the built-in `default_grammar.gram`, exported as `grammar.gram` |
| `<grammar name>.analysis.md` | Analysis report: source grammar / exported file / generation mode + conflict resolution + unresolved conflicts + the full text of the `--report` report (`pl0.gram` → `pl0.analysis.md`) |
| `RESULT.md` | Generation info (source grammar / exported file / analysis report name / generation mode) + product file list + "how to use" |
| Files from `%copy` | Copied into the output directory as before (none if there are none) |

In one line:

```sh
ParseEngine example/pl0.gram --out-dir out --lang gram     # 只要文法 + 报告
ls out                       # pl0.gram  pl0.analysis.md  RESULT.md
```

The body of `<grammar name>.analysis.md` is the same as what `--report FILE` writes out (conflicts /
unresolved conflicts / grammar analysis report), so there is **no need** to store a separate `--report`; changing
`--mode` to `lalr` / `ll1` / `lex` gives another analysis (under `lex` there are no parse tables, and the report has a
lexical section + token table). `%namespace` / `%class` / `%package` / `%import` / `%class_body` /
`%destructor` / `%cpp_only` are all merely noted and ignored for this target — no code is generated, so they have no
landing point. See [`07-codegen.md`](../07-codegen.md) §13 for details.

## 14. Rust Output Artifacts (`--lang rust`)

The artifacts are a **Cargo project**: `Cargo.toml` + `src/parser.rs` (lexer + parse tables + runtime
+ public API) + `src/token.rs` (token type constants + lexical unit) + `src/main.rs` (demo entry point)
+ `RESULT.md`. Lexical mode uses the same files, except that `parser.rs` contains only the lexer and `tokenize`.

```sh
cargo build
printf '1+2*3' | cargo run --quiet     # 演示程序从标准输入读一行
```

```rust
let mut engine = Parser::new();
let status = engine.run_text("1+2*3");
if status == parser::STATUS_OK
{
    println!("{:?}", engine.result());
}
else
{
    println!("{:?}", engine.last_error());
}
```

**Only the standard library is used, with no third-party dependency** (`cargo build --offline` succeeds): parse tables
are written as array constants such as `static _ACTION: &[&[i32]]`, and the runtime only does table lookups.

### 14.1 Semantic Values: Copy by Value

Rust semantic values are **copied by value** on the parse stack, so the semantic value type must satisfy
`Copy + Default + Debug` at the same time:

| How it is written | Type in the artifacts |
| --- | --- |
| Neither written | `i64` (numeric tokens give an integer, other tokens give `0`) |
| `%value_type T` | `T` (copied into the artifacts as-is; you need to `#[derive(Clone, Copy, Default, Debug)]` yourself) |
| `%union { ival: i32, dval: f64 }` | `pub struct ParserValue { pub ival: i32, pub dval: f64 }`, with `#[derive(Clone, Copy, Default, Debug)]` built in |

**Members must be written the Rust way** (`ival: i32`), not as a C declaration (`int ival;`) —
so C grammars such as `example/union_demo.gram` cannot produce Rust artifacts directly, and they are not listed in the
smoke-test checklist. **Non-`Copy`** types such as `String` / `Vec<T>` / `Box<T>` are not applicable (compilation
reports `the trait bound ... Copy ... is not satisfied`).

### 14.2 Semantic Actions and Dialect

Actions are translated as Rust expressions (`ActionTranslator`'s `rustStyle`):

| How it is written | Expands to |
| --- | --- |
| `$$` | `result.value` (`result.value.<member>` when `%union` is written) |
| `$1` | `values[0]` for LR reductions, `values[1]` for LL expansion (same as Python / C#) |
| `yyerrok` / `yyclearin` | `self.yy_err_ok();` / `self.yy_clearin();` |
| `$line` / `$column` / `$token` / `$code` / `$message` | `self.current_line()` / `self.current_column()` / `self.current_token_text()` / `self.last_error_code()` / `self.last_error_message()` |
| Type constructors such as `int32(x)` | `((x) as i32)` (Rust has no C-style cast) |

Statements end with a semicolon, and the action code is written inside `impl Parser` (so the body of `%class_body` is
inserted as an `impl Parser { … }` block, `%package` becomes the crate name in `Cargo.toml`, and `%import` becomes
`use`).

### 14.3 Boundaries

- **`%destructor` is not supported**: semantic values are copied by value and reclaimed automatically by ownership
  when discarded; there is no "call cleanup code when discarding" layer. Writing it in the grammar reports an error
  directly at generation time (`%lang rust` or `--lang rust`).
- **There is no portable `#line`**: errors only give `src/parser.rs`'s own line numbers, so a comment line is inserted
  before and after each "code segment coming from the .gram" — `// 在 <grammar file> 的第 <N> 行` says where it came
  from, and `// 继续 parser.rs 的正文` says that the artifact's own body resumes.
- Nested `%union` is not done, and no complex cross-language type mapping is done.

### 14.4 Lexical: Fixed Literals + the Official `regex`

The lexer of Rust artifacts **does not use the generator's built-in DFA**; instead the regexes go as they are to Rust's
official [`regex`](https://docs.rs/regex) crate (`regex = "1"` in `Cargo.toml`, and
`use regex::Regex;` / `use std::sync::OnceLock;` at the top of `src/parser.rs`):

| Declaration in the grammar | Table in `src/parser.rs` |
| --- | --- |
| `%token regex NAME "正则"` | `_REGEX_RES` (`OnceLock<Vec<Regex>>`, together with `_REGEX_TYPES` / `_REGEX_ORDERS`) |
| `%identifier "正则"` | `_IDENTIFIER_RE` (`OnceLock<Regex>`; not generated when absent, falling back to the built-in identifier rule) |
| `%commit` / `%trivia "模式"` | `_COMMIT_RES` (`OnceLock<Vec<Regex>>`, together with `_COMMIT_TYPES` / `_COMMIT_SKIPS` / `_COMMIT_ORDERS`) |

The match order is the same as the other targets: built-in strings → fixed literals and the regexes above together,
**taking the longest**; on equal length, literals win, then declaration order; fragments matched by `%commit` do not
enter the grammar level.

Two implementation details:

- Regexes are **lazily loaded**: each table is compiled with `Regex::new` the first time it is used (`OnceLock`) and
  reused thereafter; if compilation fails, `lex_compile_regex()` panics with "正则 xxx 编译失败：…", which beats
  hunting for ages over "the lexeme does not match";
- `Regex::find_at(text, start)` **searches forward from start** and is not anchored at start, so
  `lex_regex_match()` also requires `m.start() == start`, and what it returns is a **character count** (`regex`
  locates by byte, while the lexical tables are indexed by `Vec<char>`) — only then do the semantics match the original
  DFA. Longest matching relies on `regex`'s greedy semantics (the same convention as C++17 / Python / C#), so the two
  points in §10.4 — "`\b` means different things" and "regexes with branches of different lengths take the match in
  branch order" — apply to Rust as well, and the DFA construction pitfall recorded in
  `05-grammar.md` **does not exist in Rust artifacts either**.

Generation-time validation follows the same convention as the Python target (see the last paragraph of §10.4): when the
grammar is loaded it is checked with `source/src/regex_dfa.cpp`, and **only C99 treats a failure as a hard error**; Rust
artifacts hand the regex as-is to `regex`, so when the built-in compiler rejects it only a warning is printed and
generation proceeds as usual.

Fragments collected by `%commit` and their location info go through `ParserCommit` / `commits()` and
`ParserLocation` / `current_location()`.

> Rust 1.70+ is required (the stable version of `std::sync::OnceLock`), and `regex` is the **only
> third-party dependency** — the first `cargo build` needs network access; afterwards `cargo build --offline` works.

### 14.5 One Hard Rule for the Generator Implementation

`source/src/code_gen_rust.cpp` is the **exemplar of "all artifact text goes into templates"**: there is not a single
`replaceAll` in it, and not one `out += "…"` that assembles artifacts; the C++ side only does "compute tables /
compute flag bits / escape string literals";
all wording and layout (including the separators and indentation of tables, and the indentation of multi-line bodies)
are decided by 46 `source/templates/rust_*.tmpl` files.

## 15. Go Output Artifacts (`--lang go`)

The artifacts are a **Go module**: `go.mod` + `parser.go` (lexer + parse tables + runtime + public API)
+ `token.go` (token type constants + lexical unit) + `main.go` (demo entry point) + `RESULT.md`.
All three `.go` files are `package main` (one package, using each other's symbols directly); lexical mode uses the
same files, except that `parser.go` contains only the lexer and `Tokenize`.

```sh
go build
printf '1+2*3' | go run .     # 演示程序从标准输入读一行
```

```go
engine := NewParser()
status := engine.RunText("1+2*3")
if status == StatusOK {
	fmt.Println(engine.Result())
} else {
	fmt.Println(engine.LastError())
}
```

**Only the standard library is used, with no third-party dependency**: parse tables are written as package-level
arrays such as `var sParserAction = [][]int32{…}`, and the runtime only does table lookups; internal helper symbols
uniformly carry the `sParser` prefix, and the public API uses exported Go spelling
(`Parser` / `Token` / `NewParser` / `RunText` / `Result`).

| File | Contents |
| --- | --- |
| `go.mod` | `module <path>` + `go 1.21`; the module path comes from `%package` (`parserdemo` by default) |
| `parser.go` | Status codes, capacity constants, token table, parse tables, `Parser` and all its methods, lexer, with `%epilogue` appended at the end of `parser.go` |
| `token.go` | `TOKEN_<name>` constants, `TOKEN_NAMES`, `TOKEN_ALIASES`, `Token` and its constructor / `TokenName` / `TokenText` |
| `main.go` | Entry point: the built-in demo main (reads one line from standard input) or the `%main` body (the body supplies its own `import`) |
| `RESULT.md` | Generation info, product file list, location info and the `%commit` table, build instructions |

### 15.1 Public API

| Content | Spelling |
| --- | --- |
| Status codes | `StatusOK`(0) / `StatusErrStack`(-20) / `StatusErrSyntax`(-21) / `StatusErrLexer`(-22) / `StatusErrExpected`(-26) / `StatusErrUnexpectedEOF`(-27) / `StatusErrLexUnterminated`(-28) / `StatusErrLexChar`(-29)… (values consistent with the other targets); get the name with `StatusName(status)` |
| Construction and entry points | `NewParser()`; `RunText(text string) int32`, `Run(tokens []Token) int32`, `ParseTree(text string) *Tree` (builds the tree only, without running actions) |
| Results and errors | `Result()`, `Errors() []*ParserError`, `LastError() *ParserError` (`ParserError` implements the `error` interface, and its text is "第 N 行第 M 列：…") |
| Location and `%commit` | `CurrentLocation() ParserLocation`, `Commits() []ParserCommit` |
| Token | `Token{Type int32; Text string; Value <semantic value type>; Line, Column, Offset int32}`, `TokenName(kind)`, `TokenText(item)` |
| Lexical mode | `Tokenize(text string) (int32, []Token)` (consistent with the other targets, with a trailing `TOKEN_END`) |

The same `Parser` instance can parse several times in a row: every `RunText` / `Run` / `ParseTree` first clears the
previous error table and result.

### 15.2 Semantic Values: Copy by Value

| How it is written | Type in the artifacts |
| --- | --- |
| Neither written | `int64` (numeric tokens give an integer, other tokens give the zero value) |
| `%value_type T` | `T` (copied into the artifacts as-is; it must support zero-value initialization and assignment) |
| `%union { int64 ival; }` | `type ParserValue struct { ival int64 }` |

**Members are written as C declarations** (`int64 ival;`), and the generator assembles them into Go field spelling
(`ival int64`) — this is consistent with C / C#, and it is the Rust target that uses the `ival: i32` style. Semantic
values are copied by value on the parse stack, and scalars such as `int64`, structs such as `ParserValue`, and
reference types such as slices / maps all work (copying a reference type copies the reference, so in actions watch out
for several slots sharing the same underlying data).

The conversion from lexeme to semantic value is concentrated in two generated functions: `valueFromText(kind, text)`
and `valueFromNumber(number float64, isFloat bool)`.

### 15.3 Semantic Actions and Dialect

Actions are translated as Go expressions (`ActionTranslator`'s `goStyle`):

| How it is written | Expands to |
| --- | --- |
| `$$` | `result.value` (`result.value.<member>` when `%union` is written) |
| `$1` | `values[0]` for LR reductions, `values[1]` for LL expansion (same as Python / C# / Rust) |
| `yyerrok` / `yyclearin` | `p.YyErrOk()` / `p.YyClearIn()` (the action code is written in a method on `*Parser`, with the receiver always named `p`) |
| `$line` / `$column` / `$token` / `$code` / `$message` | `p.CurrentLine()` / `p.CurrentColumn()` / `p.CurrentTokenText()` / `p.LastErrorCode()` / `p.LastErrorMessage()` |
| Type constructors such as `int32(x)` | `int32((x))` (Go's conversions are written in function style) |

Statements end with a semicolon; `%package` becomes the module path in `go.mod`, `%import` goes into the import block,
and the body of `%class_body` is pasted as a set of `func (p *Parser) …` methods (Go has no class layer).

### 15.4 Boundaries

- **`%destructor` is not supported**: semantic values are reclaimed by the GC, and there is no "call cleanup code when
  discarding" layer; writing it in the grammar reports an error directly at generation time (`%lang go` or
  `--lang go`).
- **There is no portable `#line`**: errors only give `parser.go`'s own line numbers, so a comment line is inserted
  before and after each "code segment coming from the .gram" — `// 在 <grammar file> 的第 <N> 行` says where it came
  from, and `// 继续 parser.go 的正文` says that the artifact's own body resumes (the same approach as the Rust
  target).
- The body of `%code ... %endcode` is inserted after the import block, so **do not write another `import` in the
  body** (use `%import` to add packages); the `%main` body must supply its own `import` and `func main()`
  (the shell only provides the file header comment and `package main`).
- In `%import` names, `::` may be used as a path separator (normalized to `/`), and packages duplicated with the
  standard library are skipped (importing the same package twice in Go is a compile error); nested `%union` is not
  done, and no complex cross-language type mapping is done.

### 15.5 Lexical: Fixed Literals + Standard-Library `regexp`

The lexer of Go artifacts **does not use the generator's built-in DFA**; instead the regexes go as they are to Go's
standard-library [`regexp`](https://pkg.go.dev/regexp) (the RE2 implementation, with each `regexp.MustCompile`
compiled once at package-level variable initialization):

| Declaration in the grammar | Table in `parser.go` |
| --- | --- |
| `%token regex NAME "正则"` | `sParserRegexRes` (`[]*regexp.Regexp`, together with `sParserRegexTypes` / `sParserRegexOrders`) |
| `%identifier "正则"` | `sParserIdentifierRe` (not generated when absent, falling back to the built-in identifier rule) |
| `%commit` / `%trivia "模式"` | `sParserCommitRes`, together with `sParserCommitTypes` / `sParserCommitSkips` / `sParserCommitOrders` |

The match order is the same as the other targets: built-in strings → fixed literals and the regexes above together,
**taking the longest**; on equal length, literals win, then declaration order; fragments matched by `%commit` do not
enter the grammar level (`skip` / `%trivia` hits are dropped immediately, and the rest enter `Commits()` in order of
appearance).

Two implementation details:

- `regexp`'s `FindStringIndex` **searches forward from the start point** and is not anchored, so
  `sParserLexRegexMatch()` also requires the match to really begin at the start point; what it returns is a
  **character count** (`regexp` locates by byte, while the lexical tables are indexed by `[]rune`) — only then do the
  semantics match the original DFA. Longest matching relies on `regexp`'s greedy semantics, so the two points in §10.4
  — "`\b` means different things" and "regexes with branches of different lengths take the match in branch order" —
  apply to Go as well.
- `regexp` is imported only when there are regexes (Go treats "imported and not used" as a hard error), so artifacts
  with "no regex written at all" contain neither the regex tables nor the `regexp` line.

Generation-time validation follows the same convention as the Python / Rust targets (see the last paragraph of §10.4):
when the grammar is loaded it is checked with `source/src/regex_dfa.cpp`, and **only C99 treats a failure as a hard
error**; Go artifacts hand the regex as-is to `regexp`, so when the built-in compiler rejects it only a warning is
printed and generation proceeds as usual.

### 15.6 Generator Implementation

`source/src/code_gen_go.cpp` follows the same approach as the Rust target: there is not a single `replaceAll` and no
`out += "…"` that assembles artifacts; artifact fragments always go through template rendering (the generator only
does "compute tables / compute flag bits / escape string literals"); all wording and layout are decided by 53
`source/templates/go_*.tmpl` files.

### C++17 LR Reduction Line Hook

Define `PARSER_USER_REDUCTION_LINE(line)` in the user header, and the C++17 LR driver will compile a symbol start-line
stack on demand and call the macro before each semantic action. Non-empty productions use the start line of the first
right-hand-side symbol; empty productions use the current token line. When the macro is undefined, no position-stack
overhead is added. Magel uses it to record post-preprocessing line numbers on syntax nodes and then restores the
original file and line through the preprocessing source map. This hook is for ordinary LR text analysis; source
persistence for LL/GLR and incremental checkpoints is outside the scope of this interface.
