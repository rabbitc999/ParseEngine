/**
 * @file action_translator.hpp
 * @brief 语义动作翻译：把 $$ / $n 等写法翻译成普通 C 语句。
 */

#ifndef SCRIPT_ENGINE_ACTION_TRANSLATOR_HPP
#define SCRIPT_ENGINE_ACTION_TRANSLATOR_HPP

#include <optional>
#include <set>
#include <string>
#include <vector>

#include "ParseEngine/error.hpp"
#include "ParseEngine/grammar_types.hpp"

namespace ParseEngine
{

/**
 * @brief 动作表达式节点。
 *
 * code 是普通 C 表达式文本；lvalue 描述该节点能否作为赋值左值。
 */
struct ExprNode
{
    /**
     * @brief 节点类别。
     */
    enum class Kind
    {
        Raw,    ///< C 标量表达式 / 标识符 / 函数实参
        Cbin,   ///< 二元运算
        Cun,    ///< 一元运算
        Ctern,  ///< 三元运算
        Cfield, ///< 成员访问
        Ccall,  ///< 函数调用
        Ccast,  ///< 类型转换
    };

    /**
     * @brief 左值类别。
     */
    enum class LvKind
    {
        None,   ///< 不是左值
        Result, ///< $$
        Slot,   ///< $n
    };

    Kind kind = Kind::Raw;       ///< 节点类别
    std::string code;            ///< C 表达式文本
    LvKind lvKind = LvKind::None; ///< 左值类别
    std::size_t lvOffset = 0;    ///< $n 在动作原文里的字节偏移（报错定位用）
};

/**
 * @brief %union 的按符号成员表（每条产生式一份）。
 *
 * 生成器在翻译每条产生式之前填好：符号名到 %union 成员名的映射、左部符号
 * （供 $$ 用）、以及「第 n 个 $ 对应哪个符号」（供 $n 用）。symbolMember 为空
 * 表示文法没写 %union，翻译器生成裸的 values[n] / result.value。
 */
struct UnionValueSpec
{
    const std::map<std::string, std::string> *symbolMember = nullptr; ///< 符号 -> 成员
    std::string lhsSymbol;                ///< 左部符号（$$ 用）
    std::vector<std::string> slotSymbols; ///< 第 n 个 $ 对应的符号（0 起）
    int productionId = -1;                ///< 产生式编号（报错用；< 0 表示未知）
};

/**
 * @brief 把 Yacc 风格动作表达式翻译为 C 语句的翻译器。
 *
 * 语义值类型由用户提供：`$n` 展开成分析栈里的语义槽位，`$$` 展开成
 * `result.value`，其余按普通 C 表达式生成。
 */
class ActionTranslator
{
public:
    /**
     * @brief 构造翻译器并做词法分析。
     * @param action 动作文本。
     * @param mode "ll1" / "lr0" / "slr" / "lalr" / "lr1" / "ielr"。
     * @param unionSpec %union 成员表；没写 %union 时传 nullptr。
     * @param cpp17 目标语言是 C++17 时置 true（yyerrok / yyclearin 按成员函数写）。
     * @param python 目标语言是 Python 时置 true：$n 按 Python 列表下标展开、
     *               yyerrok / yyclearin 调解析器的方法、语句不带分号，
     *               二元运算额外接受 and / or（not 作一元运算）。
     * @param csharpStyle 目标语言是 C# 时置 true：$n 按 0 起 / 1 起的
     *               列表下标展开（与 Python 一致）、语句带分号、`yyerrok` /
     *               `yyclearin` 调 `yyErrOk()` / `yyClearIn()`、
     *               `$line` 这些错误现场信息调解析器的方法、`int32(x)` 这类
     *               类型构造映射成 C# 的基础类型。
     * @param rustStyle 目标语言是 Rust 时置 true：$n 按 0 起 / 1 起的
     *               列表下标展开（与 Python / C# 一致）、语句带分号、
     *               `yyerrok` / `yyclearin` 调 `self.yy_err_ok()` /
     *               `self.yy_clearin()`、`$line` 这些错误现场信息调解析器的
     *               方法、`int32(x)` 这类类型构造映射成 `((x) as i32)`。
     * @param goStyle 目标语言是 Go 时置 true：$n / $$ 的展开与 rustStyle
     *               相同（`values[...]` / `result.value`、语句带分号）、
     *               `yyerrok` / `yyclearin` 调接收者固定叫 `p` 的方法
     *               （`p.YyErrOk()` / `p.YyClearIn()`）、`$line` 这些错误现场
     *               信息调 `p.CurrentLine()` 这类方法、`int32(x)` 这类类型
     *               构造映射成 Go 的函数式转换 `int32((x))`。
     */
    ActionTranslator(std::string action, std::string mode,
                     const UnionValueSpec *unionSpec = nullptr,
                     bool cpp17 = false, bool python = false,
                     bool csharpStyle = false, bool rustStyle = false,
                     bool goStyle = false);

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
     * @brief 翻译整条动作。
     * @param mode 覆盖构造时的模式；为空表示沿用构造参数。
     * @param rhsLength 右部长度（仅 LR 模式有意义）。
     * @param out 可直接写入生成的 C 文件的语句文本（成功时写入）。
     * @param error 出错时写入错误说明。
     * @return 成功返回 true；动作里有非法写法返回 false。
     */
    bool translate(const std::string &mode, int rhsLength, std::string &out,
                   Error &error);

