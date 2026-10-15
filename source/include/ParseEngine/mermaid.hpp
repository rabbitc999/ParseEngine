/**
 * @file mermaid.hpp
 * @brief 教学模式的状态机图（Mermaid）生成：文法 LR 自动机与词法 DFA。
 *
 * 由独立的 `%mermaid` / `--mermaid` 开关控制，**与教学模式无关**，只对
 * LR 家族模式（lr0 / slr / lalr / lr1 / ielr）有意义：这些模式手里
 * 都有完整的状态机（LR 项集族 + GOTO 转移），画成图正好和教材上的状态转换图
 * 对上。词法 DFA（`%token regex` / `%identifier`）则与模式无关，LL(1) 下也能画。
 *
 * 产出的图是 Mermaid 的 `flowchart` 源码，包进自带 CSS、从 CDN 加载 Mermaid
 * 的独立 HTML 页面里：用浏览器直接打开 `mermaid/index.html` 就能看图，不需要
 * 装任何工具。同一份源码也以 `.mmd` 形式落盘，方便贴进 Markdown 或
 * mermaid.live。没有网络时页面里的「Mermaid 源码」小节仍然可读可复制。
 */

#ifndef SCRIPT_ENGINE_MERMAID_HPP
#define SCRIPT_ENGINE_MERMAID_HPP

#include <string>
#include <utility>
#include <vector>

#include "ParseEngine/analysis.hpp"
#include "ParseEngine/error.hpp"
#include "ParseEngine/grammar_types.hpp"

namespace ParseEngine
{

/**
 * @brief 一张 Mermaid 图：标题 + 说明 + 源码。
 */
struct MermaidDiagram
{
    std::string title;  ///< 图标题（同时用作页面小节标题）
    std::string note;   ///< 图说明（HTML 片段，可为空）
    std::string source; ///< Mermaid 源码（`flowchart LR` 开头）
};

/**
 * @brief Mermaid 状态机图生成器（教学模式用）。
 */
class Mermaid
{
public:
    /**
     * @brief 生成文法自动机（LR 状态机）的 Mermaid 图。
     *
     * 状态用 LR 项集做标签（`lr0` / `slr` 画 LR(0) 项集，`lalr` / `lr1` /
     * `ielr` 按教材写法连展望符一起画），转移按 GOTO 关系连线：
     * 终结符上是移进边、非终结符上是 GOTO 边；状态按「有冲突 / 接受 / 归约 /
     * 移进」上色，和 ACTION 表里能填出什么动作一一对应。
     * @param analysis 分析结果。
     * @return 图的列表；不是 LR 家族模式（如 LL(1)）时返回空列表。
     */
    static std::vector<MermaidDiagram> grammarDiagrams(
        const AnalysisResult &analysis);

    /**
     * @brief 生成词法 DFA 的 Mermaid 图（每个 `%token regex` / `%identifier` 一张）。
     * @param lexSpec 词法扩展配置（正则 Token 与标识符正则）。
     * @param error 出错时写入错误说明。
     * @return 图的列表；没有正则 Token 时返回空列表。
     */
    static std::vector<MermaidDiagram> lexerDiagrams(const LexSpec &lexSpec,
                                                    Error &error);

    /**
     * @brief 把若干张图拼成一个独立 HTML 页面（自带 CSS，从 CDN 加载 Mermaid）。
     * @param title 页面标题。
     * @param intro 页面开头的说明（HTML 片段，可为空）。
     * @param diagrams 页面里的图；每张图一个小节。
     * @param links 页面开头的互链列表，元素是（文件名，说明）。
     * @return 完整的 HTML 文本。
     */
    static std::string htmlPage(
        const std::string &title, const std::string &intro,
        const std::vector<MermaidDiagram> &diagrams,
        const std::vector<std::pair<std::string, std::string>> &links);

    /**
     * @brief 生成要写进产物的 `mermaid/` 文件。
     * @param analysis 分析结果。
     * @param lexSpec 词法扩展配置。
     * @param tokenDefs 固定字面量 Token。
     * @param enabled 是否打开了状态机图示（`%mermaid` / `--mermaid`）。
     * @param out 输出（相对路径，文件内容）列表，顺序固定；开关关闭或无图可画时为空。
     * @param error 出错时写入错误说明。
     * @return 成功返回 true；词法正则编译失败返回 false。
     */
    static bool projectFiles(
        const AnalysisResult &analysis, const LexSpec &lexSpec,
        const std::map<std::string, std::vector<std::string>> &tokenDefs,
        bool enabled, std::vector<std::pair<std::string, std::string>> &out,
        Error &error);

    /**
     * @brief 把若干张图拼成 `.mmd` 源码（多张图之间用 `%% ====` 分隔）。
     * @param diagrams 图的列表。
     * @return 源码文本。
     */
    static std::string joinSources(const std::vector<MermaidDiagram> &diagrams);

    /**
     * @brief 把若干张图按 Markdown 围栏格式打印用文本（`--dump-mermaid` 用）。
     * @param diagrams 图的列表。
     * @return 形如“[教学图示] 标题”加 ```mermaid 围栏的文本。
     */
    static std::string markdownFences(const std::vector<MermaidDiagram> &diagrams);

    /**
     * @brief 转义 Mermaid 标签里的特殊字符（`[` -> `#91;` 等）。
     *
     * 这些 `#nn;` 形式由 Mermaid 自己解码，写进 HTML 时不会和 HTML 实体混淆。
     * @param text 原始文本。
     * @return 可直接放进带引号标签里的文本。
     */
    static std::string escapeLabel(const std::string &text);

private:
    /**
     * @brief 生成词法页面的说明（Token 表 + 共享字符类划分表）。
     * @param lexSpec 词法扩展配置。
     * @param tokenDefs 固定字面量 Token（只用于说明，不画 DFA）。
     * @param error 出错时写入错误说明。
     * @return HTML 片段；正则编译失败时返回空串。
     */
    static std::string lexerIntro(
        const LexSpec &lexSpec,
        const std::map<std::string, std::vector<std::string>> &tokenDefs,
        Error &error);
};

} // namespace ParseEngine

#endif // SCRIPT_ENGINE_MERMAID_HPP
