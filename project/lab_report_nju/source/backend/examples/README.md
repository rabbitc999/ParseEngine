# 端到端流水线示例

这里的 `.quad` 只用**两边都有的指令**（`:=`、`+ - * /`、`label`、`j`、`jz`、`write`、
`halt`），所以桥能把它们全部翻译成三地址文本，第 ⑦ 步的跨模块对照才有意义。

| 文件 | 说明 | 期望输出 |
| --- | --- | --- |
| `01_arith.quad` | 直线算术（`:=` / `+` / `-` / `write`） | `7 6` |
| `02_branch.quad` | 分支汇合（`jz` / `j` / `label`） | `10` |
| `03_loop.quad` | 循环累加 1..5（回边 φ、并行复制、寄存器压力） | `15 5` |

```text
make -C library/transfer check
build/transfer-one-thread/t1t pipeline library/transfer/examples/03_loop.quad
build/transfer-one-thread/t1t-pipeline-suite library/transfer/examples
```

`pipeline_suite.cpp` 对每个示例要求五件事：流水线跑通、桥的跳过条数为 0、
降级后的程序能解释执行、**transfer 模拟输出与三地址解释输出一致**、与文件里
`# 期望:` 一致。

元信息格式（`#` 注释，程序本身照样能吃）：

```text
# 名称: 循环累加 1..5
# 期望: 15 5
# 说明: 回边上的 i 与 s 都要 φ；transfer 的模拟与三地址解释必须给出同一串输出
```
