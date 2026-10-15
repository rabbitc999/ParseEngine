/**
 * @file code_gen_cpp17.hpp
 * @brief C++17 语言后端：独立于 C99 模板的 parser.hpp / parser.cpp / main.cpp。
 *
 * 与 C99 后端（code_gen.hpp）的分工：
 *   - 分析表 / 词法 DFA 仍然复用生成期算好的结果，只是重新排版成 C++17 的
 *     写法（`inline constexpr` 常量、`std::uint16_t` 数组、无 `#define`）；
 *   - 运行时骨架来自 source/templates/cpp17_*.tmpl，与 C 模板完全分离；
 *   - 语义动作仍然按普通 C 表达式翻译（`$n` / `$$` 展开成 C++ 也能编译的
 *     分析栈下标访问），所以同一段动作两种目标语言都能用。
 */

#ifndef SCRIPT_ENGINE_CODE_GEN_CPP17_HPP
#define SCRIPT_ENGINE_CODE_GEN_CPP17_HPP

#include <optional>
#include <string>
#include <vector>

#include "ParseEngine/analysis.hpp"
#include "ParseEngine/error.hpp"
#include "ParseEngine/project.hpp"

namespace ParseEngine
{

/**
 * @brief C++17 产物生成器（parser.hpp / parser.cpp / main.cpp）。
 *
 * 产物只有一个命名空间（缺省 `Parser`）、一个公开类 `Engine` 与 d-pointer，
 * 全部内存由标准库管理——**不支持自定义分配器**，也没有释放钩子。
 */
class CodeGenCpp17
{
public:
    /**
     * @brief 生成 parser.hpp（公开接口）。
     * @param request 组装入参。
     * @param mode 归一化后的生成模式。
     * @param analysis 分析结果。
     * @param unionBodyWithLines %union 正文（已插好 #line；没有 %union 时为空串）。
     * @param out 输出文本（成功时写入）。
     * @param error 出错时写入错误说明。
     * @return 成功返回 true。
     */
    static bool parserHpp(const ProjectRequest &request, const std::string &mode,
                          const AnalysisResult &analysis,
                          const std::string &unionBodyWithLines,
                          std::string &out, Error &error);

    /**
     * @brief 生成 parser.cpp（运行时实现）。
     * @param request 组装入参。
     * @param mode 归一化后的生成模式。
     * @param analysis 分析结果。
     * @param out 输出文本（成功时写入）。
     * @param error 出错时写入错误说明。
     * @return 成功返回 true；语义动作非法时返回 false。
     */
    static bool parserCpp(const ProjectRequest &request, const std::string &mode,
                          const AnalysisResult &analysis, std::string &out,
                          Error &error);

    /**
     * @brief 生成 main.cpp。
     *
     * 文法里没写 %main 时用内建 demo main（读标准输入一行 → 解析 → 报告
     * 结果）；写了就整段替换掉内建 demo main（外壳不变）。
     * @param request 组装入参。
     * @param mode 归一化后的生成模式。
     * @param mainBody %main 正文（已插好 #line）；为空表示用内建 demo main。
     * @return main.cpp 文本。
     */
    static std::string mainCpp(const ProjectRequest &request,
                               const std::string &mode,
                               const std::string &mainBody);

    /**
     * @brief 生成 parser_user.h 的正文（只有用户代码）。
     *
     * C++17 下 `%value_type` / `%union` 会写进 parser.hpp 的命名空间里，所以
     * parser_user.h 只放 `%code` 的用户代码；没写用户代码时返回空
     * optional（不生成该文件）。
     * @param request 组装入参。
     * @param userCodeWithLines 插好 #line 的用户代码。
     * @return 文件内容；没有用户代码时返回空 optional。
     */
    static std::optional<std::string> userHeader(const ProjectRequest &request,
                                                 const std::string &userCodeWithLines);

    /**
     * @brief 命名空间名（空串按 `Parser` 处理）。
     * @param request 组装入参。
     * @return 命名空间名。
     */
    static std::string namespaceName(const ProjectRequest &request);

    /**
     * @brief 产物文件名前缀（`%namespace` / `--namespace` 的名字 + `_`）。
     *
     * 没写 `%namespace` 时是空串：产物就叫 `parser.hpp` / `parser.cpp` /
     * `main.cpp` / `parser_user.h`；写了 `%namespace myapp` 时是 `myapp_`，
     * 产物是 `myapp_parser.hpp` 等（多个解析器可以同时链接）。
     * @param request 组装入参。
     * @return 文件名前缀。
     */
    static std::string filePrefix(const ProjectRequest &request);

    /// @return parser.hpp 的产物文件名（含前缀）。
    static std::string hppName(const ProjectRequest &request);
    /// @return parser.cpp 的产物文件名（含前缀）。
    static std::string parserSourceName(const ProjectRequest &request);
    /// @return main.cpp 的产物文件名（含前缀）。
    static std::string mainSourceName(const ProjectRequest &request);

    /// @return 词法模式产物头文件名（``lexer.hpp``，带 %namespace 前缀）。
    static std::string lexerHppName(const ProjectRequest &request);
    /// @return 词法模式产物源文件名（``lexer.cpp``，带 %namespace 前缀）。
    static std::string lexerSourceName(const ProjectRequest &request);

    /**
     * @brief 生成词法模式（`%mode lex`）的 lexer.hpp。
     * @param request 组装入参。
     * @param terminals Token 列表（内置 Token + 文法声明的 + END）。
     * @param unionBodyWithLines `%union` 正文（带 #line 占位符）。
     * @param out 输出文本（成功时写入）。
     * @param error 出错时写入错误说明。
     * @return 成功返回 true。
     */
    static bool lexerHpp(const ProjectRequest &request,
                         const std::vector<std::string> &terminals,
                         const std::string &unionBodyWithLines,
                         std::string &out, Error &error);

    /**
     * @brief 生成词法模式（`%mode lex`）的 lexer.cpp：只有词法器与 tokenize。
     * @param request 组装入参。
     * @param terminals Token 列表。
     * @param unionBodyWithLines `%union` 正文（带 #line 占位符）。
     * @param out 输出文本（成功时写入）。
     * @param error 出错时写入错误说明。
     * @return 成功返回 true。
     */
    static bool lexerCpp(const ProjectRequest &request,
                         const std::vector<std::string> &terminals,
                         const std::string &unionBodyWithLines,
                         std::string &out, Error &error);

    /**
     * @brief 生成词法模式（`%mode lex`）的 main.cpp。
     * @param request 组装入参。
     * @param mainBody `%main` 正文；为空表示用内建 demo。
     * @return main.cpp 文本。
     */
    static std::string lexerMainCpp(const ProjectRequest &request,
                                    const std::string &mainBody);
    /// @return parser_user.h 的产物文件名（含前缀）。
    static std::string userHeaderName(const ProjectRequest &request);
};

} // namespace ParseEngine

#endif // SCRIPT_ENGINE_CODE_GEN_CPP17_HPP
