/**
 * @file token_dict.cpp
 * @brief `%token NAME include "csv"` 展开实现。
 *
 * 流程：
 *
 *   1. 按行扫预处理后的文本（%if 假分支已被 Preprocessor 丢掉），识别：
 *      a) 已声明的 Token 名（`%token X` / `%define X` / `%token regex X`），
 *         用于「别名 == 已声明 Token 名」的冲突检查；
 *      b) `%token ... include "csv"` 指令（位置 / Token 名 / union 标签 /
 *         CSV 路径）；
 *      c) 手写的字面量声明（`%token X "lit"` / `%define X "lit"`），用于
 *         与 include 合并——现有文法里同一个 Token 的多个字面量必须写在
 *         一条指令里（`"x|y"`），多条会报「Token X 重复定义」，所以展开
 *         时要把「手写字面量 + CSV 字面量」合并成一条声明。
 *
 *   2. 按规范化路径缓存读 CSV（同一 `.gram` 内多处引用同一份只读一次）：
 *      每行取前两列，第 1 列 = Token 名、第 2 列 = 字面量（双引号字符串），
 *      第 3 列及以后忽略；空行与 `#` 注释行跳过；表头行（第 1 列对不上任何
 *      被导入的 Token 名）自然被忽略。RFC 4180 最小子集：双引号包裹、字段
 *      内逗号、`""` 表示一个 `"`；不支持跨行字段。
 *
 *   3. 全量唯一性扫描（一次收齐再判，避免「先展开后到」的顺序问题）：
 *      - 同一 Token 下重复字面量：静默去重，保留首次出现顺序；
 *      - 不同 Token 用同一字面量：报错，错误指向 CSV 行与首次分配处；
 *      - 字面量与已声明 Token 名相同：报错；
 *      - CSV 第 1 列不在「被导入 Token 名集合」：整行忽略（不报错）。
 *
 *   4. 展开：对每个被 include 的 Token X，把它的全部字面量来源（手写行 +
 *      include 行）按出现顺序合并成一条 `%token [<tag>] X "lit1|lit2|..."`
 *      （alias 如果第一条来源行带了就保留），写在**第一条来源行**的位置；
 *      其余来源行删除。字面量按现有 `%define` 规则转义（`|` → `\|`、
 *      `\` → `\\`、`"` → `\"`），交给 GramParser 后与手写
 *      `%token X "lit1|lit2"` 完全等价。
 *
 * 没有任何 include 时文本原样返回（与输入逐字节相同），保证不用此特性的
 * 文法产物不变。
 */

#include "ParseEngine/token_dict.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "ParseEngine/grammar_types.hpp"

