/** @file manual.cpp
 * @brief `ParseEngine man` 子命令：在终端里离线查询内联的手册。
 *
 * 数据来自 ParseEngine/manual_data.hpp（由 tools/embed_manual.py 在构建期从
 * `doc/`、`doc/en/` 与 `example/manifest.tsv` 生成），所以运行时不需要仓库文件，
 * 也不依赖 Python。输出在终端上带 ANSI 标题色、长文档走 `less -R` 分页；
 * 重定向到文件或管道时只写普通 UTF-8 文本。
 */

#include "ParseEngine/manual.hpp"

#include "ParseEngine/i18n.hpp"
#include "ParseEngine/manual_data.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace ParseEngine
{
namespace Manual
{
namespace
{

/// 条目表的取值函数（directiveAt / modeAt / languageAt / exampleAt 形状一致）。
using EntryAccessor = const ManualEntry &(*)(std::size_t);

/**
 * @brief 把文本折成小写（只处理 ASCII，中文按原字节保留）。
 * @param text 原文本。
 * @return 小写副本。
 */
std::string lower(std::string text)
{
    for (char &ch : text)
    {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return text;
}

/**
 * @brief 当前的界面语言短名（`zh` / `en` / `ja`）。
 * @return 短名。
 */
std::string languageTag()
{
    switch (I18n::language())
    {
    case I18n::Language::Zh:
        return "zh";
    case I18n::Language::Ja:
        return "ja";
    default:
        return "en";
    }
}

/**
 * @brief 解析 `--lang` 的取值。
 * @param value 取值文本。
 * @param language 输出语言。
 * @return 取值合法时为真。
 */
bool parseLanguage(const std::string &value, I18n::Language &language)
{
    const std::string name = lower(value);
    if (name == "zh")
    {
        language = I18n::Language::Zh;
    }
    else if (name == "en")
    {
        language = I18n::Language::En;
    }
    else if (name == "ja")
    {
        language = I18n::Language::Ja;
    }
    else if (name == "auto")
    {
        language = I18n::Language::Auto;
    }
    else
    {
        return false;
    }
    return true;
}

/**
 * @brief 按字节宽度给纯 ASCII 列补空格。
 * @param text 文本。
 * @param width 目标宽度。
 * @return 补齐后的文本。
 */
std::string padTo(const std::string &text, std::size_t width)
{
    if (text.size() >= width)
    {
        return text;
    }
    return text + std::string(width - text.size(), ' ');
}

/**
 * @brief 去掉一段文本两端的空白。
 * @param text 原文本。
 * @return 去空白后的文本。
 */
std::string trim(const std::string &text)
{
    const std::size_t begin = text.find_first_not_of(" \t\r");
    if (begin == std::string::npos)
    {
        return {};
    }
    const std::size_t end = text.find_last_not_of(" \t\r");
    return text.substr(begin, end - begin + 1u);
}

/**
 * @brief 终端着色与分页；重定向时只写普通 UTF-8 文本。
 *
 * 只在第一次调用 `shell()` 时构造一次，进程退出时恢复 Windows 控制台模式；
 * 中间不再构造副本，免得析构时把虚拟终端处理提前关掉。
 */
class Terminal
{
    bool terminal_ = false;
    bool color_ = false;
#if defined(_WIN32)
    HANDLE handle_ = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode_ = 0;
#endif

public:
    /// 按终端能力启用 ANSI（Windows 打开虚拟终端处理），`NO_COLOR` 一律关色。
    Terminal()
    {
#if defined(_WIN32)
        terminal_ = GetConsoleMode(handle_, &mode_) != 0;
        color_ = terminal_ &&
                 SetConsoleMode(handle_, mode_ | ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0;
#else
        terminal_ = isatty(STDOUT_FILENO) != 0;
        color_ = terminal_;
#endif
        if (std::getenv("NO_COLOR") != nullptr)
        {
            color_ = false;
        }
    }

    /// 恢复 Windows 控制台模式（先冲刷缓冲区，免得转义序列被截断）。
    ~Terminal()
    {
        std::cout.flush();
#if defined(_WIN32)
        if (terminal_)
        {
            SetConsoleMode(handle_, mode_);
        }
#endif
    }

    /// 标题加粗青色。
    std::string title(const std::string &text) const
    {
        return color_ ? "\x1b[1;36m" + text + "\x1b[0m" : text;
    }

    /// 次要信息用暗色。
    std::string dim(const std::string &text) const
    {
        return color_ ? "\x1b[2m" + text + "\x1b[0m" : text;
    }

    /// 输出文本；长文且 PATH 里有 less 时分页。
    void print(const std::string &text) const
    {
        bool pager = false;
        if (terminal_ && std::count(text.begin(), text.end(), '\n') > 24)
        {
#if defined(_WIN32)
            const char separator = ';';
            const char *program = "less.exe";
#else
            const char separator = ':';
            const char *program = "less";
#endif
            const char *environment = std::getenv("PATH");
            const std::string paths = environment != nullptr ? environment : "";
            for (std::size_t start = 0u; start <= paths.size();)
            {
                const std::size_t end = paths.find(separator, start);
                const std::string directory =
                    paths.substr(start, end == std::string::npos ? std::string::npos
                                                                 : end - start);
                if (!directory.empty())
                {
                    std::string candidate = directory;
                    if (candidate.back() != '/' && candidate.back() != '\\')
                    {
                        candidate += '/';
                    }
                    candidate += program;
                    if (std::FILE *probe = std::fopen(candidate.c_str(), "rb"))
                    {
                        std::fclose(probe);
                        pager = true;
                        break;
                    }
                }
                if (end == std::string::npos)
                {
                    break;
                }
                start = end + 1u;
            }
        }
        if (pager)
        {
#if defined(_WIN32)
            std::FILE *pipe = _popen("less -R", "w");
#else
            std::FILE *pipe = popen("less -R", "w");
#endif
            if (pipe != nullptr)
            {
                std::fwrite(text.data(), 1, text.size(), pipe);
#if defined(_WIN32)
                _pclose(pipe);
#else
                pclose(pipe);
#endif
                return;
            }
        }
        std::cout << text;
    }
};

/**
 * @brief 进程内唯一的终端能力对象。
 * @return 只读引用。
 */
const Terminal &shell()
{
    static const Terminal instance;
    return instance;
}

/**
 * @brief 转义成 JSON 字符串字面量（含首尾引号）。
 * @param text 原文本。
 * @return `"…"` 形式。
 */
std::string jsonString(const std::string &text)
{
    std::string out = "\"";
    for (const unsigned char ch : text)
    {
        switch (ch)
        {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (ch < 0x20u)
            {
                char buffer[8];
                std::snprintf(buffer, sizeof(buffer), "\\u%04x", static_cast<unsigned>(ch));
                out += buffer;
            }
            else
            {
                out += static_cast<char>(ch);
            }
            break;
        }
    }
    return out + "\"";
}

/**
 * @brief 一条条目转成 JSON 对象。
 * @param entry 条目。
 * @return JSON 文本。
 */
std::string jsonEntry(const ManualEntry &entry)
{
    return std::string("{\"name\":") + jsonString(entry.name) +
           ",\"usage\":" + jsonString(entry.usage) +
           ",\"summary\":" + jsonString(entry.summary) +
           ",\"summaryEn\":" + jsonString(entry.summaryEn) + "}";
}

/**
 * @brief 一批条目转成 JSON 数组。
 * @param accessor 取值函数。
 * @param count 条目数。
 * @return JSON 文本。
 */
std::string jsonEntries(EntryAccessor accessor, std::size_t count)
{
    std::string out = "[";
    for (std::size_t index = 0u; index < count; ++index)
    {
        if (index > 0u)
        {
            out += ",";
        }
        out += jsonEntry(accessor(index));
    }
    return out + "]";
}

/**
 * @brief 一页的元信息转成 JSON 对象。
 * @param page 页面。
 * @return JSON 文本。
 */
std::string jsonPage(const ManualPage &page)
{
    return std::string("{\"id\":") + jsonString(page.id) +
           ",\"stem\":" + jsonString(page.stem) +
           ",\"file\":" + jsonString(page.file) +
           ",\"title\":" + jsonString(page.title) +
           ",\"titleEn\":" + jsonString(page.titleEn) +
           ",\"summary\":" + jsonString(page.summary) +
           ",\"hasEnglish\":" + (page.hasEnglish ? "true" : "false") + "}";
}

/**
 * @brief 该显示哪个标题：英文模式下优先取英文正文的一级标题。
 *
 * 正文语言（`--lang en` / 英文区域）下，标题还留在中文会很突兀；英文版缺失
 * 时退回中文标题，不显示空串。
 *
 * @param page 文档页元信息。
 * @param english 是否正显示英文正文。
 * @return 标题文本。
 */
const char *pageTitle(const ManualPage &page, bool english)
{
    if (english && page.titleEn != nullptr && page.titleEn[0] != '\0')
    {
        return page.titleEn;
    }
    return page.title;
}

/**
 * @brief JSON 文档头（未闭合，调用方接着写字段）。
 * @param kind `kind` 取值。
 * @return JSON 文本。
 */
std::string jsonHead(const char *kind)
{
    return std::string("{\"schemaVersion\":1,\"kind\":") + jsonString(kind) +
           ",\"language\":" + jsonString(languageTag());
}

/**
 * @brief 取一页的正文。
 * @param page 页面元信息。
 * @param english 是否取英文正文。
 * @return 正文；没有对应切片时是空串。
 */
std::string pageBody(const ManualPage &page, bool english)
{
    const std::string key = std::string(english ? "page-en/" : "page/") + page.file;
    for (std::size_t index = 0u; index < sliceCount(); ++index)
    {
        if (key == sliceKeyAt(index))
        {
            return sliceAt(index);
        }
    }
    return {};
}

/**
 * @brief 按短编号 / 文件名 / 标题 / 文件名后缀找一页。
 * @param selector 用户写的目标。
 * @return 命中的下标；没命中返回 pageCount()。
 */
std::size_t findPage(const std::string &selector)
{
    const std::string wanted = lower(selector);
    std::size_t best = pageCount();
    int bestRank = 0;
    for (std::size_t index = 0u; index < pageCount(); ++index)
    {
        const ManualPage &page = pageAt(index);
        const std::string stem = lower(page.stem);
        const std::string number = lower(page.id);
        int rank = 0;
        if (wanted == stem || wanted == lower(page.file))
        {
            rank = 4;
        }
        else if (!number.empty() &&
                 (wanted == number ||
                  (number.size() == 2u && number[0] == '0' && wanted == number.substr(1u))))
        {
            rank = 3;
        }
        else if (wanted == lower(page.title) || wanted == lower(page.titleEn))
        {
            rank = 2;
        }
        else if (wanted.size() >= 3u && stem.size() > wanted.size() + 1u &&
                 stem.compare(stem.size() - wanted.size(), wanted.size(), wanted) == 0 &&
                 stem[stem.size() - wanted.size() - 1u] == '-')
        {
            rank = 1;
        }
        if (rank > bestRank)
        {
            bestRank = rank;
            best = index;
        }
    }
    return best;
}

/**
 * @brief 在一批条目里按名字精确查找（不分大小写）。
 * @param accessor 取值函数。
 * @param count 条目数。
 * @param name 名字。
 * @return 命中的下标；没命中返回 count。
 */
std::size_t findEntry(EntryAccessor accessor, std::size_t count, const std::string &name)
{
    const std::string wanted = lower(name);
    for (std::size_t index = 0u; index < count; ++index)
    {
        if (wanted == lower(accessor(index).name))
        {
            return index;
        }
    }
    return count;
}

/**
 * @brief 取条目的说明；界面是英文而英文源缺失时回落到中文。
 * @param entry 条目。
 * @param english 界面是否英文。
 * @return 说明文本。
 */
std::string entrySummary(const ManualEntry &entry, bool english)
{
    if (english && entry.summaryEn[0] != '\0')
    {
        return entry.summaryEn;
    }
    return entry.summary;
}

/**
 * @brief 把一句说明裁短到清单里放得下。
 *
 * 先找第一个句号（中英文都认），找不到再按字节截断——截断时退到 UTF-8 字符边界，
 * 免得切出半个汉字；末尾补省略号。详情视图不用它，那里给全文。
 *
 * @param text 说明。
 * @param limit 上限（字节）。
 * @return 裁短后的说明。
 */
std::string brief(const std::string &text, std::size_t limit)
{
    if (text.size() <= limit)
    {
        return text;
    }
    /// 句子边界（位置 + 该标记的字节数）。
    struct Stop
    {
        std::size_t at;
        std::size_t length;
    };
    const Stop stops[] = {{text.find("。"), 3u}, {text.find("；"), 3u},
                          {text.find("; "), 2u}, {text.find(". "), 2u}};
    for (const Stop &stop : stops)
    {
        if (stop.at != std::string::npos && stop.at + stop.length <= limit)
        {
            return text.substr(0u, stop.at + stop.length);
        }
    }
    std::size_t cut = limit;
    while (cut > 0u && (static_cast<unsigned char>(text[cut]) & 0xc0u) == 0x80u)
    {
        --cut;
    }
    return text.substr(0u, cut) + "…";
}

/// 名字列的对齐宽度（按条目里最长的名字算）。
std::size_t nameWidth(EntryAccessor accessor, std::size_t count)
{
    std::size_t width = 0u;
    for (std::size_t index = 0u; index < count; ++index)
    {
        width = std::max(width, std::strlen(accessor(index).name));
    }
    return width + 2u;
}

/**
 * @brief 把一批条目排成「名字 + 说明」或「名字 + 用法 — 说明」的清单。
 * @param accessor 取值函数。
 * @param count 条目数。
 * @param english 界面是否英文。
 * @param withUsage 是否在名字后附上用法列。
 * @return 文本。
 */
std::string renderEntries(EntryAccessor accessor, std::size_t count, bool english,
                          bool withUsage)
{
    const std::size_t width = nameWidth(accessor, count);
    std::string out;
    for (std::size_t index = 0u; index < count; ++index)
    {
        const ManualEntry &entry = accessor(index);
        out += "  " + padTo(entry.name, width);
        std::string summary = brief(entrySummary(entry, english), 120u);
        if (withUsage && entry.usage[0] != '\0')
        {
            out += entry.usage;
            if (summary == entry.usage)
            {
                summary.clear();
            }
            if (!summary.empty())
            {
                out += "  —  " + summary;
            }
        }
        else
        {
            out += summary;
        }
        out += "\n";
    }
    return out;
}

/**
 * @brief 把 Markdown 正文改成终端可读的形式：图片写成一句提示，其余原样。
 * @param markdown 正文。
 * @return 终端文本。
 */
std::string toTerminal(const std::string &markdown)
{
    const std::string label = PE_TEXT("图片");
    std::string out;
    out.reserve(markdown.size());
    for (std::size_t at = 0u; at < markdown.size();)
    {
        if (markdown[at] == '!' && at + 1u < markdown.size() && markdown[at + 1u] == '[')
        {
            const std::size_t close = markdown.find(']', at + 2u);
            const std::size_t open =
                (close != std::string::npos && close + 1u < markdown.size() &&
                 markdown[close + 1u] == '(')
                    ? close + 1u
                    : std::string::npos;
            const std::size_t end =
                open == std::string::npos ? std::string::npos : markdown.find(')', open + 1u);
            if (end != std::string::npos)
            {
                out += "[" + label + "] " + markdown.substr(at + 2u, close - at - 2u);
                const std::string path = markdown.substr(open + 1u, end - open - 1u);
                if (!path.empty())
                {
                    out += " (" + path + ")";
                }
                at = end + 1u;
                continue;
            }
        }
        out += markdown[at++];
    }
    return out;
}

/**
 * @brief 取正文里第一个含关键词的行，作为搜索结果的一行摘要。
 * @param body 正文。
 * @param needle 小写关键词。
 * @return 摘要行；没有命中返回空串。
 */
std::string firstMatchLine(const std::string &body, const std::string &needle)
{
    std::size_t start = 0u;
    while (start <= body.size())
    {
        const std::size_t end = body.find('\n', start);
        const std::string line = body.substr(
            start, end == std::string::npos ? std::string::npos : end - start);
        const std::string text = trim(line);
        if (!text.empty() && lower(text).find(needle) != std::string::npos)
        {
            if (text.size() <= 140u)
            {
                return text;
            }
            std::size_t cut = 140u;
            while (cut > 0u && (static_cast<unsigned char>(text[cut]) & 0xc0u) == 0x80u)
            {
                --cut;
            }
            return text.substr(0u, cut) + "…";
        }
        if (end == std::string::npos)
        {
            break;
        }
        start = end + 1u;
    }
    return {};
}

/// 手册用法。与 source/src/main.cpp 的 printUsage 同一套路：中文正文直接打印，
/// 英文 / 日文正文放在 source/i18n/messages.tsv 的「ParseEngine man 手册正文」里。
void printUsage(const std::string &program)
{
    if (I18n::language() != I18n::Language::Zh)
    {
        std::cout << PE_TEXT("用法：") << program << PE_TEXT(" man [目标] [选项]\n")
                  << PE_TEXT("ParseEngine man 手册正文");
        return;
    }
    std::cout
        << "用法：" << program << " man [目标] [选项]\n"
        << "查询构建期内联的手册：离线可用，运行时不读仓库文件。\n"
        << "\n"
        << "  man                    列出全部条目：文档页、文法指令、生成模式、目标语言、示例\n"
        << "  man <页>               显示一页文档正文。页可以写短编号 01、文件名\n"
        << "                         01-overview.md、去后缀的 01-overview 或标题片段\n"
        << "  man directives         列出全部文法指令（%define / %token / %mode …）\n"
        << "  man modes              列出十六种生成模式\n"
        << "  man languages          列出全部目标语言（--lang 取值）\n"
        << "  man examples           列出 example/ 下的示例文法\n"
        << "  man %token             显示一条文法指令的用法与说明\n"
        << "  man lalr               显示一个生成模式的说明\n"
        << "  man python             显示一种目标语言的说明\n"
        << "  man --search <文本>    在文档页、指令、模式、语言与示例里搜关键词\n"
        << "\n"
        << "  --json                 输出 JSON（可与任意目标组合）\n"
        << "  --exact                配 --search：整串精确匹配\n"
        << "  --lang zh|en|ja|auto   界面与正文语言；等价于写在 man 之前的 --ui-lang\n"
        << "\n"
        << "示例：\n"
        << "  " << program << " man 01\n"
        << "  " << program << " man 13 --lang en\n"
        << "  " << program << " man directives --json\n"
        << "  " << program << " man --search 展望符\n";
}

/**
 * @brief 打印一条参数错误。
 * @param message 消息。
 * @param machine 是否 JSON 模式。
 * @return 固定返回 2。
 */
int fail(const std::string &message, bool machine)
{
    if (machine)
    {
        std::cout << jsonHead("error") << ",\"code\":\"PE1901\",\"message\":"
                  << jsonString(message) << "}\n";
    }
    else
    {
        std::cerr << "PE1901: " << message << "\n";
    }
    return 2;
}

/**
 * @brief 默认视图：把所有条目列一个总览。
 * @param program 程序名。
 * @return 文本。
 */
std::string renderIndex(const std::string &program)
{
    const Terminal &terminal = shell();
    std::string out = terminal.title(PE_TEXT("ParseEngine 内嵌手册")) + "\n" +
                      terminal.dim(PE_TEXT("用法：") + program +
                                   PE_TEXT(" man <目标>")) + "\n\n";
    std::size_t width = 0u;
    for (std::size_t index = 0u; index < pageCount(); ++index)
    {
        const ManualPage &page = pageAt(index);
        width = std::max(width, std::strlen(page.id) + std::strlen(page.file));
    }
    width += 4u;
    const bool english = I18n::language() != I18n::Language::Zh;
    out += terminal.title(PE_TEXT("文档页")) + "  (" + std::to_string(pageCount()) + ")\n";
    for (std::size_t index = 0u; index < pageCount(); ++index)
    {
        const ManualPage &page = pageAt(index);
        out += "  " + padTo(std::string(page.id) + "  " + page.file, width) +
               pageTitle(page, english);
        if (page.hasEnglish)
        {
            out += terminal.dim("  [en]");
        }
        out += "\n";
    }
    out += "\n";
    out += terminal.title(PE_TEXT("文法指令")) + "  (" + std::to_string(directiveCount()) +
           ")   " + program + " man directives\n";
    out += terminal.title(PE_TEXT("生成模式")) + "  (" + std::to_string(modeCount()) + ")   " +
           program + " man modes\n";
    out += terminal.title(PE_TEXT("目标语言")) + "  (" + std::to_string(languageCount()) +
           ")   " + program + " man languages\n";
    out += terminal.title(PE_TEXT("示例文法")) + "  (" + std::to_string(exampleCount()) +
           ")   " + program + " man examples\n";
    out += "\n";
    out += terminal.dim(PE_TEXT("搜关键词：") + program + PE_TEXT(" man --search <关键词>")) + "\n";
    return out;
}

/**
 * @brief `man --search` 的结果。
 * @param program 程序名。
 * @param query 关键词。
 * @param exact 是否整串精确匹配。
 * @param english 界面是否英文。
 * @param hits 输出命中条数。
 * @return 文本。
 */
std::string renderSearch(const std::string &program, const std::string &query, bool exact,
                         bool english, int &hits)
{
    const Terminal &terminal = shell();
    const std::string needle = lower(query);
    const auto accepts = [&](const char *text)
    {
        const std::string value = lower(text != nullptr ? text : "");
        return exact ? value == needle : value.find(needle) != std::string::npos;
    };
    std::string out = terminal.title(PE_TEXT("搜索：") + query) + "\n\n";
    hits = 0;

    std::string pages;
    for (std::size_t index = 0u; index < pageCount(); ++index)
    {
        const ManualPage &page = pageAt(index);
        bool matched = accepts(page.title) || accepts(page.titleEn) || accepts(page.summary) ||
                       accepts(page.stem) || accepts(page.file);
        std::string excerpt;
        if (!matched && !exact)
        {
            excerpt = firstMatchLine(pageBody(page, false) + pageBody(page, true), needle);
            matched = !excerpt.empty();
        }
        if (matched)
        {
            ++hits;
            pages += "  " + padTo(page.file, 26u) + pageTitle(page, english) + "\n";
            if (!excerpt.empty())
            {
                pages += terminal.dim("      " + excerpt) + "\n";
            }
        }
    }
    if (!pages.empty())
    {
        out += terminal.title(PE_TEXT("文档页")) + "\n" + pages + "\n";
    }

    /// 一组要搜的条目。
    struct Group
    {
        const char *label;
        EntryAccessor accessor;
        std::size_t count;
    };
    const Group groups[] = {
        {PE_TEXT("文法指令"), directiveAt, directiveCount()},
        {PE_TEXT("生成模式"), modeAt, modeCount()},
        {PE_TEXT("目标语言"), languageAt, languageCount()},
        {PE_TEXT("示例文法"), exampleAt, exampleCount()},
    };
    for (const Group &group : groups)
    {
        std::string body;
        for (std::size_t index = 0u; index < group.count; ++index)
        {
            const ManualEntry &entry = group.accessor(index);
            if (accepts(entry.name) || accepts(entry.usage) || accepts(entry.summary) ||
                accepts(entry.summaryEn))
            {
                ++hits;
                body += "  " + padTo(entry.name, 20u) +
                        brief(entrySummary(entry, english), 90u) + "\n";
            }
        }
        if (!body.empty())
        {
            out += terminal.title(group.label) + "\n" + body + "\n";
        }
    }
    if (hits == 0)
    {
        out += terminal.dim(std::string(PE_TEXT("提示：")) + program +
                            PE_TEXT(" man 可看全部条目")) + "\n";
    }
    return out;
}

} // namespace

int runCommandLine(int argc, char **argv)
{
    const std::string program = (argc > 0 && argv[0] != nullptr) ? argv[0] : "ParseEngine";
    bool machine = false;
    bool exact = false;
    bool languageGiven = false;
    I18n::Language language = I18n::Language::Auto;
    std::string target;
    std::string query;

    for (int at = 2; at < argc; ++at)
    {
        const std::string arg = argv[at] != nullptr ? argv[at] : "";
        if (arg == "--json")
        {
            machine = true;
        }
        else if (arg == "--exact")
        {
            exact = true;
        }
        else if (arg == "--lang" || arg == "--search")
        {
            const bool isLanguage = arg == "--lang";
            if (at + 1 >= argc)
            {
                return fail(isLanguage ? PE_TEXT("--lang 后需要 zh、en、ja 或 auto")
                                       : PE_TEXT("--search 后需要关键词"),
                            machine);
            }
            const std::string value = argv[++at] != nullptr ? argv[at] : "";
            if (isLanguage)
            {
                if (!parseLanguage(value, language))
                {
                    return fail(std::string(PE_TEXT("不支持的显示语言：")) + value, machine);
                }
                languageGiven = true;
            }
            else
            {
                query = value;
            }
        }
        else if (arg.compare(0u, 7u, "--lang=") == 0)
        {
            if (!parseLanguage(arg.substr(7u), language))
            {
                return fail(std::string(PE_TEXT("不支持的显示语言：")) + arg.substr(7u), machine);
            }
            languageGiven = true;
        }
        else if (arg.compare(0u, 9u, "--search=") == 0)
        {
            query = arg.substr(9u);
        }
        else if (arg == "--help" || arg == "-h")
        {
            target = "--help";
        }
        else if (arg.size() > 1u && arg[0] == '-')
        {
            return fail(std::string(PE_TEXT("手册参数无效：")) + arg, machine);
        }
        else if (target.empty())
        {
            target = arg;
        }
        else
        {
            return fail(std::string(PE_TEXT("手册参数无效：")) + arg, machine);
        }
    }

    if (languageGiven)
    {
        I18n::selected = language;
    }
    const bool english = I18n::language() != I18n::Language::Zh;
    const Terminal &terminal = shell();

    if (target == "--help")
    {
        if (machine)
        {
            std::cout << jsonHead("help") << "}\n";
            return 0;
        }
        printUsage(program);
        return 0;
    }

    // --search：在全部条目里找关键词。
    if (!query.empty())
    {
        if (!target.empty())
        {
            return fail(std::string(PE_TEXT("手册参数无效：")) + target, machine);
        }
        int hits = 0;
        const std::string text = renderSearch(program, query, exact, english, hits);
        if (machine)
        {
            std::cout << jsonHead("search") << ",\"query\":" << jsonString(query)
                      << ",\"exact\":" << (exact ? "true" : "false")
                      << ",\"hitCount\":" << hits << ",\"text\":" << jsonString(text) << "}\n";
        }
        else
        {
            terminal.print(text);
        }
        return hits > 0 ? 0 : 1;
    }

    // 没有目标：总览。
    if (target.empty())
    {
        if (machine)
        {
            std::string pages = "[";
            for (std::size_t index = 0u; index < pageCount(); ++index)
            {
                pages += (index > 0u ? "," : "") + jsonPage(pageAt(index));
            }
            std::cout << jsonHead("index") << ",\"pages\":" << pages << "],"
                      << "\"directives\":" << jsonEntries(directiveAt, directiveCount())
                      << ",\"modes\":" << jsonEntries(modeAt, modeCount())
                      << ",\"languages\":" << jsonEntries(languageAt, languageCount())
                      << ",\"examples\":" << jsonEntries(exampleAt, exampleCount()) << "}\n";
            return 0;
        }
        terminal.print(renderIndex(program));
        return 0;
    }

    const std::string key = lower(target);

    /// 一组清单：别名、JSON 的 kind、标题与数据源。
    struct Listing
    {
        const char *const *aliases;  ///< 小写别名，以空指针结束
        const char *kind;            ///< JSON 的 kind
        const char *label;           ///< 中文标题
        EntryAccessor accessor;
        std::size_t count;           ///< 条目数
        const char *hint;            ///< 尾部的「看详情」示例目标
        bool withUsage;              ///< 是否输出用法列
    };
    const char *const directivesAliases[] = {"directives", "directive", "指令", nullptr};
    const char *const modesAliases[] = {"modes", "mode", "模式", nullptr};
    const char *const languagesAliases[] = {"languages", "language", "langs", "语言", nullptr};
    const char *const examplesAliases[] = {"examples", "example", "示例", nullptr};
    const Listing listings[] = {
        {directivesAliases, "directives", "文法指令", directiveAt, directiveCount(), "%token",
         false},
        {modesAliases, "modes", "生成模式", modeAt, modeCount(), "lalr", true},
        {languagesAliases, "languages", "目标语言", languageAt, languageCount(), "python", true},
        {examplesAliases, "examples", "示例文法", exampleAt, exampleCount(), nullptr, false},
    };
    const Listing *listing = nullptr;
    for (const Listing &candidate : listings)
    {
        for (const char *const *alias = candidate.aliases; *alias != nullptr; ++alias)
        {
            if (key == *alias)
            {
                listing = &candidate;
                break;
            }
        }
        if (listing != nullptr)
        {
            break;
        }
    }
    if (listing != nullptr)
    {
        const std::size_t count = listing->count;
        if (machine)
        {
            std::cout << jsonHead(listing->kind) << ",\"count\":" << count
                      << ",\"entries\":" << jsonEntries(listing->accessor, count) << "}\n";
            return 0;
        }
        std::string body;
        if (listing->accessor == exampleAt)
        {
            // 示例的「用法」是验证过的语言档，「说明」是固定输入。
            const std::size_t width = nameWidth(exampleAt, count);
            for (std::size_t index = 0u; index < count; ++index)
            {
                const ManualEntry &entry = exampleAt(index);
                body += "  " + padTo(entry.name, width) + padTo(entry.usage, 34u);
                if (entry.summary[0] != '\0')
                {
                    body += std::string(PE_TEXT("输入：")) + entry.summary;
                }
                body += "\n";
            }
        }
        else
        {
            body = renderEntries(listing->accessor, count, english, listing->withUsage);
        }
        if (listing->hint != nullptr)
        {
            body += "\n" + terminal.dim(PE_TEXT("用法：") + program + " man " + listing->hint) +
                    "\n";
        }
        terminal.print(terminal.title(PE_TEXT(listing->label)) + "  (" +
                       std::to_string(count) + ")\n\n" + body);
        return 0;
    }

    /// 一条条目的详情页（指令 / 模式 / 目标语言共用）；`hint` 指到讲它的文档页。
    const auto detail = [&](const char *kind, const ManualEntry &entry, const char *hint)
    {
        if (machine)
        {
            std::cout << jsonHead(kind) << ",\"entry\":" << jsonEntry(entry) << "}\n";
            return;
        }
        std::string body = terminal.title(entry.name) + "\n\n";
        const std::string summary = entrySummary(entry, english);
        if (entry.usage[0] != '\0')
        {
            body += std::string(PE_TEXT("用法：")) + entry.usage + "\n\n";
        }
        if (summary != entry.usage)
        {
            body += summary + "\n\n";
        }
        body += terminal.dim(std::string(PE_TEXT("用法：")) + program + " man " + hint) + "\n";
        terminal.print(body);
    };

    // `man %token`：一条文法指令。
    if (key.size() > 1u && key[0] == '%')
    {
        const std::size_t index = findEntry(directiveAt, directiveCount(), target);
        if (index == directiveCount())
        {
            if (machine)
            {
                std::cout << jsonHead("error") << ",\"code\":\"PE1901\",\"message\":"
                          << jsonString(PE_TEXT("未找到手册目标：") + target) << "}\n";
                return 1;
            }
            std::cerr << PE_TEXT("未找到手册目标：") << target << "\n";
            return 1;
        }
        detail("directive", directiveAt(index), "12-grammar-usage");
        return 0;
    }

    // `man lalr`：一个生成模式。
    const std::size_t mode = findEntry(modeAt, modeCount(), target);
    if (mode != modeCount())
    {
        detail("mode", modeAt(mode), "13-modes");
        return 0;
    }

    // `man python`：一种目标语言。
    const std::size_t languageIndex = findEntry(languageAt, languageCount(), target);
    if (languageIndex != languageCount())
    {
        detail("language", languageAt(languageIndex), "14-languages-and-products");
        return 0;
    }

    // 剩下的都当文档页。
    const std::size_t pageIndex = findPage(target);
    if (pageIndex == pageCount())
    {
        if (machine)
        {
            std::cout << jsonHead("error") << ",\"code\":\"PE1901\",\"message\":"
                      << jsonString(PE_TEXT("未找到手册目标：") + target) << "}\n";
            return 1;
        }
        std::cerr << PE_TEXT("未找到手册目标：") << target << "\n"
                  << PE_TEXT("用法：") << program << " man\n";
        return 1;
    }
    const ManualPage &page = pageAt(pageIndex);
    const bool useEnglish = english && page.hasEnglish;
    const std::string body = pageBody(page, useEnglish);
    if (machine)
    {
        std::cout << jsonHead("page") << ",\"page\":" << jsonPage(page)
                  << ",\"contentLanguage\":" << jsonString(useEnglish ? "en" : "zh")
                  << ",\"content\":" << jsonString(body) << "}\n";
        return 0;
    }
    std::string out = terminal.title("[" + std::string(page.file) + "] " +
                                     pageTitle(page, useEnglish)) + "\n\n" +
                      toTerminal(body) + "\n" +
                      terminal.dim(std::string(PE_TEXT("来源：")) + "doc/" + page.file);
    if (page.hasEnglish)
    {
        out += terminal.dim("    " + std::string(PE_TEXT("英文版：")) + "doc/en/" + page.file);
    }
    out += "\n";
    terminal.print(out);
    return 0;
}

} // namespace Manual
} // namespace ParseEngine
