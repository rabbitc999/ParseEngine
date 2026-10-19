/**
 * @file code_gen_cpp17.cpp
 * @brief C++17 语言后端实现：parser.hpp / parser.cpp / main.cpp。
 */

#include "ParseEngine/code_gen_cpp17.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <vector>

#include "ParseEngine/code_gen.hpp"
#include "ParseEngine/chart_grammar.hpp"
#include "ParseEngine/action_translator.hpp"
#include "ParseEngine/grammar_loader.hpp"
#include "ParseEngine/regex_dfa.hpp"
#include "ParseEngine/regex_vm.hpp"
#include "ParseEngine/utils.hpp"

// 产物模板（source/templates/cpp17_*.tmpl）走 library/template 的模板引擎：
// 占位符 `{{ 变量 }}` 由这里摆「名字 -> 值」，条件与循环归模板自己管。
#include "template/engine.hpp"

namespace ParseEngine
{

namespace
{


/**
 * @brief 关键字表的一行（`cpp17_keyword.cpp.tmpl` 的数组元素）。
 */
struct KeywordRow
{
    std::string text;  ///< 字面量（已写成 C 字符串字面量）
    std::string token; ///< 对应 Token（`TOKEN_X`）
};

/**
 * @brief 固定字面量词法规则表的一行（`cpp17_lex_rules.cpp.tmpl` 的数组元素）。
 *
 * 表尾那条哨兵也走同一个模型：text 写 `nullptr`、token 写 `TokenType::TOKEN_END`。
 */
struct LexRuleRow
{
    std::string text;       ///< 字面量（已写成 C 字符串字面量）
    std::string length;     ///< 字节长度（产物里补 `u` 后缀）
    std::string token;      ///< Token（`TOKEN_X` 或 `TokenType::TOKEN_END`）
    std::string blankFirst; ///< `true` / `false`
    std::string wildcard;   ///< `true` / `false`
    std::string order;      ///< 词法声明序
};

/**
 * @brief Token 名称表的一行（`cpp17_token_names.cpp.tmpl` 的数组元素）。
 */
struct TokenNameRow
{
    std::string name;  ///< Token 名（已写成 C 字符串字面量）
    std::string alias; ///< 别名（没有就是空字面量）
};

/**
 * @brief LL(1) 展开分支里的一项（`cpp17_expand_cases.cpp.tmpl` 的数组元素）。
 */
struct LlExpandItem
{
    bool note = false;    ///< 只写一行注释（空尾规则）
    bool terminal = false; ///< push 的是终结语：符号要写成 TOKEN_X
    std::string kind;     ///< ItemKind 全名（ItemKind::Terminal / …）
    std::string symbol;   ///< TOKEN_X 或符号号（产物里补 u）
    std::string slot;     ///< 槽位（产物里补 u）
};

/**
 * @brief LL(1) 的一条产生式展开分支（`cpp17_expand_cases.cpp.tmpl` 的数组元素）。
 */
struct LlExpandCase
{
    std::string id;                    ///< 产生式编号
    std::vector<LlExpandItem> items;   ///< 展开项（按生成顺序）
};

/**
 * @brief 一条 `%token regex` 声明（`cpp17_lex_regex.cpp.tmpl` 的数组元素）。
 */
struct RegexEntry
{
    std::string pattern; ///< 正则（已写成 C 字符串字面量）
    std::string token;   ///< Token 名（产物里补 TOKEN_ 前缀）
    std::string order;   ///< 词法声明序
};

/**
 * @brief 一条 `%commit` 声明（同上）。
 *
 * 没有 %commit 时给一条占位：pattern 空 → `std::regex()`，
 * 其余字段就是占位值（kCommitCount 是 0，运行时不会读到）。
 */
/**
 * @brief 一条 %token regex 的「Token -> %union 成员」绑定
 *        （`cpp17_lexer_match.cpp.tmpl` 的数组元素）。
 */
struct RegexMember
{
    std::string token;  ///< Token 名（产物里补 TOKEN_ 前缀）
    std::string member; ///< 它绑的 %union 成员
};

struct CommitEntry
{
    bool placeholder = false; ///< 占位那一行（产物里写作 `{ nullptr, nullptr }`）
    std::string pattern;      ///< 正则（已写成 C 字符串字面量）
    std::string type;         ///< 片段类型（已写成 C 字符串字面量）
    std::string alias;        ///< 别名（已写成 C 字符串字面量）
    std::string skip;         ///< `1u` / `0u`
    std::string order;        ///< 声明序
};

/**
 * @brief 按 UTF-8 统计字符个数（词法规则按「看得见的长度」排序）。
 * @param text 输入文本。
 * @return 字符个数。
 */
std::size_t utf8Length(const std::string &text)
{
    std::size_t count = 0u;
    for (const char ch : text)
    {
        if ((static_cast<unsigned char>(ch) & 0xC0u) != 0x80u)
        {
            ++count;
        }
    }
    return count;
}

/**
 * @brief 判断文本里是否有非空白字符。
 * @param text 输入文本。
 * @return 有可见字符返回 true。
 */
bool hasVisibleText(const std::string &text)
{
    return text.find_first_not_of(" \t\n\r\v\f") != std::string::npos;
}

/**
 * @brief 全大写（头文件保护宏用）。
 * @param text 输入文本。
 * @return 大写文本。
 */
std::string upperCase(const std::string &text)
{
    std::string result = text;
    for (char &ch : result)
    {
        ch = static_cast<char>(
            std::toupper(static_cast<unsigned char>(ch)));
    }
    return result;
}


/**
 * @brief 头文件保护宏名：产物文件名全大写、`.` 换成 `_`。
 * @param fileName 产物文件名（`parser.hpp` / `myapp_parser.hpp`）。
 * @return 保护宏名（`PARSER_HPP` / `MYAPP_PARSER_HPP`）。
 */
std::string guardNameOf(const std::string &fileName)
{
    std::string replaced = fileName;
    for (char &ch : replaced)
    {
        if (ch == '.')
        {
            ch = '_';
        }
    }
    return upperCase(replaced);
}

/**
 * @brief 取 %union 里某个符号绑定的成员名。
 * @param symbolMember 符号 -> 成员表；没写 %union 时可为 nullptr。
 * @param symbol 符号名。
 * @return 成员名；没绑定返回空串。
 */
std::string lexMemberOf(
    const std::map<std::string, std::string> *symbolMember,
    const std::string &symbol)
{
    if (symbolMember == nullptr)
    {
        return "";
    }
    const auto memberIt = symbolMember->find(symbol);
    const std::string *member =
        memberIt != symbolMember->end() ? &memberIt->second : nullptr;
    return (member != nullptr) ? *member : "";
}

/**
 * @brief 取 %union 的成员列表对应的名字（没有成员时返回空串）。
 * @param symbolMember 符号 -> 成员表。
 * @param symbol 符号名。
 * @return 成员名；没绑定返回空串。
 */
std::string memberOrEmpty(
    const std::map<std::string, std::string> *symbolMember,
    const std::string &symbol)
{
    return lexMemberOf(symbolMember, symbol);
}

/// 语义值模型：没用 %union 时 Token.value 是 int32，用了就是联合体本身。
bool unionMode(const ProjectRequest &request)
{
    return request.symbolValueMember != nullptr;
}

/* ---------------- 词法表 ---------------- */

/**
 * @brief 关键字表与 keywordType()（C++17 版）。
 * @param tokenDefs 固定字面量 Token。
 * @return C++ 代码段。
 */
std::string generatorKeywordCpp17(
    const std::map<std::string, std::vector<std::string>> &tokenDefs)
{
    std::vector<KeywordRow> keywords;
    for (const auto &entry : tokenDefs)
    {
        for (const std::string &literal : entry.second)
        {
            bool identifier = !literal.empty() &&
                              (std::isalpha(static_cast<unsigned char>(
                                   literal[0])) != 0 ||
                               literal[0] == '_');
            for (std::size_t index = 1; identifier && (index < literal.size());
                 ++index)
            {
                const char ch = literal[index];
                identifier = (std::isalnum(static_cast<unsigned char>(ch)) !=
                              0) ||
                             (ch == '_');
            }
            if (!identifier)
            {
                continue;
            }
            KeywordRow row;
            row.text = Utils::varCString(literal);
            row.token = "TOKEN_" + entry.first;
            keywords.push_back(std::move(row));
        }
    }
    // 有没有关键字决定生成哪套 keywordType()，那两条分支在模板里
    return tmpl::renderValues(Utils::loadTemplate("cpp17_keyword.cpp"),
                              {{"has_keywords", !keywords.empty()},
                               {"keywords", tmpl::encode(keywords)}});
}

/**
 * @brief 固定字面量词法规则表（按字面量长度降序）。
 * @param tokenDefs 固定字面量 Token。
 * @param tokenLexOrder Token 名 -> 词法声明序（%commit 同长度比较用）。
 * @return 初始化列表文本（每行一条规则）。
 */
std::string generatorLexRulesCpp17(
    const std::map<std::string, std::vector<std::string>> &tokenDefs,
    const std::map<std::string, int> &tokenLexOrder)
{
    struct Rule
    {
        std::string literal;
        std::string tokenName;
        int order = 0;
        int declOrder = 0;
    };
    std::vector<Rule> rules;
    int definitionOrder = 0;
    for (const auto &entry : tokenDefs)
    {
        const auto orderIt = tokenLexOrder.find(entry.first);
        const int declOrder = (orderIt != tokenLexOrder.end())
                                  ? orderIt->second
                                  : definitionOrder;
        for (const std::string &literal : entry.second)
        {
            rules.push_back(
                {literal, entry.first, definitionOrder, declOrder});
            ++definitionOrder;
        }
    }
    std::stable_sort(rules.begin(), rules.end(),
                     [](const Rule &left, const Rule &right)
                     {
                         const std::size_t leftLen = utf8Length(left.literal);
                         const std::size_t rightLen = utf8Length(right.literal);
                         if (leftLen != rightLen)
                         {
                             return leftLen > rightLen;
                         }
                         return left.order < right.order;
                     });
    // 一行一行摆数据；`cpp17_lex_rules.cpp.tmpl` 负责排版与分隔符
    std::vector<LexRuleRow> rows;
    for (const Rule &rule : rules)
    {
        LexRuleRow row;
        row.text = Utils::varCString(rule.literal);
        row.length = std::to_string(rule.literal.size());
        row.token = "TOKEN_" + rule.tokenName;
        row.blankFirst = literalCanStartBlank(rule.literal) ? "true" : "false";
        row.wildcard =
            literalHasBlankWildcard(rule.literal) ? "true" : "false";
        row.order = std::to_string(rule.declOrder);
        rows.push_back(std::move(row));
    }
    // 表尾哨兵：遍历到它时 text 为空，长度匹配自然失败
    rows.push_back(
        {"nullptr", "0", "TokenType::TOKEN_END", "false", "false", "0"});
    return tmpl::renderValues(Utils::loadTemplate("cpp17_lex_rules.cpp"),
                              {{"rules", tmpl::encode(rows)}});
}

/** @brief 生成 Unicode 程序的不可变数据表。
 * @param patterns 正则列表。 @param out 输出片段。 @return 有 Unicode 程序时返回真。
 */
bool generatorUnicodeRegexCpp17(const std::vector<std::string> &patterns, std::string &out)
{
    auto programs = std::make_shared<tmpl::Array>();
    std::set<std::string> seen;
    for (const auto &pattern : patterns)
    {
        if (!RegexVm::unicodeMode(pattern) || !seen.insert(pattern).second) continue;
        RegexVmProgram program;
        Error error;
        if (!RegexVm::compile(pattern, program, error)) fatalInternal(error.message);
        auto item = std::make_shared<tmpl::Object>();
        item->insert("name", "s_cppUnicodeRegex" + std::to_string(programs->items.size()));
        item->insert("pattern", Utils::varCString(pattern));
        item->insert("count", std::to_string(program.code.size()));
        item->insert("entry", std::to_string(program.entry));
        item->insert("groups", std::to_string(program.groups));
        std::vector<std::string> rows, ranges;
        for (const auto &instruction : program.code)
        {
            std::string row = "{ " + std::to_string(instruction.op) + ", " + std::to_string(instruction.next) +
                ", " + std::to_string(instruction.other) + ", " + std::to_string(instruction.value) + ", { ";
            for (std::size_t index = 0; index < instruction.bits.size(); ++index)
                row += (index ? ", " : "") + std::to_string(instruction.bits[index]);
            rows.push_back(row + " }, " + std::to_string(instruction.rangeOffset) + "u, " +
                           std::to_string(instruction.rangeCount) + "u }");
        }
        for (const auto point : program.ranges) ranges.push_back(std::to_string(point) + "u");
        if (ranges.empty()) ranges.push_back("0u");
        item->insert("rows", tmpl::encode(rows));
        item->insert("ranges", tmpl::encode(ranges));
        programs->items.emplace_back(item);
    }
    if (programs->items.empty()) return false;
    out += tmpl::renderValues(Utils::loadTemplate("cpp17_lex_unicode.cpp"),
        {{"runtime", Utils::loadTemplate("regex_unicode_data.h") + Utils::loadTemplate("regex_vm_runtime.h")},
         {"programs", tmpl::Value(programs)}});
    return true;
}

/**
 * @brief 生成 %token regex / %identifier / %commit 的**正则表**（C++17 版）。
 *
 * 字节模式交给 std::regex；显式 Unicode 模式使用生成期编译的共享程序。
 * 生成期用自带编译器做的那一遍校验在文法加载时（`readRegexPattern`）就做过了，
 * 认不下的由 `main.cpp` 分流：C99 是硬错误、其余档只警告（自带编译器不认识
 * 不等于 `std::regex` 不认，见 `GrammarConfig::regexDiagnostics`）。
 * @param regexDefs 正则 Token（有序）。
 * @param identifierPattern 标识符正则；没写 %identifier 时为空。
 * @param commits %commit 声明（声明序）；没有时为空。
 * @param tokenLexOrder Token 名 -> 词法声明序（%commit 同长度比较用）。
 * @param outTables 输出表定义段。
 */
void generatorLexRegexCpp17(
    const std::map<std::string, std::string> &regexDefs,
    const std::optional<std::string> &identifierPattern,
    const std::vector<CommitSpec> &commits,
    const std::map<std::string, int> &tokenLexOrder, std::string &outTables)
{
    std::vector<std::string> patterns;
    for (const auto &entry : regexDefs) patterns.push_back(entry.second);
    if (identifierPattern) patterns.push_back(*identifierPattern);
    for (const auto &commit : commits) patterns.push_back(commit.pattern);
    const bool hasUnicode = generatorUnicodeRegexCpp17(patterns, outTables);
    const bool hasIdentifier = identifierPattern.has_value();
    std::vector<std::pair<std::string, std::string>> entries;
    for (const auto &entry : regexDefs)
    {
        entries.emplace_back(entry.first, entry.second);
    }
    // 这里不再自己校验正则：文法加载时（readRegexPattern）已经用自带编译器验过
    // 一遍，认不下的由 main.cpp 按目标语言分流——C99 是硬错误（它要编 DFA 表），
    // 其余档打警告后照常生成（见 GrammarConfig::regexDiagnostics）。
    // 表的排版与两个匹配函数都在 `cpp17_lex_regex.cpp.tmpl` 里；这里只摆数据
    std::vector<RegexEntry> regexEntries;
    for (std::size_t index = 0; index < entries.size(); ++index)
    {
        const auto orderIt = tokenLexOrder.find(entries[index].first);
        RegexEntry row;
        row.pattern = Utils::varCString(entries[index].second);
        row.token = entries[index].first;
        row.order = std::to_string((orderIt != tokenLexOrder.end())
                                       ? orderIt->second
                                       : static_cast<int>(index));
        regexEntries.push_back(std::move(row));
    }
    std::vector<CommitEntry> commitEntries;
    for (const CommitSpec &commit : commits)
    {
        CommitEntry row;
        row.pattern = Utils::varCString(commit.pattern);
        row.type = Utils::varCString(commit.type);
        row.alias = Utils::varCString(commit.alias);
        row.skip = commit.isSkip ? "1u" : "0u";
        row.order = std::to_string(commit.order);
        commitEntries.push_back(std::move(row));
    }
    if (commitEntries.empty())
    {
        // 占位：kCommitCount 是 0，循环不会读到它（std::regex 没有「空指针」态）
        commitEntries.push_back(
            {true, "", "nullptr", "nullptr", "0u", "0"});
    }
    outTables += tmpl::renderValues(
        Utils::loadTemplate("cpp17_lex_regex.cpp"),
        {{"has_unicode", hasUnicode},
         {"has_regex", !regexEntries.empty()},
         {"regex_count", std::to_string(entries.size())},
         {"regex_entries", tmpl::encode(regexEntries)},
         {"commit_count", std::to_string(commits.size())},
         {"commit_entries", tmpl::encode(commitEntries)},
         {"has_identifier", hasIdentifier},
         {"identifier_pattern",
          hasIdentifier ? Utils::varCString(*identifierPattern) : std::string()}});
}

/* ---------------- 可选 DFA 词法器（%cpp_lexer dfa） ---------------- */

/**
 * @brief 词法器里一条「需要编译成 DFA」的规则（参数统一收在一起）。
 */
struct CppLexRegexSpecs
{
    const std::map<std::string, std::string> *regexDefs = nullptr; ///< %token regex
    const std::string *identifier = nullptr;                       ///< %identifier
    const std::vector<CommitSpec> *commits = nullptr;              ///< %commit / %trivia
    const std::map<std::string, int> *tokenLexOrder = nullptr;     ///< 词法声明序
};

/**
 * @brief 一条正则规则的 DFA 表（`cpp17_lex_dfa_tables.cpp.tmpl` 的元素）。
 */
struct CppLexDfaRow
{
    std::string kind;        ///< `Regex` / `Identifier` / `Commit`
    std::string label;       ///< 注释里的名字（Token 名 / “标识符” / 规则号）
    std::string pattern;     ///< 原始正则（已写成 C 字符串字面量）
    std::string slot;        ///< 规则号（产物里补 `u`；%token 是它在表中的下标）
    std::string order;       ///< 词法声明序（同长度比较用）
    std::string transName;   ///< 转移表名（按规则种类定名，模板直接引用）
    std::string acceptName;  ///< 接受表名
    std::string stateCount;  ///< 状态个数（产物里补 `u`）
    bool isRegex = false;    ///< 是不是 %token regex 行（模板不支持比较，先算好）
    bool isCommit = false;   ///< 是不是 %commit / %trivia 行
    bool isIdentifier = false; ///< 是不是 %identifier 行
    std::vector<std::string> trans; ///< 转移表（按状态 × 字符类展平）
    std::vector<std::string> accept; ///< 每状态是否接受（`1u` / `0u`）
};

/**
 * @brief 可选 DFA 词法器的全部数据（表段 + 词法块要用的几段文本）。
 */
struct CppLexDfaData
{
    bool enabled = false;              ///< 是否走 DFA 词法器
    bool hasRegex = false;             ///< 有没有 %token regex（决定正则循环用不用）
    std::string classMap;              ///< 字符类快速表（256 项）
    std::string body;                  ///< 表段正文（模板渲染结果）
    std::string skipHelpers;           ///< 跳空白 / 识别码点的辅助函数
    std::string regexLoop;             ///< 词法器里的 %token regex 循环
    std::string commitLoop;            ///< 词法器里的 %commit 循环
    std::string identifierBlock;       ///< 标识符识别
};

/**
 * @brief 把一个区间列表的端点全部收集起来（用于切字符类）。
 * @param ranges 区间列表。
 * @param points 输出：端点列表（追加）。
 */
void collectRangePoints(const std::vector<std::pair<char32_t, char32_t>> &ranges,
                        std::vector<char32_t> &points)
{
    for (const auto &range : ranges)
    {
        points.push_back(range.first);
        if (range.second < 0x10FFFF)
        {
            points.push_back(static_cast<char32_t>(range.second + 1));
        }
    }
}

/**
 * @brief 判断一个码点是否落在区间列表里（区间已排序）。
 * @param ranges 区间列表。
 * @param point 码点。
 * @return 落在里面返回 true。
 */
bool rangesHold(const std::vector<std::pair<char32_t, char32_t>> &ranges,
                char32_t point)
{
    for (const auto &range : ranges)
    {
        if (point < range.first)
        {
            return false;
        }
        if (point <= range.second)
        {
            return true;
        }
    }
    return false;
}

/**
 * @brief 计算「码点 → 字符类」的两级压缩表。
 *
 * 产物里的 `lexDfaClass` 对非 ASCII 码点要回答「这个码点属于哪个类」。最省事
 * 的做法是把生成期的字符类划分抄成一张覆盖整个码点轴的字节表，但
 * U+0000..U+10FFFF 有 1.1M 项，只为了查几种字符太浪费。所以压成两级：
 * 高 8 位索引（`kLexPageIndex`）+ 每段自己的一张 256 项字节表
 * （`kLexPageData`）；整段都是「谁也不要」的码点不占表。
 * @param cells 字符类划分。
 * @param pageIndex 输出：`1 + (码点 >> 8)` → 段号（0 表示整段都是类 0）。
 * @param pageData 输出：全部段表的内容（按段号顺序摊平）。
 *
 * 类号直接用 DFA 的**原始类号**（转移表的列号），不做重映射：转移表、接受表、
 * 这里的段表必须是同一套编号，错一位就整条规则失效。「谁也不要」用 0 表示，
 * 恰好 DFA 的第 0 列通常就是空白类，但转移表里对它是陷阱，语义仍然正确。
 */
void buildClassPages(const std::vector<RegexCell> &cells,
                     std::vector<std::string> &pageIndex,
                     std::vector<std::string> &pageData)
{
    const std::size_t pageCount = 0x110000u / 256u;
    pageIndex.assign(pageCount + 1u, "0u");
    pageData.clear();
    std::map<std::string, int> dedup; // 内容相同的段共用一张表
    for (std::size_t page = 0u; page < pageCount; ++page)
    {
        const char32_t base = static_cast<char32_t>(page * 256u);
        std::vector<int> contents(256u, 0);
        bool useful = false;
        for (std::size_t offset = 0u; offset < 256u; ++offset)
        {
            const char32_t point = static_cast<char32_t>(base + offset);
            for (std::size_t index = 0u; index < cells.size(); ++index)
            {
                if (rangesHold(cells[index].ranges, point))
                {
                    contents[offset] = static_cast<int>(index);
                    useful = true;
                    break;
                }
            }
        }
        if (!useful)
        {
            continue;
        }
        std::string key;
        for (const int slot : contents)
        {
            key += std::to_string(slot);
            key.push_back(',');
        }
        int table = 0;
        const auto found = dedup.find(key);
        if (found != dedup.end())
        {
            table = found->second;
        }
        else
        {
            table = static_cast<int>(dedup.size());
            dedup.emplace(key, table);
            for (const int slot : contents)
            {
                pageData.push_back(std::to_string(slot) + "u");
            }
        }
        pageIndex[page + 1u] = std::to_string(table + 1) + "u";
    }
}

/**
 * @brief 把一条 DFA 摊平成「状态 × 字符类」的转移表与接受表。
 * @param dfa 已编译的 DFA。
 * @param classCount 字符类个数。
 * @param row 输出行（追加转移与接受项）。
 */
void fillDfaRow(const DfaProgram &dfa, int classCount, CppLexDfaRow &row)
{
    row.stateCount = std::to_string(dfa.stateCount);
    for (int state = 0; state < dfa.stateCount; ++state)
    {
        for (int cls = 0; cls < classCount; ++cls)
        {
            row.trans.push_back(
                std::to_string(dfa.transitions.at(state).at(cls)) + "u");
        }
        row.accept.push_back(dfa.accept.count(state) ? "1u" : "0u");
    }
}

/**
 * @brief 有没有以空白 / \b 通配开头的固定字面量。
 * @param tokenDefs 固定字面量 Token；可为 nullptr。
 * @return 有返回 true。
 */
bool lexerHasBlankRule(
    const std::map<std::string, std::vector<std::string>> *tokenDefs)
{
    if (tokenDefs == nullptr)
    {
        return false;
    }
    for (const auto &entry : *tokenDefs)
    {
        for (const std::string &literal : entry.second)
        {
            if (literalCanStartBlank(literal))
            {
                return true;
            }
        }
    }
    return false;
}

/**
 * @brief 组装 DFA 词法器的跳空白 / 解码辅助函数段。
 *
 * 扫描器放在 Engine::Private 中；跳空白与固定字面量辅助函数复用标准后端。
 * @param hasDfa 是否需要生成字符分类和扫描函数。
 * @return C++ 代码段。
 */
std::string renderDfaSkipHelpers(bool hasDfa = true)
{
    std::string text = tmpl::renderValues(
        Utils::loadTemplate("cpp17_lex_dfa_scan.cpp"), {{"has_dfa", hasDfa}});
    return text;
}

/**
 * @brief 判断模式是否属于与 std::regex 等价的保守 DFA 快速路径。
 *
 * 只接受 ASCII 字面量、正字符类及末尾单个贪婪量词；其余模式交给
 * 标准库，以保留字节语义、分支顺序、断言与非贪婪语义。
 * @param pattern 原始模式。
 * @return 能保证等价时返回 true。
 */
bool cppRegexDfaCompatible(const std::string &pattern)
{
    bool atom = false;
    for (std::size_t i = 0; i < pattern.size(); ++i)
    {
        const unsigned char ch = static_cast<unsigned char>(pattern[i]);
        if ((ch == 0u) || (ch >= 128u)) return false;
        if (ch == '\\')
        {
            if (++i == pattern.size()) return false;
            const char escaped = pattern[i];
            // 扩展转义、反向引用和依赖 locale 的预定义类不进入快速路径。
            if (std::string("nrtfv\\.[](){}*+?^$|-/").find(escaped) == std::string::npos)
                return false;
            atom = true;
        }
        else if (ch == '[')
        {
            const std::size_t start = ++i;
            if ((i == pattern.size()) || (pattern[i] == '^')) return false;
            for (; (i < pattern.size()) && (pattern[i] != ']'); ++i)
            {
                const unsigned char member = static_cast<unsigned char>(pattern[i]);
                if ((member == 0u) || (member >= 128u) || (member == '[') || (member == '\\'))
                    return false;
            }
            if ((i == pattern.size()) || (i == start)) return false;
            atom = true;
        }
        else if ((ch == '*') || (ch == '+') || (ch == '?'))
        {
            return atom && (i + 1u == pattern.size());
        }
        else if (std::string(".|(){}^$]").find(static_cast<char>(ch)) != std::string::npos)
            return false;
        else atom = true;
    }
    return true;
}

/**
 * @brief 组装可选 DFA 词法器的全部数据（`%cpp_lexer dfa`）。
 *
 * 生成期用 `RegexDfa` 把 %token regex / %identifier / %commit 的正则编译成
 * DFA，产物里只放 DFA 表 + 一个 `lexDfaScan`（按字符类走表、边扫边记最长
 * 接受位置）。不能保证与标准库等价时，整份词法器回退到 `<regex>`。
 * @param request 组装入参。
 * @param data 输出数据（成功时写入）。
 * @param error 出错时写入错误说明。
 * @return 成功返回 true。
 */
bool buildCppLexDfa(const ProjectRequest &request, CppLexDfaData &data,
                    Error &error)
{
    (void)request;
    const LexSpec emptyLex;
    const LexSpec &lexSpec =
        (request.lexSpec == nullptr) ? emptyLex : *request.lexSpec;

    std::vector<std::pair<std::string, std::string>> entries;
    for (const auto &entry : lexSpec.regex)
    {
        entries.emplace_back(entry.first, entry.second);
    }
    std::vector<std::pair<std::string, std::string>> patterns = entries;
    if (lexSpec.identifier.has_value())
    {
        patterns.emplace_back("__identifier__", *lexSpec.identifier);
    }
    for (std::size_t index = 0u; index < lexSpec.commits.size(); ++index)
    {
        patterns.emplace_back("__commit" + std::to_string(index) + "__",
                              lexSpec.commits[index].pattern);
    }

    for (const auto &pattern : patterns)
    {
        if (!cppRegexDfaCompatible(pattern.second))
        {
            // 整份词法器使用同一后端，Token、标识符和注释的行为保持一致。
            data.enabled = false;
            return true;
        }
    }
    data.enabled = true;
    data.hasRegex = !entries.empty();
    if (patterns.empty())
    {
        // 没有正则规则：词法器退化成「固定字面量 + 数字 + 标识符」，不需要
        // 任何 DFA 表，只需要跳空白那段（要能跳过 UTF-8 的续字节）。
        data.body = tmpl::renderValues(
            Utils::loadTemplate("cpp17_lex_dfa_tables.cpp"), {{"has_dfa", false}});
        data.skipHelpers = renderDfaSkipHelpers(false);
        return true;
    }

    RegexCompileResult compiled;
    if (!RegexDfa::compileAll(patterns, compiled, error))
    {
        return false;
    }
    const int classCount = static_cast<int>(compiled.cells.size());

    // 非 ASCII 的「码点 -> 类」压缩表（两级），类号与转移表同源。
    std::vector<std::string> pageIndex;
    std::vector<std::string> pageData;
    buildClassPages(compiled.cells, pageIndex, pageData);

    // 0..255 的快速表：单字节码点查这张就够，不再解 UTF-8。
    std::vector<std::string> classMap;
    classMap.reserve(256u);
    for (int code = 0; code < 256; ++code)
    {
        classMap.push_back(
            std::to_string(compiled.byteMap[static_cast<std::size_t>(code)]) +
            "u");
    }

    std::vector<CppLexDfaRow> rows;
    rows.reserve(compiled.compiled.size());
    int regexSlot = 0;
    int commitIndex = 0;
    for (const CompiledRegex &item : compiled.compiled)
    {
        CppLexDfaRow row;
        const bool identifier = (item.name == "__identifier__");
        const bool commit = (item.name.rfind("__commit", 0) == 0);
        if (identifier)
        {
            row.kind = "Identifier";
            row.isIdentifier = true;
            row.label = "标识符";
            row.slot = "0";
            row.order = "0";
            row.transName = "s_lexIdentifierDfaTrans";
            row.acceptName = "s_lexIdentifierDfaAccept";
        }
        else if (commit)
        {
            // %commit / %trivia 可能有好几条：每条一张表，按出现序编号。
            const std::string index = std::to_string(commitIndex++);
            row.kind = "Commit";
            row.isCommit = true;
            row.label = "commit #" + index;
            row.slot = index;
            row.order = "0";
            row.transName = "s_lexCommitDfaTrans" + index;
            row.acceptName = "s_lexCommitDfaAccept" + index;
        }
        else
        {
            const std::string index = std::to_string(regexSlot);
            row.kind = "Regex";
            row.isRegex = true;
            row.label = item.name;
            row.slot = index;
            const auto orderIt = lexSpec.tokenLexOrder.find(item.name);
            row.order = std::to_string((orderIt != lexSpec.tokenLexOrder.end())
                                           ? orderIt->second
                                           : regexSlot);
            row.transName = "s_lexTokenDfaTrans" + index;
            row.acceptName = "s_lexTokenDfaAccept" + index;
            ++regexSlot;
        }
        row.pattern = Utils::varCString(item.pattern);
        // 只是注释，别让正则里的 `*/` 把注释提前掐断
        std::string comment;
        for (std::size_t pos = 0u; pos < row.pattern.size(); ++pos)
        {
            if ((row.pattern[pos] == '*') && (pos + 1u < row.pattern.size()) &&
                (row.pattern[pos + 1u] == '/'))
            {
                comment += "* /";
                ++pos;
                continue;
            }
            comment.push_back(row.pattern[pos]);
        }
        row.pattern = std::move(comment);
        fillDfaRow(item.dfa, classCount, row);
        rows.push_back(std::move(row));
    }

    // %token regex 的元信息：DFA 只给「能匹配多长」，Token 类型与声明序靠这几张表
    std::vector<RegexEntry> regexEntries;
    for (std::size_t index = 0u; index < entries.size(); ++index)
    {
        const auto orderIt = lexSpec.tokenLexOrder.find(entries[index].first);
        RegexEntry row;
        row.pattern = Utils::varCString(entries[index].second);
        row.token = entries[index].first;
        row.order = std::to_string((orderIt != lexSpec.tokenLexOrder.end())
                                       ? orderIt->second
                                       : static_cast<int>(index));
        regexEntries.push_back(std::move(row));
    }

    // %commit 的元信息（类型 / 别名 / skip / 声明序）：DFA 表只回答「这条规则
    // 能匹配多长」，具体是什么还得靠这几张表。
    std::vector<CommitEntry> commitEntries;
    for (const CommitSpec &commit : lexSpec.commits)
    {
        CommitEntry row;
        row.pattern = Utils::varCString(commit.pattern);
        row.type = Utils::varCString(commit.type);
        row.alias = Utils::varCString(commit.alias);
        row.skip = commit.isSkip ? "1u" : "0u";
        row.order = std::to_string(commit.order);
        commitEntries.push_back(std::move(row));
    }
    if (commitEntries.empty())
    {
        commitEntries.push_back({true, "", "nullptr", "nullptr", "0u", "0"});
    }

    data.body = tmpl::renderValues(
        Utils::loadTemplate("cpp17_lex_dfa_tables.cpp"),
        {{"has_dfa", true},
         {"class_count", std::to_string(classCount)},
         {"class_map", tmpl::encode(classMap)},
         {"page_index", tmpl::encode(pageIndex)},
         {"page_data", tmpl::encode(pageData)},
         {"has_regex", data.hasRegex},
         {"regex_count", std::to_string(lexSpec.regex.size())},
         {"regex_entries", tmpl::encode(regexEntries)},
         {"has_commit", !lexSpec.commits.empty()},
         {"commit_count", std::to_string(lexSpec.commits.size())},
         {"commit_entries", tmpl::encode(commitEntries)},
         {"dfas", tmpl::encode(rows)}});
    data.skipHelpers = renderDfaSkipHelpers();
    if (data.hasRegex)
    {
        data.regexLoop =
            Utils::loadTemplate("cpp17_lexer_regex_loop_dfa.cpp");
    }
    if (!lexSpec.commits.empty())
    {
        data.commitLoop =
            Utils::loadTemplate("cpp17_lexer_commit_loop_dfa.cpp");
    }
    return true;
}

/* ---------------- 分析表 ---------------- */

/**
 * @brief Token 名称表（与枚举同序，末尾补 INVALID）。
 * @param terminals 终结符列表。
 * @return 初始化列表文本。
 */
std::string generatorTokenNamesCpp17(const std::vector<std::string> &terminals,
                                     const std::map<std::string, std::string> &aliases)
{
    // 表怎么排版由 `cpp17_token_names.cpp.tmpl` 说了算；这里只摆每行的两个词
    std::vector<TokenNameRow> names;
    for (std::size_t index = 0u; index <= terminals.size(); ++index)
    {
        const bool invalid = (index == terminals.size());
        const std::string name = invalid ? "INVALID" : terminals[index];
        const auto found = invalid ? aliases.end() : aliases.find(name);
        TokenNameRow row;
        row.name = Utils::varCString(name);
        row.alias = Utils::varCString(
            (found != aliases.end()) ? found->second : std::string());
        names.push_back(std::move(row));
    }
    return tmpl::renderValues(Utils::loadTemplate("cpp17_token_names.cpp"),
                              {{"names", tmpl::encode(names)}});
}

/**
 * @brief SLR / LALR / LR(1) / IELR 的 ACTION / GOTO 与产生式元数据表。
 * @param analysis 分析结果。
 * @return C++ 代码段。
 */
std::string generatorLrTablesCpp17(const AnalysisResult &analysis)
{
    const std::size_t stateCount = analysis.stateCount();
    std::vector<std::vector<std::string>> actionRows;
    for (std::size_t stateId = 0; stateId < stateCount; ++stateId)
    {
        std::vector<std::string> cells;
        for (std::size_t index = 0; index < analysis.terminals.size(); ++index)
        {
            const auto found = analysis.action.find(
                std::make_pair(static_cast<int>(stateId),
                               analysis.terminals[index]));
            if (found == analysis.action.end())
            {
                cells.push_back("0");
            }
            else if (found->second.kind == LrAction::Kind::Shift)
            {
                cells.push_back(std::to_string(found->second.value + 1));
            }
            else if (found->second.kind == LrAction::Kind::Reduce)
            {
                cells.push_back(std::to_string(-found->second.value - 1));
            }
            else
            {
                cells.push_back("INT32_MAX");
            }
        }
        actionRows.push_back(std::move(cells));
    }
    std::vector<std::vector<std::string>> gotoRows;
    for (std::size_t stateId = 0; stateId < stateCount; ++stateId)
    {
        std::vector<std::string> cells;
        for (std::size_t index = 0; index < analysis.nonterms.size(); ++index)
        {
            const auto found = analysis.gotoTable.find(
                std::make_pair(static_cast<int>(stateId),
                               static_cast<int>(index)));
            cells.push_back((found == analysis.gotoTable.end())
                                ? std::string("0xFFFFu")
                                : std::to_string(found->second));
        }
        gotoRows.push_back(std::move(cells));
    }
    std::vector<std::string> productionLengths;
    std::vector<std::string> productionLhs;
    std::map<std::string, int> nontermIds;
    for (std::size_t index = 0; index < analysis.nonterms.size(); ++index)
    {
        nontermIds[analysis.nonterms[index]] = static_cast<int>(index);
    }
    for (const Production &production : analysis.productions)
    {
        productionLengths.push_back(std::to_string(production.rhs.size()) + "u");
        productionLhs.push_back(
            std::to_string(nontermIds[production.lhs]) + "u");
    }
    // LR(k) / LALR(k)：第二层 ACTION（按 k 元组展望符建列的归约/接受）。
    // k = 1 时这一层不用（kLookaheadK = 1 会走单层查表），但仍要发出
    // 合法的最小表，免得模板里引用不到符号。
    const std::size_t columnCount =
        std::max<std::size_t>(1u, analysis.lookaheadColumns.size());
    const std::size_t k =
        static_cast<std::size_t>(std::max(1, analysis.lookaheadK));
    std::map<std::string, int> terminalIds;
    for (std::size_t index = 0; index < analysis.terminals.size(); ++index)
    {
        terminalIds[analysis.terminals[index]] = static_cast<int>(index);
    }
    std::vector<std::vector<std::string>> reduceRows;
    for (std::size_t stateId = 0; stateId < stateCount; ++stateId)
    {
        std::vector<std::string> cells;
        for (std::size_t column = 0; column < columnCount; ++column)
        {
            if (column >= analysis.lookaheadColumns.size())
            {
                cells.push_back("0");
                continue;
            }
            const std::string key =
                lookaheadKeyOf(analysis.lookaheadColumns[column]);
            const auto found = analysis.action.find(
                std::make_pair(static_cast<int>(stateId), key));
            if (found == analysis.action.end())
            {
                cells.push_back("0");
            }
            else if (found->second.kind == LrAction::Kind::Reduce)
            {
                cells.push_back(std::to_string(-found->second.value - 1));
            }
            else if (found->second.kind == LrAction::Kind::Accept)
            {
                cells.push_back("INT32_MAX");
            }
            else
            {
                cells.push_back("0");
            }
        }
        reduceRows.push_back(std::move(cells));
    }
    const std::size_t base = analysis.terminals.size();
    std::vector<std::pair<long long, std::size_t>> codes;
    std::vector<std::vector<std::string>> keyRows;
    for (std::size_t column = 0; column < columnCount; ++column)
    {
        std::vector<std::string> cells;
        long long code = 0;
        long long scale = 1;
        for (std::size_t index = 0; index < k; ++index)
        {
            int id = 0;
            if ((column < analysis.lookaheadColumns.size()) &&
                (index < analysis.lookaheadColumns[column].size()))
            {
                id = terminalIds[analysis.lookaheadColumns[column][index]];
            }
            cells.push_back(std::to_string(id) + "u");
            code += static_cast<long long>(id) * scale;
            scale *= static_cast<long long>(base);
        }
        keyRows.push_back(std::move(cells));
        codes.push_back({code, column});
    }
    std::sort(codes.begin(), codes.end());
    std::vector<std::string> codeList;
    std::vector<std::string> columnList;
    for (const auto &entry : codes)
    {
        codeList.push_back(std::to_string(entry.first));
        columnList.push_back(std::to_string(entry.second) + "u");
    }
    // 排版全在 `cpp17_lr_tables.cpp.tmpl` 里；这里只摆数据
    return tmpl::renderValues(
        Utils::loadTemplate("cpp17_lr_tables.cpp"),
        {{"state_count", std::to_string(stateCount)},
         {"terminal_count", std::to_string(analysis.terminals.size())},
         {"nonterm_count", std::to_string(analysis.nonterms.size())},
         {"column_count", std::to_string(columnCount)},
         {"lookahead_k", std::to_string(k)},
         {"action_rows", tmpl::encode(actionRows)},
         {"goto_rows", tmpl::encode(gotoRows)},
         {"reduce_rows", tmpl::encode(reduceRows)},
         {"key_rows", tmpl::encode(keyRows)},
         {"code_list", tmpl::encode(codeList)},
         {"column_list", tmpl::encode(columnList)},
         {"production_lengths", tmpl::encode(productionLengths)},
         {"production_lhs", tmpl::encode(productionLhs)}});
}
std::string generatorLlTablesCpp17(const AnalysisResult &analysis)
{
    // 列轴与 C 后端一致：k = 1 是 Token（密表）、k = 0 只有一列、
    // k >= 2 是「用到的 k 元组」并另发编码表
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
    std::vector<std::vector<std::string>> tableRows;
    for (std::size_t row = 0; row < analysis.nonterms.size(); ++row)
    {
        std::vector<std::string> cells;
        for (std::size_t column = 0; column < columnKeys.size(); ++column)
        {
            const auto found = analysis.table.find(
                std::make_pair(analysis.nonterms[row], columnKeys[column]));
            cells.push_back((found == analysis.table.end())
                                ? std::string("-1")
                                : std::to_string(found->second));
        }
        tableRows.push_back(std::move(cells));
    }
    std::vector<std::string> leftEmpty;
    for (const Production &production : analysis.productions)
    {
        leftEmpty.push_back((production.transform == "left_empty") ? "1u"
                                                                  : "0u");
    }
    std::map<std::string, int> terminalIds;
    for (std::size_t index = 0; index < analysis.terminals.size(); ++index)
    {
        terminalIds[analysis.terminals[index]] = static_cast<int>(index);
    }
    const std::size_t base = analysis.terminals.size();
    std::vector<std::pair<long long, std::size_t>> codes;
    if (k >= 2)
    {
        for (std::size_t column = 0; column < analysis.lookaheadColumns.size();
             ++column)
        {
            long long code = 0;
            long long scale = 1;
            for (const std::string &symbol : analysis.lookaheadColumns[column])
            {
                code += static_cast<long long>(terminalIds[symbol]) * scale;
                scale *= static_cast<long long>(base);
            }
            codes.push_back({code, column});
        }
    }
    std::sort(codes.begin(), codes.end());
    std::vector<std::string> codeList;
    std::vector<std::string> columnList;
    for (const auto &entry : codes)
    {
        codeList.push_back(std::to_string(entry.first));
        columnList.push_back(std::to_string(entry.second) + "u");
    }
    // k <= 1 时这两张表用不到（列号直接由 Token 号 / 常数 0 给出），但
    // 模板里的 llTableColumn 会引用它们，所以留一个占位元素保证可编译
    if (codeList.empty())
    {
        codeList.push_back("0");
        columnList.push_back("0u");
    }
    std::vector<std::vector<std::string>> followRows;
    if (k != 0)
    {
        for (std::size_t row = 0; row < analysis.nonterms.size(); ++row)
        {
            const auto followIt = analysis.follow.find(analysis.nonterms[row]);
            const std::set<std::string> *follow =
                followIt != analysis.follow.end() ? &followIt->second : nullptr;
            std::vector<std::string> cells;
            for (std::size_t column = 0; column < analysis.terminals.size();
                 ++column)
            {
                const bool member =
                    (follow != nullptr) &&
                    (follow->count(analysis.terminals[column]) != 0u);
                cells.push_back(member ? "true" : "false");
            }
            followRows.push_back(std::move(cells));
        }
    }
    // 表本身的排版（行 / 单元 / 分隔符、LL(0) 与其它情形两条分支）都在模板里
    return tmpl::renderValues(
        Utils::loadTemplate("cpp17_ll_tables.cpp"),
        {{"nonterm_count", std::to_string(analysis.nonterms.size())},
         {"terminal_count", std::to_string(analysis.terminals.size())},
         {"column_count",
          std::to_string(columnKeys.empty() ? 1u : columnKeys.size())},
         {"lookahead_count", std::to_string(codeList.size())},
         {"ll0", k == 0},
         {"table_rows", tmpl::encode(tableRows)},
         {"follow_rows", tmpl::encode(followRows)},
         {"code_list", tmpl::encode(codeList)},
         {"column_list", tmpl::encode(columnList)},
         {"left_empty", tmpl::encode(leftEmpty)}});
}

/**
 * @brief GLR 的冲突候选动作表与起始符号。
 *
 * 主动作在 ACTION 表里（与其它 LR 模式同一套）；这里只放「同一个 (状态, Token)
 * 单元里除主动作外的候选」，运行时按它把一个 GSS 节点分裂成多条路径。
 * 码值约定（与另外两档一致）：移进 = 状态号 + 1、归约 = -(产生式号) - 1、
 * 接受 = INT32_MAX；没有候选的单元是 0xFFFF / 0。
 * @param analysis 分析结果。
 * @return C++ 代码段。
 */
std::string generatorGlrTablesCpp17(const AnalysisResult &analysis)
{
    const std::size_t stateCount = analysis.stateCount();
    const std::size_t termCount = analysis.terminals.size();
    const std::size_t cellCount =
        (stateCount == 0u || termCount == 0u) ? 1u : stateCount * termCount;
    std::map<std::string, std::size_t> termIndex;
    for (std::size_t index = 0; index < analysis.terminals.size(); ++index)
    {
        termIndex[analysis.terminals[index]] = index;
    }
    std::vector<int> starts(cellCount, 0);
    std::vector<int> counts(cellCount, 0);
    std::vector<std::string> data;
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
        std::vector<std::string> extras;
        for (std::size_t index = 1; index < entry.second.size(); ++index)
        {
            const LrAction &action = entry.second[index];
            if (action.kind == LrAction::Kind::Shift)
            {
                extras.push_back(std::to_string(action.value + 1));
            }
            else if (action.kind == LrAction::Kind::Reduce)
            {
                extras.push_back(std::to_string(-action.value - 1));
            }
            else
            {
                extras.push_back("2147483647");
            }
        }
        if (extras.empty())
        {
            continue;
        }
        starts[cell] = static_cast<int>(data.size());
        counts[cell] = static_cast<int>(extras.size());
        for (const std::string &value : extras)
        {
            data.push_back(value);
        }
    }
    const auto startRows = [&]()
    {
        std::string text;
        for (std::size_t stateId = 0; stateId < stateCount; ++stateId)
        {
            if (stateId != 0)
            {
                text += ",\n";
            }
            text += "    { ";
            for (std::size_t index = 0; index < termCount; ++index)
            {
                if (index != 0)
                {
                    text += ", ";
                }
                text += counts[stateId * termCount + index] > 0
                            ? std::to_string(starts[stateId * termCount + index])
                            : std::string("0xFFFF");
            }
            text += " }";
        }
        return text;
    };
    const auto countRows = [&]()
    {
        std::string text;
        for (std::size_t stateId = 0; stateId < stateCount; ++stateId)
        {
            if (stateId != 0)
            {
                text += ",\n";
            }
            text += "    { ";
            for (std::size_t index = 0; index < termCount; ++index)
            {
                if (index != 0)
                {
                    text += ", ";
                }
                text += std::to_string(counts[stateId * termCount + index]) + "u";
            }
            text += " }";
        }
        return text;
    };
    std::string dataText;
    if (data.empty())
    {
        dataText = "0";
    }
    else
    {
        for (std::size_t index = 0; index < data.size(); ++index)
        {
            if (index != 0)
            {
                dataText += ", ";
            }
            dataText += data[index];
        }
    }
    // 接受状态由增广产生式 S' -> S . 触发；进入该状态的边符号是用户起始符号 S。
    std::string startSymbol = analysis.grammar.start;
    const auto startRulesIt = analysis.grammar.rules.find(analysis.grammar.start);
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
    const std::size_t rowsState = (stateCount == 0u) ? 1u : stateCount;
    const std::size_t rowsTerm = (termCount == 0u) ? 1u : termCount;
    return tmpl::renderValues(
        Utils::loadTemplate("cpp17_glr_tables.cpp"),
        {{"start_symbol", static_cast<long long>(termCount + startId)},
         {"rows_state", static_cast<long long>(rowsState)},
         {"rows_term", static_cast<long long>(rowsTerm)},
         {"rows", (stateCount == 0u || termCount == 0u) ? "    { 0 }"
                                                        : startRows()},
         {"count_rows", (stateCount == 0u || termCount == 0u) ? "    { 0 }"
                                                              : countRows()},
         {"data", dataText}});
}

/**
 * @brief 递归下降的表：非终结符 -> 产生式区间 + 语义槽位个数。
 *
 * 产生式在摊平表里「同一左部连续排在一起」（摊平时按非终结符名有序、规则内按
 * 声明顺序），所以一个非终结符对应一段连续编号——回溯时依次试的就是这个顺序。
 * @param analysis 分析结果。
 * @return C++ 代码段（三条 `inline constexpr` 数组）。
 */
std::string generatorDdTablesCpp17(const AnalysisResult &analysis)
{
    std::vector<std::string> prodStarts;
    std::vector<std::string> prodCounts;
    std::vector<std::string> prodSlots;
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
        prodStarts.push_back(std::to_string(first == total ? 0u : first));
        prodCounts.push_back(std::to_string(count));
        // 语义槽位：第 0 个留空，所以是「最长的右部 + 1」
        prodSlots.push_back(std::to_string(maxRhs + 1u));
    }
    // 排版（`Nu` 与分隔符）在 `cpp17_dd_tables.cpp.tmpl` 里
    return tmpl::renderValues(
        Utils::loadTemplate("cpp17_dd_tables.cpp"),
        {{"prod_starts", tmpl::encode(prodStarts)},
         {"prod_counts", tmpl::encode(prodCounts)},
         {"prod_slots", tmpl::encode(prodSlots)}});
}

