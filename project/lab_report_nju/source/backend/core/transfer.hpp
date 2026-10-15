#pragma once
#include "types.hpp"
#include <cstdint>
#include <initializer_list>
#include <istream>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace transfer {

/**
 * 四元式 `(op, arg1, arg2, result)` 或 `(op, arg1, arg2, result, extra)`；
 * 无意义字段统一写 `_`。`extra` 只用于双结果指令（`mul` 的高半、`div` 的余数）。
 *
 * 操作类别：
 * - 声明：`decl`（`(decl, i32, global, x)` 声明变量类型，见 types.hpp）；
 * - 算术 / 关系：`+ - * / %`、`== != < <= > >=`；
 * - 进位算术：`+c`（进位加）、`-b`（借位减），进位/借位在状态寄存器里串起来；
 * - 双寄存器乘除：`mul`（`(mul, a, b, lo, hi)`）、`div`（`(div, a, b, q, r)`）；
 * - 移位 / 按位：`<< >> & | ^` 以及一元 `~`（16 位宽度，右移为算术右移，
 *   移位位数按 16 取模）；
 * - 一元 / 赋值 / 转换：`neg`、`odd`、`cvt`、`:=`；
 * - 控制：`label`、`j`、`jz`、`jnz`、`halt`；
 * - 调用与中断：`param`、`call`、`ret`、`int`、`iret`；
 * - I/O：`read`、`write`、`print`、`putc`、`getc`；
 * - 流：`stream`（取得流句柄）、`sread`、`swrite`、`sclose`（STDIN/STDOUT/…）；
 * - 字符串：`scopy`、`slen`、`scmp`、`scat`、`ssub`（地址+长度）、`sfind`；
 * - 向量：`vload`、`vstore`、`vcopy`、`vfill`、`vsum`、`vmax`、`vmin`
 *   （`v*` 的第二个操作数是下标或长度，元素位数由类型后缀给出）。
 * - 文件：`fopen`、`fread`、`fwrite`、`fclose`、`feof`（真实的外部文件 IO）；
 * - 系统调用：`system`（exit / time / ticks / getpid / rand / brk / putchar / getchar）。
 * - 数据区：`addr`、`load`、`store`（把数据区里的符号基址取到变量里、
 *   按地址读 / 写 8 字节小端；与 `library/stackvm` 的三条同名栈指令同义）。
 * 字符串类型是 `str`（字面量写 `"..."`）；字符串按字节计数与比较。
 *
 * 运算符都可以写类型后缀，如 `+:u32`、`*:f64`、`<<:i8`；不写后缀时按
 * 「结果变量类型 -> 第一个操作数类型 -> i16」推断。见 types.hpp 的类型表。
 */
struct Quad {
    std::string op;
    std::string a;
    std::string b;
    std::string result;
    /**
     * 双结果指令的第二目标（`mul` 的高半、`div` 的余数）；其他指令为空。
     */
    std::string extra;
};

using Program = std::vector<Quad>;
enum class Target { Mips, I8086, Arm };
/**
 * 目标机描述。配置文件采用简单的 `key=value` 文本格式，故不需要 JSON
 * 库。例如 `word_bits=16`、`reg_a=$t0`、`cmd.add=ADDU {dst}, {lhs}, {rhs}`。
 * 花括号占位符会在生成阶段替换为寄存器或操作数。未配置的字段使用内建
 * MIPS/8086/ARM 默认值。配置对象是只读值，适合在线程间共享。
 */
