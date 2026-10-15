# Mini-BASIC → 栈式字节码（C++17 工程示例）

这是 `project/` 下七个完整工程示例之一（总入口见 [`../README.md`](../README.md)）。
它比 [`pl0-ir`](../pl0-ir/README.md) 小，
但补上了那一份没有的东西：**解析动作在归约
的那一刻直接发字节码，末尾由一个几十行的栈式虚拟机执行**，所以样例能真的跑出结果，
而不是只打印语法树。生成器本身一行没改，工程里只有文法、样例与构建脚本。

**只针对 C++17 产物**（`--lang cpp17`）：文法的 `%namespace basic`、`%main` /
`%epilogue` 的写法、以及 `Makefile` 的构建都按 C++17 来。

```
project/mini-basic/
├── mini_basic.gram        Mini-BASIC 文法 + 字节码生成 + 虚拟机（%epilogue / %main）
├── Makefile               make / generate / run / check / dump / clean
├── check.sh               跑全部样例并比对期望输出
├── samples/*.bas          7 个正常样例（见下面的测试矩阵）
├── samples-error/*.bas    4 个出错样例（语法错 + 运行时错）
├── expected/*.out         正常样例的期望 stdout（已人工核对）
├── expected-error/*.txt   出错样例的期望报错首行
└── build/                 生成与编译产物（.gitignore 忽略）
```

## 构建与运行

```sh
# 0) 先在仓库根目录构建生成器（本工程只用它，不改它）
make

# 1) 生成 + 编译（产物在 project/mini-basic/build/）
make -C project/mini-basic

# 2) 跑一个样例 / 全部样例
make -C project/mini-basic run                 # 跑 samples/while.bas
make -C project/mini-basic check               # 7 个正常 + 4 个错误

# 3) 看某份程序编译出来的字节码（stderr 打反汇编）
make -C project/mini-basic dump                # = MINI_BASIC_DUMP=1 … samples/nested.bas
MINI_BASIC_DUMP=1 project/mini-basic/build/parser_demo project/mini-basic/samples/while.bas
```

`make -C project/mini-basic check` 的输出：

```
[ok]   comments / expr / hello / if_else / nested / vars / while
[ok]   7 个正常样例
[ok]   bad_char（报错：第 1 行第 9 列：认不出来的字符）
...
全部通过（7 个正常样例 + 4 个错误样例）
```

## 语言范围

| 类别 | 支持 |
| --- | --- |
| 语句 | `let x = 表达式;`、`x = 表达式;`（可省 `let`）、`print 表达式;`、`if 条件 then 语句表 [else 语句表] end;`、`while 条件 do 语句表 end;` |
| 表达式 | `+ - * / %`、一元负号、括号、六种比较 `== != < <= > >=`（结果是 1 / 0） |
| 变量 | 赋值即声明；没赋过值就是 0；名字区分大小写 |
| 注释 | `' 到行尾` 与 `REM 到行尾`（两种都由 `%trivia` 在词法器里跳过） |

**不支持**：字符串、数组、函数、`goto`、行号、嵌套注释、`and/or/not`。

两个小细节：

- `REM` 后面必须是行尾 / 空白 / 非标识符字符，所以 `remark`、`REMARK` 这类
  **名字**还能正常当变量用（`samples/comments.bas` 就是这么写的）；但一个**刚好
  叫 `REM`** 的变量名写不出来。
- `print` 只接一个表达式。想一次打印多个值就写多条 `print`（这属于语言范围，不是
  生成器的限制）。

## 字节码与虚拟机

文法里的每条动作都在做两件事之一：**发一条指令**，或者**回填一条跳转**。指令只有
三个字段（算子、参数、源文法行号），参数按算子解释成常量 / 变量下标 / 跳转目标：

| 算子 | 参数 | 说明 |
| --- | --- | --- |
| `PUSH` / `LOAD` / `STORE` | 常量值 / 变量下标 | 压常量、压变量、弹栈顶存变量 |
| `ADD` `SUB` `MUL` `DIV` `MOD` `NEG` | — | 算术（除零与取模零在运行时报错） |
| `EQ` `NE` `LT` `LE` `GT` `GE` | — | 比较，结果 1 / 0 |
| `JMP` / `JZ` | 目标下标 | 无条件跳 / 栈顶为 0 时跳 |
| `PRINT` | — | 弹栈顶按 `%g` 打印（整数不带小数点） |
| `HALT` | — | 停机（`%main` 在解析完补一条） |

`samples/while.bas` 编译出来是这样（`make dump` 打的就是这个）：

```
   0: PUSH  0    (行 2)      let i = 0;
   1: STORE 0    (行 3)
   2: PUSH  0    (行 3)      let sum = 0;
   3: STORE 1    (行 4)
   4: LOAD  0    (行 4)      while i < 5 do   ← 循环起点：条件从这里开始
   5: PUSH  5    (行 4)
   6: LT    0    (行 4)
   7: JZ    17    (行 4)     ← 为假跳出循环
   8: LOAD  1    (行 5)      sum = sum + i;
   9: LOAD  0    (行 5)
  10: ADD   0    (行 5)
  11: STORE 1    (行 6)
  12: LOAD  0    (行 6)      i = i + 1;
  13: PUSH  1    (行 6)
  14: ADD   0    (行 6)
  15: STORE 0    (行 7)
  16: JMP   4    (行 8)      ← 跳回**条件代码**，不是跳回 JZ
  17: LOAD  1    (行 8)      print sum;
  18: PRINT 0    (行 9)
  19: LOAD  0    (行 9)      print i;
  20: PRINT 0    (行 10)
  21: HALT  0    (行 0)
```

