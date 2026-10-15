/**
 * @file code_gen_rust.cpp
 * @brief Rust 语言后端实现：把生成期算好的表整理成**数据**，其余全部交给
 *        source/templates/rust_*.tmpl 里的模板引擎渲染。
 *
 * 与 C# 后端逐项对齐：
 *   - 生成期只换语法：`%package` -> crate 名（Cargo.toml）、`%import` -> `use`、
 *     `%class_body` -> 一段 `impl Parser { ... }`、类型名 PascalCase；
 *   - 语义值**按值拷贝**（`Copy + Default + Debug`），数组表用 `&[&[i32]]`；
 *   - 词法器把「Token 词素 -> 语义值」集中在生成出来的 `value_from_text` /
 *     `value_from_number` 两个函数里；
 *   - **没有 GLR、没有 `%ast_*`**（那两样已从本项目整体移除）。
 *
 * **本文件里没有 `replaceAll`**：产物长什么样完全由
 * `source/templates/rust_*.tmpl` 说了算，这里只负责「算表 / 算标志位 / 转义
 * 字符串字面量」这些**数据准备**。产物片段一律走模板渲染，C++ 里不再有
 * `out += "…"` 拼出来的文案（唯一一处 `out +=` 是 `rustString()` 这个**字符串
 * 字面量转义器**，它产出的是「值」而不是产物正文）。要改文案或排版，改模板。
 */

#include "ParseEngine/code_gen_rust.hpp"

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
#include "ParseEngine/code_gen.hpp"
#include "ParseEngine/chart_grammar.hpp"
#include "ParseEngine/grammar_types.hpp"
#include "ParseEngine/utils.hpp"

// Rust 产物模板（source/templates/rust_*.tmpl）走 library/template 的模板引擎
#include "template/engine.hpp"
#include "template/naming.hpp"

namespace ParseEngine
{

// %commit / %trivia 容器（ParserCommit / Parser::commits()）与位置信息 API
// （ParserLocation / Parser::current_location()）在这里落地：类型定义在
// rust_engine.rs（parser.rs），词法匹配在 rust_lexer_match.rs，演示输出在
// rust_demo_main.rs；与另外几档共用同一套词法声明序。

namespace
{

/**
 * @brief 一维整数表里的一个数（`rust_int_array.txt` 的元素）。
 */
struct RustIntItem
{
    bool br = false;     ///< 这个元素前面要不要换行（生成器按「每行几个」算好）
    long long value = 0; ///< 数值
};

/**
 * @brief 二维整数表的一行（`rust_int_rows.txt` 的元素）。
 */
struct RustIntRow
{
    std::vector<RustIntItem> items; ///< 这一行的数值
};

/// 固定字面量规则表的一行（`rust_lex_tables.rs` 的数组元素）。
struct RustLexRuleRow
{
    std::string text; ///< 字面量（**已带引号**的 Rust 字符串字面量）
    int type = 0;     ///< Token 类型号
    int length = 0;   ///< 字面量在 Rust 里占几个 char
    int blank = 0;    ///< 能不能以空白 / \b 通配开头（0 / 1）
    int order = 0;    ///< 词法声明序
};

/// 关键字查表的一行（`rust_lexer_helpers.rs` 之外的那个 match 分支）。
struct RustKeywordRow
{
    std::string literal; ///< 关键字字面量（已带引号）
    std::string token;   ///< Token 名（不带 `TOKEN_` 前缀）
};

/// 一个 Token 类型常量。
struct RustTokenConst
{
    std::string name; ///< Token 名（不带 `TOKEN_` 前缀）
    int value = 0;    ///< 常量值（就是它在 Token 表里的下标）
};

/// 一条 Token 别名。
struct RustAliasRow
{
    std::string name;  ///< Token 名（已带引号）
    std::string alias; ///< 显示别名（已带引号）
};

/// `%union` 里一条「词素能吃字符串」的成员（`rust_value_helpers.rs` 的 match 分支）。
struct RustValueCaseRow
{
    int kind = 0;        ///< Token 类型号
    std::string member;  ///< 成员名
};

/// `%token regex` 的一条正则（`rust_lex_regexes.rs` 的 regexes 元素）。
struct RustRegexRow
{
    std::string pattern; ///< 模式（Rust 字符串字面量，已转义）
    long long type = 0;  ///< Token 类型号
    long long order = 0; ///< 词法声明序
};

/// `%commit` 的一条声明（`rust_lex_regexes.rs` 的 commits 元素）。
struct RustCommitRow
{
    std::string pattern; ///< 模式（Rust 字符串字面量，已转义）
    std::string type;    ///< 类型名（已带引号）
    bool skip = false;   ///< 命中即丢
    int order = 0;       ///< 词法声明序
};

/// `%commit` 的类型 -> 显示别名（`rust_lex_regexes.rs` 的 commit_aliases 元素）。
struct RustCommitAliasRow
{
    std::string type;  ///< 类型名（已带引号）
    std::string alias; ///< 别名（已带引号）
};

/// 语义动作分支的一行（`rust_action_cases.rs` 的元素）。
struct RustActionRow
{
    bool first = false;         ///< 是不是第一条（决定写 `if` 还是 `else if`）
    int id = 0;                 ///< 产生式编号
    bool hasLine = false;       ///< 动作原文来自 .gram 的某一行
    int line = 0;               ///< 那一行
    std::vector<std::string> body; ///< 动作正文（**按行**；缩进交给模板）
    std::string fileName;       ///< 产物文件名（后向注释里用）
};

/// LL 展开分支里的一步（`rust_expand_cases.rs` 的 steps 元素）。
struct RustExpandStepRow
{
    bool isComment = false; ///< 空尾规则：只写一行注释
    std::string kind;       ///< `_ITEM_TERMINAL` / `_ITEM_NONTERM` / …
    std::string symbol;     ///< 符号号
    int slot = 0;           ///< 语义槽位
};

/// LL 展开分支的一条产生式（`rust_expand_cases.rs` 的 prods 元素）。
struct RustExpandProdRow
{
    int id = 0;                              ///< 产生式编号
    std::vector<RustExpandStepRow> steps;    ///< 压栈步骤
};

} // namespace
} // namespace ParseEngine

