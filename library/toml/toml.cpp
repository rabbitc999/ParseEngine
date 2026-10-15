/**
 * @file toml.cpp
 * @brief toml.hpp 的实现：解析、序列化、表/值操作、路径查询、迭代器。
 */

#include "toml.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <stdexcept>

namespace toml
{
namespace
{

/** 表成员的"没有数组下标"取值。 */
constexpr std::size_t kNoIndex = static_cast<std::size_t>(-1);

/**
 * @brief 是不是 ASCII 数字。
 * @param ch 字符。
 * @return 是数字返回 true。
 */
bool isDigit(char ch) noexcept { return (ch >= '0') && (ch <= '9'); }

/**
 * @brief 是不是裸键字符（`A-Z a-z 0-9 _ -`）。
 * @param ch 字符。
 * @return 是裸键字符返回 true。
 */
bool isBareKeyChar(char ch) noexcept
{
    return ((ch >= 'A') && (ch <= 'Z')) || ((ch >= 'a') && (ch <= 'z')) ||
           ((ch >= '0') && (ch <= '9')) || (ch == '_') || (ch == '-');
}

/**
 * @brief 在给定进制下是不是合法数字字符。
 * @param ch 字符。
 * @param base 进制（2 / 8 / 10 / 16）。
 * @return 合法返回 true。
 */
bool isDigitForBase(char ch, int base) noexcept
{
    if ((ch >= '0') && (ch <= '9')) return (ch - '0') < base;
    if ((ch >= 'a') && (ch <= 'f')) return (base == 16);
    if ((ch >= 'A') && (ch <= 'F')) return (base == 16);
    return false;
}

/**
 * @brief 把整数写成文本。
 * @param value 整数。
 * @return 文本。
 */
std::string formatInt(std::int64_t value)
{
    char buffer[32];
    const std::to_chars_result result = std::to_chars(buffer, buffer + sizeof(buffer), value);
    return std::string(buffer, result.ptr);
}

/**
 * @brief 把浮点写成 TOML 文本。
 *
 * 先按 17 位有效数字打印，再逐级缩短到能"往返"（再解析回来还是同一个值）为止。
 * 整数值的浮点补一个 `.0`，保证再解析回来还是浮点；`inf` / `nan` 按 TOML 写法输出。
 *
 * @param value 浮点。
 * @return 文本。
 */
std::string formatDouble(double value)
{
    if (std::isnan(value)) return "nan";
    if (std::isinf(value)) return (value < 0.0) ? "-inf" : "inf";

    char buffer[64];
    std::string text;
    for (int precision = 17; precision >= 1; --precision)
    {
        std::snprintf(buffer, sizeof(buffer), "%.*g", precision, value);
        text = buffer;
        char *end = nullptr;
        const double reparsed = std::strtod(text.c_str(), &end);
        if ((end != nullptr) && (*end == '\0') && (reparsed == value)) break;
    }
    if (text.find_first_of(".eE") == std::string::npos) text += ".0";
    return text;
}

/**
 * @brief 把秒的小数部分写成 TOML 文本（去掉末尾多余的 0）。
 * @param nanosecond 纳秒。
 * @return 形如 `.5` / `.123456789`；为 0 时返回空串。
 */
std::string formatFraction(std::uint32_t nanosecond)
{
    if (nanosecond == 0u) return std::string();
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "%09u", nanosecond);
    std::string digits(buffer);
    while ((digits.size() > 1u) && (digits.back() == '0')) digits.pop_back();
    return "." + digits;
}

/**
 * @brief 把字符串按 TOML 基本字符串的转义规则写进输出串。
 * @param out 输出串。
 * @param text 原文（UTF-8，非 ASCII 原样透传）。
 */
void escapeInto(std::string &out, std::string_view text)
{
    out += '"';
    for (const char raw : text)
    {
        const unsigned char ch = static_cast<unsigned char>(raw);
        switch (ch)
        {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (ch < 0x20u)
            {
                char buffer[8];
                std::snprintf(buffer, sizeof(buffer), "\\u%04x", ch);
                out += buffer;
            }
            else
            {
                out += static_cast<char>(ch);
            }
            break;
        }
    }
    out += '"';
}

/**
 * @brief 写一个键：能用裸键就用裸键，否则加引号。
 * @param out 输出串。
 * @param key 键名。
 */
void writeKeyInto(std::string &out, std::string_view key)
{
    bool bare = !key.empty();
    for (const char ch : key)
    {
        if (!isBareKeyChar(ch)) { bare = false; break; }
    }
    if (bare) out += std::string(key);
    else escapeInto(out, key);
}

/**
 * @brief 递归写一个值的内联形式。
 * @param out 输出串。
 * @param value 值。
 */
void dumpInlineInto(std::string &out, const Value &value)
{
    if (value.isBool()) { out += value.asBool().value() ? "true" : "false"; return; }
    if (value.isInt()) { out += formatInt(value.asInt().value()); return; }
    if (value.isDouble()) { out += formatDouble(value.asDouble().value()); return; }
    if (value.isString()) { escapeInto(out, value.asString().value()); return; }
    if (value.isDateTime()) { out += value.asDateTime().value().toString(); return; }

    if (const Array *items = value.asArray())
    {
        out += '[';
        for (std::size_t index = 0; index < items->size(); ++index)
        {
            if (index != 0u) out += ", ";
            dumpInlineInto(out, (*items)[index]);
        }
        out += ']';
        return;
    }

    // 剩下的就是表：写成内联表。
    const Object *table = value.asTable();
    if (table->empty()) { out += "{}"; return; }
    out += "{ ";
    bool first = true;
    for (const Member &member : *table)
    {
        if (!first) out += ", ";
        first = false;
        writeKeyInto(out, member.key);
        out += " = ";
        dumpInlineInto(out, member.value);
    }
    out += " }";
}

/**
 * @brief 判断一个值是不是"表数组"（非空且每个元素都是表）。
 * @param value 值。
 * @return 是表数组返回 true。
 */
bool isArrayOfTables(const Value &value)
{
    const Array *items = value.asArray();
    if ((items == nullptr) || items->empty()) return false;
    for (const Value &item : *items)
    {
        if (!item.isTable()) return false;
    }
    return true;
}

/**
 * @brief 递归写一张表的 TOML 文档内容。
 *
 * 先写标量 / 内联值成员，再写 `[子表]` 与 `[[表数组]]`——保证键值对出现在子表
 * 表头之前，这是 TOML 的硬性要求。
 *
 * @param out 输出串。
 * @param table 表。
 * @param prefix 表头的路径前缀（如 `server.tls`）。
 */
void dumpTableInto(std::string &out, const Object &table, const std::string &prefix)
{
    // 1. 标量 / 数组成员。
    for (const Member &member : table)
    {
        if (member.value.isTable() || isArrayOfTables(member.value)) continue;
        writeKeyInto(out, member.key);
        out += " = ";
        dumpInlineInto(out, member.value);
        out += '\n';
    }

    // 2. 子表与表数组。
    for (const Member &member : table)
    {
        std::string path = prefix;
        if (!path.empty()) path += '.';
        writeKeyInto(path, member.key);

        if (member.value.isTable())
        {
            if (!out.empty() && (out.back() == '\n')) out += '\n';
            out += '[' + path + "]\n";
            dumpTableInto(out, *member.value.asTable(), path);
        }
        else if (isArrayOfTables(member.value))
        {
            for (const Value &element : *member.value.asArray())
            {
                if (!out.empty() && (out.back() == '\n')) out += '\n';
                out += "[[" + path + "]]\n";
                dumpTableInto(out, *element.asTable(), path);
            }
        }
    }
}

/**
 * @brief 比较两个值（数字跨 int / double 按数值比；表按成员集合比）。
 * @param left 左。
 * @param right 右。
 * @return 相等返回 true。
 */
bool equalsValue(const Value &left, const Value &right)
{
    if (left.isNumber() && right.isNumber())
    {
        return left.asDouble().value() == right.asDouble().value();
    }
    if (left.storage().index() != right.storage().index()) return false;

    if (const Array *items = left.asArray())
    {
        const Array *other = right.asArray();
        return (items->size() == other->size()) &&
               std::equal(items->begin(), items->end(), other->begin(),
                          [](const Value &a, const Value &b) { return equalsValue(a, b); });
    }
    if (const Object *table = left.asTable())
    {
        const Object *other = right.asTable();
        if (table->size() != other->size()) return false;
        for (const Member &member : *table)
        {
            const Value *peer = other->tryGet(member.key);
            if ((peer == nullptr) || !equalsValue(member.value, *peer)) return false;
        }
        return true;
    }
    if (left.isString()) return left.asString().value() == right.asString().value();
    if (left.isBool()) return left.asBool().value() == right.asBool().value();
    if (left.isDateTime()) return left.asDateTime().value() == right.asDateTime().value();
    return true;
}

/**
 * @brief TOML 解析器（手写、逐行驱动）。
 *
 * 遇到第一个错误就停下，并把行列信息记进诊断。除了解析语法，它还维护一棵与值树
 * 并行的"元数据树"，用来判定 `[table]` / `[[array]]` / 点分键之间的定义冲突。
 */
class Parser
{
public:
    /**
     * @brief 构造。
     * @param text 文本（UTF-8，允许带 BOM）。
     * @param options 选项。
     */
    Parser(std::string_view text, const ParseOptions &options)
        : text_(text), options_(options)
    {
        if ((text_.size() >= 3u) && (static_cast<unsigned char>(text_[0]) == 0xEFu) &&
            (static_cast<unsigned char>(text_[1]) == 0xBBu) &&
            (static_cast<unsigned char>(text_[2]) == 0xBFu))
        {
            index_ = 3u;
        }
    }

