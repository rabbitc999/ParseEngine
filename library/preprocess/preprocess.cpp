/**
 * @file preprocess.cpp
 * @brief preprocess.hpp 的实现：行扫描、条件栈、表达式求值、include 展开、行号映射。
 */

#include "preprocess.hpp"

#include <cctype>
#include <fstream>
#include <filesystem>
#include <functional>
#include <set>
#include <sstream>
#include <utility>

namespace preprocess
{
namespace
{

// ---------------------------------------------------------------------------
//  小工具：字符串与路径
// ---------------------------------------------------------------------------

/**
 * @brief 字符是不是空白。
 * @param ch 字符。
 * @return 是空白返回 true。
 */
bool isSpace(char ch)
{
    return (ch == ' ') || (ch == '\t') || (ch == '\r') || (ch == '\n') ||
           (ch == '\f') || (ch == '\v');
}

/**
 * @brief 去掉首尾空白。
 * @param text 原文。
 * @return 去空白后的文本。
 */
std::string trim(const std::string &text)
{
    std::size_t begin = 0;
    std::size_t end = text.size();
    while ((begin < end) && isSpace(text[begin])) ++begin;
    while ((end > begin) && isSpace(text[end - 1u])) --end;
    return text.substr(begin, end - begin);
}

/**
 * @brief 去掉**指令参数末尾的注释**（行注释 `//`，或块注释那一对定界符）。
 *
 * 指令行常常写成 `%include "a.inc"` 后面再跟一句说明 —— 注释不属于参数，不剥掉的话
 * `%include` 会看见一个"不是引号开头"的路径、`%symbol` 会把注释当成值。
 * 引号里的注释定界符当普通字符（路径 / 消息里可能出现），不剥；
 * 只认"参数里第一处注释起点"，从这里一刀切到行尾（不支持注释夹在表达式中间）。
 *
 * @param argument 参数原文（已 trim）。
 * @return 去掉注释后再 trim 的结果。
 */
std::string stripTrailingComment(const std::string &argument)
{
    char quote = '\0';
    for (std::size_t index = 0; index < argument.size(); ++index)
    {
        const char ch = argument[index];
        if (quote != '\0')
        {
            if (ch == quote) quote = '\0';
            continue;
        }
        if ((ch == '"') || (ch == '\''))
        {
            quote = ch;
            continue;
        }
        if ((ch == '/') && (index + 1u < argument.size()) &&
            ((argument[index + 1u] == '/') || (argument[index + 1u] == '*')))
        {
            return trim(argument.substr(0u, index));
        }
    }
    return argument;
}

/**
 * @brief 转成小写（只处理 ASCII）。
 * @param text 原文。
 * @return 小写文本。
 */
std::string toLower(const std::string &text)
{
    std::string out = text;
    for (char &ch : out)
    {
        if ((ch >= 'A') && (ch <= 'Z')) ch = static_cast<char>(ch - 'A' + 'a');
    }
    return out;
}

/**
 * @brief `text` 在 `index` 处是否匹配 `token`。
 * @param text 文本。
 * @param index 下标。
 * @param token 待匹配的串（空串视为不匹配）。
 * @return 匹配返回 true。
 */
bool matchesAt(const std::string &text, std::size_t index, const std::string &token)
{
    if (token.empty()) return false;
    if (index + token.size() > text.size()) return false;
    return text.compare(index, token.size(), token) == 0;
}

/**
 * @brief 名字首字符是否合法（`[A-Za-z_]`）。
 * @param ch 字符。
 * @return 合法返回 true。
 */
bool isNameStart(char ch)
{
    return (ch == '_') || ((ch >= 'a') && (ch <= 'z')) || ((ch >= 'A') && (ch <= 'Z'));
}

/**
 * @brief 名字后续字符是否合法（`[A-Za-z0-9_]`）。
 * @param ch 字符。
 * @return 合法返回 true。
 */
bool isNamePart(char ch)
{
    return isNameStart(ch) || ((ch >= '0') && (ch <= '9'));
}

/**
 * @brief 路径规范化：`\` → `/`，消掉 `.` 与 `..`，保留盘符与绝对路径前缀。
 * @param path 原始路径。
 * @return 规范化后的路径。
 */
std::string normalizePath(const std::string &path)
{
    std::string raw = path;
    for (char &ch : raw)
    {
        if (ch == '\\') ch = '/';
    }

    // 1. 拆出前缀：`C:/` 或 `/`。
    std::string prefix;
    std::size_t index = 0;
    if ((raw.size() >= 2u) && (raw[1] == ':') &&
        (((raw[0] >= 'A') && (raw[0] <= 'Z')) || ((raw[0] >= 'a') && (raw[0] <= 'z'))))
    {
        prefix = raw.substr(0, 2);
        index = 2;
        if ((index < raw.size()) && (raw[index] == '/')) ++index;
        prefix += '/';
    }
    else if ((!raw.empty()) && (raw[0] == '/'))
    {
        prefix = "/";
        index = 1;
    }

    // 2. 逐段处理 `.` 与 `..`。
    std::vector<std::string> parts;
    while (index <= raw.size())
    {
        std::size_t slash = raw.find('/', index);
        if (slash == std::string::npos) slash = raw.size();
        const std::string part = raw.substr(index, slash - index);
        if (part.empty() || (part == "."))
        {
            // 跳过
        }
        else if (part == "..")
        {
            if (!parts.empty() && (parts.back() != "..")) parts.pop_back();
            else if (prefix.empty()) parts.push_back("..");
        }
        else
        {
            parts.push_back(part);
        }
        if (slash == raw.size()) break;
        index = slash + 1u;
    }

    // 3. 拼回去。
    std::string out = prefix;
    for (std::size_t i = 0; i < parts.size(); ++i)
    {
        if (i != 0u) out += '/';
        out += parts[i];
    }
    return out.empty() ? "." : out;
}

/**
 * @brief 取目录部分（不含结尾斜杠）。
 * @param path 路径。
 * @return 目录；没有目录部分时返回空串。
 */
std::string dirName(const std::string &path)
{
    const std::string normalized = normalizePath(path);
    const std::size_t slash = normalized.find_last_of('/');
    if (slash == std::string::npos) return std::string();
    if (slash == 0u) return "/";
    return normalized.substr(0, slash);
}

/**
 * @brief 拼一个相对路径到目录上。
 * @param dir 目录。
 * @param relative 相对路径（绝对路径原样返回）。
 * @return 拼接并规范化后的路径。
 */
std::string joinPath(const std::string &dir, const std::string &relative)
{
    if (relative.empty()) return normalizePath(dir);
    const bool absolute = (relative[0] == '/') || (relative[0] == '\\') ||
                          ((relative.size() >= 2u) && (relative[1] == ':'));
    if (absolute || dir.empty()) return normalizePath(relative);
    return normalizePath(dir + "/" + relative);
}

/**
 * @brief 读整个文件。
 * @param path 路径。
 * @param out 文件内容（成功时写入）。
 * @return 成功返回 true。
 */
bool readFile(const std::string &path, std::string &out)
{
    std::ifstream stream(std::filesystem::u8path(path.begin(), path.end()), std::ios::binary);
    if (!stream) return false;
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    out = buffer.str();
    return true;
}

/**
 * @brief 文件是否存在（不区分目录）。
 * @param path 路径。
 * @return 存在返回 true。
 */
bool fileExists(const std::string &path)
{
    std::ifstream stream(std::filesystem::u8path(path.begin(), path.end()), std::ios::binary);
    return static_cast<bool>(stream);
}

/**
 * @brief 去掉 UTF-8 BOM。
 * @param text 文本。
 * @return 去掉 BOM 的文本。
 */
std::string stripBom(const std::string &text)
{
    if ((text.size() >= 3u) && (static_cast<unsigned char>(text[0]) == 0xEFu) &&
        (static_cast<unsigned char>(text[1]) == 0xBBu) &&
        (static_cast<unsigned char>(text[2]) == 0xBFu))
    {
        return text.substr(3);
    }
    return text;
}

/**
 * @brief 把文本切成「行」（保留是否有结尾换行）。
 * @param text 文本。
 * @return 行列表；每行的 `\r` 已去掉。
 */
std::vector<std::string> splitLines(const std::string &text)
{
    std::vector<std::string> lines;
    if (text.empty()) return lines;

    std::size_t index = 0;
    while (index < text.size())
    {
        std::size_t end = text.find('\n', index);
        const bool last = (end == std::string::npos);
        if (last) end = text.size();
        std::string line = text.substr(index, end - index);
        if ((!line.empty()) && (line.back() == '\r')) line.pop_back();
        lines.push_back(line);
        if (last) break;
        index = end + 1u;
    }
    return lines;
}

/**
 * @brief 诊断级别 → 文本。
 * @param severity 级别。
 * @return `error` / `warning` / `note`。
 */
const char *severityText(Diagnostic::Severity severity)
{
    switch (severity)
    {
    case Diagnostic::Severity::Error: return "error";
    case Diagnostic::Severity::Warning: return "warning";
    default: return "note";
    }
}

// ---------------------------------------------------------------------------
//  符号表（可选大小写不敏感）
// ---------------------------------------------------------------------------

/**
 * @brief 符号表的比较器。
 */
struct SymbolLess
{
    bool caseInsensitive = false;

