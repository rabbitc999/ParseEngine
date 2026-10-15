# 07 示例导读

示例的目标语言由 manifest 与文法约束决定；完整模式支持范围见 [12 支持矩阵](15-language-mode-support.md)。

`example/` 下共 46 个文法，逐个生成 + 编译一遍就能挡住大部分回归——`make -C source test-smoke` 会按
[`example/manifest.tsv`](../example/manifest.tsv) 自动跑这一遍（对不上
`example/<name>.expected` 就非零退出）；**`tools/smoke.sh` 目前是个「禁止跑冒烟测试」的桩**，
这套验收现在要手工走（`tools/gen_all.sh` 全量重生成 + `tools/diff.sh` 比对 + 挑几份手工跑）。
它们同时也是文法写法的“活文档”：
大多数示例用规则 `|` 分支写法。按 `manifest.tsv` 的覆盖是：C **37** 份、
C++17 **43** 份、Python **20** 份、C# **18** 份、Rust **17** 份、Go **16** 份，
六个完整后端合计 **151** 个（文法, 语言）组合；加上 Java / JS 的 DD、Earley、CYK
样例后，全表共 **155** 个组合。GLR 例子列 C++17 / Python / C# / Rust / Go，
Earley / CYK 也覆盖 Go；其余语言限制与固定输入以 manifest 为准。

## 1. 表达式、分支与 LR 基础

| 文法 | 模式 | 语义值类型 | 关键特性 |
| --- | --- | --- | --- |
| `or_branch.gram` | LALR | `%value_type int` | `\|` 分支最小示例 + 语义动作 |
| `calc.gram` | LALR | `%value_type double` | 优先级、`%code ... %endcode` 里的变量表、`:=` 赋值、`sqrt/pow/fmod` |
| `calc_prec.gram` | LALR | `%value_type double` | 二义文法 + `%left/%right/%prec` 消解 |
| `calc_branch.gram` | LALR | `%value_type double` | 与 `calc_prec` 等价，但候选写在一条 `\|` 规则里 |
| `logic.gram` | SLR | `%value_type int` | `%left/%right` 优先级 + `\|` 分支（唯一 SLR 示例） |
| `lr0.gram` | LR(0) | `%value_type int` | 真 LR(0) 文法（5 状态、零冲突），完整项在整行归约；自带 `%teach` + `%mermaid` |

## 2. LL 与树结构

| 文法 | 模式 | 语义值类型 | 关键特性 |
| --- | --- | --- | --- |
| `pl0_ll1.gram` | LL(1) | 兜底 `int` | PL/0 的 LL(1) 版（尾递归消除左递归）+ `%tree` |
| `json.gram` | LL(1) | 兜底 `int` | `%tree` + `\|` 分支，可选部分用尾链 `%empty` |
| `ini.gram` | LL(1) | 兜底 `int` | `%tree` + `\|` 分支 + `%identifier` |
| `mini_lang.gram` | LL(1) | 兜底 `int` | `%tree` + `\|` 分支，语句/表达式尾链 |
| `ll0_demo.gram` | （不写 `%mode`） | `%tree` | LL(0)：不看 lookahead，每个非终结符恰好一条产生式 |
| `llk_demo.gram` | （不写 `%mode`） | `%tree` | LL(2) 而不是 LL(1) 的文法；`--mode 'LL(1)'` 报冲突，`--mode 'LL(2)'` 通过 |

## 3. LR(1)、LR(k) 与完整语言

| 文法 | 模式 | 语义值类型 | 关键特性 |
| --- | --- | --- | --- |
| `pl0.gram` | LALR | 兜底 `int` | 左递归 PL/0 + `%tree` |
| `ielr_demo.gram` | IELR | `%value_type int` | 经典非 LALR 文法（def/param_spec/return_spec），演示 IELR 状态拆分 |
| `ielr_split.gram` | IELR | `%value_type int` | 非 LALR 文法，需要两个独立拆分 |
| `lrk_demo.gram` | （不写 `%mode`） | `%tree` | LR(k) / LALR(k) 对照：对任何 k 都不是 LALR(k) 的经典例子 |
| `c89_subset.gram` | LALR | 兜底 `int` | C89 子集，`%tree`，规模最大的示例 |

