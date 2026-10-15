# 栈式虚拟机（`library/stackvm`）

把 `library/transfer` 那套**四元式**编译成**栈指令**并执行：`PUSH / LOAD / STORE /
ADD / JZ / CALL …`。只依赖 C++17 标准库，不参与 ParseEngine 主程序的构建。

## 为什么再做一个后端

`library/transfer` 走的是「四元式 → CFG → 优化 → 寄存器型目标代码（MIPS / 8086 /
ARM）→ 模拟执行」，要处理寄存器分配、调用约定、机器字长分片。栈式虚拟机把这些问题
换成"求值栈 + 变量槽"，是教学上最有价值的对照：

```text
四元式                 栈指令                              执行
(:=, 2, _, x)          PUSH 2 ; STORE x                    取 2 存进 x
(+, x, 3, t1)          LOAD x ; PUSH 3 ; ADD ; STORE t1     压栈相加再存回
(jz, t1, _, L)         LOAD t1 ; JZ L                      假就跳 L
(write, t1, _, _)      LOAD t1 ; PRINT                     打印
```

## 四元式格式

与 `library/transfer/core/example/*.quad` **完全兼容**（本仓库的示例直接拷了过来）：

```text
# 名称: 最小流程
# 期望: 5
0: (:=, 2, _, x)
1: (+, x, 3, t1)
2: (write, t1, _, _)
3: (halt, _, _, _)
```

`# 名称:` / `# 期望:` / `# 期望文本:` / `# 输入:` 会被解析出来，示例套件据此比对。

| 四元式 | 说明 |
| --- | --- |
| `decl` | 变量类型（`i8/u8/i16/u16/i32/u32/i64/u64/f32/f64`；不声明按 `i16`）与作用域（`global` / `local`，`local` 会在当前调用帧里开一个槽） |
| `:=` | 赋值 |
| `+ - * / %`、`& \| ^`、`<< >>`、`neg`、`~` | 算术 / 位运算，按 `:类型` 后缀回绕与判符号 |
| `== != < <= > >=` | 比较（有符号 / 无符号按后缀） |
| `mul` / `div`（4~5 参数） | 双结果：按类型位宽切 `lo/hi`、`q/r` |
| `cvt:` | 整数 ↔ 浮点 |
| `label` / `j` / `jz` / `jnz` | 标签与跳转 |
| `read` / `getc` / `write` / `print` / `putc` | 输入输出（数值与文本分开收集） |
| `param` / `call` / `ret` | 调用与递归（**每层调用各有一帧局部变量**，进函数分配、返回时释放） |
| `alloc` / `free` / `drop` | 堆：`(alloc, 字节数, _, 目标)` / `(free, 地址, _, _)`（严格，地址不对就报错）/ `(drop, 地址, _, _)`（**软释放**：不是活跃堆块就跳过，给"作用域自动释放"用）；`call` 一个不在标签表里的 `__vm_*` 名字 → 原生函数 |
| `check` / `checkptr` | 数组下标越界陷阱：`(check, 下标, 上限, 标签)`、`(checkptr, 下标, 基址, 步长, 标签)` |
| `callptr` | 函数指针的间接调用：`(callptr, 函数指针, _, 目标)`；实参按**位置**绑给目标函数的形参 |
| `halt` | 停机 |

不支持的（`+c`/`-b` 进位链、`int`/`iret` 中断、数据区等）在组装期给一条诊断，不会
悄悄跳过。

### C++ 原生函数注册表

`NativeRegistry` 是公开胶水入口。回调收发 `NativeValue` 单槽值，字符串/指针/句柄按
整数地址传递；`NativeCallContext` 提供受控的内存、字符串分配与错误报告接口：

```cpp
stackvm::NativeRegistry natives;
natives.add("cpp_add", 2,
    [](stackvm::NativeCallContext&, const std::vector<stackvm::NativeValue>& args) {
        return stackvm::NativeValue::fromInt(args[0].asInt() + args[1].asInt());
    });

stackvm::Machine machine;
machine.setData(&data);
machine.setNativeRegistry(&natives);
stackvm::RunResult result = machine.run(program);
```

