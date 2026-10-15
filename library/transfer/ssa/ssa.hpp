#pragma once
// SSA 与稀疏优化（教学库）：
//
//   CFG → 支配树 / 支配边界 → φ 插入 → 变量改名  ⇒ SSA 形式
//   SSA → 稀疏条件常量传播（SCCP）→ 全局值编号（GVN）
//   SSA → φ 消除（切临界边 + 并行复制排序）        ⇒ 回到可执行形式
//
// 变量是"虚拟寄存器"（普通名字），内存符号写成 `[x]`：本库**不做内存 SSA**，
// 所以 `store` / `load` 走的是原样保留的地址，别名分析不在范围内。
// 全程只用 C++17 标准库。
#include "typed_ir.hpp"

#include <cstddef>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace ssa {

/**
 * 指令类型：全库共用 ir/typed_ir.hpp 里的带位数三地址指令。
 */
using Instruction = t1t::TypedInstruction;



/**
 * 基本块：全库共用 ir/typed_ir.hpp 里的 t1t::Block（下标即块号，0 是入口）。
 */
using Block = t1t::Block;

using Program = std::vector<Block>;

/**
 * 解析输入程序：`label L` / `mov d, s` / `add|sub|mul|div d, s1, s2` /
 * `load d, [x]` / `store [x], s` / `jmp L` / `jz v, L` / `jnz v, L` /
 * `print v` / `halt`；`;`、`#`、`//` 起注释。
 * 解析成功后 CFG 边（后继 / 前驱）已经连好。
 */
bool parse(const std::string& text, Program& out, std::vector<std::string>& errors);

/**
 * 按同样格式写回（`phi` 后面附一条指向前驱标号的注释）。
 */
std::string print(const Program& program);

/** 反序后序编号与可达性。 */
struct Cfg {
    static constexpr std::size_t kNone = static_cast<std::size_t>(-1);
    std::vector<std::size_t> rpo;        // 反序后序：块下标序列
    std::vector<std::size_t> number;     // 块下标 → RPO 序号（不可达 = kNone）
    std::vector<char> reachable;         // 块下标 → 从入口可达？
    std::size_t edgeCount = 0;
    std::size_t unreachableBlocks = 0;
};
/**
 * 按块尾指令连 CFG 并算出反序后序编号与可达性。。
 *
 * @param program 三地址程序
 * @return CFG：rpo、块号映射、可达标记、边数、不可达块数
 */
Cfg buildCfg(const Program& program);

/**
 * 支配关系：直接支配者、支配树、支配边界（Cooper-Harvey-Kennedy 迭代法）。
 */
struct Dominators {
    std::vector<std::size_t> idom;                       // 块 → 直接支配者（不可达 = kNone）
    std::vector<std::vector<std::size_t>> children;      // 支配树
    std::vector<std::set<std::size_t>> frontier;         // 支配边界 DF(b)
    std::vector<std::size_t> depth;
/**
     * 判断 a 是否支配 b（入口支配所有可达块；自身支配自身）。。
     *
     * @param a 候选支配者
     * @param b 被支配块
     * @return a 支配 b 返回 true
     */
    bool dominates(std::size_t a, std::size_t b) const;
    /**
     * 支配树的 Mermaid 源码（`--mode=dom` 直接打出来）。
     */
    std::string treeText(const Program& program) const;
};
/**
 * 算直接支配者 / 支配树 / 支配边界（Cooper-Harvey-Kennedy 迭代 + 边界上溯）。。
 *
 * @param program 三地址程序
 * @param cfg 该程序的 CFG
 * @return 支配关系（含 depth 与 Mermaid 输出用的 children）
 */
Dominators analyzeDominators(const Program& program, const Cfg& cfg);

/**
 * 从程序里出现的可改名变量（`[x]` 与标号不算）。
 */
std::vector<std::string> variables(const Program& program);

/**
 * 块级活跃变量（LIVE.in = USE ∪ (LIVE.out − DEF)），剪枝 φ 插入要用。
 */
std::vector<std::set<std::string>> liveInSets(const Program& program);

/** SSA 构造：φ 插入 + 变量改名。 */
struct SsaInfo {
    Program program;                                       // SSA 形式
    std::size_t phiCount = 0;                              // 插入的 φ 条数
    std::size_t renamedVariables = 0;                      // 被改名的变量个数
    std::size_t renamedDefinitions = 0;                    // 新产生的 SSA 名字个数（不含初始值）
    std::map<std::string, std::size_t> phiPerVariable;     // 变量 → φ 条数
    std::map<std::string, std::size_t> naming;             // 原变量 → 新名字个数
    std::size_t prunedPhis = 0;                            // 因为"块入口不活跃"而没插的 φ
};
/**
 * 构造 SSA：φ 插入（迭代支配边界 ∩ 块入口活跃，剪枝）+ 变量改名，不可达块不参与。。
 *
 * @param program 三地址程序
 * @param cfg 该程序的 CFG
 * @param dom 支配关系
 * @return SSA 程序与 φ / 改名统计
 */
SsaInfo buildSsa(const Program& program, const Cfg& cfg, const Dominators& dom);

/**
 * 稀疏条件常量传播（Wegman-Zadeck）：格 {⊤, 常量, ⊥} + 可执行边。
 */