// 字段绑定只能写在全局作用域（`TEMPLATE_MODEL` 展开的是显式特化）
TEMPLATE_MODEL(ParseEngine::RustIntItem,
               tmpl::field("br", &ParseEngine::RustIntItem::br),
               tmpl::field("value", &ParseEngine::RustIntItem::value));

TEMPLATE_MODEL(ParseEngine::RustIntRow,
               tmpl::field("items", &ParseEngine::RustIntRow::items));

TEMPLATE_MODEL(ParseEngine::RustLexRuleRow,
               tmpl::field("text", &ParseEngine::RustLexRuleRow::text),
               tmpl::field("type", &ParseEngine::RustLexRuleRow::type),
               tmpl::field("length", &ParseEngine::RustLexRuleRow::length),
               tmpl::field("blank", &ParseEngine::RustLexRuleRow::blank),
               tmpl::field("order", &ParseEngine::RustLexRuleRow::order));

TEMPLATE_MODEL(ParseEngine::RustKeywordRow,
               tmpl::field("literal", &ParseEngine::RustKeywordRow::literal),
               tmpl::field("token", &ParseEngine::RustKeywordRow::token));

TEMPLATE_MODEL(ParseEngine::RustTokenConst,
               tmpl::field("name", &ParseEngine::RustTokenConst::name),
               tmpl::field("value", &ParseEngine::RustTokenConst::value));

TEMPLATE_MODEL(ParseEngine::RustAliasRow,
               tmpl::field("name", &ParseEngine::RustAliasRow::name),
               tmpl::field("alias", &ParseEngine::RustAliasRow::alias));

TEMPLATE_MODEL(ParseEngine::RustValueCaseRow,
               tmpl::field("kind", &ParseEngine::RustValueCaseRow::kind),
               tmpl::field("member", &ParseEngine::RustValueCaseRow::member));

TEMPLATE_MODEL(ParseEngine::RustRegexRow,
               tmpl::field("pattern", &ParseEngine::RustRegexRow::pattern),
               tmpl::field("type", &ParseEngine::RustRegexRow::type),
               tmpl::field("order", &ParseEngine::RustRegexRow::order));

TEMPLATE_MODEL(ParseEngine::RustCommitRow,
               tmpl::field("pattern", &ParseEngine::RustCommitRow::pattern),
               tmpl::field("type", &ParseEngine::RustCommitRow::type),
               tmpl::field("skip", &ParseEngine::RustCommitRow::skip),
               tmpl::field("order", &ParseEngine::RustCommitRow::order));

TEMPLATE_MODEL(ParseEngine::RustCommitAliasRow,
               tmpl::field("type",
                           &ParseEngine::RustCommitAliasRow::type),
               tmpl::field("alias",
                           &ParseEngine::RustCommitAliasRow::alias));

TEMPLATE_MODEL(ParseEngine::RustActionRow,
               tmpl::field("first", &ParseEngine::RustActionRow::first),
               tmpl::field("id", &ParseEngine::RustActionRow::id),
               tmpl::field("has_line", &ParseEngine::RustActionRow::hasLine),
               tmpl::field("line", &ParseEngine::RustActionRow::line),
               tmpl::field("body", &ParseEngine::RustActionRow::body),
               tmpl::field("file_name", &ParseEngine::RustActionRow::fileName));

TEMPLATE_MODEL(ParseEngine::RustExpandStepRow,
               tmpl::field("is_comment",
                           &ParseEngine::RustExpandStepRow::isComment),
               tmpl::field("kind", &ParseEngine::RustExpandStepRow::kind),
               tmpl::field("symbol", &ParseEngine::RustExpandStepRow::symbol),
               tmpl::field("slot", &ParseEngine::RustExpandStepRow::slot));

TEMPLATE_MODEL(ParseEngine::RustExpandProdRow,
               tmpl::field("id", &ParseEngine::RustExpandProdRow::id),
               tmpl::field("steps", &ParseEngine::RustExpandProdRow::steps));

