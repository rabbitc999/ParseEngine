# 实验三 · PL/0 语法分析程序

安徽新华学院编译原理课程实验报告。

## 一、实验目的

实现 PL/0 的语法分析程序：对源程序做 LALR(1) 分析，输出**语法树**；对错误程序
能**恢复**并一次报出多条错误。

## 二、实验仪器

Windows 电脑；本仓库的 **ParseEngine**（`make` 编译生成器）；产物用 gcc
（MinGW-w64）编译。

## 三、实验原理

### 3.1 文法

与实验讲义给出的 LALR(1) 文法一致，只把 `RELOP` / `ASSIGNOP` 展开成各自的运算符
Token（生成期好按优先级消解冲突）：

```text
Program  -> StmtList DOT
StmtList -> Stmt | Stmt StmtList | CompSt StmtList | error StmtList
Stmt     -> VAR DecList SEMI | CONST ConstList SEMI | FunDec
          | CALL ID SEMI | Exp SEMI
          | IF Exp THEN CaseSt | WHILE Exp DO CaseSt
          | READ LP ID RP SEMI | WRITE LP Exp RP SEMI
          | error SEMI
CaseSt   -> Stmt | CompSt
CompSt   -> PBEGIN StmtList PEND
DecList  -> ID | ID ASSIGNOP NUMBER | ID COMMA DecList
          | ID ASSIGNOP NUMBER COMMA DecList
ConstList-> ID ASSIGNOP NUMBER | ID ASSIGNOP NUMBER COMMA ConstList
FunDec   -> PROCEDURE ID SEMI CompSt
Exp      -> Exp ASSIGNOP Exp | Exp RELOP Exp | Exp PLUS Exp
          | Exp MINUS Exp | Exp STAR Exp | Exp SLASH Exp
          | LP Exp RP | ODD Exp | ID | NUMBER
```

两点说明：

- 讲义样例里写的是 `const b = 10;`（用的是 `=`），而文法里 ConstList 用的是
  `ASSIGNOP`（`:=`）。本实现两种都收，所以样例能干净地通过。
- `begin` / `end` 对应文法里的 `PBEGIN` / `PEND`——`END` 是运行时「输入结束」
  哨兵 Token 的保留名，不能拿来当 `end` 关键字的 Token 名（否则两者会合并）。

### 3.2 二义性与优先级

`Exp -> Exp OP Exp` 天然有二义（`1 + 2 * 3` 可以是 `(1+2)*3` 或 `1+(2*3)`）。
按讲义表里的优先级在生成期消解，`.gram` 里按「越靠下优先级越高」写：

```gram
%left ASSIGNOP                     /* 6 赋值（最低） */
%left EQ NE NE2 LE GE LT GT        /* 5 关系运算符 */
%left PLUS MINUS                   /* 4 加减 */
%left STAR SLASH                   /* 3 乘除 */
%right ODD                         /* 1 单目 odd（最高，括号隐含最高） */
```

生成器按 yacc 的规矩消解移进/归约冲突：**优先级高的胜出，同优先级看结合性**。
`1 + 2 * 3` 因此只会得到 `1+(2*3)` 一棵树；`a - b - c` 按 `%left` 得到
`(a-b)-c`。

### 3.3 错误恢复

讲义文法里已经写了三条 `error` 产生式：

```text
StmtList -> error StmtList
Stmt     -> error SEMI
```

运行时遇到语法错误，会弹栈到能移进 `error`（伪 Token）的状态，把 `error` 移进、
丢弃一段输入，再从同步点继续——所以**一次解析能报多条错**，而且还能给出一棵
「去掉坏片段之后」的部分语法树。错误记录里有行号、列号、期望的 Token 集合，
上限由 `PARSER_MAX_ERRORS`（缺省 8）控制。

## 四、程序代码

完整程序见同目录的 [`lab3_parser.gram`](lab3_parser.gram)（词法规则、文法、优先级、
`error` 产生式，以及 `%main` 里打印错误列表与语法树的驱动）。

```bash
make
build/bin/ParseEngine project/lab_report_axhu/lab3_parser/lab3_parser.gram --lang c --out-dir out/lab3
make -C out/lab3
./out/lab3/parser_demo < project/lab_report_axhu/lab3_parser/good.pl0
./out/lab3/parser_demo < project/lab_report_axhu/lab3_parser/bad.pl0
./out/lab3/parser_demo < project/lab_report_axhu/lab3_parser/bad_lex.pl0
```

