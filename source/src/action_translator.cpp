/**
 * @file action_translator.cpp
 * @brief 语义动作翻译实现。
 */

#include "ParseEngine/action_translator.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <iostream>
#include <map>

#include "ParseEngine/analysis.hpp"
#include "ParseEngine/utils.hpp"

namespace ParseEngine
{

namespace
{

/// 标量类型构造写法：`int32($1)` 之类 -> 对应的 C 类型名。
const std::map<std::string, std::string> &actionTypeTable()
{
    static const std::map<std::string, std::string> k_table = {
        {"char", "char"},       {"int8", "int8_t"},     {"uint8", "uint8_t"},
        {"int16", "int16_t"},   {"uint16", "uint16_t"}, {"int32", "int32_t"},
        {"uint32", "uint32_t"}, {"int64", "int64_t"},   {"uint64", "uint64_t"},
        {"float", "float"},     {"double", "double"}};
    return k_table;
}

/**
 * @brief C# 的基础类型构造表：`int32($1)` 之类 -> 语言里的基础类型名。
 *
 * 只做「够用就行」的映射（任务边界写明不做复杂类型跨语言映射）：DSL 里的
 * 定宽整数按语义最接近的 C# 基础类型落，`float` 映射成 `double`
 * （C# 的 `float` 精度反而比 C 的 `float` 更吃亏，交叉语言时不值得）。
 * @return 类型名映射。
 */
const std::map<std::string, std::string> &csharpTypeTable()
{
    static const std::map<std::string, std::string> k_table = {
        {"char", "char"},     {"int8", "byte"},   {"uint8", "short"},
        {"int16", "short"},   {"uint16", "int"},  {"int32", "int"},
        {"uint32", "long"},   {"int64", "long"},  {"uint64", "long"},
        {"float", "double"},  {"double", "double"}};
    return k_table;
}

/**
 * @brief Rust 的基础类型构造表：`int32($1)` 之类 -> Rust 的基础类型名。
 *
 * 与 C# 的口径一致：只做「够用就行」的映射，不做复杂类型跨语言映射
 * （任务边界写明）。Rust 没有 C 风格的强制转换，转换写成 `(x as i32)`
 * 而不是 `(i32)(x)`——这一点在 `makeConstructor` 里单独处理。
 * @return 类型名映射。
 */
const std::map<std::string, std::string> &rustTypeTable()
{
    static const std::map<std::string, std::string> k_table = {
        {"char", "u8"},       {"int8", "i8"},     {"uint8", "u8"},
        {"int16", "i16"},     {"uint16", "u16"},  {"int32", "i32"},
        {"uint32", "u32"},    {"int64", "i64"},   {"uint64", "u64"},
        {"float", "f32"},     {"double", "f64"}};
    return k_table;
}

/**
 * @brief Go 的基础类型构造表：`int32($1)` 之类 -> Go 的基础类型名。
 *
 * 与 C# / Rust 的口径一致：只做「够用就行」的映射，不做复杂类型跨语言映射
 * （任务边界写明）。Go 的定宽整数与 C 的命名几乎一一对应（`int32` / `uint64`
 * …），所以这张表基本是恒等映射，只是把 C 的 `char` / `float` / `double`
 * 换成 Go 里语义最接近的 `byte` / `float32` / `float64`。
 * @return 类型名映射。
 */
const std::map<std::string, std::string> &goTypeTable()
{
    static const std::map<std::string, std::string> k_table = {
        {"char", "byte"},     {"int8", "int8"},     {"uint8", "uint8"},
        {"int16", "int16"},   {"uint16", "uint16"}, {"int32", "int32"},
        {"uint32", "uint32"}, {"int64", "int64"},   {"uint64", "uint64"},
        {"float", "float32"}, {"double", "float64"}};
    return k_table;
}

/// 二元运算符优先级（数字越大越先结合）。
const std::map<std::string, int> &binaryPrecedence()
{
    static const std::map<std::string, int> k_table = {
        {"||", 1},  {"&&", 2}, {"|", 3},  {"^", 4},   {"&", 5},
        {"==", 6},  {"!=", 6}, {"<", 7},  {">", 7},   {"<=", 7},
        {">=", 7},  {"<<", 8}, {">>", 8}, {"+", 9},   {"-", 9},
        {"*", 10},  {"/", 10}, {"%", 10}};
    return k_table;
}

/**
 * @brief Python 产物的二元运算符优先级表（在 C 表上补 and / or）。
 * @return 运算符到优先级的映射。
 */
const std::map<std::string, int> &pythonBinaryPrecedence()
{
    static const std::map<std::string, int> k_table = []
    {
        std::map<std::string, int> table = binaryPrecedence();
        table["or"] = 1;
        table["and"] = 2;
        return table;
    }();
    return k_table;
}

/**
 * @brief 判断字符是否为数字。
 * @param ch 字符。
 * @return 是数字返回 true。
 */
bool isDigitChar(char ch)
{
    return (ch >= '0') && (ch <= '9');
}

/**
 * @brief 判断一段文本是不是合法标识符（$line / $token 这类伪变量用）。
 * @param text 文本。
 * @return 是标识符返回 true。
 */
bool isIdentifierText(const std::string &text)
{
    if (text.empty())
    {
        return false;
    }
    for (std::size_t index = 0; index < text.size(); ++index)
    {
        const char ch = text[index];
        const bool word = ((ch >= 'a') && (ch <= 'z')) ||
                          ((ch >= 'A') && (ch <= 'Z')) ||
                          ((ch >= '0') && (ch <= '9')) || (ch == '_');
        if (!word || ((index == 0) && isDigitChar(ch)))
        {
            return false;
        }
    }
    return true;
}

/**
 * @brief 判断字符是否为标识符组成字符。
 * @param ch 字符。
 * @return 是字母/数字/下划线返回 true。
 */
bool isWordChar(char ch)
{
    return std::isalnum(static_cast<unsigned char>(ch)) || (ch == '_');
}

/**
 * @brief 词法分析一条动作文本（对应 Python 的 _TOKEN_RE.finditer）。
 * @param action 动作文本。
 * @param outTokens 输出 token 列表。
 */
void tokenizeAction(const std::string &action,
                    std::vector<ActionTranslator::Token> &outTokens)
{
    std::size_t index = 0;
    const std::size_t size = action.size();
    while (index < size)
    {
        const char ch = action[index];
        std::size_t cursor = index;
        std::size_t start = index;
        // 空白
        if (std::isspace(static_cast<unsigned char>(ch)))
        {
            index += 1;
            continue;
        }
        // 数字
        {
            bool ok = false;
            cursor = index;
            if (isDigitChar(ch))
            {
                while ((cursor < size) && isDigitChar(action[cursor]))
                {
                    ++cursor;
                }
                if ((cursor < size) && (action[cursor] == '.'))
                {
                    ++cursor;
                    while ((cursor < size) && isDigitChar(action[cursor]))
                    {
                        ++cursor;
                    }
                }
                ok = true;
            }
            else if ((ch == '.') && (index + 1 < size) &&
                     isDigitChar(action[index + 1]))
            {
                ++cursor;
                while ((cursor < size) && isDigitChar(action[cursor]))
                {
                    ++cursor;
                }
                ok = true;
            }
            if (ok)
            {
                if ((cursor < size) &&
                    ((action[cursor] == 'e') || (action[cursor] == 'E')))
                {
                    std::size_t probe = cursor + 1;
                    if ((probe < size) &&
                        ((action[probe] == '+') || (action[probe] == '-')))
                    {
                        ++probe;
                    }
                    if ((probe < size) && isDigitChar(action[probe]))
                    {
                        while ((probe < size) && isDigitChar(action[probe]))
                        {
                            ++probe;
                        }
                        cursor = probe;
                    }
                }
                outTokens.push_back({"num", action.substr(start, cursor - start),
                                     start, cursor});
                index = cursor;
                continue;
            }
        }
        // 字符串字面量
        if ((ch == '"') || (ch == '\''))
        {
            const char quote = ch;
            cursor = index + 1;
            while (cursor < size)
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
            if ((cursor <= size) && (cursor > index + 1) &&
                (action[cursor - 1] == quote))
            {
                outTokens.push_back({"str",
                                     action.substr(start, cursor - start),
                                     start, cursor});
                index = cursor;
                continue;
            }
        }
        // 标识符
        if (std::isalpha(static_cast<unsigned char>(ch)) || (ch == '_'))
        {
            cursor = index + 1;
            while ((cursor < size) && isWordChar(action[cursor]))
            {
                ++cursor;
            }
            outTokens.push_back({"id", action.substr(start, cursor - start),
                                 start, cursor});
            index = cursor;
            continue;
        }
        // $$ / $n
        if (ch == '$')
        {
            cursor = index + 1;
            if ((cursor < size) && (action[cursor] == '$'))
            {
                ++cursor;
            }
            else
            {
                while ((cursor < size) && isDigitChar(action[cursor]))
                {
                    ++cursor;
                }
                // $line / $column / $token / $code / $message：字母开头的伪变量
                if ((cursor == index + 1) && (cursor < size) &&
                    (std::isalpha(static_cast<unsigned char>(action[cursor])) ||
                     (action[cursor] == '_')))
                {
                    ++cursor;
                    while ((cursor < size) && isWordChar(action[cursor]))
                    {
                        ++cursor;
                    }
                }
            }
            if (cursor > index + 1)
            {
                outTokens.push_back({"dollar",
                                     action.substr(start, cursor - start),
                                     start, cursor});
                index = cursor;
                continue;
            }
        }
        // 运算符：先试双字符
        static const char *k_twoCharOps[] = {"&&", "||", "<<", ">>",
                                             "<=", ">=", "==", "!="};
        bool matched = false;
        for (const char *op : k_twoCharOps)
        {
            if (action.compare(index, 2, op) == 0)
            {
                outTokens.push_back({"op", op, index, index + 2});
                index += 2;
                matched = true;
                break;
            }
        }
        if (matched)
        {
            continue;
        }
        static const std::string k_singleOps = "+-*/%^&|!~<>=?:(.)[];,.";
        if (k_singleOps.find(ch) != std::string::npos)
        {
            outTokens.push_back({"op", std::string(1, ch), index, index + 1});
            index += 1;
            continue;
        }
        // 无法识别的字符被正则的 finditer 直接跳过
        ++index;
    }
}

} // namespace

} // namespace ParseEngine