namespace ParseEngine
{
namespace
{

/**
 * @brief 渲染一份**当值用**的模板（插进别的产物里当变量）。
 *
 * 片段模板按惯例以换行收尾，而它是插进别的产物**当值**的，多带一个换行就是
 * 产物里多出来一行，所以取完把结尾的换行收掉。**产物正文本身**要自带换行的
 * 那几份（`rust_lr_tables.rs` 之类）直接调 `tmpl::renderValues`，不走这里。
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
 * @brief 生成 Rust 字符串字面量（控制字符转成 `\u{..}` 转义）。
 * @param text 输入文本。
 * @return 形如 `"abc"` 的字面量文本（含两侧引号）。
 */
std::string rustString(const std::string &text)
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
            char buffer[24];
            std::snprintf(buffer, sizeof(buffer), "\\u{%x}",
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
 * @brief 字面量在 Rust 里占几个 `char`。
 *
 * 与 C# / 其它目标的 `char` 计数口径一致：只看宽度是 4 的 UTF-8 序列当
 * UTF-16 代理对算 2，其余一个码点算 1。词法「最长匹配」按这个数字排序。
 * @param text 字面量。
 * @return char 个数。
 */
std::size_t rustCharCount(const std::string &text)
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
std::vector<RustIntItem> intItems(const std::vector<long long> &values,
                                  std::size_t perLine)
{
    std::vector<RustIntItem> items;
    items.reserve(values.size());
    for (std::size_t index = 0u; index < values.size(); ++index)
    {
        RustIntItem item;
        item.br = (index != 0u) && ((index % perLine) == 0u);
        item.value = values[index];
        items.push_back(item);
    }
    return items;
}

/// 一维整数表的正文（不含方括号）：每行 perLine 个，续行缩进 indent。
std::string intListText(const std::vector<long long> &values,
                        const std::string &indent, std::size_t perLine)
{
    return renderValueTemplate(
        "rust_int_array.txt",
        {{"values", tmpl::encode(intItems(values, perLine))},
         {"indent", indent}});
}

/// 二维整数表（`&[&[...], ...]`）：方括号、换行、缩进都在模板里。
std::string intRowsText(const std::vector<std::vector<long long>> &rows,
                        const std::string &indent)
{
    std::vector<RustIntRow> lines;
    lines.reserve(rows.size());
    for (const std::vector<long long> &row : rows)
    {
        RustIntRow line;
        line.items = intItems(row, 12u);
        lines.push_back(line);
    }
    return renderValueTemplate("rust_int_rows.txt",
                               {{"rows", tmpl::encode(lines)},
                                {"indent", indent}});
}

/**
 * @brief 语义值类型信息。
 */
struct RustValueInfo
{
    std::string typeText;      ///< parser.rs 里用的类型名
    std::string tokenTypeText; ///< token.rs 里用的类型名（union 要写 `crate::parser::`）
    bool isUnion = false;      ///< 语义值是带成员字段的 `ParserValue`
};

/**
 * @brief 取语义值类型信息（`%union` > `%value_type` > 缺省 `i64`）。
 * @param request 组装入参。
 * @return 类型信息。
 */
RustValueInfo rustValueInfo(const ProjectRequest &request)
{
    RustValueInfo info;
    if (request.symbolValueMember != nullptr)
    {
        info.typeText = "ParserValue";
        info.tokenTypeText = "crate::parser::ParserValue";
        info.isUnion = true;
        return info;
    }
    if (request.valueType != nullptr)
    {
        info.typeText = *request.valueType;
        info.tokenTypeText = *request.valueType;
        return info;
    }
    info.typeText = "i64";
    info.tokenTypeText = "i64";
    return info;
}

/**
 * @brief 取 `%union` 成员的类型原文。
 *
 * Rust 的成员写成 `名字: 类型`（例如 `ival: i32`），所以类型是冒号右边那段。
 * 提取不出冒号时按「整段就是类型」处理（用户写法不标准时交给编译器报错）。
 * @param member 成员（`cType` 是声明原文）。
 * @return 类型文本。
 */
std::string rustMemberType(const UnionMember &member)
{
    const std::string &text = member.cType;
    const std::size_t colon = text.find(':');
    if (colon == std::string::npos)
    {
        return text;
    }
    std::string head = text.substr(colon + 1u);
    const std::size_t begin = head.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos)
    {
        return std::string();
    }
    const std::size_t end = head.find_last_not_of(" \t\r\n");
    return head.substr(begin, end - begin + 1u);
}

/**
 * @brief GLR 的冲突候选动作表与起始符号。
 *
 * 主动作在 ACTION 表里（与其它 LR 模式同一套）；这里只放「同一个 (状态, Token)
 * 单元里除主动作外的候选」，运行时按它把一个 GSS 节点分裂成多条路径。
 * @param analysis 分析结果。
 * @return Rust 代码段。
 */
std::string rustGlrTables(const AnalysisResult &analysis)
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
        Utils::loadTemplate("rust_glr_tables.rs"),
        {{"start_symbol", static_cast<long long>(termCount + startId)},
         {"extra_start", intRowsText(toRows(true), "")},
         {"extra_count", intRowsText(toRows(false), "")},
         {"extra_data", intListText(data.empty() ? std::vector<long long>{0} : data,
                                    "    ", 16u)}});
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
            outType = rustMemberType(item);
            return true;
        }
    }
    return false;
}

/// 这个 Rust 类型能不能直接装下字符串。
bool rustTextCarrier(const std::string &type)
{
    return (type == "String") || (type == "std::string::String");
}

/// 这个 Rust 类型是不是能装下数值的基础类型（`as` 转换合法）。
bool rustNumericType(const std::string &type)
{
    return (type == "i8") || (type == "i16") || (type == "i32") ||
           (type == "i64") || (type == "i128") || (type == "isize") ||
           (type == "u8") || (type == "u16") || (type == "u32") ||
           (type == "u64") || (type == "u128") || (type == "usize") ||
           (type == "f32") || (type == "f64");
}

/// 生成 `value_from_text` / `value_from_number` 两个函数。
std::string rustValueHelpers(const ProjectRequest &request,
                             const std::vector<std::string> &terminals)
{
    const RustValueInfo info = rustValueInfo(request);
    std::vector<RustValueCaseRow> textCases;
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
            if (!rustTextCarrier(memberType))
            {
                continue;
            }
            const auto found = std::find(terminals.begin(),
                                         terminals.end(), entry.first);
            if (found == terminals.end())
            {
                continue;
            }
            RustValueCaseRow row;
            row.kind = static_cast<int>(found - terminals.begin());
            row.member = member;
            textCases.push_back(row);
        }
        hasNumberMember =
            symbolMemberType(request, "NUMBER", numberMember, numberType) &&
            rustNumericType(numberType);
    }
    return tmpl::renderValues(
        Utils::loadTemplate("rust_value_helpers.rs"),
        {{"value_type", info.typeText},
         {"is_union", info.isUnion},
         {"text_cases", tmpl::encode(textCases)},
         {"has_number_member", hasNumberMember},
         {"number_member", numberMember},
         {"number_type", numberType},
         {"is_text_carrier", rustTextCarrier(info.typeText)},
         {"is_numeric", rustNumericType(info.typeText)}});
}

/// ACTION / GOTO / 产生式元数据表（LR 家族）。
std::string rustLrTables(const AnalysisResult &analysis)
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
        Utils::loadTemplate("rust_lr_tables.rs"),
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
std::string rustLlTables(const AnalysisResult &analysis)
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
        Utils::loadTemplate("rust_ll_tables.rs"),
        {{"ll_table", intRowsText(table, "")},
         {"has_columns", hasColumns},
         {"lookahead_code", intListText(codeList, "    ", 16u)},
         {"lookahead_column", intListText(columnList, "    ", 16u)},
         {"left_empty", intListText(leftEmpty, "    ", 16u)},
         {"has_follow", hasFollow},
         {"follow", intRowsText(followRows, "")}});
}