## 五、实验结果

### 5.1 正确的 PL/0 程序

输入 `good.pl0`：

```text
var x, y, m, t;
const b = 10;
procedure mod;
begin
    t := x / y;
    t := t * y;
    m := x - t;
end.
```

输出（错误列表为空，语法树按层次缩进；终结符写成 `名字(词素)` 的形式，
非终结符只有名字）：

```text
=============== 错误列表 ===============
（无）
=============== 语法树 ===============
Program
  StmtList
    Stmt
      VAR(var)
      DecList
        VARIABLE(x)
        COMMA(,)
        DecList
          VARIABLE(y)
          COMMA(,)
          DecList
            VARIABLE(m)
            COMMA(,)
            DecList
              VARIABLE(t)
      SEMI(;)
    StmtList
      Stmt
        CONST(const)
        ConstList
          VARIABLE(b)
          EQ(=)
          NUMBER(10)
        SEMI(;)
      StmtList
        Stmt
          FunDec
            PROCEDURE(procedure)
            VARIABLE(mod)
            SEMI(;)
            CompSt
              PBEGIN(begin)
              StmtList
                Stmt
                  Exp
                    Exp
                      VARIABLE(t)
                    ASSIGNOP(:=)
                    Exp
                      Exp
                        VARIABLE(x)
                      SLASH(/)
                      Exp
                        VARIABLE(y)
                  SEMI(;)
                StmtList
                  Stmt
                    Exp
                      Exp
                        VARIABLE(t)
                      ASSIGNOP(:=)
                      Exp
                        Exp
                          VARIABLE(t)
                        STAR(*)
                        Exp
                          VARIABLE(y)
                    SEMI(;)
                  StmtList
                    Stmt
                      Exp
                        Exp
                          VARIABLE(m)
                        ASSIGNOP(:=)
                        Exp
                          Exp
                            VARIABLE(x)
                          MINUS(-)
                          Exp
                            VARIABLE(t)
                      SEMI(;)
              PEND(end)
  DOT(.)
```

树形与讲义里给出的期望结构一致：`Program` 下是 `StmtList` 的右递归链，
每层一个 `Stmt`；表达式按优先级形成 `Exp ASSIGNOP Exp` → `Exp SLASH Exp`
这样的左右结构，乘除在加减下面一层。差别只在两点：

1. 本实现的 CST 节点名用 Token 名（`VARIABLE` / `PBEGIN` / `ASSIGNOP`），
   讲义里是 `ID` / `Keyword(PBEGIN)` / `Operator(ASSIGNOP)`；
2. 本实现的 CST 不带行号，行号在**错误记录**里给（见 5.2）。

### 5.2 错误程序与恢复

输入 `bad.pl0`（把讲义样例里的词法错误 `9a` 去掉，只留语法错误）：

```text
var x, y, m, t, z-;
procedure mod;
begin
    t := x / y;
    t := t * ;
    2 * m := x - t;
end.
```

输出：

```text
=============== 错误列表 ===============
Error (Line 1): code=-26 column=18 语法错误：期望 ASSIGNOP / COMMA / SEMI 之一，实际是 MINUS
Error (Line 5): code=-26 column=14 语法错误：期望 NUMBER / VARIABLE / ODD / LP 之一，实际是 SEMI
=============== 语法树 ===============
Program
  StmtList
    Stmt
      SEMI(;)
    StmtList
      Stmt
        FunDec
          PROCEDURE(procedure)
          VARIABLE(mod)
          SEMI(;)
          CompSt
            PBEGIN(begin)
            StmtList
              Stmt
                Exp
                  Exp
                    VARIABLE(t)
                  ASSIGNOP(:=)
                  Exp
                    Exp
                      VARIABLE(x)
                    SLASH(/)
                    Exp
                      VARIABLE(y)
                SEMI(;)
              StmtList
                Stmt
                  SEMI(;)
                StmtList
                  Stmt
                    Exp
                      Exp
                        Exp
                          NUMBER(2)
                        STAR(*)
                        Exp
                          VARIABLE(m)
                      ASSIGNOP(:=)
                      Exp
                        Exp
                          VARIABLE(x)
                        MINUS(-)
                        Exp
                          VARIABLE(t)
                    SEMI(;)
            PEND(end)
  DOT(.)
```

