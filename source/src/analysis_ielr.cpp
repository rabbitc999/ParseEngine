/**
 * @file analysis_ielr.cpp
 * @brief IELR(1)：LALR 项集 + 冲突驱动的状态拆分、ACTION/GOTO 表与报告。
 *
 * 实现思路（与 Denny & Malloy 的 IELR(1) 同源，按本仓库的规模做了裁剪）：
 *
 * 1. 构造规范 LR(1) 自动机（lr1_detail），按 LR(0) core 合并得到 LALR 状态；
 * 2. 用合并后的 lookahead 建表。若没有未消解的归约/归约冲突，说明该文法
 *    就是 LALR(1)，直接输出与 LALR 完全一致的表；
 * 3. 否则做冲突驱动的状态拆分：把触发冲突的 LALR 状态按「各 LR(1) 子状态
 *    在该 Token 上启用的归约项」分组，再对分组做 goto 一致性细化
 *    （partition refinement），得到一个新的、更细的等价类划分；
 * 4. 用细分后的划分重建 lookahead 与 ACTION/GOTO 表，重复 3–4 直到没有
 *    未消解冲突（或划分已细到规范 LR(1)，此时仍冲突说明文法不是 LR(1)）。
 *
 * 因为合并只在 core 相同、且 lookahead 取并集时进行，移进动作完全由 core
 * 决定，所以「合并」不会凭空产生移进/归约冲突；LALR 相对规范 LR(1) 多出来
 * 的冲突一定是归约/归约冲突。拆分只减少 lookahead，不会引入新冲突。
 */

#include "ParseEngine/analysis.hpp"

#include <algorithm>
#include <cstdio>
#include <functional>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <vector>

#include "ParseEngine/analysis_internal.hpp"
#include "ParseEngine/grammar_loader.hpp"

