/**
 * @file code_gen_csharp.cpp
 * @brief C# 语言后端：Parser.cs / Token.cs / Program.cs / parser.csproj。
 *
 * 由生成器与模板共同产出，口径与其余语言后端逐项对齐：
 *   - 生成期只换语法：`package` -> `namespace`、`import` -> `using`、
 *     `final` -> `readonly` / `const`、`ArrayList` -> `List`、类型名大写；
 *   - 词法器同样把「Token 词素 -> 语义值」集中在 `_valueFromText` /
 *     `_valueFromNumber` 两个静态方法里；
 */

#include "ParseEngine/code_gen_csharp.hpp"

#include <algorithm>
#include <cstdio>
#include <map>
#include <set>

#include "ParseEngine/action_translator.hpp"
#include "ParseEngine/chart_grammar.hpp"
#include "ParseEngine/code_gen.hpp"
#include "ParseEngine/grammar_types.hpp"
#include "ParseEngine/utils.hpp"

// C# 产物模板（source/templates/csharp_*.tmpl）走 library/template 的模板引擎
#include "template/engine.hpp"

namespace ParseEngine
{

namespace
{

/**
 * @brief 渲染一段「当值用」的模板（收掉结尾换行）。
 *
 * 模板文件按仓库惯例以换行收尾，而这里的产物是要**插进别的产物里当值**的
 * （比如一个数组字面量），多带一个换行就是产物里多出来的一行，所以收掉。
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

/// 整数表里的一项：引擎没有取模 / "非首项"判断，分隔符在 C++ 里算好。
struct CsIntItem
{
    std::string sep;     ///< 前导分隔符（`""` / `", "` / `",\n<缩进>"`）
    long long value = 0; ///< 数值
};

/// 锯齿数组的一行（`_ACTION` / `_GOTO` 这类二维表）。
struct CsIntRow
{
    std::string indent; ///< `new int[]` 前面的缩进
    std::string cells;  ///< 该行的数组字面量（`{1, 2, 3}`）
};

/** 生成 C# 字符串字面量（控制字符转成 \u 转义）。 */
std::string csString(const std::string &text)
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

/** 字面量在 C# 里占几个 char（UTF-16 代码单元）。 */
std::size_t csCharCount(const std::string &text)
{
    std::size_t count = 0u;
    for (std::size_t index = 0u; index < text.size();)
    {
        const unsigned char ch = static_cast<unsigned char>(text[index]);
        std::size_t width = 1u;
        if ((ch & 0x80u) == 0u)
        {
            width = 1u;
        }
        else if ((ch & 0xE0u) == 0xC0u)
        {
            width = 2u;
        }
        else if ((ch & 0xF0u) == 0xE0u)
        {
            width = 3u;
        }
        else
        {
            width = 4u;
        }
        index += width;
        count += (width == 4u) ? 2u : 1u;
    }
    return count;
}

/// 一维整数表 → C# 数组字面量（每 perLine 个换行 + indent）。
std::string csIntArray(const std::vector<long long> &values,
                       const std::string &indent, std::size_t perLine)
{
    std::vector<CsIntItem> items;
    items.reserve(values.size());
    for (std::size_t index = 0u; index < values.size(); ++index)
    {
        CsIntItem item;
        item.sep = (index == 0u)
                       ? std::string()
                       : (((index % perLine) == 0u) ? (",\n" + indent) : ", ");
        item.value = values[index];
        items.push_back(item);
    }
    return renderValueTemplate("csharp_int_array.txt",
                               {{"values", tmpl::encode(items)}});
}

/// 二维整数表（锯齿数组字面量）。
std::string csIntRows(const std::vector<std::vector<long long>> &rows,
                      const std::string &indent)
{
    // C# 的锯齿数组初始化器里，每个元素都要显式 new（`{{1,2}}` 不合法）
    std::vector<CsIntRow> lines;
    lines.reserve(rows.size());
    for (const std::vector<long long> &row : rows)
    {
        CsIntRow item;
        item.indent = indent + "    ";
        item.cells = csIntArray(row, indent + "     ", 12u);
        lines.push_back(item);
    }
    return renderValueTemplate("csharp_int_rows.txt",
                               {{"rows", tmpl::encode(lines)},
                                {"indent", indent}});
}

/// 非终结符到编号的映射。
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

/// 语义值类型信息。
struct CsValueInfo
{
    std::string typeText;  ///< 填进模板 `{{ value_type }}` 的文本
    bool isUnion = false;  ///< 语义值是带成员字段的 `ParserValue` 类
    bool isObject = false; ///< 缺省 `object`
};

/// Token 常量声明（Token.cs 的 `TOKEN_*` 表）。
struct CsTokenDecl
{
    std::string name;   ///< 终端符名（拼进 `TOKEN_<名>`）
    long long id = 0;   ///< 编号
    std::string quoted; ///< 名字的 C# 字面量（名称表用）
};

/// Token 别名项（`TOKEN_ALIASES` 表）。
struct CsAlias
{
    std::string key;   ///< 别名键（C# 字面量）
    std::string value; ///< 别名值（C# 字面量）
};

/// LL 的产生式展开分支（每个产生式一条）。
struct CsExpandCase
{
    long long id = 0;   ///< 产生式编号
    std::string pushes; ///< 这一段要做的 `if (!_llPush…)` 串（不含结尾换行）
};

/// `_applyAction` 的一个分支（每个产生式一条）。
struct CsActionCase
{
    long long id = 0;     ///< 产生式编号
    std::string keyword;  ///< `if (production == ` / `else if (production == `
    bool hasLine = false; ///< 有没有那两条 `// 在 …` / `// 继续 …`
    long long line = 0;   ///< 文法行号
    std::string body;     ///< 动作体（多行，内部已按 12 空格缩进）
};

/// `%token regex` 的一行（3 张平行表共用）。
struct CsRegexRule
{
    std::string pattern; ///< `new Regex(...)` 里的模式（C# 字面量）
    long long type = 0;  ///< Token 编号
    long long order = 0; ///< 词法声明序
};

/// `%commit` / `%trivia` 的一行。
struct CsCommitRule
{
    std::string pattern; ///< 模式（C# 字面量）
    std::string type;    ///< 片段类型名（C# 字面量）
    std::string skip;    ///< `"true"` / `"false"`
    long long order = 0; ///< 声明序
};

/// `%commit` 的显示别名。
struct CsCommitAlias
{
    std::string type;  ///< 类型名（C# 字面量）
    std::string alias; ///< 别名（C# 字面量）
};

/// 固定字面量词法规则表里的一行（5 张平行表共用一份数据）。
struct CsLexRule
{
    std::string text;   ///< 字面量（C# 字面量）
    long long type = 0; ///< Token 编号
    long long length = 0; ///< 字面量占几个 C# `char`
    std::string blank;  ///< `"1"` / `"0"`：能不能以空白开头
    long long order = 0; ///< 词法声明序
};

/// `_keywordType` 的一条关键字分支。
struct CsKeyword
{
    std::string literal; ///< 关键字字面量（C# 字面量）
    long long id = 0;    ///< 对应的 Token 编号
};

/// `_valueFromText` 的 `switch` 分支（`%union` 时一个可装字符串的成员一条）。
struct CsTextCase
{
    long long id = 0;   ///< Token 编号（`case` 标签）
    std::string member; ///< `%union` 成员名
};

/// 取语义值类型信息。
CsValueInfo csValueInfo(const ProjectRequest &request)
{
    CsValueInfo info;
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
    info.typeText = "object";
    info.isObject = true;
    return info;
}

/// 取 `%union` 成员的类型原文。
std::string unionMemberType(const UnionMember &member)
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

/// 取符号绑定的 `%union` 成员（名字 + 类型）。
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
            outType = unionMemberType(item);
            return true;
        }
    }
    return false;
}

