/**
 * @file json.cpp
 * @brief json.hpp 的实现：解析、序列化、对象/值操作、路径查询、迭代器。
 */

#include "json.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <stdexcept>

namespace json
{
namespace
{

/** 数组元素在 `Reference::index` 里的"没有下标"取值。 */
constexpr std::size_t kNoIndex = static_cast<std::size_t>(-1);

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
 * @brief 把浮点写成文本。
 *
 * 先按 17 位有效数字打印，再逐级缩短到能"往返"（再解析回来还是同一个值）为止，
 * 这样既短又精确。整数值的浮点补一个 `.0`，保证再解析回来还是浮点。
 * JSON 没有 NaN / Infinity，遇到就写 `null`。
 *
 * 注意不用 `std::to_chars(double)`：GCC 11 的 libstdc++ 还没实现浮点重载，
 * `snprintf` + `strtod` 反而是各平台都稳的写法。
 *
 * @param value 浮点。
 * @return 文本。
 */
std::string formatDouble(double value)
{
    if (std::isnan(value) || std::isinf(value)) return "null";

    char buffer[64];
    std::string text;
    for (int precision = 17; precision >= 1; --precision)
    {
        std::snprintf(buffer, sizeof(buffer), "%.*g", precision, value);
        text = buffer;
        double reparsed = 0.0;
        char *end = nullptr;
        reparsed = std::strtod(text.c_str(), &end);
        if ((end != nullptr) && (*end == '\0') && (reparsed == value)) break;
    }
    if (text.find_first_of(".eE") == std::string::npos) text += ".0";
    return text;
}

/**
 * @brief 把字符串按 JSON 转义规则写进输出串。
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
 * @brief 追加缩进。
 * @param out 输出串。
 * @param indent 每层空格数。
 * @param depth 当前层数。
 */
void appendIndent(std::string &out, int indent, int depth)
{
    if (indent < 0) return;
    out += '\n';
    out.append(static_cast<std::size_t>(indent) * static_cast<std::size_t>(depth), ' ');
}

/**
 * @brief 递归写出一个值。
 * @param out 输出串。
 * @param value 值。
 * @param indent 每层空格数（< 0 单行紧凑）。
 * @param depth 当前层数。
 */
void dumpInto(std::string &out, const Value &value, int indent, int depth)
{
    // 1. 标量直接写。
    if (value.isNull()) { out += "null"; return; }
    if (value.isBool()) { out += (value.asBool().value() ? "true" : "false"); return; }
    if (value.isInt()) { out += formatInt(value.asInt().value()); return; }
    if (value.isDouble()) { out += formatDouble(value.asDouble().value()); return; }
    if (value.isString()) { escapeInto(out, value.asString().value()); return; }

    // 2. 数组。
    if (const Array *items = value.asArray())
    {
        if (items->empty()) { out += "[]"; return; }
        out += '[';
        for (std::size_t index = 0; index < items->size(); ++index)
        {
            if (index != 0u) out += ',';
            if (indent >= 0) appendIndent(out, indent, depth + 1);
            dumpInto(out, (*items)[index], indent, depth + 1);
        }
        if (indent >= 0) appendIndent(out, indent, depth);
        out += ']';
        return;
    }

    // 3. 对象。
    const Object *object = value.asObject();
    if (object->empty()) { out += "{}"; return; }
    out += '{';
    bool first = true;
    for (const Member &member : *object)
    {
        if (!first) out += ',';
        first = false;
        if (indent >= 0) appendIndent(out, indent, depth + 1);
        escapeInto(out, member.key);
        out += (indent >= 0) ? ": " : ":";
        dumpInto(out, member.value, indent, depth + 1);
    }
    if (indent >= 0) appendIndent(out, indent, depth);
    out += '}';
}

/**
 * @brief 比较两个值（数字跨 int / double 按数值比）。
 * @param left 左。
 * @param right 右。
 * @return 相等返回 true。
 */
bool equalsValue(const Value &left, const Value &right)
{
    // 1. 数字：统一按 double 比（1 与 1.0 相等）。
    if (left.isNumber() && right.isNumber())
    {
        return left.asDouble().value() == right.asDouble().value();
    }

    // 2. 类型不同就不等。
    if (left.storage().index() != right.storage().index()) return false;

    // 3. 逐个类型比。
    if (const Array *items = left.asArray())
    {
        const Array *other = right.asArray();
        return items->size() == other->size() &&
               std::equal(items->begin(), items->end(), other->begin(),
                          [](const Value &a, const Value &b) { return equalsValue(a, b); });
    }
    if (const Object *object = left.asObject())
    {
        // 对象按"成员集合"比：顺序不影响相等。
        const Object *other = right.asObject();
        if (object->size() != other->size()) return false;
        for (const Member &member : *object)
        {
            const Value *peer = other->tryGet(member.key);
            if ((peer == nullptr) || !equalsValue(member.value, *peer)) return false;
        }
        return true;
    }
    if (left.isString()) return left.asString().value() == right.asString().value();
    if (left.isBool()) return left.asBool().value() == right.asBool().value();
    return true; // 都是 null
}

/**
 * @brief JSON 解析器（递归下降）。
 *
 * 遇到第一个错误就停下，并把行列信息记进诊断——这样错误信息最贴近人的直觉。
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
        // 跳过 UTF-8 BOM。
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
        Value value;
        skipSpace();
        if (!parseValue(value))
        {
            result.diagnostics.push_back(diagnostic_);
            return result;
        }
        skipSpace();
        if (!atEnd())
        {
            fail("unexpected-token", "JSON 值后面还有多余内容");
            result.diagnostics.push_back(diagnostic_);
            return result;
        }
        result.value = std::move(value);
        return result;
    }

private:
    /**
     * @brief 是否到末尾。
     * @return 到末尾返回 true。
     */
    bool atEnd() const noexcept { return index_ >= text_.size(); }