`NativeCallContext::readList` 可把 Magel `lang::List<T>` 解包为胶水值；`invoke` 可在宿主回调
期间同步调用 Magel 函数指针，并复用当前执行的堆、对象表、全局变量与调用栈。由上下文创建的
函数、结构体和枚举视图只在本次 Native 回调内有效，不能跨线程或保存到回调结束以后。

内建 `__vm_*` 名称优先，宿主注册表不能覆盖 VM 运行时。注册表只属于宿主进程，`.svmb`
只保存调用符号；加载二进制后应重新挂接同一组实现。
`__vm_panic` 接受一个指向 VM 长度字符串的地址（8 字节长度 + UTF-8 字节，不带结尾 NUL），
并通过 `RunResult::error` 立即终止；
它是 Magel `panic(string)` 的运行时入口，不把错误当成正常 `exitCode`。

### 精确计时、指令钩子与协程上下文

`Machine::setTimeCallback()` 接收宿主提供的单调纳秒时钟。设置后，`RunResult` 会填充
`timingAvailable`、`startedNanoseconds`、`finishedNanoseconds` 与 `elapsedNanoseconds`；
VM 不擅自选择平台时钟，因此桌面和嵌入式宿主可以使用各自最准确的计时源。

`Machine::setInstructionHook(N, callback)` 在每完成 N 条指令后同步回调一次；`N=0` 或空回调
表示禁用。`InstructionHookInfo` 提供累计指令数、刚执行完的 PC、协程句柄和操作码。
钩子运行在 VM 执行线程中，不应在回调里递归调用同一个 `Machine::run()`。

`lang::SharedCoroutine::create` 返回正整数协程句柄，`wait` 等待该句柄完成，`yield` 与兼容拼写
`yelid` 让出执行权。各上下文拥有独立求值栈、调用帧、待绑定参数和 PC，共享全局区、堆、
数据区与对象表；`RunResult.coroutines` 给出主上下文和所有协程的最终状态。
`send(handle, text)` 把文本副本送进协程收件箱；`receive()` 在无消息时暂停该协程，
`current()` / `parent()` 查询协程句柄。

### 可移植宿主胶水

文件系统不写死在解释器核心。`stackvm_glue.hpp` 只定义 `FileHandle` / `FileApi` 接口，
`stackvm_glue.cpp` 提供基于 `std::fstream` 的桌面默认实现。嵌入式宿主实现这两个接口后，
在运行前注入即可：

```cpp
class BoardFileApi final : public stackvm::FileApi {
public:
    std::shared_ptr<stackvm::FileHandle> open(const std::string& path,
                                               const std::string& mode,
                                               std::string& error) override;
};

BoardFileApi files;
stackvm::Machine machine;
machine.setFileApi(&files);
stackvm::RunResult result = machine.run(program);
```

接口对象和它返回的文件句柄必须至少存活到 `run()` 返回。不能提供文件系统的固件也可以
注入一个总是返回错误的实现，使 `std::fileOpen` 正常得到 `Result::Err`，而不是依赖桌面 API。

Magel 的 `std::Parse`、`std::Path` 与 `std::Memory` 由 StackVM 的受控原生函数实现：解析
只接受完整文本，路径处理不触碰文件系统，内存层只返回统计值和 Arena 分配出的字节地址。
Arena 在对象表中只保存所持堆块的地址清单；`reset/drop` 释放这些块，Magel 侧只能经
`std::Span<T>` 使用，拿不到 `RuntimeObject` 布局。`std::Random::fromEntropy` 则刻意不放进
内建表，由宿主通过 `NativeRegistry` 单独注入；嵌入式环境可换成自己的硬件熵源。

### 局部变量：`(decl, T, local, x)` 与调用帧

