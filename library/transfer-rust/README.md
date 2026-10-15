# 四元式优化与目标代码转换器（Rust 版）

> **这是一个独立的 Rust crate：`quad_transfer`。** 一套带**变量类型**与**机器字长**
> 两个模型的四元式后端——从四元式出发，做数据流分析、局部 + 全局优化，生成
> **MIPS / 8086 / ARM** 三种教学目标机汇编，再用自带的模拟器执行。
>
> 目标代码是**教学目标机汇编 + 模拟执行**，不是真实机器码：不追求真实 ABI、指令
> 调度、目标文件格式与优化正确性的形式化证明。**只用 Rust 标准库，没有任何第三方
> 依赖**（`Cargo.lock` 里只有本 crate 自己）。所有状态都属于调用对象或返回值，
> `Program` / `Execution` 可以安全地交给不同线程处理。
>
> 内部设计与实现细节见 [技术报告.md](技术报告.md)。

```text
四元式 → 基本块 / CFG → 到达定值 / 活跃变量 / 可用表达式
       → 常量折叠 / 常量传播 / 代数化简 / CSE / 复制传播 / DCE / 循环优化
       → 优化后的四元式 → MIPS / 8086 / ARM 汇编 → 模拟执行（返回值 / 输出 / 退出码）
```

## 快速开始

三个可执行目标（`default-run` 是第一个；crate 无依赖，首次编译约 15 秒）：

```sh
# 1. 最小示例：读 core/example/minimal.quad → 优化 → ARM 汇编 → 模拟执行
cargo run --manifest-path library/transfer-rust/Cargo.toml

# 2. 示例集运行器：跑 core/example/*.quad，逐个比对文件里的 # 期望
cargo run --manifest-path library/transfer-rust/Cargo.toml --bin quad-transfer-suite

# 3. 移位 / 按位运算单文件自检
cargo run --manifest-path library/transfer-rust/Cargo.toml --bin quad-transfer-operators
```

两个运行器都能带一个可选参数覆盖示例来源（目录或单个 `.quad`）：

```sh
cargo run --manifest-path library/transfer-rust/Cargo.toml --bin quad-transfer-suite -- \
    library/transfer/core/example/06_muldiv.quad
```

编译产物：在 crate 目录里跑（`cd library/transfer-rust && cargo run`）时按
`.cargo/config.toml` 落到仓库的 `build/transfer/rust/`；从仓库根用
`--manifest-path` 跑时 cargo 读不到这份配置，落在 `library/transfer-rust/target/`。
两者都在 `.gitignore` 范围内。

## 自检结果（实测）

`quad-transfer-suite` 跑 `core/example/` 的 **26 份 `.quad`**：

```text
共 26 个示例：23 通过，0 失败。
```

| 结果 | 份数 | 说明 |
| --- | --- | --- |
| 通过 | 23 | 类型 / 无符号 / 浮点 / 宽类型 / 进位 / 双寄存器乘除 / I·O / 调用 / 中断 / 字符串 / 流 / 向量 / 文件 / 系统调用 / 低层回退 / **数据区三件套** / 跨 `call` 的循环变量 … |
| 跳过 | 3 | `18_machine_missing` / `19_unknown_quad` / `23_data_missing`：都是**负例**（`# 期望错误:`，指望读入 / 生成期报错）。Rust 版只读这个字段不比对，负例一律跳过（由 C++ 版校验） |

Rust 版**没有 `#[test]` 单元测试**，自检就是上面这个示例集运行器（外加
`quad-transfer-operators` 那份单文件自检）。

## 范围：实现了什么、没做什么

**实现了**：完整的三地址四元式模型与数值语义（含 128 位整数、浮点、字符串、流、
向量、文件、系统调用、**数据区三件套**指令族）、数据流分析与多轮优化、三套目标机
代码生成、带步数上限的模拟器、机器配置文件（`.ini`）。

**没做**：

| 没做 | 原因 |
| --- | --- |
| `# 期望错误:` 负例校验 | Rust 版只把该字段读进 `QuadFile`，不做比对（C++ 版会校验）；这类示例在自检里按负例跳过 |
| SSA、寄存器分配、数据流分模块、`t1t` 命令行 | 那些是 C++ `library/transfer` 里 `ssa/` / `regalloc/` / `dataflow/` 几块的事，本 crate 只到「四元式 core」这一层 |

## 目录与模块