/// 这个 C# 类型能不能直接装下字符串。
bool csTextCarrier(const std::string &type)
{
    return (type == "string") || (type == "String") || (type == "object") ||
           (type == "Object");
}

/// 这个 C# 类型是不是能装下数值的值类型。
bool csNumericType(const std::string &type)
{
    return (type == "sbyte") || (type == "byte") || (type == "short") ||
           (type == "ushort") || (type == "int") || (type == "uint") ||
           (type == "long") || (type == "ulong") || (type == "char") ||
           (type == "float") || (type == "double") || (type == "decimal");
}

/// 生成 `_newValue` / `_valueFromText` / `_valueFromNumber` 三个静态方法。
void csValueHelpers(const ProjectRequest &request,
                    const std::vector<std::string> &terminals,
                    std::string &out)
{
    const CsValueInfo info = csValueInfo(request);
    const std::string type = info.typeText;

    // %union：能装字符串的成员各来一条 `case`（词素直接塞进对应字段）
    std::vector<CsTextCase> textCases;
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
            if (!csTextCarrier(memberType))
            {
                continue;
            }
            const auto found = std::find(terminals.begin(),
                                         terminals.end(), entry.first);
            if (found == terminals.end())
            {
                continue;
            }
            CsTextCase item;
            item.id = static_cast<long long>(found - terminals.begin());
            item.member = member;
            textCases.push_back(item);
        }
    }

    std::string numberMember;
    std::string numberMemberType;
    const bool hasNumberMember =
        symbolMemberType(request, "NUMBER", numberMember, numberMemberType) &&
        csNumericType(numberMemberType);

    // 三个"怎么造值"的表达式：语言分支留在 C++，产物里的措辞交给模板
    const std::string newValueExpr =
        info.isUnion ? "new ParserValue()" : ("default(" + type + ")");

    std::string fromTextExpr;
    if (csTextCarrier(type))
    {
        fromTextExpr = "text";
    }
    else if (type == "char")
    {
        fromTextExpr = "(text.Length == 0) ? (char) 0 : text[0]";
    }
    else if (csNumericType(type))
    {
        fromTextExpr = "(" + type + ") 0";
    }
    else
    {
        fromTextExpr = "default(" + type + ")";
    }

    std::string fromNumberExpr;
    if (csNumericType(type))
    {
        fromNumberExpr = "(" + type + ") number";
    }
    else if (type == "object")
    {
        fromNumberExpr = "isFloat ? (object) number\n"
                         "                       : (object) (int) number";
    }
    else if (csTextCarrier(type))
    {
        fromNumberExpr =
            "number.ToString(System.Globalization.CultureInfo.InvariantCulture)";
    }
    else
    {
        fromNumberExpr = "default(" + type + ")";
    }

    out += tmpl::renderValues(
        Utils::loadTemplate("csharp_value_helpers.cs"),
        {{"value_type", type},
         {"new_value_expr", newValueExpr},
         {"is_union", info.isUnion},
         {"text_cases", tmpl::encode(textCases)},
         {"from_text_expr", fromTextExpr},
         {"has_number_member", hasNumberMember},
         {"number_member", numberMember},
         {"number_member_type", numberMemberType},
         {"from_number_expr", fromNumberExpr}});
}

/// ACTION / GOTO / 产生式元数据（LR 家族共用）。
std::string csLrTables(const AnalysisResult &analysis)
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
    std::string reduce;
    std::string lookaheadCode;
    std::string lookaheadColumn;
    const bool hasLookahead = analysis.lookaheadK > 1;
    if (hasLookahead)
    {
        const std::size_t columnCount = analysis.lookaheadColumns.size();
        std::map<std::string, int> terminalIds;
        for (std::size_t index = 0u; index < analysis.terminals.size(); ++index)
        {
            terminalIds[analysis.terminals[index]] = static_cast<int>(index);
        }
        std::vector<std::vector<long long>> reduces;
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
        std::vector<long long> codeList;
        std::vector<long long> columnList;
        for (const auto &entry : codes)
        {
            codeList.push_back(entry.first);
            columnList.push_back(static_cast<long long>(entry.second));
        }
        reduce = csIntRows(reduces, "    ");
        lookaheadCode = csIntArray(codeList, "        ", 12u);
        lookaheadColumn = csIntArray(columnList, "        ", 16u);
    }
    return tmpl::renderValues(
        Utils::loadTemplate("csharp_lr_tables.cs"),
        {{"action", csIntRows(actions, "    ")},
         {"goto_table", csIntRows(gotos, "    ")},
         {"has_lookahead", hasLookahead},
         {"reduce", reduce},
         {"lookahead_code", lookaheadCode},
         {"lookahead_column", lookaheadColumn},
         {"production_length", csIntArray(lengths, "        ", 16u)},
         {"production_lhs", csIntArray(lhs, "        ", 16u)}});
}

/// GLR 冲突候选与图表数据：ACTION 主动作仍由 LR 表提供。
std::string csGlrTables(const AnalysisResult &analysis)
{
    const std::size_t termCount = analysis.terminals.size();
    const std::size_t cellCount = analysis.stateCount() * termCount;
    std::map<std::string, std::size_t> termIds;
    for (std::size_t i = 0; i < termCount; ++i)
    {
        termIds[analysis.terminals[i]] = i;
    }
    std::vector<long long> starts(cellCount, 0), counts(cellCount, 0), data;
    for (const auto &entry : analysis.glrAlternatives)
    {
        const auto column = termIds.find(entry.first.second);
        if (column == termIds.end()) continue;
        const std::size_t cell = static_cast<std::size_t>(entry.first.first) *
                                 termCount + column->second;
        if (cell >= cellCount || entry.second.size() < 2u) continue;
        starts[cell] = static_cast<long long>(data.size());
        for (std::size_t i = 1; i < entry.second.size(); ++i)
        {
            const LrAction &action = entry.second[i];
            data.push_back(action.kind == LrAction::Kind::Shift
                               ? action.value + 1
                               : (action.kind == LrAction::Kind::Reduce
                                      ? -action.value - 1 : 2147483647));
            ++counts[cell];
        }
    }
    std::string startSymbol = analysis.grammar.start;
    const auto startRules = analysis.grammar.rules.find(startSymbol);
    if (startRules != analysis.grammar.rules.end() &&
        !startRules->second.empty() && !startRules->second.front().rhs.empty())
    {
        startSymbol = startRules->second.front().rhs.front();
    }
    const auto nonterm = std::find(analysis.nonterms.begin(),
                                   analysis.nonterms.end(), startSymbol);
    const long long startId = static_cast<long long>(termCount +
        static_cast<std::size_t>(nonterm - analysis.nonterms.begin()));
    return tmpl::renderValues(Utils::loadTemplate("csharp_glr_tables.cs"),
                              {{"start_symbol", startId},
                               {"extra_start", csIntArray(starts, "        ", 16u)},
                               {"extra_count", csIntArray(counts, "        ", 16u)},
                               {"extra_data", csIntArray(data, "        ", 16u)}});
}

