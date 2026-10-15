/**
 * @file analysis_internal.hpp
 * @brief analysis.cpp 与各模式实现文件（ll1 / slr / lalr / ielr）共用的内部声明。
 *
 * 这些符号只在生成器内部使用，不是公开 API；公开接口在
 * ParseEngine/analysis.hpp。实现放在 analysis.cpp，各模式文件按
 * namespace analysis_detail 复用，避免每个模式各写一份。
 */

#ifndef SCRIPT_ENGINE_ANALYSIS_INTERNAL_HPP
#define SCRIPT_ENGINE_ANALYSIS_INTERNAL_HPP

#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "ParseEngine/analysis.hpp"
#include "ParseEngine/grammar_types.hpp"

namespace ParseEngine
{

namespace analysis_detail
{

/**
 * @brief 按列表顺序去重（对应 Python 的 list(dict.fromkeys(...))）。
 * @param values 输入列表。
 * @return 去重后的列表。
 */
std::vector<std::string> uniqueInOrder(const std::vector<std::string> &values);

/**
 * @brief LR(0) 项集闭包（SLR 与 LR(0) 共用）。
 * @param items 初始项集。
 * @param rules 规则表。
 * @param nontermSet 非终结符集合。
 * @return 闭包项集。
 */
std::set<LrItem> lr0Closure(
    const std::set<LrItem> &items,
    const std::map<std::string, std::vector<Production>> &rules,
    const std::set<std::string> &nontermSet);

/**
 * @brief LR(0) 的 GOTO 转移（SLR 与 LR(0) 共用）。
 * @param items 当前项集。
 * @param symbol 转移符号。
 * @param rules 规则表。
 * @param nontermSet 非终结符集合。
 * @return 目标项集（空表示无转移）。
 */
std::set<LrItem> lr0Goto(
    const std::set<LrItem> &items, const std::string &symbol,
    const std::map<std::string, std::vector<Production>> &rules,
    const std::set<std::string> &nontermSet);

/**
 * @brief 构造 LR(0) 项集族（广度优先，状态编号稳定；SLR 与 LR(0) 共用）。
 * @param rules 规则表。
 * @param nontermSet 非终结符集合。
 * @param start 起始符号。
 * @param outStates 输出状态列表。
 * @param outTransitions 输出转移表。
 */
void lr0BuildAutomaton(
    const std::map<std::string, std::vector<Production>> &rules,
    const std::set<std::string> &nontermSet, const std::string &start,
    std::vector<SlrState> &outStates,
    std::map<std::pair<int, std::string>, int> &outTransitions);

/**
 * @brief 把 LR(0) 项渲染成 `lhs -> a . b` 的可读文本。
 * @param item 项。
 * @param rules 规则表。
 * @return 可读文本。
 */
std::string lr0ItemText(
    const LrItem &item,
    const std::map<std::string, std::vector<Production>> &rules);

/**
 * @brief 把整个 LR(0) 项集渲染成一行文本（用于冲突诊断）。
 * @param stateItems 项集。
 * @param rules 规则表。
 * @return 一行文本。
 */
std::string lr0StateText(
    const std::set<LrItem> &stateItems,
    const std::map<std::string, std::vector<Production>> &rules);

/**
 * @brief 计算所有非终结符的 FIRST 集合。
 * @param rules 规则表。
 * @param terminals 终结符列表。
 * @param nonterms 非终结符列表。
 * @return 非终结符到 FIRST 集合的映射。
 */
std::map<std::string, std::set<std::string>> computeFirst(
    const std::map<std::string, std::vector<Production>> &rules,
    const std::vector<std::string> &terminals,
    const std::vector<std::string> &nonterms);

/**
 * @brief 计算所有非终结符的 FOLLOW 集合。
 * @param start 起始符号。
 * @param rules 规则表。
 * @param terminals 终结符列表。
 * @param nonterms 非终结符列表。
 * @param first FIRST 集合。
 * @return 非终结符到 FOLLOW 集合的映射。
 */
std::map<std::string, std::set<std::string>> computeFollow(
    const std::string &start,
    const std::map<std::string, std::vector<Production>> &rules,
    const std::vector<std::string> &terminals,
    const std::vector<std::string> &nonterms,
    const std::map<std::string, std::set<std::string>> &first);

/**
 * @brief 给规则表里的产生式编号（(左部, 规则内下标) -> 全局编号）。
 * @param rules 规则表。
 * @return 产生式编号映射。
 */
std::map<std::pair<std::string, int>, int> slrProductionIds(
    const std::map<std::string, std::vector<Production>> &rules);

/**
 * @brief 增加虚拟起始符号，构造增广文法。
 * @param grammar 原始文法。
 * @param startSymbol 虚拟起始符号名。
 * @return 增广后的文法。
 */
Grammar augmentGrammar(const Grammar &grammar, const std::string &startSymbol);

/**
 * @brief 把一条 ACTION 表项渲染成文本。
 * @param action 动作。
 * @param rules 规则表。
 * @param productionIds 产生式编号映射。
 * @return 文本。
 */
std::string slrActionText(
    const LrAction &action,
    const std::map<std::string, std::vector<Production>> &rules,
    const std::map<std::pair<std::string, int>, int> &productionIds);

/**
 * @brief 判断两个冲突动作的种类文本。
 * @param previous 已有动作。
 * @param value 新增动作。
 * @return "移进/归约冲突" 或 "归约/归约冲突"。
 */
std::string slrConflictText(const LrAction &previous, const LrAction &value);

/**
 * @brief 计算每条产生式的优先级（%prec 或最右终结符）。
 * @param rules 规则表。
 * @param priority 优先级配置。
 * @return (左部, 规则内下标) -> (结合性, 优先级)。
 */
std::map<std::pair<std::string, int>, std::pair<std::string, int>>
computeProductionPriority(
    const std::map<std::string, std::vector<Production>> &rules,
    const PriorityConfig &priority);

/**
 * @brief 按优先级消解移进/归约冲突。
 * @param previous 已有动作。
 * @param value 新增动作。
 * @param token 触发冲突的 Token。
 * @param precedence Token 优先级。
 * @param associativity Token 结合性。
 * @param productionPriority 产生式优先级。
 * @param outReason 输出消解理由。
 * @return "shift" / "reduce" / "none"；无法消解返回空串。
 */
std::string tryResolveShiftReduce(
    const LrAction &previous, const LrAction &value, const std::string &token,
    const std::map<std::string, int> &precedence,
    const std::map<std::string, std::string> &associativity,
    const std::map<int, std::pair<std::string, int>> &productionPriority,
    std::string &outReason);

/**
 * @brief 写 ACTION 表的记录器：能消解就消解，不能就记成未消解冲突。
 */
struct LrRecorder
{
    std::string label;       ///< "SLR" / "LALR"
    const std::map<std::string, std::vector<Production>> *rules = nullptr;
    const std::map<std::pair<std::string, int>, int> *productionIds = nullptr;
    std::map<std::pair<int, std::string>, LrAction> *action = nullptr;
    std::map<std::string, int> precedence;            ///< Token 优先级
    std::map<std::string, std::string> associativity; ///< Token 结合性
    std::map<int, std::pair<std::string, int>> productionPriority; ///< 产生式优先级
    std::function<std::string(int)> stateText;          ///< 状态渲染函数
    std::vector<LrResolution> *resolutions = nullptr;   ///< 已消解冲突
    std::vector<LrConflict> *unresolved = nullptr;      ///< 未消解冲突
    std::vector<std::string> *warnings = nullptr;       ///< 生成期警告
    /**
     * @brief 归约/归约冲突是否就地决策（确定性 LR 模式为 true，IELR 的探测
     *        阶段为 false）。
     *
     * true 时按 yacc 惯例选一条产生式继续：先看 `%prec` / 终结符优先级
     * （优先级高的胜出），都一样就取**文法里更早出现的产生式**（产生式编号
     * 更小），并往 `warnings` 里塞一条醒目的大警告。false 时照旧记进
     * `unresolved`——IELR 要靠它发现「哪些状态该拆分」。
     */
    bool resolveReduceReduce = false;

