/**
 * @file analysis_slrk.cpp
 * @brief SLR(k)：LR(0) 项集 + FOLLOW_k 展望符。
 *
 * SLR(1) 与 LALR(1) 的差别是「归约用不用上下文」：SLR 只看产生式左部的
 * FOLLOW 集合，LALR / LR 看项自带的展望符。把它推广到 k 只换一样东西——
 * FOLLOW 换成 FOLLOW_k（长度恰好 k 的终结符串集合），项集族还是那套 LR(0)
 * 的，所以：
 *
 * - 状态与 SLR(1) 逐个一样（`lr0BuildAutomaton` 原样复用）；
 * - 表的形状跟 LR(k) 一样是两层（移进 + k 元组归约列），于是产物层与
 *   `lrk` 完全共用一份运行时——SLR(k) 只是「完整项的展望符换成 FOLLOW_k」；
 * - k = 1 时与 `slrAnalyze` 等价。
 */

#include "ParseEngine/analysis.hpp"

#include <algorithm>
#include <climits>
#include <set>

#include "ParseEngine/analysis_internal.hpp"
#include "ParseEngine/grammar_loader.hpp"

namespace ParseEngine
{

using namespace analysis_detail;

bool Analysis::slrkAnalyze(const Grammar &grammar,
                           const std::vector<std::string> &tokens,
                           int lookaheadK, const PriorityConfig *priority,
                           bool teach, AnalysisResult &out, Error &error)
{
    const PriorityConfig normalized = GrammarLoader::normalizePriority(priority);
    AnalysisResult &result = out;
    result.teach = teach;
    result.priority = normalized;
    const int k = std::max(1, lookaheadK);
    result.lookaheadK = k;
    std::set<std::string> nontermSet;
    prepareLrAnalysis(grammar, tokens, k_slrkStartSymbol, "slrk", result,
                      nontermSet);
    const std::string &start = result.grammar.start;
    result.firstK = lrk_detail::computeFirstK(
        result.grammar.rules, result.terminals, result.nonterms, k);
    result.followK = lrk_detail::computeFollowK(
        start, result.grammar.rules, result.nonterms, k, result.firstK);
    // 1 Token 的 FOLLOW 也留着：报告与 %sync 同步都用得到
    result.follow = computeFollow(start, result.grammar.rules, result.terminals,
                                  result.nonterms, result.first);
    result.hasFollow = true;
    std::vector<SlrState> lr0States;
    std::map<std::pair<int, std::string>, int> transitions;
    lr0BuildAutomaton(result.grammar.rules, nontermSet, start, lr0States,
                      transitions);
    // LR(0) 状态 -> 「core -> 展望符集合」：完整项的展望符就是 FOLLOW_k(左部)
    // （这正是 SLR 相对 LR 的简化），移进项不需要展望符
    std::vector<LalrState> states;
    for (const SlrState &state : lr0States)
    {
        LalrState converted;
        for (const LrItem &item : state.items)
        {
            std::set<std::string> &lookaheads = converted.items[item];
            const std::vector<std::string> &rhs =
                result.grammar.rules.at(item.lhs)
                    [static_cast<std::size_t>(item.prod)]
                        .rhs;
            if (static_cast<std::size_t>(item.dot) < rhs.size())
            {
                continue; // 移进项：展望符用不上
            }
            if (item.lhs == start)
            {
                continue; // 接受项：buildLrkStyleTables 自己按 END^k 建列
            }
            const auto valuesIt = result.followK.find(item.lhs);
            const std::set<std::string> *values =
                valuesIt != result.followK.end() ? &valuesIt->second : nullptr;
            if (values != nullptr)
            {
                lookaheads = *values;
            }
        }
        states.push_back(converted);
    }
    result.lalrStates = states;
    result.transitions = transitions;
    result.slrStates = lr0States;
    const std::string label = "SLR(" + std::to_string(k) + ")";
    lrk_detail::buildLrkStyleTables(
        result.lalrStates, result.transitions, result.grammar.rules,
        result.terminals, result.nonterms, start, k, normalized, label, result);
    if (!checkUnresolvedConflictsForMode(label, result, result.teach, error))
    {
        return false;
    }
    for (const auto &rule : result.grammar.rules)
    {
        for (const Production &production : rule.second)
        {
            Production entry = production;
            entry.id = static_cast<int>(result.productions.size());
            entry.lhs = rule.first;
            result.productions.push_back(entry);
        }
    }
    return true;
}

} // namespace ParseEngine

