# 示例

| 文件 | 用途 |
| --- | --- |
| `01_diamond.df` | 两条路径各定义一次 `d`：汇合点上到达定值有两个 `d`、常量传播是 ⊥、`a + b` 两边都可用 |
| `02_loop.df` | 循环：回边与入口在循环头汇合（活跃变量、到达定值、可用表达式三种结论完全不同） |
| `03_avail.df` | `a + b` 只在一条路径上算过 → 汇合点不可用（must 分析的典型现象） |
| `04_memory.df` | 内存读写：`store` 让 `[slot]` 不再可用；到达定值 / 活跃变量只看变量 |
| `05_roots.df` | 两个根块（主流程 + `halt` 之后的死块）：前向分析的种子必须覆盖所有根块 |

## 跑一遍

```text
make -C library/transfer/dataflow                 # 构建 build/dataflow/dataflow
build/dataflow/dataflow --mode=summary  library/transfer/dataflow/examples/01_diamond.df
build/dataflow/dataflow --analysis=live   --mode=sets    library/transfer/dataflow/examples/02_loop.df
build/dataflow/dataflow --analysis=avail  --mode=mermaid library/transfer/dataflow/examples/03_avail.df
build/dataflow/dataflow --mode=check      library/transfer/dataflow/examples/04_memory.df

# 求解器参数对照（种子策略 / 取块顺序）
build/dataflow/dataflow --analysis=reach --mode=summary --seed=all --order=lifo \
    library/transfer/dataflow/examples/02_loop.df

make -C library/transfer/dataflow check           # 单元测试 + 5 个示例 + 四段打印
```

## 文件里的元信息

`.df` 就是普通的三地址程序，期望写在 `#` 注释里（`example_suite.cpp` 解析这些行，
`dataflow` 本身把它们当注释忽略）：

```text
# 名称: 两条路径同时定义 d
# 说明: …
# 期望 live  entry in: {}
# 期望 live  entry out: {a, b}
# 期望 live  L2 in: {d}
# 期望 reach L2 in: {a@0, b@1, c@2, d@4, d@6}
# 期望 avail L2 in: {a add b}
# 期望 const L2 in: {a=1, b=2, c=3, d=⊥}
```

键的格式是 `期望 <分析> <块> <in|out>`；块名用 `label`，没有 label 的块写
`B<下标>`（比如 `B1`）。值和 `--mode=sets` 打印出来的集合逐字一致。

除了这些期望，运行器还会对**每个示例 × 每个分析**做一遍通用检查：
工作队列与朴素轮询必须给出相同的 IN / OUT、必须收敛、每次更新都必须沿格往上。

## 输入格式

每行一条指令，`;` / `#` / `//` 起注释；分支 / `halt` 之后自动开新块：

```text
label L1
  mov t1, 10             ; dst, src（src 可以是常量、变量、[内存]）
  add t2, t1, 3          ; dst, src1, src2（sub / mul / div 同形）
  load t3, [x]           ; 内存符号（可用表达式把它当表达式 [x]）
  store [x], t1
  jz t2, L2              ; 条件跳转（jnz 同形）
  jmp L1
  print t2               ; 使用 t2
  halt
```

分析输出里的符号：`a@0` 是"到达定值"里第 0 条指令对 `a` 的定值；`a add b` 是
可用表达式里的表达式；常量传播用 `变量=值` 表示映射，`⊥` 表示非常量，
键不出现表示还没算出来（⊤）。
