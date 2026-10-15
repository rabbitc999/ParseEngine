/**
 * @file analysis_lr0.cpp
 * @brief LR(0)：规范 LR(0) 项集族 + 「完整项在所有终结符上归约」的分析表。
 *
 * 与 SLR(1)（analysis_slr.cpp）的差别只有一处：LR(0) 归约时不看 FOLLOW，
 * 只要某个状态出现完整项 `A -> α .`，就在**所有**终结符上填归约动作。
 * 这既是 LR(0) 的定义，也是它比 SLR 弱的原因——状态里只要还有可移进的
 * 项，移进/归约冲突就一定会出现。所以 LR(0) 从不因冲突中止生成：冲突写进
 * 报告的 `[未消解冲突]`，表项按 yacc 默认继续写；`%teach` / `--teach` 只
 * 决定报告里要不要多印 `[教学提示]` 与 `[教学对照]`。
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

namespace
{

/**
 * @brief 构造 ACTION / GOTO 表（LR(0)：完整项在所有终结符上归约）。
 * @param states 项集族。
 * @param transitions GOTO 转移。
 * @param rules 规则表。
 * @param terminals 终结符列表。
 * @param nonterms 非终结符列表。
 * @param start 起始符号。
 * @param priority 优先级配置。
 * @param out 输出分析结果。
 */
void lr0BuildTables(const std::vector<SlrState> &states,
                    const std::map<std::pair<int, std::string>, int> &transitions,
                    const std::map<std::string, std::vector<Production>> &rules,
                    const std::vector<std::string> &terminals,
                    const std::vector<std::string> &nonterms,
                    const std::string &start, const PriorityConfig &priority,
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
        "LR(0)",
        [&states, &rules](int stateId)
        {
            return lr0StateText(states[static_cast<std::size_t>(stateId)].items,
                                rules);
        },
        rules, productionIds, out.action, priority, out.resolutions,
        out.unresolved, &out.warnings, resolveReduceReduce);
    for (std::size_t stateId = 0; stateId < states.size(); ++stateId)
    {
        for (const LrItem &item : states[stateId].items)
        {
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
            // LR(0) 的关键一步：不查 FOLLOW，完整项在整行填归约
            const auto prodIdIt = productionIds.find(std::make_pair(item.lhs, item.prod));
            const int *prodId =
                prodIdIt != productionIds.end() ? &prodIdIt->second : nullptr;
            for (const std::string &token : terminals)
            {
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
        appendReduceReduceWarning("LR(0)", out.resolutions, resolutionsBefore,
                                  out.warnings);
    }
}

/**
 * @brief 渲染冲突小节（[冲突消解] / [未消解冲突] + 教学模式小节）。
 * @param analysis 分析结果。
 * @param lines 输出行列表。
 */
void appendLr0ConflictSection(const AnalysisResult &analysis,
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

bool Analysis::lr0Analyze(const Grammar &grammar,
                          const std::vector<std::string> &tokens,
                          const PriorityConfig *priority, bool teach,
                          AnalysisResult &out, Error &error)
{
    const PriorityConfig normalized = GrammarLoader::normalizePriority(priority);
    AnalysisResult &result = out;
    result.teach = teach;
    std::set<std::string> nontermSet;
    prepareLrAnalysis(grammar, tokens, k_lr0StartSymbol, "lr0", result,
                      nontermSet);
    const std::string &start = result.grammar.start;
    // LR(0) 不看展望符：FOLLOW 在这里没有用武之地
    result.hasFollow = false;
    lr0BuildAutomaton(result.grammar.rules, nontermSet, start,
                      result.slrStates, result.transitions);
    lr0BuildTables(result.slrStates, result.transitions, result.grammar.rules,
                   result.terminals, result.nonterms, start, normalized,
                   result, true);
    // LR(0) 无条件是「只报不中止」：不开教学模式也照常生成
    if (!checkUnresolvedConflictsForMode("LR(0)", result, true, error))
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

std::string formatLr0Report(const AnalysisResult &analysis)
{
    std::vector<std::string> lines;
    lines.push_back(
        "[生成模式] LR(0)：规范 LR(0) 项集族 + 完整项在所有终结符上归约");
    lines.push_back("");
    lines.push_back(
        "（LR(0) 不看展望符：既不用 FIRST，也不用 FOLLOW，所以下面没有这两个小节）");
    lines.push_back("");
    lines.push_back("[最终产生式]");
    for (const Production &production : analysis.productions)
    {
        lines.push_back("P" + std::to_string(production.id) + ": " +
                        production.lhs + " -> " + joinRhs(production.rhs));
    }
    lines.push_back("");
    lines.push_back("[LR(0) 自动机]");
    lines.push_back("状态数 = " + std::to_string(analysis.slrStates.size()) +
                    "，GOTO 转移数 = " +
                    std::to_string(analysis.transitions.size()));
    lines.push_back("");
    lines.push_back("[ACTION 表]");
    for (std::size_t stateId = 0; stateId < analysis.slrStates.size(); ++stateId)
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
    appendLr0ConflictSection(analysis, lines);
    return joinLines(lines);
}

std::string formatLr0Automaton(const AnalysisResult &analysis)
{
    return formatSlrAutomaton(analysis);
}

std::string formatLr0Tables(const AnalysisResult &analysis)
{
    return formatSlrTables(analysis);
}

} // namespace ParseEngine
