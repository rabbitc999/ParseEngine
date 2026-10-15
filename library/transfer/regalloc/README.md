# 寄存器分配（`library/transfer/regalloc`）

> 该模块已在合并库 [library/transfer](../README.md) 内，与 irbridge / core / dataflow / ssa / regalloc 共用**一套带位数信息的三地址 IR**（`ir/typed_ir.hpp`，指令类型、基本块类型、解析与打印各只有一份）。本文里的命令按**单模块**写；整条流水线用 `make -C library/transfer check` 或 `build/transfer-one-thread/t1t pipeline <file>`。

后端里"虚拟寄存器 → 物理寄存器"这一步。输入是一段**带标签的虚拟寄存器程序**
（每行一条指令），输出三样东西：

1. **分配表**：每个虚拟寄存器分到哪个物理寄存器，谁没分到要溢出到栈；
2. **重写后的程序**：虚拟寄存器换成物理寄存器，被溢出的值在定义后写回、
   使用前读回；两端同色的 `mov` 直接删掉；
3. **指标**：峰值活跃、干涉图边数、用到的颜色、溢出槽、插入的 `load/store`、
   合并与删除的 `mov` 条数、指令数与访存次数变化。

两种算法并排实现，方便对照：

| 算法 | `--algorithm=` | 做法 |
| --- | --- | --- |
| 图着色 | `chaitin`（缺省） | Chaitin-Briggs：干涉图 → 保守合并 → 简化 / 选择 / 溢出 |
| 线性扫描 | `linear` | Poletto-Sarkar：活跃区间按起点排序 + 活跃表，过期就退休 |

只用 C++17 标准库，没有第三方依赖。

> 设计与完整说明（干涉图公式、合并条件、溢出重写的推导、实测）见
> [`技术报告.md`](技术报告.md)。

## 输入格式

每行一条指令，`;` / `#` / `//` 起注释；`label L1` 开一个新的基本块：

```text
label L1                 ; 基本块入口，也是跳转目标
  mov t1, 10             ; dst, src（源可以是常量、虚拟寄存器、物理寄存器）
  add t2, t1, 3          ; dst, src1, src2（sub / mul / div 同形）
  load t3, [x]           ; 从内存符号 x 读进 t3（内存符号不参与分配）
  store [x], t1          ; 把 t1 写进内存符号 x
  jz t2, L2              ; 条件跳转：使用 t2（jnz 同形）
  jmp L1                 ; 无条件跳转
  call f                 ; 调用：调用者保存的寄存器不能装跨调用的值
  ret / halt
```

物理寄存器缺省是 `A B C D`（`A`、`B` 被调用者保存，`C`、`D` 调用者保存），
`--regs=N` 改成 N 个（教学机最多 `A`…`H` 八个）。`SCRATCH` / `SCRATCH2` 是
溢出重写保留的名字，不参与分配；溢出槽写出来是 `[spill.<虚拟寄存器>]`。

## 命令行

```text
regalloc [--regs=N] [--algorithm=chaitin|linear] [--no-coalesce] [file]
regalloc --help / --version
```

不给文件就读标准输入，输出写标准输出（控制台按 UTF-8 输出中文）。

```text
build/regalloc/regalloc --regs=2 library/transfer/regalloc/examples/pressure.ra
```

## 构建与自检

```text
make -C library/transfer/regalloc            # 构建 build/regalloc/{libregalloc.a, regalloc, regalloc-tests}
make -C library/transfer/regalloc check      # 单元测试 + 6 条示例命令行
make -C library/transfer/regalloc clean      # 删掉 build/regalloc
```

`check` 跑的六条示例见下表（数字就是实际输出，可直接对照）。

## 示例与实测

`examples/` 下 5 个程序，`make check` 覆盖其中 6 种组合：

| 示例 | 命令行 | 峰值活跃 | 干涉边 | 颜色 | 溢出 | 合并 / 删除 mov | 指令数 | 访存 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `pressure.ra` | `--regs=4` | 3 | 4 | 3 / 4 | 0 | 0 / 0 | 6 → 6 | 1 → 1 |
| `pressure.ra` | `--regs=2` | 3 | 4 | 2 / 2 | 1 | 0 / 0 | 6 → 8 | 1 → 3 |
| `loop.ra` | `linear --regs=3` | 2 | 1 | 2 / 3 | 0 | 0 / 0 | 7 → 7 | 1 → 1 |
| `copy.ra` | `--regs=2` | 1 | 0 | 1 / 2 | 0 | 2 / 2 | 5 → 3 | 1 → 1 |
| `redef.ra` | `--regs=3` | 2 | 1 | 2 / 3 | 0 | 0 / 0 | 5 → 5 | 1 → 1 |
| `call.ra` | `--regs=4` | 3 | 4 | 2 / 4 | 1 | 0 / 0 | 7 → 9 | 1 → 3 |

几处值得对着输出看的现象：

- **寄存器不够就溢出**：`pressure.ra` 三个变量同时活跃，`--regs=2` 时溢出 1 个，
  重写后多 2 条指令、访存 1 → 3，指标行还会提示"所有物理寄存器都用上了"。
- **复制能消失**：`copy.ra` 的 `mov b, a`、`mov c, b` 在重写后不再存在
  （5 → 3 条指令）。加 `--no-coalesce` 后"合并掉的 mov"变成 0，但复制仍然被删掉——
  干涉图对复制是**例外**的（定义与源不加边），着色本来就允许它们同色，
  合并只是把这件事提前到着色之前；真正让复制从输出里消失的是重写阶段的
  冗余复制删除。
- **不能合并的复制**：`redef.ra` 里源在复制之后被重新定义，两个变量从此都活着，
  于是必须占两个寄存器，`mov A, B` 原样保留。
- **调用点**：`call.ra` 有 4 个物理寄存器，但块里有 `call`，调用者保存的 `C`、`D`
  不能装同时在用的值，实际只用上 2 个，于是溢出 1 个（用 `--algorithm=linear`
  跑同一条也能看到同样的 1 个溢出）。

## 目录

```text
library/transfer/regalloc/
├── regalloc.hpp / regalloc.cpp   机器描述、解析、活跃变量、干涉图、两种分配算法、溢出重写、指标
├── main.cpp                      命令行（读文件或标准输入）
├── test_regalloc.cpp             单元测试（解析 / 活跃 / 干涉 / 着色 / 合并 / 线性扫描 / 重写 / 错误）
├── Makefile / CMakeLists.txt     构建与自检（Windows 上的 clean / mkdir 走 cmd 原生命令）
└── examples/                     5 个示例程序 + 说明
```

