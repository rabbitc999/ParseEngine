# 完整 PL/0 → 三元式 / 四元式（C++17 工程示例）

这是一个**完整的 C++17 工程**：用 ParseEngine 把 PL/0 文法生成成一套 C++17
解析器，再用它把 PL/0 源码编译成**三元式**（triple，形如 `i: (op, arg1, arg2)`）
或**四元式**（quadruple，形如 `i: (op, arg1, arg2, result)`）。生成器本身不需要
任何改动，工程里只有文法、样例与构建脚本。

工程名里的 **ir** = intermediate representation（中间表示），也点出这一份与
常见写法的差别：**注释由文法里的 `%trivia` 在词法器里跳过**，不再由 `%main`
预先把注释文本剥掉——所以解析器本身（用 `Parser_RunText()` 当库调）就认识
`{ … }` 与 `(* … *)` 两种注释。

**只针对 C++17 产物**（`--lang cpp17`）：文法的 `%namespace pl0`、`%main` / `%epilogue`
的写法、以及 `Makefile` 的构建都按 C++17 来，不保证 C99 / Python 后端也能用。

```
project/pl0-ir/
├── pl0.gram                    完整 PL/0 文法 + 三元式生成
├── pl0_quads.gram              完整 PL/0 文法 + 四元式生成
├── Makefile                    make（生成+编译） / generate / run / check
│                               quads / run-quads / clean
├── check.sh                    跑全部样例并比对期望输出（三元式）
├── samples/*.pl0               9 个正常样例（见下面的测试矩阵）
├── samples-error/*.pl0         13 个出错样例
├── expected/*.triples          正常样例的期望三元式（已人工核对）
├── expected-error/*.txt        出错样例的期望报错首行
├── build/                      三元式产物的生成与编译结果（.gitignore 忽略）
└── build-quads/                四元式产物的生成与编译结果（.gitignore 忽略）
```

## 构建与运行

```sh
# 0) 先在仓库根目录构建生成器（本工程只用它，不改它）
make

# 1) 生成 + 编译（默认三元式，产物在 project/pl0-ir/build/）
make -C project/pl0-ir

# 2) 跑一个样例
make -C project/pl0-ir run               # 用 samples/if_while.pl0
project/pl0-ir/build/parser_demo project/pl0-ir/samples/nested_proc.pl0

# 3) 全部样例与期望输出比对（9 个正常 + 13 个错误）
make -C project/pl0-ir check

# 4) 换成四元式：同一批样例、另一份文法
make -C project/pl0-ir quads             # 生成 + 编译四元式产物
make -C project/pl0-ir run-quads         # 跑一个样例
```

`make -C project/pl0-ir check` 的输出：

```
[ok]   comments / const / expr / if_while / nested_proc / odd / ops / proc / torture
[ok]   9 个正常样例
[ok]   call_var（报错：x 不是过程）
... 13 条错误样例 ...
全部通过（9 个正常样例 + 13 个错误样例）
```

**四元式不预置期望文件**：`pl0_quads.gram` 是给你读、对照、自己跑一遍看的——
生成 + 编译后直接喂 `samples/*.pl0`，观察输出与 `expected/*.triples` 的差异，
就能看清「三元式 → 四元式」到底改了什么。要固化自己的期望，把跑出来的输出
存成 `expected-quads/<名字>.quads`（目录自建），再在 `check.sh` 里加一段比对
即可——工程默认不带这一段。

生成器路径可以用 `ENGINE=` 覆盖（默认 `../../build/bin/ParseEngine`，
Git Bash / MSYS2 下会自动补 `.exe`）：

```sh
make -C project/pl0-ir ENGINE=../../build/bin/ParseEngine
```

## PL/0 覆盖范围

覆盖 Wirth 的 PL/0 全部语法：

| 类别 | 支持 |
| --- | --- |
| 声明 | `const`（可多条、可被表达式引用并折叠）、`var`（一条多个）、`procedure`（**可嵌套**，无参数） |
| 语句 | 赋值 `:=`、`call`、`begin … end`（可嵌套）、`if … then …`、`while … do …`、`read` / `?`、`write` / `!` |
| 空语句 | `statement` 可以为空：`begin end`、`begin ; ; x := 1 end` 都合法 |
| 表达式 | `+ - * /`、一元 `+` `-`、括号、`odd`、六种比较 `= <> < <= > >=`（`#` 作为 `<>` 的别名） |
| 词法 | 标识符（`%identifier`）、整数字面量；注释 `{ … }` 与 `(* … *)` 两种（由 `%trivia` 在词法器里跳过，见下） |