两处错误分别由 `Stmt -> error SEMI` 与 `Stmt -> error`（配合 `StmtList` 的
`error StmtList`）恢复：第 1 行的 `z-` 整条坏语句被丢掉（树里留下一个 `Stmt`→
`SEMI` 的恢复节点），第 5 行 `t := t * ;` 同理；**两处错误都报了出来**，
而且后面的 `2 * m := x - t;` 仍然正常分析进了树里——这就是错误恢复的效果。

### 5.3 词法错误（对照）

讲义样例里还有一个词法层面的错误 `9a`（数字后面直接跟字母）。把它放回原样
（`bad_lex.pl0`）后：

```text
=============== 错误列表 ===============
Error (Line 1): code=-26 column=18 语法错误：期望 ASSIGNOP / COMMA / SEMI 之一，实际是 MINUS
Error (Line 1): code=-22 column=21 词法解析失败
=============== 语法树 ===============
（没有可接受的语法树）
```

差别值得说明：**词法错误是致命的**——词法器读到 `9a` 时已经无法决定它是什么
Token，分析就此中止（`-22`），语法层的 `error` 恢复没机会介入。讲义里的原程序
把 `9a` 当成一个「Error ID Name」继续往下走，是它自己在词法层把这段兜住了；
本实现选择的是「词法错误直接报出来、不猜」，所以只报了前两条。

## 六、延伸：完整的三元式生成（`project/pl0-ir/`）

本实验做到「语法分析 + 语法树 + 错误恢复」为止。讲义在 `node.h` 那段也提过，
建文法树是**为后面的中间代码生成做准备**——这一步本仓库里已经有完整实现，就在
[`project/pl0-ir/`](../../pl0-ir/README.md)：

> **`project/pl0-ir/` 完整 PL/0 → 三元式 / 四元式（C++17 工程示例）**：用 ParseEngine 把
> `project/pl0-ir/pl0.gram` 生成成一套 C++17 解析器，再用它把 PL/0 源码编译成**三元式**
> （`i: (op, arg1, arg2)`）。生成器本身不需要任何改动，工程里只有文法、样例与
> 构建脚本（注释也在文法里用 `%trivia` 交给词法器跳过）。细节见
> [`project/pl0-ir/README.md`](../../pl0-ir/README.md) 与
> [`../../../doc/11-project-examples.md`](../../../doc/11-project-examples.md)。

```bash
make                      # 先在仓库根目录构建生成器
make -C project/pl0-ir           # 生成 + 编译（产物在 project/pl0-ir/build/parser_demo）
project/pl0-ir/build/parser_demo project/pl0-ir/samples/expr.pl0   # 跑一个样例
make -C project/pl0-ir check     # 9 个正常样例 + 13 个错误样例全量比对
```

`project/pl0-ir/samples/expr.pl0`：

```text
var x, y;
begin
    x := 1 + 2 * 3;
    y := (x - 1) / 2;
    write y
end.
```

输出（常量已折叠、临时量用 `(n)` 引用第 n 号三元式）：

```text
三元式：共 5 条
  0: (:=, 7, x)
  1: (-, x, 1)
  2: (/, (1), 2)
  3: (:=, (2), y)
  4: (write, y, _)
```

### 差异点

两者是**同一门 PL/0、同一条 LALR(1) 的分析路线**，差别全在「做到哪一步」和
「文法怎么摆」。下面按六组列清楚。

**1. 文法结构**

| 方面 | 本实验（lab3） | `project/pl0-ir/` |
| --- | --- | --- |
| 声明的位置 | `var` / `const` / `procedure` 都当**语句**（`Stmt -> VAR DecList SEMI \| CONST ConstList SEMI \| FunDec`），写在 `StmtList` 里 → **顺序任意、可以重复** | `Block -> const_part var_part proc_part stmt`，三段各自最多一次、顺序固定（三段都用 `%empty` 让它可以省） |
| `var` 带初值 | `DecList -> ID ASSIGNOP NUMBER`，允许 `var x := 10` | `var_list -> ID \| var_list COMMA ID`，**不带初值**，初值靠赋值语句 |
| 空语句 | 讲义文法里 `Stmt` 没有 ε，`begin end` 不合法 | `stmt -> … \| %empty`，`begin end`、`begin ; ; x := 1 end` 都合法 |
| 条件 | 直接用 `Exp`（`odd` 是 `Exp` 里的单目前缀），`IF Exp THEN …` | 单独一层 `cond -> ODD expr \| expr relop expr`，`relop -> = \| <> \| < \| <= \| > \| >=` |
| 过程 | `FunDec -> PROCEDURE ID SEMI CompSt` | `proc_head` + `proc_decl`，过程**可嵌套** |
| 语句表 | `StmtList -> Stmt \| Stmt StmtList \| CompSt StmtList \| error StmtList` | `stmt_list -> stmt \| stmt_list SEMI stmt` |
| 错误恢复 | 三条 **`error` 产生式** | **没有** `error` 产生式 |

