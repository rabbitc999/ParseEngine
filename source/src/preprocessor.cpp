/**
 * @file preprocessor.cpp
 * @brief .gram 文本的条件编译预处理实现。
 */

#include "ParseEngine/preprocessor.hpp"

#include <cctype>
#include <string>

namespace ParseEngine
{
namespace
{

/**
 * @brief 去掉首尾空白。
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
 * @brief 按行切分，保留每行原本的行尾（`\n` 或 `\r\n`）。
 *
 * 保留行尾是为了「条件为真的行原样回写」——只有这样，没有条件编译指令的
 * 文法才可能与改动前逐字节相同。
 * @param text 输入文本。
 * @return 行列表（每行含行尾；最后一行可能没有行尾）。
 */
std::vector<std::string> splitLines(const std::string &text)
{
    std::vector<std::string> lines;
    std::size_t index = 0u;
    while (index < text.size())
    {
        const std::size_t found = text.find('\n', index);
        if (found == std::string::npos)
        {
            lines.push_back(text.substr(index));
            break;
        }
        lines.push_back(text.substr(index, found - index + 1u));
        index = found + 1u;
    }
    return lines;
}

/**
 * @brief 去掉行尾的换行符。
 * @param line 一行（可能含行尾）。
 * @return 不含行尾的内容。
 */
std::string trimLineEnd(const std::string &line)
{
    std::size_t end = line.size();
    while ((end > 0u) && ((line[end - 1u] == '\n') || (line[end - 1u] == '\r')))
    {
        --end;
    }
    return line.substr(0u, end);
}

/**
 * @brief 变量名首字符。
 * @param ch 字符。
 * @return 合法返回 true。
 */
bool isNameStart(char ch)
{
    return (ch == '_') ||
           ((ch >= 'a') && (ch <= 'z')) || ((ch >= 'A') && (ch <= 'Z'));
}

/**
 * @brief 变量名后续字符。
 * @param ch 字符。
 * @return 合法返回 true。
 */
bool isNamePart(char ch)
{
    return isNameStart(ch) || ((ch >= '0') && (ch <= '9'));
}

/**
 * @brief 判断一段文本是否以某个指令关键字开头。
 *
 * 关键字后面必须是空白或行尾，所以 `%iffoo` / `%endifx` 不会被认成指令。
 * @param text 行首内容（已跳过空白与注释）。
 * @param keyword 关键字（如 `%if`）。
 * @return 命中返回 true。
 */
bool matchKeyword(const std::string &text, const std::string &keyword)
{
    if (text.compare(0u, keyword.size(), keyword) != 0)
    {
        return false;
    }
    if (text.size() == keyword.size())
    {
        return true;
    }
    const char ch = text[keyword.size()];
    return (ch == ' ') || (ch == '\t') || (ch == '\r');
}

/**
 * @brief 判断标记关键字后面只剩空白（即标记单独成行）。
 * @param body 一行（不含行尾）。
 * @param from 关键字之后的位置。
 * @return 只剩空白返回 true。
 */
bool isMarkerTail(const std::string &body, std::size_t from)
{
    for (std::size_t index = from; index < body.size(); ++index)
    {
        const char ch = body[index];
        if ((ch != ' ') && (ch != '\t') && (ch != '\r'))
        {
            return false;
        }
    }
    return true;
}

/**
 * @brief 找出一行里第一个「有意义」的字符位置。
 *
 * 跳过空白、`//` 行注释与 C 风格块注释；字符串 / 字符字面量内部的内容一律不当
 * 指令看待。顺带更新跨行状态：块注释与 `%code ... %endcode` 代码块
 * （后者对 `%if` 是不透明区）。
 * @param body 一行（不含行尾）。
 * @param inBlockComment 跨行块注释状态（输入输出）。
 * @param inCodeBlock 跨行 `%code ... %endcode` 状态（输入输出）。
 * @return 位置；整行都是空白 / 注释时返回 npos。
 */
std::size_t firstMeaningful(const std::string &body, bool &inBlockComment,
                            bool &inCodeBlock)
{
    std::size_t index = 0u;
    while (index < body.size())
    {
        const char ch = body[index];
        if (inBlockComment)
        {
            const std::size_t found = body.find("*/", index);
            if (found == std::string::npos)
            {
                return std::string::npos;
            }
            inBlockComment = false;
            index = found + 2u;
            continue;
        }
        if ((ch == ' ') || (ch == '\t') || (ch == '\r'))
        {
            ++index;
            continue;
        }
        if ((ch == '/') && (index + 1u < body.size()) &&
            (body[index + 1u] == '/'))
        {
            return std::string::npos;
        }
        if ((ch == '/') && (index + 1u < body.size()) &&
            (body[index + 1u] == '*'))
        {
            inBlockComment = true;
            index += 2u;
            continue;
        }
        if ((ch == '"') || (ch == '\''))
        {
            const char quote = ch;
            std::size_t cursor = index + 1u;
            bool closed = false;
            while (cursor < body.size())
            {
                if (body[cursor] == '\\')
                {
                    cursor += 2u;
                    continue;
                }
                if (body[cursor] == quote)
                {
                    ++cursor;
                    closed = true;
                    break;
                }
                ++cursor;
            }
            if (!closed)
            {
                return std::string::npos;
            }
            index = cursor;
            continue;
        }
        if ((body.compare(index, 5u, "%code") == 0) &&
            isMarkerTail(body, index + 5u))
        {
            // %code ... %endcode：用户代码不透明区
            inCodeBlock = true;
            return index;
        }
        if ((body.compare(index, 8u, "%endcode") == 0) &&
            isMarkerTail(body, index + 8u))
        {
            inCodeBlock = false;
            return index;
        }
        return index;
    }
    return std::string::npos;
}

/**
 * @brief 去掉条件里的 `//` 行注释与 C 风格块注释，再去掉首尾空白。
 * @param text 条件原文。
 * @return 清理后的条件。
 */
std::string cleanCondition(const std::string &text)
{
    std::string result;
    std::size_t index = 0u;
    while (index < text.size())
    {
        if ((text[index] == '/') && (index + 1u < text.size()) &&
            (text[index + 1u] == '/'))
        {
            break;
        }
        if ((text[index] == '/') && (index + 1u < text.size()) &&
            (text[index + 1u] == '*'))
        {
            const std::size_t found = text.find("*/", index + 2u);
            if (found == std::string::npos)
            {
                break;
            }
            index = found + 2u;
            continue;
        }
        result += text[index];
        ++index;
    }
    return strip(result);
}

/**
 * @brief 条件表达式递归下降求值器。
 *
 * 文法（优先级从低到高）：OR := AND ('||' AND)*；AND := NOT ('&&' NOT)*；
 * NOT := '!' NOT | ATOM；ATOM := NAME | '(' OR ')'。
 */
class ConditionParser
{
public:
    /**
     * @brief 构造求值器。
     * @param text 条件表达式原文。
     * @param defines 变量表。
     */
    ConditionParser(const std::string &text, const DefineMap &defines)
        : m_text(text), m_defines(defines)
    {
    }