**不支持**：数组（经典 PL/0 没有这项）、带参数的过程（PL/0 的过程没有参数，
过程之间用外层变量通信）、`else` 分支（PL/0 没有）。

`write(x)` 也能写：它会按 `WRITE expr` 解析（`expr` 本身就是 `(x)`）。但文法里
**不能单列** `WRITE LPAREN expr RPAREN` —— 那和 `factor := LPAREN expr RPAREN`
在 `write ( expr )` 上会撞成归约/归约冲突（真实的二义性），生成期就报错了。

实现全在 `%epilogue` 里，只用标准库容器：符号表、三元式/四元式表、`while` 标签栈都是
`std::vector`，名字池是 `std::vector<std::string>`，限定名前缀和词素是 `std::string`。
所以没有定长数组，也没有「256 个符号 / 1024 条三元式 / 128 个标签 / 16 层嵌套」
这类上限——只受内存限制。（语义值 `ParserValue_T` 本身仍是 POD：运行时会对它整块
清零和按值拷贝，所以名字只存名字池下标，见 [`../../doc/11-project-examples.md`](../../doc/11-project-examples.md) §3。）

## 三元式与四元式

两份文法共享同一套前端（词法、`%mode lalr`、`%namespace pl0`、符号表与作用域、
注释处理、错误检查），只在 `%epilogue` 的**代码生成**这一层分岔：

| | `pl0.gram`（三元式） | `pl0_quads.gram`（四元式） |
| --- | --- | --- |
| 记录形状 | `i: (op, arg1, arg2)` | `i: (op, arg1, arg2, result)` |
| 结果引用 | 隐含：本条的下标 `(k)` | 显式：临时量名 `t1`、`t2` … |
| 表结构 | `Pl0_Triple_T{op, arg1, arg2}` | `Pl0_Quad_T{op, arg1, arg2, result}` |
| 临时量 | 下标即引用，无独立编号 | `g_pl0TempCount` + `Pl0_NewTemp()` 分配编号 |
| 打印 | `Pl0_PrintTriples` | `Pl0_PrintQuads` |
| 期望文件 | `expected/*.triples` | 不预置，读者跑出来自己核对 |

**四元式为什么更适合做优化**：三元式里 `(k)` 是**指令下标**，含义随指令移动而变；
四元式里 `t1` 是**独立名字**，含义固定。常量传播、公共子表达式删除、指令调度、
寄存器分配、目标代码生成——这些都要求引用稳定，不随指令搬动而失效。所以四元式
通常被当作中间代码的标准形式，三元式更像一种紧凑的中间表示。

### 三元式格式

```
i: (op, arg1, arg2)
```

| 操作数写法 | 含义 |
| --- | --- |
| `x` | 顶层变量 |
| `p.x` / `p.q.x` | 过程 p（或嵌套 p.q）里的变量——用**限定名**区分作用域 |
| `7` | 常量（字面量或已折叠的常量表达式） |
| `(3)` | 第 3 号三元式的计算结果（临时量） |
| `L2` | 标签（`(label, L2, _)` 定义它，`jz` / `j` 跳到它） |
| `_` | 该位置没有操作数 |

算符：`+ - * / neg odd == != < <= > >= := read write jz j label proc endproc call`。

```pl0
var x, y;                       |   三元式：共 5 条
begin                           |     0: (:=, 7, x)      ← 1+2*3 都是常量，折叠成 7
    x := 1 + 2 * 3;             |     1: (-, x, 1)
    y := (x - 1) / 2;           |     2: (/, (1), 2)
    write y                     |     3: (:=, (2), y)
end.                            |     4: (write, y, _)
```

### 四元式格式

```
i: (op, arg1, arg2, result)
```

第 4 个字段 `result` 是**结果放哪**：二元/一元运算放新分配的临时量 `tN`，
赋值 / `read` 放目标变量，`label` 放标签名，`j` / `jz` / `write` / `proc` /
`endproc` / `call` 没有结果就填 `_`。操作数写法与三元式一致，只是临时量从
`(k)` 变成 `tN`。

```pl0
var x, y;                       |   四元式：共 5 条
begin                           |     0: (:=, 7, _, x)    ← 1+2*3 折叠成 7
    x := 1 + 2 * 3;             |     1: (-, x, 1, t1)
    y := (x - 1) / 2;           |     2: (/, t1, 2, t2)
    write y                     |     3: (:=, t2, _, y)
end.                            |     4: (write, y, _, _)
```

逐条对照：

