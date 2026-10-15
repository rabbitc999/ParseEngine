# 示例

| 文件 | 用途 |
| --- | --- |
| `01_assign.ssa` | 直线代码、没有汇合点：SCCP 把 `add` 折成常量，再把常量传进后面的 `mov` |
| `02_diamond.ssa` | 菱形汇合：`y` 在两条路径上各定义一次，汇合点插 φ；条件来自 `load`，SCCP 折不动 |
| `03_const_branch.ssa` | 条件在编译期已知：`dead` 块被 SCCP 删掉，φ 只剩一个实参后被 GVN 归并 |
| `04_loop_swap.ssa` | 循环里的交换：回边要 φ，临界边被切开，交换的并行复制成环（借临时变量） |
| `05_redundant.ssa` | 重复表达式与冗余 φ：复制传播让 φ 的实参全同，φ 消失，重复表达式归并 |

## 跑一遍

```text
make -C library/transfer/ssa                 # 构建 build/ssa/ssa
build/ssa/ssa --mode=summary library/transfer/ssa/examples/04_loop_swap.ssa
build/ssa/ssa --mode=dom     library/transfer/ssa/examples/03_const_branch.ssa   # Mermaid 源码
build/ssa/ssa --mode=ssa     library/transfer/ssa/examples/02_diamond.ssa
build/ssa/ssa --mode=sccp    library/transfer/ssa/examples/03_const_branch.ssa
build/ssa/ssa --mode=gvn     library/transfer/ssa/examples/05_redundant.ssa
build/ssa/ssa --mode=destroy library/transfer/ssa/examples/04_loop_swap.ssa
build/ssa/ssa --mode=run     library/transfer/ssa/examples/04_loop_swap.ssa

make -C library/transfer/ssa check    # 单元测试 + 上面 5 个示例的四类期望比对
```

## 文件里的元信息

`.ssa` 就是普通的三地址程序，期望写在 `#` 注释里（`example_suite.cpp` 解析这些行，
`ssa` 本身把它们当注释忽略）：

```text
# 名称: 循环里的交换
# 期望: 2 1 3          ← 解释执行原程序的 print 输出
# 期望 φ: 6            ← SSA 构造插入的 φ 条数
# 期望 折叠: 0          ← SCCP 折成常量的算术指令条数
# 期望 不可达: 0        ← SCCP 删掉的不可达块数
# 期望 GVN: 0           ← GVN 消掉的重复表达式条数
# 说明: 回边上 a、b、n 都要 φ；交换让回边的并行复制成环
```

`ssa-examples` 还会做一次**差分测试**：φ 消除之后的程序再解释执行一遍，输出必须与
原程序逐字相同（五个示例全部通过）。

## 输入格式

每行一条指令，`;` / `#` / `//` 起注释；分支 / `halt` 是终结指令，后面自动开新块：

```text
label L1                 ; 基本块入口
  mov t1, 10             ; dst, src（src 可以是常量、变量、[内存]）
  add t2, t1, 3          ; dst, src1, src2（sub / mul / div 同形）
  load t3, [x]           ; 从内存符号读入（内存不参与 SSA）
  store [x], t1          ; 写内存
  jz t2, L2              ; 条件跳转（jnz 同形）
  jmp L1                 ; 无条件跳转
  print t2               ; 输出
  halt
```

SSA 相关的名字由库生成，不用手写：`x_1`、`x_2` 是改写后的版本，`x_0` 是入口初值，
`undef` 表示"这条边上没有定义"，`tmp_N` 是打破并行复制环用的临时变量。
