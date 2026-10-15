/**
 * @file analysis_ll1.cpp
 * @brief LL(1)：消除直接左递归、FIRST/FOLLOW、预测分析表与报告。
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

namespace
{

/**
 * @brief LL(1) 选择集。
 * @param production 产生式。
 * @param lhs 左部非终结符。
 * @param terminals 终结符列表。
 * @param first FIRST 集合。
 * @param follow FOLLOW 集合。
 * @return 选择集。
 */
std::set<std::string> selectSet(
    const Production &production, const std::string &lhs,
    const std::vector<std::string> &terminals,
    const std::map<std::string, std::set<std::string>> &first,
    const std::map<std::string, std::set<std::string>> &follow)
{
    std::set<std::string> terminalSet(terminals.begin(), terminals.end());
    std::set<std::string> result;
    bool nullable = true;
    for (const std::string &symbol : production.rhs)
    {
        if (terminalSet.count(symbol))
        {
            result.insert(symbol);
            nullable = false;
            break;
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
            nullable = false;
            break;
        }
    }
    if (nullable)
    {
        result.insert(follow.at(lhs).begin(), follow.at(lhs).end());
    }
    return result;
}

/**
 * @brief 判断产生式内容是否相同（用于 Python 的 dict 相等语义）。
 * @param left 左产生式。
 * @param right 右产生式。
 * @return 相同返回 true。
 */
bool sameProduction(const Production &left, const Production &right)
{
    return (left.rhs == right.rhs) && (left.action == right.action);
}

} // namespace

namespace analysis_detail
{

/**
 * @brief 消除直接左递归，并保留语义折叠所需的元数据。
 * @param grammar 原始文法。
 * @return 变换后的文法。
 */
Grammar eliminateDirectLeftRecursion(const Grammar &grammar, Error &error)
{
    Grammar result;
    result.start = grammar.start;
    for (const auto &rule : grammar.rules)
    {
        const std::string &lhs = rule.first;
        std::vector<Production> recursive;
        std::vector<Production> base;
        for (const Production &production : rule.second)
        {
            const bool isRecursive =
                (!production.rhs.empty()) && (production.rhs[0] == lhs);
            if (isRecursive)
            {
                recursive.push_back(production);
            }
            else
            {
                bool equalToRecursive = false;
                for (const Production &candidate : rule.second)
                {
                    if ((!candidate.rhs.empty()) &&
                        (candidate.rhs[0] == lhs) &&
                        sameProduction(candidate, production))
                    {
                        equalToRecursive = true;
                        break;
                    }
                }
                if (!equalToRecursive)
                {
                    base.push_back(production);
                }
            }
        }
        if (recursive.empty())
        {
            result.rules.insert_or_assign(lhs, rule.second);
            continue;
        }
        if (base.empty())
        {
            fail(error, "文法错误：" + lhs +
                           " 只有直接左递归产生式，缺少基础产生式");
            return {};
        }
        std::string tail = lhs + "__tail";
        while (grammar.rules.count(tail) || result.rules.count(tail))
        {
            tail += "_";
        }
        std::vector<Production> lhsProductions;
        for (const Production &production : base)
        {
            Production transformed = production;
            transformed.rhs.push_back(tail);
            transformed.transform = "left_base";
            transformed.baseRhsLength =
                static_cast<int>(production.rhs.size());
            lhsProductions.push_back(transformed);
        }
        result.rules.insert_or_assign(lhs, lhsProductions);
        std::vector<Production> tailProductions;
        for (const Production &production : recursive)
        {
            Production transformed = production;
            transformed.rhs.erase(transformed.rhs.begin());
            transformed.rhs.push_back(tail);
            transformed.transform = "left_continue";
            transformed.owner = lhs;
            tailProductions.push_back(transformed);
        }
        Production empty;
        empty.action = "";
        empty.transform = "left_empty";
        empty.owner = lhs;
        tailProductions.push_back(empty);
        result.rules.insert_or_assign(tail, tailProductions);
    }
    return result;
}

/**
 * @brief 检查含可空前缀的左角图，拒绝间接左递归。
 * @param rules 规则表。
 * @param start 起始符号（仅用于构造空文法时保持一致）。
 */
bool checkIndirectLeftRecursion(
    const std::map<std::string, std::vector<Production>> &rules,
    const std::string &start, Error &error)
{
    (void)start;
    std::set<std::string> nonterms;
    for (const auto &rule : rules)
    {
        nonterms.insert(rule.first);
    }
    std::set<std::string> nullable;
    bool changed = true;
    while (changed)
    {
        changed = false;
        for (const auto &rule : rules)
        {
            for (const Production &production : rule.second)
            {
                bool allNullable = true;
                for (const std::string &symbol : production.rhs)
                {
                    if (!nullable.count(symbol))
                    {
                        allNullable = false;
                        break;
                    }
                }
                if (allNullable)
                {
                    if (!nullable.count(rule.first))
                    {
                        nullable.insert(rule.first);
                        changed = true;
                    }
                    break;
                }
            }
        }
    }
    std::map<std::string, std::vector<std::string>> graph;
    for (const auto &rule : rules)
    {
        graph.insert_or_assign(rule.first, std::vector<std::string>{});
    }
    for (const auto &rule : rules)
    {
        for (const Production &production : rule.second)
        {
            for (const std::string &symbol : production.rhs)
            {
                if (!nonterms.count(symbol))
                {
                    break;
                }
                graph[rule.first].push_back(symbol);
                if (!nullable.count(symbol))
                {
                    break;
                }
            }
        }
    }
    // 深度优先检查是否存在回到当前访问路径的边；检出环之后就不用再扫了，
    // 失败一路返回（错误槽只记第一个错，报出来的还是最先发现的那处）
    std::function<bool(const std::string &, std::vector<std::string> &)> visit =
        [&](const std::string &name, std::vector<std::string> &path) -> bool
    {
        const auto found = std::find(path.begin(), path.end(), name);
        if (found != path.end())
        {
            std::vector<std::string> cycle(found, path.end());
            cycle.push_back(name);
            std::string joined;
            for (std::size_t index = 0; index < cycle.size(); ++index)
            {
                if (index != 0)
                {
                    joined += " -> ";
                }
                joined += cycle[index];
            }
            fail(error, "文法包含间接左递归：" + joined);
            return false;
        }
        for (const std::string &target : graph.at(name))
        {
            std::vector<std::string> nextPath = path;
            nextPath.push_back(name);
            if (!visit(target, nextPath))
            {
                return false;
            }
        }
        return true;
    };
    for (const auto &rule : rules)
    {
        std::vector<std::string> path;
        if (!visit(rule.first, path))
        {
            return false;
        }
    }
    return true;
}

} // namespace analysis_detail

