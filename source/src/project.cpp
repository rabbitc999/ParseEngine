/**
 * @file project.cpp
 * @brief 产物工程组装实现。
 */

#include "ParseEngine/project.hpp"
#include "ParseEngine/regex_compatibility.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <cctype>
#include <set>

#include "ParseEngine/action_translator.hpp"
#include "ParseEngine/code_gen.hpp"
#include "ParseEngine/code_gen_c99.hpp"
#include "ParseEngine/code_gen_cpp17.hpp"
#include "ParseEngine/code_gen_csharp.hpp"
#include "ParseEngine/code_gen_python.hpp"
#include "ParseEngine/code_gen_go.hpp"
#include "ParseEngine/code_gen_java.hpp"
#include "ParseEngine/code_gen_js.hpp"
#include "ParseEngine/code_gen_rust.hpp"
#include "ParseEngine/grammar_loader.hpp"
#include "ParseEngine/mermaid.hpp"
#include "ParseEngine/utils.hpp"

// 产物模板（Makefile / CMakeLists / RESULT.md）走 library/template 的模板引擎：
// 模板里写 {{ 变量 }} / {% if %} / {% for %}，生成器只负责把数据摆成"名字 → 值"。
// （-Ilibrary 由 Makefile / CMakeLists 提供。）
#include "template/engine.hpp"

namespace ParseEngine
{

const std::vector<std::string> k_projectFiles = {
    "parser.h",      "parser.c",       "parser_token.h", "parser_token.c",
    "parser_user.h", "parser_table.c", "main.c",         "Makefile",
    "CMakeLists.txt", "RESULT.md"};

/// 「生成信息」表里「优先级指令」那一行的一项（`result_info_precedence.md`）。
struct ResultPrecedenceRow
{
    std::string assoc; ///< left / right / nonassoc
    std::string name;  ///< 符号名
};

/// 「生成信息」表里「%token regex」那一行的一项（`result_info_regex.md`）。
struct ResultRegexRow
{
    std::string name;    ///< Token 名
    std::string pattern; ///< 正则正文
};

/// 「IELR(1) 状态拆分」小节里的一条拆分记录（`result_ielr_section.md`）。
struct ResultIelrSplit
{
    long long lalrState = 0;      ///< 被拆的 LALR 状态号
    std::vector<long long> parts; ///< 拆出来的子状态号
    std::string token;            ///< 触发拆分的那类 Token
    std::string kind;             ///< 冲突类型（移进 / 归约 或 归约 / 归约）
    std::string reason;           ///< 为什么要拆
};

namespace
{

/// 词法产物的 parser_token.c / .h 历史上就没填过模式标签：产物里留的是
/// **字面量 `__MODE_NAME__`**（解析产物的同名文件由收尾阶段填上真标签）。
/// 迁到模板引擎后这个差异靠「照旧传这个字面量」保持逐字节不变；
/// 真要修的话是另一件事（会让 lexer_only / alias_demo 两组产物变样）。
const char *const k_legacyModeMarker = "__MODE_NAME__";

/** 文档示例输入；未写 `%example` 时保留清楚的占位文本。 */
std::string exampleInput(const ProjectRequest &request)
{
    return request.exampleInput.value_or("<符合文法的语言>");
}

/** POSIX Shell 单引号字面量（单引号按 `'\\''` 重新拼接）。 */
std::string shellQuote(const std::string &value)
{
    std::string result = "'";
    for (const char ch : value)
    {
        if (ch == '\'')
        {
            result += "'\\''";
        }
        else
        {
            result += ch;
        }
    }
    result += "'";
    return result;
}

/** C / C++ / Python 都能识别的双引号字符串字面量。 */
std::string codeStringLiteral(const std::string &value)
{
    std::string result = "\"";
    static const char digits[] = "01234567";
    for (const unsigned char ch : value)
    {
        switch (ch)
        {
        case '\\': result += "\\\\"; break;
        case '"': result += "\\\""; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default:
            if ((ch < 0x20u) || (ch == 0x7fu))
            {
                result += '\\';
                result += digits[(ch >> 6u) & 7u];
                result += digits[(ch >> 3u) & 7u];
                result += digits[ch & 7u];
            }
            else
            {
                result += static_cast<char>(ch);
            }
            break;
        }
    }
    result += "\"";
    return result;
}

std::string applyCPrefix(const std::string &input, const ProjectRequest &request)
{
    if (request.cPrefix.empty()) return input;
    std::string text = input;
    std::vector<std::string> saved;
    auto protect = [&](const std::string &part) {
        if (part.empty()) return;
        const std::string mark = "__SE_USER_KEEP_" + std::to_string(saved.size()) + "__";
        std::size_t pos = 0;
        while ((pos = text.find(part, pos)) != std::string::npos) { text.replace(pos, part.size(), mark); pos += mark.size(); }
        saved.push_back(part);
    };
    protect(request.epilogue == nullptr ? std::string() : *request.epilogue);
    if (request.mainBlocks != nullptr) for (const auto &part : *request.mainBlocks) protect(part);
    if (request.lines != nullptr && request.lines->userBlocks != nullptr)
        for (const auto &part : *request.lines->userBlocks) protect(part);
    const std::string upper = [&]() { std::string s = request.cPrefix; for (char &c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c))); return s; }();
    std::string out;
    for (std::size_t i = 0; i < text.size();) {
        if ((std::isalpha(static_cast<unsigned char>(text[i])) || text[i] == '_')) {
            std::size_t j = i + 1; while (j < text.size() && (std::isalnum(static_cast<unsigned char>(text[j])) || text[j] == '_')) ++j;
            const std::string id = text.substr(i, j - i);
            bool keep = id.rfind("PARSER_USER_", 0) == 0 || id == "PARSER_MEMSET" || id == "PARSER_MEMCPY";
            const std::size_t p = id.find("Parser");
            const std::size_t u = id.find("PARSER");
            if (!keep && p != std::string::npos) out += id.substr(0, p) + request.cPrefix + id.substr(p + 6);
            else if (!keep && u != std::string::npos) out += id.substr(0, u) + upper + id.substr(u + 6);
            else out += id;
            i = j;
        } else { out += text[i++]; }
    }
    for (std::size_t i = 0; i < saved.size(); ++i) {
        const std::string mark = "__SE_USER_KEEP_" + std::to_string(i) + "__";
        std::size_t pos = 0; while ((pos = out.find(mark, pos)) != std::string::npos) { out.replace(pos, mark.size(), saved[i]); pos += saved[i].size(); }
    }
    return out;
}

/// 词法模式的 Token 列表（内置 Token + 文法声明的 + END）；定义在文件后半。
std::vector<std::string> lexTerminals(const ProjectRequest &request);

// RESULT.md 里那几段"静态运行时说明"不再写成 C++ 字面量：抽成
// source/templates/result_runtime_section_{custom,cpp17,python}.md.tmpl 与
// result_run_section_c_cpp17.md.tmpl，由 RESULT.md 用 {% include %} 拼进来
// （片段表在 generatorResultMd 里按后端选）。

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
 * @brief 把每一行行首的 static const 改成 const（分文件布局用）。
 * @param text 输入文本。
 * @return 替换后的文本。
 */
std::string stripStaticConst(const std::string &text)
{
    std::string result;
    std::size_t position = 0;
    while (position <= text.size())
    {
        const std::size_t lineEnd = text.find('\n', position);
        const std::size_t stop =
            (lineEnd == std::string::npos) ? text.size() : lineEnd;
        std::string line = text.substr(position, stop - position);
        if (line.rfind("static const ", 0) == 0)
        {
            line = "const " + line.substr(13);
        }
        result += line;
        if (lineEnd == std::string::npos)
        {
            break;
        }
        result += "\n";
        position = lineEnd + 1;
    }
    return result;
}

/**
 * @brief 去掉字符串末尾的空白（对应 Python 的 str.rstrip()）。
 * @param text 输入文本。
 * @return 去掉末尾空白的文本。
 */
std::string rstripWhitespace(const std::string &text)
{
    const std::size_t found = text.find_last_not_of(" \t\n\r\v\f");
    if (found == std::string::npos)
    {
        return "";
    }
    return text.substr(0, found + 1);
}

/// 一条 %copy 展开后的产物：输出名（可含子目录）与内容。
struct CopyEntry
{
    std::string name;    ///< 相对输出目录的路径
    std::string content; ///< 文件内容（二进制原样）
};

/**
 * @brief 以二进制方式读取文件。
 * @param path 文件路径。
 * @param out 输出文件内容（成功时写入）。
 * @param error 出错时写入错误说明。
 * @return 成功返回 true。
 */
bool readBinaryFile(const std::filesystem::path &path, std::string &out,
                    Error &error)
{
    std::ifstream handle(path, std::ios::binary);
    if (!handle)
    {
        return fail(error, "无法读取 %copy 源文件: " + path.u8string());
    }
    out = std::string((std::istreambuf_iterator<char>(handle)),
                      std::istreambuf_iterator<char>());
    return true;
}

/**
 * @brief 把 %copy 指定的文件 / 目录展开成 输出名 -> 内容。
 *
 * file 默认按文件名放进输出目录；dir 递归拷贝，输出名是
 * 源目录名 + 相对路径（如 lib/util.h），保持同样的目录层次。
 * @param copies %copy 指令列表。
 * @param out 输出展开后的拷贝项（目录内按路径排序，保证顺序稳定）。
 * @param error 出错时写入错误说明。
 * @return 成功返回 true。
 */
bool loadCopyEntries(const std::vector<CopySpec> &copies,
                     std::vector<CopyEntry> &out, Error &error)
{
    std::vector<CopyEntry> entries;
    for (const CopySpec &copy : copies)
    {
        const std::filesystem::path source=std::filesystem::u8path(copy.path);
        std::error_code errorCode;
        if (copy.kind == "dir")
        {
            if (!std::filesystem::is_directory(source, errorCode))
            {
                return fail(error, "%copy 目录不存在: " + copy.path);
            }
            std::vector<std::filesystem::path> files;
            std::filesystem::recursive_directory_iterator iterator(source,
                                                                  errorCode);
            const std::filesystem::recursive_directory_iterator end;
            while (!errorCode && (iterator != end))
            {
                if (iterator->is_regular_file(errorCode))
                {
                    files.push_back(iterator->path());
                }
                iterator.increment(errorCode);
            }
            std::sort(files.begin(), files.end());
            for (const std::filesystem::path &file : files)
            {
                const std::filesystem::path relative =
                    file.lexically_relative(source);
                const std::string name =
                    (std::filesystem::path(source.filename()) / relative)
                        .generic_u8string();
                std::string content;
                if (!readBinaryFile(file, content, error))
                {
                    return false;
                }
                entries.push_back({name, std::move(content)});
            }
        }
        else
        {
            if (!std::filesystem::is_regular_file(source, errorCode))
            {
                return fail(error, "%copy 文件不存在: " + copy.path);
            }
            std::string content;
            if (!readBinaryFile(source, content, error))
            {
                return false;
            }
            entries.push_back({source.filename().generic_u8string(),
                               std::move(content)});
        }
    }
    out = std::move(entries);
    return true;
}

/**
 * @brief 从拷贝项里挑出指定后缀的输出名。
 * @param entries 拷贝项。
 * @param suffix 后缀（如 ".c"）。
 * @return 输出名列表。
 */
std::vector<std::string> copyNamesWithSuffix(
    const std::vector<CopyEntry> &entries, const std::string &suffix)
{
    std::vector<std::string> names;
    for (const CopyEntry &entry : entries)
    {
        if ((entry.name.size() >= suffix.size()) &&
            (entry.name.compare(entry.name.size() - suffix.size(),
                                suffix.size(), suffix) == 0))
        {
            names.push_back(entry.name);
        }
    }
    return names;
}

/// C 产物里带扩展名的文件名（renameProductFiles 只换这些）。
const std::vector<std::string> &cProductNames()
{
    static const std::vector<std::string> k_names = {
        "parser.h",  "parser.c",     "parser_token.h", "parser_token.c",
        "parser_table.c", "main.c",  "parser_user.h"};
    return k_names;
}

/// C++17 产物里带扩展名的文件名（renameProductFiles 只换这些）。
const std::vector<std::string> &cpp17ProductNames()
{
    static const std::vector<std::string> k_names = {
        "parser.hpp", "parser.cpp", "main.cpp", "parser_user.h"};
    return k_names;
}

/**
 * @brief 把构建脚本 / RESULT.md 里写死的产物文件名换成带前缀的名字。
 *
 * 只换带扩展名的产物名（`libparser.a`、`parser_demo`、`%copy` 拷进来的用户
 * 文件都不动）。前缀为空时原样返回。
 * @param text 文本。
 * @param prefix 前缀（`%namespace` / `--namespace` 的名字）。
 * @param cpp17 文本属于 C++17 产物（`.hpp` / `.cpp` 名字表）。
 * @return 替换后的文本。
 */
std::string renameProductFiles(const std::string &text,
                               const std::string &prefix, bool cpp17)
{
    if (prefix.empty())
    {
        return text;
    }
    const std::vector<std::string> &names =
        cpp17 ? cpp17ProductNames() : cProductNames();
    std::string result = text;
    for (const std::string &name : names)
    {
        result = replaceAll(result, name, prefix + "_" + name);
    }
    return result;
}

/**
 * @brief 生成 parser_table.c：分析表 / 产生式表 / 树元数据表。
 * @param tree 是否 %tree。
 * @param tableText 分析表段。
 * @param treeTablesText 树元数据表段。
 * @return 文件内容。
 */
std::string generatorParserTableC(bool tree, const std::string &tableText,
                                  const std::string &treeTablesText,
                                  const std::string &modeLabel)
{
    // 分文件布局下表不再是 static：parser.c 通过 extern 引用它们
    const std::string tableBody = stripStaticConst(tableText);
    std::string treeBody;
    if (tree)
    {
        treeBody = "\n" + stripStaticConst(treeTablesText);
    }
    return tmpl::renderValues(Utils::loadTemplate("c99_parser_table.c"),
                              {{"mode_name", modeLabel},
                               {"table_text", tableBody},
                               {"tree_tables", treeBody}});
}

/**
 * @brief 生成 C 产物的 Makefile。
 * @param userHeader 是否生成了 parser_user.h。
 * @param copySources %copy 拷进来的 .c 文件。
 * @param copyHeaders %copy 拷进来的 .h 文件。
 * @param modeLabel 生成模式标签（填模板里的 `{{ mode_name }}`）。
 * @return 文件内容。
 */
std::string generatorMakefileCustom(
    bool userHeader, const std::vector<std::string> &copySources,
    const std::vector<std::string> &copyHeaders,
    const std::string &modeLabel)
{
    // parser_user.h 的有无交给模板的 {% if user_header %}，%copy 清单交给
    // {% for %} 展开——生成器不再对 Makefile 做"找行尾追加""全局删串"这类手术
    return tmpl::renderValues(
        Utils::loadTemplate("Makefile_custom"),
        {{"mode_name", modeLabel},
         {"user_header", userHeader},
         {"lib_srcs", tmpl::encode(copySources)},
         {"hdr_srcs", tmpl::encode(copyHeaders)}});
}

/**
 * @brief 生成 C++17 产物的 Makefile（产物名带 %namespace 前缀）。
 * @param userHeader 是否生成了 parser_user.h。
 * @param copySources %copy 拷进来的 .c / .cpp 文件。
 * @param copyHeaders %copy 拷进来的 .h / .hpp 文件。
 * @param prefix 文件名前缀（空 = 不加）。
 * @param modeLabel 生成模式标签（填模板里的 `{{ mode_name }}`）。
 * @return 文件内容。
 */
std::string generatorMakefileCpp17(bool userHeader,
                                   const std::vector<std::string> &copySources,
                                   const std::vector<std::string> &copyHeaders,
                                   const std::string &prefix,
                                   const std::string &modeLabel)
{
    const std::string text = tmpl::renderValues(
        Utils::loadTemplate("Makefile_custom_cpp17"),
        {{"mode_name", modeLabel},
         {"user_header", userHeader},
         {"lib_srcs", tmpl::encode(copySources)},
         {"hdr_srcs", tmpl::encode(copyHeaders)}});
    return renameProductFiles(text, prefix, true);
}

/**
 * @brief 生成 C 产物的 CMakeLists.txt。
 * @param copySources %copy 拷进来的 .c 文件。
 * @param modeLabel 生成模式标签（填模板里的 `{{ mode_name }}`）。
 * @return 文件内容。
 */
std::string generatorCMakeCustom(const std::vector<std::string> &copySources,
                                 const std::string &modeLabel)
{
    return tmpl::renderValues(
        Utils::loadTemplate("CMakeLists_custom.txt"),
        {{"mode_name", modeLabel},
         {"copy_sources", tmpl::encode(copySources)}});
}

/**
 * @brief 生成 C++17 产物的 CMakeLists.txt（产物名带 %namespace 前缀）。
 * @param copySources %copy 拷进来的源文件。
 * @param prefix 文件名前缀（空 = 不加）。
 * @param modeLabel 生成模式标签（填模板里的 `{{ mode_name }}`）。
 * @return 文件内容。
 */
std::string generatorCMakeCpp17(const std::vector<std::string> &copySources,
                                const std::string &prefix,
                                const std::string &modeLabel)
{
    const std::string text = tmpl::renderValues(
        Utils::loadTemplate("CMakeLists_custom_cpp17.txt"),
        {{"mode_name", modeLabel},
         {"copy_sources", tmpl::encode(copySources)}});
    return renameProductFiles(text, prefix, true);
}

/**
 * @brief 生成 RESULT.md：生成信息、产物清单、编译运行说明与分析报告。
 * @param request 组装入参。
 * @param mode 归一化后的生成模式。
 * @param analysis 分析结果。
 * @param productFiles 产物清单。
 * @return 文件内容。
 */
