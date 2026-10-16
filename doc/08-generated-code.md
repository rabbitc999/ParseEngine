# 05 生成产物与生成代码 API

模式与语言组合统一见 [12 支持矩阵](15-language-mode-support.md)。增量 API 只在 C++17 / Rust
提供：LL/DD 同文本缓存、确定性 LR 前缀 checkpoint、GLR 家族实验缓存是三种不同能力。
Earley/CYK 共用 GLR 实验入口，不代表实现了增量图表算法；纯度策略当前只用于 GLR 家族。

## 1. 产物清单

C99 产物（`--lang c`）默认 10 个文件（C++17 产物的清单见 §1.1，Python 产物见
§10，C# 产物见 §12，Rust 产物见 §14，Go 产物见 §15）：

| 文件 | 内容 |
| --- | --- |
| `parser.h` | 公开 API：`Parser_Run*`、树、错误 |
| `parser.c` | Lexer、解析主循环、语义动作、树实现；写了 `%epilogue` 时末尾追加这段用户代码 |
| `parser_token.h` | Token 枚举、`ParserToken_T`、Token 文本接口、词法 DFA 声明 |
| `parser_token.c` | Token 名字表、文本辅助、正则 DFA 与标识符接口实现 |
| `parser_user.h` | 用户 `%code ... %endcode` 代码 + `%value_type` / `%union` 生成的 `ParserValue_T`；三者都没有时不生成 |
| `parser_table.c` | 预测分析表 / ACTION / GOTO / 产生式表 |
| `main.c` | 入口：默认是生成器内建的 demo main；写了 `%main`（且条件编译后只有一段生效）时换成用户正文 |
| `Makefile` | `make` → `libparser.a` + `parser_demo` |
| `CMakeLists.txt` | 与 `Makefile` 等价的 CMake 目标（`parser` + `parser_demo`） |
| `RESULT.md` | 生成信息、产物清单、冲突消解与未消解冲突（分析报告全文默认不写，见 §1.7） |

`%copy` 的文件会追加进产物目录，其中的 `.c` / `.h` 同时计入 `Makefile`
与 `CMakeLists.txt`。

两个段的产物落点：

| 段 | 落点 | 备注 |
| --- | --- | --- |
| `%epilogue` | `parser.c` 末尾（C++17 是 `parser.cpp` 末尾，在命名空间之外，正文前补一行 `using namespace`；Python 是 `parser.py` 末尾；Rust 是 `src/parser.rs` 末尾；Go 是 `parser.go` 末尾） | 不新增文件；能直接用 `parser_user.h` 里的声明，也能按 C99 名字调生成器产出的函数（C++17 靠兼容层，见 §1.2；Python 与解析器类同模块） |
| `%main` | `main.c` 正文（C++17 是 `main.cpp`；Python 是 `main.py` 的演示体；C# 是 `Program.cs` 里入口类的成员；Rust 是 `src/main.rs` 的演示体；Go 是 `main.go` 的演示体） | 外壳（`stdio` / `string` / `parser.h`；Python 是头注释 + `import parser` + 类名别名；C# 是文件头注释 + `namespace` + 入口类声明）仍是模板；产物清单不变 |

### 1.1 目标语言：C++17（缺省）/ C99 / Python / C# / Rust / Go

缺省目标是 C++17，产物是**另一套后端**（`source/src/code_gen_cpp17.cpp` +
`source/templates/cpp17_*`）生成的，不是对 C 产物做文本改写：

| 文件 | 内容 |
| --- | --- |
| `parser.hpp` | 公开接口：命名空间里的 `Status` / `Token` / `Error` / `Tree` / `Engine`，以及 `ParserValue_T`（`%union` 正文也在这里） |
| `parser.cpp` | 运行时：分析表、词法 DFA、词法器、LR/LL 驱动、公开 API 实现 |
| `main.cpp` | 入口：内建 demo main（读标准输入一行）或 `%main` 正文 |
| `parser_user.h` | `%code ... %endcode` 的用户代码；没写时不生成 |

写了 `%namespace myapp`（或命令行 `--namespace myapp`）时文件名带前缀：
`myapp_parser.hpp` / `myapp_parser.cpp` / `myapp_main.cpp` / `myapp_parser_user.h`，
命名空间也换成 `myapp`；`Makefile` / `CMakeLists.txt` / `RESULT.md` 不加前缀，
`%copy` 的文件保持原名。**C99 产物用不了 `%namespace`**（配 `--lang c` 会报错）。

C++17 产物与 C99 产物的关系不是「一对一改名」，API 完全不同（见 §1.2）；Python
产物又是第三套接口（见 §10）。`%tree` / `%sync` / 错误恢复这些
特性三种目标都有，语义（含错误消息文本）逐条对齐，只是写法不同
（`%value_type` / `%union` 是 C / C++17 专用的语义值模型，Python 用任意对象）。

加 `--lang c`（或文法里写 `%lang c`）得到本文档其余小节描述的 C99 形态。文法依赖
C++17 专有的东西时可以在文法里写 `%cpp_only`：再配 `--lang c` 会直接报错。

`--lang python`（或 `%lang python`）是第三种：`source/src/code_gen_python.cpp` +
`source/templates/python_*` 生成 `parser.py` / `main.py` 两个文件，不编译、不生成
`Makefile`，语义值是任意 Python 对象——API 见 §10。`%code ... %endcode`
的正文归一化后插在 `parser.py` 的 import 之后；`%value_type` / `%union` /
`%namespace` 这些 C 专属写法**不支持**（写了在生成期直接报错），类名用
`%class` / `--class` 改。

`--lang csharp` 是第四种：`source/src/code_gen_csharp.cpp` + `source/templates/csharp_*` 生成
`Parser.cs` / `Token.cs` / `Program.cs` / `parser.csproj`——API 见 §12。这一档的
语义值是「`%value_type` 原样填 / `%union` → 带成员字段的 `ParserValue` 类 /
都不写 → `object`」三态；`%package` / `%import` / `%class_body` 三个指令
对这一档生效。报错定位不用 `#line`，改成五处成对的注释行（§12.3）。

