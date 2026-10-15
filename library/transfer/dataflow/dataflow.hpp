#pragma once
// 数据流框架（教学库）：把"格 + 传递函数 + 收敛算法"抽成通用框架，新分析只写格与
// 函数。附带四个实例（前三个就是 library/transfer 优化器里手写的那三个）：
//
//   live   活跃变量      后向 / 并集      （may）
//   reach  到达定值      前向 / 并集      （may）
//   avail  可用表达式    前向 / 交集      （must）
//   const  常量传播      前向 / 映射格    （⊤ < 常量 < ⊥）
//
// 求解器有两种实现，互为对照：工作队列（worklist）与朴素的轮询（naive）。
// 框架还会检查"每次更新都沿格往上走"（单调性），这是抽象解释里最重要的前提。
// 只用 C++17 标准库。
#include "typed_ir.hpp"

#include <cstddef>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace dataflow {

/**
 * 指令类型：全库共用 ir/typed_ir.hpp 里的带位数三地址指令。
 */
using Instruction = t1t::TypedInstruction;



/**
 * 基本块：全库共用 ir/typed_ir.hpp 里的 t1t::Block（label / code / successors / predecessors）。
 */
using Block = t1t::Block;

using Program = std::vector<Block>;

/**
 * 解析：`label` / `mov` / `add|sub|mul|div` / `load` / `store` / `jmp` / `jz` /
 * `jnz` / `print` / `halt`；`;`、`#`、`//` 起注释。分支之后自动开新块。
 */
bool parse(const std::string& text, Program& out, std::vector<std::string>& errors);
/**
 * 按三地址文本格式写回程序（每条指令一行，块标号写 `label`），
 * 便于把分析结果打印出来与输入对照。
 *
 * @param program 三地址程序
 * @return 文本
 */
std::string print(const Program& program);

/**
 * 变量的定义 / 使用（只算可改名变量：`[x]` 与标号不算）。
 */
std::vector<std::string> definedVariables(const Instruction& ins);
/**
 * 取一条指令**使用**的变量名（`[内存]`、常量、标号都算不了变量）。。
 *
 * @param ins 待分析的指令
 * @return 使用到的变量名列表（可能为空）
 */
std::vector<std::string> usedVariables(const Instruction& ins);

// ---------------------------------------------------------------- 格元素

/**
 * 格元素：两种形状——**集合**（集合型格）与**映射**（表型格）。
 * 字符串就是这个教学库里的"值域"：常量写十进制，`⊤` 表示还没算出来，
 * `⊥` 表示非常量。
 */
struct Element {
    enum class Shape { Set, Map };
    Shape shape = Shape::Set;
    std::set<std::string> items;                    // Set 型
    std::map<std::string, std::string> values;      // Map 型

    bool operator==(const Element& other) const;
    bool operator!=(const Element& other) const { return !(*this == other); }
/**
     * 构造集合型格元素（LIVE / RD / AE 用）。。
     *
     * @param items 集合内容
     * @return 集合型元素
     */
    static Element ofSet(std::set<std::string> items);
/**
     * 构造映射型格元素（常量传播用，键是变量、值是常量或 ⊤ / ⊥）。。
     *
     * @param values 映射内容
     * @return 映射型元素
     */
    static Element ofMap(std::map<std::string, std::string> values);
};

/**
 * 打印：集合按字典序空格分隔；映射按 `键=值` 排好序。
 */
std::string formatElement(const Element& element);

// ---------------------------------------------------------------- 分析接口

enum class Direction { Forward, Backward };

/**
 * 一个数据流分析 = 方向 + 单位元 + 边界条件 + 合并（merge）+ 一条指令的传递函数。
 * 求解器完全不关心具体分析是什么。
 */
class Analysis {
public:
    virtual ~Analysis() = default;
    virtual std::string name() const = 0;        // live / reach / avail / const
    virtual std::string title() const = 0;       // 中文标题
    virtual Direction direction() const = 0;
    /**
     * 合并的单位元：`merge(identity, x) == x`。
     */
    virtual Element identity() const = 0;
    /** 边界条件：入口块（前向）或出口块（后向）的初值。 */
    virtual Element boundary() const = 0;
    virtual Element merge(const Element& a, const Element& b) const = 0;
/**
     * 判断两个格元素是否相等（形状、集合、映射都要一致）。。
     *
     * @param a 左元素
     * @param b 右元素
     * @return 相等返回 true
     */
    virtual bool equal(const Element& a, const Element& b) const;
    /**
     * 格序 `a ⪯ b`：缺省用 `merge(a, b) == b` 判定（与合并自洽）。
     */
    virtual bool lessEqual(const Element& a, const Element& b) const;
    /**
     * 一条指令的传递函数。前向分析里 `state` 是这条指令之前的 IN，返回之后的 OUT；
     * 后向分析反过来（`state` 是之后的 OUT，返回之前的 IN）。
     */
    virtual Element transfer(const Program& program, std::size_t block, std::size_t index,
                             const Instruction& ins, const Element& state) const = 0;
};

/**
 * 按名字造分析：live / reach / avail / const。
 * 有些格（可用表达式的全集）依赖具体程序，所以要把程序一起传进来。
 */
std::unique_ptr<Analysis> makeAnalysis(const std::string& name, const Program& program);
/**
 * 全部实例的名字（live、reach、avail、const），顺序固定。
 */
std::vector<std::string> analysisNames();

// ---------------------------------------------------------------- 求解器

struct Options {
    bool lifo = false;           // 工作队列取块顺序：假 = 先进先出（教科书顺序）
    bool seedAll = false;        // 假 = 只把边界块入队（入口 / 出口），真 = 全部入队
    bool checkMonotone = true;   // 每次更新都检查是否沿格往上走
    std::size_t maxRounds = 10000;   // 兜底：防止格写错导致不收敛
};

struct Solution {
    std::vector<Element> in;         // 每块的 IN
    std::vector<Element> out;        // 每块的 OUT
    bool converged = true;
    bool monotone = true;            // 每一步是否都沿格往上
    std::size_t transferApplications = 0;   // 传递函数调用次数
    std::size_t merges = 0;                 // 合并次数
    std::size_t blockVisits = 0;            // 块被处理的次数
    std::vector<std::size_t> visitsPerBlock;
    std::string solver;                     // "worklist" / "naive"
    std::string analysis;
};

Solution solveWorklist(const Program& program, const Analysis& analysis, const Options& options = {});
Solution solveNaive(const Program& program, const Analysis& analysis, const Options& options = {});
/**
 * 两种求解器的结果是否逐块一致（含 IN 与 OUT）。
 */
bool sameSolution(const Solution& a, const Solution& b, const Program& program);

/**
 * 指标表格（中文）：两种求解器的对照 + 单调性 + 收敛性。
 */
std::string report(const Program& program, const Analysis& analysis,
                   const Solution& worklist, const Solution& naive);

/** 每块 IN / OUT 的明细表。 */
std::string dumpSets(const Program& program, const Analysis& analysis, const Solution& solution);

/**
 * CFG + IN / OUT 的 Mermaid 源码（节点标签里带集合）。
 */
std::string mermaid(const Program& program, const Analysis& analysis, const Solution& solution);

/**
 * 程序里出现过的表达式（可用表达式分析的"全集"）。
 */
std::vector<std::string> expressions(const Program& program);

} // namespace dataflow