| 源码 | 三元式 | 四元式 |
| --- | --- | --- |
| `a + b` | `(+, a, b)`，结果 `(k)` | `(+, a, b, t1)` |
| `-x` | `(neg, x, _)`，结果 `(k)` | `(neg, x, _, t1)` |
| `x := e` | `(:=, e, x)` | `(:=, e, _, x)` |
| `if c then S` | `(<, a, b)` `(jz, (k), L1)` … `(label, L1, _)` | `(<, a, b, t1)` `(jz, t1, _, L1)` … `(label, _, _, L1)` |
| `while c do S` | `(label, L0, _)` … `(j, _, L0)` `(label, L1, _)` | `(label, _, _, L0)` … `(j, _, _, L0)` `(label, _, _, L1)` |
| `read x` / `? x` | `(read, _, x)` | `(read, _, _, x)` |
| `write e` / `! e` | `(write, e, _)` | `(write, e, _, _)` |
| `procedure p; …` | `(proc, p, _)` … `(endproc, p, _)` | `(proc, p, _, _)` … `(endproc, p, _, _)` |
| `call p` | `(call, p, _)` | `(call, p, _, _)` |

过程与作用域（`samples/proc.pl0`、`samples/nested_proc.pl0`）：

```
  0: (proc, add, _, _)          ← 过程体用 proc / endproc 包起来
  1: (:=, 0, _, add.step)       ← add 的局部变量：限定名 add.step
  ...
 11: (endproc, add, _, _)
 13: (call, add, _, _)          ← call 生成 call
```

嵌套过程里同名变量互不干扰：`samples/nested_proc.pl0` 的 `outer.x` 与顶层 `x`
在四元式里分得清清楚楚。

## 出错时

```
未声明的标识符: y                                     （语义检查）
重复声明: x
未声明的过程: nosuch
过程不能当变量用: p
x 不是过程
不能给常量赋值: n
除数为常量 0
注释没有闭合（{ 少一个 }）
语法错误: code=-27 line=4 column=1 message=语法错误：期望 NUMBER / VARIABLE / PLUS / MINUS 等 5 个之一，实际是 END   （解析器报错）
```

两份文法的错误检查完全一致（同一套 `%epilogue` 前端逻辑），只是打印函数不同。
语法错误的行列号来自生成器插入的 `#line`：它就指回 `pl0_quads.gram` 里对应的动作。

## 注释在哪一层处理

注释**不是**在驱动里剥掉的，而是文法里两条 `%trivia` 由生成出来的词法器跳过：

```c
%trivia "\{[^}]*\}"                    /* { ... }  */
%trivia "\(\*([^*]|\*+[^)]+)*\*+\)"    /* (* ... *) */
```

