/**
 * @file analysis.hpp
 * @brief LL(1) / LL(0) / LL(k) / LR(0) / SLR(1) / LALR(1) / LR(1) / IELR(1) /
 *        LR(k) / SLR(k) / GLR / DD（递归下降）的文法分析、分析表构造与分析报告。
 */

#ifndef SCRIPT_ENGINE_ANALYSIS_HPP
#define SCRIPT_ENGINE_ANALYSIS_HPP

#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "ParseEngine/error.hpp"
#include "ParseEngine/grammar_types.hpp"

namespace ParseEngine
{

/**
 * @brief 判断模式是否属于 LR 家族（使用 ACTION/GOTO 表与 LR 风格语义动作）。
 * @param mode "ll1" / "lr0" / "slr" / "lalr" / "lr1" / "ielr" / "glr"。
 * @return lr0 / slr / lalr / lr1 / ielr / glr 返回 true，其余返回 false。
 */
bool isLrMode(const std::string &mode);

/**
 * @brief 判断模式是否为 GLR（图结构栈、冲突保留到运行时）。
 * @param mode 模式名。
 * @return "glr" 返回 true。
 */
bool isGlrMode(const std::string &mode);

/**
 * @brief 判断模式是否为 DD（带回溯的递归下降）。
 * @param mode 模式名。
 * @return "dd" 返回 true。
 */
bool isDdMode(const std::string &mode);

/**
 * @brief 这一档要不要发「语法树绘图」能力（绘图白名单）。
 *
 * 白名单是**规范 / 近规范 LR 家族**的三档：`lalr` / `lr1` / `ielr`。
 * 判据与产物侧的两条事实有关：这三档共用同一套 LR 运行时（`lalrStates` 那一桶，
 * 见 `AnalysisResult::stateCount()`），而且 `%tree` 之外也能用 `parse_tree()`
 * 单独建一棵树来画。`lr0` / `slr` / `lrk` / `lalrk` / `slrk`、LL 家族与 `dd`
 * 都不发；`glr` 发的是另一种图（GSS + 各结果树，见 `isGlrMode`）。
 *
 * 目前只有 Python 档用它（`CodeGenPython`），别的档要接也照这一条判定。
 * @param mode 模式名。
 * @return "lalr" / "lr1" / "ielr" 返回 true。
 */
bool isTreeDrawMode(const std::string &mode);

/**
 * @brief 是否只导出词法器的模式（`%mode lex`）。
 *
 * lex 模式不做语法分析：文法里可以完全没有产生式；产物里只有词法器与
 * 「看成什么 Token」的演示 CLI，没有解析器。
 * @param mode 归一化后的模式名。
 * @return 是 lex 模式返回 true。
 */
bool isLexMode(const std::string &mode);

/**
 * @brief 模式的展示标签。
 * @param mode "ll1" / "lr0" / "slr" / "lalr" / "lr1" / "ielr" / "glr"。
 * @return "LL(1)" / "LR(0)" / "SLR(1)" / "LALR(1)" / "LR(1)" / "IELR(1)" / "GLR"。
 */
std::string modeLabelText(const std::string &mode);

/**
 * @brief 模式的短名（用于产物里的文字说明）。
 * @param mode "ll1" / "lr0" / "slr" / "lalr" / "lr1" / "ielr" / "glr"。
 * @return "LL" / "LR0" / "SLR" / "LALR" / "LR1" / "IELR" / "GLR"。
 */
std::string modeNameText(const std::string &mode);

/**
 * @brief 归一化模式名：大小写不敏感，括号里可以带 k。
 *
 * 接受三种写法（大小写随意、中间的空格忽略）：
 *
 * - 短名：`ll0` / `ll1` / `llk` / `lr0` / `lr1` / `lrk` / `slr` / `slrk` /
 *   `lalr` / `lalrk` / `ielr`；
 * - 大写短名：`LL1` / `LALR` / `SLRK` …；
 * - 带括号：`LL(0)` / `LL(1)` / `LL(2)` / `LL(k)` / `LR(0)` / `LR(1)` /
 *   `LALR(k)` / `SLR(2)` / `IELR(1)` …
 *
 * 括号里的 k 直接决定展望符长度（`LL(2)` 就是 `llk` + k = 2，不用再写
 * `%lookahead`）；写字母 `k` 表示「用 `%lookahead` / 缺省值」。
 * @param text 用户写的模式名。
 * @param outMode 归一化后的短名（ll0 / ll1 / llk / lr0 / slr / slrk / lalr /
 *        lalrk / lr1 / lrk / ielr）。
 * @param outLookahead 模式名里写明的 k；没写时置 -1（表示「交给 %lookahead」）。
 * @return 认得出来返回 true。
 */
bool normalizeModeName(const std::string &text, std::string &outMode,
                       int &outLookahead);

/**
 * @brief k 元组列编码在 64 位里放得下的最大 k。
 *
 * 列编码是 $\sum_i \mathrm{Token号}_i\cdot(\text{Token 个数})^i$，所以
 * $(\text{Token 个数})^k$ 不能超过 `INT64_MAX`。k 本身没有策略上限
 * （0..INT_MAX 都收），但编码放不下时产物会算错，所以在生成期拦一下并告诉
 * 用户这个文法最多能到多少。
 * @param terminalCount 终结符个数（含 END）。
 * @return 最大 k；终结符个数 ≤ 1 时返回 `INT_MAX`（怎么都不会溢出）。
 */
int lookaheadEncodingMaxK(std::size_t terminalCount);

/// 虚拟起始符号（SLR）。
extern const std::string k_slrStartSymbol;
/// 虚拟起始符号（LR(0)）。
extern const std::string k_lr0StartSymbol;
/// 虚拟起始符号（LALR）。
extern const std::string k_lalrStartSymbol;
/// 虚拟起始符号（规范 LR(1)）。
extern const std::string k_lr1StartSymbol;
/// 虚拟起始符号（IELR）。
extern const std::string k_ielrStartSymbol;
/// 虚拟起始符号（LR(k)）。
extern const std::string k_lrkStartSymbol;
/// 虚拟起始符号（LALR(k)）。
extern const std::string k_lalrkStartSymbol;
/// 虚拟起始符号（SLR(k)）。
extern const std::string k_slrkStartSymbol;
/// 虚拟起始符号（LL(k)）。
extern const std::string k_llkStartSymbol;
/// 虚拟起始符号（GLR）。
extern const std::string k_glrStartSymbol;

/**
 * @brief LR(0) / LALR 项：产生式 + 圆点位置。
 */
struct LrItem
{
    std::string lhs; ///< 左部非终结符
    int prod = 0;    ///< 规则内产生式下标
    int dot = 0;     ///< 圆点位置

