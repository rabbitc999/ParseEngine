/**
 * @file code_gen_python.cpp
 * @brief Python 语言后端实现：把生成期算好的表重排成 Python 字面量，再套进
 *        source/templates/python_*.tmpl 里的标准库运行时。
 */

#include "ParseEngine/code_gen_python.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "ParseEngine/action_translator.hpp"
#include "ParseEngine/code_gen.hpp"
#include "ParseEngine/chart_grammar.hpp"
#include "ParseEngine/grammar_types.hpp"
#include "ParseEngine/utils.hpp"

// Python 产物模板（source/templates/python_*.tmpl）走 library/template 的模板引擎
#include "template/engine.hpp"

namespace ParseEngine
{

namespace
{

/**
 * @brief 整数表里的一个数（`python_int_array.txt` 的元素）。
 */
struct PyIntItem
{
    bool br = false;     ///< 这个元素要不要另起一行（生成器按「每行几个」算好）
    long long value = 0; ///< 数值
};

/**
 * @brief 整数表里的一行（`python_int_rows.txt` 的元素）。
 */
struct PyIntRow
{
    std::vector<PyIntItem> items; ///< 该行的各个数
};

/**
 * @brief 一个非终结符的 FOLLOW 行（`python_ll_tables.py` 的数组元素）。
 */
struct PyFollowRow
{
    bool empty = false;               ///< 没有 FOLLOW 成员 → 产物里是 `frozenset()`
    std::vector<std::string> members; ///< Token 名（不含 `TOKEN_` 前缀）
};

/**
 * @brief 一条固定字面量词法规则（`python_lex_rules.py` 的数组元素）。
 */
struct PyLexRule
{
    std::string literal;   ///< 已转义的 Python 字符串字面量
    std::string tokenName; ///< Token 名（不含 `TOKEN_` 前缀）
    int length = 0;        ///< 字面量的 UTF-8 字符数
    bool blank = false;    ///< 字面量以空白 / `\b` 通配开头
    int order = 0;         ///< 声明序（模板里不出现，排序用）
    int declOrder = 0;     ///< 词法声明序（产物里是最后一列）
};

/**
 * @brief 关键字表里的一行（`python_keywords.py` 的数组元素）。
 */
struct PyKeyword
{
    std::string literal;   ///< 已转义的字面量
    std::string tokenName; ///< Token 名（不含 `TOKEN_` 前缀）
};

/**
 * @brief `%token regex` 的一行（`python_lex_regexes.py` 的数组元素）。
 */
struct PyRegexRule
{
    std::string pattern;   ///< 已转义的正则
    std::string tokenName; ///< Token 名（不含 `TOKEN_` 前缀）
    int order = 0;         ///< 词法声明序
};

/**
 * @brief `%commit` / `%trivia` 的一行（`_COMMIT_RES` 的元素）。
 */
struct PyCommitPattern
{
    std::string pattern; ///< 已转义的正则
    std::string type;    ///< 已转义的片段类型名
    bool skip = false;   ///< `%commit skip` 命中的片段不进容器
    int order = 0;       ///< 声明序
};

/**
 * @brief `COMMIT_ALIASES` 的一行。
 */
struct PyCommitAlias
{
    std::string type;  ///< 已转义的片段类型名
    std::string alias; ///< 已转义的别名
};

/**
 * @brief `_EXPECTED` 的一行（`python_constants.py` 的数组元素）。
 */
struct PyExpectedRow
{
    std::string cells; ///< 该行期望的 Token 号正文（`pyIntList` 渲染好的）
};

/**
 * @brief 一个 Token 的编号行（`python_token_types.py` 的数组元素）。
 */
struct PyTokenId
{
    std::string name; ///< Token 名（不含 `TOKEN_` 前缀）
    int value = 0;    ///< 类型值（就是它在终结符表里的下标）
};

/**
 * @brief `TOKEN_ALIASES` 的一行。
 */
struct PyTokenAlias
{
    std::string name;  ///< 已转义的 Token 名
    std::string alias; ///< 已转义的别名
};

/**
 * @brief 语义动作分支里的一段（`python_action_cases.py` 的数组元素）。
 */
struct PyActionCase
{
    int id = 0;                     ///< 产生式编号
    bool hasMarker = false;         ///< 有「动作来自 .gram 第 N 行」的注释
    std::string marker;             ///< 注释正文（不含缩进与换行）
    std::vector<std::string> lines; ///< 动作正文（已按行拆好，缩进由模板给）
};

/**
 * @brief LL(1) 展开分支里的一次 `_ll_push`（`python_expand_cases.py`）。
 */
struct PyPushCall
{
    std::string kind;      ///< `_ITEM_TERMINAL` / `_ITEM_NONTERM` / `_ITEM_ACTION` …
    bool terminal = false; ///< 终结符写 `TOKEN_名字`，否则写编号
    std::string name;      ///< 终结符名（不含 `TOKEN_` 前缀）
    int id = 0;            ///< 非终结符 / 产生式编号
    int slot = 0;          ///< 语义槽位
};

/**
 * @brief LL(1) 的一个产生式展开分支（`python_expand_cases.py` 的数组元素）。
 */
struct PyExpandProd
{
    int id = 0;                     ///< 产生式编号
    std::string note;               ///< 单独一行的注释（空尾规则用）
    std::vector<PyPushCall> pushes; ///< 逆序压栈的那些调用
};

} // namespace
} // namespace ParseEngine

// 字段绑定只能写在全局作用域（`TEMPLATE_MODEL` 展开的是显式特化）
TEMPLATE_MODEL(ParseEngine::PyIntItem,
               tmpl::field("br", &ParseEngine::PyIntItem::br),
               tmpl::field("value", &ParseEngine::PyIntItem::value));

TEMPLATE_MODEL(ParseEngine::PyIntRow,
               tmpl::field("items", &ParseEngine::PyIntRow::items));

TEMPLATE_MODEL(ParseEngine::PyFollowRow,
               tmpl::field("empty", &ParseEngine::PyFollowRow::empty),
               tmpl::field("members", &ParseEngine::PyFollowRow::members));

TEMPLATE_MODEL(ParseEngine::PyLexRule,
               tmpl::field("literal", &ParseEngine::PyLexRule::literal),
               tmpl::field("token_name", &ParseEngine::PyLexRule::tokenName),
               tmpl::field("length", &ParseEngine::PyLexRule::length),
               tmpl::field("blank", &ParseEngine::PyLexRule::blank),
               tmpl::field("decl_order", &ParseEngine::PyLexRule::declOrder));

TEMPLATE_MODEL(ParseEngine::PyKeyword,
               tmpl::field("literal", &ParseEngine::PyKeyword::literal),
               tmpl::field("token_name", &ParseEngine::PyKeyword::tokenName));

TEMPLATE_MODEL(ParseEngine::PyRegexRule,
               tmpl::field("pattern", &ParseEngine::PyRegexRule::pattern),
               tmpl::field("token_name", &ParseEngine::PyRegexRule::tokenName),
               tmpl::field("order", &ParseEngine::PyRegexRule::order));

TEMPLATE_MODEL(ParseEngine::PyCommitPattern,
               tmpl::field("pattern", &ParseEngine::PyCommitPattern::pattern),
               tmpl::field("type", &ParseEngine::PyCommitPattern::type),
               tmpl::field("skip", &ParseEngine::PyCommitPattern::skip),
               tmpl::field("order", &ParseEngine::PyCommitPattern::order));

TEMPLATE_MODEL(ParseEngine::PyCommitAlias,
               tmpl::field("type", &ParseEngine::PyCommitAlias::type),
               tmpl::field("alias", &ParseEngine::PyCommitAlias::alias));

TEMPLATE_MODEL(ParseEngine::PyExpectedRow,
               tmpl::field("cells", &ParseEngine::PyExpectedRow::cells));

TEMPLATE_MODEL(ParseEngine::PyTokenId,
               tmpl::field("name", &ParseEngine::PyTokenId::name),
               tmpl::field("value", &ParseEngine::PyTokenId::value));

TEMPLATE_MODEL(ParseEngine::PyTokenAlias,
               tmpl::field("name", &ParseEngine::PyTokenAlias::name),
               tmpl::field("alias", &ParseEngine::PyTokenAlias::alias));

TEMPLATE_MODEL(ParseEngine::PyActionCase,
               tmpl::field("id", &ParseEngine::PyActionCase::id),
               tmpl::field("has_marker", &ParseEngine::PyActionCase::hasMarker),
               tmpl::field("marker", &ParseEngine::PyActionCase::marker),
               tmpl::field("lines", &ParseEngine::PyActionCase::lines));

TEMPLATE_MODEL(ParseEngine::PyPushCall,
               tmpl::field("kind", &ParseEngine::PyPushCall::kind),
               tmpl::field("terminal", &ParseEngine::PyPushCall::terminal),
               tmpl::field("name", &ParseEngine::PyPushCall::name),
               tmpl::field("id", &ParseEngine::PyPushCall::id),
               tmpl::field("slot", &ParseEngine::PyPushCall::slot));

TEMPLATE_MODEL(ParseEngine::PyExpandProd,
               tmpl::field("id", &ParseEngine::PyExpandProd::id),
               tmpl::field("note", &ParseEngine::PyExpandProd::note),
               tmpl::field("pushes", &ParseEngine::PyExpandProd::pushes));

