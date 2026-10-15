/**
 * @file analysis_dd.cpp
 * @brief DD：带回溯的递归下降（每个非终结符一个函数，按产生式声明顺序逐条试）。
 *
 * DD 与其它模式最大的不同是**生成期几乎没有表**：递归下降把文法直接写成
 * 代码，运行时按「非终结符 → 它的产生式列表」逐条试，失败就回退输入位置再试
 * 下一条。所以这里算出来的东西只有三类：
 *
 * - 报告的底料：终结符 / 非终结符 / FIRST / FOLLOW / 摊平的产生式；
 * - 运行时要用的**产生式元数据**：每条产生式的右部长度与右部符号（各档产物
 *   按同一份数据发 `*_dd_tables`，「第 i 条产生式右部从哪开始、多长」）；
 * - 唯一的硬性文法限制：**左递归检测**。`A -> A α` 这种（含「跳过可空前缀
 *   后仍以自己开头」的间接形式）会让递归下降无限递归，生成期直接报错并给出
 *   环路，而不是让用户拿到一个跑起来就爆栈的产物。
 *
 * 与 LR 家族的区别还有一处：DD **不用增广文法**（不需要虚拟起始符号），
 * 产物直接从用户写的 `%start` 开始解析。
 */

#include "ParseEngine/analysis.hpp"

#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "ParseEngine/analysis_internal.hpp"
#include "ParseEngine/grammar_loader.hpp"

