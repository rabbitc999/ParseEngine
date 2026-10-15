/**
 * @file analysis_lr1.cpp
 * @brief LR(1)：规范 LR(1) 项集族 + 按展望符归约的分析表（教学用，不做同心合并）。
 *
 * 与 LALR(1)（analysis_lalr.cpp）的唯一区别是不合并同心状态：状态数与
 * 能力都取上界，代价是表大。教学上最有用的两个数就是「规范 LR(1) 状态数」
 * 与「LALR 同心合并后状态数」——前者减后者就是合并省下来的状态（开了
 * `%teach` / `--teach` 才会写进报告的 `[教学对照]`）。
 *
 * 与 LR(0) 一样，未消解冲突从不中止生成：只写进报告的 `[未消解冲突]`。
 */

#include "ParseEngine/analysis.hpp"

#include <algorithm>
#include <cstdio>
#include <iostream>
#include <sstream>

#include "ParseEngine/analysis_internal.hpp"
#include "ParseEngine/grammar_loader.hpp"

namespace ParseEngine
{

using namespace analysis_detail;
using namespace lr1_detail;

namespace
{

/**
 * @brief 渲染冲突小节（[冲突消解] / [未消解冲突] + 教学模式小节）。
 * @param analysis 分析结果。
 * @param lines 输出行列表。
 */
void appendLr1ConflictSection(const AnalysisResult &analysis,
                             std::vector<std::string> &lines)
{
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
    // 教学模式的 [教学提示] / [教学对照] 只在 %teach / --teach 下输出
    appendTeachingSection(analysis, lines);
}

/**
 * @brief 把行列表拼成报告文本。
 * @param lines 行列表。
 * @return 报告文本（末尾带换行）。
 */
std::string joinLines(const std::vector<std::string> &lines)
{
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

} // namespace

bool Analysis::lr1Analyze(const Grammar &grammar,
                          const std::vector<std::string> &tokens,
                          const PriorityConfig *priority, bool teach,
                          AnalysisResult &out, Error &error)
{
    const PriorityConfig normalized = GrammarLoader::normalizePriority(priority);
    AnalysisResult &result = out;
    result.teach = teach;
    std::set<std::string> nontermSet;
    prepareLrAnalysis(grammar, tokens, k_lr1StartSymbol, "lr1", result,
                      nontermSet);
    const std::string &start = result.grammar.start;
    // 规范 LR(1) 项集族：与 LALR / IELR 用的是同一份构造
    std::vector<std::set<Lr1Item>> lr1States;
    std::map<std::pair<int, std::string>, int> lr1Transitions;
    lr1BuildAutomaton(result.grammar.rules, nontermSet, start, result.first,
                      lr1States, lr1Transitions);
    result.lr1StateCount = static_cast<int>(lr1States.size());
    // 不合并：状态编号与规范 LR(1) 一一对应
    lr1ToCanonicalStates(lr1States, result.lalrStates);
    result.transitions = lr1Transitions;
    // 教学对照：同一份 LR(1) 项集族按 core 合并后还剩多少状态
    std::vector<LalrState> mergedStates;
    std::vector<int> stateMap;
    lalrMergeStates(lr1States, mergedStates, stateMap);
    result.lr1MergedStateCount = static_cast<int>(mergedStates.size());
    result.hasFollow = false;
    buildLalrStyleTables(result.lalrStates, result.transitions,
                         result.grammar.rules, result.terminals,
                         result.nonterms, start, normalized, "LR(1)", result);
    // LR(1) 与 LR(0) 一样：只报不中止，不开教学模式也照常生成
    if (!checkUnresolvedConflictsForMode("LR(1)", result, true, error))
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

std::string formatLr1Report(const AnalysisResult &analysis)
{
    std::map<std::string, int> order;
    for (std::size_t index = 0; index < analysis.terminals.size(); ++index)
    {
        order[analysis.terminals[index]] = static_cast<int>(index);
    }
    order[k_epsilon] = static_cast<int>(order.size());
    std::vector<std::string> lines;
    lines.push_back(
        "[生成模式] LR(1)：规范 LR(1) 项集 + 按项自带展望符归约（不合并同心状态）");
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
    lines.push_back("[LR(1) 自动机]");
    lines.push_back("状态数 = " + std::to_string(analysis.lalrStates.size()) +
                    "（未合并），GOTO 转移数 = " +
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
    appendLr1ConflictSection(analysis, lines);
    return joinLines(lines);
}

std::string formatLr1Automaton(const AnalysisResult &analysis)
{
    std::vector<std::string> lines;
    lines.push_back("[LR(1) 项集族]");
    for (std::size_t stateId = 0; stateId < analysis.lalrStates.size(); ++stateId)
    {
        lines.push_back("I" + std::to_string(stateId) + ":");
        for (const auto &entry : analysis.lalrStates[stateId].items)
        {
            const LrItem &core = entry.first;
            std::vector<std::string> rhs =
                analysis.grammar.rules.at(core.lhs)[static_cast<std::size_t>(
                    core.prod)].rhs;
            const std::size_t position = std::min(
                static_cast<std::size_t>(core.dot), rhs.size());
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
            // 规范 LR(1) 的项带单个展望符：一个展望符一行，就是教材写法
            for (const std::string &token : entry.second)
            {
                lines.push_back("    [" + core.lhs + " -> " + joinedRhs +
                                ", " + token + "]");
            }
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
    return joinLines(lines);
}

std::string formatLr1Tables(const AnalysisResult &analysis)
{
    return formatSlrTables(analysis);
}

} // namespace ParseEngine