    /**
     * @brief 比较两个名字。
     * @param left 左。
     * @param right 右。
     * @return 左是否排在右前面。
     */
    bool operator()(const std::string &left, const std::string &right) const
    {
        if (!caseInsensitive) return left < right;
        return toLower(left) < toLower(right);
    }
};

using SymbolMap = std::map<std::string, std::string, SymbolLess>;

// ---------------------------------------------------------------------------
//  表达式求值
// ---------------------------------------------------------------------------

/**
 * @brief 条件运算符的递归下降解析器。
 *
 * 优先级：`||` < `&&` < `!` < 比较 < 原子。
 */
class ExprParser
{
public:
    /** 查符号用的回调：找到返回 true，并把值写进第二个参数（可为 nullptr）。 */
    using Lookup = std::function<bool(const std::string &, std::string *)>;

    /**
     * @brief 构造解析器。
     * @param text 表达式原文。
     * @param lookup 符号查询回调。
     */
    ExprParser(const std::string &text, Lookup lookup)
        : text_(text), lookup_(std::move(lookup))
    {
    }

    /**
     * @brief 解析并求值。
     * @param value 结果（成功时写入）。
     * @param error 失败时的说明。
     * @param column 失败位置（1 基列号，相对表达式原文）。
     * @return 成功返回 true。
     */
    bool run(bool &value, std::string &error, std::size_t &column)
    {
        skipSpace();
        if (atEnd())
        {
            fail("条件表达式是空的", 1u, error, column);
            return false;
        }
        value = parseOr(error, column);
        if (!error.empty()) return false;
        skipSpace();
        if (!atEnd())
        {
            fail("条件表达式后面还有多余内容", index_ + 1u, error, column);
            return false;
        }
        return true;
    }

private:
    /**
     * @brief 跳过空白。
     */
    void skipSpace()
    {
        while ((index_ < text_.size()) && isSpace(text_[index_])) ++index_;
    }

    /**
     * @brief 是否到末尾。
     * @return 到末尾返回 true。
     */
    bool atEnd() const { return index_ >= text_.size(); }

    /**
     * @brief 记一次失败。
     * @param message 说明。
     * @param column 列号。
     * @param error 输出说明。
     * @param outColumn 输出列号。
     */
    void fail(const std::string &message, std::size_t column,
              std::string &error, std::size_t &outColumn)
    {
        if (error.empty())
        {
            error = message;
            outColumn = column;
        }
    }

    /**
     * @brief 尝试吃掉一个运算符（后面不能再跟名字字符）。
     * @param token 运算符。
     * @return 吃掉返回 true。
     */
    bool eat(const std::string &token)
    {
        skipSpace();
        if (!matchesAt(text_, index_, token)) return false;
        index_ += token.size();
        return true;
    }

    /**
     * @brief 或运算。
     * @param error 错误说明。
     * @param column 错误列。
     * @return 结果。
     */
    bool parseOr(std::string &error, std::size_t &column)
    {
        bool value = parseAnd(error, column);
        if (!error.empty()) return false;
        while (true)
        {
            const std::size_t save = index_;
            if (!eat("||")) break;
            const bool right = parseAnd(error, column);
            if (!error.empty()) return false;
            value = value || right;
            (void)save;
        }
        return value;
    }

    /**
     * @brief 与运算。
     * @param error 错误说明。
     * @param column 错误列。
     * @return 结果。
     */
    bool parseAnd(std::string &error, std::size_t &column)
    {
        bool value = parseNot(error, column);
        if (!error.empty()) return false;
        while (true)
        {
            if (!eat("&&")) break;
            const bool right = parseNot(error, column);
            if (!error.empty()) return false;
            value = value && right;
        }
        return value;
    }

    /**
     * @brief 非运算。
     * @param error 错误说明。
     * @param column 错误列。
     * @return 结果。
     */
    bool parseNot(std::string &error, std::size_t &column)
    {
        if (eat("!")) return !parseNot(error, column);
        return parseCompare(error, column);
    }