std::string generatorResultMd(const ProjectRequest &request,
                              const std::string &mode,
                              const AnalysisResult &analysis,
                              const std::vector<std::string> &productFiles)
{
    const std::string sample = exampleInput(request);
    const std::string sampleShell = shellQuote(sample);
    const std::string sampleCode = codeStringLiteral(sample);
    const std::string modeLabel = modeLabelText(mode);
    // LR(k) / LALR(k) 的标签要带上实际的 k（modeLabelText 只有模式名）
    const std::string modeLabelValue =
        ((mode == "lrk") || (mode == "lalrk") || (mode == "slrk") ||
         (mode == "llk"))
            ? (std::string(mode == "lrk"    ? "LR("
                           : mode == "lalrk" ? "LALR("
                           : mode == "slrk"  ? "SLR("
                                             : "LL(") +
               std::to_string(std::max(1, analysis.lookaheadK)) + ")")
            : modeLabel;
    // 产物语言：python 走 Python 后端；缺省（空串）与任何非 "c" 的取值都走 C++17
    const bool python = (request.lang == "python");
    const bool cpp17 = !python && (request.lang != "c");
    const std::string mainFileName =
        python ? "main.py" : (cpp17 ? "main.cpp" : "main.c");
    // 「只当解析库用时」那一句：直接用具体文件名拼，免得依赖占位符替换顺序
    const std::string libNote =
        python ? ("只当解析库用时 `import parser` 即可：`parser.py` 里词法器、"
                  "分析驱动与公开 API 都齐了，没有编译 / 链接步骤。\n")
               : (cpp17 ? ("只当解析库用时去掉 `" + mainFileName + "`；解析器的运行时就是 "
                 "`parser.cpp` 一个文件（接口是 `parser.hpp`），链进你的程序"
                 "即可，不需要额外链接选项。\n")
                       : ("只当解析库用时去掉 `" + mainFileName +
                          "`；链接时 parser 需要与\n`parser_token.c` / "
                          "`parser_table.c` 一起编译。\n"));
    // C / C++17 的「编译运行」段：``{{ link_command }}`` / ``{{ lib_note }}`` 由
    // result_run_section_c_cpp17.md.tmpl 取值（Python 走自己的那份模板）。
    // 直接调用编译器的完整命令行：两档差在源文件后缀与编译器名上。
    const std::string linkCommand =
        cpp17 ? "g++ -std=c++17 -Wall -Wextra -O2 parser.cpp main.cpp -lm -o "
                "parser_demo"
              : "gcc -std=c99 -Wall -Wextra -O2 *.c -lm -o parser_demo";
    // 优先级按 (数值, 名字) 排序，与 Python 的 sorted(key=...) 一致；排版交给
    // result_info_precedence.md（模板里就是「生成信息」表里那一行）
    std::vector<std::pair<int, std::string>> precedenceOrder;
    for (const auto &entry : analysis.priority.precedence)
    {
        precedenceOrder.emplace_back(entry.second, entry.first);
    }
    std::sort(precedenceOrder.begin(), precedenceOrder.end());
    std::vector<ResultPrecedenceRow> precedence;
    for (const auto &item : precedenceOrder)
    {
        const auto assoc = analysis.priority.associativity.find(item.second);
        std::string assocName = "left";
        if (assoc != analysis.priority.associativity.end())
        {
            assocName = assoc->second;
        }
        ResultPrecedenceRow row;
        row.assoc = (assocName == "right")      ? "right"
                    : (assocName == "nonassoc") ? "nonassoc"
                                                : "left";
        row.name = item.second;
        precedence.push_back(std::move(row));
    }
    const LexSpec emptyLex;
    const LexSpec &lexSpec =
        (request.lexSpec == nullptr) ? emptyLex : *request.lexSpec;
    const std::string identifierText =
        lexSpec.identifier.has_value() ? *lexSpec.identifier : "未使用";

    // 位置信息 / %commit：公开 API 就在既有产物文件里，这一节由
    // result_api_section.md 排版（名字与所有权写清楚；C99 的释放约定也在那）
    const bool hasCommits = !lexSpec.commits.empty();
    const bool lexMode = isLexMode(mode);
    // 词法产物的接口在 lexer.h 里，与解析产物的文件不同
    const std::string apiFileName =
        python ? "parser.py"
               : (cpp17 ? "parser.hpp" : (lexMode ? "lexer.h" : "parser.h"));
    const std::string locApiName =
        python ? "engine.current_location()"
               : (cpp17 ? "Engine::currentLocation()"
                        : (lexMode ? "Parser_LexCurrentLocation(&lexer)"
                                   : "Parser_CurrentLocation()"));
    const std::string commitApiName =
        python ? "ParserCommit / engine.commits"
               : (cpp17 ? "ParserCommit / Engine::commits()"
                        : (lexMode
                               ? "ParserCommit / Parser_LexGetCommits(&lexer)"
                               : "ParserCommit / Parser_GetCommits()"));
    const std::string freeApiName = lexMode ? "Parser_LexFreeCommits(&lexer)"
                                           : "Parser_FreeCommits()";
    // 「取完要手动释放」那一行只有 C99 需要（另外两档有 RAII / GC）
    const bool showFreeCommits = !python && !cpp17;

    // 正则 Token 一行（空数组时模板里写「未使用」）
    std::vector<ResultRegexRow> regexTokens;
    for (const auto &entry : lexSpec.regex)
    {
        ResultRegexRow row;
        row.name = entry.first;
        row.pattern = entry.second;
        regexTokens.push_back(std::move(row));
    }

    std::string resolutionsText;
    for (const LrResolution &entry : analysis.resolutions)
    {
        if (!resolutionsText.empty())
        {
            resolutionsText += "\n";
        }
        resolutionsText += resolutionLine(entry);
    }
    if (resolutionsText.empty())
    {
        resolutionsText = "（无）";
    }
    std::string unresolvedText;
    for (const LrConflict &entry : analysis.unresolved)
    {
        if (!unresolvedText.empty())
        {
            unresolvedText += "\n";
        }
        unresolvedText += entry.text;
    }
    if (unresolvedText.empty())
    {
        unresolvedText = "（无）";
    }

    // 状态机图示：模式里没有 LR 状态机（LL(1)）时文法图不生成，词法图仍可能有
    const std::string mermaidState =
        (!request.mermaid) ? "未启用"
                           : (isLrMode(mode) ? "启用（LR 状态机 + 词法 DFA）"
                                             : "启用（只有词法 DFA）");

    // 教学模式只对确定性 LR 模式生效，其它模式在命令行入口就被忽略了
    const std::string teachState =
        (!analysis.teach)
            ? "未启用"
            : (isLrMode(mode) ? "启用（教学模式）" : "未启用");

    // %sync：不进分析层，只在这里把锚点清单摆好（没写 %sync 时这一节由
    // result_sync_section.md 里的条件判断掉）
    std::vector<std::string> syncTokens;
    if ((request.syncTokens != nullptr) && !request.syncTokens->empty())
    {
        syncTokens = *request.syncTokens;
    }
    const std::string maxErrorsName =
        python ? "Parser.max_errors"
               : (cpp17 ? "Parser::kMaxErrors" : "PARSER_MAX_ERRORS");
    const bool isLl1 = (analysis.mode == "ll1");

    // 容量宏 / 容量常量：只有文法里写了 %max_stack_depth / %max_errors /
    // %error_recovery_tokens 时才单列一节（没写时模板里整段不出现）
    const bool capsPresent = request.maxStackDepth.has_value() ||
                             request.maxErrors.has_value() ||
                             request.errorRecoveryTokens.has_value();
    // C++17 / Python 叫「常量」，C 叫「宏」
    const bool capsConst = cpp17 || python;
    const std::string stackName =
        python ? "Parser.max_stack_depth"
               : (cpp17 ? "Parser::kMaxStackDepth" : "PARSER_MAX_STACK_DEPTH");
    const std::string errorsName =
        python ? "Parser.max_errors"
               : (cpp17 ? "Parser::kMaxErrors" : "PARSER_MAX_ERRORS");
    const std::string recoveryName =
        python ? "Parser.error_recovery_tokens"
               : (cpp17 ? "Parser::kErrorRecoveryTokens"
                        : "PARSER_ERROR_RECOVERY_TOKENS");
    const std::string stackValue =
        std::to_string(request.maxStackDepth.value_or(k_maxStackDepthDefault));
    const std::string errorsValue =
        std::to_string(request.maxErrors.value_or(k_maxErrorsDefault));
    const std::string recoveryValue = std::to_string(
        request.errorRecoveryTokens.value_or(k_errorRecoveryTokensDefault));
    const std::string stackSource =
        request.maxStackDepth.has_value() ? "`%max_stack_depth`" : "缺省";
    const std::string errorsSource =
        request.maxErrors.has_value() ? "`%max_errors`" : "缺省";
    const std::string recoverySource =
        request.errorRecoveryTokens.has_value() ? "`%error_recovery_tokens`"
                                               : "缺省";

    // %epilogue / %main：只有文法里写了这两个段时才单列一节（没写时模板里
    // 整段不出现）
    const std::size_t mainCount =
        (request.mainBlocks == nullptr) ? 0u : request.mainBlocks->size();
    const bool hasUserSections = request.hasEpilogue || (mainCount > 0u);
    const std::string userParserName =
        python ? "parser.py" : (cpp17 ? "parser.cpp" : "parser.c");
    const std::string userNamespace =
        cpp17 ? CodeGenCpp17::namespaceName(request) : std::string();

    const bool isIelr = (mode == "ielr");
    std::vector<ResultIelrSplit> ielrSplits;
    for (const IelrSplit &split : analysis.ielrSplits)
    {
        ResultIelrSplit row;
        row.lalrState = static_cast<long long>(split.lalrState);
        for (const int part : split.parts)
        {
            row.parts.push_back(static_cast<long long>(part));
        }
        row.token = split.token;
        row.kind = split.kind;
        row.reason = split.reason;
        ielrSplits.push_back(std::move(row));
    }

    const bool hasLookahead =
        (mode == "lrk") || (mode == "lalrk") || (mode == "slrk") ||
        (mode == "llk");
    const bool lookaheadIsLlk = (mode == "llk");
    const std::string lookaheadFamily =
        (mode == "lalrk")
            ? "LALR"
            : ((mode == "slrk") ? "SLR" : ((mode == "llk") ? "LL" : "LR"));

    // 状态机图示：产物清单里出现 mermaid/ 才算生成了
    std::vector<std::string> mermaidFiles;
    for (const std::string &name : productFiles)
    {
        if (name.compare(0, 8, "mermaid/") == 0)
        {
            mermaidFiles.push_back(name);
        }
    }

    // 各小节的模板都在自己的 .tmpl 里，RESULT.md 用 {% include %} 拼进来：
    // 名字对应哪一份模板由后端决定，**不该出现的名字不放**（include 出空串，
    // 正是「这一节不出现」）；小节内部的条件（有没有 %sync / %max_* / %mermaid…）
    // 由模板自己的条件判断决定。
    tmpl::SnippetTable snippets;
    snippets.emplace("run_section",
                     Utils::loadTemplate(python
                                             ? "result_run_section_python.md"
                                             : "result_run_section_c_cpp17.md"));
    snippets.emplace("runtime_section",
                     Utils::loadTemplate(
                         python ? "result_runtime_section_python.md"
                                : (cpp17 ? "result_runtime_section_cpp17.md"
                                         : "result_runtime_section_custom.md")));
    if (request.tree)
    {
        snippets.emplace("tree_section",
                         Utils::loadTemplate(
                             python ? "result_tree_section_python.md"
                                    : (cpp17 ? "result_tree_section_cpp17.md"
                                             : "result_tree_section_custom.md")));
    }
    // 绘图（语法树 / 语法图）：目前只有 Python 档白名单模式会发这套方法
    if (python && (isTreeDrawMode(analysis.mode) || isGlrMode(analysis.mode)))
    {
        snippets.emplace("draw_section",
                         Utils::loadTemplate(
                             isGlrMode(analysis.mode)
                                 ? "result_draw_section_python_glr.md"
                                 : "result_draw_section_python.md"));
    }
    // 「生成信息」表里那两行（语言中立）
    snippets.emplace("info_precedence",
                     Utils::loadTemplate("result_info_precedence.md"));
    snippets.emplace("info_regex",
                     Utils::loadTemplate("result_info_regex.md"));
    // 语言说明：C 档没有这一段
    if (python)
    {
        snippets.emplace("lang_note_section",
                         Utils::loadTemplate("result_lang_note_python.md"));
    }
    else if (cpp17)
    {
        snippets.emplace("lang_note_section",
                         Utils::loadTemplate("result_lang_note_cpp17.md"));
    }
    // 数据驱动小节：有无由模板里的条件判断决定
    snippets.emplace("api_section",
                     Utils::loadTemplate("result_api_section.md"));
    snippets.emplace("sync_section",
                     Utils::loadTemplate("result_sync_section.md"));
    snippets.emplace("caps_section",
                     Utils::loadTemplate("result_caps_section.md"));
    snippets.emplace("user_section",
                     Utils::loadTemplate("result_user_section.md"));
    snippets.emplace("lookahead_section",
                     Utils::loadTemplate("result_lookahead_section.md"));
    snippets.emplace("ielr_section",
                     Utils::loadTemplate("result_ielr_section.md"));
    snippets.emplace("mermaid_section",
                     Utils::loadTemplate("result_mermaid_section.md"));
    // 数据摆成「名字 → 值」交给 source/templates/RESULT.md.tmpl 与它 include 进来
    // 的各小节模板：产物清单用循环展开，可选小节的**有无由模板里的条件判断决定**
    // （生成器只管把数据摆齐，不该出现时那一节自己输出空串）
    std::string text = tmpl::renderValues(
        Utils::loadTemplate("RESULT.md"),
        {{"source_name", request.sourceName.empty() ? "(命令行内联文法)"
                                                    : request.sourceName},
         {"mode_label", modeLabelValue},
         {"lang_label", python ? "Python" : (cpp17 ? "C++17" : "C99")},
         {"tree_state", request.tree ? "启用" : "未启用"},
         {"teach_state", teachState},
         {"mermaid_state", mermaidState},
         {"expect", analysis.priority.expectConflicts.has_value()
                        ? std::to_string(*analysis.priority.expectConflicts)
                        : std::string("未使用")},
         {"identifier", identifierText},
         // 「生成信息」表里那两行 + 各小节（详见 source/templates/result_*.md.tmpl）
         {"precedence", tmpl::encode(precedence)},
         {"regex_tokens", tmpl::encode(regexTokens)},
         {"class_name", CodeGenPython::className(request)},
         {"loc_api", locApiName},
         {"loc_file", apiFileName},
         {"has_commits", hasCommits},
         {"commit_api", commitApiName},
         {"commit_file", apiFileName},
         {"show_free_commits", showFreeCommits},
         {"free_api", freeApiName},
         {"free_file", apiFileName},
         {"sync_tokens", tmpl::encode(syncTokens)},
         {"max_errors_name", maxErrorsName},
         {"is_ll1", isLl1},
         {"caps_present", capsPresent},
         {"caps_const", capsConst},
         {"python", python},
         {"cpp17", cpp17},
         {"stack_name", stackName},
         {"stack_value", stackValue},
         {"stack_source", stackSource},
         {"errors_name", errorsName},
         {"errors_value", errorsValue},
         {"errors_source", errorsSource},
         {"recovery_name", recoveryName},
         {"recovery_value", recoveryValue},
         {"recovery_source", recoverySource},
         {"has_user_sections", hasUserSections},
         {"has_epilogue", request.hasEpilogue},
         {"main_count", static_cast<long long>(mainCount)},
         {"parser_name", userParserName},
         {"main_name", mainFileName},
         {"namespace_name", userNamespace},
         {"is_ielr", isIelr},
         {"lalr_state_count",
          static_cast<long long>(analysis.ielrLalrStateCount)},
         {"ielr_state_count", static_cast<long long>(analysis.stateCount())},
         {"lr1_state_count", static_cast<long long>(analysis.lr1StateCount)},
         {"action_count", static_cast<long long>(analysis.action.size())},
         {"transition_count",
          static_cast<long long>(analysis.transitions.size())},
         {"ielr_splits", tmpl::encode(ielrSplits)},
         {"has_lookahead", hasLookahead},
         {"lookahead_is_llk", lookaheadIsLlk},
         {"family", lookaheadFamily},
         {"k", static_cast<long long>(analysis.lookaheadK)},
         {"select_count", static_cast<long long>(analysis.table.size())},
         {"action_state_count",
          static_cast<long long>(analysis.stateCount())},
         {"lookahead_column_count",
          static_cast<long long>(analysis.lookaheadColumns.size())},
         {"mermaid_files", tmpl::encode(mermaidFiles)},
         {"product_files", tmpl::encode(productFiles)},
         {"resolutions", resolutionsText},
         {"unresolved", unresolvedText},
         {"report", rstripWhitespace(request.reportText)},
         // RESULT.md 默认是简化版（到「未消解冲突」收尾）：末尾那一节
         // `## 文法分析报告` 只在 --full-report 时输出，模板里用 {% if %} 把关。
         {"full_report", request.fullReport},
         // 下面三个不直接出现在 RESULT.md 里，是给 include 进来的
         // 「编译运行」片段用的——片段按模板渲染，用的是同一个上下文。
         {"main_file", mainFileName},
         {"link_command", linkCommand},
         {"lib_note", libNote},
         {"example_shell", sampleShell},
         {"example_code", sampleCode}},
        snippets);
    // RESULT.md 里的文件名（产物清单来自 displayFiles，正文里的说明也一起对齐）
    // 前缀由 renameProductFiles 自己加下划线（`myapp` -> `myapp_parser.hpp`）
    return renameProductFiles(
        text, cpp17 ? request.cppNamespace : std::string(), cpp17);
}

/**
 * @brief 校验 %main 段数并生成插好 #line 的正文。
 *
 * 多段 %main 同时生效说明 %if 的条件写错了（正常写法保证只有一段生效），
 * 直接报错而不是随便挑一段。
 * @param request 组装入参。
 * @param outName main 产物文件名（#line 恢复行指向它）。
 * @param out 正文；空字符串表示用内建 demo main。
 * @param error 出错时写入错误说明。
 * @return 成功返回 true。
 */
bool buildMainBody(const ProjectRequest &request, const std::string &outName,
                   std::string &out, Error &error, bool lineDirectives = true)
{
    const std::vector<std::string> emptyMains;
    const std::vector<std::string> &mainBlocks =
        (request.mainBlocks == nullptr) ? emptyMains : *request.mainBlocks;
    if (mainBlocks.size() > 1u)
    {
        std::size_t line = 0u;
        if ((request.mainBlockLines != nullptr) &&
            (request.mainBlockLines->size() > 1u))
        {
            line = (*request.mainBlockLines)[1u];
        }
        return fail(
            error,
            "多个 %main 同时生效（共 " + std::to_string(mainBlocks.size()) +
                " 段），请用 %if 保证只有一个；要覆盖内建 demo main 也可以"
                "改用 %copy file 自己的 main 源文件",
            line);
    }
    out = mainBlocks.empty() ? std::string() : mainBlocks[0];
    if (!lineDirectives)
    {
        // Python 产物没有 #line 概念，%main 正文原样贴
        return true;
    }
    if (out.empty() || (request.lines == nullptr) ||
        (request.lines->mainLines == nullptr) ||
        request.lines->mainLines->empty() ||
        ((*request.lines->mainLines)[0] == 0u))
    {
        return true;
    }
    if (out.back() != '\n')
    {
        out += "\n";
    }
    // 段正文是从标记行的**下一行**开始的，所以前向 #line 指向 markerLine + 1，
    // 这样正文第一行才正好落在它自己的行号上
    out = "#line " + std::to_string((*request.lines->mainLines)[0] + 1u) +
          " \"" + CodeGen::grammarFilePlaceholder() + "\"\n" + out +
          "#line " + CodeGen::lineBackPlaceholder() + " \"" + outName +
          "\"\n";
    return true;
}

/**
 * @brief 把 %epilogue 正文追加到产物源码末尾（同时补 #line）。
 *
 * C++17 下先补一行 `using namespace <命名空间>;`：%epilogue 里的代码常常是
 * 照 C 产物写的（`Parser_RunText` 这类名字），这样能直接用。
 * @param request 组装入参。
 * @param outName 产物文件名（#line 恢复行指向它）。
 * @param content 产物正文（原地追加）。
 * @param outsideNamespace C++17 产物要在末尾补 `using namespace`。
 */
void appendEpilogue(const ProjectRequest &request, const std::string &outName,
                    bool outsideNamespace, std::string &content)
{
    if (!request.hasEpilogue || (request.epilogue == nullptr) ||
        request.epilogue->empty())
    {
        return;
    }
    if (!content.empty() && (content.back() != '\n'))
    {
        content += "\n";
    }
    content += "\n";
    if (outsideNamespace)
    {
        content += "using namespace " + CodeGenCpp17::namespaceName(request) +
                   ";\n\n";
    }
    // %epilogue 前后的 #line（行号先留占位符，下面统一回填）
    const bool hasLine = (request.lines != nullptr) &&
                         (request.lines->epilogueLine != 0u);
    if (hasLine)
    {
        // 正文从 %epilogue 的下一行开始：前向 #line 指向 +1
        content += "#line " +
                   std::to_string(request.lines->epilogueLine + 1u) + " \"" +
                   CodeGen::grammarFilePlaceholder() + "\"\n";
    }
    content += *request.epilogue;
    if (content.back() != '\n')
    {
        content += "\n";
    }
    if (hasLine)
    {
        content += "#line " + std::string(CodeGen::lineBackPlaceholder()) +
                   " \"" + outName + "\"\n";
    }
}

/**
 * @brief 手写后端的 RESULT.md 末尾按 `--full-report` 附上分析报告。
 *
 * C# / Rust / Go 三档的 RESULT.md 是逐行拼出来的字符串，**不走 `RESULT.md`
 * 模板**，模板里那把 `{% if full_report %}` 管不到它们；`--full-report` 要生效
 * 就得在这里补一次，语义与模板档一致（同一份报告原文、同一节标题）。
 *
 * @param request 组装入参。
 * @param text 已经拼好的 RESULT.md。
 * @return 追加了报告一节（未开开关时原样）的文本。
 */
std::string appendFullReport(const ProjectRequest &request, std::string text)
{
    if (!request.fullReport)
    {
        return text;
    }
    text += "\n## 文法分析报告\n\n```text\n";
    text += rstripWhitespace(request.reportText);
    text += "\n```\n";
    return text;
}

/**
 * @brief C# 产物的 RESULT.md（生成信息 + 产物清单 + 编译运行）。
 * @param request 组装入参。
 * @param mode 归一化后的生成模式。
 * @param productFiles 产物清单（按写出顺序）。
 * @param terminals Token 列表（词法模式列出 Token 表用）。
 * @param lexMode 是否词法模式。
 * @return RESULT.md 文本。
 */
