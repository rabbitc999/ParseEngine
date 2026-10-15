# 数据流框架（`library/transfer/dataflow`）

> 该模块已在合并库 [library/transfer](../README.md) 内，与 irbridge / core / dataflow / ssa / regalloc 共用**一套带位数信息的三地址 IR**（`ir/typed_ir.hpp`，指令类型、基本块类型、解析与打印各只有一份）。本文里的命令按**单模块**写；整条流水线用 `make -C library/transfer check` 或 `build/transfer-one-thread/t1t pipeline <file>`。

把"格 + 传递函数 + 收敛算法"从具体分析里抽出来：**新分析只写格与函数**，
求解、迭代、对照、指标全交给框架。库里附带四个实例，前三个就是
`library/transfer` 优化器里手写的那三个分析：

| `--analysis=` | 分析 | 方向 | 格与合并 | 一句话 |
| --- | --- | --- | --- | --- |
| `live` | 活跃变量 | 后向 | 集合 / 并集（may） | 这条语句之后还会用到哪些变量 |
| `reach` | 到达定值 | 前向 | 集合 / 并集（may） | 哪些定义能活着走到这里 |
| `avail` | 可用表达式 | 前向 | 集合 / **交集**（must） | 到这里为止一定算过、且之后没被改过的表达式 |
| `const` | 常量传播 | 前向 | 映射 / ⊤ < 常量 < ⊥ | 每个变量在这里是哪个常量 |

框架自带两件"能自己抓错"的事：

- **两种求解器互为对照**：工作队列（worklist）与朴素轮询（naive）。
  同样的方程跑出不一样的结果，说明框架或格写错了；
- **单调性检查**：每次更新都检查 `老 ⊑ 新`。格不单调是抽象解释最致命的错误，
  框架会直接把它标出来（`单调 ✗`），而不是默默算完。

只用 C++17 标准库。

> 设计与完整说明（格的代数律、方程组、收敛上界、四个实例的公式、两个踩过的坑、
> 实测）见 [`技术报告.md`](技术报告.md)。

## 框架接口

```text
Element                 格元素：集合型（LIVE / RD / AE）或映射型（常量传播）
Analysis                一个分析 = 方向 + 单位元 identity + 边界 boundary
                        + merge + 一条指令的传递函数 transfer
solveWorklist(...)      工作队列求解器（--order=fifo|lifo、--seed=all|boundary）
solveNaive(...)         朴素轮询求解器（对照组）
sameSolution(...)       两种求解器逐块比对
report / dumpSets / mermaid   指标 / 每块 IN、OUT / CFG + 集合的 Mermaid 源码
```
写一个新分析只要实现 `Analysis` 的五个函数，`--analysis=` 之外的一切
（迭代、单调性检查、两种求解器对照、打印）都能直接复用；`test_dataflow.cpp`
里那个"故意写错的格"就是这么加的。

## 输入格式

每行一条指令，`;` / `#` / `//` 起注释；分支 / `halt` 之后自动开新块
（与 `library/transfer/ssa`、`library/transfer/regalloc` 同一套三地址语言）：

```text
label entry
  mov a, 1               ; dst, src（src 可以是常量、变量、[内存]）
  add c, a, b            ; dst, src1, src2
  load x, [slot]         ; 内存符号：可用表达式把它当成表达式 [slot]
  store [out], c         ; 写内存会杀掉 [out] 的可用性
  jz c, L1               ; 条件跳转（jnz 同形）
  jmp L2
  print c                ; 使用 c（活变量分析会看到）
  halt
```

约定：可改名变量是普通名字；`[x]` 是内存符号；常量写成十进制（正负都行）。
到达定值把每条定值编号成 `变量@指令序号`（序号是**全程序**的指令下标，
所以输出里能看到 `a@0`、`d@4`、`d@6` 这样的东西）；可用表达式把表达式写成
`a add b`、`k sub 3`、`[slot]`（`add` / `mul` 会把两个操作数排序，
所以 `b add a` 与 `a add b` 是同一个表达式）。

## 命令行

```text
dataflow [--analysis=live|reach|avail|const|all] [--mode=summary|sets|mermaid|check]
         [--order=fifo|lifo] [--seed=all|boundary] [--no-monotone] [file]
dataflow --help / --version
```

