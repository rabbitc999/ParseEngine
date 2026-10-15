# 示例目录

这里的每个 `.quad` 文件都是一段四元式程序，元信息写在 `#` 注释里，由
`example_suite.cpp` 读取并自动比对结果：

```text
# 名称: 无符号乘法 / 除法（双寄存器）
# 输入: 300 4          # read 依次取用的输入项（可选）
# 期望: 75             # write 输出的数值，空格分隔
# 期望文本: ok         # print / putc 的文本输出（可选）
# 配置: example/arm32.ini   # 用哪份机器配置（可选）
# 目标: 8086 / # 字长: 32   # 也可以直接给目标机和字长（可选）
# 数据: example/x.data.bin  # 数据区字节镜像（addr / load / store 用，可选）
# 布局: example/x.layout    # 数据区符号表；留空时按 <数据>.layout 找（可选）
0: (decl, u32, global, a)
...
```

## 基础与算术

| 文件 | 内容 |
| --- | --- |
| `minimal.quad` | 最小流程：没有 `decl` 时变量按 i16，沿用旧版语义 |
| `operators.quad` | 移位与按位运算 `<< >> & \| ^ ~`、强度削弱 `n*8 -> n<<3` |
| `01_types.quad` | `decl` 声明类型（i8/u8/i32）与按类型解释的有符号比较 |
| `02_unsigned.quad` | 无符号 `mul` / `div`：32 位结果在 16 位机器上分片，进固定寄存器对 |
| `03_float.quad` | `f32` / `f64` 运算与 `cvt` 类型转换（截断、单精度舍入） |
| `04_wide.quad` | 16 位机器上的 `u64` / `i128`：多字展开、进位链、立即数逐字装载 |
| `05_carry.quad` | 进位加 `+c` / 借位减 `-b`（进位标志串起多字加减） |
| `06_muldiv.quad` | 双寄存器乘法（高低半）与除法（商和余数） |
| `07_io.quad` | `read` / `write` / `print` / `putc` / `getc` 五种输入输出 |
| `08_call.quad` | `param` / `call` / `ret`：u32 阶乘递归（局部变量按帧存放） |
| `09_interrupt.quad` | 软中断 `int` / `iret` |
| `10_signed.quad` | 同一组位模式的有符号 / 无符号解释、算术右移与逻辑右移 |

## 数据、I/O 与系统调用

| 文件 | 内容 |
| --- | --- |
| `11_data.quad` | 命名数据 `data.*`、只读区 `rodata.*`、原始字节 `data_bin` |
| `12_word32.quad` | 同一份四元式换到 32 位机器：`u64` 只分两个字 |
| `13_strings.quad` | 字符串 `str`：复制 / 长度 / 比较 / 拼接 / 子串 / 查找 |
| `14_streams.quad` | 流指令：`STDIN` / `STDOUT` / `STDERR`，流获取与流写入 |
| `15_vector.quad` | 向量：地址 + 下标或长度，元素位数写在指令后缀里 |
| `16_syscall.quad` | 系统调用：`getpid` / `rand` / `time` / `putchar` / `exit` |
| `17_file.quad` | 外部文件 IO：`fopen` / `fwrite` / `fread` / `feof` / `fclose` |
| `21_data_area.quad` | 数据区三件套：`addr` 取基址、`load` 按地址读 8 字节、`store` 按地址写（配 `21_data_area.data.bin` + `.layout`） |
| `config.quad` | 外部机器配置、CFG 报告与 ARM 生成示例 |

## 负例与低级替代

| 文件 | 内容 |
| --- | --- |
| `18_machine_missing.quad` | 负例：四元式存在但机器没有该指令族（配合 `mini_machine.ini`） |
| `19_unknown_quad.quad` | 负例：四元式本身不存在（未知操作） |
| `20_low_level_fallback.quad` | 机器没有乘除法时，用移位、按位与循环常量展开替代 |
| `22_loop_call.quad` | 循环 + 调用：跨 `call` 的循环变量不能被传播成环内临时量（优化器回归） |
| `23_data_missing.quad` | 负例：机器关掉数据区指令族（配合 `no_data.ini`） |

## 机器配置与数据

| 文件 | 内容 |
| --- | --- |
| `mips16.ini` | 16 位 MIPS 教学机（内建默认配置的显式写法） |
| `mips32.ini` | 32 位 MIPS 教学机：`u32` 不再分片，`u64` 只占两个字 |
| `arm32.ini` | 32 位 ARM：自定义寄存器 r4/r5/r6、只读区、命名数据、Bin 数据 |
| `x86_16.ini` | 16 位 8086：乘除法使用固定的 AX/DX 寄存器对 |
| `mini_machine.ini` | 精简机器：只有移位 / 按位 / 进位，用来演示"机器没有该指令" |
| `no_muldiv.ini` | 没有乘除法与浮点的机器，演示低级语义替换 |
| `no_data.ini` | 关掉数据区指令族（`feature.data=off`）：`addr` / `load` / `store` 报"机器不存在" |
| `sample.data.bin` | 示例 Bin 数据（6 字节，按顺序放进 `.data` 的 `byte_0`…） |
| `21_data_area.data.bin` | 数据区三件套示例的字节镜像（4 个 8 字节槽位：7 / 15 / 30 / 60） |
| `21_data_area.layout` | 上面那份镜像的符号表（`arr_1 offset=0 size=32 stride=8`） |

配置项说明（`key=value` 纯文本，不需要 JSON）：

```ini
target=mips              # mips / 8086 / arm
word_bits=16             # 机器原生字长：8 / 16 / 32 / 64
reg_a=$t0                # 工作寄存器与乘除法固定结果对
reg_hi=HI
reg_lo=LO
stack_words=1024         # 模拟栈容量
data_bin=example/sample.data.bin   # 原始字节 -> .data 的 byte_0、byte_1……
data.NAME=1000           # 命名数据，类型由 decl 决定
rodata.PI=3.14159        # 只读变量，模拟器拒绝写入
cmd.add=ADDU {dst}, {lhs}, {rhs}   # 指令模板可整体替换
load.i32=LW {dst}, {name}          # 也可以按类型覆盖取内存模板
features=shift,bitwise,carry       # 这台机器实现了哪些指令族（缺省全开）
feature.float=off                  # 也可以单独关掉某一族
# 一条命令可以由多条指令完成：写 \n 或重复写同一个键追加
cmd.div=CWD
cmd.div=IDIV {rhs}
cmd.div=MOV {dst}, AX
```

## 怎么跑

```text
make -C library/transfer check                  # 构建并跑全部示例自检
build/transfer-one-thread/t1t-example-suite core/example mips   # 只跑示例集（可换 8086 / arm）
build/transfer-one-thread/t1t-example core/example/03_float.quad
build/transfer-one-thread/t1t-example-config core/example/config.quad core/example/arm32.ini
build/transfer-one-thread/t1t transfer core/example/21_data_area.quad \
    --data=core/example/21_data_area.data.bin --layout=core/example/21_data_area.layout
```

调试用 `TRANSFER_TRACE=1` 可以让模拟器把每条执行的指令打印到 stderr。
