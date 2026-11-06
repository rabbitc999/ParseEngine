/** @file code_gen_chali.cpp @brief ChaLi LL(1) 与 LR(1) 系列代码生成。 */
#include "ParseEngine/code_gen_chali.hpp"
#include "ParseEngine/utils.hpp"
#include "template/engine.hpp"
#include <algorithm>
#include <cctype>
#include <sstream>
#include <regex>

namespace ParseEngine {
namespace {
/** @brief 编码 ChaLi 字符串。@param text 原文。@return 字符串字面量。 */
std::string quote(const std::string &text) {
    std::string out = "\"";
    for (char ch : text) {
        switch (ch) {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        case '\v': out += "\\v"; break;
        case '\f': out += "\\f"; break;
        default: out += ch;
        }
    }
    return out + "\"";
}
/** @brief 编码整数列表。@param values 整数序列。@return 列表正文。 */
std::string numbers(const std::vector<int> &values) {
    std::ostringstream out;
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i) out << ", ";
        out << values[i];
    }
    return out.str();
}
/** @brief 查找符号编号。@param names 符号表。@param name 名称。@return 编号或 -1。 */
int indexOf(const std::vector<std::string> &names, const std::string &name) {
    auto it = std::find(names.begin(), names.end(), name);
    return it == names.end() ? -1 : static_cast<int>(it - names.begin());
}
/** @brief 展开动作中的值引用，保留字符串与注释。
 * @param source 动作原文。@param count 右部长度。@param out 译文。
 * @param error 失败原因。@return 引用均合法时为真。
 */
bool action(const std::string &source, int count, std::string &out, Error &error) {
    for (std::size_t i = 0; i < source.size();) {
        const char ch = source[i];
        if (ch == '"' || ch == '\'') {
            const char delimiter = ch;
            out += source[i++];
            while (i < source.size()) {
                char c = source[i++]; out += c;
                if (c == '\\' && i < source.size()) out += source[i++];
                else if (c == delimiter) break;
            }
        } else if (ch == '/' && i + 1 < source.size() &&
                   (source[i + 1] == '/' || source[i + 1] == '*')) {
            const bool line = source[i + 1] == '/';
            auto end = source.find(line ? "\n" : "*/", i + 2);
            end = end == std::string::npos ? source.size() : end + (line ? 1 : 2);
            out += source.substr(i, end - i); i = end;
        } else if (ch == '$') {
            ++i;
            if (i < source.size() && source[i] == '$') { out += "result"; ++i; }
            else {
                const auto begin = i;
                while (i < source.size() && std::isdigit(static_cast<unsigned char>(source[i]))) ++i;
                if (begin == i || i - begin > 8)
                    return fail(error, "ChaLi 动作只支持 $$ 与 $n 值引用");
                const int n = std::stoi(source.substr(begin, i - begin));
                if (n < 1 || n > count) return fail(error, "ChaLi 动作的 $n 超出产生式右部范围");
                out += "values[" + std::to_string(n - 1) + "]";
            }
        } else { out += ch; ++i; }
    }
    return true;
}
}

