/**
 * @file analysis_lrk.cpp
 * @brief LR(k) / LALR(k)：k 元组展望符的项集、FIRST_k、ACTION/GOTO 表与报告。
 *
 * 这一份实现刻意复用 LR(1) 的那套机器（lr1_detail / analysis_detail），
 * 只把「一个展望符」换成「k 个展望符」：
 *
 * - 项仍然是 Lr1Item，`lookahead` 存的是 k 元组键（k = 1 时就是 Token 名本身，
 *   所以 k = 1 的行为和 LR(1) / LALR(1) 完全一致）；
 * - 状态表示仍然是 LalrState（core -> 展望符集合），同心合并 / 规范化 /
 *   GOTO 表 / 冲突消解全部照旧，只有闭包里的展望符传播要用 FIRST_k；
 * - ACTION 表分两层：移进只依赖下一个 Token（单 Token 列），归约按 k 元组
 *   展望符建列。运行时的查找顺序是「先按 k 元组找归约列，没有再按下一个
 *   Token 找移进」，这正是 LR(k) 表的标准两层查法，也让「按优先级选择移进 /
 *   归约」这类消解能精确到具体的 k 元组。
 */

#include "ParseEngine/analysis.hpp"

#include <algorithm>
#include <climits>
#include <set>
#include <unordered_map>

#include "ParseEngine/analysis_internal.hpp"
#include "ParseEngine/grammar_loader.hpp"

