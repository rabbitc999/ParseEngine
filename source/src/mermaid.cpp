/**
 * @file mermaid.cpp
 * @brief 状态机图（Mermaid）生成实现。
 *
 * 由 `%mermaid` / `--mermaid` 开关控制，与教学模式（`%teach`）无关：教学模式
 * 只决定报告里要不要多印 `[教学提示]` / `[教学对照]`。
 *
 * 图的种类：
 *   - 文法 LR 自动机：状态是 LR 项集，边是 GOTO 转移（终结符 = 移进，
 *     非终结符 = GOTO），状态按「冲突 / 接受 / 归约 / 移进」上色；
 *   - 词法 DFA：`%token regex` 与 `%identifier` 各一张，状态是 DFA 状态，
 *     边是字符类（`a-z` 这样压缩过的可读区间）。
 *
 * 生成的都是 Mermaid 的 `flowchart LR` 源码，标签里的 `[`、`(`、`"` 等
 * 换成 Mermaid 自己的 `#nn;` 转义（`flowchart` 的引号标签最经得起折腾），
 * 换行用 `<br/>`——这几条组合是逐个用 Mermaid 官方解析器验证过的。
 */

#include "ParseEngine/mermaid.hpp"

#include <algorithm>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "ParseEngine/regex_dfa.hpp"
#include "ParseEngine/regex_vm.hpp"
// 图的源码、页面骨架与文案都在模板里（AGENTS.md §1 规则 10：产物文本一律进模板）
#include "ParseEngine/utils.hpp"
#include "template/engine.hpp"