bool Analysis::prepareGrammar(const Grammar &grammar,
                              const std::vector<std::string> &tokens,
                              AnalysisResult &out, Error &error)
{
    AnalysisResult &result = out;
    result.mode = "ll1";
    result.grammar = eliminateDirectLeftRecursion(grammar, error);
    if (!error.empty())
    {
        return false;
    }
    if (!checkIndirectLeftRecursion(result.grammar.rules, result.grammar.start,
                                    error))
    {
        return false;
    }
    result.terminals = uniqueInOrder([&tokens]()
    {
        std::vector<std::string> all = k_defaultTokenNames;
        all.insert(all.end(), tokens.begin(), tokens.end());
        all.push_back("END");
        return all;
    }());
    for (const auto &rule : result.grammar.rules)
    {
        result.nonterms.push_back(rule.first);
    }
    result.first = computeFirst(result.grammar.rules, result.terminals,
                                result.nonterms);
    result.follow = computeFollow(result.grammar.start, result.grammar.rules,
                                  result.terminals, result.nonterms,
                                  result.first);
    result.hasFollow = true;
    std::map<std::pair<std::string, std::string>, int> table;
    for (const auto &rule : result.grammar.rules)
    {
        const std::string &lhs = rule.first;
        for (const Production &production : rule.second)
        {
            Production entry = production;
            entry.id = static_cast<int>(result.productions.size());
            entry.lhs = lhs;
            const int prodId = entry.id;
            result.productions.push_back(entry);
            const std::set<std::string> selected =
                selectSet(entry, lhs, result.terminals, result.first,
                          result.follow);
            for (const std::string &token : selected)
            {
                const auto key = std::make_pair(lhs, token);
                const auto existing = table.find(key);
                if (existing != table.end())
                {
                    const Production &previous =
                        result.productions[static_cast<std::size_t>(
                            existing->second)];
                    std::string previousRhs;
                    for (std::size_t index = 0; index < previous.rhs.size();
                         ++index)
                    {
                        if (index != 0)
                        {
                            previousRhs += " ";
                        }
                        previousRhs += previous.rhs[index];
                    }
                    if (previousRhs.empty())
                    {
                        previousRhs = "%empty";
                    }
                    std::string entryRhs;
                    for (std::size_t index = 0; index < entry.rhs.size();
                         ++index)
                    {
                        if (index != 0)
                        {
                            entryRhs += " ";
                        }
                        entryRhs += entry.rhs[index];
                    }
                    if (entryRhs.empty())
                    {
                        entryRhs = "%empty";
                    }
                    fail(error,
                         "文法不是 LL(1)：M[" + lhs + ", " + token +
                             "] 同时对应 " + previous.lhs + " -> " +
                             previousRhs + " 和 " + lhs + " -> " + entryRhs);
                    return false;
                }
                table[key] = prodId;
                result.table[key] = prodId;
            }
        }
    }
    return true;
}

std::string formatGrammarReport(const AnalysisResult &analysis)
{
    std::map<std::string, int> order;
    for (std::size_t index = 0; index < analysis.terminals.size(); ++index)
    {
        order[analysis.terminals[index]] = static_cast<int>(index);
    }
    order[k_epsilon] = static_cast<int>(order.size());
    std::vector<std::string> lines;
    lines.push_back("[最终产生式]");
    for (const Production &production : analysis.productions)
    {
        lines.push_back("P" + std::to_string(production.id) + ": " +
                        production.lhs + " -> " +
                        joinRhs(production.rhs));
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
    lines.push_back("[预测分析表]");
    for (const std::string &name : analysis.nonterms)
    {
        for (const std::string &token : analysis.terminals)
        {
            const auto found = analysis.table.find(std::make_pair(name, token));
            if (found != analysis.table.end())
            {
                lines.push_back("M[" + name + ", " + token + "] = P" +
                                std::to_string(found->second));
            }
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

} // namespace ParseEngine