`decl` 先登记类型（`program.types[名字] = T`，前端据此挑打印格式、做传参归一化），
**作用域是 `local` 时还会发一条 `DECLARE x` 指令**：执行到它就往当前调用帧里开一个
槽（初值 0；已经有同名槽就不动，形参 / `self` 是调用时绑进来的）。

帧的生命周期：

- `call` —— 在**堆上**开一帧（C++ 里是 `std::make_unique<Frame>()`，对应 `malloc`），
  实参按形参名绑进这一帧；
- `ret` —— 把这一帧的所有权摘下来，帧析构时本帧全部局部变量一起释放（对应 `free`）。

所以**递归的每一层各有自己的一片局部变量**：`(decl, i32, local, acc)` 在每次调用里
都是新的 `acc`，回溯时不会被内层覆盖。用 `unique_ptr` 而不是裸 `new` / `delete` 是
为了"分配点 / 释放点还是进函数 / 出函数这两处，但任何一个返回分支都不会漏释放"。

要找变量时先看当前帧的局部、再看全局；没有帧（顶层代码）时 `DECLARE` 落到全局槽，
与"函数外的局部名就是全局"一致。名字仍然是**按名字全程序唯一**的：同一个名字不可能
既是一层的局部、又是另一层的局部 —— 前端要保证这一点（magel 的重名自动加 `$序号`）。

### 堆：`(alloc, 字节数, _, 目标)` / `(free, 地址, _, _)`

数据区是**静态**的：符号与槽位在编译期就定死，程序跑起来只有那一片。`new` / `malloc`
这类"要多少字节运行期才知道"的对象不放数据区、也不放在求值栈 / 调用帧里，而是走堆：

```text
0: (alloc, 16, _, p)      ; p = 堆上连续 16 字节
1: (store:i32, p, 42, _)  ; 往 p 写（和数据区同一套 (load …) / (store …)）
2: (+:i64, p, 8, t)       ; 指针算术：p + 8 就是第二个槽
3: (load:i32, t, _, v)
4: (free, p, _, _)        ; 还给堆
```

- **地址空间是连着的**：数据区占低地址，堆紧跟其后（没有数据区时从 8 起，地址 0 留给
  空指针），所以 `(load, 地址, …)` 不关心对象在哪一段。
- 分配：1 字节单元，运行期间地址只向后增长；`free` 把旧块标记为不可访问，
  同一次 `run()` 内不复用其地址，防止悬空指针命中新对象。记录和字节存储在运行结束后
  一并释放，或在下次 `reset(base)` 时清空。长时间运行且频繁分配的程序会增加已提交内存；
  `free(NULL)`（地址 0）是空操作，同一块 `free` 两次、或者读写已经 `free` 掉 / 没分配过的
  地址都会**报错**，不会悄悄读写脏内存。
- `RunResult` 里带 `heapPeakBytes`（峰值）、`heapLiveBytes` / `heapLiveBlocks`（停机时
  还没释放的）；CLI 也会打一行，漏了就一眼看到。

#### 自定义 VM 堆

`Machine::setHeapAllocator()` 可以替换上述字节堆。实现公开的 `HeapAllocator` 接口后，
`alloc/free`、按地址 load/store、`checkptr`、内置字符串运行时和
`NativeCallContext::makeString()` 都会走同一个分配器。每次 `run()` 开始前 VM 会调用
`reset(base)`，地址 0 仍保留为空指针；不设置时使用上述不复用地址的动态堆。

固定容量、静态块表的参考实现与测试在
[`library/embed_stackvm`](../embed_stackvm/README.md)。这个接口只接管 VM 字节堆，不接管
C++ 调用帧、STL 容器、大整数或运行期对象表的宿主分配。

#### 自定义求值栈上限

`Machine::setStackLimit(slots)` 限制每个协程各自的求值栈槽数；`0` 表示不限制，也是默认值。
所有 VM 压栈指令共用这一检查，超限时 `RunResult.ok` 为 false，`error` 给出上限、指令位置
和指令文本。该接口限制的是 StackVM 的临时求值栈，不包含函数调用帧或宿主 C++ 线程栈。