## 4. 词法与文本容器

| 文法 | 模式 | 语义值类型 | 关键特性 |
| --- | --- | --- | --- |
| `lex_demo.gram` | LALR | `%value_type double` | `%token regex`、`%identifier`、关键字、用户变量表 |
| `lexer_only.gram` | `%mode lex` | 无（只出词法器） | 一条产生式都没有也能导出；六档都支持 |
| `commit_demo.gram` | SLR | 兜底 `int`（`%tree`） | `%commit` / `%trivia` 的块注释 / 行注释 / 缺省类型 / `skip` 五种写法 |
| `alias_demo.gram` | `%mode lex` | 无（只出词法器） | Token、正则 Token、`%identifier`、`%commit` 和 `%trivia` 的别名示例 |
| `token_dict_demo.gram` | （不写 `%mode`） | `%union`（`%tree`） | `%token NAME include "csv"`：字面量别名放 CSV，只追加不新建；带 `<ival>` 成员、未导入的对照行、第 3/4 列噪音都在示例里 |

## 5. 树、语义值与空白 Token

| 文法 | 模式 | 语义值类型 | 关键特性 |
| --- | --- | --- | --- |
| `custom_tree.gram` | LALR | 用户头文件（`%copy`） | `%tree` + `%copy` |
| `union_demo.gram` | LL(1) | `%union` | `%union` + `%type` / `%token <成员>`，按符号分 C 类型 |
| `destructor_demo.gram` | LALR | `%union` | `%destructor <成员>`：错误恢复弹栈时按成员调清理代码 |
| `space_demo.gram` | IELR | 兜底 `int`（`%tree`） | 空白 / 换行当 Token：`%token NL` / `%token SP "\b"` / `%space` |

## 6. 错误恢复

| 文法 | 模式 | 语义值类型 | 关键特性 |
| --- | --- | --- | --- |
| `error_recovery.gram` | LALR | `%value_type double` | `error` 产生式 + `yyerrok`，一次解析报多个错 |
| `sync_recovery.gram` | LALR | `%value_type double` | `%sync SEMI` panic mode，弹栈 + 丢输入后继续 |
| `ll_follow_recovery.gram` | LL(1) | `%value_type double` | 不写恢复指令，靠 FOLLOW 集恢复 |

## 7. 歧义与回溯（GLR / 递归下降）

| 文法 | 模式 | 语义值类型 | 关键特性 |
| --- | --- | --- | --- |
| `glr_dangling_else.gram` | GLR | `%tree` | 悬垂 else 的经典二义性：GSS 把两种解释都列出来（cpp17 / python / csharp / rust / go 五档） |
| `english-clauses.gram` | GLR | `%tree` | 英语句法中的介词短语附着与从句；`the man saw the girl with the telescope` 得到两棵树（cpp17 / python / csharp / rust / go 五档） |
| `english-syntax.gram` | Earley / CYK | `%tree` | 主谓宾、定语、状语、主宾补语、祈使句、虚拟条件与 wish、关系/宾语/状语从句；默认 Earley，`--mode cyk` 切换；介词短语附着保留两棵树（cpp17 / python / csharp / rust） |
| `glr_expr.gram` | GLR | 兜底 `int` | 二义表达式：同一次解析拿到多个结果 |
| `glr_recovery.gram` | GLR | 兜底 `int` | GLR 的死路恢复（吞 Token） |
| `chart_nullable.gram` | Earley / CYK | `%tree` | 可空前缀、空句、单元规则、长右部与左递归；默认 Earley，`--mode cyk` 切换（cpp17 / python / csharp / rust） |
| `chart_value_csharp.gram` | Earley / CYK | `%value_type int` | C# 语义动作在两条歧义路径上分别得到 14 与 20 |
| `dd_expr.gram` | 递归下降 `dd` | `%tree` | 右递归、不建表：按产生式声明顺序逐条试、失败回退输入位置，只保留第 1 条匹配路径（**六档全支持**） |

`english-syntax.gram` 是受控英语片段，不检查主谓一致、时态搭配或语义选择。
运行 `python tests/english_syntax.py --langs python,cpp17,rust` 可验证肯定/否定句式、
语法树节点和介词短语附着的两种结果；Python 同时对照 GLR。

