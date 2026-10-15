/**
 * parser.hpp —— LALR(1) 解析器公开接口（C++17）
 *
 * 由 ParseEngine 自动生成，请勿手工修改。
 *
 * 目标语言是 C++17：
 *   - 符号收在命名空间 pl0sgram 里，不再有 Parser_Xxx 下划线前缀；
 *   - 字符串 / 容器 / 智能指针直接用标准库（std::string_view / std::string /
 *     std::vector / std::unique_ptr / std::function）；
 *   - 常量用 inline constexpr（C++17 的 inline 变量），不写 #define；
 *   - 内存一律由标准库分配，**不支持自定义分配器**，也没有
 *     PARSER_USER_DISCARD_VALUE 这类释放钩子（析构函数就是释放）。
 *
 * 语义值类型 ParserValue_T 由文法提供（%value_type / %union / %code ... %endcode），
 * 都没有时生成器补 `typedef int ParserValue_T;`。
 */

#ifndef PL0SGRAM_PARSER_HPP
#define PL0SGRAM_PARSER_HPP

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#ifndef PARSER_USER_HEADER_INCLUDED
#define PARSER_USER_HEADER_INCLUDED 1
#include "pl0sgram_parser_user.h"
#endif


namespace pl0sgram
{

/// 解析状态码（数值与 C 目标一致，方便两份产物对照）。
enum class Status : std::int32_t
{
    Ok = 0,           ///< 成功
    ErrStack = -20,   ///< 分析栈 / 语义帧栈溢出
    ErrSyntax = -21,  ///< 语法错误
    ErrLexer = -22,   ///< 词法错误
        ErrDuplicate = -23, ///< 重复定义（保留，与 C 目标一致）
        ErrUndefined = -24, ///< 未定义符号（保留，与 C 目标一致）
        ErrSystem = -25,  ///< 系统错误
        ErrExpected = -26, ///< 期望若干 Token，实际是别的
        ErrUnexpectedEof = -27, ///< 提前遇到输入末尾
        ErrLexUnterminated = -28, ///< 未闭合的字符串 / 注释
        ErrLexChar = -29  ///< 认不出来的字符
};

/**
 * @brief 取状态码的名字（如 `Ok` / `ErrSyntax`）。
 * @param status 状态码。
 * @return 只读名字；未知状态返回 `Unknown`。
 */
const char *statusName(Status status) noexcept;

/// 语义动作里 `$1..$n` 的槽位个数。
inline constexpr std::size_t kValueSlots = 11u;
/// 分析栈最大深度（`%max_stack_depth`）。
inline constexpr std::size_t kMaxStackDepth = 64u;
/// 一次解析最多记录多少条错误（`%max_errors`）。
inline constexpr std::size_t kMaxErrors = 8u;
/// 恢复成功后要连续移进多少个真实 Token 才退出错误状态（`%error_recovery_tokens`）。
inline constexpr std::size_t kErrorRecoveryTokens = 3u;


/// 语义值类型：`%value_type` / `%union` 指定；都没有时是 int。
typedef double ParserValue_T;

/// 词法单元类型（末尾的 TOKEN_END / TOKEN_INVALID 是哨兵）。
enum TokenType : std::uint16_t
{
    TOKEN_STRING, TOKEN_NUMBER, TOKEN_VARIABLE, TOKEN_DECIMAL, TOKEN_PROGRAM, TOKEN_CONST, TOKEN_VAR, TOKEN_ARRAY, TOKEN_STR, TOKEN_STRUCT, TOKEN_REF, TOKEN_PROCEDURE, TOKEN_BEGIN, TOKEN_END, TOKEN_IF, TOKEN_THEN, TOKEN_ELSE, TOKEN_WHILE, TOKEN_DO, TOKEN_FOR, TOKEN_TO, TOKEN_DOWNTO, TOKEN_CALL, TOKEN_WRITE, TOKEN_GOTO, TOKEN_LABEL, TOKEN_AND, TOKEN_OR, TOKEN_NOT, TOKEN_INT, TOKEN_FLOAT, TOKEN_ASSIGN, TOKEN_PLUS, TOKEN_MINUS, TOKEN_STAR, TOKEN_SLASH, TOKEN_MOD, TOKEN_EQ, TOKEN_NE, TOKEN_LT, TOKEN_LE, TOKEN_GT, TOKEN_GE, TOKEN_LPAREN, TOKEN_RPAREN, TOKEN_LBRACK, TOKEN_RBRACK, TOKEN_SEMI, TOKEN_COLON, TOKEN_COMMA, TOKEN_DOT, TOKEN_UMINUS, TOKEN_INVALID
};

/// Token 类型个数（含 TOKEN_INVALID），也是 kTokenNames 的长度。
inline constexpr std::size_t kTokenNameCount = 53u;


/**
 * @brief 输入里的一个位置：行、列、偏移。
 *
 * 行 / 列都是 1 起；`offset` 是从输入开头算起的 **UTF-8 字节偏移**
 * （0 起）。位置指的是当前 lookahead Token 的起点；Token 数组模式
 * （`Engine::run`）下 `offset` 由调用方给的 Token 决定（没填就是 0）。
 */
struct ParserLocation
{
    int line = 0;   ///< 行号（1 起）
    int column = 0; ///< 列号（1 起）
    int offset = 0; ///< UTF-8 字节偏移（0 起）
};

/**
 * @brief 词法单元。
 *
 * 这是个聚合体（public 数据成员）：`%code ... %endcode` 里的
 * `PARSER_USER_TOKEN_VALUE(pToken)` 宏按 C 目标的写法直接读
 * `pToken->type` / `pToken->value` 就能用，不用改用户代码。
 *
 * 三份词素视图放的是同一段文本，按用途选一个：
 *   - `text`：C++17 的 `std::string_view`，不保证 '\0' 结尾，最省事；
 *   - `pName` / `name`：C 目标里的两个字段（NUL 结尾的 `const char *`），
 *     只为兼容按 C 产物写的 `%code ... %endcode` 代码而保留。
 *
 * 它们都指向词法缓冲或输入文本，只在**下一次取词法单元之前**有效；要长期
 * 保存请自己拷成 std::string。
 */
struct Token
{
    TokenType type = TokenType::TOKEN_INVALID; ///< 词法类型
    std::int32_t value = 0;     ///< 数值型 Token 的数值（与 C 产物一致）
    std::string_view text{};     ///< 词素（C++17 视图）
    const char *name = nullptr;  ///< 词素（兼容 C 产物：字符串 / 数字 Token）
    const char *pName = nullptr; ///< 词素（兼容 C 产物：字面量 / 标识符 / 正则）
    std::uint32_t line = 1u;     ///< 行号（1 起）
    std::uint32_t column = 1u;   ///< 列号（1 起）
    std::uint32_t offset = 0u;   ///< UTF-8 字节偏移（0 起）
};

/**
 * @brief 取 Token 类型名（枚举名去掉 `TOKEN_` 之前的部分原样返回）。
 * @param type Token 类型。
 * @return 只读名字；越界返回 nullptr。
 */
const char *tokenName(TokenType type) noexcept;

/**
 * @brief 取 Token 的显示文本：优先词素，没有词素时退回类型名。
 * @param token 词法单元。
 * @return 文本（std::string，调用方持有）。
 */
std::string tokenText(const Token &token);

/**
 * @brief `%commit` 收集到的一条片段：行号 + 类型 + 原文。
 *
 * 匹配到的原文（含字面量本身）按出现顺序收进 `Engine::commits()`；
 * `%commit skip "..."` 命中的片段不进容器。类型名来自文法，缺省 "default"。
 */
struct ParserCommit
{
    int line = 0;                 ///< 片段起始行（1 起）
    std::string type = "default"; ///< %commit 的类型名
    std::string text;             ///< 匹配到的原文（含字面量本身）
};

/**
 * @brief 语法树节点（`%tree` 文法）：名字 + 词素 + 子节点。
 *
 * 父子所有权：父节点拥有子节点（std::unique_ptr），拷贝被禁用——要复制整棵
 * 树请自己写克隆。节点用 std::string 存名字与词素，所以不依赖任何外部缓冲区，
 * 整棵树析构即释放。
 */
class Tree
{
public:
    Tree() = default;
    Tree(const Tree &) = delete;
    Tree &operator=(const Tree &) = delete;
    Tree(Tree &&) = default;
    Tree &operator=(Tree &&) = default;