std::string csChartTables(const AnalysisResult &analysis)
{
    const ChartGrammar chart = buildChartGrammar(analysis);
    const auto array = [](const std::vector<int> &values)
    {
        return csIntArray(std::vector<long long>(values.begin(), values.end()),
                          "        ", 16u);
    };
    std::vector<int> unaryLhs, unaryToken, binaryLhs, binaryLeft, binaryRight;
    for (const auto &rule : chart.terminalRules)
    {
        unaryLhs.push_back(rule[0]); unaryToken.push_back(rule[1]);
    }
    for (const auto &rule : chart.binaryRules)
    {
        binaryLhs.push_back(rule[0]); binaryLeft.push_back(rule[1]);
        binaryRight.push_back(rule[2]);
    }
    return tmpl::renderValues(Utils::loadTemplate("csharp_chart_tables.cs"),
        {{"start", chart.startNonterm},
         {"production_count", static_cast<long long>(chart.lhs.size())},
         {"accepts_empty", chart.acceptsEmpty},
         {"lhs", array(chart.lhs)}, {"rhs_start", array(chart.rhsStart)},
         {"rhs_length", array(chart.rhsLength)},
         {"rhs_symbols", array(chart.rhsSymbols)},
         {"unary_lhs", array(unaryLhs)}, {"unary_token", array(unaryToken)},
         {"binary_lhs", array(binaryLhs)}, {"binary_left", array(binaryLeft)},
         {"binary_right", array(binaryRight)}});
}

/// 预测分析表与左递归空尾标记表（LL）。
std::string csLlTables(const AnalysisResult &analysis)
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
    std::string lookaheadCode;
    std::string lookaheadColumn;
    std::string llFollow;
    const bool hasLookahead = (k >= 2);
    if (hasLookahead)
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
        std::vector<long long> codeList;
        std::vector<long long> columnList;
        for (const auto &entry : codes)
        {
            codeList.push_back(entry.first);
            columnList.push_back(static_cast<long long>(entry.second));
        }
        lookaheadCode = csIntArray(codeList, "        ", 12u);
        lookaheadColumn = csIntArray(columnList, "        ", 16u);
    }
    const bool hasFollow = (k != 0);
    if (hasFollow)
    {
        std::vector<std::vector<long long>> followRows;
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
        llFollow = csIntRows(followRows, "    ");
    }
    return tmpl::renderValues(
        Utils::loadTemplate("csharp_ll_tables.cs"),
        {{"ll_table", csIntRows(table, "    ")},
         {"has_lookahead", hasLookahead},
         {"lookahead_code", lookaheadCode},
         {"lookahead_column", lookaheadColumn},
         {"production_is_left_empty", csIntArray(leftEmpty, "        ", 16u)},
         {"has_follow", hasFollow},
         {"ll_follow", llFollow}});
}

/// 语法树元数据：产生式右部符号表。
std::string csTreeTables(const AnalysisResult &analysis)
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
        Utils::loadTemplate("csharp_tree_tables.cs"),
        {{"tree_prod_start", csIntArray(starts, "        ", 16u)},
         {"tree_prod_length", csIntArray(lengths, "        ", 16u)},
         {"tree_prod_symbols",
          csIntArray(symbols.empty() ? std::vector<long long>{0} : symbols,
                     "        ", 16u)}});
}

/// 递归下降的表：非终结符 -> 产生式区间 + 语义槽位个数。
///
/// 产生式在 `analysis.productions` 里「同一左部连续排在一起」（摊平时按非终结符
/// 名有序、规则内按声明顺序），所以一个非终结符对应一段连续编号——回溯时依次试
/// 的就是这个顺序。右部符号不在这里重发，直接查 `_TREE_PROD_*`。
std::string csDdTables(const AnalysisResult &analysis)
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
        Utils::loadTemplate("csharp_dd_tables.cs"),
        {{"prod_start", csIntArray(starts, "        ", 16u)},
         {"prod_count", csIntArray(counts, "        ", 16u)},
         {"prod_slots", csIntArray(slots, "        ", 16u)}});
}

/// 取 Token 名在终结符表里的下标。
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

/// 固定字面量词法规则表（按长度降序）。
std::string csLexRules(
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
            rule.length = csCharCount(literal);
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
    std::vector<CsLexRule> rows;
    rows.reserve(rules.size());
    for (const Rule &rule : rules)
    {
        CsLexRule row;
        row.text = csString(rule.literal);
        row.type =
            static_cast<long long>(terminalIndex(terminals, rule.tokenName));
        row.length = static_cast<long long>(rule.length);
        row.blank = literalCanStartBlank(rule.literal) ? "1" : "0";
        row.order = static_cast<long long>(rule.declOrder);
        rows.push_back(row);
    }
    return tmpl::renderValues(Utils::loadTemplate("csharp_lex_rules.cs"),
                              {{"rules", tmpl::encode(rows)}});
}

/// 取 Token 前跳过的空白字符集（`%space`）。
std::string csSkipChars(const std::string *spaceChars)
{
    const std::string &chars =
        (spaceChars == nullptr) ? blankChars() : *spaceChars;
    return tmpl::renderValues(Utils::loadTemplate("csharp_lex_skip_chars.cs"),
                              {{"skip_chars", csString(chars)}});
}

/// 关键字表。
void csKeywordMethod(
    const std::map<std::string, std::vector<std::string>> &tokenDefs,
    const std::vector<std::string> &terminals, std::string &out)
{
    std::vector<CsKeyword> keywords;
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
            CsKeyword item;
            item.literal = csString(literal);
            item.id =
                static_cast<long long>(terminalIndex(terminals, entry.first));
            keywords.push_back(item);
        }
    }
    out += tmpl::renderValues(Utils::loadTemplate("csharp_keyword_method.cs"),
                              {{"keywords", tmpl::encode(keywords)}});
}