std::string generatorCsharpResultMd(
    const ProjectRequest &request, const std::string &mode,
    const std::vector<std::string> &productFiles,
    const std::vector<std::string> &terminals, bool lexMode)
{
    std::string valueText = "`object`（缺省：词法器给出 `int` / `double` / "
                            "`string`）";
    if ((request.symbolValueMember != nullptr) && (request.unionMembers != nullptr))
    {
        valueText = "%union（生成带成员字段的 `ParserValue`）";
    }
    else if (request.valueType != nullptr)
    {
        valueText = "`%value_type " + *request.valueType + "`";
    }
    std::string text;
    text += "# 生成结果（C# 产物）\n\n";
    text += "- 文法文件：`";
    text += ((request.lines == nullptr) || request.lines->grammarPath.empty())
                ? std::string("<default_grammar.gram>")
                : request.lines->grammarPath;
    text += "`\n";
    text += "- 生成模式：" + modeLabelText(mode) +
            (lexMode ? "（只导出词法器，产物里没有解析器）" : "") + "\n";
    text += "- 目标语言：`--lang csharp`\n";
    if (!lexMode)
    {
        text += "- 语义值类型：" + valueText + "\n";
        text += std::string("- 语法树（`%tree`）：") +
                (request.tree ? "启用" : "未启用") + "\n";
    }
    if (!request.packageName.empty())
    {
        text += std::string("- `%package`：`") + request.packageName +
                "`（生成 namespace）\n";
    }
    if ((request.imports != nullptr) && !request.imports->empty())
    {
        text += "- `%import`：`using ";
        for (std::size_t index = 0u; index < request.imports->size(); ++index)
        {
            if (index != 0u)
            {
                text += "`、`";
            }
            text += (*request.imports)[index];
        }
        text += "`\n";
    }
    if ((request.classBody != nullptr) && !request.classBody->empty())
    {
        text += "- `%class_body`：有（正文插在解析器类内部）\n";
    }
    text += "\n## 产物\n\n";
    for (const std::string &name : productFiles)
    {
        text += "- `" + name + "`\n";
    }
    // 位置信息 / %commit：与 C/C++17/Python 的 RESULT.md 同一节口径
    text += "\n## 位置信息与 %commit\n\n";
    text += "| 内容 | 位置 | 说明 |\n| --- | --- | --- |\n";
    text += std::string("| `") +
            "Parser.CurrentLocation" +
            "` | `Parser.cs" +
            "` | 当前 lookahead 的位置（行 / 列 1 起，`Offset` 是 UTF-8 字节"
            "偏移） |\n";
    if ((request.lexSpec != nullptr) && !request.lexSpec->commits.empty())
    {
        text += std::string("| `") +
            "Parser.Commit / Parser.Commits" +
                "` | `Parser.cs" +
                "` | `%commit` 收集到的片段（按出现顺序；元素含行号 / 类型 / "
                "原文） |\n";
    }
    else
    {
        text += "| `%commit` | — | 本文法没有声明 `%commit`，容器恒为空 |\n";
    }
    text += "\n## 编译与运行\n\n";
    text += "```sh\n";
    text += "dotnet build\n";
    text += "printf '%s\\n' " + shellQuote(exampleInput(request)) +
            " | dotnet run    # 演示程序从标准输入读一行\n";
    text += "```\n\n";
    text += "只当解析库用时把 `Main.cs` 的 `OutputType=Exe` 改成 "
            "`Library`，或者直接引用 `Parser.cs` / `Token.cs`（同一个 "
            "namespace 或同一个工程）。\n";
    text += "\n";
    if (lexMode)
    {
        text += "## Token\n\n";
        text += "| Token | 写法 |\n| --- | --- |\n";
        const std::map<std::string, std::vector<std::string>> emptyDefs;
        const auto &tokenDefs =
            (request.tokenDefs == nullptr) ? emptyDefs : *request.tokenDefs;
        const LexSpec emptyLex;
        const LexSpec &lexSpec =
            (request.lexSpec == nullptr) ? emptyLex : *request.lexSpec;
        for (const std::string &name : terminals)
        {
            std::string how;
            const auto literalsIt = tokenDefs.find(name);
            const std::vector<std::string> *literals =
                literalsIt != tokenDefs.end() ? &literalsIt->second : nullptr;
            if (literals != nullptr)
            {
                how = "固定字面量";
                for (std::size_t index = 0u; index < literals->size(); ++index)
                {
                    how += (index == 0u) ? " " : " / ";
                    how += "`" + (*literals)[index] + "`";
                }
            }
            const auto patternIt = lexSpec.regex.find(name);
            const std::string *pattern =
                patternIt != lexSpec.regex.end() ? &patternIt->second : nullptr;
            if (pattern != nullptr)
            {
                how = "正则 `" + *pattern + "`";
            }
            if (how.empty())
            {
                how = "由词法层外部产生（没有字面量 / 正则）";
            }
            text += "| `" + name + "` | " + how + " |\n";
        }
        if (lexSpec.identifier.has_value())
        {
            text += "| `VARIABLE` | `%identifier` `" +
                    *lexSpec.identifier + "` |\n";
        }
        text += "\n";
    }
    text += "## 说明\n\n";
    text += "- 报错定位：C# 没有可移植的 `#line`，所以「来自 .gram 的"
            "代码段」\n";
    text += "  前后各插了一行注释——`// 在 <文法文件> 的第 <N> 行` 指明出处，\n";
    text += "  `// 继续 <产物文件> 的正文` 说明后面回到产物自己的正文。\n";
    text += "- 边界：不做嵌套 `%union`、不支持 `%destructor`、不做复杂类型跨"
            "语言映射。\n";
    text += "- 详细的分析报告（产生式 / FIRST / FOLLOW / 冲突）加 "
            "`--full-report` 会写在本文档末尾，也可以用 `--report FILE` "
            "单独导出一份。\n";
    return appendFullReport(request, std::move(text));
}

/**
 * @brief 生成 C# 产物工程（Parser.cs / Token.cs / Program.cs / parser.csproj）。
 * @param request 组装入参。
 * @param mode 归一化后的生成模式。
 * @param copies %copy 展开后的拷贝项。
 * @param analysis 分析结果。
 * @param out 输出文件名到文件内容的映射。
 * @param error 出错时写入错误说明。
 * @return 成功返回 true。
 */
bool generateCsharpProject(const ProjectRequest &request,
                           const std::string &mode,
                           const std::vector<CopyEntry> &copies,
                           const AnalysisResult &analysis,
                           std::map<std::string, std::string> &out,
                           Error &error)
{
    std::string parserText;
    if (!CodeGenCsharp::parserCs(request, mode, analysis, parserText, error))
    {
        return false;
    }
    std::string tokenText;
    CodeGenCsharp::tokenCs(request, analysis.terminals, tokenText);
    std::string mainBody;
    if (!buildMainBody(request, "Program.cs", mainBody, error,
                       /*lineDirectives=*/false))
    {
        return false;
    }
    if (!mainBody.empty() && (request.lines != nullptr) &&
        (request.lines->mainLines != nullptr) &&
        !request.lines->mainLines->empty() &&
        ((*request.lines->mainLines)[0] != 0u))
    {
        mainBody = CodeGenCsharp::withLineMarkers(
            mainBody, (*request.lines->mainLines)[0] + 1u, "", "Program.cs");
    }
    std::string mainText = CodeGenCsharp::mainCs(request, mode, mainBody);
    if (request.hasEpilogue && (request.epilogue != nullptr) &&
        !request.epilogue->empty())
    {
        if (!parserText.empty() && (parserText.back() != '\n'))
        {
            parserText += "\n";
        }
        parserText += "\n    // ---------- %epilogue（用户正文）----------\n";
        const std::size_t line = (request.lines == nullptr)
                                     ? 0u
                                     : request.lines->epilogueLine;
        parserText += CodeGenCsharp::withLineMarkers(
            *request.epilogue, (line == 0u) ? 0u : line + 1u, "", "Parser.cs");
    }
    const std::string grammarPath =
        ((request.lines == nullptr) || request.lines->grammarPath.empty())
            ? std::string("<default_grammar.gram>")
            : request.lines->grammarPath;
    parserText = replaceAll(parserText, CodeGen::grammarFilePlaceholder(),
                            grammarPath);
    tokenText = replaceAll(tokenText, CodeGen::grammarFilePlaceholder(),
                           grammarPath);
    mainText = replaceAll(mainText, CodeGen::grammarFilePlaceholder(),
                          grammarPath);
    std::vector<std::string> files = {"Parser.cs", "Token.cs", "Program.cs",
                                      "parser.csproj", "RESULT.md"};
    for (const CopyEntry &entry : copies)
    {
        files.push_back(entry.name);
    }
    std::map<std::string, std::string> project;
    project.insert_or_assign("Parser.cs", parserText);
    project.insert_or_assign("Token.cs", tokenText);
    project.insert_or_assign("Program.cs", mainText);
    project.insert_or_assign("parser.csproj", CodeGenCsharp::csproj(request));
    for (const CopyEntry &entry : copies)
    {
        project.insert_or_assign(entry.name, entry.content);
    }
    std::vector<std::string> baseFiles = {"Parser.cs", "Token.cs",
                                          "Program.cs", "parser.csproj"};
    for (const CopyEntry &entry : copies)
    {
        baseFiles.push_back(entry.name);
    }
    baseFiles.push_back("RESULT.md");
    project.insert_or_assign("RESULT.md", generatorCsharpResultMd(
                                 request, mode, baseFiles, analysis.terminals,
                                 /*lexMode=*/false));
    for (const std::string &name : files)
    {
        const auto contentIt = project.find(name);
        const std::string *content =
            contentIt != project.end() ? &contentIt->second : nullptr;
        if (content != nullptr)
        {
            out.insert_or_assign(name, *content);
        }
    }
    return true;
}

/**
 * @brief 生成词法模式（`%mode lex`）的 C# 产物。
 * @param request 组装入参。
 * @param copies %copy 展开后的拷贝项。
 * @param out 输出文件名到文件内容的映射。
 * @param error 出错时写入错误说明。
 * @return 成功返回 true。
 */
bool generateLexCsharpProject(const ProjectRequest &request,
                              const std::vector<CopyEntry> &copies,
                              std::map<std::string, std::string> &out,
                              Error &error)
{
    const std::vector<std::string> terminals = lexTerminals(request);
    std::string parserText;
    CodeGenCsharp::lexerCs(request, terminals, parserText);
    std::string tokenText;
    CodeGenCsharp::tokenCs(request, terminals, tokenText);
    std::string mainBody;
    if (!buildMainBody(request, "Program.cs", mainBody, error,
                       /*lineDirectives=*/false))
    {
        return false;
    }
    if (!mainBody.empty() && (request.lines != nullptr) &&
        (request.lines->mainLines != nullptr) &&
        !request.lines->mainLines->empty() &&
        ((*request.lines->mainLines)[0] != 0u))
    {
        mainBody = CodeGenCsharp::withLineMarkers(
            mainBody, (*request.lines->mainLines)[0] + 1u, "", "Program.cs");
    }
    std::string mainText = CodeGenCsharp::lexerMainCs(request, mainBody);
    if (request.hasEpilogue && (request.epilogue != nullptr) &&
        !request.epilogue->empty())
    {
        if (!parserText.empty() && (parserText.back() != '\n'))
        {
            parserText += "\n";
        }
        parserText += "\n    // ---------- %epilogue（用户正文）----------\n";
        const std::size_t line = (request.lines == nullptr)
                                     ? 0u
                                     : request.lines->epilogueLine;
        parserText += CodeGenCsharp::withLineMarkers(
            *request.epilogue, (line == 0u) ? 0u : line + 1u, "", "Parser.cs");
    }
    const std::string grammarPath =
        ((request.lines == nullptr) || request.lines->grammarPath.empty())
            ? std::string("<default_grammar.gram>")
            : request.lines->grammarPath;
    parserText = replaceAll(parserText, CodeGen::grammarFilePlaceholder(),
                            grammarPath);
    tokenText = replaceAll(tokenText, CodeGen::grammarFilePlaceholder(),
                           grammarPath);
    mainText = replaceAll(mainText, CodeGen::grammarFilePlaceholder(),
                          grammarPath);
    std::vector<std::string> files = {"Parser.cs", "Token.cs", "Program.cs",
                                      "parser.csproj", "RESULT.md"};
    for (const CopyEntry &entry : copies)
    {
        files.push_back(entry.name);
    }
    std::map<std::string, std::string> project;
    project.insert_or_assign("Parser.cs", parserText);
    project.insert_or_assign("Token.cs", tokenText);
    project.insert_or_assign("Program.cs", mainText);
    project.insert_or_assign("parser.csproj", CodeGenCsharp::csproj(request));
    for (const CopyEntry &entry : copies)
    {
        project.insert_or_assign(entry.name, entry.content);
    }
    std::vector<std::string> baseFiles = {"Parser.cs", "Token.cs",
                                          "Program.cs", "parser.csproj"};
    for (const CopyEntry &entry : copies)
    {
        baseFiles.push_back(entry.name);
    }
    baseFiles.push_back("RESULT.md");
    project.insert_or_assign("RESULT.md", generatorCsharpResultMd(
                                 request, "lex", baseFiles, terminals,
                                 /*lexMode=*/true));
    for (const std::string &name : files)
    {
        const auto contentIt = project.find(name);
        const std::string *content =
            contentIt != project.end() ? &contentIt->second : nullptr;
        if (content != nullptr)
        {
            out.insert_or_assign(name, *content);
        }
    }
    return true;
}

/**
 * @brief 生成 Rust 产物（Cargo 工程）的 RESULT.md。
 * @param request 组装入参。
 * @param mode 归一化后的生成模式。
 * @param productFiles 产物清单（按写出顺序）。
 * @param terminals Token 列表。
 * @param lexMode 是否词法模式（`%mode lex`）。
 * @return RESULT.md 文本。
 */
std::string generatorRustResultMd(const ProjectRequest &request,
                                  const std::string &mode,
                                  const std::vector<std::string> &productFiles,
                                  const std::vector<std::string> &terminals,
                                  bool lexMode)
{
    std::string valueText = "`i64`（缺省：数字 Token 给整数，其它 Token 给 0）";
    if ((request.symbolValueMember != nullptr) &&
        (request.unionMembers != nullptr))
    {
        valueText = "`%union`（生成带成员字段的 `ParserValue` 结构体）";
    }
    else if (request.valueType != nullptr)
    {
        valueText = "`%value_type " + *request.valueType + "`";
    }
    std::string text;
    text += "# 生成结果（Rust 产物）\n\n";
    text += "- 文法文件：`";
    text += ((request.lines == nullptr) || request.lines->grammarPath.empty())
                ? std::string("<default_grammar.gram>")
                : request.lines->grammarPath;
    text += "`\n";
    text += "- 生成模式：" + modeLabelText(mode) +
            (lexMode ? "（只导出词法器，产物里没有解析器）" : "") + "\n";
    text += "- 目标语言：`--lang rust`\n";
    text += "- crate 名：`" + CodeGenRust::crateName(request) + "`";
    if (!request.packageName.empty())
    {
        text += "（来自 `%package " + request.packageName +
                "`，点号换成下划线）";
    }
    text += "\n";
    if (!lexMode)
    {
        text += "- 语义值类型：" + valueText + "\n";
        text += "- 语法树（`%tree`）：" +
                std::string(request.tree ? "启用" : "未启用") + "\n";
    }
    if ((request.imports != nullptr) && !request.imports->empty())
    {
        text += "- `%import`：`use ";
        for (std::size_t index = 0u; index < request.imports->size(); ++index)
        {
            if (index != 0u)
            {
                text += "`、`";
            }
            text += (*request.imports)[index];
        }
        text += "`\n";
    }
    if ((request.classBody != nullptr) && !request.classBody->empty())
    {
        text += "- `%class_body`：有（正文插成一段 `impl Parser { ... }`）\n";
    }
    text += "\n## 产物\n\n";
    for (const std::string &name : productFiles)
    {
        text += "- `" + name + "`\n";
    }
    // 位置信息 / %commit：与另外几档同一节口径（Rust 的类型在 src/parser.rs）
    text += "\n## 位置信息与 %commit\n\n";
    text += "| 内容 | 位置 | 说明 |\n| --- | --- | --- |\n";
    text += "| `ParserLocation` / `current_location()` | `src/parser.rs` | "
            "当前 lookahead 的位置（行 / 列 1 起，`offset` 是 UTF-8 字节偏移） |\n";
    if ((request.lexSpec != nullptr) && !request.lexSpec->commits.empty())
    {
        text += "| `ParserCommit` / `commits()` | `src/parser.rs` | "
                "`%commit` 收集到的片段（按出现顺序；字段 `line` / `kind` / "
                "`text`） |\n";
    }
    else
    {
        text += "| `%commit` | — | 本文法没有声明 `%commit`，容器恒为空 |\n";
    }
    text += "\n## 编译与运行\n\n";
    text += "```sh\n";
    text += "cargo build\n";
    text += "printf '%s\\n' " + shellQuote(exampleInput(request)) +
            " | cargo run --quiet    # 演示程序从标准输入读一行\n";
    text += "```\n\n";
    text += "只当解析库用时：去掉 `src/main.rs`，把 `src/parser.rs` / "
            "`src/token.rs` 加进你自己的 crate 当模块用。\n\n";
    text += "## 语义值类型的要求（重要）\n\n";
    text += "Rust 的语义值在分析栈上**按值拷贝**，所以语义值类型要同时满足\n";
    text += "`Copy + Default + Debug`：\n\n";
    text += "- 标量（`i64` / `f64` / `bool` / `u8` …）天然满足；\n";
    text += "- `%union` 生成的 `ParserValue` 会自带 "
            "`#[derive(Clone, Copy, Default, Debug)]`，只要成员类型也满足这三条；\n";
    text += "- `String` / `Vec<T>` / `Box<T>` 这类**非 `Copy`** 类型不适用"
            "（编译期会报 `the trait bound ... Copy ... is not satisfied`）；\n";
    text += "- 用 `%value_type` 指定自己的类型时，请给它 "
            "`#[derive(Clone, Copy, Default, Debug)]`（或多个类型共同的 "
            "`impl`）。\n\n";
    text += "`Copy` 之外的三个 trait 里，`Debug` 只有演示程序打印结果时才需要"
            "（`println!(\"{:?}\", engine.result())`）。\n";
    text += "\n";
    if (lexMode)
    {
        text += "## Token\n\n";
        text += "| Token | 写法 |\n| --- | --- |\n";
        const std::map<std::string, std::vector<std::string>> emptyDefs;
        const auto &tokenDefs =
            (request.tokenDefs == nullptr) ? emptyDefs : *request.tokenDefs;
        const LexSpec emptyLex;
        const LexSpec &lexSpec =
            (request.lexSpec == nullptr) ? emptyLex : *request.lexSpec;
        for (const std::string &name : terminals)
        {
            std::string how;
            const auto literalsIt = tokenDefs.find(name);
            const std::vector<std::string> *literals =
                literalsIt != tokenDefs.end() ? &literalsIt->second : nullptr;
            if (literals != nullptr)
            {
                how = "固定字面量";
                for (std::size_t index = 0u; index < literals->size(); ++index)
                {
                    how += (index == 0u) ? " " : " / ";
                    how += "`" + (*literals)[index] + "`";
                }
            }
            const auto patternIt = lexSpec.regex.find(name);
            const std::string *pattern =
                patternIt != lexSpec.regex.end() ? &patternIt->second : nullptr;
            if (pattern != nullptr)
            {
                how = "正则 `" + *pattern + "`";
            }
            if (how.empty())
            {
                how = "由词法层外部产生（没有字面量 / 正则）";
            }
            text += "| `" + name + "` | " + how + " |\n";
        }
        if (lexSpec.identifier.has_value())
        {
            text += "| `VARIABLE` | `%identifier` `" +
                    *lexSpec.identifier + "` |\n";
        }
        text += "\n";
    }
    text += "## 说明\n\n";
    text += "- 报错定位：Rust 没有可移植的 `#line`，所以「来自 .gram 的代码段」\n";
    text += "  前后各插了一行注释——`// 在 <文法文件> 的第 <N> 行` 指明出处，\n";
    text += "  `// 继续 <产物文件> 的正文` 说明后面回到产物自己的正文。\n";
    text += "- 边界：不做嵌套 `%union`、不支持 `%destructor`、不做复杂类型跨"
            "语言映射。\n";
    text += "- 详细的分析报告（产生式 / FIRST / FOLLOW / 冲突）加 "
            "`--full-report` 会写在本文档末尾，也可以用 `--report FILE` "
            "单独导出一份。\n";
    return appendFullReport(request, std::move(text));
}

/**
 * @brief 生成 Rust 产物工程（Cargo 工程：Cargo.toml + src 下的三个 .rs）。
 * @param request 组装入参。
 * @param mode 归一化后的生成模式。
 * @param copies %copy 展开后的拷贝项。
 * @param analysis 分析结果。
 * @param out 输出文件名到文件内容的映射。
 * @param error 出错时写入错误说明。
 * @return 成功返回 true。
 */
