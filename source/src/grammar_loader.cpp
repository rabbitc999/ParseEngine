/**
 * @file grammar_loader.cpp
 * @brief .gram 文本文法加载实现。
 */

#include "ParseEngine/grammar_loader.hpp"

#include "ParseEngine/preprocessor.hpp"
#include "ParseEngine/token_dict.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <system_error>
#include <fstream>
#include <limits>
#include <sstream>
#include <vector>

#include "ParseEngine/regex_dfa.hpp"
#include "ParseEngine/regex_vm.hpp"
#include "ParseEngine/utils.hpp"
#include "ParseEngine/analysis.hpp"

namespace ParseEngine
{

const std::vector<std::string> k_defaultTokenNames = {"STRING", "NUMBER",
                                                      "VARIABLE"};
const std::string k_errorTokenName = "error";
const std::string k_epsilon = "";
const int k_maxStackDepthDefault = 64;
const int k_maxStackDepthLimit = 65535;
const int k_maxErrorsDefault = 8;
const int k_maxErrorsLimit = 4096;
const int k_errorRecoveryTokensDefault = 3;
const int k_errorRecoveryTokensLimit = 4096;

namespace
{

/**
 * @brief 把词法器给出的 num 文本安全地转成浮点。
 *
 * 词法器允许 `1e3` / `3.7` 这类写法，所以按浮点解析、再由调用方决定怎么用。
 * 旧代码用 `std::stod`：`1e999` 这种超出 double 范围的输入会抛
 * std::out_of_range（生成器现在不走异常），这里改用 `strtod` —— 溢出时只是
 * 返回 HUGE_VAL 并置 ERANGE，不会抛；顺带把非数字尾巴也一并拦掉。
 * @param text num 文本。
 * @param out 输出浮点值（成功时写入）。
 * @return 成功返回 true；解析失败或溢出返回 false。
 */
bool parseNumText(const std::string &text, double &out)
{
    errno = 0;
    const char *begin = text.c_str();
    char *end = nullptr;
    const double value = std::strtod(begin, &end);
    if ((end == begin) || (end != (begin + text.size())) ||
        (errno == ERANGE) || !std::isfinite(value))
    {
        return false;
    }
    out = value;
    return true;
}

/**
 * @brief 把已经解析出来的浮点值渲染成报错文本（保持旧提示的写法）。
 * @param raw 浮点值。
 * @param text 原始 num 文本（超出 int 范围时原样回显）。
 * @return 报错文本。
 */
std::string numTextForError(double raw, const std::string &text)
{
    if ((raw < static_cast<double>(std::numeric_limits<int>::min())) ||
        (raw > static_cast<double>(std::numeric_limits<int>::max())))
    {
        return text;
    }
    return std::to_string(static_cast<int>(raw));
}

/**
 * @brief 判断字符是否为十进制数字。
 * @param ch 待判断字符。
 * @return 是数字返回 true。
 */
bool isDigit(char ch)
{
    return (ch >= '0') && (ch <= '9');
}

/**
 * @brief 判断字符是否为标识符组成字符（不含首字符规则）。
 * @param ch 待判断字符。
 * @return 是字母/数字/下划线返回 true。
 */
bool isIdPart(char ch)
{
    return std::isalnum(static_cast<unsigned char>(ch)) || (ch == '_');
}

/**
 * @brief 判断字符是否为标识符首字符（额外允许 %）。
 * @param ch 待判断字符。
 * @return 允许返回 true。
 */
bool isIdStart(char ch)
{
    return std::isalpha(static_cast<unsigned char>(ch)) || (ch == '_') ||
           (ch == '%');
}
/// C++ 关键字（`%namespace` / `--namespace` 不接受这些名字）。
const std::set<std::string> &cppKeywords()
{
    static const std::set<std::string> keywords = {
        "alignas",      "alignof",      "and",           "and_eq",
        "asm",          "atomic_cancel", "atomic_commit", "atomic_noexcept",
        "auto",         "bitand",       "bitor",         "bool",
        "break",        "case",         "catch",         "char",
        "char8_t",      "char16_t",     "char32_t",      "class",
        "compl",        "concept",      "const",         "consteval",
        "constexpr",    "constinit",    "const_cast",    "continue",
        "co_await",     "co_return",    "co_yield",      "decltype",
        "default",      "delete",       "do",            "double",
        "dynamic_cast", "else",         "enum",          "explicit",
        "export",       "extern",       "false",         "float",
        "for",          "friend",       "goto",          "if",
        "inline",       "int",          "long",          "mutable",
        "namespace",    "new",          "noexcept",      "not",
        "not_eq",       "nullptr",      "operator",      "or",
        "or_eq",        "private",      "protected",     "public",
        "reflexpr",     "register",     "reinterpret_cast", "requires",
        "return",       "short",        "signed",        "sizeof",
        "static",       "static_assert", "static_cast",  "struct",
        "switch",       "synchronized", "template",      "this",
        "thread_local", "throw",        "true",          "try",
        "typedef",      "typeid",       "typename",      "union",
        "unsigned",     "using",        "virtual",       "void",
        "volatile",     "wchar_t",      "while",         "xor",
        "xor_eq"};
    return keywords;
}

/// Python 关键字（`%class` 不接受这些名字）。
const std::set<std::string> &pythonKeywords()
{
    static const std::set<std::string> keywords = {
        "False",  "None",   "True",   "and",    "as",     "assert",
        "async",  "await",  "break",  "class",  "continue", "def",
        "del",    "elif",   "else",   "except", "finally", "for",
        "from",   "global", "if",     "import", "in",     "is",
        "lambda", "nonlocal", "not",  "or",     "pass",   "raise",
        "return", "try",    "while",  "with",   "yield"};
    return keywords;
}

/**
 * @brief 名字是不是合法的 C++ 标识符。
 * @param name 名字。
 * @param allowEmpty 空串是否算合法。
 * @return 合法返回 true。
 */
bool isCppIdentifier(const std::string &name, bool allowEmpty)
{
    if (name.empty())
    {
        return allowEmpty;
    }
    if (name.find("::") != std::string::npos)
    {
        return false;
    }
    if ((!isIdStart(name[0])) || (name[0] == '%'))
    {
        return false;
    }
    for (const char ch : name)
    {
        if (!isIdPart(ch))
        {
            return false;
        }
    }
    return cppKeywords().count(name) == 0;
}


/**
 * @brief 去掉首尾空白（与 Python str.strip 一致）。
 * @param text 输入文本。
 * @return 去掉首尾空白后的文本。
 */
std::string strip(const std::string &text)
{
    const std::string k_spaces = " \t\n\r\v\f";
    const std::size_t begin = text.find_first_not_of(k_spaces);
    if (begin == std::string::npos)
    {
        return "";
    }
    const std::size_t end = text.find_last_not_of(k_spaces);
    return text.substr(begin, end - begin + 1);
}

/**
 * @brief 生成 Python str 的 repr（用于错误信息里的 %r）。
 * @param text 输入文本。
 * @return 形如 'abc' 或 "abc" 的文本。
 */
std::string pythonRepr(const std::string &text)
{
    const bool hasSingle = text.find('\'') != std::string::npos;
    const bool hasDouble = text.find('"') != std::string::npos;
    const char quote = (hasSingle && !hasDouble) ? '"' : '\'';
    std::string result(1, quote);
    for (const char ch : text)
    {
        if ((ch == '\\') || (ch == quote))
        {
            result += '\\';
        }
        result += ch;
    }
    result += quote;
    return result;
}

/**
 * @brief .gram 词法单元。
 */
struct GramToken
{
    std::string kind;  ///< num / str / id / dollar / op / nl
    std::string text;  ///< 原文
    std::size_t start = 0; ///< 起始字节偏移
    std::size_t end = 0;   ///< 结束字节偏移
};

/**
 * @brief 行号换算表：字节偏移 -> **原文件**行号。
 *
 * 条件编译预处理会丢掉若干整行，预处理后的行号与原文行号不再一致，所以
 * `source/src/preprocessor.cpp` 会给出一张「预处理行 -> 原文行」的映射表；这里再按
 * 每行的起始偏移把任意字节偏移换算成行号。
 */
struct SourceLines
{
    std::vector<std::size_t> lineStarts; ///< 每行起始字节偏移（第 i 项 = 第 i+1 行）
    std::vector<std::size_t> lineMap;    ///< 预处理行 -> 原文件行；空 = 恒等

    /**
     * @brief 建立换算表。
     * @param text 预处理后的文本。
     * @param map 预处理行 -> 原文件行；空表示没有条件编译指令。
     * @return 换算表。
     */
    static SourceLines build(const std::string &text,
                             const std::vector<std::size_t> &map)
    {
        SourceLines lines;
        lines.lineMap = map;
        lines.lineStarts.push_back(0u);
        for (std::size_t index = 0u; index < text.size(); ++index)
        {
            if (text[index] == '\n')
            {
                lines.lineStarts.push_back(index + 1u);
            }
        }
        return lines;
    }

    /**
     * @brief 把字节偏移换算成原文件行号。
     * @param offset 字节偏移。
     * @return 行号（1 起）；换算不出来时返回 0。
     */
    std::size_t lineOf(std::size_t offset) const
    {
        if (lineStarts.empty())
        {
            return 0u;
        }
        const auto found =
            std::upper_bound(lineStarts.begin(), lineStarts.end(), offset);
        const std::size_t index =
            static_cast<std::size_t>(found - lineStarts.begin()) - 1u;
        if (lineMap.empty())
        {
            return index + 1u;
        }
        if (index >= lineMap.size())
        {
            return 0u;
        }
        return lineMap[index];
    }
};

/**
 * @brief 把 .gram 文本切成 token（注释跳过，用户代码块单独收集）。
 */
class GramTokenizer
{
public:
    /**
     * @brief 扫描整段文本。
     * @param text .gram 文本。
     * @param lines 行号换算表（可空）。
     * @param baseDir 文法文件所在目录：`include "…"` 的相对路径按它解析
     *                （空表示没有文件上下文，按当前工作目录算）。
     */
    explicit GramTokenizer(const std::string &text,
                           const SourceLines *lines = nullptr,
                           const std::string &baseDir = std::string())
        : m_lines(lines)
    {
        m_baseDir = baseDir;
        scan(text);
    }

    /**
     * @brief 是否已经出错。
     * @return 出过错返回 true。
     */
    bool failed() const
    {
        return !m_error.empty();
    }

    /**
     * @brief 取第一个错误。
     * @return 错误值。
     */
    const Error &error() const
    {
        return m_error;
    }

    std::vector<GramToken> tokens;            ///< 普通 token
    std::vector<std::string> userCodeParts;   ///< %code 内容，按出现顺序
    std::vector<std::size_t> userCodeLines;   ///< 每个 %code 块的原文件行号
    std::string epilogue;                     ///< %epilogue 正文（原样，不含标记行）
    bool hasEpilogue = false;                 ///< 是否写了 %epilogue
    std::size_t epilogueLine = 0u;            ///< %epilogue 标记所在的原文件行号
    std::vector<std::string> mainBlocks;      ///< %main 正文（按出现顺序）
    std::vector<std::size_t> mainBlockLines;  ///< 每个 %main 标记的原文件行号
    std::string classBody;                    ///< %class_body 正文（原样，不含标记行）
    bool hasClassBody = false;                ///< 是否写了 %class_body
    std::size_t classBodyLine = 0u;           ///< %class_body 标记所在的原文件行号

private:
    std::string m_baseDir; ///< 文法文件所在目录（`include "…"` 的相对路径基准）

    /**
     * @brief 把字节偏移换算成原文件行号。
     * @param offset 字节偏移。
     * @return 行号（1 起）；没有换算表时返回 0（表示未知）。
     */
    std::size_t lineOf(std::size_t offset) const
    {
        return (m_lines == nullptr) ? 0u : m_lines->lineOf(offset);
    }

    const SourceLines *m_lines = nullptr; ///< 行号换算表（可空）

    /**
     * @brief 记录第一个错误（错误槽已满时不覆盖）。
     * @param message 错误说明。
     * @param line 原文件行号（1 起）；0 表示位置未知。
     */
    void fail(const std::string &message, std::size_t line = 0u)
    {
        ParseEngine::fail(m_error, message, line);
    }

    Error m_error; ///< 第一个错误

    /**
     * @brief 判断某个位置之前是不是只有空白（即它在一行的行首）。
     * @param text 文本。
     * @param index 位置。
     * @return 行首（或文件开头）返回 true。
     */
    static bool isLineStartPosition(const std::string &text, std::size_t index)
    {
        for (std::size_t cursor = index; cursor > 0u; --cursor)
        {
            const char ch = text[cursor - 1u];
            if (ch == '\n')
            {
                return true;
            }
            if ((ch != ' ') && (ch != '\t') && (ch != '\r'))
            {
                return false;
            }
        }
        return true;
    }

    /**
     * @brief 判断 index 处是不是“单独成行”的段标记（前后只允许空白）。
     * @param text 文本。
     * @param index 位置。
     * @param marker 标记名（含 `%`，如 `%epilogue`）。
     * @return 命中返回 true。
     */
    static bool isMarkerLine(const std::string &text, std::size_t index,
                             const std::string &marker)
    {
        const std::string tail = markerTail(text, index, marker);
        return (tail.empty() && isMarkerMatch(text, index, marker));
    }

    /**
     * @brief 取段标记行在标记之后的剩余内容（去掉空白与行尾换行）。
     *
     * 除了「标记单独成行」，段标记还能带一个 include：
     *
     * ```text
     * %code include "user.h"        // 正文从文件读，不用写 %endcode
     * %epilogue include "epi.cpp"   // 段正文从文件读
     * %main include "main.cpp"
     * ```
     *
     * `include` 与路径之间允许不写等号（推荐）或写 `=`。标记不匹配时返回空串，
     * 调用方用 isMarkerMatch 区分「不是这个标记」和「标记后面没写东西」。
     * @param text 文本。
     * @param index 位置（行首的 `%`）。
     * @param marker 标记名（含 `%`）。
     * @return 标记后面的内容（trim 过）；不是这个标记时返回空串。
     */
    static std::string markerTail(const std::string &text, std::size_t index,
                                  const std::string &marker)
    {
        if (!isMarkerMatch(text, index, marker))
        {
            return std::string();
        }
        std::size_t cursor = index + marker.size();
        while ((cursor < text.size()) && (text[cursor] != '\n'))
        {
            ++cursor;
        }
        return strip(text.substr(index + marker.size(),
                                 cursor - (index + marker.size())));
    }

    /**
     * @brief index 处（行首）是不是以 marker 开头的行。
     * @param text 文本。
     * @param index 位置。
     * @param marker 标记名（含 `%`）。
     * @return 命中返回 true。
     */
    static bool isMarkerMatch(const std::string &text, std::size_t index,
                              const std::string &marker)
    {
        return isLineStartPosition(text, index) &&
               (text.compare(index, marker.size(), marker) == 0);
    }

    /**
     * @brief 解析段标记后面的 `include "路径"`。
     * @param tail markerTail() 取到的剩余内容。
     * @param path 输出：路径（原样，未做路径拼接）。
     * @return 是这个形状返回 true；空串或别的形状返回 false。
     */
    static bool parseIncludeTail(const std::string &tail, std::string *path)
    {
        std::size_t index = 0u;
        while ((index < tail.size()) && (tail[index] == ' ')) ++index;
        static const std::string kKeyword = "include";
        if (tail.compare(index, kKeyword.size(), kKeyword) != 0) return false;
        index += kKeyword.size();
        while ((index < tail.size()) && (tail[index] == ' ')) ++index;
        if ((index < tail.size()) && (tail[index] == '='))   // 也接受 include="…"
        {
            ++index;
            while ((index < tail.size()) && (tail[index] == ' ')) ++index;
        }
        if ((index >= tail.size()) || ((tail[index] != '"') && (tail[index] != '\'')))
        {
            return false;
        }
        const char quote = tail[index];
        ++index;
        const std::size_t end = tail.find(quote, index);
        if (end == std::string::npos) return false;
        const std::string value = tail.substr(index, end - index);
        if (value.empty()) return false;
        const std::size_t rest = tail.find_first_not_of(' ', end + 1u);
        if (rest != std::string::npos) return false;   // 引号后面还有别的东西
        *path = value;
        return true;
    }

    /**
     * @brief 读一个 include 进来的文件。
     * @param path 标记行里写的路径。
     * @param line 标记所在行（报错用）。
     * @param out 输出：文件内容（原样，不做换行归一）。
     * @return 成功返回 true。
     */
    bool readIncluded(const std::string &path, std::size_t line,
                      std::string *out)
    {
        std::filesystem::path resolved=std::filesystem::u8path(path);
        if (resolved.is_relative() && !m_baseDir.empty())
        {
            resolved = std::filesystem::u8path(m_baseDir) / resolved;
        }
        std::ifstream stream(resolved, std::ios::binary);
        if (!stream)
        {
            fail("include 的文件打不开: " + resolved.u8string(), line);
            return false;
        }
        *out = std::string((std::istreambuf_iterator<char>(stream)),
                           std::istreambuf_iterator<char>());
        return true;
    }

    /**
     * @brief 找下一个「行首是 %」的行（%epilogue / %main 段的结束位置）。
     * @param text 文本。
     * @param start 搜索起点（某一行的行首）。
     * @return 该行的起始偏移；找不到返回 text.size()。
     */
    static std::size_t findNextDirectiveLine(const std::string &text,
                                             std::size_t start)
    {
        std::size_t lineStart = start;
        while (lineStart < text.size())
        {
            const std::size_t found = text.find('\n', lineStart);
            const std::size_t lineEnd =
                (found == std::string::npos) ? text.size() : found;
            const std::string body =
                strip(text.substr(lineStart, lineEnd - lineStart));
            if (!body.empty() && (body[0] == '%'))
            {
                return lineStart;
            }
            if (found == std::string::npos)
            {
                break;
            }
            lineStart = found + 1u;
        }
        return text.size();
    }

    /**
     * @brief 在一段正文里用简单词法扫描找 `main` 定义。
     *
     * 只看行首（忽略空白与注释）那一段声明符：`main(...)`、`int main(...)`、
     * `static int main (...)` 都算命中；一旦遇到 `=`、`(`、`;` 这类说明「不在
     * 声明里」的字符就停止看这一行。不做完整的 C 声明符解析——宁可误报也不
     * 漏报，因为漏报会在链接期炸出两份 main。
     * @param body 正文。
     * @param bodyOffset 正文在整份文本里的起始偏移（用于换算原文件行号）。
     * @return 命中的原文件行号；没命中返回 0。
     */
    std::size_t findMainDefinition(const std::string &body,
                                   std::size_t bodyOffset) const
    {
        std::size_t index = 0u;
        bool inBlockComment = false;
        while (index <= body.size())
        {
            const std::size_t lineStart = index;
            const std::size_t found = body.find('\n', lineStart);
            const std::size_t lineEnd =
                (found == std::string::npos) ? body.size() : found;
            std::size_t cursor = lineStart;
            bool matched = false;
            while (cursor < lineEnd)
            {
                const char ch = body[cursor];
                if (inBlockComment)
                {
                    const std::size_t close = body.find("*/", cursor);
                    if ((close == std::string::npos) || (close >= lineEnd))
                    {
                        cursor = lineEnd;
                        break;
                    }
                    inBlockComment = false;
                    cursor = close + 2u;
                    continue;
                }
                if ((ch == ' ') || (ch == '\t') || (ch == '\r'))
                {
                    ++cursor;
                    continue;
                }
                if ((ch == '/') && (cursor + 1u < lineEnd) &&
                    (body[cursor + 1u] == '/'))
                {
                    break;
                }
                if ((ch == '/') && (cursor + 1u < lineEnd) &&
                    (body[cursor + 1u] == '*'))
                {
                    inBlockComment = true;
                    cursor += 2u;
                    continue;
                }
                // 取一个标识符；不是标识符开头（`= `、`(`、`;` 之类）就不再
                // 看这一行——那些位置不可能是在声明函数
                if (!isIdStart(body[cursor]))
                {
                    break;
                }
                std::size_t after = cursor + 1u;
                while ((after < lineEnd) && isIdPart(body[after]))
                {
                    ++after;
                }
                const std::string word = body.substr(cursor, after - cursor);
                if (word == "main")
                {
                    if (after >= lineEnd)
                    {
                        matched = true;
                    }
                    else
                    {
                        const char next = body[after];
                        matched = (next == '(') || (next == ' ') ||
                                  (next == '\t') || (next == '\r');
                    }
                    break;
                }
                // 只允许「类型 / 限定符 + 空白」这样的前缀，遇到别的东西
                // （`=`、`(`、`;`……）就认为这行不是函数声明
                if ((after < lineEnd) && (body[after] != ' ') &&
                    (body[after] != '\t') && (body[after] != '\r'))
                {
                    break;
                }
                cursor = after;
            }
            if (matched)
            {
                return lineOf(bodyOffset + lineStart);
            }
            if (found == std::string::npos)
            {
                break;
            }
            index = found + 1u;
        }
        return 0u;
    }