/// `%token regex` / `%identifier` / `%commit` 的正则表（.NET 自带的 Regex）。
///
/// 产物里不再放 DFA 表：词法匹配交给 `System.Text.RegularExpressions.Regex`。
/// 生成期用自带编译器做的那一遍校验在文法加载时（`readRegexPattern`）就做过了，
/// 认不下的由 `main.cpp` 分流：C99 是硬错误、其余档只警告（自带编译器不认识
/// 不等于 .NET 的 `Regex` 不认，见 `GrammarConfig::regexDiagnostics`）。
void csLexRegexes(const std::map<std::string, std::string> &regexDefs,
                  const std::optional<std::string> &identifierPattern,
                  const std::vector<CommitSpec> &commits,
                  const std::map<std::string, int> &tokenLexOrder,
                  const std::vector<std::string> &terminals, std::string &out)
{
    const bool hasIdentifier = identifierPattern.has_value();
    // 这里不再自己校验正则：文法加载时（readRegexPattern）已经用自带编译器验过
    // 一遍，认不下的由 main.cpp 按目标语言分流——C99 是硬错误（它要编 DFA 表），
    // 其余档打警告后照常生成（见 GrammarConfig::regexDiagnostics）。
    std::vector<CsRegexRule> regexRows;
    regexRows.reserve(regexDefs.size());
    {
        int fallbackOrder = 0;
        for (const auto &entry : regexDefs)
        {
            const auto orderIt = tokenLexOrder.find(entry.first);
            CsRegexRule row;
            row.pattern = csString(entry.second);
            row.type =
                static_cast<long long>(terminalIndex(terminals, entry.first));
            row.order = static_cast<long long>(
                (orderIt != tokenLexOrder.end()) ? orderIt->second
                                                 : fallbackOrder);
            regexRows.push_back(row);
            ++fallbackOrder;
        }
    }
    std::vector<CsCommitRule> commitRows;
    commitRows.reserve(commits.size());
    std::vector<CsCommitAlias> aliasRows;
    for (const CommitSpec &commit : commits)
    {
        CsCommitRule row;
        row.pattern = csString(commit.pattern);
        row.type = csString(commit.type);
        row.skip = commit.isSkip ? "true" : "false";
        row.order = static_cast<long long>(commit.order);
        commitRows.push_back(row);
        if (!commit.alias.empty())
        {
            CsCommitAlias alias;
            alias.type = csString(commit.type);
            alias.alias = csString(commit.alias);
            aliasRows.push_back(alias);
        }
    }
    out += tmpl::renderValues(
        Utils::loadTemplate("csharp_lex_regexes.cs"),
        {{"regexes", tmpl::encode(regexRows)},
         {"identifier_re", hasIdentifier ? ("new Regex(" + csString(*identifierPattern) + ")")
                                         : std::string("null")},
         {"commits", tmpl::encode(commitRows)},
         {"commit_aliases", tmpl::encode(aliasRows)}});
}

/// 同步 Token / error 伪 Token / 非终结符名表等杂项表。
std::string csMiscTables(const ProjectRequest &request,
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
    std::vector<std::string> nonterms;
    nonterms.reserve(analysis.nonterms.size());
    for (const std::string &nonterm : analysis.nonterms)
    {
        nonterms.push_back(csString(nonterm));
    }
    return tmpl::renderValues(
        Utils::loadTemplate("csharp_misc_tables.cs"),
        {{"sync_tokens", csIntArray(syncTypes, "        ", 16u)},
         {"has_error_token", std::string(request.usesError ? "true" : "false")},
         {"error_token_id", static_cast<long long>(errorTokenId)},
         {"nonterms", tmpl::encode(nonterms)}});
}

/// 组装语义值类型定义（`%union` 时生成 ParserValue 类）。
std::string csValueTypeDecl(const ProjectRequest &request)
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
        members.push_back(member.cType);
    }
    return tmpl::renderValues(Utils::loadTemplate("csharp_value_type_decl.cs"),
                              {{"members", tmpl::encode(members)}});
}

/// 容量常量段。
std::string csConstants(const AnalysisResult &analysis,
                        const std::string &mode)
{
    const std::map<std::string, int> ids = nontermIds(analysis.nonterms);
    const auto startFound = ids.find(analysis.grammar.start);
    std::map<std::string, int> expectedIds;
    for (std::size_t index = 0u; index < analysis.terminals.size(); ++index)
    {
        expectedIds[analysis.terminals[index]] = static_cast<int>(index);
    }
    std::vector<std::string> expectedRows;
    for (const std::vector<std::string> &row : expectedTokensByRow(analysis))
    {
        std::vector<long long> rowIds;
        rowIds.reserve(row.size());
        for (const std::string &token : row)
        {
            rowIds.push_back(static_cast<long long>(expectedIds[token]));
        }
        expectedRows.push_back(csIntArray(rowIds, "         ", 16u));
    }
    return tmpl::renderValues(
        Utils::loadTemplate("csharp_constants.cs"),
        {{"terminal_count",
          static_cast<long long>(analysis.terminals.size())},
         {"nonterm_count", static_cast<long long>(analysis.nonterms.size())},
         {"start_id", static_cast<long long>(
                          (startFound == ids.end()) ? 0 : startFound->second)},
         {"state_count",
          static_cast<long long>(isLrMode(mode) ? analysis.stateCount() : 0u)},
         {"lookahead_k", static_cast<long long>(std::max(0, analysis.lookaheadK))},
         {"lookahead_column_count",
          static_cast<long long>(analysis.lookaheadColumns.size())},
         {"expected_rows", tmpl::encode(expectedRows)},
         {"expected_fallback", csString(expectedFallbackText(analysis.mode))}});
}

/// Token 类型段（常量 + 名字表），进 Token.cs。
std::string csTokenTypes(const std::vector<std::string> &terminals,
                         const std::map<std::string, std::string> &aliases = {})
{
    std::vector<CsTokenDecl> tokens;
    tokens.reserve(terminals.size());
    for (std::size_t index = 0u; index < terminals.size(); ++index)
    {
        CsTokenDecl decl;
        decl.name = terminals[index];
        decl.id = static_cast<long long>(index);
        decl.quoted = csString(terminals[index]);
        tokens.push_back(decl);
    }
    std::vector<CsAlias> aliasList;
    aliasList.reserve(aliases.size());
    for (const auto &entry : aliases)
    {
        CsAlias item;
        item.key = csString(entry.first);
        item.value = csString(entry.second);
        aliasList.push_back(item);
    }
    return tmpl::renderValues(
        Utils::loadTemplate("csharp_token_types.cs"),
        {{"tokens", tmpl::encode(tokens)},
         {"token_count", static_cast<long long>(terminals.size())},
         {"aliases", tmpl::encode(aliasList)}});
}