namespace ParseEngine
{

using namespace analysis_detail;

const std::string k_lrkStartSymbol = "__lrk_start";
const std::string k_lalrkStartSymbol = "__lalrk_start";
const std::string k_slrkStartSymbol = "__slrk_start";
const std::string k_llkStartSymbol = "__llk_start";

namespace
{

/// k 元组键的分隔符：终结符名里不会出现控制字符，不会跟 Token 名撞
const char k_lookaheadSeparator = '\x01';

/**
 * @brief 把键拆回符号列表（空串表示「空前缀」，返回空列表）。
 * @param key 键。
 * @return 符号列表。
 */
std::vector<std::string> splitLookaheadKey(const std::string &key)
{
    std::vector<std::string> symbols;
    if (key.empty())
    {
        return symbols;
    }
    std::string current;
    for (const char ch : key)
    {
        if (ch == k_lookaheadSeparator)
        {
            symbols.push_back(current);
            current.clear();
            continue;
        }
        current += ch;
    }
    symbols.push_back(current);
    return symbols;
}

/**
 * @brief 键里符号个数。
 * @param key 键。
 * @return 符号个数（空串为 0）。
 */
std::size_t keyLength(const std::string &key)
{
    if (key.empty())
    {
        return 0u;
    }
    return static_cast<std::size_t>(
               std::count(key.begin(), key.end(), k_lookaheadSeparator)) +
           1u;
}

/**
 * @brief 拼接两个键并截断到 k 个符号。
 * @param prefix 前缀键（长度 ≤ k）。
 * @param tail 后缀键。
 * @param k 展望符长度。
 * @return 拼接后的键（最多 k 个符号）。
 */
std::string joinTruncated(const std::string &prefix, const std::string &tail,
                          int k)
{
    std::vector<std::string> symbols = splitLookaheadKey(prefix);
    const std::vector<std::string> rest = splitLookaheadKey(tail);
    for (const std::string &symbol : rest)
    {
        if (static_cast<int>(symbols.size()) >= k)
        {
            break;
        }
        symbols.push_back(symbol);
    }
    return lookaheadKeyOf(symbols);
}

/**
 * @brief 取键的第一个符号（归约列的 %prec 按元组首 Token 判定）。
 * @param key 键。
 * @return 第一个符号；键为空时返回空串。
 */
std::string firstSymbolOfKey(const std::string &key)
{
    const std::vector<std::string> symbols = splitLookaheadKey(key);
    return symbols.empty() ? std::string() : symbols.front();
}

/**
 * @brief k 个 END 的键（接受动作所在的列）。
 * @param k 展望符长度。
 * @return 键。
 */
std::string endLookaheadKey(int k)
{
    return lookaheadKeyOf(std::vector<std::string>(
        static_cast<std::size_t>(k), std::string("END")));
}

} // namespace

std::string lookaheadKeyOf(const std::vector<std::string> &symbols)
{
    std::string key;
    for (std::size_t index = 0; index < symbols.size(); ++index)
    {
        if (index != 0)
        {
            key += k_lookaheadSeparator;
        }
        key += symbols[index];
    }
    return key;
}

std::string lookaheadKeyText(const std::string &key)
{
    const std::vector<std::string> symbols = splitLookaheadKey(key);
    std::string text;
    for (std::size_t index = 0; index < symbols.size(); ++index)
    {
        if (index != 0)
        {
            text += " ";
        }
        text += symbols[index];
    }
    return text;
}

std::string lookaheadSetText(const std::set<std::string> &keys)
{
    std::string text;
    for (const std::string &key : keys)
    {
        if (!text.empty())
        {
            text += ", ";
        }
        text += key.empty() ? std::string("%empty") : lookaheadKeyText(key);
    }
    return text;
}

std::vector<std::string> lookaheadKeySymbols(const std::string &key)
{
    return splitLookaheadKey(key);
}

namespace lrk_detail
{

std::map<std::string, std::set<std::string>> computeFirstK(
    const std::map<std::string, std::vector<Production>> &rules,
    const std::vector<std::string> &terminals,
    const std::vector<std::string> &nonterms, int k)
{
    (void)terminals;
    std::set<std::string> nontermSet(nonterms.begin(), nonterms.end());
    std::map<std::string, std::set<std::string>> firstK;
    for (const std::string &name : nonterms)
    {
        firstK[name];
    }
    bool changed = true;
    while (changed)
    {
        changed = false;
        for (const auto &rule : rules)
        {
            std::set<std::string> &target = firstK[rule.first];
            for (const Production &production : rule.second)
            {
                // 左到右卷积：current 是「前缀集合」，每碰到一个符号就接上
                std::set<std::string> current;
                current.insert(""); // 空串 = 什么都不产生
                for (const std::string &symbol : production.rhs)
                {
                    std::set<std::string> pieces;
                    if (nontermSet.count(symbol) != 0u)
                    {
                        pieces = firstK[symbol];
                    }
                    else
                    {
                        pieces.insert(symbol);
                    }
                    std::set<std::string> next;
                    for (const std::string &prefix : current)
                    {
                        for (const std::string &piece : pieces)
                        {
                            next.insert(joinTruncated(prefix, piece, k));
                        }
                    }
                    current.swap(next);
                    bool allFull = true;
                    for (const std::string &value : current)
                    {
                        if (keyLength(value) < static_cast<std::size_t>(k))
                        {
                            allFull = false;
                            break;
                        }
                    }
                    if (allFull)
                    {
                        // 已经全部截断到 k 个符号，再接符号也不会变
                        break;
                    }
                }
                for (const std::string &value : current)
                {
                    if (target.insert(value).second)
                    {
                        changed = true;
                    }
                }
            }
        }
    }
    return firstK;
}

std::set<std::string> firstKOfSequence(
    const std::vector<std::string> &sequence, int k,
    const std::set<std::string> &nontermSet,
    const std::map<std::string, std::set<std::string>> &firstK)
{
    std::set<std::string> current;
    current.insert("");
    for (const std::string &symbol : sequence)
    {
        std::set<std::string> pieces;
        if (nontermSet.count(symbol) != 0u)
        {
            const auto foundIt = firstK.find(symbol);
            const std::set<std::string> *found =
                foundIt != firstK.end() ? &foundIt->second : nullptr;
            if (found == nullptr)
            {
                return {};
            }
            pieces = *found;
        }
        else
        {
            pieces.insert(symbol);
        }
        std::set<std::string> next;
        for (const std::string &prefix : current)
        {
            for (const std::string &piece : pieces)
            {
                next.insert(joinTruncated(prefix, piece, k));
            }
        }
        current.swap(next);
        bool allFull = true;
        for (const std::string &value : current)
        {
            if (keyLength(value) < static_cast<std::size_t>(k))
            {
                allFull = false;
                break;
            }
        }
        if (allFull)
        {
            break;
        }
    }
    return current;
}

std::set<std::string> extendLookahead(const std::set<std::string> &prefixes,
                                      const std::string &tail, int k)
{
    std::set<std::string> result;
    for (const std::string &prefix : prefixes)
    {
        result.insert(joinTruncated(prefix, tail, k));
    }
    return result;
}

std::map<std::string, std::set<std::string>> computeFollowK(
    const std::string &start,
    const std::map<std::string, std::vector<Production>> &rules,
    const std::vector<std::string> &nonterms, int k,
    const std::map<std::string, std::set<std::string>> &firstK)
{
    std::set<std::string> nontermSet(nonterms.begin(), nonterms.end());
    std::map<std::string, std::set<std::string>> followK;
    for (const std::string &name : nonterms)
    {
        followK[name];
    }
    followK[start].insert(endLookaheadKey(k));
    bool changed = true;
    while (changed)
    {
        changed = false;
        for (const auto &rule : rules)
        {
            const std::string &lhs = rule.first;
            const auto lhsFollowIt = followK.find(lhs);
            const std::set<std::string> *lhsFollow =
                lhsFollowIt != followK.end() ? &lhsFollowIt->second : nullptr;
            if (lhsFollow == nullptr)
            {
                continue;
            }
            for (const Production &production : rule.second)
            {
                const std::vector<std::string> &rhs = production.rhs;
                for (std::size_t index = 0; index < rhs.size(); ++index)
                {
                    const std::string &symbol = rhs[index];
                    if (nontermSet.count(symbol) == 0u)
                    {
                        continue;
                    }
                    const std::vector<std::string> suffix(
                        rhs.begin() + static_cast<std::ptrdiff_t>(index) + 1,
                        rhs.end());
                    const std::set<std::string> betaFirst =
                        firstKOfSequence(suffix, k, nontermSet, firstK);
                    std::set<std::string> &target = followK[symbol];
                    for (const std::string &tail : *lhsFollow)
                    {
                        const std::set<std::string> extended =
                            extendLookahead(betaFirst, tail, k);
                        for (const std::string &value : extended)
                        {
                            // 与 FOLLOW 排除 error 的规则一致：error 是恢复用的
                            // 伪终结符，不进 FOLLOW_k、不往外传播
                            if (firstSymbolOfKey(value) == k_errorTokenName)
                            {
                                continue;
                            }
                            if (target.insert(value).second)
                            {
                                changed = true;
                            }
                        }
                    }
                }
            }
        }
    }
    return followK;
}

std::set<Lr1Item> lrkClosure(
    const std::set<Lr1Item> &items,
    const std::map<std::string, std::vector<Production>> &rules,
    const std::set<std::string> &nontermSet, int k,
    const std::map<std::string, std::set<std::string>> &firstK)
{
    // 与 LR(1) 闭包同构：按 core 分组，展望符集合整体传播
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
        const auto ruleFound = rules.find(core.lhs);
        if (ruleFound == rules.end())
        {
            continue;
        }
        const std::vector<std::string> &rhs =
            ruleFound->second[static_cast<std::size_t>(core.prod)].rhs;
        if (static_cast<std::size_t>(core.dot) >= rhs.size())
        {
            continue;
        }
        const std::string &symbol = rhs[static_cast<std::size_t>(core.dot)];
        if (nontermSet.count(symbol) == 0u)
        {
            continue;
        }
        const std::set<std::string> &coreLookaheads = groups[core];
        // FIRST_k(β)：只跟 core 有关，算一次即可；再逐个接到本组的展望符上
        const std::vector<std::string> suffix(
            rhs.begin() + static_cast<std::ptrdiff_t>(core.dot) + 1, rhs.end());
        const std::set<std::string> betaFirst =
            firstKOfSequence(suffix, k, nontermSet, firstK);
        std::set<std::string> newLookaheads;
        for (const std::string &tail : coreLookaheads)
        {
            const std::set<std::string> extended =
                extendLookahead(betaFirst, tail, k);
            newLookaheads.insert(extended.begin(), extended.end());
        }
        const auto targetRule = rules.find(symbol);
        if (targetRule == rules.end())
        {
            continue;
        }
        for (std::size_t index = 0; index < targetRule->second.size(); ++index)
        {
            const LrItem next{symbol, static_cast<int>(index), 0};
            std::set<std::string> &target = groups[next];
            const std::size_t before = target.size();
            target.insert(newLookaheads.begin(), newLookaheads.end());
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

std::set<Lr1Item> lrkGoto(
    const std::set<Lr1Item> &items, const std::string &symbol,
    const std::map<std::string, std::vector<Production>> &rules,
    const std::set<std::string> &nontermSet, int k,
    const std::map<std::string, std::set<std::string>> &firstK)
{
    std::set<Lr1Item> moved;
    for (const Lr1Item &item : items)
    {
        const std::vector<std::string> &rhs =
            rules.at(item.lhs)[static_cast<std::size_t>(item.prod)].rhs;
        if ((static_cast<std::size_t>(item.dot) < rhs.size()) &&
            (rhs[static_cast<std::size_t>(item.dot)] == symbol))
        {
            moved.insert({item.lhs, item.prod, item.dot + 1, item.lookahead});
        }
    }
    if (moved.empty())
    {
        return {};
    }
    return lrkClosure(moved, rules, nontermSet, k, firstK);
}

void lrkBuildAutomaton(
    const std::map<std::string, std::vector<Production>> &rules,
    const std::set<std::string> &nontermSet, const std::string &start, int k,
    const std::map<std::string, std::set<std::string>> &firstK,
    std::vector<std::set<Lr1Item>> &outStates,
    std::map<std::pair<int, std::string>, int> &outTransitions)
{
    const std::set<Lr1Item> startState = lrkClosure(
        {{start, 0, 0, endLookaheadKey(k)}}, rules, nontermSet, k, firstK);
    outStates.clear();
    outStates.push_back(startState);
    std::unordered_map<std::size_t, std::vector<int>> stateIds;
    stateIds[lr1_detail::lr1StateHash(startState)].push_back(0);
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
            const std::set<Lr1Item> target = lrkGoto(
                outStates[static_cast<std::size_t>(stateId)], symbol, rules,
                nontermSet, k, firstK);
            if (target.empty())
            {
                continue;
            }
            const std::size_t targetHash = lr1_detail::lr1StateHash(target);
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

std::string lrkStateText(
    const LalrState &state,
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
            lookaheads += lookaheadKeyText(token);
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

void buildLrkStyleTables(
    const std::vector<LalrState> &states,
    const std::map<std::pair<int, std::string>, int> &transitions,
    const std::map<std::string, std::vector<Production>> &rules,
    const std::vector<std::string> &terminals,
    const std::vector<std::string> &nonterms, const std::string &start, int k,
    const PriorityConfig &priority, const std::string &label,
    AnalysisResult &out, bool resolveReduceReduce)
{
    if (k <= 1)
    {
        // k = 1 时就是 LR(1) / LALR(1)：直接走同一条建表路径，行为完全一致
        lr1_detail::buildLalrStyleTables(states, transitions, rules, terminals,
                                         nonterms, start, priority, label, out,
                                         resolveReduceReduce);
        return;
    }
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
            return lrkStateText(states[static_cast<std::size_t>(stateId)], rules);
        },
        rules, productionIds, out.action, priority, out.resolutions,
        out.unresolved, &out.warnings, resolveReduceReduce);
    const std::string endKey = endLookaheadKey(k);
    std::map<int, std::vector<std::pair<std::string, LrAction>>> reduces;
    // 第一趟：移进 / 接受。移进只看下一个 Token，所以还是单 Token 的列
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
                if (terminalSet.count(symbol) != 0u)
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
                recorder.record(static_cast<int>(stateId), endKey,
                                {LrAction::Kind::Accept, 0}, "END",
                                lookaheadKeyText(endKey));
                continue;
            }
            for (const std::string &token : entry.second)
            {
                const auto prodIdIt = productionIds.find(std::make_pair(item.lhs, item.prod));
                const int *prodId =
                    prodIdIt != productionIds.end() ? &prodIdIt->second : nullptr;
                reduces[static_cast<int>(stateId)].push_back(
                    {token, {LrAction::Kind::Reduce, *prodId}});
            }
        }
    }
    // 第二趟：归约。跨层冲突（归约列的首 Token 上正好有移进）按 yacc 惯例
    // 用 %prec / 结合性消解；消解不掉时移进优先，归约列不写（运行时按
    // 「先归约列、后移进」查，等价于这个元组上选择了移进）。
    for (auto &stateReduces : reduces)
    {
        const int stateId = stateReduces.first;
        for (const auto &entry : stateReduces.second)
        {
            const std::string &key = entry.first;
            const LrAction &value = entry.second;
            const std::string first = firstSymbolOfKey(key);
            const auto shiftFound =
                out.action.find(std::make_pair(stateId, first));
            if (shiftFound != out.action.end())
            {
                std::string reason;
                const std::string resolved = tryResolveShiftReduce(
                    shiftFound->second, value, first, recorder.precedence,
                    recorder.associativity, recorder.productionPriority,
                    reason);
                if (resolved == "reduce")
                {
                    out.resolutions.push_back(
                        {stateId, lookaheadKeyText(key), "移进/归约冲突",
                         reason, "归约"});
                }
                else if (resolved == "shift")
                {
                    out.resolutions.push_back(
                        {stateId, lookaheadKeyText(key), "移进/归约冲突",
                         reason, "移进"});
                    continue;
                }
                else
                {
                    std::string text =
                        label + " 冲突（状态 " + std::to_string(stateId) +
                        "，Token " + lookaheadKeyText(key) + "）：移进/归约冲突" +
                        "；已有 " +
                        slrActionText(shiftFound->second, rules,
                                      productionIds) +
                        "，新增 " +
                        slrActionText(value, rules, productionIds) +
                        "；涉及项集 " +
                        lrkStateText(states[static_cast<std::size_t>(stateId)],
                                     rules) +
                        "；没有可用的优先级，默认移进优先（可用 %expect 放行）";
                    if (first == k_errorTokenName)
                    {
                        // error 列按 yacc 惯例移进优先，只记警告不让生成失败
                        out.warnings.push_back(
                            text + "；error 列按 yacc 惯例取移进优先");
                    }
                    else
                    {
                        out.unresolved.push_back(
                            {"移进/归约冲突", stateId, lookaheadKeyText(key),
                             text});
                    }
                    continue;
                }
            }
            recorder.record(stateId, key, value, first, lookaheadKeyText(key));
        }
    }
    // 归约列的编号：按 k 元组的字典序排，保证同一份文法每次生成的列号一致
    std::set<std::string> columns;
    for (const auto &entry : out.action)
    {
        const std::string &key = entry.first.second;
        if (key != "END" && terminalSet.count(key) == 0u)
        {
            columns.insert(key);
        }
    }
    out.lookaheadColumns.clear();
    out.lookaheadColumnIndex.clear();
    for (const std::string &key : columns)
    {
        out.lookaheadColumnIndex[key] =
            static_cast<int>(out.lookaheadColumns.size());
        out.lookaheadColumns.push_back(splitLookaheadKey(key));
    }
    for (const auto &entry : transitions)
    {
        const std::string &symbol = entry.first.second;
        if (terminalSet.count(symbol) == 0u)
        {
            out.gotoTable[std::make_pair(entry.first.first,
                                         nontermIds[symbol])] = entry.second;
        }
    }
    if (resolveReduceReduce)
    {
        appendReduceReduceWarning(label, out.resolutions, resolutionsBefore,
                                  out.warnings);
    }
}

} // namespace lrk_detail