/// 语法树元数据：每条产生式的右部符号表。
std::string rustTreeTables(const AnalysisResult &analysis)
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
        Utils::loadTemplate("rust_tree_tables.rs"),
        {{"prod_start", intListText(starts, "    ", 16u)},
         {"prod_length", intListText(lengths, "    ", 16u)},
         {"prod_symbols", intListText(
                              symbols.empty()
                                  ? std::vector<long long>{0}
                                  : symbols,
                              "    ", 16u)}});
}

std::string rustChartTables(const AnalysisResult &analysis)
{
    const ChartGrammar chart = buildChartGrammar(analysis);
    const auto list = [](const std::vector<int> &values)
    {
        return intListText(
            std::vector<long long>(values.begin(), values.end()), "    ", 16u);
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
        Utils::loadTemplate("rust_chart_tables.rs"),
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

/// 递归下降的表：非终结符 -> 产生式区间 + 语义槽位个数。
///
/// 产生式在 `analysis.productions` 里「同一左部连续排在一起」（摊平时按非终结符
/// 名有序、规则内按声明顺序），所以一个非终结符对应一段连续编号——回溯时依次试
/// 的就是这个顺序。右部符号不在这里重发，直接查 `_TREE_PROD_*`。
std::string rustDdTables(const AnalysisResult &analysis)
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
        Utils::loadTemplate("rust_dd_tables.rs"),
        {{"prod_start", intListText(starts, "    ", 16u)},
         {"prod_count", intListText(counts, "    ", 16u)},
         {"prod_slots", intListText(slots, "    ", 16u)}});
}

/// 固定字面量词法规则表（按长度降序）。
std::vector<RustLexRuleRow> rustLexRules(
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
            rule.length = rustCharCount(literal);
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
    std::vector<RustLexRuleRow> rows;
    rows.reserve(rules.size());
    for (const Rule &rule : rules)
    {
        RustLexRuleRow row;
        row.text = rustString(rule.literal);
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
 * 产物里不再放 DFA 表：词法匹配交给 Rust 官方的 `regex` crate（`Cargo.toml`
 * 里带一个 `regex = "1"` 依赖，表在第一次取 Token 时编译一遍）。生成期用自带
 * 编译器做的那一遍校验在文法加载时（`readRegexPattern`）就做过了，认不下的由
 * `main.cpp` 分流：C99 是硬错误、其余档只警告（自带编译器不认识不等于 `regex`
 * 不认，见 `GrammarConfig::regexDiagnostics`）。
 * @param regexDefs Token 名 -> 正则。
 * @param identifierPattern `%identifier` 的正则；没写时为空。
 * @param commits `%commit` 声明（按声明序）。
 * @param tokenLexOrder Token 名 -> 词法声明序。
 * @param terminals Token 列表。
 * @param out 输出文本。
 */
void rustLexRegexes(const std::map<std::string, std::string> &regexDefs,
                    const std::optional<std::string> &identifierPattern,
                    const std::vector<CommitSpec> &commits,
                    const std::map<std::string, int> &tokenLexOrder,
                    const std::vector<std::string> &terminals,
                    std::string &out)
{
    const bool hasIdentifier = identifierPattern.has_value();
    // 这里不再自己校验正则：文法加载时（readRegexPattern）已经用自带编译器验过
    // 一遍，认不下的由 main.cpp 按目标语言分流——C99 是硬错误（它要编 DFA 表），
    // 其余档打警告后照常生成（见 GrammarConfig::regexDiagnostics）。

    std::vector<RustRegexRow> regexRows;
    {
        int fallbackOrder = 0;
        for (const auto &entry : regexDefs)
        {
            const auto orderIt = tokenLexOrder.find(entry.first);
            RustRegexRow row;
            row.pattern = rustString(entry.second);
            row.type =
                static_cast<long long>(terminalIndex(terminals, entry.first));
            row.order = (orderIt != tokenLexOrder.end()) ? orderIt->second
                                                         : fallbackOrder;
            ++fallbackOrder;
            regexRows.push_back(row);
        }
    }
    std::vector<RustCommitRow> commitRows;
    std::vector<RustCommitAliasRow> commitAliases;
    for (const CommitSpec &commit : commits)
    {
        RustCommitRow row;
        row.pattern = rustString(commit.pattern);
        row.type = rustString(commit.type);
        row.skip = commit.isSkip;
        row.order = commit.order;
        commitRows.push_back(row);
        if (!commit.alias.empty())
        {
            RustCommitAliasRow alias;
            alias.type = rustString(commit.type);
            alias.alias = rustString(commit.alias);
            commitAliases.push_back(alias);
        }
    }
    out = tmpl::renderValues(
        Utils::loadTemplate("rust_lex_regexes.rs"),
        {{"regexes", tmpl::encode(regexRows)},
         {"has_identifier", hasIdentifier},
         {"identifier_pattern",
          hasIdentifier ? rustString(*identifierPattern) : std::string()},
         {"commits", tmpl::encode(commitRows)},
         {"commit_aliases", tmpl::encode(commitAliases)}});
}

/// 词法表段（固定字面量规则 + `%space` + 正则表）。
///
/// 这一段没有失败路径：正则认不下只是在 `rustLexRegexes` 里打警告。
void rustLexTables(const ProjectRequest &request,
                   const std::vector<std::string> &terminals, std::string &out)
{
    const std::map<std::string, std::vector<std::string>> emptyTokenDefs;
    const auto &tokenDefs =
        (request.tokenDefs == nullptr) ? emptyTokenDefs : *request.tokenDefs;
    const LexSpec emptyLex;
    const LexSpec &lexSpec =
        (request.lexSpec == nullptr) ? emptyLex : *request.lexSpec;
    const std::string &chars =
        (request.spaceChars == nullptr) ? blankChars() : *request.spaceChars;
    out = tmpl::renderValues(
        Utils::loadTemplate("rust_lex_tables.rs"),
        {{"rules", tmpl::encode(
                       rustLexRules(tokenDefs, terminals,
                                    lexSpec.tokenLexOrder))},
         {"skip_chars", rustString(chars)}});
    std::string regexTables;
    rustLexRegexes(lexSpec.regex, lexSpec.identifier, lexSpec.commits,
                   lexSpec.tokenLexOrder, terminals, regexTables);
    out += regexTables;
}

/// 同步 Token / error 伪 Token / 非终结符名表等杂项表。
std::string rustMiscTables(const ProjectRequest &request,
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
        nontermNames.push_back(rustString(nonterm));
    }
    return tmpl::renderValues(
        Utils::loadTemplate("rust_misc_tables.rs"),
        {{"sync_tokens", intListText(syncTypes, "    ", 16u)},
         {"has_error_token", request.usesError},
         {"error_token_id", static_cast<long long>(errorTokenId)},
         {"nonterm_names", tmpl::encode(nontermNames)}});
}

/// 组装语义值类型定义（`%union` 时生成 `ParserValue` 结构体）。
std::string rustValueTypeDecl(const ProjectRequest &request)
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
    return tmpl::renderValues(Utils::loadTemplate("rust_value_type_decl.rs"),
                              {{"members", tmpl::encode(members)}});
}