    /**
     * @brief 处理 %epilogue / %main 段标记并原样收集正文。
     * @param text 预处理后的整份文本。
     * @param index 当前位置（行首的 `%`）。
     * @return 收集完之后的扫描位置；不是段标记时返回 -1。
     */
    long long collectSection(const std::string &text, std::size_t index)
    {
        std::string marker;
        if (isMarkerMatch(text, index, "%epilogue"))
        {
            marker = "%epilogue";
        }
        else if (isMarkerMatch(text, index, "%main"))
        {
            marker = "%main";
        }
        else
        {
            return -1;
        }
        const std::size_t markerLine = lineOf(index);
        const std::string tail = markerTail(text, index, marker);
        const bool included = !tail.empty();
        std::size_t bodyStart = 0u;
        std::size_t bodyEnd = 0u;
        std::size_t nextIndex = 0u;
        std::string body;
        if (included)
        {
            // `%epilogue include "…"` / `%main include "…"`：段正文来自文件，
            // 标记行后面就接着写别的指令
            bodyStart = markerLine;   // 正文报错位置用标记行
            nextIndex = collectIncludedBody(text, index, marker, tail, &body);
            if (failed()) return -1;
        }
        else
        {
            const std::size_t found = text.find('\n', index);
            bodyStart = (found == std::string::npos) ? text.size() : found + 1u;
            bodyEnd = findNextDirectiveLine(text, bodyStart);
            nextIndex = bodyEnd;
            body = text.substr(bodyStart, bodyEnd - bodyStart);
        }

        // 段正文到「下一个行首是 % 的行」为止：那个终止行如果是 %epilogue，
        // 要么是重复的 epilogue，要么是 %main 里嵌了 epilogue（两者都到文件
        // 末尾，顺序反了语义不清），都报错；是 %main 则是正常的段切换
        // （%main 可以写多段，配合 %if 表达多分支）。
        if (!included && (bodyEnd < text.size()) &&
            isMarkerLine(text, bodyEnd, "%epilogue"))
        {
            const std::size_t line = lineOf(bodyEnd);
            if (marker == "%epilogue")
            {
                fail("%epilogue 只允许一个（第二个 %epilogue 出现在这里）",
                     line);
                return -1;
            }
            fail(
                "%main 与 %epilogue 段不能嵌套：%epilogue 段要写在 %main 段"
                "之前，两者都到文件末尾",
                line);
            return -1;
        }

        if (marker == "%epilogue")
        {
            const std::size_t mainLine =
                findMainDefinition(body, included ? 0u : bodyStart);
            if (mainLine != 0u)
            {
                fail(
                    "%epilogue 里不允许定义 main；请把 main 放到 %main 段里",
                    included ? markerLine : mainLine);
                return -1;
            }
            hasEpilogue = true;
            epilogue = body;
            epilogueLine = markerLine;
        }
        else
        {
            mainBlocks.push_back(body);
            mainBlockLines.push_back(markerLine);
        }
        return static_cast<long long>(nextIndex);
    }

    std::size_t collectCodeBlock(const std::string &text, std::size_t index)
    {
        // 标记行的行尾之后就是正文；正文到「单独成行的 %endcode」为止
        const std::size_t found = text.find('\n', index);
        const std::size_t bodyStart =
            (found == std::string::npos) ? text.size() : found + 1u;
        const std::size_t bodyEnd = findCodeEnd(text, bodyStart);
        if (bodyEnd == std::string::npos)
        {
            fail("%code 段缺少单独成行的结束标记 %endcode", lineOf(index));
            return 0u;
        }
        // 行号记「正文第一行」：前向 #line 指向它，段里的报错才会落在正确的行
        userCodeLines.push_back(
            (bodyStart < text.size()) ? lineOf(bodyStart) : lineOf(index));
        userCodeParts.push_back(text.substr(bodyStart, bodyEnd - bodyStart));
        const std::size_t endLine = text.find('\n', bodyEnd);
        return (endLine == std::string::npos) ? text.size() : endLine + 1u;
    }

    /**
     * @brief 处理「段标记 + include」：正文从文件读，标记行后面直接接着写别的。
     *
     * ```text
     * %code include "user.h"        // 等价于把 user.h 的内容写进 %code … %endcode
     * %epilogue include "epi.cpp"   // 等价于把 epi.cpp 写成 %epilogue 的正文
     * %main include "main.cpp"
     * ```
     *
     * 路径里的相对路径按**文法文件所在目录**解析（与 `%copy` 同一套规则）。
     * @param text 文本。
     * @param index 标记行位置。
     * @param marker 标记名（报错用）。
     * @param tail markerTail() 的结果。
     * @param body 输出：文件内容。
     * @return 扫描继续的位置；出错返回 index（用 failed() 判）。
     */
    std::size_t collectIncludedBody(const std::string &text, std::size_t index,
                                    const std::string &marker,
                                    const std::string &tail, std::string *body)
    {
        const std::size_t markerLine = lineOf(index);
        std::string path;
        if (!parseIncludeTail(tail, &path))
        {
            fail(marker +
                     " 后面只能写 include \"路径\"；段正文要么换行写在下面，"
                     "要么放进文件",
                 markerLine);
            return index;
        }
        if (!readIncluded(path, markerLine, body)) return index;
        const std::size_t found = text.find('\n', index);
        return (found == std::string::npos) ? text.size() : found + 1u;
    }

    /**
     * @brief 处理 %class_body / %endbody 段标记并原样收集正文。
     *
     * 与 `%code … %endcode` 同一套写法：两个标记各自单独成行，正文到单独成行
     * 的 `%endbody` 为止，中间内容原样收集（不解析、不转义、不配平花括号）。
     * 正文会插到解析器类内部（只对 csharp 有意义：放在解析器类里）。
     * @param text 预处理后的整份文本。
     * @param index 当前位置（行首的 `%`）。
     * @return 收集完之后的扫描位置。
     */
    std::size_t collectClassBody(const std::string &text, std::size_t index)
    {
        const std::size_t found = text.find('\n', index);
        const std::size_t bodyStart =
            (found == std::string::npos) ? text.size() : found + 1u;
        const std::size_t bodyEnd = findBodyEnd(text, bodyStart);
        if (bodyEnd == std::string::npos)
        {
            fail("%class_body 段缺少单独成行的结束标记 %endbody",
                 lineOf(index));
            return 0u;
        }
        if (hasClassBody)
        {
            fail("%class_body 只允许一个（第二个 %class_body 出现在这里）",
                 lineOf(index));
            return 0u;
        }
        hasClassBody = true;
        classBody = text.substr(bodyStart, bodyEnd - bodyStart);
        classBodyLine = lineOf(index);
        const std::size_t endLine = text.find('\n', bodyEnd);
        return (endLine == std::string::npos) ? text.size() : endLine + 1u;
    }

    /**
     * @brief 从 start 起按行查找「单独成行」的 %endbody。
     * @param text 全文。
     * @param start 搜索起点（正文第一行的行首）。
     * @return `%endbody` 那一行的起始偏移；找不到返回 npos。
     */
    static std::size_t findBodyEnd(const std::string &text, std::size_t start)
    {
        std::size_t position = start;
        while (position <= text.size())
        {
            const std::size_t lineEnd = text.find('\n', position);
            const std::size_t end =
                (lineEnd == std::string::npos) ? text.size() : lineEnd;
            if (strip(text.substr(position, end - position)) == "%endbody")
            {
                return position;
            }
            if (lineEnd == std::string::npos)
            {
                break;
            }
            position = lineEnd + 1u;
        }
        return std::string::npos;
    }

    /**
     * @brief 从 start 起按行查找「单独成行」的 %endcode。
     * @param text 全文。
     * @param start 搜索起点（正文第一行的行首）。
     * @return `%endcode` 那一行的起始偏移；找不到返回 npos。
     */
    static std::size_t findCodeEnd(const std::string &text, std::size_t start)
    {
        std::size_t position = start;
        while (position <= text.size())
        {
            const std::size_t lineEnd = text.find('\n', position);
            const std::size_t end =
                (lineEnd == std::string::npos) ? text.size() : lineEnd;
            if (strip(text.substr(position, end - position)) == "%endcode")
            {
                return position;
            }
            if (lineEnd == std::string::npos)
            {
                break;
            }
            position = lineEnd + 1u;
        }
        return std::string::npos;
    }

    /**
     * @brief 逐字符扫描文本。
     * @param text .gram 文本。
     */
    void scan(const std::string &text)
    {
        std::size_t index = 0;
        while (!failed() && (index < text.size()))
        {
            const char ch = text[index];
            // 行注释
            if ((ch == '/') && (index + 1 < text.size()) &&
                (text[index + 1] == '/'))
            {
                while ((index < text.size()) && (text[index] != '\n'))
                {
                    ++index;
                }
                continue;
            }
            // 块注释
            if ((ch == '/') && (index + 1 < text.size()) &&
                (text[index + 1] == '*'))
            {
                index += 2;
                while ((index + 1 < text.size()) &&
                       !((text[index] == '*') && (text[index + 1] == '/')))
                {
                    ++index;
                }
                index = std::min(index + 2, text.size());
                continue;
            }
            if ((ch == ' ') || (ch == '\t') || (ch == '\r'))
            {
                ++index;
                continue;
            }
            if (ch == '\n')
            {
                tokens.push_back({"nl", "\n", index, index + 1});
                ++index;
                continue;
            }
            // 旧用户代码块已禁用；只保留 %code ... %endcode。
            if ((ch == '%') && (index + 2 < text.size()) &&
                (text[index + 1] == '{') && (text[index + 2] == '{'))
            {
                fail("%{{ ... }}% 语法已禁用；请改用单独成行的 %code 与 "
                     "%endcode",
                     lineOf(index));
                return;
            }
            // %epilogue / %main：单独成行的段标记，正文原样收集到下一个
            // 「行首是 %」的行或文件末尾。
            if (ch == '%')
            {
                // %code ... %endcode：用户代码正文进 parser_user.h。
                if (isMarkerMatch(text, index, "%code"))
                {
                    const std::string tail = markerTail(text, index, "%code");
                    if (tail.empty())
                    {
                        index = collectCodeBlock(text, index);
                    }
                    else
                    {
                        // `%code include "…"`：正文来自文件，不用写 %endcode
                        std::string body;
                        const std::size_t next =
                            collectIncludedBody(text, index, "%code", tail, &body);
                        if (failed()) return;
                        userCodeLines.push_back(lineOf(index));
                        userCodeParts.push_back(body);
                        index = next;
                    }
                    continue;
                }
                if (isMarkerLine(text, index, "%endcode"))
                {
                    fail(
                        "%endcode 没有对应的 %code（%code 段必须以 %endcode 收尾）",
                        lineOf(index));
                    return;
                }
                // %class_body ... %endbody：原样收集要插进解析器类内部的正文
                if (isMarkerMatch(text, index, "%class_body"))
                {
                    const std::string tail =
                        markerTail(text, index, "%class_body");
                    if (tail.empty())
                    {
                        index = collectClassBody(text, index);
                    }
                    else
                    {
                        std::string body;
                        const std::size_t next = collectIncludedBody(
                            text, index, "%class_body", tail, &body);
                        if (failed()) return;
                        if (hasClassBody)
                        {
                            fail(
                                "%class_body 只允许一个（第二个出现在这里）",
                                lineOf(index));
                            return;
                        }
                        hasClassBody = true;
                        classBody = body;
                        classBodyLine = lineOf(index);
                        index = next;
                    }
                    continue;
                }
                if (isMarkerLine(text, index, "%endbody"))
                {
                    fail(
                        "%endbody 没有对应的 %class_body（%class_body 段必须"
                        "以 %endbody 收尾）",
                        lineOf(index));
                    return;
                }
                const long long next = collectSection(text, index);
                if (failed())
                {
                    return;
                }
                if (next >= 0)
                {
                    index = static_cast<std::size_t>(next);
                    continue;
                }
            }
            const std::size_t matched =
                matchToken(text, index, tokens);
            if (matched == 0)
            {
                fail("无法识别的字符: " + pythonRepr(std::string(1, ch)) +
                         " (位置 " + std::to_string(index) + ")",
                     lineOf(index));
                return;
            }
            index += matched;
        }
    }