    /**
     * @brief 当前字符（不前进）。
     * @return 字符；到末尾返回 '\0'。
     */
    char peek() const noexcept { return atEnd() ? '\0' : text_[index_]; }

    /**
     * @brief 取一个字符并推进位置。
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
     * @brief 记一条诊断。
     * @param code 短码。
     * @param message 说明。
     */
    void fail(std::string code, std::string message)
    {
        if (!diagnostic_.message.empty()) return; // 只留第一条
        diagnostic_.line = line_;
        diagnostic_.column = column_;
        diagnostic_.offset = index_;
        diagnostic_.code = std::move(code);
        diagnostic_.message = std::move(message);
    }

    /**
     * @brief 跳过空白与注释。
     */
    void skipSpace()
    {
        while (!atEnd())
        {
            const char ch = peek();
            if ((ch == ' ') || (ch == '\t') || (ch == '\r') || (ch == '\n'))
            {
                get();
                continue;
            }
            if (options_.allowComments && (ch == '/') && (index_ + 1u < text_.size()))
            {
                const char next = text_[index_ + 1u];
                // 1. 行注释。
                if (next == '/')
                {
                    while (!atEnd() && (peek() != '\n')) get();
                    continue;
                }
                // 2. 块注释。
                if (next == '*')
                {
                    get();
                    get();
                    while (!atEnd() && !((peek() == '*') && (index_ + 1u < text_.size()) &&
                                         (text_[index_ + 1u] == '/')))
                    {
                        get();
                    }
                    if (atEnd())
                    {
                        fail("unterminated-comment", "块注释没有闭合");
                        return;
                    }
                    get();
                    get();
                    continue;
                }
            }
            break;
        }
    }

    /**
     * @brief 解析一个值。
     * @param out 结果。
     * @return 成功返回 true。
     */
    bool parseValue(Value &out)
    {
        skipSpace();
        if (atEnd())
        {
            fail("unexpected-end", "内容在值之前就结束了");
            return false;
        }

        switch (peek())
        {
        case '{': return parseObject(out);
        case '[': return parseArray(out);
        case '"': return parseString(out);
        case 't':
        case 'f':
        case 'n': return parseLiteral(out);
        default: return parseNumber(out);
        }
    }