/**
 * @brief 语法树元数据：产生式右部符号表（LL(1) 建树用）。
 * @param analysis 分析结果。
 * @return C++ 代码段。
 */
std::string generatorTreeTablesCpp17(const AnalysisResult &analysis)
{
    std::set<std::string> terminalSet(analysis.terminals.begin(),
                                      analysis.terminals.end());
    std::map<std::string, int> nontermIds;
    for (std::size_t index = 0; index < analysis.nonterms.size(); ++index)
    {
        nontermIds[analysis.nonterms[index]] = static_cast<int>(index);
    }
    const std::size_t termCount = analysis.terminals.size();
    std::vector<std::string> prodStarts;
    std::vector<std::string> prodLengths;
    std::vector<std::string> prodSymbols;
    for (const Production &production : analysis.productions)
    {
        prodStarts.push_back(std::to_string(prodSymbols.size()));
        prodLengths.push_back(std::to_string(production.rhs.size()));
        for (const std::string &symbol : production.rhs)
        {
            if (terminalSet.count(symbol) != 0u)
            {
                const auto found =
                    std::find(analysis.terminals.begin(),
                              analysis.terminals.end(), symbol);
                prodSymbols.push_back(
                    std::to_string(found - analysis.terminals.begin()));
            }
            else
            {
                prodSymbols.push_back(std::to_string(
                    termCount + static_cast<std::size_t>(nontermIds.at(symbol))));
            }
        }
    }
    if (prodSymbols.empty())
    {
        prodSymbols.push_back("0");
    }
    // 三张表一行一项；排版（`Nu` 与分隔符）在 `cpp17_tree_tables.cpp.tmpl` 里
    return tmpl::renderValues(
        Utils::loadTemplate("cpp17_tree_tables.cpp"),
        {{"prod_starts", tmpl::encode(prodStarts)},
         {"prod_lengths", tmpl::encode(prodLengths)},
         {"prod_symbols", tmpl::encode(prodSymbols)}});
}

