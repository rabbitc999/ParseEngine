/**
 * parser.cpp —— LALR(1) 解析器实现（C++17）
 *
 * 由 Script-Engine 自动生成，请勿手工修改。
 *
 * 文件布局：
 *   1) 未命名命名空间：生成期算好的分析表 / 词法 DFA / 树与动作辅助函数；
 *   2) Engine::Private ：词法器与分析驱动（Qt 风格 d-pointer）；
 *   3) 公开 API 定义   ：statusName / tokenName / tokenText / Tree / Engine。
 *
 * 内存全部由标准库管理（std::string / std::string_view / std::vector /
 * std::unique_ptr），不使用自定义分配器。
 */

#include "pl0_parser.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pl0
{

namespace
{
/* ============ 生成期算好的表：词法 DFA / ACTION-GOTO / 预测分析表 ============ */

/// Token 名称表：下标就是 TokenType 的值。
inline constexpr const char *const kTokenNames[] =
{
    "STRING",
    "NUMBER",
    "VARIABLE",
    "CONST",
    "VAR",
    "BEGIN",
    "END",
    "IF",
    "THEN",
    "WHILE",
    "DO",
    "READ",
    "WRITE",
    "ODD",
    "ASSIGN",
    "EQ",
    "NEQ",
    "LE",
    "GE",
    "LT",
    "GT",
    "PLUS",
    "MINUS",
    "STAR",
    "SLASH",
    "LPAREN",
    "RPAREN",
    "COMMA",
    "SEMI",
    "DOT",
    "INVALID"
};

/* ---------- 词法表 ---------- */
/// DFA 描述：转移表按「状态 × 字符类」展平（%token regex / %identifier 生成）。
struct LexDfa
{
    const std::uint16_t *trans; ///< 转移表
    const std::uint8_t *accept; ///< 接受标记
    std::uint16_t stateCount;   ///< 状态个数
    std::uint16_t startState;   ///< 起始状态
    std::uint16_t trapState;    ///< 陷阱状态
};

/// 固定字面量词法规则（按长度降序；表尾是一个哨兵）。
struct LexRule
{
    const char *text;     ///< 字面量（UTF-8；\b 通配写成 0x1F）
    std::uint32_t length; ///< 字节长度
    TokenType type;       ///< 命中的 Token 类型
    bool blankFirst;      ///< 首字节是空白 / \b：跳空白时先看它
    bool wildcard;        ///< 含 \b 通配：命中后词素按输入抄一份
};

inline constexpr LexRule kLexRules[] =
{
    { "const", 5u, TOKEN_CONST, false, false },
    { "begin", 5u, TOKEN_BEGIN, false, false },
    { "while", 5u, TOKEN_WHILE, false, false },
    { "write", 5u, TOKEN_WRITE, false, false },
    { "then", 4u, TOKEN_THEN, false, false },
    { "read", 4u, TOKEN_READ, false, false },
    { "var", 3u, TOKEN_VAR, false, false },
    { "end", 3u, TOKEN_END, false, false },
    { "odd", 3u, TOKEN_ODD, false, false },
    { "if", 2u, TOKEN_IF, false, false },
    { "do", 2u, TOKEN_DO, false, false },
    { ":=", 2u, TOKEN_ASSIGN, false, false },
    { "<>", 2u, TOKEN_NEQ, false, false },
    { "<=", 2u, TOKEN_LE, false, false },
    { ">=", 2u, TOKEN_GE, false, false },
    { "=", 1u, TOKEN_EQ, false, false },
    { "<", 1u, TOKEN_LT, false, false },
    { ">", 1u, TOKEN_GT, false, false },
    { "+", 1u, TOKEN_PLUS, false, false },
    { "-", 1u, TOKEN_MINUS, false, false },
    { "*", 1u, TOKEN_STAR, false, false },
    { "/", 1u, TOKEN_SLASH, false, false },
    { "(", 1u, TOKEN_LPAREN, false, false },
    { ")", 1u, TOKEN_RPAREN, false, false },
    { ",", 1u, TOKEN_COMMA, false, false },
    { ";", 1u, TOKEN_SEMI, false, false },
    { ".", 1u, TOKEN_DOT, false, false },
    { nullptr, 0u, TokenType::TOKEN_END, false, false }
};

/// 关键字表项。
struct Keyword
{
    const char *text; ///< 字面量
    TokenType type;   ///< 对应的 Token
};

/// 关键字表：%define / %token 里像标识符的字面量。
inline constexpr Keyword kKeywords[] =
{
    { "const", TOKEN_CONST },
    { "var", TOKEN_VAR },
    { "begin", TOKEN_BEGIN },
    { "end", TOKEN_END },
    { "if", TOKEN_IF },
    { "then", TOKEN_THEN },
    { "while", TOKEN_WHILE },
    { "do", TOKEN_DO },
    { "read", TOKEN_READ },
    { "write", TOKEN_WRITE },
    { "odd", TOKEN_ODD }
};

/**
 * @brief 关键字查表。
 * @param pText 文本。
 * @return 关键字对应的 Token；不是关键字时返回 TOKEN_INVALID。
 */
inline TokenType keywordType(const char *pText)
{
    if (pText == nullptr)
    {
        return TokenType::TOKEN_INVALID;
    }
    for (const Keyword &entry : kKeywords)
    {
        if (std::strcmp(pText, entry.text) == 0)
        {
            return entry.type;
        }
    }
    return TokenType::TOKEN_INVALID;
}

/* 字符类划分：共 3 类（256 个字节各归一类） */
inline constexpr std::uint8_t kLexClassMap[256] =
{
    0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u,
    0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u,
    0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u,
    1u, 1u, 1u, 1u, 1u, 1u, 1u, 1u, 1u, 1u, 0u, 0u, 0u, 0u, 0u, 0u,
    0u, 2u, 2u, 2u, 2u, 2u, 2u, 2u, 2u, 2u, 2u, 2u, 2u, 2u, 2u, 2u,
    2u, 2u, 2u, 2u, 2u, 2u, 2u, 2u, 2u, 2u, 2u, 0u, 0u, 0u, 0u, 2u,
    0u, 2u, 2u, 2u, 2u, 2u, 2u, 2u, 2u, 2u, 2u, 2u, 2u, 2u, 2u, 2u,
    2u, 2u, 2u, 2u, 2u, 2u, 2u, 2u, 2u, 2u, 2u, 0u, 0u, 0u, 0u, 0u,
    0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u,
    0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u,
    0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u,
    0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u,
    0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u,
    0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u,
    0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u,
    0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u
};
inline constexpr std::uint16_t kLexClassCount = 3u;

/* 标识符：[A-Za-z_][A-Za-z0-9_]*（3 状态 × 3 字符类） */
inline constexpr std::uint16_t kIdentifierDfaTrans[3][3] =
{
    { 1u, 1u, 2u },
    { 1u, 1u, 1u },
    { 1u, 2u, 2u }
};
inline constexpr std::uint8_t kIdentifierDfaAccept[3] = { 0u, 0u, 1u };

/* %identifier 声明的标识符 DFA */
inline constexpr LexDfa kIdentifierDfa =
    { &kIdentifierDfaTrans[0][0], kIdentifierDfaAccept, 3u, 0u, 1u };

/**
 * @brief 用 DFA 做最长前缀匹配。
 * @param pDfa DFA。
 * @param pText 文本。
 * @return 匹配到的字节数（0 表示不匹配）。
 */
inline std::uint32_t lexDfaMatch(const LexDfa *pDfa, const char *pText)
{
    if ((pDfa == nullptr) || (pText == nullptr))
    {
        return 0u;
    }
    std::uint32_t length = 0u;
    std::uint32_t best = 0u;
    std::uint16_t state = pDfa->startState;
    while (pText[length] != '\0')
    {
        const std::uint8_t cls =
            kLexClassMap[static_cast<unsigned char>(pText[length])];
        state = pDfa->trans[static_cast<std::uint32_t>(state) *
                                static_cast<std::uint32_t>(
                                    kLexClassCount) +
                            cls];
        if (state == pDfa->trapState)
        {
            break;
        }
        ++length;
        if (pDfa->accept[state] != 0u)
        {
            best = length;
        }
    }
    return best;
}

/**
 * @brief 取最长标识符前缀长度。
 * @param pText 文本。
 * @return 长度（0 表示不是标识符开头）。
 */
inline std::uint32_t identifierLength(const char *pText)
{
    if (pText == nullptr)
    {
        return 0u;
    }
    return lexDfaMatch(&kIdentifierDfa, pText);
}


/* ---------- 分析表 ---------- */
/* ACTION：0 出错，>0 移进到 action-1，<0 归约 -action-1，INT32_MAX 接受；GOTO：0xFFFF 出错。 */
inline constexpr std::int32_t kAction[81][30] =
{
    { 0, 0, -5, 4, -5, -5, 0, -5, 0, -5, 0, -5, -5, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, -5 },
    { 0, 0, 0, 0, 0, 0, INT32_MAX, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, -10, 0, 7, -10, 0, -10, 0, -10, 0, -10, -10, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, -10 },
    { 0, 0, 8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 11 },
    { 0, 0, 12, 0, 0, 14, 0, 15, 0, 24, 0, 17, 25, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, -14 },
    { 0, 0, 26, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 28, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, -6, -6, 0 },
    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 29, 30, 0 },
    { 0, 0, 0, 0, 0, 0, -2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 31, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, -3 },
    { 0, 0, 12, 0, 0, 14, 0, 15, 0, 24, 0, 17, 25, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 0, 38, 37, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 34, 0, 0, 0, 0, 0, 0, 0, 41, 40, 0, 0, 39, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, -13 },
    { 0, 0, 44, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, -15, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, -15, -15 },
    { 0, 0, 0, 0, 0, 0, -16, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, -16, -16 },
    { 0, 0, 0, 0, 0, 0, -17, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, -17, -17 },
    { 0, 0, 0, 0, 0, 0, -18, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, -18, -18 },
    { 0, 0, 0, 0, 0, 0, -19, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, -19, -19 },
    { 0, 0, 0, 0, 0, 0, -20, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, -20, -20 },
    { 0, -27, -27, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, -27, 0, 0, 0, 0, 0, 0, 0, -27, -27, 0, 0, -27, 0, 0, 0, 0 },
    { 0, 38, 37, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 41, 40, 0, 0, 39, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, -11, -11, 0 },
    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 47, 48, 0 },
    { 0, 49, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, -4, 0, -4, -4, 0, -4, 0, -4, 0, -4, -4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, -4 },
    { 0, 38, 37, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 41, 40, 0, 0, 39, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 52, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 53, 0 },
    { 0, 0, 0, 0, 0, 0, -23, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, -23, 0 },
    { 0, 38, 37, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 41, 40, 0, 0, 39, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 58, 59, 61, 63, 60, 62, 56, 57, 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, -38, 0, -38, 0, -38, 0, 0, 0, 0, -38, -38, -38, -38, -38, -38, -38, -38, 64, 65, 0, -38, 0, -38, -38 },
    { 0, 0, 0, 0, 0, 0, -44, 0, -44, 0, -44, 0, 0, 0, 0, -44, -44, -44, -44, -44, -44, -44, -44, -44, -44, 0, -44, 0, -44, -44 },
    { 0, 0, 0, 0, 0, 0, -45, 0, -45, 0, -45, 0, 0, 0, 0, -45, -45, -45, -45, -45, -45, -45, -45, -45, -45, 0, -45, 0, -45, -45 },
    { 0, 38, 37, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 41, 40, 0, 0, 39, 0, 0, 0, 0 },
    { 0, 38, 37, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 41, 40, 0, 0, 39, 0, 0, 0, 0 },
    { 0, 38, 37, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 41, 40, 0, 0, 39, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0, 0, 69, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, -41, 0, -41, 0, -41, 0, 0, 0, 0, -41, -41, -41, -41, -41, -41, -41, -41, -41, -41, 0, -41, 0, -41, -41 },
    { 0, 0, 0, 0, 0, 0, -28, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, -28, -28 },
    { 0, 38, 37, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 34, 0, 0, 0, 0, 0, 0, 0, 41, 40, 0, 0, 39, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, -29, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 56, 57, 0, 0, 0, 0, 0, -29, -29 },
    { 0, 0, 71, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, -9, 0, 0, -9, 0, -9, 0, -9, 0, -9, -9, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, -9 },
    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, -8, -8, 0 },
    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, -7, -7, 0 },
    { 0, 0, 0, 0, 0, 0, -21, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 56, 57, 0, 0, 0, 0, 0, -21, -21 },
    { 0, 0, 0, 0, 0, 0, -22, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, -22, -22 },
    { 0, 0, 12, 0, 0, 14, 0, 15, 0, 24, 0, 17, 25, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0, 0, -30, 0, -30, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 56, 57, 0, 0, 0, 0, 0, 0, 0 },
    { 0, 38, 37, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 41, 40, 0, 0, 39, 0, 0, 0, 0 },
    { 0, 38, 37, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 41, 40, 0, 0, 39, 0, 0, 0, 0 },
    { 0, 38, 37, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 41, 40, 0, 0, 39, 0, 0, 0, 0 },
    { 0, -32, -32, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, -32, -32, 0, 0, -32, 0, 0, 0, 0 },
    { 0, -33, -33, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, -33, -33, 0, 0, -33, 0, 0, 0, 0 },
    { 0, -34, -34, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, -34, -34, 0, 0, -34, 0, 0, 0, 0 },
    { 0, -35, -35, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, -35, -35, 0, 0, -35, 0, 0, 0, 0 },
    { 0, -36, -36, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, -36, -36, 0, 0, -36, 0, 0, 0, 0 },
    { 0, -37, -37, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, -37, -37, 0, 0, -37, 0, 0, 0, 0 },
    { 0, 38, 37, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 41, 40, 0, 0, 39, 0, 0, 0, 0 },
    { 0, 38, 37, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 41, 40, 0, 0, 39, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 56, 57, 0, 0, 0, 78, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, -47, 0, -47, 0, -47, 0, 0, 0, 0, -47, -47, -47, -47, -47, -47, -47, -47, -47, -47, 0, -47, 0, -47, -47 },
    { 0, 0, 0, 0, 0, 0, -48, 0, -48, 0, -48, 0, 0, 0, 0, -48, -48, -48, -48, -48, -48, -48, -48, -48, -48, 0, -48, 0, -48, -48 },
    { 0, 0, 12, 0, 0, 14, 0, 15, 0, 24, 0, 17, 25, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, -12, -12, 0 },
    { 0, 0, 0, 0, 0, 0, -24, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, -24, 0 },
    { 0, 0, 0, 0, 0, 0, 0, 0, -31, 0, -31, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 56, 57, 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, -39, 0, -39, 0, -39, 0, 0, 0, 0, -39, -39, -39, -39, -39, -39, -39, -39, 64, 65, 0, -39, 0, -39, -39 },
    { 0, 0, 0, 0, 0, 0, -40, 0, -40, 0, -40, 0, 0, 0, 0, -40, -40, -40, -40, -40, -40, -40, -40, 64, 65, 0, -40, 0, -40, -40 },
    { 0, 0, 0, 0, 0, 0, -42, 0, -42, 0, -42, 0, 0, 0, 0, -42, -42, -42, -42, -42, -42, -42, -42, -42, -42, 0, -42, 0, -42, -42 },
    { 0, 0, 0, 0, 0, 0, -43, 0, -43, 0, -43, 0, 0, 0, 0, -43, -43, -43, -43, -43, -43, -43, -43, -43, -43, 0, -43, 0, -43, -43 },
    { 0, 0, 0, 0, 0, 0, -46, 0, -46, 0, -46, 0, 0, 0, 0, -46, -46, -46, -46, -46, -46, -46, -46, -46, -46, 0, -46, 0, -46, -46 },
    { 0, 0, 0, 0, 0, 0, -25, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, -25, -25 },
    { 0, 0, 12, 0, 0, 14, 0, 15, 0, 24, 0, 17, 25, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, -26, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, -26, -26 }
};
inline constexpr std::uint16_t kGoto[81][23] =
{
    { 0xFFFFu, 1, 4, 2, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu },
    { 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu },
    { 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 5, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu },
    { 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 9, 8, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu },
    { 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu },
    { 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 12, 15, 17, 18, 0xFFFFu, 19, 20, 0xFFFFu, 21, 22, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu },
    { 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 26, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu },
    { 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu },
    { 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu },
    { 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu },
    { 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu },
    { 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu },
    { 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu },
    { 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 32, 17, 18, 31, 19, 20, 0xFFFFu, 21, 22, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu },
    { 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 41, 0xFFFFu, 34, 35, 42 },
    { 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu },
    { 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu },
    { 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu },
    { 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu },
    { 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu },
    { 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu },
    { 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu },
    { 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu },
    { 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 44, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu },
    { 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 45, 35, 42 },
    { 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu },
    { 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu },
    { 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu },
    { 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 49, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu },
    { 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu },
    { 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 50, 35, 42 },
    { 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu },
    { 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu },
    { 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 53, 35, 42 },
    { 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 54, 0xFFFFu, 0xFFFFu, 0xFFFFu },
    { 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu },
    { 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu },
    { 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu },
    { 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 65, 35, 42 },
    { 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 66 },
    { 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 67 },
    { 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu },
    { 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu },
    { 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu },
    { 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 69, 0xFFFFu, 34, 35, 42 },
    { 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu },
    { 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu },
    { 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu },
    { 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu },
    { 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu },
    { 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu },
    { 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu },
    { 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 71, 17, 18, 0xFFFFu, 19, 20, 0xFFFFu, 21, 22, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu },
    { 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu },
    { 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 72, 35, 42 },
    { 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 73, 42 },
    { 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 74, 42 },
    { 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu },
    { 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu },
    { 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu },
    { 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu },
    { 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu },
    { 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu },
    { 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 75 },
    { 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 76 },
    { 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu },
    { 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu },
    { 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu },
    { 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 78, 17, 18, 0xFFFFu, 19, 20, 0xFFFFu, 21, 22, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu },
    { 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu },
    { 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu },
    { 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu },
    { 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu },
    { 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu },
    { 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu },
    { 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu },
    { 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu },
    { 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu },
    { 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu },
    { 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 80, 17, 18, 0xFFFFu, 19, 20, 0xFFFFu, 21, 22, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu },
    { 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu }
};
inline constexpr std::uint16_t kProductionLength[] =
{
    1u, 2u, 3u, 3u, 0u, 1u, 3u, 3u, 3u, 0u, 1u, 3u, 1u, 0u, 1u, 1u, 1u, 1u, 1u, 1u, 3u, 3u, 1u, 3u, 4u, 5u, 0u, 2u, 2u, 2u, 3u, 1u, 1u, 1u, 1u, 1u, 1u, 1u, 3u, 3u, 1u, 3u, 3u, 1u, 1u, 3u, 2u, 2u
};
inline constexpr std::uint16_t kProductionLhs[] =
{
    0u, 1u, 2u, 3u, 3u, 4u, 4u, 5u, 6u, 6u, 7u, 7u, 8u, 8u, 9u, 9u, 9u, 9u, 9u, 9u, 10u, 11u, 12u, 12u, 13u, 14u, 15u, 16u, 17u, 18u, 18u, 19u, 19u, 19u, 19u, 19u, 19u, 20u, 20u, 20u, 21u, 21u, 21u, 22u, 22u, 22u, 22u, 22u
};




