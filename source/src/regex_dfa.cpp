/**
 * @file regex_dfa.cpp
 * @brief 简单正则到 DFA 的编译器实现。
 */

#include "ParseEngine/regex_dfa.hpp"

#include <algorithm>
#include <cstdint>
#include <deque>
#include <optional>

namespace ParseEngine
{

namespace
{

const int k_regexMaxRepeat = 32;
const int k_linearProbes = 9;
const int k_perturbShift = 5;

const char k_emptyKey = '\0';
const std::int64_t k_slotEmpty = INT64_MIN;
const std::int64_t k_slotDummy = INT64_MIN + 1;

/**
 * @brief 复刻 CPython 整数 set 的槽位布局，保证迭代顺序与 Python 完全一致。
 *
 * 生成产物里的 DFA 状态编号取决于最小化时集合的迭代顺序，而 Python 的
 * set 迭代顺序是 CPython 实现细节（哈希取模 + 线性探测 + 扰动跳转）。
 * 这里按 setobject.c 的算法重写一遍，只支持整数元素。
 */
class PyIntSet
{
public:
    /**
     * @brief 构造空集合（初始 8 槽，与 CPython 一致）。
     */
    PyIntSet()
    {
        m_table.assign(8, k_slotEmpty);
        m_mask = 7;
    }

    /**
     * @brief 插入一个元素；已存在时保持原样。
     * @param value 待插入的整数。
     */
    void add(std::int64_t value)
    {
        const std::int64_t hash = value;
        std::size_t i = static_cast<std::size_t>(hash) & m_mask;
        std::int64_t freeslot = -1;
        std::int64_t perturb = hash;
        while (true)
        {
            std::size_t slot = i;
            int probes = (i + k_linearProbes <= m_mask) ? k_linearProbes : 0;
            while (true)
            {
                const std::int64_t current = m_table[slot];
                if (current == k_slotEmpty)
                {
                    if (freeslot >= 0)
                    {
                        m_table[static_cast<std::size_t>(freeslot)] = value;
                        ++m_used;
                        return;
                    }
                    m_table[slot] = value;
                    ++m_fill;
                    ++m_used;
                    if (static_cast<std::size_t>(m_fill) * 5 <
                        m_mask * 3)
                    {
                        return;
                    }
                    resizeTable(m_used * 4);
                    return;
                }
                if (current == value)
                {
                    return;
                }
                if (current == k_slotDummy && freeslot < 0)
                {
                    freeslot = static_cast<std::int64_t>(slot);
                }
                ++slot;
                if (probes-- == 0)
                {
                    break;
                }
            }
            perturb >>= k_perturbShift;
            i = (i * 5 + 1 + static_cast<std::size_t>(perturb)) & m_mask;
        }
    }

    /**
     * @brief 查询元素是否在集合里。
     * @param value 待查询的整数。
     * @return 存在返回 true。
     */
    bool contains(std::int64_t value) const
    {
        const std::int64_t hash = value;
        std::size_t i = static_cast<std::size_t>(hash) & m_mask;
        std::int64_t perturb = hash;
        while (true)
        {
            std::size_t slot = i;
            int probes = (i + k_linearProbes <= m_mask) ? k_linearProbes : 0;
            while (true)
            {
                const std::int64_t current = m_table[slot];
                if (current == k_slotEmpty)
                {
                    return false;
                }
                if (current == value)
                {
                    return true;
                }
                ++slot;
                if (probes-- == 0)
                {
                    break;
                }
            }
            perturb >>= k_perturbShift;
            i = (i * 5 + 1 + static_cast<std::size_t>(perturb)) & m_mask;
        }
    }

    /**
     * @brief 删除元素（不存在时静默返回）。
     * @param value 待删除的整数。
     */
    void discard(std::int64_t value)
    {
        const std::int64_t hash = value;
        std::size_t i = static_cast<std::size_t>(hash) & m_mask;
        std::int64_t perturb = hash;
        while (true)
        {
            std::size_t slot = i;
            int probes = (i + k_linearProbes <= m_mask) ? k_linearProbes : 0;
            while (true)
            {
                const std::int64_t current = m_table[slot];
                if (current == k_slotEmpty)
                {
                    return;
                }
                if (current == value)
                {
                    // CPython 删除时把槽位标记成 dummy，保留探测链。
                    m_table[slot] = k_slotDummy;
                    --m_used;
                    return;
                }
                ++slot;
                if (probes-- == 0)
                {
                    break;
                }
            }
            perturb >>= k_perturbShift;
            i = (i * 5 + 1 + static_cast<std::size_t>(perturb)) & m_mask;
        }
    }

    /**
     * @brief 按槽位顺序返回所有元素（即 Python 的迭代顺序）。
     * @return 元素列表。
     */
    std::vector<std::int64_t> values() const
    {
        std::vector<std::int64_t> result;
        result.reserve(static_cast<std::size_t>(m_used));
        for (const std::int64_t slot : m_table)
        {
            if (slot != k_slotEmpty && slot != k_slotDummy)
            {
                result.push_back(slot);
            }
        }
        return result;
    }