| 文件 | 作用 |
| --- | --- |
| `src/lib.rs` | 库主体：类型与数值（`Value` / 13 种类型 / 字面量与类型转换 / 字符串与向量语义 / 128 位整数）、`Quad` / `Program`、`Target` / `MachineConfig`、`read_quads` / `write_quads` / `validate`、`default_config`、`load_config(_file)`、`variable_types` / `quad_type` |
| `src/optimize.rs` | `analyze`（基本块 + CFG + 到达定值 / 活跃变量 / 可用表达式）、`report`（分析报告文本）、`optimize_detailed`（附 `trace` 与 `rounds`）、`optimize` |
| `src/generate.rs` | `generate(program, config)`：优化后的四元式 → 目标机汇编文本 |
| `src/simulate.rs` | `simulate` / `simulate_streams`：执行生成的汇编（`Machine` / `Frame`），带步数上限 |
| `src/quadfile.rs` | `.quad` 读取：注释元信息、`resolve_config_path`、`render_output` |
| `src/main.rs` | `quad-transfer-example`：最小端到端示例 |
| `src/bin/suite.rs` | `quad-transfer-suite`：示例集运行器 |
| `src/bin/operators.rs` | `quad-transfer-operators`：移位 / 按位示例 |

## 四元式

一行一条，`(op, arg1, arg2, result)` 或带第五个字段 `extra`（`mul` 的高半 /
`div` 的余数）；无意义字段写 `_`。运算符可带类型后缀（`+:u32`、`*:f64`、`<<:i8`），
不写时按「结果变量类型 → 第一个操作数类型 → `i16`」推断。

```text
0: (decl, i32, global, b)      # 声明变量类型：作用域 global / local / _
1: (addr, arr_1, _, base)
2: (*:i64, 2, 8, offset)
3: (+:i64, base, offset, slot)
4: (load, slot, _, loaded)
5: (:=, loaded, _, b)
6: (label, _, _, L1)
7: (jz, b, _, L2)
8: (call, fact, n)
9: (ret, r, _, _)
10: (halt, _, _, _)
```

| 类别 | 写法 | 说明 |
| --- | --- | --- |
| 声明 | `decl` | `(decl, 类型, global\|local\|_, 变量)`；未声明按 `i16` |
| 赋值 | `:=` | `(:=, 源, _, 目标)` |
| 算术 | `+ - * / %` | 整数按位宽取模回绕；浮点按 IEEE-754；除零报错 |
| 进位 | `+c` `-b` | 进位加 / 借位减，用于多字加减链 |
| 双寄存器乘除 | `(mul, a, b, lo, hi)`、`(div, a, b, q, r)` | 乘法给高低半、除法给商与余数（写进第 5 个字段） |
| 一元 | `neg` `odd` `~` | 取负 / 奇偶 / 按位取反 |
| 转换 | `cvt:f64` `cvt:i32` | 整数↔浮点、位宽与符号转换 |
| 关系 | `== != < <= > >=` | 结果 0/1（默认 `i8`）；有符号 / 无符号按类型 |
| 移位 / 按位 | `<< >> & \| ^` | 右移：有符号算术右移、无符号逻辑右移；位数按类型宽度取模 |
| 控制 | `label` `j` `jz` `jnz` `halt` | 标签、无条件跳转、条件跳转、停机 |
| 调用 | `param` `call` `ret` | `param v, 形参`；`call f, 结果变量`；`ret v` |
| 中断 | `int` `iret` | 进中断处理程序；`iret` 弹出中断帧 |
| 输入输出 | `read` `write` `print` `putc` `getc` | 数值输入输出 / 字符串 / 字符（类型取声明类型，也可写 `write:f64`） |
| **数据区** | `addr` `load` `store` | 取数据区符号基址 / 按地址读 8 字节 / 按地址写 8 字节（都按小端；地址是数据区字节偏移）。见下面「数据区」一节 |
| 字符串 | 复制 / 长度 / 比较 / 拼接 / 子串 / 查找 | 见 `example/13_strings.quad` |
| 流 | 取流 / 写流 | STDIN / STDOUT / STDERR；见 `example/14_streams.quad` |
| 向量 | `vload` / `vstore` | 地址 + 长度，元素位数由类型后缀给出 |
| 文件 | `fopen` / `fread` / `fwrite` / `feof` | 见 `example/17_file.quad` |
| 系统调用 | `getpid` / `rand` / `time` / `putchar` / `exit` | 见 `example/16_syscall.quad` |
| 低层回退 | 运行时伪指令 `RCALL` / `RCALLW` | 机器字不够宽时（`W > M`）把整条运算交给运行时例程 |