/* ============ 内部常量、Token 语义值钩子与错误表 ============ */

/// 文法里是否写了 error 产生式（写了才能把 error 当伪 Token 移进）。
inline constexpr bool kHasErrorToken = false;
/// error 伪终结符在终结符表里的列号（没写 error 产生式时是 0）。
inline constexpr std::uint16_t kErrorTokenId = 0u;
/// 文法里是否声明了 %sync。
inline constexpr bool kHasSyncTokens = false;

/**
 * @brief 一次归约的语义结果。
 *
 * 语义动作里的 `$$` 翻成 `result.value`（`$n` 翻成 `stack->values[...]`），
 * LR / LL / GLR 三条驱动共用这一个壳子。
 */
struct ParseResult
{
    ParserValue_T value{}; ///< `$$`
};

/**
 * @brief 把状态码转成错误码数值（与 C 目标的 PARSER_ERR_* 一一对应）。
 * @param status 状态码。
 * @return 数值形式。
 */
inline std::int32_t failCode(Status status) noexcept
{
    return static_cast<std::int32_t>(status);
}

/**
 * @brief 取 Token 类型名（与 C 目标的 Parser_TokenName 同义，越界时 "UNKNOWN"）。
 * @param type Token 类型。
 * @return 只读名字。
 */
inline std::string tokenTypeText(TokenType type)
{
    const char *name = tokenName(type);
    return (name != nullptr) ? std::string(name) : std::string("UNKNOWN");
}

