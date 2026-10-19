/**
 * @file regex_dfa.hpp
 * @brief 正则到 DFA 的编译器（%token regex / %identifier / %commit 词法用）。
 *
 * 支持的正则语法（UTF-8 文本，字符域是 Unicode 码点）：
 *   - 字面量：任意 UTF-8 字符；元字符 `\ . | ( ) [ ] { } * + ? ^ $` 要转义；
 *   - 预定义类：`\d \D \w \W \s \S \h \H`（`\h` 是十六进制数字）；
 *   - 转义：`\n \r \t \f \v \a \e \0`、`\xHH`、`\uHHHH`、`\u{H…}`（码点）、
 *     `\p{…}` / `\P{…}` 码点属性（L / Lu / Ll / N / Nd / Z / Zs / P / S / C 等）、
 *     `\o{…}`（八进制码点）；
 *   - 字符类 `[...]`：区间、`^` 取反、`[]` 里以字面量出现的 `]`、类内联合
 *     （`[[:alpha:]]` / `[[:^alpha:]]`、`\p{L}`、`\d` 等）
 *   - 上下文语法：`^` / `$`、`(?=...)` / `(?!...)`、`\b` / `\B`、捕获组与数字
 *     反向引用、模式开头的 `(?i)` / `(?u)` / `(?iu)`；由 RegexVm 或目标标准库执行，不能当作空串 DFA；
 *   - 重复 `*` `+` `?` `{m}` `{m,}` `{m,n}`（`m` 上限 k_maxRepeat），以及四种
 *     非贪婪写法 `*?` `+?` `??` `{m,n}?`（最长匹配语义下等价于贪婪版，见
 *     `RegexNode::lazy`）。
 */

#ifndef SCRIPT_ENGINE_REGEX_DFA_HPP
#define SCRIPT_ENGINE_REGEX_DFA_HPP

#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "ParseEngine/error.hpp"

namespace ParseEngine
{

/// 重复次数 `{m,n}` 里 m / n 的上限（超过就报错，避免展开后状态爆炸）。
constexpr int k_maxRepeat = 1000;

/**
 * @brief 正则 AST 节点（class / assertion / empty / concat / alt / star / plus / opt / repeat）。
 */
struct RegexNode
{
    /**
     * @brief 节点类别。
     */
    enum class Kind
    {
        Class,      ///< 字符类，ranges 有效
        Assertion,  ///< 断言（^ / $）
        Empty,      ///< 空串
        Concat,     ///< 连接，left / right 有效
        Alt,        ///< 选择，left / right 有效
        Star,       ///< 闭包，left 有效
        Plus,       ///< 正闭包，left 有效
        Opt,        ///< 可选，left 有效
        Group,      ///< 捕获组，group 为从 1 开始的编号
        LookAhead,  ///< 正前瞻，left 为断言表达式
        NegativeLookAhead, ///< 负前瞻
        WordBoundary, ///< 单词边界
        NotWordBoundary, ///< 非单词边界
        Backref,    ///< 数字反向引用，group 为目标组号
        Repeat,     ///< 有界重复 {m,n}，left 有效，min / max 有效
    };

    /**
     * @brief 断言类别。
     */
    enum class Assertion
    {
        LineStart, ///< `^`
        LineEnd,   ///< `$`
    };

    Kind kind = Kind::Empty;                       ///< 节点类别
    Assertion assertion = Assertion::LineStart;    ///< Assertion 节点的类别
    int group = 0;                                ///< 捕获组或反向引用编号
    bool unicode = false;                          ///< 模式开头 (?u) 的码点语义
    bool ignoreCase = false;                       ///< 大小写标记（字节模式 ASCII，Unicode 模式简单折叠）
    bool lazy = false;                             ///< 非贪婪标记（*? +? ?? {m,n}?）
    std::shared_ptr<RegexNode> left;               ///< 一元/二元子节点
    std::shared_ptr<RegexNode> right;              ///< 二元子节点
    /// Class 节点的码点区间（闭区间，已排序去重，互不重叠）。
    std::vector<std::pair<char32_t, char32_t>> ranges;
    int min = 0;                                   ///< Repeat 节点下限
    int max = 0;                                   ///< Repeat 节点上限（-1 表示无上限）
};

/**
 * @brief 划分出来的一个字符类（一列按码点排序的不相交区间）。
 */
struct RegexCell
{
    char32_t representative = 0;                            ///< 代表码点（用于取转移）
    std::vector<std::pair<char32_t, char32_t>> ranges;      ///< 该类包含的码点区间
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
    std::vector<RegexCell> cells;         ///< 字符类划分（下标即 DFA 字母表）
    std::vector<int> byteMap;             ///< 字节(0..255) -> 字符类编号
    std::vector<CompiledRegex> compiled;  ///< 每条正则的编译结果
};

/**
 * @brief 正则编译器：解析 AST → Thompson NFA → 子集构造 → 划分法最小化。
 *
 * 这是低层语言识别器，返回最长接受前缀，不保留分支顺序、捕获或非贪婪语义。
 * 生成器只在规则的有序语义与 DFA 等价时使用此路径，其余使用有序运行时或目标库。
 */
class RegexDfa
{
public:
    /**
     * @brief 解析一条正则，返回 AST。
     * @param pattern 正则文本（已做过字面量反转义）。
     * @param out 解析出的 AST 根节点（成功时写入）。
     * @param error 出错时写入错误说明。
     * @return 成功返回 true；正则语法有误返回 false。
     */
    static bool parse(const std::string &pattern,
                      std::shared_ptr<RegexNode> &out, Error &error);

    /**
     * @brief 码点区间表（闭区间；已排序，但相邻区间可能还没合并）。
     */
    using RangeList = std::vector<std::pair<char32_t, char32_t>>;

    /**
     * @brief 收集 AST 里出现过的所有码点区间表。
     * @param node 待遍历的 AST 节点。
     * @param found 输出列表，按出现顺序追加。
     */
    static void collectClassSets(const std::shared_ptr<RegexNode> &node,
                                 std::vector<RangeList> &found);

    /**
     * @brief 把若干码点区间表划分成互不相交的字符类。
     *
     * 用「区间端点扫描」切分 0..U+10FFFF：每个端点把码点轴切开，落在同一段里的
     * 码点对各集合的属于关系完全相同，所以一段就是一个字符类。规模只与区间数
     * 成正比（`\p{L}` 这类大属性表也不会炸）。
     * @param classSets 参与划分的码点区间表列表。
     * @return 二元组（字符类列表，字节(0..255) 到类编号的映射）。
     */
    static std::pair<std::vector<RegexCell>, std::vector<int>> charClasses(
        const std::vector<RangeList> &classSets);

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

    /**
     * @brief 把一段 UTF-8 文本按码点编码（无效字节按单字节 U+DC00+值 处理）。
     *
     * 词法器运行时用同一套编码去查 `charMap`（`byteMap` 只看首字节，续字节
     * 一律落进「不可能出现」的类）。
     * @param text UTF-8 文本。
     * @return 码点序列。
     */
    static std::vector<char32_t> decodeUtf8(const std::string &text);
};

} // namespace ParseEngine

#endif // SCRIPT_ENGINE_REGEX_DFA_HPP