/// 语义动作分支（LR / LL 共用一套 `_applyAction`）。
std::string csActionCases(const AnalysisResult &analysis,
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
    std::vector<CsActionCase> cases;
    for (const Production &production : analysis.productions)
    {
        if (production.transform == "left_empty")
        {
            continue;
        }
        const std::string action = trim(production.action);
        std::string body;
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
                                 /*csharpStyle=*/true))
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
                std::string line = trim(translated.substr(
                    pos, (found == std::string::npos) ? std::string::npos
                                                      : found - pos));
                if (!line.empty())
                {
                    if (!body.empty())
                    {
                        body += "\n            ";
                    }
                    body += line;
                }
                if (found == std::string::npos)
                {
                    break;
                }
                pos = found + 1u;
            }
        }
        CsActionCase item;
        item.id = static_cast<long long>(production.id);
        item.keyword =
            cases.empty() ? "if (production == " : "else if (production == ";
        item.hasLine = !body.empty() && (production.actionLine != 0u);
        item.line = static_cast<long long>(production.actionLine);
        item.body = body;
        cases.push_back(item);
    }
    return tmpl::renderValues(Utils::loadTemplate("csharp_action_cases.cs"),
                              {{"cases", tmpl::encode(cases)},
                               {"file", fileName}});
}

/// 产生式展开分支（仅 LL）。
std::string csExpandCases(const AnalysisResult &analysis,
                          const std::vector<std::string> &terminals)
{
    const std::set<std::string> terminalSet(terminals.begin(),
                                            terminals.end());
    const std::map<std::string, int> ids = nontermIds(analysis.nonterms);
    const auto pushCall = [](const std::string &kind, const std::string &symbol,
                             int slot)
    {
        return tmpl::renderValues(Utils::loadTemplate("csharp_ll_push.txt"),
                                  {{"kind", kind},
                                   {"symbol", symbol},
                                   {"slot", static_cast<long long>(slot)}});
    };
    const auto pushSymbol = [&terminalSet, &terminals, &ids,
                             &pushCall](const std::string &symbol, int slot)
    {
        if (terminalSet.count(symbol) != 0u)
        {
            const auto found =
                std::find(terminals.begin(), terminals.end(), symbol);
            return pushCall("_ITEM_TERMINAL",
                            std::to_string(found - terminals.begin()), slot);
        }
        return pushCall("_ITEM_NONTERM", std::to_string(ids.at(symbol)), slot);
    };
    std::vector<CsExpandCase> cases;
    for (const Production &production : analysis.productions)
    {
        const std::vector<std::string> &rhs = production.rhs;
        const std::string &transform = production.transform;
        std::string pushes;
        if (transform == "left_base")
        {
            const int baseLength = production.baseRhsLength;
            pushes += pushCall("_ITEM_LEFT_LOOP",
                               std::to_string(ids.at(rhs.back())), 0);
            pushes += pushCall("_ITEM_ACTION_KEEP",
                               std::to_string(production.id), 0);
            for (std::size_t index = static_cast<std::size_t>(baseLength);
                 index-- > 0u;)
            {
                pushes += pushSymbol(rhs[index], 1 + static_cast<int>(index));
            }
        }
        else if (transform == "left_continue")
        {
            pushes += pushCall("_ITEM_LEFT_LOOP",
                               std::to_string(ids.at(rhs.back())), 0);
            pushes += pushCall("_ITEM_ACTION_KEEP",
                               std::to_string(production.id), 0);
            for (std::size_t index = rhs.size() - 1u; index-- > 0u;)
            {
                pushes += pushSymbol(rhs[index], 2 + static_cast<int>(index));
            }
        }
        else if (transform == "left_empty")
        {
            pushes += "            // 空尾规则由 LeftLoop 直接完成\n";
        }
        else
        {
            pushes += pushCall("_ITEM_ACTION", std::to_string(production.id),
                               0);
            for (std::size_t index = rhs.size(); index-- > 0u;)
            {
                pushes += pushSymbol(rhs[index], 1 + static_cast<int>(index));
            }
        }
        // 收掉结尾换行：模板里这一串独占一行，那个换行由模板补
        while (!pushes.empty() && (pushes.back() == '\n'))
        {
            pushes.pop_back();
        }
        CsExpandCase item;
        item.id = static_cast<long long>(production.id);
        item.pushes = pushes;
        cases.push_back(item);
    }
    return tmpl::renderValues(Utils::loadTemplate("csharp_expand_cases.cs"),
                              {{"cases", tmpl::encode(cases)}});
}

/// 模式入口 `_runLoop`。
std::string csRunLoop(const std::string &mode)
{
    const std::string target = isDdMode(mode)
                                   ? "_runDdLoop"
                                   : (isGlrMode(mode) ? "_runGlrLoop"
                                                     : (isLrMode(mode) ? "_runLrLoop"
                                                     : "_runLlLoop"));
    return tmpl::renderValues(Utils::loadTemplate("csharp_run_loop.cs"),
                              {{"target", target}});
}

/// `using` 指令（`%import` 的展开 + 运行时要用的 System.Collections.Generic）。
/// @param request 组装入参。
/// @param withRegex 产物里有没有词法器（`Parser.cs` 才有）：有的话补一条
///        `System.Text.RegularExpressions`（`%token regex` / `%identifier` /
///        `%commit` 的正则用它）。
std::string csImportLines(const ProjectRequest &request, bool withRegex)
{
    std::string out;
    bool hasGeneric = false;
    bool hasRegex = false;
    if (request.imports != nullptr)
    {
        for (const std::string &name : *request.imports)
        {
            if (name == "System.Collections.Generic")
            {
                hasGeneric = true;
            }
            if (name == "System.Text.RegularExpressions")
            {
                hasRegex = true;
            }
            out += tmpl::renderValues(
                Utils::loadTemplate("csharp_import_line.txt"),
                {{"name", name}});
        }
    }
    if (!hasGeneric)
    {
        out += tmpl::renderValues(
            Utils::loadTemplate("csharp_import_line.txt"),
            {{"name", "System.Collections.Generic"}});
    }
    if (withRegex && !hasRegex)
    {
        out += tmpl::renderValues(
            Utils::loadTemplate("csharp_import_line.txt"),
            {{"name", "System.Text.RegularExpressions"}});
    }
    return out;
}

/// 文件级 `namespace NAME;`（没写 `%package` 时是空串）。
std::string csNamespaceLine(const ProjectRequest &request)
{
    if (request.packageName.empty())
    {
        return std::string();
    }
    return "namespace " + request.packageName + ";\n";
}

/// 词法器各段的替换表。
struct CsLexerBlocks
{
    std::string stringBlock;
    std::string matchBlock;
    std::string numberBlock;
    std::string identifierBlock;
};