using namespace lr1_detail;

namespace
{

/**
 * @brief 把若干产生式摊平进分析结果（各 LR 模式都这么做，编号从 0 递增）。
 * @param result 分析结果。
 */
void flattenProductions(AnalysisResult &result)
{
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
}

/**
 * @brief 模式标签：LR(2) / LALR(2)。
 * @param name "LR" 或 "LALR"。
 * @param k 展望符长度。
 * @return 标签文本。
 */
std::string lookaheadLabel(const std::string &name, int k)
{
    return name + "(" + std::to_string(k) + ")";
}

} // namespace

bool Analysis::lrkAnalyze(const Grammar &grammar,
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
    prepareLrAnalysis(grammar, tokens, k_lrkStartSymbol, "lrk", result,
                      nontermSet);
    const std::string &start = result.grammar.start;
    result.firstK = lrk_detail::computeFirstK(
        result.grammar.rules, result.terminals, result.nonterms, k);
    std::vector<std::set<Lr1Item>> states;
    std::map<std::pair<int, std::string>, int> transitions;
    lrk_detail::lrkBuildAutomaton(result.grammar.rules, nontermSet, start, k,
                                  result.firstK, states, transitions);
    result.lr1StateCount = static_cast<int>(states.size());
    lr1ToCanonicalStates(states, result.lalrStates);
    result.transitions = transitions;
    result.hasFollow = false;
    const std::string label = lookaheadLabel("LR", k);
    lrk_detail::buildLrkStyleTables(
        result.lalrStates, result.transitions, result.grammar.rules,
        result.terminals, result.nonterms, start, k, normalized, label, result);
    // 与 LR(1) / LR(0) 一样：只报不中止，冲突留给报告的 [未消解冲突]
    if (!checkUnresolvedConflictsForMode(label, result, true, error))
    {
        return false;
    }
    flattenProductions(result);
    return true;
}

