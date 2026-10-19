/**
 * @file code_gen_c99.cpp
 * @brief C99 后端：parser.c / parser.h / parser_token.* / parser_table.c / main.c。
 *
 * 与 `code_gen.cpp`（共用部分）分开：这里放 C99 特有的词法器、LL / LR 运行时、
 * 语法树、分析表与整段拼装。共用部分通过 `CodeGen::` 调用（`actionCases` /
 * `destructorSymbolGroups` / `extractSection` / `resolveLineDirectives`）。
 */

#include "ParseEngine/code_gen_c99.hpp"

#include <algorithm>
#include <cstdio>
#include <functional>
#include <map>
#include <regex>
#include <set>

#include "ParseEngine/action_translator.hpp"
#include "ParseEngine/code_gen.hpp"
#include "ParseEngine/error.hpp"
#include "ParseEngine/grammar_loader.hpp"
#include "ParseEngine/regex_dfa.hpp"
#include "ParseEngine/regex_vm.hpp"
#include "ParseEngine/utils.hpp"

// C99 产物模板（source/templates/c99_*.tmpl）走 library/template 的模板引擎：
// 占位符 `{{ 变量 }}` 由这里摆「名字 -> 值」。
#include "template/engine.hpp"

namespace ParseEngine
{

namespace
{

/**
 * @brief 转义会提前结束 C 块注释的「星号 + 斜杠」序列。
 * @param text 原文（如正则模式）。
 * @return 可以安全放进 C 块注释的文本。
 */
std::string commentSafeText(const std::string &text)
{
    std::string out;
    out.reserve(text.size());
    for (std::size_t index = 0u; index < text.size(); ++index)
    {
        if ((text[index] == '*') && (index + 1u < text.size()) &&
            (text[index + 1u] == '/'))
        {
            out += "*\\/";
            ++index;
            continue;
        }
        out += text[index];
    }
    return out;
}

/**
 * @brief 判断字面量是否像标识符。
 * @param text 字面量。
 * @return 像标识符返回 true。
 */
bool isIdentifierLiteral(const std::string &text)
{
    if (text.empty())
    {
        return false;
    }
    if (!(std::isalpha(static_cast<unsigned char>(text[0])) ||
          (text[0] == '_')))
    {
        return false;
    }
    for (std::size_t index = 1; index < text.size(); ++index)
    {
        const char ch = text[index];
        if (!(std::isalnum(static_cast<unsigned char>(ch)) || (ch == '_')))
        {
            return false;
        }
    }
    return true;
}

/**
 * @brief 按 UTF-8 统计字符个数。
 * @param text 输入文本。
 * @return 字符个数。
 */
std::size_t utf8Length(const std::string &text)
{
    std::size_t count = 0;
    for (const char ch : text)
    {
        if ((static_cast<unsigned char>(ch) & 0xC0) != 0x80)
        {
            ++count;
        }
    }
    return count;
}

/**
 * @brief 生成关键字表与 Parser_TokenKeyword。
 * @param tokenDefs 固定字面量 Token。
 * @param staticPrefix 定义前的前缀（如 static）。
 * @return C 代码段。
 */
/**
 * @brief 关键字表的一行（`c99_keyword_table.c.tmpl` 的元素）。
 */
struct KeywordRow
{
    std::string text;  ///< 字面量（C 字符串字面量，已转义）
    std::string token; ///< Token 枚举名去掉 TOKEN_ 前缀
};

std::string generatorKeywordC(
    const std::map<std::string, std::vector<std::string>> &tokenDefs,
    const std::string &staticPrefix)
{
    std::vector<std::pair<std::string, std::string>> keywords;
    for (const auto &entry : tokenDefs)
    {
        for (const std::string &literal : entry.second)
        {
            if (isIdentifierLiteral(literal))
            {
                keywords.emplace_back(literal, entry.first);
            }
        }
    }
    // 表体与逗号整个在模板里（source/templates/c99_keyword_table.c.tmpl）：
    // 这里只把每行的两个字摆好（字面量转义过、Token 枚举名）。
    std::vector<KeywordRow> rows;
    rows.reserve(keywords.size());
    for (const auto &keyword : keywords)
    {
        rows.push_back({Utils::varCString(keyword.first), keyword.second});
    }
    return tmpl::renderValues(
        Utils::loadTemplate("c99_keyword_table.c"),
        {{"keywords", tmpl::encode(rows)},
         {"keyword_count", std::to_string(keywords.size())},
         {"static_prefix", staticPrefix}});
}

/**
 * @brief 字符类划分表里的一行（`c99_lex_dfa_tables.c.tmpl` 的元素）。
 */
struct LexDfaCellRow
{
    std::vector<int> cells; ///< 这一行里每个字节的字符类编号
};

/**
 * @brief 一个已编译 DFA 的转移表 / 接受表（`c99_lex_dfa_tables.c.tmpl` 的元素）。
 */
struct LexDfaInfo
{
    std::string label;                      ///< 注释里写的名字（标识符 / Token 名）
    std::string pattern;                    ///< 注释里写的模式（已做注释转义）
    std::string trans_name;                 ///< 转移表变量名
    std::string accept_name;                ///< 接受表变量名
    int state_count = 0;                    ///< 状态数
    int class_count = 0;                    ///< 字符类个数
    std::vector<LexDfaCellRow> transitions; ///< 状态 × 字符类
    std::vector<int> accept;                ///< 每个状态是不是接受态（0 / 1）
};

/**
 * @brief DFA 描述符 `{ &Trans[0][0], Accept, S, start, trap }` 的字段。
 */
struct LexDfaDescriptor
{
    std::string trans_name; ///< 转移表变量名
    std::string accept_name;///< 接受表变量名
    int state_count = 0;    ///< 状态数
    int start_state = 0;    ///< 起始状态编号
    int trap_state = 0;     ///< 陷阱状态编号
};

/**
 * @brief 生成扩展正则的有序程序、规则描述和元数据。
 * @param patterns 全部正则。 @param entries Token 规则。 @param hasIdentifier 有无标识符。
 * @param commits 注释规则。 @param tokenLexOrder 声明顺序。 @param out 输出表。
 * @param error 错误说明。 @return 成功返回 true。
 */
bool generatorLexVmC(
    const std::vector<std::pair<std::string, std::string>> &patterns,
    const std::vector<std::pair<std::string, std::string>> &entries,
    bool hasIdentifier, const std::vector<CommitSpec> &commits,
    const std::map<std::string, int> &tokenLexOrder, std::string &out, Error &error)
{
    auto programs = std::make_shared<tmpl::Array>();
    std::vector<std::string> descriptors, regexDescriptors, regexTypes, regexOrders;
    std::vector<std::string> commitDescriptors, commitTypes, commitSkips, commitOrders;
    for (std::size_t index = 0; index < patterns.size(); ++index)
    {
        RegexVmProgram program;
        if (!RegexVm::compile(patterns[index].second, program, error)) return false;
        const std::string name = "s_ParserRegexVm" + std::to_string(index);
        std::vector<std::string> rows;
        for (const auto &instruction : program.code)
        {
            std::string row = "{ " + std::to_string(instruction.op) + ", " +
                std::to_string(instruction.next) + ", " + std::to_string(instruction.other) +
                ", " + std::to_string(instruction.value) + ", { ";
            for (std::size_t byte = 0; byte < instruction.bits.size(); ++byte)
                row += (byte ? ", " : "") + std::to_string(instruction.bits[byte]);
            rows.push_back(row + " } }");
        }
        auto item = std::make_shared<tmpl::Object>();
        item->insert("name", name);
        item->insert("rows", tmpl::encode(rows));
        programs->items.emplace_back(item);
        descriptors.push_back("{ " + name + ", " + std::to_string(program.code.size()) +
            "u, " + std::to_string(program.entry) + "u, " + std::to_string(program.groups) + "u }");
    }
    for (std::size_t index = 0; index < entries.size(); ++index)
    {
        regexDescriptors.push_back(descriptors[index]);
        regexTypes.push_back("TOKEN_" + entries[index].first);
        const auto found = tokenLexOrder.find(entries[index].first);
        regexOrders.push_back(std::to_string(found == tokenLexOrder.end() ? static_cast<int>(index) : found->second));
    }
    const std::size_t commitStart = entries.size() + (hasIdentifier ? 1u : 0u);
    for (std::size_t index = 0; index < commits.size(); ++index)
    {
        commitDescriptors.push_back(descriptors[commitStart + index]);
        commitTypes.push_back(Utils::varCString(commits[index].type));
        commitSkips.push_back(commits[index].isSkip ? "1u" : "0u");
        commitOrders.push_back(std::to_string(commits[index].order));
    }
    if (commits.empty())
    {
        commitDescriptors.push_back("{ NULL, 0u, 0u, 0u }");
        commitTypes.push_back("NULL");
        commitSkips.push_back("0u");
        commitOrders.push_back("0");
    }
    out += tmpl::renderValues(Utils::loadTemplate("c99_lex_vm_tables.c"),
        {{"runtime", Utils::loadTemplate("regex_vm_runtime.h")}, {"programs", tmpl::Value(programs)},
         {"has_regex", !entries.empty()}, {"has_identifier", hasIdentifier},
         {"regex_descriptors", tmpl::encode(regexDescriptors)}, {"regex_types", tmpl::encode(regexTypes)},
         {"regex_orders", tmpl::encode(regexOrders)}, {"commit_descriptors", tmpl::encode(commitDescriptors)},
         {"commit_types", tmpl::encode(commitTypes)}, {"commit_skips", tmpl::encode(commitSkips)},
         {"commit_orders", tmpl::encode(commitOrders)}, {"commit_slots", std::to_string(commitTypes.size())},
         {"identifier_descriptor", hasIdentifier ? descriptors[entries.size()] : std::string()}});
    return true;
}

/**
 * @brief 生成 %token regex / %identifier 的 DFA 表与识别接口。
 * @param tokenDefs 固定字面量 Token。
 * @param regexDefs 正则 Token（有序）。
 * @param identifierPattern 标识符正则；空表示未声明。
 * @param outDecls 输出声明段。
 * @param outTables 输出表定义段。
 * @param error 出错时写入错误说明。
 * @return 成功返回 true；正则语法有误返回 false。
 */
bool generatorLexDfaC(
    const std::map<std::string, std::vector<std::string>> &tokenDefs,
    const std::map<std::string, std::string> &regexDefs,
    const std::string &identifierPattern, bool hasIdentifier,
    const std::vector<CommitSpec> &commits,
    const std::map<std::string, int> &tokenLexOrder, std::string &outDecls,
    std::string &outTables, Error &error)
{
    std::vector<std::pair<std::string, std::string>> entries;
    for (const auto &entry : regexDefs)
    {
        entries.emplace_back(entry.first, entry.second);
    }
    std::vector<std::pair<std::string, std::string>> patterns = entries;
    if (hasIdentifier)
    {
        patterns.emplace_back("__identifier__", identifierPattern);
    }
    for (std::size_t index = 0u; index < commits.size(); ++index)
    {
        patterns.emplace_back("__commit" + std::to_string(index) + "__",
                              commits[index].pattern);
    }
    bool hasVm = false;
    for (const auto &pattern : patterns)
    {
        std::shared_ptr<RegexNode> ast;
        if (!RegexDfa::parse(pattern.second, ast, error)) return false;
        hasVm = hasVm || RegexVm::required(ast) ||
            std::any_of(pattern.second.begin(), pattern.second.end(),
                        [](unsigned char byte) { return byte >= 128u; });
    }
    // 关键字表先出来：它是独立的一段产物（在下面两份模板之外）
    outTables = generatorKeywordC(tokenDefs, "");

    const bool hasDfa = !patterns.empty();
    const bool hasRegex = !entries.empty();
    const bool hasCommits = !commits.empty();
    const std::size_t commitSlots = hasCommits ? commits.size() : 1u;

    // 声明段整段在模板里（source/templates/c99_lex_dfa_decls.h.tmpl）：外壳、
    // DFA 结构体、按条件出现的三段声明都在那边用 `{% if %}` 选。
    outDecls = tmpl::renderValues(
        Utils::loadTemplate("c99_lex_dfa_decls.h"),
        {{"has_vm", hasVm},
         {"has_dfa", hasDfa},
         {"has_regex", hasRegex},
         {"has_identifier", hasIdentifier},
         {"regex_count", std::to_string(entries.size())},
         {"commit_count", std::to_string(commits.size())},
         {"commit_slots", std::to_string(commitSlots)}});

    if (!hasDfa)
    {
        // 没有 %token regex / %identifier / %commit 时词法器走「固定字面量」
        // 那一段，不会引用任何 DFA / commit 表，这里什么都不用发。
        return true;
    }
    if (hasVm)
    {
        return generatorLexVmC(patterns, entries, hasIdentifier, commits,
                               tokenLexOrder, outTables, error);
    }
    RegexCompileResult compileResult;
    if (!RegexDfa::compileAll(patterns, compileResult, error))
    {
        return false;
    }
    const int classCount = static_cast<int>(compileResult.cells.size());

    // 字符类划分（字节 -> 类编号）：16 行 × 16 列。行与分隔符都归模板管，
    // 这里只把数字摆好。
    std::vector<LexDfaCellRow> classMap;
    classMap.reserve(16u);
    for (int start = 0; start < 256; start += 16)
    {
        LexDfaCellRow row;
        row.cells.reserve(16u);
        for (int code = start; code < start + 16; ++code)
        {
            row.cells.push_back(
                compileResult.byteMap[static_cast<std::size_t>(code)]);
        }
        classMap.push_back(std::move(row));
    }

    // 每个 DFA 的转移表 / 接受表 + 它的描述符。描述符按名字另存一份：
    // %identifier 与 %commit 各自还要单独发一张表。
    std::vector<LexDfaInfo> dfas;
    std::vector<LexDfaDescriptor> regexDfas;
    std::map<std::string, LexDfaDescriptor> descriptorByName;
    for (const CompiledRegex &item : compileResult.compiled)
    {
        const bool identifier = (item.name == "__identifier__");
        const std::string prefix =
            identifier ? "s_ParserIdentifierDfa" : "s_ParserDfa" + item.name;
        LexDfaInfo info;
        info.label = identifier ? "标识符" : item.name;
        info.pattern = commentSafeText(item.pattern);
        info.trans_name = prefix + "Trans";
        info.accept_name = prefix + "Accept";
        info.state_count = item.dfa.stateCount;
        info.class_count = classCount;
        info.transitions.reserve(
            static_cast<std::size_t>(item.dfa.stateCount));
        info.accept.reserve(static_cast<std::size_t>(item.dfa.stateCount));
        for (int state = 0; state < item.dfa.stateCount; ++state)
        {
            LexDfaCellRow row;
            row.cells.reserve(static_cast<std::size_t>(classCount));
            for (int cls = 0; cls < classCount; ++cls)
            {
                row.cells.push_back(item.dfa.transitions.at(state).at(cls));
            }
            info.transitions.push_back(std::move(row));
            info.accept.push_back(item.dfa.accept.count(state) ? 1 : 0);
        }
        LexDfaDescriptor descriptor;
        descriptor.trans_name = info.trans_name;
        descriptor.accept_name = info.accept_name;
        descriptor.state_count = item.dfa.stateCount;
        descriptor.start_state = item.dfa.start;
        descriptor.trap_state = item.dfa.trap;
        descriptorByName.insert_or_assign(item.name, descriptor);
        if (!identifier)
        {
            regexDfas.push_back(descriptor);
        }
        dfas.push_back(std::move(info));
    }

    LexDfaDescriptor identifierDfa;
    if (hasIdentifier)
    {
        const auto found = descriptorByName.find("__identifier__");
        if (found != descriptorByName.end())
        {
            identifierDfa = found->second;
        }
    }

    std::vector<std::string> regexTypes;
    std::vector<std::string> regexOrders;
    regexTypes.reserve(entries.size());
    regexOrders.reserve(entries.size());
    for (std::size_t index = 0; index < entries.size(); ++index)
    {
        regexTypes.push_back("TOKEN_" + entries[index].first);
        const auto orderIt = tokenLexOrder.find(entries[index].first);
        regexOrders.push_back(std::to_string(
            (orderIt != tokenLexOrder.end()) ? orderIt->second
                                             : static_cast<int>(index)));
    }

    // %commit：模式与 Token 一起做最长匹配；匹配到就收进容器（skip 丢掉）
    std::vector<LexDfaDescriptor> commitDfas;
    std::vector<std::string> commitTypes;
    std::vector<std::string> commitSkips;
    std::vector<std::string> commitOrders;
    for (std::size_t index = 0u; index < commits.size(); ++index)
    {
        const auto found = descriptorByName.find(
            "__commit" + std::to_string(index) + "__");
        commitDfas.push_back(found != descriptorByName.end()
                                 ? found->second
                                 : LexDfaDescriptor{});
        commitTypes.push_back(Utils::varCString(commits[index].type));
        commitSkips.push_back(commits[index].isSkip ? "1u" : "0u");
        commitOrders.push_back(std::to_string(commits[index].order));
    }

    outTables += tmpl::renderValues(
        Utils::loadTemplate("c99_lex_dfa_tables.c"),
        {{"has_dfa", true},
         {"has_regex", hasRegex},
         {"has_identifier", hasIdentifier},
         {"has_commits", hasCommits},
         {"class_count", std::to_string(classCount)},
         {"class_map", tmpl::encode(classMap)},
         {"dfas", tmpl::encode(dfas)},
         {"regex_dfas", tmpl::encode(regexDfas)},
         {"regex_types", tmpl::encode(regexTypes)},
         {"regex_orders", tmpl::encode(regexOrders)},
         {"identifier_dfa", tmpl::encode(identifierDfa)},
         {"commit_dfas", tmpl::encode(commitDfas)},
         {"commit_types", tmpl::encode(commitTypes)},
         {"commit_skips", tmpl::encode(commitSkips)},
         {"commit_orders", tmpl::encode(commitOrders)},
         {"commit_slots", std::to_string(commitSlots)}});
    return true;
}

/**
 * @brief 生成按长度降序排列的固定字符串 Lexer 表。
 * @param tokenDefs 固定字面量 Token。
 * @param tokenLexOrder Token 名 -> 词法声明序（%commit 同长度比较用）。
 * @return C 初始化列表文本。
 */
/**
 * @brief 固定字面量匹配表的一行（`c99_lex_rules.c.tmpl` 的元素）。
 */
struct LexRuleRow
{
    std::string text;           ///< 字面量（C 字符串字面量，已转义）
    std::string length;         ///< 字节数（模板补 u 后缀）
    std::string token;          ///< Token 枚举名去掉 TOKEN_ 前缀
    std::string blank_first;    ///< 以空白开头：1u / 0u
    std::string blank_wildcard; ///< 含 \b 通配：1u / 0u
    std::string decl_order;     ///< 声明顺序
};

std::string generatorLexRules(
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
    // 表的每一行在片段模板里出（source/templates/c99_lex_rules.c.tmpl）：
    // 空表的占位那一句也在模板里。这里只把每行的字段算成现成文本。
    std::vector<LexRuleRow> rows;
    rows.reserve(rules.size());
    for (const Rule &rule : rules)
    {
        rows.push_back({Utils::varCString(rule.literal),
                        std::to_string(rule.literal.size()),
                        rule.tokenName,
                        literalCanStartBlank(rule.literal) ? "1u" : "0u",
                        literalHasBlankWildcard(rule.literal) ? "1u" : "0u",
                        std::to_string(rule.declOrder)});
    }
    return tmpl::renderValues(Utils::loadTemplate("c99_lex_rules.c"),
                              {{"lex_rules", tmpl::encode(rows)}});
}

/**
 * @brief 展开时要压栈的一个符号（`c99_ll_expand_cases.c.tmpl` 的元素）。
 */
struct LlExpandItem
{
    std::string kind;      ///< PARSER_ITEM_TERMINAL / PARSER_ITEM_NONTERM
    std::string symbol_id; ///< Token 枚举名，或统一编号（带 u 后缀）
    int slot = 0;          ///< 压进第几个槽
};

/**
 * @brief 一个产生式的左递归展开 case（同上的元素）。
 */
struct LlExpandCase
{
    std::string id;                    ///< 产生式号（十进制文本）
    bool left_loop = false;            ///< 要不要发 LEFT_LOOP 那一句
    std::string loop_symbol_id;        ///< LEFT_LOOP 用的符号编号
    bool keep_action = false;          ///< 要不要发 ACTION_KEEP 那一句
    bool empty_tail = false;           ///< 空尾规则（只发一句注释）
    bool plain_action = false;         ///< 普通产生式：发 ACTION 那一句
    std::vector<LlExpandItem> symbols; ///< 要压栈的符号（声明序的倒序）
};

/**
 * @brief 生成产生式展开分支（仅 LL）。
 * @param analysis 分析结果。
 * @param terminals 终结符列表。
 * @param nontermIds 非终结符到编号的映射。
 * @return C 代码段。
 */
std::string generatorExpandCases(
    const AnalysisResult &analysis, const std::vector<std::string> &terminals,
    const std::map<std::string, int> &nontermIds)
{
    const std::set<std::string> terminalSet(terminals.begin(), terminals.end());
    // 「压一个符号」要用的 kind 与编号：终结符用 Token 枚举名，非终结符用统一编号。
    // 正文（那一句 Parser_Push）在模板里，这里只算数据。
    const auto pushItem = [&terminalSet, &nontermIds](const std::string &symbol,
                                                      int slot)
    {
        LlExpandItem item;
        item.slot = slot;
        if (terminalSet.count(symbol) != 0u)
        {
            item.kind = "PARSER_ITEM_TERMINAL";
            item.symbol_id = "TOKEN_" + symbol;
        }
        else
        {
            item.kind = "PARSER_ITEM_NONTERM";
            item.symbol_id = std::to_string(nontermIds.at(symbol)) + "u";
        }
        return item;
    };
    std::vector<LlExpandCase> cases;
    cases.reserve(analysis.productions.size());
    for (const Production &production : analysis.productions)
    {
        const std::vector<std::string> &rhs = production.rhs;
        const std::string &transform = production.transform;
        LlExpandCase item;
        item.id = std::to_string(production.id);
        std::vector<std::string> symbols;
        int startSlot = 1;
        if (transform == "left_base")
        {
            item.left_loop = true;
            item.loop_symbol_id =
                std::to_string(nontermIds.at(rhs.back())) + "u";
            item.keep_action = true;
            symbols.assign(rhs.begin(),
                           rhs.begin() + static_cast<std::ptrdiff_t>(
                                              production.baseRhsLength));
            startSlot = 1;
        }
        else if (transform == "left_continue")
        {
            symbols.assign(rhs.begin(), rhs.end() - 1);
            item.left_loop = true;
            item.loop_symbol_id =
                std::to_string(nontermIds.at(rhs.back())) + "u";
            item.keep_action = true;
            startSlot = 2;
        }
        else if (transform == "left_empty")
        {
            item.empty_tail = true;
            startSlot = 1;
        }
        else
        {
            symbols = rhs;
            item.plain_action = true;
            startSlot = 1;
        }
        item.symbols.reserve(symbols.size());
        for (std::size_t index = symbols.size(); index-- > 0;)
        {
            item.symbols.push_back(
                pushItem(symbols[index], startSlot + static_cast<int>(index)));
        }
        cases.push_back(std::move(item));
    }
    return tmpl::renderValues(Utils::loadTemplate("c99_ll_expand_cases.c"),
                              {{"cases", tmpl::encode(cases)}});
}

/**
 * @brief 生成 %destructor 的按符号分派函数（C / C++17 产物共用）。
 *
 * 统一符号编号（终结符 = Token 号，非终结符 = 终结符个数 + 非终结符编号）→
 * 该符号用 %token <成员> / %type <成员> 绑定的成员 → 该成员的清理片段。同一个
 * 成员的符号合并成一组 case，片段里的 $$ 展开成 pValue->成员。
 *
 * 返回 0 表示这个符号没绑 %destructor，调用方可以退回
 * PARSER_USER_DISCARD_VALUE 钩子——这就是「写了 %destructor 的成员走它、没写
 * 的保持现状」这条约定。
 * @param analysis 分析结果。
 * @param symbolMember 符号 -> %union 成员表；没写 %union 时传 nullptr。
 * @param destructors 成员 -> 清理片段；nullptr / 空表示没写 %destructor。
 * @param error 出错时写入错误说明。
 * @return 分派函数文本；没写 %destructor（或没有任何符号绑到它们）时返回空串。
 */
/**
 * @brief %destructor 分派表里的一项（`c99_destructor_dispatch.c.tmpl` 的元素）。
 */
struct DestructorCase
{
    std::string member;              ///< %union 成员名（注释里写出来的那个）
    std::string names;               ///< 绑了这个成员的符号名，顿号分隔
    std::vector<std::string> labels; ///< 这一组 `case Nu:` 的标号（十进制文本）
    std::vector<std::string> body;   ///< 清理片段按行拆开；空表示整块不输出
};

/**
 * @brief 把一段多行文本按行拆开（保留空行，包括结尾那个空行）。
 * @param text 输入文本。
 * @return 每行一份；空串返回空数组。
 */
std::vector<std::string> splitLines(const std::string &text)
{
    std::vector<std::string> lines;
    std::size_t begin = 0u;
    while (begin <= text.size())
    {
        const std::size_t found = text.find('\n', begin);
        const std::size_t end =
            (found == std::string::npos) ? text.size() : found;
        lines.push_back(text.substr(begin, end - begin));
        if (found == std::string::npos)
        {
            break;
        }
        begin = found + 1u;
    }
    if (lines.size() == 1u && lines.front().empty())
    {
        lines.clear();
    }
    return lines;
}

std::string generatorDestructorDispatchC(
    const AnalysisResult &analysis,
    const std::map<std::string, std::string> *symbolMember,
    const std::map<std::string, std::string> *destructors, Error &error)
{
    const std::map<std::string,
                     std::vector<std::pair<std::string, std::size_t>>>
        cases = CodeGen::destructorSymbolGroups(analysis, symbolMember,
                                                destructors);
    bool anyCase = false;
    for (const auto &entry : cases)
    {
        anyCase = anyCase || !entry.second.empty();
    }
    if (!anyCase)
    {
        // %destructor 的成员没有任何符号绑定：产物维持原样，别多出一个永远
        // 返回 0 的函数。
        return std::string();
    }
    // 分派表整段在模板里（source/templates/c99_destructor_dispatch.c.tmpl）：
    // 这里只把「每个成员一组 case」算成现成的数据，正文缩进也在这里算好。
    std::vector<DestructorCase> rows;
    rows.reserve(cases.size());
    for (const auto &entry : cases)
    {
        const std::vector<std::pair<std::string, std::size_t>> &slots =
            entry.second;
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
        if (slots.empty())
        {
            continue;
        }
        DestructorCase item;
        item.member = entry.first;
        for (std::size_t index = 0u; index < slots.size(); ++index)
        {
            if (index != 0u)
            {
                item.names += "、";
            }
            item.names += slots[index].first;
            item.labels.push_back(std::to_string(slots[index].second));
        }
        // 清理片段按行拆开交给模板：续行的 8 空格缩进由模板写
        // （模板里那条"只有缩进"的续行会被编辑器清掉，所以不留行尾空白）
        item.body = splitLines(translated);
        rows.push_back(std::move(item));
    }
    return tmpl::renderValues(Utils::loadTemplate("c99_destructor_dispatch.c"),
                              {{"cases", tmpl::encode(rows)}});
}

/**
 * @brief Lexer 里“固定字面量最长匹配”的代码。
 * @param hasRegex 是否声明了 %token regex。
 * @param hasCommits 是否声明了 %commit / %trivia。
 * @return C 代码段。
 */
std::string generatorLexerFixedBlockC(bool hasRegex, bool hasCommits)
{
    if (hasRegex || hasCommits)
    {
        return "";
    }
    return Utils::loadTemplate("c99_lexer_fixed_block.c");
}

/**
 * @brief `%token regex` 命中后按 bestType 写语义值的一个 `case`。
 */
struct LexRegexCase
{
    std::string token;  ///< Token 名（产物里是 `TOKEN_<名>`）
    std::string member; ///< 该 Token 绑的 %union 成员
};

/// 取某个符号绑的 %union 成员名（没写 %union / 没绑时返回空串）。定义见文件后半。
std::string lexMemberOf(
    const std::map<std::string, std::string> *symbolMember,
    const std::string &symbol);

/**
 * @brief Lexer 里 %token regex 与固定字面量一起最长匹配的代码。
 * @param hasRegex 是否声明了 %token regex。
 * @param hasCommits 是否声明了 %commit / %trivia。
 * @param regexCases 命中正则后按 Token 写语义值的 case 清单（没写 %union 时为空）。
 * @return C 代码段。
 */
std::string generatorLexerRegexBlockC(bool hasRegex, bool hasCommits,
                                      const std::vector<LexRegexCase>
                                          &regexCases)
{
    if (!hasRegex && !hasCommits)
    {
        return "";
    }
    return tmpl::renderValues(
        Utils::loadTemplate("c99_lexer_regex_block.c"),
        {{"has_regex", hasRegex},
         {"regex_cases", tmpl::encode(regexCases)}});
}

/**
 * @brief 内建双引号字符串规则。
 * @param hasStringRule 用户是否已为 STRING 声明词法规则。
 * @param stringMember STRING 绑的 %union 成员；没写 %union / 没绑时为空串。
 * @return C 代码段。
 */
std::string generatorLexerStringBlockC(bool hasStringRule,
                                       const std::string &stringMember)
{
    if (hasStringRule)
    {
        return "";
    }
    return tmpl::renderValues(Utils::loadTemplate("c99_lexer_string_block.c"),
                              {{"string_member", stringMember}});
}

/**
 * @brief 内建数字规则。
 * @param hasNumberRule 用户是否已为 NUMBER 声明词法规则。
 * @return C 代码段。
 */
/**
 * @brief 内建数字规则。
 * @param hasNumberRule 用户是否已为 NUMBER 声明词法规则。
 * @param numberMember NUMBER 绑的 %union 成员；没绑时为空串。
 * @param unioned 是否写了 %union（没绑成员时那一行不发：联合体里没地方放数字）。
 * @return C 代码段。
 */
std::string generatorLexerNumberBlockC(bool hasNumberRule,
                                       const std::string &numberMember,
                                       bool unioned)
{
    if (hasNumberRule)
    {
        return "";
    }
    return tmpl::renderValues(
        Utils::loadTemplate("c99_lexer_number_block.c"),
        {{"number_member", numberMember},
         {"number_in_union", unioned && numberMember.empty()}});
}

/**
 * @brief Lexer 里标识符识别的代码。
 * @param hasIdentifier 是否声明了 %identifier。
 * @param hasVariableRule 用户是否已为 VARIABLE 声明词法规则。
 * @return C 代码段。
 */
std::string generatorLexerIdentifierBlockC(bool hasIdentifier,
                                           bool hasVariableRule,
                                           const std::string &variantMember)
{
    if (hasIdentifier)
    {
        return tmpl::renderValues(
            Utils::loadTemplate("c99_lexer_identifier_regex.c"),
            {{"variant_member", variantMember}});
    }
    if (hasVariableRule)
    {
        return "";
    }
    return tmpl::renderValues(
        Utils::loadTemplate("c99_lexer_identifier_default.c"),
        {{"variant_member", variantMember}});
}

/**
 * @brief 词法器的固定字面量命中判断 / 跳字符判断（C）。
 * @param spaceChars %space 声明的跳过字符集；nullptr 表示没写（缺省全部空白）。
 * @param hasBlankRule 是否有以空白 / \b 通配开头的固定字面量。
 * @return C 代码段。
 */
std::string generatorLexerSkipHelpersC(const std::string *spaceChars,
                                       bool hasBlankRule)
{
    // 两个辅助函数的正文都在模板里（source/templates/c99_lexer_skip_helpers.c.tmpl）。
    // 既没写 %space 也没有以空白开头的字面量时，「该不该跳过这个字符」这个函数
    // 用不上（跳过逻辑与从前完全一致），只发命中判断。
    const bool hasSkipSet = (spaceChars != nullptr) || hasBlankRule;
    return tmpl::renderValues(
        Utils::loadTemplate("c99_lexer_skip_helpers.c"),
        {{"has_skip_set", hasSkipSet},
         {"space_chars", spaceChars != nullptr ? Utils::varCString(*spaceChars)
                                               : std::string()}});
}

/**
 * @brief 取 Token 前的跳空白循环（C）。
 * @param spaceChars %space 声明的跳过字符集；nullptr 表示没写。
 * @param hasBlankRule 是否有以空白 / \b 通配开头的固定字面量。
 * @return C 代码段。
 */
std::string generatorLexerSkipLoopC(const std::string *spaceChars,
                                    bool hasBlankRule)
{
    // 循环本体在模板里（source/templates/c99_lexer_skip_loop.c.tmpl）：
    // 既没写 %space 也没有以空白开头的字面量时，退回内置 isspace 的老写法。
    const bool hasSkipSet = (spaceChars != nullptr) || hasBlankRule;
    return tmpl::renderValues(
        Utils::loadTemplate("c99_lexer_skip_loop.c"),
        {{"has_skip_set", hasSkipSet}, {"has_blank_rule", hasBlankRule}});
}

/**
 * @brief 生成式 Lexer 与 Token 前移函数的 C 模板。
 * @param hasRegex 是否声明了 %token regex。
 * @param hasIdentifier 是否声明了 %identifier。
 * @param hasCommits 是否声明了 %commit / %trivia。
 * @param tokenDefs 固定字面量 Token。
 * @param regexDefs 正则 Token。
 * @param spaceChars %space 声明的跳过字符集；nullptr 表示没写。
 * @param textOnly 词法模式（只喂文本，没有 Token 数组字段）。
 * @param symbolMember 符号 -> %union 成员表；没写 %union 时传 nullptr。
 * @return C 代码段。
 */
std::string generatorLexerC(
    bool hasRegex, bool hasIdentifier, bool hasCommits,
    const std::map<std::string, std::vector<std::string>> &tokenDefs,
    const std::map<std::string, std::string> &regexDefs,
    const std::string *spaceChars, bool textOnly,
    const std::map<std::string, std::string> *symbolMember)
{
    const auto overridden = [&tokenDefs, &regexDefs](const std::string &name)
    {
        return tokenDefs.count(name) || regexDefs.count(name);
    };
    bool hasBlankRule = false;
    for (const auto &entry : tokenDefs)
    {
        for (const std::string &literal : entry.second)
        {
            hasBlankRule = hasBlankRule || literalCanStartBlank(literal);
        }
    }
    // %union：词素按 `%token <成员>` / `%identifier` 写进 Token 的语义值成员。
    // 原先这是在渲染完的文本上做字符串替换（customizeLexerText），现在改成
    // 把成员名当数据传给模板，由模板自己的 `{% if %}` 决定那几行发不发。
    const std::string numberMember = lexMemberOf(symbolMember, "NUMBER");
    const std::string stringMember = lexMemberOf(symbolMember, "STRING");
    const std::string variantMember = lexMemberOf(symbolMember, "VARIABLE");
    std::vector<LexRegexCase> regexCases;
    for (const auto &entry : regexDefs)
    {
        const std::string member = lexMemberOf(symbolMember, entry.first);
        if (!member.empty())
        {
            regexCases.push_back({entry.first, member});
        }
    }
    // 词法模式（%mode lex）只喂文本：上下文里没有 Token 数组字段，
    // 这一段「数组模式」分支连编译都过不去，直接不发
    return tmpl::renderValues(
        Utils::loadTemplate("c99_lexer.c"),
        {{"array_mode", !textOnly},
         {"lex_skip_helpers",
          generatorLexerSkipHelpersC(spaceChars, hasBlankRule)},
         {"lex_skip_loop", generatorLexerSkipLoopC(spaceChars, hasBlankRule)},
         {"lex_string_block",
          generatorLexerStringBlockC(overridden("STRING"), stringMember)},
         {"lex_regex_block",
          generatorLexerRegexBlockC(hasRegex, hasCommits, regexCases)},
         {"lex_number_block", generatorLexerNumberBlockC(
                                  overridden("NUMBER"), numberMember,
                                  symbolMember != nullptr)},
         {"lex_fixed_block",
          generatorLexerFixedBlockC(hasRegex, hasCommits)},
         {"lex_identifier_block", generatorLexerIdentifierBlockC(
                                      hasIdentifier, overridden("VARIABLE"),
                                      variantMember)}});
}

/**
 * @brief LL(1) 的预测分析表与左递归空尾标记表。
 * @param analysis 分析结果。
 * @return C 代码段。
 */
/**
 * @brief 分析表里的一行（`c99_lr_tables.c.tmpl` 的元素）。
 */
struct LrTableRow
{
    std::vector<std::string> cells; ///< 这一行每个单元格的文本
};

/**
 * @brief 「每一行期望哪些 Token」的扁平表（语法错误时填 ParserError_T）。
 * @param analysis 分析结果。
 * @return C 代码段。
 */
std::string generatorExpectedTablesC(const AnalysisResult &analysis)
{
    const std::vector<std::vector<std::string>> rows =
        expectedTokensByRow(analysis);
    std::map<std::string, int> ids;
    for (std::size_t index = 0; index < analysis.terminals.size(); ++index)
    {
        ids[analysis.terminals[index]] = static_cast<int>(index);
    }
    // 两张表的骨架在模板里（source/templates/c99_expected_tables.c.tmpl）：
    // 这里只算数字——starts 是每行的起始下标（最后补一个总长度），data 是各行的
    // Token 号顺次拼起来。
    std::vector<std::string> starts;
    std::vector<std::string> data;
    std::size_t offset = 0u;
    for (const std::vector<std::string> &row : rows)
    {
        starts.push_back(std::to_string(offset) + "u");
        for (const std::string &token : row)
        {
            data.push_back(std::to_string(ids[token]) + "u");
            ++offset;
        }
    }
    starts.push_back(std::to_string(offset) + "u");
    return tmpl::renderValues(
        Utils::loadTemplate("c99_expected_tables.c"),
        {{"start_count", std::to_string(rows.size() + 1u)},
         {"starts", tmpl::encode(starts)},
         {"data", tmpl::encode(data)}});
}

std::string generatorLlTablesC(const AnalysisResult &analysis,
                               int terminalCount, int nontermCount)
{
    // 列轴：k = 1 还是「Token」（现状，密表，运行时用 current.type 当列号）；
    // k = 0 只有一列（LL(0) 不看 lookahead）；k >= 2 是「用到的 k 元组」，
    // 另发编码表让运行时二分查列。
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
    // 表体全部在模板里（source/templates/c99_ll_tables.c.tmpl）：这里只把每个
    // 单元格算成现成的文本，行与分隔符交给模板的 `{% for %}`。
    std::vector<LrTableRow> table;
    table.reserve(analysis.nonterms.size());
    for (std::size_t row = 0; row < analysis.nonterms.size(); ++row)
    {
        LrTableRow cells;
        cells.cells.reserve(columnKeys.size());
        for (std::size_t column = 0; column < columnKeys.size(); ++column)
        {
            const auto found = analysis.table.find(
                std::make_pair(analysis.nonterms[row], columnKeys[column]));
            cells.cells.push_back(found == analysis.table.end()
                                      ? std::string("-1")
                                      : std::to_string(found->second));
        }
        table.push_back(std::move(cells));
    }
    std::vector<std::string> leftEmpty;
    leftEmpty.reserve(analysis.productions.size());
    for (std::size_t index = 0; index < analysis.productions.size(); ++index)
    {
        leftEmpty.push_back(
            (analysis.productions[index].transform == "left_empty") ? "1u"
                                                                    : "0u");
    }
    const bool hasLookahead = k >= 2;
    std::vector<std::string> codes;
    std::vector<std::string> columns;
    if (hasLookahead)
    {
        std::map<std::string, int> terminalIds;
        for (std::size_t index = 0; index < analysis.terminals.size(); ++index)
        {
            terminalIds[analysis.terminals[index]] = static_cast<int>(index);
        }
        const std::size_t base = analysis.terminals.size();
        std::vector<std::pair<long long, std::size_t>> sortedCodes;
        sortedCodes.reserve(columnKeys.size());
        for (std::size_t column = 0; column < columnKeys.size(); ++column)
        {
            long long code = 0;
            long long scale = 1;
            for (const std::string &symbol : analysis.lookaheadColumns[column])
            {
                code += static_cast<long long>(terminalIds[symbol]) * scale;
                scale *= static_cast<long long>(base);
            }
            sortedCodes.push_back({code, column});
        }
        std::sort(sortedCodes.begin(), sortedCodes.end());
        codes.reserve(sortedCodes.size());
        columns.reserve(sortedCodes.size());
        for (const auto &entry : sortedCodes)
        {
            codes.push_back(std::to_string(entry.first));
            columns.push_back(std::to_string(entry.second) + "u");
        }
    }
    // FOLLOW 集：LL(1) / LL(k) 的错误恢复要用（非可空非终结符的 FOLLOW 不在
    // 预测表里，所以单独发一张「非终结符 × 终结符」的 0/1 表）。LL(0) 没有
    // 「当前 Token 属于谁」的信息，恢复了也用不上，不发。
    const bool hasFollow = k >= 1;
    std::vector<LrTableRow> follow;
    if (hasFollow)
    {
        follow.reserve(analysis.nonterms.size());
        for (std::size_t row = 0; row < analysis.nonterms.size(); ++row)
        {
            const auto followIt = analysis.follow.find(analysis.nonterms[row]);
            const std::set<std::string> *member =
                followIt != analysis.follow.end() ? &followIt->second : nullptr;
            LrTableRow cells;
            cells.cells.reserve(analysis.terminals.size());
            for (std::size_t column = 0; column < analysis.terminals.size();
                 ++column)
            {
                const bool inFollow =
                    (member != nullptr) &&
                    (member->count(analysis.terminals[column]) != 0u);
                cells.cells.push_back(inFollow ? "1u" : "0u");
            }
            follow.push_back(std::move(cells));
        }
    }
    return tmpl::renderValues(
        Utils::loadTemplate("c99_ll_tables.c"),
        {{"nonterm_count", std::to_string(nontermCount)},
         {"column_count",
          std::to_string(columnKeys.empty() ? 1u : columnKeys.size())},
         {"terminal_count", std::to_string(terminalCount)},
         {"table", tmpl::encode(table)},
         {"has_lookahead", hasLookahead},
         {"code_count", std::to_string(codes.size())},
         {"codes", tmpl::encode(codes)},
         {"columns", tmpl::encode(columns)},
         {"has_follow", hasFollow},
         {"follow", tmpl::encode(follow)},
         {"expected_tables", generatorExpectedTablesC(analysis)},
         {"left_empty", tmpl::encode(leftEmpty)}});
}

/**
 * @brief 生成 SLR(1) / LALR(1) 的 ACTION / GOTO 表与产生式元数据表。
 * @param analysis 分析结果。
 * @return C 代码段。
 */
std::string generatorLrTablesC(const AnalysisResult &analysis)
{
    const std::size_t stateCount = analysis.stateCount();
    // 表体全部在模板里（source/templates/c99_lr_tables.c.tmpl）：这里只把每个
    // 单元格算成现成的文本（数字、`INT32_MAX`、`UINT16_MAX`），行与分隔符交给
    // 模板的 `{% for %}`。
    std::vector<LrTableRow> action;
    action.reserve(stateCount);
    for (std::size_t stateId = 0; stateId < stateCount; ++stateId)
    {
        LrTableRow row;
        row.cells.reserve(analysis.terminals.size());
        for (std::size_t index = 0; index < analysis.terminals.size(); ++index)
        {
            const auto found = analysis.action.find(std::make_pair(
                static_cast<int>(stateId), analysis.terminals[index]));
            if (found == analysis.action.end())
            {
                row.cells.emplace_back("0");
            }
            else if (found->second.kind == LrAction::Kind::Shift)
            {
                row.cells.emplace_back(
                    std::to_string(found->second.value + 1));
            }
            else if (found->second.kind == LrAction::Kind::Reduce)
            {
                row.cells.emplace_back(
                    std::to_string(-found->second.value - 1));
            }
            else
            {
                row.cells.emplace_back("INT32_MAX");
            }
        }
        action.push_back(std::move(row));
    }
    std::vector<LrTableRow> gotoRows;
    gotoRows.reserve(stateCount);
    for (std::size_t stateId = 0; stateId < stateCount; ++stateId)
    {
        LrTableRow row;
        row.cells.reserve(analysis.nonterms.size());
        for (std::size_t index = 0; index < analysis.nonterms.size(); ++index)
        {
            const auto found = analysis.gotoTable.find(std::make_pair(
                static_cast<int>(stateId), static_cast<int>(index)));
            row.cells.push_back(found == analysis.gotoTable.end()
                                    ? std::string("UINT16_MAX")
                                    : std::to_string(found->second));
        }
        gotoRows.push_back(std::move(row));
    }
    std::map<std::string, int> nontermIds;
    for (std::size_t index = 0; index < analysis.nonterms.size(); ++index)
    {
        nontermIds[analysis.nonterms[index]] = static_cast<int>(index);
    }
    std::vector<std::string> lengths;
    std::vector<std::string> lhsIds;
    lengths.reserve(analysis.productions.size());
    lhsIds.reserve(analysis.productions.size());
    for (std::size_t index = 0; index < analysis.productions.size(); ++index)
    {
        const Production &production = analysis.productions[index];
        lengths.push_back(std::to_string(production.rhs.size()) + "u");
        lhsIds.push_back(std::to_string(nontermIds[production.lhs]) + "u");
    }
    // LR(k) / LALR(k)：ACTION 分两层。第一层（s_ParserAction）只放移进；
    // 归约/接受按 k 元组展望符建列，放在 s_ParserReduce 里。运行时先按
    // 窗口算编码、二分查列，命中就用这一列的归约，否则回落到移进层。
    const bool hasLookahead = analysis.lookaheadK > 1;
    std::vector<LrTableRow> reduce;
    std::vector<LrTableRow> keys;
    std::vector<std::string> codes;
    std::vector<std::string> columns;
    if (hasLookahead)
    {
        const std::size_t columnCount = analysis.lookaheadColumns.size();
        const std::size_t k = static_cast<std::size_t>(analysis.lookaheadK);
        std::map<std::string, int> terminalIds;
        for (std::size_t index = 0; index < analysis.terminals.size(); ++index)
        {
            terminalIds[analysis.terminals[index]] = static_cast<int>(index);
        }
        reduce.reserve(stateCount);
        for (std::size_t stateId = 0; stateId < stateCount; ++stateId)
        {
            LrTableRow row;
            row.cells.reserve(columnCount);
            for (std::size_t column = 0; column < columnCount; ++column)
            {
                const std::string key =
                    lookaheadKeyOf(analysis.lookaheadColumns[column]);
                const auto found = analysis.action.find(std::make_pair(
                    static_cast<int>(stateId), key));
                if ((found != analysis.action.end()) &&
                    (found->second.kind == LrAction::Kind::Reduce))
                {
                    row.cells.emplace_back(
                        std::to_string(-found->second.value - 1));
                }
                else if ((found != analysis.action.end()) &&
                         (found->second.kind == LrAction::Kind::Accept))
                {
                    row.cells.emplace_back("INT32_MAX");
                }
                else
                {
                    row.cells.emplace_back("0");
                }
            }
            reduce.push_back(std::move(row));
        }
        // 编码 = Σ Token号 × 终结符个数^i（低位是窗口头），与运行时的
        // Parser_LookaheadCode 一致；按编码升序排列好让运行时二分
        const std::size_t base = analysis.terminals.size();
        std::vector<std::pair<long long, std::size_t>> sortedCodes;
        keys.reserve(columnCount);
        for (std::size_t column = 0; column < columnCount; ++column)
        {
            long long code = 0;
            long long scale = 1;
            LrTableRow row;
            row.cells.reserve(k);
            for (std::size_t index = 0; index < k; ++index)
            {
                const std::string &symbol =
                    analysis.lookaheadColumns[column][index];
                const int id = terminalIds[symbol];
                row.cells.push_back(std::to_string(id) + "u");
                code += static_cast<long long>(id) * scale;
                scale *= static_cast<long long>(base);
            }
            keys.push_back(std::move(row));
            sortedCodes.push_back({code, column});
        }
        std::sort(sortedCodes.begin(), sortedCodes.end());
        codes.reserve(sortedCodes.size());
        columns.reserve(sortedCodes.size());
        for (const auto &entry : sortedCodes)
        {
            codes.push_back(std::to_string(entry.first));
            columns.push_back(std::to_string(entry.second) + "u");
        }
    }
    return tmpl::renderValues(
        Utils::loadTemplate("c99_lr_tables.c"),
        {{"state_count", std::to_string(stateCount)},
         {"terminal_count", std::to_string(analysis.terminals.size())},
         {"nonterm_count", std::to_string(analysis.nonterms.size())},
         {"action", tmpl::encode(action)},
         {"goto_rows", tmpl::encode(gotoRows)},
         {"lengths", tmpl::encode(lengths)},
         {"lhs_ids", tmpl::encode(lhsIds)},
         {"has_lookahead", hasLookahead},
         {"lookahead_k", std::to_string(analysis.lookaheadK)},
         {"column_count", std::to_string(analysis.lookaheadColumns.size())},
         {"reduce", tmpl::encode(reduce)},
         {"keys", tmpl::encode(keys)},
         {"codes", tmpl::encode(codes)},
         {"columns", tmpl::encode(columns)},
         {"expected_tables", generatorExpectedTablesC(analysis)}});
}

/**
 * @brief 演示 main 里的 `%commit` + location 打印片段（C99）。
 * @param commits 文法里是否写了 %commit / %trivia。
 * @return C 代码段；没写时为空串（演示输出与从前一样）。
 */
std::string customDemoCommitPrintC(bool commits)
{
    if (!commits)
    {
        return std::string();
    }
    // 整段固定的 C 片段，没有任何变量：模板原样取出来即可，不必过模板引擎。
    return std::string(Utils::loadTemplate("c99_demo_commit_print.c"));
}

/**
 * @brief Token 名称表的行数据。
 * @param terminals 终结符列表。
 * @return 每个 Token 的 C 字符串字面量（含引号，末尾那个是 "INVALID"）。
 */
std::vector<std::string> generatorTokenNamesC(
    const std::vector<std::string> &terminals)
{
    std::vector<std::string> names;
    names.reserve(terminals.size() + 1u);
    for (const std::string &terminal : terminals)
    {
        names.push_back(Utils::varCString(terminal));
    }
    names.push_back(Utils::varCString("INVALID"));
    return names;
}

/**
 * @brief 判断文本里是否含有非空白字符（对应 Python 的 str.strip()）。
 * @param text 输入文本。
 * @return 含非空白字符返回 true。
 */
bool hasVisibleText(const std::string &text)
{
    const std::size_t found = text.find_first_not_of(" \t\n\r\v\f");
    return found != std::string::npos;
}

/**
 * @brief 判断是否需要生成 parser_user.h。
 *
 * 有 %code ... %endcode 用户代码、或者写了 %value_type / %union（语义值类型由生成器
 * 追加）时都要生成；都没有时 ParserValue_T 走 parser.h 里的默认 typedef int。
 *
 * @param userCode %code ... %endcode 内容。
 * @param unionBody %union 的花括号内部原文；没写 %union 时为空。
 * @param valueType %value_type 的类型原文；没写时为空。
 * @return 需要生成 parser_user.h 返回 true。
 */
bool hasCustomUserHeader(const std::string &userCode,
                         const std::string &unionBody,
                         const std::string &valueType)
{
    return hasVisibleText(userCode) || !unionBody.empty() || !valueType.empty();
}

/**
 * @brief 由 %union 生成语义值类型定义。
 *
 * 成员声明原样贴进去，生成期不解析 C 声明符：真正的声明符解析交给 C 编译器。
 *
 * @param unionBody %union 的花括号内部原文。
 * @return 形如 "typedef union { ... } ParserValue_T;" 的文本。
 */
std::string unionTypedefC(const std::string &unionBody)
{
    std::string text =
        "/* 语义值类型：由文法的 %union 生成，成员声明原样来自文法 */\n";
    text += "typedef union {";
    text += unionBody;
    if ((!unionBody.empty()) && (unionBody.back() != '\n'))
    {
        text += "\n";
    }
    text += "} ParserValue_T;\n";
    return text;
}

/**
 * @brief 由 %value_type 生成语义值类型定义。
 *
 * 类型原文原样贴进去（`double`、`struct Point *` 都行），真正的声明解析交给
 * C 编译器。
 *
 * @param valueType %value_type 的类型原文。
 * @return 形如 "typedef double ParserValue_T;" 的文本。
 */
std::string valueTypeTypedefC(const std::string &valueType)
{
    return "/* 语义值类型：由文法的 %value_type 生成 */\n"
           "typedef " +
           valueType + " ParserValue_T;\n";
}

/**
 * @brief 取符号绑定的 %union 成员名。
 * @param symbolMember 符号 -> 成员表；没写 %union 时传 nullptr。
 * @param symbol 符号名。
 * @return 成员名；没写 %union 或该符号没绑成员时返回空串。
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
 * @brief 生成 custom 模式的内部结构定义（LL 语义帧 / LR 语义栈）。
 * @param mode 生成模式。
 * @param modeLabel 模式标签（如 SLR(1)）。
 * @param tree 是否 %tree。
 * @param destructors 是否生成了 %destructor 分派（LL 语义帧要按槽位记符号）。
 * @return C 代码段。
 */
std::string generatorCustomItemTypesC(const std::string &mode,
                                      const std::string &modeLabel, bool tree,
                                      bool destructors,
                                      std::size_t terminalCount)
{
    const std::string llTreeFields =
        tree ? "    ParserTree_T *pTreeRoot;\n"
               "    uint16_t pTreeDepth;\n"
               "    ParserTree_T *pTreePath[PARSER_MAX_STACK_DEPTH];\n"
             : "";
    if (isLrMode(mode))
    {
        return tmpl::renderValues(
            Utils::loadTemplate("c99_custom_lr_item_types.c"),
            {{"lr_label", modeLabel},
             {"terminal_count", std::to_string(terminalCount)},
             {"custom_lr_node_field",
              tree ? "    ParserTree_T *nodes[PARSER_MAX_STACK_DEPTH];\n"
                   : std::string()}});
    }
    if (isDdMode(mode))
    {
        // DD 不用 LL 的分析栈与语义帧，只多一组「回溯快照」字段
        return tmpl::renderValues(
            Utils::loadTemplate("c99_dd_item_types.c"), {{"tree", tree}});
    }
    // %destructor：丢弃语义帧时要按槽位查「这个值绑的是哪个成员」，所以帧上
    // 得记下每个槽位与 resultValue 的符号（统一编号）
    return tmpl::renderValues(
        Utils::loadTemplate("c99_custom_ll_item_types.c"),
        {{"custom_ll_frame_symbols",
          destructors ? "    uint16_t symbols[PARSER_VALUE_SLOTS];\n"
                        "    uint16_t resultSymbol;\n"
                        "    uint8_t resultDone;\n"
                      : std::string()},
         {"custom_ll_tree_fields", llTreeFields}});
}

/**
 * @brief 生成 custom 模式在分析运行时之前发射的语法树片段。
 *
 * LR 的 Parser_LRShift / Parser_LRReduce 需要调用建树钩子，因此节点工具与
 * Parser_TreeMakeLeaf / Parser_TreeMakeBranch 必须在运行时之前；LL 只需要
 * 节点工具，建树循环放在运行时之后。
 * @param mode 生成模式。
 * @param analysis 分析结果。
 * @param tree 是否 %tree。
 * @return C 代码段。
 */
std::string generatorCustomTreeHelpersC(const std::string &mode,
                                        const AnalysisResult &analysis,
                                        bool tree)
{
    if (!tree)
    {
        return "";
    }
    const bool lr = isLrMode(mode);
    std::string text;
    if (lr)
    {
        std::vector<std::string> nontermNames;
        for (const std::string &name : analysis.nonterms)
        {
            nontermNames.push_back(Utils::varCString(name));
        }
        // 名字表的行与逗号交给模板（`{% for %}`），这里只把转义过的名字摆好。
        // 树表就留在 parser.c 里（原来模板里有对「分文件用」的
        // `__SEC_TREE_TABLES_*` 标记要在这里删掉；迁到 .tmpl 时标记已经
        // 从模板里去掉，段内的换行原样保留，产物逐字节不变）
        text += tmpl::renderValues(Utils::loadTemplate("c99_lr_tree_header.c"),
                                   {{"tree_nonterm_names",
                                     tmpl::encode(nontermNames)}});
    }
    text += Utils::loadTemplate("c99_custom_tree_helpers.c");
    if (lr)
    {
        text += Utils::loadTemplate("c99_custom_lr_tree_body.c");
    }
    return text;
}

/**
 * @brief 生成 custom 模式在分析运行时之后发射的语法树入口。
 * @param mode 生成模式。
 * @param analysis 分析结果。
 * @param tree 是否 %tree。
 * @return C 代码段。
 */
std::string generatorCustomTreeEntryC(const std::string &mode,
                                      const AnalysisResult &analysis,
                                      bool tree)
{
    if (!tree)
    {
        return "";
    }
    if (isLrMode(mode))
    {
        return Utils::loadTemplate("c99_custom_lr_tree_api.c");
    }
    if (isDdMode(mode))
    {
        // DD 的建树回环（Parser_TreeParseLoop / Parser_ParseTreeText）在
        // c99_dd_tree_entry.c 里，签名与 LL(1) 那一份一致
        std::map<std::string, int> ddIds;
        for (std::size_t index = 0u; index < analysis.nonterms.size(); ++index)
        {
            ddIds[analysis.nonterms[index]] = static_cast<int>(index);
        }
        const auto found = ddIds.find(analysis.grammar.start);
        return tmpl::renderValues(
            Utils::loadTemplate("c99_dd_tree_entry.c"),
            {{"start_id", std::to_string(found == ddIds.end() ? 0
                                                              : found->second)}});
    }
    std::set<std::string> terminalSet(analysis.terminals.begin(),
                                      analysis.terminals.end());
    std::map<std::string, int> nontermIds;
    for (std::size_t index = 0; index < analysis.nonterms.size(); ++index)
    {
        nontermIds[analysis.nonterms[index]] = static_cast<int>(index);
    }
    const std::size_t termCount = analysis.terminals.size();
    std::vector<int> prodStarts;
    std::vector<int> prodLengths;
    std::vector<int> prodSymbols;
    for (const Production &production : analysis.productions)
    {
        prodStarts.push_back(static_cast<int>(prodSymbols.size()));
        prodLengths.push_back(static_cast<int>(production.rhs.size()));
        for (const std::string &symbol : production.rhs)
        {
            if (terminalSet.count(symbol))
            {
                const auto found =
                    std::find(analysis.terminals.begin(),
                              analysis.terminals.end(), symbol);
                prodSymbols.push_back(static_cast<int>(
                    found - analysis.terminals.begin()));
            }
            else
            {
                prodSymbols.push_back(static_cast<int>(termCount) +
                                      nontermIds.at(symbol));
            }
        }
    }
    if (prodSymbols.empty())
    {
        prodSymbols.push_back(0);
    }
    // 数字表只转成文本数组，缩进 / 逗号 / `u` 后缀由模板出
    const auto intRows = [](const std::vector<int> &values)
    {
        std::vector<std::string> rows;
        rows.reserve(values.size());
        for (const int value : values)
        {
            rows.push_back(std::to_string(value));
        }
        return rows;
    };
    std::vector<std::string> nontermNames;
    for (const std::string &name : analysis.nonterms)
    {
        nontermNames.push_back(Utils::varCString(name));
    }
    const auto startFound = nontermIds.find(analysis.grammar.start);
    return tmpl::renderValues(
        Utils::loadTemplate("c99_custom_tree_ll1.c"),
        {{"tree_nonterm_names", tmpl::encode(nontermNames)},
         {"tree_prod_starts", tmpl::encode(intRows(prodStarts))},
         {"tree_prod_lengths", tmpl::encode(intRows(prodLengths))},
         {"tree_prod_symbols", tmpl::encode(intRows(prodSymbols))},
         {"terminal_count", std::to_string(analysis.terminals.size())},
         {"nonterm_count", std::to_string(analysis.nonterms.size())},
         {"start_id", std::to_string(startFound == nontermIds.end()
                                          ? 0
                                          : startFound->second)}});
}

/**
 * @brief 生成 %sync 的同步 Token 判定函数（没写 %sync 时返回空串）。
 *
 * LR 家族还要一个「这个状态能不能移进某个同步 Token」的判定，LL(1) 不需要。
 * @param mode 生成模式。
 * @param terminals 终结符列表。
 * @param syncTokens %sync 声明的同步 Token；没写 %sync 时可为 nullptr。
 * @return C 代码段。
 */
std::string syncPredicateC(const std::string &mode,
                           const std::vector<std::string> &terminals,
                           const std::vector<std::string> *syncTokens)
{
    if ((syncTokens == nullptr) || syncTokens->empty())
    {
        return "";
    }
    // 只留真的出现在终结符表里的同步 Token；一个都没有就整段不发
    std::vector<std::string> tokens;
    for (const std::string &token : *syncTokens)
    {
        if (std::find(terminals.begin(), terminals.end(), token) ==
            terminals.end())
        {
            continue;
        }
        tokens.push_back(token);
    }
    if (tokens.empty())
    {
        return "";
    }
    // 判定函数本体在模板里（source/templates/c99_sync_predicate.c.tmpl）：
    // `switch` 的 case 由 `{% for %}` 出，「能不能移进同步 Token」只有 LR 发。
    return tmpl::renderValues(
        Utils::loadTemplate("c99_sync_predicate.c"),
        {{"tokens", tmpl::encode(tokens)},
         {"lr_mode", isLrMode(mode)},
         {"terminal_count", std::to_string(terminals.size())}});
}

std::string generatorDdRuntimeC(const AnalysisResult &analysis, bool tree,
                                const std::map<std::string, std::string>
                                    *symbolMember,
                                Error &error)
{
    const std::string actionCases =
        CodeGen::actionCases(analysis, "dd", symbolMember, false, error);
    if (!error.empty())
    {
        return {};
    }
    // 右部符号表与 %tree 的非终结符名表也在这一份里发（DD 不走 LL 的建树模板）
    std::set<std::string> terminalSet(analysis.terminals.begin(),
                                      analysis.terminals.end());
    std::map<std::string, int> nontermIds;
    for (std::size_t index = 0u; index < analysis.nonterms.size(); ++index)
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
            const auto found = std::find(analysis.terminals.begin(),
                                         analysis.terminals.end(), symbol);
            if (found != analysis.terminals.end())
            {
                prodSymbols.push_back(std::to_string(
                    static_cast<std::size_t>(found - analysis.terminals.begin())));
            }
            else
            {
                prodSymbols.push_back(std::to_string(
                    termCount +
                    static_cast<std::size_t>(nontermIds.at(symbol))));
            }
        }
    }
    if (prodSymbols.empty())
    {
        prodSymbols.push_back("0");
    }
    // 每个非终结符的产生式区间
    std::vector<std::string> ddStarts;
    std::vector<std::string> ddCounts;
    const std::size_t total = analysis.productions.size();
    for (const std::string &nonterm : analysis.nonterms)
    {
        std::size_t first = total;
        std::size_t count = 0u;
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
        }
        ddStarts.push_back(std::to_string(first == total ? 0u : first));
        ddCounts.push_back(std::to_string(count));
    }
    std::vector<std::string> nontermNames;
    for (const std::string &name : analysis.nonterms)
    {
        nontermNames.push_back(Utils::varCString(name));
    }
    const auto startFound = nontermIds.find(analysis.grammar.start);
    return tmpl::renderValues(
        Utils::loadTemplate("c99_dd_runtime.c"),
        {{"action_cases", actionCases},
         {"tree", tree},
         {"terminal_count", std::to_string(analysis.terminals.size())},
         {"start_id", std::to_string(startFound == nontermIds.end()
                                          ? 0
                                          : startFound->second)},
         {"prod_starts", tmpl::encode(prodStarts)},
         {"prod_lengths", tmpl::encode(prodLengths)},
         {"prod_symbols", tmpl::encode(prodSymbols)},
         {"tree_nonterm_names", tmpl::encode(nontermNames)},
         {"prod_dd_starts", tmpl::encode(ddStarts)},
         {"prod_dd_counts", tmpl::encode(ddCounts)}});
}

