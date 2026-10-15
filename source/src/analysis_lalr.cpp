/**
 * @file analysis_lalr.cpp
 * @brief LALR(1)：LR(1) 项集 + 同心合并 lookahead、ACTION/GOTO 表与报告。
 */

#include "ParseEngine/analysis.hpp"

#include <algorithm>
#include <cstdio>
#include <functional>
#include <iostream>
#include <sstream>
#include <unordered_map>

#include "ParseEngine/analysis_internal.hpp"
#include "ParseEngine/grammar_loader.hpp"

namespace ParseEngine
{

using namespace analysis_detail;

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
    const std::map<std::string, std::set<std::string>> &first)
{
    std::set<std::string> result;
    for (const std::string &symbol : sequence)
    {
        if (!nontermSet.count(symbol))
        {
            result.insert(symbol);
            return {result, false};
        }
        for (const std::string &value : first.at(symbol))
        {
            if (value != k_epsilon)
            {
                result.insert(value);
            }
        }
        if (!first.at(symbol).count(k_epsilon))
        {
            return {result, false};
        }
    }
    return {result, true};
}

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
    const std::map<std::string, std::set<std::string>> &first)
{
    // 同一个 core 的不同 lookahead 合并成一组：闭包传播时按组处理，
    // 每条产生式只做一次集合插入，而不是「lookahead 数 × 产生式数」次。
    std::map<LrItem, std::set<std::string>> groups;
    for (const Lr1Item &item : items)
    {
        const LrItem core{item.lhs, item.prod, item.dot};
        groups[core].insert(item.lookahead);
    }
    std::vector<LrItem> pending;
    pending.reserve(groups.size());
    for (const auto &entry : groups)
    {
        pending.push_back(entry.first);
    }
    while (!pending.empty())
    {
        const LrItem core = pending.back();
        pending.pop_back();
        const std::vector<std::string> &rhs =
            rules.at(core.lhs)[static_cast<std::size_t>(core.prod)].rhs;
        if (static_cast<std::size_t>(core.dot) >= rhs.size())
        {
            continue;
        }
        const std::string &symbol = rhs[static_cast<std::size_t>(core.dot)];
        if (!nontermSet.count(symbol))
        {
            continue;
        }
        const std::set<std::string> &coreLookaheads = groups[core];
        // 该 core 的所有 lookahead 一起传播：
        // FIRST(β a) = FIRST(β)，β 可空时再并上整组 lookahead
        const std::vector<std::string> suffix(
            rhs.begin() + static_cast<std::ptrdiff_t>(core.dot) + 1, rhs.end());
        const auto betaFirst = lr1FirstOfSequence(suffix, nontermSet, first);
        std::set<std::string> newLookaheads = betaFirst.first;
        if (betaFirst.second)
        {
            newLookaheads.insert(coreLookaheads.begin(), coreLookaheads.end());
        }
        const std::vector<Production> &targetProductions = rules.at(symbol);
        for (std::size_t index = 0; index < targetProductions.size(); ++index)
        {
            const LrItem next{symbol, static_cast<int>(index), 0};
            std::set<std::string> &target = groups[next];
            const std::size_t before = target.size();
            target.insert(newLookaheads.begin(), newLookaheads.end());
            // lookahead 集合变大才需要重新入队传播
            if (target.size() != before)
            {
                pending.push_back(next);
            }
        }
    }
    std::set<Lr1Item> result;
    for (const auto &entry : groups)
    {
        for (const std::string &token : entry.second)
        {
            result.insert({entry.first.lhs, entry.first.prod, entry.first.dot,
                           token});
        }
    }
    return result;
}

/**
 * @brief LR(1) GOTO 转移。
 * @param items 当前项集。
 * @param symbol 转移符号。
 * @param rules 规则表。
 * @param nontermSet 非终结符集合。
 * @param first FIRST 集合。
 * @return 目标项集。
 */