    /**
     * @brief 从 index 起匹配一个普通 token 并追加到 tokens。
     * @param text 全文。
     * @param index 起始偏移。
     * @param out 输出 token 列表。
     * @return 匹配消耗的字节数；0 表示无法识别。
     */
    static std::size_t matchToken(const std::string &text, std::size_t index,
                                  std::vector<GramToken> &out)
    {
        const std::size_t size = text.size();
        // 数字字面量
        {
            std::size_t cursor = index;
            bool ok = false;
            if (isDigit(text[cursor]))
            {
                while ((cursor < size) && isDigit(text[cursor]))
                {
                    ++cursor;
                }
                if ((cursor < size) && (text[cursor] == '.'))
                {
                    ++cursor;
                    while ((cursor < size) && isDigit(text[cursor]))
                    {
                        ++cursor;
                    }
                }
                ok = true;
            }
            else if ((text[cursor] == '.') && (cursor + 1 < size) &&
                     isDigit(text[cursor + 1]))
            {
                ++cursor;
                while ((cursor < size) && isDigit(text[cursor]))
                {
                    ++cursor;
                }
                ok = true;
            }
            if (ok)
            {
                // 可选指数部分：e/E [+-]? 数字
                if ((cursor < size) &&
                    ((text[cursor] == 'e') || (text[cursor] == 'E')))
                {
                    std::size_t probe = cursor + 1;
                    if ((probe < size) &&
                        ((text[probe] == '+') || (text[probe] == '-')))
                    {
                        ++probe;
                    }
                    if ((probe < size) && isDigit(text[probe]))
                    {
                        while ((probe < size) && isDigit(text[probe]))
                        {
                            ++probe;
                        }
                        cursor = probe;
                    }
                }
                out.push_back({"num", text.substr(index, cursor - index),
                               index, cursor});
                return cursor - index;
            }
        }
        // 字符串 / 字符字面量
        if ((text[index] == '"') || (text[index] == '\''))
        {
            const char quote = text[index];
            std::size_t cursor = index + 1;
            while (cursor < size)
            {
                if (text[cursor] == '\\')
                {
                    cursor += 2;
                    continue;
                }
                if (text[cursor] == quote)
                {
                    ++cursor;
                    out.push_back({"str", text.substr(index, cursor - index),
                                   index, cursor});
                    return cursor - index;
                }
                ++cursor;
            }
        }
        // 标识符 / 指令名
        if (isIdStart(text[index]))
        {
            std::size_t cursor = index + 1;
            while ((cursor < size) && isIdPart(text[cursor]))
            {
                ++cursor;
            }
            out.push_back({"id", text.substr(index, cursor - index), index,
                           cursor});
            return cursor - index;
        }
        // $$ / $n / $
        if (text[index] == '$')
        {
            std::size_t cursor = index + 1;
            if ((cursor < size) && (text[cursor] == '$'))
            {
                ++cursor;
            }
            else
            {
                while ((cursor < size) && isDigit(text[cursor]))
                {
                    ++cursor;
                }
            }
            out.push_back({"dollar", text.substr(index, cursor - index), index,
                           cursor});
            return cursor - index;
        }
        // 运算符：先试双字符，再试单字符
        static const char *k_twoCharOps[] = {":=", "&&", "||", "<<",
                                             ">>", "<=", ">=", "==", "!="};
        for (const char *op : k_twoCharOps)
        {
            if (text.compare(index, 2, op) == 0)
            {
                out.push_back({"op", op, index, index + 2});
                return 2;
            }
        }
        static const std::string k_singleOps = "+-*/%^&|!~<>=?:(.)[]{},;:";
        if (k_singleOps.find(text[index]) != std::string::npos)
        {
            out.push_back(
                {"op", std::string(1, text[index]), index, index + 1});
            return 1;
        }
        return 0;
    }
};

/**
 * @brief 跳过 %union body 里的注释 / 字符串 / 字符字面量。
 *
 * %union 的成员声明原样贴进生成代码，生成期只做括号配平与成员名提取，
 * 所以这些「内部可能带括号」的片段必须整体跳过，否则注释里的方括号会把
 * 括号计数带偏。
 *
 * @param text 文本。
 * @param index 起始下标（调用方保证不越界）。
 * @return 跳过后的下标；没有可跳过内容时返回原下标。
 */
std::size_t skipCTrivia(const std::string &text, std::size_t index)
{
    const std::size_t size = text.size();
    if ((text[index] == '/') && (index + 1 < size) && (text[index + 1] == '/'))
    {
        std::size_t cursor = index + 2;
        while ((cursor < size) && (text[cursor] != '\n'))
        {
            ++cursor;
        }
        return cursor;
    }
    if ((text[index] == '/') && (index + 1 < size) && (text[index + 1] == '*'))
    {
        std::size_t cursor = index + 2;
        while ((cursor + 1 < size) &&
               !((text[cursor] == '*') && (text[cursor + 1] == '/')))
        {
            ++cursor;
        }
        return std::min(cursor + 2, size);
    }
    if ((text[index] == '"') || (text[index] == '\''))
    {
        const char quote = text[index];
        std::size_t cursor = index + 1;
        while (cursor < size)
        {
            if (text[cursor] == '\\')
            {
                cursor += 2;
                continue;
            }
            if (text[cursor] == quote)
            {
                return cursor + 1;
            }
            ++cursor;
        }
        return size;
    }
    return index;
}

/**
 * @brief 找 %union 最外层配平的那个右花括号。
 *
 * 从 { 之后开始维护 {} / [] / () 三种括号的深度：三种深度都是 0 时遇到的
 * 第一个 } 就是 %union 的结束标记，因此嵌套 struct / union / 数组成员 /
 * 函数指针参数列表里的 } 都不会被误当成结尾。
 *
 * @param text 全文字本。
 * @param openIndex 最外层 { 的下标。
 * @return 配对的 } 下标；到文件结尾都没配平返回 std::string::npos。
 * @retval 无。
 */
std::size_t findUnionClose(const std::string &text, std::size_t openIndex,
                           Error &error)
{
    std::size_t braces = 0;
    std::size_t brackets = 0;
    std::size_t parens = 0;
    std::size_t index = openIndex + 1;
    while (index < text.size())
    {
        const std::size_t skipped = skipCTrivia(text, index);
        if (skipped != index)
        {
            index = skipped;
            continue;
        }
        const char ch = text[index];
        if (ch == '{')
        {
            ++braces;
        }
        else if (ch == '[')
        {
            ++brackets;
        }
        else if (ch == '(')
        {
            ++parens;
        }
        else if (ch == '}')
        {
            if ((braces == 0) && (brackets == 0) && (parens == 0))
            {
                return index;
            }
            if (braces == 0)
            {
                ParseEngine::fail(
                    error,
                    "%union 的括号不配平：位置 " + std::to_string(index) +
                    " 处的 } 之前还有没闭合的方括号 / 圆括号，%union 的结束"
                    "标记被挡住了");
                return std::string::npos;
            }
            --braces;
        }
        else if (ch == ']')
        {
            if (brackets == 0)
            {
                ParseEngine::fail(error,
                                   "%union 的括号不配平：位置 " +
                                       std::to_string(index) +
                                       " 处的 ] 没有匹配的开括号");
                return std::string::npos;
            }
            --brackets;
        }
        else if (ch == ')')
        {
            if (parens == 0)
            {
                ParseEngine::fail(error,
                                   "%union 的括号不配平：位置 " +
                                       std::to_string(index) +
                                       " 处的 ) 没有匹配的开括号");
                return std::string::npos;
            }
            --parens;
        }
        ++index;
    }
    return std::string::npos;
}

/**
 * @brief 把 %union body 按顶层分号切成一个个成员段落。
 *
 * 只有三种括号深度都为 0 时的 ; 才是成员分隔符，顶层逗号直接报错
 * （一个成员段落里只允许一个声明符）。
 *
 * @param body %union 的花括号之间原文。
 * @param baseOffset body 首字符在全文里的下标（用于报错定位）。
 * @return 成员段落列表（已去掉首尾空白，空段落被丢弃）。
 * @retval 无。
 */
std::vector<std::string> splitUnionMembers(const std::string &body,
                                           std::size_t baseOffset,
                                           Error &error)
{
    std::vector<std::string> members;
    std::size_t braces = 0;
    std::size_t brackets = 0;
    std::size_t parens = 0;
    std::size_t start = 0;
    bool hasContent = false;
    std::size_t index = 0;
    while (index < body.size())
    {
        const std::size_t skipped = skipCTrivia(body, index);
        if (skipped != index)
        {
            index = skipped;
            continue;
        }
        const char ch = body[index];
        if (ch == '{')
        {
            ++braces;
        }
        else if (ch == '[')
        {
            ++brackets;
        }
        else if (ch == '(')
        {
            ++parens;
        }
        else if (ch == '}')
        {
            if (braces == 0)
            {
                ParseEngine::fail(error, "%union 的括号不配平：位置 " +
                                             std::to_string(baseOffset + index) +
                                             " 处的 } 没有匹配的开括号");
                return {};
            }
            --braces;
        }
        else if (ch == ']')
        {
            if (brackets == 0)
            {
                ParseEngine::fail(error, "%union 的括号不配平：位置 " +
                                             std::to_string(baseOffset + index) +
                                             " 处的 ] 没有匹配的开括号");
                return {};
            }
            --brackets;
        }
        else if (ch == ')')
        {
            if (parens == 0)
            {
                ParseEngine::fail(error, "%union 的括号不配平：位置 " +
                                             std::to_string(baseOffset + index) +
                                             " 处的 ) 没有匹配的开括号");
                return {};
            }
            --parens;
        }
        else if ((ch == ';') && (braces == 0) && (brackets == 0) &&
                 (parens == 0))
        {
            const std::string segment =
                strip(body.substr(start, index - start));
            if (!segment.empty())
            {
                members.push_back(segment);
            }
            hasContent = false;
            start = index + 1;
            ++index;
            continue;
        }
        else if ((ch == ',') && (braces == 0) && (brackets == 0) &&
                 (parens == 0))
        {
            ParseEngine::fail(
                error,
                "%union 的成员请逐个声明（一段只能有一个声明符），位置 " +
                std::to_string(baseOffset + index) + " 处出现了顶层逗号：" +
                pythonRepr(strip(body.substr(start, index - start))));
            return {};
        }
        if (!std::isspace(static_cast<unsigned char>(ch)))
        {
            hasContent = true;
        }
        ++index;
    }
    if ((braces != 0) || (brackets != 0) || (parens != 0))
    {
        ParseEngine::fail(error, "%union 的括号不配平：从位置 " +
                                     std::to_string(baseOffset) +
                                     " 起有没闭合的括号，请检查成员声明");
        return {};
    }
    if (hasContent)
    {
        ParseEngine::fail(error, "最后一段成员缺少分号：" +
                                      pythonRepr(strip(body.substr(start))));
        return {};
    }
    return members;
}

/**
 * @brief 从一个 %union 成员段落里提取成员名。
 *
 * 只做尾部扫描，不解析 C 声明符：先按需丢掉尾部的数组下标 / 函数指针参数
 * 列表，再取最后一段连续标识符。看不出来就是不清楚的声明符，报错时把原文
 * 附上并提示用 typedef 简化。
 *
 * @param segment 成员段落原文（已去掉结尾分号）。
 * @param ordinal 该段落是第几个成员（从 1 开始，仅用于报错）。
 * @param error 出错时写入错误说明。
 * @return 成员名。
 * @retval 无。提取不出成员名时返回空串并置 error。
 */
std::string extractUnionMemberName(const std::string &segment,
                                   std::size_t ordinal, Error &error)
{
    const auto tailHint = [&]() -> std::string
    {
        return "第 " + std::to_string(ordinal) + " 个成员 '" + segment +
               "' 里提取不出成员名，请确认它是「类型 成员名」形式；"
               "声明符太绕时建议先用 typedef 起个名字再作为成员类型";
    };
    std::string text = strip(segment);
    // 位域：<声明> : <宽度>，宽度不参与成员名提取
    if (!text.empty() && isDigit(text.back()))
    {
        std::size_t cursor = text.size();
        while ((cursor > 0) && isIdPart(text[cursor - 1]))
        {
            --cursor;
        }
        const std::string width = text.substr(cursor);
        std::size_t probe = cursor;
        while ((probe > 0) && std::isspace(static_cast<unsigned char>(
                                            text[probe - 1])))
        {
            --probe;
        }
        if ((probe > 0) && (text[probe - 1] == ':') && !width.empty())
        {
            text = strip(text.substr(0, probe - 1));
        }
    }
    // 尾部的数组下标 / 函数指针参数列表
    bool progressed = true;
    while (progressed)
    {
        progressed = false;
        if (text.empty())
        {
            break;
        }
        const char tail = text.back();
        if ((tail != ']') && (tail != ')'))
        {
            break;
        }
        const char open = (tail == ']') ? '[' : '(';
        std::size_t depth = 0;
        std::size_t cursor = text.size();
        while (cursor > 0)
        {
            const char ch = text[cursor - 1];
            if (ch == tail)
            {
                ++depth;
            }
            else if (ch == open)
            {
                --depth;
                if (depth == 0)
                {
                    break;
                }
            }
            --cursor;
        }
        if (depth != 0)
        {
            ParseEngine::fail(error, tailHint() + "（尾部括号不配平）");
            return {};
        }
        const std::string before =
            strip(text.substr(0, cursor - 1));
        const bool suffix = (!before.empty() &&
                             ((before.back() == ')') || (before.back() == ']')));
        if (suffix)
        {
            // 这一对括号只是后缀（参数列表 / 函数指针数组维度），成员名在
            // 前面的声明符里，继续往前剥。
            text = before;
        }
        else if (tail == ')')
        {
            // 函数指针成员：成员名在这一对括号里面（如 int (*fn)）
            text = strip(text.substr(cursor, text.size() - cursor - 1));
        }
        else
        {
            text = before;
        }
        progressed = true;
    }
    std::size_t cursor = text.size();
    while ((cursor > 0) && isIdPart(text[cursor - 1]))
    {
        --cursor;
    }
    const std::string name = text.substr(cursor);
    if (name.empty() || isDigit(name[0]))
    {
        ParseEngine::fail(error, tailHint());
        return {};
    }
    return name;
}

/**
 * @brief 用空格连接右部符号（仅用于报错信息）。
 * @param symbols 符号列表。
 * @return 连接后的文本；空列表返回空串。
 */
std::string joinSymbolNames(const std::vector<std::string> &symbols)
{
    std::string result;
    for (const std::string &symbol : symbols)
    {
        if (!result.empty())
        {
            result += " ";
        }
        result += symbol;
    }
    return result;
}

/**
 * @brief 收集动作里引用的 $$ / $n。
 *
 * 只按语义动作词法器（action_translator.cpp 的 tokenizeAction）的规则扫一遍
 * 字符串与字符字面量，剩下的 $ 后跟 $ 或数字就是引用；这里不做表达式解析，
 * 目的是在加载期就能按符号查 %union 成员。
 *
 * @param action 动作原文。
 * @return (引用文本, 在动作里的字节偏移) 列表，按出现顺序。
 */
std::vector<std::pair<std::string, std::size_t>>
collectDollarReferences(const std::string &action)
{
    std::vector<std::pair<std::string, std::size_t>> refs;
    std::size_t index = 0;
    while (index < action.size())
    {
        const char ch = action[index];
        // 字符串 / 字符字面量整体跳过（动作里的 "$1" 不是引用）
        if ((ch == '"') || (ch == '\''))
        {
            const char quote = ch;
            std::size_t cursor = index + 1;
            while (cursor < action.size())
            {
                if (action[cursor] == '\\')
                {
                    cursor += 2;
                    continue;
                }
                if (action[cursor] == quote)
                {
                    ++cursor;
                    break;
                }
                ++cursor;
            }
            index = cursor;
            continue;
        }
        if (ch == '$')
        {
            std::size_t cursor = index + 1;
            if ((cursor < action.size()) && (action[cursor] == '$'))
            {
                refs.emplace_back("$$", index);
                index = cursor + 1;
                continue;
            }
            while ((cursor < action.size()) && isDigit(action[cursor]))
            {
                ++cursor;
            }
            if (cursor > index + 1)
            {
                refs.emplace_back(action.substr(index, cursor - index), index);
            }
            index = cursor;
            continue;
        }
        ++index;
    }
    return refs;
}

/**
 * @brief 判断用户 %code 代码块里是否自己定义了 ParserValue_T。
 *
 * 只看「定义」：typedef / 具名 union、struct、enum、class、using、
 * #define。像 ((ParserValue_T)(x)) 这种使用不算，所以没写 %union 的
 * 现有示例（例如 custom_tree.gram）不会误报。
 *
 * @param code 一个 %code 代码块的原文。
 * @return 定义了返回 true。
 */
bool userCodeDefinesParserValueT(const std::string &code)
{
    const std::string target = "ParserValue_T";
    // 先切出「词 / 单字符标点」，注释与字符串整体跳过
    struct Piece
    {
        std::string text;
        bool word = false;
    };
    std::vector<Piece> pieces;
    std::size_t index = 0;
    while (index < code.size())
    {
        const std::size_t skipped = skipCTrivia(code, index);
        if (skipped != index)
        {
            index = skipped;
            continue;
        }
        const char ch = code[index];
        if (std::isspace(static_cast<unsigned char>(ch)))
        {
            ++index;
            continue;
        }
        if (isIdPart(ch) && !isDigit(ch))
        {
            std::size_t cursor = index;
            while ((cursor < code.size()) && isIdPart(code[cursor]))
            {
                ++cursor;
            }
            pieces.push_back({code.substr(index, cursor - index), true});
            index = cursor;
            continue;
        }
        pieces.push_back({std::string(1, ch), false});
        ++index;
    }
    for (std::size_t position = 0; position < pieces.size(); ++position)
    {
        if (!pieces[position].word || (pieces[position].text != target))
        {
            continue;
        }
        // #define ParserValue_T ...
        if ((position >= 2) && (pieces[position - 1].text == "define") &&
            (pieces[position - 2].text == "#"))
        {
            return true;
        }
        if (position == 0)
        {
            continue;
        }
        const std::string &previous = pieces[position - 1].text;
        // union / struct / enum / class ParserValue_T … 与 using ParserValue_T
        if ((previous == "union") || (previous == "struct") ||
            (previous == "enum") || (previous == "class") ||
            (previous == "using") || (previous == "typedef"))
        {
            return true;
        }
        // typedef 的目标名可能隔着 struct { ... } 出现，往前找到同一条
        // 语句（跳过成对花括号里的分号）里的 typedef 才算定义
        std::size_t depth = 0;
        for (std::size_t back = position; back > 0; --back)
        {
            const std::string &piece = pieces[back - 1].text;
            if (!pieces[back - 1].word)
            {
                if (piece == "}")
                {
                    ++depth;
                }
                else if (piece == "{")
                {
                    depth = (depth > 0) ? depth - 1 : 0;
                }
                else if ((piece == ";") && (depth == 0))
                {
                    break;
                }
                continue;
            }
            if (piece == "typedef")
            {
                return true;
            }
        }
    }
    return false;
}

/**
 * @brief 把 token 流解析成 GrammarConfig 的递归下降解析器。
 */
class GramParser
{
public:
    /**
     * @brief 分词并去掉用户代码块 token。
     * @param text .gram 文本（条件编译预处理之后的）。
     * @param lineMap 预处理行 -> 原文件行；空表示没有条件编译指令。
     */
    explicit GramParser(const std::string &text,
                        const std::vector<std::size_t> &lineMap = {},
                        const std::string &baseDir = std::string())
    {
        m_text = text;
        m_lines = SourceLines::build(text, lineMap);
        GramTokenizer tokenizer(text, &m_lines, baseDir);
        // 旧版分词出错是在构造期直接抛 GrammarLoadError，外层一捕获就结束；
        // 现在改成错误值，必须在这里接过来，否则分词错误会被丢掉、解析继续
        // 拿半截 token 流跑下去，报出的错就对不上号了
        if (tokenizer.failed())
        {
            m_error = tokenizer.error();
        }
        m_userCodeParts = tokenizer.userCodeParts;
        m_userBlockLines = tokenizer.userCodeLines;
        m_hasEpilogue = tokenizer.hasEpilogue;
        m_epilogue = tokenizer.epilogue;
        m_epilogueLine = tokenizer.epilogueLine;
        m_mainBlocks = tokenizer.mainBlocks;
        m_mainBlockLines = tokenizer.mainBlockLines;
        m_hasClassBody = tokenizer.hasClassBody;
        m_classBody = tokenizer.classBody;
        m_classBodyLine = tokenizer.classBodyLine;
        for (const GramToken &token : tokenizer.tokens)
        {
            m_tokens.push_back(token);
        }
        m_tokenNames = k_defaultTokenNames;
    }

    /**
     * @brief 解析整份文件。
     * @return 解析出的文法配置。
     */
    GrammarConfig parse();

    /**
     * @brief 是否已经出错。
     * @return 出过错返回 true。
     */
    bool failed() const
    {
        return !m_error.empty();
    }

    /**
     * @brief 取第一个错误。
     * @return 错误值。
     */
    const Error &error() const
    {
        return m_error;
    }

private:
    /**
     * @brief 记录第一个错误（错误槽已满时不覆盖）。
     *
     * 没有显式行号时，顺手把「出错那一刻」的 token 行号记下来：不少指令解析
     * 在发现错误后还会把本行剩下的 token 吃掉（例如 %define 出错后仍会走
     * endDirective 吃掉换行），等语句结束再取当前行号就会串到下一行去。旧版
     * 异常实现是在抛出的那一刻补行号，这里保持同一语义。
     * @param message 错误说明。
     * @param line 原文件行号（1 起）；0 表示位置未知。
     */
    void fail(const std::string &message, std::size_t line = 0u)
    {
        if (m_error.empty() && (line == 0u))
        {
            m_failLine = currentLine();
        }
        ParseEngine::fail(m_error, message, line);
    }

    /**
     * @brief 取错误槽，ParseEngine参的静态辅助函数」使用。
     *
     * 这类辅助函数内部调的是 ParseEngine::fail(error, ...)，绕过了本类的
     * fail()，不会记下出错时的行号；所以在交出错误槽的这一刻先把当前行号
     * 记下来。辅助函数成功时只是白记一个行号，不影响错误内容。
     * @return 错误槽。
     */
    Error &errorSlot()
    {
        if (m_error.empty())
        {
            m_failLine = currentLine();
        }
        return m_error;
    }

    /**
     * @brief 解析一条语句（指令或产生式）。
     */
    void parseStatement();

    /**
     * @brief 全部语句解析完之后做交叉校验并拼出配置。
     * @return 文法配置。
     */
    GrammarConfig validateAndBuild();

    /**
     * @brief 把字节偏移换算成原文件行号。
     * @param offset 字节偏移。
     * @return 行号；没有换算表时返回 0。
     */
    std::size_t lineOf(std::size_t offset) const
    {
        return m_lines.lineOf(offset);
    }

    /**
     * @brief 取当前位置所对应 token 的原文件行号。
     * @return 行号（1 起）；取不到时返回 0。
     */
    std::size_t currentLine() const
    {
        const GramToken *token = peekToken();
        if (token != nullptr)
        {
            return lineOf(token->start);
        }
        if (!m_tokens.empty())
        {
            return lineOf(m_tokens.back().start);
        }
        return 0u;
    }

    SourceLines m_lines; ///< 行号换算表（空映射 = 恒等）
    /**
     * @brief 取相对当前位置偏移的 token。
     * @param offset 偏移量。
     * @return 命中的 token 指针，否则 nullptr。
     */
    const GramToken *peekToken(std::size_t offset = 0) const
    {
        const std::size_t index = m_pos + offset;
        if (index < m_tokens.size())
        {
            return &m_tokens[index];
        }
        return nullptr;
    }

    /**
     * @brief 取偏移处 token 的原文；越界返回 "None"（与 Python 打印一致）。
     * @param offset 偏移量。
     * @return token 原文或 "None"。
     */
    std::string peekText(std::size_t offset = 0) const
    {
        const GramToken *token = peekToken(offset);
        return (token == nullptr) ? std::string("None") : token->text;
    }

    /**
     * @brief 取偏移处 token 类别；越界返回空串。
     * @param offset 偏移量。
     * @return token 类别。
     */
    std::string peekKind(std::size_t offset = 0) const
    {
        const GramToken *token = peekToken(offset);
        return (token == nullptr) ? std::string() : token->kind;
    }

    /**
     * @brief 跳过连续的换行 token。
     */
    void skipNl()
    {
        while (peekText() == "\n")
        {
            ++m_pos;
        }
    }

    /**
     * @brief 若当前 token 等于给定文本则消耗。
     * @param text 期望的 token 原文。
     * @return 命中并消耗返回 true。
     */
    bool accept(const std::string &text)
    {
        if (peekText() == text)
        {
            ++m_pos;
            return true;
        }
        return false;
    }

    /**
     * @brief 期望当前 token 等于给定文本。
     * @param text 期望的 token 原文。
     */
    void expect(const std::string &text)
    {
        if (!accept(text))
        {
            fail("期望 '" + text + "'，实际得到 '" + peekText() + "'");
        }
    }

    /**
     * @brief 解析 %token 指令。
     */
    void parseTokenDirective();
    /**
     * @brief 解析 %left / %right / %nonassoc。
     */
    void parsePrecedenceDirective();
    /**
     * @brief 解析 %expect 指令。
     */
    void parseExpectDirective();
    /**
     * @brief 读取正则字面量并用自带编译器检查一遍。
     * @param what 出错信息里使用的指令名。
     * @return 解码后的正则文本。
     */
    std::string readRegexPattern(const std::string &what);
    /**
     * @brief 记一条「自带正则编译器没认下」的诊断（**不中断解析**）。
     *
     * 只有 C99 真的要用自带编译器（`source/src/regex_dfa.cpp`）编出来的 DFA 表，所以
     * 只有 C99 会因为它是硬错误——由 `main.cpp` 按目标语言分流；其余四档把正则
     * 原样交给各自语言的正则库，只打警告、照常生成。
     * @param message 说明文本（与从前那条报错逐字相同）。
     */
    void regexDiagnostic(const std::string &message);
    /**
     * @brief 消费指令末尾的换行；若还有别的内容则报错。
     * @param what 出错信息里使用的指令名。
     */
    void endDirective(const std::string &what);
    /**
     * @brief 登记一个由正则定义的 Token。
     * @param name Token 名。
     * @param pattern 正则文本。
     */
    void registerRegexToken(const std::string &name,
                            const std::string &pattern);
    /**
     * @brief 解析 %regex 指令。
     */
    void parseRegexDirective();
    /**
     * @brief 解析 %identifier 指令。
     */
    void parseIdentifierDirective();
    /**
     * @brief 解析 %commit 指令（匹配到的文本收进容器，不进语法层）。
     *
     * 三种形式：`%commit <type> "<pattern>"`、`%commit "<pattern>"`
     * （类型缺省 "default"）、`%commit skip "<pattern>"`（匹配即丢）。
     */
    void parseCommitDirective();
    /**
     * @brief 解析 %trivia 指令（匹配即丢，不进容器、不进语法层）。
     *
     * `%trivia "<pattern>"` 与 `%commit skip "<pattern>"` 完全等价，只是
     * 语义更清楚：推荐用 %trivia 声明 trivia。
     */
    void parseTriviaDirective();
    /**
     * @brief 登记一条 %commit / %trivia 规则（含与 Token 的模式冲突检查）。
     * @param what 指令名（`%commit` / `%trivia`，只用于报错）。
     * @param type 容器元素的类型名（skip / trivia 不进容器，随便给）。
     * @param pattern 匹配模式（已通过正则检查）。
     * @param isSkip 匹配即丢（不进容器）。
     * @param line 指令所在的原文件行号。
     */
    void registerCommitRule(const std::string &what, const std::string &type,
                            const std::string &pattern, bool isSkip,
                            std::size_t line, const std::string &alias = {});
    /**
     * @brief 登记固定字符串 Token。
     * @param tokenName Token 名。
     * @param literalText 字面量原文。
     * @param what 出错信息里使用的指令名。
     */
    void registerLiteralToken(const std::string &tokenName,
                              const std::string &literalText,
                              const std::string &what);
    /**
     * @brief 解析 %define 指令。
     */
    void parseDefineDirective();
    /**
     * @brief 解析 %space 指令（取 Token 前跳过的空白字符集）。
     */
    void parseSpaceDirective();
    /**
     * @brief 解析 %start 指令。
     */
    void parseStartDirective();
    /** @brief 解析 `%example "..."` 文档示例输入。 */
    void parseExampleDirective();
    /**
     * @brief 解析 %tree 指令。
     */
    void parseTreeDirective();

    /**
     * @brief 解析不带参数的开关键指令（%teach / %mermaid / %cpp_only）。
     * @param name 指令名（用于报错）。
     * @param target 目标开关。
     */
    void parseToggleDirective(const std::string &name, bool &target);
/** 解析 %destructor <成员> { 清理代码 }。 */
void parseDestructorDirective();
    /**
     * @brief 解析 %mode 指令。
     */
void parseModeDirective();
void parseLookaheadDirective();

