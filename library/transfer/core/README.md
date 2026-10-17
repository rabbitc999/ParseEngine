# 四元式优化与目标代码转换器（C++17）

> **定位：编译器工具链的参考实现**（教学 / 对照 / 做实验用）。这里的"目标代码"
> 主要产物是教学目标机汇编 + 自带模拟器；另有可交给 Clang 的 LLVM IR 后端。不追求
> 完整 ABI、指令调度、目标文件格式与优化正确性的形式化证明。整库定位见 [../README.md](../README.md)。

> 该模块已在合并库 [library/transfer](../README.md) 内，与 irbridge / core / dataflow / ssa / regalloc 共用**一套带位数信息的三地址 IR**（`ir/typed_ir.hpp`，指令类型、基本块类型、解析与打印各只有一份）。本文里的命令按**单模块**写；整条流水线用 `make -C library/transfer check` 或 `build/transfer-one-thread/t1t pipeline <file>`。

> 设计与实现的完整说明（含 Mermaid 图与 LaTeX 公式）见
> [`技术报告.md`](技术报告.md)。

本目录是一个不依赖 JSON 或第三方库的四元式后端，带**变量类型**和**机器字长**
两个模型：

```text
四元式 → 基本块 / CFG → 数据流分析 → 局部优化 → 全局优化
       → 循环不变外提 / 强度削弱 / 归纳变量 → 重建 CFG 迭代
       → 优化后四元式 → 基础目标代码 → MIPS / 8086 / ARM / LLVM → 执行
```

类型模型：`i8/u8/i16/u16/i32/u32/i64/u64/i128/u128/f32/f64`，未声明时按 i16
（旧程序语义不变）。机器模型：`word_bits ∈ {8,16,32,64}` 是机器**原生字长**，
后端按「类型宽度 W 和字长 M」决定展开方式：

| 情况 | 后端做法 |
| --- | --- |
| `W == M` | 一条原生指令（i16 省略类型后缀，保持旧产物不变） |
| `W < M` | 一条指令 + 按类型归一化（模拟器用掩码 / 算术移位合成） |
| `W > M` | 多字展开：变量按 M 位分片（`x#0` 是低字），加减用 ADD/ADDC、SUB/SUBB 进位链，按位逐字，乘除等交给运行时例程 |

## 四元式

`(op, arg1, arg2, result)` 或 `(op, arg1, arg2, result, extra)`，无意义字段写 `_`。
运算符可以写类型后缀，例如 `+:u32`、`*:f64`、`<<:i8`；不写后缀时按
「结果变量类型 → 第一个操作数类型 → i16」推断。

| 类别 | 写法 | 说明 |
| --- | --- | --- |
| 声明 | `(decl, i32, global, x)` | 声明变量类型；作用域 `global` / `local` / `_` |
| 算术 | `+ - * / %` | 整数按位宽取模回绕；浮点按 IEEE-754；除零报错 |
| 进位 | `+c`、`-b` | 进位加 / 借位减，读写进位标志，用于多字加减链 |
| 双寄存器乘除 | `(mul, a, b, lo, hi)`、`(div, a, b, q, r)` | 乘法给高低半；除法给商和余数 |
| 一元 | `neg`、`odd`、`~` | 取负 / 奇偶 / 按位取反 |
| 转换 | `cvt:f64`、`cvt:i32` | 整数↔浮点、位宽与符号转换 |
| 关系 | `== != < <= > >=` | 结果 0/1（默认 i8）；有符号 / 无符号按类型 |
| 移位 / 按位 | `<< >> & \| ^` | 右移：有符号算术右移、无符号逻辑右移；位数按类型宽度取模 |
| 控制 | `label` `j` `jz` `jnz` `halt` | 标签、跳转、停机 |
| 调用 | `param` `call` `ret` | `param v, 形参`；`call f, 结果变量`；`ret v` |
| 中断 | `int` `iret` | 进入中断处理程序，`iret` 弹出中断帧 |
| 输入输出 | `read` `write` `print` `putc` `getc` | 数值输入输出、字符串、字符 |
| 数据区 | `addr` `load` `store` | 取数据区符号基址 / 按地址读 8 字节 / 按地址写 8 字节（见下） |