/**
 * @brief 生成 custom 模式的分析运行时（动作翻译成普通 C 表达式）。
 * @param mode 生成模式。
 * @param analysis 分析结果。
 * @param modeLabel 模式标签（如 SLR(1)）。
 * @param modeName 模式名（如 SLR）。
 * @param tree 是否 %tree。
 * @param symbolMember 符号到 %union 成员名的映射；没写 %union 时传 nullptr。
 * @param discardDispatch %destructor 的按符号分派函数文本；没写 %destructor
 *        时为空串。
 * @param syncTokens %sync 声明的同步 Token；没写 %sync 时可为 nullptr。
 * @param error 出错时写入错误说明。
 * @return C 代码段；语义动作非法时返回空串并把错误写进 error。
 */
std::string generatorCustomRuntimeC(const std::string &mode,
                                    const AnalysisResult &analysis,
                                    const std::string &modeLabel,
                                    const std::string &modeName, bool tree,
                                    const std::map<std::string, std::string>
                                        *symbolMember,
                                    const std::string &discardDispatch,
                                    const std::vector<std::string> *syncTokens,
                                    Error &error)
{
    const bool hasDestructors = !discardDispatch.empty();
    if (isDdMode(mode))
    {
        // DD 的自成一档：不建表、按产生式声明顺序回溯；%destructor 在生成期
        // 就被拒了，所以这里没有丢弃分派可发
        return generatorDdRuntimeC(analysis, tree, symbolMember, error);
    }
    if (isLrMode(mode))
    {
        const std::string actionCases = CodeGen::actionCases(
            analysis, analysis.mode, symbolMember, false, error);
        if (!error.empty())
        {
            return {};
        }
        // 丢弃一个栈上语义值 / 整栈清空这两段、以及「放弃时清栈」那几句调用，
        // 都由模板里的 `{% if has_destructors %}` / `{% if has_discard_stack %}`
        // 决定（产物逐字节不变：分支写法与原先是两个字符串二选一等同）。
        // 两者都没有时整段（函数 + 调用点）都不发。
        const bool hasDiscardStack = tree || hasDestructors;
        // 语法树那些片段（%tree 下的 treeMode 参数、叶子/分支节点、ppTree 出参…）
        // 已经内联在 `c99_custom_lr_runtime.c.tmpl` 里，由模板自己的
        // `{% if tree %}` 决定发不发；这里只传那个布尔量。
        const std::string text = tmpl::renderValues(
            Utils::loadTemplate("c99_custom_lr_runtime.c"),
            {{"custom_sync_predicate",
              syncPredicateC(mode, analysis.terminals, syncTokens)},
             {"lr_label", modeLabel},
             {"lr_name", modeName},
             {"lr_action_cases", actionCases},
             {"terminal_count", std::to_string(analysis.terminals.size())},
             {"lr_state_count", std::to_string(analysis.stateCount())},
             {"has_destructors", hasDestructors},
             {"has_discard_stack", hasDiscardStack},
             {"has_union", symbolMember != nullptr},
             {"tree", tree}});
        return discardDispatch + text;
    }
    std::map<std::string, int> nontermIds;
    for (std::size_t index = 0; index < analysis.nonterms.size(); ++index)
    {
        nontermIds[analysis.nonterms[index]] = static_cast<int>(index);
    }
    const std::string expandCases =
        generatorExpandCases(analysis, analysis.terminals, nontermIds);
    const std::string actionCases = CodeGen::actionCases(
        analysis, "ll1", symbolMember, false, error);
    if (!error.empty())
    {
        return {};
    }
    const auto startFound = nontermIds.find(analysis.grammar.start);
    const std::string startId = std::to_string(
        startFound == nontermIds.end() ? 0 : startFound->second);
    const std::string terminalCount =
        std::to_string(analysis.terminals.size());
    const std::string nontermCount = std::to_string(analysis.nonterms.size());
    // %destructor：语义帧按槽位记下「这个槽位是哪个文法符号」，丢弃时才查得到
    // 该调哪个成员。符号用统一编号（终结符 = Token 号，非终结符 = 终结符个数 +
    // 编号），与生成的分派表一致。
    //
    // 只记「真的写过值」的槽位：终结符在 Parser_CaptureTerminal 落值、
    // 子非终结符在 Parser_CompleteFrame 把结果交给父帧时落符号；帧自己
    // 的 resultValue 用 resultDone 标「动作跑过没有」。没写过值的槽位
    // 是零值占位，调 %destructor 就可能在用户代码里释放垃圾指针。
    //
    // 左递归改写的累加槽（values[1] 从 resultValue 复制过来）故意不记：
    // 那份所有权已经由「上一轮结果」这个身份交给动作了，记上会重复清理。
    //
    // 这些片段**都在模板里**（c99_custom_ll_runtime.c / c99_custom_ll_loop.c），
    // 由模板自己的 `{% if has_destructors %}` 决定发不发——这里只传那个布尔量。
    const std::string runtime = tmpl::renderValues(
        Utils::loadTemplate("c99_custom_ll_runtime.c"),
        {{"expand_cases", expandCases},
         {"action_cases", actionCases},
         {"terminal_count", terminalCount},
         {"has_destructors", hasDestructors},
         {"has_union", symbolMember != nullptr}});
    const std::string loop = tmpl::renderValues(
        Utils::loadTemplate("c99_custom_ll_loop.c"),
        {{"custom_sync_predicate",
          syncPredicateC(mode, analysis.terminals, syncTokens)},
         {"start_id", startId},
         {"terminal_count", terminalCount},
         {"nonterm_count", nontermCount},
         {"has_destructors", hasDestructors}});
    return discardDispatch + runtime + loop;
}


} // namespace