std::string generatorChartTablesCpp17(const AnalysisResult &analysis)
{
    const ChartGrammar chart = buildChartGrammar(analysis);
    const auto encodeInts = [](const std::vector<int> &values)
    {
        std::vector<std::string> text;
        for (int value : values)
        {
            text.push_back(std::to_string(value));
        }
        if (text.empty())
        {
            text.push_back("0");
        }
        return tmpl::encode(text);
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
        Utils::loadTemplate("cpp17_chart_tables.cpp"),
        {{"start", chart.startNonterm},
         {"terminal_count", chart.terminalCount},
         {"nonterm_count", chart.nontermCount},
         {"production_count", static_cast<int>(chart.lhs.size())},
         {"unary_count", static_cast<int>(unaryLhs.size())},
         {"binary_count", static_cast<int>(binaryLhs.size())},
         {"accepts_empty", chart.acceptsEmpty},
         {"lhs", encodeInts(chart.lhs)},
         {"rhs_start", encodeInts(chart.rhsStart)},
         {"rhs_length", encodeInts(chart.rhsLength)},
         {"rhs_symbols", encodeInts(chart.rhsSymbols)},
         {"unary_lhs", encodeInts(unaryLhs)},
         {"unary_token", encodeInts(unaryToken)},
         {"binary_lhs", encodeInts(binaryLhs)},
         {"binary_left", encodeInts(binaryLeft)},
         {"binary_right", encodeInts(binaryRight)}});
}

