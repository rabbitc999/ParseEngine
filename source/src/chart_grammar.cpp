#include "ParseEngine/chart_grammar.hpp"

#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <tuple>

namespace ParseEngine
{
namespace
{
struct Rule
{
    int lhs;
    std::vector<int> rhs;

    bool operator<(const Rule &other) const
    {
        return std::tie(lhs, rhs) < std::tie(other.lhs, other.rhs);
    }
};
} // namespace

ChartGrammar buildChartGrammar(const AnalysisResult &analysis)
{
    ChartGrammar out;
    out.terminalCount = static_cast<int>(analysis.terminals.size());
    out.nontermCount = static_cast<int>(analysis.nonterms.size());
    const int terminals = out.terminalCount;
    const auto start = std::find(analysis.nonterms.begin(),
                                 analysis.nonterms.end(), k_glrStartSymbol);
    out.startNonterm = static_cast<int>(start - analysis.nonterms.begin());
    std::map<std::string, int> tokenIds;
    std::map<std::string, int> nontermIds;
    for (int i = 0; i < terminals; ++i)
    {
        tokenIds[analysis.terminals[static_cast<std::size_t>(i)]] = i;
    }
    for (int i = 0; i < out.nontermCount; ++i)
    {
        nontermIds[analysis.nonterms[static_cast<std::size_t>(i)]] = i;
    }
    std::vector<Rule> original;
    for (const Production &production : analysis.productions)
    {
        Rule rule{nontermIds.at(production.lhs), {}};
        out.lhs.push_back(rule.lhs);
        out.rhsStart.push_back(static_cast<int>(out.rhsSymbols.size()));
        out.rhsLength.push_back(static_cast<int>(production.rhs.size()));
        for (const std::string &symbol : production.rhs)
        {
            const auto token = tokenIds.find(symbol);
            const int id = (token != tokenIds.end())
                               ? token->second
                               : terminals + nontermIds.at(symbol);
            rule.rhs.push_back(id);
            out.rhsSymbols.push_back(id);
        }
        original.push_back(std::move(rule));
    }

    if (analysis.mode == "earley")
    {
        return out;
    }

    std::set<int> nullable;
    bool changed = true;
    while (changed)
    {
        changed = false;
        for (const Rule &rule : original)
        {
            if (std::all_of(rule.rhs.begin(), rule.rhs.end(),
                            [&](int symbol)
                            { return symbol >= terminals &&
                                     nullable.count(symbol - terminals) != 0; }))
            {
                changed |= nullable.insert(rule.lhs).second;
            }
        }
    }
    out.acceptsEmpty = nullable.count(out.startNonterm) != 0;

    // Drop nullable occurrences, then replace terminals in long rules and
    // binarize. Synthetic symbols have no semantic action: GLR later rebuilds
    // the original derivation and evaluates its actions.
    std::set<Rule> expanded;
    for (const Rule &rule : original)
    {
        std::vector<int> rhs;
        std::function<void(std::size_t)> visit = [&](std::size_t index)
        {
            if (index == rule.rhs.size())
            {
                if (!rhs.empty())
                {
                    expanded.insert({rule.lhs, rhs});
                }
                return;
            }
            const int symbol = rule.rhs[index];
            rhs.push_back(symbol);
            visit(index + 1);
            rhs.pop_back();
            if (symbol >= terminals &&
                nullable.count(symbol - terminals) != 0)
            {
                visit(index + 1);
            }
        };
        visit(0);
    }

    std::map<int, int> preterminal;
    std::set<Rule> cnf;
    for (const Rule &rule : expanded)
    {
        std::vector<int> rhs = rule.rhs;
        if (rhs.size() >= 2)
        {
            for (int &symbol : rhs)
            {
                if (symbol < terminals)
                {
                    auto found = preterminal.find(symbol);
                    if (found == preterminal.end())
                    {
                        const int alias = out.nontermCount++;
                        found = preterminal.emplace(symbol, alias).first;
                        cnf.insert({alias, {symbol}});
                    }
                    symbol = terminals + found->second;
                }
            }
        }
        int lhs = rule.lhs;
        for (std::size_t i = 0; i + 2 < rhs.size(); ++i)
        {
            const int tail = out.nontermCount++;
            cnf.insert({lhs, {rhs[i], terminals + tail}});
            lhs = tail;
        }
        if (rhs.size() >= 3)
        {
            cnf.insert({lhs, {rhs[rhs.size() - 2], rhs.back()}});
        }
        else
        {
            cnf.insert({lhs, rhs});
        }
    }

    // Eliminate unit productions by transitive closure. The resulting rules
    // are only A -> token or A -> B C (plus the nullable start marker above).
    for (int lhs = 0; lhs < out.nontermCount; ++lhs)
    {
        std::set<int> closure{lhs};
        bool grew = true;
        while (grew)
        {
            grew = false;
            for (const Rule &rule : cnf)
            {
                if (closure.count(rule.lhs) != 0 && rule.rhs.size() == 1 &&
                    rule.rhs[0] >= terminals)
                {
                    grew |= closure.insert(rule.rhs[0] - terminals).second;
                }
            }
        }
        for (const Rule &rule : cnf)
        {
            if (closure.count(rule.lhs) == 0 ||
                (rule.rhs.size() == 1 && rule.rhs[0] >= terminals))
            {
                continue;
            }
            if (rule.rhs.size() == 1)
            {
                out.terminalRules.push_back({lhs, rule.rhs[0]});
            }
            else if (rule.rhs.size() == 2)
            {
                out.binaryRules.push_back(
                    {lhs, rule.rhs[0] - terminals,
                     rule.rhs[1] - terminals});
            }
        }
    }
    return out;
}

} // namespace ParseEngine