乘除的结果是**固定的命名寄存器对**：IR 里写 `HI` / `LO`，生成时映射到目标机
（MIPS `HI:LO`、8086 `DX:AX`、ARM `r12:r14`）。`HI` 放高半 / 余数，`LO` 放低半 / 商。

## 数据区三件套：`addr` / `load` / `store`

数组 / 结构体在 IR 里被降级成「基址 + 偏移」，真实的值放在数据区字节镜像里。

| 四元式 | 生成的伪指令 | 语义 |
| --- | --- | --- |
| `(addr, 符号, _, 目标)` | `ADDR 目标, 符号` | 把数据区里该符号的**基址**写进目标变量 |
| `(load, 指针, _, 目标)` | `LOADAT 目标, 指针` | 按地址读 **8 字节（小端）** |
| `(store, 指针, 值, _)` | `STOREAT.<类型> 指针, 值` | 按地址写 **8 字节（小端）** |

- 地址就是数据区里的**字节偏移**；`addr` / `load` 的结果按 **`i64`** 参与后续分片
  （窄机器上自动拆成多个机器字，指针因此能整值还原）。
- 三条都是**运行时伪指令**（和 `VLOAD` / `FOPEN` 一样）：地址可能是多字变量，由
  模拟器拼出完整地址再去读写数据区，所以三套目标机生成的是同一条伪指令；机器能力
  由指令族 **`data`** 表示，配置里 `feature.data=off` 就报「指令存在、机器没有」。
- 数据区由两个文件给出：字节镜像 + 符号布局表（`# 数据:` / `# 布局:` 挂在 `.quad`
  头上，`# 布局:` 留空时按 `<镜像>.layout` 找）。布局表就是 `simple_ast` 的
  `exportDataLayout()` 输出，只认 `符号名 offset=N`：

```text
arr_1 offset=0 size=32 stride=8 type=array<i32>{count=4;align=2;label=numbers}
```

- 装载接口：`load_data_bin(path)`（字节）、`load_data_layout(path)`（符号）、
  `load_data_area(&mut config, bin, layout)`（一次装两样）。
- 报错文案与 `library/stackvm` 的 `ADDR` / `LOADAT` / `STOREAT` 对齐：没装数据区报
  `没有加载数据区，无法取符号地址 / 按地址取值 / 按地址写值`；符号不在布局表里报
  `数据区里没有符号：x`；越界报 `数据区越界读/写：地址 N`。
- 生成的目标汇编里对应一段 `.dataarea`：

```text
.dataarea 124
    .sym arr_1 0
    .hex 0 0a0000000000000014…      ; 字节镜像，16 字节一行
.enddataarea
```

例子见 `core/example/21_data_area.quad`（读 4 个槽位、写回一个再读回来验证）；
`23_data_missing.quad` 演示 `feature.data=off` 时报什么。

## 类型与机器模型

**类型**（13 种）：`i8 u8 i16 u16 i32 u32 i64 u64 i128 u128 f32 f64 str`，
未声明时按 **`i16`**。`str` 是字符串（字节长度 + 文本）。

**机器字长** `word_bits ∈ {8, 16, 32, 64}` 是机器**原生字长**，不是变量类型宽度；
后端按「类型宽度 `W` 与字长 `M`」决定展开方式：

| 情况 | 后端做法 |
| --- | --- |
| `W == M` | 一条原生指令（`i16` 省略类型后缀，保持老产物不变） |
| `W < M` | 一条指令 + 按类型归一化（模拟器用掩码 / 算术移位合成） |
| `W > M` | 多字展开：变量按 `M` 位分片（`x#0` 是低字），加减走 `ADDC` / `SUBB` 进位链，乘除等交给运行时例程（`RCALL` / `RCALLW`） |

## 机器配置（`.ini`）

```ini
target=arm
word_bits=32                 # 机器原生字长：8 / 16 / 32 / 64
reg_a=r4                     # 通用寄存器
reg_b=r5
reg_c=r6
reg_hi=r12                   # 乘除法固定结果对（HI）
reg_lo=r14                   # LO
stack_words=4096
data_bin=example/sample.data.bin
data.CONST_A=1000            # 命名数据，类型由 decl 决定
rodata.PI=3.14159            # 只读变量
memory_load=LDRSH {dst}, [{name}]    # 取内存模板（i16 / 旧默认）
memory_store=STRH {src}, [{name}]
immediate_load=MOV {dst}, #{value}
load.i32=LW {dst}, {name}            # 也可以按类型覆盖
store.i32=SW {src}, {name}
cmd.add=ADD {dst}, {lhs}, {rhs}      # 指令模板：花括号是占位符
features=add,sub,mul,div,shift,logic,float,io,data       # 这台机器有哪些指令族
feature.div=off                      # 单独关掉某一族
```

