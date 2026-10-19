/** @file regex_compatibility.hpp
 * @brief 显式词法正则的引擎选择、语义兼容性和预算能力报告。
 */
#ifndef PARSEENGINE_REGEX_COMPATIBILITY_HPP
#define PARSEENGINE_REGEX_COMPATIBILITY_HPP
#include <string>
#include <vector>
#include "ParseEngine/error.hpp"
#include "ParseEngine/grammar_types.hpp"

namespace ParseEngine
{
/** @brief 与共享正则规范的兼容程度；原生不等于已确认等价。 */
enum class RegexCompatibilityKind
{
    Shared, /**< 共享实现，或已确认的保守 ASCII 等价子集。 */
    Native, /**< 交给原生库，未证明与共享规范完全一致。 */
    Unsupported, /**< 已知目标后端不能表达此规则。 */
    NotApplicable /**< 文法导出，不执行正则。 */
};

/** @brief 一条显式正则规则的执行信息。 */
struct RegexRuleCompatibility
{
    std::string name; ///< Token 名、VARIABLE 或 commit 类型。
    std::string category; ///< token、identifier、commit 或 trivia。
    std::string pattern; ///< 未经过目标语言字符串编码的正则文本。
    std::string engine; ///< 实际引擎标识。
    std::string characterMode; ///< 字节、共享 Unicode 17.0 或原生字符域。
    std::vector<std::string> features; ///< 捕获、断言、反向引用等使用能力。
    std::string reason; ///< 选择引擎或不能确认语义的原因。
    RegexCompatibilityKind compatibility = RegexCompatibilityKind::Native; ///< 兼容程度。
    bool defaultBudget = false; ///< 该规则是否具有共享的默认执行预算。
};

/** @brief 一份词法规则集的兼容性报告。 */
struct RegexCompatibilityReport
{
    std::string language; ///< 目标语言。
    std::string cppLexer; ///< C++ 请求引擎，其他语言为空。
    std::vector<RegexRuleCompatibility> rules; ///< 包含标识符和注释在内的显式规则。
    bool configurableBudgets = false; ///< 是否能对全部显式规则配置共享预算。
};

/** @brief 词法引擎选择与保守语义校验，供报告和生成器共用。 */
class RegexCompatibility
{
public:
    /** @brief 是否属于 C++ DFA 与标准库等价的保守 ASCII 子集。
     * @param pattern 原模式。@return 可使用该快速路径时为真。
     */
    static bool cppDfaCompatible(const std::string &pattern);
    /** @brief 分析显式正则，不调用目标编译器或执行原生正则库。
     * @param lex 词法定义。@param language 目标语言。@param cppLexer C++ 引擎。
     * @return 逐规则执行信息；不包含内建数字、字符串或固定字面量。
     */
    static RegexCompatibilityReport analyze(const LexSpec &lex,
        const std::string &language, const std::string &cppLexer = "std");
    /** @brief 拒绝已知不支持的规则；严格模式还拒绝未证明等价的原生规则。
     * @param report 分析结果。@param strict 是否采用严格正则模式。
     * @param error 第一条失败说明。@return 所有规则通过时为真。
     */
    static bool validate(const RegexCompatibilityReport &report, bool strict, Error &error);
    /** @brief 渲染可追加到 RESULT.md 或直接打印的 Markdown 报告。
     * @param report 分析结果。@return 已转义表格特殊字符的文本。
     */
    static std::string markdown(const RegexCompatibilityReport &report);
};
}
#endif
