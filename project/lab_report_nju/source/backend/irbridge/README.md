# 前端胶水：三元式 ↔ 四元式（`library/transfer/irbridge`）

> 该模块已在合并库 [library/transfer](../README.md) 内，与 irbridge / core / dataflow / ssa / regalloc 共用**一套带位数信息的三地址 IR**（`ir/typed_ir.hpp`，指令类型、基本块类型、解析与打印各只有一份）。本文里的命令按**单模块**写；整条流水线用 `make -C library/transfer check` 或 `build/transfer-one-thread/t1t pipeline <file>`。

`project/pl0-ir/` 有两条前端路线：`pl0.gram` 出**三元式**（`i: (op, a, b)`，结果
用下标 `(k)` 引用），`pl0_quads.gram` 出**四元式**（`i: (op, a, b, result)`，结果
显式写名字）。这个库把两者互转，并把四元式导出成
[`library/transfer`](../README.md) 能直接读的形式——打通
"`.gram` → 前端 → 中间代码 → 优化 / 目标代码"。

只用 C++17 标准库；文本格式与仓库现有产物逐字对齐（同一份 fixture 往返后
**完全相同**）。

> 设计与完整说明（对照表、转换规则、公式、实测）见 [`技术报告.md`](技术报告.md)。

## 两种中间表示

| | 三元式 | 四元式 |
| --- | --- | --- |
| 形状 | `i: (op, arg1, arg2)` | `i: (op, arg1, arg2, result)` |
| 结果引用 | 下标 `(k)` | 名字 `t1`、`t2`…（或变量名） |
| 引用稳定性 | 指令一移动，`(k)` 的含义就变 | 名字固定，搬动指令不受影响 |
| 适合 | 紧凑展示、教学对照 | 常量传播、CSE、调度、寄存器分配、目标代码 |

## 转换规则

**三元式 → 四元式**：把每个 `(k)` 换成第 k 条结果的显式名字。

- 被引用过、且本身产生值的条目分到 `t<下标>`（`3: (<=, i, 10)` → `result = t3`）；
- 没被引用的结果写 `_`；
- `label` / `j` / `write` 不产生值，被 `(k)` 引用时报错；
- `(k)` 越界报错。

**四元式 → 三元式**：把"值名"折回下标引用，变量仍然按名字写。

- 表达式的结果（临时量，或被表达式定义的名字）→ 后续引用写成 `(k)`；
- 赋值 `(:=, v, _, x)` → `i: (:=, v, x)`（目标写在第三个字段）；
- 非 `:=` 的四元式如果结果落在**变量**上（`(+, x, 1, y)`），会拆成
  "表达式 + 赋值"两条三元式：`i: (+, x, 1)`、`i+1: (:=, (i), y)`，保证语义不变；
- `label` / `j` / `jz` / `write` / `read` / `halt` 按仓库的惯例摆放字段。

## 导出给 `library/transfer`

```text
irbridge to-transfer program.quads -o program.transfer.quads
```

输出可以直接被 `library/transfer` 的 `read()` 消费：非临时变量补上前面的
`(decl, i16, global, name)` 声明（PL/0 只有整数，所以是 i16；临时量不声明，
按 transfer 的默认类型处理），后面是标准四元式。之后就能用
`optimizeDetailed` / `generate` / `simulate` 走完优化与目标代码生成。

## 命令行

```text
irbridge triples2quads [in.triples] [-o out.quads]
irbridge quads2triples [in.quads]   [-o out.triples]
irbridge to-transfer   [in]         [-o out.quads] [--no-decl]
irbridge stats         [in]
irbridge --check FILE...            只解析并报错（CI 用）
```

不给输入文件读标准输入，不给 `-o` 写标准输出。`stats` 输出总条数、算术 / 比较 /
跳转 / 标签 / 赋值 / 输入输出 / 临时量分类计数。

## 构建与自检

```text
make -C library/transfer/irbridge            # 构建 build/irbridge/{libirbridge.a, irbridge, irbridge-tests}
make -C library/transfer/irbridge check      # 单元测试 + 用 examples/ 里的真实 fixture 往返比对
```

自检做的事：

1. `irbridge-tests`：解析、`(k)` ↔ 名字互转、错误分支（越界引用、引用不产生值的
   条目、字段数不对）、统计、导出；
2. 逐字比对：`examples/if_while.triples`（就是
   `project/pl0-ir/expected/if_while.triples`）→ 四元式 → 三元式必须回到原文件，
   生成的 `examples/if_while.quads` 也必须与仓库里那份逐字一致。

```text
irbridge-tests：全部通过
三元式 ↔ 四元式往返：与 fixture 逐字相同
总条数：15 / 跳转：3 / 赋值：4 / 临时量：7
```

## 目录

```text
library/transfer/irbridge/
├── irbridge.hpp / irbridge.cpp   模型、读写、互转、导出、统计
├── main.cpp                      命令行
├── test_irbridge.cpp             单元测试（含真实 fixture 往返）
├── Makefile / CMakeLists.txt     构建与自检
└── examples/                     真实 fixture（if_while.triples / .quads）
```