/**
 * @brief 取 Token 的语义值。
 *
 * 用户定义了 PARSER_USER_TOKEN_VALUE(pToken) 就用用户的；没定义时按文法
 * 选的语义值模型给零值（写了 %union 时词法器已经按 `%token <成员>` 把词素
 * 写进 value，直接读 `pToken->value` 就是语义值）。
 */
inline ParserValue_T tokenValue(const Token &token)
{
#ifdef PARSER_USER_TOKEN_VALUE
    return PARSER_USER_TOKEN_VALUE(&token);
#else
    (void)token;
    return ParserValue_T{};
#endif
}

/**
 * @brief 这是一个同步 Token 吗（`%sync`）。
 *
 * 没写 %sync 时恒为 false：错误恢复里那些 panic 代码仍然编译，但永远不生效。
 */
inline bool isSyncToken(std::uint16_t type) noexcept
{
    switch (type)
    {
    default: return false;
    }
}

/* ---------- 文法规模（全部来自生成期，运行期只读） ---------- */

/// 终结符个数（含 END）。
inline constexpr std::uint32_t kTerminalCount = 30u;
/// 非终结符个数。
inline constexpr std::uint32_t kNontermCount = 23u;
/// 起始非终结符编号。
inline constexpr std::uint16_t kStartId = 0u;
/// LR 自动机状态个数（LL(1) 模式没有状态机，这里是 0）。
inline constexpr std::uint32_t kStateCount = 81u;
/// ACTION 表里的「接受」标记。
inline constexpr std::int32_t kAcceptAction = INT32_MAX;
/// 无效下标（对应 C 目标的 PARSER_INVALID_INDEX）。
inline constexpr std::uint16_t kInvalidIndex = 0xFFFFu;
/// 无效状态（对应 C 目标的 UINT16_MAX）。
inline constexpr std::uint16_t kInvalidState = 0xFFFFu;
/// GLR 归约路径的最大长度 / 单次解析最多访问多少条路径。
inline constexpr std::size_t kMaxPathDepth = 64u;
inline constexpr std::uint32_t kMaxPathVisits = 200000u;
/// 非终结符名表（%tree 建树用；LR 归约时把编号翻成名字）。
inline constexpr const char *const kTreeNontermNames[] =
{
    "__lalr_start",
    "program",
    "block",
    "const_part",
    "const_list",
    "const_item",
    "var_part",
    "var_list",
    "opt_stmt",
    "stmt",
    "assign",
    "compound",
    "stmt_list",
    "if_stmt",
    "while_stmt",
    "while_head",
    "read_stmt",
    "write_stmt",
    "cond",
    "relop",
    "expr",
    "term",
    "factor"
};

/* ============ 语法树辅助：建叶子 / 建分支 / 深拷贝 / 打印 ============ */

/// 名字为空时的兜底（保证树里不会出现 nullptr）。
[[maybe_unused]] std::string symbolOrPlaceholder(const char *name)
{
    return (name != nullptr) ? std::string(name) : std::string("?");
}

/// 建一个终结符叶子节点：名字是 Token 类型名，词素是 Token 的显示文本。
[[maybe_unused]] std::unique_ptr<Tree> makeLeaf(const Token &token)
{
    return std::make_unique<Tree>(symbolOrPlaceholder(tokenName(token.type)),
                                  tokenText(token));
}

/// 把栈上 [base, base + length) 的节点收成一个非终结符节点。
[[maybe_unused]] std::unique_ptr<Tree> makeBranch(
    std::vector<std::unique_ptr<Tree>> &nodes, const char *name,
    std::size_t base, std::size_t length)
{
    std::unique_ptr<Tree> node =
        std::make_unique<Tree>(symbolOrPlaceholder(name));
    for (std::size_t index = 0u; index < length; ++index)
    {
        std::unique_ptr<Tree> &child = nodes[base + index];
        if (child != nullptr)
        {
            node->addChild(std::move(child));
        }
    }
    return node;
}

/// 深拷贝一棵树（GLR 下同一条边会被多条路径共享，写入结果表时要克隆）。
[[maybe_unused]] std::unique_ptr<Tree> cloneTree(const Tree &source)
{
    std::unique_ptr<Tree> copy =
        std::make_unique<Tree>(source.name(), source.text());
    for (std::size_t index = 0u; index < source.childCount(); ++index)
    {
        const Tree *child = source.child(index);
        if (child != nullptr)
        {
            copy->addChild(cloneTree(*child));
        }
    }
    return copy;
}

/// 非终结符编号 -> 名字（%tree 建内部分支节点用）。
[[maybe_unused]] inline const char *treeTreeName(std::uint16_t id) noexcept
{
    return (id < kNontermCount) ? kTreeNontermNames[id] : nullptr;
}

/// 递归拼一棵树的缩进文本。
[[maybe_unused]] void appendTreeText(const Tree &node, std::size_t depth,
                                     std::string &out)
{
    out.append(depth * 2u, ' ');
    out += node.name();
    if (!node.text().empty())
    {
        out += '(';
        out += node.text();
        out += ')';
    }
    out += '\n';
    for (std::size_t index = 0u; index < node.childCount(); ++index)
    {
        const Tree *child = node.child(index);
        if (child != nullptr)
        {
            appendTreeText(*child, depth + 1u, out);
        }
    }
}

}

class Engine::Private
{
public:
    /* ---------- 错误表 ---------- */

    /// @return 本次解析累积的错误。
    std::vector<Error> &errors() noexcept { return m_errors; }

    /// @return 最近一条错误；没有错误时返回 nullptr。
    const Error *lastError() const noexcept
    {
        return m_errors.empty() ? nullptr : &m_errors.back();
    }

    /// 追加一条错误（错误表满时丢弃，与 C 目标的 PARSER_MAX_ERRORS 一致）。
    void setError(std::int32_t code, std::uint32_t line, std::uint32_t column,
                  std::string message)
    {
        if (m_errors.size() >= kMaxErrors)
        {
            return;
        }
        m_errors.emplace_back(code, line, column, std::move(message));
    }

    /* ---------- 解析入口 ---------- */

    /**
     * @brief 文本模式：把输入拷一份（保证 '\0' 结尾）并读入第一个词法单元。
     * @param text 输入文本。
     * @return Status::Ok 表示就绪。
     */
    Status prepareText(std::string_view text)
    {
        m_input.assign(text);
        m_text = m_input.c_str();
        m_textPos = 0u;
        m_textMode = true;
        return prepareCommon();
    }

    /**
     * @brief Token 模式：直接用调用方给的 Token 序列。
     * @param tokens Token 序列（解析期间必须保持有效）。
     * @return Status::Ok 表示就绪。
     */
    Status prepareTokens(const std::vector<Token> &tokens)
    {
        m_tokenArray = tokens.data();
        m_tokenCount = tokens.size();
        m_tokenPos = 0u;
        m_textMode = false;
        m_text = nullptr;
        m_textPos = 0u;
        return prepareCommon();
    }

    /// 两种模式共用的复位动作：清错误、清词素、取第一个 lookahead。
    Status prepareCommon()
    {
        m_errors.clear();
        m_lexeme.clear();
        m_errorStatus = 0;
        m_line = 1u;
        m_column = 1u;
        m_current = Token();
        return advance();
    }

    /**
     * @brief 读一个字符并维护行列号（只在文本模式下用）。
     * @return 读到的字符。
     */
    char readChar()
    {
        const char ch = m_text[m_textPos++];
        if (ch == '\n')
        {
            ++m_line;
            m_column = 1u;
        }
        else
        {
            ++m_column;
        }
        return ch;
    }

    /**
     * @brief 取下一个 lookahead：文本模式走词法器，Token 模式按序号取。
     * @return Status::Ok 表示成功。
     */
    Status advance()
    {
        if (!m_textMode)
        {
            if (m_tokenPos < m_tokenCount)
            {
                m_current = m_tokenArray[m_tokenPos++];
            }
            else
            {
                m_current = Token();
                m_current.type = TokenType::TOKEN_END;
            }
            return Status::Ok;
        }
        Token token;
        const Status status = lexNext(token);
        if (status != Status::Ok)
        {
            setError(failCode(Status::ErrLexer), token.line, token.column,
                     "词法解析失败");
            token.type = TokenType::TOKEN_INVALID;
            m_current = token;
            return status;
        }
        m_current = token;
        return Status::Ok;
    }

    /**
     * @brief 解析文本。
     * @param text 输入文本。
     * @param result 输出语义值；可为 nullptr。
     * @return Status::Ok 表示成功。
     */
    Status runText(std::string_view text, ParserValue_T *result)
    {
        const Status ready = prepareText(text);
        if (ready != Status::Ok)
        {
            return Status::ErrLexer;
        }
        std::unique_ptr<Tree> ignoredTree;
        return runLoop(result, &ignoredTree);
    }

    /**
     * @brief 解析 Token 序列。
     * @param tokens Token 序列。
     * @param result 输出语义值；可为 nullptr。
     * @return Status::Ok 表示成功。
     */
    Status runTokens(const std::vector<Token> &tokens, ParserValue_T *result)
    {
        const Status ready = prepareTokens(tokens);
        if (ready != Status::Ok)
        {
            return Status::ErrLexer;
        }
        std::unique_ptr<Tree> ignoredTree;
        return runLoop(result, &ignoredTree);
    }