    /**
     * @brief 字典序比较（与 Python 元组比较一致）。
     * @param other 另一个项。
     * @return 小于返回 true。
     */
    bool operator<(const LrItem &other) const
    {
        if (lhs != other.lhs)
        {
            return lhs < other.lhs;
        }
        if (prod != other.prod)
        {
            return prod < other.prod;
        }
        return dot < other.dot;
    }

    /**
     * @brief 相等比较。
     * @param other 另一个项。
     * @return 相等返回 true。
     */
    bool operator==(const LrItem &other) const
    {
        return (lhs == other.lhs) && (prod == other.prod) && (dot == other.dot);
    }
};

/**
 * @brief LR(1) 项：LR(0) 项 + 单个 lookahead。
 */
struct Lr1Item
{
    std::string lhs;       ///< 左部非终结符
    int prod = 0;          ///< 规则内产生式下标
    int dot = 0;           ///< 圆点位置
    std::string lookahead; ///< 终结符

    /**
     * @brief 字典序比较（与 Python 元组比较一致）。
     * @param other 另一个项。
     * @return 小于返回 true。
     */
    bool operator<(const Lr1Item &other) const
    {
        if (lhs != other.lhs)
        {
            return lhs < other.lhs;
        }
        if (prod != other.prod)
        {
            return prod < other.prod;
        }
        if (dot != other.dot)
        {
            return dot < other.dot;
        }
        return lookahead < other.lookahead;
    }

    /**
     * @brief 相等比较。
     * @param other 另一个项。
     * @return 相等返回 true。
     */
    bool operator==(const Lr1Item &other) const
    {
        return (lhs == other.lhs) && (prod == other.prod) &&
               (dot == other.dot) && (lookahead == other.lookahead);
    }
};

/**
 * @brief ACTION 表项。
 */
struct LrAction
{
    /**
     * @brief 动作类别。
     */
    enum class Kind
    {
        Shift,  ///< 移进
        Reduce, ///< 归约
        Accept, ///< 接受
    };

    Kind kind = Kind::Shift; ///< 动作类别
    int value = 0;           ///< 移进目标状态或归约产生式编号

    /**
     * @brief 相等比较。
     * @param other 另一个动作。
     * @return 相等返回 true。
     */
    bool operator==(const LrAction &other) const
    {
        return (kind == other.kind) && (value == other.value);
    }

    /**
     * @brief 不等比较。
     * @param other 另一个动作。
     * @return 不等返回 true。
     */
    bool operator!=(const LrAction &other) const { return !(*this == other); }
};

/**
 * @brief 未消解冲突记录。
 */
struct LrConflict
{
    std::string kind;  ///< 冲突种类文本
    int state = 0;     ///< 状态编号
    std::string token; ///< 触发冲突的 Token
    std::string text;  ///< 完整说明文本
};

/**
 * @brief 已消解冲突记录。
 */
struct LrResolution
{
    int state = 0;         ///< 状态编号
    std::string token;     ///< 触发冲突的 Token
    std::string kind;      ///< 冲突种类文本
    std::string reason;    ///< 消解理由
    std::string chosen;    ///< 选择（"移进" / "归约"）
    /// 被放弃的那个动作（归约/归约冲突时用来在警告里说清「丢了谁」）
    std::string discarded;

