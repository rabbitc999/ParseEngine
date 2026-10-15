/**
 * @file analysis_slr.cpp
 * @brief SLR(1)：LR(0) 项集族 + FOLLOW、ACTION/GOTO 表与报告。
 */

#include "ParseEngine/analysis.hpp"

#include <algorithm>
#include <cstdio>
#include <functional>
#include <iostream>
#include <sstream>
#include <climits>
#include <unordered_map>

#include "ParseEngine/analysis_internal.hpp"
#include "ParseEngine/grammar_loader.hpp"

namespace ParseEngine
{

using namespace analysis_detail;

namespace
{

/**
 * @brief 构造 ACTION / GOTO 表（SLR）。
 * @param states 项集族。
 * @param transitions GOTO 转移。
 * @param rules 规则表。
 * @param terminals 终结符列表。
 * @param nonterms 非终结符列表。
 * @param follow FOLLOW 集合。
 * @param start 起始符号。
 * @param priority 优先级配置。
 * @param out 输出分析结果。
 */
void slrBuildTables(const std::vector<SlrState> &states,
                    const std::map<std::pair<int, std::string>, int> &transitions,
                    const std::map<std::string, std::vector<Production>> &rules,
                    const std::vector<std::string> &terminals,
                    const std::vector<std::string> &nonterms,
                    const std::map<std::string, std::set<std::string>> &follow,
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
        "SLR",
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
            for (const std::string &token : follow.at(item.lhs))
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
        appendReduceReduceWarning("SLR", out.resolutions, resolutionsBefore,
                                  out.warnings);
    }
}

} // namespace

bool Analysis::slrAnalyze(const Grammar &grammar,
                          const std::vector<std::string> &tokens,
                          const PriorityConfig *priority, bool teach,
                          AnalysisResult &out, Error &error)
{
    const PriorityConfig normalized = GrammarLoader::normalizePriority(priority);
    AnalysisResult &result = out;
    result.teach = teach;
    result.mode = "slr";
    result.priority = normalized;
    result.grammar = augmentGrammar(grammar, k_slrStartSymbol);
    const std::string &start = result.grammar.start;
    std::vector<std::string> allTokens = k_defaultTokenNames;
    allTokens.insert(allTokens.end(), tokens.begin(), tokens.end());
    allTokens.push_back("END");
    // error 是恢复用的伪终结符：只有文法真的写了 error 才加进终结符集合
    // （加在末尾，没用 error 的文法索引不变），且必须在算 FIRST / FOLLOW 之前
    bool usesError = false;
    for (const auto &rule : result.grammar.rules)
    {
        for (const Production &production : rule.second)
        {
            for (const std::string &symbol : production.rhs)
            {
                usesError = usesError || (symbol == k_errorTokenName);
            }
        }
    }
    if (usesError)
    {
        allTokens.push_back(k_errorTokenName);
    }
    result.terminals = uniqueInOrder(allTokens);
    std::set<std::string> nontermSet;
    for (const auto &rule : result.grammar.rules)
    {
        result.nonterms.push_back(rule.first);
        nontermSet.insert(rule.first);
    }
    result.first = computeFirst(result.grammar.rules, result.terminals,
                                result.nonterms);
    result.follow = computeFollow(start, result.grammar.rules,
                                  result.terminals, result.nonterms,
                                  result.first);
    lr0BuildAutomaton(result.grammar.rules, nontermSet, start,
                      result.slrStates, result.transitions);
    slrBuildTables(result.slrStates, result.transitions, result.grammar.rules,
                   result.terminals, result.nonterms, result.follow, start,
                   normalized, result, true);
    if (!checkUnresolvedConflictsForMode("SLR(1)", result, result.teach,
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

std::string formatSlrReport(const AnalysisResult &analysis)
{
    std::map<std::string, int> order;
    for (std::size_t index = 0; index < analysis.terminals.size(); ++index)
    {
        order[analysis.terminals[index]] = static_cast<int>(index);
    }
    order[k_epsilon] = static_cast<int>(order.size());
    std::vector<std::string> lines;
    lines.push_back("[生成模式] SLR(1)：LR(0) 项集 + FOLLOW 集合");
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
    lines.push_back("[FOLLOW]");
    for (const std::string &name : analysis.nonterms)
    {
        std::string joined;
        for (const std::string &item : orderedTokens(analysis.follow.at(name), order))
        {
            if (!joined.empty())
            {
                joined += ", ";
            }
            joined += item;
        }
        lines.push_back("FOLLOW(" + name + ") = { " + joined + " }");
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

std::string formatSlrAutomaton(const AnalysisResult &analysis)
{
    std::vector<std::string> lines;
    lines.push_back("[LR(0) 项集族]");
    for (std::size_t stateId = 0; stateId < analysis.slrStates.size(); ++stateId)
    {
        lines.push_back("I" + std::to_string(stateId) + ":");
        for (const LrItem &item : analysis.slrStates[stateId].items)
        {
            lines.push_back("    " + lr0ItemText(item, analysis.grammar.rules));
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

std::string formatSlrTables(const AnalysisResult &analysis)
{
    std::vector<std::string> lines;
    std::string header = "状态  ";
    for (std::size_t index = 0; index < analysis.terminals.size(); ++index)
    {
        if (index != 0)
        {
            header += "  ";
        }
        header += analysis.terminals[index];
    }
    lines.push_back("[ACTION 表]");
    lines.push_back(header);
    const std::size_t stateCount = analysis.stateCount();
    for (std::size_t stateId = 0; stateId < stateCount; ++stateId)
    {
        std::string row;
        for (std::size_t index = 0; index < analysis.terminals.size(); ++index)
        {
            if (index != 0)
            {
                row += "  ";
            }
            const auto found = analysis.action.find(
                std::make_pair(static_cast<int>(stateId),
                               analysis.terminals[index]));
            if (found == analysis.action.end())
            {
                row += ".";
            }
            else if (found->second.kind == LrAction::Kind::Shift)
            {
                row += "s" + std::to_string(found->second.value);
            }
            else if (found->second.kind == LrAction::Kind::Reduce)
            {
                row += "r" + std::to_string(found->second.value);
            }
            else
            {
                row += "acc";
            }
        }
        char prefix[16];
        std::snprintf(prefix, sizeof(prefix), "%-4d", static_cast<int>(stateId));
        lines.push_back(std::string(prefix) + "  " + row);
    }
    std::string gotoCells;
    for (std::size_t index = 0; index < analysis.nonterms.size(); ++index)
    {
        if (index != 0)
        {
            gotoCells += "  ";
        }
        gotoCells += analysis.nonterms[index];
    }
    lines.push_back("");
    lines.push_back("[GOTO 表]");
    lines.push_back("状态  " + gotoCells);
    for (std::size_t stateId = 0; stateId < stateCount; ++stateId)
    {
        std::string row;
        for (std::size_t index = 0; index < analysis.nonterms.size(); ++index)
        {
            if (index != 0)
            {
                row += "  ";
            }
            const auto found = analysis.gotoTable.find(
                std::make_pair(static_cast<int>(stateId),
                               static_cast<int>(index)));
            row += (found == analysis.gotoTable.end())
                       ? std::string(".")
                       : std::to_string(found->second);
        }
        char prefix[16];
        std::snprintf(prefix, sizeof(prefix), "%-4d", static_cast<int>(stateId));
        lines.push_back(std::string(prefix) + "  " + row);
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

} // namespace ParseEngine