（`JZ` / `JMP` 的参数是「指令下标」。指令上的行号来自动作里的 `$line`，而
`$line` 是**当前 lookahead** 的行——所以 `STORE` 那种「语句末尾才归约」的指令会
报成下一行（分号后面换行了）。它只用于运行时错误的粗略定位：算术 / 比较 / `print`
这些「同一行内归约」的指令行号是准的，`print 1 / 0;` 就报在第 2 行。）

## 两个踩过的坑（都在跳转回填里）

1. **`while` 的回跳目标必须是「条件代码开头」，不是 `JZ` 的位置。** 一开始我把
   循环起点记在发 `JZ` 的时候，结果回跳跳到 `JZ` 上——第二次判断时栈上已经没有
   条件值，虚拟机读空栈，直接堆损坏崩掉。修法是用一个中间非终结符当
   **mid-rule action**：`while_mark := WHILE` 先记下起点，再编译条件，最后才发 `JZ`。
2. **`if` 的 `JZ` 要回填到「跳过 else 的那条 `JMP` 之后」。** 先回填再发 `JMP`，
   条件为假时就会正好落在 `JMP` 上，被它直接送到 `end`——`else` 分支永远不执行
   （表现是「else 里的 print 一句都不输出」）。正确顺序是：先发 `JMP`（待回填），
   再把 `JZ` 回填到 `JMP` 之后，最后 `end` 时回填 `JMP` 到尾部。

第 1 条尤其值得记：它不会报语法错、也不一定马上崩——只有循环**真的转第二圈**时才
发作（`while 0 do …` 这种一次都不进循环的写法完全看不出问题）。用
`MINI_BASIC_DUMP=1` 把字节码打出来，一眼就能看出回跳目标错了。

## 这个工程用到生成器的哪些特性

| 用到的特性 | 位置 | 作用 |
| --- | --- | --- |
| `%mode lalr` | 文件头 | Mini-BASIC 是 LALR(1) 文法，直接产出确定性解析器 |
| `%namespace basic` | 文件头 | C++17 产物的命名空间与文件名前缀（`basic_parser.hpp` 等） |
| `%value_type double` | 文件头 | 语义值就是 double：NUMBER 是数字本身，标识符是名字池下标 |
| `%identifier` + `%define` | 词法段 | 标识符走 DFA，关键字 / 运算符走固定字面量（关键字优先、最长匹配） |
| `%trivia` | 词法段 | `' …` 与 `REM …` 两种注释匹配即丢，解析器本身不认识注释 |
| `%left` / `%right` / `%prec` | 优先级段 | expr 写成二义形式，优先级全在生成期消解，运行时零判断 |
| mid-rule action（中间非终结符） | `while_mark` | 在「条件代码之前」记下循环起点（`WHILE` 一移进就记） |
| `%code ... %endcode` | 用户代码块 | 算子码枚举、`PARSER_USER_TOKEN_VALUE`、帮助函数声明、标准库头 |
| `%epilogue` | 文法末尾 | 字节码表 / 名字池 / 三个补丁栈、代码生成函数、虚拟机、反汇编 |
| `%main` | 文法末尾 | 命令行入口：读文件或 stdin → 解析（边归约边发码）→ 跑虚拟机 |
| `$line` | 语义动作 | 每条指令记下源文法行号，运行时报错能指到行（`运行时错误：第 2 行：除以零`） |
| 结构化位置 | `%main` 的错误分支 | 语法错误打「第 N 行第 M 列：期望 X / Y 之一，实际是 Z」 |
| `#line` | 生成产物 | 语法错误的行号指回 `mini_basic.gram` |

## 样例与期望输出

`samples/`（成功 + 与 `expected/*.out` 逐行比对）：

| 样例 | 覆盖 |
| --- | --- |
| `hello.bas` | 最小程序：一条 `print` + 行尾 `REM` 注释 |
| `vars.bas` | 赋值、用变量算新变量、未赋值变量当 0、负数结果 |
| `if_else.bas` | `if/else/end`、省略 `else`、连续两个 `if` |
| `while.bas` | `while` 循环 + 循环外打印（回跳正确性的主样例） |
| `nested.bas` | `if` 里套 `while`、`while` 里再套 `if/else`（三层跳转回填） |
| `expr.bas` | 优先级、括号、一元负号、取模、六种比较 |
| `comments.bas` | 两种注释、注释里的关键字不生效、`rem` 开头的变量名 |

`samples-error/`（必须报错退出，stderr 首行与 `expected-error/*.txt` 一致）：

| 样例 | 报错 |
| --- | --- |
| `syntax.bas` | 表达式缺右操作数：`期望 NUMBER / VARIABLE / MINUS / LPAREN 之一，实际是 SEMI` |
| `missing_end.bas` | `if` 少了 `end;` |
| `bad_char.bas` | 词法错：`认不出来的字符` |
| `div_zero.bas` | 运行时错：`运行时错误：第 2 行：除以零` |

加样例就是往 `samples/` 放 `.bas`、把跑出来的 stdout 存进 `expected/<名字>.out`
（先人工核对数值对不对，再固化）；错误样例同理放 `samples-error/` +
`expected-error/<名字>.txt`（只比首行）。