    /// 默认构造（聚合式初始化也用得到）。
    LrResolution() = default;

    /**
     * @brief 构造一条消解记录（discarded 可省）。
     * @param stateId 状态编号。
     * @param tokenText 触发冲突的 Token。
     * @param kindText 冲突种类文本。
     * @param reasonText 消解理由。
     * @param chosenText 选择的动作。
     * @param discardedText 被放弃的动作；默认空。
     */
    LrResolution(int stateId, std::string tokenText, std::string kindText,
                 std::string reasonText, std::string chosenText,
                 std::string discardedText = std::string())
        : state(stateId), token(std::move(tokenText)),
          kind(std::move(kindText)), reason(std::move(reasonText)),
          chosen(std::move(chosenText)), discarded(std::move(discardedText))
    {
    }
};

/**
 * @brief SLR / LR(0) 状态：一个 LR(0) 项集。
 */
struct SlrState
{
    std::set<LrItem> items; ///< 项集
};

/**
 * @brief LALR / LR(1) / IELR 状态：core -> lookahead 集合。
 *
 * lookahead 集合只有一个元素时就是规范 LR(1) 项。
 */
struct LalrState
{
    std::map<LrItem, std::set<std::string>> items; ///< 项集
};

/**
 * @brief IELR 状态拆分记录（写进报告，说明哪些状态被拆分、为什么）。
 */
struct IelrSplit
{
    int lalrState = 0;          ///< 拆分前的 LALR 状态编号
    std::string token;          ///< 触发拆分的 Token
    std::string kind;           ///< 冲突种类文本
    std::string reason;         ///< 拆分原因
    std::vector<int> parts;     ///< 拆分后的 IELR 状态编号
};

/**
 * @brief 分析结果（LL / LR(0) / SLR / LALR / LR(1) / IELR 共用）。
 */
struct AnalysisResult
{
    std::string mode = "ielr";                       ///< 模式名（ll1 / lr0 / slr / lalr / lr1 / ielr）
    Grammar grammar;                                ///< 最终文法
    std::vector<std::string> terminals;             ///< 终结符
    std::vector<std::string> nonterms;              ///< 非终结符
    std::map<std::string, std::set<std::string>> first;  ///< FIRST 集合
    std::map<std::string, std::set<std::string>> follow; ///< FOLLOW 集合
    bool hasFollow = true;                          ///< LALR / LR(1) 无 FOLLOW
    int lr1StateCount = 0;                          ///< 规范 LR(1) 状态数（LALR / LR(1) / IELR）
    std::vector<Production> productions;            ///< 摊平的产生式
    std::vector<SlrState> slrStates;                ///< LR(0) 项集族（SLR / LR(0)）
    std::vector<LalrState> lalrStates;              ///< LALR / LR(1) / IELR 项集族
    std::map<std::pair<int, std::string>, int> transitions; ///< GOTO 转移
    std::map<std::pair<int, std::string>, LrAction> action; ///< ACTION 表
    /**
     * @brief LR(k) / LALR(k) 的 k；1 表示只看一个 Token（其余模式都是 1）。
     *
     * k > 1 时 ACTION 表分成两层：移进/接受仍在单 Token 的 `action` 里（键是
     * 终结符名，和 k = 1 一样），归约按**k 元组展望符**分开记录：
     * `action` 的键是 `lookaheadKeyOf(...)` 拼出来的 k 元组键，
     * `lookaheadColumns` / `lookaheadColumnIndex` 给出这些键的列号（产物层
     * 按列号发 k 元组查找表）。
     */
    int lookaheadK = 1;
    std::vector<std::vector<std::string>> lookaheadColumns; ///< ACTION 的 k 元组列（仅 k>1）
    std::map<std::string, int> lookaheadColumnIndex; ///< k 元组键 -> 列号（仅 k>1）
    std::map<std::string, std::set<std::string>> firstK; ///< FIRST_k（仅 LR(k) / LALR(k)）
    /**
     * @brief FOLLOW_k（k 元组展望符集合）；只有 SLR(k) / LL(k) 会填。
     *
     * SLR(k) 的归约展望符直接取 FOLLOW_k（这就是 SLR 的定义：只看产生式左部，
     * 不看项里的上下文），LL(k) 的 SELECT_k 也要用它。
     */
    std::map<std::string, std::set<std::string>> followK;
    std::map<std::pair<int, int>, int> gotoTable;   ///< GOTO 表
    std::map<std::pair<std::string, std::string>, int> table; ///< LL(1) 预测分析表
    std::vector<LrConflict> unresolved;             ///< 未消解冲突
    std::vector<LrResolution> resolutions;          ///< 已消解冲突
    std::vector<std::string> warnings;              ///< 生成期警告（如 error 列上的冲突）
    PriorityConfig priority;                        ///< 优先级配置
    int ielrLalrStateCount = 0;                     ///< 拆分前的 LALR 状态数（仅 IELR）
    std::vector<IelrSplit> ielrSplits;              ///< IELR 状态拆分记录
    std::map<std::pair<int, std::string>, std::vector<LrAction>>
        glrAlternatives;                            ///< GLR 冲突单元的完整动作列表
    /**
     * @brief DD：检测到的左递归环路（空表示没有左递归）。
     *
     * 环路按「A ⇒ B ⇒ … ⇒ A」的节点顺序存一份（不含收尾重复的那个 A）。
     * 生成期发现左递归就直接报错，所以这一项只在报告里用得到。
     */
    std::vector<std::string> ddLeftRecursionCycle;
    int lr1MergedStateCount = 0;                    ///< 同心合并后的 LALR 状态数（仅 LR(1)，教学对照）
    bool teach = false;                             ///< 教学模式（%teach / --teach）；开启才输出 [教学提示] / [教学对照] 两节

