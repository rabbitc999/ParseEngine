/**
 * @file code_gen_narrow.hpp
 * @brief Java / TypeScript 的 DD、Earley 与 CYK 后端共用数据模型。
 */

#ifndef PARSE_ENGINE_CODE_GEN_NARROW_HPP
#define PARSE_ENGINE_CODE_GEN_NARROW_HPP

#include <string>

#include "ParseEngine/analysis.hpp"
#include "ParseEngine/error.hpp"
#include "ParseEngine/project.hpp"

namespace ParseEngine
{

struct NarrowBackendData
{
    std::string mode;
    std::string modeLabel;
    std::string sourceName;
    std::string exampleShell;
    bool tree = false;
    bool chart = false;
    bool cyk = false;
    long long terminalCount = 0;
    long long startNonterm = 0;
    long long endToken = 0;
    long long numberToken = -1;
    long long stringToken = -1;
    long long variableToken = -1;
    long long maxDepth = 64;
    std::string tokenNames;
    std::string nontermNames;
    std::string productionLhs;
    std::string productionRhsStart;
    std::string productionRhsLength;
    std::string productionRhsSymbols;
    std::string ddProductionStart;
    std::string ddProductionCount;
    long long cykNontermCount = 0;
    bool cykAcceptsEmpty = false;
    std::string cykTerminalLhs;
    std::string cykTerminalToken;
    std::string cykBinaryLhs;
    std::string cykBinaryLeft;
    std::string cykBinaryRight;
    std::string literalTexts;
    std::string literalTypes;
    std::string literalOrders;
    std::string regexTexts;
    std::string regexTypes;
    std::string regexOrders;
    std::string identifierPattern;
    std::string skipChars;
};

bool buildNarrowBackendData(const ProjectRequest &request,
                            const AnalysisResult &analysis,
                            const std::string &language,
                            NarrowBackendData &out, Error &error);

} // namespace ParseEngine

#endif