namespace ParseEngine
{
namespace
{

/**
 * @brief 渲染一段「当值用」的模板。
 *
 * 与块级模板的区别：模板文件按仓库惯例以换行收尾，而这里的结果是要**插进别的
 * 产物里当值**的（比如一张表、一个分段标记），多带一个换行就是产物里多出来的一
 * 行，所以取完把结尾的换行收掉。
 * @param name 模板名（`Utils::loadTemplate` 的名字，不带后缀）。
 * @param values 模板变量。
 * @return 渲染结果；结尾的换行已去掉。
 */
std::string renderValueTemplate(
    const std::string &name,
    std::initializer_list<std::pair<std::string, tmpl::Value>> values)
{
    std::string text = tmpl::renderValues(Utils::loadTemplate(name), values);
    while (!text.empty() && ((text.back() == '\n') || (text.back() == '\r')))
    {
        text.pop_back();
    }
    return text;
}

/**
 * @brief 生成 Python 字符串字面量（UTF-8 源码，控制字符转义）。
 * @param text 输入文本。
 * @return 字面量文本（含引号）。
 */
std::string pyString(const std::string &text)
{
    std::string out = "\"";
    for (const char raw : text)
    {
        const unsigned char ch = static_cast<unsigned char>(raw);
        switch (ch)
        {
        case '\\':
            out += "\\\\";
            break;
        case '"':
            out += "\\\"";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if ((ch < 0x20u) || (ch == 0x7Fu))
            {
                char buffer[8];
                std::snprintf(buffer, sizeof(buffer), "\\x%02x",
                              static_cast<unsigned>(ch));
                out += buffer;
            }
            else
            {
                out += static_cast<char>(ch);
            }
            break;
        }
    }
    out += "\"";
    return out;
}

/// 一维整数表 → 模板的元素数组（哪几个数之后要换行在这里算，排版在模板里）。
std::vector<PyIntItem> intItems(const std::vector<long long> &values,
                                std::size_t perLine)
{
    std::vector<PyIntItem> items;
    items.reserve(values.size());
    for (std::size_t index = 0u; index < values.size(); ++index)
    {
        PyIntItem item;
        item.br = (index != 0u) && ((index % perLine) == 0u);
        item.value = values[index];
        items.push_back(item);
    }
    return items;
}

/// 一维整数列表的正文（不含方括号）：每行 perLine 个，续行缩进 indent。
std::string pyIntList(const std::vector<long long> &values,
                      const std::string &indent, std::size_t perLine)
{
    return renderValueTemplate(
        "python_int_array.txt",
        {{"values", tmpl::encode(intItems(values, perLine))},
         {"indent", indent}});
}

/// 二维整数表（ACTION / GOTO / 预测分析表）：方括号、换行、缩进都在模板里。
std::string pyIntRows(const std::vector<std::vector<long long>> &rows,
                      const std::string &indent)
{
    std::vector<PyIntRow> lines;
    lines.reserve(rows.size());
    for (const std::vector<long long> &row : rows)
    {
        PyIntRow line;
        line.items = intItems(row, 12u);
        lines.push_back(line);
    }
    return renderValueTemplate("python_int_rows.txt",
                               {{"rows", tmpl::encode(lines)},
                                {"indent", indent}});
}

/// FOLLOW 集 → 模板的行数组（一个非终结符一行；Token 名不带 `TOKEN_` 前缀，
/// 空行由模板写成 `frozenset()`）。
std::vector<PyFollowRow> pyFollowRows(const AnalysisResult &analysis)
{
    std::vector<PyFollowRow> rows;
    rows.reserve(analysis.nonterms.size());
    for (const std::string &nonterm : analysis.nonterms)
    {
        const auto followIt = analysis.follow.find(nonterm);
        const std::set<std::string> *follow =
            followIt != analysis.follow.end() ? &followIt->second : nullptr;
        PyFollowRow row;
        for (const std::string &terminal : analysis.terminals)
        {
            if ((follow != nullptr) && (follow->count(terminal) != 0u))
            {
                row.members.push_back(terminal);
            }
        }
        row.empty = row.members.empty();
        rows.push_back(row);
    }
    return rows;
}

/**
 * @brief 非终结符到编号的映射。
 * @param nonterms 非终结符列表。
 * @return 名字到编号的映射。
 */
std::map<std::string, int> nontermIds(
    const std::vector<std::string> &nonterms)
{
    std::map<std::string, int> ids;
    for (std::size_t index = 0u; index < nonterms.size(); ++index)
    {
        ids[nonterms[index]] = static_cast<int>(index);
    }
    return ids;
}

/* ---------------- 分析表 ---------------- */

/**
 * @brief ACTION / GOTO / 产生式元数据（LR 家族共用）。
 * @param analysis 分析结果。
 * @return Python 代码段。
 */
std::string pyLrTables(const AnalysisResult &analysis)
{
    const std::size_t stateCount = analysis.stateCount();
    std::vector<std::vector<long long>> actions;
    std::vector<std::vector<long long>> gotos;
    for (std::size_t state = 0u; state < stateCount; ++state)
    {
        std::vector<long long> row;
        for (const std::string &terminal : analysis.terminals)
        {
            const auto found = analysis.action.find(std::make_pair(
                static_cast<int>(state), terminal));
            if (found == analysis.action.end())
            {
                row.push_back(0);
            }
            else if (found->second.kind == LrAction::Kind::Shift)
            {
                row.push_back(found->second.value + 1);
            }
            else if (found->second.kind == LrAction::Kind::Reduce)
            {
                row.push_back(-found->second.value - 1);
            }
            else
            {
                row.push_back(2147483647);
            }
        }
        actions.push_back(row);
    }
    for (std::size_t state = 0u; state < stateCount; ++state)
    {
        std::vector<long long> row;
        for (std::size_t index = 0u; index < analysis.nonterms.size(); ++index)
        {
            const auto found = analysis.gotoTable.find(std::make_pair(
                static_cast<int>(state), static_cast<int>(index)));
            row.push_back((found == analysis.gotoTable.end())
                              ? 65535
                              : static_cast<long long>(found->second));
        }
        gotos.push_back(row);
    }
    const std::map<std::string, int> ids = nontermIds(analysis.nonterms);
    std::vector<long long> lengths;
    std::vector<long long> lhs;
    for (const Production &production : analysis.productions)
    {
        lengths.push_back(static_cast<long long>(production.rhs.size()));
        lhs.push_back(ids.at(production.lhs));
    }
    const bool hasReduce = analysis.lookaheadK > 1;
    std::vector<std::vector<long long>> reduces;
    std::vector<long long> codeList;
    std::vector<long long> columnList;
    if (hasReduce)
    {
        // LR(k) / LALR(k)：ACTION 的第二层——按 k 元组展望符建列的归约/接受。
        // 编码 = Σ Token号 × 终结符个数^i（低位是窗口头），与运行时的
        // _lookahead_code 一致；按编码排好序让运行时二分。
        const std::size_t columnCount = analysis.lookaheadColumns.size();
        std::map<std::string, int> terminalIds;
        for (std::size_t index = 0u; index < analysis.terminals.size(); ++index)
        {
            terminalIds[analysis.terminals[index]] = static_cast<int>(index);
        }
        for (std::size_t state = 0u; state < stateCount; ++state)
        {
            std::vector<long long> row;
            for (std::size_t column = 0u; column < columnCount; ++column)
            {
                const std::string key =
                    lookaheadKeyOf(analysis.lookaheadColumns[column]);
                const auto found = analysis.action.find(std::make_pair(
                    static_cast<int>(state), key));
                if (found == analysis.action.end())
                {
                    row.push_back(0);
                }
                else if (found->second.kind == LrAction::Kind::Reduce)
                {
                    row.push_back(-found->second.value - 1);
                }
                else if (found->second.kind == LrAction::Kind::Accept)
                {
                    row.push_back(2147483647);
                }
                else
                {
                    row.push_back(0);
                }
            }
            reduces.push_back(row);
        }
        const long long base = static_cast<long long>(analysis.terminals.size());
        std::vector<std::pair<long long, std::size_t>> codes;
        for (std::size_t column = 0u; column < columnCount; ++column)
        {
            long long code = 0;
            long long scale = 1;
            for (const std::string &symbol : analysis.lookaheadColumns[column])
            {
                code += static_cast<long long>(terminalIds[symbol]) * scale;
                scale *= base;
            }
            codes.push_back({code, column});
        }
        std::sort(codes.begin(), codes.end());
        for (const auto &entry : codes)
        {
            codeList.push_back(entry.first);
            columnList.push_back(static_cast<long long>(entry.second));
        }
    }
    // 产物文本（`_ACTION = ` 这些行、节标题、方括号与换行）都在
    // python_lr_tables.py 里，生成器只摆数据。
    return tmpl::renderValues(
        Utils::loadTemplate("python_lr_tables.py"),
        {{"action", pyIntRows(actions, "")},
         {"goto", pyIntRows(gotos, "")},
         {"has_reduce", hasReduce},
         {"reduce", pyIntRows(reduces, "")},
         {"lookahead_code", pyIntList(codeList, "            ", 16u)},
         {"lookahead_column", pyIntList(columnList, "            ", 16u)},
         {"production_length", pyIntList(lengths, "            ", 16u)},
         {"production_lhs", pyIntList(lhs, "            ", 16u)}});
}

/**
 * @brief 预测分析表与左递归空尾标记表（LL(1)）。
 * @param analysis 分析结果。
 * @return Python 代码段。
 */
std::string pyLlTables(const AnalysisResult &analysis)
{
    // 列轴：k = 1 是 Token（密表）、k = 0 只有一列、k >= 2 是「用到的 k 元组」
    const int k = analysis.lookaheadK;
    std::vector<std::string> columnKeys;
    if (k == 0)
    {
        columnKeys.push_back("");
    }
    else if (k == 1)
    {
        columnKeys = analysis.terminals;
    }
    else
    {
        for (const std::vector<std::string> &column : analysis.lookaheadColumns)
        {
            columnKeys.push_back(lookaheadKeyOf(column));
        }
    }
    std::vector<std::vector<long long>> table;
    for (const std::string &nonterm : analysis.nonterms)
    {
        std::vector<long long> row;
        for (const std::string &columnKey : columnKeys)
        {
            const auto found = analysis.table.find(std::make_pair(
                nonterm, columnKey));
            row.push_back((found == analysis.table.end())
                              ? -1
                              : static_cast<long long>(found->second));
        }
        table.push_back(row);
    }
    std::vector<long long> leftEmpty;
    for (const Production &production : analysis.productions)
    {
        leftEmpty.push_back((production.transform == "left_empty") ? 1 : 0);
    }
    std::vector<long long> codeList;
    std::vector<long long> columnList;
    if (k >= 2)
    {
        std::map<std::string, int> terminalIds;
        for (std::size_t index = 0u; index < analysis.terminals.size(); ++index)
        {
            terminalIds[analysis.terminals[index]] = static_cast<int>(index);
        }
        const long long base = static_cast<long long>(analysis.terminals.size());
        std::vector<std::pair<long long, std::size_t>> codes;
        for (std::size_t column = 0u; column < analysis.lookaheadColumns.size();
             ++column)
        {
            long long code = 0;
            long long scale = 1;
            for (const std::string &symbol : analysis.lookaheadColumns[column])
            {
                code += static_cast<long long>(terminalIds[symbol]) * scale;
                scale *= base;
            }
            codes.push_back({code, column});
        }
        std::sort(codes.begin(), codes.end());
        for (const auto &entry : codes)
        {
            codeList.push_back(entry.first);
            columnList.push_back(static_cast<long long>(entry.second));
        }
    }
    // 产物文本全在 python_ll_tables.py 里（`_LL_TABLE = ` 这些行、节标题、
    // FOLLOW 集），生成器只摆数据。
    return tmpl::renderValues(
        Utils::loadTemplate("python_ll_tables.py"),
        {{"ll_table", pyIntRows(table, "")},
         {"has_columns", k >= 2},
         {"lookahead_code", pyIntList(codeList, "            ", 16u)},
         {"lookahead_column", pyIntList(columnList, "            ", 16u)},
         {"left_empty", pyIntList(leftEmpty, "            ", 16u)},
         {"has_follow", k != 0},
         {"follow", tmpl::encode(pyFollowRows(analysis))}});
}

/**
 * @brief GLR 的冲突候选动作表与起始符号。
 *
 * 主动作在 ACTION 表里（与其它 LR 模式同一套）；这里只放「同一个 (状态, Token)
 * 单元里除主动作外的候选」，运行时按它把一个 GSS 节点分裂成多条路径。
 * @param analysis 分析结果。
 * @return Python 代码段。
 */
std::string pyGlrTables(const AnalysisResult &analysis)
{
    const std::size_t stateCount = analysis.stateCount();
    const std::size_t termCount = analysis.terminals.size();
    const std::size_t cellCount =
        (stateCount == 0u || termCount == 0u) ? 1u : stateCount * termCount;
    std::map<std::string, std::size_t> termIndex;
    for (std::size_t index = 0u; index < analysis.terminals.size(); ++index)
    {
        termIndex[analysis.terminals[index]] = index;
    }
    std::vector<long long> starts(cellCount, 0);
    std::vector<long long> counts(cellCount, 0);
    std::vector<long long> data;
    for (const auto &entry : analysis.glrAlternatives)
    {
        const auto column = termIndex.find(entry.first.second);
        if ((column == termIndex.end()) || (stateCount == 0u))
        {
            continue;
        }
        const std::size_t cell =
            static_cast<std::size_t>(entry.first.first) * termCount +
            column->second;
        if (cell >= cellCount)
        {
            continue;
        }
        std::vector<long long> extras;
        for (std::size_t index = 1u; index < entry.second.size(); ++index)
        {
            const LrAction &action = entry.second[index];
            if (action.kind == LrAction::Kind::Shift)
            {
                extras.push_back(action.value + 1);
            }
            else if (action.kind == LrAction::Kind::Reduce)
            {
                extras.push_back(-action.value - 1);
            }
            else
            {
                extras.push_back(2147483647);
            }
        }
        if (extras.empty())
        {
            continue;
        }
        starts[cell] = static_cast<long long>(data.size());
        counts[cell] = static_cast<long long>(extras.size());
        for (const long long value : extras)
        {
            data.push_back(value);
        }
    }
    const auto toRows = [termCount, stateCount, &starts, &counts](bool useStart)
    {
        std::vector<std::vector<long long>> rows;
        if ((stateCount == 0u) || (termCount == 0u))
        {
            rows.push_back({0});
            return rows;
        }
        for (std::size_t state = 0u; state < stateCount; ++state)
        {
            std::vector<long long> row;
            for (std::size_t index = 0u; index < termCount; ++index)
            {
                const std::size_t cell = (state * termCount) + index;
                if (useStart)
                {
                    row.push_back((counts[cell] > 0) ? starts[cell] : 65535);
                }
                else
                {
                    row.push_back(counts[cell]);
                }
            }
            rows.push_back(row);
        }
        return rows;
    };
    // 接受状态由增广产生式 S' -> S . 触发；进入该状态的边符号是用户起始符号 S。
    std::string startSymbol = analysis.grammar.start;
    const auto startRulesIt =
        analysis.grammar.rules.find(analysis.grammar.start);
    const std::vector<Production> *startRules =
        startRulesIt != analysis.grammar.rules.end() ? &startRulesIt->second
                                                     : nullptr;
    if ((startRules != nullptr) && !startRules->empty() &&
        !startRules->front().rhs.empty())
    {
        startSymbol = startRules->front().rhs.front();
    }
    std::size_t startId = 0u;
    for (std::size_t index = 0u; index < analysis.nonterms.size(); ++index)
    {
        if (analysis.nonterms[index] == startSymbol)
        {
            startId = index;
            break;
        }
    }
    return tmpl::renderValues(
        Utils::loadTemplate("python_glr_tables.py"),
        {{"start_symbol", static_cast<long long>(termCount + startId)},
         {"extra_start", pyIntRows(toRows(true), "")},
         {"extra_count", pyIntRows(toRows(false), "")},
         {"extra_data", pyIntList(
                            data.empty() ? std::vector<long long>{0} : data,
                            "            ", 16u)}});
}

/**
 * @brief 递归下降的表：非终结符 -> 产生式区间 + 语义槽位个数。
 *
 * 产生式在 `analysis.productions` 里是「同一左部连续排在一起」的（摊平时按
 * 非终结符名有序、规则内按声明顺序），所以一个非终结符对应一段连续编号；
 * 回溯时依次试的就是这个顺序。右部符号不在这里重发——直接查 `_TREE_PROD_*`
 * （那一套就是按同一份产生式编号建的）。
 * @param analysis 分析结果。
 * @return Python 代码段。
 */
std::string pyDdTables(const AnalysisResult &analysis)
{
    std::vector<long long> starts;
    std::vector<long long> counts;
    std::vector<long long> slots;
    const std::size_t total = analysis.productions.size();
    for (const std::string &nonterm : analysis.nonterms)
    {
        std::size_t first = total;
        std::size_t count = 0u;
        std::size_t maxRhs = 0u;
        for (std::size_t index = 0u; index < total; ++index)
        {
            const Production &production = analysis.productions[index];
            if (production.lhs != nonterm)
            {
                continue;
            }
            if (first == total)
            {
                first = index;
            }
            ++count;
            maxRhs = std::max(maxRhs, production.rhs.size());
        }
        starts.push_back(static_cast<long long>(first == total ? 0u : first));
        counts.push_back(static_cast<long long>(count));
        // 语义槽位：第 0 个留空，所以是「最长的右部 + 1」
        slots.push_back(static_cast<long long>(maxRhs + 1u));
    }
    return tmpl::renderValues(
        Utils::loadTemplate("python_dd_tables.py"),
        {{"prod_start", pyIntList(starts, "            ", 16u)},
         {"prod_count", pyIntList(counts, "            ", 16u)},
         {"prod_slots", pyIntList(slots, "            ", 16u)}});
}

/**
 * @brief 语法树元数据：产生式右部符号表（LL(1) 建树用）。
 * @param analysis 分析结果。
 * @return Python 代码段。
 */
std::string pyTreeTables(const AnalysisResult &analysis)
{
    std::set<std::string> terminalSet(analysis.terminals.begin(),
                                      analysis.terminals.end());
    const std::map<std::string, int> ids = nontermIds(analysis.nonterms);
    const long long termCount =
        static_cast<long long>(analysis.terminals.size());
    std::vector<long long> starts;
    std::vector<long long> lengths;
    std::vector<long long> symbols;
    for (const Production &production : analysis.productions)
    {
        starts.push_back(static_cast<long long>(symbols.size()));
        lengths.push_back(static_cast<long long>(production.rhs.size()));
        for (const std::string &symbol : production.rhs)
        {
            const auto found = std::find(analysis.terminals.begin(),
                                         analysis.terminals.end(), symbol);
            if (found != analysis.terminals.end())
            {
                symbols.push_back(
                    static_cast<long long>(found - analysis.terminals.begin()));
            }
            else
            {
                symbols.push_back(termCount + ids.at(symbol));
            }
        }
    }
    // 产物文本在 python_tree_tables.py 里（节标题、三个表的行与方括号）
    return tmpl::renderValues(
        Utils::loadTemplate("python_tree_tables.py"),
        {{"prod_start", pyIntList(starts, "            ", 16u)},
         {"prod_length", pyIntList(lengths, "            ", 16u)},
         {"prod_symbols",
          pyIntList(symbols.empty() ? std::vector<long long>{0} : symbols,
                    "            ", 16u)}});
}

std::string pyChartTables(const AnalysisResult &analysis)
{
    const ChartGrammar chart = buildChartGrammar(analysis);
    const auto asLong = [](const std::vector<int> &items)
    {
        return std::vector<long long>(items.begin(), items.end());
    };
    std::vector<int> unaryLhs, unaryToken, binaryLhs, binaryLeft, binaryRight;
    for (const auto &rule : chart.terminalRules)
    {
        unaryLhs.push_back(rule[0]);
        unaryToken.push_back(rule[1]);
    }
    for (const auto &rule : chart.binaryRules)
    {
        binaryLhs.push_back(rule[0]);
        binaryLeft.push_back(rule[1]);
        binaryRight.push_back(rule[2]);
    }
    const auto list = [&](const std::vector<int> &items)
    { return pyIntList(asLong(items), "    ", 16u); };
    return tmpl::renderValues(
        Utils::loadTemplate("python_chart_tables.py"),
        {{"start", chart.startNonterm},
         {"production_count", static_cast<int>(chart.lhs.size())},
         {"unary_count", static_cast<int>(unaryLhs.size())},
         {"binary_count", static_cast<int>(binaryLhs.size())},
         {"accepts_empty", chart.acceptsEmpty},
         {"lhs", list(chart.lhs)},
         {"rhs_start", list(chart.rhsStart)},
         {"rhs_length", list(chart.rhsLength)},
         {"rhs_symbols", list(chart.rhsSymbols)},
         {"unary_lhs", list(unaryLhs)},
         {"unary_token", list(unaryToken)},
         {"binary_lhs", list(binaryLhs)},
         {"binary_left", list(binaryLeft)},
         {"binary_right", list(binaryRight)}});
}

/* ---------------- 词法表 ---------------- */

/// 字面量的 UTF-8 字符数（与 C 目标的字节数无关，Python 侧一律按字符算）。
std::size_t utf8Length(const std::string &text)
{
    std::size_t count = 0u;
    for (const char raw : text)
    {
        if ((static_cast<unsigned char>(raw) & 0xC0u) != 0x80u)
        {
            ++count;
        }
    }
    return count;
}

/**
 * @brief 固定字面量词法规则表（按长度降序，同长度按定义序）。
 * @param tokenDefs 固定字面量 Token。
 * @return Python 代码段。
 */
std::string pyLexRules(
    const std::map<std::string, std::vector<std::string>> &tokenDefs,
    const std::map<std::string, int> &tokenLexOrder)
{
    std::vector<PyLexRule> rules;
    int order = 0;
    for (const auto &entry : tokenDefs)
    {
        const auto orderIt = tokenLexOrder.find(entry.first);
        const int declOrder = (orderIt != tokenLexOrder.end())
                                  ? orderIt->second
                                  : order;
        for (const std::string &literal : entry.second)
        {
            PyLexRule rule;
            rule.literal = pyString(literal);
            rule.tokenName = entry.first;
            rule.length = static_cast<int>(utf8Length(literal));
            rule.blank = literalCanStartBlank(literal);
            rule.order = order;
            rule.declOrder = declOrder;
            ++order;
            rules.push_back(rule);
        }
    }
    std::stable_sort(rules.begin(), rules.end(),
                     [](const PyLexRule &left, const PyLexRule &right)
                     {
                         if (left.length != right.length)
                         {
                             return left.length > right.length;
                         }
                         return left.order < right.order;
                     });
    // 节标题与整张规则表的文本在 python_lex_rules.py 里（它同时是
    // 「# ---------- 词法表 ----------」这一节的开头）
    return tmpl::renderValues(Utils::loadTemplate("python_lex_rules.py"),
                              {{"rules", tmpl::encode(rules)}});
}

/**
 * @brief 取 Token 前跳过的空白字符集（`%space`）。
 *
 * 字面量里的 `\b` 已经展开成六个空白字符，所以这里就是一张普通字符表。
 * @param spaceChars %space 声明的跳过字符集；nullptr 表示没写（缺省全部空白）。
 * @return Python 代码段。
 */
std::string pySkipChars(const std::string *spaceChars)
{
    const std::string &chars = (spaceChars == nullptr) ? blankChars()
                                                       : *spaceChars;
    return tmpl::renderValues(Utils::loadTemplate("python_lex_skip_chars.py"),
                              {{"chars", pyString(chars)}});
}

/**
 * @brief 关键字表（像标识符的字面量）：%identifier 的 DFA 命中后查它。
 * @param tokenDefs 固定字面量 Token。
 * @return Python 代码段。
 */
std::string pyKeywords(
    const std::map<std::string, std::vector<std::string>> &tokenDefs)
{
    std::vector<PyKeyword> keywords;
    for (const auto &entry : tokenDefs)
    {
        for (const std::string &literal : entry.second)
        {
            bool identifier =
                !literal.empty() &&
                ((std::isalpha(static_cast<unsigned char>(literal[0])) != 0) ||
                 (literal[0] == '_'));
            for (std::size_t index = 1u; identifier && (index < literal.size());
                 ++index)
            {
                const char ch = literal[index];
                identifier =
                    (std::isalnum(static_cast<unsigned char>(ch)) != 0) ||
                    (ch == '_');
            }
            if (identifier)
            {
                PyKeyword keyword;
                keyword.literal = pyString(literal);
                keyword.tokenName = entry.first;
                keywords.push_back(keyword);
            }
        }
    }
    return tmpl::renderValues(Utils::loadTemplate("python_keywords.py"),
                              {{"keywords", tmpl::encode(keywords)}});
}

/**
 * @brief `%token regex` / `%identifier` / `%commit` 的**正则表**。
 *
 * 产物里不再放 DFA 表：词法匹配交给 Python 的 `re` 模块（`re.compile` 在
 * 模块加载时编译一次）。生成期用自带编译器做的那一遍校验在文法加载时
 * （`readRegexPattern`）就做过了，认不下的由 `main.cpp` 分流：C99 是硬错误、
 * 其余档只警告（自带编译器不认识不等于 `re` 不认，见
 * `GrammarConfig::regexDiagnostics`）。
 * @param regexDefs 正则 Token（有序）。
 * @param identifierPattern 标识符正则；没写 %identifier 时为空。
 * @param commits %commit 声明（声明序）；没有时为空。
 * @param tokenLexOrder Token 名 -> 词法声明序（%commit 同长度比较用）。
 * @param out 输出 Python 代码段。
 */
void pyLexRegexes(const std::map<std::string, std::string> &regexDefs,
                  const std::optional<std::string> &identifierPattern,
                  const std::vector<CommitSpec> &commits,
                  const std::map<std::string, int> &tokenLexOrder,
                  std::string &out)
{
    const bool hasIdentifier = identifierPattern.has_value();
    // 这里不再自己校验正则：文法加载时（readRegexPattern）已经用自带编译器验过
    // 一遍，认不下的由 main.cpp 按目标语言分流——C99 是硬错误（它要编 DFA 表），
    // 其余档打警告后照常生成（见 GrammarConfig::regexDiagnostics）。
    // 三张正则表的文本都在 python_lex_regexes.py 里，这里只摆数据
    std::vector<PyRegexRule> regexRules;
    int fallbackOrder = 0;
    for (const auto &entry : regexDefs)
    {
        const auto orderIt = tokenLexOrder.find(entry.first);
        PyRegexRule rule;
        rule.pattern = pyString(entry.second);
        rule.tokenName = entry.first;
        rule.order = (orderIt != tokenLexOrder.end()) ? orderIt->second
                                                      : fallbackOrder;
        ++fallbackOrder;
        regexRules.push_back(rule);
    }
    std::vector<PyCommitPattern> commitRes;
    std::vector<PyCommitAlias> commitAliases;
    for (const CommitSpec &commit : commits)
    {
        PyCommitPattern item;
        item.pattern = pyString(commit.pattern);
        item.type = pyString(commit.type);
        item.skip = commit.isSkip;
        item.order = commit.order;
        commitRes.push_back(item);
        if (!commit.alias.empty())
        {
            PyCommitAlias alias;
            alias.type = pyString(commit.type);
            alias.alias = pyString(commit.alias);
            commitAliases.push_back(alias);
        }
    }
    out += tmpl::renderValues(
        Utils::loadTemplate("python_lex_regexes.py"),
        {{"regex_rules", tmpl::encode(regexRules)},
         {"has_identifier", hasIdentifier},
         {"identifier_pattern",
          hasIdentifier ? pyString(*identifierPattern) : std::string()},
         {"commit_res", tmpl::encode(commitRes)},
         {"commit_aliases", tmpl::encode(commitAliases)}});
}

/**
 * @brief 语义动作分支（LR / LL 共用一套 `_apply_action`）。
 * @param analysis 分析结果。
 * @param translateMode 传给动作翻译器的模式。
 * @param error 出错时写入错误说明。
 * @return Python 代码段；动作非法时返回空串并把错误写进 error。
 */
std::string pyActionCases(const AnalysisResult &analysis,
                          const std::string &translateMode,
                          Error &error)
{
    const auto trim = [](const std::string &text)
    {
        const std::size_t begin = text.find_first_not_of(" \t\n\r\v\f");
        if (begin == std::string::npos)
        {
            return std::string();
        }
        const std::size_t end = text.find_last_not_of(" \t\n\r\v\f");
        return text.substr(begin, end - begin + 1u);
    };
    std::vector<PyActionCase> cases;
    for (const Production &production : analysis.productions)
    {
        if (production.transform == "left_empty")
        {
            continue;
        }
        const std::string action = trim(production.action);
        std::string translated;
        if (action.empty())
        {
            translated = "result.value = None";
        }
        else if (!translateAction(action, translateMode,
                                  static_cast<int>(production.rhs.size()),
                                  nullptr, false, true, translated, error))
        {
            // 动作出错时补上 .gram 里的行号（翻译器手上只有动作原文）
            if ((error.line == 0u) && (production.actionLine != 0u))
            {
                error.line = production.actionLine;
            }
            return {};
        }
        // 动作正文按行拆开（空行丢掉），缩进由 python_action_cases.py 统一给
        PyActionCase item;
        item.id = production.id;
        std::size_t pos = 0u;
        while (pos <= translated.size())
        {
            const std::size_t found = translated.find('\n', pos);
            const std::string line =
                trim(translated.substr(pos, (found == std::string::npos)
                                                ? std::string::npos
                                                : found - pos));
            if (!line.empty())
            {
                item.lines.push_back(line);
            }
            if (found == std::string::npos)
            {
                break;
            }
            pos = found + 1u;
        }
        // 标注这段动作来自 .gram 的哪一行：Python 没有 #line，只能用注释
        item.marker =
            trim(CodeGenPython::grammarLineMarker(production.actionLine));
        item.hasMarker = !item.marker.empty();
        cases.push_back(item);
    }
    // 分支文本（`if/elif production == N:` 与 12 空格缩进）全在
    // python_action_cases.py 里
    return tmpl::renderValues(Utils::loadTemplate("python_action_cases.py"),
                              {{"cases", tmpl::encode(cases)}});
}

/**
 * @brief 产生式展开分支（仅 LL(1)）。
 * @param analysis 分析结果。
 * @param terminals 终结符列表。
 * @param ids 非终结符到编号的映射。
 * @return Python 代码段。
 */
std::string pyExpandCases(const AnalysisResult &analysis,
                          const std::vector<std::string> &terminals,
                          const std::map<std::string, int> &ids)
{
    const std::set<std::string> terminalSet(terminals.begin(),
                                            terminals.end());
    const auto makeCall = [](const std::string &kind, bool terminal,
                             const std::string &name, int id, int slot)
    {
        PyPushCall call;
        call.kind = kind;
        call.terminal = terminal;
        call.name = name;
        call.id = id;
        call.slot = slot;
        return call;
    };
    const auto pushSymbol = [&terminalSet, &ids, &makeCall](
                                const std::string &symbol, int slot)
    {
        if (terminalSet.count(symbol) != 0u)
        {
            return makeCall("_ITEM_TERMINAL", true, symbol, 0, slot);
        }
        return makeCall("_ITEM_NONTERM", false, std::string(),
                        ids.at(symbol), slot);
    };
    // 展开分支的文本全在 python_expand_cases.py 里，这里只摆数据
    std::vector<PyExpandProd> expandProds;
    for (const Production &production : analysis.productions)
    {
        const std::vector<std::string> &rhs = production.rhs;
        const std::string &transform = production.transform;
        PyExpandProd item;
        item.id = production.id;
        if (transform == "left_base")
        {
            const int baseLength = production.baseRhsLength;
            item.pushes.push_back(makeCall("_ITEM_LEFT_LOOP", false,
                                           std::string(),
                                           ids.at(rhs.back()), 0));
            item.pushes.push_back(makeCall("_ITEM_ACTION_KEEP", false,
                                           std::string(),
                                           production.id, 0));
            for (std::size_t index = static_cast<std::size_t>(baseLength);
                 index-- > 0u;)
            {
                item.pushes.push_back(
                    pushSymbol(rhs[index], 1 + static_cast<int>(index)));
            }
        }
        else if (transform == "left_continue")
        {
            item.pushes.push_back(makeCall("_ITEM_LEFT_LOOP", false,
                                           std::string(),
                                           ids.at(rhs.back()), 0));
            item.pushes.push_back(makeCall("_ITEM_ACTION_KEEP", false,
                                           std::string(),
                                           production.id, 0));
            for (std::size_t index = rhs.size() - 1u; index-- > 0u;)
            {
                item.pushes.push_back(
                    pushSymbol(rhs[index], 2 + static_cast<int>(index)));
            }
        }
        else if (transform == "left_empty")
        {
            item.note = "# 空尾规则由 LeftLoop 直接完成";
        }
        else
        {
            item.pushes.push_back(makeCall("_ITEM_ACTION", false,
                                           std::string(),
                                           production.id, 0));
            for (std::size_t index = rhs.size(); index-- > 0u;)
            {
                item.pushes.push_back(
                    pushSymbol(rhs[index], 1 + static_cast<int>(index)));
            }
        }
        expandProds.push_back(item);
    }
    return tmpl::renderValues(Utils::loadTemplate("python_expand_cases.py"),
                              {{"productions", tmpl::encode(expandProds)}});
}

/**
 * @brief 模式入口 `_run_loop`。
 * @param mode 归一化后的模式。
 * @return Python 方法文本。
 */
std::string pyRunLoop(const std::string &mode)
{
    return tmpl::renderValues(
        Utils::loadTemplate("python_run_loop_method.py"),
        {{"target", isDdMode(mode)
                        ? "_run_dd_loop"
                        : (isGlrMode(mode) ? "_run_glr_loop"
                                           : (isLrMode(mode) ? "_run_lr_loop"
                                                             : "_run_ll_loop"))}});
}

/* ---------------- 组装 ---------------- */

/**
 * @brief 容量常量段。
 * @param analysis 分析结果。
 * @param mode 归一化后的模式。
 * @return Python 代码段。
 */
std::string pyConstants(const AnalysisResult &analysis,
                        const std::string &mode)
{
    std::size_t maxSlots = 4u;
    for (const Production &production : analysis.productions)
    {
        maxSlots = std::max(maxSlots, production.rhs.size() + 3u);
    }
    const std::map<std::string, int> ids = nontermIds(analysis.nonterms);
    const auto startFound = ids.find(analysis.grammar.start);
    // `_EXPECTED` 的每一行：这一行期望哪些 Token（LR 与 LL 都要，放共用常量段）
    std::map<std::string, int> expectedIds;
    for (std::size_t index = 0u; index < analysis.terminals.size(); ++index)
    {
        expectedIds[analysis.terminals[index]] = static_cast<int>(index);
    }
    std::vector<PyExpectedRow> expectedRows;
    for (const std::vector<std::string> &row : expectedTokensByRow(analysis))
    {
        std::vector<long long> tokenIds;
        for (const std::string &token : row)
        {
            tokenIds.push_back(static_cast<long long>(expectedIds[token]));
        }
        PyExpectedRow item;
        item.cells = pyIntList(tokenIds, "", 16u);
        expectedRows.push_back(item);
    }
    const long long startId = (startFound == ids.end())
                                  ? 0
                                  : startFound->second;
    // 常量段的文本全在 python_constants.py 里，这里只摆数据
    return tmpl::renderValues(
        Utils::loadTemplate("python_constants.py"),
        {{"terminal_count",
          static_cast<long long>(analysis.terminals.size())},
         {"nonterm_count",
          static_cast<long long>(analysis.nonterms.size())},
         {"start_id", startId},
         {"state_count",
          static_cast<long long>(isLrMode(mode) ? analysis.stateCount() : 0u)},
         {"lookahead_k",
          static_cast<long long>(std::max(0, analysis.lookaheadK))},
         {"lookahead_column_count",
          static_cast<long long>(analysis.lookaheadColumns.size())},
         {"value_slots", static_cast<long long>(maxSlots)},
         {"expected_rows", tmpl::encode(expectedRows)},
         {"expected_fallback", pyString(expectedFallbackText(analysis.mode))},
         {"glr", isGlrMode(mode)},
         {"dd", isDdMode(mode)}});
}

/**
 * @brief Token 类型段（常量 + 名字表）。
 * @param terminals 终结符列表。
 * @return Python 代码段。
 */
std::string pyTokenTypes(const std::vector<std::string> &terminals,
                         const std::map<std::string, std::string> &aliases = {})
{
    std::vector<PyTokenId> tokens;
    std::vector<std::string> names;
    for (std::size_t index = 0u; index < terminals.size(); ++index)
    {
        PyTokenId token;
        token.name = terminals[index];
        token.value = static_cast<int>(index);
        tokens.push_back(token);
        names.push_back(pyString(terminals[index]));
    }
    std::vector<PyTokenAlias> aliasRows;
    for (const auto &entry : aliases)
    {
        PyTokenAlias item;
        item.name = pyString(entry.first);
        item.alias = pyString(entry.second);
        aliasRows.push_back(item);
    }
    return tmpl::renderValues(
        Utils::loadTemplate("python_token_types.py"),
        {{"tokens", tmpl::encode(tokens)},
         {"invalid_id", static_cast<long long>(terminals.size())},
         {"names", tmpl::encode(names)},
         {"aliases", tmpl::encode(aliasRows)}});
}

/**
 * @brief Python 产物不支持的 C 专属写法检查。
 *
 * `%code … %endcode` 是**支持**的（正文归一化后插在模块 import
 * 之后），所以这里不拦；C 专属的语义值模型（`%value_type` / `%union` /
 * `%token <成员>`）与 `%namespace` 仍然直接报错。
 * @param request 组装入参。
 * @param error 出错时写入错误说明。
 * @return 全部合法返回 true。
 */
bool checkPythonSupported(const ProjectRequest &request, Error &error)
{
    if (request.valueType != nullptr)
    {
        return fail(error,
                    "Python 产物不支持 %value_type：Python 的语义值是任意对象"
                    "（词法器直接给出 str / int / float），不需要声明类型");
    }
    if (request.unionBody != nullptr)
    {
        return fail(error,
                    "Python 产物不支持 %union / %token <成员>：Python 的语义值"
                    "是任意对象，没有联合体成员这一层");
    }
    if (!request.cppNamespace.empty())
    {
        return fail(error,
                    "Python 产物不支持 %namespace / --namespace：Python 没有"
                    "命名空间，改解析器类名请用 %class / --class");
    }
    return true;
}

/* ---------------- 词法器各段 ---------------- */

/// 词法器各段的替换表。
struct LexerBlocks
{
    std::string stringBlock;     ///< 内建双引号字符串
    std::string matchBlock;      ///< 固定字面量 + 正则最长匹配
    std::string numberBlock;     ///< 数字
    std::string identifierBlock; ///< 标识符
};

/**
 * @brief 组装词法器各段（哪几段生效由文法声明的词法规则决定）。
 * @param request 组装入参。
 * @param blocks 输出各段文本。
 */
void buildLexerBlocks(const ProjectRequest &request, LexerBlocks &blocks)
{
    const std::map<std::string, std::vector<std::string>> emptyTokenDefs;
    const auto &tokenDefs =
        (request.tokenDefs == nullptr) ? emptyTokenDefs : *request.tokenDefs;
    const LexSpec emptyLex;
    const LexSpec &lexSpec =
        (request.lexSpec == nullptr) ? emptyLex : *request.lexSpec;
    const auto overridden = [&tokenDefs, &lexSpec](const std::string &name)
    {
        return tokenDefs.count(name) || lexSpec.regex.count(name);
    };

    // 内建字符串：用户自己声明了 STRING 的词法规则时不生成
    if (!overridden("STRING"))
    {
        blocks.stringBlock = Utils::loadTemplate("python_lexer_string.py");
    }
    std::string regexLoop;
    if (!lexSpec.regex.empty())
    {
        regexLoop = Utils::loadTemplate("python_lexer_regex_loop.py");
    }
    blocks.matchBlock =
        tmpl::renderValues(Utils::loadTemplate("python_lexer_match.py"),
                           {{"lex_regex_loop", regexLoop}});
    // 数字：用户自己声明了 NUMBER 的词法规则时不生成
    if (!overridden("NUMBER"))
    {
        blocks.numberBlock = Utils::loadTemplate("python_lexer_number.py");
    }
    // 标识符：%identifier 的正则优先；用户自己声明了 VARIABLE 时两条都跳过
    if (lexSpec.identifier.has_value())
    {
        blocks.identifierBlock =
            Utils::loadTemplate("python_lexer_identifier_dfa.py");
    }
    else if (!overridden("VARIABLE"))
    {
        blocks.identifierBlock =
            Utils::loadTemplate("python_lexer_identifier.py");
    }
}

/**
 * @brief 同步 Token / error 伪 Token / 非终结符名表等杂项表。
 * @param request 组装入参。
 * @param analysis 分析结果。
 * @return Python 代码段。
 */
std::string pyMiscTables(const ProjectRequest &request,
                         const AnalysisResult &analysis)
{
    std::vector<std::string> syncTokens;
    if (request.syncTokens != nullptr)
    {
        for (const std::string &token : *request.syncTokens)
        {
            if (std::find(analysis.terminals.begin(), analysis.terminals.end(),
                          token) == analysis.terminals.end())
            {
                continue;
            }
            syncTokens.push_back(token);
        }
    }
    std::size_t errorTokenId = 0u;
    if (request.usesError)
    {
        const auto found = std::find(analysis.terminals.begin(),
                                     analysis.terminals.end(), "error");
        if (found != analysis.terminals.end())
        {
            errorTokenId = static_cast<std::size_t>(found -
                                                    analysis.terminals.begin());
        }
    }
    std::vector<std::string> nontermNames;
    for (const std::string &nonterm : analysis.nonterms)
    {
        nontermNames.push_back(pyString(nonterm));
    }
    // 这一段的文本在 python_misc_tables.py 里，这里只摆数据
    return tmpl::renderValues(
        Utils::loadTemplate("python_misc_tables.py"),
        {{"sync_tokens", tmpl::encode(syncTokens)},
         {"has_sync", !syncTokens.empty()},
         {"has_error", request.usesError},
         {"error_token_id", static_cast<long long>(errorTokenId)},
         {"nonterm_names", tmpl::encode(nontermNames)}});
}

/**
 * @brief 演示 main 里的 `%commit` 打印片段：行号 + 类型 + 原文。
 * @param request 组装入参。
 * @param engineExpr 解析器实例的表达式（`engine` / `lexer`）。
 * @return Python 代码段；文法里没写 %commit 时为空串（演示输出与从前一样）。
 */
std::string pyDemoCommitPrint(const ProjectRequest &request,
                              const std::string &engineExpr)
{
    const LexSpec emptyLex;
    const LexSpec &lexSpec =
        (request.lexSpec == nullptr) ? emptyLex : *request.lexSpec;
    if (lexSpec.commits.empty())
    {
        return std::string();
    }
    return tmpl::renderValues(
        Utils::loadTemplate("python_demo_commit_print.py"),
        {{"engine", engineExpr}});
}

/**
 * @brief 绘图能力的产物片段（语法树绘图 / 语法图）。
 *
 * 两套能力的白名单不同（见 `isTreeDrawMode` / `isGlrMode`）：语法树绘图只给
 * `lalr` / `lr1` / `ielr`，语法图只给 `glr`；不在白名单里时四个片段全是空串，
 * 产物与没有这个功能时逐字节相同。
 */
struct PyDrawBlocks
{
    std::string common; ///< 模块级绘图内核（两套能力共用，自选一种就发）
    std::string tree;   ///< `class Tree` 的绘图方法（lalr / lr1 / ielr）
    std::string graph;  ///< 解析器类的语法图方法（glr）
    std::string demo;   ///< 演示 main 里那段「按选项写出文件」的调用
};

/**
 * @brief 按模式渲染上面四个片段（不在白名单里就全给空串）。
 * @param mode 归一化后的生成模式。
 * @param modeLabel 模式标签（如 LALR(1)，进默认图标题）。
 * @return 四个片段。
 */
PyDrawBlocks pyDrawBlocks(const std::string &mode,
                          const std::string &modeLabel)
{
    PyDrawBlocks blocks;
    const bool treeDraw = isTreeDrawMode(mode);
    const bool graphDraw = isGlrMode(mode);
    if (!treeDraw && !graphDraw)
    {
        return blocks;
    }
    blocks.common = tmpl::renderValues(
        Utils::loadTemplate("python_draw_common.py"),
        {{"mode_label", modeLabel}});
    if (treeDraw)
    {
        blocks.tree = tmpl::renderValues(
            Utils::loadTemplate("python_draw_tree.py"),
            {{"mode_label", modeLabel}});
        // 这两段小片段也要过一遍模板引擎（模板头上有 `{# … #}` 说明）
        blocks.demo = tmpl::renderValues(
            Utils::loadTemplate("python_draw_demo_tree.py"));
    }
    else
    {
        blocks.graph = tmpl::renderValues(
            Utils::loadTemplate("python_draw_graph.py"),
            {{"mode_label", modeLabel}});
        blocks.demo = tmpl::renderValues(
            Utils::loadTemplate("python_draw_demo_graph.py"));
    }
    return blocks;
}

/**
 * @brief 演示 main 的正文（没有写 %main 时用）。
 * @param request 组装入参。
 * @return Python 代码段。
 */
std::string pyDemoBody(const ProjectRequest &request,
                       const std::string &mode)
{
    if (isGlrMode(mode))
    {
        std::string print;
        if (request.tree)
        {
            print =
                "    for index in range(engine.glr_result_count()):\n"
                "        print(\"结果[%d] 语法树:\" % index)\n"
                "        tree = engine.glr_tree(index)\n"
                "        if tree is not None:\n"
                "            tree.print()\n";
        }
        else
        {
            print = "    for index in range(engine.glr_result_count()):\n"
                    "        print(\"结果[%d]: %r\" % (index,\n"
                    "                                 engine.glr_result(index)))\n";
        }
        return tmpl::renderValues(
            Utils::loadTemplate("python_glr_demo_main.py"),
            {{"glr_run", request.tree
                              ? "engine.run_glr_tree(input_text)"
                              : "engine.run_glr(input_text)"},
             {"glr_print", print},
             // 语法图：只有 glr 档有（--svg / --dot 才会多打行）
             {"draw", isGlrMode(mode)},
             {"demo_drawings",
              pyDrawBlocks(mode, modeLabelText(mode)).demo},
             {"demo_commits", pyDemoCommitPrint(request, "engine")}});
    }
    const std::string treePrint = tmpl::renderValues(
        Utils::loadTemplate("python_demo_tree_print.py"));
    if (isDdMode(mode))
    {
        // DD：与 LR 那套一样「读一行 → 解析 → 打结果 / 树」，只是多一句
        // 「只保留第 1 条匹配路径」的说明（回溯式不像 GLR 那样列全部解释）
        return tmpl::renderValues(
            Utils::loadTemplate("python_dd_demo_main.py"),
            {{"demo_commits", pyDemoCommitPrint(request, "engine")},
             {"demo_tree", request.tree ? treePrint : std::string()}});
    }
    const PyDrawBlocks draw = pyDrawBlocks(mode, modeLabelText(mode));
    return tmpl::renderValues(
        Utils::loadTemplate("python_demo_main.py"),
        {{"demo_commits", pyDemoCommitPrint(request, "engine")},
         // 语法树绘图：只有 lalr / lr1 / ielr 这三档有（--svg / --dot）
         {"draw", !draw.demo.empty()},
         {"demo_drawings", draw.demo},
         {"demo_tree", request.tree ? treePrint : std::string()}});
}

} // namespace

std::string CodeGenPython::className(const ProjectRequest &request)
{
    if (request.pythonClass.empty())
    {
        return "Parser";
    }
    return request.pythonClass;
}

std::string CodeGenPython::grammarLineMarker(std::size_t lineNumber)
{
    if (lineNumber == 0u)
    {
        return std::string();
    }
    // 路径留在模板里当占位符：收尾时由 Project 换成真实的 .gram 路径（与 C 的 #line 同一套）
    return tmpl::renderValues(
        Utils::loadTemplate("python_grammar_line_marker.txt"),
        {{"line", static_cast<long long>(lineNumber)}});
}

std::string CodeGenPython::normalizeUserCode(const std::string &code)
{
    // 1. 切行（顺手吃掉行尾的 \r，文法文件可能是 CRLF）
    std::vector<std::string> lines;
    std::string current;
    for (const char raw : code)
    {
        if (raw != '\n')
        {
            current.push_back(raw);
            continue;
        }
        if (!current.empty() && (current.back() == '\r'))
        {
            current.pop_back();
        }
        lines.push_back(current);
        current.clear();
    }
    if (!current.empty())
    {
        if (current.back() == '\r')
        {
            current.pop_back();
        }
        lines.push_back(current);
    }
    const auto isBlank = [](const std::string &line)
    {
        for (const char ch : line)
        {
            if ((ch != ' ') && (ch != '\t'))
            {
                return false;
            }
        }
        return true;
    };
    // 2. 去掉首尾的空行
    std::size_t begin = 0u;
    std::size_t end = lines.size();
    while ((begin < end) && isBlank(lines[begin]))
    {
        ++begin;
    }
    while ((end > begin) && isBlank(lines[end - 1u]))
    {
        --end;
    }
    if (begin == end)
    {
        return std::string();
    }
    // 3. 所有非空行的最小前导空白宽度（tab 记 4 列）就是公共缩进
    std::size_t minIndent = 0u;
    bool sawContent = false;
    for (std::size_t index = begin; index < end; ++index)
    {
        if (isBlank(lines[index]))
        {
            continue;
        }
        std::size_t width = 0u;
        for (const char ch : lines[index])
        {
            if (ch == ' ')
            {
                width += 1u;
            }
            else if (ch == '\t')
            {
                width += 4u;
            }
            else
            {
                break;
            }
        }
        if (!sawContent || (width < minIndent))
        {
            minIndent = width;
            sawContent = true;
        }
    }
    // 4. 去掉公共缩进；剩下的前导 tab 统一换成 4 个空格（行首空白之外的
    //    字符一个都不动，多行字符串里的内容因此保持不变）
    std::string out;
    for (std::size_t index = begin; index < end; ++index)
    {
        const std::string &line = lines[index];
        if (isBlank(line))
        {
            out += '\n';
            continue;
        }
        std::size_t cursor = 0u;
        std::size_t width = 0u;
        while ((cursor < line.size()) && (width < minIndent))
        {
            if (line[cursor] == ' ')
            {
                width += 1u;
            }
            else if (line[cursor] == '\t')
            {
                width += 4u;
            }
            else
            {
                break;
            }
            ++cursor;
        }
        while ((cursor < line.size()) &&
               ((line[cursor] == ' ') || (line[cursor] == '\t')))
        {
            if (line[cursor] == '\t')
            {
                out += "    ";
            }
            else
            {
                out += ' ';
            }
            ++cursor;
        }
        out.append(line, cursor, std::string::npos);
        out += '\n';
    }
    return out;
}

std::string CodeGenPython::parserFileName() { return "parser.py"; }

std::string CodeGenPython::mainFileName() { return "main.py"; }

/**
 * @brief 词法表段（固定字面量规则表 + `%token regex` / `%identifier` 的正则表）。
 *
 * 解析产物与词法模式共用这一段；写完就是「# ---------- 词法表 ----------」到
 * `pyLexRegexes` 结束，后面的分析表由调用方自己接着拼。这一段没有失败路径：
 * 正则认不下只是在 `pyLexRegexes` 里打警告。
 * @param request 组装入参。
 * @param out 追加写入的表段。
 */
void pyLexTables(const ProjectRequest &request, std::string &out)
{
    const std::map<std::string, std::vector<std::string>> emptyTokenDefs;
    const auto &tokenDefs =
        (request.tokenDefs == nullptr) ? emptyTokenDefs : *request.tokenDefs;
    const LexSpec emptyLex;
    const LexSpec &lexSpec =
        (request.lexSpec == nullptr) ? emptyLex : *request.lexSpec;
    out += pyLexRules(tokenDefs, lexSpec.tokenLexOrder);
    out += pySkipChars(request.spaceChars);
    out += pyKeywords(tokenDefs);
    pyLexRegexes(lexSpec.regex, lexSpec.identifier, lexSpec.commits,
                 lexSpec.tokenLexOrder, out);
}

/**
 * @brief 词法模式的常量段：只需要 Token 个数（词法 DFA 用的表在词法表段里）。
 * @param terminals Token 列表（内置 Token + 文法声明的 + END）。
 * @return 常量段文本。
 */
std::string pyLexConstants(const std::vector<std::string> &terminals)
{
    return tmpl::renderValues(
        Utils::loadTemplate("python_lex_constants.py"),
        {{"terminal_count", static_cast<long long>(terminals.size())}});
}

bool CodeGenPython::parserPy(const ProjectRequest &request,
                            const std::string &mode,
                            const AnalysisResult &analysis, std::string &out,
                            Error &error)
{
    if (!checkPythonSupported(request, error))
    {
        return false;
    }
    const std::vector<std::string> &terminals = analysis.terminals;
    // 表段（词法表在 pyLexTables 里，分析表接着拼）
    std::string tables;
    pyLexTables(request, tables);
    // 分析表 / 语法树表两节的标题与前置空行都在各自的模板里
    if (isLrMode(mode))
    {
        tables += pyLrTables(analysis);
        if (isGlrMode(mode))
        {
            tables += pyGlrTables(analysis);
            if ((mode == "earley") || (mode == "cyk"))
            {
                tables += pyChartTables(analysis);
            }
        }
    }
    else
    {
        tables += pyLlTables(analysis);
    }
    if (!isLrMode(mode))
    {
        tables += pyTreeTables(analysis);
    }
    if (isDdMode(mode))
    {
        tables += pyDdTables(analysis);
    }
    tables += "\n";
    tables += pyMiscTables(request, analysis);

    // 动作分支：LR 家族按「分析栈右部切片」取 $n，LL(1) 与 DD 按帧槽位取
    std::string actionCases;
    if (isLrMode(mode))
    {
        actionCases =
            pyActionCases(analysis, analysis.mode, error);
    }
    else if (isDdMode(mode))
    {
        // DD 的 values 与 LL 的语义帧同一个约定（第 0 个槽位留空），但错误恢复
        // 那套 yyerrok / yyclearin 在递归下降里没有落点，所以翻译模式要传
        // "dd"（翻译器会给出明确报错，而不是静默生成一个不起作用的调用）
        actionCases = pyActionCases(analysis, "dd", error);
    }
    else
    {
        actionCases = pyActionCases(analysis, "ll1", error);
    }
    if (!error.empty())
    {
        return false;
    }

    // 引擎
    LexerBlocks blocks;
    buildLexerBlocks(request, blocks);
    const std::string lexer = tmpl::renderValues(
        Utils::loadTemplate("python_lexer.py"),
        {{"lex_skip_loop",
          "        while _lex_is_skip_char(self._peek_char()):\n"
          "            # 空白也可能是 Token（%define NL \"\\n\" / "
          "%token SP \"\\b\"）：\n"
          "            # 命中以空白开头的字面量就停手，交给词法匹配\n"
          "            if _lex_blank_rule_hit(self._input, "
          "self._text_pos):\n"
          "                break\n"
          "            self._read_char()"},
         {"lex_string_block", blocks.stringBlock},
         {"lex_match_block", blocks.matchBlock},
         {"lex_number_block", blocks.numberBlock},
         {"lex_identifier_block", blocks.identifierBlock}});

    std::string lrText = tmpl::renderValues(
        Utils::loadTemplate("python_lr_action.py"),
        {{"lr_action_cases", actionCases},
         {"yy_helpers", !isDdMode(mode)},
         {"slot_note", isDdMode(mode)
                           ? std::string("``$1`` 就是 ``values[1]``，第 0 个"
                                         "槽位留空")
                           : std::string("``$1`` 就是 ``values[0]``")}});
    std::string llText;
    std::string ddText;
    // 绘图（语法树 / 语法图）：白名单外的模式这四个片段都是空串
    const PyDrawBlocks draw = pyDrawBlocks(mode, modeLabelText(mode));
    if (isDdMode(mode))
    {
        // DD：动作分支 + 带回溯的递归下降驱动（不建表，直接照文法走）
        ddText = tmpl::renderValues(Utils::loadTemplate("python_dd.py"));
    }
    else if (isLrMode(mode))
    {
        if (isGlrMode(mode))
        {
            // GLR：动作分支 + 图结构栈驱动（替代 LR 的分析栈驱动）
            lrText += tmpl::renderValues(
                Utils::loadTemplate("python_glr.py"),
                {{"lr_name", modeNameText(mode)},
                 {"chart", (mode == "earley") || (mode == "cyk")}});
            if ((mode == "earley") || (mode == "cyk"))
            {
                lrText += tmpl::renderValues(
                    Utils::loadTemplate("python_chart.py"),
                    {{"earley", mode == "earley"}});
            }
        }
        else
        {
            lrText += tmpl::renderValues(Utils::loadTemplate("python_lr.py"),
                                         {{"lr_name", modeNameText(mode)}});
        }
    }
    else
    {
        llText = tmpl::renderValues(
            Utils::loadTemplate("python_ll.py"),
            {{"ll_expand_cases",
              pyExpandCases(analysis, terminals,
                            nontermIds(analysis.nonterms))}});
    }

    const std::string engine = tmpl::renderValues(
        Utils::loadTemplate("python_engine.py"),
        {{"class", className(request)},
         {"mode_name", modeLabelText(mode)},
         {"max_stack_depth",
          std::to_string(request.maxStackDepth.value_or(64))},
         {"max_errors", std::to_string(request.maxErrors.value_or(8))},
         {"error_recovery_tokens",
          std::to_string(request.errorRecoveryTokens.value_or(3))},
         // 槽位个数由 pyConstants 算好（模块常量 _VALUE_SLOTS）
         {"value_slots", "_VALUE_SLOTS"},
         {"glr", isGlrMode(mode)},
         {"run_loop", pyRunLoop(mode)},
         {"lexer", lexer},
         {"lr", lrText},
         {"ll", llText},
         {"dd", ddText},
         {"draw_graph", draw.graph}});

    // %code … %endcode：逐块归一化后插在模块 import 之后、常量之前
    std::string userCode;
    if ((request.lines != nullptr) && (request.lines->userBlocks != nullptr) &&
        !request.lines->userBlocks->empty())
    {
        for (std::size_t index = 0u;
             index < request.lines->userBlocks->size(); ++index)
        {
            const std::string normalized =
                normalizeUserCode((*request.lines->userBlocks)[index]);
            if (normalized.empty())
            {
                continue;
            }
            if (!userCode.empty())
            {
                userCode += "\n";
            }
            // 这块用户代码来自 .gram 的哪一行（%code … %endcode）
            if ((request.lines->userBlockLines != nullptr) &&
                (index < request.lines->userBlockLines->size()))
            {
                userCode += CodeGenPython::grammarLineMarker(
                    (*request.lines->userBlockLines)[index]);
            }
            userCode += normalized;
        }
    }
    else
    {
        userCode = normalizeUserCode(request.userCode);
    }
    const auto emptyAliases = std::map<std::string, std::string>{};
    const auto &aliases = request.lexSpec ? request.lexSpec->tokenAliases : emptyAliases;
    std::string text = tmpl::renderValues(
        Utils::loadTemplate("python_parser.py"),
        {{"mode_name", modeLabelText(mode)},
         {"class", className(request)},
         {"user_code",
          userCode.empty()
              ? std::string()
              : tmpl::renderValues(
                    Utils::loadTemplate("python_user_code_block.txt"),
                    {{"body", userCode}})},
         {"constants", pyConstants(analysis, mode)},
         {"token_types", pyTokenTypes(terminals, aliases)},
         {"tables", tables},
         {"engine", engine},
         // 解析产物：docstring 写解析用法，Tree 类与它的排版辅助函数都在
         {"lex", false},
         {"tree_draw", draw.tree},
         {"draw_common", draw.common},
         {"tree_class", true}});
    if (!text.empty() && (text.back() != '\n'))
    {
        text += "\n";
    }
    out = std::move(text);
    return true;
}

std::string CodeGenPython::mainPy(const ProjectRequest &request,
                                  const std::string &mode,
                                  const std::string &mainBody)
{
    std::string body;
    if (!mainBody.empty())
    {
        // %main：正文整段替换内建 demo（用户正文就是 main.py 的顶层代码）
        body = tmpl::renderValues(
            Utils::loadTemplate("python_main_block.txt"),
            {{"body", normalizeUserCode(mainBody)}});
    }
    else
    {
        body = pyDemoBody(request, mode);
    }
    return tmpl::renderValues(Utils::loadTemplate("python_main.py"),
                              {{"mode_name", modeLabelText(mode)},
                               {"class", className(request)},
                               {"demo_body", body}});
}

bool CodeGenPython::lexerPy(const ProjectRequest &request,
                            const std::vector<std::string> &terminals,
                            std::string &out, Error &error)
{
    if (!checkPythonSupported(request, error))
    {
        return false;
    }
    // 表段：词法模式的产物里只有词法表（没有分析表 / 语法树表）
    std::string tables;
    pyLexTables(request, tables);
    // 词法器：与解析产物同一份模板（顺序与 C / C++17 目标一致）
    LexerBlocks blocks;
    buildLexerBlocks(request, blocks);
    const std::string lexer = tmpl::renderValues(
        Utils::loadTemplate("python_lexer.py"),
        {{"lex_skip_loop",
          "        while _lex_is_skip_char(self._peek_char()):\n"
          "            # 空白也可能是 Token（%define NL \"\\n\" / "
          "%token SP \"\\b\"）：\n"
          "            # 命中以空白开头的字面量就停手，交给词法匹配\n"
          "            if _lex_blank_rule_hit(self._input, "
          "self._text_pos):\n"
          "                break\n"
          "            self._read_char()"},
         {"lex_string_block", blocks.stringBlock},
         {"lex_match_block", blocks.matchBlock},
         {"lex_number_block", blocks.numberBlock},
         {"lex_identifier_block", blocks.identifierBlock}});
    // 引擎：词法模式只有词法器 + tokenize（没有驱动与语义动作）
    const std::string engine = tmpl::renderValues(
        Utils::loadTemplate("python_lex_engine.py"),
        {{"class", className(request)},
         {"mode_name", "LEX(词法模式)"},
         {"max_errors", std::to_string(request.maxErrors.value_or(8))},
         {"lexer", lexer}});

    std::string userCode;
    if ((request.lines != nullptr) && (request.lines->userBlocks != nullptr) &&
        !request.lines->userBlocks->empty())
    {
        for (std::size_t index = 0u;
             index < request.lines->userBlocks->size(); ++index)
        {
            const std::string normalized =
                normalizeUserCode((*request.lines->userBlocks)[index]);
            if (normalized.empty())
            {
                continue;
            }
            if (!userCode.empty())
            {
                userCode += "\n";
            }
            // 这块用户代码来自 .gram 的哪一行（%code … %endcode）
            if ((request.lines->userBlockLines != nullptr) &&
                (index < request.lines->userBlockLines->size()))
            {
                userCode += CodeGenPython::grammarLineMarker(
                    (*request.lines->userBlockLines)[index]);
            }
            userCode += normalized;
        }
    }
    else
    {
        userCode = normalizeUserCode(request.userCode);
    }
    const auto emptyAliases = std::map<std::string, std::string>{};
    const auto &aliases = request.lexSpec ? request.lexSpec->tokenAliases : emptyAliases;
    std::string text = tmpl::renderValues(
        Utils::loadTemplate("python_parser.py"),
        {{"mode_name", "LEX(词法模式)"},
         {"class", className(request)},
         {"user_code",
          userCode.empty()
              ? std::string()
              : tmpl::renderValues(
                    Utils::loadTemplate("python_user_code_block.txt"),
                    {{"body", userCode}})},
         {"constants", pyLexConstants(terminals)},
         {"token_types", pyTokenTypes(terminals, aliases)},
         {"tables", tables},
         {"engine", engine},
         // 词法产物：docstring 换成词法用法（tokenize），Tree 类整段不出现
         // —— 原先靠「两处整句 replaceAll + 按 `class Tree(object):` /
         // `def _is_digit(ch):` 两个锚点 erase」做字符串手术，现在模板里的
         // `{% if lex %}` / `{% if tree_class %}` 直接决定，C++ 侧不再依赖
         // 模板正文（改模板不会再悄悄改坏产物）。
         {"lex", true},
         // 词法产物没有语法树、也没有语法图：绘图片段一律空串
         {"tree_draw", std::string()},
         {"draw_common", std::string()},
         {"tree_class", false}});
    if (!text.empty() && (text.back() != '\n'))
    {
        text += "\n";
    }
    out = std::move(text);
    return true;
}

std::string CodeGenPython::lexerMainPy(const ProjectRequest &request,
                                       const std::string &mainBody)
{
    std::string body;
    if (!mainBody.empty())
    {
        // %main：正文整段替换内建 demo（用户正文就是 main.py 的顶层代码）
        body = tmpl::renderValues(
            Utils::loadTemplate("python_main_block.txt"),
            {{"body", normalizeUserCode(mainBody)}});
    }
    else
    {
        body = tmpl::renderValues(
            Utils::loadTemplate("python_lex_demo_main.py"),
            {{"demo_commits", pyDemoCommitPrint(request, "lexer")}});
    }
    return tmpl::renderValues(Utils::loadTemplate("python_lex_main.py"),
                              {{"mode_name", "LEX(词法模式)"},
                               {"class", className(request)},
                               {"demo_body", body}});
}

} // namespace ParseEngine