namespace ParseEngine
{

ActionTranslator::ActionTranslator(std::string action, std::string mode,
                                   const UnionValueSpec *unionSpec, bool cpp17,
                                   bool python, bool csharpStyle,
                                   bool rustStyle, bool goStyle)
    : m_action(std::move(action)), m_mode(std::move(mode)),
    m_unionSpec(unionSpec), m_cpp17(cpp17), m_python(python),
    m_csharp(csharpStyle), m_rust(rustStyle), m_go(goStyle)
{
    m_union = (unionSpec != nullptr) && (unionSpec->symbolMember != nullptr);
    // LR 家族（含 lr0 / lr1 教学模式）都用 LR 状态栈取 $n
    m_slrMode = isLrMode(m_mode);
    tokenizeAction(m_action, m_tokens);
}

bool ActionTranslator::unionMode() const
{
    return m_union;
}

const std::string *ActionTranslator::memberOf(const std::string &symbol) const
{
    if (!m_union)
    {
        return nullptr;
    }
    const auto member = m_unionSpec->symbolMember->find(symbol);
    return member != m_unionSpec->symbolMember->end() ? &member->second : nullptr;
}

std::string ActionTranslator::slotMemberSuffix(int index) const
{
    if (!m_union)
    {
        return "";
    }
    if ((index < 1) ||
        (static_cast<std::size_t>(index) > m_unionSpec->slotSymbols.size()))
    {
        return "";
    }
    const std::string &symbol =
        m_unionSpec->slotSymbols[static_cast<std::size_t>(index) - 1];
    const std::string *member = memberOf(symbol);
    if (member == nullptr)
    {
        fail(
            "动作 '" + m_action + "'：" + productionWhere() + "的 $" +
            std::to_string(index) + "（符号 " + symbol +
            "）没有绑定 %union 成员，写 %union 时每个被 $n 引用的符号都要用 "
            "%type <成员> 或 %token <成员> 绑定");
        return {};
    }
    return "." + *member;
}

std::string ActionTranslator::lhsMemberSuffix() const
{
    if (!m_union)
    {
        return "";
    }
    const std::string *member = memberOf(m_unionSpec->lhsSymbol);
    if (member == nullptr)
    {
        fail(
            "动作 '" + m_action + "'：" + productionWhere() + "的 $$（左部符号 " +
            m_unionSpec->lhsSymbol +
            "）没有绑定 %union 成员，请用 %type <成员> 绑定它");
        return {};
    }
    return "." + *member;
}

std::string ActionTranslator::productionWhere() const
{
    if ((m_unionSpec == nullptr) || (m_unionSpec->productionId < 0))
    {
        return "";
    }
    return "产生式 P" + std::to_string(m_unionSpec->productionId) + " ";
}

std::string ActionTranslator::unionHint() const
{
    if (!m_union)
    {
        return "";
    }
    return "；写了 %union 时语义值就是联合体本身，直接写 $n.<成员名>";
}

std::optional<std::string> ActionTranslator::peek() const
{
    if (m_pos < m_tokens.size())
    {
        return m_tokens[m_pos].text;
    }
    return std::nullopt;
}

bool ActionTranslator::accept(const std::string &text)
{
    const auto current = peek();
    if (current.has_value() && (current.value() == text))
    {
        ++m_pos;
        return true;
    }
    return false;
}

void ActionTranslator::expect(const std::string &text)
{
    if (!accept(text))
    {
        const auto current = peek();
        fail(
            "动作 '" + m_action + "' 期望 '" + text + "'，实际遇到 '" +
            (current.has_value() ? current.value() : std::string("None")) +
            "'");
    }
}

void ActionTranslator::warn(const std::string &message)
{
    if (!m_warned.count(message))
    {
        m_warned.insert(message);
        std::cerr << "警告: " << message << std::endl;
    }
}

std::string ActionTranslator::customValueCode(int index) const
{
    // $n 越界（含 $0）：右部没有这个符号，生成出来的成员后缀会缺一段，
    // 用户只能对着产物的 C 类型错误猜——这里直接拦在生成期。
    if ((m_slotCount > 0) && ((index < 1) || (index > m_slotCount)))
    {
        fail("动作 '" + m_action + "'：" + productionWhere() + "的 $" +
             std::to_string(index) + " 越界：这条产生式右部只有 " +
             std::to_string(m_slotCount) + " 个符号" +
             ((index < 1) ? "（$n 从 $1 起）" : ""));
        return {};
    }
    std::string code;
    if (m_python || m_csharp || m_rust || m_go)
    {
        // Python / C# / Rust / Go 产物：LR 归约时动作看到的是「右部语义值切片」
        // （下标 0 对应 $1），LL 则沿用与 C 目标一致的 1 起下标（帧里第 0 个
        // 槽位留空）
        code = m_slrMode
                   ? ("values[" + std::to_string(index - 1) + "]")
                   : ("values[" + std::to_string(index) + "]");
        // 写了 %union 时按符号绑定的成员名补上「.成员」（C# / Rust 下
        // 语义值是带成员字段的类 / 结构体，展开方式与 C 一致）
        code += slotMemberSuffix(index);
        return code;
    }
    if (m_slrMode)
    {
        code = "stack->values[stack->top - " + std::to_string(m_rhsLength) +
               "u + " + std::to_string(index - 1) + "u]";
    }
    else
    {
        code = "values[" + std::to_string(index) + "]";
    }
    // 写了 %union 时按符号绑定的成员名补上「.成员」
    code += slotMemberSuffix(index);
    return failed() ? std::string() : code;
}

bool ActionTranslator::translate(const std::string &modeIn, int rhsLength,
                                 std::string &out, Error &error)
{
    const std::string actualMode = modeIn.empty() ? m_mode : modeIn;
    m_mode = actualMode;
    // 右部符号个数：LR 用来算槽位，所有模式都用来校验 $n 有没有越界
    m_slotCount = std::max(0, rhsLength);
    if (isLrMode(actualMode))
    {
        m_slrMode = true;
        m_rhsLength = std::max(0, rhsLength);
    }
    else
    {
        m_slrMode = false;
        m_rhsLength = 0;
    }
    const std::string statements = translateStatements();
    if (failed())
    {
        return ParseEngine::fail(error, m_error);
    }
    out = statements;
    return true;
}

std::string ActionTranslator::translateStatements()
{
    std::vector<std::string> statements;
    while (!failed() && (m_pos < m_tokens.size()))
    {
        if ((peek().value_or("") == "delete") &&
            (m_pos + 1 < m_tokens.size()) &&
            (m_tokens[m_pos + 1].text == "["))
        {
            fail(
                "动作 '" + m_action +
                "'：delete[...] 是内置运行时特有的写法，已随 builtin 运行时"
                "移除；请在 %code ... %endcode 里自行实现");
            return {};
        }
        // yyerrok / yyclearin：yacc 的恢复伪函数，只能整个语句这么写
        {
            const std::string ident = peek().value_or("");
            if ((ident == "yyerrok") || (ident == "yyclearin"))
            {
                const std::string recovered = recoveryCall(ident);
                if (failed())
                {
                    return {};
                }
                statements.push_back(recovered);
                continue;
            }
        }
        const ExprNode lhs = parseTernary();
        if (failed())
        {
            return {};
        }
        std::string statement;
        if (accept("="))
        {
            const ExprNode rhs = parseTernary();
            if (failed())
            {
                return {};
            }
            statement = buildAssignment(lhs, rhs);
            if (failed())
            {
                return {};
            }
        }
        else
        {
            statement = m_python ? lhs.code : (lhs.code + ";");
        }
        expect(";");
        if (failed())
        {
            return {};
        }
        statements.push_back(statement);
    }
    if (m_pos != m_tokens.size())
    {
        fail("动作 '" + m_action + "' 存在无法识别的尾部内容：'" +
             m_tokens[m_pos].text + "'");
        return {};
    }
    std::string result;
    for (std::size_t index = 0; index < statements.size(); ++index)
    {
        if (index != 0)
        {
            result += "\n        ";
        }
        result += statements[index];
    }
    return result;
}

std::string ActionTranslator::recoveryCall(const std::string &ident)
{
    if ((m_mode == "ll1") || (m_mode == "llk"))
    {
        fail(
            "动作 '" + m_action + "'：" + ident +
            " 只在 LR 家族（lr0 / slr / lalr / lr1 / ielr / lrk / lalrk / "
            "slrk）里可用；LL 家族的错误恢复用 %sync 声明同步 Token");
        return {};
    }
    if (isDdMode(m_mode))
    {
        fail(
            "动作 '" + m_action + "'：" + ident +
            " 只在 LR 家族（lr0 / slr / lalr / lr1 / ielr / lrk / lalrk / "
            "slrk）里可用；DD（递归下降）没有分析栈，错误恢复用 %sync "
            "声明同步 Token");
        return {};
    }
    ++m_pos;
    expect(";");
    if (failed())
    {
        return {};
    }
    if (m_python)
    {
        // Python 产物里它们是解析器的方法（动作代码就写在 Parser 类里）
        return (ident == "yyerrok") ? "self._yy_err_ok()"
                                    : "self._yy_clearin()";
    }
    if (m_csharp)
    {
        // C# 产物里它们同样是解析器类的方法（动作代码就写在类里）
        return (ident == "yyerrok") ? "yyErrOk();" : "yyClearIn();";
    }
    if (m_rust)
    {
        // Rust 产物里它们是解析器的方法（动作代码就写在 impl 里，要带 self.）
        return (ident == "yyerrok") ? "self.yy_err_ok();"
                                    : "self.yy_clearin();";
    }
    if (m_go)
    {
        // Go 产物里它们是 *Parser 的方法（动作代码写在方法里，接收者固定叫 p）
        return (ident == "yyerrok") ? "p.YyErrOk()" : "p.YyClearIn()";
    }
    if (m_cpp17)
    {
        // C++17 产物里它们是 Engine::Private 的成员函数
        return (ident == "yyerrok") ? "yyErrOk();" : "yyClearIn();";
    }
    return (ident == "yyerrok") ? "Parser_LrYyErrOk(pCtx);"
                                : "Parser_LrYyClearIn(pCtx);";
}

ExprNode ActionTranslator::customBinary(const std::string &op,
                                        const ExprNode &left,
                                        const ExprNode &right) const
{
    const auto operand = [](const ExprNode &node)
    {
        if ((node.kind == ExprNode::Kind::Cbin) ||
            (node.kind == ExprNode::Kind::Cun) ||
            (node.kind == ExprNode::Kind::Ctern))
        {
            return "(" + node.code + ")";
        }
        return node.code;
    };
    ExprNode result;
    result.kind = ExprNode::Kind::Cbin;
    result.code = operand(left) + " " + op + " " + operand(right);
    return result;
}

ExprNode ActionTranslator::parseTernary()
{
    const ExprNode condition = parseBinary(1);
    if (accept("?"))
    {
        const ExprNode thenNode = parseTernary();
        expect(":");
        const ExprNode elseNode = parseTernary();
        return makeTernary(condition, thenNode, elseNode);
    }
    return condition;
}

ExprNode ActionTranslator::parseBinary(int minPrec)
{
    ExprNode left = parseUnary();
    while (true)
    {
        const auto op = peek();
        if (!op.has_value())
        {
            break;
        }
        const std::map<std::string, int> &table =
            m_python ? pythonBinaryPrecedence() : binaryPrecedence();
        const auto found = table.find(op.value());
        if (found == table.end())
        {
            break;
        }
        const int prec = found->second;
        if (prec < minPrec)
        {
            break;
        }
        ++m_pos;
        const ExprNode right = parseBinary(prec + 1);
        left = makeBinary(op.value(), left, right);
    }
    return left;
}

ExprNode ActionTranslator::parseUnary()
{
    const auto op = peek();
    if (op.has_value() && ((op.value() == "!") || (op.value() == "-") ||
                           (op.value() == "+") || (op.value() == "~") ||
                           (m_python && (op.value() == "not"))))
    {
        ++m_pos;
        return makeUnary(op.value(), parseUnary());
    }
    return parsePostfix();
}

ExprNode ActionTranslator::parsePostfix()
{
    ExprNode node = parsePrimary();
    while (true)
    {
        const auto token = peek();
        if (!token.has_value() || (token.value() != "."))
        {
            break;
        }
        std::optional<std::string> next;
        if (m_pos + 1 < m_tokens.size())
        {
            next = m_tokens[m_pos + 1].text;
        }
        // $n.<DSL 类型> 是内置运行时的写法，已随 builtin 移除；
        // 其余 .字段名 按普通 C 成员访问原样生成。
        if (next.has_value() && (actionTypeTable().count(next.value()) != 0))
        {
            fail("动作 '" + m_action + "'：$n." + next.value() +
                 " 类型字段访问是内置运行时特有的写法，已随 builtin 运行时"
                 "移除；自定义结构体请直接用 $n.<字段名>" + unionHint());
            return {};
        }
        if (!next.has_value() || (m_tokens[m_pos + 1].kind != "id"))
        {
            break;
        }
        m_pos += 2;
        ExprNode field;
        field.kind = ExprNode::Kind::Cfield;
        field.code = node.code + "." + next.value();
        // 成员访问不改变左值性质：$$.字段 可赋值，$n.字段 仍不能赋（$n 归它自己）
        field.lvKind = node.lvKind;
        field.lvOffset = node.lvOffset;
        node = field;
    }
    return node;
}

ExprNode ActionTranslator::parsePrimary()
{
    const auto tokenOpt = peek();
    if (!tokenOpt.has_value())
    {
        fail("动作 '" + m_action + "' 意外结束");
        return {};
    }
    const std::string token = tokenOpt.value();
    if (m_tokens[m_pos].kind == "num")
    {
        const std::string text = m_tokens[m_pos].text;
        ++m_pos;
        ExprNode node;
        node.kind = ExprNode::Kind::Raw;
        node.code = text;
        return node;
    }
    if (token == "(")
    {
        ++m_pos;
        const ExprNode node = parseTernary();
        expect(")");
        return node;
    }
    if (token == "$$")
    {
        ++m_pos;
        ExprNode node;
        node.kind = ExprNode::Kind::Raw;
        node.code = "result.value" + lhsMemberSuffix();
        node.lvKind = ExprNode::LvKind::Result;
        return node;
    }
    // $line / $column / $token / $code / $message：错误现场信息，可以写进动作里
    // 交给 printf 之类的调用（$code / $message 只在 error 产生式里有意义）
    if ((token.size() > 1) && (token[0] == '$') &&
        isIdentifierText(token.substr(1)))
    {
        const std::string name = token.substr(1);
        // 注：$code / $message 只在 error 产生式里有意义，这条限制在
        // grammar_loader 的生成期校验里查（那里能看到右部符号表）
        std::string code;
        if (name == "line")
        {
            code = m_python ? "self.current_location().line"
                            : (m_rust ? "self.current_line()"
                                      : (m_go ? "p.CurrentLine()"
                                      : (m_csharp ? "currentLine()"
                                      : (m_cpp17 ? "currentLocation().line"
                                                 : "pCtx->current.line"))));
        }
        else if (name == "column")
        {
            code = m_python
                       ? "self.current_location().column"
                       : (m_rust ? "self.current_column()"
                                 : (m_go ? "p.CurrentColumn()"
                                 : (m_csharp ? "currentColumn()"
                                 : (m_cpp17 ? "currentLocation().column"
                                            : "pCtx->current.column"))));
        }
        else if (name == "token")
        {
            code = m_python ? "(self._current.text or \"\")"
                            : (m_rust ? "self.current_token_text()"
                                      : (m_go ? "p.CurrentTokenText()"
                                      : ((m_csharp || m_cpp17)
                                             ? "currentTokenText()"
                                             : "Parser_CurrentTokenText(pCtx)")));
        }
        else if (name == "code")
        {
            code = m_python ? "(self._errors[-1].code if self._errors else 0)"
                            : (m_rust ? "self.last_error_code()"
                                      : (m_go ? "p.LastErrorCode()"
                                      : ((m_csharp || m_cpp17)
                                             ? "lastErrorCode()"
                                             : "Parser_GetLastError()->code")));
        }
        else if (name == "message")
        {
            code = m_python
                       ? "(self._errors[-1].message if self._errors else \"\")"
                       : (m_rust ? "self.last_error_message()"
                                 : (m_go ? "p.LastErrorMessage()"
                                 : ((m_csharp || m_cpp17)
                                        ? "lastErrorMessage()"
                                        : "Parser_GetLastError()->message")));
        }
        else
        {
            fail("动作 '" + m_action + "'：不认识的 $" + name +
                 "（可用的有 $line / $column / $token / $code / $message）");
            return {};
        }
        ++m_pos;
        ExprNode node;
        node.kind = ExprNode::Kind::Raw;
        node.code = code;
        return node;
    }
    if ((token.size() > 1) && (token[0] == '$') &&
        std::all_of(token.begin() + 1, token.end(), isDigitChar))
    {
        const std::size_t offset = m_tokens[m_pos].start;
        ++m_pos;
        // 手写十进制解析：超大下标（如 $99999999999999999999）只报错，不
        // 触发 std::stoi 的 std::out_of_range；上限与 int 一致
        const int kMaxIndex = 2147483647;
        long long index = 0;
        bool tooLarge = false;
        for (std::size_t i = 1; i < token.size(); ++i)
        {
            const int digit = token[i] - '0';
            if (index > ((kMaxIndex - digit) / 10))
            {
                tooLarge = true;
                break;
            }
            index = (index * 10) + digit;
        }
        if (tooLarge)
        {
            fail("动作 '" + m_action + "'：$" + token.substr(1) +
                 " 下标超出 int 范围；请检查 $n 引用");
            return {};
        }
        ExprNode node;
        node.kind = ExprNode::Kind::Raw;
        node.code = customValueCode(index);
        node.lvKind = ExprNode::LvKind::Slot;
        node.lvOffset = offset;
        return node;
    }
    if (token == "[")
    {
        fail("动作 '" + m_action +
             "'：[$n] 变量引用是内置运行时特有的写法，已随 builtin 运行时"
             "移除；请在 %code ... %endcode 里自行实现变量表" + unionHint());
        return {};
    }
    if (m_tokens[m_pos].kind == "id")
    {
        const std::string ident = token;
        ++m_pos;
        if ((ident == "def") && (peek().value_or("") == "["))
        {
            fail("动作 '" + m_action +
                 "'：def[...] 是内置运行时特有的写法，已随 builtin 运行时"
                 "移除；请在 %code ... %endcode 里自行实现" + unionHint());
            return {};
        }
        if ((ident == "yyerrok") || (ident == "yyclearin"))
        {
            fail("动作 '" + m_action + "'：" + ident +
                 " 只能单独成句（yyerrok; / yyclearin;），不能写在表达式里；"
                 "LR 家族里它们分别「清除错误状态」与「丢弃当前 lookahead」");
            return {};
        }
        if (peek().value_or("") == "(")
        {
            if (actionTypeTable().count(ident) != 0)
            {
                ++m_pos;
                const ExprNode arg = parseTernary();
                expect(")");
                return makeConstructor(ident, arg);
            }
            return makeCustomCall(ident);
        }
        // 未跟 '(' 的标识符按普通 C 标识符 / 常量处理
        ExprNode node;
        node.kind = ExprNode::Kind::Raw;
        node.code = ident;
        return node;
    }
    fail("动作 '" + m_action + "'：无法解析 '" + token + "'");
    return {};
}

ExprNode ActionTranslator::makeCustomCall(const std::string &ident)
{
    std::vector<ExprNode> arguments;
    expect("(");
    if (peek().value_or(")") != ")")
    {
        while (true)
        {
            arguments.push_back(parseTernary());
            if (!accept(","))
            {
                break;
            }
        }
    }
    expect(")");
    std::string joined;
    for (std::size_t index = 0; index < arguments.size(); ++index)
    {
        if (index != 0)
        {
            joined += ", ";
        }
        joined += arguments[index].code;
    }
    ExprNode node;
    node.kind = ExprNode::Kind::Ccall;
    node.code = ident + "(" + joined + ")";
    return node;
}

ExprNode ActionTranslator::makeConstructor(const std::string &dtype,
                                           const ExprNode &arg)
{
    if (m_go)
    {
        // Go 的转换写成 `T(x)`（函数式），参数一律加括号免得优先级意外
        ExprNode node;
        node.kind = ExprNode::Kind::Ccast;
        node.code = goTypeTable().at(dtype) + "((" + arg.code + "))";
        return node;
    }
    if (m_rust)
    {
        // Rust 没有 C 风格的强制转换：写成 `(x as i32)`（操作数一律加括号，
        // 免得 `a + b as i32` 这种优先级意外）
        ExprNode node;
        node.kind = ExprNode::Kind::Ccast;
        node.code = "((" + arg.code + ") as " + rustTypeTable().at(dtype) + ")";
        return node;
    }
    const auto &entry =
        m_csharp ? csharpTypeTable().at(dtype) : actionTypeTable().at(dtype);
    const std::string ctype = entry;
    ExprNode node;
    node.kind = ExprNode::Kind::Ccast;
    node.code = "(" + ctype + ")" +
                ((arg.kind == ExprNode::Kind::Cbin ||
                  arg.kind == ExprNode::Kind::Cun ||
                  arg.kind == ExprNode::Kind::Ctern)
                     ? "(" + arg.code + ")"
                     : arg.code);
    return node;
}

ExprNode ActionTranslator::makeTernary(const ExprNode &condition,
                                       const ExprNode &thenNodeIn,
                                       const ExprNode &elseNodeIn) const
{
    ExprNode thenNode = thenNodeIn;
    ExprNode elseNode = elseNodeIn;
    const auto operand = [](const ExprNode &node)
    {
        if ((node.kind == ExprNode::Kind::Cbin) ||
            (node.kind == ExprNode::Kind::Cun) ||
            (node.kind == ExprNode::Kind::Ctern))
        {
            return "(" + node.code + ")";
        }
        return node.code;
    };
    ExprNode node;
    node.kind = ExprNode::Kind::Ctern;
    node.code = operand(condition) + " ? " + operand(thenNode) + " : " +
                operand(elseNode);
    return node;
}

ExprNode ActionTranslator::makeUnary(const std::string &op,
                                     const ExprNode &nodeIn)
{
    ExprNode node;
    node.kind = ExprNode::Kind::Cun;
    node.code = op + (((nodeIn.kind == ExprNode::Kind::Cbin) ||
                       (nodeIn.kind == ExprNode::Kind::Cun) ||
                       (nodeIn.kind == ExprNode::Kind::Ctern))
                          ? "(" + nodeIn.code + ")"
                          : nodeIn.code);
    return node;
}

ExprNode ActionTranslator::makeBinary(const std::string &op,
                                      const ExprNode &left,
                                      const ExprNode &right) const
{
    return customBinary(op, left, right);
}

} // namespace ParseEngine

