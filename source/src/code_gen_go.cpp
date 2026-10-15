/**
 * @file code_gen_go.cpp
 * @brief Go 语言后端实现：把生成期算好的表整理成**数据**，其余全部交给
 *        source/templates/go_*.tmpl 里的模板引擎渲染。
 *
 * 与 Rust 后端逐项对齐：
 *   - 生成期只换语法：`%package` -> go.mod 的 module path、`%import` -> import
 *     块、`%class_body` -> 一组 `func (p *Parser) …` 方法、类型名暴露用的
 *     Go 写法（Parser / Token / Tree / ParserError）；
 *   - 语义值**按值拷贝**（`%union` 生成 `type ParserValue struct { … }`）；
 *   - 词法器把「Token 词素 -> 语义值」集中在生成出来的 `valueFromText` /
 *     `valueFromNumber` 两个函数里；
 *   - **词法正则用标准库的 `regexp`**（Go 自带，没有第三方依赖）：`%token regex`
 *     / `%identifier` / `%commit` 的模式原样交给它，产物里没有 DFA 表；
 *   - GLR / Earley / CYK 共用 GSS 多结果重建；Earley / CYK 先做图表判定；
 *   - **没有 `%ast_*`**（已从本项目整体移除）。
 *
 * **本文件里没有 `replaceAll`**：产物长什么样完全由
 * `source/templates/go_*.tmpl` 说了算，这里只负责「算表 / 算标志位 / 转义
 * 字符串字面量」这些**数据准备**。产物片段一律走模板渲染，C++ 里不再有
 * `out += "…"` 拼出来的文案（唯一一处 `out +=` 是 `goString()` 这个**字符串
 * 字面量转义器**，它产出的是「值」而不是产物正文）。要改文案或排版，改模板。
 */

#include "ParseEngine/code_gen_go.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdio>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "ParseEngine/action_translator.hpp"
#include "ParseEngine/chart_grammar.hpp"
#include "ParseEngine/code_gen.hpp"
#include "ParseEngine/grammar_types.hpp"
#include "ParseEngine/utils.hpp"

// Go 产物模板（source/templates/go_*.tmpl）走 library/template 的模板引擎
#include "template/engine.hpp"
#include "template/naming.hpp"

namespace ParseEngine
{

// %commit / %trivia 容器（ParserCommit / Commits()）与位置信息 API
// （ParserLocation / CurrentLocation()）在这里落地：类型定义在 go_engine.go
// （parser.go），词法匹配在 go_lexer_match.go，演示输出在 go_demo_main.go；
// 与另外几档共用同一套词法声明序。

namespace
{

/**
 * @brief 一维整数表里的一个数（`go_int_array.txt` 的元素）。
 */
struct GoIntItem
{
    bool br = false;     ///< 这个元素前面要不要换行（生成器按「每行几个」算好）
    long long value = 0; ///< 数值
};

/**
 * @brief 二维整数表的一行（`go_int_rows.txt` 的元素）。
 */
struct GoIntRow
{
    std::vector<GoIntItem> items; ///< 这一行的数值
};

/// 固定字面量规则表的一行（`go_lex_tables.go` 的数组元素）。
struct GoLexRuleRow
{
    std::string text; ///< 字面量（**已带引号**的 Go 字符串字面量）
    int type = 0;     ///< Token 类型号
    int length = 0;   ///< 字面量在 Go 里占几个 rune
    int blank = 0;    ///< 能不能以空白 / \b 通配开头（0 / 1）
    int order = 0;    ///< 词法声明序
};

/// 关键字查表的一行（`go_lexer_keyword.go` 的 switch 分支）。
struct GoKeywordRow
{
    std::string literal; ///< 关键字字面量（已带引号）
    std::string token;   ///< Token 名（不带 `TOKEN_` 前缀）
};

/// 一个 Token 类型常量。
struct GoTokenConst
{
    std::string name; ///< Token 名（不带 `TOKEN_` 前缀）
    int value = 0;    ///< 常量值（就是它在 Token 表里的下标）
};

/// 一条 Token 别名。
struct GoAliasRow
{
    std::string name;  ///< Token 名（已带引号）
    std::string alias; ///< 显示别名（已带引号）
};

/// `%union` 里一条「词素能吃字符串」的成员（`go_value_helpers.go` 的 if 分支）。
struct GoValueCaseRow
{
    int kind = 0;       ///< Token 类型号
    std::string member; ///< 成员名
};

/// `%token regex` 的一条正则（`go_lex_regexes.go` 的 regexes 元素）。
struct GoRegexRow
{
    std::string pattern; ///< 模式（Go 字符串字面量，已转义）
    long long type = 0;  ///< Token 类型号
    long long order = 0; ///< 词法声明序
};

/// `%commit` 的一条声明（`go_lex_regexes.go` 的 commits 元素）。
struct GoCommitRow
{
    std::string pattern; ///< 模式（Go 字符串字面量，已转义）
    std::string type;    ///< 类型名（已带引号）
    bool skip = false;   ///< 命中即丢
    int order = 0;       ///< 词法声明序
};

/// `%commit` 的类型 -> 显示别名（`go_lex_regexes.go` 的 commitAlias 分支）。
struct GoCommitAliasRow
{
    std::string type;  ///< 类型名（已带引号）
    std::string alias; ///< 别名（已带引号）
};

/// 语义动作分支的一行（`go_action_cases.go` 的元素）。
struct GoActionRow
{
    bool first = false;            ///< 是不是第一条（决定写 `if` 还是 `else if`）
    int id = 0;                    ///< 产生式编号
    bool hasLine = false;          ///< 动作原文来自 .gram 的某一行
    int line = 0;                  ///< 那一行
    std::vector<std::string> body; ///< 动作正文（**按行**；缩进交给模板）
    std::string fileName;          ///< 产物文件名（后向注释里用）
};

/// LL 展开分支里的一步（`go_expand_cases.go` 的 steps 元素）。
struct GoExpandStepRow
{
    bool isComment = false; ///< 空尾规则：只写一行注释
    std::string kind;       ///< `sItemTerminal` / `sItemNonterm` / …
    std::string symbol;     ///< 符号号
    int slot = 0;           ///< 语义槽位
};

/// LL 展开分支的一条产生式（`go_expand_cases.go` 的 prods 元素）。
struct GoExpandProdRow
{
    int id = 0;                           ///< 产生式编号
    std::vector<GoExpandStepRow> steps;   ///< 压栈步骤
};

} // namespace
} // namespace ParseEngine

// 字段绑定只能写在全局作用域（`TEMPLATE_MODEL` 展开的是显式特化）
TEMPLATE_MODEL(ParseEngine::GoIntItem,
               tmpl::field("br", &ParseEngine::GoIntItem::br),
               tmpl::field("value", &ParseEngine::GoIntItem::value));

TEMPLATE_MODEL(ParseEngine::GoIntRow,
               tmpl::field("items", &ParseEngine::GoIntRow::items));

TEMPLATE_MODEL(ParseEngine::GoLexRuleRow,
               tmpl::field("text", &ParseEngine::GoLexRuleRow::text),
               tmpl::field("type", &ParseEngine::GoLexRuleRow::type),
               tmpl::field("length", &ParseEngine::GoLexRuleRow::length),
               tmpl::field("blank", &ParseEngine::GoLexRuleRow::blank),
               tmpl::field("order", &ParseEngine::GoLexRuleRow::order));

TEMPLATE_MODEL(ParseEngine::GoKeywordRow,
               tmpl::field("literal", &ParseEngine::GoKeywordRow::literal),
               tmpl::field("token", &ParseEngine::GoKeywordRow::token));

TEMPLATE_MODEL(ParseEngine::GoTokenConst,
               tmpl::field("name", &ParseEngine::GoTokenConst::name),
               tmpl::field("value", &ParseEngine::GoTokenConst::value));

TEMPLATE_MODEL(ParseEngine::GoAliasRow,
               tmpl::field("name", &ParseEngine::GoAliasRow::name),
               tmpl::field("alias", &ParseEngine::GoAliasRow::alias));

