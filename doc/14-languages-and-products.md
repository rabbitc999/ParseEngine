# 11 目标语言、产物与 API（用户手册）

本章从 [`README.md`](../README.md) 拆出：八种目标语言（C99 / C++17 / Python / C# / Rust / Go / Java / JS-TS）与
「只要文法与分析报告」的导出档 `--lang gram` 的选择与
产物形态、`%namespace` / `%cpp_only` / `%class` 几个语言相关开关、生成产物清单、生成代码的 API
与内存管理。

语言后端的实现与模板见 [`07-codegen.md`](07-codegen.md)，各档产物的 API 细节见
[`08-generated-code.md`](08-generated-code.md)，词法模式的产物见 [`13-modes.md`](13-modes.md)。
各语言可用模式及增量 API 的统一矩阵见 [12 各语言支持的模式](15-language-mode-support.md)。

## 目标语言（C99 / C++17 / Python / C# / Rust / Go / Java / JS-TS）

同一套分析结果可以产出八种语言的工程，由三个来源决定，优先级从高到低：

1. 命令行 `--lang {c,cpp17,python,csharp,rust,go,java,js,gram}`；
2. 文法里的 `%lang {c,cpp17,python,csharp,rust,go,java,js,gram}`；
3. 都没有时取默认值 `cpp17`。

也就是说 `--lang` 覆盖 `%lang`（和 `--mode` 覆盖 `%mode` 的策略一致），不报错。
`%mode` 与 `--mode` 不一致时同样以命令行为准。