    /**
     * @brief 解析对象。
     * @param out 结果。
     * @return 成功返回 true。
     */
    bool parseObject(Value &out)
    {
        get(); // '{'
        Object object;
        skipSpace();
        if (peek() == '}')
        {
            get();
            out = Value(std::move(object));
            return true;
        }

        while (true)
        {
            skipSpace();
            if (peek() != '"')
            {
                fail("expected-key", "对象的成员名必须是字符串");
                return false;
            }
            Value key;
            if (!parseString(key)) return false;

            skipSpace();
            if (peek() != ':')
            {
                fail("expected-colon", "成员名后面要跟冒号");
                return false;
            }
            get();

            Value member;
            if (!parseValue(member)) return false;
            object.insert(std::string(key.asString().value()), std::move(member));

            skipSpace();
            if (peek() == ',')
            {
                get();
                skipSpace();
                if (peek() == '}')
                {
                    if (!options_.allowTrailingCommas)
                    {
                        fail("trailing-comma", "对象末尾多了一个逗号（可用 --trailing-commas 允许）");
                        return false;
                    }
                    get();
                    break;
                }
                continue;
            }
            if (peek() == '}')
            {
                get();
                break;
            }
            fail("expected-comma", "对象成员之间要有逗号，结尾要有 '}'");
            return false;
        }

        out = Value(std::move(object));
        return true;
    }

    /**
     * @brief 解析数组。
     * @param out 结果。
     * @return 成功返回 true。
     */
    bool parseArray(Value &out)
    {
        get(); // '['
        Array items;
        skipSpace();
        if (peek() == ']')
        {
            get();
            out = Value(std::move(items));
            return true;
        }

        while (true)
        {
            Value item;
            if (!parseValue(item)) return false;
            items.push_back(std::move(item));

            skipSpace();
            if (peek() == ',')
            {
                get();
                skipSpace();
                if (peek() == ']')
                {
                    if (!options_.allowTrailingCommas)
                    {
                        fail("trailing-comma", "数组末尾多了一个逗号（可用 --trailing-commas 允许）");
                        return false;
                    }
                    get();
                    break;
                }
                continue;
            }
            if (peek() == ']')
            {
                get();
                break;
            }
            fail("expected-comma", "数组元素之间要有逗号，结尾要有 ']'");
            return false;
        }

        out = Value(std::move(items));
        return true;
    }

    /**
     * @brief 解析字符串（含 `\uXXXX` 与代理对）。
     * @param out 结果。
     * @return 成功返回 true。
     */
    bool parseString(Value &out)
    {
        get(); // '"'
        std::string text;
        while (!atEnd())
        {
            const char ch = get();
            if (ch == '"')
            {
                out = Value(std::move(text));
                return true;
            }
            if (static_cast<unsigned char>(ch) < 0x20u)
            {
                fail("control-char", "字符串里出现了未转义的控制字符");
                return false;
            }
            if (ch != '\\')
            {
                text += ch;
                continue;
            }

            // 转义序列。
            if (atEnd())
            {
                fail("bad-escape", "转义序列不完整");
                return false;
            }
            const char escape = get();
            switch (escape)
            {
            case '"': text += '"'; break;
            case '\\': text += '\\'; break;
            case '/': text += '/'; break;
            case 'b': text += '\b'; break;
            case 'f': text += '\f'; break;
            case 'n': text += '\n'; break;
            case 'r': text += '\r'; break;
            case 't': text += '\t'; break;
            case 'u':
            {
                std::uint32_t code = 0;
                if (!parseHex4(code)) return false;
                // 代理对：高位后面必须跟 \uDC00..\uDFFF。
                if ((code >= 0xD800u) && (code <= 0xDBFFu))
                {
                    if (atEnd() || (peek() != '\\'))
                    {
                        fail("bad-surrogate", "高位代理后面缺少 \\uXXXX 低位代理");
                        return false;
                    }
                    get();
                    if (atEnd() || (get() != 'u'))
                    {
                        fail("bad-surrogate", "高位代理后面缺少 \\uXXXX 低位代理");
                        return false;
                    }
                    std::uint32_t low = 0;
                    if (!parseHex4(low)) return false;
                    if ((low < 0xDC00u) || (low > 0xDFFFu))
                    {
                        fail("bad-surrogate", "低位代理不在 DC00..DFFF 范围内");
                        return false;
                    }
                    code = 0x10000u + ((code - 0xD800u) << 10u) + (low - 0xDC00u);
                }
                appendUtf8(text, code);
                break;
            }
            default:
                fail("bad-escape", std::string("不认识的转义：\\") + escape);
                return false;
            }
        }

        fail("unterminated-string", "字符串没有闭合");
        return false;
    }