namespace ParseEngine
{
namespace
{

/// 文法自动机图里的一个状态节点（`mermaid_grammar.mmd`）。
struct MermaidStateNode
{
    std::string id;                    ///< 状态号（十进制文本）
    bool isStart = false;              ///< 是不是初态
    bool isAccept = false;             ///< 是不是接受状态
    std::vector<std::string> items;    ///< 框里列的 LR 项（已转义、已截断）
    long long hiddenItems = 0;         ///< 被截断掉的项数
    std::string actions;               ///< ACTION 摘要（已转义；空串=没有）
    std::string klass;                 ///< classDef 名（shiftState / …）
};

/// Mermaid 图里的一条边（`mermaid_grammar.mmd` / `mermaid_dfa.mmd`）。
struct MermaidGraphEdge
{
    std::string from;  ///< 起点编号（文本）
    std::string to;    ///< 终点编号（文本）
    std::string label; ///< 边标签（已转义）
};

/// DFA 图里的一个状态（`mermaid_dfa.mmd`）。
struct MermaidDfaState
{
    std::string id;       ///< 状态号（十进制文本）
    bool isAccept = false; ///< 双圆（接受状态）
};

/// DFA 图里的一个 classDef 指派。
struct MermaidDfaClass
{
    std::string id;    ///< 状态号（十进制文本）
    std::string klass; ///< startState / acceptState / dfaState
};

/// ACTION 摘要里的一句话（`mermaid_state_actions.txt`）。
struct MermaidActionPart
{
    std::string word;                  ///< 移进 / 归约 / 接受 / 冲突
    std::vector<std::string> values;   ///< 这句话里跟的值（没有就是空数组）
};

/// 词法页表格里的一行（`mermaid_lexer_intro.html`）。
struct MermaidLexerRow
{
    std::string name;    ///< Token 名（已转义）
    std::string pattern; ///< 正则正文（已转义）
    std::string states;  ///< 状态数
    std::string accepts; ///< 接受状态清单（已转义）
};

/// 字符类表里的一行（`mermaid_lexer_intro.html`）。
struct MermaidCellRow
{
    std::string index; ///< 编码
    std::string text;  ///< 字符集合（已转义）
};

/// 页面里的一条链接（`mermaid_page.html`）。
struct MermaidPageLink
{
    std::string href; ///< 链接地址（也当显示文本）
    std::string desc; ///< 说明
};

/// 页面里内嵌的一张图（`mermaid_page.html`）。
struct MermaidPageDiagram
{
    std::string title;         ///< 图标题（已转义）
    std::string note;          ///< 图下面的说明（HTML 片段）
    std::string source;        ///< Mermaid 源码（原样嵌进 div.mermaid）
    std::string sourceEscaped; ///< 源码的 HTML 转义版（折叠框里用）
    bool showTitle = false;    ///< 要不要发 <h2>（单图页面且标题相同时不发）
};

/// 单个状态最多画多少行项，超出只报总数（避免图大到没法看）。
const std::size_t k_maxItemLines = 60;

/// 字符类最多逐个列出多少个字符，超出按区段截断。
const std::size_t k_maxCellChars = 16;

/// 一张图最多画多少个状态 / 多少条边 / 多少字节（超了只画一句说明）。
///
/// Mermaid 自己也有上限（默认 `maxEdges` = 500、`maxTextSize` = 50000），
/// 而且真画出来浏览器也扛不住：c89_subset 这种真实文法的 LALR 自动机有
/// 340 个状态、3117 条转移，规范 LR(1) 更是 1691 个状态、16586 条转移。
/// 与其生成一个打不开的页面，不如在图上写清楚它有多大、完整内容去哪看。
const std::size_t k_maxDiagramStates = 150;
const std::size_t k_maxDiagramEdges = 400;
const std::size_t k_maxSourceBytes = 120000;

/// 标识符正则在 DFA 里的内部名字（与 code_gen.cpp 保持一致）。
const std::string k_identifierToken = "__identifier__";

/// 页面样式：一张图一个白底卡片，源码块深色。
const std::string k_pageStyle =
    "body{font-family:-apple-system,'Segoe UI','Noto Sans CJK SC',sans-serif;"
    "margin:0 auto;max-width:1100px;padding:24px 24px 64px;"
    "color:#1f2937;background:#f8fafc;line-height:1.7;}"
    "h1{font-size:22px;margin:0 0 8px;}h2{font-size:18px;margin:32px 0 4px;}"
    "p.note{color:#475569;margin:4px 0;}"
    "p.tip{color:#0f172a;background:#e0f2fe;border-left:4px solid #0284c7;"
    "padding:8px 12px;border-radius:4px;}"
    "ul.links{list-style:none;padding:0;}ul.links li{margin:4px 0;}"
    "div.mermaid{background:#fff;border:1px solid #e2e8f0;border-radius:8px;"
    "padding:12px;margin:8px 0;overflow-x:auto;}"
    "pre{background:#0f172a;color:#e2e8f0;padding:12px;border-radius:8px;"
    "overflow-x:auto;font-size:13px;line-height:1.5;}"
    "code{background:#eef2f7;padding:1px 4px;border-radius:3px;}"
    "table{border-collapse:collapse;margin:8px 0;font-size:14px;}"
    "th,td{border:1px solid #d7dee8;padding:3px 10px;text-align:left;}"
    "th{background:#eef2f7;}"
    "ul.legend{list-style:none;padding:0;}ul.legend li{margin:2px 0;}"
    ".sw{display:inline-block;width:12px;height:12px;margin-right:6px;"
    "border-radius:3px;border:1px solid #94a3b8;vertical-align:-1px;}"
    ".sw.shiftState{background:#dbeafe;}.sw.reduceState{background:#fef3c7;}"
    ".sw.acceptState{background:#dcfce7;}.sw.conflictState{background:#fee2e2;}"
    ".sw.plainState{background:#f1f5f9;}";

/**
 * @brief HTML 文本转义（只用于页面本身的文字，不用于 Mermaid 源码）。
 * @param text 原始文本。
 * @return 转义后的文本。
 */
std::string htmlEscape(const std::string &text)
{
    std::string result;
    result.reserve(text.size());
    for (const char value : text)
    {
        switch (value)
        {
        case '&':
            result += "&amp;";
            break;
        case '<':
            result += "&lt;";
            break;
        case '>':
            result += "&gt;";
            break;
        case '"':
            result += "&quot;";
            break;
        default:
            result += value;
            break;
        }
    }
    return result;
}


/**
 * @brief 判断模式的状态存的是 LALR 风格项集（core -> lookahead）。
 * @param mode 模式名。
 * @return lalr / lr1 / ielr 返回 true，lr0 / slr 返回 false。
 */
bool usesLalrStates(const std::string &mode)
{
    return (mode == "lalr") || (mode == "lr1") || (mode == "ielr") ||
           (mode == "lrk") || (mode == "lalrk");
}

/**
 * @brief 取某状态下的项清单（一项一行）。
 * @param analysis 分析结果。
 * @param stateId 状态编号。
 * @return 项文本列表。
 */
std::vector<std::string> stateItems(const AnalysisResult &analysis,
                                    int stateId)
{
    const std::size_t index = static_cast<std::size_t>(stateId);
    std::vector<std::string> items;
    const auto render = [&analysis](const LrItem &item,
                                    const std::string &lookahead)
    {
        std::vector<std::string> rhs =
            analysis.grammar.rules.at(item.lhs)[static_cast<std::size_t>(
                item.prod)].rhs;
        const std::size_t position =
            std::min(static_cast<std::size_t>(item.dot), rhs.size());
        rhs.insert(rhs.begin() + static_cast<std::ptrdiff_t>(position), ".");
        std::string joined;
        for (std::size_t part = 0; part < rhs.size(); ++part)
        {
            if (part != 0)
            {
                joined += " ";
            }
            joined += rhs[part];
        }
        if (lookahead.empty())
        {
            return "[" + item.lhs + " -> " + joined + "]";
        }
        return "[" + item.lhs + " -> " + joined + ", " + lookahead + "]";
    };
    if (usesLalrStates(analysis.mode))
    {
        for (const auto &entry : analysis.lalrStates[index].items)
        {
            for (const std::string &token : entry.second)
            {
                items.push_back(render(entry.first, token));
            }
        }
    }
    else
    {
        for (const LrItem &item : analysis.slrStates[index].items)
        {
            items.push_back(render(item, std::string()));
        }
    }
    return items;
}

/**
 * @brief 统计 DFA 画出来之后有多少条边（同一个目标状态合并成一条）。
 * @param dfa DFA。
 * @return 边数（不含指向陷阱状态的边）。
 */
std::size_t dfaEdgeCount(const DfaProgram &dfa)
{
    std::size_t count = 0;
    for (int state = 0; state < dfa.stateCount; ++state)
    {
        if (state == dfa.trap)
        {
            continue;
        }
        std::set<int> targets;
        for (const auto &entry : dfa.transitions.at(state))
        {
            if (entry.second != dfa.trap)
            {
                targets.insert(entry.second);
            }
        }
        count += targets.size();
    }
    return count;
}

/**
 * @brief 把接受状态清单拼成 `s0、s3` 这样的文本。
 * @param accept 接受状态集合。
 * @return 清单文本（空集合返回空串）。
 */
std::string acceptStateText(const std::set<int> &accept)
{
    std::string text;
    for (const int state : accept)
    {
        if (!text.empty())
        {
            text += "、";
        }
        text += "s" + std::to_string(state);
    }
    return text;
}

/**
 * @brief 图太大时用的占位源码（只写一句说明）。
 * @param what 图的名字。
 * @param states 状态数。
 * @param edges 边数。
 * @return Mermaid 源码。
 */
std::string oversizeSource(const std::string &what, std::size_t states,
                           std::size_t edges)
{
    const std::string detail =
        std::to_string(states) + " 个状态、" + std::to_string(edges) +
        " 条边，超过绘图上限（" + std::to_string(k_maxDiagramStates) +
        " 状态 / " + std::to_string(k_maxDiagramEdges) + " 条边 / " +
        std::to_string(k_maxSourceBytes) + " 字节），完整内容看分析报告与 " +
        "--dump-lr-automaton 的输出。";
    return tmpl::renderValues(Utils::loadTemplate("mermaid_oversize.mmd"),
                              {{"what", Mermaid::escapeLabel(what)},
                               {"detail", Mermaid::escapeLabel(detail)}});
}

/**
 * @brief 超过绘图上限就把图换成一句说明。
 * @param source 原本的 Mermaid 源码。
 * @param what 图的名字。
 * @param states 状态数。
 * @param edges 边数。
 * @return 可用的 Mermaid 源码。
 */
std::string limitSource(const std::string &source, const std::string &what,
                        std::size_t states, std::size_t edges)
{
    if ((states > k_maxDiagramStates) || (edges > k_maxDiagramEdges) ||
        (source.size() > k_maxSourceBytes))
    {
        return oversizeSource(what, states, edges);
    }
    return source;
}


/**
 * @brief 取某状态的 ACTION 动作摘要（移进 / 归约 / 接受 / 冲突）。
 * @param analysis 分析结果。
 * @param stateId 状态编号。
 * @return 形如“移进 a b；归约 P2；冲突 a”的摘要，没有动作时返回空串。
 */
std::string stateActions(const AnalysisResult &analysis, int stateId)
{
    std::vector<std::string> shifts;
    std::vector<std::string> reduces;
    for (const std::string &token : analysis.terminals)
    {
        const auto found =
            analysis.action.find(std::make_pair(stateId, token));
        if (found == analysis.action.end())
        {
            continue;
        }
        if (found->second.kind == LrAction::Kind::Shift)
        {
            shifts.push_back(token);
        }
        else if (found->second.kind == LrAction::Kind::Reduce)
        {
            reduces.push_back("P" + std::to_string(found->second.value));
        }
    }
    std::sort(shifts.begin(), shifts.end());
    shifts.erase(std::unique(shifts.begin(), shifts.end()), shifts.end());
    reduces.erase(std::unique(reduces.begin(), reduces.end()), reduces.end());

    std::vector<std::string> conflicts;
    for (const LrConflict &conflict : analysis.unresolved)
    {
        if (conflict.state == stateId)
        {
            conflicts.push_back(conflict.token);
        }
    }
    const bool accept =
        analysis.action.count(std::make_pair(stateId, std::string("END"))) != 0 &&
        analysis.action.at(std::make_pair(stateId, std::string("END"))).kind ==
            LrAction::Kind::Accept;

    // 一句话 = 一个词 + 一串值；「；」分隔与空格拼接都在模板里
    std::vector<MermaidActionPart> parts;
    if (!shifts.empty())
    {
        parts.push_back(MermaidActionPart{"移进", shifts});
    }
    if (!reduces.empty())
    {
        parts.push_back(MermaidActionPart{"归约", reduces});
    }
    if (accept)
    {
        parts.push_back(MermaidActionPart{"接受", std::vector<std::string>{"END"}});
    }
    if (!conflicts.empty())
    {
        parts.push_back(MermaidActionPart{"冲突", conflicts});
    }
    return tmpl::renderValues(
        Utils::loadTemplate("mermaid_state_actions.txt"),
        {{"parts", tmpl::encode(parts)}});
}

/**
 * @brief 找初态：含 `S' -> . S` 的那个状态。
 * @param analysis 分析结果。
 * @return 初态编号；找不到时返回 0。
 */
int initialState(const AnalysisResult &analysis)
{
    const std::string &start = analysis.grammar.start;
    for (std::size_t stateId = 0; stateId < analysis.stateCount(); ++stateId)
    {
        if (usesLalrStates(analysis.mode))
        {
            for (const auto &entry : analysis.lalrStates[stateId].items)
            {
                const LrItem &core = entry.first;
                if ((core.lhs == start) && (core.prod == 0) &&
                    (core.dot == 0))
                {
                    return static_cast<int>(stateId);
                }
            }
        }
        else
        {
            const LrItem item{start, 0, 0};
            if (analysis.slrStates[stateId].items.count(item) != 0)
            {
                return static_cast<int>(stateId);
            }
        }
    }
    return 0;
}

/**
 * @brief 取某状态在状态机图里的配色类别。
 * @param analysis 分析结果。
 * @param stateId 状态编号。
 * @param conflict 该状态是否有未消解冲突。
 * @param accept 该状态是否是接受状态。
 * @return classDef 名（shiftState / reduceState / acceptState / conflictState
 *         / plainState）。
 */
std::string stateClass(const AnalysisResult &analysis, int stateId,
                       bool conflict, bool accept)
{
    if (conflict)
    {
        return "conflictState";
    }
    if (accept)
    {
        return "acceptState";
    }
    bool shift = false;
    for (const std::string &token : analysis.terminals)
    {
        const auto found = analysis.action.find(std::make_pair(stateId, token));
        if (found != analysis.action.end())
        {
            if (found->second.kind == LrAction::Kind::Reduce)
            {
                return "reduceState";
            }
            if (found->second.kind == LrAction::Kind::Shift)
            {
                shift = true;
            }
        }
    }
    return shift ? "shiftState" : "plainState";
}

/**
 * @brief 生成文法 LR 自动机的 Mermaid 源码。
 * @param analysis 分析结果。
 * @return Mermaid 源码。
 */
std::string grammarSource(const AnalysisResult &analysis)
{
    const int startState = initialState(analysis);
    const std::size_t stateCount = analysis.stateCount();
    std::set<int> conflictStates;
    for (const LrConflict &conflict : analysis.unresolved)
    {
        conflictStates.insert(conflict.state);
    }
    std::set<int> acceptStates;
    for (const auto &entry : analysis.action)
    {
        if (entry.second.kind == LrAction::Kind::Accept)
        {
            acceptStates.insert(entry.first.first);
        }
    }
    std::set<std::string> terminalSet(analysis.terminals.begin(),
                                      analysis.terminals.end());

    std::vector<MermaidStateNode> states;
    for (std::size_t stateId = 0; stateId < stateCount; ++stateId)
    {
        const int id = static_cast<int>(stateId);
        MermaidStateNode node;
        node.id = std::to_string(id);
        node.isStart = (id == startState);
        node.isAccept = (acceptStates.count(id) != 0);
        const std::vector<std::string> items = stateItems(analysis, id);
        const std::size_t drawn = std::min(items.size(), k_maxItemLines);
        for (std::size_t index = 0; index < drawn; ++index)
        {
            node.items.push_back(Mermaid::escapeLabel(items[index]));
        }
        node.hiddenItems = static_cast<long long>(items.size() - drawn);
        node.actions = Mermaid::escapeLabel(stateActions(analysis, id));
        node.klass = stateClass(analysis, id, conflictStates.count(id) != 0,
                                acceptStates.count(id) != 0);
        states.push_back(std::move(node));
    }
    std::vector<std::string> accepts;
    for (const int id : acceptStates)
    {
        accepts.push_back(std::to_string(id));
    }
    std::vector<MermaidGraphEdge> transitions;
    for (const auto &entry : analysis.transitions)
    {
        const std::string &symbol = entry.first.second;
        std::string text = symbol;
        if (terminalSet.count(symbol) == 0)
        {
            text += " (GOTO)";
        }
        MermaidGraphEdge edge;
        edge.from = std::to_string(entry.first.first);
        edge.to = std::to_string(entry.second);
        edge.label = Mermaid::escapeLabel(text);
        transitions.push_back(std::move(edge));
    }
    return tmpl::renderValues(
        Utils::loadTemplate("mermaid_grammar.mmd"),
        {{"start_state", std::to_string(startState)},
         {"states", tmpl::encode(states)},
         {"accepts", tmpl::encode(accepts)},
         {"transitions", tmpl::encode(transitions)}});
}

/**
 * @brief 把一个码点渲染成字符类标签里的样子。
 * @param value 码点。
 * @return 可读文本（空格写成“空格”，控制字符写成 `\xNN`，非 ASCII 写成 `\u{…}`）。
 */
std::string charText(char32_t value)
{
    // 空格是一个词、其余是「转义后的样子」，文案在模板里
    if (value == ' ')
    {
        return tmpl::renderValues(
            Utils::loadTemplate("mermaid_char_text.txt"),
            {{"is_space", true}, {"text", std::string()}});
    }
    std::string text;
    if (value == 0x09u)
    {
        text = "\t";
    }
    else if (value == 0x0Au)
    {
        text = "\n";
    }
    else if (value == 0x0Du)
    {
        text = "\r";
    }
    if (text.empty())
    {
        if ((value < 0x20u) || ((value >= 0x7Fu) && (value < 0xA0u)))
        {
            char buffer[16];
            std::snprintf(buffer, sizeof(buffer), "\\x%02X",
                          static_cast<unsigned int>(value));
            text = std::string(buffer);
        }
        else if (value < 0x80u)
        {
            text = std::string(1, static_cast<char>(value));
        }
        else
        {
            text = "\\u{" + std::to_string(static_cast<unsigned long>(value)) + "}";
        }
    }
    return tmpl::renderValues(Utils::loadTemplate("mermaid_char_text.txt"),
                              {{"is_space", false}, {"text", text}});
}

/**
 * @brief 把一列码点区间按连续区段压缩成 `a-z 0-9 _` 这样的文本。
 * @param ranges 升序、互不重叠的码点区间表。
 * @param limit 最多画多少段（超出的部分只在 total 里计数）。
 * @param total 输出：区间覆盖的码点总数。
 * @return 文本片段。
 */
std::vector<std::string> encodeRanges(
    const std::vector<std::pair<char32_t, char32_t>> &ranges,
    std::size_t limit, std::size_t &total)
{
    std::vector<std::string> pieces;
    total = 0u;
    std::size_t drawn = 0u;
    for (const auto &range : ranges)
    {
        total += static_cast<std::size_t>(range.second - range.first) + 1u;
        if (drawn >= limit)
        {
            continue;
        }
        ++drawn;
        if (range.first == range.second)
        {
            pieces.push_back(charText(range.first));
        }
        else
        {
            pieces.push_back(charText(range.first) + "-" + charText(range.second));
        }
    }
    return pieces;
}

/**
 * @brief 把一个字符类渲染成边标签。
 * @param cell 字符类。
 * @return 文本；区段太多时按段截断并补上码点总数。
 */
std::string cellText(const RegexCell &cell)
{
    const bool empty = cell.ranges.empty();
    std::size_t total = 0u;
    const std::vector<std::string> pieces =
        empty ? std::vector<std::string>()
              : encodeRanges(cell.ranges, k_maxCellChars, total);
    const bool truncated = (!empty) && (pieces.size() > k_maxCellChars);
    return tmpl::renderValues(
        Utils::loadTemplate("mermaid_char_class.txt"),
        {{"empty", empty},
         {"pieces", tmpl::encode(pieces)},
         {"truncated", truncated},
         {"total", static_cast<long long>(total)}});
}

/**
 * @brief 生成一条正则 DFA 的 Mermaid 源码。
 * @param item 已编译的正则。
 * @param cells 共享的字符类划分。
 * @param label 图里用的 Token 名（标识符写成“标识符”）。
 * @return Mermaid 源码。
 */
std::string dfaSource(const CompiledRegex &item,
                      const std::vector<RegexCell> &cells,
                      const std::string &label)
{
    const DfaProgram &dfa = item.dfa;
    std::vector<MermaidDfaState> states;
    for (int state = 0; state < dfa.stateCount; ++state)
    {
        if (state == dfa.trap)
        {
            continue;
        }
        MermaidDfaState node;
        node.id = std::to_string(state);
        node.isAccept = (dfa.accept.count(state) != 0);
        states.push_back(std::move(node));
    }
    std::vector<MermaidGraphEdge> edges;
    for (int state = 0; state < dfa.stateCount; ++state)
    {
        if (state == dfa.trap)
        {
            continue;
        }
        // 同一个目标状态上的多个字符类合并成一条边，图才看得清
        std::map<int, std::string> merged;
        for (const auto &entry : dfa.transitions.at(state))
        {
            const int target = entry.second;
            if (target == dfa.trap)
            {
                continue;
            }
            const std::string text =
                cellText(cells[static_cast<std::size_t>(entry.first)]);
            std::string &mergedLabel = merged[target];
            if (!mergedLabel.empty())
            {
                mergedLabel += " ";
            }
            mergedLabel += text;
        }
        for (const auto &edge : merged)
        {
            MermaidGraphEdge item_edge;
            item_edge.from = std::to_string(state);
            item_edge.to = std::to_string(edge.first);
            item_edge.label = Mermaid::escapeLabel(edge.second);
            edges.push_back(std::move(item_edge));
        }
    }
    std::vector<MermaidDfaClass> classes;
    for (int state = 0; state < dfa.stateCount; ++state)
    {
        if (state == dfa.trap)
        {
            continue;
        }
        MermaidDfaClass entry;
        entry.id = std::to_string(state);
        entry.klass = (state == dfa.start) ? "startState"
                      : (dfa.accept.count(state) != 0) ? "acceptState"
                                                      : "dfaState";
        classes.push_back(std::move(entry));
    }
    return tmpl::renderValues(
        Utils::loadTemplate("mermaid_dfa.mmd"),
        {{"label", label},
         {"pattern", Mermaid::escapeLabel(item.pattern)},
         {"state_count", std::to_string(dfa.stateCount)},
         {"trap", std::to_string(dfa.trap)},
         {"start", std::to_string(dfa.start)},
         {"states", tmpl::encode(states)},
         {"edges", tmpl::encode(edges)},
         {"classes", tmpl::encode(classes)}});
}

/**
 * @brief 拼出 DFA 状态图里用到的正则列表（正则 Token + 标识符）。
 * @param lexSpec 词法扩展配置。
 * @return （名字，正则）列表。
 */
std::vector<std::pair<std::string, std::string>> regexPatterns(
    const LexSpec &lexSpec)
{
    std::vector<std::pair<std::string, std::string>> patterns;
    for (const auto &entry : lexSpec.regex)
    {
        patterns.emplace_back(entry.first, entry.second);
    }
    if (lexSpec.identifier.has_value())
    {
        patterns.emplace_back(k_identifierToken, *lexSpec.identifier);
    }
    // 上下文断言和反向引用没有等价的普通 DFA，不能把它们画成空串边。
    patterns.erase(std::remove_if(patterns.begin(), patterns.end(), [](const auto &entry) {
        std::shared_ptr<RegexNode> ast;
        Error error;
        return RegexDfa::parse(entry.second, ast, error) && RegexVm::contextRequired(ast);
    }), patterns.end());
    return patterns;
}

} // namespace

std::string Mermaid::escapeLabel(const std::string &text)
{
    std::string result;
    result.reserve(text.size());
    for (const char value : text)
    {
        switch (value)
        {
        case '#':
            result += "#35;";
            break;
        case '&':
            result += "#amp;";
            break;
        case '<':
            result += "#lt;";
            break;
        case '[':
            result += "#91;";
            break;
        case ']':
            result += "#93;";
            break;
        case '(':
            result += "#40;";
            break;
        case ')':
            result += "#41;";
            break;
        case '{':
            result += "#123;";
            break;
        case '}':
            result += "#125;";
            break;
        case '"':
            result += "#quot;";
            break;
        case '|':
            result += "#124;";
            break;
        default:
            result += value;
            break;
        }
    }
    return result;
}

std::vector<MermaidDiagram> Mermaid::grammarDiagrams(
    const AnalysisResult &analysis)
{
    std::vector<MermaidDiagram> diagrams;
    // 只有 LR 家族手里有 LR 状态机；LL(1) 的预测分析表不是状态机，画不了
    if (!isLrMode(analysis.mode))
    {
        return diagrams;
    }
    const std::string label = modeLabelText(analysis.mode);
    const std::size_t stateCount = analysis.stateCount();
    const std::size_t edges = analysis.transitions.size();
    MermaidDiagram diagram;
    diagram.title = tmpl::renderValues(
        Utils::loadTemplate("mermaid_grammar_title.txt"),
        {{"mode_label", label}});
    // 图下面的说明与图例（虚拟起始符号那一段是可选的，另有一份小模板）
    diagram.note = tmpl::renderValues(
        Utils::loadTemplate("mermaid_grammar_note.html"),
        {{"state_count", static_cast<long long>(stateCount)},
         {"edge_count", static_cast<long long>(edges)},
         {"mode_label", htmlEscape(label)},
         {"start_state", std::to_string(initialState(analysis))}});
    if (analysis.grammar.start.compare(0, 2, "__") == 0)
    {
        diagram.note += tmpl::renderValues(
            Utils::loadTemplate("mermaid_note_virtual_start.html"),
            {{"start_symbol", htmlEscape(analysis.grammar.start)},
             {"start_symbol_tail",
              htmlEscape(analysis.grammar.start.substr(2))}});
    }
    diagram.source = limitSource(grammarSource(analysis), diagram.title,
                                 stateCount, edges);
    diagrams.push_back(diagram);
    return diagrams;
}

std::vector<MermaidDiagram> Mermaid::lexerDiagrams(const LexSpec &lexSpec,
                                                 Error &error)
{
    std::vector<MermaidDiagram> diagrams;
    const auto patterns = regexPatterns(lexSpec);
    if (patterns.empty())
    {
        return diagrams;
    }
    RegexCompileResult compiled;
    if (!RegexDfa::compileAll(patterns, compiled, error))
    {
        return diagrams;
    }
    for (const CompiledRegex &item : compiled.compiled)
    {
        const bool identifier = (item.name == k_identifierToken);
        const std::string label = identifier ? "标识符" : item.name;
        MermaidDiagram diagram;
        diagram.title = tmpl::renderValues(
            Utils::loadTemplate("mermaid_dfa_title.txt"), {{"label", label}});
        diagram.note = tmpl::renderValues(
            Utils::loadTemplate("mermaid_dfa_note.html"),
            {{"pattern", htmlEscape(item.pattern)},
             {"state_count", std::to_string(item.dfa.stateCount)},
             {"accept_list", htmlEscape(acceptStateText(item.dfa.accept))},
             {"trap", std::to_string(item.dfa.trap)}});
        diagram.source = limitSource(
            dfaSource(item, compiled.cells, label), diagram.title,
            static_cast<std::size_t>(item.dfa.stateCount),
            dfaEdgeCount(item.dfa));
        diagrams.push_back(diagram);
    }
    return diagrams;
}

std::string Mermaid::lexerIntro(
    const LexSpec &lexSpec,
    const std::map<std::string, std::vector<std::string>> &tokenDefs,
    Error &error)
{
    const auto patterns = regexPatterns(lexSpec);
    const std::size_t originalCount = lexSpec.regex.size() + (lexSpec.identifier.has_value() ? 1u : 0u);
    const std::string runtimeNote = patterns.size() < originalCount
        ? Utils::loadTemplate("mermaid_lexer_intro_runtime.html") : std::string();
    if (patterns.empty())
    {
        if (!runtimeNote.empty()) return runtimeNote;
        // 没有正则可画：页面导语是一份静态片段（原文末尾不带换行）
        return tmpl::renderValues(
            Utils::loadTemplate("mermaid_lexer_intro_empty.html"));
    }
    RegexCompileResult compiled;
    if (!RegexDfa::compileAll(patterns, compiled, error))
    {
        return std::string();
    }
    std::vector<std::string> literalTokens;
    for (const auto &entry : tokenDefs)
    {
        literalTokens.push_back(htmlEscape(entry.first));
    }
    std::vector<MermaidLexerRow> rows;
    for (const CompiledRegex &item : compiled.compiled)
    {
        const bool identifier = (item.name == k_identifierToken);
        MermaidLexerRow row;
        row.name = htmlEscape(identifier ? "标识符" : item.name);
        row.pattern = htmlEscape(item.pattern);
        row.states = std::to_string(item.dfa.stateCount);
        row.accepts = htmlEscape(acceptStateText(item.dfa.accept));
        rows.push_back(std::move(row));
    }
    std::vector<MermaidCellRow> cells;
    for (std::size_t index = 0; index < compiled.cells.size(); ++index)
    {
        MermaidCellRow row;
        row.index = std::to_string(index);
        row.text = htmlEscape(cellText(compiled.cells[index]));
        cells.push_back(std::move(row));
    }
    return runtimeNote + tmpl::renderValues(
        Utils::loadTemplate("mermaid_lexer_intro.html"),
        {{"literal_tokens", tmpl::encode(literalTokens)},
         {"rows", tmpl::encode(rows)},
         {"cells", tmpl::encode(cells)}});
}

std::string Mermaid::htmlPage(
    const std::string &title, const std::string &intro,
    const std::vector<MermaidDiagram> &diagrams,
    const std::vector<std::pair<std::string, std::string>> &links)
{
    // 单图页面的 <h1> 就是图标题，不再重复一次 <h2>
    const bool titleIsDiagram =
        (diagrams.size() == 1) && (diagrams.front().title == title);
    std::vector<MermaidPageLink> linkRows;
    for (const auto &link : links)
    {
        MermaidPageLink row;
        row.href = htmlEscape(link.first);
        row.desc = link.second;
        linkRows.push_back(std::move(row));
    }
    std::vector<MermaidPageDiagram> pages;
    for (const MermaidDiagram &diagram : diagrams)
    {
        MermaidPageDiagram page;
        page.title = htmlEscape(diagram.title);
        page.note = diagram.note;
        page.source = diagram.source;
        page.sourceEscaped = htmlEscape(diagram.source);
        page.showTitle = !titleIsDiagram;
        pages.push_back(std::move(page));
    }
    return tmpl::renderValues(
        Utils::loadTemplate("mermaid_page.html"),
        {{"title_escaped", htmlEscape(title)},
         {"intro", intro},
         {"page_style", tmpl::renderValues(
                            Utils::loadTemplate("mermaid_page_style.txt"))},
         {"links", tmpl::encode(linkRows)},
         {"diagrams", tmpl::encode(pages)}});
}

std::string Mermaid::joinSources(const std::vector<MermaidDiagram> &diagrams)
{
    std::vector<MermaidPageDiagram> rows;
    for (const MermaidDiagram &diagram : diagrams)
    {
        MermaidPageDiagram row;
        row.title = diagram.title;
        row.source = diagram.source;
        rows.push_back(std::move(row));
    }
    return tmpl::renderValues(Utils::loadTemplate("mermaid_sources.mmd"),
                              {{"diagrams", tmpl::encode(rows)}});
}

std::string Mermaid::markdownFences(const std::vector<MermaidDiagram> &diagrams)
{
    std::vector<MermaidPageDiagram> rows;
    for (const MermaidDiagram &diagram : diagrams)
    {
        MermaidPageDiagram row;
        row.title = diagram.title;
        row.source = diagram.source;
        rows.push_back(std::move(row));
    }
    return tmpl::renderValues(Utils::loadTemplate("mermaid_fences.md"),
                              {{"diagrams", tmpl::encode(rows)}});
}

bool Mermaid::projectFiles(
    const AnalysisResult &analysis, const LexSpec &lexSpec,
    const std::map<std::string, std::vector<std::string>> &tokenDefs,
    bool enabled, std::vector<std::pair<std::string, std::string>> &out,
    Error &error)
{
    std::vector<std::pair<std::string, std::string>> files;
    if (!enabled)
    {
        out = std::move(files);
        return true;
    }
    const std::vector<MermaidDiagram> grammar = grammarDiagrams(analysis);
    const std::vector<MermaidDiagram> lexer = lexerDiagrams(lexSpec, error);
    if (!error.empty())
    {
        return false;
    }
    if (grammar.empty() && lexer.empty())
    {
        out = std::move(files);
        return true;
    }
    if (!grammar.empty())
    {
        files.emplace_back("mermaid/grammar.html",
                           htmlPage(grammar.front().title, "",
                                    grammar, {}));
        files.emplace_back("mermaid/grammar.mmd", joinSources(grammar));
    }
    if (!lexer.empty())
    {
        files.emplace_back(
            "mermaid/lexer.html",
            htmlPage(tmpl::renderValues(
                         Utils::loadTemplate("mermaid_lexer_page_title.txt"),
                         {{"count", static_cast<long long>(lexer.size())}}),
                     lexerIntro(lexSpec, tokenDefs, error), lexer, {}));
        files.emplace_back("mermaid/lexer.mmd", joinSources(lexer));
    }
    std::vector<std::pair<std::string, std::string>> links;
    if (!grammar.empty())
    {
        links.emplace_back("grammar.html",
                           "文法 " + modeLabelText(analysis.mode) +
                               " 自动机（LR 状态机）");
    }
    if (!lexer.empty())
    {
        links.emplace_back("lexer.html", "词法 DFA（每个正则 Token 一张）");
    }
    links.emplace_back("grammar.mmd",
                       "文法图的 Mermaid 源码（可复制到 Markdown 等）");
    if (!lexer.empty())
    {
        links.emplace_back("lexer.mmd", "词法图的 Mermaid 源码");
    }
    std::vector<MermaidDiagram> all = grammar;
    all.insert(all.end(), lexer.begin(), lexer.end());
    const std::string intro = tmpl::renderValues(
        Utils::loadTemplate("mermaid_index_intro.html"),
        {{"mode_label", htmlEscape(modeLabelText(analysis.mode))}});
    files.emplace_back(
        "mermaid/index.html",
        htmlPage(tmpl::renderValues(
                     Utils::loadTemplate("mermaid_index_title.txt")),
                 intro, all, links));
    out = std::move(files);
    return true;
}

} // namespace ParseEngine

