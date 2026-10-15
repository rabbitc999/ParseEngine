/**
 * @file grammar_loader.hpp
 * @brief .gram 文本文法加载模块。
 */

#ifndef SCRIPT_ENGINE_GRAMMAR_LOADER_HPP
#define SCRIPT_ENGINE_GRAMMAR_LOADER_HPP

#include <string>

#include "ParseEngine/error.hpp"
#include "ParseEngine/grammar_types.hpp"

namespace ParseEngine
{

/**
 * @brief 名字能不能当 C++ 命名空间 / 文件名前缀。
 *
 * `%namespace` 与 `--namespace` 共用同一条规则：必须匹配
 * `[A-Za-z_][A-Za-z0-9_]*`、不能含 `::`、不能是 C++ 关键字；**空串合法**
 * （表示退化成现状：命名空间 `Parser`、文件名不带前缀）。
 * @param name 名字。
 * @return 合法返回 true。
 */
bool isValidCppNamespaceName(const std::string &name);

/** @brief 校验 C99 公开符号前缀。 */
bool isValidCPrefixName(const std::string &name);

/**
 * @brief 名字能不能当 Python 产物的解析器类名。
 *
 * `%class` 与 `--class` 共用同一条规则：必须匹配
 * `[A-Za-z_][A-Za-z0-9_]*`、不能是 Python 关键字；空串**不合法**
 * （调用方把空串当「没写」处理，用缺省类名 `Parser`）。
 * @param name 名字。
 * @return 合法返回 true。
 */
bool isValidPythonClassName(const std::string &name);

/**
 * @brief .gram 文本解析器（手写词法分析 + 递归下降）。
 */
class GrammarLoader
{
public:
    /**
     * @brief 解析 .gram 文本。
     * @param text .gram 文本内容（会容忍 UTF-8 BOM）。
     * @param out 解析出的文法配置（成功时写入）。
     * @param error 出错时写入错误说明（带原文件行号）。
     * @return 成功返回 true；文法有误返回 false。
     */
    static bool loadGrammarText(const std::string &text, GrammarConfig &out,
                                Error &error);

    /**
     * @brief 解析 .gram 文本（先做 %if 条件编译预处理）。
     * @param text .gram 文本内容。
     * @param defines 条件编译变量表（`--define` 解析出来的结果）。
     * @param out 解析出的文法配置（成功时写入）。
     * @param error 出错时写入错误说明（带原文件行号）。
     * @return 成功返回 true；文法有误返回 false。
     */
    static bool loadGrammarText(const std::string &text,
                                const DefineMap &defines, GrammarConfig &out,
                                Error &error);

    /**
     * @brief 读取并解析 .gram 文件。
     * @param path 文件路径。
     * @param out 解析出的文法配置（成功时写入）。
     * @param error 出错时写入错误说明（带原文件行号）。
     * @return 成功返回 true；读不出文件或文法有误返回 false。
     */
    static bool loadGrammarFile(const std::string &path, GrammarConfig &out,
                                Error &error);

    /**
     * @brief 读取并解析 .gram 文件（先做 %if 条件编译预处理）。
     * @param path 文件路径。
     * @param defines 条件编译变量表（`--define` 解析出来的结果）。
     * @param out 解析出的文法配置（成功时写入）。
     * @param error 出错时写入错误说明（带原文件行号）。
     * @return 成功返回 true；读不出文件或文法有误返回 false。
     */
    static bool loadGrammarFile(const std::string &path,
                                const DefineMap &defines, GrammarConfig &out,
                                Error &error);

    /**
     * @brief 取内置默认测试文法文本。
     * @return default_grammar.gram 模板内容。
     */
    static std::string defaultLoadText();

    /**
     * @brief 把 C 风格字符串/字符字面量转成实际字符串。
     * @param text 带引号的字面量文本。
     * @return 解码后的字符串。
     */
    static std::string decodeStringLiteral(const std::string &text);

    /**
     * @brief 解析正则字面量（只还原 \\\\ 与 \\" ，其余转义原样保留）。
     * @param text 带引号的字面量文本。
     * @return 解码后的正则文本。
     */
    static std::string decodeRegexLiteral(const std::string &text);

    /**
     * @brief 按 `|` 拆分固定字面量候选，`\\|` 表示普通竖线。
     * @param text 带引号的字面量文本。
     * @param what 出错信息里使用的指令名。
     * @param out 拆分后的候选字面量（成功时写入）。
     * @param error 出错时写入错误说明。
     * @return 成功返回 true；字面量有误返回 false。
     */
    static bool splitDefineLiteral(const std::string &text,
                                   const std::string &what,
                                   std::vector<std::string> &out,
                                   Error &error);

    /**
     * @brief 从文法配置里取出优先级元数据。
     * @param cfg 文法配置。
     * @return 优先级配置。
     */
    static PriorityConfig priorityFromConfig(const GrammarConfig &cfg);

    /**
     * @brief 从文法配置里取出词法扩展声明。
     * @param cfg 文法配置。
     * @return 词法扩展配置。
     */
    static LexSpec lexSpecFromConfig(const GrammarConfig &cfg);

    /**
     * @brief 把可能为空的优先级配置规范成四键结构。
     * @param priority 输入配置；可为 nullptr。
     * @return 规范后的优先级配置。
     */
    static PriorityConfig normalizePriority(const PriorityConfig *priority);

private:
    /**
     * @brief 解析 .gram 文本的实现（带 `include "…"` 的基准目录）。
     *
     * `loadGrammarText` / `loadGrammarFile` 都走它：前者没有文件上下文
     * （baseDir 为空，相对路径按当前工作目录算），后者把**文法文件所在目录**
     * 传进来，段标记上的 `include "…"` 就按它解析（与 `%copy` 同一套规则）。
     * @param text .gram 文本内容。
     * @param defines 条件编译变量表。
     * @param baseDir 文法文件所在目录（可为空）。
     * @param gramPath 传给 `--grammar-file` 的文法路径原文（`%token ... include`
     *                 的报错定位用；可为空）。
     * @param out 解析出的文法配置（成功时写入）。
     * @param error 出错时写入错误说明。
     * @return 成功返回 true。
     */
    static bool parseGrammar(const std::string &text, const DefineMap &defines,
                             const std::string &baseDir,
                             const std::string &gramPath, GrammarConfig &out,
                             Error &error);
};

} // namespace ParseEngine

#endif // SCRIPT_ENGINE_GRAMMAR_LOADER_HPP