    /**
     * @brief 返回迭代顺序下的第一个元素。
     * @return 第一个元素；空集合时返回 0。
     */
    std::int64_t front() const
    {
        for (const std::int64_t slot : m_table)
        {
            if (slot != k_slotEmpty && slot != k_slotDummy)
            {
                return slot;
            }
        }
        return 0;
    }

    /**
     * @brief 集合大小。
     * @return 元素个数。
     */
    std::int64_t size() const
    {
        return m_used;
    }

private:
    /**
     * @brief 重新分配表并把旧元素按新掩码重新插入。
     * @param minUsed CPython 传来的最小容量（used*4）。
     */
    void resizeTable(std::int64_t minUsed)
    {
        std::size_t newSize = 8;
        while (newSize <= static_cast<std::size_t>(minUsed))
        {
            newSize <<= 1;
        }
        const std::vector<std::int64_t> oldTable = m_table;
        const std::size_t oldMask = m_mask;
        m_table.assign(newSize, k_slotEmpty);
        m_mask = newSize - 1;
        m_fill = 0;
        m_used = 0;
        // 按旧表槽位顺序重新插入（set_insert_clean），保持与 CPython 一致。
        for (std::size_t index = 0; index <= oldMask; ++index)
        {
            const std::int64_t key = oldTable[index];
            if (key != k_slotEmpty && key != k_slotDummy)
            {
                insertClean(key);
            }
        }
    }

    /**
     * @brief CPython set_insert_clean：只探测空槽，不查重。
     * @param value 待插入的整数。
     */
    void insertClean(std::int64_t value)
    {
        const std::int64_t hash = value;
        std::size_t i = static_cast<std::size_t>(hash) & m_mask;
        std::int64_t perturb = hash;
        while (true)
        {
            std::size_t slot = i;
            int probes = (i + k_linearProbes <= m_mask) ? k_linearProbes : 0;
            while (true)
            {
                if (m_table[slot] == k_slotEmpty)
                {
                    m_table[slot] = value;
                    ++m_fill;
                    ++m_used;
                    return;
                }
                ++slot;
                if (probes-- == 0)
                {
                    break;
                }
            }
            perturb >>= k_perturbShift;
            i = (i * 5 + 1 + static_cast<std::size_t>(perturb)) & m_mask;
        }
    }

    std::vector<std::int64_t> m_table; ///< 槽位表（空 / dummy / 值）
    std::size_t m_mask = 7;            ///< 槽位掩码
    std::int64_t m_fill = 0;           ///< 已用槽数（含 dummy）
    std::int64_t m_used = 0;           ///< 有效元素个数
};

/**
 * @brief 所有参与匹配的字符（1..255，不含 NUL）。
 * @return 字符集合。
 */
std::set<char> allChars()
{
    std::set<char> result;
    for (int code = 1; code <= 255; ++code)
    {
        result.insert(static_cast<char>(code));
    }
    return result;
}

/**
 * @brief \d 的字符集合。
 * @return 数字字符。
 */
std::set<char> digitChars()
{
    std::set<char> result;
    for (char ch = '0'; ch <= '9'; ++ch)
    {
        result.insert(ch);
    }
    return result;
}

/**
 * @brief \w 的字符集合（按 Python str.isalnum 的 Latin-1 结果硬编码）。
 * @return 单词字符。
 */
std::set<char> wordChars()
{
    static const int k_codes[] = {
        48,  49,  50,  51,  52,  53,  54,  55,  56,  57,  65,  66,  67,  68,
        69,  70,  71,  72,  73,  74,  75,  76,  77,  78,  79,  80,  81,  82,
        83,  84,  85,  86,  87,  88,  89,  90,  95,  97,  98,  99,  100, 101,
        102, 103, 104, 105, 106, 107, 108, 109, 110, 111, 112, 113, 114, 115,
        116, 117, 118, 119, 120, 121, 122, 170, 178, 179, 181, 185, 186, 188,
        189, 190, 192, 193, 194, 195, 196, 197, 198, 199, 200, 201, 202, 203,
        204, 205, 206, 207, 208, 209, 210, 211, 212, 213, 214, 216, 217, 218,
        219, 220, 221, 222, 223, 224, 225, 226, 227, 228, 229, 230, 231, 232,
        233, 234, 235, 236, 237, 238, 239, 240, 241, 242, 243, 244, 245, 246,
        248, 249, 250, 251, 252, 253, 254, 255};
    std::set<char> result;
    for (const int code : k_codes)
    {
        result.insert(static_cast<char>(code));
    }
    return result;
}

/**
 * @brief \s 的字符集合。
 * @return 空白字符。
 */
std::set<char> spaceChars()
{
    return std::set<char>{' ', '\t', '\r', '\n', '\f', '\v'};
}

/**
 * @brief 求两个集合的差集（保留 a 中不在 b 里的元素）。
 * @param first 被减集合。
 * @param second 减数集合。
 * @return 差集。
 */
std::set<char> setDifference(const std::set<char> &first,
                             const std::set<char> &second)
{
    std::set<char> result;
    for (const char ch : first)
    {
        if (!second.count(ch))
        {
            result.insert(ch);
        }
    }
    return result;
}

/**
 * @brief \d / \w / \s 一类转义返回字符集合。
 * @param ch 反斜杠后的字符。
 * @return 对应的字符集合；不是预定义转义时返回空 optional。
 */
std::optional<std::set<char>> escapeSet(char ch)
{
    std::set<char> chars;
    const char lowered = static_cast<char>(
        std::tolower(static_cast<unsigned char>(ch)));
    if (lowered == 'd')
    {
        chars = digitChars();
    }
    else if (lowered == 'w')
    {
        chars = wordChars();
    }
    else if (lowered == 's')
    {
        chars = spaceChars();
    }
    else
    {
        return std::nullopt;
    }
    if (std::isupper(static_cast<unsigned char>(ch)))
    {
        return setDifference(allChars(), chars);
    }
    return chars;
}

/**
 * @brief 把 \ 转义还原成一个字符。
 * @param ch 反斜杠后的字符。
 * @return 还原后的字符。
 */
char escapeChar(char ch)
{
    switch (ch)
    {
    case 'n': return '\n';
    case 'r': return '\r';
    case 't': return '\t';
    case 'f': return '\f';
    case 'v': return '\v';
    case '0': return '\0';
    default: return ch;
    }
}

/**
 * @brief `.` 的字符集合：除换行以外的任意字符。
 * @return 字符集合。
 */
std::set<char> dotSet()
{
    std::set<char> result = allChars();
    result.erase('\n');
    return result;
}

/**
 * @brief 正则递归下降解析器（对应 Python 的 _RegexParser）。
 */
class RegexParserImpl
{
public:
    /**
     * @brief 绑定待解析的正则文本。
     * @param pattern 正则文本。
     */
    explicit RegexParserImpl(std::string pattern)
        : m_pattern(std::move(pattern))
    {
    }