#### 性能基准

`benchmark_stackvm.cpp` 提供可复现的整数循环基准。它在组装之外计时，先预热一轮，再报告
多轮执行的中位数、实际指令数、MIPS、求值栈峰值与求和校验值：

```sh
make benchmark CXXFLAGS='-std=c++17 -O2 -DNDEBUG -Wall -Wextra -Wpedantic'
../../build/stackvm/stackvm-benchmark 1000000 7
```

不同编译器、CPU、电源策略和调试开关会显著影响结果；对比优化前后时应固定参数与构建旗标。

### 数组下标检查：`(check …)` / `(checkptr …)`

front end 在每次下标之前发一条陷阱指令，越界就**终止运行**并报出是谁越界：

```text
0: (check, i, 6, m)          ; i 必须落在 [0, 6)：上限可以是常量，也可以是变量
1: (checkptr, i, p, 16, row) ; 第 i 个元素（p + i×16，共 16 字节）必须落在 p 所在的**那一块**里
```

- `check`：前端知道长度的场合（`T[N]` 的每一维、运行时数组从对象里读出来的长度）；
- `checkptr`：前端不知道长度的场合（裸指针）。它按"基址所在那一块"的容量算：
  堆块按**分配大小**、数据区按布局里的 `size=`（所以 `.layout` 的 `size=` 现在真的会被用）；
  基址**不在任何一块已分配内存里**（没分配 / 已经 `free`）同样报错 —— 这也顺手把
  "释放后再用" 在下标这一侧拦住了。基址落在块中间时，容量按"从基址到块尾"算。

### 内部 C++ 运行时（字符串 / 格式化）

运行时使用一张 **Slot 原生函数表**（`kSlotNatives`）：参数与结果都按 `Slot` 走
（地址 / 长度必须落在**整数槽**里），能拿到堆与数据区、能报错，并且支持**变参**。

| 名字 | 作用 |
| --- | --- |
| `__vm_str_lit` / `__vm_str_len` | 把字面量字节复制成显式长度堆串 / 读取 8 字节长度头 |
| `__vm_vec_pack` | 把变长调用实参打包成运行时 `vec<T>` 对象 |
| `__vm_str_join` | 变参：把一串字符串接成一个（只分配一个结果） |
| `__vm_str_sub` | 切片（半开区间，越界夹住） |
| `__vm_str_at` / `__vm_str_set` | 按字节取 / 写一个字符（越界报错） |
| `__vm_str_cmp` / `__vm_str_pos` | 字典序比较 / 找子串 |
| `__vm_str_fmt` / `__vm_print` | 按格式串拼出新串 / 追加到文本输出（`snprintf` 排版） |
| `__vm_read_text` | 把下一项输入原文读成堆串 |
| `__vm_memcpy` / `__vm_memset` | 字节级搬 / 填 |
| `__vm_file_open/_read/_write/_eof/_close` | 文件资源句柄：打开、读一行、写入、EOF、关闭 |
| `__vm_proc_run/_read/_close/_system` | 宿主进程：stdout 管道读一行 / 关闭取退出码，或直接 `system` |

于是"字符串"这件事在虚拟机里是**一串真字节 + C++ 实现**：语言侧不写逐字节循环，
标准输出的宽度 / 精度、拼接、切片与格式化都不用前端逐指令展开。

### 运行期对象表：大整数、容器、装箱与资源

除"地址 + 堆"之外，虚拟机还维护一张**对象表**（`ObjectStore`：句柄 → C++ 对象）：