`dd_expr.gram` 的产生式书写顺序就是尝试顺序（`LPAREN expr RPAREN` → `NUMBER PLUS expr`
→ `NUMBER`），喂 `1 + 2` 得到 `NUMBER(1) PLUS(+) expr(NUMBER(2))` 的右结合树；
左递归文法（`calc.gram` 那种）在生成期就会被拦下并给出改写建议。
用法与限制见 [`13-modes.md`](13-modes.md) 的「递归下降（`%mode dd`）」。

## 8. 用户代码、入口与目标语言

| 文法 | 模式 | 语义值类型 | 关键特性 |
| --- | --- | --- | --- |
| `if_demo.gram` | LALR | `%value_type double` | `%if` 条件编译控制 `%token`、产生式与动作片段 |
| `epilogue_demo.gram` | LALR | `%value_type double` | `%epilogue` 放函数定义，语义动作调用 |
| `main_demo.gram` | LALR | `%value_type double` | `%main` 替换内建 demo main |
| `line_demo.gram` | LALR | `%value_type double` | `#line` 成对插入，报错指回 `.gram` |
| `namespace_demo.gram` | LALR | `%value_type double` | `%namespace myapp` 改 C++17 命名空间与文件名 |
| `prefix_demo.gram` | LALR | `%value_type int` | `%prefix Demo` 改 C99 公开符号前缀 |
| `code_demo.gram` | LALR | `%value_type double` | `%code ... %endcode` 用户代码块 |
| `include_demo.gram` | LALR | `%value_type double` | `%code include` / `%epilogue include` / `%main include`：正文放外部文件，`.gram` 只留文法 |
| `python_demo.gram` | LALR | 任意 Python 对象 | `%class PyCalc` + Python 动作，只出 Python 产物 |

生成任意一个（默认 C++17 产物，加 `--lang c` 切回 C99，加 `--lang python`
换成免编译的 Python 产物，加 `--lang csharp` 换成 `dotnet build` 直接编译的
C# 产物，加 `--lang rust` 换成 `cargo build` 的 Cargo 工程，加 `--lang gram`
则只要文法与分析报告）：

```sh
./build/bin/ParseEngine --grammar-file example/<名字>.gram --out-dir example/<名字>_out
cd example/<名字>_out && make      # g++ -std=c++17，产物是 parser.hpp / parser.cpp / main.cpp

# 或者用产物自带的 CMakeLists.txt（与 make 等价）
cmake -S . -B build && cmake --build build
```

`%tree` 且没有语义动作的文法，`parser_demo` 会直接打印 CST；带动作的会打印
求值结果。

## 9. `|` 分支写法

想学分支语法看这三份就够：

- `example/or_branch.gram`：最小对照，三个候选各带动作；
  注意固定字面量有关键字边界检查，`B` `C` 两个字母 Token 要写 `"b c"`。
- `example/calc_branch.gram`：`|` 分支 + `%prec`，和
  `example/calc_prec.gram` 行为完全一致，可以用来看两种写法怎么对应。
- `example/logic.gram`：`expr := expr OR expr | expr AND expr | ...`
  一条规则写完，靠 `%left OR` / `%left AND` / `%right NOT` 消解冲突。

分支写法与“一行一条”完全等价，整条规则必须以 `;` 结束，每个分支各带自己的
动作（`%tree` 文法可以都不写动作）。

## 10. 按主题找例子

- **想抄 LL(1) 文法结构**：`json.gram`、`ini.gram`、`mini_lang.gram`，
  它们的列表都用“尾链 + `%empty`”绕开左递归。
- **想用优先级消解二义**：`calc_prec.gram`、`calc_branch.gram`、`logic.gram`。
- **想用语法树**：`pl0.gram`（`%tree`）、
  `pl0_ll1.gram`、`custom_tree.gram`。