| 模式 | 输出 |
| --- | --- |
| `summary`（缺省） | 每个分析的指标：两种求解器的块访问 / 传递函数次数 / 合并次数、单调、收敛、结果是否一致 |
| `sets` | 每块的 `IN` / `OUT`（就是分析与格的产物） |
| `mermaid` | CFG + 集合的 Mermaid 源码（每块标签里带 `IN` / `OUT`） |
| `check` | 只做"两种求解器一致 + 单调 + 收敛"的判定，不一致就返回 1（CI 用） |

`--analysis=all`（缺省）依次跑四个分析；`--order` 是工作队列的取块顺序，
`--seed` 是只播边界种子还是把所有块都入队（缺省 `fifo` + `boundary`，
见技术报告里的实测对比）。

```text
build/dataflow/dataflow --analysis=live --mode=sets    library/transfer/dataflow/examples/02_loop.df
build/dataflow/dataflow --analysis=avail --mode=mermaid library/transfer/dataflow/examples/03_avail.df
build/dataflow/dataflow --mode=check                   library/transfer/dataflow/examples/01_diamond.df
```

## 构建与自检

```text
make -C library/transfer/dataflow            # 构建 build/dataflow/{libdataflow.a, dataflow, dataflow-tests, dataflow-examples}
make -C library/transfer/dataflow check      # 单元测试 + 5 个示例 + 四段打印
make -C library/transfer/dataflow clean      # 删掉 build/dataflow
```

`dataflow-examples` 对每个 `examples/*.df` 做两类检查：① 四个分析 × 两种求解器
全部自洽、单调、收敛；② 文件里声明的期望逐条比对
（`# 期望 <分析> <块> <in|out>: <集合>`，块名用 label，没有 label 的写 `B<下标>`）。

## 示例与实测

工作队列 / 朴素轮询的**块访问次数**（缺省参数，越小越好）：

| 示例 | 说明 | live | reach | avail | const |
| --- | --- | --- | --- | --- | --- |
| `01_diamond.df` | 两条路径各定义一次 `d` | 4 / 12 | 4 / 8 | 4 / 8 | 4 / 8 |
| `02_loop.df` | 循环（回边 + 入口汇合） | 6 / 9 | 5 / 9 | 4 / 6 | 7 / 12 |
| `03_avail.df` | 只在一条路径上算过的表达式 | 4 / 12 | 4 / 8 | 4 / 8 | 4 / 8 |
| `04_memory.df` | 内存的读与写 | 1 / 1 | 1 / 2 | 1 / 2 | 1 / 2 |
| `05_roots.df` | 两个根块（`halt` 之后的死块 / 函数体） | 2 / 2 | 2 / 4 | 2 / 2 | 2 / 4 |

几个值得对着输出看的结果：

- **同一个汇合点，三个分析的结论完全不同**（`01_diamond.df` 的 `L2`）：
  到达定值 `{a@0, b@1, c@2, d@4, d@6}`——`d` 的两个定值都到达；
  可用表达式 `{a add b}`——两条路径都算过；
  常量传播 `{a=1, b=2, c=3, d=⊥}`——`d` 一边是 1 一边是 2，合并成"非常量"。
- **must 分析会被汇合点"卡住"**（`03_avail.df`）：`a add b` 只在左分支算过，
  所以 `L2` 的 IN 里没有它（交集丢掉了）；把它挪到分支之前就会变成可用。
- **循环头是三种分析最热闹的地方**（`02_loop.df`）：活跃变量 `{i, s}`、
  到达定值 `{i@0, i@3, k@5, s@1, s@2}`（入口与回边都到达）、
  可用表达式 `{}`（交集把循环头清空了）。
- **内存只影响两个分析**（`04_memory.df`）：可用表达式把 `[slot]` 当表达式，
  `store` 之后它不再可用；到达定值与活跃变量只看变量，所以 `load` 出来的
  `sum` 只有一个定值。
- **前向分析的边界种子是"所有没有前驱的块"**（`05_roots.df`）：`halt` 之后的
  死块、`call` 的目标函数体都可能是根块；只种 0 号块的话工作队列永远不碰它们，
  `reach` / `const` 就会和朴素轮询对不上（这个示例正是照着那次不一致写的）。

## 目录

```text
library/transfer/dataflow/
├── dataflow.hpp / dataflow.cpp   格元素、Analysis 接口、四个实例、两种求解器、打印
├── main.cpp                      命令行（4 种模式，读文件或标准输入）
├── test_dataflow.cpp             单元测试（格律、四个实例的结果、求解器对照、单调性反例）
├── example_suite.cpp             示例运行器（通用自洽检查 + 逐条期望比对）
├── Makefile / CMakeLists.txt
└── examples/                     5 个示例程序 + 说明
```
