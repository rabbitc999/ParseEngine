/** @file code_gen_java.cpp @brief Java DD / Earley / CYK 代码生成后端。 */

#include "ParseEngine/code_gen_java.hpp"

#include "ParseEngine/code_gen_narrow.hpp"
#include "ParseEngine/utils.hpp"
#include "template/engine.hpp"

namespace ParseEngine
{

bool CodeGenJava::generateProject(const ProjectRequest &request,
                                  const AnalysisResult &analysis,
                                  std::map<std::string, std::string> &out,
                                  Error &error)
{
    NarrowBackendData data;
    if (!buildNarrowBackendData(request, analysis, "java", data, error))
        return false;
    out.insert_or_assign(
        "Parser.java",
        tmpl::renderValues(
            Utils::loadTemplate("java_parser.java"),
            {{"mode", data.mode}, {"mode_label", data.modeLabel},
             {"source_name", data.sourceName},
             {"tree", data.tree}, {"chart", data.chart}, {"cyk", data.cyk},
             {"terminal_count", data.terminalCount},
             {"start_nonterm", data.startNonterm},
             {"end_token", data.endToken}, {"number_token", data.numberToken},
             {"string_token", data.stringToken},
             {"variable_token", data.variableToken},
             {"max_depth", data.maxDepth}, {"token_names", data.tokenNames},
             {"nonterm_names", data.nontermNames},
             {"production_lhs", data.productionLhs},
             {"production_rhs_start", data.productionRhsStart},
             {"production_rhs_length", data.productionRhsLength},
             {"production_rhs_symbols", data.productionRhsSymbols},
             {"dd_production_start", data.ddProductionStart},
             {"dd_production_count", data.ddProductionCount},
             {"cyk_nonterm_count", data.cykNontermCount},
             {"cyk_accepts_empty", data.cykAcceptsEmpty},
             {"cyk_terminal_lhs", data.cykTerminalLhs},
             {"cyk_terminal_token", data.cykTerminalToken},
             {"cyk_binary_lhs", data.cykBinaryLhs},
             {"cyk_binary_left", data.cykBinaryLeft},
             {"cyk_binary_right", data.cykBinaryRight},
             {"literal_texts", data.literalTexts},
             {"literal_types", data.literalTypes},
             {"literal_orders", data.literalOrders},
             {"regex_texts", data.regexTexts},
             {"regex_types", data.regexTypes},
             {"regex_orders", data.regexOrders},
             {"identifier_pattern", data.identifierPattern},
             {"skip_chars", data.skipChars}}));
    out.insert_or_assign(
        "RESULT.md",
        tmpl::renderValues(Utils::loadTemplate("java_result.md"),
                           {{"source_name", data.sourceName},
                            {"mode_label", data.modeLabel},
                            {"example_shell", data.exampleShell},
                            {"tree", data.tree}}));
    return true;
}

} // namespace ParseEngine
