/**
 * @file analysis.cpp
 * @brief 文法分析的共享实现与模式分派。
 *
 * 各模式自己的分析与报告在 analysis_ll1.cpp / analysis_lr0.cpp /
 * analysis_slr.cpp / analysis_lalr.cpp / analysis_lr1.cpp /
 * analysis_ielr.cpp；本文件放它们共用的辅助函数
 * （FIRST/FOLLOW、LR(0) 项集构造、LR 冲突消解、报告小工具）以及
 * Analysis::analyze 的分派入口，声明见 ParseEngine/analysis_internal.hpp。
 */

#include "ParseEngine/analysis.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <cstdio>
#include <functional>
#include <iostream>
#include <sstream>
#include <unordered_map>

#include "ParseEngine/analysis_internal.hpp"
#include "ParseEngine/grammar_loader.hpp"

namespace ParseEngine
{

const std::string k_slrStartSymbol = "__slr_start";
const std::string k_lr0StartSymbol = "__lr0_start";
const std::string k_lalrStartSymbol = "__lalr_start";
const std::string k_lr1StartSymbol = "__lr1_start";
const std::string k_ielrStartSymbol = "__ielr_start";
const std::string k_glrStartSymbol = "__glr_start";

namespace analysis_detail
{

/**
 * @brief LR(0) 项集闭包。
 * @param items 初始项集。
 * @param rules 规则表。
 * @param nontermSet 非终结符集合。
 * @return 闭包项集。
 */
std::set<LrItem> lr0Closure(
    const std::set<LrItem> &items,
    const std::map<std::string, std::vector<Production>> &rules,
    const std::set<std::string> &nontermSet)
{
    std::set<LrItem> result = items;
    std::vector<LrItem> pending(items.begin(), items.end());
    while (!pending.empty())
    {
        const LrItem item = pending.back();
        pending.pop_back();
        const std::vector<std::string> &rhs =
            rules.at(item.lhs)[static_cast<std::size_t>(item.prod)].rhs;
        if (static_cast<std::size_t>(item.dot) >= rhs.size())
        {
            continue;
        }
        const std::string &symbol = rhs[static_cast<std::size_t>(item.dot)];
        if (!nontermSet.count(symbol))
        {
            continue;
        }
        for (std::size_t index = 0; index < rules.at(symbol).size(); ++index)
        {
            LrItem next{symbol, static_cast<int>(index), 0};
            if (!result.count(next))
            {
                result.insert(next);
                pending.push_back(next);
            }
        }
    }
    return result;
}

/**
 * @brief LR(0) 的 GOTO 转移。
 * @param items 当前项集。
 * @param symbol 转移符号。
 * @param rules 规则表。
 * @param nontermSet 非终结符集合。
 * @return 目标项集（空表示无转移）。
 */
std::set<LrItem> lr0Goto(
    const std::set<LrItem> &items, const std::string &symbol,
    const std::map<std::string, std::vector<Production>> &rules,
    const std::set<std::string> &nontermSet)
{
    std::set<LrItem> moved;
    for (const LrItem &item : items)
    {
        const std::vector<std::string> &rhs =
            rules.at(item.lhs)[static_cast<std::size_t>(item.prod)].rhs;
        if ((static_cast<std::size_t>(item.dot) < rhs.size()) &&
            (rhs[static_cast<std::size_t>(item.dot)] == symbol))
        {
            moved.insert({item.lhs, item.prod, item.dot + 1});
        }
    }
    if (moved.empty())
    {
        return {};
    }
    return lr0Closure(moved, rules, nontermSet);
}

/**
 * @brief 构造 LR(0) 项集族（广度优先，状态编号稳定）。
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
    std::map<std::pair<int, std::string>, int> &outTransitions)
{
    const std::set<LrItem> startState =
        lr0Closure({{start, 0, 0}}, rules, nontermSet);
    outStates.clear();
    outStates.push_back({startState});
    std::map<std::set<LrItem>, int> stateIds;
    stateIds[startState] = 0;
    std::vector<int> pending{0};
    while (!pending.empty())
    {
        const int stateId = pending.front();
        pending.erase(pending.begin());
        std::vector<std::string> symbols;
        for (const LrItem &item : outStates[static_cast<std::size_t>(stateId)].items)
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
            const std::set<LrItem> target =
                lr0Goto(outStates[static_cast<std::size_t>(stateId)].items,
                        symbol, rules, nontermSet);
            if (target.empty())
            {
                continue;
            }
            if (!stateIds.count(target))
            {
                const int newId = static_cast<int>(outStates.size());
                stateIds[target] = newId;
                outStates.push_back({target});
                pending.push_back(newId);
            }
            outTransitions[std::make_pair(stateId, symbol)] =
                stateIds[target];
        }
    }
}

/**
 * @brief 把 LR(0) 项渲染成 lhs -> a . b 的可读文本。
 * @param item 项。
 * @param rules 规则表。
 * @return 可读文本。
 */
std::string lr0ItemText(
    const LrItem &item,
    const std::map<std::string, std::vector<Production>> &rules)
{
    std::vector<std::string> rhs =
        rules.at(item.lhs)[static_cast<std::size_t>(item.prod)].rhs;
    const std::size_t position =
        std::min(static_cast<std::size_t>(item.dot), rhs.size());
    rhs.insert(rhs.begin() + static_cast<std::ptrdiff_t>(position), ".");
    std::string joined;
    for (std::size_t index = 0; index < rhs.size(); ++index)
    {
        if (index != 0)
        {
            joined += " ";
        }
        joined += rhs[index];
    }
    return "(" + item.lhs + " -> " + joined + ")";
}

/**
 * @brief 把整个 LR(0) 项集渲染成一行文本，用于冲突诊断。
 * @param stateItems 项集。
 * @param rules 规则表。
 * @return 一行文本。
 */
std::string lr0StateText(
    const std::set<LrItem> &stateItems,
    const std::map<std::string, std::vector<Production>> &rules)
{
    std::string joined;
    for (const LrItem &item : stateItems)
    {
        if (!joined.empty())
        {
            joined += ", ";
        }
        joined += lr0ItemText(item, rules);
    }
    return "[" + joined + "]";
}

/**
 * @brief 按列表顺序去重（对应 Python 的 list(dict.fromkeys(...))）。
 * @param values 输入列表。
 * @return 去重后的列表。
 */
std::vector<std::string> uniqueInOrder(const std::vector<std::string> &values)
{
    std::vector<std::string> result;
    for (const std::string &value : values)
    {
        if (std::find(result.begin(), result.end(), value) == result.end())
        {
            result.push_back(value);
        }
    }
    return result;
}

/**
 * @brief 计算某非终结符的 FIRST 集合（含 EPS 表示可空）。
 * @param rules 规则表。
 * @param terminals 终结符列表。
 * @param nonterms 非终结符列表。
 * @return 非终结符到 FIRST 集合的映射。
 */
std::map<std::string, std::set<std::string>> computeFirst(
    const std::map<std::string, std::vector<Production>> &rules,
    const std::vector<std::string> &terminals,
    const std::vector<std::string> &nonterms)
{
    std::set<std::string> terminalSet(terminals.begin(), terminals.end());
    std::map<std::string, std::set<std::string>> first;
    for (const std::string &name : nonterms)
    {
        first.insert_or_assign(name, std::set<std::string>{});
    }
    bool changed = true;
    while (changed)
    {
        changed = false;
        for (const auto &rule : rules)
        {
            const std::string &lhs = rule.first;
            for (const Production &production : rule.second)
            {
                const std::vector<std::string> &rhs = production.rhs;
                std::size_t index = 0;
                bool fellThrough = true;
                while (index < rhs.size())
                {
                    const std::string &symbol = rhs[index];
                    if (terminalSet.count(symbol))
                    {
                        if (!first.at(lhs).count(symbol))
                        {
                            first[lhs].insert(symbol);
                            changed = true;
                        }
                        fellThrough = false;
                        break;
                    }
                    const std::size_t before = first.at(lhs).size();
                    for (const std::string &value : first.at(symbol))
                    {
                        if (value != k_epsilon)
                        {
                            first[lhs].insert(value);
                        }
                    }
                    if (first.at(lhs).size() != before)
                    {
                        changed = true;
                    }
                    if (!first.at(symbol).count(k_epsilon))
                    {
                        fellThrough = false;
                        break;
                    }
                    ++index;
                }
                if (fellThrough && !first.at(lhs).count(k_epsilon))
                {
                    first[lhs].insert(k_epsilon);
                    changed = true;
                }
            }
        }
    }
    return first;
}

/**
 * @brief 计算每个非终结符的 FOLLOW 集合。
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
    const std::map<std::string, std::set<std::string>> &first)
{
    std::set<std::string> terminalSet(terminals.begin(), terminals.end());
    std::set<std::string> nontermSet(nonterms.begin(), nonterms.end());
    std::map<std::string, std::set<std::string>> follow;
    for (const std::string &name : nonterms)
    {
        follow.insert_or_assign(name, std::set<std::string>{});
    }
    follow[start].insert("END");
    bool changed = true;
    while (changed)
    {
        changed = false;
        for (const auto &rule : rules)
        {
            const std::string &lhs = rule.first;
            for (const Production &production : rule.second)
            {
                const std::vector<std::string> &rhs = production.rhs;
                for (std::size_t index = 0; index < rhs.size(); ++index)
                {
                    const std::string &symbol = rhs[index];
                    if (!nontermSet.count(symbol))
                    {
                        continue;
                    }
                    std::set<std::string> suffixFirst;
                    bool nullable = true;
                    for (std::size_t after = index + 1; after < rhs.size();
                         ++after)
                    {
                        const std::string &next = rhs[after];
                        if (next == k_errorTokenName)
                        {
                            // FOLLOW 里不含 error：error 是恢复用的伪终结符，
                            // 只当它是「后面还有东西、但不可空」的边界来截断传播
                            nullable = false;
                            break;
                        }
                        if (terminalSet.count(next))
                        {
                            suffixFirst.insert(next);
                            nullable = false;
                            break;
                        }
                        for (const std::string &value : first.at(next))
                        {
                            if (value != k_epsilon)
                            {
                                suffixFirst.insert(value);
                            }
                        }
                        if (!first.at(next).count(k_epsilon))
                        {
                            nullable = false;
                            break;
                        }
                    }
                    const std::size_t before = follow.at(symbol).size();
                    follow[symbol].insert(suffixFirst.begin(),
                                          suffixFirst.end());
                    if (nullable)
                    {
                        follow[symbol].insert(follow.at(lhs).begin(),
                                              follow.at(lhs).end());
                    }
                    if (follow.at(symbol).size() != before)
                    {
                        changed = true;
                    }
                }
            }
        }
    }
    return follow;
}

/**
 * @brief 给摊平后的产生式编号：{(lhs, 规则内下标): 全局编号}。
 * @param rules 规则表。
 * @return 编号映射。
 */
std::map<std::pair<std::string, int>, int> slrProductionIds(
    const std::map<std::string, std::vector<Production>> &rules)
{
    std::map<std::pair<std::string, int>, int> ids;
    for (const auto &rule : rules)
    {
        for (std::size_t index = 0; index < rule.second.size(); ++index)
        {
            ids.insert_or_assign(std::make_pair(rule.first, static_cast<int>(index)),
                    static_cast<int>(ids.size()));
        }
    }
    return ids;
}

/**
 * @brief 新增虚拟起始规则。
 * @param grammar 原始文法。
 * @param startSymbol 虚拟起始符号名。
 * @return 增广后的文法。
 */
Grammar augmentGrammar(const Grammar &grammar, const std::string &startSymbol)
{
    Grammar result;
    result.start = startSymbol;
    Production startProduction;
    startProduction.rhs.push_back(grammar.start);
    startProduction.action = "";
    result.rules.insert_or_assign(startSymbol, std::vector<Production>{startProduction});
    for (const auto &rule : grammar.rules)
    {
        result.rules.insert_or_assign(rule.first, rule.second);
    }
    return result;
}

/**
 * @brief 把 ACTION 表项渲染成可读文本。
 * @param action 动作。
 * @param rules 规则表。
 * @param productionIds 产生式编号映射。
 * @return 可读文本。
 */
std::string slrActionText(
    const LrAction &action,
    const std::map<std::string, std::vector<Production>> &rules,
    const std::map<std::pair<std::string, int>, int> &productionIds)
{
    if (action.kind == LrAction::Kind::Shift)
    {
        return "移进到状态 " + std::to_string(action.value);
    }
    if (action.kind == LrAction::Kind::Accept)
    {
        return "接受";
    }
    for (const auto &entry : productionIds)
    {
        if (entry.second == action.value)
        {
            std::string rhs;
            for (const std::string &symbol :
                 rules.at(entry.first.first)[static_cast<std::size_t>(
                     entry.first.second)].rhs)
            {
                if (!rhs.empty())
                {
                    rhs += " ";
                }
                rhs += symbol;
            }
            if (rhs.empty())
            {
                rhs = "%empty";
            }
            return "按产生式 " + std::to_string(action.value) + "（" +
                   entry.first.first + " -> " + rhs + "）归约";
        }
    }
    return "按产生式 " + std::to_string(action.value) + " 归约";
}

/**
 * @brief 按新旧动作类型给出冲突种类文本。
 * @param previous 已有动作。
 * @param value 新增动作。
 * @return 冲突种类文本。
 */
std::string slrConflictText(const LrAction &previous, const LrAction &value)
{
    bool hasShift = (previous.kind == LrAction::Kind::Shift) ||
                    (value.kind == LrAction::Kind::Shift);
    bool hasReduce = (previous.kind == LrAction::Kind::Reduce) ||
                     (value.kind == LrAction::Kind::Reduce);
    if (hasShift && hasReduce)
    {
        return "移进/归约冲突";
    }
    if (hasReduce)
    {
        return "归约/归约冲突";
    }
    if (hasShift)
    {
        return "移进/移进冲突";
    }
    return "接受动作冲突";
}

/**
 * @brief 算出每条产生式的 (结合性, 优先级)。
 * @param rules 规则表。
 * @param priority 优先级配置。
 * @return 产生式编号（规则内下标）到 (结合性, 优先级) 的映射。
 */
std::map<std::pair<std::string, int>, std::pair<std::string, int>>
computeProductionPriority(
    const std::map<std::string, std::vector<Production>> &rules,
    const PriorityConfig &priority)
{
    std::map<std::pair<std::string, int>, std::pair<std::string, int>> result;
    for (const auto &rule : rules)
    {
        const std::string &lhs = rule.first;
        for (std::size_t index = 0; index < rule.second.size(); ++index)
        {
            const Production &production = rule.second[index];
            const auto tokenIt = priority.productionPrec.find(
                std::make_pair(lhs, static_cast<int>(index)));
            const std::string *token =
                tokenIt != priority.productionPrec.end() ? &tokenIt->second : nullptr;
            std::string tokenValue;
            if (token != nullptr)
            {
                tokenValue = *token;
            }
            else
            {
                for (auto it = production.rhs.rbegin();
                     it != production.rhs.rend(); ++it)
                {
                    if (!rules.count(*it))
                    {
                        tokenValue = *it;
                        break;
                    }
                }
            }
            if (tokenValue.empty())
            {
                continue;
            }
            const auto levelIt = priority.precedence.find(tokenValue);
            const int *level =
                levelIt != priority.precedence.end() ? &levelIt->second : nullptr;
            if (level == nullptr)
            {
                continue;
            }
            const auto assocIt = priority.associativity.find(tokenValue);
            const std::string *assoc =
                assocIt != priority.associativity.end() ? &assocIt->second : nullptr;
            result[std::make_pair(lhs, static_cast<int>(index))] =
                std::make_pair(assoc != nullptr ? *assoc : std::string("left"),
                               *level);
        }
    }
    return result;
}

/**
 * @brief 按优先级 / 结合性消解一个移进/归约冲突。
 * @param previous 已有动作。
 * @param value 新增动作。
 * @param token 冲突 Token。
 * @param precedence Token 优先级表。
 * @param associativity Token 结合性表。
 * @param productionPriority 产生式编号到 (结合性, 优先级) 的映射。
 * @param outReason 输出消解理由。
 * @return "shift" / "reduce" / 空字符串（无法消解）。
 */
std::string tryResolveShiftReduce(
    const LrAction &previous, const LrAction &value, const std::string &token,
    const std::map<std::string, int> &precedence,
    const std::map<std::string, std::string> &associativity,
    const std::map<int, std::pair<std::string, int>> &productionPriority,
    std::string &outReason)
{
    const bool hasShift = (previous.kind == LrAction::Kind::Shift) ||
                          (value.kind == LrAction::Kind::Shift);
    const bool hasReduce = (previous.kind == LrAction::Kind::Reduce) ||
                           (value.kind == LrAction::Kind::Reduce);
    if (!(hasShift && hasReduce))
    {
        return "";
    }
    const LrAction &reduceAction =
        (previous.kind == LrAction::Kind::Reduce) ? previous : value;
    const int reduceId = reduceAction.value;
    const auto shiftLevelIt = precedence.find(token);
    const int *shiftLevel =
        shiftLevelIt != precedence.end() ? &shiftLevelIt->second : nullptr;
    const auto reduceConf = productionPriority.find(reduceId);
    if ((shiftLevel == nullptr) || (reduceConf == productionPriority.end()))
    {
        return "";
    }
    const std::string reduceAssoc = reduceConf->second.first;
    const int reduceLevel = reduceConf->second.second;
    const auto tokenAssocIt = associativity.find(token);
    const std::string *tokenAssoc =
        tokenAssocIt != associativity.end() ? &tokenAssocIt->second : nullptr;
    const std::string assoc =
        (tokenAssoc != nullptr) ? *tokenAssoc : reduceAssoc;
    if (*shiftLevel > reduceLevel)
    {
        outReason = "优先级（移进 " + token + " 优先级 " +
                    std::to_string(*shiftLevel) + " 高于产生式 P" +
                    std::to_string(reduceId) + " 的 " +
                    std::to_string(reduceLevel) + "）";
        return "shift";
    }
    if (*shiftLevel < reduceLevel)
    {
        outReason = "优先级（产生式 P" + std::to_string(reduceId) +
                    " 优先级 " + std::to_string(reduceLevel) + " 高于移进 " +
                    token + " 的 " + std::to_string(*shiftLevel) + "）";
        return "reduce";
    }
    if (assoc == "left")
    {
        outReason = "%left 结合性（" + token + "）";
        return "reduce";
    }
    if (assoc == "right")
    {
        outReason = "%right 结合性（" + token + "）";
        return "shift";
    }
    if (assoc == "nonassoc")
    {
        outReason = "%nonassoc 不允许链式使用（" + token + "）";
        return "none";
    }
    return "";
}

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
                            std::vector<std::string> *warnings,
                            bool resolveReduceReduce)
{
    LrRecorder recorder;
    recorder.label = label;
    recorder.rules = &rules;
    recorder.productionIds = &productionIds;
    recorder.action = &action;
    recorder.precedence = priority.precedence;
    recorder.associativity = priority.associativity;
    for (const auto &entry :
         computeProductionPriority(rules, priority))
    {
        const auto idIt = productionIds.find(entry.first);
        const int *id =
            idIt != productionIds.end() ? &idIt->second : nullptr;
        if (id != nullptr)
        {
            recorder.productionPriority[*id] = entry.second;
        }
    }
    recorder.stateText = std::move(stateText);
    recorder.resolutions = &resolutions;
    recorder.unresolved = &unresolved;
    recorder.warnings = warnings;
    recorder.resolveReduceReduce = resolveReduceReduce;
    return recorder;
}

void appendReduceReduceWarning(const std::string &label,
                               const std::vector<LrResolution> &resolutions,
                               std::size_t from,
                               std::vector<std::string> &outWarnings)
{
    std::vector<const LrResolution *> entries;
    for (std::size_t index = from; index < resolutions.size(); ++index)
    {
        if (resolutions[index].kind == "归约/归约冲突")
        {
            entries.push_back(&resolutions[index]);
        }
    }
    if (entries.empty())
    {
        return;
    }
    const std::size_t shown = std::min<std::size_t>(8u, entries.size());
    std::string banner;
    banner +=
        "================================================================\n";
    banner += "归约/归约冲突！" + label + " 分析表里有 " +
              std::to_string(entries.size()) +
              " 处归约/归约冲突，已自动选择一条产生式继续生成：\n";
    for (std::size_t index = 0; index < shown; ++index)
    {
        const LrResolution &entry = *entries[index];
        banner += "  · 状态 " + std::to_string(entry.state) + "，Token " +
                  entry.token + "：选 " + entry.chosen;
        if (!entry.discarded.empty())
        {
            banner += "，弃 " + entry.discarded;
        }
        if (!entry.reason.empty())
        {
            banner += "（" + entry.reason + "）";
        }
        banner += "\n";
    }
    if (entries.size() > shown)
    {
        banner += "  · 还有 " + std::to_string(entries.size() - shown) +
                  " 处同类冲突，详见产物 RESULT.md 的「冲突消解」一节\n";
    }
    banner +=
        "  这说明文法在这里是二义的（或对 " + label +
        " 二义），解析结果取决于这些选择；换 k 或换模式可能换一条产生式。\n";
    banner +=
        "  想自己拍板就给产生式加 %prec（优先级高的胜出），或者改写文法把两种"
        "情况分开。\n";
    banner +=
        "================================================================";
    outWarnings.push_back(banner);
}

/**
 * @brief %expect 语义：允许保留预期数量的移进/归约冲突，其余一律报错。
 * @param label 模式标签。
 * @param unresolved 未消解冲突列表。
 * @param expectConflicts %expect 值；可为空。
 */
bool checkUnresolvedConflicts(const std::string &label,
                              const std::vector<LrConflict> &unresolved,
                              const std::optional<int> &expectConflicts,
                              Error &error)
{
    if (unresolved.empty())
    {
        return true;
    }
    for (const LrConflict &item : unresolved)
    {
        if (item.kind == "归约/归约冲突")
        {
            fail(error, item.text);
            return false;
        }
    }
    if (!expectConflicts.has_value())
    {
        fail(error, unresolved[0].text);
        return false;
    }
    if (static_cast<int>(unresolved.size()) != expectConflicts.value())
    {
        fail(error,
             label + "：%expect " + std::to_string(expectConflicts.value()) +
                 " 与实际未消解冲突数 " + std::to_string(unresolved.size()) +
                 " 不一致；" + unresolved[0].text);
        return false;
    }
    std::string joined;
    for (std::size_t index = 0; index < unresolved.size(); ++index)
    {
        if (index != 0)
        {
            joined += "；";
        }
        joined += unresolved[index].text;
    }
    std::cerr << label << "：" << unresolved.size()
              << " 个未消解冲突与 %expect " << expectConflicts.value()
              << " 一致，移进/归约冲突按移进优先继续生成；" << joined
              << std::endl;
    return true;
}

/**
 * @brief 教学模式的冲突提示语（LR(0) / LR(1) 报告用）。
 * @param label 模式标签。
 * @param unresolved 未消解冲突列表。
 * @return 提示语；没有冲突时返回空串。
 */
std::string teachingConflictNote(const std::string &label,
                                 const std::vector<LrConflict> &unresolved)
{
    if (unresolved.empty())
    {
        return std::string();
    }
    int shiftReduce = 0;
    int reduceReduce = 0;
    for (const LrConflict &item : unresolved)
    {
        if (item.kind == "归约/归约冲突")
        {
            ++reduceReduce;
        }
        else
        {
            ++shiftReduce;
        }
    }
    std::string note = "该文法不是 " + label + " 文法：表里有 " +
                       std::to_string(unresolved.size()) +
                       " 处未消解冲突（移进/归约 " +
                       std::to_string(shiftReduce) + " 处，归约/归约 " +
                       std::to_string(reduceReduce) +
                       " 处）。教学模式不中止生成：移进/归约取移进"
                       "（yacc 默认），归约/归约取先到的归约；生成的解析器"
                       "在这几处不再是唯一的 LR 分析。";
    return note;
}

/**
 * @brief 按模式处理未消解冲突（各模式严宽差别都收在这里）。
 * @param label 模式标签。
 * @param result 分析结果。
 * @param allowConflicts 是否允许带着未消解冲突继续生成。
 */
bool checkUnresolvedConflictsForMode(const std::string &label,
                                     const AnalysisResult &result,
                                     bool allowConflicts, Error &error)
{
    if (allowConflicts && !result.priority.expectConflicts.has_value())
    {
        // 放行：冲突不中止生成，写进报告交给读者判断
        return true;
    }
    return checkUnresolvedConflicts(label, result.unresolved,
                                    result.priority.expectConflicts, error);
}

/**
 * @brief 追加教学模式的 `[教学提示]` 与 `[教学对照]` 小节。
 * @param analysis 分析结果。
 * @param lines 输出行列表。
 */
void appendTeachingSection(const AnalysisResult &analysis,
                           std::vector<std::string> &lines)
{
    if (!analysis.teach)
    {
        return;
    }
    const std::string label = modeLabelText(analysis.mode);
    lines.push_back("");
    lines.push_back("[教学提示]");
    if (analysis.unresolved.empty())
    {
        lines.push_back("该文法是 " + label +
                        " 文法：表里没有未消解冲突，解析动作唯一确定。");
    }
    else
    {
        lines.push_back(teachingConflictNote(label, analysis.unresolved));
    }
    lines.push_back("");
    lines.push_back("[教学对照]");
    const auto count = [&lines](const std::string &name, std::size_t value)
    {
        lines.push_back(name + " = " + std::to_string(value));
    };
    if (analysis.mode == "lr0")
    {
        count("LR(0) 状态数", analysis.slrStates.size());
        count("LR(0) 表未消解冲突数", analysis.unresolved.size());
    }
    else if (analysis.mode == "slr")
    {
        count("LR(0) 状态数", analysis.slrStates.size());
        count("SLR(1) 表未消解冲突数", analysis.unresolved.size());
    }
    else if (analysis.mode == "lalr")
    {
        count("规范 LR(1) 状态数", static_cast<std::size_t>(
                                        analysis.lr1StateCount));
        count("LALR 状态数", analysis.lalrStates.size());
        count("LALR 表未消解冲突数", analysis.unresolved.size());
    }
    else if (analysis.mode == "lr1")
    {
        count("规范 LR(1) 状态数", analysis.lalrStates.size());
        count("LALR 同心合并后状态数",
              static_cast<std::size_t>(analysis.lr1MergedStateCount));
        count("LR(1) 表未消解冲突数", analysis.unresolved.size());
    }
    else if (analysis.mode == "ielr")
    {
        count("LALR 状态数（拆分前）",
              static_cast<std::size_t>(analysis.ielrLalrStateCount));
        count("IELR 状态数（拆分后）", analysis.lalrStates.size());
        count("规范 LR(1) 状态数",
              static_cast<std::size_t>(analysis.lr1StateCount));
        count("状态拆分数", analysis.ielrSplits.size());
        count("IELR 表未消解冲突数", analysis.unresolved.size());
    }
}

/**
 * @brief 按 Token 声明顺序排列集合元素。
 * @param values 待排序集合。
 * @param order Token 到序号的映射（EPS 排在最后）。
 * @return 排序后的列表。
 */
std::vector<std::string> orderedTokens(
    const std::set<std::string> &values,
    const std::map<std::string, int> &order)
{
    std::vector<std::string> result(values.begin(), values.end());
    std::sort(result.begin(), result.end(),
              [&order](const std::string &left, const std::string &right)
              {
                  const auto leftFound = order.find(left);
                  const auto rightFound = order.find(right);
                  const int leftOrder = (leftFound == order.end())
                                            ? 999999
                                            : leftFound->second;
                  const int rightOrder = (rightFound == order.end())
                                             ? 999999
                                             : rightFound->second;
                  if (leftOrder != rightOrder)
                  {
                      return leftOrder < rightOrder;
                  }
                  return left < right;
              });
    return result;
}

/**
 * @brief 把右部渲染成一行文本（空右部显示 %empty）。
 * @param rhs 右部符号。
 * @return 一行文本。
 */
std::string joinRhs(const std::vector<std::string> &rhs)
{
    if (rhs.empty())
    {
        return "%empty";
    }
    std::string joined;
    for (std::size_t index = 0; index < rhs.size(); ++index)
    {
        if (index != 0)
        {
            joined += " ";
        }
        joined += rhs[index];
    }
    return joined;
}

/**
 * @brief 把一条 ACTION 表项渲染成可读文本（与 Python 一致，归约返回 None）。
 * @param entry 动作。
 * @return 可读文本。
 */
std::string slrEntryText(const LrAction &entry)
{
    if (entry.kind == LrAction::Kind::Shift)
    {
        return "移进 " + std::to_string(entry.value);
    }
    if (entry.kind == LrAction::Kind::Accept)
    {
        return "接受";
    }
    if (entry.kind == LrAction::Kind::Reduce)
    {
        return "归约 P" + std::to_string(entry.value);
    }
    return "None";
}

/**
 * @brief LR 分析的公共准备：增广文法、终结符/非终结符列表、FIRST 集合。
 * @param grammar 原始文法。
 * @param tokens 文法声明的 Token。
 * @param startSymbol 虚拟起始符号。
 * @param mode 模式名。
 * @param out 输出分析结果。
 * @param outNontermSet 输出非终结符集合。
 */
void prepareLrAnalysis(const Grammar &grammar,
                       const std::vector<std::string> &tokens,
                       const std::string &startSymbol,
                       const std::string &mode, AnalysisResult &out,
                       std::set<std::string> &outNontermSet)
{
    out.mode = mode;
    out.grammar = augmentGrammar(grammar, startSymbol);
    std::vector<std::string> allTokens = k_defaultTokenNames;
    allTokens.insert(allTokens.end(), tokens.begin(), tokens.end());
    allTokens.push_back("END");
    // error 是恢复用的伪终结符：只有文法真的写了 error 才把它加进终结符集合
    // （加在末尾，所以没用 error 的文法索引完全不变），且必须在算 FIRST 之前。
    bool usesError = false;
    for (const auto &rule : out.grammar.rules)
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
    out.terminals = uniqueInOrder(allTokens);
    outNontermSet.clear();
    out.nonterms.clear();
    for (const auto &rule : out.grammar.rules)
    {
        out.nonterms.push_back(rule.first);
        outNontermSet.insert(rule.first);
    }
    out.first = computeFirst(out.grammar.rules, out.terminals, out.nonterms);
}

} // namespace analysis_detail