struct MachineConfig {
    Target target = Target::Mips;
    /**
     * 机器**原生字长**（8 / 16 / 32 / 64），不是变量类型宽度。后端据此决定
     * 一条运算能不能用单条原生指令：类型宽度等于字长时直接生成；更窄时先按
     * 原生字长算完再归一化（掩码或算术移位扩展）；更宽时展开成多字序列或用
     * 运行时例程。默认 16 位，与旧版教学机一致。
     */
    unsigned wordBits = 16;
    std::string regA;
    std::string regB;
    std::string regC;
    std::string regHi;
    std::string regLo;
    std::string memoryLoad, memoryStore, immediateLoad;
    std::map<std::string, std::string> commands;
    /**
     * 用户提供的静态数据区：符号名 -> 字面量文本（整数或浮点，按变量的
     * 声明类型解释；未声明时按 i16）。保留文本是为了支持 u64/i128/f64
     * 这类超出 int32 的初值。
     */
    std::map<std::string, std::string> data;
    /**
     * 只读静态变量区；目标代码只能读取，不能被 :=/read 改写。
     */
    std::map<std::string, std::string> readOnlyData;
    /**
     * 外部 Bin 的原始字节，按文件顺序放入 .data 段。
     */
    std::vector<std::uint8_t> dataImage;
    /**
     * 可选 Bin 路径；loadConfigFile 会自动读取，loadConfig(stream) 只保留路径。
     */
    std::string dataBinPath;
    /**
     * 数据区字节镜像（CLI 的 `--data=<bin>`，即 `simple_ast` 生成的 `.data.bin`）。
     * 与 `dataAreaSymbols` 一起支撑 `addr` / `load` / `store` 三件套：地址就是
     * 这里的**字节偏移**，读写都是 8 字节小端，与 `library/stackvm` 的 `DataArea`
     * 完全一致。三个字段都为空时三件套在生成期合法、在模拟期报"没有加载数据区"。
     */
    std::vector<std::uint8_t> dataAreaBytes;
    /**
     * 数据区符号表：符号名 -> 基址（CLI 的 `--layout=<file>`，即 `simple_ast`
     * 的 `.layout`）。`(addr, 符号, _, 目标)` 取的就是这张表里的偏移。
     */
    std::map<std::string, std::size_t> dataAreaSymbols;
    /**
     * 模拟栈容量（以 word_bits 为单位）；生成器写入 .stack 段。
     */
    std::size_t stackWords = 1024;
    /**
     * 这台机器**实现了哪些指令族**：`carry`（进位加/借位减）、`shift`、
     * `bitwise`、`mul`、`div`、`float`、`string`、`vector`、`stream`、`file`、
     * `system`、`data`（数据区三件套 `addr`/`load`/`store`）。缺省全开；
     * 配置里用 `features=a,b,c` 替换，或用
     * `feature.string=off` 单独关掉。四元式合法但机器没有对应指令时，
     * 生成阶段报告"指令存在、机器不存在"的错误。
     */
    std::set<std::string> features;
    /** 这台机器是否实现了某个指令族。 */
    bool supports(const std::string& feature) const { return features.count(feature) != 0; }
};
/**
 * 模拟器结果；memory 同时包含 .data、.rodata 和运行时变量（带类型）。
 */
struct Execution {
    std::map<std::string, Value> memory;
    /**
     * `write` 输出：保留类型，便于按目标类型打印。
     */
    std::vector<Value> output;
    /** `print` / `putc` 的文本输出。 */
    std::string text;
    /**
     * 其他流（非 STDOUT）的数值输出：流名 -> 值序列。
     */
    std::map<std::string, std::vector<Value>> streams;
    /**
     * 其他流（非 STDOUT）的文本输出：流名 -> 文本。
     */
    std::map<std::string, std::string> streamText;
    /** `system exit` 的退出码。 */
    int exitCode = 0;
    std::size_t steps = 0;
};
/** CFG 中的一个最大基本块及其数据流摘要。 */
struct Block {
    /**
     * 基本块采用半开区间 [begin,end)，避免删除指令时产生歧义。
     */
    std::size_t begin = 0;
    std::size_t end = 0; // 指令区间为 [begin, end)
    std::vector<std::size_t> successors;
    std::vector<std::size_t> predecessors;
    bool reachable = false;
    /** 到达定值集合：元素是定义所在的四元式下标。 */
    std::set<std::size_t> reachingIn, reachingOut;
    /**
     * 活跃变量集合：变量在块入口/出口之后仍可能被读取。
     */
    std::set<std::string> liveIn, liveOut;
    /**
     * 可用表达式集合：表达式已计算且操作数未被重新定义。
     */
    std::set<std::string> availableIn, availableOut;
};
struct Analysis {
    std::vector<Block> blocks;
    std::vector<std::size_t> instructionBlock;
};

/** 优化后的程序、每轮变换说明和实际迭代轮数。 */
struct Optimization {
    Program program;
    std::vector<std::string> trace;
    std::size_t rounds = 0;
};
/**
 * 未声明类型的名字按 i16 处理；所有整数类型都按位宽取模回绕。
 */
/**
 * 从文本读取四元式；字段可用逗号或空白分隔，支持 `12: (op,a,b,r)`。
 */
Program read(std::istream& input);
/**
 * 将四元式按稳定的带下标格式写回文本，便于审查优化前后差异。
 */