bool CodeGenC99::lexDfa(
    const std::map<std::string, std::vector<std::string>> &tokenDefs,
    const std::map<std::string, std::string> &regexDefs,
    const std::optional<std::string> &identifierPattern,
    const std::vector<CommitSpec> &commits,
    const std::map<std::string, int> &tokenLexOrder, std::string &outDecls,
    std::string &outTables, Error &error)
{
    return generatorLexDfaC(tokenDefs, regexDefs,
                            identifierPattern.value_or(""),
                            identifierPattern.has_value(), commits,
                            tokenLexOrder, outDecls, outTables, error);
}

std::string CodeGenC99::lrTables(const AnalysisResult &analysis)
{
    return generatorLrTablesC(analysis);
}

std::string CodeGenC99::llTables(const AnalysisResult &analysis,
                              int terminalCount, int nontermCount)
{
    return generatorLlTablesC(analysis, terminalCount, nontermCount);
}

std::string CodeGenC99::tokenNames(const std::vector<std::string> &terminals)
{
    // 行与逗号交给模板（source/templates/c99_token_names.c.tmpl）
    return tmpl::renderValues(
        Utils::loadTemplate("c99_token_names.c"),
        {{"names", tmpl::encode(generatorTokenNamesC(terminals))}});
}

std::string CodeGenC99::tableDeclsC(const std::string &mode,
                                 const AnalysisResult &analysis)
{
    const bool lrMode = isLrMode(mode);
    // 「这里期望哪些 Token」表：LR 按状态、LL 按非终结符，行数取两者之一
    const std::size_t expectedRows =
        lrMode ? analysis.stateCount() : analysis.nonterms.size();
    // LR(k) 归约表的列数（没算过 k 元组列时占 1 列）
    const std::size_t reduceColumns =
        analysis.lookaheadColumns.empty() ? 1u
                                         : analysis.lookaheadColumns.size();
    // 预测分析表的列数：k = 1 用终结符数，否则用 k 元组列数（至少 1 列）
    const std::size_t llColumns =
        (analysis.lookaheadK == 1)
            ? analysis.terminals.size()
            : std::max<std::size_t>(1u, analysis.lookaheadColumns.size());
    // 声明段整段在模板里（source/templates/c99_table_decls.h.tmpl）：
    // LR / LL 两套 extern 由 `{% if lr_mode %}` 选。
    return tmpl::renderValues(
        Utils::loadTemplate("c99_table_decls.h"),
        {{"lr_mode", lrMode},
         {"state_count", std::to_string(analysis.stateCount())},
         {"terminal_count", std::to_string(analysis.terminals.size())},
         {"nonterm_count", std::to_string(analysis.nonterms.size())},
         {"expected_rows", std::to_string(expectedRows)},
         {"expected_start_count", std::to_string(expectedRows + 1u)},
         {"expected_fallback", std::string(expectedFallbackText(mode))},
         {"lr_reduce", analysis.lookaheadK > 1},
         {"reduce_columns", std::to_string(reduceColumns)},
         {"ll_columns", std::to_string(llColumns)},
         {"ll_lookahead", analysis.lookaheadK >= 2},
         {"ll_follow", analysis.lookaheadK >= 1}});
}