    /**
     * @brief 跑一遍解析。
     * @return 结果。
     */
    ParseResult run()
    {
        ParseResult result;
        while (true)
        {
            skipHorizontal();
            if (atEnd()) break;

            const char ch = peek();
            if ((ch == '\n') || (ch == '\r')) { consumeNewline(); continue; }
            if (ch == '#') { skipComment(); continue; }

            const bool ok = (ch == '[') ? parseTableHeader() : parseKeyValue();
            if (!ok)
            {
                result.diagnostics.push_back(diagnostic_);
                return result;
            }
            if (!skipLineEnd())
            {
                fail("expected-newline", "这条语句后面只允许空白、注释与换行");
                result.diagnostics.push_back(diagnostic_);
                return result;
            }
        }

        result.value = std::move(root_);
        return result;
    }

private:
    // ---- 元数据树 ---------------------------------------------------------

    /** 元数据节点的种类。 */
    enum class MetaKind
    {
        Table,          ///< 普通表（含隐式表、表头定义的表、点分键表）。
        ArrayOfTables,  ///< 表数组 `[[x]]`。
        Value,          ///< 已经赋过值的叶子。
    };

    /** 与值树并行的定义信息：用来判定重复 / 冲突。 */
    struct MetaNode
    {
        MetaKind kind = MetaKind::Table;                       ///< 种类。
        bool explicitHeader = false;                           ///< 由 `[x]` / `[[x]]` 定义。
        bool dottedTable = false;                              ///< 由点分键创建。
        bool inlineTable = false;                              ///< 内联表（封闭）。
        std::vector<std::pair<std::string, MetaNode>> children; ///< 子节点。
        std::vector<MetaNode> elements;                        ///< 表数组的元素。
    };

    /** 从根到某个表 / 数组元素的路径里的一段。 */
    struct Step
    {
        std::string key;                       ///< 键名。
        std::size_t element = kNoIndex;        ///< 数组下标；`kNoIndex` 表示普通表成员。
    };

    // ---- 基础读写 ---------------------------------------------------------

    /**
     * @brief 是否到末尾。
     * @return 到末尾返回 true。
     */
    bool atEnd() const noexcept { return index_ >= text_.size(); }

    /**
     * @brief 看一个字符（不前进）。
     * @param ahead 向前看几个字符。
     * @return 字符；越界返回 '\0'。
     */
    char peek(std::size_t ahead = 0u) const noexcept
    {
        return (index_ + ahead < text_.size()) ? text_[index_ + ahead] : '\0';
    }

    /**
     * @brief 取一个字符并推进，同时维护行列号。
     * @return 字符。
     */
    char get() noexcept
    {
        const char ch = text_[index_++];
        if (ch == '\n') { ++line_; column_ = 1u; }
        else { ++column_; }
        return ch;
    }

    /**
     * @brief 记下第一条诊断。
     * @param code 短码。
     * @param message 说明。
     */
    void fail(std::string code, std::string message)
    {
        diagnostic_.line = line_;
        diagnostic_.column = column_;
        diagnostic_.offset = index_;
        diagnostic_.code = std::move(code);
        diagnostic_.message = std::move(message);
    }

    /** 跳过空格 / 制表符。 */
    void skipHorizontal() noexcept
    {
        while (!atEnd() && ((peek() == ' ') || (peek() == '\t'))) get();
    }

    /** 跳过一个 `#` 注释（不消费换行）。 */
    void skipComment() noexcept
    {
        while (!atEnd() && (peek() != '\n') && (peek() != '\r')) get();
    }

    /** 消费一个换行（CR、LF 或 CRLF）。 */
    void consumeNewline() noexcept
    {
        if (peek() == '\r') get();
        if (peek() == '\n') get();
    }

    /**
     * @brief 语句结尾：只允许空白、注释，然后换行 / 结束。
     * @return 合法返回 true。
     */
    bool skipLineEnd()
    {
        skipHorizontal();
        if (peek() == '#') skipComment();
        if (atEnd()) return true;
        if ((peek() == '\n') || (peek() == '\r')) { consumeNewline(); return true; }
        return false;
    }

    /** 跳过数组里的空白、换行与注释。 */
    void skipArraySpace() noexcept
    {
        while (!atEnd())
        {
            const char ch = peek();
            if ((ch == ' ') || (ch == '\t') || (ch == '\n') || (ch == '\r')) { get(); continue; }
            if (ch == '#') { skipComment(); continue; }
            break;
        }
    }

    // ---- 键 ---------------------------------------------------------------

    /**
     * @brief 解析一个简单键（裸键 / 基本字符串 / 字面量字符串）。
     * @param out 键名。
     * @return 成功返回 true。
     */
    bool parseSimpleKey(std::string &out)
    {
        const char ch = peek();
        if (ch == '"') return parseBasicString(out, false);
        if (ch == '\'') return parseLiteralString(out, false);

        std::string key;
        while (!atEnd() && isBareKeyChar(peek())) { key += peek(); get(); }
        if (key.empty())
        {
            fail("expected-key", "这里需要一个键（裸键或带引号的键）");
            return false;
        }
        out = std::move(key);
        return true;
    }

    /**
     * @brief 解析点分键（`a.b.c`）。
     * @param out 各段。
     * @return 成功返回 true。
     */
    bool parseKeyPath(std::vector<std::string> &out)
    {
        while (true)
        {
            skipHorizontal();
            std::string key;
            if (!parseSimpleKey(key)) return false;
            out.push_back(std::move(key));
            skipHorizontal();
            if (peek() == '.') { get(); continue; }
            break;
        }
        return true;
    }

    // ---- 字符串 -----------------------------------------------------------

    /**
     * @brief 判断当前位置的 `\` 是不是"行尾折行"。
     * @return 是折行返回 true。
     */
    bool isLineEndingBackslash() const noexcept
    {
        std::size_t position = index_ + 1u;
        while ((position < text_.size()) && ((text_[position] == ' ') || (text_[position] == '\t')))
        {
            ++position;
        }
        if (position >= text_.size()) return true;
        return (text_[position] == '\n') || (text_[position] == '\r');
    }

