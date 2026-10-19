/** @file regex_unicode.cpp
 * @brief 验证显式 Unicode 模式的码点、属性、大小写、捕获及 UTF-8 边界。
 */
#include "ParseEngine/regex_vm.hpp"
#include "../source/templates/regex_unicode_data.h.tmpl"
#include "../source/templates/regex_vm_runtime.h.tmpl"
#include <cassert>
#include <iostream>
#include <vector>

/** @brief 编译并执行一条规则。 @param pattern 正则。 @param text UTF-8 输入。
 * @param expected 期望的接受字节数，负一表示不匹配。
 */
static void check(const std::string &pattern, const std::string &text, int expected)
{
    ParseEngine::RegexVmProgram compiled;
    ParseEngine::Error error;
    if (!ParseEngine::RegexVm::compile(pattern, compiled, error))
    {
        std::cerr << pattern << ": " << error.message << '\n';
        std::abort();
    }
    std::vector<PeRegexInst> code;
    for (const auto &instruction : compiled.code)
    {
        PeRegexInst item{};
        item.op = instruction.op; item.next = instruction.next;
        item.other = instruction.other; item.value = instruction.value;
        item.rangeOffset = static_cast<size_t>(instruction.rangeOffset);
        item.rangeCount = static_cast<size_t>(instruction.rangeCount);
        for (unsigned index = 0; index < 32; ++index) item.bits[index] = static_cast<unsigned char>(instruction.bits[index]);
        code.push_back(item);
    }
    const PeRegexProgram program{code.data(), code.size(), static_cast<size_t>(compiled.entry),
        static_cast<size_t>(compiled.groups), compiled.ranges.data(), compiled.unicode ? 1 : 0};
    std::vector<size_t> captures(2 * (program.groups + 1), (size_t)-1);
    size_t end = 0;
    const int found = PeRegexRun(&program, text.data(), text.size(), program.entry, 0, captures.data(), &end);
    if (found != (expected >= 0 ? 1 : 0) || (found == 1 && end != static_cast<size_t>(expected)))
    {
        std::cerr << pattern << " 输入=" << text << " 实际=" << found << ',' << end << " 期望=" << expected << '\n';
        std::abort();
    }
}
/** @brief 运行 Unicode 语义回归。 @return 成功返回零。 */
int main()
{
    check("(?u).", "中", 3);
    check(".", "中", 1);
    check("(?u).{2}", "中🙂a", 7);
    check("(?u).{2}", "é", 3);
    check("(?u).+?", "中🙂", 3);
    check("(?u)[中🙂]+", "中🙂🙂!", 11);
    check("(?u)[一-龥]+", "中文!", 6);
    check(R"((?u)\p{L}+)", "中文ΩЖabc١", 13);
    check(R"((?u)\p{gc=Lu}+)", "ΩЖa", 4);
    check(R"((?u)\p{General_Category=So}+)", "🙂!", 4);
    check(R"((?u)\p{L})", "𠀀", 4);
    check(R"((?u)\p{Lo})", "\U0001E6C0", 4);
    check("(?iu)\uA7CE", "\uA7CF", 3);
    check(R"((?u)[[:alpha:]]+)", "中文Ω!", 8);
    check(R"((?iu)[[:upper:]]+)", "ςKſ!", 7);
    check(R"((?iu)[[:^upper:]])", "ς", -1);
    check(R"((?u)\d+)", "١٢۳４!", 9);
    check(R"((?u)\w+)", "é_中文١!", 12);
    check(R"((?u)\b中文\b)", "中文!", 6);
    check(R"((?u)\b中\b)", "中文", -1);
    check(R"((?u)中\B文)", "中文", 6);
    check(R"((?u)\s+)", "　\u00a0\u202f!", 8);
    check(R"((?u)\s)", "\ufeff", -1);
    check("(?u).", "\u2028", -1);
    check(R"((?u)\p{Zl})", "\u2028", 3);
    check("(?iu)[a-z]+", "ſKK!", 6);
    check("(?ui)[^k]", "K", -1);
    check("(?i)(?u)σ+", "Σσς!", 6);
    check(R"((?iu)(Σ)-\1)", "σ-ς", 5);
    check(R"((?iu)(K)-\1)", "K-k", 5);
    check(R"((?iu)(K)-\1)", "k-K", 5);
    check("(?iu)ß", "ẞ", 3);
    check("(?iu)ß", "SS", -1);
    check("(?iu)𐐀", "𐐨", 4);
    check("(?u)é", "é", -1);
    check(R"((?u)(?=(中文))\1)", "中文!", 6);
    check(R"((?u)中(?!英))", "中文", 3);
    check(R"((?iu)[\P{Lu}]+)", "a", -1);
    check(R"((?u)\u{1F642})", "🙂", 4);
    for (const std::string bad : {std::string("\xC0\xAF"), std::string("\xE0\x80\xAF"),
         std::string("\xED\xA0\x80"), std::string("\xF4\x90\x80\x80"), std::string("\xF0\x9F"), std::string("\x80")})
        check("(?u).+", bad, -1);
    const std::string large(20000, 'a');
    check("(?u).+", "中" + large + "🙂", 20007);
    std::cout << "Unicode 码点、属性、边界、折叠、反向引用及非法 UTF-8 回归通过\n";
}