- **想用正则 Token / 标识符**：`lex_demo.gram`。
- **想只要词法器（不做语法分析）**：`lexer_only.gram`——`%mode lex`，文法里一条
  产生式都没有；C 出 `lexer.h` / `lexer.c` / `parser_token.*` / `main.c`，C++17 出
  `lexer.hpp` / `lexer.cpp` / `main.cpp`，Python 出 `parser.py`（词法器 +
  `tokenize`）/ `main.py`。`printf 'a + 12' | ./lexer_demo`（Python 是
  `python3 main.py`）会逐个打印 Token 的类型与词素（`VARIABLE "a" 1:1` 这样），
  认不出来的字符报 `-29`、字符串没闭合报 `-28`。
  解析相关的写法（产生式、`%tree`、`%sync` 等）会被忽略并提示一句。
- **想用 `%copy` 拷外部语义值头文件**：`custom_tree.gram`，配套
  `example/lib/parser_value.h`（`%copy file` 拷进产物目录）。
- **想用 `%union` 分类型的语义值**：`union_demo.gram`——`%union` 里放了
  `int` / `double` / 结构体指针三种成员，`%type <ival> expr term`、
  `%token <ival> NUMBER`，动作里照旧写 `$$ = $1 + $3;`，生成器按符号补成员；
  同一份文法切成 `%mode lalr` 也能用。词法器会按 `%token <ival> NUMBER` 把数字
  写进 `pToken->value.ival`，所以这个例子不需要 `PARSER_USER_TOKEN_VALUE`。
  没写 `%union` 时这套成员展开完全不参与。
- **想登记「丢弃语义值时怎么清理」**：`destructor_demo.gram`——`%union` 里的
  `char *str` 配 `%destructor <str> { free($$); }`（丢弃时 `$$` 就是那个成员），
  `long num` 配 `%destructor <num> { ... }`；两个计数器分别数「释放了几次堆内存」
  「丢弃了几个数值」，喂错误输入（例如 `a b )`）后 `strDiscarded=1` 就说明恢复
  弹栈 / 失败收尾那条路径真的把值清掉了。成功解析不清栈（结果归调用方），
  所以 `a b ; c ;` 的两个计数都是 0。
- **想看 IELR / 非 LALR 文法**：`ielr_demo.gram`（Bison 的 def/param_spec 例子）
  与 `ielr_split.gram`（两处独立冲突，需要两个状态拆分）；两者换成 `%mode lalr`
  会看到归约/归约冲突的**大警告**（确定性 LR 模式自动选一条「更早的产生式」继续
  生成），换成 `%mode ielr` 则靠拆分真正解掉这些冲突、不产生警告。
- **想看 LR(0) / 规范 LR(1) 自动机**：`lr0.gram` 是真 LR(0) 文法（`%mode lr0`），
  适合对着 `--dump-lr-automaton --dump-slr-tables` 讲闭包、GOTO 与整行归约；
  想看 LR(1) 与 LALR 的状态数差距，用 `--mode lr1` 跑任意示例（差距最大的是
  `c89_subset.gram`：1691 → 340），想看「合并不合并的后果」就用
  `ielr_split.gram`（`--mode lr1` 得到 26 → 24，LALR 那 24 个状态里有两处
  归约/归约冲突）。
- **想看状态机图示**：`lr0.gram` 自带 `%mermaid`（等价于命令行 `--mermaid`），
  生成后用浏览器打开产物里的 `mermaid/index.html` 就能看到 LR 状态机图；
  `lex_demo.gram` 加 `--mermaid` 还能多出 FLOAT / DATE / 标识符三张词法 DFA，
  `ini.gram` 这种 LL(1) 文法只有词法图。命令行 `--dump-mermaid` 不用任何别的
  开关就能把源码打到标准输出。图示与教学模式无关，见
  [`06-analysis.md`](06-analysis.md) §5.4。
- **想看错误恢复**：`error_recovery.gram` 用 `stmt := error SEMI { yyerrok; }` 把
  坏语句整条丢掉（`1+2; 3; 4 5; 6 7; 8;` → 报 2 条错后仍解析到 8）；
  `sync_recovery.gram` 没写 error 产生式，只用 `%sync SEMI` 做 panic mode
  （`1 + + 2 ; 3` → 归约到语句边界后命中锚点，继续解析到 3）；
  `ll_follow_recovery.gram` 是自顶向下那一侧——**什么恢复指令都没写**，LL(1)
  出错时自动用 FOLLOW 集恢复（`1 + + 2 ; 3 ;` 报 1 条错后继续）；
  三者的运行结果都用 `Parser_GetErrorCount()` / `Parser_GetError(index)` 可以逐条
  核对。写法与语义见 [`05-grammar.md`](05-grammar.md) §3.1 / §3.2。