    /**
     * @brief 解析文本并构建语法树（只建树，不跑语义动作）。
     * @param text 输入文本。
     * @return 语法树根；失败返回 nullptr（原因在 errors() 里）。
     */
    std::unique_ptr<Tree> parseTree(std::string_view text)
    {
        std::unique_ptr<Tree> tree;
        if (prepareText(text) != Status::Ok)
        {
            return tree;
        }
        (void)runLoop(nullptr, &tree, true);
        return tree;
    }

    /* ---------- 引擎状态 ---------- */

    std::vector<Error> m_errors;      ///< 本次解析的错误表
    std::string m_input;              ///< 文本模式的输入副本
    const char *m_text = nullptr;     ///< m_input.c_str()
    std::size_t m_textPos = 0u;       ///< 文本模式的读写位置
    const Token *m_tokenArray = nullptr; ///< Token 数组（调用方所有）
    std::size_t m_tokenCount = 0u;    ///< Token 个数
    std::size_t m_tokenPos = 0u;      ///< 下一个 Token 下标
    bool m_textMode = true;           ///< true：文本模式；false：Token 模式
    Token m_current;                  ///< 当前 lookahead
    std::string m_lexeme;             ///< 词素缓冲
    std::uint32_t m_line = 1u;        ///< 当前行号
    std::uint32_t m_column = 1u;      ///< 当前列号
    std::int32_t m_errorStatus = 0;   ///< >0 表示正处在错误恢复过程中

    /**
     * @brief 固定字面量命中判断（\b 通配命中任意一个空白字符）。
     * @param rule 词法规则。
     * @param pText 文本起点。
     * @return 命中返回 true。
     */
    static bool lexRuleHit(const LexRule &rule, const char *pText)
    {
        if (!rule.wildcard)
        {
            return std::strncmp(pText, rule.text, rule.length) == 0;
        }
        for (std::uint32_t index = 0u; index < rule.length; ++index)
        {
            if (rule.text[index] == '\x1f')
            {
                const unsigned char ch =
                    static_cast<unsigned char>(pText[index]);
                if ((ch != ' ') && (ch != '\t') && (ch != '\n') &&
                    (ch != '\v') && (ch != '\f') && (ch != '\r'))
                {
                    return false;
                }
            }
            else if (rule.text[index] != pText[index])
            {
                return false;
            }
        }
        return true;
    }

    /* ---------- 词法器 ---------- */

    /**
     * @brief 取下一个词法单元。
     *
     * 顺序与 C 目标一致：双引号字符串（内建，或用户自己声明了 STRING 的词法
     * 时跳过）→ %token regex 与固定字面量一起做最长匹配（字面量优先）→ 数字
     * → 标识符（%identifier 的 DFA 或默认的 C 风格标识符）。固定字面量命中时
     * 词素直接指向字面量本身，所以 Token::text() 不会失效。
     * @param token 输出词法单元（失败时也会带上行列号）。
     * @return Status::Ok 表示取到；ErrLexer 表示词法错误。
     */
    Status lexNext(Token &token)
    {
        token = Token();
        while (std::isspace(static_cast<unsigned char>(m_text[m_textPos])) != 0)
        {
            readChar();
        }
        token.line = m_line;
        token.column = m_column;
        const char *pStart = &m_text[m_textPos];
        if (*pStart == '\0')
        {
            token.type = TokenType::TOKEN_END;
            return Status::Ok;
        }

        /* 内建双引号字符串：词法阶段完成反转义，结果写进 m_lexeme。 */
        if (*pStart == '"')
        {
            m_lexeme.clear();
            readChar();
            while ((m_text[m_textPos] != '\0') && (m_text[m_textPos] != '"'))
            {
                char ch = readChar();
                if (ch == '\\')
                {
                    const char escaped = readChar();
                    switch (escaped)
                    {
                    case 'n': ch = '\n'; break;
                    case 'r': ch = '\r'; break;
                    case 't': ch = '\t'; break;
                    case '\\': ch = '\\'; break;
                    case '"': ch = '"'; break;
                    default: return Status::ErrLexer;
                    }
                }
                m_lexeme.push_back(ch);
            }
            if (m_text[m_textPos] != '"')
            {
                return Status::ErrLexer;
            }
            readChar();
            token.type = TokenType::TOKEN_STRING;
            token.text = m_lexeme;
            token.name = m_lexeme.c_str();

            return Status::Ok;
        }

        /* 固定字面量与 %token regex 一起做最长匹配，字面量优先。 */
        {
            std::uint32_t bestLength = 0u;
            TokenType bestType = TokenType::TOKEN_INVALID;
            bool bestFromRegex = false;
            bool bestWildcard = false;
            const char *pBestText = nullptr;
            for (const LexRule &rule : kLexRules)
            {
                if ((rule.text != nullptr) &&
                    lexRuleHit(rule, pStart))
                {
                    const unsigned char last = static_cast<unsigned char>(
                        rule.text[rule.length - 1u]);
                    const unsigned char next =
                        static_cast<unsigned char>(pStart[rule.length]);
                    if (((std::isalnum(last) != 0) || (last == '_')) &&
                        ((std::isalnum(next) != 0) || (next == '_')))
                    {
                        continue;
                    }
                    if (rule.length > bestLength)
                    {
                        bestLength = rule.length;
                        bestType = rule.type;
                        bestFromRegex = false;
                        bestWildcard = rule.wildcard;
                        pBestText = rule.text;
                    }
                }
            }
            if (bestType != TokenType::TOKEN_INVALID)
            {
                for (std::uint32_t index = 0u; index < bestLength; ++index)
                {
                    readChar();
                }
                token.type = bestType;
                if (bestFromRegex)
                {
                    m_lexeme.assign(pStart, bestLength);
                    token.text = m_lexeme;
                    token.pName = m_lexeme.c_str();

                }
                else if (bestWildcard)
                {
                    /* \b 通配命中：词素要按输入抄一份，不能直接指字面量 */
                    m_lexeme.assign(pStart, bestLength);
                    token.text = m_lexeme;
                    token.pName = m_lexeme.c_str();
                }
                else
                {
                    token.text = pBestText;
                    token.pName = pBestText;
                }
                return Status::Ok;
            }
        }

        /* 小数点和指数决定 float，后缀 d 决定 double；后缀 u 只用于整数。 */
        if ((std::isdigit(static_cast<unsigned char>(*pStart)) != 0) ||
            ((*pStart == '.') &&
             (std::isdigit(static_cast<unsigned char>(pStart[1])) != 0)))
        {
            char *pEnd = nullptr;
            bool hasDot = false;
            bool hasExponent = false;
            char suffix = '\0';
            double parsed = 0.0;
            while (std::isdigit(
                       static_cast<unsigned char>(m_text[m_textPos])) != 0)
            {
                readChar();
            }
            if (m_text[m_textPos] == '.')
            {
                hasDot = true;
                readChar();
                while (std::isdigit(
                           static_cast<unsigned char>(m_text[m_textPos])) != 0)
                {
                    readChar();
                }
            }
            if ((m_text[m_textPos] == 'e') || (m_text[m_textPos] == 'E'))
            {
                hasExponent = true;
                readChar();
                if ((m_text[m_textPos] == '+') || (m_text[m_textPos] == '-'))
                {
                    readChar();
                }
                if (std::isdigit(
                        static_cast<unsigned char>(m_text[m_textPos])) == 0)
                {
                    return Status::ErrLexer;
                }
                while (std::isdigit(
                           static_cast<unsigned char>(m_text[m_textPos])) != 0)
                {
                    readChar();
                }
            }
            if (std::isalpha(
                    static_cast<unsigned char>(m_text[m_textPos])) != 0)
            {
                suffix = readChar();
            }
            if (std::isalpha(
                    static_cast<unsigned char>(m_text[m_textPos])) != 0)
            {
                return Status::ErrLexer;
            }
            const std::size_t length =
                static_cast<std::size_t>(&m_text[m_textPos] - pStart);
            m_lexeme.assign(pStart, length);
            if (suffix != '\0')
            {
                m_lexeme.resize(length - 1u);
            }
            errno = 0;
            if ((suffix == 'u') || (suffix == 'U'))
            {
                unsigned long value = 0ul;
                if (hasDot || hasExponent)
                {
                    return Status::ErrLexer;
                }
                value = std::strtoul(m_lexeme.c_str(), &pEnd, 10);
                if ((errno == ERANGE) || (*pEnd != '\0') ||
                    (value > UINT32_MAX))
                {
                    return Status::ErrLexer;
                }
                parsed = static_cast<double>(static_cast<std::uint32_t>(value));
            }
            else if ((suffix == 'd') || (suffix == 'D'))
            {
                if (!hasDot && !hasExponent)
                {
                    return Status::ErrLexer;
                }
                parsed = std::strtod(m_lexeme.c_str(), &pEnd);
                if ((errno == ERANGE) || (*pEnd != '\0') ||
                    (std::isfinite(parsed) == 0))
                {
                    return Status::ErrLexer;
                }
            }
            else if (suffix != '\0')
            {
                return Status::ErrLexer;
            }
            else if (hasDot || hasExponent)
            {
                const float value = std::strtof(m_lexeme.c_str(), &pEnd);
                if ((errno == ERANGE) || (*pEnd != '\0') ||
                    (std::isfinite(value) == 0))
                {
                    return Status::ErrLexer;
                }
                parsed = static_cast<double>(value);
            }
            else
            {
                const long value = std::strtol(m_lexeme.c_str(), &pEnd, 10);
                if ((errno == ERANGE) || (*pEnd != '\0') ||
                    (value < INT32_MIN) || (value > INT32_MAX))
                {
                    return Status::ErrLexer;
                }
                parsed = static_cast<double>(value);
            }
            token.type = TokenType::TOKEN_NUMBER;
            token.text = m_lexeme;
            token.name = m_lexeme.c_str();
            token.value = static_cast<std::int32_t>(parsed);

            return Status::Ok;
        }

        const std::uint32_t identLength = identifierLength(pStart);
        if (identLength > 0u)
        {
            for (std::uint32_t index = 0u; index < identLength; ++index)
            {
                readChar();
            }
            m_lexeme.assign(pStart, identLength);
            const TokenType keyword = keywordType(m_lexeme.c_str());
            token.type = (keyword != TokenType::TOKEN_INVALID)
                             ? keyword
                             : TokenType::TOKEN_VARIABLE;
            token.text = m_lexeme;
            token.pName = m_lexeme.c_str();

            return Status::Ok;
        }

        return Status::ErrLexer;
    }