bool generateRustProject(const ProjectRequest &request,
                         const std::string &mode,
                         const std::vector<CopyEntry> &copies,
                         const AnalysisResult &analysis,
                         std::map<std::string, std::string> &out,
                         Error &error)
{
    std::string parserText;
    if (!CodeGenRust::parserRs(request, mode, analysis, parserText, error))
    {
        return false;
    }
    std::string tokenText;
    CodeGenRust::tokenRs(request, analysis.terminals, tokenText);
    std::string mainBody;
    if (!buildMainBody(request, "src/main.rs", mainBody, error,
                       /*lineDirectives=*/false))
    {
        return false;
    }
    if (!mainBody.empty() && (request.lines != nullptr) &&
        (request.lines->mainLines != nullptr) &&
        !request.lines->mainLines->empty() &&
        ((*request.lines->mainLines)[0] != 0u))
    {
        mainBody = CodeGenRust::withLineMarkers(
            mainBody, (*request.lines->mainLines)[0] + 1u, "", "main.rs");
    }
    std::string mainText = CodeGenRust::mainRs(request, mode, mainBody);
    if (request.hasEpilogue && (request.epilogue != nullptr) &&
        !request.epilogue->empty())
    {
        if (!parserText.empty() && (parserText.back() != '\n'))
        {
            parserText += "\n";
        }
        parserText += "\n// ---------- %epilogue（用户正文）----------\n";
        const std::size_t line = (request.lines == nullptr)
                                     ? 0u
                                     : request.lines->epilogueLine;
        parserText += CodeGenRust::withLineMarkers(
            *request.epilogue, (line == 0u) ? 0u : line + 1u, "", "parser.rs");
    }
    const std::string grammarPath =
        ((request.lines == nullptr) || request.lines->grammarPath.empty())
            ? std::string("<default_grammar.gram>")
            : request.lines->grammarPath;
    parserText = replaceAll(parserText, CodeGen::grammarFilePlaceholder(),
                            grammarPath);
    tokenText = replaceAll(tokenText, CodeGen::grammarFilePlaceholder(),
                           grammarPath);
    mainText = replaceAll(mainText, CodeGen::grammarFilePlaceholder(),
                          grammarPath);
    std::vector<std::string> files = {"Cargo.toml", "src/parser.rs",
                                      "src/token.rs", "src/main.rs",
                                      "RESULT.md"};
    for (const CopyEntry &entry : copies)
    {
        files.push_back(entry.name);
    }
    std::map<std::string, std::string> project;
    project.insert_or_assign("Cargo.toml", CodeGenRust::cargoToml(request));
    project.insert_or_assign("src/parser.rs", parserText);
    project.insert_or_assign("src/token.rs", tokenText);
    project.insert_or_assign("src/main.rs", mainText);
    for (const CopyEntry &entry : copies)
    {
        project.insert_or_assign(entry.name, entry.content);
    }
    std::vector<std::string> baseFiles = {"Cargo.toml", "src/parser.rs",
                                          "src/token.rs", "src/main.rs"};
    for (const CopyEntry &entry : copies)
    {
        baseFiles.push_back(entry.name);
    }
    baseFiles.push_back("RESULT.md");
    project.insert_or_assign("RESULT.md", generatorRustResultMd(
                                 request, mode, baseFiles, analysis.terminals,
                                 /*lexMode=*/false));
    for (const std::string &name : files)
    {
        const auto contentIt = project.find(name);
        const std::string *content =
            contentIt != project.end() ? &contentIt->second : nullptr;
        if (content != nullptr)
        {
            out.insert_or_assign(name, *content);
        }
    }
    return true;
}

/**
 * @brief 生成词法模式（`%mode lex`）的 Rust 产物。
 * @param request 组装入参。
 * @param copies %copy 展开后的拷贝项。
 * @param out 输出文件名到文件内容的映射。
 * @param error 出错时写入错误说明。
 * @return 成功返回 true。
 */
bool generateLexRustProject(const ProjectRequest &request,
                            const std::vector<CopyEntry> &copies,
                            std::map<std::string, std::string> &out,
                            Error &error)
{
    const std::vector<std::string> terminals = lexTerminals(request);
    std::string parserText;
    CodeGenRust::lexerRs(request, terminals, parserText);
    std::string tokenText;
    CodeGenRust::tokenRs(request, terminals, tokenText);
    std::string mainBody;
    if (!buildMainBody(request, "src/main.rs", mainBody, error,
                       /*lineDirectives=*/false))
    {
        return false;
    }
    if (!mainBody.empty() && (request.lines != nullptr) &&
        (request.lines->mainLines != nullptr) &&
        !request.lines->mainLines->empty() &&
        ((*request.lines->mainLines)[0] != 0u))
    {
        mainBody = CodeGenRust::withLineMarkers(
            mainBody, (*request.lines->mainLines)[0] + 1u, "", "main.rs");
    }
    std::string mainText = CodeGenRust::lexerMainRs(request, mainBody);
    if (request.hasEpilogue && (request.epilogue != nullptr) &&
        !request.epilogue->empty())
    {
        if (!parserText.empty() && (parserText.back() != '\n'))
        {
            parserText += "\n";
        }
        parserText += "\n// ---------- %epilogue（用户正文）----------\n";
        const std::size_t line = (request.lines == nullptr)
                                     ? 0u
                                     : request.lines->epilogueLine;
        parserText += CodeGenRust::withLineMarkers(
            *request.epilogue, (line == 0u) ? 0u : line + 1u, "", "parser.rs");
    }
    const std::string grammarPath =
        ((request.lines == nullptr) || request.lines->grammarPath.empty())
            ? std::string("<default_grammar.gram>")
            : request.lines->grammarPath;
    parserText = replaceAll(parserText, CodeGen::grammarFilePlaceholder(),
                            grammarPath);
    tokenText = replaceAll(tokenText, CodeGen::grammarFilePlaceholder(),
                           grammarPath);
    mainText = replaceAll(mainText, CodeGen::grammarFilePlaceholder(),
                          grammarPath);
    std::vector<std::string> files = {"Cargo.toml", "src/parser.rs",
                                      "src/token.rs", "src/main.rs",
                                      "RESULT.md"};
    for (const CopyEntry &entry : copies)
    {
        files.push_back(entry.name);
    }
    std::map<std::string, std::string> project;
    project.insert_or_assign("Cargo.toml", CodeGenRust::cargoToml(request));
    project.insert_or_assign("src/parser.rs", parserText);
    project.insert_or_assign("src/token.rs", tokenText);
    project.insert_or_assign("src/main.rs", mainText);
    for (const CopyEntry &entry : copies)
    {
        project.insert_or_assign(entry.name, entry.content);
    }
    std::vector<std::string> baseFiles = {"Cargo.toml", "src/parser.rs",
                                          "src/token.rs", "src/main.rs"};
    for (const CopyEntry &entry : copies)
    {
        baseFiles.push_back(entry.name);
    }
    baseFiles.push_back("RESULT.md");
    project.insert_or_assign("RESULT.md",
                             generatorRustResultMd(request, "lex", baseFiles,
                                                   terminals,
                                                   /*lexMode=*/true));
    for (const std::string &name : files)
    {
        const auto contentIt = project.find(name);
        const std::string *content =
            contentIt != project.end() ? &contentIt->second : nullptr;
        if (content != nullptr)
        {
            out.insert_or_assign(name, *content);
        }
    }
    return true;
}

/**
 * @brief 生成 Go 产物（Go 模块）的 RESULT.md。
 * @param request 组装入参。
 * @param mode 归一化后的生成模式。
 * @param productFiles 产物清单（按写出顺序）。
 * @param terminals Token 列表。
 * @param lexMode 是否词法模式（`%mode lex`）。
 * @return RESULT.md 文本。
 */
std::string generatorGoResultMd(const ProjectRequest &request,
                                const std::string &mode,
                                const std::vector<std::string> &productFiles,
                                const std::vector<std::string> &terminals,
                                bool lexMode)
{
    std::string valueText =
        "`int64`（缺省：数字 Token 给整数，其它 Token 给零值）";
    if ((request.symbolValueMember != nullptr) &&
        (request.unionMembers != nullptr))
    {
        valueText = "`%union`（生成带成员字段的 `ParserValue` 结构体）";
    }
    else if (request.valueType != nullptr)
    {
        valueText = "`%value_type " + *request.valueType + "`";
    }
    std::string text;
    text += "# 生成结果（Go 产物）\n\n";
    text += "- 文法文件：`";
    text += ((request.lines == nullptr) || request.lines->grammarPath.empty())
                ? std::string("<default_grammar.gram>")
                : request.lines->grammarPath;
    text += "`\n";
    text += "- 生成模式：" + modeLabelText(mode) +
            (lexMode ? "（只导出词法器，产物里没有解析器）" : "") + "\n";
    text += "- 目标语言：`--lang go`\n";
    text += "- module path：`" + CodeGenGo::moduleName(request) + "`";
    if (!request.packageName.empty())
    {
        text += "（来自 `%package " + request.packageName +
                "`，`::` 归一成 `/`）";
    }
    text += "\n";
    if (!lexMode)
    {
        text += "- 语义值类型：" + valueText + "\n";
        text += "- 语法树（`%tree`）：" +
                std::string(request.tree ? "启用" : "未启用") + "\n";
    }
    if ((request.imports != nullptr) && !request.imports->empty())
    {
        text += "- `%import`：`import ";
        for (std::size_t index = 0u; index < request.imports->size(); ++index)
        {
            if (index != 0u)
            {
                text += "`、`";
            }
            text += (*request.imports)[index];
        }
        text += "`\n";
    }
    if ((request.classBody != nullptr) && !request.classBody->empty())
    {
        text += "- `%class_body`：有（正文贴成一组 `func (p *Parser) …` 方法）\n";
    }
    text += "\n## 产物\n\n";
    for (const std::string &name : productFiles)
    {
        text += "- `" + name + "`\n";
    }
    // 位置信息 / %commit：与另外几档同一节口径（Go 的类型在 parser.go）
    text += "\n## 位置信息与 %commit\n\n";
    text += "| 内容 | 位置 | 说明 |\n| --- | --- | --- |\n";
    text += "| `ParserLocation` / `CurrentLocation()` | `parser.go` | "
            "当前 lookahead 的位置（行 / 列 1 起，`Offset` 是 UTF-8 字节偏移） |\n";
    if ((request.lexSpec != nullptr) && !request.lexSpec->commits.empty())
    {
        text += "| `ParserCommit` / `Commits()` | `parser.go` | "
                "`%commit` 收集到的片段（按出现顺序；字段 `Line` / `Type` / "
                "`Text`） |\n";
    }
    else
    {
        text += "| `%commit` | — | 本文法没有声明 `%commit`，容器恒为空 |\n";
    }
    text += "\n## 编译与运行\n\n";
    text += "```sh\n";
    text += "go build\n";
    text += "printf '%s\\n' " + shellQuote(exampleInput(request)) +
            " | go run .    # 演示程序从标准输入读一行\n";
    text += "```\n\n";
    text += "只当解析库用时：删掉 `main.go`，把 `parser.go` / `token.go` 加进"
            "你自己的包里\n（它们都是 `package main`，改成你的包名即可）。\n\n";
    text += "## 语义值类型的要求\n\n";
    text += "Go 的语义值在分析栈上**按值拷贝**（`int64` 这类标量、"
            "`%union` 生成的\n`ParserValue` 结构体都是值）：\n\n";
    text += "- 标量（`int64` / `float64` / `bool` / `string` …）直接可用；\n";
    text += "- `%union` 生成的 `ParserValue` 是普通结构体，成员声明照 C 的写法"
            "（`int32 ival;`）\n  拼成 Go 的字段写法（`ival int32`），"
            "类型要能零值初始化；\n";
    text += "- 切片 / map 这类引用类型也能用（拷贝的是引用），但语义动作里要"
            "注意\n  多个槽位共享同一份底层数据的情况；\n";
    text += "- 用 `%value_type` 指定自己的类型时，它要是可比较 / 可赋值的"
            "普通 Go 类型。\n";
    text += "\n";
    if (lexMode)
    {
        text += "## Token\n\n";
        text += "| Token | 写法 |\n| --- | --- |\n";
        const std::map<std::string, std::vector<std::string>> emptyDefs;
        const auto &tokenDefs =
            (request.tokenDefs == nullptr) ? emptyDefs : *request.tokenDefs;
        const LexSpec emptyLex;
        const LexSpec &lexSpec =
            (request.lexSpec == nullptr) ? emptyLex : *request.lexSpec;
        for (const std::string &name : terminals)
        {
            std::string how;
            const auto literalsIt = tokenDefs.find(name);
            const std::vector<std::string> *literals =
                literalsIt != tokenDefs.end() ? &literalsIt->second : nullptr;
            if (literals != nullptr)
            {
                how = "固定字面量";
                for (std::size_t index = 0u; index < literals->size(); ++index)
                {
                    how += (index == 0u) ? " " : " / ";
                    how += "`" + (*literals)[index] + "`";
                }
            }
            const auto patternIt = lexSpec.regex.find(name);
            const std::string *pattern =
                patternIt != lexSpec.regex.end() ? &patternIt->second : nullptr;
            if (pattern != nullptr)
            {
                how = "正则 `" + *pattern + "`";
            }
            if (how.empty())
            {
                how = "由词法层外部产生（没有字面量 / 正则）";
            }
            text += "| `" + name + "` | " + how + " |\n";
        }
        if (lexSpec.identifier.has_value())
        {
            text += "| `VARIABLE` | `%identifier` `" +
                    *lexSpec.identifier + "` |\n";
        }
        text += "\n";
    }
    text += "## 说明\n\n";
    text += "- 报错定位：Go 没有可移植的 `#line`，所以「来自 .gram 的代码段」\n";
    text += "  前后各插了一行注释——`// 在 <文法文件> 的第 <N> 行` 指明出处，\n";
    text += "  `// 继续 <产物文件> 的正文` 说明后面回到产物自己的正文。\n";
    text += "- 词法正则用标准库的 `regexp`（RE2）：`%token regex` / `%identifier` /\n";
    text += "  `%commit` 的模式原样交给它，产物里没有生成期算好的 DFA 表"
            "（`%commit`\n  相关的表只在文法里写了 `%commit` 时才出现）。\n";
    text += "- 边界：不做嵌套 `%union`、不支持 `%destructor`、不做复杂类型跨"
            "语言映射。\n";
    text += "- 详细的分析报告（产生式 / FIRST / FOLLOW / 冲突）加 "
            "`--full-report` 会写在本文档末尾，也可以用 `--report FILE` "
            "单独导出一份。\n";
    return appendFullReport(request, std::move(text));
}

/**
 * @brief 生成 Go 产物工程（Go 模块：go.mod + parser.go / token.go / main.go）。
 * @param request 组装入参。
 * @param mode 归一化后的生成模式。
 * @param copies %copy 展开后的拷贝项。
 * @param analysis 分析结果。
 * @param out 输出文件名到文件内容的映射。
 * @param error 出错时写入错误说明。
 * @return 成功返回 true。
 */
bool generateGoProject(const ProjectRequest &request, const std::string &mode,
                       const std::vector<CopyEntry> &copies,
                       const AnalysisResult &analysis,
                       std::map<std::string, std::string> &out, Error &error)
{
    std::string parserText;
    if (!CodeGenGo::parserGo(request, mode, analysis, parserText, error))
    {
        return false;
    }
    std::string tokenText;
    CodeGenGo::tokenGo(request, analysis.terminals, tokenText);
    std::string mainBody;
    if (!buildMainBody(request, "main.go", mainBody, error,
                       /*lineDirectives=*/false))
    {
        return false;
    }
    if (!mainBody.empty() && (request.lines != nullptr) &&
        (request.lines->mainLines != nullptr) &&
        !request.lines->mainLines->empty() &&
        ((*request.lines->mainLines)[0] != 0u))
    {
        mainBody = CodeGenGo::withLineMarkers(
            mainBody, (*request.lines->mainLines)[0] + 1u, "", "main.go");
    }
    std::string mainText = CodeGenGo::mainGo(request, mode, mainBody);
    if (request.hasEpilogue && (request.epilogue != nullptr) &&
        !request.epilogue->empty())
    {
        if (!parserText.empty() && (parserText.back() != '\n'))
        {
            parserText += "\n";
        }
        parserText += "\n// ---------- %epilogue（用户正文）----------\n";
        const std::size_t line = (request.lines == nullptr)
                                     ? 0u
                                     : request.lines->epilogueLine;
        parserText += CodeGenGo::withLineMarkers(
            *request.epilogue, (line == 0u) ? 0u : line + 1u, "", "parser.go");
    }
    const std::string grammarPath =
        ((request.lines == nullptr) || request.lines->grammarPath.empty())
            ? std::string("<default_grammar.gram>")
            : request.lines->grammarPath;
    parserText = replaceAll(parserText, CodeGen::grammarFilePlaceholder(),
                            grammarPath);
    tokenText = replaceAll(tokenText, CodeGen::grammarFilePlaceholder(),
                           grammarPath);
    mainText = replaceAll(mainText, CodeGen::grammarFilePlaceholder(),
                          grammarPath);
    std::vector<std::string> files = {"go.mod", "parser.go", "token.go",
                                      "main.go", "RESULT.md"};
    for (const CopyEntry &entry : copies)
    {
        files.push_back(entry.name);
    }
    std::map<std::string, std::string> project;
    project.insert_or_assign("go.mod", CodeGenGo::goMod(request));
    project.insert_or_assign("parser.go", parserText);
    project.insert_or_assign("token.go", tokenText);
    project.insert_or_assign("main.go", mainText);
    for (const CopyEntry &entry : copies)
    {
        project.insert_or_assign(entry.name, entry.content);
    }
    std::vector<std::string> baseFiles = {"go.mod", "parser.go", "token.go",
                                          "main.go"};
    for (const CopyEntry &entry : copies)
    {
        baseFiles.push_back(entry.name);
    }
    baseFiles.push_back("RESULT.md");
    project.insert_or_assign("RESULT.md",
                             generatorGoResultMd(request, mode, baseFiles,
                                                 analysis.terminals,
                                                 /*lexMode=*/false));
    for (const std::string &name : files)
    {
        const auto contentIt = project.find(name);
        const std::string *content =
            contentIt != project.end() ? &contentIt->second : nullptr;
        if (content != nullptr)
        {
            out.insert_or_assign(name, *content);
        }
    }
    return true;
}

/**
 * @brief 生成词法模式（`%mode lex`）的 Go 产物。
 * @param request 组装入参。
 * @param copies %copy 展开后的拷贝项。
 * @param out 输出文件名到文件内容的映射。
 * @param error 出错时写入错误说明。
 * @return 成功返回 true。
 */
bool generateLexGoProject(const ProjectRequest &request,
                          const std::vector<CopyEntry> &copies,
                          std::map<std::string, std::string> &out,
                          Error &error)
{
    const std::vector<std::string> terminals = lexTerminals(request);
    std::string parserText;
    CodeGenGo::lexerGo(request, terminals, parserText);
    std::string tokenText;
    CodeGenGo::tokenGo(request, terminals, tokenText);
    std::string mainBody;
    if (!buildMainBody(request, "main.go", mainBody, error,
                       /*lineDirectives=*/false))
    {
        return false;
    }
    if (!mainBody.empty() && (request.lines != nullptr) &&
        (request.lines->mainLines != nullptr) &&
        !request.lines->mainLines->empty() &&
        ((*request.lines->mainLines)[0] != 0u))
    {
        mainBody = CodeGenGo::withLineMarkers(
            mainBody, (*request.lines->mainLines)[0] + 1u, "", "main.go");
    }
    std::string mainText = CodeGenGo::lexerMainGo(request, mainBody);
    if (request.hasEpilogue && (request.epilogue != nullptr) &&
        !request.epilogue->empty())
    {
        if (!parserText.empty() && (parserText.back() != '\n'))
        {
            parserText += "\n";
        }
        parserText += "\n// ---------- %epilogue（用户正文）----------\n";
        const std::size_t line = (request.lines == nullptr)
                                     ? 0u
                                     : request.lines->epilogueLine;
        parserText += CodeGenGo::withLineMarkers(
            *request.epilogue, (line == 0u) ? 0u : line + 1u, "", "parser.go");
    }
    const std::string grammarPath =
        ((request.lines == nullptr) || request.lines->grammarPath.empty())
            ? std::string("<default_grammar.gram>")
            : request.lines->grammarPath;
    parserText = replaceAll(parserText, CodeGen::grammarFilePlaceholder(),
                            grammarPath);
    tokenText = replaceAll(tokenText, CodeGen::grammarFilePlaceholder(),
                           grammarPath);
    mainText = replaceAll(mainText, CodeGen::grammarFilePlaceholder(),
                          grammarPath);
    std::vector<std::string> files = {"go.mod", "parser.go", "token.go",
                                      "main.go", "RESULT.md"};
    for (const CopyEntry &entry : copies)
    {
        files.push_back(entry.name);
    }
    std::map<std::string, std::string> project;
    project.insert_or_assign("go.mod", CodeGenGo::goMod(request));
    project.insert_or_assign("parser.go", parserText);
    project.insert_or_assign("token.go", tokenText);
    project.insert_or_assign("main.go", mainText);
    for (const CopyEntry &entry : copies)
    {
        project.insert_or_assign(entry.name, entry.content);
    }
    std::vector<std::string> baseFiles = {"go.mod", "parser.go", "token.go",
                                          "main.go"};
    for (const CopyEntry &entry : copies)
    {
        baseFiles.push_back(entry.name);
    }
    baseFiles.push_back("RESULT.md");
    project.insert_or_assign("RESULT.md",
                             generatorGoResultMd(request, "lex", baseFiles,
                                                 terminals, /*lexMode=*/true));
    for (const std::string &name : files)
    {
        const auto contentIt = project.find(name);
        const std::string *content =
            contentIt != project.end() ? &contentIt->second : nullptr;
        if (content != nullptr)
        {
            out.insert_or_assign(name, *content);
        }
    }
    return true;
}

/**
 * @brief `--lang gram` 导出到产物目录的文法文件名。
 *
 * 沿用输入文件的名字（`example/json.gram` → `json.gram`），这样「原文件导出」
 * 一眼看得出是哪份文法；没给文件（走内建 default_grammar.gram）时叫
 * `grammar.gram`。
 * @param request 组装入参。
 * @return 产物目录里的文件名（不含目录）。
 */