    /**
     * @brief 建一个节点。
     * @param name 符号名（终结符 / 非终结符）。
     * @param text 终结符词素；非终结符留空。
     * @param location 节点覆盖文本的起点（行 / 列 / UTF-8 字节偏移）。
     */
    explicit Tree(std::string name, std::string text = std::string(),
                  ParserLocation location = ParserLocation());

    /// @return 符号名。
    const std::string &name() const noexcept { return m_name; }
    /// @return 终结符词素（非终结符为空串）。
    const std::string &text() const noexcept { return m_text; }
    /**
     * @return 节点覆盖文本的起点。叶节点是 Token 的起点；分支节点取第一个
     *         有位置的子节点（按需计算，所以先建父节点再填子节点也对）；
     *         都没有时三个字段都是 0。
     */
    const ParserLocation &location() const noexcept;
    /// @brief 设置节点位置（行号 0 表示「未知」）。
    void setLocation(const ParserLocation &location) noexcept
    {
        m_location = location;
    }
    /// @return 子节点个数。
    std::size_t childCount() const noexcept { return m_children.size(); }
    /// @param index 下标。@return 第 index 个子节点；越界返回 nullptr。
    const Tree *child(std::size_t index) const noexcept;
    /// @param index 下标。@return 第 index 个子节点；越界返回 nullptr。
    Tree *child(std::size_t index) noexcept;
    /**
     * @brief 追加一个子节点（所有权转移给本节点）。
     * @param child 子节点。
     */
    void addChild(std::unique_ptr<Tree> child);

