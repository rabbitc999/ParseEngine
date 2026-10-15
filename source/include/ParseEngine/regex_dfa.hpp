/**
 * @file regex_dfa.hpp
 * @brief 简单正则到 DFA 的编译器（%token regex / %identifier 词法用）。
 */

#ifndef SCRIPT_ENGINE_REGEX_DFA_HPP
#define SCRIPT_ENGINE_REGEX_DFA_HPP

#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "ParseEngine/error.hpp"

namespace ParseEngine
{
/**
 * @brief 正则 AST 节点（class / empty / concat / alt / star / plus / opt）。
 */
struct RegexNode
{
    /**
     * @brief 节点类别。
     */
    enum class Kind
    {
        Class,  ///< 字符类，chars 有效
        Empty,  ///< 空串
        Concat, ///< 连接，left / right 有效
        Alt,    ///< 选择，left / right 有效
        Star,   ///< 闭包，left 有效
        Plus,   ///< 正闭包，left 有效
        Opt,    ///< 可选，left 有效
    };

    Kind kind = Kind::Empty;                       ///< 节点类别
    std::shared_ptr<RegexNode> left;               ///< 一元/二元子节点
    std::shared_ptr<RegexNode> right;              ///< 二元子节点
    std::set<char> chars;                          ///< Class 节点的字符集合
};

/**
 * @brief 划分出来的一个字符类。
 */
struct RegexCell
{
    char representative = '\0'; ///< 代表字符（用于取转移）
    std::set<char> chars;       ///< 该类包含的全部字符
};

/**
 * @brief 一条正则编译出的极小 DFA。
 */
struct DfaProgram
{
    std::string pattern;                      ///< 原始正则文本
    int stateCount = 0;                       ///< 状态数（含陷阱状态）
    int start = 0;                            ///< 起始状态编号
    int trap = 0;                             ///< 陷阱状态编号
    std::set<int> accept;                     ///< 接受状态集合
    std::map<int, std::map<int, int>> transitions; ///< [状态][字符类] = 目标状态
};

/**
 * @brief 一条已编译的正则（名字 + 模式 + DFA）。
 */
struct CompiledRegex
{
    std::string name;    ///< Token 名
    std::string pattern; ///< 原始正则文本
    DfaProgram dfa;      ///< 编译结果
};

/**
 * @brief 批量编译结果：共享一份字符类划分。
 */
struct RegexCompileResult
{
    std::vector<RegexCell> cells;         ///< 字符类划分
    std::vector<int> charMap;             ///< 字符编码(0..255) -> 字符类编号
    std::vector<CompiledRegex> compiled;  ///< 每条正则的编译结果
};

/**
 * @brief 简单正则编译器：解析 AST → Thompson NFA → 子集构造 → 划分法最小化。
 */
class RegexDfa
{
public:
    /**
     * @brief 解析一条简单正则，返回 AST。
     * @param pattern 正则文本（已做过字面量反转义）。
     * @param out 解析出的 AST 根节点（成功时写入）。
     * @param error 出错时写入错误说明。
     * @return 成功返回 true；正则语法有误返回 false。
     */
    static bool parse(const std::string &pattern,
                      std::shared_ptr<RegexNode> &out, Error &error);

    /**
     * @brief 收集 AST 里出现过的所有字符集合。
     * @param node 待遍历的 AST 节点。
     * @param found 输出列表，按出现顺序追加。
     */
    static void collectClassSets(const std::shared_ptr<RegexNode> &node,
                                 std::vector<std::set<char>> &found);

    /**
     * @brief 把若干字符集合划分成互不相交的字符类。
     * @param classSets 参与划分的字符集合列表。
     * @return 二元组（字符类列表，字符编码到类编号的映射）。
     */
    static std::pair<std::vector<RegexCell>, std::vector<int>> charClasses(
        const std::vector<std::set<char>> &classSets);

    /**
     * @brief 把一条正则编译成极小 DFA。
     * @param pattern 正则文本。
     * @param cells 共享的字符类划分。
     * @param out 编译出的 DFA（成功时写入）。
     * @param error 出错时写入错误说明。
     * @return 成功返回 true；正则语法有误返回 false。
     */
    static bool buildDfa(const std::string &pattern,
                         const std::vector<RegexCell> &cells, DfaProgram &out,
                         Error &error);

    /**
     * @brief 把多条正则全部编译成 DFA，共用一份字符类划分。
     * @param regexDefs 有序的 (名字, 正则) 列表。
     * @param out 字符类划分与全部编译结果（成功时写入）。
     * @param error 出错时写入错误说明。
     * @return 成功返回 true；某条正则语法有误返回 false。
     */
    static bool compileAll(
        const std::vector<std::pair<std::string, std::string>> &regexDefs,
        RegexCompileResult &out, Error &error);
};

} // namespace ParseEngine

#endif // SCRIPT_ENGINE_REGEX_DFA_HPP