std::string write(const Program& program);
/**
 * 检查操作数、标签、跳转目标和操作类别，发现错误抛出 std::runtime_error。
 */
void validate(const Program& program);
/**
 * 收集 `decl` 声明的变量类型；未声明的名字不在表里（按 i16 处理）。
 */
std::map<std::string, std::string> variableTypes(const Program& program);
/**
 * 求一条四元式的运算类型：显式后缀优先，其次结果变量、第一个操作数的声明
 * 类型，最后回落到 i16。
 */
std::string quadType(const Quad& quad, const std::map<std::string, std::string>& declared);
/**
 * 优化快捷接口，等价于 optimizeDetailed(program).program。
 */
Program optimize(const Program& program);
/**
 * 划分基本块、构造 CFG，并计算 RD/LIVE/AE 三种数据流结果。
 */
Analysis analyze(const Program& program);
/** 输出 CFG 边和三种数据流集合的可读报告。 */
std::string report(const Program& program, const Analysis& analysis);
/**
 * 迭代局部优化、全局优化、DCE 和循环优化直到不再改变或达到上限。
 */
Optimization optimizeDetailed(const Program& program, std::size_t maxRounds = 100);
/**
 * 并行优化互不相关的程序；每个任务拥有独立 CFG、数据流集合和临时名。
 */
std::vector<Optimization> optimizeParallel(const std::vector<Program>& programs,
                                           std::size_t workerCount = 0);
/** 使用内建目标机描述生成基础汇编。 */
std::string generate(const Program& program, Target target);
/**
 * 使用外部可配置的寄存器、段和指令模板生成基础汇编。
 */
std::string generate(const Program& program, const MachineConfig& config);
/** 返回目标机的默认寄存器和指令模板。 */
MachineConfig defaultConfig(Target target);
/** 从 key=value 文本读取机器配置。 */
MachineConfig loadConfig(std::istream& input, Target fallback = Target::Mips);
/**
 * 从配置文件读取机器配置，并按 data_bin 自动装载 Bin 数据。
 */
MachineConfig loadConfigFile(const std::string& path,
                             Target fallback = Target::Mips);
/**
 * 读取原始 Bin 文件；字节顺序保持不变，不附加文件头。
 */
std::vector<std::uint8_t> loadDataBin(const std::string& path);
/**
 * 读取数据区布局表（`simple_ast` 的 `exportDataLayout()` 输出，一行一个符号：
 * `符号 offset=0 size=32 stride=8 type=…`），返回 符号名 -> 基址。
 * 与 `read` 一样，遇到读不了的文件直接抛异常。
 */
std::map<std::string, std::size_t> loadDataLayout(const std::string& path);
/**
 * 装载数据区三件套需要的两份数据：`binPath` 是字节镜像（`.data.bin`），
 * `layoutPath` 是布局表（`.layout`，留空时按 `<binPath>.layout` 找）。
 * 结果写进 `config.dataAreaBytes` / `config.dataAreaSymbols`。
 */
void loadDataArea(MachineConfig& config, const std::string& binPath,
                  const std::string& layoutPath = std::string());
/**
 * 执行生成的汇编文本，而不是直接解释原始四元式。
 * `input` 是 `read` 依次取用的字面量文本（整数或浮点，按 read 的类型解析）；
 * 直接写 `{6}` 这种整数列表也可以，见下面的重载。
 */
Execution simulate(const std::string& assembly,
                   const std::vector<std::string>& input = {},
                   std::size_t stepLimit = 1000000);
/**
 * 带命名流的模拟：`streams` 是流名（STDIN / STDOUT / STDERR / 自定义）到
 * 输入项的映射；没有给出 STDIN 时用空输入。输出按流名放在 `Execution.streams`
 * （数值）与 `Execution.streamText`（文本）里，STDOUT 同时是 `output` / `text`。
 */
Execution simulate(const std::string& assembly,
                   const std::map<std::string, std::vector<std::string>>& streams,
                   std::size_t stepLimit = 1000000);
/**
 * 整数输入列表的便捷重载，等价于把每个整数转成文本后调用上面的版本。
 */
Execution simulate(const std::string& assembly, std::initializer_list<std::int32_t> input,
                   std::size_t stepLimit = 1000000);
/**
 * 将 mips、8086、arm 转成目标枚举；未知名称抛出异常。
 */
Target target(const std::string& name);
/** 返回目标枚举的稳定短名称。 */
const char* name(Target value);
} // namespace transfer