// 字段绑定只能写在全局作用域（`TEMPLATE_MODEL` 展开的是显式特化）
TEMPLATE_MODEL(ParseEngine::MermaidStateNode,
               tmpl::field("id", &ParseEngine::MermaidStateNode::id),
               tmpl::field("is_start", &ParseEngine::MermaidStateNode::isStart),
               tmpl::field("is_accept", &ParseEngine::MermaidStateNode::isAccept),
               tmpl::field("items", &ParseEngine::MermaidStateNode::items),
               tmpl::field("hidden_items",
                           &ParseEngine::MermaidStateNode::hiddenItems),
               tmpl::field("actions", &ParseEngine::MermaidStateNode::actions),
               tmpl::field("klass", &ParseEngine::MermaidStateNode::klass));

TEMPLATE_MODEL(ParseEngine::MermaidGraphEdge,
               tmpl::field("from", &ParseEngine::MermaidGraphEdge::from),
               tmpl::field("to", &ParseEngine::MermaidGraphEdge::to),
               tmpl::field("label", &ParseEngine::MermaidGraphEdge::label));

TEMPLATE_MODEL(ParseEngine::MermaidDfaState,
               tmpl::field("id", &ParseEngine::MermaidDfaState::id),
               tmpl::field("is_accept", &ParseEngine::MermaidDfaState::isAccept));