namespace ParseEngine
{

namespace
{

/**
 * @brief 是否是 Token 名首字符（与 .gram 词法的 isIdStart 一致）。
 */
bool isNameStart(char ch)
{
    return std::isalpha(static_cast<unsigned char>(ch)) || (ch == '_') ||
           (ch == '%');
}

/**
 * @brief Token 名后续字符。
 */
bool isNamePart(char ch)
{
    return std::isalnum(static_cast<unsigned char>(ch)) || (ch == '_');
}

/**
 * @brief 去掉首尾空白。
 */
std::string stripText(const std::string &text)
{
    const std::string spaces = " \t\n\r\v\f";
    const std::size_t begin = text.find_first_not_of(spaces);
    if (begin == std::string::npos)
    {
        return std::string();
    }
    const std::size_t end = text.find_last_not_of(spaces);
    return text.substr(begin, end - begin + 1u);
}

/**
 * @brief 按行切分，保留每行原本的行尾（`\n` 或 `\r\n`）。
 *
 * 与 Preprocessor::splitLines 一致：保留行尾是「没有 include 时与原文本
 * 逐字节相同」的必要条件。
 */
std::vector<std::string> splitLines(const std::string &text)
{
    std::vector<std::string> lines;
    std::size_t index = 0u;
    while (index < text.size())
    {
        const std::size_t found = text.find('\n', index);
        if (found == std::string::npos)
        {
            lines.push_back(text.substr(index));
            break;
        }
        lines.push_back(text.substr(index, found - index + 1u));
        index = found + 1u;
    }
    return lines;
}

/**
 * @brief 去掉一行的行尾换行（`\n` / `\r\n`）。
 */
std::string trimLineEnd(const std::string &line)
{
    std::size_t end = line.size();
    while ((end > 0u) &&
           ((line[end - 1u] == '\n') || (line[end - 1u] == '\r')))
    {
        --end;
    }
    return line.substr(0u, end);
}

/**
 * @brief 把一个已解码的字面量按 `%define` 规则编码（`|` → `\|`、
 *        `\` → `\\`、`"` → `\"`），不带外层引号。
 */
std::string encodeLiteralBody(const std::string &value)
{
    std::string result;
    result.reserve(value.size());
    for (const char ch : value)
    {
        if (ch == '\\')
        {
            result += "\\\\";
        }
        else if (ch == '"')
        {
            result += "\\\"";
        }
        else if (ch == '|')
        {
            result += "\\|";
        }
        else
        {
            result += ch;
        }
    }
    return result;
}

/**
 * @brief 解码手写字面量声明 `"x\|y"` 为候选列表。
 *
 * 与 GrammarLoader::splitDefineLiteral 同一套规则（`|` 分隔候选、`\|` 是
 * 普通竖线、`\n` / `\r` / `\t` / `\v` / `\f` / `\b` / `\0` / `\\` / `\"`
 * 按 C 转义、其它 `\x` → `x`）。解码失败（孤立 `\` / 空分支）时返回
 * false——此时该行保留原样交给 GramParser 报它自己的错，TokenDict 不掺和。
 *
 * @param quotedText 带外层双引号的字面量原文。
 * @param out 解码后的候选列表。
 * @return 成功返回 true。
 */
bool decodeLiteralCandidates(const std::string &quotedText,
                             std::vector<std::string> &out)
{
    out.clear();
    if ((quotedText.size() < 2u) || (quotedText.front() != '"') ||
        (quotedText.back() != '"'))
    {
        return false;
    }
    const std::string body =
        quotedText.substr(1u, quotedText.size() - 2u);
    std::vector<std::string> branches;
    std::string current;
    std::size_t index = 0u;
    while (index < body.size())
    {
        const char ch = body[index];
        if (ch == '\\')
        {
            if (index + 1u >= body.size())
            {
                return false; // 孤立转义符
            }
            const char next = body[index + 1u];
            switch (next)
            {
            case 'n': current += '\n'; break;
            case 'r': current += '\r'; break;
            case 't': current += '\t'; break;
            case 'v': current += '\v'; break;
            case 'f': current += '\f'; break;
            case 'b': current += k_blankWildcardChar; break;
            case '0': current += '\0'; break;
            case '\\': current += '\\'; break;
            case '"': current += '"'; break;
            case '|': current += '|'; break;
            default: current += next; break;
            }
            index += 2u;
            continue;
        }
        if (ch == '|')
        {
            if (current.empty())
            {
                return false; // 空分支
            }
            branches.push_back(current);
            current.clear();
        }
        else
        {
            current += ch;
        }
        ++index;
    }
    if (current.empty())
    {
        return false;
    }
    branches.push_back(current);
    out = branches;
    return true;
}

// ---------------------------------------------------------------------
// 行内词法
// ---------------------------------------------------------------------

/**
 * @brief 一行内的 token：只识别 %token / %define 指令需要看到的形态。
 *
 * kind 取值：`id` / `str`（带引号字面量）/ `lt`（<）/ `gt`（>）/ `comma`。
 * 空白与注释跳过；遇到别的字符停住（kind 为空串），由外层决定怎么处理。
 */
struct LineToken
{
    std::string kind;  ///< id / str / lt / gt / comma；空 = 不可识别
    std::string text;  ///< 原文
    std::size_t start = 0u; ///< 在行内的起始偏移
    std::size_t end = 0u;   ///< 在行内的结束偏移（exclusive）
};

/**
 * @brief 把一行切成 LineToken 序列。
 */
std::vector<LineToken> tokenizeLine(const std::string &line)
{
    std::vector<LineToken> out;
    std::size_t index = 0u;
    while (index < line.size())
    {
        const char ch = line[index];
        if ((ch == ' ') || (ch == '\t') || (ch == '\r'))
        {
            ++index;
            continue;
        }
        if (ch == '\n')
        {
            break;
        }
        if ((ch == '/') && (index + 1u < line.size()) &&
            (line[index + 1u] == '/'))
        {
            break; // 行注释
        }
        if ((ch == '/') && (index + 1u < line.size()) &&
            (line[index + 1u] == '*'))
        {
            const std::size_t end = line.find("*/", index + 2u);
            index = (end == std::string::npos) ? line.size() : end + 2u;
            continue;
        }
        if (ch == ',')
        {
            out.push_back({"comma", ",", index, index + 1u});
            ++index;
            continue;
        }
        if (ch == '<')
        {
            out.push_back({"lt", "<", index, index + 1u});
            ++index;
            continue;
        }
        if (ch == '>')
        {
            out.push_back({"gt", ">", index, index + 1u});
            ++index;
            continue;
        }
        if ((ch == '"') || (ch == '\''))
        {
            const char quote = ch;
            std::size_t end = index + 1u;
            while (end < line.size())
            {
                if (line[end] == '\\')
                {
                    if (end + 1u >= line.size())
                    {
                        break;
                    }
                    end += 2u;
                    continue;
                }
                if (line[end] == quote)
                {
                    ++end;
                    break;
                }
                ++end;
            }
            out.push_back({"str", line.substr(index, end - index), index, end});
            index = end;
            continue;
        }
        if (isNameStart(ch))
        {
            std::size_t end = index + 1u;
            while ((end < line.size()) && isNamePart(line[end]))
            {
                ++end;
            }
            out.push_back({"id", line.substr(index, end - index), index, end});
            index = end;
            continue;
        }
        // 不可识别字符
        out.push_back({"", std::string(1, ch), index, index + 1u});
        ++index;
    }
    return out;
}

// ---------------------------------------------------------------------
// 扫描数据结构
// ---------------------------------------------------------------------

/**
 * @brief 一个 Token 的一条「字面量来源」：手写声明或 include 指令。
 *
 * 合并时同一 Token 的全部来源按 lineStart 排序；第一条来源行的区间被
 * 替换为合并后的声明，其余来源行的区间被删除。
 */
struct SourceDecl
{
    std::size_t lineStart = 0u;     ///< 替换区间起始（指向 `%token` / `%define` 的 `%`）
    std::size_t lineEnd = 0u;       ///< 替换区间结束（行尾，不含换行）
    std::size_t sourceLine = 0u;    ///< 在预处理后文本里的 1-based 行号
    std::string tokenName;          ///< Token 名
    std::string unionTag;           ///< `<成员>` 的成员名；空 = 没写
    std::string aliasRaw;           ///< `alias "..."` 原文（不带 alias 关键字）；空 = 没写
    bool isInclude = false;         ///< true = include 指令；false = 手写字面量声明
    std::vector<std::string> handwrittenCandidates; ///< 手写：解码后的候选
    std::string csvPath;            ///< include：指令里写的路径（去引号）
    std::filesystem::path resolvedCsvPath; ///< include：解析后的绝对路径
    bool decoded = false;           ///< 手写：字面量是否解码成功
};

/**
 * @brief CSV 解析后的单行（前两列）。
 */
struct CsvRow
{
    std::string tokenName;          ///< 第 1 列
    std::string literal;            ///< 第 2 列（引号内的内容，`""` 已还原）
    std::size_t sourceLine = 0u;    ///< CSV 文件里的 1-based 行号
    std::size_t literalColumn = 0u; ///< 第 2 列起始的 1-based 列号
};

/**
 * @brief CSV 文件解析结果。
 */
struct CsvData
{
    std::filesystem::path resolvedPath; ///< 解析后的绝对路径（读盘用）
    std::string displayPath;            ///< 报错用的相对路径（include 里写的那个）
    std::vector<CsvRow> rows;
};

/**
 * @brief 扫描结果。
 */
struct ScanResult
{
    std::vector<SourceDecl> sources;   ///< include 指令 + 手写字面量声明
    std::vector<std::string> declared; ///< 已声明的 Token 名（含 include 的）
    bool sawInclude = false;
};

// ---------------------------------------------------------------------
// CSV 读取
// ---------------------------------------------------------------------

/**
 * @brief 读 CSV 文件并按行解析前两列。
 *
 * 行解析规则（「表头忽略、不校验、只读前两列」）：
 * - 空行 / 只有空白 / 行首 `#`：跳过；
 * - 第 1 列（Token 名）：允许带双引号或不带；读到分隔 `,` 为止；
 *   前后空白不裁剪（字面量里的空格有意义，Token 名一般无空白）。
 *   **第 1 列不在 `importedNames` 里时整行直接跳过，第 2 列连解析都不做**
 *   ——这就是「表头不校验」：`token,word` 这类表头行因为第一列对不上，
 *   自然被忽略，不会因为第二列不是合法字面量而报错；
 * - `,` 之后是第 2 列（只对第 1 列匹配上的行解析）：必须是双引号字符串
 *   字面量（`""` 表示一个 `"`、字段内可含逗号）；不以 `"` 开头或引号
 *   不闭合 / 为空 → 报错；
 * - 第 3 列及以后：一律忽略。
 *
 * @param path 已解析为绝对路径的 CSV 文件（读盘用）。
 * @param displayPath 报错时显示的路径（include 里写的相对路径）。
 * @param importedNames 被 `%token ... include` 导入的 Token 名集合。
 * @param data 成功时写入 rows 与两个路径。
 * @param error 出错时写入错误说明（信息里带 文件:行:列）。
 * @return 成功返回 true。
 */
bool readCsv(const std::filesystem::path &path,
             const std::string &displayPath,
             const std::set<std::string> &importedNames, CsvData &data,
             Error &error)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
    {
        return false; // 由调用方按「.gram 内错误」的格式报
    }
    std::string content((std::istreambuf_iterator<char>(stream)),
                        std::istreambuf_iterator<char>());
    data.resolvedPath = path;
    data.displayPath = displayPath;
    const std::vector<std::string> lines = splitLines(content);
    for (std::size_t lineIndex = 0u; lineIndex < lines.size(); ++lineIndex)
    {
        const std::string line = trimLineEnd(lines[lineIndex]);
        std::size_t cursor = 0u;
        while ((cursor < line.size()) &&
               ((line[cursor] == ' ') || (line[cursor] == '\t')))
        {
            ++cursor;
        }
        if (cursor >= line.size())
        {
            continue; // 空行
        }
        if (line[cursor] == '#')
        {
            continue; // 注释行
        }
        // ---- 第 1 列：Token 名 ----
        const std::size_t tokenColumn = cursor + 1u;
        std::string tokenField;
        if (line[cursor] == '"')
        {
            ++cursor;
            bool closed = false;
            while (cursor < line.size())
            {
                if (line[cursor] == '"')
                {
                    if ((cursor + 1u < line.size()) &&
                        (line[cursor + 1u] == '"'))
                    {
                        tokenField += '"';
                        cursor += 2u;
                        continue;
                    }
                    ++cursor;
                    closed = true;
                    break;
                }
                tokenField += line[cursor];
                ++cursor;
            }
            if (!closed)
            {
                return fail(error,
                            displayPath + ":" +
                                std::to_string(lineIndex + 1u) + ":" +
                                std::to_string(tokenColumn) +
                                ": 第 1 列（Token 名）的双引号没有闭合");
            }
        }
        else
        {
            while ((cursor < line.size()) && (line[cursor] != ','))
            {
                tokenField += line[cursor];
                ++cursor;
            }
        }
        // 「表头不校验」：第 1 列非空但对不上任何被导入 Token 名 → 整行跳过，
        // 第 2 列不解析（因此 `token,word` 表头不会因为第二列不是合法字面量
        // 而报错）。**第 1 列为空是格式错误**（与是否匹配无关），照报。
        if (tokenField.empty())
        {
            return fail(error,
                        displayPath + ":" + std::to_string(lineIndex + 1u) +
                            ":" + std::to_string(tokenColumn) +
                            ": 第 1 列（Token 名）不能为空");
        }
        if (importedNames.find(tokenField) == importedNames.end())
        {
            continue;
        }
        // 跳过第 1 列后的空白，找分隔逗号
        while ((cursor < line.size()) &&
               ((line[cursor] == ' ') || (line[cursor] == '\t')))
        {
            ++cursor;
        }
        if (cursor >= line.size())
        {
            // 第 1 列匹配上了却没有第 2 列：报「第 2 列不能为空」
            return fail(error,
                        displayPath + ":" + std::to_string(lineIndex + 1u) +
                            ":" + std::to_string(tokenColumn + 1u) +
                            ": 第 2 列（字面量）不能为空");
        }
        if (line[cursor] != ',')
        {
            return fail(error,
                        displayPath + ":" + std::to_string(lineIndex + 1u) +
                            ":" + std::to_string(cursor + 1u) +
                            ": 第 2 列（字面量）前缺少分隔逗号");
        }
        ++cursor; // 消耗 `,`
        // ---- 第 2 列：字面量 ----
        while ((cursor < line.size()) &&
               ((line[cursor] == ' ') || (line[cursor] == '\t')))
        {
            ++cursor;
        }
        const std::size_t literalColumn = cursor + 1u;
        if (cursor >= line.size())
        {
            return fail(error,
                        displayPath + ":" + std::to_string(lineIndex + 1u) +
                            ":" + std::to_string(literalColumn) +
                            ": 第 2 列（字面量）不能为空");
        }
        if (line[cursor] != '"')
        {
            return fail(error,
                        displayPath + ":" + std::to_string(lineIndex + 1u) +
                            ":" + std::to_string(literalColumn) +
                            ": 第 2 列（字面量）必须是带双引号的字符串字面量"
                            "（与 %token NAME \"...\" 的写法一致）");
        }
        std::string literalRaw;
        ++cursor;
        bool closed = false;
        while (cursor < line.size())
        {
            if (line[cursor] == '"')
            {
                if ((cursor + 1u < line.size()) && (line[cursor + 1u] == '"'))
                {
                    literalRaw += '"';
                    cursor += 2u;
                    continue;
                }
                closed = true;
                ++cursor;
                break;
            }
            literalRaw += line[cursor];
            ++cursor;
        }
        if (!closed)
        {
            return fail(error,
                        displayPath + ":" + std::to_string(lineIndex + 1u) +
                            ":" + std::to_string(literalColumn) +
                            ": 第 2 列（字面量）的双引号没有闭合");
        }
        if (tokenField.empty())
        {
            return fail(error,
                        displayPath + ":" + std::to_string(lineIndex + 1u) +
                            ":" + std::to_string(tokenColumn) +
                            ": 第 1 列（Token 名）不能为空");
        }
        if (literalRaw.empty())
        {
            return fail(error,
                        displayPath + ":" + std::to_string(lineIndex + 1u) +
                            ":" + std::to_string(literalColumn) +
                            ": 第 2 列（字面量）不能为空");
        }
        // 第 3 列及以后忽略
        CsvRow row;
        row.tokenName = tokenField;
        row.literal = literalRaw;
        row.sourceLine = lineIndex + 1u;
        row.literalColumn = literalColumn;
        data.rows.push_back(std::move(row));
    }
    return true;
}

// ---------------------------------------------------------------------
// %token / %define 行解析
// ---------------------------------------------------------------------

/**
 * @brief 解析一行 `%token ...` 指令，收集其中的 include 指令与字面量声明。
 *
 * 需要消化的形态（与 GramParser::parseTokenDirective 一致）：
 *
 *   %token X                          纯声明（记进 declared，不进 sources）
 *   %token X "lit"                    手写字面量声明 → SourceDecl
 *   %token X include "csv"            include 指令 → SourceDecl
 *   %token regex X "pattern"          正则声明（记进 declared）
 *   %token <member> ...               带成员标签（作用于本行后续符号）
 *   ... alias "显示名"                 显示别名（保留在合并后的声明里）
 *
 * 同一行可以有多个符号（`%token A B C`）；每个 `X include "csv"` 都是一条
 * 独立的 SourceDecl。
 *
 * @param line 行内容（不含行尾）。
 * @param lineBase 该行在整份文本里的起始偏移。
 * @param sourceLine 该行的 1-based 行号。
 * @param out 收集结果。
 * @return 解析成功返回 true；形态不合法返回 false（外层报错）。
 */
bool parseTokenLine(const std::string &line, std::size_t lineBase,
                    std::size_t sourceLine, ScanResult &out)
{
    const std::vector<LineToken> tokens = tokenizeLine(line);
    if (tokens.empty() ||
        (tokens.front().kind != "id") || (tokens.front().text != "%token"))
    {
        return true;
    }
    const std::size_t directiveStart = lineBase + tokens.front().start;
    std::string tag;
    std::size_t pos = 1u;
    while (pos < tokens.size())
    {
        if (tokens[pos].kind == "lt")
        {
            if ((pos + 2u >= tokens.size()) ||
                (tokens[pos + 1u].kind != "id") ||
                (tokens[pos + 2u].kind != "gt"))
            {
                return false;
            }
            tag = tokens[pos + 1u].text;
            pos += 3u;
            continue;
        }
        if ((tokens[pos].kind == "id") && (tokens[pos].text == "alias"))
        {
            // alias 属于前面最近声明的符号；这里只做「跳过」处理，
            // 合并时 alias 保留在第一条来源行（见 resolveIncludes）。
            if ((pos + 1u >= tokens.size()) || (tokens[pos + 1u].kind != "str"))
            {
                return false;
            }
            pos += 2u;
            continue;
        }
        if ((tokens[pos].kind == "id") && (tokens[pos].text == "regex"))
        {
            if ((pos + 2u >= tokens.size()) ||
                (tokens[pos + 1u].kind != "id") ||
                (tokens[pos + 2u].kind != "str"))
            {
                return false;
            }
            out.declared.push_back(tokens[pos + 1u].text);
            pos += 3u;
            continue;
        }
        if (tokens[pos].kind == "comma")
        {
            ++pos;
            continue;
        }
        if (tokens[pos].kind != "id")
        {
            return false;
        }
        const std::string &name = tokens[pos].text;
        // `X include "csv"`：include 指令
        if ((pos + 2u < tokens.size()) &&
            (tokens[pos + 1u].kind == "id") &&
            (tokens[pos + 1u].text == "include") &&
            (tokens[pos + 2u].kind == "str"))
        {
            SourceDecl decl;
            decl.tokenName = name;
            decl.unionTag = tag;
            decl.isInclude = true;
            decl.csvPath = tokens[pos + 2u].text;
            if ((decl.csvPath.size() >= 2u) &&
                ((decl.csvPath.front() == '"') ||
                 (decl.csvPath.front() == '\'')) &&
                (decl.csvPath.back() == decl.csvPath.front()))
            {
                decl.csvPath =
                    decl.csvPath.substr(1u, decl.csvPath.size() - 2u);
            }
            // alias（可选）：`X include "csv" alias "Name"`
            if ((pos + 4u < tokens.size()) &&
                (tokens[pos + 3u].kind == "id") &&
                (tokens[pos + 3u].text == "alias") &&
                (tokens[pos + 4u].kind == "str"))
            {
                decl.aliasRaw = tokens[pos + 4u].text;
                pos += 5u;
            }
            else
            {
                pos += 3u;
            }
            decl.lineStart = directiveStart;
            decl.lineEnd = lineBase + line.size();
            decl.sourceLine = sourceLine;
            out.sources.push_back(std::move(decl));
            out.sawInclude = true;
            out.declared.push_back(name);
            continue;
        }
        // `X "lit"`：手写字面量声明
        if ((pos + 1u < tokens.size()) && (tokens[pos + 1u].kind == "str"))
        {
            SourceDecl decl;
            decl.tokenName = name;
            decl.unionTag = tag;
            decl.isInclude = false;
            decl.handwrittenCandidates.clear();
            decl.decoded =
                decodeLiteralCandidates(tokens[pos + 1u].text,
                                        decl.handwrittenCandidates);
            if ((pos + 3u < tokens.size()) &&
                (tokens[pos + 2u].kind == "id") &&
                (tokens[pos + 2u].text == "alias") &&
                (tokens[pos + 3u].kind == "str"))
            {
                decl.aliasRaw = tokens[pos + 3u].text;
                pos += 4u;
            }
            else
            {
                pos += 2u;
            }
            decl.lineStart = directiveStart;
            decl.lineEnd = lineBase + line.size();
            decl.sourceLine = sourceLine;
            out.sources.push_back(std::move(decl));
            out.declared.push_back(name);
            continue;
        }
        // `X`：纯声明
        out.declared.push_back(name);
        ++pos;
    }
    return true;
}

/**
 * @brief 解析一行 `%define X "lit"` 指令（手写字面量声明）。
 */
bool parseDefineLine(const std::string &line, std::size_t lineBase,
                     std::size_t sourceLine, ScanResult &out)
{
    const std::vector<LineToken> tokens = tokenizeLine(line);
    if (tokens.empty() ||
        (tokens.front().kind != "id") || (tokens.front().text != "%define"))
    {
        return true;
    }
    const std::size_t directiveStart = lineBase + tokens.front().start;
    std::size_t pos = 1u;
    if ((pos >= tokens.size()) || (tokens[pos].kind != "id"))
    {
        return true;
    }
    const std::string &name = tokens[pos].text;
    ++pos;
    if ((pos >= tokens.size()) || (tokens[pos].kind != "str"))
    {
        return true;
    }
    SourceDecl decl;
    decl.tokenName = name;
    decl.isInclude = false;
    decl.decoded =
        decodeLiteralCandidates(tokens[pos].text, decl.handwrittenCandidates);
    ++pos;
    if ((pos + 1u < tokens.size()) && (tokens[pos].kind == "id") &&
        (tokens[pos].text == "alias") && (tokens[pos + 1u].kind == "str"))
    {
        decl.aliasRaw = tokens[pos + 1u].text;
    }
    decl.lineStart = directiveStart;
    decl.lineEnd = lineBase + line.size();
    decl.sourceLine = sourceLine;
    out.sources.push_back(std::move(decl));
    out.declared.push_back(name);
    return true;
}

/**
 * @brief 解析一行 `%regex X "pattern"`（只记 declared）。
 */
void parseRegexLine(const std::string &line, ScanResult &out)
{
    const std::vector<LineToken> tokens = tokenizeLine(line);
    if (tokens.empty() ||
        (tokens.front().kind != "id") || (tokens.front().text != "%regex"))
    {
        return;
    }
    if ((tokens.size() >= 3u) && (tokens[1u].kind == "id") &&
        (tokens[2u].kind == "str"))
    {
        out.declared.push_back(tokens[1u].text);
    }
}

/**
 * @brief 判断一行去掉空白后是否是某个段标记（`%epilogue` / `%main` 等）。
 */
bool isLineMarker(const std::string &line, const std::string &marker)
{
    const std::string stripped = stripText(line);
    if (stripped.size() < marker.size())
    {
        return false;
    }
    if (stripped.compare(0u, marker.size(), marker) != 0)
    {
        return false;
    }
    for (std::size_t k = marker.size(); k < stripped.size(); ++k)
    {
        const char ch = stripped[k];
        if ((ch != ' ') && (ch != '\t') && (ch != '\r'))
        {
            return false;
        }
    }
    return true;
}

/**
 * @brief 扫预处理后的文本：收集 include 指令、手写字面量声明、已声明 Token 名。
 *
 * 与 Preprocessor 一致地跳过「不透明区」：`%code ... %endcode` 段、
 * 块注释；行注释在 tokenizeLine 里跳过。
 * `%epilogue` / `%main` 段的正文按「非指令行」对待（不扫）。
 */
bool scanPreprocessedText(const std::string &text, ScanResult &result,
                          Error &error)
{
    const std::vector<std::string> lines = splitLines(text);
    std::size_t lineBase = 0u;
    bool inCodeBlock = false;
    bool inBlockComment = false;
    bool inSection = false; // %epilogue / %main 正文
    for (std::size_t lineIndex = 0u; lineIndex < lines.size(); ++lineIndex)
    {
        const std::string rawLine = lines[lineIndex];
        const std::string line = trimLineEnd(rawLine);
        const std::size_t sourceLine = lineIndex + 1u;
        const std::string stripped = stripText(line);
        if (inCodeBlock)
        {
            if (isLineMarker(line, "%endcode"))
            {
                inCodeBlock = false;
            }
            lineBase += rawLine.size();
            continue;
        }
        if (inBlockComment)
        {
            if (stripped.find("*/") != std::string::npos)
            {
                inBlockComment = false;
            }
            lineBase += rawLine.size();
            continue;
        }
        if (inSection)
        {
            // %epilogue / %main 正文：到下一个「行首是 %」的行为止
            if (!stripped.empty() && (stripped[0] == '%'))
            {
                inSection = false; // 掉出段，继续按指令行处理
            }
            else
            {
                lineBase += rawLine.size();
                continue;
            }
        }
        // 块注释打开？
        if (!inBlockComment)
        {
            const std::size_t openAt = stripped.find("/*");
            if ((openAt != std::string::npos) &&
                (stripped.find("*/", openAt + 2u) == std::string::npos))
            {
                inBlockComment = true;
                lineBase += rawLine.size();
                continue;
            }
        }
        // %code ... %endcode
        if (isLineMarker(line, "%code"))
        {
            inCodeBlock = true;
            lineBase += rawLine.size();
            continue;
        }
        // %epilogue / %main 段
        if (isLineMarker(line, "%epilogue") || isLineMarker(line, "%main"))
        {
            inSection = true;
            lineBase += rawLine.size();
            continue;
        }
        // 指令行
        if (!stripped.empty() && (stripped[0] == '%'))
        {
            // 注意：std::string::compare(pos, len, str) 是「取 substr(pos, len)
            // 与 str 比较」，len 必须等于 str 的长度，否则长度不等直接非零。
            if ((stripped.compare(0, 6u, "%token") == 0) &&
                ((stripped.size() == 6u) || (stripped[6] == ' ') ||
                 (stripped[6] == '\t')))
            {
                if (!parseTokenLine(line, lineBase, sourceLine, result))
                {
                    return fail(error,
                                "无法解析 %token 指令：" + stripped,
                                sourceLine);
                }
            }
            else if ((stripped.compare(0, 7u, "%define") == 0) &&
                     ((stripped.size() == 7u) || (stripped[7] == ' ') ||
                      (stripped[7] == '\t')))
            {
                parseDefineLine(line, lineBase, sourceLine, result);
            }
            else if ((stripped.compare(0, 6u, "%regex") == 0) &&
                     ((stripped.size() == 6u) || (stripped[6] == ' ') ||
                      (stripped[6] == '\t')))
            {
                parseRegexLine(line, result);
            }
        }
        lineBase += rawLine.size();
    }
    return true;
}

// ---------------------------------------------------------------------
// 合并与唯一性
// ---------------------------------------------------------------------

/**
 * @brief 展开计划：替换区间 → 新文本（空串 = 删除该区间）。
 */
struct Replacement
{
    std::size_t start = 0u; ///< 区间起始（文本内字节偏移）
    std::size_t end = 0u;   ///< 区间结束（exclusive）
    std::string text;       ///< 替换文本；空 = 删除
};

/**
 * @brief 收集所有 include 的 CSV（按规范化路径缓存）。
 *
 * 每个 SourceDecl 的 resolvedCsvPath / displayPath 在这里填好；
 * 读盘失败报 `<gramPath>:<行>:<列>: 找不到 CSV 文件 <相对路径>`。
 *
 * @param sources 全部来源（会就地填 resolvedCsvPath）。
 * @param baseDir 文法文件所在目录（CSV 相对路径按它解析）。
 * @param gramPath `--grammar-file` 的原文（报错用；可为空）。
 * @param importedNames 被导入的 Token 名集合（CSV 行过滤用）。
 */
bool collectCsvs(std::vector<SourceDecl> &sources,
                 const std::string &baseDir, const std::string &gramPath,
                 const std::set<std::string> &importedNames,
                 std::map<std::filesystem::path, CsvData> &cache,
                 Error &error)
{
    for (SourceDecl &decl : sources)
    {
        if (!decl.isInclude)
        {
            continue;
        }
        std::filesystem::path raw(decl.csvPath);
        std::filesystem::path resolved;
        if (raw.is_relative() && !baseDir.empty())
        {
            resolved = std::filesystem::path(baseDir) / raw;
        }
        else
        {
            resolved = raw;
        }
        std::error_code ec;
        auto canonical = std::filesystem::weakly_canonical(resolved, ec);
        if (!ec)
        {
            resolved = canonical;
        }
        decl.resolvedCsvPath = resolved;
        if (cache.find(resolved) != cache.end())
        {
            continue;
        }
        CsvData data;
        if (!readCsv(resolved, decl.csvPath, importedNames, data, error))
        {
            if (!error.empty())
            {
                // CSV 文件存在但内容有错：错误信息已带文件:行:列，直接往上抛。
                return false;
            }
            // 读盘失败（文件不存在 / 不可读）→ 报「.gram 内错误」的格式
            if (!gramPath.empty())
            {
                return fail(error,
                            gramPath + ":" + std::to_string(decl.sourceLine) +
                                ":1: 找不到 CSV 文件 " + decl.csvPath);
            }
            return fail(error,
                        "第 " + std::to_string(decl.sourceLine) +
                            " 行的 %token include: 找不到 CSV 文件 " +
                            decl.csvPath,
                        decl.sourceLine);
        }
        cache[resolved] = std::move(data);
    }
    return true;
}

/**
 * @brief 合并每个被 include 的 Token 的全部字面量来源，生成替换计划。
 *
 * 唯一性规则（需求 §别名唯一性）：
 * 1. 同一 Token 下重复字面量 → 静默去重，保留首次出现顺序；
 * 2. 不同 Token 用同一字面量 → 报错（错误指向 CSV 行与首次分配处）；
 * 3. 字面量与已声明的 Token 名相同 → 报错；
 * 4. 正则 Token 不参与字面量别名唯一性判定（本函数只看字面量来源）。
 *
 * 合并输出：每个被 include 的 Token X 的第一条来源行区间替换为
 * `%token [<tag>] X "lit1|lit2|..."`（第一条来源行的 alias 保留），
 * 其余来源行区间删除。手写字面量声明不在被 include 的 Token 上时
 * 原样保留（保证「不使用 include 的文法行为完全不变」）。
 */
bool resolveAndPlan(const ScanResult &scan,
                    const std::map<std::filesystem::path, CsvData> &csvCache,
                    std::vector<Replacement> &plan, Error &error)
{
    // 被导入的 Token 名集合
    std::set<std::string> importedNames;
    for (const SourceDecl &decl : scan.sources)
    {
        if (decl.isInclude)
        {
            importedNames.insert(decl.tokenName);
        }
    }
    if (importedNames.empty())
    {
        return true;
    }
    // 已声明 Token 名 → 首次声明行（冲突报错用）
    std::unordered_map<std::string, std::size_t> declaredNames;
    for (const std::string &name : scan.declared)
    {
        if (declaredNames.find(name) == declaredNames.end())
        {
            declaredNames[name] = 0u; // 行号信息在下面补
        }
    }
    for (const SourceDecl &decl : scan.sources)
    {
        if (declaredNames.count(decl.tokenName) &&
            (declaredNames[decl.tokenName] == 0u))
        {
            declaredNames[decl.tokenName] = decl.sourceLine;
        }
    }
    // 字面量归属：literal -> (token 名, csv 路径, csv 行, csv 列)。
    // 手写来源没有 CSV 位置，报错时用 .gram 行号。
    struct OwnerInfo
    {
        std::string tokenName;
        std::string where; ///< 来源描述（CSV 路径:行:列 或 .gram 行号）
    };
    std::unordered_map<std::string, OwnerInfo> ownerByLiteral;
    // 按 Token 分组来源（保持扫描顺序）
    std::map<std::string, std::vector<const SourceDecl *>> byToken;
    for (const SourceDecl &decl : scan.sources)
    {
        byToken[decl.tokenName].push_back(&decl);
    }
    for (const auto &entry : byToken)
    {
        const std::string &tokenName = entry.first;
        const std::vector<const SourceDecl *> &decls = entry.second;
        const bool hasInclude = importedNames.count(tokenName) != 0;
        if (!hasInclude)
        {
            continue; // 没有 include 的 Token：不动它的手写声明
        }
        // ---- 合并字面量（按来源出现顺序）----
        std::vector<std::string> merged;
        std::set<std::string> seen;
        const SourceDecl *firstDecl = nullptr;
        for (const SourceDecl *decl : decls)
        {
            if (decl->isInclude)
            {
                const auto csvIt = csvCache.find(decl->resolvedCsvPath);
                if (csvIt == csvCache.end())
                {
                    return fail(error,
                                "CSV 路径 " +
                                    decl->csvPath +
                                    " 没有读到（内部错误）",
                                decl->sourceLine);
                }
                for (const CsvRow &row : csvIt->second.rows)
                {
                    // 只取第 1 列等于本 Token 名的行
                    if (row.tokenName != tokenName)
                    {
                        continue;
                    }
                    // 别名 == 已声明 Token 名（且不是自己）→ 报错
                    const auto declaredIt = declaredNames.find(row.literal);
                    if ((declaredIt != declaredNames.end()) &&
                        (declaredIt->first != tokenName))
                    {
                        return fail(error,
                                    csvIt->second.displayPath +
                                        ":" + std::to_string(row.sourceLine) +
                                        ":" +
                                        std::to_string(row.literalColumn) +
                                        ": 别名 \"" + row.literal +
                                        "\" 与已声明的 token 名 " +
                                        declaredIt->first + " 冲突",
                                    decl->sourceLine);
                    }
                    // 不同 Token 用同一字面量 → 报错
                    const auto ownerIt = ownerByLiteral.find(row.literal);
                    if ((ownerIt != ownerByLiteral.end()) &&
                        (ownerIt->second.tokenName != tokenName))
                    {
                        return fail(error,
                                    csvIt->second.displayPath +
                                        ":" + std::to_string(row.sourceLine) +
                                        ":" +
                                        std::to_string(row.literalColumn) +
                                        ": 别名 \"" + row.literal +
                                        "\" 已被 token " +
                                        ownerIt->second.tokenName +
                                        " 使用，不能重复分配给 token " +
                                        tokenName + "（首次出现于 " +
                                        ownerIt->second.where + "）",
                                    decl->sourceLine);
                    }
                    // 同一 Token 下重复 → 静默去重
                    if (seen.count(row.literal))
                    {
                        continue;
                    }
                    seen.insert(row.literal);
                    merged.push_back(row.literal);
                    if (ownerByLiteral.find(row.literal) ==
                        ownerByLiteral.end())
                    {
                        OwnerInfo info;
                        info.tokenName = tokenName;
                        info.where = csvIt->second.displayPath + ":" +
                                     std::to_string(row.sourceLine) + ":" +
                                     std::to_string(row.literalColumn);
                        ownerByLiteral[row.literal] = info;
                    }
                }
            }
            else
            {
                // 手写来源：解码失败就保留原样（GramParser 会报它自己的错）
                if (!decl->decoded)
                {
                    continue;
                }
                for (const std::string &literal : decl->handwrittenCandidates)
                {
                    const auto ownerIt = ownerByLiteral.find(literal);
                    if ((ownerIt != ownerByLiteral.end()) &&
                        (ownerIt->second.tokenName != tokenName))
                    {
                        return fail(error,
                                    "第 " + std::to_string(decl->sourceLine) +
                                        " 行的手写字面量 \"" + literal +
                                        "\" 已被 token " +
                                        ownerIt->second.tokenName +
                                        " 使用（首次出现于 " +
                                        ownerIt->second.where + "）",
                                    decl->sourceLine);
                    }
                    if (seen.count(literal))
                    {
                        continue;
                    }
                    seen.insert(literal);
                    merged.push_back(literal);
                    if (ownerByLiteral.find(literal) ==
                        ownerByLiteral.end())
                    {
                        OwnerInfo info;
                        info.tokenName = tokenName;
                        info.where = ".gram 第 " +
                                     std::to_string(decl->sourceLine) + " 行";
                        ownerByLiteral[literal] = info;
                    }
                }
            }
            if (firstDecl == nullptr)
            {
                firstDecl = decl;
            }
        }
        // ---- 生成替换计划 ----
        if (firstDecl == nullptr)
        {
            continue;
        }
        if (merged.empty())
        {
            // CSV 里没有这个 Token 的行：所有相关 include 行删除
            for (const SourceDecl *decl : decls)
            {
                if (decl->isInclude)
                {
                    Replacement drop;
                    drop.start = decl->lineStart;
                    drop.end = decl->lineEnd;
                    plan.push_back(std::move(drop));
                }
            }
            continue;
        }
        // 合并声明写在第一条来源行的位置
        std::string replaced = "%token ";
        if (!firstDecl->unionTag.empty())
        {
            replaced += "<" + firstDecl->unionTag + "> ";
        }
        replaced += tokenName;
        replaced += " \"";
        for (std::size_t k = 0u; k < merged.size(); ++k)
        {
            if (k > 0u)
            {
                replaced += "|";
            }
            replaced += encodeLiteralBody(merged[k]);
        }
        replaced += "\"";
        if (!firstDecl->aliasRaw.empty())
        {
            replaced += " alias " + firstDecl->aliasRaw;
        }
        Replacement head;
        head.start = firstDecl->lineStart;
        head.end = firstDecl->lineEnd;
        head.text = replaced;
        plan.push_back(std::move(head));
        // 其余来源行（含手写）删除
        for (const SourceDecl *decl : decls)
        {
            if (decl == firstDecl)
            {
                continue;
            }
            Replacement drop;
            drop.start = decl->lineStart;
            drop.end = decl->lineEnd;
            plan.push_back(std::move(drop));
        }
    }
    return true;
}

} // namespace

