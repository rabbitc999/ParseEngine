/**
 * @file analysis_llk.cpp
 * @brief LL(k)：消除直接左递归 + FIRST_k / FOLLOW_k + SELECT_k 预测分析表。
 *
 * 与 LL(1)（`analysis_ll1.cpp`）的关系就是 LR(k) 与 LR(1) 的关系：项集、栈机、
 * 语义帧、`%tree` 全都照旧，只有「按几个 Token 选产生式」这一处从
 * 一个 Token 放宽到 k 个：
 *
 * - SELECT_k(A -> α) = FIRST_k(α · FOLLOW_k(A))——α 能推出的前缀接上 FOLLOW_k
 *   的每个 k 元组再截断到 k；
 * - 表项 `M[A, u] = 产生式`（u 是长度恰好 k 的终结符串），同一个 `(A, u)`
 *   撞上两条产生式就报「文法不是 LL(k)」；
 * - k = 1 时与 `prepareGrammar`（LL(1)）等价。
 *
 * 左递归消除沿用 LL(1) 那套（直接左递归改写成循环，`transform` 字段记下
 * `left_base` / `left_loop` / `left_empty`），所以 `calc` 这种左递归文法在
 * LL(k) 下同样能生成——判定的对象是**变换之后**的文法。
 */

#include "ParseEngine/analysis.hpp"

#include <algorithm>
#include <map>
#include <set>

#include "ParseEngine/analysis_internal.hpp"
#include "ParseEngine/grammar_loader.hpp"

namespace ParseEngine
{

using namespace analysis_detail;

namespace
{

/**
 * @brief Select_k(A -> α)：α 的 k 前缀接上 FOLLOW_k(A) 得到的 k 元组集合。
 * @param production 产生式（右部）。
 * @param lhs 左部非终结符。
 * @param k 展望符长度。
 * @param nontermSet 非终结符集合。
 * @param firstK FIRST_k 表。
 * @param followK FOLLOW_k 表。
 * @return k 元组键集合。
 */
std::set<std::string> selectSetK(
    const Production &production, const std::string &lhs, int k,
    const std::set<std::string> &nontermSet,
    const std::map<std::string, std::set<std::string>> &firstK,
    const std::map<std::string, std::set<std::string>> &followK)
{
    const std::set<std::string> alphaFirst =
        lrk_detail::firstKOfSequence(production.rhs, k, nontermSet, firstK);
    std::set<std::string> result;
    const auto trailersIt = followK.find(lhs);
    const std::set<std::string> *trailers =
        trailersIt != followK.end() ? &trailersIt->second : nullptr;
    if (trailers == nullptr)
    {
        return result;
    }
    for (const std::string &tail : *trailers)
    {
        const std::set<std::string> extended =
            lrk_detail::extendLookahead(alphaFirst, tail, k);
        result.insert(extended.begin(), extended.end());
    }
    return result;
}

/**
 * @brief 把产生式右部渲染成一行文本（空右部写成 %empty）。
 * @param rhs 右部符号列表。
 * @return 文本。
 */
std::string rhsText(const std::vector<std::string> &rhs)
{
    return rhs.empty() ? std::string("%empty") : joinRhs(rhs);
}

} // namespace

bool Analysis::llkAnalyze(const Grammar &grammar,
                          const std::vector<std::string> &tokens,
                          int lookaheadK, bool teach, AnalysisResult &out,
                          Error &error)
{
    AnalysisResult &result = out;
    result.teach = teach;
    // k = 0 就是 LL(0)：不靠 lookahead 选产生式（每个非终结符只能有一条）
    const int k = std::max(0, lookaheadK);
    result.lookaheadK = k;
    result.mode = (k == 0) ? "ll0" : "llk";
    result.grammar = eliminateDirectLeftRecursion(grammar, error);
    if (!error.empty())
    {
        return false;
    }
    if (!checkIndirectLeftRecursion(result.grammar.rules,
                                    result.grammar.start, error))
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
    std::set<std::string> nontermSet;
    for (const auto &rule : result.grammar.rules)
    {
        result.nonterms.push_back(rule.first);
        nontermSet.insert(rule.first);
    }
    result.first = computeFirst(result.grammar.rules, result.terminals,
                                result.nonterms);
    result.follow = computeFollow(result.grammar.start, result.grammar.rules,
                                  result.terminals, result.nonterms,
                                  result.first);
    result.hasFollow = true;
    result.firstK = lrk_detail::computeFirstK(
        result.grammar.rules, result.terminals, result.nonterms, k);
    result.followK = lrk_detail::computeFollowK(
        result.grammar.start, result.grammar.rules, result.nonterms, k,
        result.firstK);
    std::map<std::pair<std::string, std::string>, int> table;
    std::set<std::string> columns;
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
            const std::set<std::string> selected = selectSetK(
                entry, lhs, k, nontermSet, result.firstK, result.followK);
            for (const std::string &token : selected)
            {
                const auto key = std::make_pair(lhs, token);
                const auto existing = table.find(key);
                if (existing != table.end())
                {
                    const Production &previous =
                        result.productions[static_cast<std::size_t>(
                            existing->second)];
                    fail(error,
                         "文法不是 LL(" + std::to_string(k) + ")：M[" + lhs +
                             ", " +
                             (token.empty() ? std::string("ε")
                                            : lookaheadKeyText(token)) +
                             "] 同时对应 " +
                             previous.lhs + " -> " + rhsText(previous.rhs) +
                             " 和 " + lhs + " -> " + rhsText(entry.rhs));
                    return false;
                }
                table[key] = prodId;
                result.table[key] = prodId;
                if (k >= 2)
                {
                    columns.insert(token);
                }
            }
        }
    }
    // k >= 2 时预测分析表的列是 k 元组：按字典序定列号，产物按同一顺序发表
    result.lookaheadColumns.clear();
    result.lookaheadColumnIndex.clear();
    for (const std::string &key : columns)
    {
        result.lookaheadColumnIndex[key] =
            static_cast<int>(result.lookaheadColumns.size());
        result.lookaheadColumns.push_back(lookaheadKeySymbols(key));
    }
    return true;
}