TEMPLATE_MODEL(ParseEngine::MermaidDfaClass,
               tmpl::field("id", &ParseEngine::MermaidDfaClass::id),
               tmpl::field("klass", &ParseEngine::MermaidDfaClass::klass));

TEMPLATE_MODEL(ParseEngine::MermaidActionPart,
               tmpl::field("word", &ParseEngine::MermaidActionPart::word),
               tmpl::field("values", &ParseEngine::MermaidActionPart::values));

TEMPLATE_MODEL(ParseEngine::MermaidLexerRow,
               tmpl::field("name", &ParseEngine::MermaidLexerRow::name),
               tmpl::field("pattern", &ParseEngine::MermaidLexerRow::pattern),
               tmpl::field("states", &ParseEngine::MermaidLexerRow::states),
               tmpl::field("accepts", &ParseEngine::MermaidLexerRow::accepts));

TEMPLATE_MODEL(ParseEngine::MermaidCellRow,
               tmpl::field("index", &ParseEngine::MermaidCellRow::index),
               tmpl::field("text", &ParseEngine::MermaidCellRow::text));

TEMPLATE_MODEL(ParseEngine::MermaidPageLink,
               tmpl::field("href", &ParseEngine::MermaidPageLink::href),
               tmpl::field("desc", &ParseEngine::MermaidPageLink::desc));

TEMPLATE_MODEL(ParseEngine::MermaidPageDiagram,
               tmpl::field("title", &ParseEngine::MermaidPageDiagram::title),
               tmpl::field("note", &ParseEngine::MermaidPageDiagram::note),
               tmpl::field("source", &ParseEngine::MermaidPageDiagram::source),
               tmpl::field("source_escaped",
                           &ParseEngine::MermaidPageDiagram::sourceEscaped),
               tmpl::field("show_title",
                           &ParseEngine::MermaidPageDiagram::showTitle));
