/** @file regex_dfa_std.cpp
 * @brief 对照标准库验证 C/C++ 共用的重复次数与转义解析。
 */
#include "ParseEngine/regex_dfa.hpp"
#include <cassert>
#include <regex>

/** @brief 对照完整匹配并验证 AST，失败时终止回归。 @return 成功返回零。 */
int main()
{
    using namespace ParseEngine;
    for (const std::string pattern : {"a{2,3}", "a{2,}", "a{0,0}", "a{0,2}", "a{3}", "a{2,3}?", "a{2,}?", "a{0,0}?"})
    {
        RegexCompileResult compiled;
        Error error;
        assert(RegexDfa::compileAll({{"A", pattern}}, compiled, error));
        const auto &dfa = compiled.compiled.front().dfa;
        const std::regex reference(pattern);
        for (int length = 0; length < 8; ++length)
        {
            int state = dfa.start;
            for (int i = 0; i < length; ++i)
                state = dfa.transitions.at(state).at(compiled.byteMap['a']);
            assert((dfa.accept.count(state) != 0) == std::regex_match(std::string(length, 'a'), reference));
        }
        std::shared_ptr<RegexNode> ast;
        assert(RegexDfa::parse(pattern, ast, error));
        assert(ast->lazy == (pattern.back() == '?'));
    }
    for (const std::string pattern : {"a{3,2}", "a{1001}", "a{2,1001}", "a{2,"})
    {
        std::shared_ptr<RegexNode> ast;
        Error error;
        assert(!RegexDfa::parse(pattern, ast, error));
    }
    std::shared_ptr<RegexNode> ast;
    Error error;
    assert(RegexDfa::parse(R"(\0)", ast, error));
    assert(ast->ranges.front().first == 0);
}