最实际的一条：**经典 PL/0 的声明顺序是 `const` → `var` → `procedure`**，而本实验
的样例把 `var` 写在了 `const` 前面（讲义文法允许任意顺序的结果）。所以本实验的
`good.pl0` 直接喂给 `project/pl0-ir/` 会报「期望 VARIABLE / PROCEDURE / BEGIN / IF
等 12 个之一，实际是 CONST」——把 `const` 那行挪到 `var` 前面就能出三元式。

**2. 语义与产物**

- 本实验：`%tree`，**产生式不带动作**，产出 CST 交给调用方（树在 `%main` 里打印）。
- `project/pl0-ir/`：**没有 `%tree`**，每条产生式都带语义动作，规约的同时往符号表和
  三元式表里写——**不建树**，树只是中间手段。

**3. 中间代码**

- 本实验：没有。
- `project/pl0-ir/`：三元式 `i: (op, arg1, arg2)`——常量折叠、`(n)` 引用第 n 号三元式的
  临时量、`if` / `while` 的 `label` / `jz` / `j` 回跳、`proc` / `endproc` / `call`、
  过程局部变量的**限定名**（`add.step`）。

**4. 符号表与作用域**

- 本实验：没有。
- `project/pl0-ir/`：栈式符号表 + 限定名，支持过程嵌套、同名遮蔽、非局部引用。

**5. 词法**

| 方面 | 本实验 | `project/pl0-ir/` |
| --- | --- | --- |
| 标识符 | 内建 `VARIABLE` 规则 | `%identifier` 显式声明（走 DFA） |
| 关键字 / 运算符 | `%define` 固定字面量 | 同，但多 `?` / `!`（`read` / `write` 的别名） |
| 注释 | 不支持 | `{ … }` 与 `(* … *)` 两种，文法里两条 **`%trivia`** 交给词法器**跳过**（匹配即丢，原文不动、行列号一致） |
| 比较运算符 | `=` `#` `<>` `<` `<=` `>` `>=` | 同 |

**6. 错误处理与用户代码分工**

- 错误处理各修了一半：本实验修的是**语法错误恢复**（`error` 产生式 → 一次报多条
  + 部分语法树）；`project/pl0-ir/` 不恢复语法错（第一条就停），但多了整套**语义检查**
  ——未声明标识符 / 重复声明 / 给常量赋值 / `read` 常量 / 过程当变量 / `call` 变量 /
  `call` 未声明过程 / 常量除零 / 注释没闭合，13 个错误样例各比一行报错。
- 用户代码：本实验只有 `%main` 一个打印驱动；`project/pl0-ir/` 是
  `%code ... %endcode`（类型与声明）+ `%epilogue`（实现）+ `%main`（入口）三分工。

## 七、实验总结

- 优先级写对之后，`1 + 2 * 3` 这类二义表达式在**生成期**就被消解掉了，运行时
  不需要任何额外判断；`Exp` 一棵树即可表示。
- `error` 产生式的恢复效果明显：错误程序报出 2 条错，同时给出一棵树，
  `2 * m := x - t;` 这段坏语句**后面**的代码仍然正常进了树。
- 与讲义实现的两点差异是**工具定位**造成的，不是缺陷：CST 不带行号（行号在
  错误记录里，且更精确，带列号与期望 Token 集合）；词法错误直接中止，不做
  「猜一个 Token 继续走」。
- 踩坑：`%define END "end"` 会和内置的输入结束哨兵合并，`end` 关键字必须换个
  Token 名（本实现用 `PEND`，与讲义一致）。