    /**
     * @brief 解析整条正则。
     * @return AST 根节点；语法有误返回 nullptr 并置 error。
     */
    std::shared_ptr<RegexNode> parse()
    {
        if (m_pattern.empty())
        {
            fail("正则表达式不能为空");
            return {};
        }
        std::shared_ptr<RegexNode> node = parseAlt();
        if (failed())
        {
            return {};
        }
        if (m_pos != m_pattern.size())
        {
            fail("位置 " + std::to_string(m_pos) + " 处有多余字符 '" +
                 peekText() + "'");
            return {};
        }
        return node;
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

    /**
     * @brief 记录第一个错误（错误槽已满时不覆盖）。
     * @param message 错误说明。
     */
    void fail(const std::string &message)
    {
        ParseEngine::fail(m_error, message);
    }

private:
    /**
     * @brief 查看当前位置偏移处的字符。
     * @param offset 相对当前位置的偏移。
     * @return 字符编码；越界返回 -1。
     */
    int peek(std::size_t offset = 0) const
    {
        const std::size_t index = m_pos + offset;
        if (index < m_pattern.size())
        {
            return static_cast<unsigned char>(m_pattern[index]);
        }
        return -1;
    }

    /**
     * @brief 把当前字符渲染成插入错误信息用的文本。
     * @return 单字符文本；越界返回 "None"。
     */
    std::string peekText() const
    {
        const int code = peek();
        if (code < 0)
        {
            return "None";
        }
        return std::string(1, static_cast<char>(code));
    }

    /**
     * @brief 取出当前字符并前移。
     * @return 字符编码；越界返回 -1。
     */
    int next()
    {
        const int code = peek();
        if (code >= 0)
        {
            ++m_pos;
        }
        return code;
    }

    /**
     * @brief alt := concat ('|' concat)*
     * @return 解析出的节点。
     */
    std::shared_ptr<RegexNode> parseAlt()
    {
        std::shared_ptr<RegexNode> node = parseConcat();
        while (!failed() && (peek() == '|'))
        {
            next();
            const std::shared_ptr<RegexNode> right = parseConcat();
            if (failed())
            {
                return {};
            }
            node = makeBinary(RegexNode::Kind::Alt, node, right);
        }
        return node;
    }

    /**
     * @brief concat := factor*
     * @return 解析出的节点；空串返回 Empty 节点。
     */
    std::shared_ptr<RegexNode> parseConcat()
    {
        std::vector<std::shared_ptr<RegexNode>> nodes;
        while (!failed() && (peek() >= 0) && (peek() != '|') && (peek() != ')'))
        {
            const std::shared_ptr<RegexNode> factor = parseFactor();
            if (failed())
            {
                return {};
            }
            nodes.push_back(factor);
        }
        if (nodes.empty())
        {
            return makeNode(RegexNode::Kind::Empty);
        }
        std::shared_ptr<RegexNode> node = nodes[0];
        for (std::size_t index = 1; index < nodes.size(); ++index)
        {
            node = makeBinary(RegexNode::Kind::Concat, node, nodes[index]);
        }
        return node;
    }

    /**
     * @brief factor := base ('*' | '+' | '?' | '{m,n}')*
     * @return 解析出的节点。
     */
    std::shared_ptr<RegexNode> parseFactor()
    {
        std::shared_ptr<RegexNode> node = parseBase();
        while (!failed())
        {
            const int ch = peek();
            if (ch == '*')
            {
                next();
                node = makeUnary(RegexNode::Kind::Star, node);
            }
            else if (ch == '+')
            {
                next();
                node = makeUnary(RegexNode::Kind::Plus, node);
            }
            else if (ch == '?')
            {
                next();
                node = makeUnary(RegexNode::Kind::Opt, node);
            }
            else if (ch == '{')
            {
                const std::shared_ptr<RegexNode> repeated = parseRepeat(node);
                if (failed())
                {
                    return {};
                }
                node = repeated;
            }
            else
            {
                break;
            }
        }
        if (failed())
        {
            return {};
        }
        return node;
    }

    /**
     * @brief 把 {m} / {m,} / {m,n} 展开成等价的连接与可选。
     * @param node 重复的基节点。
     * @return 展开后的节点。
     */
    std::shared_ptr<RegexNode> parseRepeat(const std::shared_ptr<RegexNode> &node)
    {
        next(); // '{'
        std::string digits;
        while ((peek() >= 0) && std::isdigit(peek()))
        {
            digits += static_cast<char>(next());
        }
        const int minimum = digits.empty() ? 0 : parseRepeatCount(digits);
        int maximum = minimum;
        bool unbounded = false;
        if (peek() == ',')
        {
            next();
            digits.clear();
            while ((peek() >= 0) && std::isdigit(peek()))
            {
                digits += static_cast<char>(next());
            }
            if (digits.empty())
            {
                unbounded = true;
            }
            else
            {
                maximum = parseRepeatCount(digits);
            }
        }
        // parseRepeatCount 用 -1 表示「超出上限」：这里与下面的 limit 检查
        // 合起来给出同一条「重复次数不能超过 N」的报错。
        if ((minimum < 0) || (maximum < 0))
        {
            fail("重复次数不能超过 " + std::to_string(k_regexMaxRepeat));
            return {};
        }
        if (peek() != '}')
        {
            fail("重复次数缺少右花括号 }");
            return {};
        }
        next();
        if (!unbounded && maximum < minimum)
        {
            fail("重复次数上限小于下限");
            return {};
        }
        const int limit = unbounded ? minimum : maximum;
        if (limit > k_regexMaxRepeat)
        {
            fail("重复次数不能超过 " + std::to_string(k_regexMaxRepeat));
            return {};
        }
        std::vector<std::shared_ptr<RegexNode>> pieces;
        for (int index = 0; index < minimum; ++index)
        {
            pieces.push_back(node);
        }
        if (unbounded)
        {
            pieces.push_back(makeUnary(RegexNode::Kind::Star, node));
        }
        else
        {
            for (int index = 0; index < maximum - minimum; ++index)
            {
                pieces.push_back(makeUnary(RegexNode::Kind::Opt, node));
            }
        }
        if (pieces.empty())
        {
            fail("{0} 不匹配任何输入");
            return {};
        }
        std::shared_ptr<RegexNode> result = pieces[0];
        for (std::size_t index = 1; index < pieces.size(); ++index)
        {
            result = makeBinary(RegexNode::Kind::Concat, result, pieces[index]);
        }
        return result;
    }

    /**
     * @brief 把 {m,n} 里的十进制数字转成整数（不抛异常）。
     *
     * 超过 k_regexMaxRepeat 的数字一律当成「超上限」处理：先返回 -1，由调用
     * 方按「重复次数不能超过 N」报错，避免 std::stoi 在超大数字上抛
     * std::out_of_range。
     * @param digits 纯十进制数字串。
     * @return 解析出的数值；超过上限返回 -1。
     */
    static int parseRepeatCount(const std::string &digits)
    {
        long long value = 0;
        for (const char ch : digits)
        {
            value = value * 10 + (ch - '0');
            if (value > k_regexMaxRepeat)
            {
                return -1;
            }
        }
        return static_cast<int>(value);
    }

    /**
     * @brief base := '(' alt ')' | '[' class ']' | 转义 | '.' | 普通字符
     * @return 解析出的节点。
     */
    std::shared_ptr<RegexNode> parseBase()
    {
        const int ch = peek();
        if (ch == '(')
        {
            next();
            std::shared_ptr<RegexNode> node = parseAlt();
            if (failed())
            {
                return {};
            }
            if (peek() != ')')
            {
                fail("缺少右括号 )");
                return {};
            }
            next();
            return node;
        }
        if (ch == '[')
        {
            return parseClass();
        }
        if (ch == '\\')
        {
            next();
            const int escaped = next();
            if (escaped < 0)
            {
                fail("反斜杠后没有字符");
                return {};
            }
            const auto shorthand = escapeSet(static_cast<char>(escaped));
            if (shorthand.has_value())
            {
                return makeClass(shorthand.value());
            }
            return makeClass(std::set<char>{
                escapeChar(static_cast<char>(escaped))});
        }
        if (ch == '.')
        {
            next();
            return makeClass(dotSet());
        }
        if ((ch < 0) || (ch == '|') || (ch == ')') || (ch == '*') ||
            (ch == '+') || (ch == '?') || (ch == '{'))
        {
            fail("意外的字符 '" + peekText() + "'");
            return {};
        }
        next();
        return makeClass(std::set<char>{static_cast<char>(ch)});
    }

    /**
     * @brief class := '[' '^'? (字符 | 字符'-'字符)* ']'
     * @return 解析出的字符类节点。
     */
    std::shared_ptr<RegexNode> parseClass()
    {
        next(); // '['
        bool negate = false;
        if (peek() == '^')
        {
            next();
            negate = true;
        }
        std::set<char> chars;
        bool first = true;
        while (!failed())
        {
            const int ch = peek();
            if (ch < 0)
            {
                fail("字符集缺少右括号 ]");
                return {};
            }
            if ((ch == ']') && !first)
            {
                next();
                break;
            }
            first = false;
            char start = '\0';
            if (ch == '\\')
            {
                next();
                const int escaped = next();
                if (escaped < 0)
                {
                    fail("字符集里的反斜杠后没有字符");
                    return {};
                }
                const auto shorthand = escapeSet(static_cast<char>(escaped));
                if (shorthand.has_value())
                {
                    chars.insert(shorthand->begin(), shorthand->end());
                    continue;
                }
                start = escapeChar(static_cast<char>(escaped));
            }
            else
            {
                next();
                start = static_cast<char>(ch);
            }
            if ((peek() == '-') && (peek(1) >= 0) && (peek(1) != ']'))
            {
                next();
                int endChar = next();
                if (endChar == '\\')
                {
                    endChar = next();
                    if (endChar < 0)
                    {
                        fail("字符集区间不完整");
                        return {};
                    }
                    endChar = static_cast<unsigned char>(
                        escapeChar(static_cast<char>(endChar)));
                }
                if (static_cast<unsigned char>(start) >
                    static_cast<unsigned char>(endChar))
                {
                    fail("字符集区间 " + std::string(1, start) + "-" +
                         std::string(1, static_cast<char>(endChar)) +
                         " 顺序颠倒");
                    return {};
                }
                for (int code = static_cast<unsigned char>(start);
                     code <= static_cast<unsigned char>(endChar); ++code)
                {
                    chars.insert(static_cast<char>(code));
                }
            }
            else
            {
                chars.insert(start);
            }
        }
        if (negate)
        {
            chars = setDifference(allChars(), chars);
        }
        chars.erase(k_emptyKey);
        if (chars.empty())
        {
            fail("字符集不能为空");
            return {};
        }
        return makeClass(chars);
    }

    /**
     * @brief 构造一个带字符集合的 Class 节点。
     * @param chars 字符集合。
     * @return 新建节点。
     */
    static std::shared_ptr<RegexNode> makeClass(const std::set<char> &chars)
    {
        std::shared_ptr<RegexNode> node = makeNode(RegexNode::Kind::Class);
        node->chars = chars;
        return node;
    }

    /**
     * @brief 构造一个只有类别的节点。
     * @param kind 节点类别。
     * @return 新建节点。
     */
    static std::shared_ptr<RegexNode> makeNode(RegexNode::Kind kind)
    {
        std::shared_ptr<RegexNode> node = std::make_shared<RegexNode>();
        node->kind = kind;
        return node;
    }

    /**
     * @brief 构造一元节点。
     * @param kind 节点类别。
     * @param child 子节点。
     * @return 新建节点。
     */
    static std::shared_ptr<RegexNode> makeUnary(
        RegexNode::Kind kind, const std::shared_ptr<RegexNode> &child)
    {
        std::shared_ptr<RegexNode> node = makeNode(kind);
        node->left = child;
        return node;
    }

    /**
     * @brief 构造二元节点。
     * @param kind 节点类别。
     * @param left 左子节点。
     * @param right 右子节点。
     * @return 新建节点。
     */
    static std::shared_ptr<RegexNode> makeBinary(
        RegexNode::Kind kind, const std::shared_ptr<RegexNode> &left,
        const std::shared_ptr<RegexNode> &right)
    {
        std::shared_ptr<RegexNode> node = makeNode(kind);
        node->left = left;
        node->right = right;
        return node;
    }

    std::string m_pattern; ///< 正则文本
    std::size_t m_pos = 0; ///< 当前解析位置
    Error m_error;         ///< 第一个错误
};

/**
 * @brief Thompson 构造的 NFA；转移符号是字符类编号。
 */
class RegexNfa
{
public:
    /**
     * @brief 绑定共享的字符类划分。
     * @param cells 字符类划分。
     */
    explicit RegexNfa(const std::vector<RegexCell> &cells)
        : m_cells(cells)
    {
    }