std::string grammarExportName(const ProjectRequest &request)
{
    if (request.lines == nullptr)
    {
        return "grammar.gram";
    }
    const std::string &path = request.lines->grammarPath;
    if (path.empty())
    {
        return "grammar.gram";
    }
    // `std::filesystem::path` 在 Windows 上同时认 `/` 与 `\`，Linux 上 `\` 是
    // 普通字符（那种路径本来也不合法），所以取文件名直接用它。
    const std::string name = std::filesystem::u8path(path).filename().u8string();
    return name.empty() ? std::string("grammar.gram") : name;
}

/**
 * @brief 文法导出（`--lang gram`）里分析报告的文件名。
 *
 * 与导出的文法原文件成对：`pl0.gram` → `pl0.analysis.md`（没有扩展名时直接
 * 加后缀，例如 `Makefile` → `Makefile.analysis.md`），这样一份文法对应一份
 * 分析，摆在一起一眼看得出是哪份文法的报告。
 * @param exportName 文法原文件在产物目录里的名字。
 * @return 分析报告的文件名（不含目录）。
 */
std::string grammarAnalysisName(const std::string &exportName)
{
    const std::filesystem::path path=std::filesystem::u8path(exportName);
    const std::string stem = path.stem().u8string();
    if (stem.empty())
    {
        return "grammar.analysis.md";
    }
    return stem + ".analysis.md";
}

/**
 * @brief 文法导出（`--lang gram`）的分析报告（`<文法名>.analysis.md`）：
 *        生成信息 + 冲突消解 + 未消解冲突 + 文法分析报告全文。
 *
 * 正文里的分析报告就是 `--report` 写出来的那份（逐字），单独成文件是为了
 * 「文法 + 报告」一起归档时，报告有自己的名字、不和生成说明混在一起。
 * @param request 组装入参。
 * @param mode 归一化后的生成模式。
 * @param exportName 文法原文件在产物目录里的名字。
 * @param lexMode 是否词法模式（词法模式没有分析表，报告只有词法一节）。
 * @return 分析报告文本。
 */
std::string generatorGramAnalysisMd(const ProjectRequest &request,
                                    const std::string &mode,
                                    const std::string &exportName, bool lexMode)
{
    const std::string sourcePath =
        ((request.lines == nullptr) || request.lines->grammarPath.empty())
            ? std::string("（内建 default_grammar.gram）")
            : request.lines->grammarPath;
    std::string text;
    text += "# 文法分析报告（" + modeLabelText(mode) + "）\n\n";
    text += "- 源文法：`" + sourcePath + "`\n";
    text += "- 导出文件：`" + exportName + "`（输入文件原样，逐字节）\n";
    text += "- 目标：`--lang gram`（只要文法与分析报告，不要代码）\n";
    if (lexMode)
    {
        text += "- 这一份是 `%mode lex`（词法模式）：没有分析表，下面是词法一节 + Token 表\n";
        text += "\n";
        // 词法模式：把 Token 表列出来（与词法产物的 RESULT.md 同一套口径）
        text += "## Token\n\n";
        text += "| Token | 写法 |\n| --- | --- |\n";
        const std::map<std::string, std::vector<std::string>> emptyDefs;
        const auto &tokenDefs =
            (request.tokenDefs == nullptr) ? emptyDefs : *request.tokenDefs;
        const LexSpec emptyLex;
        const LexSpec &lexSpec =
            (request.lexSpec == nullptr) ? emptyLex : *request.lexSpec;
        for (const std::string &name : lexTerminals(request))
        {
            std::string how;
            const auto literalsIt = tokenDefs.find(name);
            const std::vector<std::string> *literals =
                literalsIt != tokenDefs.end() ? &literalsIt->second : nullptr;
            if (literals != nullptr)
            {
                how = "固定字面量";
                for (std::size_t index = 0u; index < literals->size(); ++index)
                {
                    how += (index == 0u) ? " " : " / ";
                    how += "`" + (*literals)[index] + "`";
                }
            }
            const auto patternIt = lexSpec.regex.find(name);
            const std::string *pattern =
                patternIt != lexSpec.regex.end() ? &patternIt->second : nullptr;
            if (pattern != nullptr)
            {
                how = "正则 `" + *pattern + "`";
            }
            if (how.empty())
            {
                how = "由词法层外部产生（没有字面量 / 正则）";
            }
            text += "| `" + name + "` | " + how + " |\n";
        }
        if (lexSpec.identifier.has_value())
        {
            text += "| `VARIABLE` | `%identifier` `" + *lexSpec.identifier +
                    "` |\n";
        }
        text += "\n";
    }
    else
    {
        const AnalysisResult *analysis = request.analysis;
        std::string resolutionsText;
        std::string unresolvedText;
        if (analysis != nullptr)
        {
            for (const LrResolution &entry : analysis->resolutions)
            {
                if (!resolutionsText.empty())
                {
                    resolutionsText += "\n";
                }
                resolutionsText += resolutionLine(entry);
            }
            for (const LrConflict &entry : analysis->unresolved)
            {
                if (!unresolvedText.empty())
                {
                    unresolvedText += "\n";
                }
                unresolvedText += entry.text;
            }
        }
        if (resolutionsText.empty())
        {
            resolutionsText = "（无）";
        }
        if (unresolvedText.empty())
        {
            unresolvedText = "（无）";
        }
        text += "\n## 冲突消解\n\n" + resolutionsText + "\n\n";
        text += "## 未消解冲突\n\n" + unresolvedText + "\n\n";
    }
    text += "## 文法分析报告\n\n";
    text += "```text\n" + rstripWhitespace(request.reportText) + "\n```\n";
    text += "\n## 说明\n\n";
    text += "- 这一份就是 `--report FILE` 写出来的那份分析（逐字），换 `--mode`\n";
    text += "  再导一次就是另一套分析（产生式 / FIRST / FOLLOW / 状态机 / 冲突）。\n";
    if (lexMode)
    {
        text += "- `--lang gram` 这一档**不生成词法器**：要词法器请用 `--lang c` / `c++17` /\n";
        text += "  `python` / `csharp` / `rust` / `go` 配 `--mode lex`；\n";
        text += "  词法分析的结果在上面的报告与 Token 表里（固定字面量 /\n";
        text += "  `%token regex` / `%identifier`）。\n";
    }
    else
    {
        text += "- 这一档**不生成解析器 / 词法器**，也没有构建脚本：产物目录里\n";
        text += "  只有文法原文件、本文件与 `RESULT.md`。\n";
    }
    text += "- 报错定位：报告里的行号就是 `.gram` 的行号（这一档不生成代码，"
            "所以没有\n  `#line` / 行标记那一层）。\n";
    text += "- 边界：不做嵌套 `%union`、不支持 `%destructor`、不做复杂类型跨"
            "语言映射——\n  这些边界是给代码生成档的，文法导出档不涉及。\n";
    return text;
}

/**
 * @brief 文法导出（`--lang gram`）的 RESULT.md：生成信息 + 产物清单 + 怎么用。
 *
 * 分析报告单独放在 `<文法名>.analysis.md` 里（见 generatorGramAnalysisMd），
 * 这份文档只讲「这一档产出了什么、怎么用」，不重复贴报告正文。
 * @param request 组装入参。
 * @param mode 归一化后的生成模式。
 * @param productFiles 产物清单（按写出顺序）。
 * @param exportName 文法原文件在产物目录里的名字。
 * @param analysisName 分析报告的文件名。
 * @param lexMode 是否词法模式。
 * @return RESULT.md 文本。
 */
std::string generatorGramResultMd(const ProjectRequest &request,
                                  const std::string &mode,
                                  const std::vector<std::string> &productFiles,
                                  const std::string &exportName,
                                  const std::string &analysisName, bool lexMode)
{
    const std::string sourcePath =
        ((request.lines == nullptr) || request.lines->grammarPath.empty())
            ? std::string("（内建 default_grammar.gram）")
            : request.lines->grammarPath;
    std::string text;
    text += "# 生成结果（文法导出）\n\n";
    text += "这一档（`--lang gram`）**不生成解析器 / 词法器代码**：产物目录里\n";
    text += "只有文法原文件与分析报告，用来把「这份文法 + 它的分析结果」一起\n";
    text += "归档 / 提交 / 交接给别的工具。\n\n";
    text += "## 生成信息\n\n";
    text += "| 项目 | 值 |\n| --- | --- |\n";
    text += "| 生成器 | ParseEngine |\n";
    text += "| 源文法 | `" + sourcePath + "` |\n";
    text += "| 导出文件 | `" + exportName + "`（输入文件原样，逐字节） |\n";
    text += "| 分析报告 | `" + analysisName + "`（冲突 + 分析报告全文） |\n";
    text += "| 生成模式 | " + modeLabelText(mode) + " |\n";
    text += "| 目标 | `--lang gram`（只要文法与分析报告，不要代码） |\n";
    if (!lexMode)
    {
        text += "| %tree | " +
                std::string(request.tree ? "启用" : "未启用") + " |\n";
    }
    text += "\n## 产物\n\n";
    for (const std::string &name : productFiles)
    {
        text += "- `" + name + "`\n";
    }
    text += "\n## 怎么用\n\n";
    text += "把这两份文件一起收着就行（`" + exportName + "` 是文法本身，\n";
    text += "`" + analysisName + "` 是它的分析报告）；想要某种语言的解析器时，\n";
    text += "拿这份 `.gram` 换一个 `--lang` 重新生成：\n\n";
    text += "```sh\n";
    text += "ParseEngine " + exportName + " --out-dir out --lang c\n";
    text += "ParseEngine " + exportName + " --out-dir out --lang python\n";
    text += "```\n\n";
    text += "分析报告随生成模式变化：换 `--mode` 再导一次就是另一套分析\n";
    text += "（`" + analysisName + "` 会跟着重写）。\n\n";
    text += "## 说明\n\n";
    if (lexMode)
    {
        text += "- 这一档**不生成词法器**：要词法器请用 `--lang c` / `c++17` /\n";
        text += "  `python` / `csharp` / `rust` / `go` 配 `--mode lex`；\n";
        text += "  词法分析的结果在 `" + analysisName + "` 里。\n";
    }
    else
    {
        text += "- 这一档**不生成解析器 / 词法器**，也没有构建脚本：产物目录里\n";
        text += "  只有文法原文件、分析报告与本文件。\n";
    }
    text += "- 报错定位：分析报告里的行号就是 `.gram` 的行号（这一档不生成代码，"
            "所以\n  没有 `#line` / 行标记那一层）。\n";
    text += "- 边界：不做嵌套 `%union`、不支持 `%destructor`、不做复杂类型跨"
            "语言映射——\n  这些边界是给代码生成档的，文法导出档不涉及。\n";
    return text;
}

/**
 * @brief 生成文法导出（`--lang gram`）产物：文法原文件 + 分析报告 + RESULT.md。
 *
 * 没有解析器 / 词法器 / 构建脚本（这一档连 `%mode lex` 的词法器都不出，
 * 只出分析报告）。`%copy` 的文件照旧拷进产物目录。
 * @param request 组装入参。
 * @param mode 归一化后的生成模式。
 * @param copies %copy 展开后的拷贝项。
 * @param out 输出文件名到文件内容的映射，顺序与产物清单一致。
 * @param error 出错时写入错误说明。
 * @return 成功返回 true。
 */
bool generateGramProject(const ProjectRequest &request, const std::string &mode,
                         const std::vector<CopyEntry> &copies,
                         std::map<std::string, std::string> &out,
                         Error &error)
{
    if (request.grammarText == nullptr)
    {
        return fail(error,
                    "--lang gram 需要文法原文才能导出原文件：调用方要先读一份"
                    "（见 ProjectRequest::grammarText）");
    }
    const std::string exportName = grammarExportName(request);
    const std::string analysisName = grammarAnalysisName(exportName);
    std::vector<std::string> files = {exportName, analysisName, "RESULT.md"};
    for (const CopyEntry &entry : copies)
    {
        files.push_back(entry.name);
    }
    std::map<std::string, std::string> project;
    project.insert_or_assign(exportName, *request.grammarText);
    for (const CopyEntry &entry : copies)
    {
        project.insert_or_assign(entry.name, entry.content);
    }
    project.insert_or_assign(analysisName, generatorGramAnalysisMd(request, mode, exportName,
                                                      isLexMode(mode)));
    project.insert_or_assign("RESULT.md", generatorGramResultMd(
                                 request, mode, files, exportName, analysisName,
                                 isLexMode(mode)));
    for (const std::string &name : files)
    {
        const auto contentIt = project.find(name);
        const std::string *content =
            contentIt != project.end() ? &contentIt->second : nullptr;
        if (content != nullptr)
        {
            out.insert_or_assign(name, *content);
        }
    }
    return true;
}

/**
 * @brief 生成 Python 产物工程（parser.py / main.py）。
 *
 * Python 产物不需要编译，所以没有 Makefile / CMakeLists；产物清单是
 * parser.py / main.py / RESULT.md（+ 可选 mermaid/ 与 %copy 的文件）。
 * @param request 组装入参。
 * @param mode 归一化后的生成模式。
 * @param copies %copy 展开后的拷贝项。
 * @param analysis 分析结果。
 * @param out 输出文件名到文件内容的映射，顺序与产物清单一致。
 * @param error 出错时写入错误说明。
 * @return 成功返回 true。
 */
bool generatePythonProject(const ProjectRequest &request,
                           const std::string &mode,
                           const std::vector<CopyEntry> &copies,
                           const AnalysisResult &analysis,
                           std::map<std::string, std::string> &out,
                           Error &error)
{
    const std::string parserFile = CodeGenPython::parserFileName();
    const std::string mainFile = CodeGenPython::mainFileName();
    std::string parserText;
    if (!CodeGenPython::parserPy(request, mode, analysis, parserText, error))
    {
        return false;
    }
    std::string mainBody;
    if (!buildMainBody(request, mainFile, mainBody, error,
                       /*lineDirectives=*/false))
    {
        return false;
    }
    // Python 没有 #line：%main 正文前贴一行注释，标明它来自 .gram 的哪一行。
    // 正文从 %main 标记行的**下一行**开始，所以指向 +1（与 C 目标的约定一致）。
    if (!mainBody.empty() && (request.lines != nullptr) &&
        (request.lines->mainLines != nullptr) &&
        !request.lines->mainLines->empty() &&
        ((*request.lines->mainLines)[0] != 0u))
    {
        mainBody = CodeGenPython::grammarLineMarker(
                       (*request.lines->mainLines)[0] + 1u) +
                   mainBody;
    }
    std::string mainText = CodeGenPython::mainPy(request, mode, mainBody);
    if (request.hasEpilogue && (request.epilogue != nullptr))
    {
        // %epilogue 在 Python 下同样按缩进规则归一化：正文常常跟着文法缩进写，
        // 直接贴进模块顶层会是 IndentationError。
        const std::string epilogue =
            CodeGenPython::normalizeUserCode(*request.epilogue);
        if (!epilogue.empty())
        {
            if (parserText.back() != '\n')
            {
                parserText += "\n";
            }
            parserText += "\n# ---------- %epilogue（用户正文）----------\n\n";
            // 同 %main：Python 没有 #line，用一行注释标出 %epilogue 在 .gram
            // 里的位置（正文从 %epilogue 的下一行开始，所以指向 +1）
            if ((request.lines != nullptr) &&
                (request.lines->epilogueLine != 0u))
            {
                parserText += CodeGenPython::grammarLineMarker(
                    request.lines->epilogueLine + 1u);
            }
            parserText += epilogue;
        }
    }

    // Mermaid 状态机图示（与目标语言无关）
    const std::map<std::string, std::vector<std::string>> emptyTokenDefs;
    const LexSpec emptyLex;
    std::vector<std::pair<std::string, std::string>> mermaidFiles;
    if (!Mermaid::projectFiles(analysis,
                               (request.lexSpec == nullptr) ? emptyLex
                                                            : *request.lexSpec,
                               (request.tokenDefs == nullptr)
                                   ? emptyTokenDefs
                                   : *request.tokenDefs,
                               request.mermaid, mermaidFiles, error))
    {
        return false;
    }

    std::vector<std::string> files = {parserFile, mainFile};
    for (const auto &entry : mermaidFiles)
    {
        files.push_back(entry.first);
    }
    for (const CopyEntry &entry : copies)
    {
        files.push_back(entry.name);
    }
    files.push_back("RESULT.md");

    std::map<std::string, std::string> project;
    // 「# 语法文件 … 行 N」里的路径占位符落地。这里是注释不是 C 字符串字面量，
    // 所以不做 C 的转义（Windows 路径照样按用户给的形式写出来）。
    const std::string grammarPath =
        ((request.lines == nullptr) || request.lines->grammarPath.empty())
            ? std::string("<default_grammar.gram>")
            : request.lines->grammarPath;
    parserText =
        replaceAll(parserText, CodeGen::grammarFilePlaceholder(), grammarPath);
    mainText =
        replaceAll(mainText, CodeGen::grammarFilePlaceholder(), grammarPath);
    project.insert_or_assign(parserFile, parserText);
    project.insert_or_assign(mainFile, mainText);
    for (const auto &entry : mermaidFiles)
    {
        project.insert_or_assign(entry.first, entry.second);
    }
    for (const CopyEntry &entry : copies)
    {
        project.insert_or_assign(entry.name, entry.content);
    }
    std::vector<std::string> baseFiles = {parserFile, mainFile};
    for (const auto &entry : mermaidFiles)
    {
        baseFiles.push_back(entry.first);
    }
    for (const CopyEntry &entry : copies)
    {
        baseFiles.push_back(entry.name);
    }
    baseFiles.push_back("RESULT.md");
    project.insert_or_assign("RESULT.md",
                generatorResultMd(request, mode, analysis, baseFiles));

    std::map<std::string, std::string> result;
    for (const std::string &name : files)
    {
        const auto textIt = project.find(name);
        const std::string *text =
            textIt != project.end() ? &textIt->second : nullptr;
        if (text != nullptr)
        {
            result.insert_or_assign(name, *text);
        }
    }
    out = std::move(result);
    return true;
}

/**
 * @brief 生成 C++17 产物工程（parser.hpp / parser.cpp / main.cpp）。
 *
 * 与 C 产物完全分开：模板（source/templates/cpp17_*.tmpl）与后端
 * （code_gen_cpp17.cpp）都是独立的一套，%epilogue / %main 的正文原样贴，
 * 不做任何生成期文本改写。
 * @param request 组装入参。
 * @param mode 归一化后的生成模式。
 * @param copies %copy 展开后的拷贝项。
 * @param analysis 分析结果。
 * @param userCodeWithLines 已插 #line 的 %code ... %endcode 正文。
 * @param unionBodyWithLines 已插 #line 的 %union 正文（没有 %union 时为空串）。
 * @param valueType %value_type（空 = 没有）。
 * @param out 输出文件名到文件内容的映射，顺序与产物清单一致。
 * @param error 出错时写入错误说明。
 * @return 成功返回 true。
 */