bool isLrMode(const std::string &mode)
{
    return (mode == "lr0") || (mode == "slr") || (mode == "lalr") ||
           (mode == "lr1") || (mode == "ielr") || (mode == "glr") ||
           (mode == "earley") || (mode == "cyk") ||
           (mode == "lrk") || (mode == "lalrk") || (mode == "slrk");
}

bool isGlrMode(const std::string &mode)
{
    return (mode == "glr") || (mode == "earley") || (mode == "cyk");
}

bool isDdMode(const std::string &mode) { return mode == "dd"; }

bool isTreeDrawMode(const std::string &mode)
{
    return (mode == "lalr") || (mode == "lr1") || (mode == "ielr");
}

bool isLexMode(const std::string &mode) { return mode == "lex"; }

std::string modeLabelText(const std::string &mode)
{
    if (mode == "lex")
    {
        // 词法模式没有分析层，标签只在产物注释里出现（与其它模式的风格一致）
        return "LEX";
    }
    if (mode == "lr0")
    {
        return "LR(0)";
    }
    if (mode == "slr")
    {
        return "SLR(1)";
    }
    if (mode == "lalr")
    {
        return "LALR(1)";
    }
    if (mode == "lr1")
    {
        return "LR(1)";
    }
    if (mode == "ielr")
    {
        return "IELR(1)";
    }
    if (mode == "glr")
    {
        return "GLR";
    }
    if (mode == "earley")
    {
        return "Earley";
    }
    if (mode == "cyk")
    {
        return "CYK";
    }
    if (mode == "dd")
    {
        return "DD";
    }
    if (mode == "lrk")
    {
        return "LR(k)";
    }
    if (mode == "lalrk")
    {
        return "LALR(k)";
    }
    if (mode == "slrk")
    {
        return "SLR(k)";
    }
    if (mode == "llk")
    {
        return "LL(k)";
    }
    if (mode == "ll0")
    {
        return "LL(0)";
    }
    return "LL(1)";
}

