/**
 * @file project.hpp
 * @brief 产物工程组装：写出 parser.c / parser.h / Makefile / RESULT.md 等文件。
 */

#ifndef SCRIPT_ENGINE_PROJECT_HPP
#define SCRIPT_ENGINE_PROJECT_HPP

#include <set>
#include <string>
#include <vector>

#include "ParseEngine/analysis.hpp"
#include "ParseEngine/grammar_types.hpp"

namespace ParseEngine
{

/// 产物清单（自定义语义值运行时；写 %code / %union 时多一个 parser_user.h）。
extern const std::vector<std::string> k_projectFiles;

/**
 * @brief generateParserProject 的入参集合（替代 Python 的关键字参数）。
 */
struct ProjectRequest
{
    /**
     * @brief #line 指令需要的行号信息（文法路径 + 各段在 .gram 里的起始行）。
     */
    struct LineInfo
    {
        std::string grammarPath;                  ///< --grammar-file 的路径（保持原样）
        std::size_t unionLine = 0u;               ///< %union 那一行
        std::size_t epilogueLine = 0u;            ///< %epilogue 那一行
        const std::vector<std::size_t> *userBlockLines = nullptr; ///< %code 各行
        const std::vector<std::size_t> *mainLines = nullptr;      ///< %main 各行
        const std::vector<std::string> *userBlocks = nullptr;     ///< %code 各块正文
    };

    const Grammar *grammar = nullptr;                  ///< 文法；必填
    const std::vector<std::string> *tokens = nullptr;  ///< 终结符列表；必填
    const std::map<std::string, std::vector<std::string>> *tokenDefs =
        nullptr;                                ///< 固定字面量 Token
    bool strictRegex = false;                   ///< 拒绝未证明与共享规范等价的原生正则。
    bool tree = false;                          ///< 是否 %tree
    bool teach = false;                         ///< 是否教学模式（%teach / --teach）
    bool mermaid = false;                       ///< 是否生成 mermaid/ 状态机图示
    bool usesError = false;                     ///< 文法里写了 error 产生式
    /// `%error_context`：错误记录里带「出错那一行 + ^ 指示」（费内存，默认关）
    bool errorContext = false;
    /**
     * @brief `%destructor <成员> { ... }`：成员名 -> 清理片段（可空）。
     *
     * 产物层按「栈上符号的统一编号 -> 成员」查表，在丢弃语义值时调对应片段。
     * 没写 `%destructor` 时为空，行为与从前完全一样。
     */
    const std::map<std::string, std::string> *destructors = nullptr;
    const std::vector<std::string> *syncTokens = nullptr; ///< %sync 的 Token（声明序）
    bool hasEpilogue = false;                   ///< 文法里写了 %epilogue
    const std::string *epilogue = nullptr;       ///< %epilogue 正文
    /**
     * @brief `%space` 声明的「取 Token 前跳过的空白字符集」。
     *
     * nullptr 表示文法里没写 `%space`（缺省跳过全部空白）；非空指针指向的
     * 字符串为空表示「一个空白都不跳过」。
     */
    const std::string *spaceChars = nullptr;
    const LineInfo *lines = nullptr;             ///< #line 的行号信息（没写就不插）
    const std::vector<std::string> *mainBlocks = nullptr; ///< %main 段（按出现顺序）
    const std::vector<std::size_t> *mainBlockLines = nullptr; ///< 每个 %main 的行号
    std::optional<int> maxStackDepth;           ///< %max_stack_depth（空 = 缺省 64）
    std::optional<int> maxErrors;               ///< %max_errors（空 = 缺省 8）
    std::optional<int> errorRecoveryTokens;     ///< %error_recovery_tokens（空 = 缺省 3）
    std::string mode = "ielr";                   ///< ll1 / slr / lalr
    int lookaheadK = 1;                          ///< LR(k) / LALR(k) 的 k（只对 lrk / lalrk）
    std::string lang = "cpp17";                 ///< 目标语言：c / cpp17
    /// `%example "..."`；未声明时生成文档显示通用占位输入。
    std::optional<std::string> exampleInput;
    /**
     * @brief C++17 产物的命名空间名与文件名前缀（`%namespace` / `--namespace`）。
     *
     * 空字符串 = 退化成现状：命名空间 `Parser`、产物文件名不带前缀。
     */
    std::string cppNamespace;
    std::string cPrefix;                          ///< C99 公开符号前缀（%prefix / --prefix）
    /**
     * @brief C++17 产物的词法引擎（`%cpp_lexer` / `--cpp-lexer`）。
     *
     * `std`（缺省）使用标准库字节正则；`dfa` 仅优化已证明等价的 ASCII 子集；
     * `vm` 使用支持预算的有序运行时。显式 Unicode 规则始终使用共享运行时。
     * 只对 `--lang cpp17` 有意义。
     */
    std::string cppLexer = "std";
    /**
     * @brief Python 产物的解析器类名（`%class` / `--class`）。
     *
     * 空字符串 = 用缺省类名 `Parser`；只对 `--lang python` 有意义。
     */
    std::string pythonClass;
    /**
     * @brief `%package NAME`：C# 的 `namespace NAME`。
     *
     * 空字符串 = 没写；只对 `--lang csharp` 有意义。
     */
    std::string packageName;
    const std::vector<std::string> *imports = nullptr; ///< `%import` 的名字（按出现顺序）
    /**
     * @brief `%class_body` … `%endbody`：原样插进解析器类内部的用户正文。
     *
     * nullptr / 空串表示没写；只对 `--lang csharp` 有意义。
     */
    const std::string *classBody = nullptr;
    /// `%class_body` 标记所在的原文件行号（正文从它的下一行开始）。
    std::size_t classBodyLine = 0u;
    std::string sourceName;                     ///< 文法文件名
    std::string reportText;                     ///< 分析报告文本
    /**
     * @brief RESULT.md 是否带上分析报告全文（命令行 `--full-report`）。
     *
     * 缺省 false = 简化版报告：写到「未消解冲突」就收尾，不带末尾那一节
     * `## 文法分析报告`（正文与 `{{ report }}` 一致，随时可用 `--report FILE`
     * 单独导出）。true 时那一节照旧输出——C99 / C++17 / Python 走模板里的
     * `{% if full_report %}`，C# / Rust / Go 由 appendFullReport 补上。
     * `--lang gram` 导出的 `<文法名>.analysis.md` 本身就是报告，不受影响。
     */
    bool fullReport = false;
    const PriorityConfig *priority = nullptr;   ///< 优先级配置
    const LexSpec *lexSpec = nullptr;           ///< 词法扩展配置
    std::string userCode;                       ///< %code 拼接内容
    /**
     * @brief 文法的原始文本（只给 `--lang gram` 导出原文件用）。
     *
     * 就是 `--grammar-file` / 位置参数那份文件的原始内容（`%if` 还没展开、
     * 注释与空行都在），没给文件时是内建的 `default_grammar.gram`。
     * nullptr 表示调用方没提供，`--lang gram` 会直接报错（别的目标语言不用
     * 这个字段）。
     */
    const std::string *grammarText = nullptr;
    const std::string *valueType = nullptr;     ///< %value_type；没写时为空
    const std::string *unionBody = nullptr;     ///< %union body；没写 %union 时为空
    const std::vector<UnionMember> *unionMembers = nullptr; ///< %union 成员（声明序）
    const std::map<std::string, std::string> *symbolValueMember = nullptr; ///< 符号 -> %union 成员
    const AnalysisResult *analysis = nullptr;   ///< 已算好的分析结果；可为空
    const std::vector<CopySpec> *copies = nullptr; ///< %copy 指令；可为空
};

/**
 * @brief 分文件工程组装器。
 */
class Project
{
public:
    /**
     * @brief 生成分文件工程（返回顺序与产物清单一致）。
     * @param request 组装入参。
     * @param out 输出文件名到文件内容的映射，顺序与产物清单一致。
     * @param error 出错时写入错误说明。
     * @return 成功返回 true。
     */
    static bool generateParserProject(
        const ProjectRequest &request,
        std::map<std::string, std::string> &out, Error &error);