bool Analysis::lalrkAnalyze(const Grammar &grammar,
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
    prepareLrAnalysis(grammar, tokens, k_lalrkStartSymbol, "lalrk", result,
                      nontermSet);
    const std::string &start = result.grammar.start;
    result.firstK = lrk_detail::computeFirstK(
        result.grammar.rules, result.terminals, result.nonterms, k);
    std::vector<std::set<Lr1Item>> states;
    std::map<std::pair<int, std::string>, int> transitions;
    lrk_detail::lrkBuildAutomaton(result.grammar.rules, nontermSet, start, k,
                                  result.firstK, states, transitions);
    result.lr1StateCount = static_cast<int>(states.size());
    std::vector<int> stateMap;
    lalrMergeStates(states, result.lalrStates, stateMap);
    for (const auto &entry : transitions)
    {
        result.transitions[std::make_pair(
            stateMap[static_cast<std::size_t>(entry.first.first)],
            entry.first.second)] =
            stateMap[static_cast<std::size_t>(entry.second)];
    }
    result.hasFollow = false;
    const std::string label = lookaheadLabel("LALR", k);
    lrk_detail::buildLrkStyleTables(
        result.lalrStates, result.transitions, result.grammar.rules,
        result.terminals, result.nonterms, start, k, normalized, label, result);
    if (!checkUnresolvedConflictsForMode(label, result, result.teach, error))
    {
        return false;
    }
    flattenProductions(result);
    return true;
}