    /* ---------- LR 分析驱动 ---------- */

    /// LR 分析栈：状态 / 符号 / 语义值 / 变量名 / （%tree）语法树节点。
    struct LrStack
    {
        std::vector<std::uint16_t> states;
        std::vector<std::uint16_t> symbols;
        std::vector<ParserValue_T> values;
        std::vector<std::string> names;
        std::vector<std::unique_ptr<Tree>> nodes;
        std::size_t top = 0u; ///< 有效槽位数（栈顶下标 = top - 1）

        /// 保证第 index 个槽位可用（按需扩容，不预分配整条栈）。
        void grow(std::size_t index)
        {
            if (index < states.size())
            {
                return;
            }
            const std::size_t size = (index < 256u) ? 256u : (index * 2u);
            states.resize(size);
            symbols.resize(size);
            values.resize(size);
            names.resize(size);
            nodes.resize(size);
        }

        /// 入栈；栈深超过 kMaxStackDepth 时返回 false。
        bool push(std::uint16_t state, std::uint16_t symbol,
                  ParserValue_T value, std::string_view name)
        {
            if (top >= kMaxStackDepth)
            {
                return false;
            }
            grow(top);
            states[top] = state;
            symbols[top] = symbol;
            values[top] = std::move(value);
            names[top].assign(name);
            nodes[top].reset();
            ++top;
            return true;
        }

        /// 弹到 newTop：语义值 / 变量名 / 语法树节点全部按 RAII 释放。
        void popTo(std::size_t newTop)
        {
            for (std::size_t index = newTop; index < top; ++index)
            {
                values[index] = ParserValue_T{};
                names[index].clear();
                nodes[index].reset();
            }
            top = newTop;
        }

        /// 丢弃栈顶一个槽位（错误恢复用）。
        void discardTop()
        {
            if (top > 0u)
            {
                popTo(top - 1u);
            }
        }
    };

    /**
     * @brief 执行一个产生式的语义动作（LR / GLR 共用）。
     *
     * 动作里的 `$n` 由生成器翻成 `stack->values[stack->top - k + n - 1]`，
     * 所以这里必须传**指针**：LR 传自己的分析栈，GLR 传按路径填好的临时栈
     * （top = 右部长度）。
     * @param production 产生式编号。
     * @param stack 分析栈（语义动作读它的 values / names）。
     * @param result 语义结果（$$ 写它的 value）。
     * @return Status::Ok 表示成功。
     */
    Status applyActionCode(std::uint16_t production, LrStack *stack,
                           ParseResult &result)
    {
        (void)stack;
        switch (production)
        {
        case 0u:
        {
            result.value = ParserValue_T{};
            break;
        }
        case 1u:
        {
#line 142 "pl0.gram"
            result.value = stack->values[stack->top - 2u + 0u];
#line 1221 "pl0_parser.cpp"
            
            break;
        }
        case 2u:
        {
#line 148 "pl0.gram"
            result.value = stack->values[stack->top - 3u + 2u];
#line 1229 "pl0_parser.cpp"
            
            break;
        }
        case 3u:
        {
#line 154 "pl0.gram"
            result.value = stack->values[stack->top - 3u + 1u];
#line 1237 "pl0_parser.cpp"
            
            break;
        }
        case 4u:
        {
#line 157 "pl0.gram"
            result.value = Pl0_PlaceNone();
#line 1245 "pl0_parser.cpp"
            
            break;
        }
        case 5u:
        {
#line 161 "pl0.gram"
            result.value = stack->values[stack->top - 1u + 0u];
#line 1253 "pl0_parser.cpp"
            
            break;
        }
        case 6u:
        {
#line 165 "pl0.gram"
            result.value = stack->values[stack->top - 3u + 2u];
#line 1261 "pl0_parser.cpp"
            
            break;
        }
        case 7u:
        {
#line 171 "pl0.gram"
            Pl0_DefineConst(stack->values[stack->top - 3u + 0u], stack->values[stack->top - 3u + 2u]);
                    result.value = Pl0_PlaceNone();
#line 1270 "pl0_parser.cpp"
            
            break;
        }
        case 8u:
        {
#line 178 "pl0.gram"
            result.value = stack->values[stack->top - 3u + 1u];
#line 1278 "pl0_parser.cpp"
            
            break;
        }
        case 9u:
        {
#line 181 "pl0.gram"
            result.value = Pl0_PlaceNone();
#line 1286 "pl0_parser.cpp"
            
            break;
        }
        case 10u:
        {
#line 185 "pl0.gram"
            Pl0_DefineVar(stack->values[stack->top - 1u + 0u]);
                    result.value = Pl0_PlaceNone();
#line 1295 "pl0_parser.cpp"
            
            break;
        }
        case 11u:
        {
#line 190 "pl0.gram"
            Pl0_DefineVar(stack->values[stack->top - 3u + 2u]);
                    result.value = stack->values[stack->top - 3u + 0u];
#line 1304 "pl0_parser.cpp"
            
            break;
        }
        case 12u:
        {
#line 197 "pl0.gram"
            result.value = stack->values[stack->top - 1u + 0u];
#line 1312 "pl0_parser.cpp"
            
            break;
        }
        case 13u:
        {
#line 200 "pl0.gram"
            result.value = Pl0_PlaceNone();
#line 1320 "pl0_parser.cpp"
            
            break;
        }
        case 14u:
        {
#line 205 "pl0.gram"
            result.value = stack->values[stack->top - 1u + 0u];
#line 1328 "pl0_parser.cpp"
            
            break;
        }
        case 15u:
        {
#line 209 "pl0.gram"
            result.value = stack->values[stack->top - 1u + 0u];
#line 1336 "pl0_parser.cpp"
            
            break;
        }
        case 16u:
        {
#line 213 "pl0.gram"
            result.value = stack->values[stack->top - 1u + 0u];
#line 1344 "pl0_parser.cpp"
            
            break;
        }
        case 17u:
        {
#line 217 "pl0.gram"
            result.value = stack->values[stack->top - 1u + 0u];
#line 1352 "pl0_parser.cpp"
            
            break;
        }
        case 18u:
        {
#line 221 "pl0.gram"
            result.value = stack->values[stack->top - 1u + 0u];
#line 1360 "pl0_parser.cpp"
            
            break;
        }
        case 19u:
        {
#line 225 "pl0.gram"
            result.value = stack->values[stack->top - 1u + 0u];
#line 1368 "pl0_parser.cpp"
            
            break;
        }
        case 20u:
        {
#line 231 "pl0.gram"
            Pl0_Assign(Pl0_Resolve(stack->values[stack->top - 3u + 0u]), stack->values[stack->top - 3u + 2u]);
                    result.value = Pl0_PlaceNone();
#line 1377 "pl0_parser.cpp"
            
            break;
        }
        case 21u:
        {
#line 238 "pl0.gram"
            result.value = Pl0_PlaceNone();
#line 1385 "pl0_parser.cpp"
            
            break;
        }
        case 22u:
        {
#line 244 "pl0.gram"
            result.value = stack->values[stack->top - 1u + 0u];
#line 1393 "pl0_parser.cpp"
            
            break;
        }
        case 23u:
        {
#line 248 "pl0.gram"
            result.value = stack->values[stack->top - 3u + 2u];
#line 1401 "pl0_parser.cpp"
            
            break;
        }
        case 24u:
        {
#line 254 "pl0.gram"
            Pl0_EndIf(stack->values[stack->top - 4u + 1u]);
                    result.value = Pl0_PlaceNone();
#line 1410 "pl0_parser.cpp"
            
            break;
        }
        case 25u:
        {
#line 261 "pl0.gram"
            Pl0_EndWhile(stack->values[stack->top - 5u + 2u]);
                    result.value = Pl0_PlaceNone();
#line 1419 "pl0_parser.cpp"
            
            break;
        }
        case 26u:
        {
#line 267 "pl0.gram"
            result.value = Pl0_BeginWhile();
#line 1427 "pl0_parser.cpp"
            
            break;
        }
        case 27u:
        {
#line 271 "pl0.gram"
            Pl0_Read(Pl0_Resolve(stack->values[stack->top - 2u + 1u]));
                    result.value = Pl0_PlaceNone();
#line 1436 "pl0_parser.cpp"
            
            break;
        }
        case 28u:
        {
#line 278 "pl0.gram"
            Pl0_Write(stack->values[stack->top - 2u + 1u]);
                    result.value = Pl0_PlaceNone();
#line 1445 "pl0_parser.cpp"
            
            break;
        }
        case 29u:
        {
#line 286 "pl0.gram"
            result.value = Pl0_OddJump(stack->values[stack->top - 2u + 1u]);
#line 1453 "pl0_parser.cpp"
            
            break;
        }
        case 30u:
        {
#line 290 "pl0.gram"
            result.value = Pl0_CondJump(stack->values[stack->top - 3u + 1u].value, stack->values[stack->top - 3u + 0u], stack->values[stack->top - 3u + 2u]);
#line 1461 "pl0_parser.cpp"
            
            break;
        }
        case 31u:
        {
#line 295 "pl0.gram"
            result.value = Pl0_Code(PL0_CMP_EQ);
#line 1469 "pl0_parser.cpp"
            
            break;
        }
        case 32u:
        {
#line 296 "pl0.gram"
            result.value = Pl0_Code(PL0_CMP_NE);
#line 1477 "pl0_parser.cpp"
            
            break;
        }
        case 33u:
        {
#line 297 "pl0.gram"
            result.value = Pl0_Code(PL0_CMP_LT);
#line 1485 "pl0_parser.cpp"
            
            break;
        }
        case 34u:
        {
#line 298 "pl0.gram"
            result.value = Pl0_Code(PL0_CMP_LE);
#line 1493 "pl0_parser.cpp"
            
            break;
        }
        case 35u:
        {
#line 299 "pl0.gram"
            result.value = Pl0_Code(PL0_CMP_GT);
#line 1501 "pl0_parser.cpp"
            
            break;
        }
        case 36u:
        {
#line 300 "pl0.gram"
            result.value = Pl0_Code(PL0_CMP_GE);
#line 1509 "pl0_parser.cpp"
            
            break;
        }
        case 37u:
        {
#line 304 "pl0.gram"
            result.value = stack->values[stack->top - 1u + 0u];
#line 1517 "pl0_parser.cpp"
            
            break;
        }
        case 38u:
        {
#line 308 "pl0.gram"
            result.value = Pl0_Binary(PL0_OP_ADD, stack->values[stack->top - 3u + 0u], stack->values[stack->top - 3u + 2u]);
#line 1525 "pl0_parser.cpp"
            
            break;
        }
        case 39u:
        {
#line 312 "pl0.gram"
            result.value = Pl0_Binary(PL0_OP_SUB, stack->values[stack->top - 3u + 0u], stack->values[stack->top - 3u + 2u]);
#line 1533 "pl0_parser.cpp"
            
            break;
        }
        case 40u:
        {
#line 318 "pl0.gram"
            result.value = stack->values[stack->top - 1u + 0u];
#line 1541 "pl0_parser.cpp"
            
            break;
        }
        case 41u:
        {
#line 322 "pl0.gram"
            result.value = Pl0_Binary(PL0_OP_MUL, stack->values[stack->top - 3u + 0u], stack->values[stack->top - 3u + 2u]);
#line 1549 "pl0_parser.cpp"
            
            break;
        }
        case 42u:
        {
#line 326 "pl0.gram"
            result.value = Pl0_Binary(PL0_OP_DIV, stack->values[stack->top - 3u + 0u], stack->values[stack->top - 3u + 2u]);
#line 1557 "pl0_parser.cpp"
            
            break;
        }
        case 43u:
        {
#line 332 "pl0.gram"
            result.value = Pl0_Resolve(stack->values[stack->top - 1u + 0u]);
#line 1565 "pl0_parser.cpp"
            
            break;
        }
        case 44u:
        {
#line 336 "pl0.gram"
            result.value = stack->values[stack->top - 1u + 0u];
#line 1573 "pl0_parser.cpp"
            
            break;
        }
        case 45u:
        {
#line 340 "pl0.gram"
            result.value = stack->values[stack->top - 3u + 1u];
#line 1581 "pl0_parser.cpp"
            
            break;
        }
        case 46u:
        {
#line 344 "pl0.gram"
            result.value = Pl0_Negate(stack->values[stack->top - 2u + 1u]);
#line 1589 "pl0_parser.cpp"
            
            break;
        }
        case 47u:
        {
#line 348 "pl0.gram"
            result.value = stack->values[stack->top - 2u + 1u];
#line 1597 "pl0_parser.cpp"
            
            break;
        }
        default: return Status::ErrSyntax;
        }
        return Status::Ok;
    }