/**
 * @brief 生成产生式展开分支（仅 LL(1)）。
 * @param analysis 分析结果。
 * @param terminals 终结符列表。
 * @param nontermIds 非终结符到编号的映射。
 * @return C++ 代码段。
 */
std::string generatorExpandCasesCpp17(
    const AnalysisResult &analysis, const std::vector<std::string> &terminals,
    const std::map<std::string, int> &nontermIds)
{
    std::set<std::string> terminalSet(terminals.begin(), terminals.end());
    std::vector<LlExpandCase> cases;
    for (const Production &production : analysis.productions)
    {
        const std::vector<std::string> &rhs = production.rhs;
        const std::string &transform = production.transform;
        LlExpandCase item;
        item.id = std::to_string(production.id);
        const auto push = [&item](const std::string &kind,
                                  const std::string &symbol, bool terminal,
                                  int slot)
        {
            LlExpandItem entry;
            entry.kind = kind;
            entry.symbol = symbol;
            entry.terminal = terminal;
            entry.slot = std::to_string(slot);
            item.items.push_back(std::move(entry));
        };
        const auto pushSymbol = [&terminalSet, &nontermIds, &push](
                                    const std::string &symbol, int slot)
        {
            if (terminalSet.count(symbol) != 0u)
            {
                push("ItemKind::Terminal", symbol, true, slot);
                return;
            }
            push("ItemKind::Nonterm", std::to_string(nontermIds.at(symbol)),
                 false, slot);
        };
        std::vector<std::string> symbols;
        int startSlot = 1;
        if (transform == "left_base")
        {
            const int baseLength = production.baseRhsLength;
            push("ItemKind::LeftLoop",
                 std::to_string(nontermIds.at(rhs.back())), false, 0);
            push("ItemKind::ActionKeep", std::to_string(production.id), false,
                 0);
            symbols.assign(
                rhs.begin(),
                rhs.begin() + static_cast<std::ptrdiff_t>(baseLength));
        }
        else if (transform == "left_continue")
        {
            symbols.assign(rhs.begin(), rhs.end() - 1);
            push("ItemKind::LeftLoop",
                 std::to_string(nontermIds.at(rhs.back())), false, 0);
            push("ItemKind::ActionKeep", std::to_string(production.id), false,
                 0);
            startSlot = 2;
        }
        else if (transform == "left_empty")
        {
            LlExpandItem entry;
            entry.note = true;
            item.items.push_back(std::move(entry));
        }
        else
        {
            symbols = rhs;
            push("ItemKind::Action", std::to_string(production.id), false, 0);
        }
        for (std::size_t index = symbols.size(); index-- > 0u;)
        {
            pushSymbol(symbols[index], startSlot + static_cast<int>(index));
        }
        cases.push_back(std::move(item));
    }
    return tmpl::renderValues(Utils::loadTemplate("cpp17_expand_cases.cpp"),
                              {{"cases", tmpl::encode(cases)}});
}