namespace
{

/**
 * @brief 渲染 LR(k) / LALR(k) 报告（两种模式只差「合并前后状态数」一行）。
 * @param analysis 分析结果。
 * @param family 家族名（"LR" / "LALR" / "SLR"）。
 * @return 报告文本。
 */
std::string formatLrkFamilyReport(const AnalysisResult &analysis,
                                  const std::string &family)
{
    const bool merged = (family == "LALR");
    const int k = std::max(1, analysis.lookaheadK);
    std::map<std::string, int> order;
    for (std::size_t index = 0; index < analysis.terminals.size(); ++index)
    {
        order[analysis.terminals[index]] = static_cast<int>(index);
    }
    order[k_epsilon] = static_cast<int>(order.size());
    std::vector<std::string> lines;
    if (family == "SLR")
    {
        lines.push_back(
            "[生成模式] SLR(" + std::to_string(k) + ")：LR(0) 项集 + FOLLOW_" +
            std::to_string(k) + " 集合");
    }
    else if (merged)
    {
        lines.push_back(
            "[生成模式] LALR(" + std::to_string(k) +
            ")：LR(" + std::to_string(k) + ") 项集 + 同心项集合并 lookahead");
    }
    else
    {
        lines.push_back(
            "[生成模式] LR(" + std::to_string(k) + ")：规范 LR(" +
            std::to_string(k) +
            ") 项集 + 按 " + std::to_string(k) +
            " 元组展望符归约（不合并同心状态）");
    }
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
        for (const std::string &item :
             orderedTokens(analysis.first.at(name), order))
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
    lines.push_back("[FIRST_" + std::to_string(k) + "]（长度 ≤ " +
                    std::to_string(k) + " 的终结符串）");
    for (const std::string &name : analysis.nonterms)
    {
        const auto valuesIt = analysis.firstK.find(name);
        const std::set<std::string> *values =
            valuesIt != analysis.firstK.end() ? &valuesIt->second : nullptr;
        std::string joined;
        if (values != nullptr)
        {
            for (const std::string &item : *values)
            {
                if (!joined.empty())
                {
                    joined += ", ";
                }
                joined += (item.empty() ? std::string("%empty")
                                        : lookaheadKeyText(item));
            }
        }
        lines.push_back("FIRST_" + std::to_string(k) + "(" + name + ") = { " +
                        joined + " }");
    }
    lines.push_back("");
    if (!analysis.followK.empty())
    {
        lines.push_back("[FOLLOW_" + std::to_string(k) + "]");
        for (const std::string &name : analysis.nonterms)
        {
            const auto valuesIt = analysis.followK.find(name);
            const std::set<std::string> *values =
                valuesIt != analysis.followK.end() ? &valuesIt->second : nullptr;
            std::string joined;
            if (values != nullptr)
            {
                for (const std::string &item : *values)
                {
                    if (!joined.empty())
                    {
                        joined += ", ";
                    }
                    joined += lookaheadKeyText(item);
                }
            }
            lines.push_back("FOLLOW_" + std::to_string(k) + "(" + name +
                            ") = { " + joined + " }");
        }
        lines.push_back("");
    }
    lines.push_back("[" + family + "(" + std::to_string(k) + ") 自动机]");
    if (merged)
    {
        lines.push_back(
            "LR(" + std::to_string(k) + ") 状态数 = " +
            std::to_string(analysis.lr1StateCount) +
            "，合并后 LALR(" + std::to_string(k) + ") 状态数 = " +
            std::to_string(analysis.lalrStates.size()) + "，GOTO 转移数 = " +
            std::to_string(analysis.transitions.size()));
    }
    else
    {
        lines.push_back("状态数 = " +
                        std::to_string(analysis.lalrStates.size()) +
                        "（未合并），GOTO 转移数 = " +
                        std::to_string(analysis.transitions.size()));
    }
    lines.push_back("");
    lines.push_back("[ACTION 表]");
    lines.push_back("（移进 / 接受：列是单个 Token）");
    const std::string endKey = lookaheadKeyOf(
        std::vector<std::string>(static_cast<std::size_t>(k), "END"));
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
    if (k > 1)
    {
        lines.push_back("");
        lines.push_back("（归约：列是 " + std::to_string(k) +
                        " 元组展望符；运行时先查这一层，查不到再查上面的移进）");
        for (std::size_t column = 0;
             column < analysis.lookaheadColumns.size(); ++column)
        {
            std::string joined;
            for (const std::string &symbol : analysis.lookaheadColumns[column])
            {
                if (!joined.empty())
                {
                    joined += " ";
                }
                joined += symbol;
            }
            lines.push_back("列 " + std::to_string(column) + " = " + joined +
                            (analysis.lookaheadColumns[column] ==
                                     std::vector<std::string>(
                                         static_cast<std::size_t>(k), "END")
                                 ? "（接受列）"
                                 : ""));
        }
        for (std::size_t stateId = 0;
             stateId < analysis.lalrStates.size(); ++stateId)
        {
            for (const auto &entry : analysis.lookaheadColumnIndex)
            {
                const auto found = analysis.action.find(std::make_pair(
                    static_cast<int>(stateId), entry.first));
                if (found != analysis.action.end())
                {
                    lines.push_back("ACTION[" + std::to_string(stateId) +
                                    ", 列 " + std::to_string(entry.second) +
                                    "] = " + slrEntryText(found->second));
                }
            }
        }
    }
    else
    {
        // k = 1：接受列就是 END，上面已经打过了
        (void)endKey;
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

/**
 * @brief 渲染 LR(k) / LALR(k) 的项集族（每个 core 带 k 元组展望符）。
 * @param analysis 分析结果。
 * @param family 家族名（"LR" / "LALR" / "SLR"）。
 * @return 报告文本。
 */
std::string formatLrkFamilyAutomaton(const AnalysisResult &analysis,
                                     const std::string &family)
{
    const int k = std::max(1, analysis.lookaheadK);
    std::vector<std::string> lines;
    lines.push_back("[" + family + "(" +
                    std::to_string(k) + ") 项集族]");
    for (std::size_t stateId = 0; stateId < analysis.lalrStates.size(); ++stateId)
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
                lookaheads += lookaheadKeyText(token);
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

} // namespace

std::string formatLrkReport(const AnalysisResult &analysis)
{
    return formatLrkFamilyReport(analysis, "LR");
}

std::string formatLalrkReport(const AnalysisResult &analysis)
{
    return formatLrkFamilyReport(analysis, "LALR");
}

std::string formatSlrkReport(const AnalysisResult &analysis)
{
    return formatLrkFamilyReport(analysis, "SLR");
}

std::string formatLrkAutomaton(const AnalysisResult &analysis)
{
    return formatLrkFamilyAutomaton(analysis, "LR");
}

std::string formatLalrkAutomaton(const AnalysisResult &analysis)
{
    return formatLrkFamilyAutomaton(analysis, "LALR");
}

std::string formatSlrkAutomaton(const AnalysisResult &analysis)
{
    return formatLrkFamilyAutomaton(analysis, "SLR");
}

std::string formatLrkTables(const AnalysisResult &analysis)
{
    const int k = std::max(1, analysis.lookaheadK);
    std::vector<std::string> lines;
    const std::string family = (analysis.mode == "lalrk")
                                   ? "LALR"
                                   : ((analysis.mode == "slrk") ? "SLR" : "LR");
    lines.push_back("[" + family + "(" + std::to_string(k) +
                    ") ACTION / GOTO]");
    lines.push_back("");
    lines.push_back("ACTION（移进 / 接受；行 = 状态，列 = 单个 Token）：");
    for (std::size_t stateId = 0; stateId < analysis.lalrStates.size(); ++stateId)
    {
        std::string cells;
        for (const std::string &token : analysis.terminals)
        {
            const auto found = analysis.action.find(
                std::make_pair(static_cast<int>(stateId), token));
            if (found == analysis.action.end())
            {
                continue;
            }
            if (!cells.empty())
            {
                cells += "  ";
            }
            cells += token + "=" + slrEntryText(found->second);
        }
        lines.push_back("状态 " + std::to_string(stateId) + "：" +
                        (cells.empty() ? std::string("（无）") : cells));
    }
    if (k > 1)
    {
        lines.push_back("");
        lines.push_back("ACTION（归约；列 = " + std::to_string(k) +
                        " 元组展望符，先查这一层）：");
        for (std::size_t stateId = 0;
             stateId < analysis.lalrStates.size(); ++stateId)
        {
            std::string cells;
            for (std::size_t column = 0;
                 column < analysis.lookaheadColumns.size(); ++column)
            {
                const std::string key =
                    lookaheadKeyOf(analysis.lookaheadColumns[column]);
                const auto found = analysis.action.find(
                    std::make_pair(static_cast<int>(stateId), key));
                if (found == analysis.action.end())
                {
                    continue;
                }
                if (!cells.empty())
                {
                    cells += "  ";
                }
                cells += "列" + std::to_string(column) + "=" +
                         slrEntryText(found->second);
            }
            if (!cells.empty())
            {
                lines.push_back("状态 " + std::to_string(stateId) + "：" +
                                cells);
            }
        }
    }
    lines.push_back("");
    lines.push_back("GOTO：");
    for (const auto &entry : analysis.gotoTable)
    {
        lines.push_back(
            "GOTO[" + std::to_string(entry.first.first) + ", " +
            analysis.nonterms[static_cast<std::size_t>(entry.first.second)] +
            "] = " + std::to_string(entry.second));
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