std::string modeNameText(const std::string &mode)
{
    if (mode == "lr0")
    {
        return "LR0";
    }
    if (mode == "slr")
    {
        return "SLR";
    }
    if (mode == "lalr")
    {
        return "LALR";
    }
    if (mode == "lr1")
    {
        return "LR1";
    }
    if (mode == "ielr")
    {
        return "IELR";
    }
    if (mode == "glr")
    {
        return "GLR";
    }
    if (mode == "earley")
    {
        return "EARLEY";
    }
    if (mode == "cyk")
    {
        return "CYK";
    }
    if (mode == "dd")
    {
        return "DD";
    }
    if (mode == "lrk")
    {
        return "LRK";
    }
    if (mode == "lalrk")
    {
        return "LALRK";
    }
    if (mode == "slrk")
    {
        return "SLRK";
    }
    if (mode == "llk")
    {
        return "LLK";
    }
    if (mode == "ll0")
    {
        return "LL0";
    }
    if (mode == "lex")
    {
        return "LEX";
    }
    return "LL";
}

/**
 * @brief 把一条冲突消解记录渲染成一行文本。
 * @param entry 消解记录。
 * @return 一行文本。
 */
std::string resolutionLine(const LrResolution &entry)
{
    return "状态 " + std::to_string(entry.state) + "，Token " + entry.token +
           "：" + entry.kind + "，按" + entry.reason + "选择" + entry.chosen;
}