    /**
     * @brief `yyerrok`：清掉「正在恢复」状态，后面的语法错误照常上报。
     *
     * 只给语义动作用（生成期把动作里的 `yyerrok;` 翻成本调用）。
     */
    void yyErrOk() noexcept
    {
        m_errorStatus = 0;
    }

    /**
     * @brief `yyclearin`：丢掉当前 lookahead（下一个 Token 由主循环再取）。
     */
    void yyClearIn()
    {
        if (m_current.type != TokenType::TOKEN_END)
        {
            (void)advance();
        }
    }

    /**
     * @brief 移进：把当前 Token 压栈；%tree 时顺带建一个叶子节点。
     * @param stack 分析栈。
     * @param state 移进目标状态。
     * @param treeMode 是否构建语法树。
     * @return Status::Ok 表示成功。
     */
    Status lrShift(LrStack &stack, std::uint16_t state, bool treeMode)
    {
        if (!stack.push(state, static_cast<std::uint16_t>(m_current.type),
                        tokenValue(m_current), m_current.text))
        {
            return Status::ErrStack;
        }
        if (treeMode)
        {
            stack.nodes[stack.top - 1u] = makeLeaf(m_current);
        }
        return Status::Ok;
    }

    /**
     * @brief 归约：执行语义动作、弹出右部、按 GOTO 压回结果。
     * @param production 产生式编号。
     * @param treeMode 是否构建语法树（建树时不执行语义动作）。
     * @return Status::Ok 表示成功。
     */
    Status lrReduce(std::uint16_t production, bool treeMode)
    {
        LrStack &stack = m_lrStack;
        const std::uint32_t length = kProductionLength[production];
        const std::uint16_t lhs = kProductionLhs[production];
        ParseResult result;
        std::unique_ptr<Tree> node;
        if (length > stack.top)
        {
            return Status::ErrStack;
        }
        const std::size_t base = stack.top - length;
        if (treeMode)
        {
            node = makeBranch(stack.nodes, treeTreeName(lhs), base, length);
        }
        else if (applyActionCode(production, &stack, result) != Status::Ok)
        {
            return Status::ErrSyntax;
        }
        stack.popTo(base);
        if (stack.top == 0u)
        {
            setError(failCode(Status::ErrStack), m_current.line, m_current.column,
                     "LALR 归约后栈为空");
            return Status::ErrStack;
        }
        const std::uint16_t state =
            kGoto[stack.states[stack.top - 1u]][lhs];
        if (state == kInvalidState)
        {
            setError(failCode(Status::ErrSyntax), m_current.line,
                     m_current.column, "LALR GOTO 表没有匹配项");
            return Status::ErrSyntax;
        }
        if (!stack.push(state, lhs, result.value, std::string_view()))
        {
            return Status::ErrStack;
        }
        if (treeMode)
        {
            stack.nodes[stack.top - 1u] = std::move(node);
        }
        return Status::Ok;
    }

    /**
     * @brief panic reduce：%sync 恢复时先把栈顶状态上待归约的项做掉。
     *
     * 语法错误常常出现在「归约到一半」的构造里（例如 `1 + + 2` 卡在
     * `expr PLUS . term`），这时栈上没有任何状态能直接移进同步 Token；沿着
     * 归约链做掉待归约的项，才能回到语句边界那种能移进同步 Token 的状态。
     * 空产生式归约不会缩短栈，跳过它以免原地打转。
     * @param state 栈顶状态。
     * @param treeMode 是否构建语法树。
     * @return Status::Ok 表示做掉了一次归约。
     */
    Status lrPanicReduce(std::uint16_t state, bool treeMode)
    {
        for (std::uint32_t terminal = 0u; terminal < kTerminalCount; ++terminal)
        {
            const std::int32_t action = kAction[state][terminal];
            if (action >= 0)
            {
                continue;
            }
            const std::uint16_t production =
                static_cast<std::uint16_t>(-action - 1);
            const std::uint32_t length = kProductionLength[production];
            if ((length == 0u) || (length > m_lrStack.top))
            {
                return Status::ErrSyntax;
            }
            return lrReduce(production, treeMode);
        }
        return Status::ErrSyntax;
    }

    /// 栈顶状态能不能移进某个同步 Token。
    bool lrCanShiftSyncToken(std::uint16_t state) const noexcept
    {
        if (!kHasSyncTokens)
        {
            return false;
        }
        for (std::uint32_t token = 0u; token < kTerminalCount; ++token)
        {
            if (isSyncToken(static_cast<std::uint16_t>(token)) &&
                (kAction[state][token] > 0))
            {
                return true;
            }
        }
        return false;
    }

    /**
     * @brief 语法错误恢复：error 产生式优先，%sync 兜底。
     * @param stack 分析栈。
     * @param treeMode 是否构建语法树。
     * @return Status::Ok 表示找到了恢复点（主循环继续）。
     */
    Status lrRecover(LrStack &stack, bool treeMode)
    {
        // ① 已经在错误状态里：先丢掉当前 lookahead 再试，避免同一处连续刷屏
        if (m_errorStatus > 0)
        {
            while (m_current.type != TokenType::TOKEN_END)
            {
                if (advance() != Status::Ok)
                {
                    return Status::ErrLexer;
                }
                const std::uint32_t token =
                    static_cast<std::uint32_t>(m_current.type);
                if ((token < kTerminalCount) &&
                    (kAction[stack.states[stack.top - 1u]][token] != 0))
                {
                    m_errorStatus = static_cast<std::int32_t>(kErrorRecoveryTokens);
                    return Status::Ok;
                }
            }
            return Status::ErrSyntax;
        }
        // ② error 产生式：弹栈到能移进 error 的状态，把 error 当伪 Token 移进
        //    （不消耗 lookahead，继续用同一个 Token 走主循环）
        if (kHasErrorToken)
        {
            while (stack.top > 0u)
            {
                const std::int32_t action =
                    kAction[stack.states[stack.top - 1u]][kErrorTokenId];
                if (action > 0)
                {
                    ParserValue_T placeholder{};
                    if (!stack.push(static_cast<std::uint16_t>(action - 1u),
                                    kErrorTokenId, placeholder, "error"))
                    {
                        return Status::ErrStack;
                    }
                    m_errorStatus = static_cast<std::int32_t>(kErrorRecoveryTokens);
                    return Status::Ok;
                }
                stack.discardTop();
            }
        }
        // ③ %sync panic mode：弹栈到能移进某个同步 Token 的状态，再丢输入到它。
        //    弹栈之前先做掉栈顶状态上待归约的项（panic reduce），否则「归约到
        //    一半」的错误（例如 `1 + + 2`）找不到任何能移进同步 Token 的状态。
        if (kHasSyncTokens)
        {
            std::uint32_t reduceBudget = 256u;
            while (stack.top > 0u)
            {
                const std::uint16_t state = stack.states[stack.top - 1u];
                if (lrCanShiftSyncToken(state))
                {
                    while ((m_current.type != TokenType::TOKEN_END) &&
                           !isSyncToken(static_cast<std::uint16_t>(m_current.type)))
                    {
                        if (advance() != Status::Ok)
                        {
                            return Status::ErrLexer;
                        }
                    }
                    if (m_current.type == TokenType::TOKEN_END)
                    {
                        return Status::ErrSyntax;
                    }
                    const std::int32_t action =
                        kAction[state][static_cast<std::uint32_t>(m_current.type)];
                    if (action > 0)
                    {
                        if (lrShift(stack, static_cast<std::uint16_t>(action - 1u),
                                    treeMode) != Status::Ok)
                        {
                            return Status::ErrStack;
                        }
                        if (advance() != Status::Ok)
                        {
                            return Status::ErrLexer;
                        }
                        m_errorStatus = static_cast<std::int32_t>(kErrorRecoveryTokens);
                        return Status::Ok;
                    }
                }
                else if ((reduceBudget > 0u) &&
                         (lrPanicReduce(state, treeMode) == Status::Ok))
                {
                    --reduceBudget;
                    continue;
                }
                stack.discardTop();
            }
        }
        return Status::ErrSyntax;
    }