std::string CodeGenC99::customUserInclude(const std::string &userCode,
                                       const std::string &unionBody,
                                       const std::string &valueType)
{
    if (!hasCustomUserHeader(userCode, unionBody, valueType))
    {
        return "";
    }
    return "#include \"parser_user.h\"\n\n"
           "#define PARSER_USER_HEADER_INCLUDED 1\n";
}

std::optional<std::string> CodeGenC99::customUserH(const std::string &userCode,
                                                const std::string &unionBody,
                                                const std::string &valueType)
{
    const bool hasUnion = !unionBody.empty();
    const bool hasValueType = !valueType.empty();
    if (!hasUnion && !hasValueType)
    {
        if (!hasVisibleText(userCode))
        {
            return std::nullopt;
        }
        return userCode;
    }
    // 写了 %union / %value_type：用户代码排在前面（类型可以引用用户代码里
    // 定义的类型），语义值类型追加在后面
    std::string text = userCode;
    if (hasVisibleText(text))
    {
        if (text.back() != '\n')
        {
            text += "\n";
        }
        text += "\n";
    }
    return text + (hasUnion ? unionTypedefC(unionBody)
                            : valueTypeTypedefC(valueType));
}

std::string CodeGenC99::customParserH(const std::string &defines,
                                   const std::string &errorType,
                                   const std::string &userCode,
                                   const std::string &unionBody,
                                   const std::string &valueType, bool tree,
                                   const std::string &modeLabel)
{
    const bool userHeader =
        hasCustomUserHeader(userCode, unionBody, valueType);
    std::string include;
    if (userHeader)
    {
        include = "#ifndef PARSER_USER_HEADER_INCLUDED\n";
        if (!unionBody.empty())
        {
            // 写了 %union 时 parser_token.h 也会包含 parser_user.h：
            // 两边都定义守卫宏，谁先展开谁生效，避免重复定义 ParserValue_T
            include += "#define PARSER_USER_HEADER_INCLUDED 1\n";
        }
        include += "#include \"parser_user.h\"\n"
                   "#endif\n";
    }
    std::string allDefines = defines;
    if (!userHeader)
    {
        // 没有 %code ... %endcode 也没有 %union 时给一个默认标量语义值类型，保证工程
        // 开箱即编译；%tree 文法只建语法树、不存语义值，用默认类型即可
        allDefines = "typedef int ParserValue_T;\n\n" + defines;
    }
    return tmpl::renderValues(
        Utils::loadTemplate("c99_parser_h_custom.h"),
        {{"mode_name", modeLabel},
         {"user_include", include},
         {"defines", allDefines},
         {"error_type", errorType},
         {"tree_api",
          tree ? Utils::loadTemplate("c99_custom_tree_api.h") : std::string()}});
}