    /**
     * @brief 解析一个转义序列（当前位置在 `\` 之后）。
     * @param out 输出串。
     * @return 成功返回 true。
     */
    bool parseEscape(std::string &out)
    {
        if (atEnd()) { fail("bad-escape", "转义序列不完整"); return false; }
        const char escape = get();
        switch (escape)
        {
        case '"': out += '"'; break;
        case '\\': out += '\\'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'n': out += '\n'; break;
        case 'r': out += '\r'; break;
        case 't': out += '\t'; break;
        case 'u':
        {
            std::uint32_t code = 0;
            if (!parseHexDigits(4, code)) return false;
            appendUtf8(out, code);
            break;
        }
        case 'U':
        {
            std::uint32_t code = 0;
            if (!parseHexDigits(8, code)) return false;
            if (code > 0x10FFFFu)
            {
                fail("bad-escape", "\\U 的码点超过 0x10FFFF");
                return false;
            }
            appendUtf8(out, code);
            break;
        }
        default:
            fail("bad-escape", std::string("不认识的转义：\\") + escape);
            return false;
        }
        return true;
    }

    /**
     * @brief 读若干个十六进制位。
     * @param count 位数。
     * @param out 结果。
     * @return 成功返回 true。
     */
    bool parseHexDigits(int count, std::uint32_t &out)
    {
        std::uint32_t value = 0;
        for (int index = 0; index < count; ++index)
        {
            if (atEnd()) { fail("bad-escape", "十六进制转义位数不够"); return false; }
            const char ch = get();
            value <<= 4u;
            if ((ch >= '0') && (ch <= '9')) value |= static_cast<std::uint32_t>(ch - '0');
            else if ((ch >= 'a') && (ch <= 'f')) value |= static_cast<std::uint32_t>(ch - 'a' + 10);
            else if ((ch >= 'A') && (ch <= 'F')) value |= static_cast<std::uint32_t>(ch - 'A' + 10);
            else { fail("bad-escape", "十六进制转义里有非法字符"); return false; }
        }
        out = value;
        return true;
    }

    /**
     * @brief 把码点按 UTF-8 追加进字符串。
     * @param out 输出串。
     * @param code 码点。
     */
    static void appendUtf8(std::string &out, std::uint32_t code)
    {
        if (code <= 0x7Fu)
        {
            out += static_cast<char>(code);
        }
        else if (code <= 0x7FFu)
        {
            out += static_cast<char>(0xC0u | (code >> 6u));
            out += static_cast<char>(0x80u | (code & 0x3Fu));
        }
        else if (code <= 0xFFFFu)
        {
            out += static_cast<char>(0xE0u | (code >> 12u));
            out += static_cast<char>(0x80u | ((code >> 6u) & 0x3Fu));
            out += static_cast<char>(0x80u | (code & 0x3Fu));
        }
        else
        {
            out += static_cast<char>(0xF0u | (code >> 18u));
            out += static_cast<char>(0x80u | ((code >> 12u) & 0x3Fu));
            out += static_cast<char>(0x80u | ((code >> 6u) & 0x3Fu));
            out += static_cast<char>(0x80u | (code & 0x3Fu));
        }
    }

    /**
     * @brief 解析基本字符串（单行或三引号多行）。
     * @param out 结果。
     * @param multiline 是不是多行。
     * @return 成功返回 true。
     */
    bool parseBasicString(std::string &out, bool multiline)
    {
        if (multiline)
        {
            for (int quote = 0; quote < 3; ++quote) get();
            // 紧跟开定界符的换行会被裁掉。
            if ((peek() == '\n') || (peek() == '\r')) consumeNewline();
        }
        else
        {
            get();
        }

        while (!atEnd())
        {
            const char ch = peek();

            if (ch == '"')
            {
                std::size_t run = 0;
                while (peek(run) == '"') ++run;
                if (!multiline)
                {
                    get();
                    return true;
                }
                if (run >= 3)
                {
                    const std::size_t extra = run - 3u;
                    if (extra > 2u) { fail("bad-string", "多行字符串里连续引号太多"); return false; }
                    for (std::size_t step = 0; step < extra; ++step) { out += '"'; get(); }
                    get(); get(); get();
                    return true;
                }
                for (std::size_t step = 0; step < run; ++step) { out += '"'; get(); }
                continue;
            }

            if (ch == '\\')
            {
                if (multiline && isLineEndingBackslash())
                {
                    get();
                    while (!atEnd())
                    {
                        const char white = peek();
                        if ((white == ' ') || (white == '\t') || (white == '\n') || (white == '\r')) get();
                        else break;
                    }
                    continue;
                }
                get();
                if (!parseEscape(out)) return false;
                continue;
            }

            if (!multiline && ((ch == '\n') || (ch == '\r')))
            {
                fail("unterminated-string", "单行字符串里不能换行");
                return false;
            }
            if ((static_cast<unsigned char>(ch) < 0x20u) && (ch != '\t') && (ch != '\n') && (ch != '\r'))
            {
                fail("control-char", "字符串里出现了不允许的控制字符");
                return false;
            }
            out += ch;
            get();
        }

        fail("unterminated-string", "字符串没有闭合");
        return false;
    }

    /**
     * @brief 解析字面量字符串（单引号，无转义）。
     * @param out 结果。
     * @param multiline 是不是多行。
     * @return 成功返回 true。
     */
    bool parseLiteralString(std::string &out, bool multiline)
    {
        if (multiline)
        {
            for (int quote = 0; quote < 3; ++quote) get();
            if ((peek() == '\n') || (peek() == '\r')) consumeNewline();
        }
        else
        {
            get();
        }

        while (!atEnd())
        {
            const char ch = peek();

            if (ch == '\'')
            {
                std::size_t run = 0;
                while (peek(run) == '\'') ++run;
                if (!multiline)
                {
                    get();
                    return true;
                }
                if (run >= 3)
                {
                    const std::size_t extra = run - 3u;
                    if (extra > 2u) { fail("bad-string", "多行字符串里连续引号太多"); return false; }
                    for (std::size_t step = 0; step < extra; ++step) { out += '\''; get(); }
                    get(); get(); get();
                    return true;
                }
                for (std::size_t step = 0; step < run; ++step) { out += '\''; get(); }
                continue;
            }

            if (!multiline && ((ch == '\n') || (ch == '\r')))
            {
                fail("unterminated-string", "单行字符串里不能换行");
                return false;
            }
            if ((static_cast<unsigned char>(ch) < 0x20u) && (ch != '\t') && (ch != '\n') && (ch != '\r'))
            {
                fail("control-char", "字符串里出现了不允许的控制字符");
                return false;
            }
            out += ch;
            get();
        }

        fail("unterminated-string", "字符串没有闭合");
        return false;
    }

    /**
     * @brief 解析一个字符串值（按引号形式自动分派）。
     * @param out 结果。
     * @return 成功返回 true。
     */
    bool parseStringValue(std::string &out)
    {
        const char ch = peek();
        if (ch == '"')
        {
            const bool multiline = (peek(1) == '"') && (peek(2) == '"');
            return parseBasicString(out, multiline);
        }
        if (ch == '\'')
        {
            const bool multiline = (peek(1) == '\'') && (peek(2) == '\'');
            return parseLiteralString(out, multiline);
        }
        fail("bad-string", "这里需要一个字符串");
        return false;
    }

    // ---- 数字与日期时间 ---------------------------------------------------

    /**
     * @brief 读连续的十进制数字（允许 `_` 夹在数字之间）。
     * @param out 输出串（不含 `_`）。
     * @return 至少读到一位数字返回 true。
     */
    bool readDigitRun(std::string &out)
    {
        if (!isDigit(peek())) return false;
        while (true)
        {
            if (isDigit(peek())) { out += peek(); get(); }
            else if (peek() == '_')
            {
                if (!isDigit(peek(1))) { fail("bad-number", "下划线两侧必须是数字"); return false; }
                get();
            }
            else
            {
                break;
            }
        }
        return true;
    }