| 对象 | 里面是什么 | 谁在做 |
| --- | --- | --- |
| `bint` | `bigint::BigInt`（任意精度整数） | [`library/big_number`](../big_number/README.md)，加 / 减 / 乘 / 除 / 取余都在那边 |
| `bfloat` | `bigfloat::BigFloat`（任意精度十进制浮点） | 尾数复用 `BigInt`，四则与比较保持十进制语义 |
| `vec<T>` | `std::vector<ObjectValue>` | 本库（有序表：增删改查 / 按下标访问） |
| `set<T>` / `map<K,V>` | `std::map<ObjectKey, ObjectValue>` | 本库（按键排序，元素不重复 / 键值对） |
| `obj` | 一个装箱值 + 类型号 | 本库（给前端做 `nameof` / 泛型式派发） |
| `file` / `proc` | `std::fstream` / `popen` 管道 | 本库（宿主文件 IO 与进程 stdout 管道；资源释放时自动关闭） |

- 句柄取一个**很高的区间**（`kObjectHandleBase = 1e12` 起），与堆地址、数据区地址分开：
  `(free, …)` / `(drop, …)` 一眼能分出"这是对象句柄还是内存地址"；
- `(drop, 句柄, _, _)` 和堆那条一个脾气（软释放：已经释放过就当没这回事），
  `(free, 句柄, _, _)` 是严格的（重复释放报错）；
- `RunResult::liveObjects` 是停机时还挂着的对象个数（不为 0 就是漏了）；
- 容器里的**字符串按副本存**（`std::string`），取出来时在堆上复制一份新的 ——
  容器因此活得比某根字符串久也不会悬空。
- 容器可按前端传入的摊平字段布局保存结构体快照；字符串和对象字段递归复制，
  显式指针字段仅复制地址。取出的结构体临时块在本次 `run()` 结束时释放。
- `file` / `proc` 是资源句柄，不做对象值语义复制；显式 close 后句柄仍可被释放，释放时会跳过已经关闭的宿主资源。

**大整数是附加功能**，编译期开关 `STACKVM_WITH_BIGINT`（见 `stackvm.hpp`，缺省 1）：

```sh
make -C library/stackvm                        # 缺省：带上大整数（会一起编 bigint.o 进归档）
make -C library/stackvm STACKVM_WITH_BIGINT=0  # 轻量构建：不依赖 big_number
```

关掉之后 `__vm_bint_*` 那几个名字还在表里，但一调就报
"这个构建没有把大整数编进来（附加功能）"——比"函数入口不存在"清楚。宿主侧想按构建
分支的话用 `stackvm::kBigIntEnabled`（编译期常量）。

大浮点另有 `STACKVM_WITH_BIGFLOAT`，缺省跟随 `STACKVM_WITH_BIGINT`。由于尾数依赖
`BigInt`，不能在 `STACKVM_WITH_BIGINT=0` 时单独开启：

```sh
make -C library/stackvm STACKVM_WITH_BIGFLOAT=0
make -C library/stackvm STACKVM_WITH_BIGINT=0 STACKVM_WITH_BIGFLOAT=0
```

### 数学库边界

StackVM 不再内建 `__vm_sin` / `__vm_pow` 等数学指令。语言前端应声明普通宿主函数，
再通过 `NativeRegistry` 注册平台数学库；Magel 的桌面实现见
`magel/glue/std_math.hpp`。这样嵌入式移植可替换数学库，不需要修改 VM。

## 四元式化简

参考 `library/transfer/core/optimizer.cpp` 与 `project/pl0-super`，只做保守、能一句话
说清的四趟（默认打开，CLI 的 `--no-opt` 关闭）：

| 趟 | 做什么 | 例子 |
| --- | --- | --- |
| 常量折叠 | 字面量运算 + 已知常量代入 | `(+, 2, 3, t)` → `(:=, 5, _, t)` |
| 代数化简 | 加 0 / 乘 1 / 移 0 … | `(+, x, 0, a)` → `(:=, x, _, a)` |
| 死分支 | 常量条件的 `jz` / `jnz`；`j` 之后到下一个标签的代码 | `(jz, 0, _, L)` → `(j, _, _, L)` |
| 死代码 | 结果变量从没被读过的 `:=` | `(:=, 2, _, x)`（x 无人读）→ 删除 |