    /**
     * @brief 读 4 位十六进制。
     * @param out 结果。
     * @return 成功返回 true。
     */
    bool parseHex4(std::uint32_t &out)
    {
        std::uint32_t value = 0;
        for (int digit = 0; digit < 4; ++digit)
        {
            if (atEnd())
            {
                fail("bad-escape", "\\u 后面要跟 4 位十六进制");
                return false;
            }
            const char ch = get();
            value <<= 4u;
            if ((ch >= '0') && (ch <= '9')) value |= static_cast<std::uint32_t>(ch - '0');
            else if ((ch >= 'a') && (ch <= 'f')) value |= static_cast<std::uint32_t>(ch - 'a' + 10);
            else if ((ch >= 'A') && (ch <= 'F')) value |= static_cast<std::uint32_t>(ch - 'A' + 10);
            else
            {
                fail("bad-escape", "\\u 后面要跟 4 位十六进制");
                return false;
            }
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
     * @brief 解析数字（整数优先，放不下才当浮点）。
     * @param out 结果。
     * @return 成功返回 true。
     */
    bool parseNumber(Value &out)
    {
        const std::size_t start = index_;
        bool isDouble = false;

        // 1. 符号。
        if (peek() == '-') get();

        // 2. 整数部分（JSON 不允许前导 0）。
        if (peek() == '0')
        {
            get();
            if ((peek() >= '0') && (peek() <= '9'))
            {
                fail("bad-number", "数字不允许前导 0");
                return false;
            }
        }
        else if ((peek() >= '1') && (peek() <= '9'))
        {
            while ((peek() >= '0') && (peek() <= '9')) get();
        }
        else
        {
            fail("unexpected-token", std::string("不认识的字符 '") + peek() + "'");
            return false;
        }

        // 3. 小数部分。
        if (peek() == '.')
        {
            isDouble = true;
            get();
            if ((peek() < '0') || (peek() > '9'))
            {
                fail("bad-number", "小数点后面要有数字");
                return false;
            }
            while ((peek() >= '0') && (peek() <= '9')) get();
        }

        // 4. 指数部分。
        if ((peek() == 'e') || (peek() == 'E'))
        {
            isDouble = true;
            get();
            if ((peek() == '+') || (peek() == '-')) get();
            if ((peek() < '0') || (peek() > '9'))
            {
                fail("bad-number", "指数后面要有数字");
                return false;
            }
            while ((peek() >= '0') && (peek() <= '9')) get();
        }

        const std::string_view token = text_.substr(start, index_ - start);

        // 5. 整数优先：放不下就当浮点。
        if (!isDouble)
        {
            std::int64_t integer = 0;
            const std::from_chars_result result =
                std::from_chars(token.data(), token.data() + token.size(), integer);
            if ((result.ec == std::errc()) && (result.ptr == token.data() + token.size()))
            {
                out = Value(integer);
                return true;
            }
        }

        double number = 0.0;
        // 浮点用 strtod：同样是"libstdc++ 还没有浮点 from_chars"的兼容写法。
        const std::string text(token);
        char *end = nullptr;
        number = std::strtod(text.c_str(), &end);
        if ((end == nullptr) || (end != text.c_str() + text.size()))
        {
            fail("bad-number", "数字格式不对：" + std::string(token));
            return false;
        }
        out = Value(number);
        return true;
    }

    /**
     * @brief 解析 `true` / `false` / `null`。
     * @param out 结果。
     * @return 成功返回 true。
     */
    bool parseLiteral(Value &out)
    {
        if (text_.compare(index_, 4u, "true") == 0)
        {
            for (int step = 0; step < 4; ++step) get();
            out = Value(true);
            return true;
        }
        if (text_.compare(index_, 5u, "false") == 0)
        {
            for (int step = 0; step < 5; ++step) get();
            out = Value(false);
            return true;
        }
        if (text_.compare(index_, 4u, "null") == 0)
        {
            for (int step = 0; step < 4; ++step) get();
            out = Value(nullptr);
            return true;
        }
        fail("unexpected-token", "这里只认 true / false / null");
        return false;
    }

    std::string_view text_;     ///< 输入文本。
    ParseOptions options_;      ///< 选项。
    std::size_t index_ = 0;     ///< 当前字节偏移。
    std::size_t line_ = 1;      ///< 当前行号。
    std::size_t column_ = 1;    ///< 当前列号。
    Diagnostic diagnostic_;     ///< 第一条诊断。
};

} // namespace

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

bool Object::contains(std::string_view key) const noexcept
{
    return find(key) != members_.end();
}

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

Value Value::object(std::initializer_list<std::pair<std::string, Value>> items)
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
    if (isNull()) return "null";
    if (isBool()) return "bool";
    if (isInt()) return "int";
    if (isDouble()) return "double";
    if (isString()) return "string";
    if (isArray()) return "array";
    return "object";
}

std::optional<bool> Value::asBool() const noexcept
{
    const bool *value = std::get_if<bool>(&storage_);
    return value == nullptr ? std::nullopt : std::optional<bool>(*value);
}

std::optional<std::int64_t> Value::asInt() const noexcept
{
    if (const std::int64_t *value = std::get_if<std::int64_t>(&storage_))
    {
        return *value;
    }
    // 浮点但正好是整数，也允许取（`3.0` → 3）。
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
    return value == nullptr ? std::nullopt : std::optional<std::string_view>(*value);
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
Object *Value::asObject() noexcept { return std::get_if<Object>(&storage_); }
const Object *Value::asObject() const noexcept { return std::get_if<Object>(&storage_); }

Array &Value::requireArray()
{
    Array *items = asArray();
    if (items == nullptr) throw std::domain_error("当前 JSON 值不是数组");
    return *items;
}

const Array &Value::requireArray() const
{
    const Array *items = asArray();
    if (items == nullptr) throw std::domain_error("当前 JSON 值不是数组");
    return *items;
}

Object &Value::requireObject()
{
    Object *object = asObject();
    if (object == nullptr) throw std::domain_error("当前 JSON 值不是对象");
    return *object;
}

const Object &Value::requireObject() const
{
    const Object *object = asObject();
    if (object == nullptr) throw std::domain_error("当前 JSON 值不是对象");
    return *object;
}

// ---------------------------------------------------------------------------
//  Value：访问与修改
// ---------------------------------------------------------------------------

Value &Value::at(std::size_t index)
{
    return requireArray().at(index);
}

const Value &Value::at(std::size_t index) const
{
    return requireArray().at(index);
}

Value &Value::operator[](std::string_view key)
{
    return requireObject()[key];
}

const Value *Value::find(std::string_view key) const noexcept
{
    const Object *object = asObject();
    return object == nullptr ? nullptr : object->tryGet(key);
}

Value *Value::find(std::string_view key) noexcept
{
    Object *object = asObject();
    return object == nullptr ? nullptr : object->tryGet(key);
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
    if (const Object *object = asObject()) return object->size();
    if (const std::string *text = std::get_if<std::string>(&storage_)) return text->size();
    return 0u;
}

bool Value::empty() const noexcept
{
    if (const Array *items = asArray()) return items->empty();
    if (const Object *object = asObject()) return object->empty();
    if (const std::string *text = std::get_if<std::string>(&storage_)) return text->empty();
    return isNull();
}

void Value::clear() noexcept
{
    if (Array *items = asArray())
    {
        items->clear();
        return;
    }
    if (Object *object = asObject())
    {
        object->clear();
        return;
    }
    if (!isNull()) storage_ = Null{};
}

const Value *Value::atPath(std::string_view path) const noexcept
{
    const Value *current = this;
    std::size_t index = 0;

    while ((index < path.size()) && (current != nullptr))
    {
        // 1. `[n]`：数组下标。
        if (path[index] == '[')
        {
            ++index;
            std::size_t number = 0;
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
            continue;
        }

        // 2. 段首出现 `.`：`a..b` / `.a` 这类空段，判为找不到。
        if (path[index] == '.') return nullptr;

        // 3. 成员名：读到下一个 `.` 或 `[`。
        const std::size_t start = index;
        while ((index < path.size()) && (path[index] != '.') && (path[index] != '[')) ++index;
        const std::string_view key = path.substr(start, index - start);
        if (key.empty()) return nullptr;
        current = current->find(key);

        // 4. 名字后面要么结束、要么跟 `[`，要么是单个 `.`（不能是空段或结尾的点）。
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
        else if (Object *object = owner_->asObject())
        {
            // Value 是 Object 的友元，可以直接用成员表（成员定义在本文件里已完整）。
            Member &member = object->members_.at(position_);
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

std::string Value::dump(int indent) const
{
    std::string out;
    out.reserve(64u);
    dumpInto(out, *this, indent, 0);
    return out;
}

bool operator==(const Value &left, const Value &right) noexcept
{
    return equalsValue(left, right);
}

std::ostream &operator<<(std::ostream &stream, const Value &value)
{
    return stream << value.dump();
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

} // namespace json
