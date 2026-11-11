/** @file code_gen_chali.cpp @brief ChaLi LL / LR / GLR 代码生成。 */
#include "ParseEngine/code_gen_chali.hpp"
#include "ParseEngine/utils.hpp"
#include "template/engine.hpp"
#include <algorithm>
#include <cctype>
#include <regex>
#include <set>
#include <sstream>

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

/** @brief 将 LR 动作编码为移进、归约或接受命令。 */
int command(const LrAction &action) {
    if (action.kind == LrAction::Kind::Accept) return 2147483647;
    return action.kind == LrAction::Kind::Shift ? action.value + 1 : -action.value - 1;
}

/** @brief 检测可在不消费终结符时回到自身的归约环。 */
bool hasZeroWidthCycle(const AnalysisResult &analysis) {
    std::set<std::string> nullable;
    const auto isNullable = [&](const std::string &symbol) {
        return nullable.count(symbol) != 0;
    };
    std::size_t previous;
    do {
        previous = nullable.size();
        for (const auto &production : analysis.productions) {
            if (std::all_of(production.rhs.begin(), production.rhs.end(), isNullable))
                nullable.insert(production.lhs);
        }
    } while (nullable.size() != previous);

    std::vector<std::vector<int>> edges(analysis.nonterms.size());
    std::vector<int> indegree(analysis.nonterms.size());
    for (const auto &production : analysis.productions) {
        const int lhs = indexOf(analysis.nonterms, production.lhs);
        if (lhs < 0) continue;
        const auto &rhs = production.rhs;
        for (auto it = rhs.begin(); it != rhs.end(); ++it) {
            const int target = indexOf(analysis.nonterms, *it);
            if (target >= 0 && std::all_of(rhs.begin(), it, isNullable) &&
                std::all_of(it + 1, rhs.end(), isNullable)) {
                edges[lhs].push_back(target);
                ++indegree[target];
            }
        }
    }
    std::vector<int> ready;
    for (std::size_t i = 0; i < indegree.size(); ++i)
        if (indegree[i] == 0) ready.push_back(static_cast<int>(i));
    for (std::size_t i = 0; i < ready.size(); ++i)
        for (int next : edges[ready[i]])
            if (--indegree[next] == 0) ready.push_back(next);
    return ready.size() != indegree.size();
}
}

bool supportsChaLiMode(const std::string &mode) {
    return mode == "ll1" || mode == "llk" || mode == "slr" || mode == "lalr" ||
           mode == "lr1" || mode == "ielr" || mode == "lrk" || mode == "glr";
}