    /**
     * @brief 比较：`NAME == 值` / `NAME != 值`。
     * @param error 错误说明。
     * @param column 错误列。
     * @return 结果。
     */
    bool parseCompare(std::string &error, std::size_t &column)
    {
        const bool left = parseAtom(error, column);
        if (!error.empty()) return false;

        const std::size_t save = index_;
        bool negate = false;
        if (eat("==")) negate = false;
        else if (eat("!=")) negate = true;
        else
        {
            index_ = save;
            return left;
        }

        // 右边只接受一个字面量（带引号或不带引号的单词）。
        skipSpace();
        std::string literal;
        if (!parseLiteral(literal, error, column)) return false;
        const bool equal = (lastValue_ == literal);
        (void)left;
        return negate ? !equal : equal;
    }

    /**
     * @brief 原子：`defined(NAME)`、括号、符号名。
     * @param error 错误说明。
     * @param column 错误列。
     * @return 结果。
     */
    bool parseAtom(std::string &error, std::size_t &column)
    {
        skipSpace();
        if (atEnd())
        {
            fail("条件表达式不完整", index_ + 1u, error, column);
            return false;
        }

        // 1. defined(NAME)
        if ((text_[index_] == 'd') && matchesAt(text_, index_, "defined"))
        {
            const std::size_t start = index_;
            index_ += 7u;
            skipSpace();
            if (atEnd() || (text_[index_] != '('))
            {
                fail("defined 后面要跟 (名字)", start + 1u, error, column);
                return false;
            }
            ++index_;
            skipSpace();
            std::string name;
            if (!parseName(name, error, column)) return false;
            skipSpace();
            if (atEnd() || (text_[index_] != ')'))
            {
                fail("defined(名字) 缺少右括号", index_ + 1u, error, column);
                return false;
            }
            ++index_;
            return lookup_(name, nullptr);
        }

        // 2. 括号
        if (text_[index_] == '(')
        {
            ++index_;
            const bool value = parseOr(error, column);
            if (!error.empty()) return false;
            skipSpace();
            if (atEnd() || (text_[index_] != ')'))
            {
                fail("条件表达式缺少右括号", index_ + 1u, error, column);
                return false;
            }
            ++index_;
            return value;
        }

        // 3. 符号名：未定义 = 假；定义了没值 = 真；有值按值判真假。
        std::string name;
        if (!parseName(name, error, column)) return false;
        std::string value;
        if (!lookup_(name, &value))
        {
            lastValue_.clear();
            return false;
        }
        lastValue_ = value;
        return value.empty() ? true : Preprocessor::parseTruthValue(value);
    }

    /**
     * @brief 解析一个字面量（带引号的原样取内容，不带引号取到空白/运算符为止）。
     * @param literal 结果。
     * @param error 错误说明。
     * @param column 错误列。
     * @return 成功返回 true。
     */
    bool parseLiteral(std::string &literal, std::string &error, std::size_t &column)
    {
        skipSpace();
        if (atEnd())
        {
            fail("比较运算缺少右操作数", index_ + 1u, error, column);
            return false;
        }

        const char first = text_[index_];
        if ((first == '"') || (first == '\''))
        {
            ++index_;
            std::string value;
            bool closed = false;
            while (!atEnd())
            {
                const char ch = text_[index_++];
                if (ch == '\\' && !atEnd())
                {
                    value.push_back(text_[index_++]);
                    continue;
                }
                if (ch == first)
                {
                    closed = true;
                    break;
                }
                value.push_back(ch);
            }
            if (!closed)
            {
                fail("字面量缺少收尾引号", index_ + 1u, error, column);
                return false;
            }
            literal = value;
            return true;
        }

        std::string value;
        while (!atEnd())
        {
            const char ch = text_[index_];
            if (isSpace(ch) || (ch == ')') || (ch == '(')) break;
            if ((ch == '&') || (ch == '|') || (ch == '=') || (ch == '!')) break;
            value.push_back(ch);
            ++index_;
        }
        if (value.empty())
        {
            fail("比较运算缺少右操作数", index_ + 1u, error, column);
            return false;
        }
        literal = value;
        return true;
    }

    /**
     * @brief 解析一个符号名。
     * @param name 结果。
     * @param error 错误说明。
     * @param column 错误列。
     * @return 成功返回 true。
     */
    bool parseName(std::string &name, std::string &error, std::size_t &column)
    {
        skipSpace();
        if (atEnd() || !isNameStart(text_[index_]))
        {
            fail("这里要写一个符号名", index_ + 1u, error, column);
            return false;
        }
        const std::size_t start = index_;
        while ((!atEnd()) && isNamePart(text_[index_])) ++index_;
        name = text_.substr(start, index_ - start);
        return true;
    }

    const std::string text_;   ///< 表达式原文。
    Lookup lookup_;            ///< 符号查询。
    std::size_t index_ = 0;    ///< 当前下标。
    std::string lastValue_;    ///< 最近一次查到的符号值（比较运算用）。
};

} // namespace

} // namespace preprocess

namespace preprocess
{
namespace
{

// ---------------------------------------------------------------------------
//  扫描状态、条件帧、指令分类
// ---------------------------------------------------------------------------

/**
 * @brief 跨行保留的扫描状态。
 */
struct ScanState
{
    bool inBlockComment = false;      ///< 是否正处在块注释里。
    std::string blockCommentEnd;      ///< 当前块注释的结束串。
    std::string opaqueEnd;            ///< 非空表示正处在某个不透明区域，值是它的结束标记。
};

/**
 * @brief 一行的扫描结果。
 */
struct ScanInfo
{
    bool hasContent = false;      ///< 行内是否有"有意义"的字符。
    std::size_t contentIndex = 0; ///< 有意义字符的下标。
    bool opaqueStart = false;     ///< 行首是不透明区域的开始（此时不当指令）。
};

/**
 * @brief 条件编译的一帧。
 */
struct CondFrame
{
    std::string file;               ///< `%if` 所在文件（报错用）。
    std::size_t line = 0;           ///< `%if` 所在行（报错用）。
    bool parentActive = true;       ///< 外层是否活跃。
    bool active = true;             ///< 本分支当前是否输出。
    bool taken = true;              ///< 是否已有分支被选中（`%elif` / `%else` 用）。
    bool sawElse = false;           ///< 是否已经出现过 `%else`。
};

/**
 * @brief 指令种类。
 */
enum class Directive
{
    Symbol,      ///< `%symbol`（`%define` 别名）
    Unsymbol,    ///< `%unsymbol`（`%undefine` 别名）
    If,          ///< `%if`
    IfDef,       ///< `%ifdef NAME`（等价 `%if defined(NAME)`）
    IfNotDef,    ///< `%ifndef NAME`（等价 `%if !defined(NAME)`）
    Elif,        ///< `%elif`
    Else,        ///< `%else`
    Endif,       ///< `%endif`
    Include,     ///< `%include`
    IncludeOnce, ///< `%include_once` / `%import`
    Once,        ///< `%once`
    Pragma,      ///< `%pragma once`
    Error,       ///< `%error`
    Warning,     ///< `%warning`
    Unknown,     ///< 其它
};

/**
 * @brief 一次预处理过程的实现体。
 *
 * 生命周期就是一次 `runFile` / `runText`：符号表、包含集合、输出都在这里。
 */
class Engine
{
public:
    /**
     * @brief 构造。
     * @param options 选项。
     * @param out 结果。
     */
    Engine(const Options &options, Result &out)
        : options_(options), out_(out),
          symbols_(SymbolLess{options.caseInsensitive})
    {
    }

