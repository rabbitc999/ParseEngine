/**
 * @file code_gen_rust.hpp
 * @brief Rust 语言后端：Cargo 工程（src/parser.rs + src/token.rs + src/main.rs）。
 *
 * 结构照抄 C# 后端（code_gen_csharp.*），逐项对齐：
 *   - 类型名 PascalCase（Parser / Token / Tree / ParserError / ParserValue），
 *     方法与字段用 Rust 的 snake_case；
 *   - `%package` 决定 Cargo.toml 里的 crate 名（Rust 没有 namespace 这一层）、
 *     `%import` 生成 `use NAME;`、`%class_body` 插成一段 `impl Parser { ... }`；
 *   - 语义值**按值拷贝**：语义值类型要满足 `Copy + Default + Debug`，
 *     `%union` 生成的 `ParserValue` 会 `#[derive(Clone, Copy, Default, Debug)]`；
 *   - 没有 `%destructor`（由所有权回收，文法里写了直接报错）；
 *   - 产物模板是 `source/templates/rust_*.tmpl`，走 `library/template` 的
 *     模板引擎（`tmpl::renderValues`）。
 */

#ifndef SCRIPT_ENGINE_CODE_GEN_RUST_HPP
#define SCRIPT_ENGINE_CODE_GEN_RUST_HPP

#include <string>
#include <vector>

#include "ParseEngine/analysis.hpp"
#include "ParseEngine/error.hpp"
#include "ParseEngine/project.hpp"

namespace ParseEngine
{

/**
 * @brief Rust 产物生成器（Cargo 工程：src/parser.rs / src/token.rs / src/main.rs）。
 */
class CodeGenRust
{
public:
    /**
     * @brief 生成 src/parser.rs（解析器 + 常量 + 表 + 运行时）。
     *
     * 文法里写了 Rust 不支持的 `%destructor` 时返回 false 并把原因写进 error。
     * @param request 组装入参。
     * @param mode 归一化后的生成模式。
     * @param analysis 分析结果。
     * @param out 输出文本（成功时写入）。
     * @param error 出错时写入错误说明。
     * @return 成功返回 true。
     */
    static bool parserRs(const ProjectRequest &request, const std::string &mode,
                         const AnalysisResult &analysis, std::string &out,
                         Error &error);

    /**
     * @brief 生成 src/token.rs（Token 类型常量 + 词法单元）。
     * @param request 组装入参。
     * @param terminals Token 列表（内置 Token + 文法声明的 + END）。
     * @param out 输出文本（成功时写入）。
     */
    static void tokenRs(const ProjectRequest &request,
                        const std::vector<std::string> &terminals,
                        std::string &out);

    /**
     * @brief 生成 src/main.rs。
     *
     * 文法里没写 %main 时用内建 demo（读标准输入一行 → 解析 → 报告结果 /
     * 打印语法树）；写了就整段替换掉内建 demo（外壳 use 不变）。
     * @param request 组装入参。
     * @param mode 归一化后的生成模式。
     * @param mainBody %main 正文（已插好行标记）；为空表示用内建 demo。
     * @return main.rs 文本。
     */
    static std::string mainRs(const ProjectRequest &request,
                              const std::string &mode,
                              const std::string &mainBody);

    /**
     * @brief 生成词法模式（`%mode lex`）的 src/parser.rs：只有词法器与 tokenize。
     *
     * 产物里没有分析表 / 分析栈 / 解析入口；`%class` 式的用户正文照常生效。
     * 这一段没有失败路径（词法正则认不下只打警告）。
     * @param request 组装入参。
     * @param terminals Token 列表。
     * @param out 输出文本。
     */
    static void lexerRs(const ProjectRequest &request,
                        const std::vector<std::string> &terminals,
                        std::string &out);

    /**
     * @brief 生成词法模式（`%mode lex`）的 src/main.rs。
     *
     * 内建 demo 把标准输入读到 EOF，逐个打印 Token 的类型与词素；写了 `%main`
     * 就整段替换内建 demo（外壳 `use` 不变）。
     * @param request 组装入参。
     * @param mainBody %main 正文；为空表示用内建 demo。
     * @return main.rs 文本。
     */
    static std::string lexerMainRs(const ProjectRequest &request,
                                   const std::string &mainBody);

    /**
     * @brief 生成一行「这段代码来自 .gram 的哪一行」的标记注释。
     *
     * Rust 没有 C 的 `#line` 指令——报错只会给 `parser.rs` / `main.rs` 自己的
     * 行号。这里用一行**注释**把文法文件与行号记在生成产物里：
     *
     * ```rust
     * // 在 example/calc.gram 的第 42 行
     * result.value = values[0] + values[2];
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
     * @brief 生成 Cargo.toml（crate 名来自 `%package`，缺省 `parser_demo`）。
     * @param request 组装入参。
     * @return Cargo.toml 文本。
     */
    static std::string cargoToml(const ProjectRequest &request);

    /**
     * @brief `%package` 映射成的 crate 名（点号换成下划线；没写时 `parser_demo`）。
     * @param request 组装入参。
     * @return crate 名。
     */
    static std::string crateName(const ProjectRequest &request);
};

} // namespace ParseEngine

#endif // SCRIPT_ENGINE_CODE_GEN_RUST_HPP
