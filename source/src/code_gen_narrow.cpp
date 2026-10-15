/**
 * @file code_gen_narrow.cpp
 * @brief Java / TypeScript 的 DD、Earley 与 CYK 后端共用数据准备。
 */

#include "ParseEngine/code_gen_narrow.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <sstream>
#include <vector>

#include "ParseEngine/chart_grammar.hpp"

namespace ParseEngine
{
namespace
{

std::string quote(const std::string &text)
{
    std::string out = "\"";
    const char *hex = "0123456789ABCDEF";
    for (const unsigned char ch : text)
    {
        switch (ch)
        {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        default:
            if (ch < 0x20u)
            {
                out += "\\u00";
                out += hex[(ch >> 4u) & 0x0fu];
                out += hex[ch & 0x0fu];
            }
            else out += static_cast<char>(ch);
            break;
        }
    }
    return out + "\"";
}

std::string shellQuote(const std::string &text)
{
    std::string out = "'";
    for (const char ch : text)
    {
        out += (ch == '\'') ? "'\\''" : std::string(1, ch);
    }
    return out + "'";
}

template <typename T>
std::string numberList(const std::vector<T> &values)
{
    std::ostringstream out;
    for (std::size_t index = 0; index < values.size(); ++index)
    {
        if (index != 0u) out << ", ";
        out << values[index];
    }
    return out.str();
}

std::string stringList(const std::vector<std::string> &values)
{
    std::ostringstream out;
    for (std::size_t index = 0; index < values.size(); ++index)
    {
        if (index != 0u) out << ", ";
        out << quote(values[index]);
    }
    return out.str();
}

long long symbolIndex(const std::vector<std::string> &symbols,
                      const std::string &name)
{
    const auto found = std::find(symbols.begin(), symbols.end(), name);
    return found == symbols.end()
               ? -1
               : static_cast<long long>(found - symbols.begin());
}

bool hasText(const std::string &text)
{
    return std::any_of(text.begin(), text.end(), [](unsigned char ch)
                       { return !std::isspace(ch); });
}

struct LexRow
{
    std::string text;
    int type = 0;
    int order = 0;
};

bool rejectUnsupported(const ProjectRequest &request,
                       const AnalysisResult &analysis,
                       const std::string &language, Error &error)
{
    if ((request.mode != "dd") && (request.mode != "earley") &&
        (request.mode != "cyk"))
    {
        return fail(error, "--lang " + language +
                               " 当前只支持 --mode dd / earley / cyk，实际模式是 " +
                               request.mode);
    }
    for (const Production &production : analysis.productions)
    {
        if (hasText(production.action))
        {
            return fail(error, "--lang " + language +
                                   " 的 DD / Earley / CYK 后端暂不支持语义动作；"
                                   "请删除产生式动作或改用已有完整后端");
        }
    }
    if ((request.valueType != nullptr) || (request.unionBody != nullptr) ||
        (request.symbolValueMember != nullptr))
    {
        return fail(error, "--lang " + language +
                               " 暂不支持 %value_type / %union / %token <成员>");
    }
    if (!request.userCode.empty() || request.hasEpilogue ||
        ((request.mainBlocks != nullptr) && !request.mainBlocks->empty()) ||
        ((request.classBody != nullptr) && hasText(*request.classBody)) ||
        !request.packageName.empty() ||
        ((request.imports != nullptr) && !request.imports->empty()))
    {
        return fail(error, "--lang " + language +
                               " 的窄后端暂不支持用户代码、%main、%epilogue、"
                               "%package、%import 或 %class_body");
    }
    if ((request.destructors != nullptr) && !request.destructors->empty())
    {
        return fail(error, "--lang " + language +
                               " 不支持 %destructor（运行时负责对象回收）");
    }
    if ((request.syncTokens != nullptr) && !request.syncTokens->empty())
    {
        return fail(error, "--lang " + language +
                               " 的 DD / Earley / CYK 窄后端暂不支持 %sync");
    }
    if ((request.lexSpec != nullptr) && !request.lexSpec->commits.empty())
    {
        return fail(error, "--lang " + language +
                               " 的 DD / Earley / CYK 窄后端暂不支持 %commit");
    }
    if ((request.copies != nullptr) && !request.copies->empty())
    {
        return fail(error, "--lang " + language +
                               " 的 DD / Earley / CYK 窄后端暂不支持 %copy");
    }
    return true;
}

} // namespace

bool buildNarrowBackendData(const ProjectRequest &request,
                            const AnalysisResult &analysis,
                            const std::string &language,
                            NarrowBackendData &out, Error &error)
{
    if (!rejectUnsupported(request, analysis, language, error)) return false;

    out.mode = request.mode;
    out.modeLabel = request.mode == "dd"
                        ? "DD（递归下降）"
                        : (request.mode == "earley" ? "Earley" : "CYK");
    out.chart = request.mode != "dd";
    out.cyk = request.mode == "cyk";
    out.sourceName = request.sourceName.empty()
                         ? "<default_grammar.gram>"
                         : request.sourceName;
    out.exampleShell = shellQuote(
        request.exampleInput.value_or("<符合文法的语言>"));
    out.tree = request.tree;
    out.terminalCount = static_cast<long long>(analysis.terminals.size());
    out.startNonterm = symbolIndex(analysis.nonterms, analysis.grammar.start);
    out.endToken = symbolIndex(analysis.terminals, "END");
    out.numberToken = symbolIndex(analysis.terminals, "NUMBER");
    out.stringToken = symbolIndex(analysis.terminals, "STRING");
    out.variableToken = symbolIndex(analysis.terminals, "VARIABLE");
    out.maxDepth = request.maxStackDepth.value_or(64);
    out.tokenNames = stringList(analysis.terminals);
    out.nontermNames = stringList(analysis.nonterms);

    std::vector<int> lhs;
    std::vector<int> rhsStart;
    std::vector<int> rhsLength;
    std::vector<int> rhsSymbols;
    if ((request.mode == "earley") || (request.mode == "cyk"))
    {
        const ChartGrammar chart = buildChartGrammar(analysis);
        lhs = chart.lhs;
        rhsStart = chart.rhsStart;
        rhsLength = chart.rhsLength;
        rhsSymbols = chart.rhsSymbols;
        out.startNonterm = chart.startNonterm;
        if (request.mode == "cyk")
        {
            std::vector<int> terminalLhs, terminalToken;
            std::vector<int> binaryLhs, binaryLeft, binaryRight;
            for (const auto &rule : chart.terminalRules)
            {
                terminalLhs.push_back(rule[0]);
                terminalToken.push_back(rule[1]);
            }
            for (const auto &rule : chart.binaryRules)
            {
                binaryLhs.push_back(rule[0]);
                binaryLeft.push_back(rule[1]);
                binaryRight.push_back(rule[2]);
            }
            out.cykNontermCount = chart.nontermCount;
            out.cykAcceptsEmpty = chart.acceptsEmpty;
            out.cykTerminalLhs = numberList(terminalLhs);
            out.cykTerminalToken = numberList(terminalToken);
            out.cykBinaryLhs = numberList(binaryLhs);
            out.cykBinaryLeft = numberList(binaryLeft);
            out.cykBinaryRight = numberList(binaryRight);
        }
    }
    else
    {
        std::map<std::string, int> nontermIds;
        for (std::size_t index = 0; index < analysis.nonterms.size(); ++index)
            nontermIds[analysis.nonterms[index]] = static_cast<int>(index);
        for (std::size_t nonterm = 0; nonterm < analysis.nonterms.size();
             ++nonterm)
        {
            for (const Production &production : analysis.productions)
            {
                if (production.lhs != analysis.nonterms[nonterm]) continue;
                lhs.push_back(static_cast<int>(nonterm));
                rhsStart.push_back(static_cast<int>(rhsSymbols.size()));
                rhsLength.push_back(static_cast<int>(production.rhs.size()));
                for (const std::string &symbol : production.rhs)
                {
                    const long long terminal =
                        symbolIndex(analysis.terminals, symbol);
                    rhsSymbols.push_back(
                        terminal >= 0
                            ? static_cast<int>(terminal)
                            : static_cast<int>(analysis.terminals.size()) +
                                  nontermIds.at(symbol));
                }
            }
        }
    }
    out.productionLhs = numberList(lhs);
    out.productionRhsStart = numberList(rhsStart);
    out.productionRhsLength = numberList(rhsLength);
    out.productionRhsSymbols = numberList(rhsSymbols);

    std::vector<int> ddStart(analysis.nonterms.size(), 0);
    std::vector<int> ddCount(analysis.nonterms.size(), 0);
    for (std::size_t nonterm = 0; nonterm < analysis.nonterms.size(); ++nonterm)
    {
        bool first = true;
        for (std::size_t production = 0; production < lhs.size(); ++production)
        {
            if (lhs[production] != static_cast<int>(nonterm)) continue;
            if (first)
            {
                ddStart[nonterm] = static_cast<int>(production);
                first = false;
            }
            ++ddCount[nonterm];
        }
    }
    out.ddProductionStart = numberList(ddStart);
    out.ddProductionCount = numberList(ddCount);

    std::vector<LexRow> literals;
    if (request.tokenDefs != nullptr)
    {
        int fallbackOrder = 0;
        for (const auto &entry : *request.tokenDefs)
        {
            const int type = static_cast<int>(
                symbolIndex(analysis.terminals, entry.first));
            if (type < 0) continue;
            int declared = fallbackOrder;
            if (request.lexSpec != nullptr)
            {
                const auto found =
                    request.lexSpec->tokenLexOrder.find(entry.first);
                if (found != request.lexSpec->tokenLexOrder.end())
                    declared = found->second;
            }
            for (const std::string &literal : entry.second)
                literals.push_back({literal, type, declared});
            ++fallbackOrder;
        }
    }
    std::stable_sort(literals.begin(), literals.end(),
                     [](const LexRow &left, const LexRow &right)
                     {
                         if (left.text.size() != right.text.size())
                             return left.text.size() > right.text.size();
                         return left.order < right.order;
                     });
    std::vector<std::string> literalTexts;
    std::vector<int> literalTypes, literalOrders;
    for (const LexRow &row : literals)
    {
        literalTexts.push_back(row.text);
        literalTypes.push_back(row.type);
        literalOrders.push_back(row.order);
    }
    out.literalTexts = stringList(literalTexts);
    out.literalTypes = numberList(literalTypes);
    out.literalOrders = numberList(literalOrders);

    std::vector<LexRow> regexes;
    if (request.lexSpec != nullptr)
    {
        int fallbackOrder = 100000;
        for (const auto &entry : request.lexSpec->regex)
        {
            const int type = static_cast<int>(
                symbolIndex(analysis.terminals, entry.first));
            if (type < 0) continue;
            const auto found = request.lexSpec->tokenLexOrder.find(entry.first);
            regexes.push_back({entry.second, type,
                               found == request.lexSpec->tokenLexOrder.end()
                                   ? fallbackOrder++
                                   : found->second});
        }
    }
    std::vector<std::string> regexTexts;
    std::vector<int> regexTypes, regexOrders;
    for (const LexRow &row : regexes)
    {
        regexTexts.push_back(row.text);
        regexTypes.push_back(row.type);
        regexOrders.push_back(row.order);
    }
    out.regexTexts = stringList(regexTexts);
    out.regexTypes = numberList(regexTypes);
    out.regexOrders = numberList(regexOrders);
    out.identifierPattern = quote(
        (request.lexSpec != nullptr && request.lexSpec->identifier.has_value())
            ? *request.lexSpec->identifier
            : "[A-Za-z_][A-Za-z0-9_]*");
    out.skipChars = quote(request.spaceChars == nullptr
                              ? std::string(" \t\n\r\v\f")
                              : *request.spaceChars);
    return true;

}

} // namespace ParseEngine