    /**
     * @brief 读某个进制的数字串（允许 `_` 夹在数字之间），用于 `0x` / `0o` / `0b`。
     * @param out 输出串（不含 `_`）。
     * @param base 进制。
     * @return 至少读到一位数字返回 true。
     */
    bool readRadixDigits(std::string &out, int base)
    {
        if (!isDigitForBase(peek(), base)) { fail("bad-number", "进制前缀后面要有数字"); return false; }
        while (true)
        {
            if (isDigitForBase(peek(), base)) { out += peek(); get(); }
            else if (peek() == '_')
            {
                if (!isDigitForBase(peek(1), base)) { fail("bad-number", "下划线两侧必须是数字"); return false; }
                get();
            }
            else
            {
                break;
            }
        }
        return true;
    }

    /**
     * @brief 把整数文本按进制解析成值。
     * @param out 结果。
     * @param token 文本。
     * @param base 进制。
     * @return 成功返回 true。
     */
    bool finishInteger(Value &out, const std::string &token, int base)
    {
        // std::from_chars 不接受前导 '+'，先手工去掉。
        std::string_view view(token);
        if (!view.empty() && (view.front() == '+')) view.remove_prefix(1u);

        std::int64_t number = 0;
        const char *begin = view.data();
        const char *end = begin + view.size();
        const std::from_chars_result result = std::from_chars(begin, end, number, base);
        if ((result.ec != std::errc()) || (result.ptr != end))
        {
            fail("bad-number", "整数超出 64 位范围或格式不对：" + token);
            return false;
        }
        out = Value(number);
        return true;
    }

    /**
     * @brief 解析数字（整数 / 浮点 / `inf` / `nan`）。
     * @param out 结果。
     * @return 成功返回 true。
     */
    bool parseNumber(Value &out)
    {
        std::string token;
        bool negative = false;
        if ((peek() == '+') || (peek() == '-'))
        {
            negative = (peek() == '-');
            token += peek();
            get();
        }

        if (matchWord("inf"))
        {
            const double infinity = std::numeric_limits<double>::infinity();
            out = Value(negative ? -infinity : infinity);
            return true;
        }
        if (matchWord("nan"))
        {
            out = Value(std::numeric_limits<double>::quiet_NaN());
            return true;
        }

        // 带前缀的整数不允许符号。
        if ((peek() == '0') && ((peek(1) == 'x') || (peek(1) == 'X')))
        {
            if (!token.empty()) { fail("bad-number", "十六进制整数不能带正负号"); return false; }
            get(); get();
            if (!readRadixDigits(token, 16)) return false;
            return finishInteger(out, token, 16);
        }
        if ((peek() == '0') && ((peek(1) == 'o') || (peek(1) == 'O')))
        {
            if (!token.empty()) { fail("bad-number", "八进制整数不能带正负号"); return false; }
            get(); get();
            if (!readRadixDigits(token, 8)) return false;
            return finishInteger(out, token, 8);
        }
        if ((peek() == '0') && ((peek(1) == 'b') || (peek(1) == 'B')))
        {
            if (!token.empty()) { fail("bad-number", "二进制整数不能带正负号"); return false; }
            get(); get();
            if (!readRadixDigits(token, 2)) return false;
            return finishInteger(out, token, 2);
        }

        // 十进制整数部分。
        if (peek() == '0')
        {
            token += '0';
            get();
            if (isDigit(peek()) || (peek() == '_'))
            {
                fail("bad-number", "十进制整数不能有前导 0");
                return false;
            }
        }
        else if (isDigit(peek()))
        {
            if (!readDigitRun(token)) return false;
        }
        else
        {
            fail("unexpected-token", std::string("不认识的字符 '") + peek() + "'");
            return false;
        }

        bool isFloat = false;
        if (peek() == '.')
        {
            isFloat = true;
            token += '.';
            get();
            std::string fraction;
            if (!readDigitRun(fraction))
            {
                fail("bad-number", "小数点后面要有数字");
                return false;
            }
            token += fraction;
        }
        if ((peek() == 'e') || (peek() == 'E'))
        {
            isFloat = true;
            token += 'e';
            get();
            if ((peek() == '+') || (peek() == '-')) { token += peek(); get(); }
            std::string exponent;
            if (!readDigitRun(exponent))
            {
                fail("bad-number", "指数后面要有数字");
                return false;
            }
            token += exponent;
        }

        if (!isFloat) return finishInteger(out, token, 10);

        char *end = nullptr;
        const double number = std::strtod(token.c_str(), &end);
        if ((end == nullptr) || (*end != '\0'))
        {
            fail("bad-number", "浮点格式不对：" + token);
            return false;
        }
        out = Value(number);
        return true;
    }

    /**
     * @brief 匹配一个关键字，并要求后面不是裸键字符。
     * @param word 关键字。
     * @return 匹配返回 true（消费掉关键字）。
     */
    bool matchWord(std::string_view word)
    {
        if (text_.compare(index_, word.size(), word) != 0) return false;
        if (isBareKeyChar(peek(word.size()))) return false;
        for (std::size_t step = 0; step < word.size(); ++step) get();
        return true;
    }

    /**
     * @brief 某一位置起是不是 `YYYY-MM-DD`。
     * @param position 位置。
     * @return 是日期返回 true。
     */
    bool looksLikeDateAt(std::size_t position) const noexcept
    {
        if (position + 10u > text_.size()) return false;
        return isDigit(text_[position]) && isDigit(text_[position + 1u]) &&
               isDigit(text_[position + 2u]) && isDigit(text_[position + 3u]) &&
               (text_[position + 4u] == '-') && isDigit(text_[position + 5u]) &&
               isDigit(text_[position + 6u]) && (text_[position + 7u] == '-') &&
               isDigit(text_[position + 8u]) && isDigit(text_[position + 9u]);
    }

    /**
     * @brief 某一位置起是不是 `HH:MM:SS`。
     * @param position 位置。
     * @return 是时间返回 true。
     */
    bool looksLikeTimeAt(std::size_t position) const noexcept
    {
        if (position + 8u > text_.size()) return false;
        return isDigit(text_[position]) && isDigit(text_[position + 1u]) &&
               (text_[position + 2u] == ':') && isDigit(text_[position + 3u]) &&
               isDigit(text_[position + 4u]) && (text_[position + 5u] == ':') &&
               isDigit(text_[position + 6u]) && isDigit(text_[position + 7u]);
    }

    /**
     * @brief 读固定位数的十进制数字。
     * @param count 位数。
     * @param out 结果。
     * @return 成功返回 true。
     */
    bool readFixedDigits(int count, int &out)
    {
        int value = 0;
        for (int index = 0; index < count; ++index)
        {
            if (!isDigit(peek())) { fail("bad-datetime", "日期 / 时间里的数字位数不对"); return false; }
            value = value * 10 + (peek() - '0');
            get();
        }
        out = value;
        return true;
    }

    /**
     * @brief 读时间部分（`HH:MM:SS[.fff]`）。
     * @param out 结果（就地写入 hour / minute / second / nanosecond）。
     * @return 成功返回 true。
     */
    bool readTimeFields(DateTime &out)
    {
        if (!readFixedDigits(2, out.hour)) return false;
        if (peek() != ':') { fail("bad-datetime", "时间要用 ':' 分隔"); return false; }
        get();
        if (!readFixedDigits(2, out.minute)) return false;
        if (peek() != ':') { fail("bad-datetime", "时间要用 ':' 分隔"); return false; }
        get();
        if (!readFixedDigits(2, out.second)) return false;

        if (peek() == '.')
        {
            get();
            if (!isDigit(peek())) { fail("bad-datetime", "秒的小数部分要有数字"); return false; }
            int total = 0;
            std::uint32_t fraction = 0;
            while (isDigit(peek()))
            {
                if (total < 9) fraction = fraction * 10u + static_cast<std::uint32_t>(peek() - '0');
                ++total;
                get();
            }
            for (int index = total; index < 9; ++index) fraction *= 10u;
            out.nanosecond = fraction;
        }
        return true;
    }