    void parseLangDirective();
    /**
     * @brief 解析 `%build_files on|off` 指令。
     *
     * 控制是否导出 CMakeLists / Makefile / Cargo.toml / *.csproj / pom.xml /
     * package.json / go.mod 这类工程构建文件；缺省 `on`。命令行
     * `--build-files` / `--no-build-files` 覆盖文法里的声明。
     */
    void parseBuildFilesDirective();
    /**
     * @brief 解析 %package 指令（C# 的 namespace 名）。
     */
    void parsePackageDirective();
    /**
     * @brief 解析 %import 指令（C# 的 using；可重复）。
     */
    void parseImportDirective();
    /**
     * @brief 读一行里剩下的原文（%package / %import 的点分名字会被切成多个 token）。
     * @return 去掉首尾空白后的整行文本。
     */
    std::string readDirectiveRestOfLine();
    /**
     * @brief 解析 %namespace 指令（C++17 产物的命名空间名 + 文件名前缀）。
     */
    void parseNamespaceDirective();
    void parsePrefixDirective();

    /**
     * @brief 解析 %class 指令（Python 产物的解析器类名）。
     */
    void parseClassDirective();
    /**
     * @brief 解析 %cpp_lexer 指令（C++17 产物的词法引擎：std / dfa）。
     */
    void parseCppLexerDirective();
    /**
     * @brief %runtime 指令已删除：语义值运行时只有 custom（也是缺省值），
     *        写了直接报错并给出提示。
     */
    void parseRuntimeDirective();
    /**
     * @brief 解析 %value_type 指令（语义值类型）。
     */
    void parseValueTypeDirective();
    /**
     * @brief 解析 %sync 指令（错误恢复的同步 Token）。
     */
    void parseSyncDirective();
    /**
     * @brief 解析 %copy 指令。
     */
    void parseCopyDirective();
    /**
     * @brief 解析 %var_max_count 指令。
     *
     * builtin 运行时已被移除，这个指令只用于给出明确的迁移提示。
     */
    void parseVarMaxCountDirective();
    /**
     * @brief 解析容量宏指令（%max_stack_depth / %max_errors / %error_recovery_tokens）。
     * @param directive 指令名（如 `%max_errors`）。
     * @param minimum 允许的最小值。
     * @param maximum 允许的最大值。
     * @param target 输出目标。
     */
    void parseCapacityDirective(const std::string &directive, int minimum,
                                int maximum, std::optional<int> &target);
    /**
     * @brief 解析 %union { ... } 指令。
     */
    void parseUnionDirective();
    /**
     * @brief 解析 %type <成员> 符号... 指令。
     */
    void parseTypeDirective();
    /**
     * @brief 读一串非终结符名（%type / %sync 共用）。
     * @param keyword 指令名（仅用于报错）。
     * @param names 输出名字列表（保持出现顺序）。
     * @param what 名字的类别（仅用于报错，例如 "非终结符名" / "Token 名"）。
     */
    void readSymbolNameList(const std::string &keyword,
                            std::vector<std::string> &names,
                            const std::string &what = "非终结符名");
    /**
     * @brief 记录「符号 -> %union 成员」绑定。
     * @param symbol 符号名（终结符或非终结符）。
     * @param member %union 成员名。
     * @param what 发起绑定的指令名（仅用于报错）。
     * @param line 指令所在的原文件行号（报错定位用）。
     */
    void bindSymbolMember(const std::string &symbol,
                          const std::string &member,
                          const std::string &what, std::size_t line);
    /**
     * @brief %union / %type / %token <...> 的收尾校验。
     */
    void validateUnionUsage();
    /**
     * @brief 解析 %var 声明。
     *
     * builtin 运行时已被移除，这个指令只用于给出明确的迁移提示。
     */
    void parseVarDirective();
    /**
     * @brief 解析一条产生式。
     */
    void parseProduction();
    /**
     * @brief 解析 | 分支写法中单个分支的右部。
     * @param lhs 左部非终结符（仅用于报错）。
     * @return 右部符号列表与可选的 %prec 覆盖 Token。
     */
    std::pair<std::vector<std::string>, std::optional<std::string>>
    parseAlternativeRhs(const std::string &lhs);
    /**
     * @brief 解析 | 分支写法中单个分支的语义动作。
     * @param lhs 左部非终结符（仅用于报错）。
     * @return 动作文本；该分支没有动作时为空串。
     */
    std::string parseAlternativeAction(const std::string &lhs);
    /**
     * @brief 读取 { ... } 动作块。
     * @return 去掉外层花括号并 strip 后的动作文本。
     */
    std::string readActionBlock();
    /**
     * @brief 读取无大括号动作（读到行尾）。
     * @param lhs 左部非终结符（仅用于报错）。
     * @return 动作文本。
     */
    std::string readInlineAction(const std::string &lhs);