`default_config(target)` 给三套默认值：

| 目标 | 通用寄存器 | 乘除对 | 取内存 / 存内存 / 立即数 |
| --- | --- | --- | --- |
| `Target::Mips` | `$t0` / `$t1` / `$t2` | `HI` / `LO` | `LH` / `SH` / `LI` |
| `Target::I8086` | `AX` / `BX` / `CX` | `DX` / `AX` | `MOV`（`[name]` 内存操作数） |
| `Target::Arm` | `r0` / `r1` / `r2` | `r12` / `r14` | `LDRSH` / `STRH` / `MOV ... #imm` |

现成配置样例在 `library/transfer/core/example/`：`arm32.ini`、`mips32.ini`、
`mips16.ini`、`x86_16.ini`、`no_muldiv.ini`（关掉乘除，看 `RCALL` 回退）、
`no_data.ini`（关掉数据区指令族）、`mini_machine.ini`。用 `# 配置: xxx.ini`
挂在 `.quad` 文件头上，或直接调 `load_config_file`。

## 输入文件里的注释元信息

| 注释 | 含义 |
| --- | --- |
| `# 名称:` | 示例名（不写时用文件路径） |
| `# 输入:` | 演示程序的输入（按空白切成 `Vec<String>`） |
| `# 期望:` | 期望的**数值**输出（空格分隔） |
| `# 期望文本:` | 期望的**文本**输出（`print` / `putc`） |
| `# 配置:` | 机器配置 `.ini` 的相对路径（先按当前目录找，再相对 `.quad` 所在目录找） |
| `# 数据:` | 数据区**字节镜像**的相对路径（`addr` / `load` / `store` 用，如 `example/21_data_area.data.bin`） |
| `# 布局:` | 数据区**符号布局表**的相对路径；留空时按 `<镜像>.layout` 找 |
| `# 期望错误:` | 负例：期望读入 / 生成期报错（**Rust 版只读不校验**，这类示例在自检里跳过） |

## 公开接口

```rust
// 读入与配置
load_quad_file(path) -> QuadFile              // .quad + 注释元信息
load_config(text, fallback) -> MachineConfig  // 也接受 load_config_file(path, fallback)
default_config(target) -> MachineConfig       // Mips / I8086 / Arm

// 模型与数值
read_quads(text) / write_quads(program) / validate(program)
variable_types(program) / quad_type(quad, declared) / op_base(op) / op_type(op)
literal_value / literal_for / convert_value / display_value / eval_typed
add_with_carry / sub_with_borrow / multiply_wide / divide_wide
is_integer_literal / is_float_literal / is_numeric_literal / is_string_literal
string_value / string_length / string_compare / string_concat / substring / string_find
value_is_zero / value_low_byte

// 分析 → 优化 → 生成 → 执行
analyze(program) -> Analysis                   // 基本块 + CFG + 三类数据流集合
report(program, analysis) -> String
optimize(program) / optimize_detailed(program) -> Optimization
generate(program, config) -> String            // 目标机汇编文本
simulate(assembly, input, step_limit) -> Execution
simulate_streams(assembly, input, step_limit, ...) -> Execution
render_output(execution) -> String             // 数值输出渲染成一行文本
```

`Execution` 里带：`memory`（变量 → 值）、`output`（`write` 的值，保留类型）、
`text`（`print` / `putc` 的文本）、`stream_text` / `streams`（非 STDOUT 流）、
`exit_code`（`system exit` 的退出码）与 `steps`（执行的指令条数）。

## 怎么加一份示例

在 `library/transfer/core/example/` 里放一个 `NN_xxx.quad`，头部写 `# 名称:` 与
`# 期望:`（有文本输出再写 `# 期望文本:`，要换机器写 `# 配置:`），然后跑

```sh
cargo run --manifest-path library/transfer-rust/Cargo.toml --bin quad-transfer-suite
```

通过 / 失败会逐条打印；`Err` 收尾（有失败时退出码非 0）。注意负例（`# 期望错误:`）
在 Rust 版里会被跳过，不会计入通过数。