std::string CodeGenC99::lexerOnlyH(const std::string &defines,
                                const std::string &errorType,
                                const std::string &userCode,
                                const std::string &unionBody,
                                const std::string &valueType)
{
    const bool userHeader =
        hasCustomUserHeader(userCode, unionBody, valueType);
    std::string include;
    if (userHeader)
    {
        include = "#ifndef PARSER_USER_HEADER_INCLUDED\n";
        if (!unionBody.empty())
        {
            // 写了 %union 时 parser_token.h 也会包含 parser_user.h：两边都定义
            // 守卫宏，谁先展开谁生效，避免重复定义 ParserValue_T
            include += "#define PARSER_USER_HEADER_INCLUDED 1\n";
        }
        include += "#include \"parser_user.h\"\n"
                   "#endif\n";
    }
    std::string allDefines = defines;
    if (!userHeader)
    {
        // 没有 %code ... %endcode / %union 时给个默认标量语义值类型，保证开箱即编译
        allDefines = "typedef int ParserValue_T;\n\n" + defines;
    }
    return tmpl::renderValues(
        Utils::loadTemplate("c99_lexer_h_custom.h"),
        {{"mode_name", "LEX"},
         {"user_include", include},
         {"defines", allDefines},
         {"error_type", errorType}});
}