bool normalizeModeName(const std::string &text, std::string &outMode,
                       int &outLookahead)
{
    outLookahead = -1;
    // 去掉空白、统一成大写，再拆出「名字」与「括号里的 k」
    std::string name;
    for (const char ch : text)
    {
        if ((ch == ' ') || (ch == '\t'))
        {
            continue;
        }
        name += static_cast<char>(
            std::toupper(static_cast<unsigned char>(ch)));
    }
    std::string inner;
    const std::size_t open = name.find('(');
    if (open != std::string::npos)
    {
        const std::size_t close = name.find(')', open);
        if (close == std::string::npos)
        {
            return false;
        }
        inner = name.substr(open + 1, close - open - 1);
        if (close + 1 != name.size())
        {
            return false;
        }
        name = name.substr(0, open);
    }
    // 也接受不带括号、直接把 k 贴在后面的写法：LL2 / LR1 / SLR3 / LALR2 / LL12
    if (inner.empty() && (name.size() > 1))
    {
        std::size_t digits = 0u;
        while ((digits < name.size()) &&
               (name[name.size() - 1u - digits] >= '0') &&
               (name[name.size() - 1u - digits] <= '9'))
        {
            ++digits;
        }
        if ((digits > 0u) && (digits < name.size()))
        {
            inner = name.substr(name.size() - digits);
            name = name.substr(0, name.size() - digits);
        }
    }
    // 括号里（或贴在后面）的 k 是十进制非负整数；没有策略上限，只受 int 限制。
    // 这里手工累加而不是 std::stoll：生成器自身不开异常（`-fno-exceptions`
    // 也要能构建），而 stoll 在超范围时会抛 out_of_range；逐位累加并在
    // 每一步检查 int 上限，既能拦住溢出、也不必引入异常。
    const auto parseInlineK = [](const std::string &text, int &out) -> bool
    {
        if (text.empty())
        {
            return false;
        }
        long long value = 0;
        for (const char ch : text)
        {
            if ((ch < '0') || (ch > '9'))
            {
                return false;
            }
            value = value * 10 + static_cast<long long>(ch - '0');
            if (value >
                static_cast<long long>((std::numeric_limits<int>::max)()))
            {
                return false;
            }
        }
        out = static_cast<int>(value);
        return true;
    };
    const auto withLookahead = [&outMode, &outLookahead, &inner, &parseInlineK](
                                   const std::string &mode, int plainK,
                                   const std::string &kMode) -> bool
    {
        if (inner.empty())
        {
            outMode = mode;
            return true;
        }
        if (inner == "K")
        {
            outMode = kMode;
            return true;
        }
        int k = 0;
        if (!parseInlineK(inner, k))
        {
            return false;
        }
        if (k == plainK)
        {
            outMode = mode;
            return true;
        }
        outMode = kMode;
        outLookahead = k;
        return true;
    };
    if (name == "LL")
    {
        if (inner.empty())
        {
            outMode = "ll1";
            return true;
        }
        if (inner == "K")
        {
            outMode = "llk";
            return true;
        }
        if (inner == "0")
        {
            outMode = "ll0";
            return true;
        }
        if (inner == "1")
        {
            outMode = "ll1";
            return true;
        }
        int k = 0;
        if (parseInlineK(inner, k))
        {
            // LL(0) 是「不看 lookahead」单独一个模式，其它都是 llk 的某个 k
            outMode = (k == 0) ? "ll0" : ((k == 1) ? "ll1" : "llk");
            if (k >= 2)
            {
                outLookahead = k;
            }
            return true;
        }
        return false;
    }
    if (name == "LL0")
    {
        outMode = "ll0";
        return inner.empty() || (inner == "0");
    }
    if (name == "LL1")
    {
        outMode = "ll1";
        return inner.empty() || (inner == "1");
    }
    if (name == "LLK")
    {
        outMode = "llk";
        return inner.empty() || (inner == "K");
    }
    if (name == "LR0")
    {
        return withLookahead("lr0", 0, "lrk");
    }
    if (name == "LR1")
    {
        return withLookahead("lr1", 1, "lrk");
    }
    if (name == "LRK")
    {
        outMode = "lrk";
        return inner.empty() || (inner == "K");
    }
    if (name == "LR")
    {
        // 光写 LR 有歧义（LR(0) 与 LR(1) 是两种东西），必须带括号或数字
        if (inner.empty())
        {
            return false;
        }
        if (inner == "K")
        {
            outMode = "lrk";
            return true;
        }
        if (inner == "0")
        {
            outMode = "lr0";
            return true;
        }
        if (inner == "1")
        {
            outMode = "lr1";
            return true;
        }
        int k = 0;
        if (parseInlineK(inner, k) && (k >= 2))
        {
            outMode = "lrk";
            outLookahead = k;
            return true;
        }
        return false;
    }
    if (name == "SLR")
    {
        return withLookahead("slr", 1, "slrk");
    }
    if (name == "SLRK")
    {
        outMode = "slrk";
        return inner.empty() || (inner == "K");
    }
    if (name == "LALR")
    {
        return withLookahead("lalr", 1, "lalrk");
    }
    if (name == "LALRK")
    {
        outMode = "lalrk";
        return inner.empty() || (inner == "K");
    }
    if (name == "IELR")
    {
        outMode = "ielr";
        return inner.empty() || (inner == "1");
    }
    // %mode glr：保留冲突到运行时的图结构栈 GLR
    if (name == "GLR")
    {
        outMode = "glr";
        return inner.empty();
    }
    if ((name == "EARLEY") || (name == "CYK"))
    {
        outMode = (name == "EARLEY") ? "earley" : "cyk";
        return inner.empty();
    }
    // %mode dd：带回溯的递归下降（不需要表，直接照文法生成解析函数）
    if (name == "DD")
    {
        outMode = "dd";
        return inner.empty();
    }
    // %mode lex：只导出词法器（不做语法分析，不需要产生式）
    if (name == "LEX")
    {
        outMode = "lex";
        return inner.empty();
    }
    return false;
}