    /**
     * @brief 解析日期时间（日期、可选的时间与偏移）。
     * @param out 结果。
     * @return 成功返回 true。
     */
    bool parseDateTime(Value &out)
    {
        DateTime dateTime;
        if (!readFixedDigits(4, dateTime.year)) return false;
        if (peek() != '-') { fail("bad-datetime", "日期要用 '-' 分隔"); return false; }
        get();
        if (!readFixedDigits(2, dateTime.month)) return false;
        if (peek() != '-') { fail("bad-datetime", "日期要用 '-' 分隔"); return false; }
        get();
        if (!readFixedDigits(2, dateTime.day)) return false;

        bool hasTime = false;
        if ((peek() == 'T') || (peek() == 't')) hasTime = true;
        else if ((peek() == ' ') && looksLikeTimeAt(index_ + 1u)) hasTime = true;

        if (!hasTime)
        {
            if ((dateTime.month < 1) || (dateTime.month > 12) || (dateTime.day < 1) || (dateTime.day > 31))
            {
                fail("bad-datetime", "日期超出范围");
                return false;
            }
            dateTime.kind = DateTimeKind::LocalDate;
            out = Value(dateTime);
            return true;
        }

        get(); // 'T' / 't' / 空格
        if (!readTimeFields(dateTime)) return false;

        if ((peek() == 'Z') || (peek() == 'z'))
        {
            get();
            dateTime.offsetMinutes = 0;
            dateTime.kind = DateTimeKind::OffsetDateTime;
        }
        else if ((peek() == '+') || (peek() == '-'))
        {
            const int sign = (peek() == '+') ? 1 : -1;
            get();
            int offsetHour = 0;
            int offsetMinute = 0;
            if (!readFixedDigits(2, offsetHour)) return false;
            if (peek() != ':') { fail("bad-datetime", "时区偏移要写成 ±HH:MM"); return false; }
            get();
            if (!readFixedDigits(2, offsetMinute)) return false;
            dateTime.offsetMinutes = sign * (offsetHour * 60 + offsetMinute);
            dateTime.kind = DateTimeKind::OffsetDateTime;
        }
        else
        {
            dateTime.kind = DateTimeKind::LocalDateTime;
        }

        if ((dateTime.month < 1) || (dateTime.month > 12) || (dateTime.day < 1) || (dateTime.day > 31) ||
            (dateTime.hour > 23) || (dateTime.minute > 59) || (dateTime.second > 60))
        {
            fail("bad-datetime", "日期时间超出范围");
            return false;
        }
        out = Value(dateTime);
        return true;
    }

    /**
     * @brief 解析本地时间（纯 `HH:MM:SS[.fff]`）。
     * @param out 结果。
     * @return 成功返回 true。
     */
    bool parseLocalTime(Value &out)
    {
        DateTime dateTime;
        dateTime.kind = DateTimeKind::LocalTime;
        if (!readTimeFields(dateTime)) return false;
        if ((dateTime.hour > 23) || (dateTime.minute > 59) || (dateTime.second > 60))
        {
            fail("bad-datetime", "时间超出范围");
            return false;
        }
        out = Value(dateTime);
        return true;
    }

    /**
     * @brief 解析一个数字或日期时间（按形状分派）。
     * @param out 结果。
     * @return 成功返回 true。
     */
    bool parseNumberOrDateTime(Value &out)
    {
        if (looksLikeDateAt(index_)) return parseDateTime(out);
        if (looksLikeTimeAt(index_)) return parseLocalTime(out);
        return parseNumber(out);
    }

    // ---- 值 ---------------------------------------------------------------

    /**
     * @brief 解析布尔。
     * @param out 结果。
     * @return 成功返回 true。
     */
    bool parseBoolean(Value &out)
    {
        if (matchWord("true")) { out = Value(true); return true; }
        if (matchWord("false")) { out = Value(false); return true; }
        fail("unexpected-token", "这里只认 true / false");
        return false;
    }

    /**
     * @brief 解析数组（可以跨行，允许尾逗号与注释）。
     * @param out 结果。
     * @return 成功返回 true。
     */
    bool parseArray(Value &out)
    {
        get(); // '['
        Array items;
        while (true)
        {
            skipArraySpace();
            if (atEnd()) { fail("unterminated-array", "数组没有闭合"); return false; }
            if (peek() == ']') { get(); break; }

            Value item;
            if (!parseValue(item)) return false;
            items.push_back(std::move(item));

            skipArraySpace();
            if (atEnd()) { fail("unterminated-array", "数组没有闭合"); return false; }
            if (peek() == ',') { get(); continue; }
            if (peek() == ']') { get(); break; }
            fail("expected-comma", "数组元素之间要有逗号，结尾要有 ']'");
            return false;
        }
        out = Value(std::move(items));
        return true;
    }

    /**
     * @brief 把点分键的值塞进内联表。
     * @param base 目标表。
     * @param segments 点分键。
     * @param index 当前段。
     * @param value 值。
     * @return 成功返回 true。
     */
    bool insertInlineKey(Object &base, const std::vector<std::string> &segments,
                         std::size_t index, Value &&value)
    {
        const std::string &key = segments[index];
        if (index + 1u == segments.size())
        {
            if (base.contains(key) && !options_.allowDuplicateKeys)
            {
                fail("duplicate-key", "内联表里重复的键 '" + key + "'");
                return false;
            }
            base.insert(key, std::move(value));
            return true;
        }

        Value *member = base.tryGet(key);
        if (member != nullptr)
        {
            if (!member->isTable())
            {
                fail("key-conflict", "内联表里的键 '" + key + "' 已经被值占用");
                return false;
            }
        }
        else
        {
            base.insert(key, Value(Object{}));
            member = base.tryGet(key);
        }
        return insertInlineKey(*member->asTable(), segments, index + 1u, std::move(value));
    }

    /**
     * @brief 解析内联表（单行，不允许尾逗号与换行）。
     * @param out 结果。
     * @return 成功返回 true。
     */
    bool parseInlineTable(Value &out)
    {
        get(); // '{'
        Object table;
        skipHorizontal();
        if (peek() == '}') { get(); out = Value(std::move(table)); return true; }

        while (true)
        {
            skipHorizontal();
            std::vector<std::string> segments;
            if (!parseKeyPath(segments)) return false;
            skipHorizontal();
            if (peek() != '=') { fail("expected-equals", "内联表里键后面要跟 '='"); return false; }
            get();
            skipHorizontal();

            Value value;
            if (!parseValue(value)) return false;
            if (!insertInlineKey(table, segments, 0u, std::move(value))) return false;

            skipHorizontal();
            if (peek() == ',')
            {
                get();
                skipHorizontal();
                if (peek() == '}') { fail("trailing-comma", "内联表不允许尾逗号"); return false; }
                continue;
            }
            if (peek() == '}') { get(); break; }
            fail("expected-comma", "内联表成员之间要有逗号，结尾要有 '}'");
            return false;
        }
        out = Value(std::move(table));
        return true;
    }

    /**
     * @brief 解析一个值。
     * @param out 结果。
     * @return 成功返回 true。
     */
    bool parseValue(Value &out)
    {
        skipHorizontal();
        if (atEnd()) { fail("unexpected-end", "内容在值之前就结束了"); return false; }

        const char ch = peek();
        if (ch == '{') return parseInlineTable(out);
        if (ch == '[') return parseArray(out);
        if ((ch == '"') || (ch == '\''))
        {
            std::string text;
            if (!parseStringValue(text)) return false;
            out = Value(std::move(text));
            return true;
        }
        if ((ch == 't') || (ch == 'f')) return parseBoolean(out);
        return parseNumberOrDateTime(out);
    }

    // ---- 值树 / 元数据树的导航与写入 --------------------------------------

    /**
     * @brief 在元数据树里找一个子节点。
     * @param node 父节点。
     * @param key 键名。
     * @return 指针；没有返回 nullptr。
     */
    static MetaNode *findChild(MetaNode *node, std::string_view key) noexcept
    {
        for (auto &child : node->children)
        {
            if (child.first == key) return &child.second;
        }
        return nullptr;
    }