std::string CodeGenC99::lexerOnlyC(
    const std::map<std::string, std::vector<std::string>> &tokenDefs,
    const LexSpec &lexSpec, const std::string *spaceChars,
    const std::string &userCode, const std::string &unionBody,
    const std::string &valueType,
    const std::map<std::string, std::string> *symbolMember)
{
    const std::string lexer =
        generatorLexerC(!lexSpec.regex.empty(), lexSpec.identifier.has_value(),
                        !lexSpec.commits.empty(), tokenDefs, lexSpec.regex,
                        spaceChars,
                        /*textOnly=*/true, symbolMember);
    return tmpl::renderValues(
        Utils::loadTemplate("c99_lexer_c_custom.c"),
        {{"mode_name", "LEX"},
         {"user_include", customUserInclude(userCode, unionBody, valueType)},
         {"lex_rules", generatorLexRules(tokenDefs, lexSpec.tokenLexOrder)},
         {"lexer", lexer}});
}

std::string CodeGenC99::customParserTokenH(const std::string &tokenTypes,
                                        const std::string &lexDecls,
                                        const std::string &modeLabel)
{
    const std::string lexBlock = lexDecls.empty() ? "" : ("\n" + lexDecls);
    return tmpl::renderValues(
        Utils::loadTemplate("c99_parser_token_h_custom.h"),
        {{"mode_name", modeLabel},
         {"token_types", tokenTypes},
         {"lex_decls", lexBlock}});
}