bool TokenDict::expand(const std::string &text, const std::string &baseDir,
                       const std::string &gramPath, TokenDictExpandResult &out,
                       Error &error)
{
    out.text.clear();
    out.lineMap.clear();
    ScanResult scan;
    if (!scanPreprocessedText(text, scan, error))
    {
        return false;
    }
    if (!scan.sawInclude)
    {
        // 没有任何 include：原样返回，保证不用此特性的文法逐字节不变
        out.text = text;
        return true;
    }
    // 被导入的 Token 名集合（CSV 行过滤用）
    std::set<std::string> importedNames;
    for (const SourceDecl &decl : scan.sources)
    {
        if (decl.isInclude)
        {
            importedNames.insert(decl.tokenName);
        }
    }
    std::map<std::filesystem::path, CsvData> csvCache;
    if (!collectCsvs(scan.sources, baseDir, gramPath, importedNames, csvCache,
                     error))
    {
        return false;
    }
    std::vector<Replacement> plan;
    if (!resolveAndPlan(scan, csvCache, plan, error))
    {
        return false;
    }
    // ---- 应用替换计划 ----
    // 替换区间都在指令行内（不含行尾），行数不变：处理后第 N 行仍对应
    // 处理前第 N 行，因此 out.lineMap 与输入的行号恒等——但 GramParser 需要
    // 的映射是「处理后行 → 原文件行」，那由 grammar_loader 把 Preprocessor
    // 的映射叠加得到；这里 lineMap 填恒等映射（或空表示恒等）。
    std::sort(plan.begin(), plan.end(),
              [](const Replacement &a, const Replacement &b)
              {
                  return a.start < b.start;
              });
    std::string result;
    result.reserve(text.size());
    std::size_t cursor = 0u;
    for (const Replacement &rep : plan)
    {
        if (rep.start > cursor)
        {
            result += text.substr(cursor, rep.start - cursor);
        }
        result += rep.text;
        cursor = rep.end;
    }
    if (cursor < text.size())
    {
        result += text.substr(cursor);
    }
    out.text = std::move(result);
    // 行号恒等（展开不增减行数）；填空表示恒等，与 Preprocessor 的约定一致。
    return true;
}

} // namespace ParseEngine