TEMPLATE_MODEL(ParseEngine::GoValueCaseRow,
               tmpl::field("kind", &ParseEngine::GoValueCaseRow::kind),
               tmpl::field("member", &ParseEngine::GoValueCaseRow::member));

TEMPLATE_MODEL(ParseEngine::GoRegexRow,
               tmpl::field("pattern", &ParseEngine::GoRegexRow::pattern),
               tmpl::field("type", &ParseEngine::GoRegexRow::type),
               tmpl::field("order", &ParseEngine::GoRegexRow::order));

TEMPLATE_MODEL(ParseEngine::GoCommitRow,
               tmpl::field("pattern", &ParseEngine::GoCommitRow::pattern),
               tmpl::field("type", &ParseEngine::GoCommitRow::type),
               tmpl::field("skip", &ParseEngine::GoCommitRow::skip),
               tmpl::field("order", &ParseEngine::GoCommitRow::order));

TEMPLATE_MODEL(ParseEngine::GoCommitAliasRow,
               tmpl::field("type", &ParseEngine::GoCommitAliasRow::type),
               tmpl::field("alias", &ParseEngine::GoCommitAliasRow::alias));

TEMPLATE_MODEL(ParseEngine::GoActionRow,
               tmpl::field("first", &ParseEngine::GoActionRow::first),
               tmpl::field("id", &ParseEngine::GoActionRow::id),
               tmpl::field("has_line", &ParseEngine::GoActionRow::hasLine),
               tmpl::field("line", &ParseEngine::GoActionRow::line),
               tmpl::field("body", &ParseEngine::GoActionRow::body),
               tmpl::field("file_name", &ParseEngine::GoActionRow::fileName));

TEMPLATE_MODEL(ParseEngine::GoExpandStepRow,
               tmpl::field("is_comment",
                           &ParseEngine::GoExpandStepRow::isComment),
               tmpl::field("kind", &ParseEngine::GoExpandStepRow::kind),
               tmpl::field("symbol", &ParseEngine::GoExpandStepRow::symbol),
               tmpl::field("slot", &ParseEngine::GoExpandStepRow::slot));

TEMPLATE_MODEL(ParseEngine::GoExpandProdRow,
               tmpl::field("id", &ParseEngine::GoExpandProdRow::id),
               tmpl::field("steps", &ParseEngine::GoExpandProdRow::steps));

namespace ParseEngine
{
namespace
{

/**
 * @brief 渲染一份**当值用**的模板（插进别的产物里当变量）。
 *
 * 片段模板按惯例以换行收尾，而它是插进别的产物**当值**的，多带一个换行就是
 * 产物里多出来一行，所以取完把结尾的换行收掉。**产物正文本身**要自带换行的
 * 那几份（`go_lr_tables.go` 之类）直接调 `tmpl::renderValues`，不走这里。
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
 * @brief 生成 Go 字符串字面量（解释型双引号字符串，控制字符用转义）。
 * @param text 输入文本。
 * @return 形如 `"abc"` 的字面量文本（含两侧引号）。
 */
std::string goString(const std::string &text)
{
    std::string out = "\"";
    for (const char raw : text)
    {
        const unsigned char ch = static_cast<unsigned char>(raw);
        if (ch == '\\')
        {
            out += "\\\\";
        }
        else if (ch == '"')
        {
            out += "\\\"";
        }
        else if (ch == '\n')
        {
            out += "\\n";
        }
        else if (ch == '\r')
        {
            out += "\\r";
        }
        else if (ch == '\t')
        {
            out += "\\t";
        }
        else if (ch < 0x20u)
        {
            char buffer[16];
            std::snprintf(buffer, sizeof(buffer), "\\u%04x",
                          static_cast<unsigned>(ch));
            out += buffer;
        }
        else
        {
            out += static_cast<char>(ch);
        }
    }
    out += "\"";
    return out;
}

/**
 * @brief 字面量在 Go 里占几个 rune（Unicode 码点；Go 的列号按 rune 算）。
 *
 * 词法「最长匹配」按这个数字排序，与 C / C++17 / Python / C# / Rust 的
 * `char` 计数口径一致。
 * @param text 字面量。
 * @return rune 个数。
 */
std::size_t goRuneCount(const std::string &text)
{
    std::size_t count = 0u;
    for (std::size_t index = 0u; index < text.size();)
    {
        const unsigned char ch = static_cast<unsigned char>(text[index]);
        if ((ch & 0x80u) == 0u)
        {
            index += 1u;
        }
        else if ((ch & 0xE0u) == 0xC0u)
        {
            index += 2u;
        }
        else if ((ch & 0xF0u) == 0xE0u)
        {
            index += 3u;
        }
        else
        {
            index += 4u;
        }
        ++count;
    }
    return count;
}

/**
 * @brief 非终结符到编号的映射。
 * @param nonterms 非终结符列表（声明序）。
 * @return 名字 -> 编号。
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

/**
 * @brief 取 Token 名在终结符表里的下标。
 * @param terminals Token 列表。
 * @param name Token 名。
 * @return 下标；找不到返回 0。
 */
std::size_t terminalIndex(const std::vector<std::string> &terminals,
                          const std::string &name)
{
    const auto found = std::find(terminals.begin(), terminals.end(), name);
    if (found == terminals.end())
    {
        return 0u;
    }
    return static_cast<std::size_t>(found - terminals.begin());
}

/// 一维整数表 -> 模板的元素数组（哪几个数之后换行在这里算，排版在模板里）。
std::vector<GoIntItem> intItems(const std::vector<long long> &values,
                                std::size_t perLine)
{
    std::vector<GoIntItem> items;
    items.reserve(values.size());
    for (std::size_t index = 0u; index < values.size(); ++index)
    {
        GoIntItem item;
        item.br = (index != 0u) && ((index % perLine) == 0u);
        item.value = values[index];
        items.push_back(item);
    }
    return items;
}

/// 一维整数表的正文（不含花括号）：每行 perLine 个，续行缩进 indent。
std::string intListText(const std::vector<long long> &values,
                        const std::string &indent, std::size_t perLine)
{
    return renderValueTemplate(
        "go_int_array.txt",
        {{"values", tmpl::encode(intItems(values, perLine))},
         {"indent", indent}});
}

/// 二维整数表（`[][]int32{…}`）：花括号、换行、缩进都在模板里。
std::string intRowsText(const std::vector<std::vector<long long>> &rows,
                        const std::string &indent)
{
    std::vector<GoIntRow> lines;
    lines.reserve(rows.size());
    for (const std::vector<long long> &row : rows)
    {
        GoIntRow line;
        line.items = intItems(row, 12u);
        lines.push_back(line);
    }
    return renderValueTemplate("go_int_rows.txt",
                               {{"rows", tmpl::encode(lines)},
                                {"indent", indent}});
}

/**
 * @brief 语义值类型信息。
 */
struct GoValueInfo
{
    std::string typeText; ///< 语义值类型（Go 写法）
    bool isUnion = false; ///< 语义值是带成员字段的 `ParserValue`
};

/**
 * @brief 取语义值类型信息（`%union` > `%value_type` > 缺省 `int64`）。
 * @param request 组装入参。
 * @return 类型信息。
 */
GoValueInfo goValueInfo(const ProjectRequest &request)
{
    GoValueInfo info;
    if (request.symbolValueMember != nullptr)
    {
        info.typeText = "ParserValue";
        info.isUnion = true;
        return info;
    }
    if (request.valueType != nullptr)
    {
        info.typeText = *request.valueType;
        return info;
    }
    info.typeText = "int64";
    return info;
}

/**
 * @brief 取 `%union` 成员的「类型原文」。
 *
 * Go 的 `%union` 成员和 C / C# / Rust 一样按「类型 成员名」写（例如
 * `int32 ival;`），生成器再拼成 Go 的字段写法 `ival int32`。
 * @param member %union 成员。
 * @return 类型文本。
 */
std::string goMemberType(const UnionMember &member)
{
    const std::string &text = member.cType;
    if ((text.size() >= member.name.size()) &&
        (text.compare(text.size() - member.name.size(), member.name.size(),
                      member.name) == 0))
    {
        std::string head = text.substr(0, text.size() - member.name.size());
        const std::size_t end = head.find_last_not_of(" \t\r\n");
        if (end != std::string::npos)
        {
            head = head.substr(0, end + 1u);
        }
        return head;
    }
    return text;
}

/// 取 `%union` 成员拼成 Go 字段写法（`成员名 类型`）。
std::string goMemberField(const UnionMember &member)
{
    return member.name + " " + goMemberType(member);
}

/**
 * @brief 取符号绑定的 `%union` 成员（名字 + 类型）。
 * @param request 组装入参。
 * @param symbol 符号名。
 * @param outMember 输出成员名。
 * @param outType 输出成员类型。
 * @return 该符号确实绑定了成员返回 true。
 */
bool symbolMemberType(const ProjectRequest &request, const std::string &symbol,
                      std::string &outMember, std::string &outType)
{
    if ((request.symbolValueMember == nullptr) ||
        (request.unionMembers == nullptr))
    {
        return false;
    }
    const auto memberIt = request.symbolValueMember->find(symbol);
    const std::string *member =
        memberIt != request.symbolValueMember->end() ? &memberIt->second : nullptr;
    if (member == nullptr)
    {
        return false;
    }
    for (const UnionMember &item : *request.unionMembers)
    {
        if (item.name == *member)
        {
            outMember = item.name;
            outType = goMemberType(item);
            return true;
        }
    }
    return false;
}

/// 这个 Go 类型能不能直接装下字符串。
bool goTextCarrier(const std::string &type)
{
    return (type == "string") || (type == "[]byte");
}

/// 这个 Go 类型是不是能装下数值的基础类型（`T(x)` 转换合法）。
bool goNumericType(const std::string &type)
{
    static const std::set<std::string> k_types = {
        "int",     "int8",    "int16",  "int32",  "int64",
        "uint",    "uint8",   "uint16", "uint32", "uint64",
        "float32", "float64", "byte",   "rune"};
    return k_types.count(type) != 0u;
}

/**
 * @brief 一份产物要用到的词法开关（决定发哪几块、import 哪几个包）。
 */
struct GoLexFlags
{
    bool hasRegex = false;     ///< 有 `%token regex` / `%identifier` / `%commit`
    bool hasCommits = false;   ///< 有 `%commit` / `%trivia`
    bool hasIdentifier = false; ///< 有 `%identifier`
    bool needStrconv = false;  ///< 内建数字块要发（要用 `strconv`）
};

/**
 * @brief GLR 的冲突候选动作表、起始符号与运行期上限。
 *
 * 主动作在 ACTION 表里（与其它 LR 模式同一套）；这里只放「同一个 (状态, Token)
 * 单元里除主动作外的候选」，运行时按它把一个 GSS 节点分裂成多条路径。
 * @param analysis 分析结果。
 * @return Go 代码段。
 */
std::string goGlrTables(const AnalysisResult &analysis)
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
        Utils::loadTemplate("go_glr_tables.go"),
        {{"start_symbol", static_cast<long long>(termCount + startId)},
         {"max_recovery_deletes", static_cast<long long>(256)},
         {"max_path_depth", static_cast<long long>(64)},
         {"max_path_visits", static_cast<long long>(200000)},
         {"extra_start", intRowsText(toRows(true), "")},
         {"extra_count", intRowsText(toRows(false), "")},
         {"extra_data",
          intListText(data.empty() ? std::vector<long long>{0} : data, "\t",
                      16u)}});
}

