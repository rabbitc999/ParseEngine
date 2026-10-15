# SSA 与稀疏优化（`library/transfer/ssa`）

> 该模块已在合并库 [library/transfer](../README.md) 内，与 irbridge / core / dataflow / ssa / regalloc 共用**一套带位数信息的三地址 IR**（`ir/typed_ir.hpp`，指令类型、基本块类型、解析与打印各只有一份）。本文里的命令按**单模块**写；整条流水线用 `make -C library/transfer check` 或 `build/transfer-one-thread/t1t pipeline <file>`。

后端前半段里"让每条赋值只定义一个名字"的那一步。输入是一段带标签的三地址程序，
库把它走完一整条流水线：

```text
CFG → 支配树 / 支配边界 → φ 插入 → 变量改名（SSA 形式）
    → 稀疏条件常量传播（SCCP）→ 全局值编号（GVN）
    → φ 消除（切临界边 + 并行复制）→ 可执行形式
```

每一步都能单独调用，也都能单独打印，方便对着看：

| 模式 | 看到什么 |
| --- | --- |
| `--mode=summary`（缺省） | 每一步的指标：φ 条数、折叠条数、消掉的指令、指令数变化 |
| `--mode=ssa` | SSA 形式（φ 在块首，名字带下标 `x_1` / `x_2`） |
| `--mode=dom` | 支配树（Mermaid 源码，直接贴到支持 Mermaid 的编辑器里） |
| `--mode=sccp` | 常量传播 + 常量分支折叠 + 不可达块删除之后的程序 |
| `--mode=gvn` | 值编号（消重复表达式、冗余 φ、复制传播）之后的程序 |
| `--mode=destroy` | φ 消除之后的**可执行**形式（含切开的临界边与临时变量） |
| `--mode=run` | 直接解释执行输入的普通程序 |

每次流水线结束还会把**原程序**和 **φ 消除之后的程序**各跑一遍，比对输出与内存：
对不上就报出来（`test_ssa.cpp` 的 `testFallthroughPhi()` 与
`examples/02_diamond.ssa` 钉的就是这条）。

只用 C++17 标准库。

> 设计与完整说明（支配 / 支配边界公式、φ 插入与改名、SCCP 格、GVN 的支配检查、
> 并行复制排序、实测）见 [`技术报告.md`](技术报告.md)。

## 输入格式

每行一条指令，`;` / `#` / `//` 起注释（示例文件把元信息也写在 `#` 注释里）：

```text
label entry              ; 基本块入口
  mov a, 3               ; dst, src（src 可以是常量、变量、[内存]）
  add c, a, b            ; dst, src1, src2（sub / mul / div 同形）
  load x, [slot]         ; 从内存符号读进 x（内存不参与 SSA）
  store [out], c         ; 写内存
  jz x, L1               ; 条件跳转：x 为 0 就跳到 L1（jnz 同形）
  jmp L2                 ; 无条件跳转
  print c                ; 输出（解释器用，便于比对语义）
  halt
```

几个约定：

- **基本块**：分支 / `halt` 是终结指令，后面自动开一个新块，所以 `jz` 后面直接写
  落下的代码也成立（不必额外加 `label`）；
- **可改名变量**：普通名字（字母 / 数字 / 下划线）。`[x]` 是内存符号，**不做内存
  SSA**（`store` / `load` 原样保留，别名分析不在本库范围）；
- **SSA 名字**：原变量 `v` 会得到 `v_0`（入口初值）与 `v_1`、`v_2`…；块里
  没有定义却要用的位置写 `undef`；φ 消除时为打破复制环引入 `tmp_1`、`tmp_2`…
  （所以输入里最好不要自己用这种带下划线数字的名字）；
- 输出里的 φ 写成 `phi v_3, v_1, v_2    ; ← B1, L1`，实参按**前驱顺序**排列，
  后面那截注释只是给人看的（标注这一位对应哪个前驱）。

## 命令行

```text
ssa [--mode=summary|ssa|dom|sccp|gvn|destroy|run] [--steps=N] [file]
ssa --help / --version
```

