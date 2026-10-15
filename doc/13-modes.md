# 10 生成模式与运行时（用户手册）

本章从 [`README.md`](../README.md) 拆出：16 种生成模式（LL 家族 / LR 家族 / GLR / Earley / CYK /
递归下降 `dd` / `lex`）、教学模式、Mermaid 状态机图与词法匹配顺序。

各模式的形式化定义、自动机构造与冲突处理见 [`06-analysis.md`](06-analysis.md)，
产物侧的 API 见 [`08-generated-code.md`](08-generated-code.md)。
各语言模式与增量支持范围统一见 [12 各语言支持的模式](15-language-mode-support.md)。

## LR(0) / LR(1) 规范自动机模式

`%mode lr0` / `%mode lr1`（或命令行 `--mode lr0` / `--mode lr1`）把教材里最常
被拿来对照的两套自动机原样算出来：

| 模式 | 项集族 | 归约时机 | 用途 |
| --- | --- | --- | --- |
| `lr0` | 规范 LR(0) 项集（`[A -> α . β]`） | 完整项在**所有**终结符上归约，不看 FOLLOW | 讲 LR 分析的基本套路：闭包、GOTO、ACTION/GOTO 表怎么填 |
| `lr1` | 规范 LR(1) 项集（`[A -> α . β, a]`），**不合并**同心状态 | 只在项自带的展望符上归约 | 讲「展望符从哪来、合并省多少状态、代价是什么」 |

这两个模式和别的模式一样**不需要任何开关**：`%mode lr0` / `--mode lr0` 选到就能
生成代码、能编译能跑。它们只是从不因未消解冲突中止生成——冲突写进报告的
`[未消解冲突]`，表项按 yacc 默认处理（移进/归约取移进，归约/归约取先到的归约），
终端上再打印一条警告。所以二义表达式文法也能用 `--mode lr0` 完整看到自动机与
冲突本身。

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

这两个模式的产物与其它模式**完全一致**（分文件工程、`Makefile` / `CMakeLists.txt`、
`%tree`、语义动作、LR 运行时都复用同一套），
区别只在分析表的来源与报告内容。`--dump-lr-automaton` 下 `lr1` 按教材写法一项
一行（`[A -> α . β, a]`），`lr0` 与 SLR 一样打印 LR(0) 项集族。

算法与形式化定义见 [`06-analysis.md`](06-analysis.md) §5。

## 词法模式（`%mode lex`）

`%mode lex`（或命令行 `--mode lex`）**只导出词法器**：产物里没有分析栈、分析表，
也没有解析入口；这种文法可以一条产生式都不写（其它模式必须有规则）。

```text
%mode lex

%identifier "[A-Za-z_][A-Za-z0-9_]*"
%token KEYWORD_IF "if"
%token regex FLOAT "[0-9]+\.[0-9]+"
%define PLUS "+"
```

六种目标语言都支持（同一条 `--lang` / `%lang` 规则），产物里都只有词法器：

| 目标 | 产物 | 词法器入口 |
| --- | --- | --- |
| C99（`--lang c`） | `lexer.h` / `lexer.c` / `parser_token.h` / `parser_token.c` / `main.c` | `Parser_LexInit` + `Parser_NextToken`（`ParserToken_T`） |
| C++17（`--lang cpp17`，缺省） | `lexer.hpp` / `lexer.cpp` / `main.cpp` | `Engine::tokenize`（`std::vector<Token>`） |
| Python（`--lang python`） | `parser.py` / `main.py` | `Parser().tokenize`（Token 列表） |
| C#（`--lang csharp`） | `Parser.cs` / `Token.cs` / `Program.cs` / `parser.csproj` | `new Parser().tokenize(text, tokens)`（`List<Token>`） |
| Rust（`--lang rust`） | `Cargo.toml` + `src/parser.rs` / `src/token.rs` / `src/main.rs` | `Parser::new().tokenize(text)`（`Vec<Token>`） |
| Go（`--lang go`） | `go.mod` + `parser.go` / `token.go` / `main.go` | `NewParser().Tokenize(text)`（`[]Token`） |

六档都带 `RESULT.md`（产物清单 +
Token 表：固定字面量 / 正则 / `%identifier`）；C / C++17 的构建产物是
`liblexer.a` + `lexer_demo`，C# 直接用 `dotnet build` 编译，Rust 用
`cargo build --offline`，Go 用 `go build`。
**都不生成解析器**：C 没有 `parser.h` / `parser.c` /
`parser_table.c`，C++17 没有 `parser.hpp` / `parser.cpp`，Python 的 `parser.py` 里
没有分析表 / 分析栈 / `run_text`，C# 的 `Parser.cs` 里
没有分析表 / 分析栈 / `runText`，Go 的 `parser.go` 里
没有分析表 / 分析栈 / `RunText`（只有 `Tokenize`）。