    /**
     * @brief 处理一个文件。
     * @param path 主文件路径。
     * @return 没有错误返回 true。
     */
    bool runFile(const std::string &path)
    {
        seedDefines();

        // 1. 主文件必须能读出来。
        const std::string absolute = normalizePath(path);
        std::string text;
        if (!readFile(absolute, text))
        {
            report(Diagnostic::Severity::Error, "open-failed", path, 1u, 1u,
                   "打不开文件：" + absolute);
            return false;
        }

        // 2. 处理；主文件也记进依赖表。
        addDependency(absolute);
        includeStack_.push_back(absolute);
        processSource(stripBom(text), absolute, dirName(absolute), 0u);
        includeStack_.pop_back();
        return !hasErrors();
    }

    /**
     * @brief 处理一段文本。
     * @param text 文本。
     * @param label 诊断里显示的文件名。
     * @param baseDir `%include` 的基准目录。
     * @return 没有错误返回 true。
     */
    bool runText(const std::string &text, const std::string &label,
                 const std::string &baseDir)
    {
        seedDefines();
        processSource(stripBom(text), label, baseDir, 0u);
        return !hasErrors();
    }

private:
    // ---- 主循环 -----------------------------------------------------------

    /**
     * @brief 把 `options_.defines` 灌进符号表。
     */
    void seedDefines()
    {
        for (const Define &define : options_.defines)
        {
            symbols_[define.name] = define.value;
        }
    }

    /**
     * @brief 处理一份文本（主文件或 `%include` 进来的文件）。
     * @param text 内容。
     * @param file 文件名（绝对路径或标签）。
     * @param baseDir `%include` 相对路径的基准目录。
     * @param depth 当前嵌套深度。
     */
    void processSource(const std::string &text, const std::string &file,
                       const std::string &baseDir, std::size_t depth)
    {
        const std::vector<std::string> lines = splitLines(text);
        out_.linesIn += lines.size();

        ScanState state;
        std::vector<CondFrame> frames;

        for (std::size_t index = 0; index < lines.size(); ++index)
        {
            const std::string &line = lines[index];
            const std::size_t lineNumber = index + 1u;
            const ScanInfo info = scanLine(line, state);

            // 1. 空行、纯注释行、不透明区域内部：按内容行处理。
            const bool isDirective =
                info.hasContent && !info.opaqueStart &&
                (options_.prefixes().find(line[info.contentIndex]) != std::string::npos) &&
                (info.contentIndex + 1u < line.size()) &&
                isNameStart(line[info.contentIndex + 1u]);

            if (!isDirective)
            {
                if (active(frames))
                {
                    emitLine(line, file, lineNumber);
                }
                else
                {
                    // 条件为假：默认输出一个空行占位，保证行号与输入一致。
                    emitDropped(file, lineNumber);
                }
                continue;
            }

            // 2. 指令行本身不输出（除非 keepDirectiveLines），同样补空行占位。
            const bool emitted = handleDirective(line, info, file, lineNumber,
                                                baseDir, depth, frames);
            if (!emitted)
            {
                if (options_.keepDirectiveLines) emitLine(line, file, lineNumber);
                else emitDropped(file, lineNumber);
            }
        }

        // 3. 文件结束时条件栈必须是空的（与 metro_editor 一样报出来）。
        for (const CondFrame &frame : frames)
        {
            report(Diagnostic::Severity::Error, "unterminated-if", frame.file, frame.line, 1u,
                   "这个 %if 没有对应的 %endif");
        }
    }

    /**
     * @brief 扫描一行：跳过注释、字符串、不透明区域，找第一个有意义的字符。
     * @param line 行内容（不含换行）。
     * @param state 跨行状态（会被更新）。
     * @return 扫描结果。
     */
    ScanInfo scanLine(const std::string &line, ScanState &state) const
    {
        ScanInfo info;
        std::size_t index = 0;

        // 1. 还处在不透明区域里：先找结束标记。
        if (!state.opaqueEnd.empty())
        {
            const std::size_t end = line.find(state.opaqueEnd);
            if (end == std::string::npos) return info;
            index = end + state.opaqueEnd.size();
            state.opaqueEnd.clear();
        }

        while (index < line.size())
        {
            // 2. 块注释里：找结束标记。
            if (state.inBlockComment)
            {
                const std::size_t end = line.find(state.blockCommentEnd, index);
                if (end == std::string::npos) return info;
                index = end + state.blockCommentEnd.size();
                state.inBlockComment = false;
                continue;
            }

            const char ch = line[index];
            if (isSpace(ch))
            {
                ++index;
                continue;
            }

            // 3. 行注释：命中任意一个起始串，本行后面就没有内容了。
            bool isLineComment = false;
            for (const std::string &marker : options_.lineComments)
            {
                if (matchesAt(line, index, marker))
                {
                    isLineComment = true;
                    break;
                }
            }
            if (isLineComment) return info;

            // 4. 块注释开始：记住是哪一对，跨行时用它的结束串。
            bool isBlockComment = false;
            for (const std::pair<std::string, std::string> &pair : options_.blockComments)
            {
                if (!matchesAt(line, index, pair.first)) continue;
                state.inBlockComment = true;
                state.blockCommentEnd = pair.second;
                index += pair.first.size();
                isBlockComment = true;
                break;
            }
            if (isBlockComment) continue;

            // 5. 不透明区域开始（行锚定的标记必须顶格）。
            bool enteredOpaque = false;
            for (const OpaqueRegion &region : options_.opaqueRegions)
            {
                if (!matchesAt(line, index, region.begin)) continue;
                if (region.lineAnchored && (index != 0u)) continue;
                // `%code` 不能匹配 `%codex`：标记以名字字符结尾时，后面必须是分隔符。
                const std::size_t after = index + region.begin.size();
                if (!region.begin.empty() && isNamePart(region.begin.back()) &&
                    (after < line.size()) && isNamePart(line[after]))
                {
                    continue;
                }
                state.opaqueEnd = region.end;
                index = after;
                info.hasContent = true;
                info.contentIndex = 0u;
                info.opaqueStart = true;
                enteredOpaque = true;
                break;
            }
            if (enteredOpaque)
            {
                // 区域可能在同行结束，结束后继续往后扫。
                const std::size_t end = line.find(state.opaqueEnd, index);
                if (end == std::string::npos) return info;
                index = end + state.opaqueEnd.size();
                state.opaqueEnd.clear();
                continue;
            }

            // 6. 字符串：整段跳过（里面的指令不当指令）。
            if (options_.stringDelimiters.find(ch) != std::string::npos)
            {
                const char quote = ch;
                ++index;
                while (index < line.size())
                {
                    const char current = line[index++];
                    if (current == '\\' && index < line.size())
                    {
                        ++index;
                        continue;
                    }
                    if (current == quote) break;
                }
                continue;
            }

            // 7. 找到有意义的内容。
            info.hasContent = true;
            info.contentIndex = index;
            return info;
        }

        return info;
    }