    /**
     * @brief 从 AST 构造 NFA。
     * @param ast AST 根节点。
     * @return 自身引用。
     */
    RegexNfa &build(const std::shared_ptr<RegexNode> &ast)
    {
        const std::pair<int, int> ends = buildNode(ast);
        m_start = ends.first;
        m_accept = ends.second;
        return *this;
    }

    int start() const { return m_start; }       ///< 起始状态编号
    int accept() const { return m_accept; }     ///< 接受状态编号
    int stateCount() const { return m_stateCount; } ///< 状态总数
    const std::map<int, std::map<int, std::set<int>>> &trans() const
    {
        return m_trans;
    }
    const std::map<int, std::set<int>> &eps() const { return m_eps; }

private:
    /**
     * @brief 新建一个状态。
     * @return 新状态编号。
     */
    int newState()
    {
        const int state = m_stateCount;
        ++m_stateCount;
        m_trans[state] = {};
        m_eps[state] = {};
        return state;
    }

    /**
     * @brief 把字符集合映射成字符类编号集合。
     * @param charSet 字符集合。
     * @return 命中的字符类编号列表。
     */
    std::vector<int> symbolsOf(const std::set<char> &charSet) const
    {
        std::vector<int> result;
        for (std::size_t index = 0; index < m_cells.size(); ++index)
        {
            if (charSet.count(m_cells[index].representative))
            {
                result.push_back(static_cast<int>(index));
            }
        }
        return result;
    }