/**
 * @brief 生成 %sync 的同步 Token 判定分支（没写 %sync 时返回空串）。
 * @param terminals 终结符列表。
 * @param syncTokens %sync 声明的同步 Token；可为 nullptr。
 * @return `case TOKEN_X: return true;` 行。
 */
std::string generatorSyncCasesCpp17(const std::vector<std::string> &terminals,
                                    const std::vector<std::string> *syncTokens)
{
    if (syncTokens == nullptr)
    {
        return "";
    }
    // 只挑真的声明成终结符的那些（`cpp17_sync_cases.cpp.tmpl` 管排版）
    std::vector<std::string> tokens;
    for (const std::string &token : *syncTokens)
    {
        if (std::find(terminals.begin(), terminals.end(), token) !=
            terminals.end())
        {
            tokens.push_back(token);
        }
    }
    // 每个分支都要 return true：只写 case 标签会直接落到 default 的
    // return false，%sync 就永远不生效（C 目标的生成器有这一句）
    return tmpl::renderValues(Utils::loadTemplate("cpp17_sync_cases.cpp"),
                              {{"tokens", tmpl::encode(tokens)}});
}


/**
 * @brief 把文本按换行切开（**保留**末尾的空段：缩进时才能与原来逐字节一致）。
 * @param text 输入文本。
 * @return 每一行（不含换行）。
 */
std::vector<std::string> splitLines(const std::string &text)
{
    std::vector<std::string> lines;
    std::size_t begin = 0u;
    while (true)
    {
        const std::size_t stop = text.find('\n', begin);
        if (stop == std::string::npos)
        {
            lines.push_back(text.substr(begin));
            break;
        }
        lines.push_back(text.substr(begin, stop - begin));
        begin = stop + 1u;
    }
    return lines;
}

/**
 * @brief %destructor 的一个 %union 成员分组
 *        （`cpp17_destructor_dispatch.cpp.tmpl` 的数组元素）。
 */
struct DestructorGroup
{
    std::string member;                  ///< %union 成员名
    std::vector<std::string> symbols;    ///< 绑到它、会被丢到栈上的符号名
    std::vector<std::string> caseLabels; ///< 统一符号编号（产物里 `case Nu:`）
    std::vector<std::string> bodyLines;  ///< 清理片段按行切开（空片段为空）
};

} // namespace
} // namespace ParseEngine

// 字段绑定只能写在全局作用域（`TEMPLATE_MODEL` 展开的是显式特化）
TEMPLATE_MODEL(ParseEngine::DestructorGroup,
               tmpl::field("member", &ParseEngine::DestructorGroup::member),
               tmpl::field("symbols",
                           &ParseEngine::DestructorGroup::symbols),
               tmpl::field("case_labels",
                           &ParseEngine::DestructorGroup::caseLabels),
               tmpl::field("body_lines",
                           &ParseEngine::DestructorGroup::bodyLines));

TEMPLATE_MODEL(ParseEngine::KeywordRow,
               tmpl::field("text", &ParseEngine::KeywordRow::text),
               tmpl::field("token", &ParseEngine::KeywordRow::token));

TEMPLATE_MODEL(ParseEngine::LexRuleRow,
               tmpl::field("text", &ParseEngine::LexRuleRow::text),
               tmpl::field("length", &ParseEngine::LexRuleRow::length),
               tmpl::field("token", &ParseEngine::LexRuleRow::token),
               tmpl::field("blankFirst", &ParseEngine::LexRuleRow::blankFirst),
               tmpl::field("wildcard", &ParseEngine::LexRuleRow::wildcard),
               tmpl::field("order", &ParseEngine::LexRuleRow::order));

TEMPLATE_MODEL(ParseEngine::TokenNameRow,
               tmpl::field("name", &ParseEngine::TokenNameRow::name),
               tmpl::field("alias", &ParseEngine::TokenNameRow::alias));

TEMPLATE_MODEL(ParseEngine::LlExpandItem,
               tmpl::field("note", &ParseEngine::LlExpandItem::note),
               tmpl::field("terminal", &ParseEngine::LlExpandItem::terminal),
               tmpl::field("kind", &ParseEngine::LlExpandItem::kind),
               tmpl::field("symbol", &ParseEngine::LlExpandItem::symbol),
               tmpl::field("slot", &ParseEngine::LlExpandItem::slot));

TEMPLATE_MODEL(ParseEngine::LlExpandCase,
               tmpl::field("id", &ParseEngine::LlExpandCase::id),
               tmpl::field("items", &ParseEngine::LlExpandCase::items));

TEMPLATE_MODEL(ParseEngine::RegexEntry,
               tmpl::field("pattern", &ParseEngine::RegexEntry::pattern),
               tmpl::field("token", &ParseEngine::RegexEntry::token),
               tmpl::field("order", &ParseEngine::RegexEntry::order));

TEMPLATE_MODEL(ParseEngine::RegexMember,
               tmpl::field("token", &ParseEngine::RegexMember::token),
               tmpl::field("member", &ParseEngine::RegexMember::member));

TEMPLATE_MODEL(ParseEngine::CommitEntry,
               tmpl::field("placeholder",
                           &ParseEngine::CommitEntry::placeholder),
               tmpl::field("pattern", &ParseEngine::CommitEntry::pattern),
               tmpl::field("type", &ParseEngine::CommitEntry::type),
               tmpl::field("alias", &ParseEngine::CommitEntry::alias),
               tmpl::field("skip", &ParseEngine::CommitEntry::skip),
               tmpl::field("order", &ParseEngine::CommitEntry::order));

/**
 * @brief `CppLexDfaRow` 的字段绑定（模板 `cpp17_lex_dfa_tables.cpp` 用）。
 *
 * 表名在 C++ 侧就定好了（`trans_name` / `accept_name`），模板里只写
 * `{{ dfa.trans_name }}`——模板引擎的 `{% if %}` 只认真值，语言里没有表达式拼接。
 */
TEMPLATE_MODEL(ParseEngine::CppLexDfaRow,
               tmpl::field("kind", &ParseEngine::CppLexDfaRow::kind),
               tmpl::field("label", &ParseEngine::CppLexDfaRow::label),
               tmpl::field("pattern", &ParseEngine::CppLexDfaRow::pattern),
               tmpl::field("slot", &ParseEngine::CppLexDfaRow::slot),
               tmpl::field("order", &ParseEngine::CppLexDfaRow::order),
               tmpl::field("trans_name", &ParseEngine::CppLexDfaRow::transName),
               tmpl::field("accept_name", &ParseEngine::CppLexDfaRow::acceptName),
               tmpl::field("state_count", &ParseEngine::CppLexDfaRow::stateCount),
               tmpl::field("is_regex", &ParseEngine::CppLexDfaRow::isRegex),
               tmpl::field("is_commit", &ParseEngine::CppLexDfaRow::isCommit),
               tmpl::field("is_identifier",
                           &ParseEngine::CppLexDfaRow::isIdentifier),
               tmpl::field("trans", &ParseEngine::CppLexDfaRow::trans),
               tmpl::field("accept", &ParseEngine::CppLexDfaRow::accept));