乘法和除法的结果是**固定的命名寄存器对**：IR 里写 `HI` / `LO`，生成时映射到
目标机（MIPS `HI:LO`、8086 `DX:AX`、ARM `regHi:regLo`）。`HI` 放高半 / 余数，
`LO` 放低半 / 商。

`read` / `write` 的类型取变量的声明类型（也可写 `write:f64`）；`print` 输出
字符串（支持 `\n`、`\t`），`putc` 输出一个字符，`getc` 取输入项首个字符编码。

## 数据区三件套：`addr` / `load` / `store`

`simple_ast` 把数组 / 结构体降级成"基址 + 偏移"，真实的值放在 `.data.bin` 里。
这三条指令让**寄存器后端**也能读写那片内存，语义与 `library/stackvm` 的
`ADDR` / `LOADAT` / `STOREAT` 完全一致：

| 四元式 | 生成的伪指令 | 语义 |
| --- | --- | --- |
| `(addr, 符号, _, 目标)` | `ADDR 目标, 符号` | 把数据区里该符号的基址写进目标变量 |
| `(load, 指针, _, 目标)` | `LOADAT 目标, 指针` | 按地址读 8 字节（小端） |
| `(store, 指针, 值, _)` | `STOREAT.类型 指针, 值` | 按地址写 8 字节（小端） |

- 地址就是数据区里的**字节偏移**；读写都是 8 字节小端，`addr` / `load` 的结果
  按 `i64` 参与后续分片（窄机器上自动拆成多个机器字，指针因此能整值还原）。
- 三条都是**运行时伪指令**（和 `VLOAD` / `FOPEN` 一样）：地址可能是多字变量，
  由模拟器拼出完整地址再去读写数据区，所以 MIPS / 8086 / ARM 生成同一条指令；
  机器能力由指令族 `data` 表示，配置里 `feature.data=off` 就报"机器没有该指令族"。
- 数据区随 `--data=<bin> --layout=<file>` 一起给出（`--layout` 留空时按
  `<bin>.layout` 找），生成器把它写进汇编的 `.dataarea 字节数` 段：

```text
.dataarea 124
    .sym arr_1 0                 ; 符号基址，就是 simple_ast 的 .layout 里的 offset=
    .hex 0 0a0000000000000014…   ; 字节镜像，16 字节一行
.enddataarea
```

没有挂数据区时三条指令分别报 `没有加载数据区，无法取符号地址 / 按地址取值 / 按地址写值`；
符号不在布局表里报 `数据区里没有符号：x`；越界报 `数据区越界读/写：地址 N`。
`loadDataBin` / `loadDataLayout` / `loadDataArea` 三个接口负责把这两份文件装进
`MachineConfig.dataAreaBytes` / `dataAreaSymbols`。

```text
t1t transfer demo.quad --target=mips --data=demo.data.bin --layout=demo.layout
```

## 接口

`transfer.hpp` 暴露的接口：

- `read` / `write`：读写四元式（`write` 保留类型后缀与双结果字段）；
- `validate`：检查类型、操作数、标签与跳转目标；
- `variableTypes` / `quadType`：查询声明表与一条四元式的运算类型；
- `analyze` / `report`：基本块与 CFG（`call`/`int` 连到被调用函数，`ret`/`iret`
  是出口），到达定值 / 活跃变量 / 可用表达式；
- `optimizeDetailed` / `optimizeParallel`：常量折叠、常量传播、代数化简、
  局部 / 全局 CSE、复制传播、DCE 与安全的循环优化；类型与浮点都参与折叠；
- `generate`：生成教学用 MIPS / 8086 / ARM 汇编（可由 `MachineConfig` 配置）或 LLVM IR；
- `generateLlvm`：生成无需 LLVM SDK 即可写出的文本 `.ll`；当前支持不超过 64 位的整数、
  控制流、调用（含递归）与整数输出，使用 Clang 编译执行；
- `simulate`：执行生成的汇编，返回带类型的 `Execution`（`memory` / `output` / `text`）；
- `loadDataBin` / `loadDataLayout` / `loadDataArea`：装载数据区字节镜像与符号表
  （`addr` / `load` / `store` 三件套用）。