    // ---- 指令 -------------------------------------------------------------

    /**
     * @brief 处理一条指令行。
     * @param line 行内容。
     * @param info 扫描结果（给出指令前缀的位置）。
     * @param file 当前文件。
     * @param lineNumber 当前行号（1 基）。
     * @param baseDir `%include` 的基准目录。
     * @param depth 当前嵌套深度。
     * @param frames 当前文件的条件帧栈。
     */
    bool handleDirective(const std::string &line, const ScanInfo &info,
                        const std::string &file, std::size_t lineNumber,
                        const std::string &baseDir, std::size_t depth,
                        std::vector<CondFrame> &frames)
    {
        const std::size_t prefix = info.contentIndex;
        const char prefixChar = line[prefix];
        std::size_t cursor = prefix + 1u;
        const std::size_t nameStart = cursor;
        while ((cursor < line.size()) && isNamePart(line[cursor])) ++cursor;

        const std::string name = toLower(line.substr(nameStart, cursor - nameStart));
        /* 参数末尾挂的注释不属于参数：`%include "a.inc"  // 说明` 也要能用。 */
        const std::string argument = stripTrailingComment(trim(line.substr(cursor)));
        const std::size_t argumentColumn = cursor + 1u;
        const bool activeNow = active(frames);

        switch (classify(name, prefixChar))
        {
        case Directive::Symbol:
        case Directive::Unsymbol:
        {
            // 1. `%symbol NAME [VALUE]` / `%unsymbol NAME`。
            Define define;
            Diagnostic diagnostic;
            if (!Preprocessor::parseDefine(argument, define, &diagnostic))
            {
                report(Diagnostic::Severity::Error, "bad-argument", file, lineNumber,
                       argumentColumn, diagnostic.message);
                return false;
            }
            if (activeNow)
            {
                if (classify(name, prefixChar) == Directive::Symbol) symbols_[define.name] = define.value;
                else symbols_.erase(define.name);
            }
            return false;
        }

        case Directive::If:
        case Directive::IfDef:
        case Directive::IfNotDef:
        {
            // 2. `%if EXPR` / `%ifdef NAME` / `%ifndef NAME`：
            //    外层不活跃时不再求值，只记一帧保持嵌套配平。
            bool value = false;
            if (activeNow)
            {
                const Directive kind = classify(name, prefixChar);
                const std::string expression =
                    (kind == Directive::If)
                        ? argument
                        : ((kind == Directive::IfDef) ? ("defined(" + argument + ")")
                                                      : ("!defined(" + argument + ")"));
                evaluateExpression(expression, file, lineNumber, argumentColumn, value);
            }
            frames.push_back(CondFrame{file, lineNumber, activeNow, activeNow && value,
                                       activeNow && value, false});
            return false;
        }

        case Directive::Elif:
        {
            // 3. `%elif EXPR`：只在"外层活跃 + 还没分支命中"时求值。
            if (frames.empty())
            {
                report(Diagnostic::Severity::Error, "unmatched-elif", file, lineNumber,
                       prefix + 1u, "多余的 %elif，缺少匹配的 %if");
                return false;
            }
            CondFrame &frame = frames.back();
            if (frame.sawElse)
            {
                report(Diagnostic::Severity::Error, "elif-after-else", file, lineNumber,
                       prefix + 1u, "%elif 不能写在 %else 之后");
                return false;
            }
            bool value = false;
            if (frame.parentActive && !frame.taken)
            {
                evaluateExpression(argument, file, lineNumber, argumentColumn, value);
            }
            frame.active = frame.parentActive && !frame.taken && value;
            if (frame.active) frame.taken = true;
            return false;
        }

        case Directive::Else:
        {
            // 4. `%else`：翻转当前帧。
            if (frames.empty())
            {
                report(Diagnostic::Severity::Error, "unmatched-else", file, lineNumber,
                       prefix + 1u, "多余的 %else，缺少匹配的 %if");
                return false;
            }
            CondFrame &frame = frames.back();
            if (frame.sawElse)
            {
                report(Diagnostic::Severity::Error, "duplicate-else", file, lineNumber,
                       prefix + 1u, "同一个 %if 里出现了两次 %else");
                return false;
            }
            frame.sawElse = true;
            frame.active = frame.parentActive && !frame.taken;
            frame.taken = true;
            return false;
        }

        case Directive::Endif:
        {
            // 5. `%endif`：收掉一帧。
            if (frames.empty())
            {
                report(Diagnostic::Severity::Error, "unmatched-endif", file, lineNumber,
                       prefix + 1u, "多余的 %endif，缺少匹配的 %if");
                return false;
            }
            frames.pop_back();
            return false;
        }

        case Directive::Include:
        case Directive::IncludeOnce:
        {
            // 6. 包含：不活跃的分支不展开（语法仍然检查）。
            //    展开成功时，被包含内容顶替这一行（不再补空行）。
            //    `%import` 与 `%include_once` 都按"只展开一次"处理，区别只有
            //    `imported` 标记——自定义 loader 可以据此走不同分支（比如 import 当数据源）。
            const bool imported = (name == "import");
            const bool onceRequested = (classify(name, prefixChar) == Directive::IncludeOnce);
            return handleInclude(argument, imported, onceRequested, file, lineNumber,
                                 argumentColumn, baseDir, depth, activeNow);
        }

        case Directive::Once:
            // 7. `%once`：把当前文件登记成"只展开一次"。
            onceFiles_.insert(file);
            return false;

        case Directive::Pragma:
            // 8. `%pragma once`：与 `%once` 等价；别的 pragma 只给一句警告。
            if (toLower(trim(argument)) == "once") onceFiles_.insert(file);
            else if (activeNow)
            {
                report(Diagnostic::Severity::Warning, "unknown-pragma", file, lineNumber,
                       prefix + 1u, "只认识 %pragma once，其余的忽略");
            }
            return false;

        case Directive::Error:
            // 9. `%error "…"`：活跃分支里直接报错（与 #error 一致）。
            if (activeNow)
            {
                report(Diagnostic::Severity::Error, "error-directive", file, lineNumber,
                       prefix + 1u, unquote(argument));
            }
            return false;

        case Directive::Warning:
            if (activeNow)
            {
                report(Diagnostic::Severity::Warning, "warning-directive", file, lineNumber,
                       prefix + 1u, unquote(argument));
            }
            return false;

        case Directive::Unknown:
        default:
            // 10. 不认识的指令：按选项报错 / 原样保留 / 静默丢掉。
            if (options_.unknownDirective == Options::UnknownDirective::Keep)
            {
                // 原样保留：活跃分支里把原文写出去，非活跃分支仍然补空行占位。
                if (activeNow) emitLine(line, file, lineNumber);
                else emitDropped(file, lineNumber);
                return true;
            }
            else if (options_.unknownDirective == Options::UnknownDirective::Error)
            {
                report(Diagnostic::Severity::Error, "unknown-directive", file, lineNumber,
                       prefix + 1u, "不认识的预处理指令 " + line.substr(prefix));
            }
            return false;
        }
    }