- **想看大规模文法**：`c89_subset.gram`。
- **想看条件编译**：`if_demo.gram`——同一份文法用 `--define LOG`（多一个 `!` 后缀
  Token + 日志）、`--define TRACE`（trace 输出，`TRACE && !QUIET` 控制）、
  `--define LOG=off`（与不传等价）产出不同内容；条件是 `||` / `&&` / `!` 与括号
  的组合；`--preprocess-only` 能单独看预处理层的结果。
- **想写辅助函数 / 自己的 main**：`epilogue_demo.gram`（声明放 `%code ... %endcode`、函数
  定义放 `%epilogue`、动作里调用；`1+2*3` → 13）与 `main_demo.gram`
  （`--define PROD` 用生产入口，不传回落内建 demo main）。要 include 一堆自己的
  头文件就用 `%copy`，别把函数定义塞进 `%code ... %endcode`（会重复定义）。
- **想验证「报错指回 `.gram`」**：`line_demo.gram`——注释里写了把某一行改成
  `return NoSuchGlobal;` 再用 `gcc -std=c99 -c parser.c`（或 `g++ -std=c++17 -c
  parser.cpp`）编译，就会看到报错指向 `.gram` 的行号并回显那一行源码。
  动作 / `%union` / `%code ... %endcode` / `%epilogue` / `%main` 五处都能这样核对。
- **想同时链接两个解析器**：`namespace_demo.gram`——同一份文法用 `--namespace alpha`
  与 `--namespace beta` 生成两份，两个头可以一起 include，`alpha::Engine` 与
  `beta::Engine` 互不冲突（顺带可以核对文件名前缀、头文件保护宏；`%namespace`
  配 `--lang c` 会报错）。
- **想先快速看看文法能不能跑**：`python_demo.gram`——`--lang python` 生成
  `parser.py` / `main.py`，不用编译器，`printf '1+2*3' | python3 main.py` 直接出
  结果；类名用 `%class` / `--class` 改。任何没写 C 专属语义值写法的文法（例如
  `json.gram` / `pl0_ll1.gram`）都能这样跑一份，
  用来和 C / C++17 产物的输出对照。
- **想让空白进语法层**：`space_demo.gram`——`NL`（换行）与 `SP`（一个空白字符）
  都是真实 Token，`%space` 决定取 Token 前跳过哪些空白。单行输入
  `printf 'a b' | ./parser_demo` 就能看到 `SP( )`；想看换行 Token 得喂多行文本
  （内建 demo main 只读一行，写 `%main` 或直接调 `Parser_RunText` /
  `engine.runText` / `engine.run_text`）。规则细节见
  [`05-grammar.md`](05-grammar.md) 的「空白 / 换行当 Token」一节。

## 11. 实跑样例

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

演示程序从标准输入读一行：上面每条都可以写成
`printf '<输入>' | ./parser_demo`（读到换行或 EOF 就开始解析）；
`main_demo` 的 `%main` 入口自己解析 `argv`，按它自己的用法跑。

## 12. 加自己的示例

1. 新建 `example/<名字>.gram`，首行 `%mode` 决定模式；
2. 需要额外的 C 状态（变量表、缓冲区……）就在 `%code ... %endcode` 里自己定义；
3. `./build/bin/ParseEngine --grammar-file example/<名字>.gram --out-dir /tmp/<名字>` 试生成；
4. 在 [`example/manifest.tsv`](../example/manifest.tsv) 加一行
   `名字<TAB>实测支持的档<TAB>固定输入`，并跑一次
   `tools/smoke.sh capture`（**脚本目前是桩**，得先让它放行；或按 [`09-build-and-test.md`](09-build-and-test.md) §4
   手工生成 + 编译 + 喂输入）生成 `example/<名字>.expected`；
5. `make -C source test-smoke` 应当变绿（**脚本目前是桩**，放行前按 §4 的手工姿势验），
   `example/` 下漏写 manifest 的 `.gram` 会报 FAIL。
