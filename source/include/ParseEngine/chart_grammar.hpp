#pragma once

#include <array>
#include <vector>

#include "ParseEngine/analysis.hpp"

namespace ParseEngine
{

struct ChartGrammar
{
    int terminalCount = 0;
    int nontermCount = 0;
    int startNonterm = 0;
    std::vector<int> lhs;
    std::vector<int> rhsStart;
    std::vector<int> rhsLength;
    std::vector<int> rhsSymbols;
    std::vector<std::array<int, 2>> terminalRules;
    std::vector<std::array<int, 3>> binaryRules;
    bool acceptsEmpty = false;
};

// Earley consumes the original rules; CYK consumes their CNF equivalent.
ChartGrammar buildChartGrammar(const AnalysisResult &analysis);

} // namespace ParseEngine