std::vector<std::vector<std::string>> expectedTokensByRow(
    const AnalysisResult &analysis)
{
    std::vector<std::vector<std::string>> rows;
    const bool ll = (analysis.mode == "ll1") || (analysis.mode == "llk") ||
                    (analysis.mode == "ll0");
    const int k = std::max(0, analysis.lookaheadK);
    if (ll)
    {
        if (k == 0)
        {
            // LL(0) 只有 ε 一列，说不出「期望哪个 Token」
            return std::vector<std::vector<std::string>>(
                analysis.nonterms.size());
        }
        rows.assign(analysis.nonterms.size(),
                    std::vector<std::string>());
        for (const auto &entry : analysis.table)
        {
            const std::string &row = entry.first.first;
            const auto found =
                std::find(analysis.nonterms.begin(), analysis.nonterms.end(),
                          row);
            if (found == analysis.nonterms.end())
            {
                continue;
            }
            const std::size_t index =
                static_cast<std::size_t>(found - analysis.nonterms.begin());
            std::vector<std::string> symbols =
                lookaheadKeySymbols(entry.first.second);
            if (symbols.empty())
            {
                continue;
            }
            std::vector<std::string> &target = rows[index];
            if (std::find(target.begin(), target.end(), symbols.front()) ==
                target.end())
            {
                target.push_back(symbols.front());
            }
        }
        return rows;
    }
    rows.assign(analysis.stateCount(), std::vector<std::string>());
    for (std::size_t state = 0; state < analysis.stateCount(); ++state)
    {
        std::vector<std::string> &target = rows[state];
        for (const std::string &token : analysis.terminals)
        {
            if (token == k_errorTokenName)
            {
                continue;
            }
            if (analysis.action.find(std::make_pair(
                    static_cast<int>(state), token)) != analysis.action.end())
            {
                target.push_back(token);
            }
        }
        if (k >= 2)
        {
            for (const std::vector<std::string> &column :
                 analysis.lookaheadColumns)
            {
                if (column.empty())
                {
                    continue;
                }
                if (analysis.action.find(std::make_pair(
                        static_cast<int>(state),
                        lookaheadKeyOf(column))) == analysis.action.end())
                {
                    continue;
                }
                if (std::find(target.begin(), target.end(), column.front()) ==
                    target.end())
                {
                    target.push_back(column.front());
                }
            }
        }
    }
    return rows;
}