namespace ParseEngine
{

std::string ActionTranslator::buildAssignment(const ExprNode &lhs,
                                              const ExprNode &rhs)
{
    if (lhs.lvKind == ExprNode::LvKind::None)
    {
        fail("动作 '" + m_action + "'：'=' 左侧不是可赋值目标");
        return {};
    }
    if (lhs.lvKind == ExprNode::LvKind::Slot)
    {
        // yacc 也不允许给 $n 赋值：右部符号的语义值归它自己
        std::string where = productionWhere();
        if (!where.empty())
        {
            where = "（" + where + "动作里第 " +
                    std::to_string(lhs.lvOffset + 1) + " 个字符）";
        }
        else
        {
            where = "（动作里第 " + std::to_string(lhs.lvOffset + 1) +
                    " 个字符）";
        }
        fail("动作 '" + m_action + "'：$n 不能出现在赋值左边" + where +
             "；请写 $$ = 表达式");
        return {};
    }
    return m_python ? (lhs.code + " = " + rhs.code)
                    : (lhs.code + " = " + rhs.code + ";");
}

bool translateAction(const std::string &action,
                     const std::string &mode, int rhsLength,
                     const UnionValueSpec *unionSpec, bool cpp17, bool python,
                     std::string &out, Error &error, bool csharpStyle,
                     bool rustStyle, bool goStyle)
{
    ActionTranslator translator(action, mode, unionSpec, cpp17, python,
                                csharpStyle, rustStyle, goStyle);
    return translator.translate(mode, rhsLength, out, error);
}

bool translateDestructorBody(const std::string &body, const std::string &member,
                             const std::string &valueExpr, std::string &out,
                             Error &error)
{
    std::vector<ActionTranslator::Token> tokens;
    tokenizeAction(body, tokens);
    std::string result;
    std::size_t position = 0u;
    for (const ActionTranslator::Token &token : tokens)
    {
        if (token.kind != "dollar")
        {
            continue;
        }
        if (token.text != "$$")
        {
            return fail(error,
                        "%destructor <" + member + "> 里只能写 $$（正在被丢弃的"
                        "那个值），不能写 '" + token.text +
                            "'：清理片段没有右部符号可指");
        }
        result.append(body, position, token.start - position);
        result += valueExpr;
        position = token.end;
    }
    result.append(body, position, std::string::npos);
    out = result;
    return true;
}

} // namespace ParseEngine
