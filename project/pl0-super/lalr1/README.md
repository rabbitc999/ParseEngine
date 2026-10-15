# `lalr1/`：LALR(1) 前端（自底向上的移进-归约分析）

## 这一层是什么

本目录是 pl0-super 的 **LALR(1) 前端**：`pl0-super.gram` 一份文法，交给仓库里的
ParseEngine 生成 LALR(1) 分析表与解析器（`--lang cpp17`）。**语义动作就写在文法里**
（照 [`../../pl0-ir/pl0.gram`](../../pl0-ir/pl0.gram) 的分工）：

```
%code ... %endcode   类型、Token→语义值钩子、动作函数原型（parser 与 main 共享）
%epilogue   动作函数定义 + 语义值池子（栈、表达式/语句/设计式/类型/作用域/前缀栈）
%main       驱动：解析完直接用池子拼出的 Program 跑 simplify → generate → runAsm
```

## LALR(1) 算法（自底向上）

1. **增广文法** $G' = G \cup \{S'\to S\}$；
2. **项目集规范族**：LR(0) 项目（$A\to\alpha\cdot\beta$）做闭包与转移，得到 LR(0) 自动机；
3. **看一个展望符**：给项目带 1 个终结符的展望集，得到 LR(1) 项目；
4. **合并同核项集**：把"核心（项目本体）相同、展望不同"的 LR(1) 项集合并——这是 LALR(1)
   与 LR(1) 的唯一差别，表因此小一个量级，代价是**可能引入 LR(1) 没有的归约/归约冲突**；
5. **填 ACTION / GOTO**：移进 (shift) / 归约 (reduce) / 接受 (accept) / 报错；冲突用
   **优先级与结合性声明**或文法结构消解；
6. **分析**：栈里放"状态 + 已识别的符号"，遇到记号按 ACTION 移进或按产生式规约（弹 $|\alpha|$
   个符号、按 GOTO 压新状态），规约的动作就是文法里的 `{ … }`。

本工程里这条流水线看得见的两处：

- `%left OR / AND / 比较 / 加减 / 乘除模` 与 `%right NOT UMINUS`：**二义表达式文法**靠优先级
  声明在生成期就消解掉移进/归约冲突，运行时一句判断都没有（对比 [`../ll1/README.md`](../ll1/README.md)：
  同样的表达式在 LL(1) 里必须分层）。
- `if / while / for` 的分支**强制写 `begin … end`**（`block_stmt`）：悬挂 `else` 的移进/归约
  冲突在**文法结构**上就不存在，不需要 `%left THEN / %left ELSE` 那套优先级硬掰。

## 优点

- **接受的文法多**：左递归天然支持（`stmts := stmts stmt` 这种写法在 LL 里是禁止的）；
  二义可以用优先级/结合性声明消解，表达式不必分层。
- **分析是确定的、线性的**：每个记号只移进/规约一次，没有回溯；表驱动只在生成期算一次。
- **错误恢复有标准做法**：`error` 产生式 + 弹栈找可移进项，能做到"报错之后继续解析"
  （本仓库的实验三 [`../../lab_report_axhu/lab3_parser/`](../../lab_report_axhu/lab3_parser/) 就是
  正确程序 0 错出整棵树、错误程序报 2 条错且坏语句后面的代码仍进树）。
- **声明式**：优先级、结合性、冲突处理都写在文法里；改文法只改一处（本工程的 LL(1) 前端
  要同时改 `lexer.cpp` 与 `parser.cpp` 两个文件）。

## 缺点

- **生成期要算表**：状态机与 ACTION/GOTO 表让产物变大（本工程实测：可执行 2,055,291 B
  vs LL(1) 那份 1,202,195 B），生成的解析器源码 3940 行（机器产出，不手改）。
- **依赖生成器**：构建要仓库里的 ParseEngine；这份文法不能只用 `g++` 编出前端。
- **冲突要靠经验**：报"移进/归约冲突"时要判断是语法真二义（改文法）还是可以声明消解；
  LALR(1) 的合并还可能出现**只在 LALR 里存在**的归约/归约冲突（LR(1) 没有），这时要么换
  IELR(1)/LR(1)，要么改文法。
- **调试不如递归下降直观**：出错时看到的是状态号与项集（`RESULT.md` 里能查），不像 LL(1)
  那样"函数调用栈一眼看到哪一层"。

## 怎么跑

```sh
make -C project/pl0-super grammar    # 生成前端（build/pl0sgram_parser.* + pl0sgram_main.cpp）
make -C project/pl0-super check      # 生成 + 编译 + 跑 13 个样例（走的就是这份前端）
make -C project/pl0-super asm        # 看目标机汇编
make -C project/pl0-super stats      # 看四趟简化各命中多少次
```

想换别的分析模式（同一份文法）：`../../build/bin/ParseEngine --grammar-file lalr1/pl0-super.gram
--mode ielr --out-dir build-ielr --lang cpp17` —— `--mode` 会覆盖文法里的 `%mode`。