/// 容量常量段与「期望 Token」表。
std::string rustConstants(const AnalysisResult &analysis,
                          const std::string &mode)
{
    const std::map<std::string, int> ids = nontermIds(analysis.nonterms);
    const auto startFound = ids.find(analysis.grammar.start);
    std::map<std::string, int> expectedIds;
    for (std::size_t index = 0u; index < analysis.terminals.size(); ++index)
    {
        expectedIds[analysis.terminals[index]] = static_cast<int>(index);
    }
    std::vector<RustIntRow> expectedRows;
    for (const std::vector<std::string> &row : expectedTokensByRow(analysis))
    {
        std::vector<long long> rowIds;
        rowIds.reserve(row.size());
        for (const std::string &token : row)
        {
            rowIds.push_back(static_cast<long long>(expectedIds[token]));
        }
        RustIntRow entry;
        entry.items = intItems(rowIds, 16u);
        expectedRows.push_back(entry);
    }
    return tmpl::renderValues(
        Utils::loadTemplate("rust_constants.rs"),
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
         {"expected_rows", tmpl::encode(expectedRows)},
         {"expected_fallback", rustString(expectedFallbackText(analysis.mode))},
         {"dd", isDdMode(mode)}});
}

/// Token 类型段（常量 + 名字表），进 token.rs。
std::string rustTokenTypes(const std::vector<std::string> &terminals,
                           const std::map<std::string, std::string> &aliases)
{
    std::vector<RustTokenConst> tokens;
    std::vector<std::string> names;
    tokens.reserve(terminals.size());
    names.reserve(terminals.size());
    for (std::size_t index = 0u; index < terminals.size(); ++index)
    {
        RustTokenConst item;
        item.name = terminals[index];
        item.value = static_cast<int>(index);
        tokens.push_back(item);
        names.push_back(rustString(terminals[index]));
    }
    std::vector<RustAliasRow> aliasRows;
    for (const auto &entry : aliases)
    {
        RustAliasRow row;
        row.name = rustString(entry.first);
        row.alias = rustString(entry.second);
        aliasRows.push_back(row);
    }
    return tmpl::renderValues(
        Utils::loadTemplate("rust_token_types.rs"),
        {{"tokens", tmpl::encode(tokens)},
         {"invalid_value", static_cast<long long>(terminals.size())},
         {"names_count", static_cast<long long>(terminals.size() + 1u)},
         {"names", tmpl::encode(names)},
         {"aliases", tmpl::encode(aliasRows)}});
}

/// 语义动作分支（LR / LL 共用一套 `apply_action`）。
std::string rustActionCases(const AnalysisResult &analysis,
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
    std::vector<RustActionRow> rows;
    for (const Production &production : analysis.productions)
    {
        if (production.transform == "left_empty")
        {
            continue;
        }
        RustActionRow row;
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
                                 /*csharpStyle=*/false, /*rustStyle=*/true))
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
    return tmpl::renderValues(Utils::loadTemplate("rust_action_cases.rs"),
                              {{"cases", tmpl::encode(rows)}});
}

/// 产生式展开分支（只有 LL 用）。
std::string rustExpandCases(const AnalysisResult &analysis,
                            const std::vector<std::string> &terminals)
{
    const std::set<std::string> terminalSet(terminals.begin(),
                                            terminals.end());
    const std::map<std::string, int> ids = nontermIds(analysis.nonterms);
    const auto pushCall = [](std::vector<RustExpandStepRow> &steps,
                             const std::string &kind, const std::string &symbol,
                             int slot)
    {
        RustExpandStepRow step;
        step.kind = kind;
        step.symbol = symbol;
        step.slot = slot;
        steps.push_back(step);
    };
    const auto pushSymbol = [&terminalSet, &terminals, &ids,
                             &pushCall](std::vector<RustExpandStepRow> &steps,
                                        const std::string &symbol, int slot)
    {
        if (terminalSet.count(symbol) != 0u)
        {
            const auto found =
                std::find(terminals.begin(), terminals.end(), symbol);
            pushCall(steps, "_ITEM_TERMINAL",
                     std::to_string(found - terminals.begin()), slot);
            return;
        }
        pushCall(steps, "_ITEM_NONTERM", std::to_string(ids.at(symbol)), slot);
    };
    std::vector<RustExpandProdRow> rows;
    for (const Production &production : analysis.productions)
    {
        const std::vector<std::string> &rhs = production.rhs;
        const std::string &transform = production.transform;
        RustExpandProdRow row;
        row.id = production.id;
        if (transform == "left_base")
        {
            const int baseLength = production.baseRhsLength;
            pushCall(row.steps, "_ITEM_LEFT_LOOP",
                     std::to_string(ids.at(rhs.back())), 0);
            pushCall(row.steps, "_ITEM_ACTION_KEEP",
                     std::to_string(production.id), 0);
            for (std::size_t index = static_cast<std::size_t>(baseLength);
                 index-- > 0u;)
            {
                pushSymbol(row.steps, rhs[index], 1 + static_cast<int>(index));
            }
        }
        else if (transform == "left_continue")
        {
            pushCall(row.steps, "_ITEM_LEFT_LOOP",
                     std::to_string(ids.at(rhs.back())), 0);
            pushCall(row.steps, "_ITEM_ACTION_KEEP",
                     std::to_string(production.id), 0);
            for (std::size_t index = rhs.size() - 1u; index-- > 0u;)
            {
                pushSymbol(row.steps, rhs[index], 2 + static_cast<int>(index));
            }
        }
        else if (transform == "left_empty")
        {
            RustExpandStepRow step;
            step.isComment = true;
            row.steps.push_back(step);
        }
        else
        {
            pushCall(row.steps, "_ITEM_ACTION", std::to_string(production.id),
                     0);
            for (std::size_t index = rhs.size(); index-- > 0u;)
            {
                pushSymbol(row.steps, rhs[index], 1 + static_cast<int>(index));
            }
        }
        rows.push_back(row);
    }
    return tmpl::renderValues(Utils::loadTemplate("rust_expand_cases.rs"),
                              {{"prods", tmpl::encode(rows)}});
}

