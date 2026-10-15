# `project/`：九个完整工程示例

`doc/` 讲的是生成器**自己**怎么实现；这里放的是**用生成器做出来的工程**——每份都是
「一份 `.gram` + 一个驱动 + 一批样例 + 期望输出」，能用 `make` 构建、
用 `make check` 自验，生成器本身一行都不用改。

Magel 语言已经提升为仓库根目录的独立工程，见 [`../magel/`](../magel/README.md)；
它不再属于 `project/` 示例集合。

## C++17 后端与虚拟机

| 工程 | 做什么 | 学什么 |
| --- | --- | --- |
| [`pl0-ir/`](pl0-ir/README.md) | 完整 PL/0 前端 → **三元式 / 四元式** | LALR + 语义动作直接产中间代码、作用域与符号表、`%trivia` 注释、`#line` 定位 |
| [`mini-basic/`](mini-basic/README.md) | Mini-BASIC → **栈式字节码 + 虚拟机** | `%left` / `%right` / `%prec` 消解表达式二义、mid-rule action 记循环起点、`$line` 报错定位 |
| [`pl0-super/`](pl0-super/README.md) | PL/0 超集 → **简化代码直接出目标机汇编** + 自带栈式目标机 | 不建 IR 的取舍；LALR(1) 与手写递归下降两套前端，含[技术报告](pl0-super/技术报告.md) |
| [`bigint-calc/`](bigint-calc/README.md) | **大整数计算器**：`2^10000`、`5000!`（16326 位）、4000 位字面量 | 语义值直接是 `library/big_number` 的 `BigInt`；`%token regex` 覆盖内建 NUMBER 绕开 int32 / double 限制；优先级声明 + `%prec` 解表达式二义 |

## Python 脚本与最小工程

| 工程 | 做什么 | 学什么 |
| --- | --- | --- |
| [`log-ir/`](log-ir/README.md) | 结构化日志 → **JSON**（`%commit` 把注释当数据交出来） | 免编译产物当脚本 / AI 接口、位置 API 落到输出与报错 |
| [`todo-scan/`](todo-scan/README.md) | 扫源码里的 **TODO / FIXME / NOTE** 出 Markdown | 最小的一份：`%mode lex` + `%commit`，一条产生式都不写 |

## DSL 与多目标代码生成

| 工程 | 做什么 | 学什么 |
| --- | --- | --- |
| [`protocol-dsl/`](protocol-dsl/README.md) | 二进制协议 → Python / C++17 / **零分配 C99** / **`no_std` Rust** 编解码器 + Markdown / Mermaid | Python / C++17 / Rust 三套编译器均由 ParseEngine 生成前端；大小端、枚举、嵌套 packet、动态字节串；面向嵌入式的借用输入与调用方输出缓冲区 |

## 课程实验

| 工程 | 做什么 | 学什么 |
| --- | --- | --- |
| [`lab_report_axhu/`](lab_report_axhu/README.md) | 安徽新华学院编译原理课程的三个实验：文法类型判断 / PL/0 词法分析 / PL/0 语法分析 | 用同一份生成器做课程作业：`%mode lex`、`%tree` + `%main` 遍历 CST、LALR(1) + `error` 恢复 |
| [`lab_report_nju/`](lab_report_nju/README.md) | 南京大学编译原理五次实验：严格 C-- 的词法语法、语义、IR、目标代码与优化 | ParseEngine 生成 LALR 前端，配套 AST / 符号表 / 四元式与 transfer 后端；五份报告和一键证据脚本 |

## 文档维护工具

[document/](document/README.md) 提供 Python 文档生成器、Markdown 模板和统一口径配置，维护公共说明及新技术文档。它不依赖解析器生成器，也不属于上述九个文法示例。

## 怎么跑

```sh
# 0) 先在仓库根目录构建生成器（九个示例工程都只用它，不改它）
make

# 1) 任选一个：生成（+ 编译）+ 自验
make -C project/pl0-ir      check     # PL/0 → 三元式
make -C project/pl0-ir      quads     # PL/0 → 四元式（另一份文法）
make -C project/mini-basic  check     # Mini-BASIC → 字节码，跑虚拟机
make -C project/log-ir      check     # 日志 → JSON（不需要编译）
make -C project/todo-scan   check     # 扫 TODO，出 Markdown 报告
make -C project/pl0-super   check     # PL/0 超集 → 目标机汇编并执行
make -C project/bigint-calc check     # 大整数计算器（2^10000 / 5000!）
make -C project/protocol-dsl check    # 二进制协议 → 四档编解码器与文档
# lab_report_axhu 不使用统一 Makefile，按 lab_report_axhu/README.md 的三条生成/编译命令运行
# lab_report_nju 用 PowerShell 运行 lab_report_nju/run-evidence.ps1
# 根目录 magel 与 project/bigint-calc 各有自己的挂载点（local.mk），按各自 README 运行
make -C magel check

# 2) 想看单个产物的用法，各工程的 README 里有逐条命令
```

前五个工程（`pl0-ir` / `mini-basic` / `pl0-super` / `log-ir` / `todo-scan`）
使用同一套目录约定，`check` 都是「正常样例比输出 + 错误样例比报错首行」：

```
project/<名字>/
├── <名字>.gram          文法（+ 驱动），唯一需要你写的文件
├── Makefile             生成 / 编译 / 自验（各工程的 make 目标略有差异：都有
│                        make / run / check / clean，pl0-ir 多 quads 与 run-quads，
│                        mini-basic 多 dump —— 以各自 README 为准）
├── check.sh             跑样例并比对期望输出
├── samples/             正常样例
├── expected/            正常样例的期望输出（已人工核对）
├── samples-error/       出错样例
├── expected-error/      出错样例的期望报错首行
├── README.md            这份工程自己的说明
└── build/               生成 / 编译产物（.gitignore 忽略，随时可重建）
```

指向生成器与样例的路径都写成相对路径（`ENGINE ?= ../../build/bin/ParseEngine`、
`samples/…`），所以从仓库任何位置 `make -C project/<名字>` 都能跑。

## 加一个新工程

1. 建目录 `project/<名字>/`，写一份 `<名字>.gram`（先用
   `./build/bin/ParseEngine --grammar-file … --out-dir /tmp/try --lang <档>` 试生成）；
2. 抄一份 `Makefile` / `check.sh` / `.gitignore`（改 `GRAMMAR`、`GEN_DIR` 与
   `PYTHON` 之类的变量即可），样例放 `samples/`、期望输出放 `expected/`；
3. 跑 `make -C project/<名字> check`，全绿之后再补 `README.md`；
4. 回到本文件与 [`../../doc/11-project-examples.md`](../doc/11-project-examples.md) 的表格里加一行，
   仓库根 `README.md` 的目录结构里也加上——不然别人翻不到。

写工程时最常踩的几类坑（都在这些工程里踩过并写在各自 README 里）：
注释在哪一层处理（`%trivia` vs 驱动里预剥）、`$line` 是**当前 lookahead** 的行、
动作里不能写字符串字面量 / `[]` / 以 `$n` 开头的括号、块注释正则的 DFA 友好写法。