    /**
     * @brief 状态总数（LR(0) / SLR / LALR / LR(1) / IELR / GLR 通用）。
     * @return 状态个数。
     */
    std::size_t stateCount() const
    {
        if ((mode == "lalr") || (mode == "lr1") || (mode == "ielr") ||
            (mode == "lrk") || (mode == "lalrk") || (mode == "glr") ||
            (mode == "earley") || (mode == "cyk") ||
            (mode == "slrk"))
        {
            return lalrStates.size();
        }
        return slrStates.size();
    }
};

/**
 * @brief 文法分析入口（LL(1) / SLR(1) / LALR(1) / IELR(1) / GLR）。
 */
class Analysis
{
public:
    /**
     * @brief LL(1)：消除直接左递归，算 FIRST/FOLLOW 与预测分析表。
     * @param grammar 原始文法。
     * @param tokens 终结符列表。
     * @param out 输出分析结果（成功时写入）。
     * @param error 出错时写入错误说明。
     * @return 成功返回 true；文法不是 LL(1) 返回 false。
     */
    static bool prepareGrammar(const Grammar &grammar,
                               const std::vector<std::string> &tokens,
                               AnalysisResult &out, Error &error);

    /**
     * @brief LR(0)：规范 LR(0) 项集族 + 「完整项在所有终结符上归约」的 LR(0) 表。
     *
     * 未消解冲突只写进报告的 `[未消解冲突]`，**任何模式下都不中止生成**
     * （表项按 yacc 默认继续写：移进/归约取移进，归约/归约取先到的归约），
     * 所以 `lr0` 不开教学模式也用得上；显式写了 `%expect` 时仍按其语义校验
     * 冲突数。教学模式（`%teach` / `--teach`）只影响报告：多出
     * `[教学提示]` 与 `[教学对照]` 两节。文法很少真能满足 LR(0)，多数
     * 实用文法在这里都会报出移进/归约冲突——这正是 LR(0) 的强度边界，
     * 也是教学要展示的东西。
     * @param grammar 原始文法。
     * @param tokens 终结符列表。
     * @param priority 优先级与冲突消解配置；可为 nullptr。
     * @param teach 是否教学模式（默认否）。
     * @param out 输出分析结果（成功时写入）。
     * @param error 出错时写入错误说明。
     * @return 成功返回 true。
     */
    static bool lr0Analyze(const Grammar &grammar,
                           const std::vector<std::string> &tokens,
                           const PriorityConfig *priority, bool teach,
                           AnalysisResult &out, Error &error);

    /**
     * @brief SLR(1)：LR(0) 项集族 + FOLLOW 集合。
     *
     * 与 `lalr` / `ielr` 一样，默认严格：未消解冲突报错、不产出代码；
     * 开教学模式（`%teach` / `--teach`）后改为「只报不中止」。
     * @param grammar 原始文法。
     * @param tokens 终结符列表。
     * @param priority 优先级与冲突消解配置；可为 nullptr。
     * @param teach 是否教学模式（默认否）。
     * @param out 输出分析结果（成功时写入）。
     * @param error 出错时写入错误说明。
     * @return 成功返回 true；未消解冲突且不允许时返回 false。
     */
    static bool slrAnalyze(const Grammar &grammar,
                           const std::vector<std::string> &tokens,
                           const PriorityConfig *priority, bool teach,
                           AnalysisResult &out, Error &error);