    /**
     * @brief 把路径解析到值树里的一个 Value。
     * @param path 路径。
     * @return 指针；解析失败返回 nullptr。
     */
    Value *resolveValue(const std::vector<Step> &path) noexcept
    {
        Value *current = &root_;
        for (const Step &step : path)
        {
            Object *table = current->asTable();
            if (table == nullptr) return nullptr;
            Value *member = table->tryGet(step.key);
            if (member == nullptr) return nullptr;
            if (step.element == kNoIndex)
            {
                current = member;
            }
            else
            {
                Array *items = member->asArray();
                if ((items == nullptr) || (step.element >= items->size())) return nullptr;
                current = &(*items)[step.element];
            }
        }
        return current;
    }

    /**
     * @brief 把路径解析到元数据树里的一个节点。
     * @param path 路径。
     * @return 指针；解析失败返回 nullptr。
     */
    MetaNode *resolveMeta(const std::vector<Step> &path) noexcept
    {
        MetaNode *node = &rootMeta_;
        for (const Step &step : path)
        {
            MetaNode *child = findChild(node, step.key);
            if (child == nullptr) return nullptr;
            if (step.element == kNoIndex)
            {
                node = child;
            }
            else
            {
                if (step.element >= child->elements.size()) return nullptr;
                node = &child->elements[step.element];
            }
        }
        return node;
    }

    /**
     * @brief 解析表头 / 表数组头，更新当前表位置。
     * @param segments 表头的键路径。
     * @param isArray 是不是 `[[...]]`。
     * @return 成功返回 true。
     */
    bool applyHeader(const std::vector<std::string> &segments, bool isArray)
    {
        MetaNode *node = &rootMeta_;
        Value *value = &root_;
        std::vector<Step> path;

        for (std::size_t index = 0; index < segments.size(); ++index)
        {
            const std::string &key = segments[index];
            const bool last = (index + 1u == segments.size());
            MetaNode *child = findChild(node, key);

            if (last && isArray)
            {
                if (child == nullptr)
                {
                    MetaNode fresh;
                    fresh.kind = MetaKind::ArrayOfTables;
                    node->children.emplace_back(key, std::move(fresh));
                    child = findChild(node, key);
                    value->asTable()->insert(key, Value(Array{}));
                }
                else
                {
                    if (child->kind != MetaKind::ArrayOfTables)
                    {
                        fail("key-conflict", "键 '" + key + "' 不是表数组，不能用 [[...]]");
                        return false;
                    }
                    if (child->inlineTable)
                    {
                        fail("inline-table", "内联表不能被表数组追加");
                        return false;
                    }
                }
                Value *member = value->asTable()->tryGet(key);
                Array *items = (member == nullptr) ? nullptr : member->asArray();
                if (items == nullptr)
                {
                    fail("key-conflict", "键 '" + key + "' 不是表数组");
                    return false;
                }
                child->elements.emplace_back(MetaNode{});
                items->push_back(Value(Object{}));
                const std::size_t element = items->size() - 1u;
                path.push_back(Step{key, element});
                node = &child->elements.back();
                value = &(*items)[element];
                continue;
            }

            if (last)
            {
                if (child == nullptr)
                {
                    MetaNode fresh;
                    fresh.kind = MetaKind::Table;
                    fresh.explicitHeader = true;
                    node->children.emplace_back(key, std::move(fresh));
                    child = findChild(node, key);
                    value->asTable()->insert(key, Value(Object{}));
                }
                else
                {
                    if (child->inlineTable)
                    {
                        fail("inline-table", "内联表是封闭的，不能用表头定义");
                        return false;
                    }
                    if (child->kind == MetaKind::Value)
                    {
                        fail("key-conflict", "键 '" + key + "' 已经是值，不能当表定义");
                        return false;
                    }
                    if (child->kind == MetaKind::ArrayOfTables)
                    {
                        fail("key-conflict", "键 '" + key + "' 是表数组，不能再用 [..] 定义");
                        return false;
                    }
                    if (child->dottedTable)
                    {
                        fail("duplicate-table", "点分键定义的表 '" + key + "' 不能用表头重开");
                        return false;
                    }
                    if (child->explicitHeader)
                    {
                        fail("duplicate-table", "表 '" + key + "' 重复定义");
                        return false;
                    }
                    child->explicitHeader = true;
                }
                Value *member = value->asTable()->tryGet(key);
                if ((member == nullptr) || !member->isTable())
                {
                    fail("key-conflict", "键 '" + key + "' 不是表");
                    return false;
                }
                path.push_back(Step{key, kNoIndex});
                node = child;
                value = member;
                continue;
            }

            // 中间段：表 / 隐式表 / 表数组的最后一个元素。
            if (child == nullptr)
            {
                MetaNode fresh;
                fresh.kind = MetaKind::Table;
                node->children.emplace_back(key, std::move(fresh));
                child = findChild(node, key);
                value->asTable()->insert(key, Value(Object{}));
                Value *member = value->asTable()->tryGet(key);
                path.push_back(Step{key, kNoIndex});
                node = child;
                value = member;
            }
            else if (child->inlineTable)
            {
                fail("inline-table", "内联表是封闭的，不能往下定义子表");
                return false;
            }
            else if (child->kind == MetaKind::Value)
            {
                fail("key-conflict", "键 '" + key + "' 已经是值，不能当表用");
                return false;
            }
            else if (child->kind == MetaKind::ArrayOfTables)
            {
                Value *member = value->asTable()->tryGet(key);
                Array *items = (member == nullptr) ? nullptr : member->asArray();
                if ((items == nullptr) || items->empty())
                {
                    fail("key-conflict", "表数组 '" + key + "' 还没有元素");
                    return false;
                }
                const std::size_t element = items->size() - 1u;
                path.push_back(Step{key, element});
                node = &child->elements.back();
                value = &(*items)[element];
            }
            else
            {
                Value *member = value->asTable()->tryGet(key);
                if ((member == nullptr) || !member->isTable())
                {
                    fail("key-conflict", "键 '" + key + "' 不是表");
                    return false;
                }
                path.push_back(Step{key, kNoIndex});
                node = child;
                value = member;
            }
        }

        current_ = std::move(path);
        return true;
    }

    /**
     * @brief 把 `键 = 值` 写进当前表（含点分键与冲突检查）。
     * @param segments 键路径。
     * @param value 值。
     * @return 成功返回 true。
     */
    bool applyKeyValue(const std::vector<std::string> &segments, Value &&value)
    {
        Value *baseValue = resolveValue(current_);
        MetaNode *baseMeta = resolveMeta(current_);
        if ((baseValue == nullptr) || (baseMeta == nullptr) || !baseValue->isTable())
        {
            fail("internal", "内部状态错误：当前表不存在");
            return false;
        }
        Object *base = baseValue->asTable();

        for (std::size_t index = 0; index + 1u < segments.size(); ++index)
        {
            const std::string &key = segments[index];
            MetaNode *child = findChild(baseMeta, key);
            if (child == nullptr)
            {
                MetaNode fresh;
                fresh.kind = MetaKind::Table;
                fresh.dottedTable = true;
                baseMeta->children.emplace_back(key, std::move(fresh));
                child = findChild(baseMeta, key);
                base->insert(key, Value(Object{}));
            }
            else
            {
                if (child->inlineTable)
                {
                    fail("inline-table", "内联表是封闭的，不能再加点分键");
                    return false;
                }
                if (child->kind == MetaKind::Value)
                {
                    fail("key-conflict", "键 '" + key + "' 已经是值，不能当表用");
                    return false;
                }
                if (child->kind == MetaKind::ArrayOfTables)
                {
                    fail("key-conflict", "键 '" + key + "' 是表数组，点分键不能往里加");
                    return false;
                }
            }
            Value *member = base->tryGet(key);
            if ((member == nullptr) || !member->isTable())
            {
                fail("key-conflict", "键 '" + key + "' 不是表");
                return false;
            }
            base = member->asTable();
            baseMeta = child;
        }

        const bool valueIsTable = value.isTable();
        const std::string &key = segments.back();
        MetaNode *child = findChild(baseMeta, key);
        if (child != nullptr)
        {
            if (child->kind != MetaKind::Value)
            {
                fail("key-conflict", "键 '" + key + "' 已经被表占用");
                return false;
            }
            if (!options_.allowDuplicateKeys)
            {
                fail("duplicate-key", "重复的键 '" + key + "'");
                return false;
            }
            child->inlineTable = valueIsTable;
        }
        else
        {
            MetaNode fresh;
            fresh.kind = MetaKind::Value;
            fresh.inlineTable = valueIsTable;
            baseMeta->children.emplace_back(key, std::move(fresh));
        }
        base->insert(key, std::move(value));
        return true;
    }

