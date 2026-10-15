/**
 * @file code_gen.hpp
 * @brief 产物生成的**共用**工具：语义动作分支、分段标记、#line 占位符回填。
 *
 * C99 后端自己那一套（parser.c / parser.h / parser_token.* / parser_table.c /
 * main.c）在 `code_gen_c99.hpp` 的 `CodeGenC99` 里；这里留的是各档后端都要用的
 * 那几个静态函数，`code_gen_cpp17 / python / csharp / rust` 都会 include 本文件。
 */

#ifndef SCRIPT_ENGINE_CODE_GEN_HPP
#define SCRIPT_ENGINE_CODE_GEN_HPP

#include <set>
#include <string>
#include <utility>
#include <vector>

#include "ParseEngine/analysis.hpp"
#include "ParseEngine/error.hpp"
#include "ParseEngine/grammar_types.hpp"

namespace ParseEngine
{

/**
 * @brief 产物生成的共用工具（五档后端共用）。
 */
class CodeGen
{
public:
    /**
     * @brief 生成每条最终产生式对应的语义动作分支（C / C++17 共用）。
     * @param analysis 分析结果。
     * @param translateMode 传给动作翻译器的模式。
     * @param symbolMember 符号到 %union 成员名的映射；没写 %union 时传 nullptr。
     * @param cpp17 目标语言是 C++17 时置 true（yyerrok / 空动作按 C++ 写）。
     * @param error 出错时写入错误说明。
     * @return 代码段；动作非法时返回空串并把错误写进 error。
     */
    static std::string actionCases(
        const AnalysisResult &analysis, const std::string &translateMode,
        const std::map<std::string, std::string> *symbolMember, bool cpp17,
        Error &error);

    /**
     * @brief 按「统一符号号 → %union 成员」把符号分组（%destructor 用）。
     *
     * 统一符号号：终结符就是 Token 号（0 .. 终结符个数-1），非终结符是
     * 终结符个数 + 非终结符编号——LR 栈 / LL 语义帧 / 生成的分派表都用这套。
     * 返回的每一项是「%destructor 的成员名 → 绑了它、会被丢到栈上的符号
     * （符号名 + 符号号）」，顺序与 %destructor 的书写顺序一致；没有任何符号
     * 绑定的成员保留成空列表，调用方按需跳过。
     * @param analysis 分析结果。
     * @param symbolMember 符号 -> %union 成员表；没写 %union 时传 nullptr。
     * @param destructors 成员 -> 清理片段；nullptr / 空表示没写 %destructor。
     * @return 成员 -> 符号列表；没写 %destructor 时为空表。
     */
    static std::map<std::string,
                      std::vector<std::pair<std::string, std::size_t>>>
    destructorSymbolGroups(
        const AnalysisResult &analysis,
        const std::map<std::string, std::string> *symbolMember,
        const std::map<std::string, std::string> *destructors);

    /// #line 里 .gram 路径的占位符（拼装时写它，回填时才落地真实路径）。
    static const char *grammarFilePlaceholder();

    /// 恢复行 #line 的占位符（行号与目标文件名在回填时才确定）。
    static const char *lineBackPlaceholder();

    /**
     * @brief 把产物里的 #line 占位符换成人能看懂的真实内容。
     *
     * - `#line <N> "@@SE_GRAMMAR_FILE@@"` → `#line <N> "<文法路径>"`
     *   （路径里的 `\` 与 `"` 会转义；文法路径为空时用 `<default_grammar.gram>`）
     * - `#line @@SE_LINE_BACK@@ "..."` → `#line <该指令下一行> "<fileName>"`
     *
     * 只替换行内文本、不增删行，所以行号不会自相矛盾。必须在所有片段都拼完之后
     * 再跑：那时 `fileName` 才是最终产物名（parser.cpp / main.cpp），行号也才是
     * 最终行号。
     * @param text 产物文本。
     * @param grammarPath 用户传给 --grammar-file 的路径（保持原样）。
     * @param fileName 该产物自己的文件名（basename，如 `parser.c`）。
     * @return 替换后的文本。
     */
    static std::string resolveLineDirectives(const std::string &text,
                                             const std::string &grammarPath,
                                             const std::string &fileName);

    /**
     * @brief 取分段标记名。
     * @param name 段名。
     * @param edge "BEGIN" / "END"。
     * @return 形如 __SEC_TABLES_BEGIN__ 的标记。
     */
    static std::string sectionMarker(const std::string &name,
                                     const std::string &edge);

    /**
     * @brief 取分段标记之间的内容（不含标记）。
     * @param text 输入文本。
     * @param name 段名。
     * @return 段内容。
     */
    static std::string extractSection(const std::string &text,
                                      const std::string &name);
};

} // namespace ParseEngine

#endif // SCRIPT_ENGINE_CODE_GEN_HPP