C++17 产物还多一个开关：`%namespace NAME` / `--namespace NAME`（命令行优先）决定
命名空间名与文件名前缀，见下面的[换掉命名空间与文件名前缀](#换掉命名空间与文件名前缀namespace----namespace)。

C99 产物可用 `%prefix NAME` / `--prefix NAME`（命令行优先）替换公开符号前缀；默认是 `Parser`，只影响 C99。
不写就是默认的 `Parser` + `parser.*`。

### 各档产物的形态

C++17 产物**不是**把 C 产物塞进 `.cpp`，而是另一套模板（`source/templates/cpp17_*`）与
另一个后端（`source/src/code_gen_cpp17.cpp`）生成的：文件不同、API 也不同。

| | C99 产物（`--lang c`） | C++17 产物（缺省） |
| --- | --- | --- |
| 文件 | `parser.h` / `parser.c` / `parser_token.h` / `parser_token.c` / `parser_table.c` / `main.c` / `parser_user.h` | `parser.hpp` / `parser.cpp` / `main.cpp` / `parser_user.h` |
| 解析入口 | `Parser_RunText(text, &result)` | `Engine engine; engine.runText(text, &result)` |
| 解析结果 | `Parser_Run(tokens, count, &result)` | `engine.run(tokens, &result)` |
| 错误 | `Parser_GetLastError()` / `Parser_GetError(i)` | `engine.lastError()` / `engine.errors()` |
| 错误结构 | `ParserError_T`（C 结构体） | `class Error`（`toString()` 直接出「第 N 行第 M 列：…」） |
| 语义值 | `ParserValue_T` | 同名，但要求是值语义类型（POD 或 RAII） |
| 语法树 | `ParserTree_T` + `Parser_TreePrint` / `Parser_TreeFree` | `class Tree`（`std::unique_ptr` 持有，`tree->print()`） |
| 输入 | `const char *`（NUL 结尾） | `std::string_view` |
| 内存 | 自定义钩子：`PARSER_USER_DISCARD_VALUE` 释放、`PARSER_MEMSET` / `PARSER_MEMCPY` 换掉 memset / memcpy；树走 malloc / free | 全部标准库，**不支持自定义分配器** |
| 内部结构 | 生成器产出的结构体 + 数组表 | 命名空间里的 `class Engine`（d-pointer）+ `inline constexpr` 常量 |

第三种是 `--lang python`：产物只有 `parser.py` / `main.py`，不编译、不生成
`Makefile`，解析器是一个普通 Python 类（`%class` / `--class` 改类名），语义值是
任意 Python 对象——详细对照见下面的 [Python 产物](#python-产物给文法做快速分析--lang-python)。

第四种是 `--lang csharp`：产物是 `Parser.cs` / `Token.cs` / `Program.cs` /
`parser.csproj`——详细对照见下面的 [C# 产物](#c-产物--lang-csharp)。

第五种是 `--lang rust`：产物是一个 Cargo 工程（`Cargo.toml` +
`src/parser.rs` / `src/token.rs` / `src/main.rs`），计算全部用标准库、**词法正则用 Rust
官方的 `regex` crate**（`Cargo.toml` 里一条 `regex = "1"`，第一次 `cargo build` 要联网拉），
可以 `cargo build`——详细对照见下面的 [Rust 产物](#rust-产物--lang-rust)。

第六种是 `--lang go`：产物是一个 Go 模块（`go.mod` + `parser.go` / `token.go` /
`main.go`），**只用标准库**（词法正则用标准库的 `regexp`），`go build` / `go run .`
即可——详细对照见下面的 [Go 产物](#go-产物--lang-go)。

第七、八种是 DD/Earley/CYK 专用窄后端。`--lang java` 生成自包含 `Parser.java`，用
`javac -encoding UTF-8 Parser.java` 编译；`--lang js` 生成 `parser.ts` 与 `package.json`，
代码带可擦除 TypeScript 类型标注，可由 Node.js 22+ 直接执行。两档都支持 `%tree`：
DD 按产生式声明顺序回溯，Earley 执行预测、扫描、完成并保留第一棵派生树；CYK
使用 CNF 跨度表判定，再按原文法恢复第一棵派生树。当前不支持
语义动作、`%value_type` / `%union`、`%sync`、`%commit`、`%copy` 或用户代码段；其他模式
会在生成阶段明确拒绝，完整边界见 [支持矩阵](15-language-mode-support.md)。

C / C++17 / Python 三档的**语义**一致：同一份文法、同一份输入，输出（含错误消息
文本）逐字相同（C# 在 Windows 上是平台 CRLF 行尾，
比对前统一归一化）：`example/` 里的语法类文法里，36 份在 C99 与 C++17
两种目标下都能编译、跑出同样结果（`namespace_demo.gram` 只出 C++17、
`prefix_demo.gram` 只出 C99），其中不写
C 专属语义值写法的（如 `example/python_demo.gram`）在 Python 下也一样；词法模式
示例（`example/lexer_only.gram`）六档都能出词法器（C# 的 Token 类型名与
行列号与 C 一致；Rust / Go 的行列号与 C 一致、词法正则分别走官方的 `regex` 与标准库的
`regexp`）。

`main.cpp` 里在最后一个 `#include` 之后会插一句 `using namespace Parser;`，
所以演示程序里写 C 名字的 `Parser_RunText(...)` 也能用——C++17 产物在命名空间里
自带一层薄薄的兼容层（`PARSER_OK` / `ParserError_T` / `Parser_RunText` /
`Parser_GetLastError` 这些名字都在），底下包着一个进程级共享的 `Engine`
（**非线程安全**）。新代码建议直接用 `Engine`。

用户文件不动：`%code ... %endcode` 生成的 `parser_user.h`、`%copy` 拷贝进来的 `.h` / `.c`
在 C99 / C++17 两种产物里都保持原文件名与原内容；C++17 产物的 `#include` 也指向 `parser_user.h`
这个名字（`%namespace` 时是 `<前缀>_parser_user.h`）。`%copy` 进来的 `.c` 文件在
C++17 工程里由产物的 `Makefile` / `CMakeLists.txt` 用 C 规则单独编译，C 与 C++ 混编。
Python 产物没有 `parser_user.h`（`%code ... %endcode` 的正文插入 `parser.py`），
`%copy` 进来的文件照样原样拷进产物目录。

### 换掉命名空间与文件名前缀：`%namespace` / `--namespace`

上面的 `Parser` 与 `parser.*` 是默认值。写 `%namespace myapp`（或命令行
`--namespace myapp`，命令行优先）会把 C++17 产物的命名空间名与文件名前缀一起换掉：

| 默认（不写 `%namespace`） | `%namespace myapp` |
| --- | --- |
| `namespace Parser { ... }` | `namespace myapp { ... }` |
| `Parser::Engine::runText` / `Parser::Token` | `myapp::Engine::runText` / `myapp::Token` |
| `parser.hpp` / `parser.cpp` | `myapp_parser.hpp` / `myapp_parser.cpp` |
| `main.cpp` / `parser_user.h` | `myapp_main.cpp` / `myapp_parser_user.h` |
| `using namespace Parser;`（main.cpp） | `using namespace myapp;` |

- 名字必须匹配 `[A-Za-z_][A-Za-z0-9_]*`、不能是 C++ 关键字、不支持多级（`my::lang` 报错）；
- `Makefile` / `CMakeLists.txt` / `RESULT.md` 不加前缀（它们是构建脚本与文档），
  `%copy` 拷进来的文件保持原名，`libparser.a` 与 `parser_demo` 这两个目标名也不变；
- 头文件保护宏会按产物名生成（`MYAPP_PARSER_HPP`），所以两份产物的头可以同时 include；
- **C99 产物用不了它**：`%namespace` / `--namespace` 配 `--lang c` 会直接报错
  （C 产物没有命名空间、文件名也不带前缀，静默忽略只会让人以为生效了）；
- 空名字（`--namespace ""`）或不写 = 退化成上面那张默认表。

多实例共存（C++ 下同时链接两个解析器）就是这个功能的用途：

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

注意两点：`%code ... %endcode` 与 `%epilogue` 里的**用户代码仍在全局作用域**（那是用户自己的
代码，生成器一个字都不改写），多实例时要写成 `static` / `inline` 或自己加命名空间；
兼容层里的 `Parser_RunText` 等价入口与 `PARSER_*` 常量在同一个命名空间里，所以两份
产物同时 include 时不会互相冲突（它们各自带命名空间）。

示例 [`example/namespace_demo.gram`](../example/namespace_demo.gram)：`alpha` / `beta`
两份产物可以同时 include、同时链接，各跑出各的结果（头文件保护宏按前缀生成，
否则第二份会被同名保护宏挡掉）。

### 只能出 C++17 的文法：`%cpp_only`

文法里写了 `%cpp_only` 时，命令行再写 `--lang c`（或文法里写 `%lang c`）会直接报错，
提示这份文法只能生成 C++17 产物。适合「动作里调了 `Engine` / 用了 `std::string_view`」
这类注定编不过 C 的文法：早报错好过让人对着 C 编译器的报错猜原因。

### `%commit` / `%trivia`：六档都支持

`%commit`（把注释这类「识别但不进语法层」的文本收进容器）是**跨语言**能力，
`%trivia`（匹配即丢的显式写法，与 `%commit skip` 完全等价）走同一套词法表；
**六档（C99 / C++17 / Python / C# / Rust / Go）都支持**；匹配顺序、容器
内容（行号、类型、原文）与输出逐字一致（C# / Rust 在 Windows 上是平台
CRLF 行尾）：

| 目标语言 | 容器元素 | 解析器暴露 | 备注 |
| --- | --- | --- | --- |
| C99 | `ParserCommit { int line; const char *type; char *text; }` | `Parser_GetCommits()` / `Parser_FreeCommits()` | `text` 是堆内存，`type` 指向静态字符串（不要 free），谁分配谁释放见 [`08-generated-code.md`](08-generated-code.md) |
| C++17 | `ParserCommit { int line; std::string type; std::string text; }` | `Engine::commits()` | 内存由标准库管理 |
| Python | `ParserCommit`（namedtuple：`line` / `type` / `text`） | `engine.commits` | 免编译，直接当库用 |
| C# | `Parser.Commit { int Line; string Type; string Text; }` | `Parser.Commits` | `Text` 是 `string`，UTF-16 |
| Rust | `ParserCommit { line: i32, kind: String, text: String }` | `engine.commits()` | `kind` 就是类型名 |
| Go | `ParserCommit { Line int32; Type string; Text string }` | `engine.Commits()` | `Type` 就是类型名；值是值拷贝 |

位置信息 API 也是六档都有（C99 `Parser_CurrentLocation()`、C++17
`Engine::currentLocation()`、Python `engine.current_location()`、C#
`CurrentLocation`、Rust `engine.current_location()`、Go `engine.CurrentLocation()`），
`offset` 一律按 UTF-8 字节算。

同一个模式不能既当 `%token` 又当 `%commit` / `%trivia`（`%trivia` 与
`%commit skip` 等价，两种写法生成的词法表逐字相同）；匹配与 `%token` /
内建注释规则共用一份最长匹配顺序（同长度按声明先后）。用法与完整例子见
[`05-grammar.md`](05-grammar.md) 与 [`12-grammar-usage.md`](12-grammar-usage.md)，
生成的 API 细节见 [`08-generated-code.md`](08-generated-code.md)。

Token 和 commit 类型还支持可选显示别名：

```gram
%token STRING "..." alias "字符串"
%commit block "/\\*...\\*/" alias "块注释"
```

同一规则也适用于无字面量的 `%token A B C alias "显示名"`、
`%identifier "正则" alias "标识符"` 和 `%trivia "模式" alias "空白注释"`。
其中 `%identifier` 的别名对应内建 `VARIABLE`；`%trivia` 的别名只出现在类型表，
因为 trivia 本身不会进入 commit 容器。

六档生成器都保留原始名称和别名。C99/C++17 使用两列静态名称表，其他语言使用
名称到别名的字典；别名只影响 `token_name` 等诊断显示（格式为
`STRING(字符串)`），不改变 Token 数值、语法符号或 `ParserCommit.type/kind`。
未写别名时不追加括号。

### Python 产物：给文法做快速分析（`--lang python`）

`--lang python` / `%lang python` 出的是另一套产物，只有两个文件、**不需要编译**，
适合「先看看这份文法到底能不能跑」这种快速验证：

| | Python 产物（`--lang python`） |
| --- | --- |
| 文件 | `parser.py`（词法器 + 分析驱动 + 公开 API）/ `main.py`（演示入口）/ `RESULT.md` |
| 类名 | `%class NAME` / `--class NAME`（命令行优先；缺省 `Parser`） |
| 运行 | `python3 main.py`（演示程序从标准输入读一行），或 `import parser` 当库用 |
| 解析入口 | `engine.run_text(text)` / `engine.run(tokens)` / `engine.parse_tree(text)` |
| 结果 | `engine.result`（任意 Python 对象），失败原因读 `engine.last_error` / `engine.errors` |
| 语法树 | `class Tree`（`name` + `text` + `children`，`tree.print()` 出缩进文本） |
| 用户代码 | `%code ... %endcode` 插在 `parser.py` 的 import 之后（缩进自动归一化）；`%epilogue` 贴到 `parser.py` 末尾；`%main` 替换 `main.py` 的演示体 |
| 内存 | 全部由 Python 自己管，没有分配器 / 释放钩子 |

```sh
./build/bin/ParseEngine --grammar-file example/python_demo.gram --out-dir out --lang python
printf '1+2*3' | python3 out/main.py
```

#### 给 AI / 脚本当接口：省上下文

Python 产物还有一层用处——让 AI（Codex、Claude Code 这类带终端的编码代理）**自己
跑命令拿结果**，而不是把文法、产物或日志读进上下文：`printf '...' | python3
main.py` 只回两三行；要结构化的结果就 `python3 -c "import parser; e =
parser.Parser(); e.run_text(open('in.txt').read()); print(e.result, e.errors)"`，
回一行。于是「改文法 → 重新生成 → 看结果」这个循环可以在很小的上下文里转很多圈，
不用 AI 连续阅读源码与长日志，也不必把整份输入 / 输出贴进对话。

产物只用标准库、不需要编译，所以任何有 `python3` 的地方（CI、容器、AI 的沙箱）
都能直接跑——验证一份文法不必先配好 C 工具链。

语义值在 Python 里是**任意对象**，动作直接写 Python 表达式：

```text
expr := expr PLUS term { $$ = $1 + $3; }     # $1 -> values[0]，$$ -> result.value
      | term           { $$ = $1; };
```

词法器给出的 Token 语义值：字符串 Token 是去掉引号并解好转义的 `str`，数字 Token
是 `int` / `float`，标识符与固定字面量是词素文本（例如 `+`），EOF 与 `error` 伪 Token
是 `None`。`result` 不是 `None` 时演示程序会多打一行 `解析结果: <repr>`。

`%code ... %endcode` 里的辅助函数 / 常量会插在 `parser.py` 的
`import` 之后、生成的常量之前，动作里可以直接调用（`example/python_demo.gram`
里就在动作中调了 `%code ... %endcode` 定义的 `py_value`）；
正文按 Python 的缩进规则归一化——去掉所有非空行的**公共缩进**、把行首 tab 换成
4 个空格，所以照文法排版缩进着写也不会变成 `IndentationError`。`%epilogue`
（贴到 `parser.py` 末尾）与 `%main`（替换 `main.py` 的演示体）做同样的归一化。

因为语义值是 Python 对象，Python 产物**不支持**这些 C 专属写法，写了会直接报错
（不静默忽略）：

- `%value_type`、`%union`、`%type <成员>`、`%token <成员>`；
- `%namespace` / `--namespace`（Python 没有命名空间，改类名用 `%class` / `--class`）。

其余指令都能用：`%mode`（ll1 / lr0 / slr / lalr / lr1 / ielr / lrk / lalrk）、
`%lookahead`、`%start`、
`%define` / `%token` / `%token regex` / `%identifier`、`%tree`、`%sync`、
`error` 产生式与 `yyerrok` / `yyclearin`、`%max_*`、`%expect`、
`%left` / `%right` / `%nonassoc` / `%prec`、`%teach`、`%mermaid`、`%copy`、`%if`、
`%main` / `%epilogue`、`%code ... %endcode`。同一份文法（不含上面那几条 C 专属写法时）四份产物在同样输入
下的输出逐字一致，包括错误消息、错误码、行列号与 `%tree` 的树形文本
（C# 在 Windows 上写出的行尾是平台 CRLF，比对前统一归一化）。

`%cpp_only` 声明「只能出 C++17」的文法，配 `--lang python`（或 `--lang c`）一样报错。

### C# 产物（`--lang csharp`）

C# 是和 C99 / C++17 / Python / Rust / Go 并列的一档产物，有自己的模板
（`source/templates/csharp_*`）与后端（`source/src/code_gen_csharp.cpp`）。**所有生成模式都覆盖**：LL 家族（ll0 / ll1 / llk）
走同一套 LL 运行时、LR 家族（lr0 / slr / lalr / lr1 / ielr / lrk / lalrk /
slrk）走同一套 LR 运行时；`dd` 走回溯递归下降，`glr` 用 `Dictionary` 索引
图结构栈，`earley` / `cyk` 先做图表判定再由 GLR 重建结果；`lex` 只出词法器。

```sh
# C#：dotnet build + dotnet run（目标框架 net8.0，只用标准库）
./build/bin/ParseEngine example/json.gram --out-dir /tmp/cs --lang csharp
cd /tmp/cs && dotnet build && printf '{"a": [1, 2]}' | dotnet run
```

| | C# 产物 |
| --- | --- |
| 文件 | `Parser.cs` / `Token.cs` / `Program.cs` / `parser.csproj` / `RESULT.md` |
| 入口 | `new Parser().runText("...")` → `STATUS_*` |
| 错误 | `errors()` / `lastError()`（`Parser.ParserError`） |
| 语法树 | `Parser.Tree`（`print()`） |
| 歧义结果 | `runGlr(text)` / `runGlrTree(text)`；`glrResultCount()`、`glrResult(i)`、`glrTree(i)`；`setAmbiguityResolver(...)` 可选择保留结果 |
| 语义值 | `%value_type` 原样填；`%union` → 顶层类 `ParserValue`；都没写 → `object` |
| 报错定位 | 五处「来自 `.gram` 的代码段」前后插 `// 在 <文法文件> 的第 <N> 行` 注释（行号与 C 的 `#line` 完全一致） |

三个只对 csharp 生效的指令：`%package`（C# 的 `namespace NAME;`）、`%import`
（C# 的 `using NAME;`，可重复）、`%class_body` … `%endbody`（原样插进解析器类
内部，放辅助方法 / 字段 / 内部类）。
`%code ... %endcode`、`%epilogue`、`%main` 照常用，只是正文要写
**目标语言自己的代码**（生成器一个字都不改写）。

### Rust 产物（`--lang rust`）

Rust 有自己的模板（`source/templates/rust_*`）与后端
（`source/src/code_gen_rust.cpp`，`CodeGenRust`）。**所有生成模式都覆盖**：LL 家族
（ll0 / ll1 / llk）走同一套 LL 运行时、LR 家族（lr0 / slr / lalr / lr1 / ielr /
lrk / lalrk / slrk）走同一套 LR 运行时；`dd` 走回溯递归下降，`glr` 走 GSS，
`earley` / `cyk` 做图表判定后由 GLR 重建结果；`lex` 只出词法器。

```sh
# Rust：cargo build（标准库 + 官方的 regex crate；依赖进过本地缓存后可以加 --offline）
./build/bin/ParseEngine example/json.gram --out-dir /tmp/rs --lang rust
cd /tmp/rs && cargo build && printf '1+2*3' | ./target/debug/parser_demo
```

| | Rust 产物 |
| --- | --- |
| 文件 | `Cargo.toml` / `src/parser.rs` / `src/token.rs` / `src/main.rs` / `RESULT.md` |
| crate 名 | `Cargo.toml` 的 `package.name`，缺省 `parser_demo`（`%package` 改它） |
| 解析入口 | `Parser::new()` + `engine.run_text(&text)` → `STATUS_OK` |
| 错误 | `engine.errors()` / `engine.last_error()`（`ParserError`）；`status_name(status)` 出文本 |
| 语法树 | `engine.parse_tree(&text)` → `Option<Tree>`（`Tree::print()` / `child(i)` / `child_count()`） |
| 位置 | `ParserLocation` / `engine.current_location()`（行 / 列 1 起，`offset` 是 UTF-8 字节偏移） |
| 语义值 | 缺省 `i64`（数字 Token 给整数、其它 Token 给 0）；`%union` → 带 `#[derive(Clone, Copy, Default, Debug)]` 的 `ParserValue` |
| 报错定位 | 与其它档一致：来自 `.gram` 的代码段前后插 `// 在 <文法文件> 的第 <N> 行` 注释 |
| 词法正则 | Rust **官方的 `regex` crate**（`Cargo.toml` 里一个 `regex = "1"`；产物里没有 DFA 表，模式原样交给它，`OnceLock` 里各编译一次）；`%identifier` / `%commit` / `%trivia` 同样走 `regex` |
| 内存 | 语义值按值拷贝，**不支持 `%destructor`**（由所有权回收，写了生成期直接报错） |

**语义值必须满足 `Copy + Default + Debug`**（分析栈上按值拷贝）：标量
（`i64` / `f64` / `bool` / `u8` …）天然满足；`%union` 生成的 `ParserValue` 自带
`derive`，只要成员类型也满足这三条。这是 Rust 档和别的档最大的差别，`RESULT.md`
里会专门写一段。

三个对 rust 生效的指令：`%package`（crate 名）、`%import`（`use NAME;`，可重复）、
`%class_body` … `%endbody`（原样插进解析器实现里，放辅助方法 / 字段 / 内部类型）。
`%code ... %endcode` / `%epilogue` / `%main` 照常用，只是正文要写
**Rust 自己的代码**（生成器一个字都不改写）。

只当解析库用时：去掉 `src/main.rs`，把 `src/parser.rs` / `src/token.rs` 当模块加进
你自己的 crate（产物里的 `%code ... %endcode` 项目都是 `pub`，`use parser::*;` 就能取到）。

### Go 产物（`--lang go`）

Go 有自己的模板（`source/templates/go_*`）与后端（`source/src/code_gen_go.cpp`，
`CodeGenGo`）。**支持全部 16 种模式**：LL 家族（ll0 / ll1 / llk）走同一套 LL 运行时、
LR 家族（lr0 / slr / lalr / lr1 / ielr / lrk / lalrk / slrk）走同一套 LR 运行时、
`dd` 走回溯递归下降，`glr` 走 GSS，`earley` / `cyk` 先做图表判定再由 GSS
按原文法重建全部结果树并执行语义动作，`lex` 只出词法器；产物模板一律走
`library/template` 模板引擎（与 Rust 档同一套做法）。

```sh
# Go：go build（只用标准库，没有第三方依赖）
./build/bin/ParseEngine example/json.gram --out-dir /tmp/go --lang go
cd /tmp/go && go build && printf '1+2*3' | go run .
```

| | Go 产物 |
| --- | --- |
| 文件 | `go.mod` / `parser.go` / `token.go` / `main.go` / `RESULT.md`（三个 `.go` 都是 `package main`） |
| module path | `go.mod` 的 `module`，缺省 `parserdemo`（`%package` 改它，`::` 归一成 `/`） |
| 解析入口 | `NewParser()` + `engine.RunText(text)` → `StatusOK` |
| 错误 | `engine.Errors()` / `engine.LastError()`（`ParserError` 实现 `error`）；`StatusName(status)` 出文本 |
| 语法树 | `engine.ParseTree(text)` → `*Tree`（`tree.Print()` / `tree.Child(i)` / `tree.ChildCount()`） |
| 位置 | `ParserLocation` / `engine.CurrentLocation()`（行 / 列 1 起，`Offset` 是 UTF-8 字节偏移） |
| 语义值 | 缺省 `int64`（数字 Token 给整数、其它 Token 给零值）；`%union` → 带成员字段的 `ParserValue` 结构体（成员照 C 的声明写，如 `int64 ival;`，生成器拼成 Go 字段写法） |
| 报错定位 | 与其它档一致：来自 `.gram` 的代码段前后插 `// 在 <文法文件> 的第 <N> 行` 注释 |
| 词法正则 | Go **标准库的 `regexp`**（RE2；产物里没有 DFA 表，模式原样交给它，包级变量初始化时 `regexp.MustCompile` 各编译一遍）；`%identifier` / `%commit` / `%trivia` 同样走 `regexp` |
| 内存 | 语义值按值拷贝，**不支持 `%destructor`**（由 GC 回收，写了生成期直接报错） |

`%union` 的成员写法与 C / C# 一致（`int64 ival;`），Rust 档才是 `ival: i32` 那种写法；
语义值在分析栈上按值拷贝，标量 / 结构体 / 切片都能用（切片这类引用类型拷贝的是引用）。

三个对 go 生效的指令：`%package`（module path）、`%import`（进 `import ( … )` 块，可重复，
`::` 当路径分隔符、与标准库重复的包会被跳过）、`%class_body` … `%endbody`（原样贴在
解析器代码之后，写成一组 `func (p *Parser) …` 方法——Go 没有 class 这一层）。
`%code ... %endcode` / `%epilogue` / `%main` 照常用，只是正文要写
**Go 自己的代码**：`%code ... %endcode` 的正文插在 import 块之后（**正文里不要再写 `import`**），
`%main` 的正文要自带 `import` 与 `func main()`（外壳只给文件头注释与 `package main`）。

只当解析库用时：去掉 `main.go`，把 `parser.go` / `token.go` 加进你自己的包
（它们都是 `package main`，改成你的包名即可）。

### 文法导出（`--lang gram`）

最后一档不是编程语言，而是**只要文法与分析报告、不要代码**的导出档：产物目录里
只有输入的那份 `.gram`（逐字节导出）、它的分析报告 `<文法名>.analysis.md`
（冲突消解 / 未消解冲突 + 分析报告全文）与 `RESULT.md`（生成信息 + 产物清单 +
怎么用），没有解析器、词法器，也没有构建脚本。

```sh
ParseEngine example/pl0.gram --out-dir out --lang gram
ls out                          # pl0.gram  pl0.analysis.md  RESULT.md
head -n 30 out/pl0.analysis.md  # 冲突消解 / 未消解冲突 / 分析报告
```

| | 文法导出档（`--lang gram`） |
| --- | --- |
| 文件 | `<原文件名>.gram`（没给文件时 `grammar.gram`）/ `<文法名>.analysis.md` / `RESULT.md` / `%copy` 的文件 |
| 模式 | 16 种模式均可导出；`glr` / `earley` / `cyk` 只导出文法与对应分析报告，不生成运行时。`--mode lex` 报告为词法一节 + Token 表，详见 [支持矩阵](15-language-mode-support.md) |
| 绘图 | `gram` 不生成 Python 运行时或 `--svg` / `--dot` 演示程序；Python 产物的绘图另见 [`10`](13-modes.md) |
| 报告 | 分析报告单独成文件、和 `.gram` 成对（`pl0.gram` ↔ `pl0.analysis.md`），正文与 `--report FILE` 写出来的是同一份 |
| 不适用指令 | `%namespace` / `%class` / `%package` / `%import` / `%class_body` / `%destructor` / `%cpp_only` 全部提示一句并忽略 |

用途：把「这份文法 + 当时的分析结果」一起归档 / 提交 / 交接；产物目录里只有生成
出来的源码时，回头想确认「代码是从哪份文法、用哪套分析生成的」就得翻命令行。
细节见 [`07-codegen.md`](07-codegen.md) §14 与 [`08-generated-code.md`](08-generated-code.md) §13。

### 什么时候用哪个

- 想和 C 项目链接、或目标平台只有 C 编译器：`--lang c`。
- 想直接用类语义、和 C++ 工程混编：默认的 `cpp17` 即可；生成的 C++17 代码
  只用标准库与 `<stdint.h>` 等 C 头，不引入第三方依赖。
- 只想快速验证文法（不想装编译器、不想看 C 的类型）：`--lang python`，
  `python3 main.py` 直接跑。
- 想让 AI / 脚本自己跑命令拿解析结果（省上下文、不用把产物读进来）：也是
  `--lang python`，见[上面的「给 AI / 脚本当接口」](#给-ai--脚本当接口省上下文)。
- 和 .NET 工程集成：`--lang csharp`，产物用 `dotnet build` + `dotnet run`。
- 和 Rust 工程集成：`--lang rust`，产物是 Cargo 工程（依赖只有官方的 `regex`），`cargo build` 即可
  （只用标准库，语义值要 `Copy + Default + Debug`，没有 `%destructor`）。
- 和 Go 工程集成：`--lang go`，产物是一个 Go 模块，`go build` / `go run .` 即可
  （只用标准库，语义值按值拷贝，没有 `%destructor`）。
- 只想留一份「文法 + 分析报告」、不要任何代码（归档、交接、写实验报告）：
  `--lang gram`，产物就是 `.gram` 原文件与 `RESULT.md`；换 `--mode` 再导一次
  就是另一套分析（例如 `--mode lalr --teach` 看冲突）。

## 生成产物

生成器只输出分文件工程，默认写到 `--out-dir`（缺省 `out/`）。下表是 **C99**
产物（`--lang c`）；缺省的 **C++17** 产物是另一套后端生成的
`parser.hpp` / `parser.cpp` / `main.cpp`（写了 `%code ... %endcode` 时多一个
`parser_user.h`），API 与 `Makefile` / `CMakeLists.txt` 都是 C++17 版
（见[目标语言](#目标语言c99--c17--python--c--rust--go)）；**Python** 产物（`--lang python`）
只有 `parser.py` / `main.py` / `RESULT.md`（外加 `%mermaid` / `%copy` 的文件），
没有 `Makefile` / `CMakeLists.txt`；**C#** 产物（`--lang csharp`）是
`Parser.cs` / `Token.cs` / `Program.cs` / `parser.csproj` / `RESULT.md`
（见 [C# 产物](#c-产物--lang-csharp)）；**Rust** 产物（`--lang rust`）是
`Cargo.toml` + `src/parser.rs` / `src/token.rs` / `src/main.rs` / `RESULT.md`
（见 [Rust 产物](#rust-产物--lang-rust)）。`%copy` 进来的文件
各档目标都保持原样：

| 文件 | 内容 |
| --- | --- |
| `parser.h` | 公开 API：`Parser_Run` / `Parser_RunText` / 树 / 错误 |
| `parser.c` | 词法器、`ParserCtx_T`、语义动作、LL/LR 主循环、树 API |
| `parser_token.h` | Token 枚举、`ParserToken_T`、Token 名称接口、正则 DFA 与标识符接口声明 |
| `parser_token.c` | Token 名称表、文本辅助、`%token regex`/`%identifier` 的 DFA 表与函数 |
| `parser_user.h` | `%code ... %endcode` 中的用户代码，以及由 `%value_type` / `%union` 生成的 `ParserValue_T` 定义；三者都没有时不生成 |
| `parser_table.c` | 预测分析表 / ACTION / GOTO / 产生式表 / 树元数据表 |
| `main.c` | 演示 `main`：从标准输入读一行，再调 `Parser_RunText` |
| `Makefile` | 构建脚本：`make` 出 `libparser.a` + `parser_demo`，另有 `lib` / `demo` / `run` / `clean` |
| `CMakeLists.txt` | CMake 构建脚本（与 `Makefile` 等价）：`cmake -S . -B build && cmake --build build` |
| `RESULT.md` | 生成信息、产物清单、编译运行说明、冲突消解与未消解冲突（分析报告全文要加 `--full-report`，见 [`08-generated-code.md`](08-generated-code.md#17-resultmd默认简化版)） |
| `mermaid/*` | 状态机图示：`index.html` / `grammar.html` / `lexer.html` 与 `.mmd` 源码；**只在开了 `%mermaid` / `--mermaid` 时生成**（与教学模式无关），见[状态机图示](13-modes.md#状态机图示mermaid) |

### 模板（`source/templates/`）

生成内容全部拆成模板，便于直接编辑：

| 模板 | 作用 |
| --- | --- |
| `c99_parser_h_custom.h`、`c99_parser_c_custom.c`、`c99_custom_tree_api.h`、`c99_parser_prologue.c` | parser.c / parser.h 的骨架、公开 API 头与共用宏定义（容量宏 + `PARSER_MEMSET` / `PARSER_MEMCPY` 兜底） |
| `cpp17_parser.hpp`、`cpp17_parser.cpp`、`cpp17_main.cpp`、`cpp17_compat.hpp`、`cpp17_core.cpp` | C++17 产物的骨架：公开接口（`Engine` / `Tree` / `Error`）、运行时装配、`main.cpp` 外壳与 C 名字兼容层 |
| `cpp17_tables.cpp`、`cpp17_helpers.cpp`、`cpp17_tree.cpp`、`cpp17_ll.cpp`、`cpp17_lr_*.cpp`、`cpp17_lexer*` | C++17 产物的分析表、词法器、各模式驱动与树实现 |
| `python_parser.py`、`python_engine.py`、`python_main.py` | Python 产物的骨架：模块头 / 常量表 / `Token` / `Tree` / 状态码，解析器类与 `run_text` / `run` / `parse_tree`，`main.py` 外壳 |
| `python_lr*.py`、`python_ll.py`、`python_lexer*.py` | Python 产物的动作分派、分析表、LR / LL 驱动与词法器 |
| `python_demo_main.py` | Python 的演示 `main` 正文 |
| `csharp_*` | C# 产物：解析器 / Token / 演示入口 / `parser.csproj` |
| `rust_*` | Rust 产物：`Cargo.toml` / 解析器 / Token / 演示入口（`src/*.rs`） |
| `go_*` | Go 产物：`go.mod` / 解析器 / Token / 演示入口（`parser.go` / `token.go` / `main.go`） |
| `c99_parser_token_h_custom.h`、`c99_parser_token.c`、`c99_token_impl_custom.c` | Token 模块 |
| `c99_lexer.c`、`c99_lexer_*_block.c` | 词法器（固定字面量 / 字符串 / 数字 / 正则 / 标识符各一段） |
| `c99_custom_ll_*.c`、`c99_custom_lr_*.c`、`c99_lr_tree_header.c`、`c99_custom_tree_*.c` | 各模式的分析运行时与语法树 |
| `c99_parser_table.c` | 分析表骨架 |
| `c99_main_custom.c`、`c99_demo_main_custom.c` | C 产物的演示程序 |
| `RESULT.md`、`result_run_section_{python,c_cpp17}.md`、`result_runtime_section_{custom,cpp17,python}.md`、`result_tree_section_{custom,cpp17,python}.md` | 生成结果说明与它的各小节；`RESULT.md` 用 `{% include %}` 把后三组按后端拼进来 |
| `Makefile_custom`、`Makefile_custom_cpp17` | C99 / C++17 产物的 `Makefile` |
| `CMakeLists_custom.txt`、`CMakeLists_custom_cpp17.txt` | C99 / C++17 产物的 `CMakeLists.txt` |
| `c99_default_grammar.gram` | 没有 `--grammar-file` 时用的缺省文法 |

C99 那批的文件名（`c99_*`）与产物名**可能同名**（`lexer.c` / `parser_token.c` /
`parser_table.c` 既是模板名又是产物文件名），改名字时别把产物名一起改了。

模板**一律走 `.tmpl`**：`library/template` 引擎的格式（模板里的 `{{ 变量 }}` /
`{% if %}` / `{% for %}` / `{% include %}` 由生成器渲染，生成器只摆「名字 → 值」）。
改模板只影响排版与取值名，不动生成逻辑。目前**六档后端的代码模板
（`c99_*` / `cpp17_*` / `python_*` / `csharp_*` / `rust_*` / `go_*`）全都是 `.tmpl`**
（构建脚本、`RESULT.md` 那几份也是），`.template` 已清零；`source/templates/`
**一共 384 份**，`c99_*` 50 / `cpp17_*` 68 / `python_*` 51 / `csharp_*` 49 / `rust_*` 53 /
`go_*` 55 / `java_*` 2 / `js_*` 3，其余 53 份是构建脚本（`Makefile_*` / `CMakeLists_*`）、`code_gen_*` 共用片段、
`RESULT.md` 与 `result_*.md` 片段、`mermaid_*`（状态机图示的图源码 / 页面 / 文案）。

## 生成代码的 API

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

同一件事在 C++17 产物里（默认目标语言）是**完全不同的接口**——Qt 风格的 `Engine`：

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

- `Status` 是 `enum class`（`Ok` / `ErrStack` / `ErrSyntax` / `ErrLexer` …），
  数值与 C 产物的 `PARSER_*` 一一对应；
- `Error` 是类：`code()` / `line()` / `column()` / `message()` / `toString()`；
- `Tree` 是类：`name()` / `text()` / `childCount()` / `child(i)` / `print()`，
  所有权走 `std::unique_ptr`，拷贝被禁用；
- 兼容层：`Parser_RunText` / `Parser_GetLastError` / `ParserError_T` / `PARSER_OK`
  这些 C 名字在同一个命名空间里也有（给 `%main` / `%epilogue` 用，底下是共享
  `Engine`，非线程安全）；
- `ParserValue_T` 由你的 `%value_type` / `%union` / `%code ... %endcode` 决定，两边同名。

C# 一档的完整 API 见
[`08-generated-code.md`](08-generated-code.md) §12（本节只列
C99 与 C++17 两份；上面「C# 产物」一节是速查表）。

错误码：`PARSER_OK 0`、`PARSER_ERR_STACK -20`、`PARSER_ERR_SYNTAX -21`、
`PARSER_ERR_LEXER -22`、`PARSER_ERR_DUPLICATE -23`、`PARSER_ERR_UNDEFINED -24`、
`PARSER_ERR_SYSTEM -25`。解析失败时 `Parser_GetLastError()` 给出错误码、行列与说明。

`%tree` 的 `Parser_ParseTreeText` 只建 CST、不执行语义动作，因此可以和语义动作共存：
`Parser_RunText` 按动作求值，`Parser_ParseTreeText` 出语法树。

## 内存管理

语义值由你自己管：`ParserValue_T` 按值拷贝，运行时只把它在分析栈上搬来搬去，
不 new、不 free。里头的指针指向的内存（字符串、AST 节点……）由你的
`%code ... %endcode` 负责分配与释放，通常配套一个 `Parser_TreeFree` 风格的自定义清理函数。

只有 `%tree` 的语法树由运行时分配：`ParserTree_T` 的节点与子节点数组走
`malloc` / `realloc` / `free`，用 `Parser_TreeFree(pTree)` 一次释放整棵树；
`Parser_ParseTreeText` 的所有权规则写在
[生成代码的 API](#生成代码的-api) 一节。

想把树的分配换成静态池 / RTOS 堆 / 带统计的分配器，直接改
`source/templates/c99_custom_tree_helpers.c.tmpl`、`c99_custom_lr_tree_body.c.tmpl`
里的 `malloc` / `realloc` / `free` 即可
（生成内容全在模板里）。

生成代码里的**清零与拷贝**也留了钩子：`memset` / `memcpy` 的每一处调用都走
`PARSER_MEMSET(pDst, value, size)` / `PARSER_MEMCPY(pDst, pSrc, size)`，两个宏在
`parser.h` 里用 `#ifndef` 兜底成 `<string.h>` 的同名函数；在 `%code ... %endcode` 里先
`#define` 同名宏（或编译期 `-D`）就能换成自己的实现——无 libc 环境、带统计 /
越界检查的版本、RTOS 专用搬运都行。C++17 产物没有这两个宏：内存一律由标准库管，
不支持自定义分配器，也不需要 memset / memcpy。

C# 这一档没有分配器钩子、也没有 `memset` / `memcpy` 钩子：语义值与
语法树由 GC 回收，`%destructor` 在这一档下直接报错。

`parser_user.h` 会被多个 `.c` 包含，直接写在头里的辅助函数请用 `static inline`，
表之类的全局状态建议放在函数内部的 `static` 里，避免每个编译单元各留一份。