    /**
     * @brief 把产物写进 outDir（UTF-8，自动建目录）。
     * @param outDir 输出目录。
     * @param project 文件名到文件内容的映射。
     * @param written 输出按写出顺序排列的文件路径列表。
     * @param error 出错时写入错误说明。
     * @param preserveBuildFiles 为 true 时不把构建工程文件（Makefile /
     *        CMakeLists.txt / Cargo.toml / *.csproj / pom.xml / package.json /
     *        go.mod 等）当作过期产物删除——`--no-build-files` 时用户自己写的那份
     *        必须留着；同时也意味着它们不再登记进产物清单。
     * @return 成功返回 true。
     */
    static bool writeParserProject(
        const std::string &outDir,
        const std::map<std::string, std::string> &project,
        std::vector<std::string> &written, Error &error,
        bool preserveBuildFiles = false);

    /**
     * @brief 判断产物清单里的一项是不是「工程构建文件」。
     *
     * `--no-build-files`（`%build_files off`）时这些文件不写出、也不算过期产物，
     * 用户自己写的那份同名文件必须保留。只看 basename。
     * @param relative 产物清单里的相对路径（用 `/` 分隔）。
     * @return 是构建工程文件返回 true。
     */
    static bool isBuildFileName(const std::string &relative);

    /**
     * @brief 删除 outDir 中上一次由 ParseEngine 登记的生成文件。
     *
     * 未登记的用户文件始终保留；清理后顺便删除因此变空的生成子目录。
     * @param outDir 输出目录。
     * @param removed 实际删除的生成文件数量（不含清单自身）。
     * @param error 出错时写入错误说明。
     * @return 成功返回 true；没有清单也算成功。
     */
    static bool clearParserProject(
        const std::string &outDir, std::size_t &removed, Error &error);
};

} // namespace ParseEngine

#endif // SCRIPT_ENGINE_PROJECT_HPP