std::string CodeGenC99::customParserTokenC(const std::string &tokenNames,
                                        const std::string &lexDecls,
                                        const std::string &lexTables,
                                        const std::string &modeLabel)
{
    const std::string body = tmpl::renderValues(
        Utils::loadTemplate("c99_token_impl_custom.c"),
        {{"token_names", tokenNames}, {"lex_dfa_decls", lexDecls}});
    return tmpl::renderValues(Utils::loadTemplate("c99_parser_token.c"),
                              {{"mode_name", modeLabel},
                               {"token_impl", body + lexTables}});
}

std::string CodeGenC99::customMainC(bool tree, bool commits,
                                 const std::string &modeLabel)
{
    std::string demo;
    if (tree)
    {
        demo = tmpl::renderValues(
            Utils::loadTemplate("c99_demo_main_custom.c"),
            {{"demo_tree_decl", std::string()},
             {"demo_tree_fallback",
              "    {\n"
              "        ParserTree_T *pTree = Parser_ParseTreeText(pInput);\n"
              "        if (pTree != NULL) {\n"
              "            printf(\"语法树:\\n\");\n"
              "            Parser_TreePrint(pTree);\n"
              "            Parser_TreeFree(pTree);\n"
              "        }\n"
              "    }\n"},
             {"demo_commits", customDemoCommitPrintC(commits)}});
    }
    else
    {
        demo = tmpl::renderValues(
            Utils::loadTemplate("c99_demo_main_custom.c"),
            {{"demo_tree_decl", std::string()},
             {"demo_tree_fallback", std::string()},
             {"demo_commits", customDemoCommitPrintC(commits)}});
    }
    return tmpl::renderValues(Utils::loadTemplate("c99_main_custom.c"),
                              {{"mode_name", modeLabel},
                               {"main_from_grammar", false},
                               {"demo_main", demo}});
}