    /**
     * @brief LR(1)：规范 LR(1) 项集族 + 按项自带展望符归约（不做同心合并）。
     *
     * 与 LALR(1) 的唯一区别是不合并同心状态：状态数最多、能力也最强，
     * 适合与 LALR / IELR 对照讲解「合并能省多少状态、代价是什么」。
     * 与 `lr0` 一样，未消解冲突**任何模式下都不中止生成**（只写进报告的
     * `[未消解冲突]`），`%expect` 仍会按其语义校验冲突数；开了教学模式再多出
     * `[教学对照]`（规范 LR(1) 与 LALR 合并后的状态数）。
     * @param grammar 原始文法。
     * @param tokens 终结符列表。
     * @param priority 优先级与冲突消解配置；可为 nullptr。
     * @param teach 是否教学模式（默认否）。
     * @param out 输出分析结果（成功时写入）。
     * @param error 出错时写入错误说明。
     * @return 成功返回 true。
     */
    static bool lr1Analyze(const Grammar &grammar,
                           const std::vector<std::string> &tokens,
                           const PriorityConfig *priority, bool teach,
                           AnalysisResult &out, Error &error);

    /**
     * @brief LALR(1)：LR(1) 项集 + 同心合并 + 精确 lookahead。
     * @param grammar 原始文法。
     * @param tokens 终结符列表。
     * @param priority 优先级与冲突消解配置；可为 nullptr。
     * @param teach 是否教学模式（默认否）。
     * @param out 输出分析结果（成功时写入）。
     * @param error 出错时写入错误说明。
     * @return 成功返回 true；未消解冲突且不允许时返回 false。
     */
    static bool lalrAnalyze(const Grammar &grammar,
                            const std::vector<std::string> &tokens,
                            const PriorityConfig *priority, bool teach,
                            AnalysisResult &out, Error &error);

    /**
     * @brief IELR(1)：LALR 项集 + 冲突驱动拆分，消除合并引入的归约/归约冲突。
     * @param grammar 原始文法。
     * @param tokens 终结符列表。
     * @param priority 优先级与冲突消解配置；可为 nullptr。
     * @param teach 是否教学模式（默认否）。
     * @param out 输出分析结果（成功时写入）。
     * @param error 出错时写入错误说明。
     * @return 成功返回 true；文法不是 LR(1) 时返回 false。
     */
    static bool ielrAnalyze(const Grammar &grammar,
                            const std::vector<std::string> &tokens,
                            const PriorityConfig *priority, bool teach,
                            AnalysisResult &out, Error &error);

    /**
     * @brief GLR：LALR 项集 + 多动作 ACTION 表，冲突保留给运行时。
     *
     * 优先级可消解的移进/归约冲突在生成期消解；其余冲突（包括全部归约/归约
     * 冲突）保留进 glrAlternatives，由生成的 GSS 运行时并行探索。
     * @param grammar 原始文法。
     * @param tokens 终结符列表。
     * @param priority 优先级与冲突消解配置；可为 nullptr。
     * @param out 输出分析结果（成功时写入）。
     * @param error 出错时写入错误说明。
     * @return 成功返回 true。
     */
    static bool glrAnalyze(const Grammar &grammar,
                           const std::vector<std::string> &tokens,
                           const PriorityConfig *priority,
                           AnalysisResult &out, Error &error);

    /**
     * @brief DD：带回溯的递归下降。
     *
     * 与其它模式不同，DD **不在生成期建表**：产物里每个非终结符一个解析函数，
     * 运行时按「非终结符 → 它的产生式列表」逐条试，失败就回退词法器位置与
     * Token 下标再试下一条。这里算出来的东西是报告底料（FIRST / FOLLOW /
     * 摊平的产生式）与运行时要用的产生式元数据（右部长度与右部符号）。
     *
     * 唯一的硬性文法限制是**左递归**（含「跳过可空前缀后仍以自己开头」的
     * 间接形式）：检测到就报错并给出环路，不产出会无限递归的产物。
     * @param grammar 原始文法。
     * @param tokens 终结符列表。
     * @param priority 优先级与冲突消解配置；可为 nullptr（DD 用不上优先级，
     *        只原样记进结果供报告显示）。
     * @param out 输出分析结果（成功时写入）。
     * @param error 出错时写入错误说明。
     * @return 成功返回 true；有左递归返回 false。
     */
    static bool ddAnalyze(const Grammar &grammar,
                          const std::vector<std::string> &tokens,
                          const PriorityConfig *priority, AnalysisResult &out,
                          Error &error);

    /**
     * @brief LR(k)：规范 LR(k) 项集族 + 按 k 元组展望符归约（不做同心合并）。
     *
     * 项的形式是 `[A -> α . β, u]`，u 是长度恰好 k 的终结符串（不足的用
     * `END` 补齐）；移进只依赖下一个 Token，所以 ACTION 表分两层：单 Token
     * 的移进/接受 + 按 k 元组展望符的归约。k = 1 时与 `lr1Analyze` 等价。
     * 与 LR(1) 一样只报不中止冲突（写进报告的 `[未消解冲突]`）。
     * @param grammar 原始文法。
     * @param tokens 终结符列表。
     * @param lookaheadK 展望符长度 k（1..k_maxLookaheadK）。
     * @param priority 优先级与冲突消解配置；可为 nullptr。
     * @param teach 是否教学模式（默认否）。
     * @param out 输出分析结果（成功时写入）。
     * @param error 出错时写入错误说明。
     * @return 成功返回 true。
     */
    static bool lrkAnalyze(const Grammar &grammar,
                           const std::vector<std::string> &tokens, int lookaheadK,
                           const PriorityConfig *priority, bool teach,
                           AnalysisResult &out, Error &error);

