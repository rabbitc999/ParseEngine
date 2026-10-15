/**
 * @file code_gen.cpp
 * @brief 产物生成的**共用**部分：语义动作分支、分段抽取、#line 回填。
 *
 * C99 后端自己那一套（parser.c / parser.h / parser_token.* / parser_table.c /
 * main.c 的片段生成）在 `code_gen_c99.cpp`（`class CodeGenC99`）。这里留的是
 * 各档后端都要用的：`actionCases`（C99 / C++17 / Python 共用）、
 * `destructorSymbolGroups`（C99 / C++17）、分段标记与 `resolveLineDirectives`
 * （产物拼装完统一回填 #line）。
 */

#include "ParseEngine/code_gen.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "ParseEngine/action_translator.hpp"
#include "ParseEngine/error.hpp"
#include "ParseEngine/grammar_types.hpp"
#include "ParseEngine/utils.hpp"

// 这里生成的 C 片段与「一条 #line 指令怎么拼」都在模板里
// （source/templates/code_gen_*.tmpl），走 library/template 的模板引擎。
#include "template/engine.hpp"

namespace ParseEngine
{
namespace
{

/**
 * @brief 一条产生式的语义动作分支（`code_gen_action_cases.cpp.tmpl` 的数组元素）。
 */
struct ActionCase
{
    int id = 0;           ///< 产生式编号（产物里是 `case Nu:`）
    bool hasLine = false; ///< 动作原文来自 .gram 的某一行：前后各插一条 #line
    int line = 0;         ///< #line 指回 .gram 的那一行
    bool empty = false;   ///< 这条产生式没有语义动作，走默认语句
    std::string action;   ///< 缩进好的动作正文（多行时后续行也带 indent 缩进）
};

} // namespace
} // namespace ParseEngine

// 字段绑定只能写在全局作用域（`TEMPLATE_MODEL` 展开的是显式特化）
TEMPLATE_MODEL(ParseEngine::ActionCase,
               tmpl::field("id", &ParseEngine::ActionCase::id),
               tmpl::field("has_line", &ParseEngine::ActionCase::hasLine),
               tmpl::field("line", &ParseEngine::ActionCase::line),
               tmpl::field("empty", &ParseEngine::ActionCase::empty),
               tmpl::field("action", &ParseEngine::ActionCase::action));