/// 组装词法器各段。
void buildCsLexerBlocks(const ProjectRequest &request, CsLexerBlocks &blocks)
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
    if (!overridden("STRING"))
    {
        blocks.stringBlock = Utils::loadTemplate("csharp_lexer_string.cs");
    }
    std::string match = Utils::loadTemplate("csharp_lexer_match.cs");
    std::string regexLoop;
    if (!lexSpec.regex.empty())
    {
        regexLoop = Utils::loadTemplate("csharp_lexer_regex_loop.cs");
    }
    blocks.matchBlock =
        tmpl::renderValues(match, {{"lex_regex_loop", regexLoop}});
    if (!overridden("NUMBER"))
    {
        blocks.numberBlock = Utils::loadTemplate("csharp_lexer_number.cs");
    }
    if (lexSpec.identifier.has_value())
    {
        blocks.identifierBlock =
            Utils::loadTemplate("csharp_lexer_identifier_dfa.cs");
    }
    else if (!overridden("VARIABLE"))
    {
        blocks.identifierBlock =
            Utils::loadTemplate("csharp_lexer_identifier.cs");
    }
}

/// 词法表段（固定字面量规则 + %space + 正则表）。
///
/// 这一段没有失败路径：正则认不下只是在 `csLexRegexes` 里打警告。
void csLexTables(const ProjectRequest &request,
                 const std::vector<std::string> &terminals, std::string &out)
{
    const std::map<std::string, std::vector<std::string>> emptyTokenDefs;
    const auto &tokenDefs =
        (request.tokenDefs == nullptr) ? emptyTokenDefs : *request.tokenDefs;
    const LexSpec emptyLex;
    const LexSpec &lexSpec =
        (request.lexSpec == nullptr) ? emptyLex : *request.lexSpec;
    out += csLexRules(tokenDefs, terminals, lexSpec.tokenLexOrder);
    out += csSkipChars(request.spaceChars);
    csLexRegexes(lexSpec.regex, lexSpec.identifier, lexSpec.commits,
                 lexSpec.tokenLexOrder, terminals, out);
}

/// 组装词法器（解析与词法模式共用同一份模板）。
std::string csLexerText(const ProjectRequest &request)
{
    CsLexerBlocks blocks;
    buildCsLexerBlocks(request, blocks);
    return tmpl::renderValues(
        Utils::loadTemplate("csharp_lexer.cs"),
        {{"lex_string_block", blocks.stringBlock},
         {"lex_match_block", blocks.matchBlock},
         {"lex_number_block", blocks.numberBlock},
         {"lex_identifier_block", blocks.identifierBlock}});
}

/// 取 `%code … %endcode` 的正文（带前后行标记）。
std::string csUserCode(const ProjectRequest &request,
                       const std::string &fileName)
{
    std::string out;
    if ((request.lines != nullptr) &&
        (request.lines->userBlocks != nullptr) &&
        (request.lines->userBlockLines != nullptr) &&
        (request.lines->userBlocks->size() ==
         request.lines->userBlockLines->size()))
    {
        for (std::size_t index = 0u; index < request.lines->userBlocks->size();
             ++index)
        {
            const std::string &block = (*request.lines->userBlocks)[index];
            if (block.empty())
            {
                continue;
            }
            if (!out.empty())
            {
                out += "\n";
            }
            out += CodeGenCsharp::withLineMarkers(
                block, (*request.lines->userBlockLines)[index], "", fileName);
        }
        return out;
    }
    if (!request.userCode.empty())
    {
        out = request.userCode;
    }
    return out;
}

/// 组装 Parser.cs（解析模式）。
bool buildParserCs(const ProjectRequest &request, const std::string &mode,
                   const AnalysisResult &analysis, std::string &out,
                   Error &error)
{
    const CsValueInfo info = csValueInfo(request);
    const std::vector<std::string> &terminals = analysis.terminals;
    std::size_t maxSlots = 4u;
    for (const Production &production : analysis.productions)
    {
        maxSlots = std::max(maxSlots, production.rhs.size() + 3u);
    }
    std::string lexTables;
    csLexTables(request, terminals, lexTables);
    const bool isLr = isLrMode(mode);
    const bool isGlr = isGlrMode(mode);
    const bool chartMode = (mode == "earley") || (mode == "cyk");
    std::string analysisTables =
        isLr ? csLrTables(analysis) : csLlTables(analysis);
    if (isGlr)
    {
        analysisTables += csGlrTables(analysis);
        if (chartMode) analysisTables += csChartTables(analysis);
    }
    // 「非 LR 的表」这一格里，DD 还要多一份递归下降表（产生式区间 + 槽位个数）
    std::string treeTables =
        isLr ? std::string() : csTreeTables(analysis);
    if (isDdMode(mode))
    {
        treeTables += csDdTables(analysis);
    }
    const std::string miscTables = csMiscTables(request, analysis);

    std::string actionCases;
    if (isLrMode(mode))
    {
        actionCases = csActionCases(analysis, analysis.mode, request,
                                    "Parser.cs", error);
    }
    else if (isDdMode(mode))
    {
        // DD 的 values 与 LL 的语义帧同一个约定（第 0 个槽位留空），错误恢复
        // 那套 yyerrok / yyclearin 在递归下降里没有落点，所以传 "dd"
        actionCases =
            csActionCases(analysis, "dd", request, "Parser.cs", error);
    }
    else
    {
        actionCases =
            csActionCases(analysis, "ll1", request, "Parser.cs", error);
    }
    if (!error.empty())
    {
        return false;
    }
    std::string valueHelpers;
    csValueHelpers(request, terminals, valueHelpers);
    std::string keywordMethod;
    {
        const std::map<std::string, std::vector<std::string>> emptyTokenDefs;
        const LexSpec emptyLex;
        const auto &tokenDefs = (request.tokenDefs == nullptr)
                                    ? emptyTokenDefs
                                    : *request.tokenDefs;
        const LexSpec &lexSpec =
            (request.lexSpec == nullptr) ? emptyLex : *request.lexSpec;
        if (lexSpec.identifier.has_value())
        {
            csKeywordMethod(tokenDefs, terminals, keywordMethod);
        }
    }
    // 注意：`csharp_lr_action.cs` / `csharp_lr.cs` 里的 `{{ value_type }}` 以前是靠
    // 下面那道作用在整份 engine 上的全局替换兜底的（占位符替换会一路扫进插进来的
    // 片段里），换成模板引擎后值不会二次解析，必须在这两份渲染时就把值给齐。
    std::string lrText;
    std::string llText;
    std::string ddText;
    if (isLrMode(mode))
    {
        lrText = tmpl::renderValues(Utils::loadTemplate("csharp_lr_action.cs"),
                                    {{"lr_action_cases", actionCases},
                                     {"value_type", info.typeText}}) +
                 tmpl::renderValues(Utils::loadTemplate("csharp_lr.cs"),
                                    {{"value_type", info.typeText}});
    }
    else
    {
        lrText = tmpl::renderValues(Utils::loadTemplate("csharp_lr_action.cs"),
                                    {{"lr_action_cases", actionCases},
                                     {"value_type", info.typeText}});
        if (isDdMode(mode))
        {
            // DD：动作分支 + 带回溯的递归下降驱动（不建表，直接照文法走）
            ddText = tmpl::renderValues(Utils::loadTemplate("csharp_dd.cs"),
                                        {{"value_type", info.typeText}});
        }
        else
        {
            llText = tmpl::renderValues(
                Utils::loadTemplate("csharp_ll.cs"),
                {{"ll_expand_cases", csExpandCases(analysis, terminals)}});
        }
    }
    const std::string engine = tmpl::renderValues(
        Utils::loadTemplate("csharp_engine.cs"),
        {{"constants", csConstants(analysis, mode)},
         {"lex_tables", lexTables},
         {"analysis_tables", analysisTables},
         {"tree_tables", treeTables},
         {"glr", isGlr},
         {"is_ll", !isLr},
         {"misc_tables", miscTables},
         {"value_helpers", valueHelpers},
         {"keyword_method", keywordMethod},
         {"max_stack_depth",
          std::to_string(request.maxStackDepth.value_or(64))},
         {"max_errors", std::to_string(request.maxErrors.value_or(8))},
         {"error_recovery_tokens",
          std::to_string(request.errorRecoveryTokens.value_or(3))},
         {"value_slots", std::to_string(maxSlots)},
         {"run_loop", csRunLoop(mode)},
         {"lexer", csLexerText(request)},
         {"lr", lrText},
         {"glr_body", isGlr ? tmpl::renderValues(
              Utils::loadTemplate("csharp_glr.cs"),
              {{"value_type", info.typeText}, {"chart", chartMode},
               {"chart_body", chartMode ? tmpl::renderValues(
                   Utils::loadTemplate("csharp_chart.cs"),
                   {{"earley", mode == "earley"}}) : std::string()}})
                             : std::string()},
         {"ll", llText},
         {"dd", isDdMode(mode)},
         {"dd_body", ddText},
         {"value_type", info.typeText}});

    std::string classBody;
    if ((request.classBody != nullptr) && !request.classBody->empty())
    {
        classBody = renderValueTemplate(
            "csharp_class_body.txt",
            {{"class_body",
              CodeGenCsharp::withLineMarkers(*request.classBody,
                                             request.classBodyLine + 1u,
                                             "    ", "Parser.cs")}});
    }
    std::string fileLevel = csValueTypeDecl(request);
    const std::string userCode = csUserCode(request, "Parser.cs");
    if (!fileLevel.empty() && !userCode.empty())
    {
        fileLevel += "\n";
    }
    out = tmpl::renderValues(
        Utils::loadTemplate("csharp_parser.cs"),
        {{"mode_name", modeLabelText(mode)},
         {"imports", csImportLines(request, true)},
         {"package", csNamespaceLine(request)},
         {"user_code", fileLevel + userCode},
         {"engine", engine},
         {"class_body", classBody}});
    return true;
}

} // namespace

} // namespace ParseEngine