namespace ParseEngine
{
namespace
{

/**
 * @brief 生成 %destructor 的按符号分派函数（C++17 产物）。
 *
 * 统一符号编号（终结符 = Token 号，非终结符 = 终结符个数 + 编号）→ 该符号用
 * `%token <成员>` / `%type <成员>` 绑定的成员 → 该成员的清理片段，片段里的 $$
 * 展开成 pValue->成员。C++17 没有 C 那边的 PARSER_USER_DISCARD_VALUE 钩子
 * （RAII 类型本来就自己管内存），所以没绑 %destructor 的符号什么都不做。
 * @param analysis 分析结果。
 * @param symbolMember 符号 -> %union 成员表；没写 %union 时传 nullptr。
 * @param destructors 成员 -> 清理片段；nullptr / 空表示没写 %destructor。
 * @param error 出错时写入错误说明。
 * @return 函数文本；没写 %destructor（或没有符号绑到那些成员）时返回空串。
 */
std::string generatorDestructorDispatchCpp17(
    const AnalysisResult &analysis,
    const std::map<std::string, std::string> *symbolMember,
    const std::map<std::string, std::string> *destructors, Error &error)
{
    // 分组算表在这儿；产物长什么样由 `cpp17_destructor_dispatch.cpp.tmpl` 说了算
    // （注释行、`case Nu:` 列表、片段正文的缩进都在模板里）。
    const std::map<std::string,
                     std::vector<std::pair<std::string, std::size_t>>>
        groups = CodeGen::destructorSymbolGroups(analysis, symbolMember,
                                                destructors);
    std::vector<DestructorGroup> bound;
    for (const auto &entry : groups)
    {
        const std::vector<std::pair<std::string, std::size_t>> &slots =
            entry.second;
        if (slots.empty())
        {
            continue;
        }

        const auto bodyIt = destructors->find(entry.first);
        const std::string *body =
            bodyIt != destructors->end() ? &bodyIt->second : nullptr;
        std::string translated;
        if ((body == nullptr) ||
            !translateDestructorBody(*body, entry.first,
                                     "pValue->" + entry.first, translated,
                                     error))
        {
            return std::string();
        }
        DestructorGroup group;

        group.member = entry.first;
        for (const auto &slot : slots)
        {
            group.symbols.push_back(slot.first);
            group.caseLabels.push_back(std::to_string(slot.second) + "u");
        }
        // 空片段在产物里一行都不出（原实现是 if (!indented.empty())）
        if (!translated.empty())
        {
            group.bodyLines = splitLines(translated);
        }
        bound.push_back(std::move(group));
    }
    if (bound.empty())
    {
        return std::string();
    }
    return tmpl::renderValues(
        Utils::loadTemplate("cpp17_destructor_dispatch.cpp"),
        {{"groups", tmpl::encode(bound)}});
}

/* ---------------- 词法块 ---------------- */


/**
 * @brief 词法器的固定字面量命中判断 / 跳字符判断（C++17）。
 * @param spaceChars %space 声明的跳过字符集；nullptr 表示没写。
 * @param hasBlankRule 是否有以空白 / \b 通配开头的固定字面量。
 * @return C++ 代码段。
 */
std::string cpp17LexerSkipHelpers(const std::string *spaceChars,
                                  bool hasBlankRule)
{
    // 生成哪些辅助函数、函数体长什么样都在模板里
    // （`cpp17_lexer_skip_helpers.cpp.tmpl` 的 `{% if %}` 自己决定）；
    // 这里只把「有没有 %space」「有没有以空白开头的字面量」摆成值。
    const bool hasSkipChar = (spaceChars != nullptr) || hasBlankRule;
    std::string skipCharBody;
    if (hasSkipChar)
    {
        skipCharBody = tmpl::renderValues(
            Utils::loadTemplate("cpp17_lexer_skip_char.cpp"),
            {{"has_space_chars", spaceChars != nullptr},
             {"space_chars", (spaceChars != nullptr)
                                 ? Utils::varCString(*spaceChars)
                                 : std::string()}});
    }
    return tmpl::renderValues(
        Utils::loadTemplate("cpp17_lexer_skip_helpers.cpp"),
        {{"has_skip_char", hasSkipChar},
         {"has_blank_rule", hasBlankRule},
         {"skip_char_body", skipCharBody}});
}

/**
 * @brief 取 Token 前的跳空白循环（C++17）。
 * @param spaceChars %space 声明的跳过字符集；nullptr 表示没写。
 * @param hasBlankRule 是否有以空白 / \b 通配开头的固定字面量。
 * @return C++ 代码段。
 */
std::string cpp17LexerSkipLoop(const std::string *spaceChars,
                               bool hasBlankRule)
{
    // 缺省那支（既没写 %space、也没有以空白开头的字面量）是纯静态片段，原样取出；
    // 另一支里「命中空白字面量就停手」那几句由模板的 `{% if has_blank_rule %}` 决定。
    if ((spaceChars == nullptr) && !hasBlankRule)
    {
        return Utils::loadTemplate("cpp17_lexer_skip_loop_plain.cpp");
    }
    return tmpl::renderValues(
        Utils::loadTemplate("cpp17_lexer_skip_loop.cpp"),
        {{"has_blank_rule", hasBlankRule}});
}

/// 词法器各段的替换表。
struct LexerBlocks
{
    std::string stringBlock;     ///< 内建双引号字符串
    std::string matchBlock;      ///< 固定字面量 + 正则最长匹配
    std::string numberBlock;     ///< 数字
    std::string identifierBlock; ///< 标识符
};

/**
 * @brief 组装词法器各段（含 %union 的语义值写入）。
 * @param request 组装入参。
 * @param blocks 输出各段文本。
 * @param dfa 可选 DFA 词法器的数据；nullptr 表示走 std::regex。
 * @return 成功返回 true。
 */
bool buildLexerBlocks(const ProjectRequest &request, LexerBlocks &blocks,
                      const CppLexDfaData *dfa = nullptr)
{
    const std::map<std::string, std::vector<std::string>> emptyTokenDefs;
    const auto &tokenDefs =
        (request.tokenDefs == nullptr) ? emptyTokenDefs : *request.tokenDefs;
    const LexSpec emptyLex;
    const LexSpec &lexSpec =
        (request.lexSpec == nullptr) ? emptyLex : *request.lexSpec;
    const std::map<std::string, std::string> *symbolMember =
        request.symbolValueMember;
    const bool unioned = unionMode(request);
    const auto overridden = [&tokenDefs, &lexSpec](const std::string &name)
    {
        return tokenDefs.count(name) || lexSpec.regex.count(name);
    };

    // 内建字符串：用户自己声明了 STRING 的词法规则时不生成
    if (!overridden("STRING"))
    {
        blocks.stringBlock = tmpl::renderValues(
            Utils::loadTemplate("cpp17_lexer_string.cpp"),
            {{"union_member", unioned
                                 ? memberOrEmpty(symbolMember, "STRING")
                                 : std::string()}});
    }

    // 固定字面量与 %token regex 一起做最长匹配；%union 时命中的词素要写进
    // 对应成员的语义值，那份 switch 在 `cpp17_lexer_match.cpp.tmpl` 里
    std::string regexLoop;
    std::vector<RegexMember> regexMembers;
    if (!lexSpec.regex.empty())
    {
        regexLoop = (dfa != nullptr)
                        ? dfa->regexLoop
                        : Utils::loadTemplate("cpp17_lexer_regex_loop.cpp");
        if (unioned)
        {
            for (const auto &entry : lexSpec.regex)
            {
                const std::string member =
                    memberOrEmpty(symbolMember, entry.first);
                if (member.empty())
                {
                    continue;
                }
                regexMembers.push_back({entry.first, member});
            }
        }
    }
    blocks.matchBlock = tmpl::renderValues(
        Utils::loadTemplate("cpp17_lexer_match.cpp"),
        {{"lex_regex_loop", regexLoop},
         {"lex_commit_loop",
          (dfa != nullptr) ? dfa->commitLoop
                           : Utils::loadTemplate("cpp17_lexer_commit_loop.cpp")},
         {"has_regex_loop", !regexLoop.empty()},
         {"has_regex_members", !regexMembers.empty()},
         {"regex_members", tmpl::encode(regexMembers)}});

    // 数字：用户自己声明了 NUMBER 的词法规则时不生成。
    // 「%union 绑了成员 / 写了 %union 但没绑 / 不是 %union」三种写法在模板里。
    if (!overridden("NUMBER"))
    {
        const std::string numberMember =
            unioned ? memberOrEmpty(symbolMember, "NUMBER") : std::string();
        blocks.numberBlock = tmpl::renderValues(
            Utils::loadTemplate("cpp17_lexer_number.cpp"),
            {{"union_number", unioned}, {"number_member", numberMember}});
    }

    // 标识符：%identifier 的 DFA 优先；用户自己声明了 VARIABLE 时两条都跳过
    const std::string variableMember =
        unioned ? memberOrEmpty(symbolMember, "VARIABLE") : std::string();
    if (lexSpec.identifier.has_value())
    {
        blocks.identifierBlock = tmpl::renderValues(
            Utils::loadTemplate((dfa != nullptr)
                                    ? "cpp17_lexer_identifier_dfa_scan.cpp"
                                    : "cpp17_lexer_identifier_dfa.cpp"),
            {{"union_member", variableMember}});
    }
    else if (!overridden("VARIABLE"))
    {
        blocks.identifierBlock = tmpl::renderValues(
            Utils::loadTemplate("cpp17_lexer_identifier.cpp"),
            {{"union_member", variableMember}});
    }
    return true;
}

/**
 * @brief 词法表段（Token 名表 + 固定字面量规则 + 正则表）。
 *
 * 解析产物与词法模式共用；`terminals` 是 Token 列表（解析产物用
 * `analysis.terminals`，词法模式用 project.cpp 算好的那张表）。
 * 这一段没有失败路径：正则认不下只是在 `generatorLexRegexCpp17` 里打警告。
 * @param request 组装入参。
 * @param terminals Token 列表。
 * @param out 追加写入的表段。
 * @param dfa 可选 DFA 词法器的数据；nullptr 表示走 std::regex。
 */
void buildLexTables(const ProjectRequest &request,
                    const std::vector<std::string> &terminals,
                    std::string &out, const CppLexDfaData *dfa = nullptr)
{
    const std::map<std::string, std::vector<std::string>> emptyTokenDefs;
    const auto &tokenDefs =
        (request.tokenDefs == nullptr) ? emptyTokenDefs : *request.tokenDefs;
    const LexSpec emptyLex;
    const LexSpec &lexSpec =
        (request.lexSpec == nullptr) ? emptyLex : *request.lexSpec;
    out += generatorTokenNamesCpp17(terminals, lexSpec.tokenAliases);
    out += tmpl::renderValues(
        Utils::loadTemplate("cpp17_lex_rule_decl.cpp"),
        {{"lex_rules",
          generatorLexRulesCpp17(tokenDefs, lexSpec.tokenLexOrder)}});
    out += generatorKeywordCpp17(tokenDefs);
    if (dfa != nullptr)
    {
        out += dfa->body;
        return;
    }
    generatorLexRegexCpp17(lexSpec.regex, lexSpec.identifier, lexSpec.commits,
                           lexSpec.tokenLexOrder, out);
}

/**
 * @brief 把三段表填进 cpp17_tables.cpp 模板。
 * @param lexTables 词法表段。
 * @param analysisTables 分析表段（词法模式为空）。
 * @param treeTables 语法树表段（没有时为空）。
 * @return 表段文本。
 */
std::string wrapTables(const std::string &lexTables,
                       const std::string &analysisTables,
                       const std::string &treeTables,
                       const std::string &modeTables = std::string())
{
    return tmpl::renderValues(Utils::loadTemplate("cpp17_tables.cpp"),
                              {{"lex_tables", lexTables},
                               {"analysis_tables", analysisTables},
                               {"tree_tables", treeTables},
                               // 「某个模式专属的表」：GLR 是冲突候选、DD 是
                               // 递归下降的产生式区间，两者不会同时出现
                               {"mode_tables", modeTables}});
}

/**
 * @brief 组装 parser.cpp 里未命名命名空间的表段。
 *
 * 这一段没有失败路径（词法段的正则认不下只打警告）。
 * @param request 组装入参。
 * @param mode 归一化后的生成模式。
 * @param analysis 分析结果。
 * @param out 输出表段。
 */
void buildTables(const ProjectRequest &request, const std::string &mode,
                 const AnalysisResult &analysis, std::string &out,
                 const CppLexDfaData *dfa = nullptr)
{
    std::string lexTables;
    buildLexTables(request, analysis.terminals, lexTables, dfa);
    std::string analysisTables = "/* ---------- 分析表 ---------- */\n";
    if (isLrMode(mode))
    {
        analysisTables += generatorLrTablesCpp17(analysis);
    }
    else
    {
        analysisTables += generatorLlTablesCpp17(analysis);
    }
    // 语法树表：只有 LL(1) 按右部符号表建树
    std::string treeTables;
    if (!isLrMode(mode))
    {
        treeTables = "/* ---------- 语法树表 ---------- */\n" +
                     generatorTreeTablesCpp17(analysis);
    }
    std::string modeTables;
    if (isGlrMode(mode))
    {
        modeTables = "/* ---------- GLR 冲突候选 ---------- */\n" +
                     generatorGlrTablesCpp17(analysis);
        if ((mode == "earley") || (mode == "cyk"))
        {
            modeTables += generatorChartTablesCpp17(analysis);
        }
    }
    else if (isDdMode(mode))
    {
        modeTables = "/* ---------- 递归下降表 ---------- */\n" +
                     generatorDdTablesCpp17(analysis);
    }
    out = wrapTables(lexTables, analysisTables, treeTables, modeTables);
}

} // namespace

std::string CodeGenCpp17::namespaceName(const ProjectRequest &request)
{
    if (request.cppNamespace.empty())
    {
        /* 没写 %namespace 时产物的默认命名空间：与文档 / 兼容层一致（Parser_Xxx、
         * ParserValue_T 这些名字都在这个命名空间里）。不要改成项目名——改名脚本
         * 曾经把这里连带改成过项目名，产物对不上文档。 */
        return "Parser";
    }
    return request.cppNamespace;
}

std::string CodeGenCpp17::filePrefix(const ProjectRequest &request)
{
    if (request.cppNamespace.empty())
    {
        /* 默认**不加**文件名前缀：产物就是 parser.hpp / parser.cpp / main.cpp /
         * parser_user.h，生成的 Makefile / CMakeLists 与 RESULT.md 也都按这套
         * 名字写。只有显式写了 %namespace 才加 `<namespace>_` 前缀（那时
         * renameProductFiles 会把产物正文与构建脚本里的名字一起改掉）。 */
        return "";
    }
    return request.cppNamespace + "_";
}

std::string CodeGenCpp17::hppName(const ProjectRequest &request)
{
    return filePrefix(request) + "parser.hpp";
}

std::string CodeGenCpp17::parserSourceName(const ProjectRequest &request)
{
    return filePrefix(request) + "parser.cpp";
}

std::string CodeGenCpp17::mainSourceName(const ProjectRequest &request)
{
    return filePrefix(request) + "main.cpp";
}

std::string CodeGenCpp17::lexerHppName(const ProjectRequest &request)
{
    return filePrefix(request) + "lexer.hpp";
}

std::string CodeGenCpp17::lexerSourceName(const ProjectRequest &request)
{
    return filePrefix(request) + "lexer.cpp";
}

std::string CodeGenCpp17::userHeaderName(const ProjectRequest &request)
{
    return filePrefix(request) + "parser_user.h";
}

std::optional<std::string> CodeGenCpp17::userHeader(
    const ProjectRequest &request, const std::string &userCodeWithLines)
{
    if (!hasVisibleText(userCodeWithLines))
    {
        return std::nullopt;
    }
    std::string text = tmpl::renderValues(
        Utils::loadTemplate("cpp17_user_header.cpp"),
        {{"user_code", userCodeWithLines}});
    if (!text.empty() && (text.back() != '\n'))
    {
        text += "\n";
    }
    (void)request;
    return text;
}

bool CodeGenCpp17::parserHpp(const ProjectRequest &request,
                             const std::string &mode,
                             const AnalysisResult &analysis,
                             const std::string &unionBodyWithLines,
                             std::string &out, Error &error)
{
    (void)error;
    const std::string nameSpace = namespaceName(request);
    const bool unioned = unionMode(request);
    const std::vector<std::string> &terminals = analysis.terminals;

    // 用户头文件：只有写了 %code ... %endcode 时才生成 / 包含
    std::string userInclude;
    if (hasVisibleText(request.userCode))
    {
        userInclude = tmpl::renderValues(
            Utils::loadTemplate("cpp17_user_include.hpp"),
            {{"header_name", userHeaderName(request)}});
    }

    // 容量常量（%max_stack_depth / %max_errors / %error_recovery_tokens）
    const std::size_t maxStackDepth = static_cast<std::size_t>(
        request.maxStackDepth.value_or(64));
    const std::size_t maxErrors =
        static_cast<std::size_t>(request.maxErrors.value_or(8));
    const std::size_t recoveryTokens = static_cast<std::size_t>(
        request.errorRecoveryTokens.value_or(3));
    std::size_t maxSlots = 4u;
    for (const Production &production : analysis.productions)
    {
        maxSlots = std::max(maxSlots, production.rhs.size() + 3u);
    }
    const std::string constants = tmpl::renderValues(
        Utils::loadTemplate("cpp17_constants.hpp"),
        {{"lex", false},
         {"value_slots", std::to_string(maxSlots)},
         {"max_stack_depth", std::to_string(maxStackDepth)},
         {"max_errors", std::to_string(maxErrors)},
         {"recovery_tokens", std::to_string(recoveryTokens)}});

    // 语义值类型 + Token 类型枚举：三套写法都在模板里（`cpp17_value_type_decl.hpp`
    // / `cpp17_token_value_decl.hpp`），这里只决定用哪一套、把哪些值摆进去
    std::string valueTypeDecl;
    std::string tokenValueDecl;
    if (unioned)
    {
        // %union 正文的 #line 指向 .gram（收尾时由 resolveLineDirectives 落地）
        valueTypeDecl = tmpl::renderValues(
            Utils::loadTemplate("cpp17_value_type_decl.hpp"),
            {{"union_mode", true},
             {"union_body", (unionBodyWithLines.empty() ? *request.unionBody
                                                        : unionBodyWithLines)},
             {"value_type", std::string()}});
        tokenValueDecl = tmpl::renderValues(
            Utils::loadTemplate("cpp17_token_value_decl.hpp"),
            {{"union_mode", true}});
    }
    else if ((request.valueType != nullptr) &&
             hasVisibleText(*request.valueType))
    {
        valueTypeDecl = tmpl::renderValues(
            Utils::loadTemplate("cpp17_value_type_decl.hpp"),
            {{"union_mode", false},
             {"union_body", std::string()},
             {"value_type", *request.valueType}});
        tokenValueDecl = tmpl::renderValues(
            Utils::loadTemplate("cpp17_token_value_decl.hpp"),
            {{"union_mode", false}});
    }
    else if (hasVisibleText(request.userCode))
    {
        // 写了 %code 又没写 %value_type / %union：ParserValue_T 由
        // 用户在 parser_user.h 里自己给（也可能来自 %copy 进来的头文件，例如
        // lib/parser_value.h）。parser.hpp 在打开命名空间之前包含 parser_user.h，
        // 所以命名空间里**不能**再补一个默认 typedef——那会把用户的类型遮蔽掉。
        // 这里与 C 产物（CodeGen::customParserH）的判断保持一致。
        tokenValueDecl = tmpl::renderValues(
            Utils::loadTemplate("cpp17_token_value_decl.hpp"),
            {{"union_mode", false}});
    }
    else
    {
        valueTypeDecl = tmpl::renderValues(
            Utils::loadTemplate("cpp17_value_type_decl.hpp"),
            {{"union_mode", false},
             {"union_body", std::string()},
             {"value_type", "int"}});
        tokenValueDecl = tmpl::renderValues(
            Utils::loadTemplate("cpp17_token_value_decl.hpp"),
            {{"union_mode", false}});
    }
    std::vector<std::string> tokenNames;
    for (const std::string &terminal : terminals)
    {
        tokenNames.push_back("TOKEN_" + terminal);
    }
    tokenNames.push_back("TOKEN_INVALID");
    const std::string tokenTypes = tmpl::renderValues(
        Utils::loadTemplate("cpp17_token_types.hpp"),
        {{"value_type_decl", valueTypeDecl},
         {"tokens", tmpl::encode(tokenNames)},
         {"token_name_count", std::to_string(terminals.size() + 1u)}});

    // 语法树接口：写了 %tree 才有 Engine::parseTree 的声明
    std::string engineTreeApi;
    if (request.tree)
    {
        engineTreeApi = Utils::loadTemplate("cpp17_engine_tree_api.hpp");
    }

    // 数据摆成「名字 -> 值」交给 cpp17_parser.hpp.tmpl 排版；`tree_api` /
    // `compat_api` 是整份片段模板，模板里各占一整行，所以照样按值传进去
    // （换成 {% include %} 会因为「块标签独占一行整行删掉」少一个换行）。
    const bool lrIncrementalMode = isLrMode(mode) && !isGlrMode(mode);
    const bool incrementalMode =
        lrIncrementalMode || isDdMode(mode) ||
        (!isLrMode(mode) && !isLexMode(mode));
    const std::string engineParseApi = tmpl::renderValues(
        Utils::loadTemplate("cpp17_engine_parse_api.hpp"),
        {{"incremental", incrementalMode}});

    std::string text = tmpl::renderValues(
        Utils::loadTemplate("cpp17_parser.hpp"),
        {{"mode_name", modeLabelText(mode)},
         {"ns", nameSpace},
         {"guard", guardNameOf(hppName(request))},
         {"user_include", userInclude},
         {"constants", constants},
         {"token_types", tokenTypes},
         {"token_value_decl", tokenValueDecl},
         {"tree_api", Utils::loadTemplate("cpp17_tree_api.hpp")},
         {"engine_parse_api", engineParseApi},
         {"engine_tree_api", engineTreeApi},
         {"engine_glr_resolver",
          isGlrMode(mode) ? tmpl::renderValues(
                                Utils::loadTemplate("cpp17_glr_resolver.hpp"),
                                {{"value_type", "ParserValue_T"}})
                          : std::string()},
         {"engine_glr_api",
          isGlrMode(mode) ? tmpl::renderValues(
                                Utils::loadTemplate("cpp17_glr_api.hpp"),
                                {{"value_type", "ParserValue_T"}})
                          : std::string()},
         {"compat_api", Utils::loadTemplate("cpp17_compat.hpp")},
         {"dd", isDdMode(mode)},
         {"incremental", incrementalMode},
         {"lr_incremental", lrIncrementalMode},
         {"glr", isGlrMode(mode)},
         {"lex", false}});
    if (!text.empty() && (text.back() != '\n'))
    {
        text += "\n";
    }
    out = std::move(text);
    return true;
}

bool CodeGenCpp17::parserCpp(const ProjectRequest &request,
                             const std::string &mode,
                             const AnalysisResult &analysis, std::string &out,
                             Error &error)
{
    const std::string nameSpace = namespaceName(request);
    const std::vector<std::string> &terminals = analysis.terminals;
    const std::vector<std::string> &nonterms = analysis.nonterms;
    const bool lr = isLrMode(mode);
    const bool unioned = unionMode(request);
    // %destructor：统一符号号 → %union 成员 → 清理片段的分派函数（LR / LL
    // 共用；没写 %destructor、或没有符号绑到那些成员时是空串）
    const std::string destructorDispatch = generatorDestructorDispatchCpp17(
        analysis, request.symbolValueMember, request.destructors, error);
    if (!error.empty())
    {
        return false;
    }
    const bool hasDestructors = !destructorDispatch.empty();

    // 可选 DFA 词法器：%cpp_lexer dfa / --cpp-lexer dfa 时把 %token regex /
    // %identifier / %commit 的等价子集在生成期编译成 DFA 表，其余回退标准库。
    CppLexDfaData dfaData;
    const CppLexDfaData *dfa = nullptr;
    if (request.cppLexer == "dfa")
    {
        if (!buildCppLexDfa(request, dfaData, error))
        {
            return false;
        }
        dfa = dfaData.enabled ? &dfaData : nullptr;
    }

    std::string tables;
    buildTables(request, mode, analysis, tables, dfa);

    // 未命名命名空间里的辅助函数：名字表逐行交给 `cpp17_helpers.cpp.tmpl`
    std::vector<std::string> treeNontermNames;
    for (const std::string &nonterm : nonterms)
    {
        treeNontermNames.push_back(Utils::varCString(nonterm));
    }
    const std::vector<std::string> *syncTokens = request.syncTokens;
    const std::string syncCases =
        generatorSyncCasesCpp17(terminals, syncTokens);
    const bool hasSync = !syncCases.empty();
    const bool hasErrorToken = request.usesError;
    std::string errorTokenId = "0";
    if (hasErrorToken)
    {
        const auto found =
            std::find(terminals.begin(), terminals.end(), "error");
        if (found != terminals.end())
        {
            errorTokenId = std::to_string(found - terminals.begin());
        }
    }
    std::map<std::string, int> nontermIds;
    for (std::size_t index = 0; index < nonterms.size(); ++index)
    {
        nontermIds[nonterms[index]] = static_cast<int>(index);
    }
    const auto startFound = nontermIds.find(analysis.grammar.start);
    // 期望集合表：LR 按状态、LL 按非终结符（与 C 目标同一套 expectedTokensByRow）
    const std::vector<std::vector<std::string>> expectedRows =
        expectedTokensByRow(analysis);
    std::string expectedTableText;
    {
        std::map<std::string, int> expectedIds;
        for (std::size_t index = 0; index < analysis.terminals.size(); ++index)
        {
            expectedIds[analysis.terminals[index]] = static_cast<int>(index);
        }
        std::vector<std::string> expectedStart;
        std::vector<std::string> expectedData;
        std::size_t offset = 0u;
        for (const std::vector<std::string> &row : expectedRows)
        {
            expectedStart.push_back(std::to_string(offset) + "u");
            for (const std::string &token : row)
            {
                expectedData.push_back(
                    std::to_string(expectedIds[token]) + "u");
                ++offset;
            }
        }
        // 表尾那一项是总长度；数据表为空时补一个 0u（与从前一致）
        expectedStart.push_back(std::to_string(offset) + "u");
        if (expectedData.empty())
        {
            expectedData.push_back("0u");
        }
        expectedTableText = tmpl::renderValues(
            Utils::loadTemplate("cpp17_expected_tables.cpp"),
            {{"expected_row_count",
              std::to_string(expectedRows.size() + 1u)},
             {"expected_start", tmpl::encode(expectedStart)},
             {"expected_data", tmpl::encode(expectedData)}});
    }
    // helpers 是解析产物与词法产物共用的模板：这里把解析产物才算得出来的
    // 值摆齐，词法产物在 lexerCpp 里另摆一套（那边没有 error / %sync）。
    std::string helpers = tmpl::renderValues(
        Utils::loadTemplate("cpp17_helpers.cpp"),
        {{"has_error_token", hasErrorToken ? "true" : "false"},
         {"error_token_id", errorTokenId},
         {"has_sync_tokens", hasSync ? "true" : "false"},
         {"sync_cases", syncCases},
         {"terminal_count", std::to_string(terminals.size())},
         {"nonterm_count", std::to_string(nonterms.size())},
         {"start_id", std::to_string(startFound == nontermIds.end()
                                          ? 0
                                          : startFound->second)},
         {"lr_state_count",
          lr ? std::to_string(analysis.stateCount()) : std::string("0")},
         {"lookahead_k",
          // LL(0) 就是 k = 0（不看 lookahead），不能夹到 1
          std::to_string(std::max(0, request.lookaheadK))},
         {"lookahead_column_count",
          // LR(k) 的归约列与 LL(k) 的分析表列都用这个数；k <= 1 时是 0（用不到）
          std::to_string(analysis.lookaheadColumns.size())},
         {"has_tree_nonterms", !treeNontermNames.empty()},
         {"tree_nonterm_indent", true},
         {"tree_nonterm_names", tmpl::encode(treeNontermNames)},
         {"token_value_fallback",
          unioned ? "token.value" : "ParserValue_T{}"},
         {"max_expected", "8"},
         {"expected_row_count", std::to_string(expectedRows.size())},
         {"expected_fallback",
          Utils::varCString(expectedFallbackText(analysis.mode))},
         {"expected_tables", expectedTableText}});

    // 词法器
    LexerBlocks blocks;
    if (!buildLexerBlocks(request, blocks, dfa))
    {
        return false;
    }
    const bool blankRule = lexerHasBlankRule(request.tokenDefs);
    std::string lexer = tmpl::renderValues(
        Utils::loadTemplate("cpp17_lexer.cpp"),
        {{"lex_skip_helpers",
          (dfa != nullptr)
              ? dfa->skipHelpers + cpp17LexerSkipHelpers(request.spaceChars, blankRule)
              : cpp17LexerSkipHelpers(request.spaceChars, blankRule)},
         {"lex_skip_loop",
          cpp17LexerSkipLoop(request.spaceChars, blankRule)},
         {"lex_string_block", blocks.stringBlock},
         {"lex_match_block", blocks.matchBlock},
         {"lex_number_block", blocks.numberBlock},
         {"lex_identifier_block", blocks.identifierBlock}});

    // 分析驱动（诊断文本里的模式名用短名，与 C 目标的 __LR_NAME__ 对齐）
    const std::string lrName = modeNameText(mode);
    std::string lrStack;
    std::string lrLoop;
    std::string llDriver;
    std::string ddDriver;
    std::string actionCases;
    if (lr)
    {
        actionCases = CodeGen::actionCases(analysis, analysis.mode,
                                           request.symbolValueMember, true,
                                           error);
        if (!error.empty())
        {
            return false;
        }
        // GLR 也要这一份：它提供 LrStack 与 applyActionCode（动作分派），
        // 只是不用里面的 LR 主循环
        lrStack = tmpl::renderValues(
            Utils::loadTemplate("cpp17_lr_stack.cpp"),
            {{"lr_action_cases", actionCases},
             {"has_destructors", hasDestructors}});
    }
    if (lr)
    {
        // GLR 有自己的图结构栈驱动（runLoop 在 cpp17_glr.cpp 里），别和 LR 主循环撞名
        if (isGlrMode(mode))
        {
            lrLoop = std::string();
        }
        else
        {
            // 失败退出（任何 return）都由这个守卫清栈；接受时置 keep 保住结果
            lrLoop = tmpl::renderValues(
                Utils::loadTemplate("cpp17_lr_loop.cpp"),
                {{"lr_name", lrName},
                 {"has_destructors", hasDestructors},
                 {"lr_incremental", !isGlrMode(mode)}});
        }
    }
    else if (isDdMode(mode))
    {
        // DD 的语义动作：`$n` 读 `values[n]`（与 LL 的帧同一个 1 起约定），
        // 但 yyerrok / yyclearin 在递归下降里没有落点，翻译模式传 "dd"
        const std::string ddActions = CodeGen::actionCases(
            analysis, "dd", request.symbolValueMember, true, error);
        if (!error.empty())
        {
            return false;
        }
        ddDriver = tmpl::renderValues(
            Utils::loadTemplate("cpp17_dd.cpp"),
            {{"dd_action_cases", ddActions}});
    }
    else
    {
        const std::string llActions = CodeGen::actionCases(
            analysis, "ll1", request.symbolValueMember, true, error);
        if (!error.empty())
        {
            return false;
        }
        // %destructor：语义帧按槽位记符号（只记真的写过值的槽位），丢弃时查表
        llDriver = tmpl::renderValues(
            Utils::loadTemplate("cpp17_ll.cpp"),
            {{"ll_expand_cases",
              generatorExpandCasesCpp17(analysis, terminals, nontermIds)},
             {"ll_action_cases", llActions},
             {"has_destructors", hasDestructors}});
    }

    // 公开 API：Tree / Error / Engine 的定义。原先靠 `__SEC_PUBLIC_TREE_*`
    // 分段标记 + extractSection 按 %tree 增删，现在模板里直接写
    // `{% if parse_tree %}`（Engine::parseTree）与 `{% if tree_class %}`
    // （Tree 的实现）。解析产物：Tree 实现一直有，parseTree 看 %tree；
    // 词法产物在 lexerCpp 里两个都关掉。
    const bool lrIncrementalMode = isLrMode(mode) && !isGlrMode(mode);
    const bool incrementalMode =
        lrIncrementalMode || isDdMode(mode) ||
        (!isLrMode(mode) && !isLexMode(mode));
    const std::string engineParseImpl = tmpl::renderValues(
        Utils::loadTemplate("cpp17_engine_parse_impl.cpp"),
        {{"incremental", incrementalMode}});

    std::string publicText = tmpl::renderValues(
        Utils::loadTemplate("cpp17_public.cpp"),
        {{"engine_parse_impl", engineParseImpl},
         {"glr_impl",
          isGlrMode(mode) ? tmpl::renderValues(
                                Utils::loadTemplate("cpp17_glr_impl.cpp"),
                                {{"value_type", "ParserValue_T"}})
                          : std::string()},
         {"parse_tree", request.tree},
         {"tree_class", true}});

    const std::string core = tmpl::renderValues(
        Utils::loadTemplate("cpp17_core.cpp"),
        {{"core_syntax_error", Utils::loadTemplate("cpp17_core_syntax_error.cpp")},
         {"core_parse_entry", Utils::loadTemplate("cpp17_core_parse_entry.cpp")},
         {"glr", isGlrMode(mode)},
         {"value_type", "ParserValue_T"}});
    std::string text = tmpl::renderValues(
        Utils::loadTemplate("cpp17_parser.cpp"),
        {{"mode_name", modeLabelText(mode)},
         {"hpp_name", hppName(request)},
         {"ns", nameSpace},
         {"tables", tables},
         {"helpers", helpers},
         {"tree_helpers", Utils::loadTemplate("cpp17_tree.cpp")},
         {"core", core},
         {"lexer", lexer},
         {"lr_stack", lrStack},
         {"lr_loop", lrLoop},
         {"glr_driver",
          isGlrMode(mode) ? tmpl::renderValues(
                                Utils::loadTemplate("cpp17_glr.cpp"),
                                {{"lr_name", modeNameText(mode)},
                                 {"mode_name", modeLabelText(mode)},
                                 {"chart", (mode == "earley") || (mode == "cyk")},
                                 {"chart_driver",
                                  ((mode == "earley") || (mode == "cyk"))
                                      ? tmpl::renderValues(
                                            Utils::loadTemplate("cpp17_chart.cpp"),
                                            {{"earley", mode == "earley"}})
                                      : std::string()}})
                          : std::string()},
         {"ll_driver", llDriver},
         {"dd_driver", ddDriver},
         {"destructor_dispatch", destructorDispatch},
         {"public_api", publicText},
         {"lex", false}});
    if (!text.empty() && (text.back() != '\n'))
    {
        text += "\n";
    }
    out = std::move(text);
    return true;
}

/**
 * @brief 演示 main 里的 `%commit` 打印片段：行号 + 类型 + 原文。
 * @param request 组装入参。
 * @return C++ 代码段；文法里没写 %commit 时为空串（演示输出与从前一样）。
 */
std::string cpp17DemoCommitPrint(const ProjectRequest &request)
{
    const LexSpec emptyLex;
    const LexSpec &lexSpec =
        (request.lexSpec == nullptr) ? emptyLex : *request.lexSpec;
    if (lexSpec.commits.empty())
    {
        return std::string();
    }
    // 纯静态片段：一个字都不改地取出来（`source/templates/cpp17_demo_commit_print.cpp.tmpl`）
    return Utils::loadTemplate("cpp17_demo_commit_print.cpp");
}

std::string CodeGenCpp17::mainCpp(const ProjectRequest &request,
                                  const std::string &mode,
                                  const std::string &mainBody)
{
    const std::string nameSpace = namespaceName(request);
    std::string demo;
    if (hasVisibleText(mainBody))
    {
        // %main：正文前面补一行 using namespace，生成器自己的名字直接可用
        demo = tmpl::renderValues(
            Utils::loadTemplate("cpp17_user_main.cpp"),
            {{"ns", nameSpace}, {"main_body", mainBody}});
    }
    else if (isGlrMode(mode))
    {
        // GLR：读一行 → 跑 GLR → 打印全部结果（歧义路径数 + 每个结果 / 语法树）；
        // 打印那两段是独立模板（%tree 与否二选一）。
        // 这两份片段模板带 `{# ... #}` 说明块，必须过一遍引擎才会把注释吃掉
        const std::string print = tmpl::renderValues(
            request.tree
                ? Utils::loadTemplate("cpp17_glr_demo_result_tree.cpp")
                : Utils::loadTemplate("cpp17_glr_demo_result_value.cpp"));
        demo = tmpl::renderValues(
            Utils::loadTemplate("cpp17_glr_demo_main.cpp"),
            {{"ns", nameSpace},
             {"glr_run", request.tree ? "engine.runGlrTree(input)"
                                      : "engine.runGlr(input)"},
             {"glr_print", print},
             {"demo_commits", cpp17DemoCommitPrint(request)}});
    }
    else
    {
        // %tree 的演示段在模板里（没有 %tree 时整段不出现）
        const std::string demoTree =
            request.tree ? Utils::loadTemplate("cpp17_demo_tree.cpp")
                         : std::string();
        demo = tmpl::renderValues(
            Utils::loadTemplate("cpp17_demo_main.cpp"),
            {{"ns", nameSpace},
             {"demo_commits", cpp17DemoCommitPrint(request)},
             {"demo_tree", demoTree}});
    }
    return tmpl::renderValues(
        Utils::loadTemplate("cpp17_main.cpp"),
        {{"mode_name", modeLabelText(mode)},
         {"hpp_name", hppName(request)},
         {"ns", nameSpace},
         {"demo_main", demo}});
}

bool CodeGenCpp17::lexerHpp(const ProjectRequest &request,
                            const std::vector<std::string> &terminals,
                            const std::string &unionBodyWithLines,
                            std::string &out, Error &error)
{
    (void)error;
    const std::string nameSpace = namespaceName(request);
    const bool unioned = unionMode(request);
    std::string userInclude;
    if (hasVisibleText(request.userCode))
    {
        userInclude = tmpl::renderValues(
            Utils::loadTemplate("cpp17_user_include.hpp"),
            {{"header_name", userHeaderName(request)}});
    }
    // 容量常量：词法模式只用得上 kMaxErrors，其余保留同一套名字（值给缺省）
    const std::string constants = tmpl::renderValues(
        Utils::loadTemplate("cpp17_constants.hpp"),
        {{"lex", true},
         {"value_slots", "4"},
         {"max_stack_depth",
          std::to_string(
              static_cast<std::size_t>(request.maxStackDepth.value_or(64)))},
         {"max_errors",
          std::to_string(
              static_cast<std::size_t>(request.maxErrors.value_or(8)))},
         {"recovery_tokens",
          std::to_string(static_cast<std::size_t>(
              request.errorRecoveryTokens.value_or(3)))}});
    // 语义值类型 + Token 枚举（与解析产物同一套写法；三套写法都在模板里）
    std::string valueTypeDecl;
    std::string tokenValueDecl;
    if (unioned)
    {
        valueTypeDecl = tmpl::renderValues(
            Utils::loadTemplate("cpp17_value_type_decl.hpp"),
            {{"union_mode", true},
             {"union_body", (unionBodyWithLines.empty() ? *request.unionBody
                                                        : unionBodyWithLines)},
             {"value_type", std::string()}});
        tokenValueDecl = tmpl::renderValues(
            Utils::loadTemplate("cpp17_token_value_decl.hpp"),
            {{"union_mode", true}});
    }
    else if ((request.valueType != nullptr) &&
             hasVisibleText(*request.valueType))
    {
        valueTypeDecl = tmpl::renderValues(
            Utils::loadTemplate("cpp17_value_type_decl.hpp"),
            {{"union_mode", false},
             {"union_body", std::string()},
             {"value_type", *request.valueType}});
        tokenValueDecl = tmpl::renderValues(
            Utils::loadTemplate("cpp17_token_value_decl.hpp"),
            {{"union_mode", false}});
    }
    else
    {
        valueTypeDecl = tmpl::renderValues(
            Utils::loadTemplate("cpp17_value_type_decl.hpp"),
            {{"union_mode", false},
             {"union_body", std::string()},
             {"value_type", "int"}});
        tokenValueDecl = tmpl::renderValues(
            Utils::loadTemplate("cpp17_token_value_decl.hpp"),
            {{"union_mode", false}});
    }
    std::vector<std::string> tokenNames;
    for (const std::string &terminal : terminals)
    {
        tokenNames.push_back("TOKEN_" + terminal);
    }
    tokenNames.push_back("TOKEN_INVALID");
    const std::string tokenTypes = tmpl::renderValues(
        Utils::loadTemplate("cpp17_token_types.hpp"),
        {{"value_type_decl", valueTypeDecl},
         {"tokens", tmpl::encode(tokenNames)},
         {"token_name_count", std::to_string(terminals.size() + 1u)}});

    // 词法产物：没有语法树 / 兼容层（`tree_api` / `compat_api` 留空），
    // Engine 入口换成 tokenize，文档标题与措辞由模板里的 `{% if lex %}` 选。
    std::string text = tmpl::renderValues(
        Utils::loadTemplate("cpp17_parser.hpp"),
        {{"mode_name", "LEX(词法模式)"},
         {"ns", nameSpace},
         {"guard", "LEXER_HPP"},
         {"user_include", userInclude},
         {"constants", constants},
         {"token_types", tokenTypes},
         {"token_value_decl", tokenValueDecl},
         {"tree_api", std::string()},
         {"engine_parse_api", Utils::loadTemplate("cpp17_engine_lex_api.hpp")},
         {"engine_tree_api", std::string()},
         {"compat_api", std::string()},
         {"lex", true}});
    if (!text.empty() && (text.back() != '\n'))
    {
        text += "\n";
    }
    out = std::move(text);
    return true;
}

bool CodeGenCpp17::lexerCpp(const ProjectRequest &request,
                            const std::vector<std::string> &terminals,
                            const std::string &unionBodyWithLines,
                            std::string &out, Error &error)
{
    // %union 正文只在 lexer.hpp 里落地（ParserValue_T 的定义在头文件里）
    (void)unionBodyWithLines;
    // 词法段的正则认不下只打警告，这一段没有要往 error 里写的东西
    (void)error;
    const std::string nameSpace = namespaceName(request);
    const bool unioned = unionMode(request);
    // 可选 DFA 词法器（与解析产物同一套开关）
    CppLexDfaData dfaData;
    const CppLexDfaData *dfa = nullptr;
    if (request.cppLexer == "dfa")
    {
        if (!buildCppLexDfa(request, dfaData, error))
        {
            return false;
        }
        dfa = dfaData.enabled ? &dfaData : nullptr;
    }
    // 词法表（词法模式没有分析表 / 语法树表）
    std::string lexTables;
    buildLexTables(request, terminals, lexTables, dfa);
    const std::string tables =
        wrapTables(lexTables, std::string(), std::string());
    // helpers：与解析产物同一份模板，词法模式下没有 error / %sync /
    // 期望表（几个计数值都给 0 或 1）
    std::string helpers = tmpl::renderValues(
        Utils::loadTemplate("cpp17_helpers.cpp"),
        {{"has_error_token", "false"},
         {"error_token_id", "0"},
         {"has_sync_tokens", "false"},
         {"sync_cases", std::string()},
         {"terminal_count", std::to_string(terminals.size())},
         {"nonterm_count", "0"},
         {"start_id", "0"},
         {"lr_state_count", "0"},
         {"lookahead_k", "1"},
         {"lookahead_column_count", "0"},
         {"has_tree_nonterms", false},
         {"tree_nonterm_indent", false},
         {"tree_nonterm_names", tmpl::encode(std::vector<std::string>())},
         {"expected_tables", std::string()},
         {"token_value_fallback", unioned ? "token.value" : "ParserValue_T{}"},
         {"max_expected", "0"},
         {"expected_row_count", "0"},
         {"expected_fallback", Utils::varCString(expectedFallbackText("lex"))}});
    // 词法器 + tokenize 驱动
    const bool blankRule = lexerHasBlankRule(request.tokenDefs);
    LexerBlocks blocks;
    if (!buildLexerBlocks(request, blocks, dfa))
    {
        return false;
    }
    std::string lexer = tmpl::renderValues(
        Utils::loadTemplate("cpp17_lexer.cpp"),
        {{"lex_skip_helpers",
          (dfa != nullptr) ? dfa->skipHelpers + cpp17LexerSkipHelpers(request.spaceChars, blankRule)
                           : cpp17LexerSkipHelpers(request.spaceChars, blankRule)},
         {"lex_skip_loop",
          cpp17LexerSkipLoop(request.spaceChars, blankRule)},
         {"lex_string_block", blocks.stringBlock},
         {"lex_match_block", blocks.matchBlock},
         {"lex_number_block", blocks.numberBlock},
         {"lex_identifier_block", blocks.identifierBlock}});
    // 词法模式入口（stableToken / tokenize）整段在模板里
    lexer += Utils::loadTemplate("cpp17_lexer_tokenize.cpp");
    // core：去掉「语法错误（期望表）」与解析入口
    const std::string core = tmpl::renderValues(
        Utils::loadTemplate("cpp17_core.cpp"),
        {{"core_syntax_error", std::string()},
         {"core_parse_entry", std::string()}});
    // 公开 API：词法产物没有语法树——Tree 的定义与 Engine::parseTree 都不出现，
    // Engine 入口换成 tokenize。原先靠「找 `Tree::Tree(` 与 `Engine::Engine()`
    // 两个锚点做 erase」+ `__SEC_PUBLIC_TREE_*` 分段标记，现在模板里的
    // `{% if tree_class %}` / `{% if parse_tree %}` 直接决定。
    const std::string publicText = tmpl::renderValues(
        Utils::loadTemplate("cpp17_public.cpp"),
        {{"engine_parse_impl", Utils::loadTemplate("cpp17_engine_lex_impl.cpp")},
         {"parse_tree", false},
         {"tree_class", false}});
    std::string text = tmpl::renderValues(
        Utils::loadTemplate("cpp17_parser.cpp"),
        {{"mode_name", "LEX(词法模式)"},
         {"hpp_name", lexerHppName(request)},
         {"ns", nameSpace},
         {"tables", tables},
         {"helpers", helpers},
         {"tree_helpers", std::string()},
         {"core", core},
         {"lexer", lexer},
         {"lr_stack", std::string()},
         {"lr_loop", std::string()},
         {"ll_driver", std::string()},
         {"destructor_dispatch", std::string()},
         {"public_api", publicText},
         {"lex", true}});
    if (!text.empty() && (text.back() != '\n'))
    {
        text += "\n";
    }
    out = std::move(text);
    return true;
}

std::string CodeGenCpp17::lexerMainCpp(const ProjectRequest &request,
                                      const std::string &mainBody)
{
    const std::string nameSpace = namespaceName(request);
    std::string demo;
    if (hasVisibleText(mainBody))
    {
        demo = tmpl::renderValues(
            Utils::loadTemplate("cpp17_user_main.cpp"),
            {{"ns", nameSpace}, {"main_body", mainBody}});
    }
    else
    {
        demo = tmpl::renderValues(
            Utils::loadTemplate("cpp17_lex_demo_main.cpp"),
            {{"ns", nameSpace},
             {"demo_commits", cpp17DemoCommitPrint(request)}});
    }
    return tmpl::renderValues(
        Utils::loadTemplate("cpp17_lex_main.cpp"),
        {{"mode_name", "LEX(词法模式)"},
         {"hpp_name", lexerHppName(request)},
         {"ns", nameSpace},
         {"demo_main", demo}});
}

} // namespace ParseEngine
