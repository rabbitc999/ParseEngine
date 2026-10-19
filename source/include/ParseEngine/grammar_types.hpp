/**
 * @file grammar_types.hpp
 * @brief 文法数据模型：有序字典、产生式、变量规格、文法配置。
 */

#ifndef SCRIPT_ENGINE_GRAMMAR_TYPES_HPP
#define SCRIPT_ENGINE_GRAMMAR_TYPES_HPP

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <functional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "ParseEngine/error.hpp"

namespace ParseEngine
{

/// 文本词法器始终提供的基础终结符。
extern const std::vector<std::string> k_defaultTokenNames;

/// error 产生式里那个保留的伪终结符名（yacc 风格的恢复锚点）。
extern const std::string k_errorTokenName;

/// k 的「大得离谱」提示门槛：超过它生成时会提醒一句（不再是硬上限）。
extern const int k_lookaheadWarnThreshold;

/// k 的硬上限：其实只受 `int` 限制（表规模随 k 指数增长，代价自负）。
extern const int k_maxLookaheadK;

/**
 * @brief 字面量里的「空白通配」占位字节（对应文法里的 `\b`）。
 *
 * `"\b"` 表示「一个空白字符」（空格 / `\t` / `\n` / `\v` / `\f` / `\r`）。
 * 字面量是要按字节比对的，没法直接存「任意一个空白」，所以生成期把它解码成
 * 这个控制字节（U+001F，正文里几乎不会出现），词法器匹配到它时改成「吃掉
 * 任意一个空白字符」，见各后端的 `Parser_LexRuleMatch` / `_lex_rule_match`。
 */
constexpr char k_blankWildcardChar = '\x1f';

/**
 * @brief 判断是不是 C 的空白字符（与 "C" locale 下的 isspace 一致）。
 * @param ch 待判断的字节。
 * @return 是空白字符返回 true。
 */
inline bool isBlankChar(char ch)
{
    return (ch == ' ') || (ch == '\t') || (ch == '\n') || (ch == '\v') ||
           (ch == '\f') || (ch == '\r');
}

/**
 * @brief 全部空白字符（`%space` 的缺省集合，也是 `\b` 展开出来的集合）。
 * @return 六个空白字节组成的字符串。
 */
inline const std::string &blankChars()
{
    static const std::string kBlanks = " \t\n\v\f\r";
    return kBlanks;
}

/**
 * @brief 字面量里是否含 `\b` 空白通配。
 * @param literal 解码后的字面量。
 * @return 含通配返回 true（词法器要走通配匹配路径）。
 */
inline bool literalHasBlankWildcard(const std::string &literal)
{
    return literal.find(k_blankWildcardChar) != std::string::npos;
}

/**
 * @brief 字面量能不能在空白字符处命中（首字节是空白字符或 `\b` 通配）。
 *
 * 词法器跳空白时用它判断「要不要先看看固定字面量」：只有首字节是空白 /
 * 通配的字面量才可能在这里命中，其余字面量的首字节比对必然失败。
 * @param literal 解码后的字面量。
 * @return 可能命中返回 true。
 */
inline bool literalCanStartBlank(const std::string &literal)
{
    return (!literal.empty()) && (isBlankChar(literal[0]) ||
                                  (literal[0] == k_blankWildcardChar));
}

/**
 * @brief 条件编译（%if / %elif / %else / %endif）的变量表。
 *
 * 只有命令行 `--define NAME[=VALUE]` 能往里填：变量名 -> 真假。同名后传覆盖
 * 先传。没有「值」的概念，只有真 / 假。
 */
using DefineMap = std::map<std::string, bool>;

/// 可空标记（对应 Python 里的 EPS = ""）。
extern const std::string k_epsilon;

/// `PARSER_MAX_STACK_DEPTH`（分析栈深度上限）的缺省值。
extern const int k_maxStackDepthDefault;

/// `%max_stack_depth` 允许的最大值（栈下标是 uint16_t，不能再大）。
extern const int k_maxStackDepthLimit;

/// `PARSER_MAX_ERRORS`（一次解析最多报几条错）的缺省值。
extern const int k_maxErrorsDefault;

/// `%max_errors` 允许的最大值。
extern const int k_maxErrorsLimit;

/// `PARSER_ERROR_RECOVERY_TOKENS`（错误恢复后静默的 Token 数）的缺省值。
extern const int k_errorRecoveryTokensDefault;

/// `%error_recovery_tokens` 允许的最大值。
extern const int k_errorRecoveryTokensLimit;

/**
 * @brief 一条产生式（对应 Python 里的 prod 字典）。
 */
struct Production
{
    std::vector<std::string> rhs;   ///< 右部符号
    std::string action;             ///< 语义动作原文
    std::string transform;          ///< 左递归消除标记
    int baseRhsLength = 0;          ///< left_base 的原始右部长度
    std::string owner;              ///< left_continue / left_empty 归属
    int id = 0;                     ///< 摊平后的全局产生式编号
    std::string lhs;                ///< 左部非终结符
    std::size_t sourceOffset = 0u;  ///< 该产生式在 .gram 文本里的起始字节偏移（报错定位用）
    std::size_t actionLine = 0u;    ///< 语义动作在**原文件**里的起始行号（#line 用）；0 = 未知
};

/**
 * @brief 文法本体：起始符号 + 有序规则表。
 */
struct Grammar
{
    std::string start;                                        ///< 起始符号
    std::map<std::string, std::vector<Production>> rules;   ///< 规则表
};

/**
 * @brief 优先级与冲突消解配置（SLR / LALR 共用）。
 */
struct PriorityConfig
{
    std::map<std::string, int> precedence;                    ///< Token 优先级
    std::map<std::string, std::string> associativity;         ///< Token 结合性
    std::map<std::pair<std::string, int>, std::string>
        productionPrec;                                         ///< %prec 覆盖
    std::optional<int> expectConflicts;                         ///< %expect
};

/**
 * @brief 一条 %copy 指令：把源文件 / 目录拷进产物目录。
 *
 * kind 为 "file" / "dir"（缺省 "file"）；path 已在加载阶段解析成
 * 相对文法文件目录的路径（loadGrammarText 时保持原样）。
 */
struct CopySpec
{
    std::string kind = "file"; ///< 拷贝类型：file / dir
    std::string path;          ///< 源路径
};

/**
 * @brief 一条 `%commit` 声明：匹配到的文本收进容器，不进语法层。
 *
 * `type` 是容器元素的类型名（`%commit "..."` 缺省 "default"）；`pattern`
 * 是匹配模式，按 DFA 正则编译——没有正则元字符时就是固定字面量。
 * `isSkip` 为 true（`%commit skip "..."`）时匹配即丢，不进容器。
 */
struct CommitSpec
{
    std::string type;      ///< 类型名（`%commit skip` 时是 "skip"）
    std::string pattern;   ///< 匹配模式（DFA 正则；无元字符即固定字面量）
    bool isSkip = false;   ///< 匹配即丢，不进容器
    std::size_t line = 0u; ///< 指令在**原文件**里的行号（报错 / 文档定位用）
    int order = 0;         ///< 词法声明序（同长度时先声明优先）
    std::string alias;
};

/**
 * @brief 词法扩展配置：%token regex 与 %identifier。
 */
struct LexSpec
{
    std::map<std::string, std::string> regex;      ///< Token 名 -> 正则
    std::optional<std::string> identifier;           ///< 标识符正则
    std::vector<CommitSpec> commits;                 ///< %commit（声明序）
    /**
     * @brief Token 名 -> 词法声明序（%define / %token / %token regex 共用）。
     *
     * 只用于 `%commit` 与 Token 规则的最长匹配同长度时比较「谁先声明」。
     */
    std::map<std::string, int> tokenLexOrder;
    std::map<std::string, std::string> tokenAliases;
};

/**
 * @brief %union 里的一个成员。
 *
 * 生成期不解析 C 声明符：`cType` 是成员段落原文（去掉结尾分号、去掉首尾
 * 空白），数组成员带 `[16]`、函数指针成员带参数列表，都原样保留，真正的
 * 声明符解析交给 C 编译器。`name` 是生成期按括号配平规则提取出来的成员名。
 */
struct UnionMember
{
    std::string name;   ///< 成员名（如 ival / point / callback）
    std::string cType;  ///< 成员声明原文（不含结尾分号）
};

/**
 * @brief .gram 解析结果（对应 load_grammar_text 返回的 cfg 字典）。
 */
struct GrammarConfig
{
    std::vector<std::string> tokens;                        ///< 全部终结符
    std::map<std::string, std::vector<std::string>> tokenDefs; ///< 固定字面量
    Grammar grammar;                                        ///< 起始符号与规则
    bool tree = false;                                      ///< %tree
    bool teach = false;                                     ///< %teach（教学模式）
    bool mermaid = false;                                   ///< %mermaid（Mermaid 状态机图示）
    std::optional<int> maxStackDepth;                       ///< %max_stack_depth
    std::optional<int> maxErrors;                           ///< %max_errors
    std::optional<int> errorRecoveryTokens;                 ///< %error_recovery_tokens
    bool usesError = false;                                 ///< 文法里写了 error 产生式
    bool usesSync = false;                                  ///< 文法里写了 %sync
    std::vector<std::string> syncTokens;                    ///< %sync 的 Token（声明序）
    std::string mode = "ielr";                              ///< 生成模式（缺省 IELR(1)）
    bool modeDeclared = false;                              ///< 是否写了 %mode
    /** `%example "..."`：生成文档中展示的、符合本文法的示例输入。 */
    std::optional<std::string> exampleInput;
    /**
     * @brief LR(k) / LALR(k) 的 k（`%lookahead` / `--lookahead`）。
     *
     * 只对 `%mode lrk` / `%mode lalrk` 有意义：k = 1 时这两种模式分别退化
     * 成 LR(1) / LALR(1)，k > 1 时 ACTION 表按 k 个 Token 的展望符建列。
     * 其它模式写了 `%lookahead` 会报错（LL(1) 是自顶向下，不需要 k）。
     */
    int lookahead = 2;
    bool lookaheadDeclared = false;                         ///< 是否写了 %lookahead
    /**
     * @brief `%error_context`：让错误记录里带「出错那一行 + ^ 指示」。
     *
     * 关掉（缺省）时产物里没有 context 字段，省内存；打开时每条错误多
     * `PARSER_ERROR_CONTEXT_LENGTH`（缺省 120）字节，并且只在文本模式下能拿到
     * 源文（Token 数组模式没有原文，context 留空）。
     */
    bool errorContext = false;
    bool errorContextDeclared = false;                      ///< 是否写了 %error_context
    std::string lang = "cpp17";                             ///< 目标语言
    bool langDeclared = false;                              ///< 是否写了 %lang
    /**
     * @brief `%build_files on|off`：是否导出工程构建文件。
     *
     * 打开（缺省）时照旧写 CMakeLists.txt / Makefile / Cargo.toml / *.csproj /
     * pom.xml / package.json / go.mod 等；关闭时只写目标语言源码与 RESULT.md，
     * 构建工程由用户自己维护。命令行 `--build-files` / `--no-build-files` 优先。
     */
    bool buildFiles = true;
    bool buildFilesDeclared = false;                         ///< 是否写了 %build_files
    /// `%cpp_only`：文法是 C++17 专属的，`--lang c` 会被拒绝。
    bool cppOnly = false;
    /**
     * @brief C++17 产物的命名空间名与文件名前缀（`%namespace` / `--namespace`）。
     *
     * 空字符串表示退化成现状：命名空间用 `Parser`、产物文件不带前缀。
     */
    std::string cppNamespace;
    bool cppNamespaceExplicit = false;                      ///< 是否写了 %namespace
    std::string cPrefix;                                    ///< C99 公开符号前缀（%prefix）
    bool cPrefixExplicit = false;                           ///< 是否写了 %prefix
    /**
     * @brief `%cpp_lexer dfa | std | vm`：C++17 产物的词法引擎（`--cpp-lexer`）。
     *
     * `std`（缺省）使用标准库字节正则；`dfa` 只优化等价 ASCII 子集，其余回退；
     * `vm` 使用支持预算的有序运行时。显式 Unicode 规则始终使用共享运行时。
     */
    std::string cppLexer = "std";
    bool cppLexerDeclared = false;                          ///< 是否写了 %cpp_lexer
    /**
     * @brief Python 产物的解析器类名（`%class` / `--class`）。
     *
     * 空字符串 = 用缺省类名 `Parser`；只对 `--lang python` 有意义。
     */
    std::string pythonClass;
    bool pythonClassExplicit = false;                       ///< 是否写了 %class
    /**
     * @brief `%package NAME`：C# 的 `namespace NAME`。
     *
     * 空字符串表示没写。只对 `--lang csharp` 有意义，其它
     * 目标语言下生成期给一句提示并忽略（与 `%namespace` / `%class` 的
     * 「指令不适用」处理保持一致）。
     */
    std::string packageName;
    bool packageDeclared = false;                           ///< 是否写了 %package
    std::size_t packageLine = 0u;                           ///< %package 的原文件行号
    /**
     * @brief `%import NAME`（可重复）：C# 的 `using NAME;`。
     *
     * 按出现顺序保存；只对 `--lang csharp` 有意义，生成 `using NAME;`。
     */
    std::vector<std::string> imports;
    std::vector<std::size_t> importLines;                   ///< 每个 %import 的原文件行号
    /**
     * @brief `%class_body` … `%endbody`：原样插进解析器类内部的用户正文。
     *
     * 与 `%code` … `%endcode` 一样「两个标记各自单独成行、正文原样收集」，
     * 但落点是**解析器类内部**（放辅助方法 / 字段 / 内部类）。只对
     * `--lang csharp` 有意义。`classBodyLine` 是 `%class_body` 标记所在行，
     * 正文从它的下一行开始。
     */
    bool hasClassBody = false;                              ///< 是否写了 %class_body
    std::string classBody;                                  ///< %class_body 正文（原样）
    std::size_t classBodyLine = 0u;                         ///< %class_body 的原文件行号
    std::string userCode;                                   ///< %code 拼接
    std::vector<std::string> userBlocks;                    ///< %code 各块正文（按出现顺序）
    std::vector<std::size_t> userBlockLines;                ///< 每个 %code 的起始行号
    bool hasEpilogue = false;                               ///< 写了 %epilogue
    std::string epilogue;                                   ///< %epilogue 正文（原样）
    std::size_t epilogueLine = 0u;                          ///< %epilogue 标记的原文件行号
    std::vector<std::string> mainBlocks;                    ///< %main 正文（按出现顺序）
    /**
     * @brief 每个 %main 段所在的原文件行号（与 mainBlocks 一一对应）。
     *
     * 「多个 %main 同时生效」的报错发生在产物拼装阶段，那时只剩数据模型，
     * 所以要在这里带上行号，报错才能指回原文件。
     */
    std::vector<std::size_t> mainBlockLines;
    std::size_t unionLine = 0u;                             ///< %union 标记的原文件行号
    /**
     * @brief `%destructor <成员> { 动作 }`：按 `%union` 成员登记清理代码。
     *
     * 键是成员名，值是花括号里的原文（里面用 `$$` 指「正在被丢弃的那个值」）。
     * 错误恢复弹栈、解析失败收尾清栈时，运行时按栈上符号的成员标签调对应片段；
     * 同一个成员只允许一条（bison 也是这个粒度）。
     */
    std::map<std::string, std::string> destructors;
    /// 每个 `%destructor` 的原文件行号（键同 `destructors`），报错定位用。
    std::map<std::string, std::size_t> destructorLines;
    PriorityConfig priority;                                ///< 优先级元数据
    std::map<std::string, std::string> regexDefs;         ///< 正则 Token
    /**
     * @brief 生成器自带的正则编译器没认下的正则（说明 + 原文件行号）。
     *
     * 只有 C99 真的要用自带编译器（`source/src/regex_dfa.cpp`）编出来的 DFA 表，所以
     * C99 看到它们就是**硬错误**（`main.cpp` 在进后端之前报出来，文案与从前
     * 逐字相同）；C++17 / Python / C# / Rust 把正则原样交给各自语言自带的正则
     * 库，只打警告、照常生成。
     */
    std::vector<Error> regexDiagnostics;
    std::optional<std::string> identifierPattern;           ///< %identifier
    std::vector<CommitSpec> commits;                        ///< %commit（声明序）
    /**
     * @brief Token 名 -> 词法声明序（%define / %token / %token regex 共用）。
     *
     * 只用于 `%commit` 与 Token 规则的最长匹配同长度时比较「谁先声明」。
     */
    std::map<std::string, int> tokenLexOrder;
    std::map<std::string, std::string> tokenAliases;
    /**
     * @brief `%space`：取 Token 前跳过的空白字符集。
     *
     * `spaceDeclared` 为 false 时用缺省值（全部空白，`blankChars()`）；
     * 为 true 时 `spaceChars` 是去重后的字符集，空串表示「一个都不跳过」。
     */
    bool spaceDeclared = false;                             ///< 是否写了 %space
    std::string spaceChars;                                 ///< %space 声明的跳过字符集
    std::vector<CopySpec> copies;                           ///< %copy 指令
    /**
     * @brief %value_type 声明的语义值类型；没写时为空。
     *
     * 写了就由生成器补 `typedef <类型> ParserValue_T;`（追加在 %code
     * 用户代码之后），用户不用自己 typedef。
     */
    std::optional<std::string> valueType;
    std::vector<UnionMember> unionMembers;                  ///< %union 成员（声明序）
    bool hasUnion = false;                                  ///< 是否写了 %union
    std::string unionBody;                                  ///< %union 原文（不含最外层花括号）
    std::map<std::string, std::string> symbolValueMember; ///< 符号 -> %union 成员
};

} // namespace ParseEngine

#endif // SCRIPT_ENGINE_GRAMMAR_TYPES_HPP