namespace ParseEngine
{

using namespace analysis_detail;

namespace
{

/**
 * @brief 计算可空非终结符集合（能推出空串的那些）。
 * @param rules 规则表。
 * @param nontermSet 非终结符集合。
 * @return 可空非终结符集合。
 */
std::set<std::string> computeNullable(
    const std::map<std::string, std::vector<Production>> &rules,
    const std::set<std::string> &nontermSet)
{
    std::set<std::string> nullable;
    bool changed = true;
    while (changed)
    {
        changed = false;
        for (const auto &rule : rules)
        {
            if (nullable.count(rule.first) != 0u)
            {
                continue;
            }
            for (const Production &production : rule.second)
            {
                bool allNullable = true;
                for (const std::string &symbol : production.rhs)
                {
                    const bool symbolNullable =
                        (nontermSet.count(symbol) != 0u) &&
                        (nullable.count(symbol) != 0u);
                    if (!symbolNullable)
                    {
                        allNullable = false;
                        break;
                    }
                }
                if (allNullable)
                {
                    nullable.insert(rule.first);
                    changed = true;
                    break;
                }
            }
        }
    }
    return nullable;
}

/**
 * @brief 建「左角」关系图：A → B 表示 A 能跳过可空前缀后以 B 开头。
 *
 * `A -> B C` 里 C 是不是可空与 A 能不能以 B 开头无关，所以碰到第一个
 * 「不可空的符号」就停——那个符号是终结符，或者不可空的非终结符。
 * @param rules 规则表。
 * @param nontermSet 非终结符集合。
 * @param nullable 可空非终结符集合。
 * @return 邻接表（按非终结符名有序，每个后继按首次出现顺序去重）。
 */
std::map<std::string, std::vector<std::string>> buildLeftCorner(
    const std::map<std::string, std::vector<Production>> &rules,
    const std::set<std::string> &nontermSet,
    const std::set<std::string> &nullable)
{
    std::map<std::string, std::vector<std::string>> graph;
    for (const auto &rule : rules)
    {
        std::vector<std::string> &target = graph[rule.first];
        for (const Production &production : rule.second)
        {
            for (const std::string &symbol : production.rhs)
            {
                const bool isNonterm = nontermSet.count(symbol) != 0u;
                if (isNonterm &&
                    (std::find(target.begin(), target.end(), symbol) ==
                     target.end()))
                {
                    target.push_back(symbol);
                }
                // 只跳过可空的前缀；第一个不可空的符号（不管是终结符还是
                // 非终结符）就是「左角」，后面不再看
                if (!(isNonterm && (nullable.count(symbol) != 0u)))
                {
                    break;
                }
            }
        }
    }
    return graph;
}

/**
 * @brief 在左角图上找一条环路（递归下降不能有左递归）。
 *
 * 用「路径栈 + 已完成标记」做深度优先：碰到还在栈上的后继就是一条环路，
 * 把栈里从那个后继到栈顶的一段截出来返回（`A ⇒ B ⇒ … ⇒ A`）。
 * @param graph 左角关系图。
 * @param order 非终结符的遍历顺序（报告里的顺序要稳定）。
 * @param outCycle 输出环路（不含重复的收尾节点）。
 * @return 有环路返回 true。
 */
bool findLeftRecursionCycle(
    const std::map<std::string, std::vector<std::string>> &graph,
    const std::vector<std::string> &order, std::vector<std::string> &outCycle)
{
    std::set<std::string> finished;
    std::set<std::string> onPath;
    std::vector<std::string> path;

    // 为了在递归里用「路径栈」的引用语义，这里写成一个 lambda 递归
    std::function<bool(const std::string &)> visit =
        [&](const std::string &node) -> bool
    {
        path.push_back(node);
        onPath.insert(node);
        const auto found = graph.find(node);
        if (found != graph.end())
        {
            for (const std::string &next : found->second)
            {
                if (onPath.count(next) != 0u)
                {
                    const auto begin = std::find(path.begin(), path.end(), next);
                    outCycle.assign(begin, path.end());
                    return true;
                }
                if ((finished.count(next) == 0u) && visit(next))
                {
                    return true;
                }
            }
        }
        onPath.erase(node);
        finished.insert(node);
        path.pop_back();
        return false;
    };

    for (const std::string &name : order)
    {
        if ((finished.count(name) == 0u) && visit(name))
        {
            return true;
        }
    }
    return false;
}

/**
 * @brief 把环路渲染成 `A ⇒ B ⇒ A` 这样的文本。
 * @param cycle 环路节点。
 * @return 一行文本。
 */
std::string cycleText(const std::vector<std::string> &cycle)
{
    std::string text;
    for (const std::string &name : cycle)
    {
        text += name;
        text += " ⇒ ";
    }
    if (!cycle.empty())
    {
        text += cycle.front();
    }
    return text;
}

} // namespace

bool Analysis::ddAnalyze(const Grammar &grammar,
                         const std::vector<std::string> &tokens,
                         const PriorityConfig *priority, AnalysisResult &out,
                         Error &error)
{
    (void)error;
    AnalysisResult &result = out;
    result.mode = "dd";
    result.priority = GrammarLoader::normalizePriority(priority);
    // 不增广：递归下降从用户写的起始符号开始，产物里没有虚拟起始符号
    result.grammar = grammar;

    std::vector<std::string> allTokens = k_defaultTokenNames;
    allTokens.insert(allTokens.end(), tokens.begin(), tokens.end());
    allTokens.push_back("END");
    // error 是恢复用的伪终结符；DD 用不了它（生成期就被拒），这里只是为了
    // 与其它模式保持同一份终结符编号口径——真写了 error 的文法在进门之前
    // 已经报错了
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
    result.nonterms.clear();
    for (const auto &rule : result.grammar.rules)
    {
        result.nonterms.push_back(rule.first);
        nontermSet.insert(rule.first);
    }
    result.first =
        computeFirst(result.grammar.rules, result.terminals, result.nonterms);
    result.follow = computeFollow(result.grammar.start, result.grammar.rules,
                                  result.terminals, result.nonterms,
                                  result.first);
    result.hasFollow = true;

    // 摊平产生式：编号按「非终结符名有序 + 规则内声明序」，与其余模式一致
    // （各档产物的动作分派按这个编号走）
    for (const auto &rule : result.grammar.rules)
    {
        for (const Production &production : rule.second)
        {
            Production item = production;
            item.id = static_cast<int>(result.productions.size());
            item.lhs = rule.first;
            result.productions.push_back(item);
        }
    }

    // 左递归是递归下降唯一的硬性限制：直接（A -> A α）与「跳过可空前缀后的
    // 间接」形式都会无限递归，在生成期就拦住并给出环路
    const std::set<std::string> nullable =
        computeNullable(result.grammar.rules, nontermSet);
    const std::map<std::string, std::vector<std::string>> leftCorner =
        buildLeftCorner(result.grammar.rules, nontermSet, nullable);
    std::vector<std::string> cycle;
    const bool hasLeftRecursion =
        findLeftRecursionCycle(leftCorner, result.nonterms, cycle);
    // 报告的底料：环路要么为空，要么就是那条环（渲染时用）
    result.ddLeftRecursionCycle = hasLeftRecursion ? cycle
                                                   : std::vector<std::string>();
    if (hasLeftRecursion)
    {
        std::string message =
            "%mode dd 不支持左递归：递归下降是「直接照产生式右部往下调用」，"
            "左递归会让解析函数无限递归（栈溢出），必须先改写文法。\n"
            "      检测到的环路（跳过可空前缀后）：" +
            cycleText(cycle) + "\n";
        if (cycle.size() == 1u)
        {
            message += "      这是直接左递归（" + cycle.front() + " -> " +
                       cycle.front() +
                       " …）：请改写成右递归，例如 " + cycle.front() +
                       " -> β " + cycle.front() + "\u2032（β 是非空的结尾部分）"
                       "，或改用 --mode lalr / ielr。\n";
        }
        else
        {
            std::string chain;
            for (std::size_t index = 0; index + 1u < cycle.size(); ++index)
            {
                chain += cycle[index] + " -> " + cycle[index + 1u] + " …，";
            }
            chain += cycle.back() + " -> " + cycle.front() + " …";
            message +=
                "      这是间接左递归（" + chain +
                "）：把其中一条产生式改写掉即可打断环路，或改用 --mode "
                "lalr / ielr。\n";
        }
        return fail(error, message);
    }
    return true;
}

std::string formatDdReport(const AnalysisResult &analysis)
{
    // 终结符的打印顺序：按终结符表编号（第一组），ε 排在最后
    std::map<std::string, int> order;
    for (std::size_t index = 0; index < analysis.terminals.size(); ++index)
    {
        order[analysis.terminals[index]] = static_cast<int>(index);
    }
    order[k_epsilon] = static_cast<int>(order.size());

    const auto setText = [&order](const std::set<std::string> &values)
    {
        std::string joined;
        for (const std::string &item : orderedTokens(values, order))
        {
            if (!joined.empty())
            {
                joined += ", ";
            }
            joined += (item == k_epsilon) ? "%empty" : item;
        }
        return "{ " + joined + " }";
    };

    std::vector<std::string> lines;
    lines.push_back(
        "[生成模式] DD：带回溯的递归下降（每个非终结符一个函数，按产生式声明"
        "顺序逐条试，失败回退输入位置再试下一条）");
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
        const auto found = analysis.first.find(name);
        lines.push_back("FIRST(" + name + ") = " +
                        setText(found == analysis.first.end()
                                    ? std::set<std::string>()
                                    : found->second));
    }
    lines.push_back("");
    lines.push_back("[FOLLOW]");
    for (const std::string &name : analysis.nonterms)
    {
        const auto found = analysis.follow.find(name);
        lines.push_back("FOLLOW(" + name + ") = " +
                        setText(found == analysis.follow.end()
                                    ? std::set<std::string>()
                                    : found->second));
    }
    lines.push_back("");
    lines.push_back("[递归下降]");
    lines.push_back("起始符号：" + analysis.grammar.start +
                    "（产物从它的解析函数开始，读完后要求下一个 Token 是 END）");
    lines.push_back("每个非终结符一个解析函数，函数体就是「依次试它的产生式」：");
    for (const std::string &name : analysis.nonterms)
    {
        std::vector<std::string> texts;
        for (const Production &production : analysis.productions)
        {
            if (production.lhs != name)
            {
                continue;
            }
            texts.push_back("P" + std::to_string(production.id) + " " +
                            joinRhs(production.rhs));
        }
        lines.push_back("  " + name + "：" + std::to_string(texts.size()) +
                        " 条产生式");
        for (const std::string &text : texts)
        {
            lines.push_back("      " + text);
        }
    }
    lines.push_back("");
    lines.push_back("[左递归检查]");
    lines.push_back(
        "（递归下降唯一的硬性文法限制：A 能跳过可空前缀后以自己开头时，解析"
        "函数会无限递归）");
    if (analysis.ddLeftRecursionCycle.empty())
    {
        lines.push_back("通过：没有左递归（直接或间接），可以生成递归下降产物");
    }
    else
    {
        lines.push_back("发现环路：" + cycleText(analysis.ddLeftRecursionCycle));
    }
    lines.push_back("");
    lines.push_back("[回溯代价]");
    lines.push_back(
        "回溯式递归下降按「产生式声明顺序」逐条试，匹配失败就把词法器位置与"
        " Token 下标一起回退；最坏情况下（每条产生式都在末尾才失败）会退化成"
        "指数级试探。");
    lines.push_back(
        "把「最可能需要的那条产生式写在前面」能显著减少回退次数；真正有歧义"
        "的文法只会保留**第 1 条匹配路径**的结果（不像 GLR 那样枚举全部解释）。");
    lines.push_back(
        "递归深度用 %max_stack_depth 挡（缺省 " +
        std::to_string(k_maxStackDepthDefault) + " 层），超过就报「递归太深」。");

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