/// 本模式的解析主循环入口（`run_loop`）。
std::string rustRunLoop(const std::string &mode)
{
    return tmpl::renderValues(Utils::loadTemplate("rust_run_loop.rs"),
                                                    {{"target", isDdMode(mode)
                                              ? "run_dd_loop"
                                              : (isGlrMode(mode)
                                                     ? "run_glr_loop"
                                                     : (isLrMode(mode)
                                                            ? "run_lr_loop"
                                                            : "run_ll_loop"))}});
}

/// `%import` 展开成的 `use NAME;` 行；列表为空时给空串。
std::string rustImportLines(const ProjectRequest &request)
{
    if ((request.imports == nullptr) || request.imports->empty())
    {
        return std::string();
    }
    return tmpl::renderValues(Utils::loadTemplate("rust_import_lines.rs"),
                              {{"imports", tmpl::encode(*request.imports)}});
}

/// `%package` 在产物里留下的注释（Rust 没有 package / namespace）。
std::string rustPackageComment(const ProjectRequest &request)
{
    if (request.packageName.empty())
    {
        return std::string();
    }
    return tmpl::renderValues(Utils::loadTemplate("rust_package_comment.rs"),
                              {{"package", request.packageName},
                               {"crate", CodeGenRust::crateName(request)}});
}

/**
 * @brief 词法器各段的替换表。
 */
struct RustLexerBlocks
{
    std::string stringBlock;     ///< 内建字符串字面量块
    std::string matchBlock;      ///< `lex_match` 主体（含正则 DFA 循环）
    std::string numberBlock;     ///< 内建数字块
    std::string identifierBlock; ///< 标识符块（DFA 或缺省）
};

/**
 * @brief 组装词法器各段（按文法声明决定哪几块要发）。
 * @param request 组装入参。
 * @param blocks 输出。
 */
void buildRustLexerBlocks(const ProjectRequest &request,
                          RustLexerBlocks &blocks)
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
        blocks.stringBlock = Utils::loadTemplate("rust_lexer_string.rs");
    }
    std::string regexLoop;
    if (!lexSpec.regex.empty())
    {
        regexLoop = Utils::loadTemplate("rust_lexer_regex_loop.rs");
    }
    blocks.matchBlock = tmpl::renderValues(
        Utils::loadTemplate("rust_lexer_match.rs"),
        {{"lex_regex_loop", regexLoop}});
    if (!overridden("NUMBER"))
    {
        blocks.numberBlock = Utils::loadTemplate("rust_lexer_number.rs");
    }
    if (lexSpec.identifier.has_value())
    {
        blocks.identifierBlock =
            Utils::loadTemplate("rust_lexer_identifier_dfa.rs");
    }
    else if (!overridden("VARIABLE"))
    {
        blocks.identifierBlock =
            Utils::loadTemplate("rust_lexer_identifier.rs");
    }
}

/// 组装词法器（解析与词法模式共用同一份模板）。
std::string rustLexerText(const ProjectRequest &request)
{
    RustLexerBlocks blocks;
    buildRustLexerBlocks(request, blocks);
    return tmpl::renderValues(
        Utils::loadTemplate("rust_lexer.rs"),
        {{"lex_string_block", blocks.stringBlock},
         {"lex_match_block", blocks.matchBlock},
         {"lex_number_block", blocks.numberBlock},
         {"lex_identifier_block", blocks.identifierBlock}});
}

/// 取 `%code … %endcode` 的正文（带前后行标记）。
std::string rustUserCode(const ProjectRequest &request,
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
        blocks.push_back(CodeGenRust::withLineMarkers(
            block, (*request.lines->userBlockLines)[index], "", fileName));
    }
    if (blocks.empty())
    {
        return std::string();
    }
    return renderValueTemplate("rust_user_code.rs",
                               {{"blocks", tmpl::encode(blocks)}});
}

/// 关键字表段（只有 `%identifier` 声明时才发）。
std::string rustKeywordSection(const ProjectRequest &request,
                               const std::vector<std::string> &terminals)
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
    (void) terminals; // 关键字表直接用 `TOKEN_<名字>` 常量，不用下标
    std::vector<RustKeywordRow> rows;
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
            RustKeywordRow row;
            row.literal = rustString(literal);
            row.token = entry.first;
            rows.push_back(row);
        }
    }
    return tmpl::renderValues(
        Utils::loadTemplate("rust_lexer_keyword.rs"),
        {{"rows", tmpl::encode(rows)}});
}

/// `%class_body` 正文（插成一段 `impl Parser { ... }`）。
std::string rustClassBody(const ProjectRequest &request)
{
    if ((request.classBody == nullptr) || request.classBody->empty())
    {
        return std::string();
    }
    return tmpl::renderValues(
        Utils::loadTemplate("rust_class_body.rs"),
        {{"body", CodeGenRust::withLineMarkers(*request.classBody,
                                               request.classBodyLine + 1u,
                                               "    ", "parser.rs")}});
}