std::string CodeGenC99::customMainCWithBody(const std::string &body,
                                         const std::string &modeLabel)
{
    // 外壳里的「演示 main」说明对用户自己的 main 不适用，由模板里的
    // `{% if main_from_grammar %}` 换成 %main 的说法
    return tmpl::renderValues(Utils::loadTemplate("c99_main_custom.c"),
                              {{"mode_name", modeLabel},
                               {"main_from_grammar", true},
                               {"demo_main", "\n\n" + body + "\n"}});
}

bool CodeGenC99::customParserC(
    const Grammar &grammar, const std::vector<std::string> &tokens,
    const std::map<std::string, std::vector<std::string>> &tokenDefs,
    const std::string &mode, bool tree, const PriorityConfig *priority,
    const LexSpec &lexSpec, const std::string *spaceChars, bool teach,
    const std::string &userCode, const std::string &unionBody,
    const std::string &valueType,
    const std::map<std::string, std::string> *symbolMember,
    const std::map<std::string, std::string> *destructors,
    const std::vector<std::string> *syncTokens,
    const AnalysisResult *precomputed, std::string &out, Error &error)
{
    // 若调用方已经算过分析表就直接复用，避免同一份文法反复分析
    AnalysisResult owned;
    if (precomputed == nullptr)
    {
        if (!Analysis::analyze(grammar, tokens, mode, priority, teach, owned,
                               error, 1))
        {
            return false;
        }
    }
    const AnalysisResult &analysis =
        (precomputed == nullptr) ? owned : *precomputed;
    const std::string modeLabel = modeLabelText(mode);
    const std::string modeName = modeNameText(mode);
    const std::string lexer =
        generatorLexerC(!lexSpec.regex.empty(), lexSpec.identifier.has_value(),
                        !lexSpec.commits.empty(), tokenDefs, lexSpec.regex,
                        spaceChars, false, symbolMember);
    // %destructor：统一符号号 -> %union 成员 -> 清理片段的分派函数（LR / LL
    // 共用）。没写 %destructor、或没有符号绑到那些成员时是空串。
    const std::string discardDispatch =
        generatorDestructorDispatchC(analysis, symbolMember, destructors, error);
    if (!error.empty())
    {
        return false;
    }
    const std::string customRuntime =
        generatorCustomRuntimeC(mode, analysis, modeLabel, modeName, tree,
                                symbolMember, discardDispatch, syncTokens,
                                error);
    if (!error.empty())
    {
        return false;
    }
    // 每个片段都在自己的生成函数里渲染完（`terminal_count` 这类计数以前是
    // 排在 parser.c 上再被外层统一替换一遍，现在直接传下去，不用二次替换）。
    out = tmpl::renderValues(
        Utils::loadTemplate("c99_parser_c_custom.c"),
        {{"mode_name", modeLabel},
         {"user_include", customUserInclude(userCode, unionBody, valueType)},
         {"table_decls", tableDeclsC(mode, analysis)},
         {"custom_item_types",
          generatorCustomItemTypesC(mode, modeLabel, tree,
                                    !discardDispatch.empty(),
                                    analysis.terminals.size())},
         {"custom_tree_helpers",
          generatorCustomTreeHelpersC(mode, analysis, tree)},
         {"custom_tree_entry",
          generatorCustomTreeEntryC(mode, analysis, tree)},
         {"lex_rules", generatorLexRules(tokenDefs, lexSpec.tokenLexOrder)},
         {"lexer", lexer},
         {"custom_runtime", customRuntime}});
    return true;
}


} // namespace ParseEngine

// 字段绑定只能写在全局作用域（`TEMPLATE_MODEL` 展开的是显式特化）
TEMPLATE_MODEL(ParseEngine::KeywordRow,
               tmpl::field("text", &ParseEngine::KeywordRow::text),
               tmpl::field("token", &ParseEngine::KeywordRow::token));

TEMPLATE_MODEL(ParseEngine::LexRuleRow,
               tmpl::field("text", &ParseEngine::LexRuleRow::text),
               tmpl::field("length", &ParseEngine::LexRuleRow::length),
               tmpl::field("token", &ParseEngine::LexRuleRow::token),
               tmpl::field("blank_first", &ParseEngine::LexRuleRow::blank_first),
               tmpl::field("blank_wildcard",
                           &ParseEngine::LexRuleRow::blank_wildcard),
               tmpl::field("decl_order", &ParseEngine::LexRuleRow::decl_order));

TEMPLATE_MODEL(ParseEngine::LlExpandItem,
               tmpl::field("kind", &ParseEngine::LlExpandItem::kind),
               tmpl::field("symbol_id", &ParseEngine::LlExpandItem::symbol_id),
               tmpl::field("slot", &ParseEngine::LlExpandItem::slot));

TEMPLATE_MODEL(ParseEngine::LlExpandCase,
               tmpl::field("id", &ParseEngine::LlExpandCase::id),
               tmpl::field("left_loop", &ParseEngine::LlExpandCase::left_loop),
               tmpl::field("loop_symbol_id",
                           &ParseEngine::LlExpandCase::loop_symbol_id),
               tmpl::field("keep_action",
                           &ParseEngine::LlExpandCase::keep_action),
               tmpl::field("empty_tail", &ParseEngine::LlExpandCase::empty_tail),
               tmpl::field("plain_action",
                           &ParseEngine::LlExpandCase::plain_action),
               tmpl::field("symbols", &ParseEngine::LlExpandCase::symbols));

TEMPLATE_MODEL(ParseEngine::LrTableRow,
               tmpl::field("cells", &ParseEngine::LrTableRow::cells));

TEMPLATE_MODEL(ParseEngine::LexRegexCase,
               tmpl::field("token", &ParseEngine::LexRegexCase::token),
               tmpl::field("member", &ParseEngine::LexRegexCase::member));

TEMPLATE_MODEL(ParseEngine::LexDfaCellRow,
               tmpl::field("cells", &ParseEngine::LexDfaCellRow::cells));

TEMPLATE_MODEL(ParseEngine::LexDfaInfo,
               tmpl::field("label", &ParseEngine::LexDfaInfo::label),
               tmpl::field("pattern", &ParseEngine::LexDfaInfo::pattern),
               tmpl::field("trans_name", &ParseEngine::LexDfaInfo::trans_name),
               tmpl::field("accept_name", &ParseEngine::LexDfaInfo::accept_name),
               tmpl::field("state_count", &ParseEngine::LexDfaInfo::state_count),
               tmpl::field("class_count", &ParseEngine::LexDfaInfo::class_count),
               tmpl::field("transitions", &ParseEngine::LexDfaInfo::transitions),
               tmpl::field("accept", &ParseEngine::LexDfaInfo::accept));

TEMPLATE_MODEL(ParseEngine::DestructorCase,
               tmpl::field("member", &ParseEngine::DestructorCase::member),
               tmpl::field("names", &ParseEngine::DestructorCase::names),
               tmpl::field("labels", &ParseEngine::DestructorCase::labels),
               tmpl::field("body", &ParseEngine::DestructorCase::body));

TEMPLATE_MODEL(ParseEngine::LexDfaDescriptor,
               tmpl::field("trans_name",
                           &ParseEngine::LexDfaDescriptor::trans_name),
               tmpl::field("accept_name",
                           &ParseEngine::LexDfaDescriptor::accept_name),
               tmpl::field("state_count",
                           &ParseEngine::LexDfaDescriptor::state_count),
               tmpl::field("start_state",
                           &ParseEngine::LexDfaDescriptor::start_state),
               tmpl::field("trap_state",
                           &ParseEngine::LexDfaDescriptor::trap_state));