    // ---- 顶层语句 ---------------------------------------------------------

    /**
     * @brief 解析 `[table]` / `[[array]]` 表头。
     * @return 成功返回 true。
     */
    bool parseTableHeader()
    {
        bool isArray = false;
        if ((peek() == '[') && (peek(1) == '[')) { isArray = true; get(); get(); }
        else if (peek() == '[') { get(); }
        else { fail("expected-bracket", "这里需要一个表头"); return false; }

        skipHorizontal();
        std::vector<std::string> segments;
        if (!parseKeyPath(segments)) return false;
        skipHorizontal();

        if (isArray)
        {
            if ((peek() != ']') || (peek(1) != ']'))
            {
                fail("expected-bracket", "表数组头部要以 ']]' 结束");
                return false;
            }
            get(); get();
        }
        else
        {
            if (peek() != ']')
            {
                fail("expected-bracket", "表头部要以 ']' 结束");
                return false;
            }
            get();
        }
        return applyHeader(segments, isArray);
    }

    /**
     * @brief 解析 `键 = 值`。
     * @return 成功返回 true。
     */
    bool parseKeyValue()
    {
        std::vector<std::string> segments;
        if (!parseKeyPath(segments)) return false;
        skipHorizontal();
        if (peek() != '=') { fail("expected-equals", "键后面要跟 '='"); return false; }
        get();
        skipHorizontal();

        Value value;
        if (!parseValue(value)) return false;
        return applyKeyValue(segments, std::move(value));
    }

    std::string_view text_;      ///< 输入文本。
    ParseOptions options_;       ///< 选项。
    std::size_t index_ = 0u;     ///< 当前字节偏移。
    std::size_t line_ = 1u;      ///< 当前行号。
    std::size_t column_ = 1u;    ///< 当前列号。
    Diagnostic diagnostic_;      ///< 第一条诊断。

    Value root_{};               ///< 顶层表。
    MetaNode rootMeta_{};        ///< 顶层表的定义信息。
    std::vector<Step> current_{}; ///< 当前表在值树里的路径。
};

} // namespace

// ---------------------------------------------------------------------------
//  DateTime
// ---------------------------------------------------------------------------

bool operator==(const DateTime &left, const DateTime &right) noexcept
{
    if (left.kind != right.kind) return false;
    if ((left.kind == DateTimeKind::LocalDate) || (left.kind == DateTimeKind::OffsetDateTime) ||
        (left.kind == DateTimeKind::LocalDateTime))
    {
        if ((left.year != right.year) || (left.month != right.month) || (left.day != right.day)) return false;
    }
    if ((left.kind == DateTimeKind::LocalTime) || (left.kind == DateTimeKind::OffsetDateTime) ||
        (left.kind == DateTimeKind::LocalDateTime))
    {
        if ((left.hour != right.hour) || (left.minute != right.minute) || (left.second != right.second) ||
            (left.nanosecond != right.nanosecond))
        {
            return false;
        }
    }
    if (left.kind == DateTimeKind::OffsetDateTime)
    {
        return left.offsetMinutes == right.offsetMinutes;
    }
    return true;
}