namespace ParseEngine
{

using namespace analysis_detail;
using namespace lr1_detail;

namespace
{

/// 规则表简写。
using RuleMap = std::map<std::string, std::vector<Production>>;

/**
 * @brief 判断项是否已完成（圆点在末尾）。
 * @param item LR(0) 项。
 * @param rules 规则表。
 * @return 完成返回 true。
 */
bool isCompleteItem(const LrItem &item, const RuleMap &rules)
{
    const std::vector<std::string> &rhs =
        rules.at(item.lhs)[static_cast<std::size_t>(item.prod)].rhs;
    return static_cast<std::size_t>(item.dot) >= rhs.size();
}

/**
 * @brief 把单个项渲染成 "(lhs -> a . b)"。
 * @param item 项。
 * @param rules 规则表。
 * @return 文本。
 */
std::string itemText(const LrItem &item, const RuleMap &rules)
{
    std::vector<std::string> rhs =
        rules.at(item.lhs)[static_cast<std::size_t>(item.prod)].rhs;
    const std::size_t position =
        std::min(static_cast<std::size_t>(item.dot), rhs.size());
    rhs.insert(rhs.begin() + static_cast<std::ptrdiff_t>(position), ".");
    std::string joined = item.lhs + " ->";
    for (const std::string &symbol : rhs)
    {
        joined += " " + symbol;
    }
    return "(" + joined + ")";
}

/**
 * @brief 单个 LR(1) 子状态在某个 Token 上启用的归约项。
 * @param items LR(1) 状态。
 * @param token 查询的 Token。
 * @param rules 规则表。
 * @return 启用的完成项（按 LR(1) 项顺序）。
 */
std::vector<LrItem> enabledReductions(const std::set<Lr1Item> &items,
                                      const std::string &token,
                                      const RuleMap &rules)
{
    std::vector<LrItem> enabled;
    for (const Lr1Item &item : items)
    {
        const LrItem core{item.lhs, item.prod, item.dot};
        if ((item.lookahead == token) && isCompleteItem(core, rules))
        {
            enabled.push_back(core);
        }
    }
    return enabled;
}

/**
 * @brief 合并状态里在该 Token 上可归约的完成项（用于说明冲突）。
 * @param state 合并状态。
 * @param token 查询的 Token。
 * @param rules 规则表。
 * @return 完成项列表。
 */
std::vector<LrItem> reducedItemsInBlock(const LalrState &state,
                                        const std::string &token,
                                        const RuleMap &rules)
{
    std::vector<LrItem> result;
    for (const auto &entry : state.items)
    {
        if (isCompleteItem(entry.first, rules) && entry.second.count(token))
        {
            result.push_back(entry.first);
        }
    }
    return result;
}

/**
 * @brief 由「规范 LR(1) 状态 -> 块编号」的划分重建 quotient 状态与转移。
 * @param lr1States 规范 LR(1) 项集族。
 * @param lr1Transitions 规范 LR(1) 转移。
 * @param blockOf 每个 LR(1) 状态所属块。
 * @param blockCount 块数量。
 * @param outStates 输出合并状态（core -> lookahead 并集）。
 * @param outTransitions 输出块级转移。
 */
void buildQuotient(
    const std::vector<std::set<Lr1Item>> &lr1States,
    const std::map<std::pair<int, std::string>, int> &lr1Transitions,
    const std::vector<int> &blockOf, int blockCount,
    std::vector<LalrState> &outStates,
    std::map<std::pair<int, std::string>, int> &outTransitions)
{
    outStates.assign(static_cast<std::size_t>(blockCount), LalrState{});
    for (std::size_t stateId = 0; stateId < lr1States.size(); ++stateId)
    {
        LalrState &target = outStates[static_cast<std::size_t>(
            blockOf[stateId])];
        for (const Lr1Item &item : lr1States[stateId])
        {
            target.items[LrItem{item.lhs, item.prod, item.dot}].insert(
                item.lookahead);
        }
    }
    outTransitions.clear();
    for (const auto &entry : lr1Transitions)
    {
        const int from =
            blockOf[static_cast<std::size_t>(entry.first.first)];
        const int to = blockOf[static_cast<std::size_t>(entry.second)];
        outTransitions[std::make_pair(from, entry.first.second)] = to;
    }
}

/**
 * @brief 把划分细化到对 goto 稳定（同块状态的每个符号后继必须在同一块）。
 *
 * 这是 Moore 式的 partition refinement：只有保证「同块状态的后继也同块」，
 * 块级 goto 才是良定义的，合并出来的 lookahead 才等于该块内 LR(1) 子状态
 * lookahead 的并集。
 * @param lr1States 规范 LR(1) 项集族。
 * @param lr1Transitions 规范 LR(1) 转移。
 * @param blockOf 输入 / 输出：每个 LR(1) 状态所属块。
 * @param blockCount 输入 / 输出：块数量。
 */
void stabilizePartition(
    const std::vector<std::set<Lr1Item>> &lr1States,
    const std::map<std::pair<int, std::string>, int> &lr1Transitions,
    std::vector<int> &blockOf, int &blockCount)
{
    std::vector<std::vector<std::pair<std::string, int>>> successors(
        lr1States.size());
    for (const auto &entry : lr1Transitions)
    {
        successors[static_cast<std::size_t>(entry.first.first)].push_back(
            {entry.first.second, entry.second});
    }
    bool changed = true;
    while (changed)
    {
        changed = false;
        std::map<int, std::vector<int>> members;
        for (std::size_t stateId = 0; stateId < blockOf.size(); ++stateId)
        {
            members[blockOf[stateId]].push_back(static_cast<int>(stateId));
        }
        for (const auto &entry : members)
        {
            if (entry.second.size() <= 1)
            {
                continue;
            }
            std::map<std::vector<std::pair<std::string, int>>,
                     std::vector<int>>
                groups;
            for (const int stateId : entry.second)
            {
                std::vector<std::pair<std::string, int>> key;
                for (const auto &successor : successors[static_cast<std::size_t>(
                         stateId)])
                {
                    key.push_back({successor.first,
                                   blockOf[static_cast<std::size_t>(
                                       successor.second)]});
                }
                groups[key].push_back(stateId);
            }
            if (groups.size() <= 1)
            {
                continue;
            }
            changed = true;
            bool first = true;
            for (const auto &group : groups)
            {
                const int id = first ? entry.first : blockCount++;
                first = false;
                for (const int stateId : group.second)
                {
                    blockOf[static_cast<std::size_t>(stateId)] = id;
                }
            }
        }
    }
}

/**
 * @brief 按「该 Token 上启用的归约项集合」拆分一个块。
 * @param blockOf 输入 / 输出：每个 LR(1) 状态所属块。
 * @param blockCount 输入 / 输出：块数量。
 * @param lr1States 规范 LR(1) 项集族。
 * @param block 要拆分的块编号。
 * @param token 触发拆分的 Token。
 * @param rules 规则表。
 * @return 发生了拆分返回 true；无法再拆返回 false。
 */
bool splitBlockOnToken(std::vector<int> &blockOf, int &blockCount,
                       const std::vector<std::set<Lr1Item>> &lr1States,
                       int block, const std::string &token,
                       const RuleMap &rules)
{
    std::vector<int> members;
    for (std::size_t stateId = 0; stateId < blockOf.size(); ++stateId)
    {
        if (blockOf[stateId] == block)
        {
            members.push_back(static_cast<int>(stateId));
        }
    }
    std::map<std::vector<LrItem>, std::vector<int>> groups;
    for (const int stateId : members)
    {
        groups[enabledReductions(lr1States[static_cast<std::size_t>(stateId)],
                                 token, rules)]
            .push_back(stateId);
    }
    if (groups.size() <= 1)
    {
        return false;
    }
    bool first = true;
    for (const auto &group : groups)
    {
        const int id = first ? block : blockCount++;
        first = false;
        for (const int stateId : group.second)
        {
            blockOf[static_cast<std::size_t>(stateId)] = id;
        }
    }
    return true;
}

/**
 * @brief 是否还存在未消解的归约/归约冲突。
 * @param conflicts 未消解冲突列表。
 * @return 存在返回 true。
 */
bool hasReduceReduceConflict(const std::vector<LrConflict> &conflicts)
{
    for (const LrConflict &entry : conflicts)
    {
        if (entry.kind == "归约/归约冲突")
        {
            return true;
        }
    }
    return false;
}

/**
 * @brief 取第一个未消解的归约/归约冲突。
 * @param conflicts 未消解冲突列表。
 * @return 冲突指针；不存在返回 nullptr。
 */
const LrConflict *firstReduceReduceConflict(
    const std::vector<LrConflict> &conflicts)
{
    for (const LrConflict &entry : conflicts)
    {
        if (entry.kind == "归约/归约冲突")
        {
            return &entry;
        }
    }
    return nullptr;
}

} // namespace

bool Analysis::ielrAnalyze(const Grammar &grammar,
                           const std::vector<std::string> &tokens,
                           const PriorityConfig *priority, bool teach,
                           AnalysisResult &out, Error &error)
{
    const PriorityConfig normalized = GrammarLoader::normalizePriority(priority);
    AnalysisResult &result = out;
    result.teach = teach;
    result.priority = normalized;
    std::set<std::string> nontermSet;
    prepareLrAnalysis(grammar, tokens, k_ielrStartSymbol, "ielr", result,
                      nontermSet);
    const std::string &start = result.grammar.start;

    std::vector<std::set<Lr1Item>> lr1States;
    std::map<std::pair<int, std::string>, int> lr1Transitions;
    lr1BuildAutomaton(result.grammar.rules, nontermSet, start, result.first,
                      lr1States, lr1Transitions);
    result.lr1StateCount = static_cast<int>(lr1States.size());

    std::vector<LalrState> mergedStates;
    std::vector<int> stateMap;
    lalrMergeStates(lr1States, mergedStates, stateMap);
    result.ielrLalrStateCount = static_cast<int>(mergedStates.size());
    result.hasFollow = false;

    // 划分：LR(1) 状态 -> 块编号。初始为 LALR（按 core 合并）。
    std::vector<int> blockOf = stateMap;
    int blockCount = static_cast<int>(mergedStates.size());
    std::vector<LalrState> states;
    std::map<std::pair<int, std::string>, int> transitions;
    buildQuotient(lr1States, lr1Transitions, blockOf, blockCount, states,
                  transitions);

    // 用当前划分建一次表；有未消解的归约/归约冲突就拆分再试。
    AnalysisResult candidate;
    const auto rebuild = [&]()
    {
        candidate = result;
        candidate.action.clear();
        candidate.gotoTable.clear();
        candidate.resolutions.clear();
        candidate.unresolved.clear();
        buildLalrStyleTables(states, transitions, candidate.grammar.rules,
                             candidate.terminals, candidate.nonterms,
                             candidate.grammar.start, normalized, "IELR",
                             candidate,
                             // 探测阶段不就地决策归约/归约冲突：要看见它们
                             // 才知道哪些状态该拆分
                             false);
    };
    rebuild();

    // 拆分触发原因（按拆分前的 LALR 状态编号记录，供报告使用）。
    std::vector<std::string> splitReasons(mergedStates.size());
    std::vector<std::string> splitTokens(mergedStates.size());
    std::vector<std::string> splitKinds(mergedStates.size());
    while (hasReduceReduceConflict(candidate.unresolved))
    {
        const LrConflict *conflict =
            firstReduceReduceConflict(candidate.unresolved);
        if (conflict == nullptr)
        {
            break;
        }
        const int block = conflict->state;
        int ancestor = -1;
        for (std::size_t stateId = 0; stateId < blockOf.size(); ++stateId)
        {
            if (blockOf[stateId] == block)
            {
                ancestor = stateMap[stateId];
                break;
            }
        }
        if ((ancestor >= 0) &&
            splitReasons[static_cast<std::size_t>(ancestor)].empty())
        {
            const std::vector<LrItem> items = reducedItemsInBlock(
                states[static_cast<std::size_t>(block)], conflict->token,
                candidate.grammar.rules);
            std::string detail;
            for (std::size_t index = 0; index < items.size(); ++index)
            {
                if (index != 0)
                {
                    detail += " 与 ";
                }
                detail += itemText(items[index], candidate.grammar.rules);
            }
            splitReasons[static_cast<std::size_t>(ancestor)] =
                "合并自不同 LR(1) 上下文的项在该 Token 上同时可归约（" +
                detail + "），拆分后各子状态只保留自己的 lookahead";
            splitTokens[static_cast<std::size_t>(ancestor)] = conflict->token;
            splitKinds[static_cast<std::size_t>(ancestor)] = conflict->kind;
        }
        if (!splitBlockOnToken(blockOf, blockCount, lr1States, block,
                               conflict->token, candidate.grammar.rules))
        {
            // 划分已细到规范 LR(1) 仍冲突：文法不是 LR(1)，交给最终校验报错。
            break;
        }
        stabilizePartition(lr1States, lr1Transitions, blockOf, blockCount);
        buildQuotient(lr1States, lr1Transitions, blockOf, blockCount, states,
                      transitions);
        rebuild();
    }

    result.lalrStates = states;
    result.transitions = transitions;
    // 拆分到头还剩下的归约/归约冲突（也就是「规范 LR(1) 都解不掉」的二义）：
    // 最后一遍按 yacc 惯例就地选一条产生式并给大警告，而不是直接报错。
    if (hasReduceReduceConflict(candidate.unresolved))
    {
        candidate.resolutions.clear();
        candidate.unresolved.clear();
        candidate.warnings.clear();
        buildLalrStyleTables(states, transitions, candidate.grammar.rules,
                             candidate.terminals, candidate.nonterms,
                             candidate.grammar.start, normalized, "IELR",
                             candidate, true);
    }
    result.action = candidate.action;
    result.gotoTable = candidate.gotoTable;
    result.resolutions = candidate.resolutions;
    result.unresolved = candidate.unresolved;

    // 汇总拆分记录：每个被拆开的 LALR 状态一行。
    std::vector<int> ancestorOfBlock(static_cast<std::size_t>(blockCount), -1);
    for (std::size_t stateId = 0; stateId < blockOf.size(); ++stateId)
    {
        ancestorOfBlock[static_cast<std::size_t>(blockOf[stateId])] =
            stateMap[stateId];
    }
    std::map<int, std::vector<int>> partsByLalr;
    for (int block = 0; block < blockCount; ++block)
    {
        partsByLalr[ancestorOfBlock[static_cast<std::size_t>(block)]]
            .push_back(block);
    }
    for (const auto &entry : partsByLalr)
    {
        if (entry.second.size() <= 1)
        {
            continue;
        }
        const std::size_t index = static_cast<std::size_t>(entry.first);
        IelrSplit split;
        split.lalrState = entry.first;
        split.parts = entry.second;
        split.token = splitTokens[index];
        split.kind = splitKinds[index].empty() ? "归约/归约冲突"
                                               : splitKinds[index];
        split.reason = splitReasons[index].empty()
                           ? "为保持 goto 转移一致，随相关状态一起细分"
                           : splitReasons[index];
        result.ielrSplits.push_back(split);
    }

    if (!checkUnresolvedConflictsForMode("IELR(1)", result, result.teach,
                                         error))
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

std::string formatIelrReport(const AnalysisResult &analysis)
{
    std::map<std::string, int> order;
    for (std::size_t index = 0; index < analysis.terminals.size(); ++index)
    {
        order[analysis.terminals[index]] = static_cast<int>(index);
    }
    order[k_epsilon] = static_cast<int>(order.size());
    std::vector<std::string> lines;
    lines.push_back(
        "[生成模式] IELR(1)：LALR 项集 + 冲突驱动的状态拆分（按需拆分，"
        "表规模接近 LALR）");
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
    lines.push_back("[IELR 自动机]");
    lines.push_back("LALR 状态数 = " +
                    std::to_string(analysis.ielrLalrStateCount) +
                    "，拆分后 IELR 状态数 = " +
                    std::to_string(analysis.lalrStates.size()) +
                    "，规范 LR(1) 状态数 = " +
                    std::to_string(analysis.lr1StateCount) +
                    "，ACTION 表项数 = " +
                    std::to_string(analysis.action.size()) +
                    "，GOTO 转移数 = " +
                    std::to_string(analysis.transitions.size()));
    lines.push_back("");
    lines.push_back("[IELR 状态拆分]");
    if (analysis.ielrSplits.empty())
    {
        lines.push_back("（未发生拆分：该文法的 LALR 表已无未消解冲突，"
                        "IELR 表与 LALR 等价）");
    }
    else
    {
        for (const IelrSplit &split : analysis.ielrSplits)
        {
            std::string parts;
            for (const int part : split.parts)
            {
                if (!parts.empty())
                {
                    parts += ", ";
                }
                parts += "I" + std::to_string(part);
            }
            lines.push_back("LALR 状态 I" + std::to_string(split.lalrState) +
                            " -> " + parts + "：Token " + split.token +
                            " 上的" + split.kind + "；" + split.reason);
        }
    }
    lines.push_back("");
    lines.push_back("[ACTION 表]");
    for (std::size_t stateId = 0; stateId < analysis.lalrStates.size();
         ++stateId)
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

std::string formatIelrAutomaton(const AnalysisResult &analysis)
{
    std::vector<std::string> lines;
    lines.push_back("[IELR 项集族]");
    for (std::size_t stateId = 0; stateId < analysis.lalrStates.size();
         ++stateId)
    {
        lines.push_back("I" + std::to_string(stateId) + ":");
        for (const auto &entry : analysis.lalrStates[stateId].items)
        {
            std::vector<std::string> rhs =
                analysis.grammar.rules.at(entry.first.lhs)
                    [static_cast<std::size_t>(entry.first.prod)]
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
    lines.push_back("");
    lines.push_back("[IELR 状态拆分]");
    if (analysis.ielrSplits.empty())
    {
        lines.push_back("（未发生拆分）");
    }
    else
    {
        for (const IelrSplit &split : analysis.ielrSplits)
        {
            std::string parts;
            for (const int part : split.parts)
            {
                if (!parts.empty())
                {
                    parts += ", ";
                }
                parts += "I" + std::to_string(part);
            }
            lines.push_back("LALR 状态 I" + std::to_string(split.lalrState) +
                            " -> " + parts + "：Token " + split.token +
                            " 上的" + split.kind + "；" + split.reason);
        }
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

std::string formatIelrTables(const AnalysisResult &analysis)
{
    return formatSlrTables(analysis);
}

} // namespace ParseEngine
