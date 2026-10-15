#pragma once
// 寄存器分配（教学库）：活跃变量 → 干涉图 → 图着色 / 线性扫描 → 溢出重写。
//
// 输入是一段带标签的虚拟寄存器程序（每行一条指令），输出是
//   ① 虚拟寄存器 → 物理寄存器 的分配表；
//   ② 需要溢出时重写后的程序（插入 load / store）；
//   ③ 对比指标（峰值活跃数、干涉边数、溢出数、指令与访存数变化）。
//
// 只用 C++17 标准库。
#include "typed_ir.hpp"

#include <cstddef>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace regalloc {

/**
 * 指令类型：全库共用 ir/typed_ir.hpp 里的带位数三地址指令。
 */
using Instruction = t1t::TypedInstruction;


/**
 * 目标机描述：物理寄存器、被调用者保存一览、溢出槽命名前缀。
 */
struct Machine {
    std::vector<std::string> registers;      // 参与分配的物理寄存器（顺序即编号）
    std::set<std::string> calleeSaved;       // 被调用者保存（跨 call 存活时要保存/恢复）
    std::string spillPrefix = "spill";       // 溢出槽名：spill.<vreg>
    /** 物理寄存器个数（也就是着色用的颜色数 K）。 */
    std::size_t colors() const { return registers.size(); }
    /**
     * 内建的教学机：A/B/C/D 四个通用寄存器，A、B 被调用者保存。
     */
    static Machine teaching(std::size_t count = 4);
};


/**
 * 基本块：全库共用 ir/typed_ir.hpp 里的 t1t::Block（它多带一个 predecessors）。
 */
using Block = t1t::Block;

/**
 * 一段程序 = 基本块序列（下标即编号，0 是入口）。
 */
using Program = std::vector<Block>;

/**
 * 解析：`label L`、`jmp L`、`jz v, L`、`call f`、`load d, [x]`、`store [x], s` 等。
 */
bool parse(const std::string& text, Program& out, std::vector<std::string>& errors);
/** 按同样格式写回。 */
std::string print(const Program& program);

/**
 * 指令里出现的所有虚拟寄存器（定义 + 使用），按名字排序。
 */
std::vector<std::string> virtualRegisters(const Program& program);

struct Liveness {
    /**
     * 每个块的 liveIn / liveOut（按块下标）。
     */
    std::vector<std::set<std::string>> liveIn;
    std::vector<std::set<std::string>> liveOut;
    /**
     * 峰值活跃变量数（逐指令计算 live 集合的最大值）。
     */
    std::size_t peakLive = 0;
};
/**
 * 反向数据流：LIVE.in = USE ∪ (LIVE.out − DEF)。
 */
Liveness liveness(const Program& program);

/** 干涉图：邻接表（只统计互相干涉的虚拟寄存器）。 */
struct Interference {
    std::map<std::string, std::set<std::string>> edges;
    /** 度（邻居个数）。 */
    std::size_t degree(const std::string& name) const;
/**
     * 干涉图的边数（无向，每条边只数一次）。。
     *
     * @return 边数
     */
    std::size_t edgeCount() const;
};
/**
 * 构造干涉图：逐块反向扫描，定义与"此刻活跃且不是自己"的变量干涉；
 * `mov` 的"定义 ↔ 源"例外（不给它们加边，留给合并）。
 */
Interference buildInterference(const Program& program, const Liveness& live);

/**
 * 溢出的代价估计：出现次数越多越不该溢出（默认每次 1）。
 */
std::map<std::string, double> spillCosts(const Program& program);

struct Allocation {
    std::map<std::string, std::string> assignment;  // 虚拟 → 物理
    std::set<std::string> spilled;                  // 溢出到栈的虚拟寄存器
    std::size_t colorsUsed = 0;                     // 实际用到的物理寄存器数
    std::size_t coalescedMoves = 0;                 // 被合并掉的 mov 条数
    std::string algorithm;                          // "chaitin" / "linear"
};

struct Options {
    std::size_t maxSpillRounds = 8;   // 最多重写几轮（每轮可能再溢出）
    bool coalesce = true;             // 是否做保守合并（只对图着色有意义）
    std::size_t linearScanSpillLimit = 1; // 线性扫描的溢出代价阈值
};

/**
 * 图着色（Chaitin-Briggs：简化 / 保守合并 / 选择 / 溢出）。
 */
Allocation colorByGraph(const Program& program, const Machine& machine,
                        const Options& options = {});
/**
 * 线性扫描（Poletto-Sarkar：按活跃区间起点排序 + 活跃表）。
 */
Allocation linearScan(const Program& program, const Machine& machine,
                      const Options& options = {});

struct Rewrite {
    Program program;                       // 溢出重写后的程序
    std::size_t insertedLoads = 0;
    std::size_t insertedStores = 0;
    std::size_t spillSlots = 0;
    std::size_t eliminatedMoves = 0;       // 两端落在同一物理寄存器的 mov，整条删掉
};
/**
 * 按分配结果重写程序：把分配的虚拟寄存器换成物理寄存器；被溢出的虚拟寄存器
 * 在定义后写回、每次使用前读回（用 scratch 寄存器，默认取机器里第一个
 * 不参与分配的寄存器名 `SCRATCH`）；两端映射到同一物理寄存器的 `mov` 是
 * 冗余复制，直接删除（合并之后 `mov` 就"消失"了，这正是合并的意义）。
 */
Rewrite rewrite(const Program& program, const Allocation& allocation,
                const Machine& machine, const std::string& scratch = "SCRATCH");

/**
 * 一次完整的分配：解析 → 分配 → 重写（必要时多轮）；返回可读报告。
 */
struct Stats {
    std::size_t virtualRegisters = 0;
    std::size_t peakLive = 0;
    std::size_t interferenceEdges = 0;
    std::size_t colors = 0;
    std::size_t colorsUsed = 0;
    std::size_t spilledRegisters = 0;
    std::size_t spillLoads = 0;
    std::size_t spillStores = 0;
    std::size_t spillSlots = 0;
    std::size_t eliminatedMoves = 0;
    std::size_t instructionsBefore = 0;
    std::size_t instructionsAfter = 0;
    std::size_t memoryOpsBefore = 0;
    std::size_t memoryOpsAfter = 0;
    std::size_t coalescedMoves = 0;
    std::size_t rounds = 0;
    std::string algorithm;
};

struct Result {
    Allocation allocation;
    Rewrite rewritten;
    Stats stats;
    std::vector<std::string> notes;
};

/**
 * 端到端：分配 + 溢出重写（迭代到不再溢出或达到轮数上限）。
 */
Result allocate(const Program& program, const Machine& machine, const std::string& algorithm,
                const Options& options = {});
/** 指标表格。 */
std::string report(const Result& result, const Machine& machine);

} // namespace regalloc