std::string DateTime::toString() const
{
    char buffer[64];
    std::string out;

    if ((kind == DateTimeKind::LocalDate) || (kind == DateTimeKind::LocalDateTime) ||
        (kind == DateTimeKind::OffsetDateTime))
    {
        std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d", year, month, day);
        out += buffer;
    }
    if (kind == DateTimeKind::LocalTime)
    {
        std::snprintf(buffer, sizeof(buffer), "%02d:%02d:%02d", hour, minute, second);
        out += buffer;
    }
    else if ((kind == DateTimeKind::LocalDateTime) || (kind == DateTimeKind::OffsetDateTime))
    {
        std::snprintf(buffer, sizeof(buffer), "T%02d:%02d:%02d", hour, minute, second);
        out += buffer;
    }
    out += formatFraction(nanosecond);

    if (kind == DateTimeKind::OffsetDateTime)
    {
        if (offsetMinutes == 0)
        {
            out += 'Z';
        }
        else
        {
            const int total = (offsetMinutes < 0) ? -offsetMinutes : offsetMinutes;
            std::snprintf(buffer, sizeof(buffer), "%c%02d:%02d", (offsetMinutes < 0) ? '-' : '+',
                          total / 60, total % 60);
            out += buffer;
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
//  Object
// ---------------------------------------------------------------------------

Object::iterator Object::begin() noexcept { return members_.begin(); }

Object::iterator Object::end() noexcept { return members_.end(); }

Object::const_iterator Object::begin() const noexcept { return members_.begin(); }

Object::const_iterator Object::end() const noexcept { return members_.end(); }

Object::const_iterator Object::cbegin() const noexcept { return members_.cbegin(); }

Object::const_iterator Object::cend() const noexcept { return members_.cend(); }

bool Object::empty() const noexcept { return members_.empty(); }

Object::size_type Object::size() const noexcept { return members_.size(); }

void Object::clear() noexcept { members_.clear(); }

const Object::container_type &Object::members() const noexcept { return members_; }

Object::iterator Object::find(std::string_view key) noexcept
{
    for (iterator it = members_.begin(); it != members_.end(); ++it)
    {
        if (it->key == key) return it;
    }
    return members_.end();
}

Object::const_iterator Object::find(std::string_view key) const noexcept
{
    for (const_iterator it = members_.begin(); it != members_.end(); ++it)
    {
        if (it->key == key) return it;
    }
    return members_.end();
}

bool Object::contains(std::string_view key) const noexcept { return find(key) != members_.end(); }

Object::size_type Object::count(std::string_view key) const noexcept
{
    return contains(key) ? 1u : 0u;
}

Value &Object::operator[](std::string_view key)
{
    const iterator found = find(key);
    if (found != members_.end()) return found->value;
    members_.push_back(Member{std::string(key), Value()});
    return members_.back().value;
}

const Value *Object::tryGet(std::string_view key) const noexcept
{
    const const_iterator found = find(key);
    return (found == members_.end()) ? nullptr : &found->value;
}

Value *Object::tryGet(std::string_view key) noexcept
{
    const iterator found = find(key);
    return (found == members_.end()) ? nullptr : &found->value;
}

const Value &Object::at(std::string_view key) const
{
    const const_iterator found = find(key);
    if (found == members_.end()) throw std::out_of_range("TOML 表里没有这个键：" + std::string(key));
    return found->value;
}

Member &Object::insert(std::string key, Value value)
{
    const iterator found = find(key);
    if (found != members_.end())
    {
        found->value = std::move(value);
        return *found;
    }
    members_.push_back(Member{std::move(key), std::move(value)});
    return members_.back();
}

bool Object::erase(std::string_view key) noexcept
{
    const iterator found = find(key);
    if (found == members_.end()) return false;
    members_.erase(found);
    return true;
}

// ---------------------------------------------------------------------------
//  Value：构造函数与取值
// ---------------------------------------------------------------------------

Value Value::array(std::initializer_list<Value> items)
{
    return Value(Array(items.begin(), items.end()));
}

Value Value::table(std::initializer_list<std::pair<std::string, Value>> items)
{
    Object object;
    for (const std::pair<std::string, Value> &item : items)
    {
        object.insert(item.first, item.second);
    }
    return Value(std::move(object));
}

const char *Value::typeName() const noexcept
{
    if (isBool()) return "bool";
    if (isInt()) return "int";
    if (isDouble()) return "double";
    if (isString()) return "string";
    if (isDateTime()) return "datetime";
    if (isArray()) return "array";
    return "table";
}

std::optional<bool> Value::asBool() const noexcept
{
    const bool *value = std::get_if<bool>(&storage_);
    return (value == nullptr) ? std::nullopt : std::optional<bool>(*value);
}

std::optional<std::int64_t> Value::asInt() const noexcept
{
    if (const std::int64_t *value = std::get_if<std::int64_t>(&storage_)) return *value;
    if (const double *value = std::get_if<double>(&storage_))
    {
        if (std::isfinite(*value) && (std::floor(*value) == *value) &&
            (*value >= -9.2233720368547758e18) && (*value < 9.2233720368547758e18))
        {
            return static_cast<std::int64_t>(*value);
        }
    }
    return std::nullopt;
}

std::optional<double> Value::asDouble() const noexcept
{
    if (const double *value = std::get_if<double>(&storage_)) return *value;
    if (const std::int64_t *value = std::get_if<std::int64_t>(&storage_))
    {
        return static_cast<double>(*value);
    }
    return std::nullopt;
}

std::optional<std::string_view> Value::asString() const noexcept
{
    const std::string *value = std::get_if<std::string>(&storage_);
    return (value == nullptr) ? std::nullopt : std::optional<std::string_view>(*value);
}

std::optional<DateTime> Value::asDateTime() const noexcept
{
    const DateTime *value = std::get_if<DateTime>(&storage_);
    return (value == nullptr) ? std::nullopt : std::optional<DateTime>(*value);
}

std::string_view Value::stringOr(std::string_view fallback) const noexcept
{
    const std::optional<std::string_view> value = asString();
    return value.has_value() ? *value : fallback;
}

std::int64_t Value::intOr(std::int64_t fallback) const noexcept
{
    const std::optional<std::int64_t> value = asInt();
    return value.has_value() ? *value : fallback;
}

double Value::doubleOr(double fallback) const noexcept
{
    const std::optional<double> value = asDouble();
    return value.has_value() ? *value : fallback;
}

Array *Value::asArray() noexcept { return std::get_if<Array>(&storage_); }
const Array *Value::asArray() const noexcept { return std::get_if<Array>(&storage_); }
Object *Value::asTable() noexcept { return std::get_if<Object>(&storage_); }
const Object *Value::asTable() const noexcept { return std::get_if<Object>(&storage_); }
Object *Value::asObject() noexcept { return asTable(); }
const Object *Value::asObject() const noexcept { return asTable(); }

Array &Value::requireArray()
{
    Array *items = asArray();
    if (items == nullptr) throw std::domain_error("这个 TOML 值不是数组");
    return *items;
}

const Array &Value::requireArray() const
{
    const Array *items = asArray();
    if (items == nullptr) throw std::domain_error("这个 TOML 值不是数组");
    return *items;
}

Object &Value::requireTable()
{
    Object *table = asTable();
    if (table == nullptr) throw std::domain_error("这个 TOML 值不是表");
    return *table;
}

const Object &Value::requireTable() const
{
    const Object *table = asTable();
    if (table == nullptr) throw std::domain_error("这个 TOML 值不是表");
    return *table;
}

Value &Value::at(std::size_t index)
{
    Array &items = requireArray();
    if (index >= items.size()) throw std::out_of_range("TOML 数组下标越界");
    return items[index];
}

const Value &Value::at(std::size_t index) const
{
    const Array &items = requireArray();
    if (index >= items.size()) throw std::out_of_range("TOML 数组下标越界");
    return items[index];
}

Value &Value::operator[](std::string_view key) { return requireTable()[key]; }

const Value *Value::find(std::string_view key) const noexcept
{
    const Object *table = asTable();
    return (table == nullptr) ? nullptr : table->tryGet(key);
}

Value *Value::find(std::string_view key) noexcept
{
    Object *table = asTable();
    return (table == nullptr) ? nullptr : table->tryGet(key);
}

Value &Value::push_back(Value value)
{
    Array &items = requireArray();
    items.push_back(std::move(value));
    return items.back();
}

std::size_t Value::size() const noexcept
{
    if (const Array *items = asArray()) return items->size();
    if (const Object *table = asTable()) return table->size();
    if (const std::string *text = std::get_if<std::string>(&storage_)) return text->size();
    return 0u;
}

bool Value::empty() const noexcept
{
    if (const Array *items = asArray()) return items->empty();
    if (const Object *table = asTable()) return table->empty();
    if (const std::string *text = std::get_if<std::string>(&storage_)) return text->empty();
    return false;
}

void Value::clear() noexcept
{
    if (Array *items = asArray()) { items->clear(); return; }
    if (Object *table = asTable()) { table->clear(); return; }
    storage_ = Object{};
}

const Value *Value::atPath(std::string_view path) const noexcept
{
    const Value *current = this;
    std::size_t index = 0u;

    while ((index < path.size()) && (current != nullptr))
    {
        if (path[index] == '[')
        {
            ++index;
            std::size_t number = 0u;
            bool digits = false;
            while ((index < path.size()) && (path[index] >= '0') && (path[index] <= '9'))
            {
                number = number * 10u + static_cast<std::size_t>(path[index] - '0');
                ++index;
                digits = true;
            }
            if ((index >= path.size()) || (path[index] != ']') || !digits) return nullptr;
            ++index;
            const Array *items = current->asArray();
            current = ((items == nullptr) || (number >= items->size())) ? nullptr : &items->at(number);
            // `[0].name` 这种写法：下标之后允许再跟一个 `.` 段。
            if ((current != nullptr) && (index < path.size()) && (path[index] == '.'))
            {
                ++index;
                if ((index >= path.size()) || (path[index] == '.')) return nullptr;
            }
            continue;
        }

        if (path[index] == '.') return nullptr;

        const std::size_t start = index;
        while ((index < path.size()) && (path[index] != '.') && (path[index] != '[')) ++index;
        const std::string_view key = path.substr(start, index - start);
        if (key.empty()) return nullptr;
        current = current->find(key);

        if ((current != nullptr) && (index < path.size()) && (path[index] == '.'))
        {
            ++index;
            if ((index >= path.size()) || (path[index] == '.')) return nullptr;
        }
    }

    return current;
}

// ---------------------------------------------------------------------------
//  Value：迭代
// ---------------------------------------------------------------------------

Value::iterator::reference Value::iterator::operator*() const
{
    cache_.value = nullptr;
    cache_.key = std::string_view();
    cache_.index = kNoIndex;

    if (owner_ != nullptr)
    {
        if (Array *items = owner_->asArray())
        {
            cache_.value = &(*items)[position_];
            cache_.index = position_;
        }
        else if (Object *table = owner_->asTable())
        {
            Member &member = table->members_.at(position_);
            cache_.value = &member.value;
            cache_.key = member.key;
        }
    }
    return cache_;
}

Value::iterator &Value::iterator::operator++()
{
    ++position_;
    return *this;
}

Value::iterator Value::begin() noexcept
{
    iterator it;
    it.owner_ = this;
    it.position_ = 0u;
    return it;
}

Value::iterator Value::end() noexcept
{
    iterator it;
    it.owner_ = this;
    it.position_ = size();
    return it;
}

std::string Value::dump() const
{
    std::string out;
    if (const Object *table = asTable())
    {
        dumpTableInto(out, *table, std::string());
    }
    else
    {
        dumpInlineInto(out, *this);
    }
    return out;
}

std::string Value::inlineDump() const
{
    std::string out;
    dumpInlineInto(out, *this);
    return out;
}

bool operator==(const Value &left, const Value &right) noexcept { return equalsValue(left, right); }

std::ostream &operator<<(std::ostream &stream, const Value &value)
{
    return stream << value.inlineDump();
}

// ---------------------------------------------------------------------------
//  解析入口与诊断
// ---------------------------------------------------------------------------

std::string Diagnostic::toString() const
{
    std::string text;
    if (!path.empty())
    {
        text = path + ": ";
    }
    else
    {
        text = std::to_string(line) + ":" + std::to_string(column) + ": ";
    }
    text += message;
    if (!code.empty()) text += " [" + code + "]";
    return text;
}

ParseResult parse(std::string_view text, ParseOptions options)
{
    Parser parser(text, options);
    return parser.run();
}

} // namespace toml