`types.hpp` 另外提供类型表与可移植 128 位整数（`U128`）、类型化数值
（`Value`、`evalTyped`、`addWithCarry`、`subWithBorrow`、`multiplyWide`、
`divideWide`），可以直接在别处复用。

所有状态都在调用栈或返回对象里，多个线程可以并行处理不同的 `Program`。

## 示例

示例程序都从 `example/` 目录**读四元式文件**，元信息写在文件注释里：

- `example/minimal.quad`：最小流程（`example.cpp`）；
- `example/config.quad`：外部配置、CFG 报告、ARM 生成（`example_config.cpp`）；
- `example/operators.quad`：移位 / 按位，三种目标机结果一致自检（`example_operators.cpp`）；
- `core/example/*.quad` 共 26 个程序（`example_suite.cpp` 自动比对 `# 期望:` /
  `# 期望文本:`，失败返回非 0），覆盖类型、无符号、浮点、宽类型、进位、
  双寄存器乘除、I/O、调用、中断、数据区、数据区三件套、跨 `call` 的循环变量。

示例配置共 7 份：`example/mips16.ini`、`example/mips32.ini`、`example/arm32.ini`、
`example/x86_16.ini`、`example/mini_machine.ini`、`example/no_muldiv.ini`、
`example/no_data.ini`；示例数据：`example/sample.data.bin`、`example/21_data_area.data.bin`
（+ `.layout`）与 `data.*` / `rodata.*` 命名数据。
索引见 `example/README.md`。

```text
make -C library/transfer clean all check
```

产物在 `build/transfer-one-thread`：`libtransfer-one-thread.a`、`t1t-example`、
`t1t-example-config`、`t1t-example-operators`、`t1t-example-suite`。
CTest 能找到 Clang 时还会运行 `llvm-clang-e2e`，把算术和递归样例生成 `.ll`、编译成
本机程序并核对输出。
GCC 下构建脚本会加 `-finput-charset=UTF-8 -fexec-charset=UTF-8`，保证中文注释和
中文控制台输出不因系统代码页变成乱码（MSVC 用 `/utf-8`）。
调试用 `TRANSFER_TRACE=1` 可让模拟器把每条执行的指令打印到 stderr。

## 机器配置

```ini
target=arm
word_bits=32                 # 机器原生字长：8/16/32/64
reg_a=r4
reg_b=r5
reg_c=r6
reg_hi=r12                   # 乘除法固定结果对（HI）
reg_lo=r14                   # LO
stack_words=4096
data_bin=example/sample.data.bin
data.CONST_A=1000            # 命名数据，类型由 decl 决定
rodata.PI=3.14159            # 只读变量
memory_load=LDRSH {dst}, [{name}]     # i16（旧默认）取内存模板
memory_store=STRH {src}, [{name}]
immediate_load=MOV {dst}, #{value}
load.i32=LW {dst}, {name}             # 也可以按类型覆盖取内存模板
store.i32=SW {src}, {name}
cmd.add=ADD {dst}, {lhs}, {rhs}       # 指令模板：花括号是占位符
cmd.mulu=... / cmd.divu=... / cmd.shru=...
cmd.fadd=... / cmd.fsub=... / cmd.fmul=... / cmd.fdiv=... / cmd.fneg=...
```

生成的目标代码有 `.data`（含按类型标注的声明与分片）、`.rodata`、`.stack`、
`.code` 四个区域；头部另有 `.bits`（机器字长）、`.mulpair`（固定结果对）与
`.reg`（寄存器名，模拟器据此区分寄存器与变量）。取内存 / 存内存**分位数**：
窄类型一条 `LD.<type>` / `ST.<type>`，宽类型按机器字拆成多次分片访问。

`.data` 里的 `data_bin` 字节镜像用紧凑写法：程序**声明过**的字节逐字节给初值
（`byte_0 = 16 : i32`，类型跟着声明走），其余字节打包成
`.hex 起始偏移 十六进制串`（16 字节一行；手写汇编也可以用 `.bytes 起始偏移 1,2,3`，
每个字节照旧是一个 `byte_<偏移>` 符号）。这样 64 KB 资源只多几千行，而不是逐字节六万行。