    /**
     * @brief 递归构造某个 AST 子节点对应的 NFA 片段。
     * @param node AST 节点。
     * @return (起点, 终点) 二元组。
     */
    std::pair<int, int> buildNode(const std::shared_ptr<RegexNode> &node)
    {
        switch (node->kind)
        {
        case RegexNode::Kind::Class:
        {
            const int start = newState();
            const int end = newState();
            for (const int symbol : symbolsOf(node->chars))
            {
                m_trans[start][symbol].insert(end);
            }
            return {start, end};
        }
        case RegexNode::Kind::Empty:
        {
            const int start = newState();
            const int end = newState();
            m_eps[start].insert(end);
            return {start, end};
        }
        case RegexNode::Kind::Concat:
        {
            const auto left = buildNode(node->left);
            const auto right = buildNode(node->right);
            m_eps[left.second].insert(right.first);
            return {left.first, right.second};
        }
        case RegexNode::Kind::Alt:
        {
            const auto left = buildNode(node->left);
            const auto right = buildNode(node->right);
            const int start = newState();
            const int end = newState();
            m_eps[start].insert(left.first);
            m_eps[start].insert(right.first);
            m_eps[left.second].insert(end);
            m_eps[right.second].insert(end);
            return {start, end};
        }
        case RegexNode::Kind::Star:
        {
            const auto sub = buildNode(node->left);
            const int start = newState();
            const int end = newState();
            m_eps[start].insert(sub.first);
            m_eps[start].insert(end);
            m_eps[sub.second].insert(sub.first);
            m_eps[sub.second].insert(end);
            return {start, end};
        }
        case RegexNode::Kind::Plus:
        {
            const auto sub = buildNode(node->left);
            const int start = newState();
            const int end = newState();
            m_eps[start].insert(sub.first);
            m_eps[sub.second].insert(sub.first);
            m_eps[sub.second].insert(end);
            return {start, end};
        }
        case RegexNode::Kind::Opt:
        default:
        {
            const auto sub = buildNode(node->left);
            const int start = newState();
            const int end = newState();
            m_eps[start].insert(sub.first);
            m_eps[start].insert(end);
            m_eps[sub.second].insert(end);
            return {start, end};
        }
        }
    }