std::set<Lr1Item> lr1Goto(
    const std::set<Lr1Item> &items, const std::string &symbol,
    const std::map<std::string, std::vector<Production>> &rules,
    const std::set<std::string> &nontermSet,
    const std::map<std::string, std::set<std::string>> &first)
{
    std::set<Lr1Item> moved;
    for (const Lr1Item &item : items)
    {
        const std::vector<std::string> &rhs =
            rules.at(item.lhs)[static_cast<std::size_t>(item.prod)].rhs;
        if ((static_cast<std::size_t>(item.dot) < rhs.size()) &&
            (rhs[static_cast<std::size_t>(item.dot)] == symbol))
        {
            moved.insert({item.lhs, item.prod, item.dot + 1,
                          item.lookahead});
        }
    }
    if (moved.empty())
    {
        return {};
    }
    return lr1Closure(moved, rules, nontermSet, first);
}

/**
 * @brief 把「规范 LR(1) 项集」原样转成 core -> lookahead 形式（不合并）。
 * @param lr1States LR(1) 项集族。
 * @param outStates 输出状态列表（编号与输入一致）。
 */
void lr1ToCanonicalStates(const std::vector<std::set<Lr1Item>> &lr1States,
                          std::vector<LalrState> &outStates)
{
    outStates.clear();
    for (const std::set<Lr1Item> &lr1State : lr1States)
    {
        LalrState state;
        for (const Lr1Item &item : lr1State)
        {
            state.items[LrItem{item.lhs, item.prod, item.dot}].insert(
                item.lookahead);
        }
        outStates.push_back(state);
    }
}

/**
 * @brief 合并同心项集。
 * @param lr1States LR(1) 项集族。
 * @param outStates 输出 LALR 状态列表。
 * @param outStateMap 输出 LR(1) -> LALR 状态映射。
 */
void lalrMergeStates(const std::vector<std::set<Lr1Item>> &lr1States,
                     std::vector<LalrState> &outStates,
                     std::vector<int> &outStateMap)
{
    std::map<std::set<LrItem>, int> coreIds;
    outStates.clear();
    outStateMap.clear();
    for (const std::set<Lr1Item> &lr1State : lr1States)
    {
        // 计算该状态的 core，用于查表合并
        std::set<LrItem> core;
        std::map<LrItem, std::set<std::string>> merged;
        for (const Lr1Item &item : lr1State)
        {
            const LrItem key{item.lhs, item.prod, item.dot};
            core.insert(key);
            merged[key].insert(item.lookahead);
        }
        if (!coreIds.count(core))
        {
            // 新 core：分配新状态编号
            coreIds[core] = static_cast<int>(outStates.size());
            LalrState state;
            state.items = merged;
            outStates.push_back(state);
        }
        else
        {
            // 已存在同 core 的状态：合并 lookahead
            LalrState &target = outStates[static_cast<std::size_t>(
                coreIds[core])];
            for (const auto &entry : merged)
            {
                target.items[entry.first].insert(entry.second.begin(),
                                                 entry.second.end());
            }
        }
        outStateMap.push_back(coreIds[core]);
    }
}

/**
 * @brief 把 LALR 项集渲染成一行文本（每个 core 带 lookahead 集合）。
 * @param stateItems 项集。
 * @param rules 规则表。
 * @return 一行文本。
 */
std::string lalrStateText(const LalrState &state,
                          const std::map<std::string, std::vector<Production>> &rules)
{
    std::string joined;
    for (const auto &entry : state.items)
    {
        std::vector<std::string> rhs =
            rules.at(entry.first.lhs)[static_cast<std::size_t>(entry.first.prod)]
                .rhs;
        const std::size_t position = std::min(
            static_cast<std::size_t>(entry.first.dot), rhs.size());
        rhs.insert(rhs.begin() + static_cast<std::ptrdiff_t>(position), ".");
        std::string joinedRhs;
        for (std::size_t index = 0; index < rhs.size(); ++index)
        {
            if (index != 0)
            {
                joinedRhs += " ";
            }
            joinedRhs += rhs[index];
        }
        std::string lookaheads;
        for (const std::string &token : entry.second)
        {
            if (!lookaheads.empty())
            {
                lookaheads += ", ";
            }
            lookaheads += token;
        }
        if (!joined.empty())
        {
            joined += ", ";
        }
        joined += "(" + entry.first.lhs + " -> " + joinedRhs + ", {" +
                  lookaheads + "})";
    }
    return "[" + joined + "]";
}