/**
 * @brief Earley 原文法表 / CYK CNF 表。
 *
 * 图表算法只负责整段输入判定；判定通过后仍由 Go GLR 运行时按原文法
 * 重建全部结果树并执行语义动作。
 */
std::string goChartTables(const AnalysisResult &analysis)
{
    const ChartGrammar chart = buildChartGrammar(analysis);
    const auto list = [](const std::vector<int> &values)
    {
        return intListText(
            std::vector<long long>(values.begin(), values.end()), "\t", 16u);
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
    return tmpl::renderValues(
        Utils::loadTemplate("go_chart_tables.go"),
        {{"start", chart.startNonterm},
         {"terminal_count", chart.terminalCount},
         {"nonterm_count", chart.nontermCount},
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

/**
 * @brief 取词法开关。
 * @param request 组装入参。
 * @return 开关。
 */
GoLexFlags goLexFlags(const ProjectRequest &request)
{
    GoLexFlags flags;
    const std::map<std::string, std::vector<std::string>> emptyTokenDefs;
    const auto &tokenDefs =
        (request.tokenDefs == nullptr) ? emptyTokenDefs : *request.tokenDefs;
    const LexSpec emptyLex;
    const LexSpec &lexSpec =
        (request.lexSpec == nullptr) ? emptyLex : *request.lexSpec;
    flags.hasIdentifier = lexSpec.identifier.has_value();
    flags.hasCommits = !lexSpec.commits.empty();
    flags.hasRegex = !lexSpec.regex.empty() || flags.hasIdentifier ||
                     flags.hasCommits;
    // 内建数字块没被 %token NUMBER / %token regex 覆盖时才发，也才用得上 strconv
    flags.needStrconv = (tokenDefs.count("NUMBER") == 0u) &&
                        (lexSpec.regex.count("NUMBER") == 0u);
    return flags;
}

/// 生成 `valueFromText` / `valueFromNumber` 两个函数。
std::string goValueHelpers(const ProjectRequest &request,
                           const std::vector<std::string> &terminals)
{
    const GoValueInfo info = goValueInfo(request);
    std::vector<GoValueCaseRow> textCases;
    bool hasNumberMember = false;
    std::string numberMember;
    std::string numberType;
    if (info.isUnion && (request.symbolValueMember != nullptr))
    {
        for (const auto &entry : *request.symbolValueMember)
        {
            std::string member;
            std::string memberType;
            if (!symbolMemberType(request, entry.first, member, memberType))
            {
                continue;
            }
            if (!goTextCarrier(memberType))
            {
                continue;
            }
            const auto found = std::find(terminals.begin(), terminals.end(),
                                         entry.first);
            if (found == terminals.end())
            {
                continue;
            }
            GoValueCaseRow row;
            row.kind = static_cast<int>(found - terminals.begin());
            row.member = member;
            textCases.push_back(row);
        }
        hasNumberMember =
            symbolMemberType(request, "NUMBER", numberMember, numberType) &&
            goNumericType(numberType);
    }
    return tmpl::renderValues(
        Utils::loadTemplate("go_value_helpers.go"),
        {{"value_type", info.typeText},
         {"is_union", info.isUnion},
         {"text_cases", tmpl::encode(textCases)},
         {"has_number_member", hasNumberMember},
         {"number_member", numberMember},
         {"number_type", numberType},
         {"is_text_carrier", goTextCarrier(info.typeText)},
         {"is_numeric", goNumericType(info.typeText)}});
}

/// ACTION / GOTO / 产生式元数据表（LR 家族）。
std::string goLrTables(const AnalysisResult &analysis)
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
        std::vector<std::pair<long long, std::size_t>> codes;
        for (std::size_t column = 0u; column < columnCount; ++column)
        {
            long long code = 0;
            long long scale = 1;
            for (const std::string &symbol : analysis.lookaheadColumns[column])
            {
                code += static_cast<long long>(terminalIds[symbol]) * scale;
                scale *= static_cast<long long>(analysis.terminals.size());
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
    return tmpl::renderValues(
        Utils::loadTemplate("go_lr_tables.go"),
        {{"action", intRowsText(actions, "")},
         {"goto", intRowsText(gotos, "")},
         {"has_reduce", hasReduce},
         {"reduce", intRowsText(reduces, "")},
         {"lookahead_code", intListText(codeList, "    ", 12u)},
         {"lookahead_column", intListText(columnList, "    ", 16u)},
         {"production_length", intListText(lengths, "    ", 16u)},
         {"production_lhs", intListText(lhs, "    ", 16u)}});
}

/// 预测分析表与左递归空尾标记表（LL 家族）。
std::string goLlTables(const AnalysisResult &analysis)
{
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
            const auto found =
                analysis.table.find(std::make_pair(nonterm, columnKey));
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
    const bool hasColumns = k >= 2;
    std::vector<long long> codeList;
    std::vector<long long> columnList;
    if (hasColumns)
    {
        std::map<std::string, int> terminalIds;
        for (std::size_t index = 0u; index < analysis.terminals.size(); ++index)
        {
            terminalIds[analysis.terminals[index]] = static_cast<int>(index);
        }
        std::vector<std::pair<long long, std::size_t>> codes;
        for (std::size_t column = 0u; column < analysis.lookaheadColumns.size();
             ++column)
        {
            long long code = 0;
            long long scale = 1;
            for (const std::string &symbol : analysis.lookaheadColumns[column])
            {
                code += static_cast<long long>(terminalIds[symbol]) * scale;
                scale *= static_cast<long long>(analysis.terminals.size());
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
    const bool hasFollow = k != 0;
    std::vector<std::vector<long long>> followRows;
    if (hasFollow)
    {
        for (const std::string &nonterm : analysis.nonterms)
        {
            const auto followIt = analysis.follow.find(nonterm);
            const std::set<std::string> *follow =
                followIt != analysis.follow.end() ? &followIt->second : nullptr;
            std::vector<long long> row;
            for (std::size_t index = 0u; index < analysis.terminals.size();
                 ++index)
            {
                if ((follow == nullptr) ||
                    (follow->count(analysis.terminals[index]) == 0u))
                {
                    continue;
                }
                row.push_back(static_cast<long long>(index));
            }
            followRows.push_back(row);
        }
    }
    return tmpl::renderValues(
        Utils::loadTemplate("go_ll_tables.go"),
        {{"ll_table", intRowsText(table, "")},
         {"has_columns", hasColumns},
         {"lookahead_code", intListText(codeList, "    ", 16u)},
         {"lookahead_column", intListText(columnList, "    ", 16u)},
         {"left_empty", intListText(leftEmpty, "    ", 16u)},
         {"has_follow", hasFollow},
         {"follow", intRowsText(followRows, "")}});
}

/// 语法树元数据：每条产生式的右部符号表。
std::string goTreeTables(const AnalysisResult &analysis)
{
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
                symbols.push_back(static_cast<long long>(
                    found - analysis.terminals.begin()));
            }
            else
            {
                symbols.push_back(termCount + ids.at(symbol));
            }
        }
    }
    return tmpl::renderValues(
        Utils::loadTemplate("go_tree_tables.go"),
        {{"prod_start", intListText(starts, "    ", 16u)},
         {"prod_length", intListText(lengths, "    ", 16u)},
         {"prod_symbols", intListText(
                              symbols.empty()
                                  ? std::vector<long long>{0}
                                  : symbols,
                              "    ", 16u)}});
}

/// 固定字面量词法规则表（按长度降序）。
std::vector<GoLexRuleRow> goLexRules(
    const std::map<std::string, std::vector<std::string>> &tokenDefs,
    const std::vector<std::string> &terminals,
    const std::map<std::string, int> &tokenLexOrder)
{
    struct Rule
    {
        std::string literal;
        std::string tokenName;
        std::size_t length = 0u;
        int order = 0;
        int declOrder = 0;
    };
    std::vector<Rule> rules;
    int order = 0;
    for (const auto &entry : tokenDefs)
    {
        const auto orderIt = tokenLexOrder.find(entry.first);
        const int declOrder = (orderIt != tokenLexOrder.end())
                                  ? orderIt->second
                                  : order;
        for (const std::string &literal : entry.second)
        {
            Rule rule;
            rule.literal = literal;
            rule.tokenName = entry.first;
            rule.length = goRuneCount(literal);
            rule.order = order;
            rule.declOrder = declOrder;
            ++order;
            rules.push_back(rule);
        }
    }
    std::stable_sort(rules.begin(), rules.end(),
                     [](const Rule &left, const Rule &right)
                     {
                         if (left.length != right.length)
                         {
                             return left.length > right.length;
                         }
                         return left.order < right.order;
                     });
    std::vector<GoLexRuleRow> rows;
    rows.reserve(rules.size());
    for (const Rule &rule : rules)
    {
        GoLexRuleRow row;
        row.text = goString(rule.literal);
        row.type =
            static_cast<int>(terminalIndex(terminals, rule.tokenName));
        row.length = static_cast<int>(rule.length);
        row.blank = literalCanStartBlank(rule.literal) ? 1 : 0;
        row.order = rule.declOrder;
        rows.push_back(row);
    }
    return rows;
}

/**
 * @brief `%token regex` / `%identifier` / `%commit` 的**正则表**。
 *
 * 产物里不放 DFA 表：词法匹配交给 Go 标准库的 `regexp`（RE2），模式在包级
 * 变量初始化时编译一遍。生成期用自带编译器做的那一遍校验在文法加载时
 * （`readRegexPattern`）就做过了，认不下的由 `main.cpp` 分流：C99 是硬错误、
 * 其余档只警告（自带编译器不认识不等于 `regexp` 不认，见
 * `GrammarConfig::regexDiagnostics`）。
 * @param lexSpec 词法声明。
 * @param terminals Token 列表。
 * @param flags 词法开关；`hasRegex` 为假时输出空串。
 * @return 正则表文本（空串表示这一档没有正则）。
 */
std::string goLexRegexes(const LexSpec &lexSpec,
                         const std::vector<std::string> &terminals,
                         const GoLexFlags &flags)
{
    if (!flags.hasRegex)
    {
        return std::string();
    }
    std::vector<GoRegexRow> regexRows;
    {
        int fallbackOrder = 0;
        for (const auto &entry : lexSpec.regex)
        {
            const auto orderIt = lexSpec.tokenLexOrder.find(entry.first);
            GoRegexRow row;
            row.pattern = goString(entry.second);
            row.type =
                static_cast<long long>(terminalIndex(terminals, entry.first));
            row.order = (orderIt != lexSpec.tokenLexOrder.end())
                            ? orderIt->second
                            : fallbackOrder;
            ++fallbackOrder;
            regexRows.push_back(row);
        }
    }
    std::vector<GoCommitRow> commitRows;
    std::vector<GoCommitAliasRow> commitAliases;
    for (const CommitSpec &commit : lexSpec.commits)
    {
        GoCommitRow row;
        row.pattern = goString(commit.pattern);
        row.type = goString(commit.type);
        row.skip = commit.isSkip;
        row.order = commit.order;
        commitRows.push_back(row);
        if (!commit.alias.empty())
        {
            GoCommitAliasRow alias;
            alias.type = goString(commit.type);
            alias.alias = goString(commit.alias);
            commitAliases.push_back(alias);
        }
    }
    return tmpl::renderValues(
        Utils::loadTemplate("go_lex_regexes.go"),
        {{"regexes", tmpl::encode(regexRows)},
         {"has_identifier", flags.hasIdentifier},
         {"identifier_pattern",
          flags.hasIdentifier ? goString(*lexSpec.identifier) : std::string()},
         {"has_commits", flags.hasCommits},
         {"commits", tmpl::encode(commitRows)},
         {"commit_aliases", tmpl::encode(commitAliases)}});
}

/// 词法表段（固定字面量规则 + `%space` + 正则表）。
std::string goLexTables(const ProjectRequest &request,
                        const std::vector<std::string> &terminals,
                        const GoLexFlags &flags)
{
    const std::map<std::string, std::vector<std::string>> emptyTokenDefs;
    const auto &tokenDefs =
        (request.tokenDefs == nullptr) ? emptyTokenDefs : *request.tokenDefs;
    const LexSpec emptyLex;
    const LexSpec &lexSpec =
        (request.lexSpec == nullptr) ? emptyLex : *request.lexSpec;
    const std::string &chars =
        (request.spaceChars == nullptr) ? blankChars() : *request.spaceChars;
    std::string out = tmpl::renderValues(
        Utils::loadTemplate("go_lex_tables.go"),
        {{"rules", tmpl::encode(
                       goLexRules(tokenDefs, terminals,
                                  lexSpec.tokenLexOrder))},
         {"skip_chars", goString(chars)}});
    out += goLexRegexes(lexSpec, terminals, flags);
    return out;
}

/// 同步 Token / error 伪 Token / 非终结符名表等杂项表。
std::string goMiscTables(const ProjectRequest &request,
                         const AnalysisResult &analysis)
{
    std::vector<long long> syncTypes;
    if (request.syncTokens != nullptr)
    {
        for (const std::string &token : *request.syncTokens)
        {
            const auto found = std::find(analysis.terminals.begin(),
                                         analysis.terminals.end(), token);
            if (found == analysis.terminals.end())
            {
                continue;
            }
            syncTypes.push_back(
                static_cast<long long>(found - analysis.terminals.begin()));
        }
    }
    std::size_t errorTokenId = 0u;
    if (request.usesError)
    {
        const auto found = std::find(analysis.terminals.begin(),
                                     analysis.terminals.end(), "error");
        if (found != analysis.terminals.end())
        {
            errorTokenId =
                static_cast<std::size_t>(found - analysis.terminals.begin());
        }
    }
    std::vector<std::string> nontermNames;
    nontermNames.reserve(analysis.nonterms.size());
    for (const std::string &nonterm : analysis.nonterms)
    {
        nontermNames.push_back(goString(nonterm));
    }
    return tmpl::renderValues(
        Utils::loadTemplate("go_misc_tables.go"),
        {{"sync_tokens", intListText(syncTypes, "    ", 16u)},
         {"has_error_token", request.usesError},
         {"error_token_id", static_cast<long long>(errorTokenId)},
         {"nonterm_names", tmpl::encode(nontermNames)}});
}

/// 组装语义值类型定义（`%union` 时生成 `ParserValue` 结构体）。
std::string goValueTypeDecl(const ProjectRequest &request)
{
    if ((request.symbolValueMember == nullptr) ||
        (request.unionMembers == nullptr))
    {
        return std::string();
    }
    std::vector<std::string> members;
    members.reserve(request.unionMembers->size());
    for (const UnionMember &member : *request.unionMembers)
    {
        members.push_back(goMemberField(member));
    }
    return tmpl::renderValues(Utils::loadTemplate("go_value_type_decl.go"),
                              {{"members", tmpl::encode(members)}});
}

/// 容量常量段与「期望 Token」表。
std::string goConstants(const AnalysisResult &analysis,
                        const std::string &mode)
{
    const std::map<std::string, int> ids = nontermIds(analysis.nonterms);
    const auto startFound = ids.find(analysis.grammar.start);
    std::map<std::string, int> expectedIds;
    for (std::size_t index = 0u; index < analysis.terminals.size(); ++index)
    {
        expectedIds[analysis.terminals[index]] = static_cast<int>(index);
    }
    std::vector<std::vector<long long>> expectedRows;
    for (const std::vector<std::string> &row : expectedTokensByRow(analysis))
    {
        std::vector<long long> rowIds;
        rowIds.reserve(row.size());
        for (const std::string &token : row)
        {
            rowIds.push_back(static_cast<long long>(expectedIds[token]));
        }
        expectedRows.push_back(rowIds);
    }
    return tmpl::renderValues(
        Utils::loadTemplate("go_constants.go"),
        {{"terminal_count",
          static_cast<long long>(analysis.terminals.size())},
         {"nonterm_count", static_cast<long long>(analysis.nonterms.size())},
         {"start_id",
          static_cast<long long>((startFound == ids.end())
                                     ? 0
                                     : startFound->second)},
         {"state_count",
          static_cast<long long>(isLrMode(mode) ? analysis.stateCount() : 0u)},
         {"lookahead_k", static_cast<long long>(std::max(0, analysis.lookaheadK))},
         {"lookahead_column_count",
          static_cast<long long>(analysis.lookaheadColumns.size())},
         {"expected_rows", intRowsText(expectedRows, "")},
         {"expected_fallback", goString(expectedFallbackText(analysis.mode))}});
}

/// 递归下降的表：非终结符 -> 产生式区间 + 语义槽位个数。
///
/// 产生式在 `analysis.productions` 里「同一左部连续排在一起」（摊平时按非终结符
/// 名有序、规则内按声明顺序），所以一个非终结符对应一段连续编号——回溯时依次试
/// 的就是这个顺序。右部符号不在这里重发，直接查 sParserTreeProd*。
std::string goDdTables(const AnalysisResult &analysis)
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
        Utils::loadTemplate("go_dd_tables.go"),
        {{"prod_start", intListText(starts, "\t", 16u)},
         {"prod_count", intListText(counts, "\t", 16u)},
         {"prod_slots", intListText(slots, "\t", 16u)}});
}

/// Token 类型段（常量 + 名字表），进 token.go。
std::string goTokenTypes(const std::vector<std::string> &terminals,
                         const std::map<std::string, std::string> &aliases)
{
    std::vector<GoTokenConst> tokens;
    std::vector<std::string> names;
    tokens.reserve(terminals.size());
    names.reserve(terminals.size());
    for (std::size_t index = 0u; index < terminals.size(); ++index)
    {
        GoTokenConst item;
        item.name = terminals[index];
        item.value = static_cast<int>(index);
        tokens.push_back(item);
        names.push_back(goString(terminals[index]));
    }
    std::vector<GoAliasRow> aliasRows;
    for (const auto &entry : aliases)
    {
        GoAliasRow row;
        row.name = goString(entry.first);
        row.alias = goString(entry.second);
        aliasRows.push_back(row);
    }
    return tmpl::renderValues(
        Utils::loadTemplate("go_token_types.go"),
        {{"tokens", tmpl::encode(tokens)},
         {"invalid_value", static_cast<long long>(terminals.size())},
         {"names_count", static_cast<long long>(terminals.size() + 1u)},
         {"names", tmpl::encode(names)},
         {"aliases", tmpl::encode(aliasRows)}});
}

/// 语义动作分支（LR / LL 共用一套 `applyAction`）。
std::string goActionCases(const AnalysisResult &analysis,
                          const std::string &translateMode,
                          const ProjectRequest &request,
                          const std::string &fileName, Error &error)
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
    std::vector<GoActionRow> rows;
    for (const Production &production : analysis.productions)
    {
        if (production.transform == "left_empty")
        {
            continue;
        }
        GoActionRow row;
        row.id = production.id;
        row.first = rows.empty();
        row.fileName = fileName;
        const std::string action = trim(production.action);
        if (!action.empty())
        {
            UnionValueSpec unionSpec;
            const UnionValueSpec *unionArg = nullptr;
            if (request.symbolValueMember != nullptr)
            {
                unionSpec.symbolMember = request.symbolValueMember;
                unionSpec.lhsSymbol = production.lhs;
                unionSpec.slotSymbols = production.rhs;
                unionSpec.productionId = production.id;
                if ((production.transform == "left_continue") &&
                    !production.owner.empty())
                {
                    unionSpec.lhsSymbol = production.owner;
                    unionSpec.slotSymbols.assign(1, production.owner);
                    unionSpec.slotSymbols.insert(unionSpec.slotSymbols.end(),
                                                 production.rhs.begin(),
                                                 production.rhs.end());
                }
                unionArg = &unionSpec;
            }
            std::string translated;
            if (!translateAction(action, translateMode,
                                 static_cast<int>(production.rhs.size()),
                                 unionArg, false, false, translated, error,
                                 /*csharpStyle=*/false, /*rustStyle=*/false,
                                 /*goStyle=*/true))
            {
                if ((error.line == 0u) && (production.actionLine != 0u))
                {
                    error.line = production.actionLine;
                }
                return {};
            }
            std::size_t pos = 0u;
            while (pos <= translated.size())
            {
                const std::size_t found = translated.find('\n', pos);
                const std::string line = trim(translated.substr(
                    pos, (found == std::string::npos) ? std::string::npos
                                                      : found - pos));
                if (!line.empty())
                {
                    row.body.push_back(line);
                }
                if (found == std::string::npos)
                {
                    break;
                }
                pos = found + 1u;
            }
            row.hasLine = production.actionLine != 0u;
            row.line = static_cast<int>(production.actionLine);
        }
        rows.push_back(row);
    }
    return tmpl::renderValues(Utils::loadTemplate("go_action_cases.go"),
                              {{"cases", tmpl::encode(rows)}});
}

/// 产生式展开分支（只有 LL 用）。
std::string goExpandCases(const AnalysisResult &analysis,
                          const std::vector<std::string> &terminals)
{
    const std::set<std::string> terminalSet(terminals.begin(),
                                            terminals.end());
    const std::map<std::string, int> ids = nontermIds(analysis.nonterms);
    const auto pushCall = [](std::vector<GoExpandStepRow> &steps,
                             const std::string &kind, const std::string &symbol,
                             int slot)
    {
        GoExpandStepRow step;
        step.kind = kind;
        step.symbol = symbol;
        step.slot = slot;
        steps.push_back(step);
    };
    const auto pushSymbol = [&terminalSet, &terminals, &ids,
                             &pushCall](std::vector<GoExpandStepRow> &steps,
                                        const std::string &symbol, int slot)
    {
        if (terminalSet.count(symbol) != 0u)
        {
            const auto found =
                std::find(terminals.begin(), terminals.end(), symbol);
            pushCall(steps, "sItemTerminal",
                     std::to_string(found - terminals.begin()), slot);
            return;
        }
        pushCall(steps, "sItemNonterm", std::to_string(ids.at(symbol)), slot);
    };
    std::vector<GoExpandProdRow> rows;
    for (const Production &production : analysis.productions)
    {
        const std::vector<std::string> &rhs = production.rhs;
        const std::string &transform = production.transform;
        GoExpandProdRow row;
        row.id = production.id;
        if (transform == "left_base")
        {
            const int baseLength = production.baseRhsLength;
            pushCall(row.steps, "sItemLeftLoop",
                     std::to_string(ids.at(rhs.back())), 0);
            pushCall(row.steps, "sItemActionKeep",
                     std::to_string(production.id), 0);
            for (std::size_t index = static_cast<std::size_t>(baseLength);
                 index-- > 0u;)
            {
                pushSymbol(row.steps, rhs[index], 1 + static_cast<int>(index));
            }
        }
        else if (transform == "left_continue")
        {
            pushCall(row.steps, "sItemLeftLoop",
                     std::to_string(ids.at(rhs.back())), 0);
            pushCall(row.steps, "sItemActionKeep",
                     std::to_string(production.id), 0);
            for (std::size_t index = rhs.size() - 1u; index-- > 0u;)
            {
                pushSymbol(row.steps, rhs[index], 2 + static_cast<int>(index));
            }
        }
        else if (transform == "left_empty")
        {
            GoExpandStepRow step;
            step.isComment = true;
            row.steps.push_back(step);
        }
        else
        {
            pushCall(row.steps, "sItemAction", std::to_string(production.id),
                     0);
            for (std::size_t index = rhs.size(); index-- > 0u;)
            {
                pushSymbol(row.steps, rhs[index], 1 + static_cast<int>(index));
            }
        }
        rows.push_back(row);
    }
    return tmpl::renderValues(Utils::loadTemplate("go_expand_cases.go"),
                              {{"prods", tmpl::encode(rows)}});
}

/// 本模式的解析主循环入口（`runLoop`）。
std::string goRunLoop(const std::string &mode)
{
    return tmpl::renderValues(
        Utils::loadTemplate("go_run_loop.go"),
        {{"target", isDdMode(mode)
                        ? "runDdLoop"
                        : (isGlrMode(mode)
                               ? "runGLRLoop"
                               : (isLrMode(mode) ? "runLRLoop"
                                                 : "runLLLoop"))}});
}

/**
 * @brief import 块（标准库 + `%import`）。
 *
 * 顺序按字母排（与 gofmt / goimports 排出来的顺序一致）；`strconv` 与
 * `regexp` 只在真的用到时才写进去——Go 对「imported and not used」是硬错误。
 * 用户的 `%import` 名字里的 `::` 归一成 `/`（Go 路径里的斜杠），
 * 与标准库重复的直接跳过（Go 里重复 import 同一个包也是编译错误）。
 * @param request 组装入参。
 * @param needStrconv 内建数字块要发（用到 `strconv`）。
 * @param needRegex 有词法正则（用到 `regexp`）。
 * @return import 块文本；**自带结尾空行**（所以走 renderValues 不收尾）。
 */
std::string goImportLines(const ProjectRequest &request, bool needStrconv,
                          bool needRegex)
{
    std::vector<std::string> lines;
    lines.push_back("\"fmt\"");
    if (needRegex)
    {
        lines.push_back("\"regexp\"");
    }
    if (needStrconv)
    {
        lines.push_back("\"strconv\"");
    }
    lines.push_back("\"strings\"");
    if (request.imports != nullptr)
    {
        for (const std::string &name : *request.imports)
        {
            const std::size_t begin = name.find_first_not_of(" \t\r\n");
            if (begin == std::string::npos)
            {
                continue;
            }
            const std::size_t end = name.find_last_not_of(" \t\r\n");
            std::string text = name.substr(begin, end - begin + 1u);
            // `::` 归一成 `/`：Go 的 import 路径里有斜杠，文法里写不了 `/`
            // 之外的分隔符时用 `::`（与 %package 的 module path 同一套写法）。
            std::string fixed;
            for (std::size_t index = 0u; index < text.size(); ++index)
            {
                if ((text[index] == ':') && ((index + 1u) < text.size()) &&
                    (text[index + 1u] == ':'))
                {
                    fixed += '/';
                    ++index;
                    continue;
                }
                fixed += text[index];
            }
            // 已经带引号的（含 `别名 "路径"` 写法）原样贴，否则补上引号。
            const std::string line = (fixed.find('"') != std::string::npos)
                                         ? fixed
                                         : goString(fixed);
            // 与标准库那几行重复的（Go 里重复 import 同一个包是编译错误）
            // 直接跳过：`%import "fmt"` 这种写法不该让产物编不过。
            if (std::find(lines.begin(), lines.end(), line) != lines.end())
            {
                continue;
            }
            lines.push_back(line);
        }
    }
    return tmpl::renderValues(Utils::loadTemplate("go_import_lines.go"),
                              {{"imports", tmpl::encode(lines)}});
}

/// `%package` 在产物里留下的注释（Go 的 module path 写在 go.mod 里）。
std::string goPackageComment(const ProjectRequest &request)
{
    if (request.packageName.empty())
    {
        return std::string();
    }
    return tmpl::renderValues(Utils::loadTemplate("go_package_comment.go"),
                              {{"package", request.packageName},
                               {"module", CodeGenGo::moduleName(request)}});
}

/**
 * @brief 词法器各段的替换表。
 */
struct GoLexerBlocks
{
    std::string stringBlock;     ///< 内建字符串字面量块
    std::string matchBlock;      ///< `lexNext` 的最长匹配主体
    std::string numberBlock;     ///< 内建数字块
    std::string identifierBlock; ///< 标识符块（正则或缺省）
};

/**
 * @brief 组装词法器各段（按文法声明决定哪几块要发）。
 * @param request 组装入参。
 * @param flags 词法开关。
 * @param blocks 输出。
 */
void buildGoLexerBlocks(const ProjectRequest &request, const GoLexFlags &flags,
                        GoLexerBlocks &blocks)
{
    const std::map<std::string, std::vector<std::string>> emptyTokenDefs;
    const auto &tokenDefs =
        (request.tokenDefs == nullptr) ? emptyTokenDefs : *request.tokenDefs;
    const LexSpec emptyLex;
    const LexSpec &lexSpec =
        (request.lexSpec == nullptr) ? emptyLex : *request.lexSpec;
    const auto overridden = [&tokenDefs, &lexSpec](const std::string &name)
    {
        return (tokenDefs.count(name) != 0u) ||
               (lexSpec.regex.count(name) != 0u);
    };
    if (!overridden("STRING"))
    {
        blocks.stringBlock = Utils::loadTemplate("go_lexer_string.go");
    }
    std::string regexLoop;
    if (!lexSpec.regex.empty())
    {
        regexLoop = Utils::loadTemplate("go_lexer_regex_loop.go");
    }
    blocks.matchBlock = tmpl::renderValues(
        Utils::loadTemplate("go_lexer_match.go"),
        {{"lex_regex_loop", regexLoop},
         {"has_commits", flags.hasCommits}});
    if (!overridden("NUMBER"))
    {
        blocks.numberBlock = Utils::loadTemplate("go_lexer_number.go");
    }
    if (lexSpec.identifier.has_value())
    {
        blocks.identifierBlock =
            Utils::loadTemplate("go_lexer_identifier_regex.go");
    }
    else if (!overridden("VARIABLE"))
    {
        blocks.identifierBlock =
            Utils::loadTemplate("go_lexer_identifier.go");
    }
}

/// 组装词法器（解析与词法模式共用同一份模板）。
std::string goLexerText(const ProjectRequest &request, const GoLexFlags &flags)
{
    GoLexerBlocks blocks;
    buildGoLexerBlocks(request, flags, blocks);
    return tmpl::renderValues(
        Utils::loadTemplate("go_lexer.go"),
        {{"lex_string_block", blocks.stringBlock},
         {"lex_match_block", blocks.matchBlock},
         {"lex_number_block", blocks.numberBlock},
         {"lex_identifier_block", blocks.identifierBlock}});
}

/// 取 `%code … %endcode` 的正文（带前后行标记）。
std::string goUserCode(const ProjectRequest &request,
                       const std::string &fileName)
{
    if ((request.lines == nullptr) ||
        (request.lines->userBlocks == nullptr) ||
        (request.lines->userBlockLines == nullptr) ||
        (request.lines->userBlocks->size() !=
         request.lines->userBlockLines->size()))
    {
        return request.userCode;
    }
    std::vector<std::string> blocks;
    for (std::size_t index = 0u; index < request.lines->userBlocks->size();
         ++index)
    {
        const std::string &block = (*request.lines->userBlocks)[index];
        if (block.empty())
        {
            continue;
        }
        blocks.push_back(CodeGenGo::withLineMarkers(
            block, (*request.lines->userBlockLines)[index], "", fileName));
    }
    if (blocks.empty())
    {
        return std::string();
    }
    return renderValueTemplate("go_user_code.go",
                               {{"blocks", tmpl::encode(blocks)}});
}

/// 关键字表段（只有 `%identifier` 声明时才发）。
std::string goKeywordSection(const ProjectRequest &request)
{
    const std::map<std::string, std::vector<std::string>> emptyTokenDefs;
    const LexSpec emptyLex;
    const auto &tokenDefs =
        (request.tokenDefs == nullptr) ? emptyTokenDefs : *request.tokenDefs;
    const LexSpec &lexSpec =
        (request.lexSpec == nullptr) ? emptyLex : *request.lexSpec;
    if (!lexSpec.identifier.has_value())
    {
        return std::string();
    }
    std::vector<GoKeywordRow> rows;
    for (const auto &entry : tokenDefs)
    {
        for (const std::string &literal : entry.second)
        {
            bool identifier = !literal.empty() &&
                              ((std::isalpha(static_cast<unsigned char>(
                                    literal[0])) != 0) ||
                               (literal[0] == '_'));
            for (std::size_t index = 1u;
                 identifier && (index < literal.size()); ++index)
            {
                const char ch = literal[index];
                identifier =
                    (std::isalnum(static_cast<unsigned char>(ch)) != 0) ||
                    (ch == '_');
            }
            if (!identifier)
            {
                continue;
            }
            GoKeywordRow row;
            row.literal = goString(literal);
            row.token = entry.first;
            rows.push_back(row);
        }
    }
    return tmpl::renderValues(Utils::loadTemplate("go_lexer_keyword.go"),
                              {{"rows", tmpl::encode(rows)}});
}

/// `%class_body` 正文（贴成一组 `func (p *Parser) …` 方法）。
std::string goClassBody(const ProjectRequest &request)
{
    if ((request.classBody == nullptr) || request.classBody->empty())
    {
        return std::string();
    }
    return tmpl::renderValues(
        Utils::loadTemplate("go_class_body.go"),
        {{"body", CodeGenGo::withLineMarkers(*request.classBody,
                                             request.classBodyLine + 1u, "",
                                             "parser.go")}});
}

/**
 * @brief 组装 parser.go（解析模式）。
 * @param request 组装入参。
 * @param mode 归一化后的模式。
 * @param analysis 分析结果。
 * @param out 输出文本（成功时写入）。
 * @param error 出错时写入错误说明。
 * @return 成功返回 true。
 */
bool buildParserGo(const ProjectRequest &request, const std::string &mode,
                   const AnalysisResult &analysis, std::string &out,
                   Error &error)
{
    const GoValueInfo info = goValueInfo(request);
    const GoLexFlags flags = goLexFlags(request);
    const std::vector<std::string> &terminals = analysis.terminals;
    std::size_t maxSlots = 4u;
    for (const Production &production : analysis.productions)
    {
        maxSlots = std::max(maxSlots, production.rhs.size() + 3u);
    }
    std::string tables = goLexTables(request, terminals, flags);
    tables += tmpl::renderValues(Utils::loadTemplate("go_section_marker.txt"),
                                 {{"title", "分析表"}});
    if (isLrMode(mode))
    {
        tables += goLrTables(analysis);
    }
    else
    {
        tables += goLlTables(analysis);
    }
    if (!isLrMode(mode))
    {
        tables += tmpl::renderValues(
            Utils::loadTemplate("go_section_marker.txt"),
            {{"title", "语法树表"}});
        tables += goTreeTables(analysis);
    }
    if (isGlrMode(mode))
    {
        tables += goGlrTables(analysis);
        if ((mode == "earley") || (mode == "cyk"))
        {
            tables += goChartTables(analysis);
        }
    }
    if (isDdMode(mode))
    {
        tables += goDdTables(analysis);
    }
    tables += goMiscTables(request, analysis);

    const std::string actionCases =
        goActionCases(analysis,
                      isDdMode(mode) ? "dd"
                                     : (isLrMode(mode) ? analysis.mode : "ll1"),
                      request, "parser.go", error);
    if (!error.empty())
    {
        return false;
    }
    // 语义动作与产生式展开两块：一个产物 = 一份模板，先各自渲染再把结果
    // 当变量传进引擎（渲染是一趟的，值里的 `{{ }}` 不会再被解析）。
    // `go_lr_action.go` 是**产物正文**（自带换行），走 renderValues 别收尾。
    const std::string lrAction = tmpl::renderValues(
        Utils::loadTemplate("go_lr_action.go"),
        {{"value_type", info.typeText}, {"lr_action_cases", actionCases}});
    std::string lrText = lrAction;
    std::string llText;
    std::string ddText;
    if (isDdMode(mode))
    {
        // DD：动作分支 + 带回溯的递归下降驱动（不建表，直接照文法走）
        ddText = tmpl::renderValues(Utils::loadTemplate("go_dd.go"),
                                    {{"value_type", info.typeText}});
    }
    else if (isGlrMode(mode))
    {
        // GLR：动作分支 + 图结构栈驱动（替代 LR 的分析栈驱动）
        lrText += tmpl::renderValues(
            Utils::loadTemplate("go_glr.go"),
            {{"value_type", info.typeText},
             {"chart", (mode == "earley") || (mode == "cyk")}});
        if ((mode == "earley") || (mode == "cyk"))
        {
            lrText += tmpl::renderValues(
                Utils::loadTemplate("go_chart.go"),
                {{"earley", mode == "earley"}});
        }
    }
    else if (isLrMode(mode))
    {
        lrText += tmpl::renderValues(Utils::loadTemplate("go_lr.go"),
                                     {{"value_type", info.typeText}});
    }
    else
    {
        llText = tmpl::renderValues(Utils::loadTemplate("go_ll.go"),
                                    {{"value_type", info.typeText},
                                     {"ll_expand_cases",
                                      goExpandCases(analysis, terminals)}});
    }

    std::string constants = goValueTypeDecl(request);
    constants += goConstants(analysis, mode);

    const std::string engine = tmpl::renderValues(
        Utils::loadTemplate("go_engine.go"),
        {{"mode_name", modeLabelText(mode)},
         {"max_stack_depth",
          static_cast<long long>(request.maxStackDepth.value_or(64))},
         {"max_errors", static_cast<long long>(request.maxErrors.value_or(8))},
         {"error_recovery_tokens",
          static_cast<long long>(request.errorRecoveryTokens.value_or(3))},
         {"value_slots", static_cast<long long>(maxSlots)},
         {"constants", constants},
         {"tables", tables},
         {"lexer_helpers",
          tmpl::renderValues(Utils::loadTemplate("go_lexer_helpers.go"),
                             {{"has_regex", flags.hasRegex}})},
         {"value_helpers", goValueHelpers(request, terminals)},
         {"keyword_method", goKeywordSection(request)},
         {"glr", isGlrMode(mode)},
         {"dd", isDdMode(mode)},
         {"run_loop", goRunLoop(mode)},
         {"lexer", goLexerText(request, flags)},
         {"lr", lrText},
         {"ll", llText},
         {"dd_body", ddText},
         {"value_type", info.typeText}});

    out = tmpl::renderValues(
        Utils::loadTemplate("go_parser.go"),
        {{"mode_name", modeLabelText(mode)},
         {"package_comment", goPackageComment(request)},
         {"imports", goImportLines(request, flags.needStrconv,
                                   flags.hasRegex)},
         {"user_code", goUserCode(request, "parser.go")},
         {"engine", engine},
         {"class_body", goClassBody(request)}});
    return true;
}

} // namespace

std::string CodeGenGo::grammarLineMarker(std::size_t lineNumber)
{
    if (lineNumber == 0u)
    {
        return std::string();
    }
    return tmpl::renderValues(Utils::loadTemplate("go_line_marker.go"),
                              {{"line", static_cast<long long>(lineNumber)}});
}

std::string CodeGenGo::withLineMarkers(const std::string &body,
                                       std::size_t lineNumber,
                                       const std::string &indent,
                                       const std::string &fileName)
{
    std::string text = body;
    if (!text.empty() && (text.back() != '\n'))
    {
        text += '\n';
    }
    return tmpl::renderValues(
        Utils::loadTemplate("go_with_line_markers.go"),
        {{"has_line", lineNumber != 0u},
         {"line", static_cast<long long>(lineNumber)},
         {"indent", indent},
         {"body", text},
         {"file_name", fileName}});
}

bool CodeGenGo::parserGo(const ProjectRequest &request, const std::string &mode,
                         const AnalysisResult &analysis, std::string &out,
                         Error &error)
{
    return buildParserGo(request, mode, analysis, out, error);
}

void CodeGenGo::tokenGo(const ProjectRequest &request,
                        const std::vector<std::string> &terminals,
                        std::string &out)
{
    const GoValueInfo info = goValueInfo(request);
    const std::map<std::string, std::string> emptyAliases;
    const auto &aliases = (request.lexSpec == nullptr)
                              ? emptyAliases
                              : request.lexSpec->tokenAliases;
    out = tmpl::renderValues(
        Utils::loadTemplate("go_token.go"),
        {{"package_comment", goPackageComment(request)},
         {"value_type", info.typeText},
         {"token_types", goTokenTypes(terminals, aliases)}});
}

/**
 * @brief 演示 main 里的 `%commit` + location 打印片段（Go）。
 * @param request 组装入参。
 * @return Go 代码段；文法里没写 %commit 时为空串（演示输出与从前一样）。
 */
std::string goDemoCommitPrint(const ProjectRequest &request)
{
    const LexSpec emptyLex;
    const LexSpec &lexSpec =
        (request.lexSpec == nullptr) ? emptyLex : *request.lexSpec;
    if (lexSpec.commits.empty())
    {
        return std::string();
    }
    // 纯静态片段：仍然过一遍引擎，把 `{# #}` 说明块收掉
    return tmpl::renderValues(
        Utils::loadTemplate("go_demo_commit_print.go"));
}

std::string CodeGenGo::mainGo(const ProjectRequest &request,
                              const std::string &mode,
                              const std::string &mainBody)
{
    std::string body;
    if (!mainBody.empty())
    {
        body = tmpl::renderValues(Utils::loadTemplate("go_main_body.go"),
                                  {{"body", mainBody}});
    }
    else
    {
        if (isGlrMode(mode))
        {
            const std::string print = tmpl::renderValues(
                request.tree
                    ? Utils::loadTemplate("go_glr_demo_result_tree.go")
                    : Utils::loadTemplate("go_glr_demo_result_value.go"));
            body = tmpl::renderValues(
                Utils::loadTemplate("go_glr_demo_main.go"),
                {{"glr_run", request.tree ? "engine.RunGLRTree(inputText)"
                                          : "engine.RunGLR(inputText)"},
                 {"glr_print", print},
                 {"demo_commits", goDemoCommitPrint(request)}});
        }
        else
        {
            body = tmpl::renderValues(
                Utils::loadTemplate("go_demo_main.go"),
                {{"tree", request.tree},
                 {"dd", isDdMode(mode)},
                 {"demo_commits", goDemoCommitPrint(request)}});
        }
    }
    return tmpl::renderValues(Utils::loadTemplate("go_main.go"),
                              {{"mode_name", modeLabelText(mode)},
                               {"package_comment", goPackageComment(request)},
                               {"main_body", body}});
}

std::string CodeGenGo::lexerMainGo(const ProjectRequest &request,
                                   const std::string &mainBody)
{
    std::string body;
    if (!mainBody.empty())
    {
        body = tmpl::renderValues(Utils::loadTemplate("go_main_body.go"),
                                  {{"body", mainBody}});
    }
    else
    {
        body = Utils::loadTemplate("go_lex_demo_main.go");
    }
    return tmpl::renderValues(Utils::loadTemplate("go_lex_main.go"),
                              {{"mode_name", "LEX(词法模式)"},
                               {"package_comment", goPackageComment(request)},
                               {"main_body", body}});
}

void CodeGenGo::lexerGo(const ProjectRequest &request,
                        const std::vector<std::string> &terminals,
                        std::string &out)
{
    const GoLexFlags flags = goLexFlags(request);
    std::string tables = goLexTables(request, terminals, flags);
    const std::string engine = tmpl::renderValues(
        Utils::loadTemplate("go_lex_engine.go"),
        {{"constants", tmpl::renderValues(
                           Utils::loadTemplate("go_lex_constants.go"),
                           {{"terminal_count",
                             static_cast<long long>(terminals.size())}})},
         {"tables", tables},
         {"lexer_helpers",
          tmpl::renderValues(Utils::loadTemplate("go_lexer_helpers.go"),
                             {{"has_regex", flags.hasRegex}})},
         {"value_helpers", goValueHelpers(request, terminals)},
         {"keyword_method", goKeywordSection(request)},
         {"max_errors", static_cast<long long>(request.maxErrors.value_or(8))},
         {"lexer", goLexerText(request, flags)}});
    out = tmpl::renderValues(
        Utils::loadTemplate("go_parser.go"),
        {{"mode_name", "LEX(词法模式)"},
         {"package_comment", goPackageComment(request)},
         {"imports", goImportLines(request, flags.needStrconv,
                                   flags.hasRegex)},
         {"user_code", goUserCode(request, "parser.go")},
         {"engine", engine},
         {"class_body", goClassBody(request)}});
}

std::string CodeGenGo::moduleName(const ProjectRequest &request)
{
    if (request.packageName.empty())
    {
        return "parserdemo";
    }
    // Go 的 module path 用 `/` 分层；把 C++ / Java 习惯的 `::` 也归一化。
    std::string fixed;
    for (std::size_t index = 0u; index < request.packageName.size(); ++index)
    {
        if ((request.packageName[index] == ':') &&
            ((index + 1u) < request.packageName.size()) &&
            (request.packageName[index + 1u] == ':'))
        {
            fixed += '/';
            ++index;
            continue;
        }
        fixed += request.packageName[index];
    }
    return fixed;
}

std::string CodeGenGo::goMod(const ProjectRequest &request)
{
    return tmpl::renderValues(Utils::loadTemplate("go_mod"),
                              {{"module_name", moduleName(request)}});
}

} // namespace ParseEngine