`--lang gram`（[文法导出档](14-languages-and-products.md#文法导出--lang-gram)）也接受
`--mode lex`，但那一档连词法器都不生成：产物里是 `.gram` 原文件 + `RESULT.md`，
报告里列出词法一节与 Token 表（固定字面量 / `%token regex` / `%identifier`）。

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

当库用（三份产物各自的样子）：

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

几个约定：

- **六档都支持**：C / C++17 的 Token 名与词素接口一一对应（`Parser_TokenName`
  ↔ `tokenName`），Python 的 `token_name`、C# 的
  `Token.tokenName`、Rust 的 `token_name`、Go 的 `TokenName` 同理；
  错误细分码各档一致
  （`-28` / `-29`）。
  C / C++17 的 `tokenize` / `Parser_NextToken` 返回的 Token 词素指向内部缓冲，
  只在**下一次取 Token 之前**有效（要长期保存请自己拷贝）；Python / C# / Rust / Go
  侧是普通字符串（不用管）。
- **解析相关的写法会被忽略**：产生式、`%start`、`%tree`、`%sync`、
  `%destructor`、`%error_context` 不参与产物，生成时打一句「已忽略：…」的提示；
  `%left` / `%right` / `%nonassoc` 仍然按 bison 的规矩声明 Token。
- 词法相关的写法照常生效：`%token`（固定字面量 / `regex`）、`%define`、
  `%identifier`、`%space`、`%union` + `%token <成员>`、`%code ... %endcode`、`%main`、
  `%epilogue`、`%copy`。
- 词法错误沿用细分码：`-29` 认不出来的字符、`-28` 字符串没有闭合；三份产物的 CLI
  都打印一行说明并以退出码 1 结束。
- 例子：[`example/lexer_only.gram`](../example/lexer_only.gram)。

## 教学模式（教学提示与教学对照）

**教学模式是独立于生成模式的另一个开关**：文法里写 `%teach` 或命令行加
`--teach`（`--no-teach` 反过来关掉文法里的 `%teach`），缺省**关闭**。开了以后：

1. 报告里多出 `[教学提示]`（该文法是不是 X 文法、冲突按种类计数）——哪怕这个
   模式本来就不中止，这一节也会把「为什么不是 X 文法」说清楚。
2. 报告里多出 `[教学对照]`：
   - `lr0`：`LR(0) 状态数`、`LR(0) 表未消解冲突数`；
   - `slr`：`LR(0) 状态数`、`SLR(1) 表未消解冲突数`；
   - `lalr`：`规范 LR(1) 状态数`、`LALR 状态数`、`LALR 表未消解冲突数`；
   - `lr1`：`规范 LR(1) 状态数`、`LALR 同心合并后状态数`——两个数之差就是
     同心合并省下来的状态；
   - `ielr`：拆分前后的状态数、状态拆分数、`IELR 表未消解冲突数`;
3. 顺带把 `slr` / `lalr` / `ielr` 也改成「只报不中止」：不写 `%teach` 时这三个
   模式严格，未消解冲突直接报错、不产出代码（见[冲突处理与优先级](12-grammar-usage.md#冲突处理与优先级)）。

`ll1`（没有冲突可讲）下 `%teach` 会被忽略，并在终端上说明一句。教学模式只是
报告层的开关，不改动产物内容；给自动机
配图是另一个开关，见[状态机图示（Mermaid）](#状态机图示mermaid)。

```sh
# slr：不开教学模式直接报冲突并中止（用二义表达式文法试）
# 加上 --teach 才只报不中止，并在报告里说明为什么不是 SLR(1)

# --no-teach 可以压掉文法里的 %teach（example/lr0.gram 自带 %teach）
./build/bin/ParseEngine --grammar-file example/lr0.gram --out-dir /tmp/lr0 --no-teach
```

## 状态机图示（Mermaid）

`%mermaid`（或命令行 `--mermaid` / `--no-mermaid`）把生成期算出来的状态机画成
[Mermaid](https://mermaid.js.org/) 图，随产物写进 `mermaid/` 目录；缺省**关闭**。
它和教学模式（`%teach`）**互不相干**：不开 `%teach` 也能出图，开了 `%teach` 也
不会自动出图。

| 文件 | 内容 |
| --- | --- |
| `mermaid/index.html` | 总览：所有图内嵌在一页里，带图例与互链 |
| `mermaid/grammar.html` | 文法自动机：LR 状态 = LR 项集、边 = GOTO 转移（终结符是移进、非终结符标 `GOTO`），状态按「移进 / 归约 / 接受 / 未消解冲突」上色 |
| `mermaid/lexer.html` | 词法 DFA：每个 `%token regex` / `%identifier` 一张，双圆是接受状态，边上是字符类（`a-z` 这样压缩过的区间） |
| `mermaid/*.mmd` | 上面两张图的 Mermaid 源码（`%% ====` 分隔每张图），可直接贴进 Markdown 或 mermaid.live |

- **画哪些图**：文法自动机对 LR 家族（`lr0` / `slr` / `lalr` / `lr1` / `ielr`）
  都画；`ll1` 的分析表不是状态机、没有文法图。词法 DFA 与模式无关，只要
  文法里有 `%token regex` / `%identifier` 就画。
- **没有图就没有目录**：`ll1` 且没有正则 Token 时不会出现 `mermaid/`。
- **看图**：用浏览器直接打开 `mermaid/index.html` 就行（`file://` 本地打开也行），
  页面从 CDN 加载 Mermaid，首次打开需要联网；没网时每张图下面的
  「Mermaid 源码」折叠块与 `.mmd` 文件仍然可读可复制。
- **不改产物行为**：状态机图示只是多出来的文件，`parser.c` / `Makefile` 等产物
  照常 `make` / 跑 demo。
- **只想看一眼**：`--dump-mermaid` 把同样的源码按 Markdown 围栏格式打到标准输出
  （`> diagrams.md` 重定向出去就是一份带图的 Markdown）。它在任何模式下都能用；
  没有可画的图时提示一句、什么也不输出。
- **图太大就省略**：真实文法的自动机可能大到没法学（`example/c89_subset.gram`
  的 LALR 自动机有 340 个状态、3117 条转移，规范 LR(1) 有 1691 个状态、
  16586 条转移），超过 150 状态 / 400 条边 / 120 KB 时图里只写一句说明，
  完整项集族看分析报告或 `--dump-lr-automaton`。

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

## LR(k) / LALR(k) / SLR(k) 模式

一个展望符不够用时（LALR(1) 报冲突、而更长的窗口就能区分开），把模式换成
`lrk` 或 `lalrk`，再用 `%lookahead N` / `--lookahead N` 指定 k：

```sh
./build/bin/ParseEngine --grammar-file example/lrk_demo.gram \
    --mode lrk --lookahead 2 --report /tmp/lrk2.txt --out-dir /tmp/lrk2
```

- **k 取 0..INT_MAX（没有策略上限），缺省 2**；k = 1 时 `lrk` / `lalrk` / `slrk`
  分别等价于 `lr1` / `lalr` / `slr`。**大 k 代价自负**：`FIRST_k` / `FOLLOW_k`
  与列数都随 k 指数级膨胀，k > 3 生成时会提醒一句；列编码
  （$\sum_i \mathrm{Token号}_i\cdot(\text{Token 个数})^i$）放不进 64 位时生成期
  直接报错并给出这个文法的上限。
- **两层 ACTION 表**：移进只看下一个 Token（还是单 Token 的列），归约按 k 元组
  展望符建列；运行时维护 k 个 Token 的窗口，先查归约列、再按窗口头查移进。
  这样的列数是「文法里真正用到的 k 元组个数」，不是 `Token 数^k`。
- **`lrk` 与 `lalrk` 的差别就是合不合并同心状态**：合并省状态，但会把不同上下
  文的 k 元组并到一起，从而引入 LALR(1) 那种归约/归约冲突——**加长 k 也去不掉**
  （`example/lrk_demo.gram` 对任何 k 都不是 LALR(k)）。这条边界比「k 越大越强」
  更值得记住：$\mathrm{LALR}(k)\subsetneq\mathrm{LR}(k)$。
- 冲突处理与 LR(1) 一致：`lrk` 只报不中止，`lalrk` 默认严格（`%teach` /
  `%expect` 可放行）；`%prec` 是单 Token 概念，k > 1 时按 k 元组的**首个** Token 判定。
- 错误恢复（`error` 产生式 / `%sync`）在 k > 1 下照常工作：`error` 顶在窗口头上、
  后面 k-1 个仍是真实 Token；`%sync` 的锚点扫描还是单 Token。
- `%lookahead` 配 `ll1` / 其它 LR 模式直接报错（不静默忽略）：自顶向下
  （LL(1)）不需要 k，其余 LR 模式固定 k = 1。
- **增量友好入口**：C++17 / Rust 的表驱动 LR 产物（LR0 / SLR / LALR / LR1 /
  IELR / LR(k) / LALR(k) / SLR(k)，不含 GLR / Earley / CYK）会生成
  `IncrementalCache` 与 `runTextIncremental` / `run_text_incremental`。输入文本
  完全一致时直接复用上次状态、语义结果、错误表与 `%commit` 片段；文本变化时先
  重扫 token，与缓存 token 做公共前缀匹配，再从公共前缀内最近的
  `checkpointInterval` / `checkpoint_interval` 栈快照恢复 LR 栈并只重跑后缀。
  如果恢复路径失败，会自动回退到完整解析并刷新 token cache 与 checkpoint。
  这仍是“checkpoint 后缀重跑”，不是完整 LR 增量解析：完整方案要继续处理状态栈
  局部修补、错误恢复状态延续、语义动作副作用（需要纯度约束或禁用缓存复用）以及
  CST 修补。

**模式名怎么写都行。** 大小写随意，也可以把 k 直接写进括号（命令行 `--mode`
与文法里 `%mode` 共用同一套写法）：

```sh
--mode LL(1)      # 等价 ll1
--mode 'LL(2)'    # 等价 llk + --lookahead 2（shell 里带括号要加引号）
--mode LL(k)      # 等价 llk（k 走 %lookahead 或缺省 2）
--mode LALR       # 等价 lalr
--mode 'LALR(k)'  # 等价 lalrk
--mode LR(0)      # 等价 lr0
```

## LL(k) / LL(0) 模式

自顶向下那条路：按 k 个 Token 选产生式（`Select_k(A→α) = FIRST_k(α·FOLLOW_k(A))`），
判定对象是**消除直接左递归之后**的文法，所以 `calc` 这类左递归文法在 LL(k) 下也
能生成。k = 1 时与 `ll1` 逐字节等价（只有模式名不同）。

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

| 写法 | 含义 |
| --- | --- |
| `%mode LL(0)` / `ll0` / `LL0` | LL(0)，等价于 `llk --lookahead 0` |
| `%mode LL(1)` / `ll1` / `LL1` | LL(1)（就是原来的 `ll1`） |
| `%mode LL(k)` / `llk` / `LLK` | LL(k)，k 由 `%lookahead` / `--lookahead` 给（缺省 2） |
| `%mode LL(2)` / `LL2` | LL(2)，不用再写 `--lookahead` |

产物的预测分析表按列建：k = 1 是 Token 密表（与从前一致）、k = 0 只有一列、
k ≥ 2 是「用到的 k 元组」各一列（列编码二分查，不是 `Token 数^k`）。运行时多一个
k Token 的窗口，**公开 API 与错误恢复语义不变**。

**增量入口**：C++17 / Rust 的 LL 产物会生成和 DD 相同的保守增量 API：
C++17 是 `IncrementalCache` + `Engine::runTextIncremental(...)`，Rust 是
`IncrementalCache` + `Parser::run_text_incremental(...)`。当前策略是输入文本完全
一致时复用上次状态、语义结果、错误表与 `%commit` 片段；文本变化时完整重算并刷新缓存。

示例文法：[`example/lrk_demo.gram`](../example/lrk_demo.gram)。同一份文法的冲突对照
（`lr0` 9 / `slr` 2 / `lalr` 2 / `lr1` 0 / `ielr` 0 / `lrk` 0 / `lalrk` 2）与
构造细节见 [`06-analysis.md`](06-analysis.md) §2.12。

## IELR(1) 模式

`%mode ielr`（或命令行 `--mode ielr`）在 LALR 的基础上按需拆分状态：生成器先
构造 LR(0) 项集并算出 LALR lookahead，只有当某个状态出现 LALR 特有的未消解
归约/归约冲突时，才把该状态按「每个 LR(1) 子状态在该 Token 上启用的归约项」
分组拆开，再做 goto 一致性细化，然后重建分析表。因此：

- **能力等同规范 LR(1)**：所有 LR(1) 文法都能生成，包括 LALR(1) 会报归约/归约
  冲突的非 LALR 文法（见 `example/ielr_demo.gram`、`example/ielr_split.gram`；
  后者需要两个独立拆分：LALR 会报归约/归约冲突，IELR 能正常生成并解析）。
- **规模接近 LALR(1)**：只拆分真正冲突的状态，不展开整张规范 LR(1) 表，
  状态数与 ACTION/GOTO 表规模与 LALR 同量级，远小于规范 LR(1)。
- **对 LALR 文法零代价**：没有未消解归约/归约冲突时不做任何拆分，输出与
  LALR 完全一致（`calc` / `pl0` / `c89_subset` 都能核对 IELR 与 LALR 的分析表
  是否等价）。
- **复用全部 LR 机制**：优先级 / `%prec` / `%expect`、`%tree`、
  `$n` 语义动作、LR 运行时与产物模板都与 SLR/LALR 共用。

分析报告里会多出 `[IELR 自动机]`（LALR 状态数 / 拆分后 IELR 状态数 / 规范
LR(1) 状态数 / ACTION 表项数 / GOTO 转移数）与 `[IELR 状态拆分]`（哪个状态被拆分、被哪个 Token 上的哪类
冲突触发、为什么）两节；`RESULT.md` 里也有对应的「IELR(1) 状态拆分」小节，
`--dump-lr-automaton` / `--dump-slr-tables` 同样支持 IELR。

**什么时候用 IELR(1)**：文法本身就是 LALR(1) 时用 LALR 即可；只有当 LALR
报出归约/归约冲突、而你又不想改写文法，或想确认问题是不是出在 LALR 的状态
合并上时，改用 IELR(1)。算法细节与形式化定义见
[`06-analysis.md`](06-analysis.md)。


## GLR（图结构栈）

`%mode glr`（或 `--mode glr`）保留 ACTION 表单元里的**冲突候选**，运行时用
图结构栈（GSS）把同一个输入位置上的多条路径一起探索：规则是「同时移进、同时
归约」，能让歧义文法（例如 `example/glr_dangling_else.gram`、
`example/english-clauses.gram` 与 `example/glr_expr.gram`）把**全部解释**都列出来，
而不是像 LR 那样靠优先级 /
`%prec` 挑一条。

- **产物侧**：GLR 是运行时特性，只有生成 GSS 运行时的档能用，当前是
  **C++17 / Python / C# / Rust / Go 五档**；其余档写 `--mode glr` 会在生成期直接
  报错挡下来，不会静默降级。
- **结果缓存**：C++17 / Python / C# / Rust / Go 五档都用动态容器保留被接受路径；
  默认不再设置固定结果上限。
- **错误恢复**：GSS 多路径探索用于保留歧义解释；所有活跃路径都无法移进时，运行时
  可以删词恢复，`%sync` 可作吞 Token 的边界。GLR **不支持** `error` 产生式与 `%destructor`。
- **结果管理**：`%code ... %endcode` 里可以注册歧义解决回调（`set_ambiguity_resolver`），
  拿到「已有结果 / 候选结果」自己决定要不要采用。
- **增量实验**：C++17 / Rust 已提供实验缓存，当前能力与限制如下。
  C++17 / Rust 产物提供实验缓存 `GlrIncrementalCache`；C++17 入口为
  `runGlrIncremental(...)` / `runGlrTreeIncremental(...)`，Rust 入口为
  `run_glr_incremental(...)` / `run_glr_tree_incremental(...)`。当前缓存 token span、
  token diff 窗口、reduce 结果 span 索引、受变更窗口影响的旧 reduce 条目，以及
  GSS 规模、reduce 访问量与带稳定 ID 的 packed forest；非 tree 模式在输入文本完全一致时
  只有显式设置 `cache.semanticReuse = GlrSemanticReusePolicy::Pure` 才可复用成功的
  语义结果（Rust 字段为 `semantic_reuse`）；默认 `Disabled` 每次执行动作，仍采集
  结构缓存。文本变化时局部修补 forest，GSS 控制流程仍完整重跑。
  Rust 的 `GlrIncrementalCache::new()` / `default()`
  均使用这一默认策略。
  `Pure` 是调用方的契约：语义动作、词法钩子以及结果复制必须无可观察副作用，结果
  不得依赖未声明的可变状态或已经失效的指针。文件 IO、进程启动、日志、计数器和
  符号表写入等动作应保持 `Disabled`；引擎不会自动推断纯度，也不会重放副作用。
  外部语义环境变化时递增 `cache.semanticRevision`（Rust 为 `semantic_revision`）
  或调用 `cache.clear()`；跨引擎
  使用缓存也必须满足同一语义环境契约。`clear()` 保留策略与当前环境版本。
  从禁用策略切到 `Pure` 后会先重新解析，不能复用禁用期间的语义结果。
  有歧义解决回调、解析失败或 tree 模式时不复用语义结果；
  `cache.reusedSemanticResults`（Rust 为 `reused_semantic_results`）表示本次是否命中。
  Rust 用 `clear_ambiguity_resolver()` 移除歧义闭包。
  `invalidatedReduceEntries`（Rust 为 `invalidated_reduce_entries`）指向旧缓存的
  reduce 条目。forest 已实现父依赖传递失效、未变节点 ID 复用、后缀跨度重映射、
  候选重验证和不可达节点清理；失效边数量是失效旧节点的 child 引用数。
  即使声明纯动作，GLR 在一次完整
  解析中仍可能对多个候选分支执行动作，本策略不提供事务或恰好执行一次保证。
  tree 模式局部复用的是 packed forest 的共享符号节点和归约候选，公开的 `Tree`
  仍重新物化。GSS checkpoint 恢复、CST 原位修补与错误恢复状态延续尚未实现。
  新增缓存字段及指标定义见 [12 的 Forest 修补接口](15-language-mode-support.md#forest-修补接口)。

## Earley 与 CYK 图表解析

`%mode earley` / `--mode earley` 用预测、扫描、完成三步在每个输入位置建立
Earley 项集，直接处理左递归、空产生式和二义文法。`%mode cyk` / `--mode cyk`
在**生成期**将文法消除空产生式与单元规则、替换长右部里的终结符并二分长规则；
运行时按输入跨度填 CYK 表。空串是否可接受由原起始符号的可空性单独记录。

两种模式目前支持 **C++17 / Python / C# / Rust**。图表负责判定输入能否由原文法
推导；成功后复位词法器，复用 GLR 的 GSS 再解析一次，以保留全部歧义树并执行
原产生式的语义动作。因此接口与 GLR 相同（含 `%tree`、多结果 API、Python 的
语法图绘制），代价是多一次词法与解析。CNF 转换只服务于判定，**不会改写**
用户看到的语法树，也不会给合成规则执行动作。

这两种模式沿用 GLR 结果运行时的限制：不支持 `error` 产生式与 `%destructor`。
图表判定严格检查整段输入，不做 GLR 的删词恢复，因此也不支持 `%sync`；
词法错误直接返回，语法判定失败返回 `STATUS_ERR_SYNTAX`。可用
[`example/chart_nullable.gram`](../example/chart_nullable.gram) 检查空产生式、
单元规则、长右部与左递归；英语歧义示例可用
`--mode earley` 或 `--mode cyk` 覆盖文法里的 `%mode glr`，两种模式都返回两棵树。

Earley/CYK 的 C++17 / Rust 产物也暴露 GLR 实验增量入口，限制与 GLR 相同：
输入变化后仍重建判定图表与 GSS，但会局部修补共享 forest；尚未实现图表局部更新。
完整支持矩阵见 [12](15-language-mode-support.md)。

## 递归下降（`%mode dd`）

`%mode dd`（或 `--mode dd`）**不在生成期建表**：产物里每个非终结符对应一段
「依次试它自己的产生式」的解析逻辑，匹配失败就把词法器位置、Token 下标与行列
一起回退，再试下一条。适合「文法有歧义、但你希望**取法完全由产生式的书写顺序
决定**」的场合。

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

**文法要求与取舍**：

- **不能有左递归**（直接或间接）。生成期会把「跳过可空前缀之后仍以自己开头」的
  环路找出来报错，并给出改写建议 —— 把 `expr := expr PLUS term` 改写成
  `expr := term PLUS expr` 这种右递归，或改用 `--mode lalr` / `ielr`。
- **产生式的书写顺序就是尝试顺序**：先试写在前面那条。歧义文法只保留**第 1 条
  匹配路径**的结果（与 GLR「列全部解释」相反），所以把最想要的那条写在前面。
- **回溯是「同一条产生式右部走到一半失败」时发生的**：某个非终结符**已经成功**
  返回之后，调用方不会再要求它换一种解析方式。因此 `%tree` 的输出对同一输入是
  确定的，但它不保证穷尽所有解释。
- **不支持的指令**（生成期直接报错，不静默忽略）：error 产生式（递归下降没有
  「弹栈到最近的恢复点」这个动作）、`%destructor`（回溯会把同一条产生式试很多
  遍，哪些值被丢弃没有确定答案）、动作里的 `yyerrok` / `yyclearin`（没有分析栈）。
  `%sync` 仍可用：整段解析失败时把它当锚点，跳到下一个同步 Token 之后重试一段
  （panic mode，有预算上限）。
- **被忽略的指令**：`%left` / `%right` / `%nonassoc` / `%prec` 与 `%expect` 在
  递归下降里没有落点（不建表、没有「同一格里有几个动作要挑一个」的场合），
  生成期会在分析报告里提示一句。

**产物侧**：六档全支持（C99 / C++17 / Python / C# / Rust / Go），产物形态与
该语言其它模式一致：

- 语义动作的 `$n` 与 LL 的语义帧同一个约定（第 0 个槽位留空），`%tree` 走
  与该档 LL(1) 相同的建树节点类型；
- 语法错误按「**最远失败点**」报一条：回溯不会把「试探过程中失败的期望」刷进
  错误表，最终失败时报的是「走得最远的那一次在期望哪些 Token」，例如
  `语法错误：期望 LPAREN / NUMBER 之一，实际是 END`；
- 起始符号那一次解析额外要求「匹配完之后紧接着是输入末尾」，所以
  `1 + 2` 不会因为前面某条产生式先匹配上而漏掉后面的 Token。
- **LL / DD / 确定性 LR 的增量入口只给 C++17 / Rust 产物**：C++17 生成
  `IncrementalCache` + `Engine::runTextIncremental(...)`，Rust 生成
  `IncrementalCache` + `Parser::run_text_incremental(...)`。LL / DD 是保守失效：
  输入文本完全一致时复用上次状态、语义结果、错误表与 `%commit` 片段；文本变化时
  自动完整重算并刷新缓存。确定性 LR 模式额外维护 token cache 与前缀栈 checkpoint，
  文本变化后会从公共 token 前缀内最近的 checkpoint 恢复并只重跑后缀；恢复失败时
  再回退完整解析。这不是完整 LR 增量解析；完整方案需要处理状态栈局部修补、错误
  恢复状态延续、语义动作副作用纯度约束 / 禁用策略以及 CST 修补。
  C++17 / Rust 的 GLR 另有上述实验 API，其它语言暂不生成增量 API。

**什么时候用递归下降**：文法本身有歧义、而你要「按声明顺序取第 1 条」这种
可预期的取法（典型是刻意保留二义性、靠顺序消歧的小 DSL）；要枚举全部解释或
做更细的歧义消解，用 GLR。

## Python 产物的绘图（`--svg` / `--dot`）

Python 档的产物自带**自绘 SVG** 的能力（只用标准库、不装第三方模块、布局确定性）：
**语法树绘图**只给 `lalr` / `lr1` / `ielr` 三档，**语法图**给 `glr` / `earley` / `cyk`
（图里的内容见下）。演示程序认 `--svg <路径>` / `--dot <路径>`，不给开关时输出与从前
**逐字相同**；白名单外的模式产物里连这些方法都没有（生成期决定）。

```sh
# LALR(1) 档：语法树（example/pl0.gram，输入 x := 1 + 2 * 3 .）
python main.py --svg tree.svg --dot tree.dot

# GLR 档：语法图（GSS + 各结果树）
python main.py --svg graph.svg --dot graph.dot
```

![LALR(1) 档的语法树（example/pl0.gram，输入 x := 1 + 2 * 3 .）](../image/python-tree-example.svg)

![GLR 档的语法图：左边 GSS（金色高亮 = 被接受的节点），下面并排两条歧义路径的结果树](../image/python-syntax-graph-example.svg)

| 能力 | 适用模式 | 接口 | 画什么 |
| --- | --- | --- | --- |
| 语法树绘图 | `lalr` / `lr1` / `ielr` | `Tree.to_svg()` / `to_dot()` / `write_svg(path)` / `write_dot(path)` | 整齐树布局：框里第一行符号名、第二行词素（叶节点）；分支 / 叶子不同底色 |
| 语法图 | `glr` / `earley` / `cyk` | `Parser.syntax_graph_svg()` / `syntax_graph_dot()` / `write_syntax_graph_svg(path)` / `write_syntax_graph_dot(path)` | 左边 GSS（节点=状态+节点号，移进边实线、归约边虚线带 `·P<n>`，被接受的节点金色高亮），下面并排每条歧义路径的结果树（`%tree` 才有） |

`--svg` / `--dot` 会**多解析一次**（LR 侧走 `parse_tree`，只建树、不跑语义动作），所以没写
`%tree` 的文法也能画出树；GLR 侧直接用这次解析留下的 GSS 与结果，**不重新解析**，解析
失败时画的是一张走到半路的图 —— 那正是排查歧义 / 死路时想看的。装了 Graphviz（Python
包 + `dot` 命令）时 DOT 可以直接渲染：`graphviz.Source(tree.to_dot()).render("tree")`。


## 词法：固定字面量 / 正则 Token / 标识符识别

取 Token 之前会先跳过空白（空格 / `\t` / `\n` / `\v` / `\f` / `\r`，集合由
`%space` 决定，见下面的「空白当 Token」一节）。之后的匹配顺序
（长匹配优先，同长按下面的先后）：

1. 双引号字符串 `"..."`（词法层反转义，值为 `STRING`）
2. 若文法写了 `%token regex`：正则 Token 与所有固定字面量一起做最长匹配，
   **同长度时字面量优先**（所以关键字不会被正则吃掉）；命中就返回
3. 内建数字规则（整数 / `1.2` 浮点 / `1e3` / 后缀 `u`、`d`），产出数值
4. 固定字面量（`%define` / `%token`，长的优先 + 关键字边界检查）；
   没写正则 Token 时，所有字面量都在这一步匹配
5. 标识符：声明了 `%identifier` 就用它的正则扫描（C99 查 DFA 表，其余五档用
   目标语言的正则库），再查关键字表；否则用内建 C 标识符规则（字母/下划线开头，
   字母数字下划线），映射到 `VARIABLE`

没写 `%token regex` / `%identifier` 时，第 2 步不存在、第 5 步走内建规则，
产物与不使用该指令时完全一致。

### 空白当 Token：`\b` 与 `%space`

想让换行、制表、空格进入语法层（按行或按空白分隔的 DSL），把空白写进固定字面量
就行——声明之后这些字符不再被跳过：

```gram
%token NL "\n"      // 换行是真实 Token，可以直接写进产生式
%token SP "\b"      // \b = 一个空白字符：空格 / \t / \n 都命中（强制一个空白）
%space " \t"        // 取 Token 前只跳过空格与制表，换行交给 NL
```

| 写法 | 作用 |
| --- | --- |
| `%token NAME "\n"` / `"\t"` / `" "` | 字面量里的空白不再被跳过；对得上就产出这个 Token |
| `%token NAME "\b"` | `\b` 通配任意一个空白字符，词素是输入里真实的那一个 → **解析对象 / 强制空白** |
| `%space "空白字符集"` | 取 Token 前跳过哪些空白；缺省 `"\b"`（全部空白），`%space ""` 一个都不跳过 |

词法器遇到空白时先看「以空白 / `\b` 开头的固定字面量能不能在当前位置命中」：
命中就停手交给词法匹配，否则按 `%space` 的集合跳过。所以旧文法（没声明空白
Token）的跳空白逻辑与词法行为都不变。`%token regex` 的正则里要写 `\s`，`\b` 只在固定
字面量里有这个含义。完整例子：`example/space_demo.gram`，规范见
[`05-grammar.md`](05-grammar.md)。

### 覆盖内建 Token

`STRING`（双引号字符串）、`NUMBER`（数字）、`VARIABLE`（标识符）是三个内建
Token，各带一条默认词法规则。只要用 `%define NAME "..."`、`%token NAME "..."`
或 `%token regex NAME "..."` 为同名 Token 声明规则，对应的内建规则就会被移除，
完全交给用户规则：

```gram
%token regex STRING "'[^']*'"            // 只认单引号字符串，不再接受 "..."
%token regex NUMBER "0[xX][0-9a-fA-F]+"  // 只认十六进制数字
%token regex VARIABLE "[$][A-Za-z_]+"    // 只认 $ 前缀的变量名
```

`%identifier "..."` 仍是“标识符正则 → `VARIABLE`（关键字另查表）”的快捷方式，
声明它时内建 C 标识符规则同样被替换。注意 `%identifier` 本身就在定义 `VARIABLE`，
所以它和 `VARIABLE` 的词法规则不能同时声明，冲突时直接报错，请二选一。

### 正则 Token

`%token NAME "..."` 不带 `regex` 时只支持 `|` 分隔的固定字面量（与 `%define` 完全
等价，`\\|` 表示普通竖线）；在名字前加 `regex` 才启用正则，整串按正则解释：

```gram
%token EQ    "==|="                      // 不带 regex：只拆成字面量 "==" / "="
%token PIPE  "\|"                       // 字面量竖线
%token regex FLOAT "[0-9]+[.][0-9]+"    // 带 regex：整串按正则解释
%token regex DATE  "[0-9]{4}-[0-9]{2}-[0-9]{2}"
%identifier "[A-Za-z_][A-Za-z0-9_]*"
```

支持的语法：字面量、转义、`.`、`[abc]`、`[a-z]`、`[^...]`、`(` `)`、`|`、`*`、`+`、
`?`、`{m}` `{m,}` `{m,n}`（≤32）。转义 `\* \. \[ \] \( \) \{ \} \| \^ \\ \/`
还原成字面量（写 `"\*"` 或 `"\\*"` 都可以），`\d \w \s`（大写取反）是字符集简写，
`\n \r \t \f \v \0` 是控制字符。能匹配空串的正则在生成期报错（Lexer 无法前进）；
`{m,n}` 的 32 上限只对 **C99** 是硬错误——其余五档把正则原样交给目标语言的正则库，
超上限只打一条警告（见 [`07-codegen.md`](07-codegen.md) §13）。

正则 Token 只携带文本（`Parser_TreeGetText` / `pName`），不产生数值——要数值请
继续用内建 `NUMBER`。

### 标识符识别接口

声明 `%identifier` 后，生成的 `parser_token.h/.c` 里会有：

| 接口 | 说明 |
| --- | --- |
| `Parser_TokenIdentifierLength(text)` | 最长标识符前缀长度（走生成的 DFA） |
| `Parser_TokenIsIdentifier(text)` | 整串是否标识符 |
| `Parser_TokenKeyword(text)` | 关键字查表，非关键字返回 `TOKEN_INVALID` |
| `Parser_TokenName(type)` / `Parser_TokenText(token, buf, size)` | Token 名与文本 |
| `Parser_LexDfaMatch(dfa, text)` | 通用 DFA 最长匹配（Lexer 与上面两个函数共用） |