/**
 * @brief 基于合并后的 LALR 项集构造 ACTION / GOTO 表。
 * @param states LALR 项集族。
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
    AnalysisResult &out, bool resolveReduceReduce)
{
    std::set<std::string> terminalSet(terminals.begin(), terminals.end());
    std::map<std::string, int> nontermIds;
    for (std::size_t index = 0; index < nonterms.size(); ++index)
    {
        nontermIds[nonterms[index]] = static_cast<int>(index);
    }
    const auto productionIds = slrProductionIds(rules);
    const std::size_t resolutionsBefore = out.resolutions.size();
    LrRecorder recorder = makeLrRecorder(
        label,
        [&states, &rules](int stateId)
        {
            return lalrStateText(states[static_cast<std::size_t>(stateId)],
                                 rules);
        },
        rules, productionIds, out.action, priority, out.resolutions,
        out.unresolved, &out.warnings, resolveReduceReduce);
    for (std::size_t stateId = 0; stateId < states.size(); ++stateId)
    {
        for (const auto &entry : states[stateId].items)
        {
            const LrItem &item = entry.first;
            const std::vector<std::string> &rhs =
                rules.at(item.lhs)[static_cast<std::size_t>(item.prod)].rhs;
            if (static_cast<std::size_t>(item.dot) < rhs.size())
            {
                const std::string &symbol =
                    rhs[static_cast<std::size_t>(item.dot)];
                if (terminalSet.count(symbol))
                {
                    const auto found = transitions.find(
                        std::make_pair(static_cast<int>(stateId), symbol));
                    if (found != transitions.end())
                    {
                        recorder.record(static_cast<int>(stateId), symbol,
                                        {LrAction::Kind::Shift, found->second},
                                        symbol);
                    }
                }
                continue;
            }
            if (item.lhs == start)
            {
                recorder.record(static_cast<int>(stateId), "END",
                                {LrAction::Kind::Accept, 0}, "END");
                continue;
            }
            for (const std::string &token : entry.second)
            {
                const auto prodIdIt = productionIds.find(std::make_pair(item.lhs, item.prod));
                const int *prodId =
                    prodIdIt != productionIds.end() ? &prodIdIt->second : nullptr;
                recorder.record(static_cast<int>(stateId), token,
                                {LrAction::Kind::Reduce, *prodId}, token);
            }
        }
    }
    for (const auto &entry : transitions)
    {
        const std::string &symbol = entry.first.second;
        if (!terminalSet.count(symbol))
        {
            out.gotoTable[std::make_pair(entry.first.first,
                                         nontermIds[symbol])] =
                entry.second;
        }
    }
    if (resolveReduceReduce)
    {
        appendReduceReduceWarning(label, out.resolutions, resolutionsBefore,
                                  out.warnings);
    }
}

/**
 * @brief 计算单个 LR(1) 项的哈希值。
 * @param item 项。
 * @return 哈希值。
 */
std::size_t lr1ItemHash(const Lr1Item &item)
{
    std::size_t hash = std::hash<std::string>()(item.lhs);
    hash ^= std::hash<std::string>()(item.lookahead) +
            0x9e3779b97f4a7c15u + (hash << 6) + (hash >> 2);
    hash = hash * 1099511628211u + static_cast<std::size_t>(item.prod);
    return hash * 1099511628211u + static_cast<std::size_t>(item.dot);
}

/**
 * @brief 计算 LR(1) 项集的哈希值（按有序元素顺序，仅用于分桶）。
 * @param state 项集。
 * @return 哈希值。
 */
std::size_t lr1StateHash(const std::set<Lr1Item> &state)
{
    std::size_t hash = 1469598103934665603u;
    for (const Lr1Item &item : state)
    {
        hash ^= lr1ItemHash(item);
        hash *= 1099511628211u;
    }
    return hash;
}