// 字段绑定只能写在全局作用域（`TEMPLATE_MODEL` 展开的是显式特化）
TEMPLATE_MODEL(ParseEngine::CsTokenDecl,
               tmpl::field("name", &ParseEngine::CsTokenDecl::name),
               tmpl::field("id", &ParseEngine::CsTokenDecl::id),
               tmpl::field("quoted", &ParseEngine::CsTokenDecl::quoted));
TEMPLATE_MODEL(ParseEngine::CsAlias,
               tmpl::field("key", &ParseEngine::CsAlias::key),
               tmpl::field("value", &ParseEngine::CsAlias::value));
TEMPLATE_MODEL(ParseEngine::CsIntItem,
               tmpl::field("sep", &ParseEngine::CsIntItem::sep),
               tmpl::field("value", &ParseEngine::CsIntItem::value));
TEMPLATE_MODEL(ParseEngine::CsIntRow,
               tmpl::field("indent", &ParseEngine::CsIntRow::indent),
               tmpl::field("cells", &ParseEngine::CsIntRow::cells));
TEMPLATE_MODEL(ParseEngine::CsTextCase,
               tmpl::field("id", &ParseEngine::CsTextCase::id),
               tmpl::field("member", &ParseEngine::CsTextCase::member));
TEMPLATE_MODEL(ParseEngine::CsKeyword,
               tmpl::field("literal", &ParseEngine::CsKeyword::literal),
               tmpl::field("id", &ParseEngine::CsKeyword::id));
TEMPLATE_MODEL(ParseEngine::CsLexRule,
               tmpl::field("text", &ParseEngine::CsLexRule::text),
               tmpl::field("type", &ParseEngine::CsLexRule::type),
               tmpl::field("length", &ParseEngine::CsLexRule::length),
               tmpl::field("blank", &ParseEngine::CsLexRule::blank),
               tmpl::field("order", &ParseEngine::CsLexRule::order));
TEMPLATE_MODEL(ParseEngine::CsRegexRule,
               tmpl::field("pattern", &ParseEngine::CsRegexRule::pattern),
               tmpl::field("type", &ParseEngine::CsRegexRule::type),
               tmpl::field("order", &ParseEngine::CsRegexRule::order));
TEMPLATE_MODEL(ParseEngine::CsCommitRule,
               tmpl::field("pattern", &ParseEngine::CsCommitRule::pattern),
               tmpl::field("type", &ParseEngine::CsCommitRule::type),
               tmpl::field("skip", &ParseEngine::CsCommitRule::skip),
               tmpl::field("order", &ParseEngine::CsCommitRule::order));
TEMPLATE_MODEL(ParseEngine::CsCommitAlias,
               tmpl::field("type", &ParseEngine::CsCommitAlias::type),
               tmpl::field("alias", &ParseEngine::CsCommitAlias::alias));
TEMPLATE_MODEL(ParseEngine::CsActionCase,
               tmpl::field("id", &ParseEngine::CsActionCase::id),
               tmpl::field("keyword", &ParseEngine::CsActionCase::keyword),
               tmpl::field("has_line", &ParseEngine::CsActionCase::hasLine),
               tmpl::field("line", &ParseEngine::CsActionCase::line),
               tmpl::field("body", &ParseEngine::CsActionCase::body));
TEMPLATE_MODEL(ParseEngine::CsExpandCase,
               tmpl::field("id", &ParseEngine::CsExpandCase::id),
               tmpl::field("pushes", &ParseEngine::CsExpandCase::pushes));