struct SccpResult {
    Program program;                                       // 折叠并把不可达块删掉之后的程序
    std::set<std::size_t> executableBlocks;                // 可执行块（原编号）
    std::set<std::pair<std::size_t, std::size_t>> executableEdges;
    std::map<std::string, long long> constants;            // 最终为常量的名字
    std::set<std::string> overdefined;                     // 最终为 ⊥（非常量）的名字
    std::size_t foldedInstructions = 0;                    // 整个指令被折叠成常量赋值的条数
    std::size_t foldedOperands = 0;                        // 被常量替换的操作数个数
    std::size_t foldedBranches = 0;                        // 条件已知、被改成 jmp / 删除的分支
    std::size_t removedBlocks = 0;                         // 删除的不可达块
    std::size_t removedEdges = 0;                          // 删除的不可达边
};
/**
 * 稀疏条件常量传播：⊤ / 常量 / ⊥ 格 + 可执行边，折叠常量指令与常量分支，并删掉不可达块（φ 实参按新前驱表重排）。。
 *
 * @param ssaProgram SSA 程序
 * @param cfg 该程序的 CFG
 * @return 折叠后的程序与常量 / 可执行性统计
 */
SccpResult sccp(const Program& ssaProgram, const Cfg& cfg);

/**
 * 全局值编号（哈希合一 + φ 归并）：消掉重复表达式与冗余 φ。
 */
struct GvnResult {
    Program program;
    std::map<std::string, std::string> representative;      // 名字 → 同值代表
    std::size_t redundantExpressions = 0;                   // 消掉的重复表达式条数
    std::size_t redundantPhis = 0;                          // 消掉的冗余 φ 条数
    std::size_t redundantCopies = 0;                        // 消掉的复制条数
    std::size_t replacedUses = 0;                           // 被改写的使用次数
};
/**
 * GVN 需要支配关系：只在"代表支配使用点"时才敢把两条指令并成一条。
 */
GvnResult gvn(const Program& ssaProgram, const Cfg& cfg, const Dominators& dom);

/**
 * φ 消除：切临界边 + 并行复制排序（带环时借道临时变量）。
 */
struct Destruction {
    Program program;                                        // 可直接执行的形式
    std::size_t parallelCopies = 0;                         // 插入的复制条数
    std::size_t splitEdges = 0;                             // 切开的临界边条数
    std::size_t temporaries = 0;                            // 因复制环引入的临时变量个数
};
/**
 * φ 消除：切临界边 + 并行复制排序（成环时借临时变量），回到可执行形式。。
 *
 * @param ssaProgram SSA 程序
 * @param cfg 该程序的 CFG
 * @return 可执行程序与插入的复制 / 切边 / 临时变量计数
 */
Destruction destroySsa(const Program& ssaProgram, const Cfg& cfg);

/**
 * 解释执行（尺寸很小，仅用于"优化前后语义一致"的对照）。
 */
struct RunResult {
    bool halted = true;
    std::size_t steps = 0;
    std::vector<long long> output;                           // `print` 的输出
    std::map<std::string, long long> memory;                 // 退出时的内存
    std::string error;                                       // 非空表示出错 / 超步数
};
/**
 * 解释执行三地址程序（只用于"优化前后语义一致"的对照；有 φ 时会报错，需先 destroy）。。
 *
 * @param program 三地址程序
 * @param maxSteps 步数上限（兜死循环）
 * @return 输出序列、内存快照、步数与错误信息
 */
RunResult run(const Program& program, std::size_t maxSteps = 200000);

/** 整条流水线的指标。 */
struct Stats {
    std::size_t blocks = 0;
    std::size_t edges = 0;
    std::size_t unreachableBlocks = 0;
    std::size_t variables = 0;
    std::size_t phis = 0;
    std::size_t prunedPhis = 0;          // 因为块入口不活跃而没插的 φ
    std::size_t renamedVariables = 0;
    std::size_t renamedDefinitions = 0;
    std::size_t sccpFoldedInstructions = 0;
    std::size_t sccpFoldedOperands = 0;
    std::size_t sccpFoldedBranches = 0;
    std::size_t sccpConstants = 0;
    std::size_t sccpRemovedBlocks = 0;
    std::size_t sccpRemovedEdges = 0;
    std::size_t gvnRedundantExpressions = 0;
    std::size_t gvnRedundantPhis = 0;
    std::size_t gvnRedundantCopies = 0;
    std::size_t gvnReplacedUses = 0;
    std::size_t destroyedParallelCopies = 0;
    std::size_t destroyedSplitEdges = 0;
    std::size_t destroyedTemporaries = 0;
    std::size_t instructionsBefore = 0;
    std::size_t instructionsAfter = 0;
};

/**
 * 一次完整流水线的结果（含"优化前后跑一遍输出是否一致"的对照）。
 */
struct Result {
    SsaInfo ssa;
    SccpResult sccp;
    GvnResult gvn;
    Destruction destruction;
    Stats stats;
    RunResult before;          // 原程序
    RunResult after;           // φ 消除之后的程序
    bool equivalent = false;   // 两者输出一致（且都没出错）
    std::vector<std::string> notes;
};

/**
 * 端到端：SSA 构造 → SCCP → GVN → φ 消除，并做一次语义对照。
 */
Result optimize(const Program& program);

/** 指标表格（中文）。 */
std::string report(const Result& result);

} // namespace ssa