    /**
     * @brief 动作词法单元。
     */
    struct Token
    {
        std::string kind;      ///< num / str / id / dollar / op
        std::string text;      ///< 原文
        std::size_t start = 0; ///< 起始偏移
        std::size_t end = 0;   ///< 结束偏移
    };

private:
    /**
     * @brief 取当前 token 原文。
     * @return token 原文；越界返回空 optional。
     */
    std::optional<std::string> peek() const;
    /**
     * @brief 若当前位置是指定文本则前进。
     * @param text 期望文本。
     * @return 命中并消耗返回 true。
     */
    bool accept(const std::string &text);
    /**
     * @brief 要求当前位置是指定文本。
     * @param text 期望文本。
     */
    void expect(const std::string &text);
    /**
     * @brief 去重输出生成期警告。
     * @param message 警告文本。
     */
    void warn(const std::string &message);
    /**
     * @brief $index 对应的 C 语义值表达式。
     * @param index 下标。
     * @return 表达式。
     */
    std::string customValueCode(int index) const;
    /**
     * @brief 逐条翻译成普通 C 语句。
     * @return 语句文本。
     */
    std::string translateStatements();
    /**
     * @brief 二元运算。
     * @param op 运算符。
     * @param left 左节点。
     * @param right 右节点。
     * @return 结果节点。
     */
    ExprNode customBinary(const std::string &op, const ExprNode &left,
                          const ExprNode &right) const;
    /**
     * @brief yyerrok / yyclearin 伪函数：翻译成运行时的恢复控制调用。
     * @param ident "yyerrok" 或 "yyclearin"。
     * @return 生成的 C 语句。
     */
    std::string recoveryCall(const std::string &ident);
    /**
     * @brief ternary := binary ('?' ternary ':' ternary)?
     * @return 结果节点。
     */
    ExprNode parseTernary();
    /**
     * @brief Pratt 二元表达式解析。
     * @param minPrec 最低优先级。
     * @return 结果节点。
     */
    ExprNode parseBinary(int minPrec);
    /**
     * @brief 一元前缀：! - + ~
     * @return 结果节点。
     */
    ExprNode parseUnary();
    /**
     * @brief 后缀字段访问：$1.成员名。
     * @return 结果节点。
     */
    ExprNode parsePostfix();
    /**
     * @brief 原子：数字、$$、$n、类型构造、函数调用、括号。
     * @return 结果节点。
     */
    ExprNode parsePrimary();
    /**
     * @brief 普通 C 函数调用。
     * @param ident 函数名。
     * @return 结果节点。
     */
    ExprNode makeCustomCall(const std::string &ident);
    /**
     * @brief 按括号配对称取整段调用文本。
     * @param ident 函数名。
     * @return 调用文本。
     */
    std::string consumeCall(const std::string &ident);
    /**
     * @brief 类型构造 int32(0) / float($1) -> C 强制转换。
     * @param dtype 目标类型。
     * @param arg 参数节点。
     * @return 结果节点。
     */
    ExprNode makeConstructor(const std::string &dtype, const ExprNode &arg);
    /**
     * @brief 三元表达式。
     * @param condition 条件节点。
     * @param thenNode 真分支。
     * @param elseNode 假分支。
     * @return 结果节点。
     */
    ExprNode makeTernary(const ExprNode &condition, const ExprNode &thenNode,
                         const ExprNode &elseNode) const;
    /**
     * @brief 一元运算符。
     * @param op 运算符。
     * @param node 子节点。
     * @return 结果节点。
     */
    ExprNode makeUnary(const std::string &op, const ExprNode &node);
    /**
     * @brief 二元运算符。
     * @param op 运算符。
     * @param left 左节点。
     * @param right 右节点。
     * @return 结果节点。
     */
    ExprNode makeBinary(const std::string &op, const ExprNode &left,
                        const ExprNode &right) const;
    /**
     * @brief 根据左值的种类生成赋值语句。
     * @param lhs 左值节点。
     * @param rhs 右值节点。
     * @return 生成的 C 语句。
     */
    std::string buildAssignment(const ExprNode &lhs, const ExprNode &rhs);

    /**
     * @brief 记录第一个错误（错误槽已满时不覆盖）。
     *
     * 报错点里有 const 成员（$n / $$ 的 %union 成员缺失就是），所以错误槽
     * 是 mutable 的。
     * @param message 错误说明。
     */
    void fail(const std::string &message) const
    {
        ParseEngine::fail(m_error, message);
    }