    /**
     * @brief LALR(k)：LR(k) 项集 + 同心合并（core 相同则并 k 元组展望符）。
     *
     * 与 `lalrAnalyze` 的关系就是 `lrkAnalyze` 与 `lr1Analyze` 的关系：合并
     * 同心状态能省状态，代价是可能多出归约/归约冲突（LALR(k) 严格弱于
     * LR(k)）。k = 1 时与 `lalrAnalyze` 等价。
     * @param grammar 原始文法。
     * @param tokens 终结符列表。
     * @param lookaheadK 展望符长度 k（1..k_maxLookaheadK）。
     * @param priority 优先级与冲突消解配置；可为 nullptr。
     * @param teach 是否教学模式（默认否）。
     * @param out 输出分析结果（成功时写入）。
     * @param error 出错时写入错误说明。
     * @return 成功返回 true；未消解冲突且不允许时返回 false。
     */
    static bool lalrkAnalyze(const Grammar &grammar,
                             const std::vector<std::string> &tokens,
                             int lookaheadK, const PriorityConfig *priority,
                             bool teach, AnalysisResult &out, Error &error);

    /**
     * @brief SLR(k)：LR(0) 项集 + FOLLOW_k 展望符。
     *
     * 状态与 LR(0) 完全一样（还是那套 LG(0) 项集族），区别只在「完整项什么时候
     * 归约」：SLR(k) 看 FOLLOW_k(左部)，LR(k) 看项自带的 k 元组。所以 SLR(k)
     * 的表也是 LR(k) 那种两层结构（移进 + k 元组归约列），产物与 `lrk` 共用。
     * k = 1 时与 `slrAnalyze` 等价。
     * @param grammar 原始文法。
     * @param tokens 终结符列表。
     * @param lookaheadK 展望符长度 k（1..k_maxLookaheadK）。
     * @param priority 优先级与冲突消解配置；可为 nullptr。
     * @param teach 是否教学模式（默认否）。
     * @param out 输出分析结果（成功时写入）。
     * @param error 出错时写入错误说明。
     * @return 成功返回 true；未消解冲突且不允许时返回 false。
     */
    static bool slrkAnalyze(const Grammar &grammar,
                            const std::vector<std::string> &tokens,
                            int lookaheadK, const PriorityConfig *priority,
                            bool teach, AnalysisResult &out, Error &error);

