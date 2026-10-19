/** @file regex_compatibility.cpp
 * @brief 实际引擎选择、跨目标语义及报告转义回归。
 */
#include "ParseEngine/regex_compatibility.hpp"
#include <cassert>
using namespace ParseEngine;

/** @brief 验证规则分类与整体回退。@return 全部断言通过时返回零。 */
int main()
{
    LexSpec lex;
    lex.regex["A"] = "[a-z]+";
    auto report = RegexCompatibility::analyze(lex, "cpp17", "dfa");
    assert(report.rules[0].engine == "cpp-dfa");
    assert(!report.configurableBudgets);
    Error error;
    assert(RegexCompatibility::validate(report, true, error));
    lex.regex["B"] = "a(?=b)";
    report = RegexCompatibility::analyze(lex, "cpp17", "dfa");
    assert(report.rules[0].engine == "std::regex");
    assert(report.rules[1].engine == "std::regex");
    error = {};
    assert(!RegexCompatibility::validate(report, true, error));
    report = RegexCompatibility::analyze(lex, "c");
    assert(report.configurableBudgets);
    for (const auto &rule : report.rules) assert(rule.engine == "ordered-vm");
    for (const auto &lang : {"rust", "go"}) {
        report = RegexCompatibility::analyze(lex, lang);
        assert(report.rules[1].compatibility == RegexCompatibilityKind::Unsupported);
    }
    lex.regex["B"] = "(?u)\\p{L}+";
    report = RegexCompatibility::analyze(lex, "cpp17");
    assert(!report.configurableBudgets && report.rules[1].defaultBudget);
    lex.regex["A"] = "(?u)[a-z]+";
    report = RegexCompatibility::analyze(lex, "cpp17");
    assert(report.configurableBudgets);
    for (const auto &lang : {"js", "python", "go", "csharp"}) {
        report = RegexCompatibility::analyze(lex, lang);
        assert(report.rules[1].compatibility == RegexCompatibilityKind::Unsupported);
    }
    report = RegexCompatibility::analyze(lex, "gram");
    error = {};
    assert(RegexCompatibility::validate(report, true, error));
    assert(report.rules[0].compatibility == RegexCompatibilityKind::NotApplicable);
    lex.regex.clear();
    lex.regex["ESCAPED"] = "\\\\p";
    report = RegexCompatibility::analyze(lex, "python");
    assert(report.rules[0].compatibility != RegexCompatibilityKind::Unsupported);
    lex.regex["ESCAPED"] = "[](?=]+";
    report = RegexCompatibility::analyze(lex, "go");
    assert(report.rules[0].compatibility != RegexCompatibilityKind::Unsupported);
    lex.regex["ESCAPED"] = "(?<=a)b";
    report = RegexCompatibility::analyze(lex, "cpp17");
    assert(report.rules[0].compatibility == RegexCompatibilityKind::Unsupported);
    lex.regex["ESCAPED"] = "[|`<&]+";
    report = RegexCompatibility::analyze(lex, "cpp17", "vm");
    const auto text = RegexCompatibility::markdown(report);
    assert(text.find("&#124;&#96;&lt;&amp;") != std::string::npos);
    assert(report.configurableBudgets);
    return 0;
}