    /**
     * @brief 求值整个表达式。
     * @return 真假。
     * @throws GrammarLoadError 表达式语法错误。
     */
    bool parse()
    {
        const bool value = parseOr();
        if (failed())
        {
            return false;
        }
        skipSpace();
        if (m_pos < m_text.size())
        {
            fail("条件表达式里有非法字符 '" +
                 std::string(1u, m_text[m_pos]) + "'");
            return false;
        }
        return value;
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

private:
    /**
     * @brief 记录第一个错误（错误槽已满时不覆盖）。
     * @param message 错误说明。
     */
    void fail(const std::string &message)
    {
        ParseEngine::fail(m_error, message);
    }

    /**
     * @brief 跳过空白。
     */
    void skipSpace()
    {
        while ((m_pos < m_text.size()) &&
               ((m_text[m_pos] == ' ') || (m_text[m_pos] == '\t') ||
                (m_text[m_pos] == '\r')))
        {
            ++m_pos;
        }
    }

    /**
     * @brief 当前位置是否匹配一段文本。
     * @param text 期望的文本。
     * @return 命中返回 true。
     */
    bool accept(const std::string &text)
    {
        skipSpace();
        if (m_text.compare(m_pos, text.size(), text) == 0)
        {
            m_pos += text.size();
            return true;
        }
        return false;
    }

    /**
     * @brief 报告「运算符缺操作数」。
     */
    void failMissingOperand()
    {
        fail(
            "条件表达式里运算符缺操作数（'&&' / '||' / '!' 两侧都要有变量或括号）");
    }

    /**
     * @brief OR := AND ('||' AND)*
     * @return 真假。
     */
    bool parseOr()
    {
        bool value = parseAnd();
        if (failed())
        {
            return false;
        }
        while (!failed() && accept("||"))
        {
            const bool right = parseAnd();
            if (failed())
            {
                return false;
            }
            value = value || right;
        }
        return value;
    }

    /**
     * @brief AND := NOT ('&&' NOT)*
     * @return 真假。
     */
    bool parseAnd()
    {
        bool value = parseNot();
        if (failed())
        {
            return false;
        }
        while (!failed() && accept("&&"))
        {
            const bool right = parseNot();
            if (failed())
            {
                return false;
            }
            value = value && right;
        }
        return value;
    }

    /**
     * @brief NOT := '!' NOT | ATOM
     * @return 真假。
     */
    bool parseNot()
    {
        skipSpace();
        if ((m_pos < m_text.size()) && (m_text[m_pos] == '!'))
        {
            ++m_pos;
            return !parseNot();
        }
        return parseAtom();
    }

    /**
     * @brief ATOM := NAME | '(' OR ')'
     * @return 真假。
     */
    bool parseAtom()
    {
        skipSpace();
        if (m_pos >= m_text.size())
        {
            failMissingOperand();
            return false;
        }
        const char ch = m_text[m_pos];
        if (ch == '(')
        {
            ++m_pos;
            const bool value = parseOr();
            if (failed())
            {
                return false;
            }
            skipSpace();
            if ((m_pos >= m_text.size()) || (m_text[m_pos] != ')'))
            {
                fail("条件表达式的括号不配平（少了 ')'）");
                return false;
            }
            ++m_pos;
            return value;
        }
        if (ch == ')')
        {
            fail("条件表达式的括号不配平（多了 ')'）");
            return false;
        }
        if (isNameStart(ch))
        {
            const std::size_t begin = m_pos;
            while ((m_pos < m_text.size()) && isNamePart(m_text[m_pos]))
            {
                ++m_pos;
            }
            const std::string name = m_text.substr(begin, m_pos - begin);
            const auto found = m_defines.find(name);
            // 没传 --define 的变量一律为假（不报错）
            return (found != m_defines.end()) && found->second;
        }
        if ((ch == '&') || (ch == '|'))
        {
            failMissingOperand();
            return false;
        }
        fail("条件表达式里有非法字符 '" + std::string(1u, ch) +
             "'（只支持 || / && / ! / 括号 / 变量名）");
        return false;
    }

    std::string m_text;        ///< 条件原文
    const DefineMap &m_defines; ///< 变量表
    std::size_t m_pos = 0u;    ///< 当前位置
    Error m_error;             ///< 第一个错误
};

/**
 * @brief %if 栈里的一层。
 */
struct IfFrame
{
    bool parentActive = true;  ///< 外层是否生效
    bool taken = false;        ///< 这一层是否已经有分支命中
    bool elseSeen = false;     ///< %else 是否已经出现
    bool active = true;        ///< 当前分支是否生效
    std::size_t line = 0u;     ///< %if 的原文件行号
};

/**
 * @brief 求值一份条件，并把错误挂到指定行。
 * @param text 条件原文。
 * @param defines 变量表。
 * @param line 指令所在的原文件行号。
 * @param out 真假（成功时写入）。
 * @param error 出错时写入错误说明。
 * @return 成功返回 true。
 */
bool evalCondition(const std::string &text, const DefineMap &defines,
                   std::size_t line, bool &out, Error &error)
{
    if (strip(text).empty())
    {
        return fail(
            error,
            "条件编译指令后面要写条件（例如 `%if DEBUG` 或 `%if DEBUG && !QUIET`）",
            line);
    }
    // 表达式层的报错没带行号，这里换成指令所在的行
    Error cause;
    if (!Preprocessor::evaluateCondition(text, defines, out, cause))
    {
        return fail(error, cause.message, line);
    }
    return true;
}

} // namespace

bool Preprocessor::run(const std::string &text, const DefineMap &defines,
                       PreprocessResult &out, Error &error)
{
    // 容忍记事本等工具写出的 UTF-8 BOM（与 GrammarLoader 的处理一致）
    const bool hasBom =
        (text.size() >= 3u) &&
        (static_cast<unsigned char>(text[0]) == 0xEFu) &&
        (static_cast<unsigned char>(text[1]) == 0xBBu) &&
        (static_cast<unsigned char>(text[2]) == 0xBFu);
    const std::string body = hasBom ? text.substr(3u) : text;

    const std::vector<std::string> lines = splitLines(body);
    std::vector<IfFrame> frames;
    std::string output;
    std::vector<std::size_t> map;
    bool inBlockComment = false;
    bool inCodeBlock = false;
    bool sawDirective = false;

    for (std::size_t index = 0u; index < lines.size(); ++index)
    {
        const std::string &line = lines[index];
        const std::string content = trimLineEnd(line);
        const std::size_t lineNumber = index + 1u;
        const bool active = frames.empty() ? true : frames.back().active;

        std::size_t position = std::string::npos;
        if (inCodeBlock)
        {
            // %code ... %endcode 里原样保留，直到单独成行的 %endcode
            if (strip(content) == "%endcode")
            {
                inCodeBlock = false;
            }
        }
        else
        {
            position = firstMeaningful(content, inBlockComment, inCodeBlock);
            if ((position != std::string::npos) &&
                (content.compare(position, 3u, "%{{") == 0))
            {
                return fail(error,
                            "%{{ ... }}% 语法已禁用；请改用单独成行的 "
                            "%code 与 %endcode",
                            lineNumber);
            }
        }

        const std::string head =
            (position == std::string::npos) ? std::string()
                                            : content.substr(position);
        const bool isDirective =
            !inCodeBlock &&
            ((position != std::string::npos) &&
             (head.compare(0u, 1u, "%") == 0));

        if (isDirective && (matchKeyword(head, "%ifdef") ||
                            matchKeyword(head, "%ifndef")))
        {
            return fail(
                error,
                "%ifdef / %ifndef 不支持：请写 `%if NAME` 或 `%if !NAME`",
                lineNumber);
        }

        if (isDirective && matchKeyword(head, "%if"))
        {
            sawDirective = true;
            bool value = false;
            if (!evalCondition(cleanCondition(head.substr(3u)), defines,
                               lineNumber, value, error))
            {
                return false;
            }
            IfFrame frame;
            frame.parentActive = active;
            frame.taken = value;
            frame.active = active && value;
            frame.line = lineNumber;
            frames.push_back(frame);
            continue;
        }
        if (isDirective && matchKeyword(head, "%elif"))
        {
            sawDirective = true;
            if (frames.empty())
            {
                return fail(
                    error,
                    "%elif 没有对应的 %if（每个 %elif 都必须写在某个 %if 块里）",
                    lineNumber);
            }
            IfFrame &frame = frames.back();
            if (frame.elseSeen)
            {
                return fail(error,
                            "%elif 不能出现在 %else 之后（第 " +
                                std::to_string(frame.line) +
                                " 行的 %if 已经有 %else 分支）",
                            lineNumber);
            }
            bool value = false;
            if (!evalCondition(cleanCondition(head.substr(5u)), defines,
                               lineNumber, value, error))
            {
                return false;
            }
            frame.active = frame.parentActive && !frame.taken && value;
            frame.taken = frame.taken || value;
            continue;
        }
        if (isDirective && matchKeyword(head, "%else"))
        {
            sawDirective = true;
            if (frames.empty())
            {
                return fail(
                    error,
                    "%else 没有对应的 %if（每个 %else 都必须写在某个 %if 块里）",
                    lineNumber);
            }
            IfFrame &frame = frames.back();
            if (frame.elseSeen)
            {
                return fail(error, "%else 一个 %if 块里最多写一次",
                            lineNumber);
            }
            const std::string tail = cleanCondition(head.substr(5u));
            if (!tail.empty())
            {
                return fail(error,
                            "%else 后面不能再写内容，实际得到 '" + tail + "'",
                            lineNumber);
            }
            frame.elseSeen = true;
            frame.active = frame.parentActive && !frame.taken;
            frame.taken = true;
            continue;
        }
        if (isDirective && matchKeyword(head, "%endif"))
        {
            sawDirective = true;
            if (frames.empty())
            {
                return fail(error, "%endif 没有对应的 %if（多余的 %endif）",
                            lineNumber);
            }
            const std::string tail = cleanCondition(head.substr(6u));
            if (!tail.empty())
            {
                return fail(error,
                            "%endif 后面不能再写内容，实际得到 '" + tail + "'",
                            lineNumber);
            }
            frames.pop_back();
            continue;
        }

        if (active)
        {
            output += line;
            map.push_back(lineNumber);
        }
    }

    if (!frames.empty())
    {
        return fail(error,
                    "%if 未闭合（这个 %if 没有对应的 %endif；" +
                        std::to_string(frames.size()) + " 层没配对）",
                    frames.back().line);
    }

    if (!sawDirective)
    {
        // 一个条件编译指令都没有：原样返回，保证产物逐字节相同
        out.text = text;
        out.lineMap.clear();
        return true;
    }
    out.text = std::move(output);
    out.lineMap = std::move(map);
    return true;
}

bool Preprocessor::evaluateCondition(const std::string &condition,
                                     const DefineMap &defines, bool &out,
                                     Error &error)
{
    const std::string text = strip(condition);
    if (text.empty())
    {
        return fail(error, "条件表达式为空");
    }
    ConditionParser parser(text, defines);
    const bool value = parser.parse();
    if (parser.failed())
    {
        return fail(error, parser.error());
    }
    out = value;
    return true;
}

bool Preprocessor::parseDefineValue(const std::string &value)
{
    if (value.empty())
    {
        return false;
    }
    std::string lowered;
    for (const char ch : value)
    {
        lowered += static_cast<char>(
            std::tolower(static_cast<unsigned char>(ch)));
    }
    return !((lowered == "0") || (lowered == "false") || (lowered == "no") ||
             (lowered == "off"));
}

bool Preprocessor::isValidName(const std::string &name)
{
    if (name.empty() || !isNameStart(name[0]))
    {
        return false;
    }
    for (const char ch : name)
    {
        if (!isNamePart(ch))
        {
            return false;
        }
    }
    return true;
}

bool Preprocessor::parseDefineArgument(const std::string &argument,
                                       std::string &outName, bool &outValue,
                                       Error &error)
{
    const std::size_t found = argument.find('=');
    const std::string name = (found == std::string::npos)
                                 ? argument
                                 : argument.substr(0u, found);
    if (!isValidName(name))
    {
        return fail(
            error,
            "--define 的变量名必须是 [A-Za-z_][A-Za-z0-9_]*，实际得到 '" +
            name + "'");
    }
    outName = name;
    outValue = (found == std::string::npos)
                   ? true
                   : parseDefineValue(argument.substr(found + 1u));
    return true;
}

} // namespace ParseEngine
