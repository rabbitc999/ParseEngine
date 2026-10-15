/**
 * @file code_gen_go.hpp
 * @brief Go 语言后端：Go 模块（go.mod + parser.go + token.go + main.go）。
 *
 * 结构照抄 Rust 后端（code_gen_rust.*），逐项对齐：
 *   - 类型名与字段名用导出的 Go 写法（Parser / Token / Tree / ParserError /
 *     ParserValue），方法与内部辅助函数按 Go 习惯（`func (p *Parser) …`、
 *     包级私有符号带 `sParser` 前缀）；
 *   - `%package` 落到 go.mod 的 module path（可执行程序固定 `package main`）、
 *     `%import` 写进 import 块、`%class_body` 贴成一组 `func (p *Parser) …`
 *     方法（Go 没有 class`);
 *   - 语义值**按值拷贝**：`%union` 生成 `type ParserValue struct { … }`，
 *     成员声明照 C 写法 `int32 ival;` 拼成 Go 的字段写法 `ival int32`；
 *   - 没有 `%destructor`（由 GC 回收，文法里写了直接报错）；
 *   - 十六种生成模式均可用；Earley / CYK 复用 GLR 的 GSS / packed forest
 *     运行时，并在运行前执行图表算法识别；
 *   - 产物模板是 `source/templates/go_*.tmpl`，走 `library/template` 的
 *     模板引擎（`tmpl::renderValues`）；**词法正则用标准库的 `regexp`**
 *     （没有第三方依赖）。
 */

#ifndef SCRIPT_ENGINE_CODE_GEN_GO_HPP
#define SCRIPT_ENGINE_CODE_GEN_GO_HPP

#include <string>
#include <vector>

#include "ParseEngine/analysis.hpp"
#include "ParseEngine/error.hpp"
#include "ParseEngine/project.hpp"

namespace ParseEngine
{

/**
 * @brief Go 产物生成器（go.mod + parser.go + token.go + main.go）。
 */
class CodeGenGo
{
public:
    /**
     * @brief 生成 parser.go（解析器 + 常量 + 表 + 运行时）。
     *
     * 文法里写了 Go 不支持的 `%destructor` 时返回 false 并把原因写进 error。
     * @param request 组装入参。
     * @param mode 归一化后的生成模式。
     * @param analysis 分析结果。
     * @param out 输出文本（成功时写入）。
     * @param error 出错时写入错误说明。
     * @return 成功返回 true。
     */
    static bool parserGo(const ProjectRequest &request, const std::string &mode,
                         const AnalysisResult &analysis, std::string &out,
                         Error &error);

    /**
     * @brief 生成 token.go（Token 类型常量 + 词法单元）。
     * @param request 组装入参。
     * @param terminals Token 列表（内置 Token + 文法声明的 + END）。
     * @param out 输出文本（成功时写入）。
     */
    static void tokenGo(const ProjectRequest &request,
                        const std::vector<std::string> &terminals,
                        std::string &out);

    /**
     * @brief 生成 main.go。
     *
     * 文法里没写 %main 时用内建 demo（读标准输入一行 → 解析 → 报告结果 /
     * 打印语法树）；写了就整段替换掉内建 demo（外壳的 `package main` 不变）。
     * @param request 组装入参。
     * @param mode 归一化后的生成模式。
     * @param mainBody %main 正文（已插好行标记）；为空表示用内建 demo。
     * @return main.go 文本。
     */
    static std::string mainGo(const ProjectRequest &request,
                              const std::string &mode,
                              const std::string &mainBody);

    /**
     * @brief 生成词法模式（`%mode lex`）的 parser.go：只有词法器与 Tokenize。
     *
     * 产物里没有分析表 / 分析栈 / 解析入口；`%class_body` 式的用户正文照常生效。
     * 这一段没有失败路径（词法正则认不下只打警告）。
     * @param request 组装入参。
     * @param terminals Token 列表。
     * @param out 输出文本。
     */
    static void lexerGo(const ProjectRequest &request,
                        const std::vector<std::string> &terminals,
                        std::string &out);

    /**
     * @brief 生成词法模式（`%mode lex`）的 main.go。
     *
     * 内建 demo 把标准输入读到 EOF，逐个打印 Token 的类型与词素；写了 `%main`
     * 就整段替换内建 demo（外壳的 `package main` 不变）。
     * @param request 组装入参。
     * @param mainBody %main 正文；为空表示用内建 demo。
     * @return main.go 文本。
     */
    static std::string lexerMainGo(const ProjectRequest &request,
                                   const std::string &mainBody);

    /**
     * @brief 生成一行「这段代码来自 .gram 的哪一行」的标记注释。
     *
     * Go 没有 C 的 `#line` 指令——报错只会给 `parser.go` / `main.go` 自己的
     * 行号。这里用一行**注释**把文法文件与行号记在生成产物里：
     *
     * ```go
     * // 在 example/calc.gram 的第 42 行
     * result.value = values[0] + values[2]
     * ```
     *
     * 路径先写占位符，收尾时换成真实路径（与 C 目标的 `#line` 同一套占位符）。
     * @param lineNumber `.gram` 里的行号；0 表示没有行号信息。
     * @return 一行注释（**自带结尾换行**、不带缩进）；没有行号信息时返回空串。
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
     * @brief 生成 go.mod（module path 来自 `%package`，缺省 `parserdemo`）。
     * @param request 组装入参。
     * @return go.mod 文本。
     */
    static std::string goMod(const ProjectRequest &request);

    /**
     * @brief `%package` 映射成的 module path（`::` 归一成 `/`；没写时 `parserdemo`）。
     * @param request 组装入参。
     * @return module path。
     */
    static std::string moduleName(const ProjectRequest &request);
};

} // namespace ParseEngine

#endif // SCRIPT_ENGINE_CODE_GEN_GO_HPP