    /**
     * @brief 指令名 → 种类。
     * @param name 小写指令名（不含前缀）。
     * @param prefix 该行用到的前缀字符（决定 `define` / `undef` 是否算别名）。
     * @return 种类。
     */
    Directive classify(const std::string &name, char prefix) const
    {
        if (name == "symbol") return Directive::Symbol;
        if (name == "unsymbol") return Directive::Unsymbol;
        // `#define` / `#undef` 是 C 风格写法，始终支持；
        // `%define` / `%undefine` 是 metro_editor 风格，要显式打开。
        if ((prefix == '#') || options_.percentDefineIsSymbol)
        {
            if (name == "define") return Directive::Symbol;
            if (name == "undef") return Directive::Unsymbol;
            if (name == "undefine") return Directive::Unsymbol;
        }
        if (name == "if") return Directive::If;
        if (name == "ifdef") return Directive::IfDef;
        if (name == "ifndef") return Directive::IfNotDef;
        if (name == "elif") return Directive::Elif;
        if (name == "else") return Directive::Else;
        if (name == "endif") return Directive::Endif;
        if (name == "include") return Directive::Include;
        if (name == "include_once") return Directive::IncludeOnce;
        if (name == "import") return Directive::IncludeOnce;
        if (name == "once") return Directive::Once;
        if (name == "pragma") return Directive::Pragma;
        if (name == "error") return Directive::Error;
        if (name == "warning") return Directive::Warning;
        return Directive::Unknown;
    }

    /**
     * @brief 展开一个 `%include`。
     * @param argument 指令后面的原文。
     * @param onceRequested 是否是 `%include_once`。
     * @param file 包含方文件。
     * @param imported 是否来自 `%import`（自定义 loader 据此区分行为）。
     * @param onceRequested 是否来自 `%import` / `%include_once`（按"只展开一次"处理）。
     * @param lineNumber 行号。
     * @param column 列号。
     * @param baseDir 相对路径基准目录。
     * @param depth 当前深度。
     * @param activeNow 当前分支是否活跃。
     * @return 真的展开了返回 true（调用方不再补空行占位）。
     */
    bool handleInclude(const std::string &argument, bool imported, bool onceRequested,
                       const std::string &file, std::size_t lineNumber,
                       std::size_t column, const std::string &baseDir,
                       std::size_t depth, bool activeNow)
    {
        // 1. 语法：路径必须带引号。
        std::string path;
        if (!parseQuotedPath(argument, path))
        {
            report(Diagnostic::Severity::Error, "bad-path", file, lineNumber, column,
                   "include 后面要跟一个带引号的路径，例如 %include \"common.inc\"");
            return false;
        }
        if (!activeNow) return false;

        out_.includeCount++;

        // 2. 先按惯例找一遍路径：即使有自定义 loader，也能把结果给它参考。
        const std::string resolvedPath = resolveInclude(path, file, baseDir);
        const bool onceByDefault = onceRequested || options_.includeOnceByDefault;

        // 3. 问自定义 loader：它可以自己给正文（CSV / 数据库 / 内存文本）、
        //    可以跳过，可以报错，也可以交回默认逻辑。
        IncludeResult custom;
        if (options_.includeLoader)
        {
            IncludeRequest request;
            request.path = path;
            request.resolvedPath = resolvedPath;
            request.fromFile = file;
            request.baseDir = baseDir;
            request.imported = imported;
            request.once = onceByDefault || (onceFiles_.count(resolvedPath) != 0u);
            custom = options_.includeLoader(request);
        }

        if (custom.action == IncludeResult::Action::Error)
        {
            report(Diagnostic::Severity::Error, "include-loader-error", file, lineNumber, column,
                   custom.message.empty() ? ("自定义 loader 拒绝加载 " + path) : custom.message);
            return false;
        }
        if (custom.action == IncludeResult::Action::Skip) return false;

        // 4. 定下这次展开的正文与它的标识（标识进依赖表 / 去重 / 循环检测）。
        std::string label;
        std::string text;
        bool fromMemory = false;
        if (custom.action == IncludeResult::Action::Inline)
        {
            text = custom.text;
            label = custom.label.empty() ? (resolvedPath.empty() ? path : resolvedPath)
                                         : custom.label;
            fromMemory = true;
        }
        else if (custom.action == IncludeResult::Action::UseFile)
        {
            label = custom.label.empty() ? resolvedPath : custom.label;
        }
        else
        {
            label = resolvedPath;
        }

        if (label.empty())
        {
            std::string searchDirs = baseDir.empty() ? dirName(file) : baseDir;
            for (const std::string &dir : options_.includeDirs)
            {
                searchDirs += ", " + dir;
            }
            report(Diagnostic::Severity::Error, "include-not-found", file, lineNumber, column,
                   "找不到被包含的文件 \"" + path + "\"（搜索目录：" + searchDirs + "）");
            return false;
        }

        // 5. 深度限制与循环检测。
        if (depth >= options_.maxIncludeDepth)
        {
            report(Diagnostic::Severity::Error, "include-depth", file, lineNumber, column,
                   "包含层级超过 " + std::to_string(options_.maxIncludeDepth) + " 层");
            return false;
        }
        if (std::find(includeStack_.begin(), includeStack_.end(), label) != includeStack_.end())
        {
            std::string chain;
            for (const std::string &item : includeStack_)
            {
                chain += item + " → ";
            }
            chain += label;
            report(Diagnostic::Severity::Error, "include-cycle", file, lineNumber, column,
                   "循环包含：" + chain);
            return false;
        }

        // 6. 去重：同一个文件默认只展开一次。无论这次是否去重，都要记下
        //    "已经展开过"——否则文件在内容里写 `#pragma once` / `%once` 时，
        //    第二次包含会漏掉去重。
        const bool once = onceByDefault || (onceFiles_.count(label) != 0u);
        if (once && (includedOnce_.count(label) != 0u)) return false;
        includedOnce_.insert(label);

        // 7. 取正文：loader 给了就用手上的，否则读文件。
        if (!fromMemory)
        {
            if (!readFile(label, text))
            {
                report(Diagnostic::Severity::Error, "include-not-found", file, lineNumber, column,
                       "读不了被包含的文件：" + label);
                return false;
            }
        }

        // 8. 递归展开：正文里的相对包含以被包含文件所在目录为基准。
        addDependency(label);
        includeStack_.push_back(label);
        lastMarkerFile_.clear(); // 强制给被包含内容插一条 #line
        const std::string childBase =
            (label.find('/') == std::string::npos) ? baseDir : dirName(label);
        processSource(stripBom(text), label, childBase, depth + 1u);
        includeStack_.pop_back();
        lastMarkerFile_.clear(); // 回到上一层后，下一行也要重新插 #line
        return true;
    }