不给文件就读标准输入，输出写标准输出（Windows 下先把控制台切到 UTF-8）。
`--steps=N` 是 `--mode=run` 的步数上限（缺省 20 万，用来兜住死循环）。

```text
build/ssa/ssa --mode=ssa     library/transfer/ssa/examples/02_diamond.ssa
build/ssa/ssa --mode=destroy library/transfer/ssa/examples/04_loop_swap.ssa
```

## 构建与自检

```text
make -C library/transfer/ssa            # 构建 build/ssa/{libssa.a, ssa, ssa-tests, ssa-examples}
make -C library/transfer/ssa check      # 单元测试 + 5 个示例 + 三段打印（summary / dom / ssa）
make -C library/transfer/ssa clean      # 删掉 build/ssa
```

`ssa-examples` 对 `examples/*.ssa` 逐个做四件事：解释原程序并与文件里
`# 期望:` 比对；比对 `# 期望 φ:`、`# 期望 折叠:`、`# 期望 不可达:`、
`# 期望 GVN:`；比对 φ 消除前后的语义；打印一行结论。

## 示例与实测

| 示例 | 说明 | φ | SCCP 折叠 | 删掉块 | GVN 消表达式 | φ 消除后复制 | 指令数 | 输出 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `01_assign.ssa` | 直线代码，没有汇合点 | 0 | 1 条 | 0 | 0 | 0 | 6 → 6 | `7` |
| `02_diamond.ssa` | 菱形汇合，条件来自 `load` | 1 | 0 | 0 | 0 | 2 | 7 → 9 | `3` |
| `03_const_branch.ssa` | 条件在编译期已知 | 1 | 0 | 1 | 0 | 0 | 7 → 5 | `2` |
| `04_loop_swap.ssa` | 循环里的交换（回边 φ） | 6 | 0 | 0 | 0 | 7 | 16 → 19 | `2 1 3` |
| `05_redundant.ssa` | 重复表达式与冗余 φ | 1 | 0 | 0 | 1 | 0 | 17 → 14 | `24 24` |

几处值得对着输出看的现象：

- **φ 只在需要的地方出现**：`04` 的循环头上有 `a`、`b`、`n` 三条 φ，而同一块里
  定义的 `t`、`k` 不活跃在块入口，按"剪枝 SSA"不插 φ（不剪枝的话这条程序会插
  10 条，`--mode=summary` 会写"插入 φ：6 / 剪枝掉的 φ：4"）。`--mode=ssa` 能直接
  看到差别。
- **常量分支能删掉整块**：`03` 的 `x` 是常量 1，`jz x, dead` 永远不跳，SCCP 把
  `dead` 判成不可执行并删掉，`out` 处的 φ 只剩一个实参，接着被 GVN 归并。
- **φ 消除会加指令**：`02`、`04` 的指令数**变多**了——φ 换成"每个前驱一份并行
  复制"本来就要额外的搬动；省下来的是后面寄存器分配的压力（`a`、`b` 不再是同一
  寄存器里的别名）。`04` 里交换用的复制还成环，得借一个 `tmp_1` 才排得开。
- **GVN 是真省**：`05` 的两条 `mov t, s` 归并到 `s` 之后，φ 的实参变成同一个名字，
  φ 直接消失；`add q, s, s` 与 `add r, s, s` 是同一个值编号，`q` 也消失（17 → 14）。
- **语义对照**：5 个示例的"优化前后输出一致"全部通过（`--mode=summary` 的最后一行）。

## 目录

```text
library/transfer/ssa/
├── ssa.hpp / ssa.cpp   解析、CFG、支配树 / 支配边界、活跃变量、φ 插入与改名、
│                       SCCP、GVN、φ 消除、解释器、流水线与指标
├── main.cpp            命令行（7 种模式，读文件或标准输入）
├── test_ssa.cpp        单元测试（含"顺序落下的边"回归与语义对照）
├── example_suite.cpp   示例运行器（读 examples/*.ssa 并比对 4 类期望）
├── Makefile / CMakeLists.txt
└── examples/           5 个示例程序 + 说明
```