bool generateCpp17Project(const ProjectRequest &request,
                          const std::string &mode,
                          const std::vector<CopyEntry> &copies,
                          const AnalysisResult &analysis,
                          const std::string &userCodeWithLines,
                          const std::string &unionBodyWithLines,
                          const std::string &valueType,
                          std::map<std::string, std::string> &out,
                          Error &error)
{
    const std::string hppName = CodeGenCpp17::hppName(request);
    const std::string sourceName = CodeGenCpp17::parserSourceName(request);
    const std::string mainName = CodeGenCpp17::mainSourceName(request);
    const std::string userHeaderName = CodeGenCpp17::userHeaderName(request);

    const std::optional<std::string> userHeader =
        CodeGenCpp17::userHeader(request, userCodeWithLines);

    std::string mainBody;
    if (!buildMainBody(request, mainName, mainBody, error))
    {
        return false;
    }

    std::string hpp;
    if (!CodeGenCpp17::parserHpp(request, mode, analysis,
                                 unionBodyWithLines, hpp, error))
    {
        return false;
    }
    std::string parserSource;
    if (!CodeGenCpp17::parserCpp(request, mode, analysis, parserSource, error))
    {
        return false;
    }
    // %epilogue 追加在 parser.cpp 末尾：在命名空间外面，前面补 using
    appendEpilogue(request, sourceName, true, parserSource);
    const std::string mainSource = CodeGenCpp17::mainCpp(request, mode, mainBody);

    const std::vector<CopyEntry> &allCopies = copies;
    std::vector<std::string> copySources = copyNamesWithSuffix(allCopies, ".c");
    for (const std::string &name : copyNamesWithSuffix(allCopies, ".cpp"))
    {
        copySources.push_back(name);
    }
    std::vector<std::string> copyHeaders = copyNamesWithSuffix(allCopies, ".h");
    for (const std::string &name : copyNamesWithSuffix(allCopies, ".hpp"))
    {
        copyHeaders.push_back(name);
    }

    // 产物清单用「不带前缀」的名字：RESULT.md 正文与清单里的文件名统一由
    // renameProductFiles 按 %namespace 前缀改写
    std::vector<std::string> baseFiles = {"parser.hpp", "parser.cpp",
                                          "main.cpp"};
    if (userHeader.has_value())
    {
        baseFiles.push_back("parser_user.h");
    }
    baseFiles.push_back("Makefile");
    baseFiles.push_back("CMakeLists.txt");
    baseFiles.push_back("RESULT.md");
    std::vector<std::string> files = {hppName, sourceName, mainName};
    if (userHeader.has_value())
    {
        files.push_back(userHeaderName);
    }
    files.push_back("Makefile");
    files.push_back("CMakeLists.txt");
    files.push_back("RESULT.md");

    // Mermaid 状态机图示（与目标语言无关）
    const std::map<std::string, std::vector<std::string>> emptyTokenDefs;
    const auto &tokenDefs =
        (request.tokenDefs == nullptr) ? emptyTokenDefs : *request.tokenDefs;
    const LexSpec emptyLex;
    const LexSpec &lexSpec =
        (request.lexSpec == nullptr) ? emptyLex : *request.lexSpec;
    std::vector<std::pair<std::string, std::string>> mermaidFiles;
    if (!Mermaid::projectFiles(analysis, lexSpec, tokenDefs, request.mermaid,
                               mermaidFiles, error))
    {
        return false;
    }
    for (const auto &entry : mermaidFiles)
    {
        files.push_back(entry.first);
        baseFiles.push_back(entry.first);
    }
    for (const CopyEntry &entry : copies)
    {
        files.push_back(entry.name);
        baseFiles.push_back(entry.name);
    }

    std::map<std::string, std::string> project;
    project.insert_or_assign(hppName,
                CodeGen::resolveLineDirectives(hpp,
                                               (request.lines == nullptr)
                                                   ? std::string()
                                                   : request.lines->grammarPath,
                                               hppName));
    project.insert_or_assign(sourceName,
                CodeGen::resolveLineDirectives(
                    parserSource,
                    (request.lines == nullptr) ? std::string()
                                               : request.lines->grammarPath,
                    sourceName));
    project.insert_or_assign(mainName,
                CodeGen::resolveLineDirectives(
                    mainSource,
                    (request.lines == nullptr) ? std::string()
                                               : request.lines->grammarPath,
                    mainName));
    project.insert_or_assign("Makefile",
                generatorMakefileCpp17(userHeader.has_value(), copySources,
                                       copyHeaders, request.cppNamespace,
                                       modeLabelText(mode)));
    project.insert_or_assign("CMakeLists.txt",
                generatorCMakeCpp17(copySources, request.cppNamespace,
                                    modeLabelText(mode)));
    for (const auto &entry : mermaidFiles)
    {
        project.insert_or_assign(entry.first, entry.second);
    }
    project.insert_or_assign("RESULT.md",
                generatorResultMd(request, mode, analysis, baseFiles));
    if (userHeader.has_value())
    {
        // parser_user.h 原样保留，不做空行压缩 / 占位符替换
        project.insert_or_assign(userHeaderName,
                    CodeGen::resolveLineDirectives(
                        *userHeader,
                        (request.lines == nullptr)
                            ? std::string()
                            : request.lines->grammarPath,
                        userHeaderName));
    }

    std::map<std::string, std::string> result;
    for (const std::string &name : files)
    {
        const auto textIt = project.find(name);
        const std::string *text =
            textIt != project.end() ? &textIt->second : nullptr;
        if (text == nullptr)
        {
            continue;
        }
        result.insert_or_assign(name, *text);
    }
    // %copy 的内容原样写入，不做空行压缩
    for (const CopyEntry &entry : copies)
    {
        if (result.find(entry.name) == result.end())
        {
            result.insert_or_assign(entry.name, entry.content);
        }
    }
    (void)valueType;
    out = std::move(result);
    return true;
}

/**
 * @brief 生成分文件工程（语义值类型由 %code ... %endcode / %union 提供）。
 * @param request 组装入参。
 * @param mode 归一化后的生成模式。
 * @param copies %copy 展开后的拷贝项。
 * @param out 输出文件名到文件内容的映射，顺序与产物清单一致。
 * @param error 出错时写入错误说明。
 * @return 成功返回 true。
 */
/* ---------------- 词法模式（%mode lex） ---------------- */

/**
 * @brief 词法模式产物的 Makefile。
 * @param userHeader 是否生成了 parser_user.h。
 * @param copySources %copy 拷进来的 .c。
 * @param copyHeaders %copy 拷进来的 .h。
 * @return Makefile 文本。
 */
std::string generatorMakefileLex(bool userHeader,
                                 const std::vector<std::string> &copySources,
                                 const std::vector<std::string> &copyHeaders)
{
    // 没生成 parser_user.h 时模板会把那一行整条去掉（{% if user_header %}）
    return tmpl::renderValues(
        Utils::loadTemplate("Makefile_lex"),
        {{"user_header", userHeader},
         {"lib_srcs", tmpl::encode(copySources)},
         {"hdr_srcs", tmpl::encode(copyHeaders)}});
}

/**
 * @brief 词法模式产物的 CMakeLists.txt。
 * @param copySources %copy 拷进来的 .c。
 * @return CMakeLists.txt 文本。
 */
std::string generatorCMakeLex(const std::vector<std::string> &copySources)
{
    return tmpl::renderValues(Utils::loadTemplate("CMakeLists_lex.txt"),
                              {{"copy_sources", tmpl::encode(copySources)}});
}

/**
 * @brief 词法模式产物的 C++17 Makefile（产物名带 %namespace 前缀）。
 * @param userHeader 是否生成了 parser_user.h。
 * @param copySources %copy 拷进来的源文件。
 * @param copyHeaders %copy 拷进来的头文件。
 * @param prefix 文件名前缀（空 = 不加）。
 * @return Makefile 文本。
 */
std::string generatorMakefileLexCpp17(
    bool userHeader, const std::vector<std::string> &copySources,
    const std::vector<std::string> &copyHeaders, const std::string &prefix)
{
    const std::string text = tmpl::renderValues(
        Utils::loadTemplate("Makefile_lex_cpp17"),
        {{"user_header", userHeader},
         {"lib_srcs", tmpl::encode(copySources)},
         {"hdr_srcs", tmpl::encode(copyHeaders)}});
    return renameProductFiles(text, prefix, true);
}

/**
 * @brief 词法模式产物的 C++17 CMakeLists.txt（产物名带 %namespace 前缀）。
 * @param copySources %copy 拷进来的源文件。
 * @param prefix 文件名前缀（空 = 不加）。
 * @return CMakeLists.txt 文本。
 */
std::string generatorCMakeLexCpp17(
    const std::vector<std::string> &copySources, const std::string &prefix)
{
    const std::string text = tmpl::renderValues(
        Utils::loadTemplate("CMakeLists_lex_cpp17.txt"),
        {{"copy_sources", tmpl::encode(copySources)}});
    return renameProductFiles(text, prefix, true);
}

/**
 * @brief 词法模式的 Token 表：内置 Token + 文法声明的 + END（按首次出现去重）。
 *
 * 顺序与分析层的 `analysis.terminals` 一致（Token 号 = 下标），所以词法产物的
 * Token 编号与解析产物对得上。
 * @param request 组装入参。
 * @return Token 列表。
 */
std::vector<std::string> lexTerminals(const ProjectRequest &request)
{
    std::vector<std::string> terminals;
    const auto append = [&terminals](const std::string &name)
    {
        if (std::find(terminals.begin(), terminals.end(), name) ==
            terminals.end())
        {
            terminals.push_back(name);
        }
    };
    for (const std::string &name : k_defaultTokenNames)
    {
        append(name);
    }
    if (request.tokens != nullptr)
    {
        for (const std::string &name : *request.tokens)
        {
            append(name);
        }
    }
    append("END");
    return terminals;
}

/**
 * @brief 词法模式产物的 RESULT.md：产物清单 + Token 表 + 编译运行说明。
 * @param request 组装入参。
 * @param productFiles 产物清单（按写出顺序）。
 * @param lang 目标语言：``c`` / ``cpp17`` / ``python``。
 * @return RESULT.md 文本。
 */
std::string generatorLexResultMd(const ProjectRequest &request,
                                 const std::vector<std::string> &productFiles,
                                 const std::string &lang)
{
    const std::vector<std::string> terminals = lexTerminals(request);
    const std::string sample = exampleInput(request);
    const std::string sampleShell = shellQuote(sample);
    const std::string sampleCode = codeStringLiteral(sample);
    const std::map<std::string, std::vector<std::string>> emptyTokenDefs;
    const std::map<std::string, std::vector<std::string>> &tokenDefs =
        (request.tokenDefs == nullptr) ? emptyTokenDefs : *request.tokenDefs;
    const LexSpec emptyLex;
    const LexSpec &lexSpec =
        (request.lexSpec == nullptr) ? emptyLex : *request.lexSpec;
    std::string text;
    text += "# 词法模式（%mode lex）生成结果\n\n";
    text += "这份产物**只有词法器**：没有分析栈、分析表，也没有解析入口；\n";
    text += "`lexer_demo` 把标准输入读成 Token 流，逐个打印 Token 的类型与词素。\n\n";
    text += "## 产物\n\n";
    for (const std::string &name : productFiles)
    {
        text += "- `" + name + "`\n";
    }
    text += "\n## Token\n\n";
    text += "| Token | 写法 |\n| --- | --- |\n";
    for (const std::string &name : terminals)
    {
        std::string how;
        const auto literalsIt = tokenDefs.find(name);
        const std::vector<std::string> *literals =
            literalsIt != tokenDefs.end() ? &literalsIt->second : nullptr;
        if (literals != nullptr)
        {
            how = "固定字面量";
            for (std::size_t index = 0u; index < literals->size(); ++index)
            {
                how += (index == 0u) ? " " : " / ";
                how += "`" + (*literals)[index] + "`";
            }
        }
        const auto patternIt = lexSpec.regex.find(name);
        const std::string *pattern =
            patternIt != lexSpec.regex.end() ? &patternIt->second : nullptr;
        if (pattern != nullptr)
        {
            how = "正则 `" + *pattern + "`";
        }
        if (how.empty())
        {
            how = "由词法层外部产生（没有字面量 / 正则）";
        }
        text += "| `" + name + "` | " + how + " |\n";
    }
    if (lexSpec.identifier.has_value())
    {
        text += "| `VARIABLE` | `%identifier` `" + *lexSpec.identifier + "` |\n";
    }
    text += "\n## 编译与运行\n\n";
    if (lang == "python")
    {
        text += "```sh\nprintf '%s\\n' " + sampleShell +
                " | python3 main.py\n```\n\n";
    }
    else if (lang == "cpp17")
    {
        text += "```sh\nmake\nprintf '%s\\n' " + sampleShell +
                " | ./lexer_demo\n```\n\n";
    }
    else
    {
        text += "```sh\nmake\nprintf '%s\\n' " + sampleShell +
                " | ./lexer_demo\n```\n\n";
    }
    text += "输出一行一个 Token：`<类型>  \"<词素>\"  <行>:<列>`，最后一行是总数；\n";
    text += "认不出来的字符 / 字符串没有闭合会打印一行错误说明并以退出码 1 结束。\n\n";
    text += "## 当库用\n\n";
    if (lang == "python")
    {
        text += "```python\nimport parser\n\n"
                "lexer = parser." +
                CodeGenPython::className(request) +
                "()\n"
                "status, tokens = lexer.tokenize(" + sampleCode + ")\n"
                "for token in tokens:\n"
                "    print(parser.token_name(token.type), token.text)\n"
                "```\n";
    }
    else if (lang == "cpp17")
    {
        text += "```cpp\n#include \"lexer.hpp\"\n\n"
                "std::vector<Token> tokens;\n"
                "Engine engine;\n"
                "if (engine.tokenize(" + sampleCode + ", &tokens) == "
                "Status::Ok) {\n"
                "    for (const Token &token : tokens) {\n"
                "        std::cout << tokenName(token.type) << ' '\n"
                "                  << tokenText(token) << '\\n';\n"
                "    }\n"
                "}\n```\n";
    }
    else
    {
        text += "```c\n#include \"lexer.h\"\n\n"
                "ParserLexer_T lexer;\n"
                "ParserToken_T token;\n"
                "Parser_LexInit(&lexer, " + sampleCode + ");\n"
                "while (Parser_NextToken(&lexer, &token) == PARSER_OK) {\n"
                "    if (token.type == TOKEN_END) { break; }\n"
                "    printf(\"%s\\n\", Parser_TokenName(token.type));\n"
                "}\n```\n";
    }
    return text;
}

/**
 * @brief 生成词法模式（`%mode lex`）的 C 产物。
 *
 * 产物清单：lexer.h / lexer.c / parser_token.h / parser_token.c / main.c /
 * Makefile / CMakeLists.txt / RESULT.md（+ `%code ... %endcode` 时的 parser_user.h），
 * **没有** parser.c / parser.h / parser_table.c —— 这一模式不生成解析器。
 * @param request 组装入参。
 * @param copies %copy 展开后的拷贝项。
 * @param out 输出文件名到文件内容的映射。
 * @param error 出错时写入错误说明。
 * @return 成功返回 true。
 */
bool generateLexProject(const ProjectRequest &request,
                        const std::vector<CopyEntry> &copies,
                        std::map<std::string, std::string> &out, Error &error)
{
    const LexSpec emptyLex;
    const LexSpec &lexSpec =
        (request.lexSpec == nullptr) ? emptyLex : *request.lexSpec;
    const std::string unionBody =
        (request.unionBody == nullptr) ? std::string() : *request.unionBody;
    const std::string valueType =
        (request.valueType == nullptr) ? std::string() : *request.valueType;
    // %code ... %endcode / %union 的 #line：与解析产物同一套，报错能指回 .gram
    std::string userCodeWithLines = request.userCode;
    if ((request.lines != nullptr) &&
        (request.lines->userBlocks != nullptr) &&
        (request.lines->userBlockLines != nullptr) &&
        (request.lines->userBlocks->size() ==
         request.lines->userBlockLines->size()))
    {
        userCodeWithLines.clear();
        for (std::size_t index = 0u;
             index < request.lines->userBlocks->size(); ++index)
        {
            const std::string &block = (*request.lines->userBlocks)[index];
            const std::size_t line = (*request.lines->userBlockLines)[index];
            if (line == 0u)
            {
                userCodeWithLines += block;
                continue;
            }
            userCodeWithLines +=
                "#line " + std::to_string(line) + " \"" +
                CodeGen::grammarFilePlaceholder() + "\"\n" + block;
            if (userCodeWithLines.empty() || (userCodeWithLines.back() != '\n'))
            {
                userCodeWithLines += "\n";
            }
            userCodeWithLines +=
                "#line " + std::string(CodeGen::lineBackPlaceholder()) +
                " \"parser_user.h\"\n";
        }
    }
    const std::string unionBodyWithLines =
        (unionBody.empty() || (request.lines == nullptr) ||
         (request.lines->unionLine == 0u))
            ? unionBody
            : ("\n#line " + std::to_string(request.lines->unionLine) + " \"" +
               CodeGen::grammarFilePlaceholder() + "\"\n" + unionBody +
               ((unionBody.back() == '\n') ? "" : "\n") + "#line " +
               CodeGen::lineBackPlaceholder() + " \"parser_user.h\"\n");
    // 词法模式的 Token 表与分析层一致：三个内置 Token + 文法声明的 + END
    // （没有产生式，所以不会出现 error 那种由右部带进来的伪终结符），
    // 顺序按首次出现去重——Token 号就是数组下标，改动它会动到全部产物
    const std::vector<std::string> terminals = lexTerminals(request);
    // prologue 是「分段容器」：渲染完整份之后按 `__SEC_*` 标记把 DEFINES /
    // ERROR_TYPE 两段取出来（标记不是模板语法，渲染时会原样留下）。
    // 词法产物用不到分析栈 / 语义槽：给个合法的小值，宏仍然可以被 -D 覆盖。
    const std::string prologue = tmpl::renderValues(
        Utils::loadTemplate("c99_parser_prologue.c"),
        {{"max_slots", "4"},
         {"max_stack_depth",
          std::to_string(
              request.maxStackDepth.value_or(k_maxStackDepthDefault))},
         {"max_errors",
          std::to_string(request.maxErrors.value_or(k_maxErrorsDefault))},
         {"error_recovery_tokens",
          std::to_string(
              request.errorRecoveryTokens.value_or(k_errorRecoveryTokensDefault))},
         {"lookahead_k", "1"},
         {"lookahead_column_count", "1"}});
    std::string defines = CodeGen::extractSection(prologue, "DEFINES");
    if (request.errorContext)
    {
        defines = "#define PARSER_ERROR_CONTEXT 1\n" + defines;
    }
    const std::string errorType =
        CodeGen::extractSection(prologue, "ERROR_TYPE");
    std::string tokenEnum;
    for (std::size_t index = 0u; index < terminals.size(); ++index)
    {
        if (index != 0u)
        {
            tokenEnum += ", ";
        }
        tokenEnum += "TOKEN_" + terminals[index];
    }
    tokenEnum += ", TOKEN_INVALID";
    const bool unionValue = (request.symbolValueMember != nullptr);
    std::string tokenTypes;
    if (unionValue)
    {
        tokenTypes += "#ifndef PARSER_USER_HEADER_INCLUDED\n"
                      "#define PARSER_USER_HEADER_INCLUDED 1\n"
                      "#include \"parser_user.h\"\n"
                      "#endif\n\n";
    }
    tokenTypes += "typedef enum __ParserTokenType_T { " + tokenEnum +
                  " } ParserTokenType_T;\n\n"
                  "typedef struct __ParserToken_T\n{\n"
                  "    ParserTokenType_T type;\n";
    tokenTypes += unionValue ? "    ParserValue_T value;\n"
                             : "    int32_t value;\n";
    tokenTypes += "    const char *name;\n"
                  "    const char *pName;\n"
                  "    uint32_t line;\n"
                  "    uint32_t column;\n"
                  "    uint32_t offset;\n"
                  "} ParserToken_T;\n";
    std::string lexDecls;
    std::string lexTables;
    const std::map<std::string, std::vector<std::string>> emptyTokenDefs;
    const std::map<std::string, std::vector<std::string>> &tokenDefs =
        (request.tokenDefs == nullptr) ? emptyTokenDefs : *request.tokenDefs;
    if (!CodeGenC99::lexDfa(tokenDefs, lexSpec.regex, lexSpec.identifier,
                         lexSpec.commits, lexSpec.tokenLexOrder, lexDecls,
                         lexTables, error))
    {
        return false;
    }
    const std::optional<std::string> userHeader =
        CodeGenC99::customUserH(userCodeWithLines, unionBodyWithLines, valueType);
    std::vector<std::string> files = {"lexer.h", "lexer.c", "parser_token.h",
                                      "parser_token.c"};
    if (userHeader.has_value())
    {
        files.push_back("parser_user.h");
    }
    files.push_back("main.c");
    files.push_back("Makefile");
    files.push_back("CMakeLists.txt");
    files.push_back("RESULT.md");
    const std::vector<std::string> copySources =
        copyNamesWithSuffix(copies, ".c");
    const std::vector<std::string> copyHeaders =
        copyNamesWithSuffix(copies, ".h");
    for (const CopyEntry &entry : copies)
    {
        files.push_back(entry.name);
    }
    std::map<std::string, std::string> project;
    project.insert_or_assign("lexer.h",
                CodeGenC99::lexerOnlyH(defines, errorType, request.userCode,
                                    unionBody, valueType));
    project.insert_or_assign("lexer.c",
                CodeGenC99::lexerOnlyC(tokenDefs, lexSpec, request.spaceChars,
                                    userCodeWithLines, unionBodyWithLines,
                                    valueType, request.symbolValueMember));
    project.insert_or_assign("parser_token.h",
                CodeGenC99::customParserTokenH(tokenTypes, lexDecls,
                                            k_legacyModeMarker));
    project.insert_or_assign("parser_token.c",
                CodeGenC99::customParserTokenC(CodeGenC99::tokenNames(terminals),
                                            lexDecls, lexTables,
                                            k_legacyModeMarker));
    // %main：0 段用内建词法演示 CLI；1 段用用户内容替换正文；多段报错
    std::string mainBody;
    if (!buildMainBody(request, "main.c", mainBody, error))
    {
        return false;
    }
    project.insert_or_assign(
        "main.c",
        tmpl::renderValues(Utils::loadTemplate("c99_lexer_main_custom.c"),
                           {{"main_from_grammar", !mainBody.empty()},
                            {"lex_demo_main",
                             mainBody.empty()
                                 ? std::string(Utils::loadTemplate(
                                       "c99_lexer_demo_main_custom.c"))
                                 : ("\n\n" + mainBody + "\n")}}));
    project.insert_or_assign("Makefile", generatorMakefileLex(userHeader.has_value(),
                                                 copySources, copyHeaders));
    project.insert_or_assign("CMakeLists.txt", generatorCMakeLex(copySources));
    project.insert_or_assign("RESULT.md", generatorLexResultMd(request, files, "c"));
    if (userHeader.has_value())
    {
        project.insert_or_assign("parser_user.h", *userHeader);
    }
    // 与解析产物同一套收尾：压空行、%epilogue 贴到 lexer.c 末尾、#line
    // 占位符落地（.gram 路径换真实路径、恢复行换最终行号与产物名）
    for (const std::string &name : files)
    {
        const auto contentIt = project.find(name);
        const std::string *content =
            contentIt != project.end() ? &contentIt->second : nullptr;
        if (content != nullptr)
        {
            std::string text = Utils::collapseBlankLines(*content);
            if (name == "lexer.c")
            {
                appendEpilogue(request, "lexer.c", false, text);
            }
            out.insert_or_assign(name, CodeGen::resolveLineDirectives(
                              text,
                              (request.lines == nullptr)
                                  ? std::string()
                                  : request.lines->grammarPath,
                              name));
        }
    }
    // %copy 的内容原样写入，不做空行压缩
    for (const CopyEntry &entry : copies)
    {
        out.insert_or_assign(entry.name, entry.content);
    }
    return true;
}

