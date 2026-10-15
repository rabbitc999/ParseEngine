/**
 * @file code_gen_python.hpp
 * @brief Python 语言后端：单个 parser.py + main.py。
 *
 * 与 C99 / C++17 后端的区别：
 *   - 产物就是一个 Python 模块（``parser.py``），表 / 运行时 / 公开 API 都在
 *     里面，类名可以用 `%class` / ``--class`` 改；
 *   - 语义值是任意 Python 对象，所以没有 `%union` / `%value_type` /
 *     `%token <成员>` 这些 C 专属写法；`%code … %endcode` 与
 *     `%epilogue` / `%main` 都能用，正文按 Python 的缩进规则归一化后插入；
 *   - 分析表与词法 DFA 仍然复用生成期算好的结果，只是重排成 Python 字面量。
 *
 * 这个后端主要是给「快速分析一份文法」用的：不需要编译器、不需要构建步骤，
 * 直接 ``python3 main.py`` 就能试文法。
 */

#ifndef SCRIPT_ENGINE_CODE_GEN_PYTHON_HPP
#define SCRIPT_ENGINE_CODE_GEN_PYTHON_HPP

#include <string>
#include <vector>

#include "ParseEngine/analysis.hpp"
#include "ParseEngine/error.hpp"
#include "ParseEngine/project.hpp"

namespace ParseEngine
{

/**
 * @brief Python 产物生成器（parser.py / main.py）。
 */
class CodeGenPython
{
public:
    /**
     * @brief 生成 parser.py（词法器 + 分析驱动 + 公开 API）。
     *
     * 文法里写了 Python 不支持的 C 专属指令（`%value_type` / `%union` /
     * `%token <成员>` / `%namespace`）时返回 false 并把原因写进 error；
     * `%code … %endcode` 会按 Python 缩进规则归一化后插在
     * 模块 import 之后。
     * @param request 组装入参。
     * @param mode 归一化后的生成模式。
     * @param analysis 分析结果。
     * @param out 输出文本（成功时写入）。
     * @param error 出错时写入错误说明。
     * @return 成功返回 true。
     */
    static bool parserPy(const ProjectRequest &request, const std::string &mode,
                         const AnalysisResult &analysis, std::string &out,
                         Error &error);

    /**
     * @brief 生成 main.py。
     *
     * 文法里没写 %main 时用内建 demo main（读标准输入一行 → 解析 → 报告
     * 结果）；写了就整段替换掉内建 demo（外壳 import 不变）。
     * @param request 组装入参。
     * @param mode 归一化后的生成模式。
     * @param mainBody %main 正文；为空表示用内建 demo main。
     * @return main.py 文本。
     */
    static std::string mainPy(const ProjectRequest &request,
                              const std::string &mode,
                              const std::string &mainBody);

    /**
     * @brief 生成词法模式（`%mode lex`）的 parser.py：只有词法器与 tokenize。
     *
     * 产物里没有分析表 / 分析栈 / 解析入口；`%class`、`%code`、
     * `%epilogue` 照常生效，`%value_type` / `%union` / `%token <成员>` 仍会被
     * 拒绝（Python 的语义值是任意对象）。
     * @param request 组装入参。
     * @param terminals Token 列表（内置 Token + 文法声明的 + END）。
     * @param out 输出文本（成功时写入）。
     * @param error 出错时写入错误说明。
     * @return 成功返回 true。
     */
    static bool lexerPy(const ProjectRequest &request,
                        const std::vector<std::string> &terminals,
                        std::string &out, Error &error);

    /**
     * @brief 生成词法模式（`%mode lex`）的 main.py。
     *
     * 内建 demo 把标准输入读到 EOF，逐个打印 Token 的类型与词素；写了 `%main`
     * 就整段替换内建 demo（外壳 import 不变）。
     * @param request 组装入参。
     * @param mainBody `%main` 正文；为空表示用内建 demo。
     * @return main.py 文本。
     */
    static std::string lexerMainPy(const ProjectRequest &request,
                                   const std::string &mainBody);

    /**
     * @brief Python 解析器类名（`%class` / ``--class``；缺省 ``Parser``）。
     * @param request 组装入参。
     * @return 类名。
     */
    static std::string className(const ProjectRequest &request);

    /**
     * @brief 把用户代码块归一化成可以直接贴进 Python 模块的形态。
     *
     * 做三件事：去掉首尾空行；按所有非空行的**最小**前导空白去掉公共缩进
     * （`%code` / `%epilogue` / `%main` 常常跟着文法缩进写，直接贴进
     * Python 模块会是 IndentationError）；剩下的前导 tab 统一换成 4 个空格
     * （避免同一份代码里 tab / 空格混用）。正文里除了行首空白一个字符都不动。
     * @param code 用户代码原文。
     * @return 归一化后的代码（末尾带一个换行；正文为空时返回空串）。
     */
    static std::string normalizeUserCode(const std::string &code);

    /**
     * @brief 生成一行「这段代码来自 .gram 的哪一行」的标记注释。
     *
     * Python 没有 C 的 `#line` 指令——报错只会给 `parser.py` / `main.py` 自己的
     * 行号。这里用一行**注释**把文法文件与行号记在生成产物里，回头对着看就知道
     * 某段代码是 `.gram` 的哪一行来的：
     *
     * ```python
     * # 语法文件 example/calc.gram 行 42
     * result.value = values[0] + values[2]
     * ```
     *
     * 路径先写占位符，收尾时换成真实路径（与 C 目标的 `#line` 同一套占位符）。
     * @param lineNumber `.gram` 里的行号；0 表示没有行号信息。
     * @return 一行注释（**自带结尾换行**、不带缩进）；没有行号信息时返回空串。
     */
    static std::string grammarLineMarker(std::size_t lineNumber);

    /// @return parser.py 的产物文件名（固定 ``parser.py``）。
    static std::string parserFileName();
    /// @return main.py 的产物文件名（固定 ``main.py``）。
    static std::string mainFileName();
};

} // namespace ParseEngine

#endif // SCRIPT_ENGINE_CODE_GEN_PYTHON_HPP