`--lang rust` 是第五种：`source/src/code_gen_rust.cpp` + `source/templates/rust_*` 生成
Cargo 工程（`Cargo.toml` + `src/parser.rs` / `src/token.rs` / `src/main.rs`）——API、
产物清单与「语义值必须满足 `Copy + Default + Debug`」这条硬约束见
[`14-languages-and-products.md`](14-languages-and-products.md#rust-产物--lang-rust)。
`%package`（crate 名）/ `%import`（`use`）/ `%class_body` 同样生效，
**`%destructor` 不支持**（靠所有权回收，写了在生成期直接报错）；词法正则用 Rust
**官方的 `regex` crate**（`Cargo.toml` 里一条 `regex = "1"`，`src/parser.rs` 顶部
`use regex::Regex;`），产物里没有 DFA 表——见 §14.4。

`--lang go` 是第六种：`source/src/code_gen_go.cpp` + `source/templates/go_*` 生成一个
Go 模块（`go.mod` + `parser.go` / `token.go` / `main.go`）——API、产物清单与
「`%union` 成员照 C 的声明写」这条口径见 §15。
`%package`（module path）/ `%import`（import 块）/ `%class_body`（一组
`func (p *Parser) …` 方法）同样生效，**`%destructor` 不支持**（由 GC 回收，写了在
生成期直接报错）；词法正则用 Go **标准库的 `regexp`**（没有第三方依赖），产物里没有
DFA 表——见 §15.5。

### 1.2 C++17 公开 API（Qt 风格）

一个类搞定全部解析状态：`Parser::Engine`（d-pointer，不可拷贝），方法用 camelCase：

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

| 成员 | 作用 |
| --- | --- |
| `Status runText(std::string_view text, ParserValue_T *result)` | 解析文本 |
| `Status run(const std::vector<Token> &tokens, ParserValue_T *result)` | 解析已经切好的 Token 序列 |
| `const std::vector<Error> &errors() const` | 本次解析累积的错误（一次可多条） |
| `const Error *lastError() const` | 最近一条错误；没有时 `nullptr` |
| `ParserLocation currentLocation() const` | 当前 lookahead 的位置（行 / 列 1 起，`offset` 是 UTF-8 字节偏移）；见 §1.5 |
| `std::unique_ptr<Tree> parseTree(std::string_view text)` | `%tree` 文法：只建树、不跑语义动作 |

命名空间里还有几个自由函数与类型：

- `enum class Status { Ok, ErrStack, ErrSyntax, ErrLexer, ... }`，`statusName(Status)`
  取名字；数值与 C 产物的 `PARSER_*` 一致；
- `struct Token`：`type` / `value` / `text`（`std::string_view`）/ `name` / `pName` /
  `line` / `column` / `offset`；`name` / `pName` 只为兼容按 C 产物写的 `%code ... %endcode`
  而保留；
- `tokenName(TokenType)` / `tokenText(const Token &)`；
- `class Error`：`code()` / `line()` / `column()` / `message()` / `toString()`；
- `class Tree`（`%tree`）：`name()` / `text()` / `location()` / `childCount()` /
  `child(i)` / `addChild(unique_ptr)` / `toString()` / `print(FILE * = stdout)`；
  父子所有权由 `std::unique_ptr` 管，拷贝被禁用（要复制自己写克隆）；`location()`
  见 §1.5。

**兼容层**：同一个命名空间里还提供一层 C 产物 API 的同名入口，给 `%main` /
`%epilogue` 里按 C 写法写的代码用——

```cpp
Parser_RunText("1+2*3", &result);        // = Parser::Parser_CompatEngine().runText(...)
Parser_GetLastError();                   // 返回 ParserError_T*
PARSER_OK / PARSER_ERR_* / PARSER_MAX_*  // 兼容常量（inline constexpr）
```

它底下是一个进程级共享的 `Engine`（`Parser_CompatEngine()`），所以**非线程安全**；
新代码建议直接用 `Engine`。细节见 [`07-codegen.md`](07-codegen.md) §8.1。

### 1.3 内存：没有自定义分配器

C++17 产物的内存全部交给标准库：`std::string` / `std::string_view` /
`std::vector` / `std::deque` / `std::unordered_map` / `std::unique_ptr`，
分析栈按需增长（`std::vector`），语法树析构即释放。**不支持自定义分配器**，
也没有 C 产物里的 `PARSER_USER_DISCARD_VALUE` 这个全局钩子——要控制内存就让
语义值类型遵守 RAII（`std::string` / `std::unique_ptr` 之类），或者用
`%union` 的 `%destructor <成员>` 按成员登记清理代码（见 §9.1），后者是
C 与 C++17 共用的写法。

### 1.4 `#line`：报错指回 `.gram`

产物里凡是**来自 `.gram` 的代码**都带一对 `#line`：

```c
#line 10 "example/calc.gram"
    result.value = stack->values[...];
#line 511 "parser.c"
```

所以 `gcc -std=c99 -c parser.c` 报错时会直接指向 `.gram` 的行号并回显那一行源码
（C++17 下 `g++ -std=c++17 -c parser.cpp` 同理，恢复行指向 `parser.cpp` / `main.cpp`）。
覆盖的位置：语义动作（`parser.c`）、`%code ... %endcode` 与 `%union`（`parser_user.h`）、
`%epilogue`（`parser.c` 末尾）、`%main`（`main.c`）。
纯生成内容——`parser_table.c` / `parser_token.c` / `parser.h` / `Makefile` /
`CMakeLists.txt` / `RESULT.md`——里一行 `#line` 都没有。细节见
[`07-codegen.md`](07-codegen.md) §4.2。

**Python 产物没有 `#line`**（Python 也没有这条指令），改成在每个用户代码位置前贴
一行注释 `# 语法文件 <文法文件> 行 <N>`，指向的行号与 C 的 `#line` 一致：

```python
        if production == 3:
            # 语法文件 example/calc.gram 行 10
            result.value = values[0] + values[2]
            return STATUS_OK
```

它只是给人看的导航（Python 的 traceback 仍然报 `parser.py` 自己的行号），缩进跟着
所在块走，`python -m py_compile parser.py` 照常通过。见 §10.5 与
[`07-codegen.md`](07-codegen.md) §4.2。

### 1.5 位置信息 API：`ParserLocation` / `currentLocation()`

错误消息里的行列号现在也是一份**结构化数据**：解析器暴露当前 lookahead 的位置，
语法树节点带上自己覆盖文本的起点，IDE / AST / 错误报告不用再去 Parse 消息文本。
六档（C99 / C++17 / Python / C# / Rust / Go）都支持：

| 目标语言 | 位置类型 | 解析器 API | Token 字段 | 树节点字段 |
| --- | --- | --- | --- | --- |
| C99 | `ParserLocation { int line; int column; int offset; }` | `Parser_CurrentLocation()` | `ParserToken_T::line / column / offset` | `Parser_TreeGetLocation(node)` |
| C++17 | `ParserLocation { int line; int column; int offset; }` | `Engine::currentLocation()` | `Token::line / column / offset` | `Tree::location()` |
| Python | `ParserLocation`（namedtuple：`line` / `column` / `offset`） | `engine.current_location()` | `token.line / column / offset` | `Tree.location` |
| C# | `Parser.Location`（`readonly struct`） | `engine.CurrentLocation` | `Token.line / column / offset` | `Tree.Location` |
| Rust | `ParserLocation { line / column / offset }`（`u32`） | `engine.current_location()` | `Token.line / column / offset` | `Tree::location()` |

约定（六档统一）：

- **行 / 列都是 1 起**；`offset` 是从输入开头算起的 **UTF-8 字节偏移**（0 起）。
  非 ASCII 输入下「列」是字符数（Python）还是字节数（C++17）沿用各自词法器
  现有的计数方式，`offset` 一律按 UTF-8 字节算——要对齐字节位置用 `offset`。
- 位置指的是**当前 lookahead Token 的起点**：与动作里的 `$line` / `$column`
  同源（这两个伪变量现在就是读 `currentLocation()`；`$token` / `$code` /
  `$message` 语义不变）。词法错误时 `currentLocation()` 就是出错位置，
  与 `error.line()` / `error.column()` / `toString()` 里的行列号一致。
- Token 数组模式（C++17 `run(tokens)`）下 `offset` 由调用方给的 Token 决定：
  没填就是 0，`line` / `column` 也用调用方填的值。
- **语法树节点**：叶节点是那个 Token 的起点；分支节点取第一个有位置的子节点
  （也就是节点覆盖文本的起点，按需计算，先建父节点再填子节点也对）；空节点
  （如 `%empty` 收出来的分支）三个字段都是 0，表示「未知」。
- **C99 的所有权**：`ParserCommit::text` 是 `malloc` 出来的，取完内容必须调
  `Parser_FreeCommits()`（下一次 `Parser_RunText` / `Parser_Run` 开始时也会
  自动释放上一次的）；`ParserCommit::type` 指向静态字符串，**不要 free**。
  `%mode lex` 的 C99 产物对应 `Parser_LexGetCommits(&lexer)` /
  `Parser_LexFreeCommits(&lexer)`（`Parser_LexInit` 也会自动清）。

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

验收用的对照：对同一份输入先解析成功、打印某个节点的位置，再喂一条在**同一位置**
出错的反例，错误消息里的行列号与 `currentLocation()` / `current_location()` 完全
相等。`example/json.gram` 是现成的 `%tree` 例子（用 `--lang cpp17` / `--lang python`
生成后按上面的片段取节点位置即可）。

### 1.6 错误消息格式（六档统一）

语法错误的说明不再只是「分析表没有匹配项」这类笼统说法：**说出这里期望什么、
实际遇到了什么**。六档（C99 / C++17 / Python / C# / Rust / Go）都覆盖，
同一份文法 + 同一输入下，**六档产物的
`message`、错误码、行列号逐字一致**：

| 情况 | `message` | 例子 |
| --- | --- | --- |
| 期望集非空（≥2 个） | `语法错误：期望 A / B 之一，实际是 C` | `语法错误：期望 PLUS / MINUS / RPAREN 之一，实际是 DOT` |
| 期望集非空（只有 1 个） | `语法错误：期望 A，实际是 C` | `语法错误：期望 ASSIGN，实际是 VARIABLE` |
| 期望集为空（LL 家族） | `语法错误：该状态下无可用产生式` | LL(0) / 表里没有任何可用列 |
| 期望集为空（LR 家族） | `语法错误：该状态下无可用动作` | ACTION 那一行没有任何移进/归约 |

规则与来源：

- **期望集**就是**生成期**算好的「这一行有动作的终结符」：LL 家族取预测分析表里
  该非终结符的列集合（`expectedTokensByRow`），LR 家族取 ACTION 表该状态那一行
  里所有能移进 / 归约的终结符。
- 消息里最多列 **4** 个期望 Token（按终结符声明序），超过就用
  `等 N 个之一` 带过（`N` 是该行的期望总数）；结构体 / 对象里仍然保留最多
  `PARSER_MAX_EXPECTED` / `_MAX_EXPECTED` / `MAX_EXPECTED`（8）个 +
  `expectedTotal` 总数，给调用方自己排版。
- 期望集为空时用「该状态下无可用产生式 / 动作」兜底（生成期按模式写进产物常量：
  C `PARSER_EXPECTED_FALLBACK`、C++17 `kExpectedFallback`、Python / C# / Rust / Go
  `_EXPECTED_FALLBACK` / `sParserExpectedFallback`）。
- **错误码**沿用细分码：期望集非空时 EOF 报 `PARSER_ERR_UNEXPECTED_EOF` /
  `Status::ErrUnexpectedEof` / `STATUS_ERR_UNEXPECTED_EOF`（-27），其余是
  `..._ERR_EXPECTED` / `ErrExpected` / `STATUS_ERR_EXPECTED`（-26）；期望集为空
  退回笼统的 -21（`ErrSyntax`）。
- **完整诊断**由 `ParserError_T::message` / `Error::toString()` / `str(error)` /
  `error.Display` 拼成，格式统一是 `第 N 行第 M 列：<message>`（行 / 列 1 起，
  与 §1.5 的位置 API 同源）。
- 错误结构里除了 `message` 还有：`tokenType` / `tokenText`（实际看到的 Token）、
  `state`（LR 状态号；LL 是栈顶符号编号）、`expected` / `expectedCount` /
  `expectedTotal`（上面的期望集合与总数）。
- **词法错误**不是「期望 / 实际」形态：未闭合字符串是
  `Status::ErrLexUnterminated`（-28）+「字符串没有闭合」，认不出来的字符是
  `ErrLexChar`（-29）+「认不出来的字符」，其余词法失败是 `ErrLexer`（-22）+
  「词法解析失败」。
- C99 产物的 `message` 是定长字段（`PARSER_ERROR_MESSAGE_MAX`，缺省 192 字节），
  超出按字段容量截断；其余五档的字符串不截断。典型消息（4 个期望 Token）
  在缺省值下都不会被截断。

实跑对照（`example/pl0.gram`，`%mode lalr` + `%tree`，六档同文案）：

```text
$ printf 'x := (1 + 2 .' | ./parser_demo
解析失败: code=-26 line=1 column=13 message=语法错误：期望 PLUS / MINUS / RPAREN 之一，实际是 DOT

$ printf 'x := 1 , 2 .' | ./parser_demo
解析失败: code=-26 line=1 column=8 message=语法错误：期望 END / THEN / DO / PLUS 等 16 个之一，实际是 COMMA

$ printf 'begn x := 1 .' | ./parser_demo
解析失败: code=-26 line=1 column=6 message=语法错误：期望 ASSIGN，实际是 VARIABLE
```

### 1.7 `RESULT.md`：默认简化版

`RESULT.md` 是产物目录里唯一的一份说明书，按固定顺序写这些小节：

```text
# 生成结果
## 生成信息           源文法 / 生成模式 / 目标语言 / %tree / %teach / %mermaid /
                      优先级指令 / %expect / %token regex / %identifier
（语言与兼容提示）     目标语言专属说明、%sync、容量宏、%epilogue / %main 段、
                      LL(k) 展望符、IELR 状态拆分、Mermaid 图
## 产物文件           本次写了哪些文件
（可选小节）          公开 API 示例、位置信息与 %commit、编译运行、运行时与内存、
                      语法树、图表——写了对应特性才有
## 冲突消解           每条被 %left / %right / %prec 消掉的冲突
## 未消解冲突         没能消掉的移进/归约、归约/归约冲突
```

**末尾的「`## 文法分析报告`」默认不写**：那一节的正文是逐一展开的最终产生式 /
FIRST / FOLLOW 表 / 自动机 / 冲突明细，动辄几百行，只在排查文法问题时才看，
钉进每个产物目录不合适。要看它有两种方式，拿到的是**同一份文本**：

| 方式 | 效果 |
| --- | --- |
| `--full-report` | 把报告全文贴进 `RESULT.md` 末尾的 ```` ```text ```` 代码块 |
| `--report 文件` | 把报告另存为 UTF-8 文本（与 `--full-report` 贴进去的那段逐字相同） |

```sh
ParseEngine example/calc.gram --out-dir out --lang c              # 简化版 RESULT.md
ParseEngine example/calc.gram --out-dir out --full-report         # RESULT.md 带整份报告
ParseEngine example/calc.gram --out-dir out --report calc-report.txt   # 报告单独成文件
```

报告内容随 `--mode` 变化（换模式再生成一次就是另一套分析，产生式 / FIRST /
FOLLOW / 状态机 / 冲突各不相同）。六档代码后端里，C99 / C++17 / Python 的
`RESULT.md` 走模板，C# / Rust / Go 是另一套实现，但 `--full-report` 在六档上都
会把这一节附在末尾；`--lang gram` 那一档导出的 `<文法名>.analysis.md` **本身就是
报告**（见 §13），不受这个开关影响；`java` / `js` 两个窄后端不写 `RESULT.md`。

## 2. 解析入口（C99 产物）

本节到 §9 描述的是 **C99 产物**（`--lang c`）的 API；C++17 产物是另一套接口
（`Engine` / `Tree` / `Error`，见 §1.2），只有 `ParserValue_T` 与错误码数值
两边一致。

```c
int32_t Parser_Run(const ParserToken_T *pTokens, uint32_t count,
                   ParserValue_T *pResult);
int32_t Parser_RunText(const char *pText, ParserValue_T *pResult);
const ParserError_T *Parser_GetLastError(void);
uint32_t             Parser_GetErrorCount(void);
const ParserError_T *Parser_GetError(uint32_t index);
```

`ParserValue_T` 由用户提供（POD）：常见的是 `%value_type double` 这样一行，
也可以 `%union` 或自己在 `%code ... %endcode` 里 typedef；都没有时生成器补
`typedef int ParserValue_T;`（写了 `%code ... %endcode` 时生成器不再补默认 typedef，
`parser_user.h` 在打开命名空间之前包含，用户给的类型在命名空间里同样可见）。
`PARSER_USER_TOKEN_VALUE(pToken)` 宏可把 Token
换算成语义值（例如数值 Token 取 `pToken->value`）。
成败看返回值：`PARSER_OK` 表示成功，结果写进 `pResult`。

### 2.1 错误恢复 API（`error` 产生式 / `%sync` / LL 的 FOLLOW 集）

一次解析累积的错误条数上限是 `PARSER_MAX_ERRORS`（缺省 8）：

```c
uint32_t             Parser_GetErrorCount(void);            /* 本次解析已报的条数 */
const ParserError_T *Parser_GetError(uint32_t index);       /* 第 index 条，越界返回 NULL */
const ParserError_T *Parser_GetLastError(void);             /* 仍然是最后一条（兼容旧代码） */
```

**`ParserError_T` 的字段**（`%error_context` 时才有的字段单独标出）：

| 字段 | 含义 |
| --- | --- |
| `code` / `line` / `column` / `message` | 错误码、行列、一句话说明（说明里含「期望…实际…」） |
| `tokenType` / `tokenText` | 实际看到的 Token 类型与词素（词素是拷贝，不怕被下一个 Token 覆盖） |
| `state` | LR 状态号（LL 家族是栈顶符号编号），调试用 |
| `expected[PARSER_MAX_EXPECTED]`（缺省 8）/ `expectedCount` / `expectedTotal` | 该位置期望的 Token 类型；`Total` 是真实个数（可能多于 `Count`） |
| `context`（仅 `%error_context`） | 出错那一行原文 + 一行 `^` 指示 |

错误码除了 `-21`（表里没动作且说不出期望什么），还细分出 `-26`（期望集合非空但来了
别的 Token）、`-27`（提前到输入末尾）；`-28` / `-29` 留给「未闭合字符串/注释」与
「认不出来的字符」这两个词法场景。C++17 对应 `Status::ErrExpected` /
`ErrUnexpectedEof` / `ErrLexUnterminated` / `ErrLexChar`，`Error` 上是
`expected()` / `tokenText()` / `state()` / `tokenType()` 四个访问器；Python 是
`e.expected` / `e.token_text` / `e.state` / `e.token_type`。

**动作里能读错误现场**：`$line` / `$column`（当前 lookahead 的行列）、`$token`
（当前 lookahead 的词素）、`$code` / `$message`（最近一条错误的码与说明，**只在
error 产生式里有意义**，写在别处生成期直接报错）。典型用法是把 printf 包成
`%code ... %endcode` 里的宏（动作里不能写字符串字面量），然后在 error 规则里调用：

```gram
%code
#define LOG_ERR(cd, l, c, m, t) printf("%d:%d %s (%s)\n", (l), (c), (m), (t))
%endcode
stmt := error SEMI { LOG_ERR($code, $line, $column, $message, $token); yyerrok; $$ = 0; }
      ;
```

错误表是**全局**的 `static` 数组（和 `Parser_GetLastError` 一样），每次
`Parser_RunText` / `Parser_Run` 开始时清空；写满 `PARSER_MAX_ERRORS` 条后新错误被
丢弃，解析器也不再继续恢复。两个宏都是 `#ifndef` 兜底：既能在文法里用
`%max_errors` / `%error_recovery_tokens` 定缺省值（见
[`05-grammar.md` §2](05-grammar.md#容量宏max_stack_depth--max_errors--error_recovery_tokens)），
也能编译期 `-D` 覆盖（`-D` 优先）：

| 宏 | 缺省 | 文法指令 | 含义 |
| --- | --- | --- | --- |
| `PARSER_MAX_ERRORS` | 8 | `%max_errors N`（1..4096） | 一次解析保留的错误条数上限 |
| `PARSER_ERROR_RECOVERY_TOKENS` | 3 | `%error_recovery_tokens N`（0..4096） | 恢复后「压制重复报错」的 Token 数（bison 的 3 Token 规则）：每成功移进一个真实 Token 递减，降到 0 后下一个错误重新上报；0 = 不压制 |
| `PARSER_LOOKAHEAD_K` | 1 | `%lookahead N`（0..INT_MAX）或写进模式名（`LL(2)`） | 带 k 的模式（`lrk` / `lalrk` / `slrk` / `llk` / `ll0`）的展望符长度；k > 1 时运行时维护 k 个 Token 的窗口并查 k 元组列，其它模式恒为 1（窗口为空、只有一层表）。LL(0) 是 0。**k 没有策略上限**，但列编码是 64 位的，`Token 个数^k` 放不下时生成期会报错 |
| `PARSER_LOOKAHEAD_COLUMN_COUNT` | 1 | （由分析结果定，不是指令） | k 元组列数：LR 侧是归约层列数、LL 侧是预测分析表列数；k ≤ 1 时无意义（取 1 只是为了数组合法） |

分析栈深度上限 `PARSER_MAX_STACK_DEPTH`（缺省 64）同理，可用 `%max_stack_depth N`
（1..65535）在文法里定；撞上上限时 `Parser_Run*` 返回 `PARSER_ERR_STACK`（-20）。

返回值约定（custom 签名下两种情况都适用）：

| 情况 | 返回值 | `pResult` | `Parser_GetErrorCount()` |
| --- | --- | --- | --- |
| 没有错误 | `PARSER_OK` | 有效 | 0 |
| 报过错但恢复后最终解析成功 | `PARSER_OK` | 有效，**结果可能不完整** | > 0 |
| 错误达到 `PARSER_MAX_ERRORS`，或没法恢复 | 负错误码（`PARSER_ERR_SYNTAX`） | 未定义，不要读 | 已报的条数 |

也就是说「恢复过」这件事只有调用方看 `Parser_GetErrorCount()` 才知道。

三端（C99 / C++17 / Python）的恢复策略一致，三种模式各有一条路：

- **LR 家族**：`error` 产生式优先、`%sync` 兜底；
- **LL 家族**：**不写任何指令也有恢复**——运行时按「栈顶能不能取当前 Token →
  是不是 `%sync` 锚点 → 栈顶那一项该结束了（当前 Token 在它的 FOLLOW 里）或更深处
  有项能取它 → 丢掉 Token」四步对齐，第 3 步就是 FOLLOW 集恢复；
写法见 [`05-grammar.md` §3.1 / §3.2](05-grammar.md#31-error-产生式yacc-风格的恢复锚点)；
三个可跑的示例是 `example/error_recovery.gram`、`example/sync_recovery.gram` 与
`example/ll_follow_recovery.gram`。

**LR(k) / LALR(k) 的产物形态（`%mode lrk` / `%mode lalrk`）。** k > 1 时 ACTION
分两层，产物里多出三张表（C99 是 `s_ParserReduce` / `s_ParserLookaheadCode` /
`s_ParserLookaheadColumn`，C++17 是 `kReduce` / `kLookaheadCode` /
`kLookaheadColumn`，Python 是 `_REDUCE` / `_LOOKAHEAD_CODE` / `_LOOKAHEAD_COLUMN`）：

| 表 | 内容 |
| --- | --- |
| 第一层 `s_ParserAction` / `kAction` / `_ACTION` | 只放移进（与从前同一张表） |
| 第二层 `s_ParserReduce` / … | 按 k 元组展望符建列的归约 / 接受 |
| 列索引 | 列编码 = $\sum_i \mathrm{Token号}_i\cdot(\text{Token 个数})^i$（低位是窗口头），按编码升序二分查列 |

运行时每走一步都拿「current + 后面 k-1 个 Token」组成窗口：先按窗口查第二层，命中
就用那条归约 / 接受，否则按窗口头查第一层的移进。窗口在 `%tree`、Token 数组模式、
文本流模式下都一样（文本流模式会预先多取 k-1 个 Token）；`yyclearin` 与错误恢复的
「丢弃 lookahead」都只是把窗口整体左移一位，语义与 k = 1 一致。容量上只多一个
`PARSER_LOOKAHEAD_K`：**产物 API、返回值约定、错误表都不变**。

**LL 侧（`%mode llk` / `ll0`）。** 只有一张预测分析表，列的含义与 LR 的第二层
一样是 k 元组：k = 1 是 Token 密表（与 LL(1) 逐字节一致）、k = 0 只有一列（不看
lookahead）、k ≥ 2 按用到的 k 元组建列。产物里同样多出
`s_ParserLookaheadCode` / `s_ParserLookaheadColumn`（C99）、
`kLookaheadCode` / `kLookaheadColumn`（C++17）、`_LOOKAHEAD_CODE` /
`_LOOKAHEAD_COLUMN`（Python）。**LL 的公开 API 与返回值约定完全不变**，
`Parser_GetErrorCount()` 那一套错误恢复也一样能用。

## 3. 语法树 API（`%tree`）

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

`Parser_ParseTreeText` 只建 CST、不执行语义动作，因此可以和语义动作共存：
`Parser_RunText` 求值，`Parser_ParseTreeText` 出树。

## 4. Token 与词法 API

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

`Parser_TokenText` 的取值顺序：`pName`（词素）→ `name`（数值 / 字符串词素）
→ Token 枚举名。所以固定字面量会显示成 `COLON(:)`，标识符显示
`VARIABLE(x)`，数字显示数值。

### Token / commit 显示别名

声明末尾可以写 `alias "显示名"`，例如：

```gram
%token STRING "..." alias "字符串"
%commit block "/\\*...\\*/" alias "块注释"
```

别名只用于名称表和诊断，不改变 Token 数值、语法符号或 commit 元素里的原始
`type`/`kind`。C99/C++17 生成两列静态表（原名、别名），Python / C# / Rust / Go
生成名称到别名的映射（Go 是 `TOKEN_ALIASES`，`TokenName` 命中时显示
`NAME(别名)`）；`token_name` 等诊断在有别名时显示
`STRING(字符串)`，没有别名时只显示 `STRING`。`%define NAME "..." alias "..."`
与 `%token` 使用同一规则。

## 5. 错误码

| 码 | 名称 | 含义 |
| --- | --- | --- |
| 0 | `PARSER_OK` | 成功 |
| -20 | `PARSER_ERR_STACK` | 栈/帧溢出 |
| -21 | `PARSER_ERR_SYNTAX` | 语法错误（含 LL 表无匹配项、SLR/LALR/IELR 表无匹配项） |
| -22 | `PARSER_ERR_LEXER` | 词法错误 |
| -23 | `PARSER_ERR_DUPLICATE` | 重复定义 |
| -24 | `PARSER_ERR_UNDEFINED` | 未定义 |
| -25 | `PARSER_ERR_SYSTEM` | 系统 / 内存错误 |
| -26 | `PARSER_ERR_EXPECTED` | 期望若干 Token、实际是别的（细分码，见 §1.6；各档另有 `ErrExpected` / `STATUS_ERR_EXPECTED` 等写法） |
| -27 | `PARSER_ERR_UNEXPECTED_EOF` | 输入提前结束（`ErrUnexpectedEof` / `STATUS_ERR_UNEXPECTED_EOF`）：期望集非空时用它，比笼统的 -21 更具体 |
| -28 | `PARSER_ERR_LEX_UNTERMINATED` | 词法错误：字符串没有闭合（`ErrLexUnterminated`） |
| -29 | `PARSER_ERR_LEX_CHAR` | 词法错误：认不出来的字符（`ErrLexChar`） |

`Parser_GetLastError()` 返回只读的 `ParserError_T{code, line, column,
message}`；demo 程序会把它打印成
`解析失败: code=-21 line=1 column=5 message=...`。

错误码不因目标语言而变：C# 侧是同名常量的
`STATUS_ERR_EXPECTED` / `STATUS_ERR_UNEXPECTED_EOF` / `STATUS_ERR_LEX_UNTERMINATED` /
`STATUS_ERR_LEX_CHAR`，Python 是
`STATUS_ERR_*`——取值与上表一一对应，见 §1.6。

错误码的语义不因错误恢复而改变：恢复了但最终成功时返回值仍是 `PARSER_OK`，
`PARSER_ERR_SYNTAX` 同时表示「可恢复的语法错误」与「不可恢复的语法错误」，想区分
就看 `Parser_GetErrorCount()`（见 §2.2）。

## 6. 内存：分配器与 `memset` / `memcpy` 钩子（仅 C99 产物）

C++17 产物没有这一段：内存全部由标准库管（见 §1.3），也不支持自定义分配器。
它也不生成 `PARSER_MEMSET` / `PARSER_MEMCPY`——`std::string` / `std::vector` /
`std::unique_ptr` 自己管清零与拷贝，没有 libc 调用要替换。

C99 产物里只有 `%tree` 的语法树由运行时分配：`ParserTree_T` 的节点与子数组走
`malloc` / `realloc` / `free`，用 `Parser_TreeFree` 批量释放。想换成自己的
内存池（静态池 / RTOS 堆 / 带统计的分配器），改
`source/templates/c99_custom_tree_helpers.c.tmpl`、`c99_custom_lr_tree_body.c.tmpl`、
里的这三处调用再重新生成即可。

语义值的内存完全由用户负责：`ParserValue_T` 按值拷贝，运行时只把它在分析栈上
搬运，既不分配也不释放。指向堆内存的字段（字符串、AST 节点……）请在
`%code ... %endcode` 里自己分配与回收；`parser_user.h` 会被多个 `.c` 包含，写在头里的
辅助函数用 `static inline`，全局状态放函数内部的 `static`。

**错误恢复弹栈时丢掉的值**要格外注意：`error` 产生式与 `%sync` 恢复都会弹掉
若干栈槽位，那些槽位里的 `ParserValue_T` 不会再被用到。运行时不知道用户语义值的
生命周期，可用的办法有两条：写了 `%union` 时用 `%destructor <成员> { ... }`
按成员登记清理代码（首选，C 与 C++17 产物都支持，见 §9.1）；否则 C 产物还可以
定义下面这个全局钩子：

```c
#define PARSER_USER_DISCARD_VALUE(pValue)   /* pValue 是 ParserValue_T * */
```

定义了就对这些被丢弃的值逐个调一次，没定义就一个都不调（也就是**会泄漏**你自己
`malloc` 的内存）。在 `%code ... %endcode` 里定义它即可，例如：

```c
#define PARSER_USER_DISCARD_VALUE(pValue) Ast_Free((AstNode *)*(pValue))
```

写了 `%destructor` 的成员不再走这个钩子（分派表命中就不调它），没写的成员照旧。
`%tree` 的 `ParserTree_T` 不走这个宏：树节点是运行时自己分配的，它知道怎么释放。

### 6.1 `PARSER_MEMSET` / `PARSER_MEMCPY`：换掉生成代码里的 memset / memcpy

生成代码里所有清零与拷贝都写成宏调用，宏本身在 `parser.h` 的 `#ifndef` 兜底段里
（`source/templates/c99_parser_prologue.c.tmpl` 的 `__SEC_DEFINES__` 段）：

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

`parser.h` 先展开 `parser_user.h`、再展开这段兜底，所以在 `%code ... %endcode` 里写一个
同名 `#define` 就已经生效（不会撞上重复定义），编译期 `-D PARSER_MEMSET=...` 同样
优先。典型用法：

```c
/* 带统计的清零：数一数运行时到底清了多少字节 */
#define PARSER_MEMSET(pDst, value, size) \
    (g_MemsetBytes += (size), memset((pDst), (value), (size)))
```

语义值类型是结构体数组之类的场合，这两个宏是生成代码里唯一的批量内存操作出口，
换成 `memcpy_psram` 之类即可整体迁走。宏的参数只求值一次、按位置替换，正常写
`PARSER_MEMSET(&value, 0, sizeof(value))` 这种带同名标识符的调用不会出错。

## 7. 生成 Makefile / CMakeLists.txt

| 目标 | 作用 |
| --- | --- |
| `make` / `make all` | 编译 `libparser.a` 与 `parser_demo` |
| `make lib` | 只编静态库 |
| `make demo` | 只编演示程序 |
| `make run` | 编译并运行（ARGS 会当 argv 传给 main；内建 demo main 读标准输入） |
| `make clean` | 删除 `.o`、`.a`、`parser_demo` |

C++17 产物也是同一套目标（`libparser.a` + `parser_demo`），只是源码是
`parser.cpp` / `main.cpp` 并用 `g++ -std=c++17`；不用 Makefile 也可以：

```sh
gcc -std=c99 -Wall -Wextra -O2 *.c -lm -o parser_demo            # --lang c
g++ -std=c++17 -Wall -Wextra -O2 parser.cpp main.cpp -lm \
    -o parser_demo                                               # 默认（cpp17）
```

同目录还有一份等价的 `CMakeLists.txt`，不依赖 Makefile：

```sh
cmake -S . -B build
cmake --build build
printf '<文本>' | ./build/parser_demo
```

它定义 `parser` 静态库与 `parser_demo` 两个目标，`%copy` 进来的 `.c` 会被
追加进库的源文件列表；Windows 上不链接 `libm`，其余平台与 Makefile 一样
链接 `-lm`。用 MSVC 构建时它会给两个目标加 `/utf-8`（产物源码是 UTF-8，
不加这个开关 MSVC 会按 GBK 代码页解码，中文字符串字面量编译期就被读坏）。

## 8. 演示程序行为

- `printf '<文本>' | ./parser_demo`：从标准输入读一行（到换行或 EOF 为止，
  行尾的换行 / `\r` 不算内容），然后开始解析；`%tree` 文法打印语法树，带语义
  动作的文法打印解析结果（`PARSER_USER_DEMO_PRINT_VALUE`）。
  `./parser_demo < 输入.txt` 这种重定向同理，只取文件的第一行。
- 读到的是空行时照常进入解析——空输入通常是语法错误（退出码 1），不再有
  「只打印用法」的分支。
## 9. `custom` + `%union` 的产物形态

写了 `%union` 时，`parser_user.h` 里多出一段联合体（成员声明
原样来自文法，追加在用户 `%code ... %endcode` 代码之后）：

```c
/* 语义值类型：由文法的 %union 生成，成员声明原样来自文法 */
typedef union {
    int          ival;
    double       dval;
    struct Node *node;
} ParserValue_T;
```

动作里照旧写 `$1 + $3` / `$$ = $1`，生成器按符号绑定的成员展开成成员访问：

```c
/* LL(1)：$1 / $3 是 term（<ival>），$$ 是 expr（<ival>） */
result.value.ival = values[1].ival + values[3].ival;

/* LALR(1)：$n 走状态栈槽位，成员访问跟在槽位后面 */
result.value.ival = stack->values[stack->top - 3u + 0u].ival +
                    stack->values[stack->top - 3u + 2u].ival;

/* 换一个成员：$$ 是 <dval>、$1 是 <ival>，隐式转换交给 C 编译器 */
result.value.dval = values[1].ival / 2.0;
```

因为是联合体，演示程序里要按成员取值：`ParserValue_T result;` 取结果用
`result.ival`（成员名就是文法里写的那个）。

### Token 的值就是 `ParserValue_T`

写了 `%union` 时 `ParserToken_T.value` 也从 `int32_t` 换成 `ParserValue_T`
（`parser_token.h` 按守卫包含 `parser_user.h`，与 `parser.h` 共用
`PARSER_USER_HEADER_INCLUDED`，保证只展开一次）：

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

词法器按 `%token <成员>` 把词素直接写进对应成员，于是终结符的语义值不用再手写
转换宏：

| Token 来源 | 写进成员的值 | 生成的代码形态 |
| --- | --- | --- |
| 数字字面量（`%token <ival> NUMBER`） | 词法解析出的数值 | `pToken->value.ival = (value);` |
| 双引号字符串（`%token <sval> STRING`） | 反转义后的文本 | `pToken->value.sval = pCtx->lexeme;` |
| 标识符（含 `%identifier`）绑的成员 | 匹配到的文本 | `pToken->value.sval = pCtx->lexeme;` |
| `%token <sval> regex KEYWORD "[a-z]+"` | 匹配到的文本 | `case TOKEN_KEYWORD: pToken->value.sval = pCtx->lexeme; break;` |
| 固定字面量（`%define PLUS "+"`） | 不写（没有词素值，保持 0） | 无 |
| `%union` 下漏绑成员的 Token | 不写（联合体里没有对应成员，保持 0） | 无 |

`Parser_UserTokenValue()` 也随之简化：没定义 `PARSER_USER_TOKEN_VALUE` 时直接
`return pToken->value;`（没有 `%union` 时仍然是 memset 出零值）。想自己接管就
照旧定义 `PARSER_USER_TOKEN_VALUE`，它优先。

两个要留意的地方：

- 写进去的是 `pCtx->lexeme` 的地址，而它是词法器复用的缓冲区，读下一枚 Token
  就被覆盖了；要长期保留请自己 `malloc` + `memcpy`。
- 数字 Token 建议绑数值成员、正则与标识符 Token 建议绑文本成员（词法器手上
  只有文本）。绑错了生成期不拦，由 C 编译器报警告——例如
  `%token <ival> regex NUM "[0-9]+"` 会生成
  `pToken->value.ival = pCtx->lexeme;`，要数字请在动作里转换。
- 写 `%union` 时别忘了给用到的内建 Token 绑成员：没绑就没有目标成员，词法器
  不写语义值（数字 Token 因此恒为零值）。C 产物里那几句 `pToken->value = …`
  会被整行删掉——往联合体赋 `int` 编译不过；生成期会打一条 `警告：` 提醒。

完整例子见 `example/union_demo.gram`：生成、编译（C99 与 C++17 两种产物）
后依次喂 `1 + 2 * 3` / `(1 + 2) * 3` / `2 * 3 + 4 * 5`，分别得到 7 / 9 / 26。

### 9.1 `%destructor`：丢弃语义值时调哪段代码

写了 `%destructor <成员> { ... }` 时，产物里多一个按符号分派的静态函数
（C 产物在 `parser.c`，C++17 产物在 `parser.cpp` 的未命名命名空间里）：

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

符号号是**统一编号**：终结符就是 Token 号（0 .. 终结符个数-1），非终结符是
「终结符个数 + 非终结符编号」——两段不重叠，所以光看栈上这个数就知道是哪个文法
符号，进而查到它绑的成员。同一个成员的符号合并成一组 `case`。

调用点：

| 后端 | 语义值放在哪 | 丢弃点 |
| --- | --- | --- |
| C（LR 家族） | `ParserLRStack_T.values[]` + `symbols[]` | `Parser_LRDiscardTop`（错误恢复弹栈）、`Parser_LRDiscardStack`（失败收尾清栈） |
| C（LL 家族） | `ParserSemanticFrame_T.values[]` + 每槽符号 | `Parser_LLDiscardTop`（恢复弹帧）、`Parser_ReleaseContext`（失败收尾） |
| C++17（LR 家族） | `LrStack::values` + `symbols` | `LrStack::discardTop` / `discardAll`（`runLoop` 的 RAII 守卫） |
| C++17（LL 家族） | `LlFrame::values` + 每槽符号 | `discardFrameValues`（`llDiscardTop` / `runLoop` 失败收尾） |

两条约定：

- **写了 `%destructor` 的成员走它、不再调全局钩子**：C 产物的
  `PARSER_USER_DISCARD_VALUE` 只在 `Parser_DiscardValue` 返回 0（该符号没绑
  `%destructor`）时才调；没写 `%union` 的文法产物与从前逐字节一样（分派函数、
  清栈函数都不生成）。C++17 没有这个钩子，没绑的符号什么都不做。
- **只清真的写过值的槽位**：LL 的语义帧按槽位记符号号（`symbols[]`），没写过值的
  槽位保持 `PARSER_INVALID_INDEX`，不会对它执行 `free` 之类的代码；C++17 的帧
  另有 `resultDone` 标记「动作跑过没有」。左递归改写的累加槽不记符号——那份所有权
  已经由「上一轮结果」这个身份交给动作了，记上会重复清理。

计数自检见 `example/destructor_demo.gram`：两个 `%destructor` 各加一个计数器，
喂 4 组输入打印「清理了几次」，C 与 C++17 两种产物输出一致。

## 10. Python 产物（`--lang python`）

Python 产物只有 `parser.py` / `main.py` 两个文件（外加 `%mermaid` 的 `mermaid/*`
与 `%copy` 拷进来的文件，以及 `RESULT.md`），**不需要编译**，装了 `python3` 就能跑。
用途是给文法做快速分析：同一份文法在同样输入下的错误码、行列号、错误消息、
`%tree` 树形文本和 C / C++17 产物逐字对齐，改文法的反馈回路最短。

另一层用处是给 AI / 脚本当接口：`printf '...' | python3 main.py` 只回几行；
`python3 -c "import parser; e = parser.Parser(); e.run_text(...); print(e.result, e.errors)"`
只回一行——编码代理据此就能核对文法，不用把产物或长日志读进上下文（详见
[`07-codegen.md`](07-codegen.md) §9）。

| 文件 | 内容 |
| --- | --- |
| `parser.py` | `import` + 用户代码（`%code ... %endcode`，见 §10.5）+ 模块级常量（状态码、`Token`、`Tree`、`ParserError`）+ 词法表 / 分析表 + 解析器类；写了 `%epilogue` 时末尾再追加一段用户代码 |
| `main.py` | 演示入口：头注释 + `import parser` + 类名别名 + 内建演示体；写了 `%main` 时正文整段替换演示体 |
| `RESULT.md` | 生成信息、产物清单、运行说明、冲突消解与未消解冲突（分析报告全文默认不写，见 §1.7） |

### 10.1 模块级常量与函数

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

- `class Token`：`type` / `text` / `value` / `line` / `column` / `offset`
  六个字段（`offset` 是 UTF-8 字节偏移）；
- `class Tree`（`%tree` 文法）：`name` / `text` / `location` / `children`，方法
  `child_count`（属性）/ `child(i)` / `add_child(node)` / `clone()` /
  `to_string()` / `print(stream=None)`；`location` 是 `ParserLocation`（见 §1.5）；
- `class ParserError`：`code` / `line` / `column` / `message`，
  `str(error)` 直接得到「第 N 行第 M 列：…」（与 C 产物 `Parser_SetError` 的格式一致）。

### 10.2 解析器类

类名由 `%class` / `--class` 决定（缺省 `Parser`）；`main.py` 里用
`Parser = parser.<类名>` 起别名，所以演示体里统一写 `Parser`。类的只读属性：

| 成员 | 含义 |
| --- | --- |
| `run_text(text)` | 解析一段文本；返回状态码，语义结果写进 `result`（失败时 `None`） |
| `run(tokens)` | 解析已经切好的 `Token` 序列 |
| `parse_tree(text)` | 只建语法树、不跑语义动作；返回树根，失败返回 `None`（原因在 `errors`） |
| `current_location()` | 当前 lookahead 的位置（`ParserLocation`，行 / 列 1 起 + UTF-8 字节偏移）；见 §1.5 |
| `result` | 最近一次解析的语义值（任意 Python 对象，`$$` 的落点） |
| `errors` | 本次解析累积的错误（`ParserError` 列表，错误恢复时可能多条） |
| `last_error` | 最近一条错误；没有错误时 `None` |
| `max_stack_depth` / `max_errors` / `error_recovery_tokens` / `value_slots` | `%max_*` 与内部槽位数的实际取值（只读，看 `RESULT.md` 的「容量宏」一节也行） |

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

`%tree` 文法：

```python
tree = parser.Parser().parse_tree("1+2*3")
if tree is not None:
    tree.print()                         # 与 C 产物 Parser_TreePrint 的文本一致
```

### 10.3 语义动作与词法语义值

动作里 `$$` 写 `result.value`、`$1` 读 `values[0]`（`$n` 就是右部第 n 个符号的
语义值），表达式按 Python 语法翻译（`&&` / `||` / `!` 请写 `and` / `or` / `not`，
语句结尾不写分号）；`yyerrok` / `yyclearin` 翻成 `self._yy_err_ok()` /
`self._yy_clearin()`。`%code ... %endcode`（以及 `%epilogue` /
`%main`）的正文会插进模块，做缩进归一化（见 §10.5）。因为语义值是 Python 对象，
**没有** `%value_type` / `%union` / `%token <成员>` 这些东西（写了生成期报错）。

词法器给出的 Token 语义值：字符串 Token 是去掉引号并解好转义的 `str`，数字 Token
是 `int` / `float`，标识符与固定字面量是词素文本，EOF 与 `error` 伪 Token 是
`None`。`parser.py` 里的字符判定（`_is_digit` / `_is_alpha` / `_is_space` …）走
ASCII 语义，与 C 目标的 `isdigit` / `isalpha` 对齐，所以各档产物的词法边界一致。

### 10.4 词法：固定字面量 + Python 正则

Python 产物的词法器**不使用生成器自带的 DFA**，而是把正则原样交给标准库的
`re` 模块——`parser.py` 加载时 `re.compile` 一次，词法匹配时在当前位置
`match()`：

| 文法里的声明 | `parser.py` 里的表 |
| --- | --- |
| `%token regex NAME "正则"` | `_REGEX_RULES = [(编译好的正则, TOKEN_NAME, 声明序), …]` |
| `%identifier "正则"` | `_IDENTIFIER_RE`（没写时是 `None`，退回内建的 C 风格标识符） |
| `%commit` / `%trivia "模式"` | `_COMMIT_RES = [(编译好的正则, 类型名, 是否 skip, 声明序), …]` |

匹配顺序与其它目标一致：内建字符串 → 固定字面量与上面的正则一起**取最长**，
同长度时字面量优先、再按声明序；`%commit` 命中的片段不进语法层。

两点与方言 / 语义有关，跨目标使用时留意：

- **`\b` 的含义不同**：`%token regex` 里 C / C++17 目标把 `\b` 当普通字符 `b`
  （要写 `\s`），Python 的 `re`、C# 的 `Regex`、Rust 的 `regex`、Go 的 `regexp`
  按「词边界」解释
  （见 [`05-grammar.md`](05-grammar.md)「规则细节」）。只写 ASCII 字符类 +
  `\s` / `\d` / `\w` 的话都是一致的。
- **最长匹配的执行方式不同**：只有 C99 走真 DFA（严格最长），C++17 / Python / C# /
  Rust / Go 走各自正则引擎的贪婪匹配。示例文法用到的写法（字符类 + 贪婪重复、
  `([^"\\]|\\.)*`）结果相同；写了 `a|ab` 这种「分支长度不同」的正则时，正则引擎
  按分支顺序取第一个匹配，可能与 DFA 的最长结果不同。反过来，
  `05-grammar.md` 里记的那个「`(A|B)*` + 否定类会永不匹配」的 DFA 构造坑
  **在 C++17 / Python / C# / Rust / Go 产物上不存在**（见 §12.2、§14.4、§15.5、
  `07-codegen.md` §13）。

**生成期校验**（六档同一套，只是处理方式不同）：文法加载时用 `source/src/regex_dfa.cpp`
把每个 `%token regex` / `%identifier` / `%commit` 正则验一遍。**只有 C99 把不通过当
硬错误**——它真的要拿这份 DFA 编表；Python 的产物把正则原样交给 `re`，自带编译器
认不下时只打一条**警告**（`main.cpp` 输出，带原文件行号）就照常生成：`{m,n}` 上限
32、某些否定类写法都属于「自带编译器不认、`re` 认」。「正则能匹配空串」与引擎无关，
六档一律报错。校验出来的 DFA 不再写进 Python 产物。

### 10.5 演示程序行为

`python3 main.py` 与 C / C++17 的 `parser_demo` 行为一致：从标准输入读一行
（到换行或 EOF 为止，行尾的换行 / `\r` 不算内容），然后解析；`%tree` 文法打印
语法树，`result` 不是 `None` 时（通常就是带了语义动作）多打印一行
`解析结果: <repr>`。退出码 0 表示解析成功、1 表示失败。写了 `%main` 时这一整段
被用户正文替换（外壳的
`import parser` 与类名别名 `Parser` 保留）。

示例：`example/python_demo.gram`（`%class PyCalc` + LALR + Python 动作）。

### 10.6 用户代码插在哪里：`%code` / `%epilogue` / `%main`

| 文法里的段 | 落点 | 备注 |
| --- | --- | --- |
| `%code` … `%endcode` | `parser.py` 的 `import sys` 之后、状态码常量之前 | 可写多块，按出现顺序拼接（块之间空一行），动作里可以直接调用里面定义的函数 / 常量 |
| `%epilogue` | `parser.py` 末尾 | 与解析器类同一个模块，可以调类里的方法 |
| `%main` | `main.py` 的演示体 | 外壳的 docstring、`import sys` / `import parser` 与 `Parser = parser.<类名>` 保留，入口调用要自己写 |
| `%copy` | 原样拷进产物目录 | Python 里要自己 `import`（不会自动改名 / 改内容） |

四段正文都按 Python 缩进规则**归一化**：吃掉行尾 `\r`、去掉首尾空行、按所有非空
行的最小前导空白去掉公共缩进、剩下的行首 tab 换成 4 个空格。所以照着文法排版缩进
写（`%code` 段里整体缩进 4 个空格、`%main` 正文缩进着写）也不会变成
`IndentationError`；行首空白之外的字符一个都不改。没有用户代码时不插这一段。

每段正文前面还会贴一行 `# 语法文件 <文法文件> 行 <N>` 的**行标记**（Python 没有
`#line`，只能用注释），指出这段代码来自 `.gram` 的哪一行；指向的行号与 C 目标的
`#line` 完全一致（动作 → 动作起始行，`%code` / `%epilogue` / `%main` → 正文第一行）。

示例见 `example/python_demo.gram`（动作里调用
`%code ... %endcode` 定义的 `py_value`）。

## 11. 词法模式（`%mode lex`）的产物形态

`%mode lex` 六种目标语言都支持，产物里**都没有解析器**：

| 目标 | 文件 | 内容 |
| --- | --- | --- |
| C | `lexer.h` | Token 枚举（`ParserTokenType_T`）/ `ParserToken_T` / `ParserError_T` / `PARSER_*` 宏 + 词法器入口 |
| C | `lexer.c` | 错误表 + 词法器 + `Parser_LexInit` / `Parser_NextToken` |
| C | `parser_token.h` / `parser_token.c` | `Parser_TokenName` / `Parser_TokenText` 与词法 DFA 表 |
| C | `main.c` | CLI：读标准输入到 EOF，逐个打印 Token 类型 |
| C++17 | `lexer.hpp` / `lexer.cpp` | 同一个公开面：`TokenType` / `Token` / `Error` / `Engine::tokenize` |
| C++17 | `main.cpp` | 同样的 CLI（`liblexer.a` + `lexer_demo`） |
| Python | `parser.py` / `main.py` | 词法器类 + `tokenize(text)`（返回 `(状态码, Token 列表)`）/ 同样的 CLI |
| C# | `Parser.cs` / `Token.cs` / `Program.cs` | 同左（`List<Token>`） |
| Rust | `src/parser.rs` / `src/token.rs` / `src/main.rs` | 词法器 + `engine.tokenize(text)` → `(i32, Vec<Token>)` / 同样的 CLI |
| 六档 | `Makefile` / `CMakeLists.txt` / `parser.csproj` / `Cargo.toml` | C / C++17 用 Makefile / CMake（`liblexer.a` + `lexer_demo`），C# 用 `dotnet build`，Rust 用 `cargo build`（带官方的 `regex` 依赖），Python 不需要 |
| 六档 | `RESULT.md` | 产物清单 + Token 表 |
| C / C++17 | `parser_user.h` | 有 `%code ... %endcode` / `%union` 时才生成 |

**不生成解析器**：C 没有 `parser.h` / `parser.c` / `parser_table.c`（也就没有
`Parser_Run` / `Parser_RunText`），C++17 没有 `parser.hpp` / `parser.cpp`，
Python 的 `parser.py` 里没有分析表 / 分析栈 / `run_text`，C# 的
`Parser.cs` 里没有分析表 / 分析栈 / `runText`，Rust / Go 的 `parser` 模块里
只有词法器与 `tokenize` / `Tokenize`。

公开 API（`lexer.h`）：

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

`Parser_NextToken` 取到末尾返回 `PARSER_OK` 且 `pToken->type == TOKEN_END`；词法
错误返回负的细分码（`-29` 认不出来的字符 / `-28` 字符串没有闭合），说明与行列号
在错误表里。结构体字段是「生成器自用 + 调用方只读」，调用方只需在栈上分配一个
`ParserLexer_T` 交给 `Parser_LexInit`。

CLI（内建 demo main，`%main` 可以整段替换）把标准输入读到 EOF，一行一个 Token：

```text
VARIABLE     "a"          1:1
PLUS         "+"          1:3
NUMBER       "12"         1:5
END          "END"        2:1
共 3 个 Token
```

最后一行是 Token 总数；有词法错误时打印 `词法错误: code=… line=… column=… message=…`
并以退出码 1 结束。示例：`example/lexer_only.gram`。

C++17 的公开面与解析产物同名同风格，只是把 `runText` / `run` 换成 `tokenize`：

```cpp
Engine engine;                      // 命名空间内（默认 Parser::）
std::vector<Token> tokens;
if (engine.tokenize("a + 12", &tokens) == Status::Ok) {
    for (const Token &token : tokens) {
        std::printf("%s\n", tokenName(token.type));
    }
}
```

交出去的 Token 词素指向引擎内部的稳定缓冲（`tokenize` 里拷贝进
`std::deque<std::string>`），只在下一次 `tokenize` 之前有效。

Python 侧就是模块里的词法器类：

```python
import parser

status, tokens = parser.Parser().tokenize("a + 12")
for token in tokens:
    print(parser.token_name(token.type), token.text, token.line, token.column)
```

模块里没有 `Tree` / `parse_tree` / `run_text`；错误在 `lexer.errors`（`ParserError`
列表）与 `last_error` 里，细分码与 C / C++17 一致。

## 12. C# 产物（`--lang csharp`）

产物：`Parser.cs` / `Token.cs` / `Program.cs` / `parser.csproj` / `RESULT.md`。
词法模式同样是这几个文件，只是 `Parser.cs` 里只有词法器与 `tokenize`。

### 12.1 公开 API

公开 API 的类型名首字母大写、方法名与 C++17 / Python 一致：

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

- 类型名：`Parser` / `Token` / `Parser.Tree` / `Parser.ParserError`；入口类按
  C# 惯例叫 `Program`（C# 不允许成员名与类名相同，`Main.cs` 这个名字不可行）；
- `Parser.ParserError`：`code` / `line` / `column` / `message`（`ToString()` 出
  「第 N 行第 M 列：…」）/ `tokenType` / `tokenText` / `state` / `expected`；
- `Parser.Tree`：字段 `name` / `text` / `children`，方法 `childCount()` /
  `child(i)` / `addChild(node)` / `print()`；
- `Token`：`type` / `text` / `value` / `line` / `column` / `offset`，静态
  `Token.tokenName(type)` / `Token.tokenText(token)`；
- 语义动作里的 `yyerrok` / `yyclearin` 翻译成 `yyErrOk()` / `yyClearIn()`；
  `$line` / `$column` / `$token` / `$code` / `$message` 翻译成
  `currentLine()` / `currentColumn()` / `currentTokenText()` /
  `lastErrorCode()` / `lastErrorMessage()`；
- `%package NAME` 生成文件级 `namespace NAME;`，`%import NAME` 生成
  `using NAME;`（`using` 一律排在 namespace 之前，这是 C# 的语法要求）；
- 目标框架是 `net8.0`（生成目录里的 `parser.csproj` 可直接改；模板侧的值写在
  `source/templates/csharp_csproj.xml.tmpl`，模板是纯静态的，没有占位符），只用标准库。
- `glr` 使用 `Dictionary<int, int>` 按输入位置与状态索引图结构栈；`earley` / `cyk`
  先判定原文法 / CNF 能否接受，再重建原产生式的所有语义结果或语法树。
  `runGlr` / `runGlrTree` 返回状态码，`glrResultCount` 与 `glrResult(i)` / `glrTree(i)`
  读取各条路径；不支持 `error` 产生式、`%destructor`，图表模式也不支持 `%sync`。

### 12.2 词法：固定字面量 + .NET 正则

C# 产物的词法器**不使用生成器自带的 DFA**，而是把正则原样交给 .NET 自带的
`System.Text.RegularExpressions.Regex`——`Parser.cs` 的静态字段里各构造一次
（`using System.Text.RegularExpressions;` 由生成器按「产物里有词法器」自动补上）：

| 文法里的声明 | `Parser.cs` 里的表 |
| --- | --- |
| `%token regex NAME "正则"` | `_REGEX_RE_LIST`（配上 `_REGEX_TYPES` / `_REGEX_ORDERS`） |
| `%identifier "正则"` | `_IDENTIFIER_RE`（没写时是 `null`，退回内建的标识符规则） |
| `%commit` / `%trivia "模式"` | `_COMMIT_RE_LIST`（配上 `_COMMIT_TYPES` / `_COMMIT_SKIPS` / `_COMMIT_ORDERS`） |

匹配顺序与其它目标一致：内建字符串 → 固定字面量与上面的正则一起**取最长**，
同长度时字面量优先、再按声明序；`%commit` 命中的片段不进语法层。

两个实现细节：

- `Regex.Match(text, start)` 是**从 start 往后找**、并不锚定在 start 上，
  所以 `_lexRegexMatch()` 还要求 `Match.Index == start`，语义才和原来的 DFA 一致；
- 最长匹配靠 `Regex` 的贪婪语义（与 Python 产物同一套口径），
  见 §10.4 里那两条「`\b` 的含义不同」「分支长度不同的正则按分支顺序取匹配」，
  C# 同样适用（Rust 也是这样，见 §14.4）。`05-grammar.md` 里记的那个
  「`(A|B)*` + 否定类永不匹配」的 DFA 构造坑**在 C# 产物上也不存在**。

生成期校验与 Python 档同一套口径（见 §10.4 末段）：文法加载时用
`source/src/regex_dfa.cpp` 验一遍，**只有 C99 把不通过当硬错误**；C# 产物把正则原样交给
.NET 的 `Regex`，自带编译器认不下时只打警告就照常生成。校验出来的 DFA 不再写进
C# 产物。

### 12.3 C# 产物的行标记

C# 没有可移植的 `#line`（`#line` 只能改行号、不能指到别的文件），
所以「来自 `.gram` 的代码段」前后各插一行注释：

```csharp
            // 在 example/calc.gram 的第 10 行
            result.value = values[0] + values[2];
            // 继续 Parser.cs 的正文
```

| 内容 | 落点 | 前向注释指向 | 与 C 的 `#line` |
| --- | --- | --- | --- |
| 语义动作 | `Parser.cs` 的动作分支 | 动作 `{` 那一行 | 行号完全一致 |
| `%code ... %endcode` | 文件级用户代码区 | 正文第一行 | 一致 |
| `%class_body` 正文 | 解析器类内部 | `%class_body` 的下一行 | 一致 |
| `%epilogue` 正文 | 产物文件末尾 | `%epilogue` 的下一行 | 一致 |
| `%main` 正文 | `Program.cs` | `%main` 的下一行 | 一致 |

后向注释是 `// 继续 <产物文件> 的正文`；注释行尾照样换行、缩进跟着所在块走，
不影响编译。它只是给人看的导航：C# 的编译报错仍然指产物
自己的行号。

## 13. 文法导出（`--lang gram`）

这一档不生成代码，也没有可调用的 API：产物目录里只有文法原文件、它的分析报告
与 `RESULT.md`。

| 文件 | 内容 |
| --- | --- |
| `<原文件名>.gram` | 输入的那份 `.gram`，**逐字节**导出（`%if` 没展开、注释与空行都在）；没给文件时是内建的 `default_grammar.gram`，导出名 `grammar.gram` |
| `<文法名>.analysis.md` | 分析报告：源文法 / 导出文件 / 生成模式 + 冲突消解 + 未消解冲突 + `--report` 那份报告全文（`pl0.gram` → `pl0.analysis.md`） |
| `RESULT.md` | 生成信息（源文法 / 导出文件 / 分析报告名 / 生成模式）+ 产物清单 + 「怎么用」 |
| `%copy` 的文件 | 照旧原样拷进产物目录（没有的话就没有） |

写成一行就是：

```sh
ParseEngine example/pl0.gram --out-dir out --lang gram     # 只要文法 + 报告
ls out                       # pl0.gram  pl0.analysis.md  RESULT.md
```

`<文法名>.analysis.md` 的正文与 `--report FILE` 写出来的是同一份（冲突 /
未消解冲突 / 文法分析报告），所以**不需要**再单独存一份 `--report`；`--mode` 换成 `lalr` /
`ll1` / `lex` 就是另一套分析（`lex` 下没有分析表，报告里是词法一节 +
Token 表）。`%namespace` / `%class` / `%package` / `%import` / `%class_body` /
`%destructor` / `%cpp_only` 在这一档都只提示一句并忽略——不生成代码，它们没有
落点。详见 [`07-codegen.md`](07-codegen.md) §13。

## 14. Rust 产物（`--lang rust`）

产物是一个 **Cargo 工程**：`Cargo.toml` + `src/parser.rs`（词法器 + 分析表 + 运行时
+ 公开 API）+ `src/token.rs`（Token 类型常量 + 词法单元）+ `src/main.rs`（演示入口）
+ `RESULT.md`。词法模式同样是这几个文件，只是 `parser.rs` 里只有词法器与 `tokenize`。

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

**只用标准库，没有第三方依赖**（`cargo build --offline` 就能过）：分析表写成
`static _ACTION: &[&[i32]]` 这样的数组常量，运行时只做查表。

### 14.1 语义值：按值拷贝

Rust 的语义值在分析栈上**按值拷贝**，所以语义值类型要同时满足
`Copy + Default + Debug`：

| 写法 | 产物里的类型 |
| --- | --- |
| 都没写 | `i64`（数字 Token 给整数，其它 Token 给 `0`） |
| `%value_type T` | `T`（原样填进产物，需要自己 `#[derive(Clone, Copy, Default, Debug)]`） |
| `%union { ival: i32, dval: f64 }` | `pub struct ParserValue { pub ival: i32, pub dval: f64 }`，自带 `#[derive(Clone, Copy, Default, Debug)]` |

**成员要按 Rust 的字段写法写**（`ival: i32`），不是 C 的声明（`int ival;`）——
所以 `example/union_demo.gram` 这类 C 文法不能直接出 Rust 产物，冒烟清单里也没列。
`String` / `Vec<T>` / `Box<T>` 这类**非 `Copy`** 类型不适用（编译期会报
`the trait bound ... Copy ... is not satisfied`）。

### 14.2 语义动作与方言

动作按 Rust 表达式翻译（`ActionTranslator` 的 `rustStyle`）：

| 写法 | 展开成 |
| --- | --- |
| `$$` | `result.value`（写了 `%union` 时是 `result.value.<成员>`） |
| `$1` | LR 归约 `values[0]`、LL 展开 `values[1]`（与 Python / C# 一致） |
| `yyerrok` / `yyclearin` | `self.yy_err_ok();` / `self.yy_clearin();` |
| `$line` / `$column` / `$token` / `$code` / `$message` | `self.current_line()` / `self.current_column()` / `self.current_token_text()` / `self.last_error_code()` / `self.last_error_message()` |
| `int32(x)` 这类类型构造 | `((x) as i32)`（Rust 没有 C 风格强制转换） |

语句带分号，动作代码就写在 `impl Parser` 里（所以 `%class_body` 的正文插成一段
`impl Parser { … }`，`%package` 变成 `Cargo.toml` 里的 crate 名、`%import` 变成 `use`）。

### 14.3 边界

- **不支持 `%destructor`**：语义值按值拷贝、丢弃时由所有权自动回收，没有「丢弃时调
  清理代码」这一层；文法里写了会在生成期直接报错（`%lang rust` 或 `--lang rust`）。
- **没有可移植的 `#line`**：报错只给 `src/parser.rs` 自己的行号，所以「来自 .gram 的
  代码段」前后各插一行注释——`// 在 <文法文件> 的第 <N> 行` 指明出处、
  `// 继续 parser.rs 的正文` 说明回到产物自己的正文。
- 不做嵌套 `%union`、不做复杂类型跨语言映射。

### 14.4 词法：固定字面量 + 官方 `regex`

Rust 产物的词法器**不使用生成器自带的 DFA**，而是把正则原样交给 Rust 官方的
[`regex`](https://docs.rs/regex) crate（`Cargo.toml` 里 `regex = "1"`，
`src/parser.rs` 顶部 `use regex::Regex;` / `use std::sync::OnceLock;`）：

| 文法里的声明 | `src/parser.rs` 里的表 |
| --- | --- |
| `%token regex NAME "正则"` | `_REGEX_RES`（`OnceLock<Vec<Regex>>`，配 `_REGEX_TYPES` / `_REGEX_ORDERS`） |
| `%identifier "正则"` | `_IDENTIFIER_RE`（`OnceLock<Regex>`；没写时不生成，退回内建的标识符规则） |
| `%commit` / `%trivia "模式"` | `_COMMIT_RES`（`OnceLock<Vec<Regex>>`，配 `_COMMIT_TYPES` / `_COMMIT_SKIPS` / `_COMMIT_ORDERS`） |

匹配顺序与其它目标一致：内建字符串 → 固定字面量与上面的正则一起**取最长**，
同长度时字面量优先、再按声明序；`%commit` 命中的片段不进语法层。

两个实现细节：

- 正则**懒加载**：每张表第一次被用到时才 `Regex::new` 编译（`OnceLock`），之后一直
  复用；编译不过时 `lex_compile_regex()` 会 panic 出一句「正则 xxx 编译失败：…」，
  比对着「词素对不上」查半天强；
- `Regex::find_at(text, start)` 是**从 start 往后找**、并不锚定在 start 上，所以
  `lex_regex_match()` 还要求 `m.start() == start`，而且返回的是**字符数**（`regex`
  按字节定位，词法表按 `Vec<char>` 下标走）——语义才和原来的 DFA 一致。
  最长匹配靠 `regex` 的贪婪语义（与 C++17 / Python / C# 同一套口径），§10.4 里那两条
  「`\b` 的含义不同」「分支长度不同的正则按分支顺序取匹配」对 Rust 同样适用，
  `05-grammar.md` 里记的那个 DFA 构造坑**在 Rust 产物上也不存在**。

生成期校验与 Python 档同一套口径（见 §10.4 末段）：文法加载时用 `source/src/regex_dfa.cpp`
验一遍，**只有 C99 把不通过当硬错误**；Rust 产物把正则原样交给 `regex`，自带编译器
认不下时只打警告就照常生成。

`%commit` 收集到的片段与位置信息走 `ParserCommit` / `commits()` 与
`ParserLocation` / `current_location()`。

> 需要 Rust 1.70+（`std::sync::OnceLock` 的稳定版本），并且 `regex` 是**唯一的
> 第三方依赖**——第一次 `cargo build` 要联网，之后可以 `cargo build --offline`。

### 14.5 生成器实现上的一条硬规矩

`source/src/code_gen_rust.cpp` 是**「产物文本一律进模板」的样板**：全文没有 `replaceAll`，
也没有一处 `out += "…"` 拼产物，C++ 侧只做「算表 / 算标志位 / 转义字符串字面量」；
文案与排版（含表格的分隔符与缩进、多行正文的缩进）全部由 46 份
`source/templates/rust_*.tmpl` 说了算。

## 15. Go 产物（`--lang go`）

产物是一个 **Go 模块**：`go.mod` + `parser.go`（词法器 + 分析表 + 运行时 + 公开 API）
+ `token.go`（Token 类型常量 + 词法单元）+ `main.go`（演示入口）+ `RESULT.md`。
三个 `.go` 文件都是 `package main`（同一个包，互相直接用对方的符号）；词法模式同样
是这几个文件，只是 `parser.go` 里只有词法器与 `Tokenize`。

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

**只用标准库，没有第三方依赖**：分析表写成 `var sParserAction = [][]int32{…}` 这样的
包级数组，运行时只做查表；内部辅助符号统一带 `sParser` 前缀，对外 API 用导出的 Go
写法（`Parser` / `Token` / `NewParser` / `RunText` / `Result`）。

| 文件 | 内容 |
| --- | --- |
| `go.mod` | `module <path>` + `go 1.21`；module path 来自 `%package`（缺省 `parserdemo`） |
| `parser.go` | 状态码、容量常量、Token 表、分析表、`Parser` 与全部方法、词法器、`parser.go` 末尾追加 `%epilogue` |
| `token.go` | `TOKEN_<名字>` 常量、`TOKEN_NAMES`、`TOKEN_ALIASES`、`Token` 与它的构造函数 / `TokenName` / `TokenText` |
| `main.go` | 入口：内建 demo main（读标准输入一行）或 `%main` 正文（正文自带 `import`） |
| `RESULT.md` | 生成信息、产物清单、位置信息与 `%commit` 表、编译说明 |

### 15.1 公开 API

| 内容 | 写法 |
| --- | --- |
| 状态码 | `StatusOK`(0) / `StatusErrStack`(-20) / `StatusErrSyntax`(-21) / `StatusErrLexer`(-22) / `StatusErrExpected`(-26) / `StatusErrUnexpectedEOF`(-27) / `StatusErrLexUnterminated`(-28) / `StatusErrLexChar`(-29)…（数值与其它档一致），取名字用 `StatusName(status)` |
| 构造与入口 | `NewParser()`；`RunText(text string) int32`、`Run(tokens []Token) int32`、`ParseTree(text string) *Tree`（只建树、不跑动作） |
| 结果与错误 | `Result()`、`Errors() []*ParserError`、`LastError() *ParserError`（`ParserError` 实现 `error` 接口，文本是「第 N 行第 M 列：…」） |
| 位置与 `%commit` | `CurrentLocation() ParserLocation`、`Commits() []ParserCommit` |
| Token | `Token{Type int32; Text string; Value <语义值类型>; Line, Column, Offset int32}`、`TokenName(kind)`、`TokenText(item)` |
| 词法模式 | `Tokenize(text string) (int32, []Token)`（与其它档一致，末尾带一个 `TOKEN_END`） |

`Parser` 同一个实例可以连续解析多次：每次 `RunText` / `Run` / `ParseTree` 都会先清空
上一次的错误表与结果。

### 15.2 语义值：按值拷贝

| 写法 | 产物里的类型 |
| --- | --- |
| 都没写 | `int64`（数字 Token 给整数，其它 Token 给零值） |
| `%value_type T` | `T`（原样填进产物，要能零值初始化、能赋值） |
| `%union { int64 ival; }` | `type ParserValue struct { ival int64 }` |

**成员照 C 的声明写**（`int64 ival;`），生成器拼成 Go 的字段写法（`ival int64`）
——这一点与 C / C# 一致，Rust 档才是 `ival: i32` 那种写法。语义值在分析栈上按值
拷贝，`int64` 这类标量、`ParserValue` 这类结构体、切片 / map 这类引用类型都能用
（引用类型拷贝的是引用，动作里要注意多个槽位共享同一份底层数据的情况）。

词素→语义值的转换集中在生成出来的两个函数里：`valueFromText(kind, text)` 与
`valueFromNumber(number float64, isFloat bool)`。

### 15.3 语义动作与方言

动作按 Go 表达式翻译（`ActionTranslator` 的 `goStyle`）：

| 写法 | 展开成 |
| --- | --- |
| `$$` | `result.value`（写了 `%union` 时是 `result.value.<成员>`） |
| `$1` | LR 归约 `values[0]`、LL 展开 `values[1]`（与 Python / C# / Rust 一致） |
| `yyerrok` / `yyclearin` | `p.YyErrOk()` / `p.YyClearIn()`（动作代码写在 `*Parser` 的方法里，接收者固定叫 `p`） |
| `$line` / `$column` / `$token` / `$code` / `$message` | `p.CurrentLine()` / `p.CurrentColumn()` / `p.CurrentTokenText()` / `p.LastErrorCode()` / `p.LastErrorMessage()` |
| `int32(x)` 这类类型构造 | `int32((x))`（Go 的转换是函数式写法） |

语句带分号；`%package` 变成 `go.mod` 的 module path、`%import` 进 import 块、
`%class_body` 的正文贴成一组 `func (p *Parser) …` 方法（Go 没有 class 这一层）。

### 15.4 边界

- **不支持 `%destructor`**：语义值由 GC 回收，没有「丢弃时调清理代码」这一层；
  文法里写了会在生成期直接报错（`%lang go` 或 `--lang go`）。
- **没有可移植的 `#line`**：报错只给 `parser.go` 自己的行号，所以「来自 .gram 的
  代码段」前后各插一行注释——`// 在 <文法文件> 的第 <N> 行` 指明出处、
  `// 继续 parser.go 的正文` 说明回到产物自己的正文（与 Rust 档同一套做法）。
- `%code ... %endcode` 的正文插在 import 块之后，**正文里不要再写
  `import`**（要加包请用 `%import`）；`%main` 的正文要自带 `import` 与 `func main()`
  （外壳只给文件头注释与 `package main`）。
- `%import` 的名字里可以用 `::` 当路径分隔符（归一成 `/`），与标准库重复的包会被
  跳过（Go 里重复 import 同一个包是编译错误）；不做嵌套 `%union`、不做复杂类型
  跨语言映射。

### 15.5 词法：固定字面量 + 标准库 `regexp`

Go 产物的词法器**不使用生成器自带的 DFA**，而是把正则原样交给 Go 标准库的
[`regexp`](https://pkg.go.dev/regexp)（RE2 实现，`regexp.MustCompile` 在包级变量初始化
时各编译一遍）：

| 文法里的声明 | `parser.go` 里的表 |
| --- | --- |
| `%token regex NAME "正则"` | `sParserRegexRes`（`[]*regexp.Regexp`，配 `sParserRegexTypes` / `sParserRegexOrders`） |
| `%identifier "正则"` | `sParserIdentifierRe`（没写时不生成，退回内建的标识符规则） |
| `%commit` / `%trivia "模式"` | `sParserCommitRes`，配 `sParserCommitTypes` / `sParserCommitSkips` / `sParserCommitOrders` |

匹配顺序与其它目标一致：内建字符串 → 固定字面量与上面的正则一起**取最长**，同长度
时字面量优先、再按声明序；`%commit` 命中的片段不进语法层（`skip` / `%trivia` 命中
即丢，其余按出现顺序进 `Commits()`）。

两个实现细节：

- `regexp` 的 `FindStringIndex` 是**从起点往后找**、并不锚定，所以
  `sParserLexRegexMatch()` 还要求命中真的从起点开始；返回的是**字符数**（`regexp`
  按字节定位，词法表按 `[]rune` 下标走）——语义才和原来的 DFA 一致。最长匹配靠
  `regexp` 的贪婪语义，§10.4 里那两条「`\b` 的含义不同」「分支长度不同的正则按分支
  顺序取匹配」对 Go 同样适用。
- 有正则时才 import `regexp`（Go 对「imported and not used」是硬错误），所以
  「没写任何正则」的产物里既没有正则表、也没有 `regexp` 这一行。

生成期校验与 Python / Rust 档同一套口径（见 §10.4 末段）：文法加载时用
`source/src/regex_dfa.cpp` 验一遍，**只有 C99 把不通过当硬错误**；Go 产物把正则原样交给
`regexp`，自带编译器认不下时只打警告就照常生成。

### 15.6 生成器实现

`source/src/code_gen_go.cpp` 与 Rust 档同一套做法：全文没有 `replaceAll`、没有 `out += "…"`
拼产物，产物片段一律走模板渲染（生成器只做「算表 / 算标志位 / 转义字符串字面量」）；
文案与排版全部由 53 份 `source/templates/go_*.tmpl` 说了算。

### C++17 LR 归约行号钩子

在用户头中定义 `PARSER_USER_REDUCTION_LINE(line)`，C++17 LR 驱动会按需编译符号起始行栈，并在语义动作之前调用该宏。非空产生式使用第一个右部符号的起始行，空产生式使用当前 token 行；未定义宏时不增加位置栈开销。Magel 用它给语法节点登记预处理后的行号，再通过预处理来源映射还原原始文件和行。该钩子用于普通 LR 文本分析，LL/GLR 和增量检查点的来源持久化不在此接口范围内。