    /**
     * @brief LR 主循环：移进 / 归约 / 接受。
     * @param out 输出语义值；可为 nullptr。
     * @param outTree 输出语法树；treeMode 为 true 时使用。
     * @param treeMode 是否构建语法树。
     * @return Status::Ok 表示成功。
     */
    Status runLoop(ParserValue_T *out, std::unique_ptr<Tree> *outTree,
                   bool treeMode = false)
    {
        LrStack &stack = m_lrStack;
        stack = LrStack();
        ParserValue_T initial{};
        if (!stack.push(0u, kInvalidIndex, initial, std::string_view()))
        {
            return Status::ErrStack;
        }
        for (;;)
        {
            const std::uint16_t state = stack.states[stack.top - 1u];
            const std::uint32_t token = static_cast<std::uint32_t>(m_current.type);
            if ((state >= kStateCount) || (token >= kTerminalCount))
            {
                setError(failCode(Status::ErrSyntax), m_current.line,
                         m_current.column, "LALR 状态或 Token 越界");
                return Status::ErrSyntax;
            }
            const std::int32_t action = kAction[state][token];
            if (action == kAcceptAction)
            {
                if (out != nullptr)
                {
                    *out = stack.values[stack.top - 1u];
                }
                if (outTree != nullptr)
                {
                    *outTree = std::move(stack.nodes[stack.top - 1u]);
                }
                return Status::Ok;
            }
            if (action > 0)
            {
                if (lrShift(stack, static_cast<std::uint16_t>(action - 1), treeMode)
                    != Status::Ok)
                {
                    setError(failCode(Status::ErrStack), m_current.line,
                             m_current.column, "LALR 分析栈已满");
                    return Status::ErrStack;
                }
                if (advance() != Status::Ok)
                {
                    return Status::ErrLexer;
                }
                // 3 Token 规则：恢复过程中每成功移进一个真实 Token 就递减，
                // 降到 0 就退出错误状态（下一个错误重新上报）
                if (m_errorStatus > 0)
                {
                    --m_errorStatus;
                }
            }
            else if (action < 0)
            {
                if (lrReduce(static_cast<std::uint16_t>(-action - 1), treeMode)
                    != Status::Ok)
                {
                    return Status::ErrSyntax;
                }
            }
            else
            {
                // 已经在错误状态里就不再上报（bison 的 3 Token 规则 / yyerrok
                // 就是用来控制「什么时候重新开始报错」的）
                if (m_errorStatus == 0)
                {
                    setError(failCode(Status::ErrSyntax), m_current.line,
                             m_current.column,
                             "LALR 分析表没有匹配项");
                    // 错误太多就放弃恢复
                    if (m_errors.size() >= kMaxErrors)
                    {
                        return Status::ErrSyntax;
                    }
                }
                if (lrRecover(stack, treeMode) != Status::Ok)
                {
                    return Status::ErrSyntax;
                }
                continue;
            }
        }
    }

    /// LR 分析栈（跨次解析复用，避免反复分配）。
    LrStack m_lrStack;



};

/* ============ 公开 API 的定义 ============ */

const char *statusName(Status status) noexcept
{
    switch (status)
    {
    case Status::Ok: return "Ok";
    case Status::ErrStack: return "ErrStack";
    case Status::ErrSyntax: return "ErrSyntax";
    case Status::ErrLexer: return "ErrLexer";
    case Status::ErrDuplicate: return "ErrDuplicate";
    case Status::ErrUndefined: return "ErrUndefined";
    case Status::ErrSystem: return "ErrSystem";
    }
    return "Unknown";
}

const char *tokenName(TokenType type) noexcept
{
    const std::uint32_t index = static_cast<std::uint32_t>(type);
    if (index >= kTokenNameCount)
    {
        return nullptr;
    }
    return kTokenNames[index];
}

std::string tokenText(const Token &token)
{
    if (!token.text.empty())
    {
        return std::string(token.text);
    }
    const char *name = tokenName(token.type);
    return (name != nullptr) ? std::string(name) : std::string();
}


Error::Error(std::int32_t code, std::uint32_t line, std::uint32_t column,
             std::string message)
    : m_code(code), m_line(line), m_column(column), m_message(std::move(message))
{
}

std::string Error::toString() const
{
    return "第 " + std::to_string(m_line) + " 行第 " +
           std::to_string(m_column) + " 列：" + m_message;
}

Tree::Tree(std::string name, std::string text)
    : m_name(std::move(name)), m_text(std::move(text))
{
}

const Tree *Tree::child(std::size_t index) const noexcept
{
    return (index < m_children.size()) ? m_children[index].get() : nullptr;
}

Tree *Tree::child(std::size_t index) noexcept
{
    return (index < m_children.size()) ? m_children[index].get() : nullptr;
}

void Tree::addChild(std::unique_ptr<Tree> child)
{
    if (child != nullptr)
    {
        m_children.push_back(std::move(child));
    }
}

std::string Tree::toString() const
{
    std::string text;
    appendTreeText(*this, 0u, text);
    return text;
}

void Tree::print(std::FILE *stream) const
{
    const std::string text = toString();
    std::FILE *target = (stream != nullptr) ? stream : stdout;
    std::fwrite(text.data(), 1u, text.size(), target);
}

Engine::Engine() : d(std::make_unique<Private>())
{
}

Engine::~Engine() = default;

Status Engine::runText(std::string_view text, ParserValue_T *result)
{
    return d->runText(text, result);
}

Status Engine::run(const std::vector<Token> &tokens, ParserValue_T *result)
{
    return d->runTokens(tokens, result);
}


const std::vector<Error> &Engine::errors() const noexcept
{
    return d->m_errors;
}

const Error *Engine::lastError() const noexcept
{
    return d->lastError();
}




} // namespace pl0

using namespace pl0;

#line 354 "pl0.gram"
/* ---------------------------------------------------------------
 * 三元式生成器实现。
 *
 * 这一段被原样贴到 pl0_parser.cpp 末尾 —— C++17 下它在 namespace pl0
 * **之外**，所以这里只用 C 的基本类型与自己的全局状态，不引用解析器命名
 * 空间里的任何东西（要引用就得写 pl0::Xxx 全限定名）。
 * --------------------------------------------------------------- */
#define PL0_MAX_TRIPLES 512
#define PL0_MAX_SYMBOLS 128
#define PL0_MAX_LABELS  64

typedef struct Pl0_Triple_T
{
    char op[12];
    char arg1[32];
    char arg2[32];
} Pl0_Triple_T;

typedef struct Pl0_Symbol_T
{
    char name[32];
    int  isConst;
    int  value;
} Pl0_Symbol_T;

static Pl0_Triple_T g_pl0Triples[PL0_MAX_TRIPLES];
static int          g_pl0TripleCount = 0;
static Pl0_Symbol_T g_pl0Symbols[PL0_MAX_SYMBOLS];
static int          g_pl0SymbolCount = 0;
static int          g_pl0LabelCount = 0;
static int          g_pl0LabelStack[PL0_MAX_LABELS];
static int          g_pl0LabelDepth = 0;
static int          g_pl0HasError = 0;

/* 算子码 → 三元式里的算符文本 */
static const char *Pl0_OpText(int op)
{
    switch (op)
    {
    case PL0_OP_ADD: return "+";
    case PL0_OP_SUB: return "-";
    case PL0_OP_MUL: return "*";
    case PL0_OP_DIV: return "/";
    case PL0_OP_NEG: return "neg";
    case PL0_OP_ODD: return "odd";
    case PL0_CMP_EQ: return "==";
    case PL0_CMP_NE: return "!=";
    case PL0_CMP_LT: return "<";
    case PL0_CMP_LE: return "<=";
    case PL0_CMP_GT: return ">";
    case PL0_CMP_GE: return ">=";
    default: return "?";
    }
}

/* 把一个位置渲染成三元式里的操作数文本：常量给值、变量给名字、
 * 临时量给 `(下标)`、标签给 `L<号>`、空位给 `_` */
static const char *Pl0_Operand(ParserValue_T place, char *pBuffer,
                               unsigned long size)
{
    switch (place.kind)
    {
    case PL0_PLACE_CONST:
        snprintf(pBuffer, size, "%d", place.value);
        break;
    case PL0_PLACE_VAR:
        snprintf(pBuffer, size, "%s", place.name);
        break;
    case PL0_PLACE_TEMP:
        snprintf(pBuffer, size, "(%d)", place.index);
        break;
    case PL0_PLACE_LABEL:
        snprintf(pBuffer, size, "L%d", place.value);
        break;
    default:
        snprintf(pBuffer, size, "_");
        break;
    }
    return pBuffer;
}

/* 追加一条三元式，返回它的下标 */
static int Pl0_Emit(const char *pOp, const char *pArg1, const char *pArg2)
{
    int index = g_pl0TripleCount;
    if (g_pl0TripleCount >= PL0_MAX_TRIPLES)
    {
        fprintf(stderr, "三元式太多（上限 %d）\n", PL0_MAX_TRIPLES);
        g_pl0HasError = 1;
        return 0;
    }
    g_pl0TripleCount++;
    snprintf(g_pl0Triples[index].op, sizeof(g_pl0Triples[index].op), "%s",
             pOp);
    snprintf(g_pl0Triples[index].arg1, sizeof(g_pl0Triples[index].arg1), "%s",
             pArg1);
    snprintf(g_pl0Triples[index].arg2, sizeof(g_pl0Triples[index].arg2), "%s",
             pArg2);
    return index;
}

