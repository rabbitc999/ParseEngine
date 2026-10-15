/**
 * @file code_gen_csharp.hpp
 * @brief C# 语言后端：Parser.cs / Token.cs / Program.cs / parser.csproj。
 *
 * 设计约定：
 *   - 类型名首字母大写（Parser / Token / Tree / ParserError / ParserValue）；
 *   - 公开方法名统一为 runText / errors / lastError / result；
 *   - `%package` 生成文件级 `namespace NAME;`，`%import` 生成 `using NAME;`；
 *   - `%class_body` … `%endbody` 插在解析器类内部；
 */

#ifndef SCRIPT_ENGINE_CODE_GEN_CSHARP_HPP
#define SCRIPT_ENGINE_CODE_GEN_CSHARP_HPP

#include <string>
#include <vector>

#include "ParseEngine/analysis.hpp"
#include "ParseEngine/error.hpp"
#include "ParseEngine/project.hpp"

namespace ParseEngine
{

/**
 * @brief C# 产物生成器（Parser.cs / Token.cs / Program.cs / parser.csproj）。
 */
class CodeGenCsharp
{
public:
    /**
     * @brief 生成 Parser.cs（解析器类 + 常量 + 表 + 运行时）。
     * @param request 组装入参。
     * @param mode 归一化后的生成模式。
     * @param analysis 分析结果。
     * @param out 输出文本（成功时写入）。
     * @param error 出错时写入错误说明。
     * @return 成功返回 true。
     */
    static bool parserCs(const ProjectRequest &request, const std::string &mode,
                         const AnalysisResult &analysis, std::string &out,
                         Error &error);

    /**
     * @brief 生成 Token.cs（Token 类型常量 + 词法单元类）。
     * @param request 组装入参。
     * @param terminals Token 列表。
     * @param out 输出文本（成功时写入）。
     */
    static void tokenCs(const ProjectRequest &request,
                        const std::vector<std::string> &terminals,
                        std::string &out);

    /**
     * @brief 生成 Program.cs（演示入口）。
     * @param request 组装入参。
     * @param mode 归一化后的生成模式。
     * @param mainBody %main 正文（已插好行标记）；为空表示用内建 demo。
     * @return Program.cs 文本。
     */
    static std::string mainCs(const ProjectRequest &request,
                              const std::string &mode,
                              const std::string &mainBody);

    /**
     * @brief 生成词法模式（`%mode lex`）的 Parser.cs：只有词法器与 tokenize。
     *
     * 这一段没有失败路径（词法正则认不下只打警告）。
     * @param request 组装入参。
     * @param terminals Token 列表。
     * @param out 输出文本。
     */
    static void lexerCs(const ProjectRequest &request,
                        const std::vector<std::string> &terminals,
                        std::string &out);

    /**
     * @brief 生成词法模式（`%mode lex`）的 Program.cs。
     * @param request 组装入参。
     * @param mainBody %main 正文；为空表示用内建 demo。
     * @return Program.cs 文本。
     */
    static std::string lexerMainCs(const ProjectRequest &request,
                                   const std::string &mainBody);

    /**
     * @brief 生成一行的「来自 .gram 哪一行」注释（C# 没有 #line）。
     * @param lineNumber `.gram` 里的行号；0 表示没有行号信息。
     * @return 一行注释（自带结尾换行、不带缩进）。
     */
    static std::string grammarLineMarker(std::size_t lineNumber);

    /**
     * @brief 给用户代码正文两边各插一行「来自 .gram 哪一行」的注释。
     * @param body 用户正文（原样）。
     * @param lineNumber 正文第一行在 `.gram` 里的行号；0 表示没有。
     * @param indent 注释行的缩进（跟着所在块走）。
     * @param fileName 产物文件名（后向注释里用）。
     * @return 带前后注释的正文。
     */
    static std::string withLineMarkers(const std::string &body,
                                       std::size_t lineNumber,
                                       const std::string &indent,
                                       const std::string &fileName);

    /**
     * @brief 生成 parser.csproj（C# 构建脚本）。
     *
     * 模板 `csharp_csproj.xml.tmpl` 是纯静态文件（目标框架号 net8.0 写在模板里，
     * 没有占位符），所以这里只是把模板取出来，不改一个字符。
     * @param request 组装入参（当前未使用）。
     * @return parser.csproj 文本。
     */
    static std::string csproj(const ProjectRequest &request);
};

} // namespace ParseEngine

#endif // SCRIPT_ENGINE_CODE_GEN_CSHARP_HPP