std::string formatLlkReport(const AnalysisResult &analysis)
{
    const int k = std::max(0, analysis.lookaheadK);
    const bool zero = (k == 0);
    std::map<std::string, int> order;
    for (std::size_t index = 0; index < analysis.terminals.size(); ++index)
    {
        order[analysis.terminals[index]] = static_cast<int>(index);
    }
    order[k_epsilon] = static_cast<int>(order.size());
    std::vector<std::string> lines;
    if (zero)
    {
        lines.push_back(
            "[生成模式] LL(0)：消除直接左递归 + 不看 lookahead 的预测分析表"
            "（每个非终结符只能有一条产生式——这是 LL(0) 的定义：零个展望符"
            "推不出任何选择信息）");
    }
    else
    {
        lines.push_back(
            "[生成模式] LL(" + std::to_string(k) + ")：消除直接左递归 + FIRST_" +
            std::to_string(k) + " / FOLLOW_" + std::to_string(k) +
            " + SELECT_" + std::to_string(k) + " 预测分析表");
    }
    lines.push_back("");
    lines.push_back("[最终产生式]");
    for (const Production &production : analysis.productions)
    {
        std::string transform;
        if (!production.transform.empty())
        {
            transform = "   [" + production.transform + "]";
        }
        lines.push_back("P" + std::to_string(production.id) + ": " +
                        production.lhs + " -> " +
                        (production.rhs.empty() ? std::string("%empty")
                                                : joinRhs(production.rhs)) +
                        transform);
    }
    lines.push_back("");
    lines.push_back("[FIRST]");
    for (const std::string &name : analysis.nonterms)
    {
        const auto valuesIt = analysis.first.find(name);
        const std::set<std::string> *values =
            valuesIt != analysis.first.end() ? &valuesIt->second : nullptr;
        std::string joined;
        if (values != nullptr)
        {
            for (const std::string &item : orderedTokens(*values, order))
            {
                if (!joined.empty())
                {
                    joined += ", ";
                }
                joined += (item == k_epsilon) ? "%empty" : item;
            }
        }
        lines.push_back("FIRST(" + name + ") = { " + joined + " }");
    }
    lines.push_back("");
    lines.push_back("[FIRST_" + std::to_string(k) + "]");
    for (const std::string &name : analysis.nonterms)
    {
        const auto valuesIt = analysis.firstK.find(name);
        const std::set<std::string> *values =
            valuesIt != analysis.firstK.end() ? &valuesIt->second : nullptr;
        lines.push_back("FIRST_" + std::to_string(k) + "(" + name + ") = { " +
                        ((values == nullptr) ? std::string()
                                             : lookaheadSetText(*values)) +
                        " }");
    }
    lines.push_back("");
    lines.push_back("[FOLLOW_" + std::to_string(k) + "]");
    for (const std::string &name : analysis.nonterms)
    {
        const auto valuesIt = analysis.followK.find(name);
        const std::set<std::string> *values =
            valuesIt != analysis.followK.end() ? &valuesIt->second : nullptr;
        lines.push_back("FOLLOW_" + std::to_string(k) + "(" + name +
                        ") = { " +
                        ((values == nullptr) ? std::string()
                                             : lookaheadSetText(*values)) +
                        " }");
    }
    lines.push_back("");
    lines.push_back("[SELECT_" + std::to_string(k) + " 预测分析表]");
    for (const std::string &name : analysis.nonterms)
    {
        for (const auto &entry : analysis.table)
        {
            if (entry.first.first != name)
            {
                continue;
            }
            const Production &production =
                analysis.productions[static_cast<std::size_t>(entry.second)];
            lines.push_back(
                "M[" + name + ", " +
                (entry.first.second.empty()
                     ? std::string("ε")
                     : lookaheadKeyText(entry.first.second)) +
                "] = P" + std::to_string(entry.second) + "（" + name + " -> " +
                rhsText(production.rhs) + "）");
        }
    }
    if (analysis.table.empty())
    {
        lines.push_back("（空）");
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
