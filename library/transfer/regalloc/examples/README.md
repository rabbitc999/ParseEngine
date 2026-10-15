# 示例

| 文件 | 用途 |
| --- | --- |
| `pressure.ra` | 三个变量同时活跃：`--regs=4` 不溢出，`--regs=2` 必然溢出并插入 load/store |
| `loop.ra` | 循环里的活跃区间长，用来看溢出代价与重写后的访存数变化 |
| `copy.ra` | `mov` 链：源在复制后就不再使用，保守合并让它们同色，冗余复制被删掉 |
| `redef.ra` | 源在复制之后被重新定义：两个变量从此都活着，**不能**合并，复制必须保留 |
| `call.ra` | 块里有 `call`：调用者保存的 `C`、`D` 不能装跨调用的值，4 个寄存器实际只能用 2 个 |

## 跑一遍

```text
make -C library/transfer/regalloc                 # 构建 build/regalloc/regalloc
build/regalloc/regalloc --regs=4 library/transfer/regalloc/examples/pressure.ra
build/regalloc/regalloc --regs=2 library/transfer/regalloc/examples/pressure.ra
build/regalloc/regalloc --algorithm=linear --regs=3 library/transfer/regalloc/examples/loop.ra
build/regalloc/regalloc --regs=2 library/transfer/regalloc/examples/copy.ra
build/regalloc/regalloc --regs=3 library/transfer/regalloc/examples/redef.ra
build/regalloc/regalloc --regs=4 library/transfer/regalloc/examples/call.ra
```

输出分三段：分配结果（虚拟 → 物理 / 溢出）、重写后的程序、指标
（峰值活跃、干涉边数、用到的颜色、溢出槽、插入的 load/store、合并与删除的
`mov` 条数、指令数与访存次数变化）。

## 输入格式

每行一条指令，`;` / `#` / `//` 起注释：

```text
label L1                 ; 基本块入口（也作为跳转目标）
  mov t1, 10             ; dst, src
  add t2, t1, 3          ; dst, src1, src2（sub / mul / div 同形）
  load t3, [x]           ; 从内存符号 x 读入 t3
  store [x], t1          ; 把 t1 写进内存符号 x
  jz t2, L2              ; 条件跳转（使用 t2）
  jmp L1                 ; 无条件跳转
  call f                 ; 调用：调用者保存的物理寄存器不能跨它分配
  ret / halt
```

物理寄存器缺省是 `A B C D`（A、B 被调用者保存，C、D 调用者保存），用
`--regs=N` 改成 N 个。