    /**
     * @brief 语义值是否要按 %union 成员展开。
     * @return 写了 %union 返回 true。
     */
    bool unionMode() const;
    /**
     * @brief 查符号绑定的 %union 成员。
     * @param symbol 符号名。
     * @return 成员名；没绑定（或没写 %union）返回 nullptr。
     */
    const std::string *memberOf(const std::string &symbol) const;
    /**
     * @brief $n 的「.成员」后缀。
     * @param index 下标（从 1 开始）。
     * @return 形如 ".ival" 的后缀；没写 %union 时返回空串。
     */
    std::string slotMemberSuffix(int index) const;
    /**
     * @brief $$ 的「.成员」后缀（取左部符号绑定的成员）。
     * @return 形如 ".ival" 的后缀；没写 %union 时返回空串。
     */
    std::string lhsMemberSuffix() const;
    /**
     * @brief 当前产生式的描述文本（报错定位用）。
     * @return 形如 "产生式 P3" 的文本；编号未知时返回空串。
     */
    std::string productionWhere() const;
    /**
     * @brief 写了 %union 时给内置写法的补充提示。
     * @return 提示文本；没写 %union 时返回空串。
     */
    std::string unionHint() const;

    std::string m_action;         ///< 动作原文
    std::string m_mode;           ///< 翻译模式
    bool m_slrMode = false;       ///< 是否 LR 模式
    int m_rhsLength = 0;          ///< 右部长度
    /**
     * @brief 这条产生式右部有几个符号（$n 的上界；0 = 调用方没给，不校验）。
     *
     * 与 m_rhsLength 的区别：m_rhsLength 只对 LR 有意义（算槽位下标要用），
     * 这个是**任何模式**都传的右部符号个数，用来拦 `$3` 写在只有两个符号的
     * 规则里这种越界引用——不拦的话生成出来的 C 会在成员后缀那里悄悄少一段，
     * 最后以「类型不兼容」这种和文法没关系的信息报出来。
     */
    int m_slotCount = 0;
    std::vector<Token> m_tokens;  ///< 词法单元
    std::size_t m_pos = 0;        ///< 当前下标
    std::set<std::string> m_warned; ///< 已输出的警告
    const UnionValueSpec *m_unionSpec = nullptr; ///< %union 成员表；可为空
    bool m_union = false;         ///< 是否按 %union 成员展开 $n / $$
    bool m_cpp17 = false;         ///< 目标语言是否 C++17
    bool m_python = false;        ///< 目标语言是否 Python
    bool m_csharp = false;        ///< 目标语言是否 C#（带分号、值下标 0 起）
    bool m_rust = false;          ///< 目标语言是否 Rust（带分号、值下标 0 起）
    bool m_go = false;            ///< 目标语言是否 Go（接收者 p、类型转换 T(x)）
    mutable Error m_error;        ///< 第一个错误
};

/**
 * @brief 对外入口：把一条动作文本翻译成可生成的 C 语句。
 * @param action 动作文本。
 * @param mode "ll1" / "lr0" / "slr" / "lalr" / "lr1" / "ielr"。
 * @param rhsLength 右部长度（仅 LR 模式有意义）。
 * @param unionSpec %union 成员表；没写 %union 时传 nullptr。
 * @param cpp17 目标语言是 C++17 时置 true。
 * @param python 目标语言是 Python 时置 true（与 cpp17 互斥）。
 * @param out 生成的语句（成功时写入）。
 * @param error 出错时写入错误说明。
 * @param csharpStyle 目标语言是 C# 时置 true（与 cpp17 / python 互斥）；
 *        放在参数表末尾并带缺省值，现有调用点不用改。
 * @param rustStyle 目标语言是 Rust 时置 true（与 cpp17 / python / C# 互斥）；
 *        同样放在参数表末尾并带缺省值，现有调用点不用改。
 * @param goStyle 目标语言是 Go 时置 true（与 cpp17 / python / C# / Rust 互斥）；
 *        同样放在参数表末尾并带缺省值，现有调用点不用改。
 * @return 成功返回 true；动作里有非法写法返回 false。
 */
bool translateAction(const std::string &action, const std::string &mode,
                     int rhsLength, const UnionValueSpec *unionSpec,
                     bool cpp17, bool python, std::string &out, Error &error,
                     bool csharpStyle = false, bool rustStyle = false,
                     bool goStyle = false);

/**
 * @brief 把 %destructor 的清理片段展开成 C 语句（$$ → 正在被丢弃的值）。
 *
 * %destructor 的花括号正文和语义动作一样是「原样收集」的 C 语句，但这里
 * 没有右部符号：只认 $$（展开成 valueExpr）、$<成员>$ 之外的 $n / $line
 * 之类写着没有意义，遇到就报错。字符串字面量里的 $$ 原样保留。
 * @param body %destructor 的花括号正文。
 * @param member 绑定的 %union 成员名（报错文案用）。
 * @param valueExpr 替换 $$ 的 C 表达式（如 "pValue->str"）。
 * @param out 展开后的语句（成功时写入）。
 * @param error 出错时写入错误说明。
 * @return 成功返回 true；片段里写了 $$ 以外的伪变量返回 false。
 */
bool translateDestructorBody(const std::string &body, const std::string &member,
                             const std::string &valueExpr, std::string &out,
                             Error &error);

} // namespace ParseEngine

#endif // SCRIPT_ENGINE_ACTION_TRANSLATOR_HPP