/**
 * @brief 生成词法模式（`%mode lex`）的 Python 产物。
 *
 * 产物清单：parser.py（词法器 + `tokenize`）/ main.py（SQL：读标准输入到 EOF、
 * 逐个打印 Token 类型）/ RESULT.md，**没有**分析表与解析入口。
 * @param request 组装入参。
 * @param copies %copy 展开后的拷贝项。
 * @param out 输出文件名到文件内容的映射。
 * @param error 出错时写入错误说明。
 * @return 成功返回 true。
 */
bool generateLexPythonProject(const ProjectRequest &request,
                              const std::vector<CopyEntry> &copies,
                              std::map<std::string, std::string> &out,
                              Error &error)
{
    const std::vector<std::string> terminals = lexTerminals(request);
    std::string parserText;
    if (!CodeGenPython::lexerPy(request, terminals, parserText, error))
    {
        return false;
    }
    std::string mainBody;
    if (!buildMainBody(request, "main.py", mainBody, error,
                       /*lineDirectives=*/false))
    {
        return false;
    }
    // Python 没有 #line：%main 正文前贴一行注释，标出它来自 .gram 的哪一行
    if (!mainBody.empty() && (request.lines != nullptr) &&
        (request.lines->mainLines != nullptr) &&
        !request.lines->mainLines->empty() &&
        ((*request.lines->mainLines)[0] != 0u))
    {
        mainBody = CodeGenPython::grammarLineMarker(
                       (*request.lines->mainLines)[0] + 1u) +
                   mainBody;
    }
    std::string mainText = CodeGenPython::lexerMainPy(request, mainBody);
    if (request.hasEpilogue && (request.epilogue != nullptr))
    {
        // %epilogue 在 Python 下按缩进规则归一化后再贴到模块末尾
        const std::string epilogue =
            CodeGenPython::normalizeUserCode(*request.epilogue);
        if (!epilogue.empty())
        {
            if (parserText.back() != '\n')
            {
                parserText += "\n";
            }
            parserText += "\n# ---------- %epilogue（用户正文）----------\n\n";
            // 同上：正文从 %epilogue 的下一行开始
            if ((request.lines != nullptr) &&
                (request.lines->epilogueLine != 0u))
            {
                parserText += CodeGenPython::grammarLineMarker(
                    request.lines->epilogueLine + 1u);
            }
            parserText += epilogue;
        }
    }
    std::vector<std::string> files = {"parser.py", "main.py"};
    for (const CopyEntry &entry : copies)
    {
        files.push_back(entry.name);
    }
    files.push_back("RESULT.md");
    std::map<std::string, std::string> project;
    // 「# 语法文件 … 行 N」的路径占位符落地（注释，不做 C 转义）
    const std::string grammarPath =
        ((request.lines == nullptr) || request.lines->grammarPath.empty())
            ? std::string("<default_grammar.gram>")
            : request.lines->grammarPath;
    parserText =
        replaceAll(parserText, CodeGen::grammarFilePlaceholder(), grammarPath);
    mainText =
        replaceAll(mainText, CodeGen::grammarFilePlaceholder(), grammarPath);
    project.insert_or_assign("parser.py", parserText);
    project.insert_or_assign("main.py", mainText);
    for (const CopyEntry &entry : copies)
    {
        project.insert_or_assign(entry.name, entry.content);
    }
    project.insert_or_assign("RESULT.md", generatorLexResultMd(request, files, "python"));
    for (const std::string &name : files)
    {
        const auto contentIt = project.find(name);
        const std::string *content =
            contentIt != project.end() ? &contentIt->second : nullptr;
        if (content != nullptr)
        {
            out.insert_or_assign(name, *content);
        }
    }
    return true;
}

/**
 * @brief 生成词法模式（`%mode lex`）的 C++17 产物。
 * @param request 组装入参。
 * @param copies %copy 展开后的拷贝项。
 * @param out 输出文件名到文件内容的映射。
 * @param error 出错时写入错误说明。
 * @return 成功返回 true。
 */
bool generateLexCpp17Project(const ProjectRequest &request,
                             const std::vector<CopyEntry> &copies,
                             std::map<std::string, std::string> &out,
                             Error &error)
{
    // %code ... %endcode / %union 的 #line：与解析产物同一套
    std::string userCodeWithLines = request.userCode;
    if ((request.lines != nullptr) &&
        (request.lines->userBlocks != nullptr) &&
        (request.lines->userBlockLines != nullptr) &&
        (request.lines->userBlocks->size() ==
         request.lines->userBlockLines->size()))
    {
        userCodeWithLines.clear();
        for (std::size_t index = 0u;
             index < request.lines->userBlocks->size(); ++index)
        {
            const std::string &block = (*request.lines->userBlocks)[index];
            const std::size_t line = (*request.lines->userBlockLines)[index];
            if (line == 0u)
            {
                userCodeWithLines += block;
                continue;
            }
            userCodeWithLines +=
                "#line " + std::to_string(line) + " \"" +
                CodeGen::grammarFilePlaceholder() + "\"\n" + block;
            if (userCodeWithLines.empty() || (userCodeWithLines.back() != '\n'))
            {
                userCodeWithLines += "\n";
            }
            userCodeWithLines +=
                "#line " + std::string(CodeGen::lineBackPlaceholder()) +
                " \"" + CodeGenCpp17::userHeaderName(request) + "\"\n";
        }
    }
    const std::string unionBody =
        (request.unionBody == nullptr) ? std::string() : *request.unionBody;
    const std::string unionBodyWithLines =
        (unionBody.empty() || (request.lines == nullptr) ||
         (request.lines->unionLine == 0u))
            ? unionBody
            : ("\n#line " + std::to_string(request.lines->unionLine) + " \"" +
               CodeGen::grammarFilePlaceholder() + "\"\n" + unionBody +
               ((unionBody.back() == '\n') ? "" : "\n") + "#line " +
               CodeGen::lineBackPlaceholder() + " \"" +
               CodeGenCpp17::userHeaderName(request) + "\"\n");
    const std::vector<std::string> terminals = lexTerminals(request);
    const std::string hppName = CodeGenCpp17::lexerHppName(request);
    const std::string sourceName = CodeGenCpp17::lexerSourceName(request);
    const std::string mainName = CodeGenCpp17::mainSourceName(request);
    std::string hppText;
    std::string cppText;
    if (!CodeGenCpp17::lexerHpp(request, terminals, unionBodyWithLines, hppText,
                                error))
    {
        return false;
    }
    if (!CodeGenCpp17::lexerCpp(request, terminals, unionBodyWithLines, cppText,
                                error))
    {
        return false;
    }
    std::string mainBody;
    if (!buildMainBody(request, mainName, mainBody, error))
    {
        return false;
    }
    std::string mainText = CodeGenCpp17::lexerMainCpp(request, mainBody);
    if (request.hasEpilogue && (request.epilogue != nullptr))
    {
        appendEpilogue(request, sourceName, true, cppText);
    }
    const std::optional<std::string> userHeader =
        CodeGenCpp17::userHeader(request, userCodeWithLines);
    const std::vector<std::string> copySources =
        copyNamesWithSuffix(copies, ".c");
    const std::vector<std::string> copyHeaders =
        copyNamesWithSuffix(copies, ".h");
    std::vector<std::string> files = {hppName, sourceName, mainName};
    if (userHeader.has_value())
    {
        files.push_back(CodeGenCpp17::userHeaderName(request));
    }
    files.push_back("Makefile");
    files.push_back("CMakeLists.txt");
    files.push_back("RESULT.md");
    for (const CopyEntry &entry : copies)
    {
        files.push_back(entry.name);
    }
    const std::string grammarPath =
        (request.lines == nullptr) ? std::string()
                                   : request.lines->grammarPath;
    std::map<std::string, std::string> project;
    project.insert_or_assign(hppName,
                CodeGen::resolveLineDirectives(hppText, grammarPath, hppName));
    project.insert_or_assign(
        sourceName,
        CodeGen::resolveLineDirectives(cppText, grammarPath, sourceName));
    project.insert_or_assign(
        mainName,
        CodeGen::resolveLineDirectives(mainText, grammarPath, mainName));
    if (userHeader.has_value())
    {
        const std::string userHeaderName = CodeGenCpp17::userHeaderName(request);
        project.insert_or_assign(userHeaderName, CodeGen::resolveLineDirectives(
                                        *userHeader, grammarPath,
                                        userHeaderName));
    }
    project.insert_or_assign(
        "Makefile",
        generatorMakefileLexCpp17(userHeader.has_value(), copySources,
                                  copyHeaders, request.cppNamespace));
    project.insert_or_assign(
        "CMakeLists.txt",
        generatorCMakeLexCpp17(copySources, request.cppNamespace));
    project.insert_or_assign("RESULT.md", generatorLexResultMd(request, files, "cpp17"));
    for (const std::string &name : files)
    {
        const auto contentIt = project.find(name);
        const std::string *content =
            contentIt != project.end() ? &contentIt->second : nullptr;
        if (content != nullptr)
        {
            out.insert_or_assign(name, *content);
        }
    }
    return true;
}

bool generateCustomProject(const ProjectRequest &request,
                           const std::string &mode,
                           const std::vector<CopyEntry> &copies,
                           std::map<std::string, std::string> &out,
                           Error &error)
{
    const LexSpec emptyLex;
    const LexSpec &lexSpec =
        (request.lexSpec == nullptr) ? emptyLex : *request.lexSpec;
    const std::string unionBody =
        (request.unionBody == nullptr) ? std::string() : *request.unionBody;
    // %code ... %endcode 各块前后插 #line：段里的类型/语法错误能指回 .gram 的行。
    // 用户代码在数据模型里是「多块拼接」，这里按块重新拼一遍（块内容逐字节
    // 不变，只是中间多了 #line 指令）。
    std::string userCodeWithLines = request.userCode;
    if ((request.lines != nullptr) &&
        (request.lines->userBlocks != nullptr) &&
        (request.lines->userBlockLines != nullptr) &&
        (request.lines->userBlocks->size() ==
         request.lines->userBlockLines->size()))
    {
        userCodeWithLines.clear();
        for (std::size_t index = 0u;
             index < request.lines->userBlocks->size(); ++index)
        {
            const std::string &block = (*request.lines->userBlocks)[index];
            const std::size_t line = (*request.lines->userBlockLines)[index];
            if (line == 0u)
            {
                userCodeWithLines += block;
                continue;
            }
            userCodeWithLines +=
                "#line " + std::to_string(line) + " \"" +
                CodeGen::grammarFilePlaceholder() + "\"\n" + block;
            if (userCodeWithLines.empty() || (userCodeWithLines.back() != '\n'))
            {
                userCodeWithLines += "\n";
            }
            userCodeWithLines += "#line " + std::string(CodeGen::lineBackPlaceholder()) +
                                 " \"parser_user.h\"\n";
        }
    }
    // %union 正文前后插 #line：成员类型写错时，编译器报错指回 .gram 里的
    // %union 那一行（路径先用占位符，收尾时统一落地）
    const std::string unionBodyWithLines =
        (unionBody.empty() || (request.lines == nullptr) ||
         (request.lines->unionLine == 0u))
            ? unionBody
            : ("\n#line " + std::to_string(request.lines->unionLine) + " \"" +
               CodeGen::grammarFilePlaceholder() + "\"\n" + unionBody +
               ((unionBody.back() == '\n') ? "" : "\n") + "#line " +
               CodeGen::lineBackPlaceholder() + " \"parser_user.h\"\n");
    const std::string valueType =
        (request.valueType == nullptr) ? std::string() : *request.valueType;
    // 调用方（命令行入口）通常已经算过分析表，直接复用
    AnalysisResult owned;
    if (request.analysis == nullptr)
    {
        if (!Analysis::analyze(*request.grammar, *request.tokens, mode,
                               request.priority, request.teach, owned, error,
                               request.lookaheadK))
        {
            return false;
        }
    }
    const AnalysisResult &analysis =
        (request.analysis == nullptr) ? owned : *request.analysis;
    // Java 与 JavaScript/TypeScript 是窄后端，只接 DD / Earley / CYK。要在 C99
    // prologue、DFA 与表生成之前分流，避免不相关的 C 产物逻辑介入。
    if (request.lang == "java")
    {
        return CodeGenJava::generateProject(request, analysis, out, error);
    }
    if (request.lang == "js")
    {
        return CodeGenJs::generateProject(request, analysis, out, error);
    }
    // DD（递归下降）也是「运行时特性」：产物里要有「每个非终结符一个解析函数
    // + 保存 / 恢复词法位置的原语」这一整套回溯运行时。逐档接入时把下面的
    // 白名单放开——没接上的档先挡住，免得静默生成一个没有回溯运行时的假产物。
    // 六档都接上了。
    if (isDdMode(mode) && (request.lang != "c") &&
        (request.lang != "csharp") && (request.lang != "cpp17") &&
        (request.lang != "python") && (request.lang != "rust") &&
        (request.lang != "go") && (request.lang != "java") &&
        (request.lang != "js"))
    {
        return fail(error, "DD（%mode dd / --mode dd）的递归下降运行时还没接上 "
                           "--lang " + request.lang +
                           "（分析层已经能用：--lang gram 或 --report 能看到"
                           "报告里的产生式清单与左递归检查结果）");
    }
    const std::vector<std::string> &terminals = analysis.terminals;
    const std::vector<std::string> &nonterms = analysis.nonterms;
    int maxSlots = 4;
    for (const Production &production : analysis.productions)
    {
        maxSlots = std::max(
            maxSlots, static_cast<int>(production.rhs.size()) + 3);
    }
    // prologue 是「分段容器」：整份渲染之后再按 `__SEC_*` 标记抽 DEFINES /
    // ERROR_TYPE 两段（标记不是模板语法，渲染时原样留下）。
    // 容量宏：写了 %max_stack_depth / %max_errors / %error_recovery_tokens 就用
    // 文法里的值，没写用缺省值；产物里仍是 #ifndef 兜底，编译期 -D 仍然优先。
    // LR(k) / LALR(k)：展望符长度与归约列个数（k = 1 时列数取 1 让数组合法，
    // 第二层表根本不会生成）
    const std::string prologue = tmpl::renderValues(
        Utils::loadTemplate("c99_parser_prologue.c"),
        {{"max_slots", std::to_string(maxSlots)},
         {"max_stack_depth",
          std::to_string(
              request.maxStackDepth.value_or(k_maxStackDepthDefault))},
         {"max_errors",
          std::to_string(request.maxErrors.value_or(k_maxErrorsDefault))},
         {"error_recovery_tokens",
          std::to_string(
              request.errorRecoveryTokens.value_or(k_errorRecoveryTokensDefault))},
         {"lookahead_k", std::to_string(std::max(0, request.lookaheadK))},
         {"lookahead_column_count",
          std::to_string(std::max<std::size_t>(
              1u, analysis.lookaheadColumns.size()))}});
    std::string defines = CodeGen::extractSection(prologue, "DEFINES");
    if (request.errorContext)
    {
        // %error_context：让 ParserError_T 多一个 context 字段（默认关，省内存）
        defines = "#define PARSER_ERROR_CONTEXT 1\n" + defines;
    }
    // 写了 error 产生式时，把 error 在终结符表里的下标告诉运行时：恢复循环
    // 要用它去查 ACTION 表的 error 列（error 不是真实 Token，词法器不会产生）
    if (request.usesError)
    {
        const auto found =
            std::find(terminals.begin(), terminals.end(), k_errorTokenName);
        if (found != terminals.end())
        {
            defines += "#define PARSER_TOKEN_ERROR " +
                       std::to_string(found - terminals.begin()) + "u\n";
        }
    }
    const std::string errorType =
        CodeGen::extractSection(prologue, "ERROR_TYPE");
    std::string tokenEnum;
    for (std::size_t index = 0; index < terminals.size(); ++index)
    {
        if (index != 0)
        {
            tokenEnum += ", ";
        }
        tokenEnum += "TOKEN_" + terminals[index];
    }
    tokenEnum += ", TOKEN_INVALID";
    // 写了 %union 时语义值就是 ParserValue_T：ParserToken_T.value 直接用它，
    // 词法器才能把词素按 %token <成员> 填进对应成员。这里要先把 ParserValue_T
    // 引进来，并且顺手把守卫宏定义掉——parser.h 也会包含 parser_user.h，
    // 两边都定义宏才能保证 parser_user.h 在一个编译单元里只真正展开一次。
    const bool unionValue = (request.symbolValueMember != nullptr);
    std::string tokenTypes;
    if (unionValue)
    {
        tokenTypes += "#ifndef PARSER_USER_HEADER_INCLUDED\n"
                      "#define PARSER_USER_HEADER_INCLUDED 1\n"
                      "#include \"parser_user.h\"\n"
                      "#endif\n\n";
    }
    tokenTypes += "typedef enum __ParserTokenType_T { " + tokenEnum +
                  " } ParserTokenType_T;\n\n"
                  "typedef struct __ParserToken_T\n{\n"
                  "    ParserTokenType_T type;\n";
    tokenTypes += unionValue ? "    ParserValue_T value;\n"
                             : "    int32_t value;\n";
    tokenTypes += "    const char *name;\n"
                  "    const char *pName;\n"
                  "    uint32_t line;\n"
                  "    uint32_t column;\n"
                  "    uint32_t offset;\n"
                  "} ParserToken_T;\n";

    std::string lexDecls;
    std::string lexTables;
    const std::map<std::string, std::vector<std::string>> emptyTokenDefs;
    const auto &tokenDefs =
        (request.tokenDefs == nullptr) ? emptyTokenDefs : *request.tokenDefs;
    if (!CodeGenC99::lexDfa(tokenDefs, lexSpec.regex, lexSpec.identifier,
                         lexSpec.commits, lexSpec.tokenLexOrder, lexDecls,
                         lexTables, error))
    {
        return false;
    }
    const std::string tablesText =
        isLrMode(mode)
            ? CodeGenC99::lrTables(analysis)
            : CodeGenC99::llTables(analysis, static_cast<int>(terminals.size()),
                                static_cast<int>(nonterms.size()));

    const std::optional<std::string> userHeader =
        CodeGenC99::customUserH(userCodeWithLines, unionBodyWithLines, valueType);
    const std::map<std::string, std::string> *symbolMember =
        request.symbolValueMember;
    // 目标语言在入口处分岔：C++17 / Python 都有独立的后端与模板
    // （code_gen_cpp17.cpp / code_gen_python.cpp），不再对 C 模板做生成期文本改写。
    if (request.lang == "python")
    {
        return generatePythonProject(request, mode, copies, analysis, out,
                                     error);
    }
    if (request.lang == "csharp")
    {
        return generateCsharpProject(request, mode, copies, analysis, out,
                                     error);
    }
    if (request.lang == "rust")
    {
        return generateRustProject(request, mode, copies, analysis, out,
                                   error);
    }
    if (request.lang == "go")
    {
        return generateGoProject(request, mode, copies, analysis, out, error);
    }
    if (request.lang != "c")
    {
        return generateCpp17Project(request, mode, copies, analysis,
                                    userCodeWithLines, unionBodyWithLines,
                                    valueType, out, error);
    }
    std::vector<std::string> files = k_projectFiles;
    if (!userHeader.has_value())
    {
        files.erase(std::remove(files.begin(), files.end(), "parser_user.h"),
                    files.end());
    }
    // Mermaid 状态机图示（独立开关 %mermaid / --mermaid，与教学模式无关）
    std::vector<std::pair<std::string, std::string>> mermaidFiles;
    if (!Mermaid::projectFiles(analysis, lexSpec, tokenDefs, request.mermaid,
                               mermaidFiles, error))
    {
        return false;
    }
    for (const auto &entry : mermaidFiles)
    {
        files.push_back(entry.first);
    }
    const std::string modeLabel = modeLabelText(mode);
    const std::vector<std::string> copySources =
        copyNamesWithSuffix(copies, ".c");
    const std::vector<std::string> copyHeaders =
        copyNamesWithSuffix(copies, ".h");
    for (const CopyEntry &entry : copies)
    {
        files.push_back(entry.name);
    }

    std::map<std::string, std::string> project;
    project.insert_or_assign("parser.h",
                CodeGenC99::customParserH(defines, errorType, request.userCode,
                                       unionBody, valueType, request.tree,
                                       modeLabel));
    std::string parserC;
    if (!CodeGenC99::customParserC(
            *request.grammar, *request.tokens, tokenDefs, mode, request.tree,
            request.priority, lexSpec, request.spaceChars, request.teach,
            request.userCode, unionBody, valueType,
            symbolMember, request.destructors,
            request.syncTokens, &analysis, parserC, error))
    {
        return false;
    }
    project.insert_or_assign("parser.c", std::move(parserC));
    project.insert_or_assign("parser_token.h",
                CodeGenC99::customParserTokenH(tokenTypes, lexDecls, modeLabel));
    project.insert_or_assign("parser_token.c",
                CodeGenC99::customParserTokenC(
                    CodeGenC99::tokenNames(terminals), lexDecls, lexTables,
                    modeLabel));
    project.insert_or_assign(
        "parser_table.c",
        generatorParserTableC(false, tablesText, "", modeLabel));
    // %main：0 段用内建 demo main；1 段用用户内容替换 main.c 的正文；多段报错
    std::string mainBody;
    if (!buildMainBody(request, "main.c", mainBody, error))
    {
        return false;
    }
    project.insert_or_assign("main.c", mainBody.empty()
                              ? CodeGenC99::customMainC(
                                    request.tree,
                                    (request.lexSpec != nullptr) &&
                                        !request.lexSpec->commits.empty(),
                                    modeLabel)
                              : CodeGenC99::customMainCWithBody(mainBody,
                                                             modeLabel));
    project.insert_or_assign("Makefile",
                generatorMakefileCustom(userHeader.has_value(), copySources,
                                        copyHeaders, modeLabel));
    project.insert_or_assign("CMakeLists.txt",
                generatorCMakeCustom(copySources, modeLabel));
    for (const auto &entry : mermaidFiles)
    {
        project.insert_or_assign(entry.first, entry.second);
    }
    project.insert_or_assign("RESULT.md", generatorResultMd(request, mode, analysis, files));
    if (userHeader.has_value())
    {
        // parser_user.h 原样保留，不做空行压缩 / 占位符替换
        project.insert_or_assign("parser_user.h", *userHeader);
    }

    std::map<std::string, std::string> result;
    for (const std::string &name : files)
    {
        const auto textIt = project.find(name);
        const std::string *text =
            textIt != project.end() ? &textIt->second : nullptr;
        if (text == nullptr)
        {
            continue;
        }
        if (name == "parser_user.h")
        {
            // 用户代码块 / %union 的 #line 也要在这里落地
            std::string userText = *text;
            if (!request.cPrefix.empty())
            {
                const std::size_t generated = userText.find("语义值类型");
                const std::size_t pos = (generated == std::string::npos)
                                            ? std::string::npos
                                            : userText.rfind("ParserValue_T");
                if (pos != std::string::npos && pos >= generated)
                    userText.replace(pos, 13, request.cPrefix + "Value_T");
            }
            result.insert_or_assign("parser_user.h",
                       CodeGen::resolveLineDirectives(
                           userText,
                           (request.lines == nullptr) ? std::string()
                                                      : request.lines->grammarPath,
                           "parser_user.h"));
            continue;
        }
        // 模式标签已经由各模板在渲染时填好（customParserH / customMainC /
        // generatorParserTableC 等都传了 modeLabel），这里不再做全局替换
        std::string content = Utils::collapseBlankLines(*text);
        // %epilogue：贴到 parser.c 最后（定义留在全局作用域，别的编译单元
        // 能链到，和 parser_user.h 里的声明对得上）
        if (name == "parser.c")
        {
            appendEpilogue(request, "parser.c", false, content);
        }
        content = applyCPrefix(content, request);
        // #line 占位符落地：.gram 路径换真实路径，恢复行换最终行号与产物名
        content = CodeGen::resolveLineDirectives(
            content,
            (request.lines == nullptr) ? std::string()
                                       : request.lines->grammarPath,
            name);
        result.insert_or_assign(name, std::move(content));
    }
    // %copy 的内容原样写入，不做空行压缩
    for (const CopyEntry &entry : copies)
    {
        result.insert_or_assign(entry.name, entry.content);
    }
    out = std::move(result);
    return true;
}

} // namespace