会在标签、`call`、以及任何"不是字面量赋值"的写入处**保守地清空常量表**——理由是
控制流汇合点与调用可能改掉全局，猜不得。

两条容易踩的细节（都有回归断言）：

- **`decl` 不参与"不可达代码"清扫**：它登记的是类型元信息。`simple_ast` 生成的形参
  声明就排在引导用的 `(halt)` 之后、函数标签之前，删掉它 `program.types` 就少了形参
  类型，传参归一化与 `print` 的格式都会跟着错；（`local` 作用域那条额外发出来的
  `DECLARE` 指令同样保留，但它排在函数标签之前，本来也执行不到，形参仍由 `call` 绑定。）
- **折叠浮点要看类型**：`(neg:f64, 2.5, _, t)` 折成 `-2`（先 `atoll` 再取负）就把小数
  丢了 —— 现在浮点取负 / `cvt` 到浮点都用双精度折，而且结果文本一定带小数点
  （`2` → `2.0`），免得后端把它当整数槽。

```text
$ stackvm disasm examples/minimal.quad --stats
; 化简：常量折叠 1，代数化简 0，死分支 0，死代码 1（共 2 轮）
0: PUSH 5
1: STORE t1
...
$ stackvm run examples/08_call.quad --stats
720
```

## 命令行

```text
stackvm <命令> <file.quad|file.svmb> [选项]

  run      组装并执行（默认）
  compile  把 .quad 编译为可直接加载的 .svmb
  disasm   显示栈指令（支持 .quad / .svmb）
  check    只组装或加载，检查程序是否合法

  --input "1 2"   read / getc 的输入项
  --trace         打印每条执行的指令
  --debug         按源码位置逐行打印（需要产物带调试信息），出错时给位置与调用栈
  --limit N       指令上限（缺省 100 万，防死循环）
  --no-opt        关闭四元式化简
  --out FILE      compile 输出路径（缺省 <输入>.svmb）
  --data FILE     数据区二进制
  --layout FILE   数据区布局表
  --stats         打印化简统计
```

`.svmb` 保存的不是四元式文本，而是组装后的 `Program`：操作码、立即数、字符串字段、
标签、类型、函数形参表和公开导出标签表。文件有固定魔数、版本、长度与 FNV-1a 校验和，整数按小端编码；
截断、未知版本、非法枚举、重复表项或校验失败都会拒绝加载。数据区仍由 `--data` / `--layout`
传入，因此同一份只读程序可以配不同数据镜像。

格式 v4 沿用 v2 的**变长指令**：`1 字节 opcode + 1 字节操作数掩码`，后面只写这条指令实际
拥有的字段。整数立即数和来源位置采用 ZigZag/ULEB128，字符串长度采用 ULEB128；浮点
立即数需要时才写 8 字节。无操作数指令因此只有 2 字节。v3 在元数据末尾增加 exports 表，
v4 再在 exports 之后追加**调试信息表**（指令下标 → 源码位置）；模块链接只应使用 exports
里的标签；加载器仍能读取旧的 v1 固定字段与 v2 变长指令文件，旧文件没有显式可见性，
因此兼容地把全部标签视为可见。新生成文件统一写 v4。
裸四元式本身没有模块可见性声明，因此 `assemble` 默认把全部标签放进 exports；具有模块
语义的前端（例如 Magel）会在组装后清空并按自己的公开 API 重新填写。
`check` 会报告公开入口数量，`disasm` 会在指令前以 `; export 标签` 列出它们。

### 调试