namespace ParseEngine
{
namespace
{

/**
 * @brief 渲染一段「当值用」的模板。
 *
 * 与块级模板的区别：模板文件按仓库惯例以换行收尾，而这里的结果是要**插进别的
 * 产物里当值**的（比如一条 `#line` 指令、一个分段标记），多带一个换行就是产物里
 * 多出来的一行，所以取完把结尾的换行收掉。
 * @param name 模板名（`Utils::loadTemplate` 的名字，不带后缀）。
 * @param values 模板变量。
 * @return 渲染结果；结尾的换行已去掉。
 */
std::string renderValueTemplate(
    const std::string &name,
    std::initializer_list<std::pair<std::string, tmpl::Value>> values)
{
    std::string text = tmpl::renderValues(Utils::loadTemplate(name), values);
    while (!text.empty() && ((text.back() == '\n') || (text.back() == '\r')))
    {
        text.pop_back();
    }
    return text;
}

/**
 * @brief 把字符串里的全部 needle 替换成 replacement。
 * @param text 输入文本。
 * @param needle 被替换文本。
 * @param replacement 替换文本。
 * @return 替换后的文本。
 */
std::string replaceAll(const std::string &text, const std::string &needle,
                       const std::string &replacement)
{
    if (needle.empty())
    {
        return text;
    }
    std::string result;
    std::size_t position = 0;
    while (true)
    {
        const std::size_t found = text.find(needle, position);
        if (found == std::string::npos)
        {
            result += text.substr(position);
            break;
        }
        result += text.substr(position, found - position);
        result += replacement;
        position = found + needle.size();
    }
    return result;
}

/**
 * @brief 生成每条最终产生式对应的语义动作分支（LL / LR 共用）。
 * @param analysis 分析结果。
 * @param translateMode 传给动作翻译器的模式。
 * @param symbolMember 符号到 %union 成员名的映射；没写 %union 时传 nullptr。
 * @param error 出错时写入错误说明。
 * @return C 代码段；动作非法时返回空串并把错误写进 error。
 */
/// #line 里 .gram 路径的占位符（拼装时先留它，收尾时再落地真实路径）。
const char *const k_grammarFilePlaceholder = "@@SE_GRAMMAR_FILE@@";

/// 恢复行 #line 的占位符（拼装时不知道生成文件里的实际行号，回填时才算）。
const char *const k_lineBackPlaceholder = "@@SE_LINE_BACK@@";

/**
 * 动作体在 `case` 块里的缩进（12 空格）。
 *
 * 单独拎出来是因为 `code_gen_action_cases.cpp.tmpl` 里那条"只有缩进"的续行也
 * 用它——模板里写死 12 个空格就成了一行纯空白，编辑器 / 钩子容易把它清掉。
 */
const char *const k_actionIndent = "            ";

std::string generatorActionCases(const AnalysisResult &analysis,
                                 const std::string &translateMode,
                                 const std::map<std::string, std::string>
                                     *symbolMember,
                                 bool cpp17, Error &error)
{
    std::vector<ActionCase> cases;
    for (const Production &production : analysis.productions)
    {
        if (production.transform == "left_empty")
        {
            continue;
        }
        std::string action = production.action;
        const std::size_t begin = action.find_first_not_of(" \t\n\r\v\f");
        const std::size_t end = action.find_last_not_of(" \t\n\r\v\f");
        action = (begin == std::string::npos)
                     ? std::string()
                     : action.substr(begin, end - begin + 1);
        // %union 下 $n / $$ 要按符号绑定的成员名补上「.成员」
        UnionValueSpec unionSpec;
        const UnionValueSpec *unionArg = nullptr;
        if (symbolMember != nullptr)
        {
            unionSpec.symbolMember = symbolMember;
            unionSpec.lhsSymbol = production.lhs;
            unionSpec.slotSymbols = production.rhs;
            unionSpec.productionId = production.id;
            if ((production.transform == "left_continue") &&
                !production.owner.empty())
            {
                // LL 的左递归消除：左递归那一步被搬进了 LEFT_LOOP，$1 是累加
                // 值（符号是左部非终结符），$2 起才对应改写后的右部符号
                unionSpec.lhsSymbol = production.owner;
                unionSpec.slotSymbols.assign(1, production.owner);
                unionSpec.slotSymbols.insert(unionSpec.slotSymbols.end(),
                                             production.rhs.begin(),
                                             production.rhs.end());
            }
            unionArg = &unionSpec;
        }
        std::string translated;
        if (!action.empty())
        {
            // 右部符号个数统一传进去：LR 拿它算槽位，所有模式拿它校验 $n
            // 有没有越界（越界时成员后缀会缺一段，最后变成一句看不懂的 C 错误）
            const bool translatedOk = translateAction(
                action, translateMode,
                static_cast<int>(production.rhs.size()),
                unionArg, cpp17, /*python=*/false, translated, error);
            if (!translatedOk)
            {
                // 动作出错时把出错行补上：翻译器只知道动作原文，产物这边才知道
                // 它在 .gram 里的行号（用户按行号回头改最省事）
                if ((error.line == 0u) && (production.actionLine != 0u))
                {
                    error.line = production.actionLine;
                }
                return {};
            }
        }
        ActionCase item;
        item.id = static_cast<int>(production.id);
        // 没有动作（含增广起始产生式）时走模板里的默认语句：C 用 memset 整体
        // 清零（语义值可能是结构体或联合体，对任何 POD 都成立），C++17 的语义值
        // 可能是联合体，值初始化比 memset 更直接。
        item.empty = action.empty();
        item.hasLine = (production.actionLine != 0u);
        item.line = static_cast<int>(production.actionLine);
        // 动作体按 k_actionIndent 缩进；多行动作的后续行也带这份缩进
        // （那条"只有缩进"的续行由模板的 {{ indent }} 出，模板里不留行尾空白）
        item.action =
            replaceAll(translated, "\n", "\n" + std::string(k_actionIndent));
        cases.push_back(std::move(item));
    }
    // 每条动作前后各插一行 #line 的事在模板里：前面那行指回 .gram 的动作起始行
    // （路径先写占位符，收尾时再落地），后面那行把行号还给 parser.c，行号本身也
    // 先留占位符，由 resolveLineDirectives 按最终产物回填。
    std::string result = tmpl::renderValues(
        Utils::loadTemplate("code_gen_action_cases.cpp"),
        {{"cases", tmpl::encode(cases)},
         {"cpp17", cpp17},
         {"indent", k_actionIndent}});
    // 模板每轮以换行收尾（它就是两条 case 之间的分隔），末尾那个要收掉：
    // 这个返回值是插进别的模板当值用的，原实现的结尾也不带换行。
    if (!result.empty() && (result.back() == '\n'))
    {
        result.pop_back();
    }
    return result;
}


} // namespace

std::string CodeGen::actionCases(
    const AnalysisResult &analysis, const std::string &translateMode,
    const std::map<std::string, std::string> *symbolMember, bool cpp17,
    Error &error)
{
    return generatorActionCases(analysis, translateMode, symbolMember, cpp17,
                                error);
}

std::map<std::string, std::vector<std::pair<std::string, std::size_t>>>
CodeGen::destructorSymbolGroups(
    const AnalysisResult &analysis,
    const std::map<std::string, std::string> *symbolMember,
    const std::map<std::string, std::string> *destructors)
{
    std::map<std::string, std::vector<std::pair<std::string, std::size_t>>>
        groups;
    if ((destructors == nullptr) || destructors->empty() ||
        (symbolMember == nullptr))
    {
        return groups;
    }
    for (const auto &entry : *destructors)
    {
        groups.insert_or_assign(entry.first, std::vector<std::pair<std::string, std::size_t>>{});
    }
    const std::size_t terminalCount = analysis.terminals.size();
    const auto collect = [&groups, symbolMember](const std::string &symbol,
                                                 std::size_t unified)
    {
        const auto memberIt = symbolMember->find(symbol);
        const std::string *member =
            memberIt != symbolMember->end() ? &memberIt->second : nullptr;
        if (member == nullptr)
        {
            return;
        }
        const auto slotsIt = groups.find(*member);
        std::vector<std::pair<std::string, std::size_t>> *slots =
            slotsIt != groups.end() ? &slotsIt->second : nullptr;
        if (slots != nullptr)
        {
            slots->push_back({symbol, unified});
        }
    };
    for (std::size_t index = 0u; index < analysis.terminals.size(); ++index)
    {
        collect(analysis.terminals[index], index);
    }
    for (std::size_t index = 0u; index < analysis.nonterms.size(); ++index)
    {
        collect(analysis.nonterms[index], terminalCount + index);
    }
    return groups;
}

std::string CodeGen::sectionMarker(const std::string &name,
                                   const std::string &edge)
{
    return renderValueTemplate("code_gen_section_marker.txt",
                               {{"name", name}, {"edge", edge}});
}

std::string CodeGen::extractSection(const std::string &text,
                                    const std::string &name)
{
    const std::string begin = sectionMarker(name, "BEGIN");
    const std::string end = sectionMarker(name, "END");
    const std::size_t start = text.find(begin);
    if (start == std::string::npos)
    {
        fatalInternal("缺少分段起始标记：" + begin);
    }
    const std::size_t bodyStart = start + begin.size();
    const std::size_t stop = text.find(end, bodyStart);
    if (stop == std::string::npos)
    {
        fatalInternal("缺少分段结束标记：" + end);
    }
    return text.substr(bodyStart, stop - bodyStart);
}

const char *CodeGen::grammarFilePlaceholder()
{
    return k_grammarFilePlaceholder;
}

const char *CodeGen::lineBackPlaceholder()
{
    return k_lineBackPlaceholder;
}

std::string CodeGen::resolveLineDirectives(const std::string &text,
                                           const std::string &grammarPath,
                                           const std::string &fileName)
{
    // 文法路径按用户给的形式保留（相对就相对），只把 C 字符串里的特殊字符转义；
    // 没写路径时用这个兜底名字（产物里的 #line 总得指向一个存在的文件）
    const std::string path =
        grammarPath.empty() ? std::string("<default_grammar.gram>") : grammarPath;
    std::string escapedPath;
    for (const char ch : path)
    {
        if ((ch == '\\') || (ch == '"'))
        {
            escapedPath += '\\';
        }
        escapedPath += ch;
    }
    const std::string grammarMarker = k_grammarFilePlaceholder;
    const std::string backMarker = k_lineBackPlaceholder;

    std::string result;
    std::size_t begin = 0u;
    std::size_t lineNumber = 1u;
    while (begin <= text.size())
    {
        const std::size_t found = text.find('\n', begin);
        const std::size_t end =
            (found == std::string::npos) ? text.size() : found;
        std::string line = text.substr(begin, end - begin);
        if (line.find(grammarMarker) != std::string::npos)
        {
            line = replaceAll(line, grammarMarker, escapedPath);
        }
        const std::size_t back = line.find(backMarker);
        if (back != std::string::npos)
        {
            // 恢复行：行号 = 这条指令的下一行；文件名换成该产物自己的名字
            line = renderValueTemplate("code_gen_line_back.txt",
                                       {{"line", std::to_string(lineNumber + 1u)},
                                        {"file", fileName}});
        }
        result += line;
        if (found == std::string::npos)
        {
            break;
        }
        result += '\n';
        begin = found + 1u;
        ++lineNumber;
    }
    return result;
}


} // namespace ParseEngine