    /**
     * @brief 取整棵树的缩进文本（每层两个空格）。
     * @return 文本。
     */
    std::string toString() const;
    /**
     * @brief 打印整棵树。
     * @param stream 输出流；缺省 stdout。
     */
    void print(std::FILE *stream = stdout) const;

private:
    std::string m_name;
    std::string m_text;
    ParserLocation m_location;
    std::vector<std::unique_ptr<Tree>> m_children;
};


/**
 * @brief 解析错误：一次解析可以累积多条（错误恢复时）。
 */
class Error
{
public:
    Error() = default;

    /**
     * @brief 构造一条错误。
     * @param code 错误码。
     * @param line 行号。
     * @param column 列号。
     * @param message 说明。
     */
    Error(std::int32_t code, std::uint32_t line, std::uint32_t column,
          std::string message);

    /**
     * @brief 构造一条「语法错误」：带上实际 Token、期望集合与状态号。
     * @param code 错误码（ErrExpected / ErrUnexpectedEof / ErrSyntax）。
     * @param line 行号。
     * @param column 列号。
     * @param message 说明（形如「期望 SEMI / RBRACE 之一，实际是 PLUS」）。
     * @param tokenType 实际看到的 Token 类型。
     * @param tokenText 出错位置的词素。
     * @param state LR 状态号（LL 家族是栈顶符号编号）。
     * @param expected 期望的 Token 类型列表。
     */
    Error(std::int32_t code, std::uint32_t line, std::uint32_t column,
          std::string message, std::uint32_t tokenType, std::string tokenText,
          std::uint32_t state, std::vector<std::uint32_t> expected,
          std::string context = std::string());

    /// @return 实际看到的 Token 类型（非语法错误时为 0）。
    std::uint32_t tokenType() const noexcept { return m_tokenType; }
    /// @return 出错位置的词素（非语法错误时为空）。
    const std::string &tokenText() const noexcept { return m_tokenText; }
    /// @return LR 状态号（LL 家族是栈顶符号编号；非语法错误时为 0）。
    std::uint32_t state() const noexcept { return m_state; }
    /// @return 该位置期望的 Token 类型列表（可能被上限截断）。
    const std::vector<std::uint32_t> &expected() const noexcept
    {
        return m_expected;
    }
    /// @return 出错那一行的原文 + 一行 `^` 指示（文本模式才有；否则为空）。
    const std::string &context() const noexcept { return m_context; }