`%trivia` = **匹配即丢**：命中的文本不进语法层，也不进 `%commit` 容器（想把注释
收起来给 AI 看，把 `%trivia` 换成 `%commit <类型> "…"` 就是收进容器，见
[`../../doc/12-grammar-usage.md`](../../doc/12-grammar-usage.md#commit把识别但不进语法层的文本收进容器)）。
这么做的好处是**解析器本身**就认识注释：当库用 `Parser_RunText()` 时，调用方
不必先写一遍剥注释的代码；报错行列号也天然与原文一致（原文一个字节都没动）。

两条规则都写成 DFA 友好的正则（PL/0 的注释不嵌套）：吃到第一个 `}` / `*)` 为止。
`(* … *)` 那条特意写成 `\*+[^)]+`（星号串后面跟一个非 `)` 的字符），而不是更直观的
`\*+[^*)]`——实测后者在某些位置会生成**永不接受**的 DFA（`[^*)]` / `[^)]` / `[^*]`
这一族否定类出现在「分组 + 交替 + 星号」里会踩到），`[^)]+` 语义等价且能正确匹配：
注释里有 `*`、有 `)`、有中文、跨行都验过。

注释没闭合时，词法器只会说「认不出来的字符」，所以 `%main` 里保留了一个**只读**的
`Pl0_CheckComments()`：它不改文本，只在发现没闭合的 `{` / `(*` 时打印
`注释没有闭合（{ 少一个 }）` 并退出——文案与以前逐字相同，所以
`expected-error/comment_open.txt` 不用改。

## 测试矩阵

`samples/`（成功 + 与 `expected/*.triples` 逐行比对）：

| 样例 | 覆盖 |
| --- | --- |
| `expr.pl0` | 赋值、括号、常量折叠、临时量引用 |
| `const.pl0` | 常量声明、常量参与运算折叠 |
| `if_while.pl0` | `if-then`、`while-do`、嵌套语句、比较 |
| `odd.pl0` | `odd`、`while` 回跳、`<>` |
| `proc.pl0` | 过程声明、`call`、局部变量、过程内 `while` |
| `nested_proc.pl0` | 嵌套过程、过程内再声明过程、同名遮蔽、非局部引用 |
| `ops.pl0` | 全部算符（含一元正负、六种比较、`#`、`odd 表达式`）与优先级 |
| `comments.pl0` | 两种注释、空语句、`?` / `!`、`read` / `read(x)` / `write` / `write(x)` |
| `torture.pl0` | 综合一份：三层嵌套过程 + 递归 `call` + 同名遮蔽 + 两种注释 + `?` / `!` + 空语句 |

`samples-error/`（必须报错退出，且 stderr 首行与 `expected-error/*.txt` 一致）：

| 样例 | 报错 |
| --- | --- |
| `undeclared.pl0` | 使用未声明的变量 |
| `dup_var.pl0` / `dup_proc.pl0` | 同层重复声明变量 / 过程 |
| `const_assign.pl0` / `const_read.pl0` | 给常量赋值 / `read` 一个常量 |
| `proc_as_var.pl0` / `call_var.pl0` | 把过程当变量 / `call` 一个变量 |
| `unknown_proc.pl0` | `call` 未声明的过程 |
| `div_zero.pl0` | 常量除零 |
| `comment_open.pl0` | 注释没闭合 |
| `const_expr.pl0` | 常量声明的右值不是数字（语法错误） |
| `syntax.pl0` / `missing_end.pl0` | 语法错误（行列号由 `#line` 指回 `.pl0`） |

加样例就是往 `samples/` 放 `.pl0`、把跑出来的三元式存进 `expected/`（先人工核对
语义对不对，再固化）；错误样例同理放 `samples-error/` + `expected-error/<名字>.txt`
（只比首行，避免把路径之类的噪音也固化进去）。

**四元式版不预置期望文件**：`pl0_quads.gram` 的用途是给你读、对照、跑一遍看差异——
`make -C project/pl0-ir quads` 生成 + 编译后，直接喂 `samples/*.pl0`，和
`expected/*.triples` 并排看，三元式与四元式的差别立刻清楚。要固化自己的期望，
把输出存成 `expected-quads/<名字>.quads`（目录自建），再自行在 `check.sh` 里加
一段比对即可。

## 这个工程用到生成器的哪些特性

两份文法用的特性完全一样，只是 `%epilogue` 里的表结构与打印函数不同：

| 用到的特性 | 位置 | 作用 |
| --- | --- | --- |
| `%mode lalr` | 文件头 | PL/0 是 LALR(1) 文法，直接产出确定性解析器 |
| `%namespace pl0` | 文件头 | C++17 产物的命名空间与文件名前缀（`pl0_parser.hpp` 等） |
| `%identifier` + `%define` | 词法段 | 标识符走 DFA，关键字/运算符走固定字面量（关键字优先、最长匹配） |
| `%trivia` | 词法段 | `{ … }` 与 `(* … *)` 两种注释**匹配即丢**：词法器直接跳过，解析器本身不认识注释 Token |
| `%code ... %endcode` | 用户代码块 | 语义值类型 `ParserValue_T`、算子码宏、`PARSER_USER_TOKEN_VALUE`、帮助函数声明 |
| `%epilogue` | 文法末尾 | 符号表（栈式 + 限定名）、三元式/四元式表、注释闭合检查、帮助函数实现 |
| `%main` | 文法末尾 | 命令行入口（读 `.pl0` → 检查注释 → `RunText` → 打印三元式/四元式） |
| 语义动作 | 每条产生式 | 用 `$1` / `$$` 与帮助函数生成三元式/四元式；动作里只传宏与语义值（DSL 不支持字符串字面量） |
| `#line` | 生成产物 | 语法错误的行列号指回 `pl0.gram` / `pl0_quads.gram` |

三处用户代码的分工见仓库 [`../../doc/07-codegen.md`](../../doc/07-codegen.md) 的分工表：
**声明进 `%code ... %endcode`、实现进 `%epilogue`、入口进 `%main`**。

`pl0_quads.gram` 不参与 `check`（没有预置期望文件），它的价值是**可读的对照
版本**：同一份 PL/0 源码，跑 `make -C project/pl0-ir run` 与 `make -C project/pl0-ir run-quads`，
两份输出并排一看，三元式与四元式的差别（`(k)` vs `tN`、三列 vs 四列）立刻清楚。
