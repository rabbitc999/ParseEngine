/**
 * @file preprocessor.hpp
 * @brief .gram 文本的条件编译预处理（%if / %elif / %else / %endif）。
 */

#ifndef SCRIPT_ENGINE_PREPROCESSOR_HPP
#define SCRIPT_ENGINE_PREPROCESSOR_HPP

#include <cstddef>
#include <string>
#include <vector>

#include "ParseEngine/grammar_loader.hpp"
#include "ParseEngine/grammar_types.hpp"

namespace ParseEngine
{

/**
 * @brief 预处理结果。
 */
struct PreprocessResult
{
    /**
     * @brief 处理后的文本。
     *
     * 文本里一个条件编译指令都没有时，这里与输入**逐字节相同**（会走恒等
     * 快路径，不重组行）；有条件编译指令时，只有条件为假的整行被丢掉。
     */
    std::string text;

    /**
     * @brief 行号映射：处理后第 i 行（1 起）对应原文件第 lineMap[i - 1] 行。
     *
     * 没有条件编译指令时为空（此时行号就是恒等的）。
     */
    std::vector<std::size_t> lineMap;
};

/**
 * @brief 条件编译预处理器。
 *
 * 在 GrammarLoader 之前跑一遍纯文本处理：逐行扫描、维护 %if 栈，条件为假的
 * 整行丢掉，条件为真的整行原样保留。注释、字符串字面量与 `%code ... %endcode` 用户
 * 代码块里的 `%if` 不当指令看待。
 *
 * 条件只能来自命令行 `--define NAME[=VALUE]`；文法里引用不到的变量一律为假。
 */
class Preprocessor
{
public:
    /**
     * @brief 执行条件编译预处理。
     * @param text .gram 原文（允许带 UTF-8 BOM）。
     * @param defines 条件变量表（`--define` 解析出来的结果）。
     * @param out 处理后的文本与行号映射（成功时写入）。
     * @param error 出错时写入错误说明（带原文件行号）。
     * @return 成功返回 true；指令配对或条件表达式语法有误返回 false。
     */
    static bool run(const std::string &text, const DefineMap &defines,
                    PreprocessResult &out, Error &error);

    /**
     * @brief 求值一个条件表达式（`||` / `&&` / `!` / 括号 / 变量名）。
     * @param condition 条件原文（例如 `DEBUG && !QUIET`）。
     * @param defines 条件变量表。
     * @param out 条件真假（成功时写入）；未定义的变量为假。
     * @param error 出错时写入错误说明。
     * @return 成功返回 true；表达式语法有误返回 false。
     */
    static bool evaluateCondition(const std::string &condition,
                                  const DefineMap &defines, bool &out,
                                  Error &error);

    /**
     * @brief 把 `--define NAME=VALUE` 的 VALUE 解释成真假。
     *
     * `0` / `false` / `no` / `off`（大小写不敏感）与空串为假，其余非空为真。
     * @param value 值的原文。
     * @return 真假。
     */
    static bool parseDefineValue(const std::string &value);

    /**
     * @brief 变量名是否合法（`[A-Za-z_][A-Za-z0-9_]*`）。
     * @param name 变量名。
     * @return 合法返回 true。
     */
    static bool isValidName(const std::string &name);

    /**
     * @brief 把 `--define` 的 `NAME` / `NAME=VALUE` 原文拆成名字与真假。
     * @param argument 参数原文。
     * @param outName 输出变量名。
     * @param outValue 输出真假取值。
     * @param error 出错时写入错误说明。
     * @return 成功返回 true；变量名不合法返回 false。
     */
    static bool parseDefineArgument(const std::string &argument,
                                    std::string &outName, bool &outValue,
                                    Error &error);
};

} // namespace ParseEngine

#endif // SCRIPT_ENGINE_PREPROCESSOR_HPP