bool generateChaLiProject(const ProjectRequest &request, const AnalysisResult &a,
                          std::map<std::string, std::string> &out, Error &error) {
    const auto &mode = request.mode;
    if (!supportsChaLiMode(mode))
        return fail(error, "--lang chali 只支持 ll1 / llk / slr / lalr / lr1 / ielr / lrk / glr");
    const bool llMode = mode == "ll1" || mode == "llk";
    const bool glrMode = mode == "glr";
    if (glrMode && hasZeroWidthCycle(a))
        return fail(error, "ChaLi GLR 不支持不消耗 Token 的归约环");
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
            return fail(error, "ChaLi LL 暂不支持自动左递归改写，请提供直接满足所选模式的文法");
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
    const bool wide = a.lookaheadK > 1;
    const auto &columns = a.lookaheadColumns;
    const int width = wide ? static_cast<int>(columns.size()) : terminals;
    std::vector<int> keys, reductionsByWindow(wide && !llMode ? states * width : 0, 0);
    std::vector<int> ll(llMode ? nonterms * width : 0, -1);
    std::vector<int> actions(!llMode && !glrMode ? states * terminals : 0, 0);
    std::vector<int> gotos(states * nonterms, -1), glrOffsets, glrCommands;
    if (wide) {
        for (const auto &column : columns) {
            for (int i = 0; i < a.lookaheadK; ++i)
                keys.push_back(indexOf(a.terminals, i < static_cast<int>(column.size())
                    ? column[i] : "END"));
        }
    }
    for (const auto &entry : a.table) {
        const int row = indexOf(a.nonterms, entry.first.first);
        const int col = wide ? a.lookaheadColumnIndex.at(entry.first.second)
                             : indexOf(a.terminals, entry.first.second);
        ll[row * width + col] = entry.second;
    }
    if (glrMode) {
        for (int state = 0; state < states; ++state) {
            for (const auto &terminal : a.terminals) {
                glrOffsets.push_back(static_cast<int>(glrCommands.size()));
                const auto key = std::make_pair(state, terminal);
                const auto alternatives = a.glrAlternatives.find(key);
                if (alternatives != a.glrAlternatives.end()) {
                    for (const auto &candidate : alternatives->second)
                        glrCommands.push_back(command(candidate));
                } else {
                    const auto single = a.action.find(key);
                    if (single != a.action.end()) glrCommands.push_back(command(single->second));
                }
            }
        }
        glrOffsets.push_back(static_cast<int>(glrCommands.size()));
    } else {
        for (const auto &entry : a.action) {
            const int state = entry.first.first;
            const int token = indexOf(a.terminals, entry.first.second);
            if (token >= 0) actions[state * terminals + token] = command(entry.second);
            else if (wide) reductionsByWindow[state * width +
                a.lookaheadColumnIndex.at(entry.first.second)] = command(entry.second);
        }
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
        if (!entry.isSkip && entry.type != "comment")
            return fail(error, "ChaLi 后端只支持跳过规则和注释保留");
        rule(entry.pattern, entry.isSkip ? -2 : -3, entry.order);
    }
    const auto isPlaceholder = [&](const std::string &name) {
        if (request.tokenDefs == nullptr) return false;
        const auto rule = request.tokenDefs->find(name);
        return rule != request.tokenDefs->end() && rule->second.empty();
    };
    const int number = indexOf(a.terminals, "NUMBER");
    if (number >= 0 && !lex.regex.count("NUMBER") && !isPlaceholder("NUMBER")) rule("[0-9]+(?:\\.[0-9]+)?(?:[eE][+-]?[0-9]+)?", number, 100001);
    const int variable = indexOf(a.terminals, "VARIABLE");
    if (variable >= 0 && !lex.regex.count("VARIABLE") && !isPlaceholder("VARIABLE")) rule(lex.identifier.value_or("[A-Za-z_][A-Za-z0-9_]*"), variable, 100002);
    const int string = indexOf(a.terminals, "STRING");
    if (string >= 0 && !lex.regex.count("STRING") && !isPlaceholder("STRING")) rule("\"(?:[^\"\\\\]|\\\\.)*\"", string, 100003);
    if (!validPatterns) return false;
    std::string names;
    for (const auto &name : a.terminals) { if (!names.empty()) names += ", "; names += quote(name); }
    std::string nontermNames;
    for (const auto &name : a.nonterms) { if (!nontermNames.empty()) nontermNames += ", "; nontermNames += quote(name); }
    std::string main;
    if (request.mainBlocks) for (const auto &block : *request.mainBlocks) main += block + "\n";
    const std::string valueType = request.valueType ? *request.valueType : "float64";
    const std::string glrRuntime = glrMode ? tmpl::renderValues(
        Utils::loadTemplate("chali_glr.chl"),
        {{"value_type", valueType}, {"terminal_count", terminals},
         {"nonterm_count", nonterms}, {"end", indexOf(a.terminals, "END")},
         {"max_depth", request.maxStackDepth.value_or(k_maxStackDepthDefault)}}) : "";
    out["parser.chl"] = tmpl::renderValues(Utils::loadTemplate("chali_parser.chl"), {
        {"glr_runtime", glrRuntime},
        {"post_lex", lex.postLex},
        {"post_tokens", lex.postTokens},
        {"ll", llMode}, {"glr", glrMode}, {"tree", request.tree},
        {"wide", wide}, {"k", a.lookaheadK}, {"width", width},
        {"keys", numbers(keys)}, {"window_actions", numbers(reductionsByWindow)},
        {"glr_offsets", numbers(glrOffsets)}, {"glr_commands", numbers(glrCommands)}, {"value_type", valueType},
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
        "无错误恢复或增量 API。\n";
    if (wide) out["RESULT.md"] += "\n展望符窗口：" + std::to_string(a.lookaheadK) + " 个 Token。\n";
    if (glrMode) out["RESULT.md"] +=
        "\nGLR 结果通过 alternatives 返回；truncated 表示有路径超过栈深度限制。\n";
    if (request.fullReport) out["RESULT.md"] += "\n" + request.reportText;
    return true;
}
}