    /// @return 错误码（Status 的数值形式）。
    std::int32_t code() const noexcept { return m_code; }
    /// @return 行号。
    std::uint32_t line() const noexcept { return m_line; }
    /// @return 列号。
    std::uint32_t column() const noexcept { return m_column; }
    /// @return 说明文本。
    const std::string &message() const noexcept { return m_message; }

    /**
     * @brief 拼成一行诊断文本。
     * @return 形如 `第 3 行第 5 列：语法错误：期望 SEMI / RBRACE 之一，实际是 PLUS`。
     */
    std::string toString() const;

private:
    std::int32_t m_code = 0;
    std::uint32_t m_line = 0u;
    std::uint32_t m_column = 0u;
    std::string m_message;
    std::uint32_t m_tokenType = 0u;
    std::string m_tokenText;
    std::uint32_t m_state = 0u;
    std::vector<std::uint32_t> m_expected;
    std::string m_context;
  };

/**
 * @brief 解析引擎：一次解析的全部状态都在这里（Qt 风格的私有 d-pointer）。
 *
 * 引擎不可拷贝；同一个 Engine 可以连续解析多次，每次解析会先清空上一次的
 * 错误列表与结果。
 */
class Engine
{
public:
    Engine();
    ~Engine();

    Engine(const Engine &) = delete;
    Engine &operator=(const Engine &) = delete;

    /**
     * @brief 解析一段文本。
     * @param text 输入文本。
     * @param result 输出语义值（成功时写入）；可为 nullptr。
     * @return Status::Ok 表示成功。
     */
    Status runText(std::string_view text, ParserValue_T *result);

    /**
     * @brief 解析已经切好的 Token 序列。
     * @param tokens Token 序列。
     * @param result 输出语义值（成功时写入）；可为 nullptr。
     * @return Status::Ok 表示成功。
     */
    Status run(const std::vector<Token> &tokens, ParserValue_T *result);

    /// @return 本次解析累积的错误（一次解析可以报多条）。
    const std::vector<Error> &errors() const noexcept;
    /// @return 最近一条错误；没有错误时返回 nullptr。
    const Error *lastError() const noexcept;

    /**
     * @brief 当前 lookahead 的位置（行 / 列 1 起，offset 是 UTF-8 字节偏移）。
     *
     * 语义动作里的 `$line` / `$column` 读的就是这里的值；Token 数组模式
     * （`run`）下偏移由调用方给的 Token 决定。
     * @return 位置。
     */
    ParserLocation currentLocation() const noexcept;