    /**
     * @brief 写一条 ACTION 表项并处理冲突。
     * @param stateId 状态编号。
     * @param symbol 表项符号（终结符）。
     * @param value 新增动作。
     * @param token 触发冲突的 Token 名。
     * @param tokenText 冲突说明里显示的 Token 文本；空表示与 token 相同。
     *        LR(k) / LALR(k) 的归约按 k 元组展望符建列，此时 token 传元组的
     *        首个 Token（用它查 %prec / 结合性），tokenText 传整个元组的可读
     *        文本，这样报错里能看到完整的展望符。
     */
    void record(int stateId, const std::string &symbol, const LrAction &value,
                const std::string &token,
                const std::string &tokenText = std::string())
    {
        const std::string &shown = tokenText.empty() ? token : tokenText;
        const auto key = std::make_pair(stateId, symbol);
        const auto found = action->find(key);
        if (found == action->end())
        {
            (*action)[key] = value;
            return;
        }
        const LrAction previous = found->second;
        if (previous == value)
        {
            // 同一个符号由多个项产生相同动作（GOTO 是确定性的），不是冲突
            return;
        }
        const std::string kind = slrConflictText(previous, value);
        std::string reason;
        const std::string resolved = tryResolveShiftReduce(
            previous, value, token, precedence, associativity,
            productionPriority, reason);
        if (resolved == "shift")
        {
            (*action)[key] = (previous.kind == LrAction::Kind::Shift)
                                 ? previous
                                 : value;
            resolutions->push_back(
                {stateId, token, kind, reason, "移进"});
            return;
        }
        if (resolved == "reduce")
        {
            (*action)[key] = (previous.kind == LrAction::Kind::Reduce)
                                 ? previous
                                 : value;
            resolutions->push_back(
                {stateId, token, kind, reason, "归约"});
            return;
        }
        std::string text =
            label + " 冲突（状态 " + std::to_string(stateId) + "，Token " +
            shown + "）：" + kind + "；已有 " +
            slrActionText(previous, *rules, *productionIds) + "，新增 " +
            slrActionText(value, *rules, *productionIds) + "；涉及项集 " +
            stateText(stateId);
        if (!reason.empty())
        {
            text += "；" + reason;
        }
        else if (kind == "移进/归约冲突")
        {
            (*action)[key] = (previous.kind == LrAction::Kind::Shift)
                                 ? previous
                                 : value;
            text += "；没有可用的优先级，默认移进优先（可用 %expect 放行）";
        }
        if (resolveReduceReduce && (kind == "归约/归约冲突"))
        {
            // 确定性 LR 模式的归约/归约冲突：按 yacc 惯例就地选一条产生式，
            // 让生成继续下去，同时给一条醒目的大警告（不让它静默发生）。
            const auto previousPrec = productionPriority.find(previous.value);
            const auto valuePrec = productionPriority.find(value.value);
            const int previousLevel =
                (previousPrec == productionPriority.end())
                    ? -1
                    : previousPrec->second.second;
            const int valueLevel = (valuePrec == productionPriority.end())
                                       ? -1
                                       : valuePrec->second.second;
            const bool usePrecedence = (previousLevel != valueLevel);
            // 优先级相同时取产生式编号更小的那个 —— 也就是文法里更早出现的
            const bool keepPrevious =
                usePrecedence ? (previousLevel > valueLevel)
                              : (previous.value <= value.value);
            const LrAction winner = keepPrevious ? previous : value;
            const LrAction loser = keepPrevious ? value : previous;
            (*action)[key] = winner;
            const std::string reason =
                usePrecedence
                    ? ("优先级高的胜出（优先级 " +
                       std::to_string(std::max(previousLevel, valueLevel)) +
                       " > " + std::to_string(std::min(previousLevel, valueLevel)) +
                       "，来自 %prec 或终结符优先级）")
                    : "yacc 惯例：取文法里更早出现的产生式";
            resolutions->push_back(
                {stateId, shown, kind, reason,
                 slrActionText(winner, *rules, *productionIds),
                 slrActionText(loser, *rules, *productionIds)});
            return;
        }
        if ((warnings != nullptr) && (symbol == k_errorTokenName))
        {
            // error 列按 yacc 惯例移进优先。error 不在任何 FOLLOW 里，正常
            // 文法几乎不会走到这里；真出现时只记一条警告，不让生成失败。
            if (previous.kind == LrAction::Kind::Shift)
            {
                (*action)[key] = previous;
            }
            else if (value.kind == LrAction::Kind::Shift)
            {
                (*action)[key] = value;
            }
            else
            {
                (*action)[key] = previous;
            }
            warnings->push_back(text + "；error 列按 yacc 惯例取移进优先");
            return;
        }
        unresolved->push_back({kind, stateId, token, text});
    }
};

/**
 * @brief 构造写 ACTION 表的记录器。
 * @param label 模式标签（"SLR" / "LALR"）。
 * @param stateText 状态渲染函数。
 * @param rules 规则表。
 * @param productionIds 产生式编号映射。
 * @param action 输出 ACTION 表。
 * @param priority 优先级配置。
 * @param resolutions 输出已消解冲突。
 * @param unresolved 输出未消解冲突。
 * @param warnings 输出生成期警告（可为 nullptr）。
 * @return 记录器。
 */
LrRecorder makeLrRecorder(const std::string &label,
                          std::function<std::string(int)> stateText,
                          const std::map<std::string, std::vector<Production>> &rules,
                          const std::map<std::pair<std::string, int>, int> &productionIds,
                          std::map<std::pair<int, std::string>, LrAction> &action,
                          const PriorityConfig &priority,
                            std::vector<LrResolution> &resolutions,
                            std::vector<LrConflict> &unresolved,
                            std::vector<std::string> *warnings = nullptr,
                            bool resolveReduceReduce = false);

/**
 * @brief %expect 语义：允许保留预期数量的移进/归约冲突，其余一律报错。
 * @param label 模式标签。
 * @param unresolved 未消解冲突列表。
 * @param expectConflicts %expect 值；可为空。
 */
bool checkUnresolvedConflicts(const std::string &label,
                              const std::vector<LrConflict> &unresolved,
                              const std::optional<int> &expectConflicts,
                              Error &error);

/**
 * @brief 把归约/归约冲突的自动决策汇总成一条醒目的大警告。
 *
 * 只看 `resolutions[from..]` 里的归约/归约条目（也就是这一次建表新产生的），
 * 最多列 8 条、其余汇总成一行，避免 LR(0) 这种冲突很多的模式刷屏。
 * @param label 模式标签（"LALR" / "LR(2)" / …）。
 * @param resolutions 已消解冲突列表。
 * @param from 从这个下标开始看（这次建表之前已有多少条）。
 * @param outWarnings 输出警告列表。
 */
void appendReduceReduceWarning(const std::string &label,
                               const std::vector<LrResolution> &resolutions,
                               std::size_t from,
                               std::vector<std::string> &outWarnings);

/**
 * @brief 教学模式的冲突提示语（LR(0) / LR(1) 报告用）。
 *
 * 教学模式下不因未消解冲突中止生成，但必须在报告里说清「该文法不是
 * X 文法」以及生成器接下来是怎么处理的。
 * @param label 模式标签（"LR(0)" / "LR(1)"）。
 * @param unresolved 未消解冲突列表。
 * @return 提示语；没有冲突时返回空串。
 */
std::string teachingConflictNote(const std::string &label,
                                 const std::vector<LrConflict> &unresolved);

/**
 * @brief 按模式处理未消解冲突（各模式严宽差别都收在这里）。
 *
 * - `allowConflicts = false`：等价于 `checkUnresolvedConflicts`，未消解冲突
 *   直接报错、不产出代码；
 * - `allowConflicts = true`：默认放行，把冲突留给报告里的 `[未消解冲突]`
 *   （教学模式还会多一节 `[教学提示]`）；只有显式写了 `%expect` 时才按它的
 *   语义校验冲突数。
 *
 * 谁来传 `true`：`lr0` / `lr1` 无条件是 `true`（这两个模式本来就画不出
 * 「严格」的边界，冲突是它要展示的内容，非教学模式下也照常生成）；`slr` /
 * `lalr` / `ielr` 传教学模式开关（默认严格，开了 `%teach` / `--teach` 才放行）。
 * @param label 模式标签（"LR(0)" / "SLR(1)" / …）。
 * @param result 分析结果（用 teach / unresolved / priority）。
 * @param allowConflicts 是否允许带着未消解冲突继续生成。
 * @param error 出错时写入错误说明。
 * @return 成功返回 true；不允许且冲突未消解时返回 false。
 */
bool checkUnresolvedConflictsForMode(const std::string &label,
                                     const AnalysisResult &result,
                                     bool allowConflicts, Error &error);

/**
 * @brief 追加教学模式的 `[教学提示]` 与 `[教学对照]` 小节。
 *
 * 只在 `analysis.teach` 为真时输出：
 * - `[教学提示]`：该文法是不是 LR(0) / SLR(1) / … 文法，冲突按种类计数；
 * - `[教学对照]`：各模式最有教学价值的状态数 / 冲突数（LR(1) 是规范状态数
 *   与 LALR 合并后状态数的对照）。
 * @param analysis 分析结果。
 * @param lines 输出行列表。
 */
void appendTeachingSection(const AnalysisResult &analysis,
                           std::vector<std::string> &lines);

/**
 * @brief 按固定顺序排列 Token 集合。
 * @param values 集合。
 * @param order Token 到序号。
 * @return 有序列表。
 */
std::vector<std::string> orderedTokens(
    const std::set<std::string> &values,
    const std::map<std::string, int> &order);

/**
 * @brief 把产生式右部拼接成文本。
 * @param rhs 右部符号列表。
 * @return 以空格分隔的文本。
 */
std::string joinRhs(const std::vector<std::string> &rhs);

/**
 * @brief 把一条 ACTION 表项渲染成文本。
 * @param entry 动作。
 * @return 文本。
 */
std::string slrEntryText(const LrAction &entry);

/**
 * @brief LR 分析的公共准备：增广文法、终结符/非终结符列表、FIRST 集合。
 * @param grammar 原始文法。
 * @param tokens 文法声明的 Token。
 * @param startSymbol 虚拟起始符号。
 * @param mode 模式名（"slr" / "lalr" / "ielr"）。
 * @param out 输出分析结果（写入 mode / grammar / terminals / nonterms / first）。
 * @param outNontermSet 输出非终结符集合。
 */
void prepareLrAnalysis(const Grammar &grammar,
                       const std::vector<std::string> &tokens,
                       const std::string &startSymbol,
                       const std::string &mode, AnalysisResult &out,
                       std::set<std::string> &outNontermSet);

/**
 * @brief 消除直接左递归，并保留语义折叠所需的元数据（定义在
 *        source/src/analysis_ll1.cpp；LL(1) 与 LL(k) 共用）。
 * @param grammar 原始文法。
 * @param error 出错时写入错误说明。
 * @return 变换后的文法。
 */
Grammar eliminateDirectLeftRecursion(const Grammar &grammar, Error &error);

/**
 * @brief 检查间接左递归（LL 系不能有左递归，直接的那种已经被消掉）。
 * @param rules 规则表（消除直接左递归之后）。
 * @param start 起始符号。
 * @param error 出错时写入错误说明。
 * @return 没有间接左递归返回 true。
 */
bool checkIndirectLeftRecursion(
    const std::map<std::string, std::vector<Production>> &rules,
    const std::string &start, Error &error);

} // namespace analysis_detail

/**
 * @brief 规范 LR(1) 项集构造与同心合并（定义在 source/src/analysis_lalr.cpp）。
 *
 * LALR 与 IELR 都从同一套规范 LR(1) 自动机出发：LALR 直接按 LR(0) core
 * 合并；IELR 在此基础上做冲突驱动的拆分。这里的函数保持纯构造语义，
 * 不含模式判断。
 */
namespace lr1_detail
{

/**
 * @brief 计算 FIRST(sequence)，返回 (终结符集合, 是否可空)。
 * @param sequence 符号序列。
 * @param nontermSet 非终结符集合。
 * @param first FIRST 集合。
 * @return 二元组（集合，可空标志）。
 */
std::pair<std::set<std::string>, bool> lr1FirstOfSequence(
    const std::vector<std::string> &sequence,
    const std::set<std::string> &nontermSet,
    const std::map<std::string, std::set<std::string>> &first);

/**
 * @brief LR(1) 项集闭包。
 * @param items 初始项集。
 * @param rules 规则表。
 * @param nontermSet 非终结符集合。
 * @param first FIRST 集合。
 * @return 闭包项集。
 */
std::set<Lr1Item> lr1Closure(
    const std::set<Lr1Item> &items,
    const std::map<std::string, std::vector<Production>> &rules,
    const std::set<std::string> &nontermSet,
    const std::map<std::string, std::set<std::string>> &first);

/**
 * @brief LR(1) GOTO 转移。
 * @param items 当前项集。
 * @param symbol 转移符号。
 * @param rules 规则表。
 * @param nontermSet 非终结符集合。
 * @param first FIRST 集合。
 * @return 目标项集（空表示无转移）。
 */
std::set<Lr1Item> lr1Goto(
    const std::set<Lr1Item> &items, const std::string &symbol,
    const std::map<std::string, std::vector<Production>> &rules,
    const std::set<std::string> &nontermSet,
    const std::map<std::string, std::set<std::string>> &first);

/**
 * @brief 构造规范 LR(1) 项集族（广度优先，状态编号稳定）。
 * @param rules 规则表。
 * @param nontermSet 非终结符集合。
 * @param start 起始符号。
 * @param first FIRST 集合。
 * @param outStates 输出 LR(1) 状态列表。
 * @param outTransitions 输出转移表。
 */
void lr1BuildAutomaton(
    const std::map<std::string, std::vector<Production>> &rules,
    const std::set<std::string> &nontermSet, const std::string &start,
    const std::map<std::string, std::set<std::string>> &first,
    std::vector<std::set<Lr1Item>> &outStates,
    std::map<std::pair<int, std::string>, int> &outTransitions);

/**
 * @brief 计算单个 LR(1) / LR(k) 项的哈希值（LR(k) 复用同一套，项集是同一个类型）。
 * @param item 项。
 * @return 哈希值。
 */
std::size_t lr1ItemHash(const Lr1Item &item);

/**
 * @brief 计算 LR(1) / LR(k) 项集的哈希值（按有序元素顺序，仅用于分桶）。
 * @param state 项集。
 * @return 哈希值。
 */
std::size_t lr1StateHash(const std::set<Lr1Item> &state);

/**
 * @brief 把「规范 LR(1) 项集」原样转成 core -> lookahead 形式（不做任何合并）。
 *
 * 规范 LR(1) 状态与「core 到展望符集合」的映射一一对应（同一个 core 的项只是
 * 按 core 归了组），所以用 LalrState 表示不丢信息，还能直接复用
 * buildLalrStyleTables。与 lalrMergeStates 的区别：状态数不变。
 * @param lr1States LR(1) 项集族。
 * @param outStates 输出状态列表（编号与输入一致）。
 */
void lr1ToCanonicalStates(const std::vector<std::set<Lr1Item>> &lr1States,
                          std::vector<LalrState> &outStates);

/**
 * @brief 把 LR(1) 项集族按 LR(0) core 合并成 LALR 状态。
 * @param lr1States LR(1) 项集族。
 * @param outStates 输出 LALR 状态列表。
 * @param outStateMap 输出 LR(1) -> LALR 状态映射。
 */
void lalrMergeStates(const std::vector<std::set<Lr1Item>> &lr1States,
                     std::vector<LalrState> &outStates,
                     std::vector<int> &outStateMap);

/**
 * @brief 把「core -> lookahead」状态渲染成一行文本（冲突诊断用）。
 * @param state 状态。
 * @param rules 规则表。
 * @return 一行文本。
 */
std::string lalrStateText(
    const LalrState &state,
    const std::map<std::string, std::vector<Production>> &rules);

/**
 * @brief 用「core -> lookahead」状态与转移构造 ACTION / GOTO 表（LALR / IELR 共用）。
 * @param states 状态列表。
 * @param transitions GOTO 转移。
 * @param rules 规则表。
 * @param terminals 终结符列表。
 * @param nonterms 非终结符列表。
 * @param start 起始符号。
 * @param priority 优先级配置。
 * @param label 模式标签（写入冲突说明，如 "LALR" / "IELR"）。
 * @param out 输出分析结果。
 */
void buildLalrStyleTables(
    const std::vector<LalrState> &states,
    const std::map<std::pair<int, std::string>, int> &transitions,
    const std::map<std::string, std::vector<Production>> &rules,
    const std::vector<std::string> &terminals,
    const std::vector<std::string> &nonterms, const std::string &start,
    const PriorityConfig &priority, const std::string &label,
    AnalysisResult &out, bool resolveReduceReduce = true);

} // namespace lr1_detail

/**
 * @brief LR(k) / LALR(k) 的构造（定义在 source/src/analysis_lrk.cpp）。
 *
 * k = 1 时这套构造与 lr1_detail 完全等价；k > 1 时把「一个展望符」换成
 * 「k 个展望符」。项仍然是 Lr1Item，但它的 lookahead 字段存的是
 * lookaheadKeyOf() 拼出来的 k 元组键（k = 1 时就是 Token 名本身，所以这一
 * 层对 k = 1 是完全透明的）。
 */
namespace lrk_detail
{

/**
 * @brief FIRST_k(非终结符)：长度 ≤ k 的终结符串集合。
 *
 * 集合里的元素用 lookaheadKeyOf() 的键表示；空串表示「能推出空串」。
 * 长度不足 k 的元素表示「该符号能推出的所有串都在这个前缀之后结束」，
 * 长度正好 k 的元素表示「只看得到 k 个终结符就截断了」。
 * @param rules 规则表。
 * @param terminals 终结符列表。
 * @param nonterms 非终结符列表。
 * @param k 展望符长度。
 * @return 非终结符到 FIRST_k 集合的映射。
 */
std::map<std::string, std::set<std::string>> computeFirstK(
    const std::map<std::string, std::vector<Production>> &rules,
    const std::vector<std::string> &terminals,
    const std::vector<std::string> &nonterms, int k);

/**
 * @brief 计算所有非终结符的 FOLLOW_k 集合（长度恰好 k 的终结符串，键形式）。
 *
 * 与 FOLLOW(1) 同构：起始符号先放 `END^k`，再按 `A -> α B β` 一趟趟传播
 * `FIRST_k(β)` 接到 `FOLLOW_k(A)` 的每个展望符上。`error` 不进 FOLLOW_k
 * （与 FOLLOW 排除 error 的规则一致）。
 * @param start 起始符号。
 * @param rules 规则表。
 * @param nonterms 非终结符列表。
 * @param k 展望符长度。
 * @param firstK FIRST_k 表（computeFirstK 的结果）。
 * @return 非终结符到 FOLLOW_k 集合的映射。
 */
std::map<std::string, std::set<std::string>> computeFollowK(
    const std::string &start,
    const std::map<std::string, std::vector<Production>> &rules,
    const std::vector<std::string> &nonterms, int k,
    const std::map<std::string, std::set<std::string>> &firstK);

/**
 * @brief 计算 FIRST_k(符号序列)，返回长度 ≤ k 的终结符串集合。
 * @param sequence 符号序列。
 * @param k 展望符长度。
 * @param nontermSet 非终结符集合。
 * @param firstK FIRST_k 表（computeFirstK 的结果）。
 * @return 长度 ≤ k 的终结符串集合（键形式）。
 */
std::set<std::string> firstKOfSequence(
    const std::vector<std::string> &sequence, int k,
    const std::set<std::string> &nontermSet,
    const std::map<std::string, std::set<std::string>> &firstK);

/**
 * @brief 把「长度 ≤ k 的前缀集合」接到一个长度恰好 k 的展望符上，截断到 k。
 * @param prefixes FIRST_k(β)（长度 ≤ k）。
 * @param tail 长度恰好 k 的展望符键。
 * @param k 展望符长度。
 * @return 长度恰好 k 的展望符集合（键形式）。
 */
std::set<std::string> extendLookahead(const std::set<std::string> &prefixes,
                                      const std::string &tail, int k);

/**
 * @brief LR(k) 项集闭包。
 * @param items 初始项集。
 * @param rules 规则表。
 * @param nontermSet 非终结符集合。
 * @param k 展望符长度。
 * @param firstK FIRST_k 表。
 * @return 闭包项集。
 */
std::set<Lr1Item> lrkClosure(
    const std::set<Lr1Item> &items,
    const std::map<std::string, std::vector<Production>> &rules,
    const std::set<std::string> &nontermSet, int k,
    const std::map<std::string, std::set<std::string>> &firstK);

/**
 * @brief LR(k) 的 GOTO 转移。
 * @param items 当前项集。
 * @param symbol 转移符号。
 * @param rules 规则表。
 * @param nontermSet 非终结符集合。
 * @param k 展望符长度。
 * @param firstK FIRST_k 表。
 * @return 目标项集（空表示无转移）。
 */
std::set<Lr1Item> lrkGoto(
    const std::set<Lr1Item> &items, const std::string &symbol,
    const std::map<std::string, std::vector<Production>> &rules,
    const std::set<std::string> &nontermSet, int k,
    const std::map<std::string, std::set<std::string>> &firstK);

/**
 * @brief 构造规范 LR(k) 项集族（广度优先，状态编号稳定）。
 * @param rules 规则表。
 * @param nontermSet 非终结符集合。
 * @param start 起始符号。
 * @param k 展望符长度。
 * @param firstK FIRST_k 表。
 * @param outStates 输出状态列表。
 * @param outTransitions 输出转移表。
 */
void lrkBuildAutomaton(
    const std::map<std::string, std::vector<Production>> &rules,
    const std::set<std::string> &nontermSet, const std::string &start, int k,
    const std::map<std::string, std::set<std::string>> &firstK,
    std::vector<std::set<Lr1Item>> &outStates,
    std::map<std::pair<int, std::string>, int> &outTransitions);

/**
 * @brief 把「core -> k 元组展望符」状态渲染成一行文本（冲突诊断用）。
 * @param state 状态。
 * @param rules 规则表。
 * @return 一行文本。
 */
std::string lrkStateText(
    const LalrState &state,
    const std::map<std::string, std::vector<Production>> &rules);

/**
 * @brief 用「core -> k 元组展望符」状态构造 ACTION / GOTO 表。
 *
 * k > 1 时 ACTION 分两层写进 out.action：移进/接受用单 Token 作键（与 k = 1
 * 一致），归约用 k 元组键；同时填 out.lookaheadColumns /
 * out.lookaheadColumnIndex 给出归约列的编号。
 * @param states 状态列表。
 * @param transitions GOTO 转移。
 * @param rules 规则表。
 * @param terminals 终结符列表。
 * @param nonterms 非终结符列表。
 * @param start 起始符号。
 * @param k 展望符长度。
 * @param priority 优先级配置。
 * @param label 模式标签（写入冲突说明，如 "LR(2)" / "LALR(2)"）。
 * @param out 输出分析结果。
 */
void buildLrkStyleTables(
    const std::vector<LalrState> &states,
    const std::map<std::pair<int, std::string>, int> &transitions,
    const std::map<std::string, std::vector<Production>> &rules,
    const std::vector<std::string> &terminals,
    const std::vector<std::string> &nonterms, const std::string &start, int k,
    const PriorityConfig &priority, const std::string &label,
    AnalysisResult &out, bool resolveReduceReduce = true);

} // namespace lrk_detail

} // namespace ParseEngine

#endif // SCRIPT_ENGINE_ANALYSIS_INTERNAL_HPP