static ParserValue_T Pl0_MakePlace(int kind, int value, int index,
                                   const char *pName)
{
    ParserValue_T place;
    memset(&place, 0, sizeof(place));
    place.kind = kind;
    place.value = value;
    place.index = index;
    if ((pName != NULL) && (pName[0] != '\0'))
    {
        snprintf(place.name, sizeof(place.name), "%s", pName);
    }
    return place;
}

static Pl0_Symbol_T *Pl0_FindSymbol(const char *pName)
{
    int index;
    for (index = 0; index < g_pl0SymbolCount; index++)
    {
        if (strcmp(g_pl0Symbols[index].name, pName) == 0)
        {
            return &g_pl0Symbols[index];
        }
    }
    return NULL;
}

/* ---- 对外的帮助函数 ---- */
ParserValue_T Pl0_PlaceNone(void)
{
    return Pl0_MakePlace(PL0_PLACE_NONE, 0, 0, NULL);
}

ParserValue_T Pl0_Code(int code)
{
    return Pl0_MakePlace(PL0_PLACE_CODE, code, 0, NULL);
}

ParserValue_T Pl0_PlaceFromToken(int isNumber, int isIdentifier, int value,
                                 const char *pName)
{
    if (isNumber != 0)
    {
        return Pl0_MakePlace(PL0_PLACE_CONST, value, 0, NULL);
    }
    if ((isIdentifier == 0) || (pName == NULL) || (pName[0] == '\0'))
    {
        return Pl0_PlaceNone();
    }
    /* 只记名字，不查表：这一趟也可能是 `var x` 的声明处 */
    return Pl0_MakePlace(PL0_PLACE_VAR, 0, 0, pName);
}

/* 真正使用一个标识符时查符号表：常量直接折叠成值，变量原样返回，
 * 没声明过就报错（错误计数由 Pl0_HasError 汇总给入口） */
ParserValue_T Pl0_Resolve(ParserValue_T place)
{
    Pl0_Symbol_T *pSymbol = NULL;
    if (place.kind != PL0_PLACE_VAR)
    {
        return place;
    }
    pSymbol = Pl0_FindSymbol(place.name);
    if (pSymbol == NULL)
    {
        fprintf(stderr, "未声明的标识符: %s\n", place.name);
        g_pl0HasError = 1;
        return place;
    }
    if (pSymbol->isConst != 0)
    {
        return Pl0_MakePlace(PL0_PLACE_CONST, pSymbol->value, 0, NULL);
    }
    return place;
}

ParserValue_T Pl0_Binary(int op, ParserValue_T left, ParserValue_T right)
{
    char arg1[32];
    char arg2[32];
    int index;
    /* 两个都是常量就直接折叠，省得为字面量生成三元式 */
    if ((left.kind == PL0_PLACE_CONST) && (right.kind == PL0_PLACE_CONST))
    {
        int value = 0;
        switch (op)
        {
        case PL0_OP_ADD: value = left.value + right.value; break;
        case PL0_OP_SUB: value = left.value - right.value; break;
        case PL0_OP_MUL: value = left.value * right.value; break;
        case PL0_OP_DIV:
            if (right.value == 0)
            {
                fprintf(stderr, "除数为常量 0\n");
                g_pl0HasError = 1;
                value = 0;
            }
            else
            {
                value = left.value / right.value;
            }
            break;
        default: break;
        }
        return Pl0_MakePlace(PL0_PLACE_CONST, value, 0, NULL);
    }
    Pl0_Operand(left, arg1, sizeof(arg1));
    Pl0_Operand(right, arg2, sizeof(arg2));
    index = Pl0_Emit(Pl0_OpText(op), arg1, arg2);
    return Pl0_MakePlace(PL0_PLACE_TEMP, 0, index, NULL);
}

ParserValue_T Pl0_Negate(ParserValue_T operand)
{
    char arg1[32];
    int index;
    if (operand.kind == PL0_PLACE_CONST)
    {
        return Pl0_MakePlace(PL0_PLACE_CONST, -operand.value, 0, NULL);
    }
    Pl0_Operand(operand, arg1, sizeof(arg1));
    index = Pl0_Emit("neg", arg1, "_");
    return Pl0_MakePlace(PL0_PLACE_TEMP, 0, index, NULL);
}

/* 生成「算一个结果 + 为假就跳」这一对三元式，返回假出口标签 */
static ParserValue_T Pl0_TestJump(const char *pOp, ParserValue_T operand)
{
    char arg1[32];
    char arg2[32];
    int index;
    int label;
    Pl0_Operand(operand, arg1, sizeof(arg1));
    index = Pl0_Emit(pOp, arg1, "_");
    label = ++g_pl0LabelCount;
    snprintf(arg1, sizeof(arg1), "(%d)", index);
    snprintf(arg2, sizeof(arg2), "L%d", label);
    Pl0_Emit("jz", arg1, arg2);
    return Pl0_MakePlace(PL0_PLACE_LABEL, label, 0, NULL);
}

ParserValue_T Pl0_OddJump(ParserValue_T operand)
{
    return Pl0_TestJump("odd", operand);
}

ParserValue_T Pl0_CondJump(int cmpOp, ParserValue_T left, ParserValue_T right)
{
    char arg1[32];
    char arg2[32];
    int index;
    int label;
    Pl0_Operand(left, arg1, sizeof(arg1));
    Pl0_Operand(right, arg2, sizeof(arg2));
    index = Pl0_Emit(Pl0_OpText(cmpOp), arg1, arg2);
    label = ++g_pl0LabelCount;
    snprintf(arg1, sizeof(arg1), "(%d)", index);
    snprintf(arg2, sizeof(arg2), "L%d", label);
    Pl0_Emit("jz", arg1, arg2);
    return Pl0_MakePlace(PL0_PLACE_LABEL, label, 0, NULL);
}

void Pl0_EndIf(ParserValue_T falseLabel)
{
    char arg1[32];
    snprintf(arg1, sizeof(arg1), "L%d", falseLabel.value);
    Pl0_Emit("label", arg1, "_");
}

ParserValue_T Pl0_BeginWhile(void)
{
    int label = ++g_pl0LabelCount;
    char arg1[32];
    snprintf(arg1, sizeof(arg1), "L%d", label);
    Pl0_Emit("label", arg1, "_");
    if (g_pl0LabelDepth < PL0_MAX_LABELS)
    {
        g_pl0LabelStack[g_pl0LabelDepth++] = label;
    }
    else
    {
        fprintf(stderr, "while 嵌套太深\n");
        g_pl0HasError = 1;
    }
    return Pl0_MakePlace(PL0_PLACE_LABEL, label, 0, NULL);
}

void Pl0_EndWhile(ParserValue_T falseLabel)
{
    char arg1[32];
    int beginLabel = 0;
    if (g_pl0LabelDepth > 0)
    {
        beginLabel = g_pl0LabelStack[--g_pl0LabelDepth];
    }
    snprintf(arg1, sizeof(arg1), "L%d", beginLabel);
    Pl0_Emit("j", "_", arg1);
    snprintf(arg1, sizeof(arg1), "L%d", falseLabel.value);
    Pl0_Emit("label", arg1, "_");
}

void Pl0_DefineConst(ParserValue_T name, ParserValue_T value)
{
    if (Pl0_FindSymbol(name.name) != NULL)
    {
        fprintf(stderr, "重复声明: %s\n", name.name);
        g_pl0HasError = 1;
        return;
    }
    if (g_pl0SymbolCount >= PL0_MAX_SYMBOLS)
    {
        fprintf(stderr, "符号太多（上限 %d）\n", PL0_MAX_SYMBOLS);
        g_pl0HasError = 1;
        return;
    }
    snprintf(g_pl0Symbols[g_pl0SymbolCount].name,
             sizeof(g_pl0Symbols[g_pl0SymbolCount].name), "%s", name.name);
    g_pl0Symbols[g_pl0SymbolCount].isConst = 1;
    g_pl0Symbols[g_pl0SymbolCount].value = value.value;
    g_pl0SymbolCount++;
}

void Pl0_DefineVar(ParserValue_T name)
{
    if (Pl0_FindSymbol(name.name) != NULL)
    {
        fprintf(stderr, "重复声明: %s\n", name.name);
        g_pl0HasError = 1;
        return;
    }
    if (g_pl0SymbolCount >= PL0_MAX_SYMBOLS)
    {
        fprintf(stderr, "符号太多（上限 %d）\n", PL0_MAX_SYMBOLS);
        g_pl0HasError = 1;
        return;
    }
    snprintf(g_pl0Symbols[g_pl0SymbolCount].name,
             sizeof(g_pl0Symbols[g_pl0SymbolCount].name), "%s", name.name);
    g_pl0Symbols[g_pl0SymbolCount].isConst = 0;
    g_pl0Symbols[g_pl0SymbolCount].value = 0;
    g_pl0SymbolCount++;
}

void Pl0_Assign(ParserValue_T target, ParserValue_T value)
{
    char arg1[32];
    Pl0_Operand(value, arg1, sizeof(arg1));
    Pl0_Emit(":=", arg1, target.name);
}

void Pl0_Read(ParserValue_T target)
{
    Pl0_Emit("read", "_", target.name);
}

void Pl0_Write(ParserValue_T value)
{
    char arg1[32];
    Pl0_Operand(value, arg1, sizeof(arg1));
    Pl0_Emit("write", arg1, "_");
}

int Pl0_HasError(void)
{
    return g_pl0HasError;
}

void Pl0_PrintTriples(void)
{
    int index;
    printf("三元式：共 %d 条\n", g_pl0TripleCount);
    for (index = 0; index < g_pl0TripleCount; index++)
    {
        printf("%3d: (%s, %s, %s)\n", index, g_pl0Triples[index].op,
               g_pl0Triples[index].arg1, g_pl0Triples[index].arg2);
    }
}

#line 2453 "pl0_parser.cpp"