    /**
     * @brief 本次解析收集到的 `%commit` 片段（按出现顺序）。
     * @return 片段列表；文法里没写 %commit 时恒为空。
     */
    const std::vector<ParserCommit> &commits() const noexcept;


private:
    class Private;
    std::unique_ptr<Private> d;
};

/* ============ 兼容层：C 产物 API 的同名入口（给 %main / %epilogue 用） ============ */

/**
 * @brief 兼容层：`%main` / `%epilogue` 里照 C 产物的名字调生成器函数。
 *
 * C++17 产物本身是 `Engine`（Qt 风格 d-pointer），但同一份文法里的
 * `%main` / `%epilogue` 常常是照 C 产物写的（`Parser_RunText` / `PARSER_OK`）。
 * 这一段把那些名字原样提供出来，底下包着一个进程内共享的 Engine：
 *   - `ParserValue_T` 就是上面的语义值类型，写法与 C 一致；
 *   - `Parser_RunText` / `Parser_Run` 用共享引擎解析，**非线程安全**；
 *   - `Parser_GetLastError` / `Parser_GetErrorCount` / `Parser_GetError`
 *     读共享引擎上一次解析的错误，结构与 C 产物的 `ParserError_T` 一致。
 *
 * 新代码建议直接用 `Engine`（`runText` / `run` / `errors` / `lastError`），
 * 这一段只是让已有文法不用改就能编。
 */

/// 解析成功。
inline constexpr std::int32_t PARSER_OK = 0;
/// 分析栈 / 语义帧栈溢出。
inline constexpr std::int32_t PARSER_ERR_STACK = -20;
/// 语法错误。
inline constexpr std::int32_t PARSER_ERR_SYNTAX = -21;
/// 词法错误。
inline constexpr std::int32_t PARSER_ERR_LEXER = -22;
/// 重复定义（保留，与 C 产物一致）。
inline constexpr std::int32_t PARSER_ERR_DUPLICATE = -23;
/// 未定义符号（保留，与 C 产物一致）。
inline constexpr std::int32_t PARSER_ERR_UNDEFINED = -24;
/// 系统错误。
inline constexpr std::int32_t PARSER_ERR_SYSTEM = -25;

/// 分析栈最大深度（%max_stack_depth），与 C 产物的宏同名。
inline constexpr std::size_t PARSER_MAX_STACK_DEPTH = kMaxStackDepth;
/// 一次解析最多记多少条错误（%max_errors）。
inline constexpr std::size_t PARSER_MAX_ERRORS = kMaxErrors;
/// 恢复成功后要连续移进多少个真实 Token 才退出错误状态。
inline constexpr std::size_t PARSER_ERROR_RECOVERY_TOKENS =
    kErrorRecoveryTokens;

/// 兼容 C 产物的错误结构（字段名与 `ParserError_T` 一致）。
struct ParserError_T
{
    std::int32_t code = 0;         ///< 错误码
    std::uint32_t line = 0u;       ///< 行号
    std::uint32_t column = 0u;     ///< 列号
    const char *message = nullptr; ///< 说明（指向共享引擎里的错误文本）
};

/// @return 兼容入口共用的引擎（**非线程安全**）。
inline Engine &Parser_CompatEngine()
{
    static Engine engine;
    return engine;
}

/**
 * @brief 解析文本（兼容 C 的 `Parser_RunText`）。
 * @param pText 输入文本；nullptr 当空串。
 * @param pResult 输出语义值；可为 nullptr。
 * @return PARSER_OK 表示成功。
 */
inline std::int32_t Parser_RunText(const char *pText, ParserValue_T *pResult)
{
    const std::string_view text =
        (pText != nullptr) ? std::string_view(pText) : std::string_view();
    return static_cast<std::int32_t>(Parser_CompatEngine().runText(text, pResult));
}

/**
 * @brief 解析 Token 数组（兼容 C 的 `Parser_Run`）。
 * @param tokens Token 数组。
 * @param pResult 输出语义值；可为 nullptr。
 * @return PARSER_OK 表示成功。
 */
inline std::int32_t Parser_Run(const std::vector<Token> &tokens,
                              ParserValue_T *pResult)
{
    return static_cast<std::int32_t>(Parser_CompatEngine().run(tokens, pResult));
}

/**
 * @brief 取最近一条错误（兼容 C 的 `Parser_GetLastError`）。
 *
 * 没有错误时返回一条全零记录（与 C 产物一致，不会返回 nullptr）。返回的
 * 结构由本函数持有，下一次解析后内容会被覆盖。
 * @return 只读错误结构。
 */
inline const ParserError_T *Parser_GetLastError()
{
    static ParserError_T entry;
    const Error *error = Parser_CompatEngine().lastError();
    entry = ParserError_T();
    if (error != nullptr)
    {
        entry.code = error->code();
        entry.line = error->line();
        entry.column = error->column();
        entry.message = error->message().c_str();
    }
    return &entry;
}

/// @return 本次解析累积的错误条数（兼容 C 的 `Parser_GetErrorCount`）。
inline std::uint32_t Parser_GetErrorCount()
{
    return static_cast<std::uint32_t>(Parser_CompatEngine().errors().size());
}

/**
 * @brief 取第 index 条错误（兼容 C 的 `Parser_GetError`）。
 * @param index 下标（0 起）。
 * @return 只读错误结构；越界返回 nullptr。
 */
inline const ParserError_T *Parser_GetError(std::uint32_t index)
{
    static ParserError_T entry;
    const std::vector<Error> &errors = Parser_CompatEngine().errors();
    if (index >= errors.size())
    {
        return nullptr;
    }
    entry.code = errors[index].code();
    entry.line = errors[index].line();
    entry.column = errors[index].column();
    entry.message = errors[index].message().c_str();
    return &entry;
}

} // namespace pl0sgram

#endif /* PL0SGRAM_PARSER_HPP */