    std::string m_text;                          ///< 原始文本
    std::size_t m_failLine = 0u;                 ///< 出错那一刻的行号（补行号用）
    /// 自带正则编译器没认下的正则（不含行号的说明 + 行号）；见 regexDiagnostic
    std::vector<Error> m_regexDiagnostics;
    std::vector<GramToken> m_tokens;             ///< 去掉用户代码块后的 token
    std::vector<std::string> m_userCodeParts;    ///< 用户代码块内容
    std::vector<std::string> m_tokenNames;       ///< 终结符列表
    std::map<std::string, std::string> m_tokenAliases;
    std::map<std::string, std::vector<std::string>> m_tokenDefs; ///< 固定字面量
    std::map<std::string, std::string> m_literalOwners; ///< 字面量归属
    std::optional<std::string> m_start;          ///< 起始符号
    std::size_t m_startOffset = 0u;              ///< %start 取到的符号在预处理文本里的偏移
    std::optional<std::string> m_exampleInput;   ///< %example 解码后的示例输入
    std::map<std::string, std::vector<Production>> m_rules; ///< 规则表
    std::optional<std::string> m_firstRule;      ///< 第一条规则名
    bool m_treeEnabled = false;                  ///< %tree
    bool m_teachEnabled = false;                 ///< %teach
    bool m_mermaidEnabled = false;               ///< %mermaid
    std::optional<int> m_maxStackDepth;          ///< %max_stack_depth
    std::optional<int> m_maxErrors;              ///< %max_errors
    std::optional<int> m_errorRecoveryTokens;    ///< %error_recovery_tokens
    std::vector<std::string> m_syncTokens;       ///< %sync 的 Token（声明序）
    bool m_syncDeclared = false;                 ///< 是否写过 %sync
    bool m_usesError = false;                    ///< 文法里是否用了 error 产生式
    std::vector<UnionMember> m_unionMembers;     ///< %union 成员（声明序）
    std::string m_unionBody;                     ///< %union 原文（不含最外层花括号）
    bool m_unionDeclared = false;                ///< 是否写过 %union
    bool m_hasEpilogue = false;                  ///< 是否写过 %epilogue
    std::string m_epilogue;                      ///< %epilogue 正文
    std::size_t m_epilogueLine = 0u;             ///< %epilogue 标记的原文件行号
    std::vector<std::string> m_mainBlocks;       ///< %main 正文（按出现顺序）
    std::vector<std::size_t> m_mainBlockLines;   ///< 每个 %main 的原文件行号
    std::vector<std::size_t> m_userBlockLines;   ///< 每个 %code 块的原文件行号
    std::size_t m_unionLine = 0u;                ///< %union 的原文件行号
    std::map<std::string, std::string> m_symbolValueMember; ///< 符号 -> 成员
    std::map<std::string, std::size_t> m_symbolMemberLines; ///< 符号绑成员的行号
    std::string m_mode = "ielr";                 ///< 生成模式（缺省 IELR(1)）
    bool m_modeDeclared = false;                 ///< 是否声明过 %mode
    int m_lookahead = 2;                         ///< %lookahead：LR(k) / LALR(k) 的 k
    bool m_lookaheadDeclared = false;            ///< 是否声明过 %lookahead
    bool m_errorContext = false;                 ///< %error_context：错误里带上下文行
    bool m_errorContextDeclared = false;         ///< 是否声明过 %error_context
    std::map<std::string, std::string> m_destructors; ///< %destructor：成员 -> 清理片段
    std::map<std::string, std::size_t> m_destructorLines; ///< %destructor 的原文件行号
    int m_modeLookahead = -1;                    ///< 模式名里写的 k（LL(2) / SLR(k)）；-1 = 没写
    std::string m_lang = "cpp17";                ///< 目标语言（c / cpp17）
    bool m_langDeclared = false;                 ///< 是否声明过 %lang
    bool m_buildFiles = true;                    ///< %build_files：是否导出工程构建文件
    bool m_buildFilesDeclared = false;           ///< 是否声明过 %build_files
    bool m_cppOnly = false;                      ///< 是否声明过 %cpp_only
    std::string m_cppNamespace;                  ///< %namespace（空 = 退化成现状）
    bool m_cppNamespaceDeclared = false;         ///< 是否声明过 %namespace
    std::string m_cPrefix;                       ///< %prefix（C99）
    bool m_cPrefixDeclared = false;              ///< 是否声明过 %prefix
    std::string m_cppLexer = "std";              ///< %cpp_lexer（std / dfa）
    bool m_cppLexerDeclared = false;             ///< 是否声明过 %cpp_lexer
    std::string m_pythonClass;                   ///< %class（空 = 缺省 Parser）
    bool m_pythonClassDeclared = false;          ///< 是否声明过 %class
    std::string m_packageName;                   ///< %package（空 = 没写）
    bool m_packageDeclared = false;              ///< 是否声明过 %package
    std::size_t m_packageLine = 0u;              ///< %package 的原文件行号
    std::vector<std::string> m_imports;          ///< %import 的名字（按出现顺序）
    std::vector<std::size_t> m_importLines;      ///< 每个 %import 的原文件行号
    bool m_hasClassBody = false;                 ///< 是否写过 %class_body
    std::string m_classBody;                     ///< %class_body 正文（原样）
    std::size_t m_classBodyLine = 0u;            ///< %class_body 标记的原文件行号
    std::optional<std::string> m_valueType;      ///< %value_type
    std::map<std::string, int> m_precedence;   ///< Token 优先级
    std::map<std::string, std::string> m_associativity; ///< Token 结合性
    std::map<std::pair<std::string, int>, std::string>
        m_productionPrec;                        ///< %prec 覆盖
    std::optional<int> m_expectConflicts;        ///< %expect
    int m_precedenceLevel = 0;                   ///< 当前优先级级别
    std::map<std::string, std::string> m_regexDefs; ///< 正则 Token
    std::optional<std::string> m_identifierPattern;   ///< %identifier
    std::vector<CommitSpec> m_commits;           ///< %commit（声明序）
    std::map<std::string, int> m_tokenLexOrder;  ///< Token 名 -> 词法声明序
    int m_lexOrder = 0;                          ///< 下一个词法声明的序号
    bool m_spaceDeclared = false;                ///< 是否写过 %space
    std::string m_spaceChars;                    ///< %space 的跳过字符集（去重）
    std::vector<CopySpec> m_copies;              ///< %copy 指令
    std::size_t m_pos = 0;                       ///< 当前 token 下标
    Error m_error;                               ///< 第一个错误
};

// =====================================================================
// 指令解析
// =====================================================================

void GramParser::parseTokenDirective()
{
    const std::size_t directiveLine =
        (peekToken() == nullptr) ? 0u : lineOf(peekToken()->start);
    ++m_pos;
    // %union 成员标签：%token <成员名> A B 里标签作用于后续符号（yacc 语义），
    // 直到下一个标签或本指令结束
    std::string tag;
    bool tagUsed = false;
    auto readAlias = [&](const std::string &name) {
        if ((peekKind() == "id") && (peekText() == "alias") &&
            (peekKind(1) == "str"))
        {
            ++m_pos;
            const std::string value = GrammarLoader::decodeStringLiteral(peekText());
            ++m_pos;
            m_tokenAliases[name] = value;
        }
    };
    while (true)
    {
        if (m_pos >= m_tokens.size())
        {
            break;
        }
        const std::string token = peekText();
        if (token == "\n")
        {
            ++m_pos;
            break;
        }
        if (token == ",")
        {
            ++m_pos;
            continue;
        }
        // 形式零：%token <成员名> ...
        if (token == "<")
        {
            if ((peekKind(1) != "id") || (peekText(2) != ">"))
            {
                fail(
                    "%token 后的 <成员名> 写法不完整，应写成 %token <成员名> "
                    "NAME ...");
                return;
            }
            tag = peekText(1);
            tagUsed = false;
            m_pos += 3;
            continue;
        }
        // 形式一：%token [<成员名>] regex NAME "pattern"
        if ((token == "regex") && (peekKind(1) == "id") &&
            (peekKind(2) == "str"))
        {
            const std::string name = peekText(1);
            m_pos += 2;
            const std::string pattern =
                readRegexPattern("%token regex " + name);
            registerRegexToken(name, pattern);
            readAlias(name);
            if (!tag.empty())
            {
                bindSymbolMember(name, tag, "%token", directiveLine);
                tagUsed = true;
            }
            continue;
        }
        // 形式二：%token [<成员名>] NAME "字面量"
        if ((peekKind() == "id") && (token[0] != '%') &&
            (peekKind(1) == "str"))
        {
            const std::string name = token;
            const std::string literalText = peekText(1);
            m_pos += 2;
            registerLiteralToken(name, literalText, "%token " + name);
            readAlias(name);
            if (!tag.empty())
            {
                bindSymbolMember(name, tag, "%token", directiveLine);
                tagUsed = true;
            }
            continue;
        }
        if ((peekKind() != "id") || (token[0] == '%'))
        {
            fail(
                "%token 只能包含标识符、字面量或 regex 定义，实际得到 '" +
                token + "'");
            return;
        }
        if (token == k_errorTokenName)
        {
            fail(
                "error 是恢复用的保留伪终结符，不能用作 Token 名");
            return;
        }
        bool known = false;
        for (const std::string &name : m_tokenNames)
        {
            known = known || (name == token);
        }
        if (!known)
        {
            m_tokenNames.push_back(token);
        }
        if (!tag.empty())
        {
            bindSymbolMember(token, tag, "%token", directiveLine);
            tagUsed = true;
        }
        ++m_pos;
        readAlias(token);
    }
    if (!tag.empty() && !tagUsed)
    {
        fail("%token <" + tag +
             "> 后面没有跟任何 Token 名");
        return;
    }
}

void GramParser::parseSyncDirective()
{
    ++m_pos;
    std::vector<std::string> names;
    readSymbolNameList("%sync", names, "Token 名");
    for (const std::string &name : names)
    {
        if (name == k_errorTokenName)
        {
            fail(
                "%sync 不能写 error：error 是恢复用的保留伪终结符，"
                "不是真正的 Token");
            return;
        }
        bool known = false;
        for (const std::string &existing : m_syncTokens)
        {
            known = known || (existing == name);
        }
        if (!known)
        {
            m_syncTokens.push_back(name);
        }
    }
    m_syncDeclared = true;
}

void GramParser::parsePrecedenceDirective()
{
    const std::string keyword = peekText();
    ++m_pos;
    std::string assoc;
    if (keyword == "%left")
    {
        assoc = "left";
    }
    else if (keyword == "%right")
    {
        assoc = "right";
    }
    else
    {
        assoc = "nonassoc";
    }
    std::vector<std::string> names;
    while (m_pos < m_tokens.size())
    {
        const std::string token = peekText();
        if (token == "\n")
        {
            ++m_pos;
            break;
        }
        if (token == ",")
        {
            ++m_pos;
            continue;
        }
        if ((peekKind() != "id") || (token[0] == '%'))
        {
            fail(keyword + " 只能包含 Token 名，实际得到 '" +
                 token + "'");
            return;
        }
        names.push_back(token);
        ++m_pos;
    }
    if (names.empty())
    {
        fail(keyword + " 至少需要一个 Token 名");
        return;
    }
    for (const std::string &name : names)
    {
        if (m_precedence.count(name))
        {
            fail("Token " + name + " 的优先级重复声明");
            return;
        }
        ++m_precedenceLevel;
        m_precedence.insert_or_assign(name, m_precedenceLevel);
        m_associativity.insert_or_assign(name, assoc);
        bool known = false;
        for (const std::string &existing : m_tokenNames)
        {
            known = known || (existing == name);
        }
        if (!known)
        {
            m_tokenNames.push_back(name);
        }
    }
}

void GramParser::parseExpectDirective()
{
    const std::string keyword = peekText();
    ++m_pos;
    if (peekKind() != "num")
    {
        fail(keyword + " 后应跟非负整数，实际得到 '" +
             peekText() + "'");
        return;
    }
    double raw = 0.0;
    if (!parseNumText(peekText(), raw) ||
        (raw > static_cast<double>(std::numeric_limits<int>::max())))
    {
        fail(keyword + " 后应跟非负整数，实际得到 '" + peekText() + "'");
        return;
    }
    const int value = static_cast<int>(raw);
    if (value < 0)
    {
        fail("%expect 不能为负数");
        return;
    }
    if (m_expectConflicts.has_value())
    {
        fail("%expect 只能声明一次");
        return;
    }
    m_expectConflicts = value;
    ++m_pos;
    if (peekText() == "\n")
    {
        ++m_pos;
    }
    else if (peekToken() != nullptr)
    {
        fail("%expect 后有多余内容 '" + peekText() + "'");
        return;
    }
}

void GramParser::regexDiagnostic(const std::string &message)
{
    Error item;
    item.message = message;
    item.line = currentLine();
    m_regexDiagnostics.push_back(item);
}

std::string GramParser::readRegexPattern(const std::string &what)
{
    const std::string quoted = peekText();
    if ((peekKind() != "str") || quoted.empty() ||
        ((quoted[0] != '"') && (quoted[0] != '\'')))
    {
        fail(what +
             " 后应跟引号包起来的正则，例如 \"[0-9]+\"");
        return {};
    }
    ++m_pos;
    const std::string pattern = GrammarLoader::decodeRegexLiteral(quoted);
    // 自带编译器认不下这条正则时**不在这里报错**：记成诊断，由 main.cpp 按目标
    // 语言分流（C99 要拿它编 DFA 表，那边是硬错误；其余四档只警告）。正则层的
    // 说明统一加「<谁> 的正则 "<原文>" 有误：」前缀，指到出错的指令上。
    Error regexError;
    std::shared_ptr<RegexNode> ast;
    if (!RegexDfa::parse(pattern, ast, regexError))
    {
        if (RegexVm::unicodeMode(pattern))
        {
            fail(what + " 的 Unicode 正则 " + quoted + " 有误：" + regexError.message);
            return {};
        }
        regexDiagnostic(what + " 的正则 " + quoted + " 有误：" +
                        regexError.message);
        return pattern;
    }
    if (RegexVm::required(ast))
    {
        if (RegexVm::nullable(ast))
        {
            fail(what + " 的正则 " + quoted + " 可能零宽匹配，Lexer 无法前进");
            return {};
        }
        return pattern;
    }
    std::vector<RegexDfa::RangeList> classSets;
    RegexDfa::collectClassSets(ast, classSets);
    const auto classes = RegexDfa::charClasses(classSets);
    DfaProgram dfa;
    if (!RegexDfa::buildDfa(pattern, classes.first, dfa, regexError))
    {
        regexDiagnostic(what + " 的正则 " + quoted + " 有误：" +
                        regexError.message);
        return pattern;
    }
    if (dfa.accept.count(dfa.start))
    {
        // 「能匹配空串」与用哪个引擎无关：哪个语言的 Lexer 都没法前进
        fail(what + " 的正则 " + quoted +
             " 可以匹配空串，Lexer 无法前进");
        return {};
    }
    return pattern;
}

void GramParser::endDirective(const std::string &what)
{
    if (peekText() == "\n")
    {
        ++m_pos;
    }
    else if (peekToken() != nullptr)
    {
        fail(what + " 后有多余内容 '" + peekText() + "'");
        return;
    }
}

void GramParser::registerRegexToken(const std::string &name,
                                    const std::string &pattern)
{
    if (name == k_errorTokenName)
    {
        fail(
            "error 是恢复用的保留伪终结符，不能用作 Token 名");
        return;
    }
    if (m_tokenDefs.count(name) || m_regexDefs.count(name))
    {
        fail("Token " + name + " 重复定义");
        return;
    }
    for (const CommitSpec &commit : m_commits)
    {
        if (commit.pattern == pattern)
        {
            fail("正则 " + pythonRepr(pattern) + " 已经由第 " +
                     std::to_string(commit.line) +
                     " 行的 %commit 声明：同一个模式不能既当 Token 又当 "
                     "commit");
            return;
        }
    }
    bool known = false;
    for (const std::string &existing : m_tokenNames)
    {
        known = known || (existing == name);
    }
    if (!known)
    {
        m_tokenNames.push_back(name);
    }
    m_regexDefs.insert_or_assign(name, pattern);
    if (m_tokenLexOrder.count(name) == 0)
    {
        m_tokenLexOrder.insert_or_assign(name, m_lexOrder++);
    }
}

void GramParser::parseRegexDirective()
{
    ++m_pos;
    if ((peekKind() != "id") || (peekText()[0] == '%'))
    {
        fail("%regex 后应跟 Token 名");
        return;
    }
    const std::string name = peekText();
    ++m_pos;
    const std::string pattern = readRegexPattern("%regex " + name);
    registerRegexToken(name, pattern);
    if ((peekKind() == "id") && (peekText() == "alias") &&
        (peekKind(1) == "str"))
    {
        ++m_pos;
        m_tokenAliases[name] =
            GrammarLoader::decodeStringLiteral(peekText());
        ++m_pos;
    }
    endDirective("%regex " + name);
}

void GramParser::parseIdentifierDirective()
{
    ++m_pos;
    if (m_identifierPattern.has_value())
    {
        fail("%identifier 只能声明一次");
        return;
    }
    m_identifierPattern = readRegexPattern("%identifier");
    if ((peekKind() == "id") && (peekText() == "alias") &&
        (peekKind(1) == "str"))
    {
        ++m_pos;
        m_tokenAliases["VARIABLE"] =
            GrammarLoader::decodeStringLiteral(peekText());
        ++m_pos;
    }
    endDirective("%identifier");
}

// %commit / %trivia 现在五档（C99 / C++17 / Python / C# / Rust）
// 都支持：词法声明序在这里生成，各后端的容器与位置信息 API 见 doc/07-codegen.md §17。
void GramParser::parseCommitDirective()
{
    const std::size_t directiveLine =
        (peekToken() == nullptr) ? 0u : lineOf(peekToken()->start);
    ++m_pos;
    CommitSpec spec;
    spec.line = directiveLine;
    bool explicitType = false;
    if (peekKind() == "str")
    {
        // 形式二：%commit "<pattern>"，类型缺省 "default"
        spec.type = "default";
    }
    else if ((peekKind() == "id") && (peekText() == "skip"))
    {
        // 形式三：%commit skip "<pattern>"，匹配即丢
        spec.type = "skip";
        spec.isSkip = true;
        ++m_pos;
        explicitType = true;
    }
    else if ((peekKind() == "id") && (peekText()[0] != '%'))
    {
        // 形式一：%commit <type> "<pattern>"
        spec.type = peekText();
        ++m_pos;
        explicitType = true;
    }
    else
    {
        fail(
            "%commit 后面要跟匹配模式，或者先写类型名 / skip（例如 "
            "%commit block \"/\\*([^*]|\\*+[^*/])*\\*+/\"、"
            "%commit \"0x[0-9a-fA-F]+\"、%commit skip \"//[^\\n]*\"）",
            directiveLine);
        return;
    }
    const std::string pattern =
        readRegexPattern(explicitType ? ("%commit " + spec.type) : "%commit");
    if (failed())
    {
        return;
    }
    std::string alias;
    if ((peekKind() == "id") && (peekText() == "alias") &&
        (peekKind(1) == "str"))
    {
        ++m_pos;
        alias = GrammarLoader::decodeStringLiteral(peekText());
        ++m_pos;
    }
    registerCommitRule("%commit", spec.type, pattern, spec.isSkip,
                       directiveLine, alias);
    endDirective("%commit");
}

void GramParser::parseTriviaDirective()
{
    const std::size_t directiveLine =
        (peekToken() == nullptr) ? 0u : lineOf(peekToken()->start);
    ++m_pos;
    const std::string pattern = readRegexPattern("%trivia");
    if (failed())
    {
        return;
    }
    // 与 %commit skip 完全等价：匹配即丢，不进容器也不进语法层。
    // 类型名也用 "skip"：两种写法生成的词法表逐字相同（skip 的 type 不会
    // 出现在任何容器元素里，只是表里的占位）。
    std::string alias;
    if ((peekKind() == "id") && (peekText() == "alias") &&
        (peekKind(1) == "str"))
    {
        ++m_pos;
        alias = GrammarLoader::decodeStringLiteral(peekText());
        ++m_pos;
    }
    registerCommitRule("%trivia", "skip", pattern, true, directiveLine,
                       alias);
    endDirective("%trivia");
}

void GramParser::registerCommitRule(const std::string &what,
                                    const std::string &type,
                                    const std::string &pattern, bool isSkip,
                                    std::size_t line, const std::string &alias)
{
    // 同一个模式不能既当 Token 又当 %commit / %trivia（字面量与正则都算）
    const auto literalOwner = m_literalOwners.find(pattern);
    if (literalOwner != m_literalOwners.end())
    {
        fail(what + " 的模式 " + pythonRepr(pattern) + " 已经由 Token " +
                 literalOwner->second +
                 " 的固定字面量声明：同一个模式不能既当 Token 又当 " + what,
             line);
        return;
    }
    for (const auto &entry : m_regexDefs)
    {
        if (entry.second == pattern)
        {
            fail(what + " 的模式 " + pythonRepr(pattern) + " 已经由 Token " +
                     entry.first +
                     " 的 %token regex 声明：同一个模式不能既当 Token 又当 " +
                     what,
                 line);
            return;
        }
    }
    CommitSpec spec;
    spec.type = type;
    spec.pattern = pattern;
    spec.isSkip = isSkip;
    spec.line = line;
    spec.order = m_lexOrder++;
    spec.alias = alias;
    m_commits.push_back(spec);
}

void GramParser::registerLiteralToken(const std::string &tokenName,
                                      const std::string &literalText,
                                      const std::string &what)
{
    if (tokenName == k_errorTokenName)
    {
        fail(
            "error 是恢复用的保留伪终结符，不能用作 Token 名");
        return;
    }
    std::vector<std::string> branches;
    if (!GrammarLoader::splitDefineLiteral(literalText, what, branches,
                                           errorSlot()))
    {
        return;
    }
    if (m_tokenDefs.count(tokenName) || m_regexDefs.count(tokenName))
    {
        fail("Token " + tokenName + " 重复定义");
        return;
    }
    for (const std::string &literal : branches)
    {
        for (const CommitSpec &commit : m_commits)
        {
            if (commit.pattern == literal)
            {
                fail("字面量 " + pythonRepr(literal) + " 已经由第 " +
                         std::to_string(commit.line) +
                         " 行的 %commit 声明：同一个模式不能既当 Token 又当 "
                         "commit");
                return;
            }
        }
        const auto ownerIt = m_literalOwners.find(literal);
        const std::string *owner =
            ownerIt != m_literalOwners.end() ? &ownerIt->second : nullptr;
        if (owner != nullptr)
        {
            fail("字面量 " + pythonRepr(literal) +
                 " 已由 Token " + *owner + " 定义");
            return;
        }
        m_literalOwners.insert_or_assign(literal, tokenName);
    }
    m_tokenDefs.insert_or_assign(tokenName, branches);
    bool known = false;
    for (const std::string &existing : m_tokenNames)
    {
        known = known || (existing == tokenName);
    }
    if (!known)
    {
        m_tokenNames.push_back(tokenName);
    }
    // 词法声明序：%commit 与 Token 规则同长度时比较先后用
    if (m_tokenLexOrder.count(tokenName) == 0)
    {
        m_tokenLexOrder.insert_or_assign(tokenName, m_lexOrder++);
    }
}

void GramParser::parseDefineDirective()
{
    ++m_pos;
    if ((peekKind() != "id") || (peekText()[0] == '%'))
    {
        fail("%define 后应跟 Token 名");
        return;
    }
    const std::string tokenName = peekText();
    ++m_pos;
    if ((peekKind() != "str") || (peekText()[0] != '"'))
    {
        fail("%define 规则必须使用双引号");
        return;
    }
    const std::string literalText = peekText();
    ++m_pos;
    registerLiteralToken(tokenName, literalText, "%define " + tokenName);
    if ((peekKind() == "id") && (peekText() == "alias") &&
        (peekKind(1) == "str"))
    {
        ++m_pos;
        m_tokenAliases[tokenName] = GrammarLoader::decodeStringLiteral(peekText());
        ++m_pos;
    }
    endDirective("%define " + tokenName);
}

void GramParser::parseSpaceDirective()
{
    ++m_pos;
    if (m_spaceDeclared)
    {
        fail("%space 只能声明一次");
        return;
    }
    if ((peekKind() != "str") || (peekText()[0] != '"'))
    {
        fail("%space 后面要跟一个双引号字面量，例如 %space \"\\b\""
             "（跳过全部空白）或 %space \" \\t\"（只跳过空格与制表）");
        return;
    }
    const std::string literalText = peekText();
    ++m_pos;
    std::vector<std::string> branches;
    if (literalText == "\"\"")
    {
        // 空集：一个空白都不跳过（空白必须由 Token 覆盖，否则词法报错）
        branches.clear();
    }
    else if (!GrammarLoader::splitDefineLiteral(literalText, "%space",
                                                branches, errorSlot()))
    {
        return;
    }
    std::string chars;
    bool wildcard = false;
    for (const std::string &branch : branches)
    {
        for (const char ch : branch)
        {
            if (ch == k_blankWildcardChar)
            {
                wildcard = true;
                continue;
            }
            if (!isBlankChar(ch))
            {
                fail("%space 里只能写空白字符（空格 / \\t / \\n / \\v / "
                     "\\f / \\r）或 \\b（代表全部空白），'" +
                     pythonRepr(std::string(1, ch)) + "' 不是空白字符");
                return;
            }
            if (chars.find(ch) == std::string::npos)
            {
                chars += ch;
            }
        }
    }
    if (wildcard)
    {
        for (const char ch : blankChars())
        {
            if (chars.find(ch) == std::string::npos)
            {
                chars += ch;
            }
        }
    }
    std::sort(chars.begin(), chars.end());
    m_spaceChars = chars;
    m_spaceDeclared = true;
    endDirective("%space");
}

void GramParser::parseStartDirective()
{
    ++m_pos;
    if (peekKind() != "id")
    {
        fail("%start 后应跟起始非终结符名");
        return;
    }
    if (peekText() == k_errorTokenName)
    {
        fail(
            "%start 不能写 error：error 是恢复用的保留伪终结符，不是非终结符");
        return;
    }
    m_start = peekText();
    m_startOffset = m_tokens[m_pos].start;
    ++m_pos;
    skipNl();
    if (peekText() == "\n")
    {
        ++m_pos;
    }
}

void GramParser::parseExampleDirective()
{
    ++m_pos;
    if (m_exampleInput.has_value())
    {
        fail("%example 只能声明一次");
        return;
    }
    if (peekKind() != "str")
    {
        fail("%example 后应跟一个带引号的示例输入（例如 %example \"1 + 2\"）");
        return;
    }
    std::string value = GrammarLoader::decodeStringLiteral(peekText());
    if (value.find('\0') != std::string::npos)
    {
        fail("%example 不能包含 NUL 字节：示例输入需要能通过命令行传给演示程序");
        return;
    }
    m_exampleInput = std::move(value);
    ++m_pos;
    endDirective("%example");
}

void GramParser::parseTreeDirective()
{
    ++m_pos;
    m_treeEnabled = true;
    if (peekText() == "\n")
    {
        ++m_pos;
    }
    else if (peekToken() != nullptr)
    {
        fail("%tree 指令不能带参数，实际得到 '" + peekText() +
             "'");
        return;
    }
}

/**
 * @brief 解析不带参数的开关键指令（%teach / %mermaid，都默认关闭）。
 * @param name 指令名（用于报错）。
 * @param target 目标开关。
 */
void GramParser::parseToggleDirective(const std::string &name, bool &target)
{
    ++m_pos;
    target = true;
    if (peekText() == "\n")
    {
        ++m_pos;
    }
    else if (peekToken() != nullptr)
    {
        fail(name + " 指令不能带参数，实际得到 '" +
             peekText() + "'");
        return;
    }
}

/**
 * @brief 解析 %destructor <成员> { 清理代码 }。
 *
 * 与 %union 一样「原样收集」花括号里的正文（不解析 C），用 $$ 指被丢弃的值。
 * 只在 C / C++17 产物里有意义：Python 的语义值是任意对象，不接这套，
 * 写在那种模式下直接报错。
 */
void GramParser::parseDestructorDirective()
{
    const std::size_t directiveLine =
        lineOf((peekToken() == nullptr) ? 0u : peekToken()->start);
    ++m_pos;
    if (peekText() != "<")
    {
        fail("%destructor 后应跟 <成员名>，例如 %destructor <str> { free($$); }");
        return;
    }
    if ((peekKind(1) != "id") || (peekText(2) != ">"))
    {
        fail("%destructor 的 <成员名> 写法不完整，应写成 %destructor <成员名> "
             "{ ... }");
        return;
    }
    const std::string member = peekText(1);
    m_pos += 3;
    skipNl();
    if (peekText() != "{")
    {
        fail("%destructor <" + member + "> 后应跟 { 包裹的清理代码，实际得到 '" +
             peekText() + "'");
        return;
    }
    const std::size_t openIndex = peekToken()->start;
    const std::size_t closeIndex =
        findUnionClose(m_text, openIndex, errorSlot());
    if (failed())
    {
        return;
    }
    if (closeIndex == std::string::npos)
    {
        fail("%destructor <" + member + "> 的 { 没有闭合");
        return;
    }
    const std::string body = m_text.substr(openIndex + 1u,
                                           closeIndex - openIndex - 1u);
    if (m_destructors.find(member) != m_destructors.end())
    {
        fail("%destructor <" + member +
             "> 已经声明过了：同一个成员只能有一条清理代码");
        return;
    }
    m_destructors.insert_or_assign(member, body);
    m_destructorLines.insert_or_assign(member, directiveLine);
    // 跳过整个 { ... }
    while ((peekToken() != nullptr) && (peekToken()->start < closeIndex + 1u))
    {
        ++m_pos;
    }
    if (peekText() == "\n")
    {
        ++m_pos;
    }
    else if (peekToken() != nullptr)
    {
        fail("%destructor <" + member + "> 后有多余内容 '" + peekText() + "'");
        return;
    }
}

void GramParser::parseModeDirective()
{
    ++m_pos;
    // 括号形式（LL(2) / LALR(k)）在词法上被拆成 `LALR` `(` `k` `)` 四个
    // token，这里拼回一个字符串再交给 normalizeModeName
    std::string token = peekText();
    std::size_t consumed = 1;
    if (peekText(1) == "(")
    {
        const std::string inner = peekText(2);
        if (inner.empty() || (inner == "\n") || (inner == "None") ||
            (peekText(3) != ")"))
        {
            fail("%mode 的括号写法要写成 LL(2) / LALR(k) / LR(0) 这样");
            return;
        }
        token += "(" + inner + ")";
        consumed = 4;
    }
    // 模式名大小写随意，也可以把 k 写在括号里（LL(2) / LALR(k) / LR(0) …）
    std::string normalized;
    int inlineK = -1;
    if (!normalizeModeName(token, normalized, inlineK))
    {
        fail(
            "%mode 只支持 ll0、ll1、llk、lr0、slr、slrk、lalr、lalrk、"
            "lr1、lrk 或 ielr（大小写随意，也可以写成 LL(1) / LL(2) / "
            "LALR(k) 这种带 k 的形式），实际得到 '" +
            token + "'");
        return;
    }
    if (m_modeDeclared)
    {
        fail("%mode 只能声明一次");
        return;
    }
    m_mode = normalized;
    m_modeDeclared = true;
    if (inlineK >= 0)
    {
        // 模式名里写了 k（LL(2) / SLR(3) …）：直接当 %lookahead 用；
        // 后面若再写 %lookahead，validateAndBuild 会发现两者不一致并报错
        m_modeLookahead = inlineK;
    }
    m_pos += consumed;
    if (peekText() == "\n")
    {
        ++m_pos;
    }
    else if (peekToken() != nullptr)
    {
        fail("%mode " + token + " 后有多余内容 '" +
             peekText() + "'");
        return;
    }
}

/**
 * @brief 解析 %lookahead 指令（LR(k) / LALR(k) 的 k）。
 */
void GramParser::parseLookaheadDirective()
{
    ++m_pos;
    const std::string token = peekText();
    if (peekKind() != "num")
    {
        fail("%lookahead 后应跟非负整数 k，实际得到 '" + token + "'");
        return;
    }
    double raw = 0.0;
    if (!parseNumText(token, raw) ||
        (raw > static_cast<double>(std::numeric_limits<int>::max())))
    {
        fail("%lookahead 后应跟非负整数 k，实际得到 '" + token + "'");
        return;
    }
    const int value = static_cast<int>(raw);
    if (value < 0)
    {
        fail("%lookahead 不能是负数，实际得到 " + std::to_string(value));
        return;
    }
    if (m_lookaheadDeclared)
    {
        fail("%lookahead 只能声明一次");
        return;
    }
    m_lookahead = value;
    m_lookaheadDeclared = true;
    ++m_pos;
    if (peekText() == "\n")
    {
        ++m_pos;
    }
    else if (peekToken() != nullptr)
    {
        fail("%lookahead " + token + " 后有多余内容 '" + peekText() + "'");
        return;
    }
}

void GramParser::parseLangDirective()
{
    ++m_pos;
    const std::string token = peekText();
    if ((token != "c") && (token != "cpp17") && (token != "python") &&
        (token != "csharp") && (token != "rust") && (token != "go") &&
        (token != "java") && (token != "js") && (token != "gram"))
    {
        fail(
            "%lang 只支持 c / cpp17 / python / csharp / rust / go / java / "
            "js / gram，"
            "实际得到 '" + token + "'");
        return;
    }
    if (m_langDeclared)
    {
        fail("%lang 只能声明一次");
        return;
    }
    m_lang = token;
    m_langDeclared = true;
    ++m_pos;
    if (peekText() == "\n")
    {
        ++m_pos;
    }
    else if (peekToken() != nullptr)
    {
        fail("%lang " + token + " 后有多余内容 '" +
             peekText() + "'");
        return;
    }
}

void GramParser::parseBuildFilesDirective()
{
    ++m_pos;
    const std::string token = peekText();
    if ((token != "on") && (token != "off"))
    {
        fail("%build_files 只支持 on / off，实际得到 '" + token + "'");
        return;
    }
    if (m_buildFilesDeclared)
    {
        fail("%build_files 只能声明一次");
        return;
    }
    m_buildFiles = (token == "on");
    m_buildFilesDeclared = true;
    ++m_pos;
    if (peekText() == "\n")
    {
        ++m_pos;
    }
    else if (peekToken() != nullptr)
    {
        fail("%build_files " + token + " 后有多余内容 '" + peekText() + "'");
        return;
    }
}

void GramParser::parseNamespaceDirective()
{
    ++m_pos;
    const std::string token = peekText();
    if (peekToken() == nullptr)
    {
        fail(
            "%namespace 后面要写命名空间名（例如 %namespace myapp）");
        return;
    }
    // `%namespace my::lang` 里 `my` 是标识符、`::` 是单独的 token：先给这条
    // 专门的提示，别让它落到"名字不合法"上
    if ((token.find("::") != std::string::npos) || (peekText(1) == ":") ||
        (peekText(1) == "::"))
    {
        fail(
            "%namespace 第一版只支持单级名字，不支持 '::'（写 %namespace 后面"
            "一个标识符即可）");
        return;
    }
    if (!isCppIdentifier(token, false))
    {
        if (cppKeywords().count(token) != 0)
        {
            fail("%namespace 不能用 C++ 关键字：'" + token +
                 "'");
            return;
        }
        fail(
            "%namespace 的名字必须是 C++ 标识符"
            "（[A-Za-z_][A-Za-z0-9_]*），实际得到 '" +
            token + "'");
        return;
    }
    if (m_cppNamespaceDeclared)
    {
        fail("%namespace 只能声明一次");
        return;
    }
    m_cppNamespace = token;
    m_cppNamespaceDeclared = true;
    ++m_pos;
    if (peekText() == "\n")
    {
        ++m_pos;
    }
    else if (peekToken() != nullptr)
    {
        fail("%namespace " + token + " 后有多余内容 '" +
             peekText() + "'");
        return;
    }
}

/**
 * @brief 解析 %class 指令（Python 产物的解析器类名）。
 */
void GramParser::parseClassDirective()
{
    ++m_pos;
    const std::string token = peekText();
    if (peekToken() == nullptr)
    {
        fail("%class 后面要写类名（例如 %class MyParser）");
        return;
    }
    if (pythonKeywords().count(token) != 0)
    {
        fail("%class 不能用 Python 关键字：'" + token + "'");
        return;
    }
    if (!isCppIdentifier(token, false))
    {
        fail(
            "%class 的名字必须是标识符"
            "（[A-Za-z_][A-Za-z0-9_]*），实际得到 '" +
            token + "'");
        return;
    }
    if (m_pythonClassDeclared)
    {
        fail("%class 只能声明一次");
        return;
    }
    m_pythonClass = token;
    m_pythonClassDeclared = true;
    ++m_pos;
    if (peekText() == "\n")
    {
        ++m_pos;
    }
    else if (peekToken() != nullptr)
    {
        fail("%class " + token + " 后有多余内容 '" + peekText() + "'");
        return;
    }
}

/**
 * @brief 名字文本是不是「分段标识符」（C# 命名空间 / import 名）。
 *
 * 形如 `a`、`a.b.c`（C#）、`std::vector`，每个段都是
 * `[A-Za-z_][A-Za-z0-9_]*`；分隔符 `.` 与 `::` 都接受。`allowStarTail` 为 true 时
 * 允许最后一段是 `*`（`using System.*;` 这种写法）。
 * @param text 待判断的文本。
 * @param allowStarTail 是否允许末段是 `*`。
 * @return 合法返回 true。
 */
bool isDottedNameText(const std::string &text, bool allowStarTail)
{
    if (text.empty())
    {
        return false;
    }
    std::size_t start = 0u;
    while (start <= text.size())
    {
        const std::size_t dot = text.find('.', start);
        const std::size_t colon = text.find("::", start);
        std::size_t end = text.size();
        std::size_t next = std::string::npos;
        std::size_t step = 0u;
        if (dot != std::string::npos)
        {
            end = dot;
            next = dot;
            step = 1u;
        }
        if ((colon != std::string::npos) && (colon < end))
        {
            end = colon;
            next = colon;
            step = 2u;
        }
        const std::string segment = text.substr(start, end - start);
        if (segment.empty())
        {
            return false;
        }
        const bool star = (segment == "*");
        if (star)
        {
            if (!allowStarTail || (end != text.size()))
            {
                return false;
            }
        }
        else
        {
            if (!((std::isalpha(static_cast<unsigned char>(segment[0])) != 0) ||
                  (segment[0] == '_')))
            {
                return false;
            }
            for (const char ch : segment)
            {
                if (!isIdPart(ch))
                {
                    return false;
                }
            }
        }
        if (next == std::string::npos)
        {
            break;
        }
        start = next + step;
    }
    return true;
}

/**
 * @brief 读一行里剩下的原文（`%package` / `%import` 的点分名字可能被切成多个 token）。
 * @return 去掉首尾空白后的整行文本。
 */
std::string GramParser::readDirectiveRestOfLine()
{
    const GramToken *first = peekToken();
    if ((first == nullptr) || (first->text == "\n"))
    {
        return std::string();
    }
    const std::size_t startOffset = first->start;
    std::size_t endOffset = first->end;
    while ((m_pos < m_tokens.size()) && (peekText() != "\n"))
    {
        endOffset = m_tokens[m_pos].end;
        ++m_pos;
    }
    if (peekText() == "\n")
    {
        ++m_pos;
    }
    return strip(m_text.substr(startOffset, endOffset - startOffset));
}

void GramParser::parsePackageDirective()
{
    const std::size_t directiveLine =
        (peekToken() == nullptr) ? 0u : lineOf(peekToken()->start);
    ++m_pos;
    const std::string name = readDirectiveRestOfLine();
    if (name.empty())
    {
        fail("%package 后面要写包名 / 命名空间名（例如 %package com.example）",
             directiveLine);
        return;
    }
    if (!isDottedNameText(name, false))
    {
        fail(
            "%package 的名字必须是点分标识符"
            "（[A-Za-z_][A-Za-z0-9_]* 用 '.' 连接，例如 com.example），"
            "实际得到 '" +
                name + "'",
            directiveLine);
        return;
    }
    if (m_packageDeclared)
    {
        fail("%package 只能声明一次", directiveLine);
        return;
    }
    m_packageName = name;
    m_packageDeclared = true;
    m_packageLine = directiveLine;
}

void GramParser::parseImportDirective()
{
    const std::size_t directiveLine =
        (peekToken() == nullptr) ? 0u : lineOf(peekToken()->start);
    ++m_pos;
    const std::string name = readDirectiveRestOfLine();
    if (name.empty())
    {
        fail("%import 后面要写要导入的名字（例如 %import System.Collections）",
             directiveLine);
        return;
    }
    // 带引号的写法（`%import "math/rand"` 或 `别名 "路径"`）是给 Go 留的口子：
    // Go 的 import 路径里有 `/`，点分标识符那套校验套不上，原样收下交给
    // 目标语言的编译器判（`code_gen_go.cpp` 会把 `::` 归一成 `/`）。
    if ((name.find('"') == std::string::npos) && !isDottedNameText(name, true))
    {
        fail(
            "%import 的名字必须是点分标识符（末段可以是 '*'，例如 "
            "System.Collections.*），实际得到 '" +
                name + "'",
            directiveLine);
        return;
    }
    m_imports.push_back(name);
    m_importLines.push_back(directiveLine);
}

void GramParser::parseRuntimeDirective()
{
    fail(
        "%runtime 已删除：语义值运行时只有 custom，也是缺省值（不写就等于 "
        "custom），删掉这一行即可；历史上的 builtin 运行时也已移除");
    return;
}

void GramParser::parseValueTypeDirective()
{
    ++m_pos;
    const GramToken *first = peekToken();
    if ((first == nullptr) || (first->text == "\n"))
    {
        fail(
            "%value_type 后面应跟语义值类型，例如 %value_type double");
        return;
    }
    // 类型名可能含空格（`unsigned long`、`struct Point *`），按行取原文
    const std::size_t startOffset = first->start;
    std::size_t endOffset = first->end;
    while ((m_pos < m_tokens.size()) && (peekText() != "\n"))
    {
        endOffset = m_tokens[m_pos].end;
        ++m_pos;
    }
    const std::string typeText =
        strip(m_text.substr(startOffset, endOffset - startOffset));
    if (typeText.empty())
    {
        fail(
            "%value_type 后面应跟语义值类型，例如 %value_type double");
        return;
    }
    if (m_valueType.has_value())
    {
        fail("%value_type 只能声明一次");
        return;
    }
    m_valueType = typeText;
    if (peekText() == "\n")
    {
        ++m_pos;
    }
}

void GramParser::parseCopyDirective()
{
    ++m_pos;
    std::string kind = "file";
    const GramToken *first = peekToken();
    if ((first != nullptr) && ((first->text == "file") || (first->text == "dir")))
    {
        const GramToken *next = peekToken(1);
        // 只有 file / dir 与后面的路径之间有空白时才把它当作类型关键字，
        // 这样 %copy file.txt 会被当成路径 "file.txt"，而不是类型 + ".txt"
        if ((next != nullptr) && (next->text != "\n") &&
            (next->start > first->end))
        {
            kind = first->text;
            ++m_pos;
        }
    }
    std::string path;
    if (peekKind() == "str")
    {
        path = GrammarLoader::decodeStringLiteral(peekText());
        ++m_pos;
    }
    else
    {
        while (m_pos < m_tokens.size())
        {
            const GramToken *token = peekToken();
            if ((token == nullptr) || (token->text == "\n"))
            {
                break;
            }
            path += token->text;
            ++m_pos;
        }
    }
    if (path.empty())
    {
        fail("%copy 缺少要拷贝的文件 / 目录路径");
        return;
    }
    CopySpec spec;
    spec.kind = kind;
    spec.path = path;
    m_copies.push_back(spec);
    if (peekText() == "\n")
    {
        ++m_pos;
    }
}

void GramParser::parseVarMaxCountDirective()
{
    fail(
        "%var_max_count 需要 builtin 运行时，已被移除；"
        "请在 %code ... %endcode 里自行实现变量表");
    return;
}


void GramParser::parseCapacityDirective(const std::string &directive,
                                        int minimum, int maximum,
                                        std::optional<int> &target)
{
    ++m_pos;
    if (peekKind() != "num")
    {
        fail(
            directive + " 后应跟" +
            ((minimum == 0) ? "非负整数" : "正整数") + "，实际得到 '" +
            peekText() + "'");
        return;
    }
    double raw = 0.0;
    if (!parseNumText(peekText(), raw) || (raw < 0.0) ||
        (raw != std::floor(raw)))
    {
        fail(directive + " 后应跟整数，实际得到 '" + peekText() + "'");
        return;
    }
    if ((raw < static_cast<double>(minimum)) ||
        (raw > static_cast<double>(maximum)))
    {
        fail(directive + " 必须在 " + std::to_string(minimum) +
             ".." + std::to_string(maximum) + " 之间，实际得到 " +
             numTextForError(raw, peekText()));
        return;
    }
    const int value = static_cast<int>(raw);
    ++m_pos;
    if (peekText() == "\n")
    {
        ++m_pos;
    }
    else if (peekToken() != nullptr)
    {
        fail(directive + " " + std::to_string(value) +
             " 后有多余内容 '" + peekText() + "'");
        return;
    }
    if (target.has_value())
    {
        fail(directive + " 只能声明一次");
        return;
    }
    target = value;
}

void GramParser::parseUnionDirective()
{
    // #line 要指回 %union 那一行
    m_unionLine = lineOf((peekToken() == nullptr) ? 0u : peekToken()->start);
    ++m_pos;
    skipNl();
    if (m_unionDeclared)
    {
        fail("%union 只能声明一次");
        return;
    }
    if (peekText() != "{")
    {
        fail("%union 后应跟 { 包裹成员声明，实际得到 '" +
             peekText() + "'");
        return;
    }
    const GramToken *brace = peekToken();
    const std::size_t openIndex = brace->start;
    const std::size_t closeIndex = findUnionClose(m_text, openIndex, errorSlot());
    if (failed())
    {
        return;
    }
    if (closeIndex == std::string::npos)
    {
        fail(
            "%union 的括号不配平：从位置 " + std::to_string(openIndex) +
            " 开始的 { 一直到文件结束都没有配对的 }");
        return;
    }
    m_unionBody =
        m_text.substr(openIndex + 1, closeIndex - openIndex - 1);
    const std::vector<std::string> segments =
        splitUnionMembers(m_unionBody, openIndex + 1, errorSlot());
    if (failed())
    {
        return;
    }
    if (segments.empty())
    {
        fail("%union 至少要声明一个成员");
        return;
    }
    for (std::size_t index = 0; index < segments.size(); ++index)
    {
        UnionMember member;
        member.name =
            extractUnionMemberName(segments[index], index + 1, errorSlot());
        if (failed())
        {
            return;
        }
        member.cType = segments[index];
        for (const UnionMember &existing : m_unionMembers)
        {
            if (existing.name == member.name)
            {
                fail("%union 成员名重复：" + member.name);
                return;
            }
        }
        m_unionMembers.push_back(member);
    }
    m_unionDeclared = true;
    // body 里的 token 已经被词法器切好，这里按位置整体跳过
    while ((m_pos < m_tokens.size()) &&
           (m_tokens[m_pos].start <= closeIndex))
    {
        ++m_pos;
    }
}

void GramParser::parseTypeDirective()
{
    const std::size_t directiveLine =
        (peekToken() == nullptr) ? 0u : lineOf(peekToken()->start);
    ++m_pos;
    skipNl();
    if (peekText() != "<")
    {
        fail(
            "%type 需要写成 %type <成员名> 符号...，实际得到 '" + peekText() +
            "'");
        return;
    }
    if ((peekKind(1) != "id") || (peekText(2) != ">"))
    {
        fail("%type 的 <成员名> 写法不完整");
        return;
    }
    const std::string member = peekText(1);
    m_pos += 3;
    std::vector<std::string> names;
    readSymbolNameList("%type", names);
    for (const std::string &name : names)
    {
        bindSymbolMember(name, member, "%type", directiveLine);
    }
}

void GramParser::readSymbolNameList(const std::string &keyword,
                                    std::vector<std::string> &names,
                                    const std::string &what)
{
    while (m_pos < m_tokens.size())
    {
        const std::string token = peekText();
        if (token == "\n")
        {
            ++m_pos;
            continue;
        }
        if ((peekKind() == "id") && (token[0] != '%'))
        {
            // 下一行若是产生式（名字后跟 :=），说明名字列表已经结束
            if (peekText(1) == ":=")
            {
                break;
            }
            bool known = false;
            for (const std::string &name : names)
            {
                known = known || (name == token);
            }
            if (!known)
            {
                names.push_back(token);
            }
            ++m_pos;
            continue;
        }
        if (token[0] == '%')
        {
            break;
        }
        fail(keyword + " 只能包含" + what + "，实际得到 '" +
             token + "'");
        return;
    }
    if (names.empty())
    {
        fail(keyword + " 至少需要一个" + what);
        return;
    }
}

void GramParser::bindSymbolMember(const std::string &symbol,
                                  const std::string &member,
                                  const std::string &what, std::size_t line)
{
    if (symbol == k_errorTokenName)
    {
        fail(what +
             " 不能给 error 绑定成员：error 是恢复用的保留"
             "伪终结符，语义值是未定义的占位值");
        return;
    }
    const auto existingIt = m_symbolValueMember.find(symbol);
    const std::string *existing =
        existingIt != m_symbolValueMember.end() ? &existingIt->second : nullptr;
    if (existing != nullptr)
    {
        if (*existing == member)
        {
            return;
        }
        fail(what + " 把符号 " + symbol +
             " 绑定到了两个 %union 成员：" + *existing +
             " / " + member + "（同一符号只能有一个成员）");
        return;
    }
    m_symbolValueMember.insert_or_assign(symbol, member);
    m_symbolMemberLines.insert_or_assign(symbol, line);
}

void GramParser::validateUnionUsage()
{
    // %value_type 与 %union 都在定义 ParserValue_T，只能选一个
    if (m_valueType.has_value() && m_unionDeclared)
    {
        fail(
            "%value_type 与 %union 不能同时写：%union 已经定义了 "
            "ParserValue_T，两者只能选一个");
        return;
    }
    // 写了 %value_type 时 ParserValue_T 由生成器给出，用户代码里不能再定义
    if (m_valueType.has_value())
    {
        for (std::size_t index = 0; index < m_userCodeParts.size(); ++index)
        {
            if (userCodeDefinesParserValueT(m_userCodeParts[index]))
            {
                fail(
                    "第 " + std::to_string(index + 1) +
                    " 个 %code 代码块里定义了 ParserValue_T，与 "
                    "%value_type 冲突：写了 %value_type 时这个类型由生成器"
                    "生成（typedef " + *m_valueType +
                    " ParserValue_T;），请删掉用户代码里的定义，"
                    "或去掉 %value_type");
                return;
            }
        }
    }
    if (!m_unionDeclared)
    {
        if (!m_symbolValueMember.empty())
        {
            fail(
                "文法里写了 %type <成员> / %token <成员>，但没有 %union "
                "声明这些成员");
            return;
        }
        return;
    }
    // %type / %token <...> 引用的成员必须在 %union 里
    for (const auto &binding : m_symbolValueMember)
    {
        const std::string &symbol = binding.first;
        const std::string &member = binding.second;
        bool declared = false;
        for (const UnionMember &candidate : m_unionMembers)
        {
            declared = declared || (candidate.name == member);
        }
        if (declared)
        {
            continue;
        }
        std::string known;
        for (const UnionMember &candidate : m_unionMembers)
        {
            if (!known.empty())
            {
                known += " / ";
            }
            known += candidate.name;
        }
        fail("%union 里没有成员 " + member + "（符号 " +
             symbol + " 绑定的）；已声明的成员：" + known);
        return;
    }
    // 写了 %union 时 ParserValue_T 由生成器给出，用户代码块里不能再定义
    for (std::size_t index = 0; index < m_userCodeParts.size(); ++index)
    {
        if (userCodeDefinesParserValueT(m_userCodeParts[index]))
        {
            fail(
                "第 " + std::to_string(index + 1) +
                " 个 %code 代码块里定义了 ParserValue_T，与 %union 冲突："
                "写了 %union 时这个类型由生成器生成（typedef union { ... } "
                "ParserValue_T;），请删掉用户代码里的定义，或去掉 %union");
            return;
        }
    }
    // 动作里引用到的符号（$n 的右部符号、$$ 的左部符号）必须都绑定了成员
    for (const auto &rule : m_rules)
    {
        for (std::size_t ordinal = 0; ordinal < rule.second.size(); ++ordinal)
        {
            const Production &production = rule.second[ordinal];
            if (strip(production.action).empty())
            {
                continue;
            }
            const std::string where =
                "规则 " + rule.first + " 的第 " + std::to_string(ordinal + 1) +
                " 条（" + rule.first + " := " +
                joinSymbolNames(production.rhs) + "）";
            const std::vector<std::pair<std::string, std::size_t>> refs =
                collectDollarReferences(production.action);
            for (const auto &ref : refs)
            {
                if (ref.first == "$$")
                {
                    if (m_symbolValueMember.find(rule.first) == m_symbolValueMember.end())
                    {
                        fail(
                            where + "的 $$（左部符号 " + rule.first +
                            "，动作里第 " + std::to_string(ref.second + 1) +
                            " 个字符）没有绑定 %union 成员；请用 %type <成员> " +
                            rule.first + " 绑定，或去掉 %union");
                        return;
                    }
                    continue;
                }
                const int slot = std::atoi(ref.first.c_str() + 1);
                if ((slot < 1) ||
                    (static_cast<std::size_t>(slot) > production.rhs.size()))
                {
                    continue;
                }
                const std::string &symbol =
                    production.rhs[static_cast<std::size_t>(slot) - 1];
                if (m_symbolValueMember.find(symbol) != m_symbolValueMember.end())
                {
                    continue;
                }
                fail(
                    where + "的 " + ref.first + "（第 " + std::to_string(slot) +
                    " 个符号 " + symbol + "，动作里第 " +
                    std::to_string(ref.second + 1) +
                    " 个字符）没有绑定 %union 成员；请用 %type <成员> 或 "
                    "%token <成员> 绑定它");
                return;
            }
        }
    }
}

void GramParser::parseVarDirective()
{
    fail(
        "%var 需要 builtin 运行时，已被移除；请在 %code ... %endcode 里自行实现变量表");
    return;
}

void GramParser::parseProduction()
{
    const std::string lhs = peekText();
    if (lhs[0] == '%')
    {
        fail("规则名不能以 % 开头: " + lhs);
        return;
    }
    if (lhs == k_errorTokenName)
    {
        fail(
            "error 是恢复用的保留伪终结符，不能作产生式左部");
        return;
    }
    ++m_pos;
    expect(":=");
    if (!m_firstRule.has_value())
    {
        m_firstRule = lhs;
    }
    // | 分支写法：同一条规则的多个分支写在一起，每个分支各带动作，
    // 整体必须以 ';' 结束。单分支的旧写法（不加 ';'）继续可用。
    bool branched = false;
    while (true)
    {
        // 记下这一条分支的起始偏移：后面做交叉校验（未定义符号 / 缺动作）时
        // 用它把报错行号指回原文件
        const GramToken *firstToken = peekToken();
        const std::size_t alternativeOffset =
            (firstToken == nullptr) ? 0u : firstToken->start;
        std::pair<std::vector<std::string>, std::optional<std::string>>
            alternative = parseAlternativeRhs(lhs);
        // 动作的起始位置（`{` 或行内动作的起点）：#line 要指回这一行
        const GramToken *actionToken = peekToken();
        const std::size_t actionOffset =
            (actionToken == nullptr) ? 0u : actionToken->start;
        const std::string action = parseAlternativeAction(lhs);

        const auto existingIt = m_rules.find(lhs);
        const std::vector<Production> *existing =
            existingIt != m_rules.end() ? &existingIt->second : nullptr;
        const int productionIndex =
            (existing == nullptr) ? 0 : static_cast<int>(existing->size());
        Production production;
        production.rhs = alternative.first;
        production.action = action;
        production.sourceOffset = alternativeOffset;
        if (!strip(action).empty())
        {
            production.actionLine = lineOf(actionOffset);
        }
        m_rules[lhs].push_back(production);
        if (alternative.second.has_value())
        {
            m_productionPrec.insert_or_assign(std::make_pair(lhs, productionIndex),
                                 alternative.second.value());
        }

        // 分支结束：跳过换行，看下一行是 '|' 续写、';' 收尾还是新规则
        while (peekText() == "\n")
        {
            ++m_pos;
        }
        if (peekText() == "|")
        {
            branched = true;
            ++m_pos;
            continue;
        }
        if (peekText() == ";")
        {
            ++m_pos;
            break;
        }
        if (branched)
        {
            fail("规则 " + lhs + " 的 | 分支必须以 ; 结束");
            return;
        }
        break;
    }
}

std::pair<std::vector<std::string>, std::optional<std::string>>
GramParser::parseAlternativeRhs(const std::string &lhs)
{
    std::vector<std::string> rhs;
    std::optional<std::string> precToken;
    while (true)
    {
        if (peekToken() == nullptr)
        {
            break;
        }
        const std::string token = peekText();
        const std::string kind = peekKind();
        if (token == "{")
        {
            break;
        }
        if ((token == "|") || (token == ";"))
        {
            // 分支 / 规则结束
            break;
        }
        if (token == ":=")
        {
            fail("规则 " + lhs + " 缺少语义动作");
            return {};
        }
        if (kind == "id")
        {
            if (token == "%empty")
            {
                ++m_pos;
                rhs.clear();
                continue;
            }
            if (token == "%prec")
            {
                // %prec TOKEN 出现在右部末尾，覆盖产生式的优先级
                ++m_pos;
                if ((peekKind() != "id") || (peekText()[0] == '%'))
                {
                    fail("规则 " + lhs +
                         " 的 %prec 后应跟 Token 名");
                    return {};
                }
                if (peekText() == k_errorTokenName)
                {
                    fail("规则 " + lhs +
                         " 的 %prec 不能写 error：error 是"
                         "恢复用的保留伪终结符，没有优先级");
                    return {};
                }
                precToken = peekText();
                ++m_pos;
                break;
            }
            if ((token == "def") || (token == "delete"))
            {
                break;
            }
            if (peekText(1) == "(")
            {
                break;
            }
            rhs.push_back(token);
            ++m_pos;
            continue;
        }
        if (token == "\n")
        {
            // 跳到下一行的第一个有效 token，判断是否开启新语句
            std::size_t index = m_pos;
            while ((index < m_tokens.size()) &&
                   (m_tokens[index].text == "\n"))
            {
                ++index;
            }
            if (index >= m_tokens.size())
            {
                break;
            }
            const GramToken &nextToken = m_tokens[index];
            if (nextToken.text[0] == '%')
            {
                break;
            }
            if ((nextToken.text == "|") || (nextToken.text == ";"))
            {
                break;
            }
            if ((nextToken.kind == "id") &&
                ((index + 1 >= m_tokens.size()) ||
                 (m_tokens[index + 1].text == ":=")))
            {
                break;
            }
            ++m_pos;
            continue;
        }
        break;
    }
    return std::make_pair(rhs, precToken);
}

std::string GramParser::parseAlternativeAction(const std::string &lhs)
{
    if (peekText() == "{")
    {
        return readActionBlock();
    }
    if ((m_pos >= m_tokens.size()) || (m_tokens[m_pos].text == "\n"))
    {
        // 行尾或文件尾：该分支没有动作；顺手吃掉一个换行
        if ((m_pos < m_tokens.size()) && (m_tokens[m_pos].text == "\n"))
        {
            ++m_pos;
        }
        return "";
    }
    if ((peekText() == "|") || (peekText() == ";"))
    {
        return "";
    }
    return readInlineAction(lhs);
}

std::string GramParser::readActionBlock()
{
    const GramToken startToken = m_tokens[m_pos];
    if (startToken.text != "{")
    {
        fail("期望动作 { ... }");
        return {};
    }
    int depth = 0;
    std::size_t index = m_pos;
    while (index < m_tokens.size())
    {
        const GramToken &token = m_tokens[index];
        if (token.kind == "op")
        {
            if (token.text == "{")
            {
                ++depth;
            }
            else if (token.text == "}")
            {
                --depth;
                if (depth == 0)
                {
                    break;
                }
            }
        }
        ++index;
    }
    if (index >= m_tokens.size())
    {
        fail("动作缺少右花括号 }");
        return {};
    }
    const std::string body =
        m_text.substr(startToken.start + 1,
                      m_tokens[index].start - (startToken.start + 1));
    m_pos = index + 1;
    return strip(body);
}

std::string GramParser::readInlineAction(const std::string &lhs)
{
    // 与 Python 版保持同一签名；lhs 只用于 Python 侧的报错上下文
    (void)lhs;
    const std::size_t start = m_tokens[m_pos].start;
    std::size_t endPos = m_pos;
    while ((endPos < m_tokens.size()) && (m_tokens[endPos].text != "\n"))
    {
        ++endPos;
    }
    const std::size_t end = m_tokens[endPos - 1].end;
    const std::string body = m_text.substr(start, end - start);
    m_pos = endPos;
    if ((m_pos < m_tokens.size()) && (m_tokens[m_pos].text == "\n"))
    {
        ++m_pos;
    }
    return strip(body);
}

GrammarConfig GramParser::parse()
{
    while (!failed() && (m_pos < m_tokens.size()))
    {
        skipNl();
        if (m_pos >= m_tokens.size())
        {
            break;
        }
        parseStatement();
        // 让所有文法层报错都能指回**原文件**行号：没有带行号的，用出错那
        // 一刻记下的行号补一个（条件编译会丢掉整行，行号映射在这里生效）
        if (failed() && (m_error.line == 0u))
        {
            m_error.line = m_failLine;
        }
    }
    if (failed())
    {
        return {};
    }
    return validateAndBuild();
}

void GramParser::parseStatement()
{
    {
        const std::string token = peekText();
        if (token == "%token")
        {
            parseTokenDirective();
        }
        else if (token == "%define")
        {
            parseDefineDirective();
        }
        else if (token == "%start")
        {
            parseStartDirective();
        }
        else if (token == "%example")
        {
            parseExampleDirective();
        }
        else if (token == "%mode")
        {
            parseModeDirective();
        }
        else if (token == "%lookahead")
        {
            parseLookaheadDirective();
        }
        else if (token == "%lang")
        {
            parseLangDirective();
        }
        else if (token == "%build_files")
        {
            parseBuildFilesDirective();
        }
        else if (token == "%package")
        {
            parsePackageDirective();
        }
        else if (token == "%import")
        {
            parseImportDirective();
        }
        else if (token == "%class")
        {
            parseClassDirective();
        }
        else if (token == "%namespace")
        {
            parseNamespaceDirective();
        }
        else if (token == "%prefix")
        {
            parsePrefixDirective();
        }
        else if (token == "%cpp_lexer")
        {
            parseCppLexerDirective();
        }
        else if (token == "%runtime")
        {
            parseRuntimeDirective();
        }
        else if (token == "%value_type")
        {
            parseValueTypeDirective();
        }
        else if (token == "%sync")
        {
            parseSyncDirective();
        }
        else if (token == "%tree")
        {
            parseTreeDirective();
        }
        else if (token == "%teach")
        {
            parseToggleDirective("%teach", m_teachEnabled);
        }
        else if (token == "%error_context")
        {
            // 打开后错误记录里多一段「出错那一行 + ^ 指示」（费内存，默认关）
            parseToggleDirective("%error_context", m_errorContext);
            m_errorContextDeclared = true;
        }
        else if (token == "%destructor")
        {
            parseDestructorDirective();
        }
        else if (token == "%mermaid")
        {
            parseToggleDirective("%mermaid", m_mermaidEnabled);
        }
        else if (token == "%cpp_only")
        {
            parseToggleDirective("%cpp_only", m_cppOnly);
        }
        else if (token == "%copy")
        {
            parseCopyDirective();
        }
        else if (token == "%var_max_count")
        {
            parseVarMaxCountDirective();
        }
        else if (token == "%max_stack_depth")
        {
            parseCapacityDirective("%max_stack_depth", 1, k_maxStackDepthLimit,
                                   m_maxStackDepth);
        }
        else if (token == "%max_errors")
        {
            parseCapacityDirective("%max_errors", 1, k_maxErrorsLimit,
                                   m_maxErrors);
        }
        else if (token == "%error_recovery_tokens")
        {
            parseCapacityDirective("%error_recovery_tokens", 0,
                                   k_errorRecoveryTokensLimit,
                                   m_errorRecoveryTokens);
        }
        else if (token == "%union")
        {
            parseUnionDirective();
        }
        else if (token == "%type")
        {
            parseTypeDirective();
        }
        else if (token == "%var")
        {
            parseVarDirective();
        }
        else if ((token == "%left") || (token == "%right") ||
                 (token == "%nonassoc"))
        {
            parsePrecedenceDirective();
        }
        else if (token == "%expect")
        {
            parseExpectDirective();
        }
        else if (token == "%regex")
        {
            parseRegexDirective();
        }
        else if (token == "%identifier")
        {
            parseIdentifierDirective();
        }
        else if (token == "%commit")
        {
            parseCommitDirective();
        }
        else if (token == "%trivia")
        {
            parseTriviaDirective();
        }
        else if (token == "%space")
        {
            parseSpaceDirective();
        }
        else if ((peekKind() == "id") && (peekText(1) == ":=") &&
                 (token[0] != '%'))
        {
            parseProduction();
        }
        else
        {
            fail("无法解析的语句起点: '" + token +
                 "'（第 " + std::to_string(m_pos) +
                 " 个 token）");
            return;
        }
    }
}

GrammarConfig GramParser::validateAndBuild()
{
    // %mode lex：只导出词法器。文法里可以完全没有产生式，解析相关的指令
    // （%start / %tree / %sync / %destructor / %expect）一律不参与
    // 产物——这里跳过它们的校验，入口处会打一句「已忽略」的提醒。
    const bool lexMode = (m_mode == "lex");
    if (!m_start.has_value())
    {
        if (!m_firstRule.has_value())
        {
            if (!lexMode)
            {
                fail("没有找到任何文法规则");
                return {};
            }
        }
        else
        {
            m_start = m_firstRule;
        }
    }
    // error 是保留的伪终结符：扫一遍右部就知道这份文法用没用它
    for (const auto &rule : m_rules)
    {
        for (const Production &production : rule.second)
        {
            for (const std::string &symbol : production.rhs)
            {
                if (symbol == k_errorTokenName)
                {
                    m_usesError = true;
                }
            }
        }
    }
    // 校验：%sync 里的 Token 必须已声明
    for (const std::string &name : m_syncTokens)
    {
        if (lexMode)
        {
            break; // lex 模式不用 %sync，声明错不错都不影响产物
        }
        if (std::find(m_tokenNames.begin(), m_tokenNames.end(), name) ==
            m_tokenNames.end())
        {
            fail("%sync 里的 Token " + name +
                 " 还没有声明（用 %token / %define / "
                 "%token regex 声明它）");
            return {};
        }
    }
    // 校验：error 产生式与 %sync 的模式限制。文法自己写了 %mode 时在这里就
    // 拦住；没写 %mode 时实际模式由命令行决定（--mode 覆盖），入口会再查一次。
    if (!lexMode && m_modeDeclared && (m_usesError || m_syncDeclared))
    {
        if ((m_mode == "ll1") && m_usesError)
        {
            fail(
                "%mode ll1 不支持 error 产生式：自顶向下分析没有「弹栈到"
                "最近的恢复点」这个动作，请改用 lr0 / slr / lalr / lr1 / "
                "ielr（或 lrk / lalrk / slrk），或改成 %sync SEMI 这种"
                "同步 Token");
            return {};
        }
        if ((m_mode == "llk") && m_usesError)
        {
            fail(
                "%mode llk 不支持 error 产生式：自顶向下分析没有「弹栈到"
                "最近的恢复点」这个动作，请改用 lrk / lalrk / slrk 等 LR "
                "模式，或改成 %sync SEMI 这种同步 Token");
            return {};
        }
        if (isGlrMode(m_mode) && m_usesError)
        {
            fail(
                "%mode " + m_mode + " 不支持 error 产生式：GSS 的多路径探索本身就是恢复，"
                "显式 error 会让「哪些路径进入错误状态」无法定义；请改用 "
                "slr / lalr / lr1 / ielr（或 lr0）");
            return {};
        }
        if ((m_mode == "dd") && m_usesError)
        {
            fail(
                "%mode dd 不支持 error 产生式：递归下降是「照产生式右部往下"
                "调用」，没有「弹栈到最近的恢复点」这个动作；请改用 "
                "slr / lalr / lr1 / ielr，或改成 %sync SEMI 这种同步 Token");
            return {};
        }
        /* %sync 在 GLR 下是允许的：GLR 的恢复是「死路时吞 Token」，
         * 同步 Token 就是吞的边界（吞到锚点为止）。 */
        /* %sync 在 DD 下也是允许的：递归下降没有恢复栈，解析失败时把
         * 同步 Token 当锚点往外跳过一段，再接着试。 */
    }
    // 校验：%lookahead 只配 lrk / lalrk。文法自己写了 %mode 时在这里就拦住；
    // 没写 %mode 时实际模式由命令行决定（--mode 覆盖），入口会再查一次。
    if (!lexMode && m_lookaheadDeclared && m_modeDeclared && (m_mode != "lrk") &&
        (m_mode != "lalrk") && (m_mode != "slrk") && (m_mode != "llk") &&
        (m_mode != "ll0"))
    {
        fail("%lookahead 只配 %mode lrk / lalrk / slrk / llk / ll0，当前是 "
             "%mode " +
             m_mode +
             "：展望符长度要按 k 元组建表，只有这几种模式这么做");
        return {};
    }
    // 校验：模式名里写了 k（LL(2) / SLR(3) …）就不再接受另一个 %lookahead
    if ((m_modeLookahead >= 0) && m_lookaheadDeclared &&
        (m_lookahead != m_modeLookahead))
    {
        fail("模式名里已经写了 k = " + std::to_string(m_modeLookahead) +
             "，%lookahead 又给了 " + std::to_string(m_lookahead) +
             "：两处必须一致（选一处写就够了）");
        return {};
    }
    // 校验：k = 0 只有 LL 家族有意义（LR(0) 是另一个模式，见 %mode lr0）
    if ((m_modeLookahead == 0 || (m_lookaheadDeclared && (m_lookahead == 0))) &&
        (m_modeDeclared) && (m_mode != "ll0") && (m_mode != "llk") &&
        (m_mode != "ll1"))
    {
        fail("k = 0 只有 LL 家族有意义（那就是 LL(0)，等价于 %mode ll0）："
             "LR 家族的 k = 0 是另一个模式 %mode lr0");
        return {};
    }
    // 校验：%cpp_only 与文法自己写的 %lang 是否打架。只写了 %cpp_only 时
    // 校验：%destructor 必须配 %union，成员必须存在，且只在 C / C++17 里有意义
    for (const auto &entry : m_destructors)
    {
        if (lexMode)
        {
            break; // lex 模式没有「丢弃语义值」这一层，%destructor 被忽略
        }
        const auto linePtrIt = m_destructorLines.find(entry.first);
        const std::size_t *linePtr =
            linePtrIt != m_destructorLines.end() ? &linePtrIt->second : nullptr;
        const std::size_t line = (linePtr != nullptr) ? *linePtr : 0u;
        if (!m_unionDeclared)
        {
            fail("%destructor <" + entry.first +
                     "> 需要 %union：清理代码绑在 %union 成员上，请先写 %union "
                     "{ ... }",
                 line);
            return {};
        }
        bool declared = false;
        for (const UnionMember &candidate : m_unionMembers)
        {
            declared = declared || (candidate.name == entry.first);
        }
        if (!declared)
        {
            fail("%union 里没有成员 " + entry.first + "（%destructor <" +
                     entry.first + "> 绑定的）",
                 line);
            return {};
        }
        if (m_langDeclared && (m_lang == "python"))
        {
            fail("%lang python 不支持 %destructor：Python 的语义值是任意对象，"
                 "靠引用计数回收，没有「丢弃时调清理代码」这一层",
                 line);
            return {};
        }
        if (m_langDeclared && (m_lang == "rust"))
        {
            fail("%lang rust 不支持 %destructor：Rust 的语义值按值拷贝，"
                 "丢弃时由所有权自动回收，没有「丢弃时调清理代码」这一层",
                 line);
            return {};
        }
        if (m_langDeclared && (m_lang == "go"))
        {
            fail("%lang go 不支持 %destructor：Go 的语义值由 GC 回收，"
                 "没有「丢弃时调清理代码」这一层",
                 line);
            return {};
        }
        if (m_modeDeclared && isGlrMode(m_mode))
        {
            fail("%mode " + m_mode + " 不支持 %destructor：结果重建的丢弃发生在路径合并里，"
                 "哪些值被丢、丢几次都没有确定答案；请改用 lr0 / slr / lalr / "
                 "lr1 / ielr",
                 line);
            return {};
        }
        if (m_modeDeclared && (m_mode == "dd"))
        {
            fail("%mode dd 不支持 %destructor：回溯式解析会把同一条产生式试"
                 "很多遍，哪些值被丢弃、丢几次都没有确定答案；请改用 lr0 / "
                 "slr / lalr / lr1 / ielr",
                 line);
            return {};
        }
    }
    // 缺省目标语言本来就是 cpp17，命令行再传 --lang c 由入口拦。
    if (m_cppOnly && m_langDeclared && (m_lang != "cpp17"))
    {
        fail("%cpp_only 与 %lang " + m_lang +
             " 冲突：%cpp_only 表示这份文法只能生成 C++17 产物");
        return {};
    }
    // 校验：RHS 符号必须已声明为终结符或非终结符
    std::set<std::string> known;
    for (const std::string &name : m_tokenNames)
    {
        known.insert(name);
    }
    if (m_usesError)
    {
        // error 是伪终结符，允许出现在右部（它不在 m_tokenNames 里）
        known.insert(k_errorTokenName);
    }
    for (const auto &rule : m_rules)
    {
        known.insert(rule.first);
    }
    for (const auto &rule : m_rules)
    {
        const std::string &lhs = rule.first;
        if (known.count(lhs) && m_tokenNames.end() !=
                std::find(m_tokenNames.begin(), m_tokenNames.end(), lhs))
        {
            fail("非终结符 " + lhs + " 与终结符同名");
            return {};
        }
        for (const Production &production : rule.second)
        {
            for (const std::string &symbol : production.rhs)
            {
                if (!known.count(symbol))
                {
                    fail("规则 " + lhs + " 使用了未定义的符号 " +
                             symbol,
                         lineOf(production.sourceOffset));
                    return {};
                }
            }
        }
    }
    // 校验：%type <成员> / %token <成员> 绑的符号必须真的存在（是已声明的 Token，
    // 或某条规则的左部）。写错名字时以前是静默通过——成员绑定挂在了一个不存在的
    // 符号上，产物照常生成，要等动作里引用到才发现侧面信息。
    for (const auto &entry : m_symbolValueMember)
    {
        if (known.count(entry.first) != 0)
        {
            continue;
        }
        const auto linePtrIt = m_symbolMemberLines.find(entry.first);
        const std::size_t *linePtr =
            linePtrIt != m_symbolMemberLines.end() ? &linePtrIt->second : nullptr;
        fail("符号 " + entry.first + " 没有定义：%type <" + entry.second +
                 "> / %token <" + entry.second +
                 "> 只能绑到已声明的 Token 或某条规则的左部（名字写错了？）",
             (linePtr != nullptr) ? *linePtr : 0u);
        return {};
    }
    // 校验：%start 必须指向已定义的非终结符
    // （漏了这一步，%start 写错名会一路走到 FIRST/FOLLOW 计算，最后以
    // 「键不存在」这种内部错误暴露出来，看不出是文法写错了）
    if (!lexMode && m_start.has_value() && !m_rules.count(*m_start))
    {
        fail("%start " + *m_start +
                 " 不是已定义的非终结符；请把 %start 指向某条规则左部",
             lineOf(m_startOffset));
        return {};
    }
    // 校验：%prec 指定的 Token 必须已声明
    for (const auto &rule : m_rules)
    {
        for (std::size_t index = 0; index < rule.second.size(); ++index)
        {
            const auto precIt = m_productionPrec.find(
                std::make_pair(rule.first, static_cast<int>(index)));
            const std::string *prec =
                precIt != m_productionPrec.end() ? &precIt->second : nullptr;
            if (prec == nullptr)
            {
                continue;
            }
            bool declared = false;
            for (const std::string &name : m_tokenNames)
            {
                declared = declared || (name == *prec);
            }
            if (!declared)
            {
                fail("规则 " + rule.first +
                     " 的 %prec 引用了未声明的 Token " +
                         *prec,
                     lineOf(
                         rule.second[static_cast<std::size_t>(
                             index)]
                             .sourceOffset));
                return {};
            }
        }
    }
    // 校验：%identifier 与 VARIABLE 的词法规则不能同时声明
    if (m_identifierPattern.has_value() &&
        (m_tokenDefs.count("VARIABLE") || m_regexDefs.count("VARIABLE")))
    {
        fail(
            "%identifier 与 VARIABLE 的词法规则冲突：%identifier 已经定义了 "
            "VARIABLE（关键字另查表），请二选一");
        return {};
    }
    // 校验：每条产生式都必须有语义动作，除非文法是 %tree
    if (!m_treeEnabled && !lexMode)
    {
        for (const auto &rule : m_rules)
        {
            for (const Production &production : rule.second)
            {
                if (strip(production.action).empty())
                {
                    fail("规则 " + rule.first +
                             " 缺少语义动作；请写动作或启用 %tree",
                         lineOf(production.sourceOffset));
                    return {};
                }
                // $code / $message 读的是「最近一条错误」，只有 error 产生式里
                // 才有意义——写在别处只会读到一条陈旧的（或没有）错误。
                if ((production.action.find("$code") != std::string::npos) ||
                    (production.action.find("$message") != std::string::npos))
                {
                    const bool hasError = std::find(
                        production.rhs.begin(), production.rhs.end(),
                        k_errorTokenName) != production.rhs.end();
                    if (!hasError)
                    {
                        fail("规则 " + rule.first +
                                 " 的动作里用了 $code / $message，但它们只在 "
                                 "error 产生式里有意义（读的是当前这条错误的码与"
                                 "说明）；请写进 `stmt := error SEMI { ... }` "
                                 "这样的规则里",
                             lineOf(production.sourceOffset));
                        return {};
                    }
                }
            }
        }
    }
    // 校验：%union / %type / %token <成员> 的完整性与自洽性
    validateUnionUsage();

    GrammarConfig config;
    config.tokens = m_tokenNames;
    config.tokenDefs = m_tokenDefs;
    // lex 模式可以没有产生式（也就没有起始符号）；其它模式上面已经保证有值
    config.grammar.start = m_start.has_value() ? *m_start : std::string();
    config.grammar.rules = m_rules;
    config.tree = m_treeEnabled;
    config.teach = m_teachEnabled;
    config.mermaid = m_mermaidEnabled;
    config.maxStackDepth = m_maxStackDepth;
    config.maxErrors = m_maxErrors;
    config.errorRecoveryTokens = m_errorRecoveryTokens;
    config.usesError = m_usesError;
    config.usesSync = m_syncDeclared;
    config.syncTokens = m_syncTokens;
    config.mode = m_mode;
    config.modeDeclared = m_modeDeclared;
    config.exampleInput = m_exampleInput;
    // k 的来源有两处：模式名里的括号（LL(2) / SLR(k)）与 %lookahead；
    // 前者优先（上面已经校验过两者不冲突）
    config.destructors = m_destructors;
    config.destructorLines = m_destructorLines;
    config.errorContext = m_errorContext;
    config.errorContextDeclared = m_errorContextDeclared;
    config.lookahead = (m_modeLookahead >= 0) ? m_modeLookahead : m_lookahead;
    config.lookaheadDeclared = m_lookaheadDeclared || (m_modeLookahead >= 0);
    config.lang = m_lang;
    config.langDeclared = m_langDeclared;
    config.buildFiles = m_buildFiles;
    config.buildFilesDeclared = m_buildFilesDeclared;
    config.cppOnly = m_cppOnly;
    config.pythonClass = m_pythonClass;
    config.pythonClassExplicit = m_pythonClassDeclared;
    config.packageName = m_packageName;
    config.packageDeclared = m_packageDeclared;
    config.packageLine = m_packageLine;
    config.imports = m_imports;
    config.importLines = m_importLines;
    config.hasClassBody = m_hasClassBody;
    config.classBody = m_classBody;
    config.classBodyLine = m_classBodyLine;
    config.cppNamespace = m_cppNamespace;
    config.cppNamespaceExplicit = m_cppNamespaceDeclared;
    config.cPrefix = m_cPrefix;
    config.cPrefixExplicit = m_cPrefixDeclared;
    config.cppLexer = m_cppLexer;
    config.cppLexerDeclared = m_cppLexerDeclared;
    for (const std::string &part : m_userCodeParts)
    {
        config.userCode += part;
    }
    config.userBlocks = m_userCodeParts;
    config.hasEpilogue = m_hasEpilogue;
    config.epilogue = m_epilogue;
    config.epilogueLine = m_epilogueLine;
    config.mainBlocks = m_mainBlocks;
    config.mainBlockLines = m_mainBlockLines;
    config.userBlockLines = m_userBlockLines;
    config.unionLine = m_unionLine;
    config.priority.precedence = m_precedence;
    config.priority.associativity = m_associativity;
    config.priority.productionPrec = m_productionPrec;
    config.priority.expectConflicts = m_expectConflicts;
    config.regexDefs = m_regexDefs;
    config.regexDiagnostics = m_regexDiagnostics;
    config.identifierPattern = m_identifierPattern;
    config.commits = m_commits;
    config.tokenLexOrder = m_tokenLexOrder;
    config.tokenAliases = m_tokenAliases;
    config.spaceDeclared = m_spaceDeclared;
    config.spaceChars = m_spaceChars;
    config.copies = m_copies;
    config.valueType = m_valueType;
    config.unionMembers = m_unionMembers;
    config.hasUnion = m_unionDeclared;
    config.unionBody = m_unionBody;
    config.symbolValueMember = m_symbolValueMember;
    return config;
}

} // namespace

std::string GrammarLoader::decodeStringLiteral(const std::string &text)
{
    const std::string body = text.substr(1, text.size() - 2);
    std::string result;
    std::size_t index = 0;
    while (index < body.size())
    {
        const char ch = body[index];
        if ((ch == '\\') && (index + 1 < body.size()))
        {
            const char next = body[index + 1];
            switch (next)
            {
            case 'n': result += '\n'; break;
            case 'r': result += '\r'; break;
            case 't': result += '\t'; break;
            case 'v': result += '\v'; break;
            case 'f': result += '\f'; break;
            // \b：空白通配（匹配任意一个空白字符），见 k_blankWildcardChar
            case 'b': result += k_blankWildcardChar; break;
            case '0': result += '\0'; break;
            case '\\': result += '\\'; break;
            case '"': result += '"'; break;
            case '\'': result += '\''; break;
            default: result += next; break;
            }
            index += 2;
        }
        else
        {
            result += ch;
            ++index;
        }
    }
    return result;
}

std::string GrammarLoader::decodeRegexLiteral(const std::string &text)
{
    const std::string body = text.substr(1, text.size() - 2);
    std::string result;
    std::size_t index = 0;
    while (index < body.size())
    {
        const char ch = body[index];
        if ((ch == '\\') && (index + 1 < body.size()))
        {
            const char next = body[index + 1];
            // 只还原 \\、\"、\' 三种，其余转义（\d、\* ……）原样交给正则解析器
            if ((next == '"') || (next == '\'') || (next == '\\'))
            {
                result += next;
                index += 2;
                continue;
            }
        }
        result += ch;
        ++index;
    }
    return result;
}

bool GrammarLoader::splitDefineLiteral(const std::string &text,
                                       const std::string &what,
                                       std::vector<std::string> &out,
                                       Error &error)
{
    std::vector<std::string> branches;
    std::string current;
    const std::string body = text.substr(1, text.size() - 2);
    std::size_t index = 0;
    while (index < body.size())
    {
        const char ch = body[index];
        if (ch == '\\')
        {
            if (index + 1 >= body.size())
            {
                return fail(error, what + " 字面量末尾存在孤立转义符");
            }
            const char next = body[index + 1];
            switch (next)
            {
            case 'n': current += '\n'; break;
            case 'r': current += '\r'; break;
            case 't': current += '\t'; break;
            case 'v': current += '\v'; break;
            case 'f': current += '\f'; break;
            // \b：空白通配（匹配任意一个空白字符），见 k_blankWildcardChar
            case 'b': current += k_blankWildcardChar; break;
            case '0': current += '\0'; break;
            case '\\': current += '\\'; break;
            case '"': current += '"'; break;
            case '|': current += '|'; break;
            default: current += next; break;
            }
            index += 2;
            continue;
        }
        if (ch == '|')
        {
            if (current.empty())
            {
                return fail(error, what + " 不允许空分支");
            }
            branches.push_back(current);
            current.clear();
        }
        else
        {
            current += ch;
        }
        ++index;
    }
    if (current.empty())
    {
        return fail(error, what + " 不允许空分支");
    }
    branches.push_back(current);
    {
        std::set<std::string> unique(branches.begin(), branches.end());
        if (unique.size() != branches.size())
        {
            return fail(error, what + " 内存在重复字面量");
        }
    }
    out = branches;
    return true;
}

bool GrammarLoader::loadGrammarText(const std::string &text,
                                    GrammarConfig &out, Error &error)
{
    return loadGrammarText(text, DefineMap(), out, error);
}

bool GrammarLoader::loadGrammarText(const std::string &text,
                                    const DefineMap &defines,
                                    GrammarConfig &out, Error &error)
{
    // 没有文件上下文：`include "…"` 的相对路径按当前工作目录算。
    return parseGrammar(text, defines, std::string(), std::string(), out, error);
}

bool GrammarLoader::parseGrammar(const std::string &text,
                                 const DefineMap &defines,
                                 const std::string &baseDir,
                                 const std::string &gramPath,
                                 GrammarConfig &out, Error &error)
{
    // 条件编译在解析之前做：预处理层负责丢掉条件为假的整行，并把行号映射
    // 交给解析器，这样报错行号始终指向原文件。没有 %if 系列指令时这里是
    // 恒等变换（文本按原样返回），产物与从前逐字节一致。
    PreprocessResult processed;
    if (!Preprocessor::run(text, defines, processed, error))
    {
        return false;
    }
    // %token NAME include "csv" 展开：Preprocessor 之后做，让 %if 假分支里的
    // include 不读 CSV；唯一性检查也走全量扫描（与「先展开后到」的顺序无关）。
    // 展开阶段再生成一份「预处理后行号 -> 原文件行号」的映射，把 Preprocessor
    // 的映射与 TokenDict 的映射合成到 GramParser 用的那张表里。
    TokenDictExpandResult dictExpanded;
    const std::string &preprocessed =
        processed.lineMap.empty() ? processed.text : processed.text;
    if (!TokenDict::expand(preprocessed, baseDir, gramPath, dictExpanded,
                           error))
    {
        return false;
    }
    // 合成行号映射：TokenDict 没展开 → 直接用 Preprocessor 的；展开了若干 →
    // 处理后第 N 行仍然对应原文件同一行（展开发生在原 include 行的内部，
    // 处理后行数与原行数逐行对应），所以最终映射就是 Preprocessor 的恒等映射
    // 套到展开后文本上。
    std::vector<std::size_t> combinedLineMap;
    if (!processed.lineMap.empty())
    {
        combinedLineMap.reserve(dictExpanded.lineMap.size());
        // dictExpanded.lineMap[k] = 展开后第 k+1 行 → 处理前行号；
        // 再用 processed.lineMap[处理前行号 - 1] = 原文件行号。
        for (std::size_t k = 0u; k < dictExpanded.lineMap.size(); ++k)
        {
            const std::size_t processedLine = dictExpanded.lineMap[k];
            if (processedLine == 0u || processedLine > processed.lineMap.size())
            {
                combinedLineMap.push_back(0u);
                continue;
            }
            combinedLineMap.push_back(processed.lineMap[processedLine - 1u]);
        }
    }
    const std::string &source =
        processed.lineMap.empty() ? dictExpanded.text : dictExpanded.text;

    // 容忍记事本等工具写出的 UTF-8 BOM
    GrammarConfig config;
    if ((source.size() >= 3) &&
        (static_cast<unsigned char>(source[0]) == 0xEF) &&
        (static_cast<unsigned char>(source[1]) == 0xBB) &&
        (static_cast<unsigned char>(source[2]) == 0xBF))
    {
        GramParser parser(source.substr(3),
                          combinedLineMap.empty() ? processed.lineMap
                                                  : combinedLineMap,
                          baseDir);
        config = parser.parse();
        if (parser.failed())
        {
            return fail(error, parser.error());
        }
    }
    else
    {
        GramParser parser(source,
                          combinedLineMap.empty() ? processed.lineMap
                                                  : combinedLineMap,
                          baseDir);
        config = parser.parse();
        if (parser.failed())
        {
            return fail(error, parser.error());
        }
    }
    out = config;
    return true;
}

// ---- 文件级 %include：把被引 .gram 的内容内联进来 ----
// 仅展开「顶层」的 `%include "路径"`（允许 `%include='路径'` 与单/双引号）；
// 处于 %code / %class_body / %epilogue / %main 的多行体内时不展开，避免误伤
// 代码块里的文本。支持递归包含并做循环包含检测；相对路径按「包含它的文件」
// 所在目录解析。注意：内联后报错行号指向拼接后的文本（含被引文件内容）。
namespace {

bool parseTopLevelInclude(const std::string &rawLine, std::string *outPath)
{
    std::size_t i = 0;
    const std::size_t n = rawLine.size();
    while (i < n && (rawLine[i] == ' ' || rawLine[i] == '\t')) ++i;
    if (i + 8 > n) return false;
    if (rawLine.compare(i, 8, "%include") != 0) return false;
    std::size_t j = i + 8;
    while (j < n && (rawLine[j] == ' ' || rawLine[j] == '\t')) ++j;
    if (j < n && rawLine[j] == '=') {
        ++j;
        while (j < n && (rawLine[j] == ' ' || rawLine[j] == '\t')) ++j;
    }
    if (j >= n || (rawLine[j] != '"' && rawLine[j] != '\'')) return false;
    const char q = rawLine[j];
    ++j;
    const std::size_t end = rawLine.find(q, j);
    if (end == std::string::npos) return false;
    const std::string val = rawLine.substr(j, end - j);
    if (rawLine.find_first_not_of(" \t", end + 1) != std::string::npos) return false;
    if (val.empty()) return false;
    *outPath = val;
    return true;
}

bool expandIncludesRecursive(const std::string &text,
                             const std::filesystem::path &baseDir,
                             std::vector<std::filesystem::path> *stack,
                             std::string *out, Error &error)
{
    enum class Block { None, Code, ClassBody, Epilogue };
    Block block = Block::None;
    std::size_t pos = 0;
    const std::size_t n = text.size();
    while (pos < n) {
        const std::size_t nl = text.find('\n', pos);
        const bool hasNl = (nl != std::string::npos);
        const std::string line =
            hasNl ? text.substr(pos, nl - pos) : text.substr(pos);
        const std::size_t nextPos = hasNl ? (nl + 1) : n;
        std::size_t s = 0;
        while (s < line.size() && (line[s] == ' ' || line[s] == '\t')) ++s;
        std::string trimmed;
        {
            std::size_t e = line.size();
            while (e > s && (line[e - 1] == ' ' || line[e - 1] == '\t' ||
                             line[e - 1] == '\r'))
                --e;
            trimmed = line.substr(s, e - s);
        }
        if (block == Block::None) {
            if (trimmed == "%code") block = Block::Code;
            else if (trimmed == "%class_body") block = Block::ClassBody;
            else if (trimmed == "%epilogue" || trimmed == "%main")
                block = Block::Epilogue;
        } else {
            if (block == Block::Code && trimmed == "%endcode")
                block = Block::None;
            else if (block == Block::ClassBody && trimmed == "%endbody")
                block = Block::None;
            else if (block == Block::Epilogue && !trimmed.empty() &&
                     trimmed[0] == '%')
                block = Block::None;
        }
        if (block == Block::None) {
            std::string incPath;
            if (parseTopLevelInclude(line, &incPath)) {
                std::filesystem::path resolved =
                    std::filesystem::u8path(incPath);
                if (resolved.is_relative()) {
                    const std::filesystem::path base =
                        baseDir.empty()
                            ? std::filesystem::current_path()
                            : baseDir;
                    resolved = (base / resolved).lexically_normal();
                } else {
                    resolved = resolved.lexically_normal();
                }
                std::error_code ec;
                const std::filesystem::path canon =
                    std::filesystem::canonical(resolved, ec);
                const std::filesystem::path &key = ec ? resolved : canon;
                for (const auto &seen : *stack) {
                    if (seen == key) {
                        fail(error,
                             "%include 循环包含: " + resolved.u8string());
                        return false;
                    }
                }
                std::ifstream stream(resolved, std::ios::binary);
                if (!stream) {
                    fail(error,
                         "%include 的文件打不开: " + resolved.u8string());
                    return false;
                }
                const std::string included(
                    (std::istreambuf_iterator<char>(stream)),
                    std::istreambuf_iterator<char>());
                stack->push_back(key);
                if (!expandIncludesRecursive(included,
                                            resolved.parent_path().u8string(),
                                            stack, out, error)) {
                    return false;
                }
                stack->pop_back();
                pos = nextPos;
                continue;
            }
        }
        out->append(line);
        if (hasNl) out->push_back('\n');
        pos = nextPos;
    }
    return true;
}

}  // namespace

bool GrammarLoader::loadGrammarFile(const std::string &path,
                                    GrammarConfig &out, Error &error)
{
    return loadGrammarFile(path, DefineMap(), out, error);
}

bool GrammarLoader::loadGrammarFile(const std::string &path,
                                    const DefineMap &defines,
                                    GrammarConfig &out, Error &error)
{
    std::ifstream stream(std::filesystem::u8path(path), std::ios::binary);
    if (!stream)
    {
        return fail(error, "无法打开文法文件: " + path);
    }
    std::string content((std::istreambuf_iterator<char>(stream)),
                         std::istreambuf_iterator<char>());
    // 文件级 %include：把被引 .gram 的内容内联进来（递归、防环、相对路径按包含
    // 文件所在目录解析）。仅展开顶层 %include，代码块内不展开。
    {
        std::error_code ec;
        const std::filesystem::path selfPath =
            std::filesystem::canonical(std::filesystem::u8path(path), ec);
        const std::filesystem::path &selfKey =
            ec ? std::filesystem::u8path(path) : selfPath;
        std::vector<std::filesystem::path> includeStack;
        includeStack.push_back(selfKey);
        std::string expanded;
        if (!expandIncludesRecursive(content,
                                    std::filesystem::u8path(path)
                                        .parent_path()
                                        .u8string(),
                                    &includeStack, &expanded, error)) {
            return false;
        }
        content.swap(expanded);
    }
    // 段标记上的 `include "…"` 与 %copy 一样，相对路径以文法文件所在目录为基准
    const std::filesystem::path base =
        std::filesystem::u8path(path).parent_path();
    GrammarConfig config;
    if (!parseGrammar(content, defines, base.u8string(), path, config, error))
    {
        return false;
    }
    for (CopySpec &copy : config.copies)
    {
        if (!copy.path.empty() &&
            std::filesystem::u8path(copy.path).is_relative())
        {
            copy.path = (base / std::filesystem::u8path(copy.path)).lexically_normal().u8string();
        }
    }
    out = config;
    return true;
}

std::string GrammarLoader::defaultLoadText()
{
    return Utils::loadTemplate("c99_default_grammar.gram");
}

bool isValidCppNamespaceName(const std::string &name)
{
    return isCppIdentifier(name, true);
}

void GramParser::parseCppLexerDirective()
{
    const std::size_t line =
        (peekToken() == nullptr) ? 0u : lineOf(peekToken()->start);
    ++m_pos;
    const std::string token = peekText();
    if ((peekToken() == nullptr) || (token == "\n"))
    {
        fail("%cpp_lexer 后面要写词法引擎：std（缺省，交给 std::regex）"
             "或 dfa（等价子集使用 DFA 表）或 vm（支持资源预算的有序运行时）",
             line);
        return;
    }
    if ((token != "std") && (token != "dfa") && (token != "vm"))
    {
        fail("%cpp_lexer 只接受 std、dfa 或 vm，实际得到 '" + token + "'", line);
        return;
    }
    if (m_cppLexerDeclared)
    {
        fail("%cpp_lexer 只能声明一次", line);
        return;
    }
    m_cppLexer = token;
    m_cppLexerDeclared = true;
    ++m_pos;
    if (peekText() == "\n")
    {
        ++m_pos;
    }
    else if (peekToken() != nullptr)
    {
        fail("%cpp_lexer " + token + " 后有多余内容 '" + peekText() + "'", line);
    }
}

void GramParser::parsePrefixDirective()
{
    const std::size_t line = (peekToken() == nullptr) ? 0u : lineOf(peekToken()->start);
    ++m_pos;
    const std::string token = peekText();
    if (peekToken() == nullptr || token == "\n") { if (token == "\n") ++m_pos; return; }
    if (!isValidCPrefixName(token)) {
        fail("%prefix 的名字必须是 C 标识符（[A-Za-z_][A-Za-z0-9_]*）、不能以 Parser/PARSER 开头且不能是关键字，实际得到 '" + token + "'", line);
        return;
    }
    if (m_cPrefixDeclared) { fail("%prefix 只能声明一次", line); return; }
    m_cPrefix = token;
    m_cPrefixDeclared = true;
    ++m_pos;
    if (peekText() == "\n") ++m_pos;
    else if (peekToken() != nullptr) fail("%prefix " + token + " 后有多余内容 '" + peekText() + "'", line);
}

bool isValidCPrefixName(const std::string &name)
{
    if (name.empty()) return true;
    if (!isCppIdentifier(name, false)) return false;
    return name.rfind("Parser", 0) != 0 && name.rfind("PARSER", 0) != 0;
}

bool isValidPythonClassName(const std::string &name)
{
    if (name.empty() || (pythonKeywords().count(name) != 0))
    {
        return false;
    }
    return isCppIdentifier(name, false);
}

PriorityConfig GrammarLoader::priorityFromConfig(const GrammarConfig &cfg)
{
    return cfg.priority;
}

LexSpec GrammarLoader::lexSpecFromConfig(const GrammarConfig &cfg)
{
    LexSpec spec;
    spec.regex = cfg.regexDefs;
    spec.identifier = cfg.identifierPattern;
    spec.commits = cfg.commits;
    spec.tokenLexOrder = cfg.tokenLexOrder;
    spec.tokenAliases = cfg.tokenAliases;
    return spec;
}

PriorityConfig GrammarLoader::normalizePriority(const PriorityConfig *priority)
{
    if (priority == nullptr)
    {
        return PriorityConfig();
    }
    return *priority;
}


} // namespace ParseEngine