产物里自带"指令 → 源码位置"表，VM 因而能把运行位置、出错位置与调用栈翻译成源码行。
表在**组装期**填好（`Assembler` 记下当前四元式的文本行与最近的 `(label …)`；同位置的
连续指令自动去重），随 `.svmb` 的 v4 调试节一起保存。`Machine` 侧提供
`setBreakpoints` / `clearBreakpoints` / `setSingleStep` / `setDebugHook`（回调收到
`DebugEvent`，返回 `Continue` / `StepInto` / `Abort`）/ `attachDebugInfo` /
`currentLocation` / `callStack`；`RunResult` 出错时带 `failureLocation` 与 `sourceTrace`
（调用者那一帧显示**调用点**）。命令行 `--debug` 按位置逐行打印（走 stderr），出错时
自动打印位置与调用栈；位置里的 `file`/`line` 字段留给语言源码，目前显示的是 `.quad` 行号。
完整说明与实测输出见 [magel/stackvm/README.md](../../magel/stackvm/README.md#调试)。

运行内存以 **1 字节 cell** 寻址和分配（`kRuntimeCellBytes == 1`），堆不再把短字符串或
小块请求向上补齐到 8 字节。`int64`、`double`、地址和对象句柄仍是 8 字节宽值，只是连续
占用 8 个 cell，而不是把“槽”误当成最小寻址单位。

```sh
stackvm compile demo.quad --out demo.svmb
stackvm check demo.svmb
stackvm disasm demo.svmb
stackvm run demo.svmb --data demo.data.bin --layout demo.layout
```

输出约定与 `library/transfer` 的模拟器一致：`write` 的数值放进 `output`（多个值之间
一个空格），`print` / `putc` 的文本放进 `text`。所以 `07_io.quad` 的
`# 期望: 14 7` 与 `# 期望文本: n=|Z|!` 能分别对上。

## C++ API

```cpp
#include "stackvm.hpp"

stackvm::QuadFile file = stackvm::loadQuadFile("demo.quad");   // 解析
const stackvm::OptimizationReport report = stackvm::optimize(file); // 化简（就地）

stackvm::Program program;
stackvm::assemble(file, program);                              // 组装成栈指令
stackvm::saveProgramBinary(program, "demo.svmb");             // 写二进制程序

stackvm::Program restored;
stackvm::loadProgramBinary("demo.svmb", restored);            // 不再解析四元式

stackvm::Machine machine(/*trace=*/false);
const stackvm::RunResult result = machine.run(restored, {"6"}); // 执行
std::cout << result.output << result.text;                     // 数值 / 文本
std::cout << result.exitCode;                                  // exit(n)；普通 halt 为 0
std::cout << machine.run(program).steps;                       // 指令条数
```

`Program` 里能拿到 `code`（栈指令，`Instruction::toString()` 可读）、`labels`、
`types`；`OptimizationReport` 给出每趟改了多少处与逐条 `trace`。

## 构建与自检

```text
make -C library/stackvm            # libstackvm.a + stackvm + stackvm-tests
make -C library/stackvm check      # 单元测试 + 示例套件 + 反汇编演示
```

产物在 `build/stackvm`。`check` 跑 249 项断言：类型回绕与符号、无符号比较、浮点与
`cvt`、循环与跳转、递归调用（6! = 720）、**局部变量按帧存放**（递归里各层互不影响、
返回值写回调用方那一帧）、**堆的 alloc / free**（不复用地址、双重释放、释放后读写、漏在内）、
**数组下标检查**（`check` 的常量 / 变量上限、`checkptr` 对堆块与数据区符号的容量、
释放后再查）、**函数指针**（`callptr` 按位置绑实参、空指针 / 名字不对 / 实参个数不对）、
`exit/assert/panic` 终止语义、双字乘除、输入输出、解析 / 组装诊断，以及
**化简前后语义一致**，以及 `.svmb` 的完整往返、直接执行、损坏校验与加载后通过
`NativeRegistry` 重挂宿主函数；最后把
`examples/*.quad` 逐个执行并与文件里的 `# 期望:` 对照。

## 目录

```text
library/stackvm/
├── stackvm.hpp / stackvm.cpp   四元式解析、组装、二进制程序、栈式执行、四元式化简
├── stackvm_glue.hpp            可移植的宿主文件接口
├── stackvm_glue.cpp            基于 std::fstream 的桌面默认胶水
├── main.cpp                    命令行工具（compile / run / disasm / check）
├── test_stackvm.cpp            单元测试 + 示例套件
├── examples/                   与 library/transfer/core/example 同一批 .quad
│                               加上本库自己的 13_heap（堆与运行时函数）
├── Makefile / CMakeLists.txt   两种构建方式
└── README.md
```

## 与 `library/transfer` 的关系

| | `library/transfer` | `library/stackvm`（本库） |
| --- | --- | --- |
| 输入 | 同一套四元式 | **同一套四元式**（示例直接共用） |
| 后端 | 寄存器型：MIPS / 8086 / ARM | **栈式**：求值栈 + 变量槽 |
| 优化 | CFG / 数据流 / 循环优化（30+ KB 的 optimizer） | 保守四趟（常量折叠 / 代数化简 / 死分支 / 死代码） |
| 适合 | 讲"真正的后端" | 讲"最简执行模型"，也方便当脚本引擎的原型 |

两者互不依赖，可以拿同一份 `.quad` 分别跑，对比产物与执行结果。


## 数据区：`--data` / `--layout`

`simple_ast` 把数组 / 结构体 / 字符串降级成"基址 + 偏移"后，真正的值放在 `.data.bin` 里。
`stackvm` 通过 `DataArea` 把它加载进来，四元式里的三条指令就能真读写那片内存：
`DataArea::binary` 使用 `std::vector<std::uint8_t>` 保存原始字节，不再借用
`std::string` 表示二进制数据；`char*` 只保留在与 C 风格读写缓冲区交接的 API 边界。

| 四元式 | 栈指令 | 作用 |
| --- | --- | --- |
| `(addr, 符号, _, t)` | `ADDR 符号` | 把数据区里该符号的基址压栈 |
| `(load, 指针, _, 目标)` | `LOADAT` | 弹地址，读 8 字节小端，按整数压栈 |
| `(load:f64, 指针, _, 目标)` | `LOADAT` | 同上，但这 8 字节按 **IEEE-754 双精度**解释（`f32` / `f64` 都走这条） |
| `(store, 指针, 值, _)` | `STOREAT` | 弹值、再弹地址，写 8 字节小端整数 |
| `(store:f64, 指针, 值, _)` | `STOREAT` | 同上，但写的是值的 IEEE-754 双精度位模式 |

```text
stackvm run demo.quad --data demo.data.bin --layout demo.layout
stackvm run demo.quad --data demo.data.bin --trace     # 不给 --layout 时自动找 <bin>.layout
```

`--layout` 的文件格式就是 `simple_ast` 的 `exportDataLayout()` 输出，一行一个符号：

```text
arr_1 offset=0 size=32 stride=8 type=array<i32>{count=4;align=2;label=numbers}
obj_4 offset=32 size=4 stride=8 type=struct Box {}{value}
```

- `offset` 是符号在数据区里的**基址**，`ADDR` 取的就是它；
- `stride` 是槽位步长（`simple_ast` 固定写 8），和四元式里 `(*:i64, index, 8, t)` 的步长一致；
  `DataArea` 会把它读进 `strides` 表备查，**寻址用的是四元式里的步长**；
- 读写都是 8 字节小端；**浮点靠四元式上的类型后缀区分**（`(load:f64 …)` / `(store:f32 …)`），
  没有后缀就按整数读写 —— `simple_ast` 生成的下标 / 成员访问都带后缀，整数与浮点
  共用同一片 8 字节槽位；
- 越界会报 `数据区越界读/写：地址 N`；没挂数据区时三条指令分别报
  `没有加载数据区，无法取符号地址 / 按地址取值 / 按地址写值`。

完整的端到端例子（`simple_ast` 生成 → `stackvm` 取值）：

```text
$ simple_ast out/demo                       # 产出 .quad / .data / .data.bin / .data.h / .layout
$ stackvm run out/demo.quad --data out/demo.data.bin --layout out/demo.layout
30 30
; 执行 60 条指令，栈最深 2
```
