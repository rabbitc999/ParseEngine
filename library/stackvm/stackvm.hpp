/**
 * @file stackvm.hpp
 * @brief 栈式虚拟机：读 `library/transfer` 那套四元式，编译成栈指令并执行。
 *
 * ## 为什么要"接上栈虚拟机"
 *
 * `library/transfer` 那条线是「四元式 → 基本块 / CFG → 优化 → 寄存器型目标代码
 * （MIPS / 8086 / ARM）→ 模拟执行」。寄存器型后端要处理寄存器分配、调用约定、
 * 机器字长分片；而**栈式虚拟机**把这些都换成了"求值栈 + 变量槽"：
 *
 * ```text
 * 四元式             栈指令                          执行
 * (:=, 2, _, x)      PUSH 2; STORE x               取 2 存进 x
 * (+, x, 3, t1)      LOAD x; PUSH 3; ADD; STORE t1 压栈相加再存
 * (jz, t1, _, L)     LOAD t1; JZ L                 假就跳
 * (write, t1,_,_)    LOAD t1; PRINT                打印
 * ```
 *
 * 于是"后端"只需要一个栈和一个跳转表，代价是每条指令都要访存——这正是教学上
 * 最有价值的那组对照（`project/mini-basic` 里的栈式 VM 也是这个模型）。
 *
 * ## 支持的指令子集
 *
 * | 四元式 | 说明 |
 * | --- | --- |
 * | `decl` | 变量类型与作用域（`global` / `local`；`local` 会在当前调用帧里开槽） |
 * | `:=` | 赋值 |
 * | `+ - * / %`、`& \| ^`、`<< >>`、`neg` | 算术 / 位运算（按 `:类型` 后缀取符号与回绕） |
 * | `== != < <= > >=` | 比较（有符号 / 无符号按后缀） |
 * | `mul` / `div`（4~5 个参数） | 双结果：`lo/hi`、`q/r` |
 * | `cvt:` | 整数 ↔ 浮点 |
 * | `label` / `j` / `jz` / `jnz` | 标签与跳转 |
 * | `read` / `getc` | 从输入取一项 / 取一个字符 |
 * | `write` / `print` / `putc` | 输出数值 / 字符串 / 字符 |
 * | `param` / `call` / `ret` | 调用与返回（局部变量按帧存放，所以能递归） |
 * | `alloc` / `free` | 堆：`(alloc, 字节数, _, 目标)` 拿到一个地址，`(free, 地址, _, _)` 还回去 |
 * | `check` | 下标陷阱：`(check, 下标, 上限, 标签)`，越界就终止运行 |
 * | `checkptr` | 按区块容量查下标：`(checkptr, 下标, 基址, 步长, 标签)`（堆块 / 数据区符号的 extent） |
 * | `call 名字` | 名字不在标签表里时先查**原生函数表**（数学 / 字符串 / 文件 / 进程等 `__vm_*`） |
 * | `halt` | 停机 |
 *
 * 不支持的（宽类型分片 `+c`/`-b`、中断 `int`/`iret`、数据区等）会在组装期报一条
 * 诊断并跳过——它们属于"机器字长模型"和"目标代码生成"的范畴，本库不重复实现。
 */

#ifndef PARSE_ENGINE_STACKVM_HPP
#define PARSE_ENGINE_STACKVM_HPP

#include "stackvm_glue.hpp"

/**
 * @def STACKVM_WITH_BIGINT
 * @brief **大整数（`bint`）是附加功能**：这个宏决定要不要把它编进虚拟机。
 *
 * * `1`（缺省）= 编进来：`__vm_bint_*` 与 `__vm_read_bint` 这批原生函数可用，
 *   本库需要 `library/big_number`（`bigint.hpp`）；
 * * `0` = 不编：不 include big_number、不链 bigint.o，`bint` 那批原生函数**不存在**
 *   —— 调它们会得到一句"这个构建没把大整数编进来"的运行期错误，而不是"函数入口不存在"。
 *
 * 命令行开关（Makefile 里已接）：
 *
 * ```sh
 * make -C library/stackvm STACKVM_WITH_BIGINT=0      # 不带大整数的轻量构建
 * ```
 *
 * 想在自己的宿主里看当前构建支不支持，用下面的 `kBigIntEnabled`（编译期常量，
 * `if constexpr` 用得）。
 */
#ifndef STACKVM_WITH_BIGINT
#define STACKVM_WITH_BIGINT 1
#endif

/**
 * @def STACKVM_WITH_BIGFLOAT
 * @brief 任意精度十进制浮点（`bfloat`）开关；缺省跟随大整数开关。
 *
 * BigFloat 的尾数由 BigInt 保存，因此不能在关闭 BigInt 时单独开启 BigFloat。
 */
