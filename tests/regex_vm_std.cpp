/** @file regex_vm_std.cpp
 * @brief 将 C99 正则运行时与 std::regex 做系统性前缀匹配对照。
 */
#include "ParseEngine/regex_vm.hpp"
#include "../source/templates/regex_vm_runtime.h.tmpl"
#include <cassert>
#include <iostream>
#include <regex>
#include <vector>

/** @brief 比较输入上的成功状态及首个匹配长度。
 * @param pattern 原模式。 @param text 输入。 @param program VM 程序。 @param reference 标准库正则。
 */
static void check(const std::string &pattern, const std::string &text,
                  const PeRegexProgram &program, const std::regex &reference)
{
    std::vector<size_t> captures(2 * (program.groups + 1), (size_t)-1);
    size_t end = 0;
    const bool actual = PeRegexRun(&program, text.data(), text.size(), program.entry, 0, captures.data(), &end) != 0;
    std::smatch match;
    const bool expected = std::regex_search(text, match, reference, std::regex_constants::match_continuous);
    if ((actual != expected) || (actual && end != static_cast<size_t>(match.length())))
    {
        std::cerr << "匹配不一致: " << pattern << " 输入=" << text << " VM=" << actual << ',' << end
                  << " 标准库=" << expected << ',' << match.length() << '\n';
        std::abort();
    }
}
/** @brief 对照捕获、前瞻、边界、大小写和重复回溯。 @return 成功返回零。 */
int main()
{
    using namespace ParseEngine;
    const std::vector<std::string> patterns = {
        R"(a(?=b))", R"(a(?!b))", R"((?=ab)ab)", R"((?!ab)a+)",
        R"(\bword\b)", R"(a\Bb)", R"(\Baa)", R"(a\B)", R"(a\b)",
        R"((a|ab)\1)", R"((a+)\1)", R"(([a-z]+)-\1)", R"((a)?b\1)",
        R"((?=(a+))a*b\1)", R"((?!(a))b\1)", R"((a)(b)\2\1)",
        R"((?i)(ab)\1)", R"((?i)[^a]+)", R"((?i)[a-z]{2,4})", R"((?i)abc)", R"((?i)a+b)",
        R"((a?)*b)", R"((a*)*b)", R"((a|b)+?b)", R"((ab){1,3}?a)",
        R"(a{2,3})", R"(a{2,})", R"(^a+$)", R"(a$|ab)", R"(a|ab)",
        R"([\x41-\x5a]+)", R"([\u0041-\u005a]+)", R"([\n-\r]+)", R"([\b]+)",
        R"(\s+)", R"(\S+)", R"(\w+)", R"(\d+)", R"(中(?=文))", R"([中]+)", R"((?i)中(a)\1)",
        R"((?:ab|a)*?b)", R"((a(b)?)+\2)", R"((a|b){0,3}\1)", R"(a(?=(b))\1)"
    };
    std::vector<std::string> texts = {"", "word", "word!", "wordx", "ab-ab", "abAB", "ABab", "ABC", "aaa", "aabbaa", "\n\r", "\b", "a\r", "a\n", "中文", "中aA", "中aa", "文", "\t", std::string(1, static_cast<char>(0xA0))};
    for (unsigned length = 0; length <= 7; ++length)
        for (unsigned value = 0; value < (1u << length); ++value)
        {
            std::string text(length, 'a');
            for (unsigned bit = 0; bit < length; ++bit) if (value & (1u << bit)) text[bit] = 'b';
            texts.push_back(text);
        }
    for (const auto &pattern : patterns)
    {
        RegexVmProgram compiled;
        Error error;
        if (!RegexVm::compile(pattern, compiled, error))
        {
            std::cerr << pattern << ": " << error.message << '\n';
            return 1;
        }
        std::vector<PeRegexInst> code;
        for (const auto &instruction : compiled.code)
        {
            PeRegexInst item{};
            item.op = instruction.op; item.next = instruction.next;
            item.other = instruction.other; item.value = instruction.value;
            for (unsigned index = 0; index < 32; ++index) item.bits[index] = static_cast<unsigned char>(instruction.bits[index]);
            code.push_back(item);
        }
        const PeRegexProgram program{code.data(), code.size(), static_cast<size_t>(compiled.entry), static_cast<size_t>(compiled.groups)};
        const bool ignoreCase = pattern.compare(0, 4, "(?i)") == 0;
        const auto flags = std::regex::ECMAScript | (ignoreCase ? std::regex::icase : std::regex_constants::syntax_option_type{});
        const std::regex reference(ignoreCase ? pattern.substr(4) : pattern, flags);
        for (const auto &text : texts) check(pattern, text, program, reference);
        if (pattern == "(?i)a+b")
        {
            const std::string large = std::string(50000, 'A') + "b";
            assert(PeRegexMatch(&program, large.c_str()) == large.size());
        }
    }
    for (const std::string invalid : {R"((a)\2)", R"((?<=a)b)", R"((?i:a))", R"([\x5a-\x41])", "(?=a"})
    {
        RegexVmProgram program;
        Error error;
        assert(!RegexVm::compile(invalid, program, error));
    }
    {
        std::shared_ptr<RegexNode> ast;
        Error error;
        assert(RegexDfa::parse(R"((?=(a+))\1)", ast, error));
        assert(!RegexVm::nullable(ast));
        assert(RegexDfa::parse(R"((a*)\1)", ast, error));
        assert(RegexVm::nullable(ast));
    }
    std::cout << patterns.size() * texts.size() << " 组 VM / std::regex 前缀对照通过\n";
}