/**
 * @brief 组装 parser.rs（解析模式）。
 * @param request 组装入参。
 * @param mode 归一化后的模式。
 * @param analysis 分析结果。
 * @param out 输出文本（成功时写入）。
 * @param error 出错时写入错误说明。
 * @return 成功返回 true。
 */
bool buildParserRs(const ProjectRequest &request, const std::string &mode,
                   const AnalysisResult &analysis, std::string &out,
                   Error &error)
{
    const RustValueInfo info = rustValueInfo(request);
    const std::vector<std::string> &terminals = analysis.terminals;
    std::size_t maxSlots = 4u;
    for (const Production &production : analysis.productions)
    {
        maxSlots = std::max(maxSlots, production.rhs.size() + 3u);
    }
    std::string tables;
    rustLexTables(request, terminals, tables);
    tables += tmpl::renderValues(Utils::loadTemplate("rust_section_marker.txt"),
                                 {{"title", "分析表"}});
    if (isLrMode(mode))
    {
        tables += rustLrTables(analysis);
    }
    else
    {
        tables += rustLlTables(analysis);
    }
    if (!isLrMode(mode))
    {
        tables += tmpl::renderValues(
            Utils::loadTemplate("rust_section_marker.txt"),
            {{"title", "语法树表"}});
        tables += rustTreeTables(analysis);
    }
    if (isGlrMode(mode))
    {
        tables += rustGlrTables(analysis);
        if ((mode == "earley") || (mode == "cyk"))
        {
            tables += rustChartTables(analysis);
        }
    }
    if (isDdMode(mode))
    {
        tables += rustDdTables(analysis);
    }
    tables += rustMiscTables(request, analysis);

    const std::string actionCases =
        rustActionCases(analysis,
                        isDdMode(mode) ? "dd"
                                       : (isLrMode(mode) ? analysis.mode
                                                         : "ll1"),
                        request, "parser.rs", error);
    if (!error.empty())
    {
        return false;
    }
    // 语义动作与产生式展开两块：一个产物 = 一份模板，先各自渲染再把结果
    // 当变量传进引擎（渲染是一趟的，值里的 `{{ }}` 不会再被解析）。
    // `rust_lr_action.rs` 是**产物正文**（自带换行），走 renderValues 别收尾。
    const std::string lrAction = tmpl::renderValues(
        Utils::loadTemplate("rust_lr_action.rs"),
        {{"value_type", info.typeText}, {"lr_action_cases", actionCases}});
    std::string lrText = lrAction;
    std::string llText;
    std::string ddText;
    if (isDdMode(mode))
    {
        // DD：动作分支 + 带回溯的递归下降驱动（不建表，直接照文法走）
        ddText = tmpl::renderValues(Utils::loadTemplate("rust_dd.rs"),
                                    {{"value_type", info.typeText}});
    }
    else if (isGlrMode(mode))
    {
        // GLR：动作分支 + 图结构栈驱动（替代 LR 的分析栈驱动）
        lrText += tmpl::renderValues(
            Utils::loadTemplate("rust_glr.rs"),
            {{"value_type", info.typeText},
             {"chart", (mode == "earley") || (mode == "cyk")}});
        if ((mode == "earley") || (mode == "cyk"))
        {
            lrText += tmpl::renderValues(
                Utils::loadTemplate("rust_chart.rs"),
                {{"earley", mode == "earley"}});
        }
    }
    else if (isLrMode(mode))
    {
        lrText += tmpl::renderValues(Utils::loadTemplate("rust_lr.rs"),
                                     {{"value_type", info.typeText},
                                      {"lr_incremental", !isGlrMode(mode)}});
    }
    else
    {
        llText = tmpl::renderValues(Utils::loadTemplate("rust_ll.rs"),
                                    {{"ll_expand_cases",
                                      rustExpandCases(analysis, terminals)}});
    }

    std::string constants = rustValueTypeDecl(request);
    constants += rustConstants(analysis, mode);

    const bool lrIncrementalMode = isLrMode(mode) && !isGlrMode(mode);
    const bool incrementalMode =
        lrIncrementalMode || isDdMode(mode) ||
        (!isLrMode(mode) && !isLexMode(mode));
    const std::string engine = tmpl::renderValues(
        Utils::loadTemplate("rust_engine.rs"),
        {{"mode_name", modeLabelText(mode)},
         {"max_stack_depth",
          static_cast<long long>(request.maxStackDepth.value_or(64))},
         {"max_errors", static_cast<long long>(request.maxErrors.value_or(8))},
         {"error_recovery_tokens",
          static_cast<long long>(request.errorRecoveryTokens.value_or(3))},
         {"value_slots", static_cast<long long>(maxSlots)},
         {"constants", constants},
         {"tables", tables},
         {"lexer_helpers", Utils::loadTemplate("rust_lexer_helpers.rs")},
         {"value_helpers", rustValueHelpers(request, terminals)},
         {"keyword_method", rustKeywordSection(request, terminals)},
         {"glr", isGlrMode(mode)},
         {"dd", isDdMode(mode)},
         {"incremental", incrementalMode},
         {"lr_incremental", lrIncrementalMode},
         {"run_loop", rustRunLoop(mode)},
         {"lexer", rustLexerText(request)},
         {"lr", lrText},
         {"ll", llText},
         {"dd_body", ddText},
         {"value_type", info.typeText}});

    out = tmpl::renderValues(
        Utils::loadTemplate("rust_parser.rs"),
        {{"mode_name", modeLabelText(mode)},
         {"package_comment", rustPackageComment(request)},
         {"imports", rustImportLines(request)},
         {"user_code", rustUserCode(request, "parser.rs")},
         {"engine", engine},
         {"class_body", rustClassBody(request)}});
    return true;
}

} // namespace

std::string CodeGenRust::grammarLineMarker(std::size_t lineNumber)
{
    if (lineNumber == 0u)
    {
        return std::string();
    }
    return tmpl::renderValues(Utils::loadTemplate("rust_line_marker.rs"),
                              {{"line", static_cast<long long>(lineNumber)}});
}