#ifndef STACKVM_WITH_BIGFLOAT
#define STACKVM_WITH_BIGFLOAT STACKVM_WITH_BIGINT
#endif
#if STACKVM_WITH_BIGFLOAT && !STACKVM_WITH_BIGINT
#error "STACKVM_WITH_BIGFLOAT requires STACKVM_WITH_BIGINT"
#endif

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace stackvm
{

/** VM 内存的最小寻址/分配单元：1 字节。宽值连续占用多个单元。 */
inline constexpr std::size_t kRuntimeCellBytes = 1u;
/** 当前整数、浮点、地址和对象句柄的存储宽度。 */
inline constexpr std::size_t kScalarStorageBytes = 8u;

// ---------------------------------------------------------------------------
//  四元式（与 library/transfer 的文本格式兼容）
// ---------------------------------------------------------------------------

/**
 * @brief 一条四元式：`(op, arg1, arg2, result[, extra])`。
 */
struct Quad
{
    std::size_t line = 0;              ///< 源码行号（1 基，诊断用）。
    std::string op;                    ///< 操作符，可能带类型后缀（`+:i8`）。
    std::vector<std::string> args;     ///< 参数（`_` 表示空位）。

    /**
     * @brief 取下第 index 个参数；`_` 与不存在都算空。
     * @param index 下标。
     * @return 参数文本。
     */
    [[nodiscard]] std::string arg(std::size_t index) const;
};

/**
 * @brief 一个 `.quad` 文件解析出来的内容。
 */
struct QuadFile
{
    std::string path;                  ///< 文件路径。
    std::string name;                  ///< `# 名称:`。
    std::string expected;              ///< `# 期望:`（示例套件用来比对）。
    std::string expectedText;          ///< `# 期望文本:`。
    std::string input;                 ///< `# 输入:`（`read` / `getc` 的数据）。
    std::vector<Quad> quads;           ///< 四元式。
    std::vector<std::string> diagnostics; ///< 解析诊断。

    /**
     * @brief 是否解析成功。
     * @return 没有诊断返回 true。
     */
    [[nodiscard]] bool ok() const { return diagnostics.empty(); }
};

/**
 * @brief 解析 `.quad` 文本。
 * @param text 文本（UTF-8）。
 * @param label 诊断里显示的文件名。
 * @return 解析结果。
 */
[[nodiscard]] QuadFile parseQuads(std::string_view text, std::string label);

/**
 * @brief 读一个 `.quad` 文件。
 * @param path 路径。
 * @return 解析结果（打不开时诊断里给消息）。
 */
[[nodiscard]] QuadFile loadQuadFile(const std::string &path);

// ---------------------------------------------------------------------------
//  类型
// ---------------------------------------------------------------------------

/**
 * @brief 变量类型。
 */
struct TypeInfo
{
    std::string name;         ///< `i32` / `u8` / `f64` …
    int bits = 16;            ///< 位宽。
    bool isUnsigned = false;  ///< 有无符号。
    bool isFloat = false;     ///< 是否浮点。
    std::size_t size = 2;     ///< 字节数（本库只用于展示）。
};

/**
 * @brief 名字 → 类型；没写 `decl` 时按 `i16`（与 transfer 的旧语义一致）。
 * @param name 类型名；空串表示"没声明"。
 * @return 类型信息。
 */
[[nodiscard]] TypeInfo typeOf(std::string_view name);

// ---------------------------------------------------------------------------
//  构建能力
// ---------------------------------------------------------------------------

/// 这次构建有没有把大整数（`bint`）编进来（见 `STACKVM_WITH_BIGINT`）。
inline constexpr bool kBigIntEnabled = (STACKVM_WITH_BIGINT != 0);
/// 这次构建有没有把任意精度十进制浮点（`bfloat`）编进来。
inline constexpr bool kBigFloatEnabled = (STACKVM_WITH_BIGFLOAT != 0);

// ---------------------------------------------------------------------------
//  栈指令
// ---------------------------------------------------------------------------

/**
 * @brief 打印格式（由 `write` 的类型后缀或变量声明决定）。
 */
enum class PrintFormat
{
    Signed,   ///< 有符号整数。
    Unsigned, ///< 无符号整数。
    Float,    ///< 浮点。
    Text,     ///< 字符串（`print`）。
};

/**
 * @brief 栈指令操作码。
 */
enum class Op
{
    PushInt,   ///< 压入整数立即数。
    PushDouble,///< 压入浮点立即数。
    PushText,  ///< 压入字符串。
    Load,      ///< 把变量压栈（`name`）。
    Store,     ///< 弹栈写入变量（`name`）。
    Declare,   ///< 在当前调用帧里开一个变量槽（`name`；`(decl, T, local, x)` 用它）。
    Alloc,     ///< 堆分配：弹栈取字节数，把地址压回栈（失败就终止运行）。
    Free,      ///< 堆释放：弹栈取地址（`name` 是源操作数名，仅用于诊断）。
    Drop,      ///< **软释放**：地址是活跃堆块才还回去；0 / 数据区地址 / 已经释放过 → 什么都不做。
    Check,     ///< 下标越界陷阱：弹栈取下标与上限，越界就终止（`name` 是给诊断看的标签）。
    CheckPointer, ///< 按区块容量查下标：弹栈取下标与基址，`intValue` 是元素步长。
    CallIndirect, ///< 间接调用：弹栈取"函数指针"（数据区里一串标签字节的地址），再按它调用。
    Neg,       ///< 取负。
    Not,       ///< 按位取反。
    Add, Sub, Mul, Div, Mod,      ///< 算术。
    Shl, Shr, ShrU,               ///< 移位（算术 / 逻辑右移）。
    BitAnd, BitOr, BitXor,        ///< 位运算。
    Eq, Ne, Lt, Le, Gt, Ge,       ///< 有符号比较。
    LtU, LeU, GtU, GeU,           ///< 无符号比较。
    ToDouble, ToInt,              ///< 类型转换。
    MulWide,                      ///< 双结果乘法：压入 lo，再压 hi。
    DivWide,                      ///< 双结果除法：压入 q，再压 r。
    Jmp, Jz, Jnz,                 ///< 跳转（`name` 是标签）。
    Print,                        ///< 打印栈顶（`format`）。
    PrintText,                    ///< 打印字符串。
    Read,                         ///< 读一项到变量（`name`）。
    Getc,                         ///< 读一个字符到变量（`name`）。
    Putc,                         ///< 输出栈顶字符。
    Param,                        ///< 记一个实参（`name` 是形参名）。
    Call,                         ///< 调用（`name` 是标签，`name2` 是返回值目的地）。
    Ret,                          ///< 返回：弹栈顶作为返回值。
    Halt,                         ///< 停机。
    AddrOf,                       ///< 把数据区符号的基址压栈（`name` 是符号名）。
    LoadAt,                       ///< 弹地址，读 8 字节（小端）压栈——数据区取值。
    StoreAt,                      ///< 弹值、再弹地址，往数据区写 8 字节（小端）。
};

/**
 * @brief 一条栈指令。
 */
struct Instruction
{
    Op op = Op::Halt;             ///< 操作码。
    std::string name;             ///< 变量名 / 标签 / 形参名。
    std::string name2;            ///< `Call` 的返回值目的地。
    std::int64_t intValue = 0;    ///< 立即数（整数）；`MulWide` / `DivWide` 时是位宽。
    double doubleValue = 0.0;     ///< 立即数（浮点）。
    std::string textValue;        ///< 立即数（字符串）。
    bool unsignedOp = false;      ///< `MulWide` / `DivWide` 是否按无符号做。
    bool isFloat = false;         ///< `LoadAt` / `StoreAt`：这 8 个字节按 IEEE-754 双精度解释。
    PrintFormat format = PrintFormat::Signed; ///< 打印格式。
    std::size_t quadIndex = 0;    ///< 来自第几条四元式（从 0 开始）。

    /**
     * @brief 反汇编成一行文本。
     * @return 文本。
     */
    [[nodiscard]] std::string toString() const;
};

/**
 * @brief 一条指令对应的源码位置（调试信息）。
 *
 * `file` / `line` 指向**语言源码**，`quadLine` 指向四元式文本的行。前端现在只填得出后者
 * （四元式行），所以两个都留着：谁有位置就填谁，`toString()` 按"越接近源码越优先"挑一个显示。
 */
struct SourceLocation
{
    std::string function;     ///< 所属函数 / 标签；空表示根或未知。
    std::string file;         ///< 源文件名（`.mgl` 或 `.quad`）；空表示不知道。
    std::int32_t line = 0;    ///< 源文件行号（1 基）；0 = 未知。
    std::size_t quadLine = 0; ///< 四元式文本行号（1 基）；0 = 未知。

    /** 有没有任何可展示的位置信息。 */
    [[nodiscard]] bool known() const { return line > 0 || quadLine > 0 || !function.empty(); }

    /**
     * @brief 位置文本：优先 `file:line`，退而 `.quad:行`（四元式文本行），再退到函数名。
     * @return 短文本；什么都没有时返回 `?`。
     */
    [[nodiscard]] std::string toString() const;
};

/**
 * @brief 稀疏的「指令下标 → 源码位置」表。
 *
 * 不必每条指令都登记：查询取**不超过目标下标的最近一条**，所以一条语句里的几十条指令
 * 共用一条记录就够了（函数入口、语句起点各一条即可）。
 */
struct DebugInfo
{
    /** 指令下标（递增）与对应位置，一一对应。 */
    std::vector<std::pair<std::size_t, SourceLocation>> entries;

    /** 条数。 */
    [[nodiscard]] std::size_t size() const { return entries.size(); }
    /** 是否为空。 */
    [[nodiscard]] bool empty() const { return entries.empty(); }

    /**
     * @brief 追加一条记录；与上一条位置完全相同则跳过（省地方）。
     * @param instruction 指令下标；必须不小于已有的最后一条。
     * @param location 位置。
     */
    void add(std::size_t instruction, SourceLocation location);

    /**
     * @brief 取不超过 instruction 的最近一条记录。
     * @param instruction 指令下标。
     * @return 命中返回指针；表里没有更早的记录返回 nullptr。
     */
    [[nodiscard]] const SourceLocation *lookup(std::size_t instruction) const;
};

/**
 * @brief 组装好的栈程序。
 */
struct Program
{
    std::string name;                          ///< 名字（取 `# 名称:`）。
    std::vector<Instruction> code;             ///< 指令序列。
    std::map<std::string, std::size_t> labels; ///< 标签 → 指令下标。
    std::map<std::string, TypeInfo> types;     ///< 变量 → 类型。
    /**
     * 标签 → 形参名（按声明顺序）。
     *
     * 由 `assemble` 扫出来：**紧挨着 `(label …)` 之前那一段连续的 `(decl …)`**
     * 就是这个函数的形参（前端把形参声明排在函数标签前面）。函数指针的间接调用
     * 按"第几个实参"绑定，靠的就是这张表 —— 直接调用是从调用点写着的形参名绑的。
     */
    std::map<std::string, std::vector<std::string>> parameters;
    /** 可供二进制模块调用者链接的公开标签；裸汇编默认全公开，模块前端可收窄。 */
    std::set<std::string> exports;
    /**
     * 调试信息：指令下标 → 源码位置。组装时按"四元式行的来源"自动填，
     * 前端如果能给出 `.mgl` 的文件与行号，也会一起存进来。
     */
    DebugInfo debug;
    std::vector<std::string> diagnostics;      ///< 组装诊断（不支持的指令等）。

    /**
     * @brief 是否组装成功。
     * @return 没有诊断返回 true。
     */
    [[nodiscard]] bool ok() const { return diagnostics.empty(); }
};

/** stackvm 二进制程序文件的推荐后缀。 */
inline constexpr std::string_view kProgramBinaryExtension = ".svmb";

/**
 * @brief 判断文件是否带 stackvm 二进制程序魔数。
 * @param path 文件路径。
 * @return 是二进制程序返回 true；打不开或不是返回 false。
 */
[[nodiscard]] bool isProgramBinary(const std::string &path);

/**
 * @brief 把已经组装好的程序写成稳定的小端二进制格式。
 * @param program 程序。
 * @param path 输出路径。
 * @param diagnostics 失败诊断（可为空）。
 * @param encrypt 非 0 时头部 `flags` 标上 0x0001，载荷区换成"12 字节 nonce + ChaCha20 密文"；
 *                密钥来自编译期宏 `STACKVM_PAYLOAD_KEY`（见 `stackvm_crypto.hpp`）。
 * @return 成功返回 true。
 */
bool saveProgramBinary(const Program &program, const std::string &path,
                       std::vector<std::string> *diagnostics = nullptr, bool encrypt = false);

/**
 * @brief 从 stackvm 二进制文件直接恢复组装后的程序。
 * @param path 输入路径。
 * @param program 输出程序。
 * @param diagnostics 失败诊断（可为空）。
 * @return 格式完整且版本受支持时返回 true。
 */
bool loadProgramBinary(const std::string &path, Program &program,
                       std::vector<std::string> *diagnostics = nullptr);

/**
 * @brief 把四元式组装成栈指令。
 * @param file 四元式文件。
 * @param out 组装结果。
 * @return 成功返回 true（`out.diagnostics` 里给出不支持的原因）。
 */
bool assemble(const QuadFile &file, Program &out);

/**
 * @brief 化简统计。
 */
struct OptimizationReport
{
    std::size_t rounds = 0;              ///< 实际迭代了几轮。
    std::size_t constantsFolded = 0;     ///< 常量折叠了几处。
    std::size_t algebraicSimplified = 0; ///< 代数化简了几处（`x+0`、`x*1` …）。
    std::size_t deadBranches = 0;        ///< 常量条件的分支折掉了几处。
    std::size_t deadCode = 0;            ///< 删掉了几条不可达 / 无用四元式。
    std::vector<std::string> trace;      ///< 每处化简一行说明。

    /**
     * @brief 一共改了多少处。
     * @return 处数。
     */
    [[nodiscard]] std::size_t total() const
    {
        return constantsFolded + algebraicSimplified + deadBranches + deadCode;
    }
};

/**
 * @brief 化简四元式（**就地改写** `file.quads`）。
 *
 * 参考 `library/transfer/core/optimizer.cpp` 与 `project/pl0-super` 的做法，
 * 只做**保守、能一句话说清**的四趟：
 *
 * 1. **常量折叠**：`(+, 2, 3, t)` → `(:=, 5, _, t)`；比较、位运算、移位、
 *    `neg`、`cvt` 同样折叠；已知常量的变量会代入（`(:=, 2, _, x)` 之后的
 *    `(+, x, 3, t)` 也能折叠）；
 * 2. **代数化简**：`x+0`、`x-0`、`x*1`、`x/1`、`x<<0`、`x|0`、`x^0` → `x`；
 * 3. **死分支**：条件是常量的 `jz` / `jnz` 折成 `j` 或删掉；`j` / `halt` /
 *    `ret` 之后到下一个标签之间的代码是**不可达**的，删掉；紧跟 `j` 的标签也删 `j`；
 * 4. **死代码**：结果变量从头到尾没被读过（也没通过 `param` 传出去）的 `:=`
 *    可以删——它唯一的"作用"就是写一个没人看的槽位。
 *
 * 会清空常量表的时机：标签（控制流汇合点）、`call`（可能改全局）、以及任何不是
 * `:=` 字面量赋值的写入——这些都是"别猜"的位置。
 *
 * @param file 四元式文件。
 * @param maxRounds 最多迭代几轮（缺省 4）。
 * @return 化简统计。
 */
[[nodiscard]] OptimizationReport optimize(QuadFile &file, std::size_t maxRounds = 4u);

// ---------------------------------------------------------------------------
//  执行
// ---------------------------------------------------------------------------

/** 一次运行结束时保留的协程上下文摘要。id=0 是主上下文。 */
struct CoroutineInfo
{
    std::size_t id = 0;
    std::string state;
    std::size_t instruction = 0;
    std::size_t frameDepth = 0;
    std::size_t stackDepth = 0;
    std::size_t waitingFor = 0;
};

/** 指令钩子收到的只读执行快照。 */
struct InstructionHookInfo
{
    std::size_t steps = 0;       ///< 本次 run() 已完成的指令总数。
    std::size_t instruction = 0; ///< 刚执行完的程序计数器。
    std::size_t coroutine = 0;   ///< 执行该指令的协程句柄；0 是主上下文。
    std::string opcode;          ///< 指令操作码。
};

using TimeCallback = std::function<std::uint64_t()>;
using InstructionHook = std::function<void(const InstructionHookInfo &)>;

/** 调试回调请 VM 接下来怎么做。 */
enum class DebugAction
{
    Continue, ///< 继续跑到下一个断点（清掉单步状态）。
    StepInto, ///< 单步：下一条指令执行前再回调一次。
    Abort,    ///< 中止本次运行；结果里给出中止位置与源码级调用栈。
};

/** 调用栈里的一帧（调试用）。 */
struct DebugFrame
{
    std::string function;    ///< 函数 / 标签名；空表示根。
    SourceLocation location; ///< 该帧的位置：被调函数是"当前指令"，调用者记"调用点"。
};

/** 调试回调收到的只读快照。 */
struct DebugEvent
{
    std::size_t instruction = 0;    ///< **即将执行**的指令下标。
    std::size_t steps = 0;          ///< 本次 run 已经执行过的指令数。
    std::size_t coroutine = 0;      ///< 所属协程；0 是主上下文。
    std::string opcode;             ///< 指令操作码文本。
    SourceLocation location;        ///< 该指令的源码位置。
    std::vector<DebugFrame> frames; ///< 调用栈，从外到内；最后一帧是当前函数。
    bool breakpoint = false;        ///< 是断点停的（true）还是单步停的（false）。
};

using DebugHook = std::function<DebugAction(const DebugEvent &)>;

/**
 * @brief 执行结果。
 */
struct RunResult
{
    bool ok = false;                   ///< 是否正常停机。
    int exitCode = 0;                  ///< 正常停机状态码（`exit(n)`；普通 halt 为 0）。
    std::string output;                ///< `write` 的数值输出（多个值之间一个空格）。
    std::string text;                  ///< `print` / `putc` 的文本输出（按顺序拼接）。
    std::string error;                 ///< 运行时错误（越界、未定义变量等）。
    std::size_t steps = 0;             ///< 执行的指令条数。
    bool timingAvailable = false;       ///< 是否由宿主时钟提供了精确计时。
    std::uint64_t startedNanoseconds = 0; ///< 开始时的宿主单调时钟值。
    std::uint64_t finishedNanoseconds = 0; ///< 结束时的宿主单调时钟值。
    std::uint64_t elapsedNanoseconds = 0; ///< 本次执行耗时；时钟倒退时保守记 0。
    std::size_t maxStackDepth = 0;     ///< 求值栈的最大深度。
    std::size_t heapPeakBytes = 0;     ///< 堆上同时在用字节数的峰值。
    std::size_t heapLiveBytes = 0;     ///< 停机时还有多少字节没 `free`（≠ 0 就是漏了）。
    std::size_t heapLiveBlocks = 0;    ///< 停机时还挂着几块（配合 `heapLiveBytes` 看）。
    std::size_t liveObjects = 0;       ///< 停机时还有几个运行期对象（`bint` / 容器 / `obj` / 资源句柄）没释放。
    std::vector<CoroutineInfo> coroutines; ///< 主上下文及各协程的最终状态摘要。
    std::vector<std::string> trace;    ///< 打开 trace 时每条指令一行。
    SourceLocation failureLocation;    ///< 出错（或被调试器中止）处对应的源码位置。
    /**
     * 出错处的源码级调用栈，从外到内；最后一帧是出错函数。
     * 程序带调试信息（`Program::debug`）或挂了调试表时才填得出内容。
     */
    std::vector<DebugFrame> sourceTrace;
};

/**
 * @brief 可替换的 VM 字节堆。
 *
 * `Machine::run()` 每次执行前都会调用 `reset(base)`；实现必须清空上一轮状态，并把地址 0
 * 留给空指针。接口管理的是 MAGEL/四元式 `alloc`、字符串和按地址 load/store 使用的字节堆，
 * 不接管 C++ 调用帧、STL 容器或运行期对象表的宿主内存。
 */
class HeapAllocator
{
public:
    virtual ~HeapAllocator() = default;

    virtual void reset(std::size_t base) = 0;
    [[nodiscard]] virtual std::size_t base() const = 0;
    [[nodiscard]] virtual std::size_t allocate(std::size_t bytes) = 0;
    virtual bool release(std::size_t address, std::string *reason = nullptr) = 0;
    [[nodiscard]] virtual bool contains(std::size_t address) const = 0;
    [[nodiscard]] virtual bool capacityAt(std::size_t address,
                                          std::size_t &capacity) const = 0;
    [[nodiscard]] virtual std::size_t liveBytes() const = 0;
    [[nodiscard]] virtual std::size_t liveBlocks() const = 0;
    [[nodiscard]] virtual std::size_t peakBytes() const = 0;
    [[nodiscard]] virtual bool readBytes(std::size_t address, char *out,
                                         std::size_t count) const = 0;
    virtual bool writeBytes(std::size_t address, const char *data,
                            std::size_t count) = 0;

    [[nodiscard]] bool readInt(std::size_t address, std::int64_t &out) const;
    bool writeInt(std::size_t address, std::int64_t value);
    [[nodiscard]] bool readDouble(std::size_t address, double &out) const;
    bool writeDouble(std::size_t address, double value);
};

/** C++ 胶水函数与虚拟机交换的单槽值。字符串、指针和宿主句柄使用整数地址/句柄。 */
struct NativeValue
{
    enum class Kind { Integer, Floating, Text } kind = Kind::Integer;
    std::int64_t integer = 0;
    double floating = 0.0;
    std::string text;

    static NativeValue fromInt(std::int64_t value);
    static NativeValue fromFloat(double value);
    static NativeValue fromText(std::string value);
    [[nodiscard]] bool isFloat() const { return kind == Kind::Floating; }
    [[nodiscard]] bool isText() const { return kind == Kind::Text; }
    [[nodiscard]] std::int64_t asInt() const;
    [[nodiscard]] double asFloat() const;
};

/** 一次 C++ 胶水调用可访问的 VM 上下文；对象仅在回调期间有效。 */
class NativeCallContext
{
public:
    virtual ~NativeCallContext() = default;
    [[nodiscard]] virtual bool readBytes(std::int64_t address, char *out,
                                         std::size_t count) const = 0;
    virtual bool writeBytes(std::int64_t address, const char *data, std::size_t count) = 0;
    [[nodiscard]] virtual bool readString(std::int64_t address, std::string &out) const = 0;
    /** 把 VM `lang::List<T>` 句柄读取为无所有权的胶水值序列。 */
    [[nodiscard]] virtual bool readList(std::int64_t handle,
                                        std::vector<NativeValue> &out) const = 0;
    /** 在当前 VM 上同步调用一个 MAGEL 函数指针；堆、对象表与全局状态保持不变。 */
    virtual bool invoke(std::int64_t functionPointer,
                        const std::vector<NativeValue> &arguments,
                        NativeValue &result) = 0;
    [[nodiscard]] virtual std::int64_t makeString(const std::string &text) = 0;
    /** Temporary value storage, reclaimed at the end of run(). */
    virtual std::int64_t makeTemporary(std::size_t) { fail("temporary storage unsupported"); return 0; }
    virtual void fail(const std::string &message) = 0;
};

using NativeCallback =
    std::function<NativeValue(NativeCallContext &, const std::vector<NativeValue> &)>;

/** 宿主原生函数表；可同时挂到源码即时执行和加载后的 `.svmb` 程序。 */
class NativeRegistry
{
public:
    struct Entry
    {
        std::size_t arity = 0;
        NativeCallback callback;
    };

    bool add(const std::string &name, std::size_t arity, NativeCallback callback,
             std::string *error = nullptr);
    bool remove(const std::string &name);
    [[nodiscard]] const Entry *find(const std::string &name) const;

private:
    std::map<std::string, Entry> entries_;
};

/**
 * @brief 栈式虚拟机。
 */
/**
 * @brief 数据区：加载 `.data.bin` 与布局表（`simple_ast` 的 `exportDataLayout()` 输出）。
 *
 * 有了它，四元式里的 `(addr, 符号, _, t)` / `(load, 指针, _, 目标)` / `(store, 指针, 值, _)`
 * 就能真正读写数组 / 结构体所在的那片内存。
 */
struct DataArea
{
    std::vector<std::uint8_t> binary;           ///< 数据区原始字节（不是文本）。
    std::map<std::string, std::size_t> symbols; ///< 符号 → 基址（字节偏移）。
    std::map<std::string, std::size_t> strides; ///< 符号 → 槽位步长（布局行里的 `stride=`）。
    std::map<std::string, std::size_t> sizes;   ///< 符号 → 占多少字节（布局行里的 `size=`）。

    /**
     * @brief 加载二进制与布局表。
     * @param binaryPath `.data.bin` 路径。
     * @param layoutPath 布局文件：一行 `符号 offset=0 size=8 type=…`。
     * @param diagnostics 出错信息。
     * @return 成功返回 true。
     */
    bool load(const std::string &binaryPath, const std::string &layoutPath,
              std::vector<std::string> *diagnostics = nullptr);

    /** 数据区大小。 */
    [[nodiscard]] std::size_t size() const { return binary.size(); }

    /**
     * @brief 读一个以 `\0` 结尾的字符串。
     *
     * 函数指针就是"数据区里存着函数标签的那串字节的地址"，间接调用靠它拿标签。
     * @param address 地址（数据区内的偏移）。
     * @param limit 最多读多少字节（防越界）。
     * @return 字符串；地址越界返回空串。
     */
    [[nodiscard]] std::string readCString(std::size_t address, std::size_t limit = 256u) const;

    /**
     * @brief 读 8 字节小端。
     * @param address 地址（数据区内的偏移）。
     * @param out 结果。
     * @return 越界返回 false。
     */
    [[nodiscard]] bool readInt(std::size_t address, std::int64_t &out) const;

    /**
     * @brief 写 8 字节小端。
     * @param address 地址。
     * @param value 值。
     * @return 越界返回 false。
     */
    bool writeInt(std::size_t address, std::int64_t value);

    /**
     * @brief 读 8 字节小端，按 IEEE-754 双精度解释（`(load:f64 …)` 用它）。
     * @param address 地址。
     * @param out 结果。
     * @return 越界返回 false。
     */
    [[nodiscard]] bool readDouble(std::size_t address, double &out) const;

    /**
     * @brief 把双精度写成 8 字节小端（`(store:f64 …)` 用它）。
     * @param address 地址。
     * @param value 值。
     * @return 越界返回 false。
     */
    bool writeDouble(std::size_t address, double value);

    /**
     * @brief 按字节读一段（字符串是"以 `\0` 结尾的字节串"，跨 8 字节槽）。
     * @param address 地址（数据区内的偏移）。
     * @param out 输出缓冲（至少 count 字节）。
     * @param count 字节数。
     * @return 越界返回 false。
     */
    [[nodiscard]] bool readBytes(std::size_t address, char *out, std::size_t count) const;

    /**
     * @brief 按字节写一段。
     * @param address 地址。
     * @param in 输入缓冲（至少 count 字节）。
     * @param count 字节数。
     * @return 越界返回 false。
     */
    bool writeBytes(std::size_t address, const char *in, std::size_t count);
};

class Machine
{
public:
    /**
     * @brief 构造。
     * @param trace 是否记录执行轨迹。
     */
    explicit Machine(bool trace = false) : trace_(trace) {}

    /**
     * @brief 挂上数据区；不挂时 `AddrOf` / `LoadAt` / `StoreAt` 会报"没有数据区"。
     * @param data 数据区（生命周期由调用方保证）。
     */
    void setData(DataArea *data) { data_ = data; }

    /** 挂上宿主原生函数表；注册表生命周期须覆盖 run()。传 nullptr 可取消。 */
    void setNativeRegistry(const NativeRegistry *registry) { nativeRegistry_ = registry; }

    /**
     * @brief 挂上自定义 VM 字节堆；分配器生命周期须覆盖 run()。传 nullptr 恢复默认实现。
     *
     * 同一对象可连续执行多个程序；每次 run() 都会先调用分配器的 reset(base)。
     */
    void setHeapAllocator(HeapAllocator *allocator) { heapAllocator_ = allocator; }

    /**
     * 设置每个协程的求值栈槽数上限；0 表示不限制。
     * 超限会让 run() 返回“求值栈超过上限”诊断，不会抛出宿主异常。
     */
    void setStackLimit(std::size_t slots) { stackLimit_ = slots; }

    /** 当前每协程求值栈槽数上限；0 表示不限制。 */
    [[nodiscard]] std::size_t stackLimit() const { return stackLimit_; }

    /** 挂上文件系统接口；传 nullptr 使用桌面端默认实现。 */
    void setFileApi(FileApi *api) { fileApi_ = api; }

    /** 设置本次程序入口可见的命令行参数。内容会在 run() 期间复制进 VM 的 List。 */
    void setProgramArguments(std::vector<std::string> arguments)
    {
        programArguments_ = std::move(arguments);
    }

    /** 注入返回单调纳秒值的宿主时钟；空回调禁用精确计时。 */
    void setTimeCallback(TimeCallback callback) { timeCallback_ = std::move(callback); }

    /**
     * 每完成 interval 条指令调用一次钩子。interval=0 或空钩子表示禁用。
     * 回调在 VM 执行线程同步发生，不应在回调里递归调用同一个 Machine::run()。
     */
    void setInstructionHook(std::size_t interval, InstructionHook hook)
    {
        hookInterval_ = interval;
        instructionHook_ = std::move(hook);
    }

    /**
     * @brief 挂上调试回调；空回调表示不调试。
     *
     * 回调在**每条被停下的指令执行之前**同步发生（断点命中或单步模式），返回的动作决定
     * 下一步：继续、单步、或中止。回调里可以读 `currentLocation()` / `callStack()` 看现场，
     * 但**不要**递归调用同一个 Machine::run()。
     */
    void setDebugHook(DebugHook hook) { debugHook_ = std::move(hook); }

    /**
     * @brief 设置断点集合（指令下标）。
     * @param instructions 断点位置；重复项会被忽略，空表等于清空。
     */
    void setBreakpoints(std::vector<std::size_t> instructions);

    /** 清空所有断点。 */
    void clearBreakpoints() { breakpoints_.clear(); }

    /** 当前断点个数。 */
    [[nodiscard]] std::size_t breakpointCount() const { return breakpoints_.size(); }

    /** 开关单步：开启后每条指令执行前都触发一次调试回调。 */
    void setSingleStep(bool enabled) { singleStep_ = enabled; }

    /** 单步是否开启。 */
    [[nodiscard]] bool singleStep() const { return singleStep_; }

    /**
     * @brief 临时指定调试表，优先于 `program.debug`；传 nullptr 恢复用程序自带的那张。
     *
     * 表必须活到本次 run() 结束。
     */
    void attachDebugInfo(const DebugInfo *info) { debugOverride_ = info; }

    /** 本次运行生效的调试表；没挂也没带就是 nullptr。 */
    [[nodiscard]] const DebugInfo *debugInfo() const { return debugInfo_; }

    /** 最近一次调试回调 / 停机时刻的源码位置。 */
    [[nodiscard]] SourceLocation currentLocation() const { return currentLocation_; }

    /** 最近一次调试回调 / 停机时刻的源码级调用栈（从外到内）。 */
    [[nodiscard]] std::vector<DebugFrame> callStack() const { return debugFrames_; }

    /**
     * @brief 执行一个组装好的程序。
     * @param program 程序。
     * @param input `read` / `getc` 的输入项（按顺序取）。
     * @param stepLimit 指令条数上限，防止死循环；传 `0` 表示不设上限（缺省为 0）。
     * @return 执行结果。
     */
    [[nodiscard]] RunResult run(const Program &program,
                                const std::vector<std::string> &input = {},
                                std::size_t stepLimit = 0u);

private:
    bool trace_ = false; ///< 是否记录轨迹。
    DataArea *data_ = nullptr; ///< 可选的数据区。
    const NativeRegistry *nativeRegistry_ = nullptr; ///< 可选的 C++ 胶水函数表。
    HeapAllocator *heapAllocator_ = nullptr; ///< 可选的自定义 VM 字节堆。
    std::size_t stackLimit_ = 0u; ///< 每个协程的求值栈槽数上限；0 表示不限制。
    FileApi *fileApi_ = nullptr; ///< 可选的嵌入式文件系统接口。
    std::vector<std::string> programArguments_; ///< 传给语言入口的命令行参数。
    TimeCallback timeCallback_; ///< 可选的宿主单调纳秒时钟。
    std::size_t hookInterval_ = 0; ///< 指令钩子间隔，0 表示禁用。
    InstructionHook instructionHook_; ///< 可选的同步指令钩子。
    DebugHook debugHook_;                 ///< 可选的调试回调。
    std::set<std::size_t> breakpoints_;   ///< 断点：指令下标集合。
    bool singleStep_ = false;             ///< 单步开关。
    const DebugInfo *debugOverride_ = nullptr; ///< 调用方临时指定的调试表。
    const DebugInfo *debugInfo_ = nullptr;     ///< 本次运行生效的调试表。
    SourceLocation currentLocation_;      ///< 最近一次回调 / 停机时刻的位置。
    std::vector<DebugFrame> debugFrames_; ///< 最近一次回调 / 停机时刻的调用栈快照。
};

} // namespace stackvm

#endif // PARSE_ENGINE_STACKVM_HPP