    // ---- 表达式、路径、输出、诊断 ------------------------------------------

    /**
     * @brief 求值一个条件表达式，失败时记诊断。
     * @param expression 表达式原文。
     * @param file 文件。
     * @param line 行号。
     * @param column 表达式起始列（1 基）。
     * @param out 结果。
     * @return 成功返回 true。
     */
    bool evaluateExpression(const std::string &expression, const std::string &file,
                            std::size_t line, std::size_t column, bool &out)
    {
        ExprParser parser(expression, [this](const std::string &name, std::string *value) {
            const SymbolMap::const_iterator found = symbols_.find(name);
            if (found == symbols_.end()) return false;
            if (value != nullptr) *value = found->second;
            return true;
        });

        bool value = false;
        std::string error;
        std::size_t errorColumn = 0;
        if (!parser.run(value, error, errorColumn))
        {
            report(Diagnostic::Severity::Error, "bad-expression", file, line,
                   column + (errorColumn > 0u ? errorColumn - 1u : 0u), error);
            out = false;
            return false;
        }
        out = value;
        return true;
    }

    /**
     * @brief 解析 `"路径"` / `'路径'`。
     * @param argument 原文。
     * @param out 路径。
     * @return 成功返回 true。
     */
    static bool parseQuotedPath(const std::string &argument, std::string &out)
    {
        if (argument.size() < 2u) return false;
        const char quote = argument[0];
        if ((quote != '"') && (quote != '\'')) return false;
        if (argument.back() != quote) return false;
        out = argument.substr(1u, argument.size() - 2u);
        return !out.empty();
    }

    /**
     * @brief 去掉 `%error "…"` 里的引号；没引号就原样返回。
     * @param argument 原文。
     * @return 消息文本。
     */
    static std::string unquote(const std::string &argument)
    {
        std::string path;
        if (parseQuotedPath(argument, path)) return path;
        return argument;
    }

    /**
     * @brief 解析包含路径。
     * @param path 原文路径。
     * @param fromFile 包含方文件。
     * @return 存在的绝对路径；都找不到返回空串。
     */
    std::string resolveInclude(const std::string &path, const std::string &fromFile,
                               const std::string &baseDir) const
    {
        std::vector<std::string> candidates;
        if (!baseDir.empty()) candidates.push_back(joinPath(baseDir, path));
        const std::string base = dirName(fromFile);
        if (!base.empty()) candidates.push_back(joinPath(base, path));
        for (const std::string &dir : options_.includeDirs)
        {
            candidates.push_back(joinPath(dir, path));
        }
        candidates.push_back(normalizePath(path));

        for (const std::string &candidate : candidates)
        {
            if (fileExists(candidate)) return candidate;
        }
        return std::string();
    }

    /**
     * @brief 当前是否处于活跃分支。
     * @param frames 条件栈。
     * @return 活跃返回 true。
     */
    static bool active(const std::vector<CondFrame> &frames)
    {
        return frames.empty() || frames.back().active;
    }

    /**
     * @brief 输出一行（必要时先插 `#line` 标记）。
     * @param line 行内容。
     * @param file 来源文件。
     * @param lineNumber 来源行号。
     */
    void emitLine(const std::string &line, const std::string &file, std::size_t lineNumber)
    {
        if (options_.emitLineMarkers && (lastMarkerFile_ != file))
        {
            out_.text += options_.lineMarkerPrefix + " " + std::to_string(lineNumber) +
                         " \"" + file + "\"\n";
            out_.lineMap.push_back(LineOrigin{file, lineNumber});
            out_.linesOut++;
            lastMarkerFile_ = file;
        }
        out_.text += line;
        out_.text += '\n';
        out_.lineMap.push_back(LineOrigin{file, lineNumber});
        out_.linesOut++;
    }

    /**
     * @brief 丢掉一行（按选项决定是否补一个空行）。
     */
    void emitDropped(const std::string &file, std::size_t lineNumber)
    {
        if (!options_.preserveLines) return;
        out_.text += '\n';
        out_.lineMap.push_back(LineOrigin{file, lineNumber});
        out_.linesOut++;
    }

    /**
     * @brief 记一条诊断。
     */
    void report(Diagnostic::Severity severity, const std::string &code,
                const std::string &file, std::size_t line, std::size_t column,
                const std::string &message)
    {
        Diagnostic diagnostic;
        diagnostic.severity = severity;
        diagnostic.code = code;
        diagnostic.file = file;
        diagnostic.line = line;
        diagnostic.column = column;
        diagnostic.message = message;
        out_.diagnostics.push_back(std::move(diagnostic));
    }

    /**
     * @brief 是否已经有错误。
     * @return 有错误返回 true。
     */
    bool hasErrors() const
    {
        return out_.count(Diagnostic::Severity::Error) > 0u;
    }

    /**
     * @brief 记一个依赖文件（去重、保持首次出现顺序）。
     * @param path 绝对路径。
     */
    void addDependency(const std::string &path)
    {
        if (std::find(out_.dependencies.begin(), out_.dependencies.end(), path) !=
            out_.dependencies.end())
        {
            return;
        }
        out_.dependencies.push_back(path);
    }