std::string CodeGenRust::withLineMarkers(const std::string &body,
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
        Utils::loadTemplate("rust_with_line_markers.rs"),
        {{"has_line", lineNumber != 0u},
         {"line", static_cast<long long>(lineNumber)},
         {"indent", indent},
         {"body", text},
         {"file_name", fileName}});
}

bool CodeGenRust::parserRs(const ProjectRequest &request,
                           const std::string &mode,
                           const AnalysisResult &analysis, std::string &out,
                           Error &error)
{
    return buildParserRs(request, mode, analysis, out, error);
}

void CodeGenRust::tokenRs(const ProjectRequest &request,
                          const std::vector<std::string> &terminals,
                          std::string &out)
{
    const RustValueInfo info = rustValueInfo(request);
    const std::map<std::string, std::string> emptyAliases;
    const auto &aliases =
        (request.lexSpec == nullptr) ? emptyAliases
                                     : request.lexSpec->tokenAliases;
    out = tmpl::renderValues(
        Utils::loadTemplate("rust_token.rs"),
        {{"package_comment", rustPackageComment(request)},
         {"value_type", info.tokenTypeText},
         {"token_types", rustTokenTypes(terminals, aliases)}});
}

/**
 * @brief 演示 main 里的 `%commit` + location 打印片段（Rust）。
 * @param request 组装入参。
 * @return Rust 代码段；文法里没写 %commit 时为空串（演示输出与从前一样）。
 */
std::string rustDemoCommitPrint(const ProjectRequest &request)
{
    const LexSpec emptyLex;
    const LexSpec &lexSpec =
        (request.lexSpec == nullptr) ? emptyLex : *request.lexSpec;
    if (lexSpec.commits.empty())
    {
        return std::string();
    }
    // 纯静态片段，但正文里有 `{# #}` 说明块：要过一遍引擎把注释收掉
    return tmpl::renderValues(
        Utils::loadTemplate("rust_demo_commit_print.rs"));
}

std::string CodeGenRust::mainRs(const ProjectRequest &request,
                                const std::string &mode,
                                const std::string &mainBody)
{
    std::string body;
    if (!mainBody.empty())
    {
        body = tmpl::renderValues(Utils::loadTemplate("rust_main_body.rs"),
                                  {{"body", mainBody}});
    }
    else if (isGlrMode(mode))
    {
        std::string print;
        if (request.tree)
        {
            print = "    for index in 0..engine.glr_result_count()\n"
                    "    {\n"
                    "        println!(\"结果[{}] 语法树:\", index);\n"
                    "        if let Some(tree) = engine.glr_tree(index)\n"
                    "        {\n"
                    "            tree.print();\n"
                    "        }\n"
                    "    }\n";
        }
        else
        {
            print = "    for index in 0..engine.glr_result_count()\n"
                    "    {\n"
                    "        println!(\"结果[{}]: {:?}\", index, "
                    "engine.glr_result(index).unwrap());\n"
                    "    }\n";
        }
        body = tmpl::renderValues(
            Utils::loadTemplate("rust_glr_demo_main.rs"),
            {{"glr_run", request.tree ? "engine.run_glr_tree(&input_text)"
                                      : "engine.run_glr(&input_text)"},
             {"glr_print", print},
             {"demo_commits", rustDemoCommitPrint(request)}});
    }
    else
    {
        body = tmpl::renderValues(
            Utils::loadTemplate("rust_demo_main.rs"),
            {{"tree", request.tree},
             {"dd", isDdMode(mode)},
             {"demo_commits", rustDemoCommitPrint(request)}});
    }
    return tmpl::renderValues(Utils::loadTemplate("rust_main.rs"),
                              {{"mode_name", modeLabelText(mode)},
                               {"imports", rustImportLines(request)},
                               {"main_body", body}});
}

std::string CodeGenRust::lexerMainRs(const ProjectRequest &request,
                                     const std::string &mainBody)
{
    std::string body;
    if (!mainBody.empty())
    {
        body = tmpl::renderValues(Utils::loadTemplate("rust_main_body.rs"),
                                  {{"body", mainBody}});
    }
    else
    {
        body = Utils::loadTemplate("rust_lex_demo_main.rs");
    }
    return tmpl::renderValues(Utils::loadTemplate("rust_lex_main.rs"),
                              {{"mode_name", "LEX(词法模式)"},
                               {"imports", rustImportLines(request)},
                               {"main_body", body}});
}

void CodeGenRust::lexerRs(const ProjectRequest &request,
                          const std::vector<std::string> &terminals,
                          std::string &out)
{
    std::string tables;
    rustLexTables(request, terminals, tables);
    const std::string engine = tmpl::renderValues(
        Utils::loadTemplate("rust_lex_engine.rs"),
        {{"constants", tmpl::renderValues(
                           Utils::loadTemplate("rust_lex_constants.rs"),
                           {{"terminal_count",
                             static_cast<long long>(terminals.size())}})},
         {"tables", tables},
         {"lexer_helpers", Utils::loadTemplate("rust_lexer_helpers.rs")},
         {"value_helpers", rustValueHelpers(request, terminals)},
         {"keyword_method", rustKeywordSection(request, terminals)},
         {"max_errors", static_cast<long long>(request.maxErrors.value_or(8))},
         {"lexer", rustLexerText(request)}});
    out = tmpl::renderValues(
        Utils::loadTemplate("rust_parser.rs"),
        {{"mode_name", "LEX(词法模式)"},
         {"package_comment", rustPackageComment(request)},
         {"imports", rustImportLines(request)},
         {"user_code", rustUserCode(request, "parser.rs")},
         {"engine", engine},
         {"class_body", rustClassBody(request)}});
}

std::string CodeGenRust::crateName(const ProjectRequest &request)
{
    if (request.packageName.empty())
    {
        return "parser_demo";
    }
    std::string name = request.packageName;
    std::replace(name.begin(), name.end(), '.', '_');
    return name;
}

std::string CodeGenRust::cargoToml(const ProjectRequest &request)
{
    return tmpl::renderValues(Utils::loadTemplate("rust_cargo.toml"),
                              {{"package_name", crateName(request)}});
}

} // namespace ParseEngine