    /**
     * @brief LL(k)：消除直接左递归 + FIRST_k / FOLLOW_k + SELECT_k 预测分析表。
     *
     * 表项是 `M[A, u] = 产生式`，u 是长度恰好 k 的终结符串（`Select_k(A -> α) =
     * FIRST_k(α · FOLLOW_k(A))`）。同一个 `(A, u)` 对应两条产生式就报「文法不是
     * LL(k)」。k = 1 时与 `prepareGrammar`（LL(1)）等价。
     * @param grammar 原始文法。
     * @param tokens 终结符列表。
     * @param lookaheadK 展望符长度 k（1..k_maxLookaheadK）。
     * @param teach 是否教学模式（LL 没有冲突一说，这里忽略）。
     * @param out 输出分析结果（成功时写入）。
     * @param error 出错时写入错误说明。
     * @return 成功返回 true；文法不是 LL(k) 返回 false。
     */
    static bool llkAnalyze(const Grammar &grammar,
                           const std::vector<std::string> &tokens,
                           int lookaheadK, bool teach, AnalysisResult &out,
                           Error &error);

/**
 * @brief 按生成模式构造分析结果。
 * @param grammar 原始文法。
 * @param tokens 终结符列表。
 * @param mode "ll1" / "lr0" / "slr" / "lalr" / "lr1" / "ielr"。
     * @param priority 优先级与冲突消解配置；可为 nullptr。
     * @param teach 是否教学模式（默认否）；只对确定性 LR 模式生效。
     * @param out 输出分析结果（成功时写入）。
     * @param error 出错时写入错误说明。
     * @param lookaheadK LR(k) / LALR(k) 的 k；其它模式忽略（默认 1）。
     * @return 成功返回 true；文法不满足所选模式时返回 false。
     */
    static bool analyze(const Grammar &grammar,
                        const std::vector<std::string> &tokens,
                        const std::string &mode,
                        const PriorityConfig *priority, bool teach,
                        AnalysisResult &out, Error &error,
                        int lookaheadK = 1);
};

/**
 * @brief 把一条冲突消解记录渲染成一行文本。
 * @param entry 消解记录。
 * @return 形如“状态 1，Token X：移进/归约冲突，按优先级选择移进”的一行文本。
 */
std::string resolutionLine(const LrResolution &entry);

/**
 * @brief 渲染 LL(1) 报告（最终产生式 / FIRST / FOLLOW / 预测分析表）。
 * @param analysis 分析结果。
 * @return 报告文本。
 */
std::string formatGrammarReport(const AnalysisResult &analysis);

/**
 * @brief 渲染 LR(0) 报告（项集族 / ACTION / GOTO / 冲突清单 / 教学对照）。
 * @param analysis 分析结果。
 * @return 报告文本。
 */
std::string formatLr0Report(const AnalysisResult &analysis);

/**
 * @brief 渲染 SLR(1) 报告。
 * @param analysis 分析结果。
 * @return 报告文本。
 */
std::string formatSlrReport(const AnalysisResult &analysis);

/**
 * @brief 渲染 LR(1) 报告（含规范 LR(1) 与 LALR 合并后的状态数对照）。
 * @param analysis 分析结果。
 * @return 报告文本。
 */
std::string formatLr1Report(const AnalysisResult &analysis);

/**
 * @brief 渲染 LALR(1) 报告。
 * @param analysis 分析结果。
 * @return 报告文本。
 */
std::string formatLalrReport(const AnalysisResult &analysis);

/**
 * @brief 渲染 IELR(1) 报告（含状态拆分记录）。
 * @param analysis 分析结果。
 * @return 报告文本。
 */
std::string formatIelrReport(const AnalysisResult &analysis);

/**
 * @brief 渲染 GLR 报告（含保留的冲突清单）。
 * @param analysis 分析结果。
 * @return 报告文本。
 */
std::string formatGlrReport(const AnalysisResult &analysis);

/**
 * @brief 渲染 DD 报告（最终产生式 / FIRST / FOLLOW / 递归下降清单 /
 *        左递归检查 / 回溯代价）。
 * @param analysis 分析结果。
 * @return 报告文本。
 */
std::string formatDdReport(const AnalysisResult &analysis);

/**
 * @brief 渲染 GLR 状态、项集（带 lookahead）、GOTO 转移与保留的冲突。
 * @param analysis 分析结果。
 * @return 报告文本。
 */
std::string formatGlrAutomaton(const AnalysisResult &analysis);

/**
 * @brief 渲染 GLR 的 ACTION / GOTO 矩阵（冲突单元列出全部候选动作）。
 * @param analysis 分析结果。
 * @return 报告文本。
 */
std::string formatGlrTables(const AnalysisResult &analysis);

/**
 * @brief 渲染 LR(0) 的规范项集族与 GOTO 转移（同 SLR 的 LR(0) 项集）。
 * @param analysis 分析结果。
 * @return 报告文本。
 */
std::string formatLr0Automaton(const AnalysisResult &analysis);

/**
 * @brief 渲染 SLR 的 LR(0) 状态、项集与 GOTO 转移。
 * @param analysis 分析结果。
 * @return 报告文本。
 */
std::string formatSlrAutomaton(const AnalysisResult &analysis);

/**
 * @brief 渲染 LR(1) 的状态、项集（每项一个展望符）与 GOTO 转移。
 * @param analysis 分析结果。
 * @return 报告文本。
 */
std::string formatLr1Automaton(const AnalysisResult &analysis);

/**
 * @brief 渲染 LALR 状态、项集（带 lookahead）与 GOTO 转移。
 * @param analysis 分析结果。
 * @return 报告文本。
 */
std::string formatLalrAutomaton(const AnalysisResult &analysis);

/**
 * @brief 渲染 IELR 状态、项集（带 lookahead）、GOTO 转移与拆分记录。
 * @param analysis 分析结果。
 * @return 报告文本。
 */
std::string formatIelrAutomaton(const AnalysisResult &analysis);

/**
 * @brief 渲染 LR(0) 的 ACTION / GOTO 矩阵。
 * @param analysis 分析结果。
 * @return 报告文本。
 */
std::string formatLr0Tables(const AnalysisResult &analysis);

/**
 * @brief 渲染 ACTION / GOTO 矩阵。
 * @param analysis 分析结果。
 * @return 报告文本。
 */
std::string formatSlrTables(const AnalysisResult &analysis);

/**
 * @brief 渲染 LR(1) 的 ACTION / GOTO 矩阵。
 * @param analysis 分析结果。
 * @return 报告文本。
 */
std::string formatLr1Tables(const AnalysisResult &analysis);

/**
 * @brief 渲染 LALR 的 ACTION / GOTO 矩阵。
 * @param analysis 分析结果。
 * @return 报告文本。
 */
std::string formatLalrTables(const AnalysisResult &analysis);

/**
 * @brief 渲染 IELR 的 ACTION / GOTO 矩阵。
 * @param analysis 分析结果。
 * @return 报告文本。
 */
std::string formatIelrTables(const AnalysisResult &analysis);

/**
 * @brief 渲染 LR(k) 报告（生成模式 / 产生式 / FIRST / FIRST_k / ACTION / GOTO / 冲突）。
 * @param analysis 分析结果。
 * @return 报告文本。
 */
std::string formatLrkReport(const AnalysisResult &analysis);

/**
 * @brief 渲染 LALR(k) 报告（多一节「合并前后状态数」）。
 * @param analysis 分析结果。
 * @return 报告文本。
 */
std::string formatLalrkReport(const AnalysisResult &analysis);

/**
 * @brief 渲染 LR(k) 的状态、项集（每项一个 k 元组展望符）与 GOTO 转移。
 * @param analysis 分析结果。
 * @return 报告文本。
 */
std::string formatLrkAutomaton(const AnalysisResult &analysis);

/**
 * @brief 渲染 LALR(k) 状态、项集（带 k 元组 lookahead）与 GOTO 转移。
 * @param analysis 分析结果。
 * @return 报告文本。
 */
std::string formatLalrkAutomaton(const AnalysisResult &analysis);

/**
 * @brief 渲染 LR(k) / LALR(k) 的 ACTION / GOTO 矩阵。
 *
 * ACTION 分两块打印：单 Token 的移进/接受（所有列），以及按 k 元组展望符
 * 的归约（只打有动作的列）。这样 k 很大时也不会刷出「一堆空列」。
 * @param analysis 分析结果。
 * @return 报告文本。
 */
std::string formatLrkTables(const AnalysisResult &analysis);

/**
 * @brief 把 k 元组展望符渲染成可读文本（`a b c`）。
 * @param key `lookaheadKeyOf(...)` 拼出来的键。
 * @return 以空格分隔的文本。
 */
std::string lookaheadKeyText(const std::string &key);

/**
 * @brief 把 k 元组键拆回符号列表（`lookaheadKeyOf` 的逆运算）。
 * @param key 拼接键。
 * @return 符号列表；空键返回空列表。
 */
std::vector<std::string> lookaheadKeySymbols(const std::string &key);

/**
 * @brief 把 k 元组展望符拼成 ACTION 表的键。
 * @param symbols k 个终结符。
 * @return 拼接键（k = 1 时就是那个 Token 名本身）。
 */
std::string lookaheadKeyOf(const std::vector<std::string> &symbols);

/**
 * @brief 渲染 SLR(k) 报告（LR(0) 项集 + FOLLOW_k）。
 * @param analysis 分析结果。
 * @return 报告文本。
 */
std::string formatSlrkReport(const AnalysisResult &analysis);

/**
 * @brief 渲染 SLR(k) 的状态、项集与 GOTO 转移。
 * @param analysis 分析结果。
 * @return 报告文本。
 */
std::string formatSlrkAutomaton(const AnalysisResult &analysis);

/**
 * @brief 渲染 LL(k) 报告（FIRST_k / FOLLOW_k / SELECT_k 预测分析表）。
 * @param analysis 分析结果。
 * @return 报告文本。
 */
std::string formatLlkReport(const AnalysisResult &analysis);

/**
 * @brief 把 k 元组展望符集合渲染成 `a b, c d` 这样的文本。
 * @param keys k 元组键集合。
 * @return 逗号分隔文本。
 */
std::string lookaheadSetText(const std::set<std::string> &keys);

/**
 * @brief 每一行「期望哪些终结符」：语法错误时告诉用户「这里期望 X / Y 之一」。
 *
 * 行号的含义跟产物里的表一致——LR 家族是状态号，LL 家族是非终结符编号。
 * 取法是「这一行里所有有动作（移进或归约）的列」：
 *
 * - 移进列直接就是那个 Token；
 * - k ≥ 2 的归约列取 k 元组的**首个** Token（因为下一个 Token 必须先对得上）；
 * - `error` 列不算（那是运行时的恢复伪 Token，用户写不出来）；
 * - LL(0) 只有 ε 一列，没法说「期望哪个 Token」，返回空表。
 * @param analysis 分析结果。
 * @return 每行一个有序去重列表。
 */
std::vector<std::vector<std::string>> expectedTokensByRow(
    const AnalysisResult &analysis);

/**
 * @brief 期望集为空时的兜底说明（五档产物逐字一致）。
 *
 * LL 家族（`ll0` / `ll1` / `llk`）说「该状态下无可用产生式」，LR 家族
 * 说「该状态下无可用动作」；生成期把它写成产物里的常量，运行时
 * 只在「这一行一个动作都没有」时用。
 * @param mode 生成模式（`AnalysisResult::mode`）。
 * @return 兜底说明（UTF-8，静态字符串）。
 */
const char *expectedFallbackText(const std::string &mode);

} // namespace ParseEngine

#endif // SCRIPT_ENGINE_ANALYSIS_HPP