namespace ParseEngine
{

std::string CodeGenCsharp::grammarLineMarker(std::size_t lineNumber)
{
    if (lineNumber == 0u)
    {
        return std::string();
    }
    return "// 在 " + std::string(CodeGen::grammarFilePlaceholder()) +
           " 的第 " + std::to_string(lineNumber) + " 行\n";
}

std::string CodeGenCsharp::withLineMarkers(const std::string &body,
                                           std::size_t lineNumber,
                                           const std::string &indent,
                                           const std::string &fileName)
{
    std::string out;
    if (lineNumber != 0u)
    {
        out += indent + grammarLineMarker(lineNumber);
    }
    out += body;
    if (!out.empty() && (out.back() != '\n'))
    {
        out += "\n";
    }
    if (lineNumber != 0u)
    {
        out += indent + "// 继续 " + fileName + " 的正文\n";
    }
    return out;
}

bool CodeGenCsharp::parserCs(const ProjectRequest &request,
                             const std::string &mode,
                             const AnalysisResult &analysis, std::string &out,
                             Error &error)
{
    return buildParserCs(request, mode, analysis, out, error);
}

void CodeGenCsharp::tokenCs(const ProjectRequest &request,
                            const std::vector<std::string> &terminals,
                            std::string &out)
{
    const CsValueInfo info = csValueInfo(request);
    const auto emptyAliases = std::map<std::string, std::string>{};
    const auto &aliases = request.lexSpec ? request.lexSpec->tokenAliases : emptyAliases;
    // 这份模板里本来就没有 `__IMPORTS__`（Token.cs 只用标准库，不需要 using），
    // 原来那句替换一直是空转的死调用，顺手删掉。
    out = tmpl::renderValues(
        Utils::loadTemplate("csharp_token.cs"),
        {{"package", csNamespaceLine(request)},
         {"value_type", info.typeText},
         {"token_types", csTokenTypes(terminals, aliases)}});
}

/**
 * @brief 演示 main 里的 `%commit` + location 打印片段。
 * @param request 组装入参。
 * @return C# 代码段；文法里没写 %commit 时为空串（演示输出与从前一样）。
 */
std::string csDemoCommitPrint(const ProjectRequest &request)
{
    const LexSpec emptyLex;
    const LexSpec &lexSpec =
        (request.lexSpec == nullptr) ? emptyLex : *request.lexSpec;
    if (lexSpec.commits.empty())
    {
        return std::string();
    }
    // 整段固定的产物片段，没有可变部分：当静态模板取出来即可，不引模板引擎。
    return std::string(Utils::loadTemplate("csharp_demo_commit_print.cs"));
}

std::string CodeGenCsharp::mainCs(const ProjectRequest &request,
                                  const std::string &mode,
                                  const std::string &mainBody)
{
    std::string body;
    if (!mainBody.empty())
    {
        body = tmpl::renderValues(Utils::loadTemplate("csharp_user_main.txt"),
                                  {{"main_body", mainBody}});
    }
    else if (isGlrMode(mode))
    {
        body = tmpl::renderValues(
            Utils::loadTemplate("csharp_glr_demo_main.cs"),
            {{"tree", request.tree}, {"demo_commits", csDemoCommitPrint(request)}});
    }
    else
    {
        std::string print;
        if (request.tree)
        {
            print = "        Parser.Tree tree = engine.parseTree(inputText);\n"
                    "        if (tree != null)\n"
                    "        {\n"
                    "            System.Console.WriteLine(\"语法树:\");\n"
                    "            tree.print();\n"
                    "        }\n";
        }
        else
        {
            print = "        System.Console.WriteLine(\"解析结果: \" + "
                    "engine.result());\n";
        }
        body = tmpl::renderValues(
            Utils::loadTemplate("csharp_demo_main.cs"),
            {{"demo_print", print},
             {"dd", isDdMode(mode)},
             {"demo_commits", csDemoCommitPrint(request)}});
    }
    return tmpl::renderValues(
        Utils::loadTemplate("csharp_program.cs"),
        {{"mode_name", modeLabelText(mode)},
         {"imports", csImportLines(request, false)},
         {"package", csNamespaceLine(request)},
         {"main_body", body}});
}

std::string CodeGenCsharp::lexerMainCs(const ProjectRequest &request,
                                       const std::string &mainBody)
{
    std::string body;
    if (!mainBody.empty())
    {
        body = tmpl::renderValues(Utils::loadTemplate("csharp_user_main.txt"),
                                  {{"main_body", mainBody}});
    }
    else
    {
        body = Utils::loadTemplate("csharp_lex_demo_main.cs");
    }
    return tmpl::renderValues(
        Utils::loadTemplate("csharp_lex_program.cs"),
        {{"mode_name", "LEX(词法模式)"},
         {"imports", csImportLines(request, false)},
         {"package", csNamespaceLine(request)},
         {"main_body", body}});
}

void CodeGenCsharp::lexerCs(const ProjectRequest &request,
                            const std::vector<std::string> &terminals,
                            std::string &out)
{
    const CsValueInfo info = csValueInfo(request);
    std::string tables;
    csLexTables(request, terminals, tables);
    std::string keywordMethod;
    {
        const std::map<std::string, std::vector<std::string>> emptyTokenDefs;
        const LexSpec emptyLex;
        const auto &tokenDefs = (request.tokenDefs == nullptr)
                                    ? emptyTokenDefs
                                    : *request.tokenDefs;
        const LexSpec &lexSpec =
            (request.lexSpec == nullptr) ? emptyLex : *request.lexSpec;
        if (lexSpec.identifier.has_value())
        {
            csKeywordMethod(tokenDefs, terminals, keywordMethod);
        }
    }
    std::string valueHelpers;
    csValueHelpers(request, terminals, valueHelpers);
    const std::string constants = tmpl::renderValues(
        Utils::loadTemplate("csharp_lex_constants.cs"),
        {{"terminal_count", static_cast<long long>(terminals.size())}});
    const std::string engine = tmpl::renderValues(
        Utils::loadTemplate("csharp_lex_engine.cs"),
        {{"constants", constants},
         {"tables", tables},
         {"value_helpers", valueHelpers},
         {"keyword_method", keywordMethod},
         {"max_errors", std::to_string(request.maxErrors.value_or(8))},
         {"lexer", csLexerText(request)},
         {"value_type", info.typeText}});
    std::string classBody;
    if ((request.classBody != nullptr) && !request.classBody->empty())
    {
        classBody = renderValueTemplate(
            "csharp_class_body.txt",
            {{"class_body",
              CodeGenCsharp::withLineMarkers(*request.classBody,
                                             request.classBodyLine + 1u,
                                             "    ", "Parser.cs")}});
    }
    std::string fileLevel = csValueTypeDecl(request);
    const std::string userCode = csUserCode(request, "Parser.cs");
    if (!fileLevel.empty() && !userCode.empty())
    {
        fileLevel += "\n";
    }
    out = tmpl::renderValues(
        Utils::loadTemplate("csharp_parser.cs"),
        {{"mode_name", "LEX(词法模式)"},
         {"imports", csImportLines(request, true)},
         {"package", csNamespaceLine(request)},
         {"user_code", fileLevel + userCode},
         {"engine", engine},
         {"class_body", classBody}});
}

std::string CodeGenCsharp::csproj(const ProjectRequest &request)
{
    (void) request;
    // 这份产物没有任何**可变**部分：目标框架号 net8.0 直接写在模板里，随
    // 产物一起改。所以模板是纯静态文件，取出来原样返回即可，不需要渲染，
    // 也不需要给这个文件引入模板引擎依赖（cf. project.cpp）。
    return std::string(Utils::loadTemplate("csharp_csproj.xml"));
}

} // namespace ParseEngine