    Options options_;                            ///< 选项副本。
    Result &out_;                                ///< 结果。
    SymbolMap symbols_;                          ///< 符号表。
    std::vector<std::string> includeStack_;      ///< 包含栈（循环检测 + 报错链）。
    std::set<std::string> includedOnce_;         ///< 已经展开过的"只展开一次"文件。
    std::set<std::string> onceFiles_;            ///< 写了 %once 的文件。
    std::string lastMarkerFile_;                 ///< 上一次插 #line 时的文件。
};

} // namespace

// ---------------------------------------------------------------------------
//  公开接口
// ---------------------------------------------------------------------------

IncludeResult IncludeResult::useDefault()
{
    return IncludeResult{};
}

IncludeResult IncludeResult::useFile(const std::string &label)
{
    IncludeResult result;
    result.action = Action::UseFile;
    result.label = label;
    return result;
}

IncludeResult IncludeResult::inlineText(const std::string &text, const std::string &label)
{
    IncludeResult result;
    result.action = Action::Inline;
    result.text = text;
    result.label = label;
    return result;
}

IncludeResult IncludeResult::skip()
{
    IncludeResult result;
    result.action = Action::Skip;
    return result;
}

IncludeResult IncludeResult::error(const std::string &message)
{
    IncludeResult result;
    result.action = Action::Error;
    result.message = message;
    return result;
}

std::string Diagnostic::toString() const
{
    std::string text = file.empty() ? std::string("<memory>") : file;
    text += ":";
    text += std::to_string(line);
    if (column > 0u)
    {
        text += ":";
        text += std::to_string(column);
    }
    text += ": ";
    text += severityText(severity);
    text += ": ";
    text += message;
    if (!code.empty())
    {
        text += " [";
        text += code;
        text += "]";
    }
    return text;
}

bool Result::ok() const
{
    return count(Diagnostic::Severity::Error) == 0u;
}

std::size_t Result::count(Diagnostic::Severity severity) const
{
    std::size_t total = 0u;
    for (const Diagnostic &diagnostic : diagnostics)
    {
        if (diagnostic.severity == severity) ++total;
    }
    return total;
}

Preprocessor::Preprocessor(Options options) : options_(std::move(options))
{
}

bool Preprocessor::runFile(const std::string &path, Result &out)
{
    Engine engine(options_, out);
    return engine.runFile(path);
}

bool Preprocessor::runText(const std::string &text, const std::string &label,
                           const std::string &baseDir, Result &out)
{
    Engine engine(options_, out);
    return engine.runText(text, label, baseDir);
}

bool Preprocessor::evaluate(const std::string &expression,
                            const std::map<std::string, std::string> &symbols,
                            bool &value, Diagnostic *diagnostic) const
{
    ExprParser parser(expression, [&symbols](const std::string &name, std::string *out) {
        const std::map<std::string, std::string>::const_iterator found = symbols.find(name);
        if (found == symbols.end()) return false;
        if (out != nullptr) *out = found->second;
        return true;
    });

    bool result = false;
    std::string error;
    std::size_t column = 0u;
    if (!parser.run(result, error, column))
    {
        if (diagnostic != nullptr)
        {
            diagnostic->severity = Diagnostic::Severity::Error;
            diagnostic->code = "bad-expression";
            diagnostic->column = column;
            diagnostic->message = error;
        }
        return false;
    }
    value = result;
    return true;
}

bool Preprocessor::isValidName(const std::string &name)
{
    if (name.empty() || !isNameStart(name[0])) return false;
    for (std::size_t index = 1; index < name.size(); ++index)
    {
        if (!isNamePart(name[index])) return false;
    }
    return true;
}

bool Preprocessor::parseDefine(const std::string &argument, Define &out,
                               Diagnostic *diagnostic)
{
    const std::string text = trim(argument);
    const std::size_t equal = text.find('=');
    const std::size_t space = text.find_first_of(" \t");

    // 1. 两种写法都认：
    //      NAME=VALUE      （命令行 -D / #define 的等号写法）
    //      NAME VALUE…     （%symbol NAME VALUE 的空格写法）
    std::string name;
    std::string value;
    if ((equal != std::string::npos) && ((space == std::string::npos) || (equal < space)))
    {
        name = trim(text.substr(0u, equal));
        value = trim(text.substr(equal + 1u));
    }
    else
    {
        const std::size_t split = (space == std::string::npos) ? text.size() : space;
        name = trim(text.substr(0u, split));
        value = trim(text.substr(split));
    }

    if (!isValidName(name))
    {
        if (diagnostic != nullptr)
        {
            diagnostic->severity = Diagnostic::Severity::Error;
            diagnostic->code = "bad-name";
            diagnostic->message = name.empty()
                                      ? std::string("这里要写一个符号名，例如 %symbol DEBUG")
                                      : ("符号名不合法：" + name);
        }
        return false;
    }

    out.name = name;
    out.value = value;
    return true;
}

bool Preprocessor::parseTruthValue(const std::string &value)
{
    const std::string lowered = toLower(trim(value));
    if (lowered.empty()) return false;
    if (lowered == "0") return false;
    if (lowered == "false") return false;
    if (lowered == "no") return false;
    if (lowered == "off") return false;
    return true;
}

Options Preprocessor::forGrammar()
{
    Options options;
    // `.gram` 的语义动作里可能出现 `#`，所以只认 `%` 前缀。
    options.syntax = Options::Syntax::Percent;
    options.unknownDirective = Options::UnknownDirective::Keep;
    options.percentDefineIsSymbol = false;
    // `.gram` 里这三处的正文不解析（正文里可能出现 `%if` 之类的字面量）。
    options.opaqueRegions = {
        {"%code", "%endcode", true},
        {"%class_body", "%endbody", true},
    };
    return options;
}

Options Preprocessor::forC()
{
    Options options;
    options.syntax = Options::Syntax::Hash;
    options.lineComments = {"//"};
    options.blockComments = {{"/*", "*/"}};
    options.unknownDirective = Options::UnknownDirective::Error;
    return options;
}

Options Preprocessor::forMetro()
{
    Options options;
    options.syntax = Options::Syntax::Percent;
    options.percentDefineIsSymbol = true;
    options.lineComments = {"//"};
    options.blockComments = {{"/*", "*/"}};
    options.unknownDirective = Options::UnknownDirective::Error;
    return options;
}

std::string Options::prefixes() const
{
    switch (syntax)
    {
    case Syntax::Auto:
        return "#%";
    case Syntax::Hash:
        return "#";
    case Syntax::Percent:
        return "%";
    case Syntax::Custom:
    default:
        return directivePrefixes;
    }
}

} // namespace preprocess