namespace
{

const char *const k_projectManifestName = ".parseengine-manifest";
const char *const k_projectManifestHeader = "# ParseEngine generated files v1";

bool managedRelativePath(const std::string &text, std::filesystem::path &path,
                         Error &error)
{
    path = std::filesystem::u8path(text).lexically_normal();
    if (path.empty() || path == "." || path.is_absolute() || path.has_root_path())
    {
        return fail(error, "产物清单包含不安全路径：" + text);
    }
    for (const std::filesystem::path &part : path)
    {
        if (part == "..")
        {
            return fail(error, "产物清单不能越出输出目录：" + text);
        }
    }
    if (path.generic_u8string() == k_projectManifestName)
    {
        return fail(error, "产物不能占用内部清单名：" +
                               std::string(k_projectManifestName));
    }
    return true;
}

bool rejectSymlinkParents(const std::filesystem::path &root,
                          const std::filesystem::path &relative,
                          Error &error)
{
    std::filesystem::path current = root;
    for (const std::filesystem::path &part : relative.parent_path())
    {
        current /= part;
        std::error_code statusError;
        const std::filesystem::file_status status =
            std::filesystem::symlink_status(current, statusError);
        if (statusError && statusError != std::errc::no_such_file_or_directory)
            return fail(error, "无法检查产物目录：" + current.u8string() + "（" +
                                   statusError.message() + "）");
        if (!statusError && std::filesystem::is_symlink(status))
            return fail(error, "产物目录不能经过符号链接：" + current.u8string());
    }
    return true;
}

bool readProjectManifest(const std::filesystem::path &root,
                         std::set<std::string> &files, bool &exists,
                         Error &error)
{
    files.clear();
    const std::filesystem::path manifest = root / k_projectManifestName;
    std::error_code statusError;
    exists = std::filesystem::exists(manifest, statusError);
    if (statusError)
    {
        return fail(error, "无法检查产物清单：" + manifest.u8string() + "（" +
                               statusError.message() + "）");
    }
    if (!exists) return true;

    std::ifstream input(manifest, std::ios::binary);
    if (!input) return fail(error, "无法读取产物清单：" + manifest.u8string());
    std::string line;
    if (!std::getline(input, line))
        return fail(error, "产物清单为空或损坏：" + manifest.u8string());
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line != k_projectManifestHeader)
        return fail(error, "产物清单版本无法识别：" + manifest.u8string());
    while (std::getline(input, line))
    {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        std::filesystem::path relative;
        if (!managedRelativePath(line, relative, error)) return false;
        files.insert(relative.generic_u8string());
    }
    if (!input.eof()) return fail(error, "读取产物清单失败：" + manifest.u8string());
    return true;
}

bool removeManagedFiles(const std::filesystem::path &root,
                        const std::set<std::string> &files,
                        const std::set<std::string> &kept,
                        std::size_t &removed, Error &error)
{
    std::vector<std::filesystem::path> parents;
    for (const std::string &name : files)
    {
        if (kept.count(name) != 0u) continue;
        std::filesystem::path relative;
        if (!managedRelativePath(name, relative, error)) return false;
        if (!rejectSymlinkParents(root, relative, error)) return false;
        const std::filesystem::path target = root / relative;
        std::error_code statusError;
        const bool present = std::filesystem::exists(target, statusError) ||
                             std::filesystem::is_symlink(target, statusError);
        if (statusError)
            return fail(error, "无法检查旧产物：" + target.u8string() + "（" +
                                   statusError.message() + "）");
        if (present)
        {
            std::error_code removeError;
            if (!std::filesystem::remove(target, removeError) || removeError)
                return fail(error, "无法删除旧产物：" + target.u8string() + "（" +
                                       removeError.message() + "）");
            ++removed;
        }
        for (std::filesystem::path parent = target.parent_path();
             parent != root && !parent.empty(); parent = parent.parent_path())
            parents.push_back(parent);
    }
    std::sort(parents.begin(), parents.end(),
              [](const std::filesystem::path &left,
                 const std::filesystem::path &right)
              {
                  if (left.native().size() != right.native().size())
                      return left.native().size() > right.native().size();
                  return left.native() < right.native();
              });
    parents.erase(std::unique(parents.begin(), parents.end()), parents.end());
    for (const std::filesystem::path &parent : parents)
    {
        std::error_code emptyError;
        const bool empty = std::filesystem::is_empty(parent, emptyError);
        if (emptyError == std::errc::no_such_file_or_directory) continue;
        if (emptyError)
            return fail(error, "无法检查产物目录：" + parent.u8string() + "（" +
                                   emptyError.message() + "）");
        if (!empty) continue;
        std::error_code removeError;
        if (!std::filesystem::remove(parent, removeError) || removeError)
            return fail(error, "无法清理空产物目录：" + parent.u8string() + "（" +
                                   removeError.message() + "）");
    }
    return true;
}

bool writeProjectManifest(const std::filesystem::path &root,
                          const std::set<std::string> &files, Error &error)
{
    const std::filesystem::path manifest = root / k_projectManifestName;
    std::ofstream output(manifest, std::ios::binary | std::ios::trunc);
    if (!output) return fail(error, "无法写入产物清单：" + manifest.u8string());
    output << k_projectManifestHeader << '\n';
    for (const std::string &file : files) output << file << '\n';
    if (!output) return fail(error, "写入产物清单失败：" + manifest.u8string());
    return true;
}

bool projectRoot(const std::string &outDir, std::filesystem::path &root,
                 Error &error)
{
    std::error_code pathError;
    root = std::filesystem::absolute(std::filesystem::u8path(outDir.empty() ? "." : outDir), pathError)
               .lexically_normal();
    if (pathError)
        return fail(error, "无法解析输出目录：" + outDir + "（" +
                               pathError.message() + "）");
    return true;
}

} // namespace

/**
 * @brief 判断产物清单里的一项是不是「工程构建文件」。
 *
 * `--no-build-files` 时这些文件不该由生成器写出，也不该被当作过期产物删掉——
 * 用户可能已经手写了同名文件（例如 magel 自己维护的 `Makefile`）。
 * 只看路径的 basename，覆盖各语言后端的构建工程文件。
 * @param relative 产物清单里的相对路径（用 `/` 分隔）。
 * @return 是构建工程文件返回 true。
 */
bool Project::isBuildFileName(const std::string &relative)
{
    const std::size_t slash = relative.find_last_of("/\\");
    const std::string name =
        (slash == std::string::npos) ? relative : relative.substr(slash + 1u);
    if ((name == "Makefile") || (name == "makefile") ||
        (name == "GNUmakefile") || (name == "CMakeLists.txt") ||
        (name == "Cargo.toml") || (name == "Cargo.lock") ||
        (name == "go.mod") || (name == "go.sum") ||
        (name == "pom.xml") || (name == "build.gradle") ||
        (name == "build.gradle.kts") || (name == "settings.gradle") ||
        (name == "settings.gradle.kts") || (name == "package.json") ||
        (name == "package-lock.json") || (name == "tsconfig.json"))
    {
        return true;
    }
    const std::size_t dot = name.find_last_of('.');
    if (dot != std::string::npos)
    {
        const std::string extension = name.substr(dot);
        if ((extension == ".csproj") || (extension == ".sln") ||
            (extension == ".vcxproj") || (extension == ".fsproj") ||
            (extension == ".props") || (extension == ".targets"))
        {
            return true;
        }
    }
    return false;
}


/** @brief 组装已通过兼容性校验的工程。
 * @param request 生成配置。@param out 内存产物。@param error 失败原因。
 * @return 组装成功时为真。
 */
static bool generateParserProjectImpl(
    const ProjectRequest &request,
    std::map<std::string, std::string> &out, Error &error)
{
    std::string mode = request.mode;
    if (mode.empty())
    {
        mode = "ielr";
    }
    std::transform(mode.begin(), mode.end(), mode.begin(),
                   [](unsigned char ch)
                   { return static_cast<char>(std::tolower(ch)); });
    if ((mode != "ll1") && (mode != "llk") && (mode != "ll0") &&
        !isLrMode(mode) && !isLexMode(mode) && !isDdMode(mode))
    {
        return fail(error, "未知的生成模式：" + mode +
                               "（只支持 lex、ll1、lr0、slr、lalr、lr1、"
                               "ielr、glr、earley、cyk、dd、lrk、lalrk、slrk 或 llk）");
    }
    // GLR 家族只在带相应运行时的后端生成；C99 暂不支持。
    const bool narrowChart = ((mode == "earley") || (mode == "cyk")) &&
        ((request.lang == "java") || (request.lang == "js"));
    if (isGlrMode(mode) && !narrowChart && (request.lang != "gram") &&
        (request.lang != "cpp17") && (request.lang != "python") &&
        (request.lang != "rust") && (request.lang != "csharp") &&
        (request.lang != "go"))
    {
        return fail(error, modeLabelText(mode) +
                           " 只支持 --lang cpp17 / python / rust / csharp" +
                           " / go" +
                           "，当前目标语言是 " + request.lang);
    }
    if (isGlrMode(mode) && (request.lang != "gram") && request.usesError)
    {
        return fail(error, modeLabelText(mode) +
                           " 不支持 error 产生式：多路径结果重建没有确定的恢复点");
    }
    if (isGlrMode(mode) && (request.lang != "gram") &&
        request.destructors != nullptr &&
        !request.destructors->empty())
    {
        return fail(error, modeLabelText(mode) +
                           " 不支持 %destructor：多路径结果重建没有确定的丢弃次数");
    }
    if ((request.lang != "gram") &&
        ((mode == "earley") || (mode == "cyk")) &&
        request.syncTokens != nullptr && !request.syncTokens->empty())
    {
        return fail(error, modeLabelText(mode) +
                           " 不支持 %sync：图表判定严格检查整段输入，不跳过错误 Token");
    }
    // k 没有策略上限，但 k 元组列编码（Σ Token号 × Token个数^i）放不下就
    // 会算错，所以在生成期拦一下并给出这个文法的上限
    if (((mode == "lrk") || (mode == "lalrk") || (mode == "slrk") ||
         (mode == "llk") || (mode == "ll0")) &&
        (request.lookaheadK > lookaheadEncodingMaxK(
                                  request.tokens == nullptr
                                      ? 0u
                                      : request.tokens->size())))
    {
        return fail(error,
                    "k = " + std::to_string(request.lookaheadK) +
                        " 太大——k 元组列编码放不进 64 位（这个文法最多 k = " +
                        std::to_string(lookaheadEncodingMaxK(
                            request.tokens == nullptr
                                ? 0u
                                : request.tokens->size())) +
                        "）");
    }
    const std::vector<CopySpec> emptyCopies;
    const std::vector<CopySpec> &copySpecs =
        (request.copies == nullptr) ? emptyCopies : *request.copies;
    std::vector<CopyEntry> copies;
    if (!loadCopyEntries(copySpecs, copies, error))
    {
        return false;
    }
    // 文法导出档（--lang gram）：产物目录里只有文法原文件 + 分析报告，没有
    // 解析器 / 词法器 / 构建脚本——所以它在模式分岔之前就单独走一条路（lex
    // 模式也一样，这一档连词法器都不出）。
    if (request.lang == "gram")
    {
        return generateGramProject(request, mode, copies, out, error);
    }
    // 词法模式：产物里只有词法器与「看成什么 Token」的 CLI，没有解析器。
    if (isLexMode(mode))
    {
        if (request.lang == "python")
        {
            return generateLexPythonProject(request, copies, out, error);
        }
        if (request.lang == "csharp")
        {
            return generateLexCsharpProject(request, copies, out, error);
        }
        if (request.lang == "rust")
        {
            return generateLexRustProject(request, copies, out, error);
        }
        if (request.lang == "go")
        {
            return generateLexGoProject(request, copies, out, error);
        }
        if (request.lang != "c")
        {
            return generateLexCpp17Project(request, copies, out, error);
        }
        return generateLexProject(request, copies, out, error);
    }
    return generateCustomProject(request, mode, copies, out, error);
}

/** @brief 校验正则兼容性并组装带逐规则报告的内存工程。
 * @param request 生成配置。@param out 内存产物。@param error 第一条失败原因。
 * @return 校验和组装均成功时为真。
 */
bool Project::generateParserProject(
    const ProjectRequest &request, std::map<std::string, std::string> &out,
    Error &error)
{
    const LexSpec emptyLex;
    const auto report = RegexCompatibility::analyze(
        request.lexSpec ? *request.lexSpec : emptyLex, request.lang, request.cppLexer);
    if (!RegexCompatibility::validate(report, request.strictRegex, error)) return false;
    if (!generateParserProjectImpl(request, out, error)) return false;
    out["RESULT.md"] += RegexCompatibility::markdown(report);
    return true;
}

bool Project::writeParserProject(
    const std::string &outDir,
    const std::map<std::string, std::string> &project,
    std::vector<std::string> &written, Error &error,
    bool preserveBuildFiles)
{
    std::filesystem::path root;
    if (!projectRoot(outDir, root, error)) return false;
    std::error_code directoryError;
    if (!std::filesystem::is_directory(root, directoryError))
    {
        directoryError.clear();
        std::filesystem::create_directories(root, directoryError);
        if (directoryError)
            return fail(error, "无法创建输出目录：" + root.u8string() + "（" +
                                   directoryError.message() + "）");
    }
    std::set<std::string> currentFiles;
    for (const auto &entry : project)
    {
        std::filesystem::path relative;
        if (!managedRelativePath(entry.first, relative, error)) return false;
        if (!rejectSymlinkParents(root, relative, error)) return false;
        currentFiles.insert(relative.generic_u8string());
    }
    std::set<std::string> oldFiles;
    bool hasManifest = false;
    if (!readProjectManifest(root, oldFiles, hasManifest, error)) return false;
    for (const auto &entry : project)
    {
        const std::filesystem::path path = root / std::filesystem::u8path(entry.first);
        if (path.has_parent_path())
        {
            std::error_code parentError;
            std::filesystem::create_directories(path.parent_path(), parentError);
            if (parentError)
                return fail(error, "无法创建产物目录：" +
                                       path.parent_path().u8string() + "（" +
                                       parentError.message() + "）");
        }
        std::ofstream handle(path, std::ios::binary | std::ios::trunc);
        if (!handle)
        {
            return fail(error, "无法写入文件：" + path.u8string());
        }
        handle.write(entry.second.data(),
                     static_cast<std::streamsize>(entry.second.size()));
        written.push_back(path.u8string());
    }
    std::size_t staleRemoved = 0u;
    std::set<std::string> staleFiles = oldFiles;
    if (preserveBuildFiles)
    {
        /* `--no-build-files`：构建工程文件不归生成器管，别删用户的 Makefile 等。 */
        for (auto it = staleFiles.begin(); it != staleFiles.end();)
        {
            if (Project::isBuildFileName(*it))
                it = staleFiles.erase(it);
            else
                ++it;
        }
    }
    if (hasManifest &&
        !removeManagedFiles(root, staleFiles, currentFiles, staleRemoved, error))
        return false;
    return writeProjectManifest(root, currentFiles, error);
}

bool Project::clearParserProject(const std::string &outDir,
                                 std::size_t &removed, Error &error)
{
    removed = 0u;
    std::filesystem::path root;
    if (!projectRoot(outDir, root, error)) return false;
    std::set<std::string> files;
    bool hasManifest = false;
    if (!readProjectManifest(root, files, hasManifest, error)) return false;
    if (!hasManifest) return true;
    if (!removeManagedFiles(root, files, {}, removed, error)) return false;
    const std::filesystem::path manifest = root / k_projectManifestName;
    std::error_code removeError;
    if (!std::filesystem::remove(manifest, removeError) || removeError)
        return fail(error, "无法删除产物清单：" + manifest.u8string() + "（" +
                               removeError.message() + "）");
    return true;
}

} // namespace ParseEngine

// 字段绑定只能写在全局作用域（`TEMPLATE_MODEL` 展开的是显式特化）
TEMPLATE_MODEL(ParseEngine::ResultPrecedenceRow,
               tmpl::field("assoc", &ParseEngine::ResultPrecedenceRow::assoc),
               tmpl::field("name", &ParseEngine::ResultPrecedenceRow::name));

TEMPLATE_MODEL(ParseEngine::ResultRegexRow,
               tmpl::field("name", &ParseEngine::ResultRegexRow::name),
               tmpl::field("pattern", &ParseEngine::ResultRegexRow::pattern));

TEMPLATE_MODEL(ParseEngine::ResultIelrSplit,
               tmpl::field("lalr_state",
                           &ParseEngine::ResultIelrSplit::lalrState),
               tmpl::field("parts", &ParseEngine::ResultIelrSplit::parts),
               tmpl::field("token", &ParseEngine::ResultIelrSplit::token),
               tmpl::field("kind", &ParseEngine::ResultIelrSplit::kind),
               tmpl::field("reason", &ParseEngine::ResultIelrSplit::reason));