/**
 * @brief 构造 LR(1) 项集族（广度优先）。
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
    std::map<std::pair<int, std::string>, int> &outTransitions)
{
    const std::set<Lr1Item> startState =
        lr1Closure({{start, 0, 0, "END"}}, rules, nontermSet, first);
    outStates.clear();
    outStates.push_back(startState);
    // 用「哈希 -> 候选状态编号」分桶：绝大多数查找只比一次，
    // 不必像 std::map<set<Lr1Item>> 那样每次都做整集逐元素比较。
    std::unordered_map<std::size_t, std::vector<int>> stateIds;
    stateIds[lr1StateHash(startState)].push_back(0);
    std::vector<int> pending{0};
    while (!pending.empty())
    {
        const int stateId = pending.front();
        pending.erase(pending.begin());
        std::vector<std::string> symbols;
        for (const Lr1Item &item : outStates[static_cast<std::size_t>(stateId)])
        {
            const std::vector<std::string> &rhs =
                rules.at(item.lhs)[static_cast<std::size_t>(item.prod)].rhs;
            if (static_cast<std::size_t>(item.dot) >= rhs.size())
            {
                continue;
            }
            const std::string &symbol = rhs[static_cast<std::size_t>(item.dot)];
            if (std::find(symbols.begin(), symbols.end(), symbol) ==
                symbols.end())
            {
                symbols.push_back(symbol);
            }
        }
        for (const std::string &symbol : symbols)
        {
            const std::set<Lr1Item> target =
                lr1Goto(outStates[static_cast<std::size_t>(stateId)], symbol,
                        rules, nontermSet, first);
            if (target.empty())
            {
                continue;
            }
            const std::size_t targetHash = lr1StateHash(target);
            int targetId = -1;
            const auto bucket = stateIds.find(targetHash);
            if (bucket != stateIds.end())
            {
                for (const int candidate : bucket->second)
                {
                    if (outStates[static_cast<std::size_t>(candidate)] == target)
                    {
                        targetId = candidate;
                        break;
                    }
                }
            }
            if (targetId < 0)
            {
                targetId = static_cast<int>(outStates.size());
                outStates.push_back(target);
                stateIds[targetHash].push_back(targetId);
                pending.push_back(targetId);
            }
            outTransitions[std::make_pair(stateId, symbol)] = targetId;
        }
    }
}

} // namespace lr1_detail

using namespace lr1_detail;

bool Analysis::lalrAnalyze(const Grammar &grammar,
                           const std::vector<std::string> &tokens,
                           const PriorityConfig *priority, bool teach,
                           AnalysisResult &out, Error &error)
{
    const PriorityConfig normalized = GrammarLoader::normalizePriority(priority);
    AnalysisResult &result = out;
    result.teach = teach;
    result.priority = normalized;
    std::set<std::string> nontermSet;
    prepareLrAnalysis(grammar, tokens, k_lalrStartSymbol, "lalr", result,
                      nontermSet);
    const std::string &start = result.grammar.start;
    std::vector<std::set<Lr1Item>> lr1States;
    std::map<std::pair<int, std::string>, int> lr1Transitions;
    lr1BuildAutomaton(result.grammar.rules, nontermSet, start, result.first,
                      lr1States, lr1Transitions);
    result.lr1StateCount = static_cast<int>(lr1States.size());
    std::vector<int> stateMap;
    lalrMergeStates(lr1States, result.lalrStates, stateMap);
    for (const auto &entry : lr1Transitions)
    {
        result.transitions[std::make_pair(
            stateMap[static_cast<std::size_t>(entry.first.first)],
            entry.first.second)] =
            stateMap[static_cast<std::size_t>(entry.second)];
    }
    result.hasFollow = false;
    buildLalrStyleTables(result.lalrStates, result.transitions,
                         result.grammar.rules, result.terminals,
                         result.nonterms, start, normalized, "LALR", result);
    if (!checkUnresolvedConflictsForMode("LALR", result, result.teach, error))
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

std::string formatLalrReport(const AnalysisResult &analysis)
{
    std::map<std::string, int> order;
    for (std::size_t index = 0; index < analysis.terminals.size(); ++index)
    {
        order[analysis.terminals[index]] = static_cast<int>(index);
    }
    order[k_epsilon] = static_cast<int>(order.size());
    std::vector<std::string> lines;
    lines.push_back("[生成模式] LALR(1)：LR(1) 项集 + 同心项集合并 lookahead");
    lines.push_back("");
    lines.push_back("[最终产生式]");
    for (const Production &production : analysis.productions)
    {
        lines.push_back("P" + std::to_string(production.id) + ": " +
                        production.lhs + " -> " + joinRhs(production.rhs));
    }
    lines.push_back("");
    lines.push_back("[FIRST]");
    for (const std::string &name : analysis.nonterms)
    {
        std::string joined;
        for (const std::string &item : orderedTokens(analysis.first.at(name), order))
        {
            if (!joined.empty())
            {
                joined += ", ";
            }
            joined += (item == k_epsilon) ? "%empty" : item;
        }
        lines.push_back("FIRST(" + name + ") = { " + joined + " }");
    }
    lines.push_back("");
    lines.push_back("[LALR 自动机]");
    lines.push_back("LR(1) 状态数 = " + std::to_string(analysis.lr1StateCount) +
                    "，合并后 LALR 状态数 = " +
                    std::to_string(analysis.lalrStates.size()) +
                    "，GOTO 转移数 = " +
                    std::to_string(analysis.transitions.size()));
    lines.push_back("");
    lines.push_back("[ACTION 表]");
    for (std::size_t stateId = 0; stateId < analysis.lalrStates.size(); ++stateId)
    {
        for (const std::string &token : analysis.terminals)
        {
            const auto found = analysis.action.find(
                std::make_pair(static_cast<int>(stateId), token));
            if (found != analysis.action.end())
            {
                lines.push_back("ACTION[" + std::to_string(stateId) + ", " +
                                token + "] = " + slrEntryText(found->second));
            }
        }
    }
    lines.push_back("");
    lines.push_back("[GOTO 表]");
    for (const auto &entry : analysis.gotoTable)
    {
        lines.push_back(
            "GOTO[" + std::to_string(entry.first.first) + ", " +
            analysis.nonterms[static_cast<std::size_t>(entry.first.second)] +
            "] = " + std::to_string(entry.second));
    }
    lines.push_back("");
    lines.push_back("[冲突消解]");
    if (!analysis.resolutions.empty())
    {
        for (const LrResolution &entry : analysis.resolutions)
        {
            lines.push_back(resolutionLine(entry));
        }
    }
    else
    {
        lines.push_back("（无）");
    }
    lines.push_back("");
    lines.push_back("[未消解冲突]");
    if (!analysis.unresolved.empty())
    {
        for (const LrConflict &item : analysis.unresolved)
        {
            lines.push_back(item.text);
        }
    }
    else
    {
        lines.push_back("（无）");
    }
    appendTeachingSection(analysis, lines);
    std::string result;
    for (std::size_t index = 0; index < lines.size(); ++index)
    {
        if (index != 0)
        {
            result += "\n";
        }
        result += lines[index];
    }
    result += "\n";
    return result;
}

std::string formatLalrAutomaton(const AnalysisResult &analysis)
{
    std::vector<std::string> lines;
    lines.push_back("[LALR 项集族]");
    for (std::size_t stateId = 0; stateId < analysis.lalrStates.size(); ++stateId)
    {
        lines.push_back("I" + std::to_string(stateId) + ":");
        for (const auto &entry : analysis.lalrStates[stateId].items)
        {
            std::vector<std::string> rhs =
                analysis.grammar.rules.at(entry.first.lhs)[static_cast<std::size_t>(
                    entry.first.prod)].rhs;
            const std::size_t position = std::min(
                static_cast<std::size_t>(entry.first.dot), rhs.size());
            rhs.insert(rhs.begin() + static_cast<std::ptrdiff_t>(position), ".");
            std::string joinedRhs;
            for (std::size_t index = 0; index < rhs.size(); ++index)
            {
                if (index != 0)
                {
                    joinedRhs += " ";
                }
                joinedRhs += rhs[index];
            }
            std::string lookaheads;
            for (const std::string &token : entry.second)
            {
                if (!lookaheads.empty())
                {
                    lookaheads += ", ";
                }
                lookaheads += token;
            }
            lines.push_back("    (" + entry.first.lhs + " -> " + joinedRhs +
                            ", {" + lookaheads + "})");
        }
    }
    lines.push_back("");
    lines.push_back("[GOTO 转移]");
    for (const auto &entry : analysis.transitions)
    {
        lines.push_back("GOTO(I" + std::to_string(entry.first.first) + ", " +
                        entry.first.second + ") = I" +
                        std::to_string(entry.second));
    }
    std::string result;
    for (std::size_t index = 0; index < lines.size(); ++index)
    {
        if (index != 0)
        {
            result += "\n";
        }
        result += lines[index];
    }
    result += "\n";
    return result;
}

std::string formatLalrTables(const AnalysisResult &analysis)
{
    return formatSlrTables(analysis);
}

} // namespace ParseEngine