bool generateChaLiProject(const ProjectRequest &request, const AnalysisResult &a,
                          std::map<std::string, std::string> &out, Error &error) {
    const auto &mode = request.mode;
    if (mode != "ll1" && mode != "slr" && mode != "lalr" && mode != "lr1" && mode != "ielr")
        return fail(error, "--lang chali 只支持 ll1 / slr / lalr / lr1 / ielr");
    if (request.unionBody || request.symbolValueMember ||
        (request.destructors && !request.destructors->empty()) || request.usesError ||
        (request.syncTokens && !request.syncTokens->empty()) ||
        (request.classBody && !request.classBody->empty()) || !request.packageName.empty() ||
        (request.imports && !request.imports->empty()) ||
        (request.copies && !request.copies->empty()))
        return fail(error, "ChaLi 后端暂不支持 %union、成员标注、%destructor、error、%sync、%class_body、%package、%import 或 %copy");
    const int terminals = static_cast<int>(a.terminals.size());
    const int nonterms = static_cast<int>(a.nonterms.size());
    const int states = a.stateCount();
    std::vector<int> lhs, starts, lengths, rhs;
    std::string reductions;
    for (std::size_t p = 0; p < a.productions.size(); ++p) {
        const auto &production = a.productions[p];
        if (!production.transform.empty())
            return fail(error, "ChaLi LL(1) 暂不支持自动左递归改写，请提供 LL(1) 文法");
        lhs.push_back(indexOf(a.nonterms, production.lhs));
        starts.push_back(static_cast<int>(rhs.size()));
        lengths.push_back(static_cast<int>(production.rhs.size()));
        for (const auto &symbol : production.rhs) {
            const int token = indexOf(a.terminals, symbol);
            rhs.push_back(token >= 0 ? token : terminals + indexOf(a.nonterms, symbol));
        }
        std::string body;
        if (!action(production.action, lengths.back(), body, error)) return false;
        reductions += "    if (production == " + std::to_string(p) + ") {\n" + body + "\n    }\n";
    }
    std::vector<int> ll(nonterms * terminals, -1), actions(states * terminals, 0), gotos(states * nonterms, -1);
    for (const auto &entry : a.table)
        ll[indexOf(a.nonterms, entry.first.first) * terminals + indexOf(a.terminals, entry.first.second)] = entry.second;
    for (const auto &entry : a.action) {
        const auto &v = entry.second;
        actions[entry.first.first * terminals + indexOf(a.terminals, entry.first.second)] =
            v.kind == LrAction::Kind::Accept ? 2147483647 :
            (v.kind == LrAction::Kind::Shift ? v.value + 1 : -v.value - 1);
    }
    for (const auto &entry : a.gotoTable) gotos[entry.first.first * nonterms + entry.first.second] = entry.second;
    std::string patterns, types, orders;
    bool validPatterns = true;
    auto rule = [&](const std::string &pattern, int type, int order) {
        try {
            const std::regex compiled("^(?:" + pattern + ")");
            if (std::regex_search(std::string(), compiled)) {
                validPatterns = false;
                fail(error, "ChaLi 词法规则不能匹配空串：" + pattern);
            }
        } catch (const std::regex_error &) {
            validPatterns = false;
            fail(error, "ChaLi ECMAScript 正则无效：" + pattern);
        }
        if (!patterns.empty()) { patterns += ", "; types += ", "; orders += ", "; }
        patterns += quote("^(?:" + pattern + ")");
        types += std::to_string(type); orders += std::to_string(order);
    };
    const LexSpec empty;
    const LexSpec &lex = request.lexSpec ? *request.lexSpec : empty;
    auto orderOf = [&](const std::string &name) {
        auto it = lex.tokenLexOrder.find(name);
        return it == lex.tokenLexOrder.end() ? 100000 : it->second;
    };
    if (request.tokenDefs) for (const auto &entry : *request.tokenDefs) {
        for (const auto &text : entry.second) {
            std::string pattern;
            for (char c : text) {
                if (c == k_blankWildcardChar) { pattern += "[ \\t\\r\\n\\v\\f]"; continue; }
                if (std::string("\\.^$|()[]{}*+?").find(c) != std::string::npos) pattern += '\\';
                pattern += c;
            }
            rule(pattern, indexOf(a.terminals, entry.first), orderOf(entry.first));
        }
    }
    for (const auto &entry : lex.regex) rule(entry.second, indexOf(a.terminals, entry.first), orderOf(entry.first));
    for (const auto &entry : lex.commits) {
        if (!entry.isSkip) return fail(error, "ChaLi 后端暂不支持带值 %commit；%trivia 与 %commit skip 可用");
        rule(entry.pattern, -2, entry.order);
    }
    const int number = indexOf(a.terminals, "NUMBER");
    if (!lex.regex.count("NUMBER") && number >= 0) rule("[0-9]+(?:\\.[0-9]+)?(?:[eE][+-]?[0-9]+)?", number, 100001);
    const int variable = indexOf(a.terminals, "VARIABLE");
    if (variable >= 0 && !lex.regex.count("VARIABLE")) rule(lex.identifier.value_or("[A-Za-z_][A-Za-z0-9_]*"), variable, 100002);
    const int string = indexOf(a.terminals, "STRING");
    if (string >= 0 && !lex.regex.count("STRING")) rule("\"(?:[^\"\\\\]|\\\\.)*\"", string, 100003);
    if (!validPatterns) return false;
    std::string names;
    for (const auto &name : a.terminals) { if (!names.empty()) names += ", "; names += quote(name); }
    std::string nontermNames;
    for (const auto &name : a.nonterms) { if (!nontermNames.empty()) nontermNames += ", "; nontermNames += quote(name); }
    std::string main;
    if (request.mainBlocks) for (const auto &block : *request.mainBlocks) main += block + "\n";
    const std::string valueType = request.valueType ? *request.valueType : "float64";
    out["parser.chl"] = tmpl::renderValues(Utils::loadTemplate("chali_parser.chl"), {
        {"ll", mode == "ll1"}, {"tree", request.tree}, {"value_type", valueType},
        {"numeric", valueType == "float64"}, {"terminal_count", terminals}, {"nonterm_count", nonterms},
        {"start", indexOf(a.nonterms, a.grammar.start)}, {"end", indexOf(a.terminals, "END")},
        {"number", number}, {"max_depth", request.maxStackDepth.value_or(k_maxStackDepthDefault)},
        {"lhs", numbers(lhs)}, {"starts", numbers(starts)}, {"lengths", numbers(lengths)},
        {"rhs", numbers(rhs)}, {"ll_table", numbers(ll)}, {"actions", numbers(actions)},
        {"gotos", numbers(gotos)}, {"reductions", reductions}, {"patterns", patterns},
        {"types", types}, {"orders", orders}, {"names", names}, {"nonterm_names", nontermNames},
        {"space", quote(request.spaceChars ? *request.spaceChars : blankChars())},
        {"user_code", request.userCode}, {"epilogue", request.epilogue ? *request.epilogue : ""},
        {"main", main}, {"custom_main", !main.empty()}});
    out["RESULT.md"] = "# ChaLi 解析器\n\n模式：" + modeLabelText(mode) +
        "。自包含源码：`parser.chl`。\n\n运行：`chali run parser.chl`，默认入口从标准输入读取一行。\n"
        "构建：`chali build parser.chl --out parser`。\n\n"
        "API：`Parser::runText(String)` 返回 `ParserResult`（ok / value / tree / message / offset）；"
        "`Parser::runTokens(List<ParserToken>)` 接受以 END 结尾的 Token 序列。"
        "Token 含 kind / text / offset / value。offset 为字节偏移。\n\n"
        "默认语义值 float64；NUMBER 自动转换。自定义 %value_type 时由 runTokens 调用者提供 Token 值。"
        "支持 $$ / $n、%code、%main、%epilogue、%tree、%space、%trivia、原生 ECMAScript 正则。"
        "解析失败立即返回，无错误恢复或增量 API。\n";
    if (request.fullReport) out["RESULT.md"] += "\n" + request.reportText;
    return true;
}
}
