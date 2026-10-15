# `ll1/`：手写递归下降（LL(1) 的直接编码）

## 这一层是什么

本目录是 pl0-super 的**手写前端**：`lexer.cpp`（词法）+ `parser.cpp`（**递归下降**的预测分析器）。
它和 `../lalr1/` 那份是**同一门语言的两种前端**，产出的"简化代码"结构相同，后面接的是同一套库
（`simplify.cpp` / `codegen.cpp` / `vm.cpp`）。

⚠️ 三者的关系要先分清（细节见 [`../语法分析对比.md`](../语法分析对比.md)）：

1. `../lalr1/` 是 **LALR(1) 表驱动**（生成器出的，另一类算法）；
2. **LL(1) 表驱动**是"建一张 $M[A,t]$ 预测表 + 显式栈"跑（生成器 `--mode ll1` 的产物）；
3. 本目录是 **LL(1) 的直接编码**——不建表，把同一张表的分支**写进递归函数**。

只要那份文法本身是 LL(1) 的，② 和 ③ 接受的语言**完全一样**；差别是实现代价与运行常数
（③ 省掉建表与查表，多一层函数调用，实测最快）。

## LL(1) 算法（自顶向下）

1. **求 FIRST / FOLLOW**：对每个非终结符算 $FIRST(\alpha)$，再算 $FOLLOW(A)$；
2. **求预测集**：$SELECT(A\to\alpha)=FIRST(\alpha)\cdot FOLLOW(A)$（$\alpha$ 可空时并入）；
3. **填预测表** $M[A,t]$：同一格只能有一条产生式，否则**不是 LL(1)**；
4. **分析**：从栈里的 $S$ 出发，看栈顶非终结符 $A$ 与当前记号 $t$，查 $M[A,t]$ 决定用哪条
   产生式（表驱动）；合成就成了**递归下降**——每个非终结符写一个函数，函数体的分支顺序
   就是 $M[A,\cdot]$ 的编码，本目录的 `parser.cpp` 就是这样：

   ```
   parseStmt        → 看首记号是 begin / write / if / while / for / goto / label / call … 分支
   parseExpr        → parseOr → parseAnd → parseCompare → parseAdd → parseMul → parseUnary
                      （表达式**按优先级分层**，靠结构消解二义，不靠声明）
   ```

   一份可选的"先导记号 → 该走哪个分支"的对照：`parseDecls` 里 `const`/`var`/`array`/`str`/
   `struct`/`ref`/`procedure` 各占一支，每支只看当前一个记号就能决定——这就是 LL(1) 的
   "一个记号展望"。

## 优点

- **实现直观、可调试**：一层一个函数，调用栈就是推导过程，报错点就在函数里；加一条语法
  往往只是加一个 `if`。
- **不用生成表**：没有分析表与状态机，产物小（本工程的实测：可执行 1,202,195 B）。
- **错误信息好写**：手里有"期望什么"的上下文，`fail("if 的分支要用 begin … end 包起来")`
  这种信息 LL(1) 天然写得出。
- **零工具依赖**：不依赖 ParseEngine，`g++` 直接编。

## 缺点

- **文法限制最硬**：不能有左递归、不能有公共左因子，$SELECT$ 集不能相交；否则必须改写
  文法（左递归消除、提取左因子），改写后语义动作的插入点跟着变。
- **表达式必须分层**：本工程的表达式是 7 层（or → and → 比较 → 加减 → 乘除模 → 一元 → 基本项），
  每层一个函数；**优先级写在代码结构里**，加一个运算符就要动对应用户层，不像 LALR(1) 那样
  加一行 `%left` 就能消解二义。
- **回溯要么禁止、要么代价高**：真正的 LL(1) 一次只有 1 个记号展望，遇到需要更多展望的
  地方（本工程 `for ... downto` 这类）只能提前多读或多写一层，不能像 LR 那样"先移进再决定"。
- **错误恢复弱**：LL(1) 一旦预测表查不到就只能报错；要做"跳过记号继续"得自己写同步集合
  （LALR(1) 有 `error` 产生式 + 弹栈恢复的标准做法，见 `../../lab_report_axhu/lab3_parser/`）。

## 这份文法的 LL(1) 现状（实测证据）

把 `../lalr1/pl0-super.gram`（带 `%left/%right` 的二义表达式文法）**直接换成 `--mode ll1`**
生成，生成器会拒绝：

```text
错误：文法不是 LL(1)：M[expr__tail, AND] 同时对应
      expr__tail -> AND expr expr__tail 和 expr__tail -> %empty
```

原因很直白：LL(1) 世界里**没有优先级声明**——`%left AND` 这类信息只能通过"把表达式分层"
编码进文法结构。要把它做成真 LL(1) 的文法，得写成经典形状：

```text
  expr  := term expr_tail
  expr_tail := OR term expr_tail | %empty
  term  := factor term_tail
  term_tail := AND factor term_tail | %empty
  factor := primary cmp_tail
  ...
```

也就是说 **LALR(1) 用声明消解的二义，LL(1) 必须用结构消解**——这正是下面
[`../语法分析对比.md`](../语法分析对比.md) 里两种算法的核心差别。

## 怎么跑

```sh
make -C project/pl0-super lib                       # 编出两套前端都要用的库对象
g++ -std=c++17 -O2 -I. -Ill1 ll1/lexer.cpp ll1/parser.cpp simplify.cpp codegen.cpp vm.cpp main.cpp -o build/pl0-super-lib
build/pl0-super-lib samples/array_sum.p0s           # 手写 LL(1) 前端：编译 + 跑目标机
build/pl0-super-lib --stats samples/simplify_demo.p0s
```

（`build/pl0-super` 是 `../lalr1/` 那份生成前端；两份的输出由 `../check.sh` 用同一批样例比对。）