    std::vector<RegexCell> m_cells;                    ///< 字符类划分
    std::map<int, std::map<int, std::set<int>>> m_trans; ///< 非 ε 转移
    std::map<int, std::set<int>> m_eps;                ///< ε 转移
    int m_start = 0;                                   ///< 起始状态
    int m_accept = 0;                                  ///< 接受状态
    int m_stateCount = 0;                              ///< 状态总数
};

/**
 * @brief ε 闭包。
 * @param nfa 目标 NFA。
 * @param states 起始状态集合。
 * @return 闭包集合。
 */
std::set<int> epsilonClosure(const RegexNfa &nfa, const std::set<int> &states)
{
    std::vector<int> stack(states.begin(), states.end());
    std::set<int> closure = states;
    while (!stack.empty())
    {
        const int state = stack.back();
        stack.pop_back();
        const auto found = nfa.eps().find(state);
        if (found == nfa.eps().end())
        {
            continue;
        }
        for (const int target : found->second)
        {
            if (!closure.count(target))
            {
                closure.insert(target);
                stack.push_back(target);
            }
        }
    }
    return closure;
}

/**
 * @brief 子集构造：NFA -> DFA（补齐陷阱状态，转移全覆盖）。
 * @param nfa 目标 NFA。
 * @param alphabet 字符类编号列表。
 * @param outTransitions 输出转移表。
 * @param outStart 输出起始状态。
 * @param outAccept 输出接受状态集合（PyIntSet，保持 CPython 顺序）。
 * @param outTrap 输出陷阱状态。
 * @return 状态总数（含陷阱状态）。
 */
int subsetConstruction(const RegexNfa &nfa,
                       const std::vector<int> &alphabet,
                       std::map<int, std::map<int, int>> &outTransitions,
                       int &outStart, PyIntSet &outAccept, int &outTrap)
{
    const std::set<int> startClosure = epsilonClosure(nfa, {nfa.start()});
    std::map<std::set<int>, int> stateMap;
    stateMap[startClosure] = 0;
    std::map<int, std::map<int, int>> transitions;
    std::deque<std::set<int>> queue;
    queue.push_back(startClosure);
    int nextId = 1;
    while (!queue.empty())
    {
        const std::set<int> current = queue.front();
        queue.pop_front();
        const int currentId = stateMap[current];
        if (current.count(nfa.accept()))
        {
            outAccept.add(currentId);
        }
        std::map<int, int> row;
        for (const int symbol : alphabet)
        {
            std::set<int> following;
            for (const int state : current)
            {
                const auto found = nfa.trans().find(state);
                if (found == nfa.trans().end())
                {
                    continue;
                }
                const auto symbolFound = found->second.find(symbol);
                if (symbolFound != found->second.end())
                {
                    following.insert(symbolFound->second.begin(),
                                     symbolFound->second.end());
                }
            }
            if (following.empty())
            {
                continue;
            }
            const std::set<int> closure = epsilonClosure(nfa, following);
            if (!stateMap.count(closure))
            {
                stateMap[closure] = nextId;
                ++nextId;
                queue.push_back(closure);
            }
            row[symbol] = stateMap[closure];
        }
        transitions[currentId] = row;
    }
    const int trap = nextId;
    for (const int symbol : alphabet)
    {
        transitions[trap][symbol] = trap;
    }
    for (int state = 0; state < nextId; ++state)
    {
        for (const int symbol : alphabet)
        {
            if (!transitions[state].count(symbol))
            {
                transitions[state][symbol] = trap;
            }
        }
    }
    outTransitions = transitions;
    outStart = 0;
    outTrap = trap;
    return nextId;
}

/**
 * @brief 划分法最小化（保留陷阱状态）。
 * @param transitions DFA 转移表。
 * @param start 起始状态。
 * @param accept 接受状态集合。
 * @param trap 陷阱状态。
 * @param alphabet 字符类编号列表。
 * @param outTransitions 输出新转移表。
 * @param outStart 输出新起始状态。
 * @param outAccept 输出新接受状态集合。
 * @param outTrap 输出新陷阱状态。
 * @param outStateCount 输出新状态总数。
 */
void minimize(const std::map<int, std::map<int, int>> &transitions, int start,
              const PyIntSet &accept, int trap,
              const std::vector<int> &alphabet,
              std::map<int, std::map<int, int>> &outTransitions, int &outStart,
              std::set<int> &outAccept, int &outTrap, int &outStateCount)
{
    // 可达性分析：按 Python 的集合迭代顺序加入，保证后续划分顺序一致。
    PyIntSet reachable;
    std::vector<int> stack{start};
    while (!stack.empty())
    {
        const int state = stack.back();
        stack.pop_back();
        if (reachable.contains(state))
        {
            continue;
        }
        reachable.add(state);
        for (const int symbol : alphabet)
        {
            const int target = transitions.at(state).at(symbol);
            if (!reachable.contains(target))
            {
                stack.push_back(target);
            }
        }
    }
    // accept & reachable：按较小集合的迭代顺序构造新集合。
    PyIntSet acceptReachable;
    {
        const PyIntSet &smaller =
            (accept.size() > reachable.size()) ? reachable : accept;
        const PyIntSet &larger =
            (accept.size() > reachable.size()) ? accept : reachable;
        for (const std::int64_t value : smaller.values())
        {
            if (larger.contains(value))
            {
                acceptReachable.add(value);
            }
        }
    }
    // reachable - accept：先拷贝 reachable，再逐个删除 accept 里的元素。
    PyIntSet nonAccept;
    for (const std::int64_t value : reachable.values())
    {
        nonAccept.add(value);
    }
    for (const std::int64_t value : acceptReachable.values())
    {
        nonAccept.discard(value);
    }
    std::vector<PyIntSet> partitions;
    if (nonAccept.size() > 0)
    {
        partitions.push_back(nonAccept);
    }
    if (acceptReachable.size() > 0)
    {
        partitions.push_back(acceptReachable);
    }
    while (true)
    {
        std::vector<PyIntSet> refined;
        for (const PyIntSet &group : partitions)
        {
            // buckets 用 [签名 -> 状态集合]，保持签名首次出现的顺序。
            std::vector<std::pair<std::vector<int>, PyIntSet>> buckets;
            for (const std::int64_t stateValue : group.values())
            {
                const int state = static_cast<int>(stateValue);
                std::vector<int> signature;
                signature.reserve(alphabet.size());
                for (const int symbol : alphabet)
                {
                    const int target = transitions.at(state).at(symbol);
                    int partIndex = 0;
                    for (std::size_t index = 0; index < partitions.size();
                         ++index)
                    {
                        if (partitions[index].contains(target))
                        {
                            partIndex = static_cast<int>(index);
                            break;
                        }
                    }
                    signature.push_back(partIndex);
                }
                bool found = false;
                for (auto &bucket : buckets)
                {
                    if (bucket.first == signature)
                    {
                        bucket.second.add(state);
                        found = true;
                        break;
                    }
                }
                if (!found)
                {
                    buckets.emplace_back(signature, PyIntSet());
                    buckets.back().second.add(state);
                }
            }
            for (auto &bucket : buckets)
            {
                refined.push_back(bucket.second);
            }
        }
        if (refined.size() == partitions.size())
        {
            break;
        }
        partitions = refined;
    }
    std::map<int, int> stateToGroup;
    for (std::size_t index = 0; index < partitions.size(); ++index)
    {
        for (const std::int64_t state : partitions[index].values())
        {
            stateToGroup[static_cast<int>(state)] =
                static_cast<int>(index);
        }
    }
    std::map<int, std::map<int, int>> newTransitions;
    for (std::size_t index = 0; index < partitions.size(); ++index)
    {
        const int representative = static_cast<int>(partitions[index].front());
        for (const int symbol : alphabet)
        {
            newTransitions[static_cast<int>(index)][symbol] =
                stateToGroup[transitions.at(representative).at(symbol)];
        }
    }
    outAccept.clear();
    for (const std::int64_t state : acceptReachable.values())
    {
        outAccept.insert(stateToGroup[static_cast<int>(state)]);
    }
    outTransitions = newTransitions;
    outStart = stateToGroup[start];
    outTrap = stateToGroup[trap];
    outStateCount = static_cast<int>(partitions.size());
}

} // namespace

bool RegexDfa::parse(const std::string &pattern,
                     std::shared_ptr<RegexNode> &out, Error &error)
{
    RegexParserImpl parser(pattern);
    const std::shared_ptr<RegexNode> node = parser.parse();
    if (parser.failed())
    {
        return fail(error, parser.error());
    }
    out = node;
    return true;
}

void RegexDfa::collectClassSets(const std::shared_ptr<RegexNode> &node,
                                std::vector<std::set<char>> &found)
{
    switch (node->kind)
    {
    case RegexNode::Kind::Class:
        found.push_back(node->chars);
        break;
    case RegexNode::Kind::Concat:
    case RegexNode::Kind::Alt:
        collectClassSets(node->left, found);
        collectClassSets(node->right, found);
        break;
    case RegexNode::Kind::Star:
    case RegexNode::Kind::Plus:
    case RegexNode::Kind::Opt:
        collectClassSets(node->left, found);
        break;
    case RegexNode::Kind::Empty:
    default:
        break;
    }
}

std::pair<std::vector<RegexCell>, std::vector<int>> RegexDfa::charClasses(
    const std::vector<std::set<char>> &classSets)
{
    // 按“对每个集合的属于/不属于”签名分组，再按组内最小字符排序。
    std::map<std::vector<bool>, std::vector<char>> groups;
    for (int code = 0; code < 256; ++code)
    {
        const char ch = static_cast<char>(code);
        std::vector<bool> signature;
        signature.reserve(classSets.size());
        for (const std::set<char> &chars : classSets)
        {
            signature.push_back(chars.count(ch) != 0);
        }
        groups[signature].push_back(ch);
    }
    std::vector<std::pair<int, std::vector<char>>> ordered;
    ordered.reserve(groups.size());
    for (auto &entry : groups)
    {
        ordered.emplace_back(static_cast<unsigned char>(entry.second.front()),
                             entry.second);
    }
    std::stable_sort(ordered.begin(), ordered.end(),
                     [](const auto &left, const auto &right)
                     { return left.first < right.first; });
    std::vector<RegexCell> cells;
    std::vector<int> charMap(256, 0);
    for (auto &entry : ordered)
    {
        RegexCell cell;
        cell.representative = entry.second.front();
        for (const char ch : entry.second)
        {
            cell.chars.insert(ch);
            charMap[static_cast<unsigned char>(ch)] =
                static_cast<int>(cells.size());
        }
        cells.push_back(cell);
    }
    return {cells, charMap};
}

bool RegexDfa::buildDfa(const std::string &pattern,
                        const std::vector<RegexCell> &cells, DfaProgram &out,
                        Error &error)
{
    std::shared_ptr<RegexNode> ast;
    if (!parse(pattern, ast, error))
    {
        return false;
    }
    RegexNfa nfa(cells);
    nfa.build(ast);
    std::vector<int> alphabet;
    for (std::size_t index = 0; index < cells.size(); ++index)
    {
        alphabet.push_back(static_cast<int>(index));
    }
    std::map<int, std::map<int, int>> transitions;
    int start = 0;
    int trap = 0;
    PyIntSet accept;
    subsetConstruction(nfa, alphabet, transitions, start, accept, trap);
    std::map<int, std::map<int, int>> minimizedTransitions;
    std::set<int> minimizedAccept;
    int minimizedStart = 0;
    int minimizedTrap = 0;
    int minimizedCount = 0;
    minimize(transitions, start, accept, trap, alphabet, minimizedTransitions,
             minimizedStart, minimizedAccept, minimizedTrap, minimizedCount);
    out.pattern = pattern;
    out.stateCount = minimizedCount;
    out.start = minimizedStart;
    out.trap = minimizedTrap;
    out.accept = minimizedAccept;
    out.transitions = minimizedTransitions;
    return true;
}

bool RegexDfa::compileAll(
    const std::vector<std::pair<std::string, std::string>> &regexDefs,
    RegexCompileResult &out, Error &error)
{
    std::vector<std::shared_ptr<RegexNode>> asts;
    std::vector<std::set<char>> classSets;
    std::vector<std::pair<std::string, std::string>> defs;
    for (const auto &entry : regexDefs)
    {
        std::shared_ptr<RegexNode> ast;
        if (!parse(entry.second, ast, error))
        {
            return false;
        }
        asts.push_back(ast);
        defs.push_back(entry);
        collectClassSets(ast, classSets);
    }
    const auto classes = charClasses(classSets);
    out.cells = classes.first;
    out.charMap = classes.second;
    for (std::size_t index = 0; index < defs.size(); ++index)
    {
        CompiledRegex compiled;
        compiled.name = defs[index].first;
        compiled.pattern = defs[index].second;
        if (!buildDfa(defs[index].second, out.cells, compiled.dfa, error))
        {
            return false;
        }
        out.compiled.push_back(compiled);
    }
    return true;
}

} // namespace ParseEngine