const char *expectedFallbackText(const std::string &mode)
{
    if ((mode == "ll0") || (mode == "ll1") || (mode == "llk"))
    {
        return "该状态下无可用产生式";
    }
    return "该状态下无可用动作";
}

int lookaheadEncodingMaxK(std::size_t terminalCount)
{
    if (terminalCount <= 1u)
    {
        return (std::numeric_limits<int>::max)();
    }
    const unsigned long long limit =
        static_cast<unsigned long long>((std::numeric_limits<std::int64_t>::max)());
    unsigned long long value = 1u;
    int k = 0;
    while (value <= (limit / terminalCount))
    {
        value *= terminalCount;
        ++k;
    }
    return k;
}

bool Analysis::analyze(const Grammar &grammar,
                       const std::vector<std::string> &tokens,
                       const std::string &mode,
                       const PriorityConfig *priority, bool teach,
                       AnalysisResult &out, Error &error, int lookaheadK)
{
    if (mode == "lrk")
    {
        return lrkAnalyze(grammar, tokens, lookaheadK, priority, teach, out,
                          error);
    }
    if (mode == "lalrk")
    {
        return lalrkAnalyze(grammar, tokens, lookaheadK, priority, teach, out,
                            error);
    }
    if (mode == "slrk")
    {
        return slrkAnalyze(grammar, tokens, lookaheadK, priority, teach, out,
                           error);
    }
    if (mode == "llk")
    {
        return llkAnalyze(grammar, tokens, lookaheadK, teach, out, error);
    }
    if (mode == "ll0")
    {
        return llkAnalyze(grammar, tokens, 0, teach, out, error);
    }
    if (mode == "lalr")
    {
        return lalrAnalyze(grammar, tokens, priority, teach, out, error);
    }
    if (mode == "ielr")
    {
        return ielrAnalyze(grammar, tokens, priority, teach, out, error);
    }
    if (mode == "glr")
    {
        return glrAnalyze(grammar, tokens, priority, out, error);
    }
    if ((mode == "earley") || (mode == "cyk"))
    {
        if (!glrAnalyze(grammar, tokens, priority, out, error))
        {
            return false;
        }
        out.mode = mode;
        return true;
    }
    if (mode == "dd")
    {
        return ddAnalyze(grammar, tokens, priority, out, error);
    }
    if (mode == "slr")
    {
        return slrAnalyze(grammar, tokens, priority, teach, out, error);
    }
    if (mode == "lr0")
    {
        return lr0Analyze(grammar, tokens, priority, teach, out, error);
    }
    if (mode == "lr1")
    {
        return lr1Analyze(grammar, tokens, priority, teach, out, error);
    }
    return prepareGrammar(grammar, tokens, out, error);
}

} // namespace ParseEngine
