/**
 * @file test_preprocess.cpp
 * @brief preprocess 的单元测试：条件编译、符号、包含、行号映射、诊断。
 *
 * 运行：
 *   preprocess-tests [示例目录]
 *
 * 示例目录缺省是 `examples`（相对当前目录）；里面有 `main.pp` / `common.inc` /
 * `debug.inc` / `expected-debug.txt`，用来做一次端到端的文件级对照。
 */

#include "preprocess.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <direct.h>
#define PP_MKDIR(path) _mkdir(path)
#else
#include <sys/stat.h>
#define PP_MKDIR(path) mkdir(path, 0755)
#endif

namespace
{

int checks = 0;
int failures = 0;

/**
 * @brief 记一条断言。
 */
#define CHECK(condition)                                                          \
    do                                                                           \
    {                                                                            \
        ++checks;                                                                \
        if (!(condition))                                                        \
        {                                                                        \
            ++failures;                                                          \
            std::cerr << "  失败：" #condition " （第 " << __LINE__ << " 行）\n"; \
        }                                                                        \
    } while (false)

/**
 * @brief 把字符串转义成一行，便于在失败信息里看。
 * @param text 原文。
 * @return 转义后的文本。
 */
std::string escape(const std::string &text)
{
    std::string out;
    for (char ch : text)
    {
        if (ch == '\n') out += "\\n";
        else if (ch == '\r') out += "\\r";
        else if (ch == '\t') out += "\\t";
        else out.push_back(ch);
    }
    return out;
}

/** 把工作区文本文件可能采用的 CRLF 统一为预处理器输出使用的 LF。 */
std::string normalizeNewlines(const std::string &text)
{
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i)
    {
        if (text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n') continue;
        out.push_back(text[i]);
    }
    return out;
}

/**
 * @brief 断言两个字符串相等，失败时把两边打印出来。
 * @param title 用例名。
 * @param actual 实际值。
 * @param expected 期望值。
 */
void checkText(const std::string &title, const std::string &actual,
               const std::string &expected)
{
    ++checks;
    if (actual == expected) return;
    ++failures;
    std::cerr << "  失败：" << title << "\n    实际：" << escape(actual)
              << "\n    期望：" << escape(expected) << "\n";
}

/**
 * @brief 取临时目录（Windows 用 TEMP，其余用 TMPDIR / /tmp）。
 * @return 目录路径。
 */
std::string tempDir()
{
    const char *base = std::getenv("TEMP");
    if (base == nullptr) base = std::getenv("TMPDIR");
    if (base == nullptr) base = "/tmp";
    return std::string(base);
}

/**
 * @brief 写一个临时文件。
 *
 * 文件放在 `<临时目录>/preprocess-tests/` 下，名字**原样使用**——因为被包含的
 * 文件里写的是相对路径（`%include "cycle-b.inc"`），名字必须对得上。
 *
 * @param name 文件名。
 * @param text 内容。
 * @return 完整路径。
 */
std::string writeTemp(const std::string &name, const std::string &text)
{
    const std::string dir = tempDir() + "/preprocess-tests";
    PP_MKDIR(dir.c_str()); // 已存在会返回 -1，忽略即可
    const std::string path = dir + "/" + name;
    std::ofstream stream(path, std::ios::binary);
    stream << text;
    stream.close();
    return path;
}

/**
 * @brief 删除临时文件。
 * @param path 路径。
 */
void removeTemp(const std::string &path)
{
    std::remove(path.c_str());
}

/**
 * @brief 把路径里的反斜杠换成正斜杠，便于和库输出的路径比较（库会规范化路径）。
 * @param path 路径。
 * @return 正斜杠版本。
 */
std::string normalized(std::string path)
{
    std::replace(path.begin(), path.end(), '\\', '/');
    return path;
}

// ---------------------------------------------------------------------------
//  用例
// ---------------------------------------------------------------------------

/**
 * @brief 没有指令时输入输出逐字节相同。
 */
void testIdentity()
{
    preprocess::Preprocessor preprocessor;
    preprocess::Result result;
    CHECK(preprocessor.runText("a\nb\n\nc\n", "<t>", "", result));
    checkText("无条件指令时逐字节不变", result.text, "a\nb\n\nc\n");
    CHECK(result.diagnostics.empty());
    CHECK(result.linesIn == 4u);
    CHECK(result.linesOut == 4u);
}

/**
 * @brief 默认「空行占位」：指令行与假分支都保留行数。
 */
void testPreserveLines()
{
    const std::string source =
        "%symbol DEBUG\n"
        "%if DEBUG\n"
        "keep\n"
        "%else\n"
        "drop\n"
        "%endif\n"
        "tail\n";

    preprocess::Preprocessor preprocessor;
    preprocess::Result result;
    CHECK(preprocessor.runText(source, "<t>", "", result));
    checkText("指令行与假分支都变成空行（行号不变）",
              result.text, "\n\nkeep\n\n\n\ntail\n");
    CHECK(result.linesOut == 7u);

    // 行号映射：第 i 行来自原文第 i 行。
    bool oneToOne = result.lineMap.size() == 7u;
    for (std::size_t index = 0; oneToOne && (index < result.lineMap.size()); ++index)
    {
        if (result.lineMap[index].line != index + 1u) oneToOne = false;
    }
    CHECK(oneToOne);

    // 关掉空行占位就是紧凑输出。
    preprocess::Options options;
    options.preserveLines = false;
    preprocess::Preprocessor compact(options);
    preprocess::Result compactResult;
    CHECK(compact.runText(source, "<t>", "", compactResult));
    checkText("--drop-lines 风格：只留下命中分支", compactResult.text, "keep\ntail\n");
}

/**
 * @brief C 风格的 #define / #undef / #ifdef / #ifndef。
 */
void testHashStyle()
{
    const std::string source =
        "#define DEBUG 1\n"
        "#ifdef DEBUG\n"
        "debug\n"
        "#endif\n"
        "#ifndef RELEASE\n"
        "not-release\n"
        "#endif\n"
        "#undef DEBUG\n"
        "#ifdef DEBUG\n"
        "never\n"
        "#else\n"
        "gone\n"
        "#endif\n";

    preprocess::Preprocessor preprocessor;
    preprocess::Result result;
    CHECK(preprocessor.runText(source, "<t>", "", result));
    checkText("#define / #ifdef / #ifndef / #undef",
              result.text, "\n\ndebug\n\n\nnot-release\n\n\n\n\n\ngone\n\n");
    CHECK(result.count(preprocess::Diagnostic::Severity::Error) == 0u);
}

/**
 * @brief 条件表达式：`|| && ! ()`、比较、defined()。
 */
void testExpressions()
{
    preprocess::Preprocessor preprocessor;

    // 1. 公共求值接口。
    std::map<std::string, std::string> symbols;
    symbols["A"] = "";
    symbols["B"] = "0";
    symbols["C"] = "hello";
    bool value = false;
    CHECK(preprocessor.evaluate("A", symbols, value) && value);
    CHECK(preprocessor.evaluate("B", symbols, value) && !value);
    CHECK(preprocessor.evaluate("!B", symbols, value) && value);
    CHECK(preprocessor.evaluate("A && !B", symbols, value) && value);
    CHECK(preprocessor.evaluate("B || (A && C == hello)", symbols, value) && value);
    CHECK(preprocessor.evaluate("defined(A) && !defined(NOPE)", symbols, value) && value);
    CHECK(preprocessor.evaluate("C != hello", symbols, value) && !value);
    preprocess::Diagnostic diagnostic;
    CHECK(!preprocessor.evaluate("A &&", symbols, value, &diagnostic));
    CHECK(diagnostic.code == "bad-expression");

    // 2. 条件编译里同样的语法。
    const std::string source =
        "%symbol LOG\n"
        "%symbol QUIET 0\n"
        "%if LOG && !QUIET\n"
        "verbose\n"
        "%elif LOG\n"
        "quiet\n"
        "%else\n"
        "silent\n"
        "%endif\n";
    preprocess::Result result;
    CHECK(preprocessor.runText(source, "<t>", "", result));
    CHECK(result.text.find("verbose") != std::string::npos);
    CHECK(result.text.find("silent") == std::string::npos);
}

/**
 * @brief 名字与定义解析的边界。
 */
void testNames()
{
    CHECK(preprocess::Preprocessor::isValidName("DEBUG"));
    CHECK(preprocess::Preprocessor::isValidName("_x1"));
    CHECK(!preprocess::Preprocessor::isValidName(""));
    CHECK(!preprocess::Preprocessor::isValidName("1x"));
    CHECK(!preprocess::Preprocessor::isValidName("a-b"));

    preprocess::Define define;
    preprocess::Diagnostic diagnostic;
    CHECK(preprocess::Preprocessor::parseDefine("NAME=value", define, &diagnostic));
    CHECK(define.name == "NAME");
    CHECK(define.value == "value");
    CHECK(preprocess::Preprocessor::parseDefine("ONLY", define, &diagnostic));
    CHECK(define.value.empty());
    CHECK(!preprocess::Preprocessor::parseDefine("=x", define, &diagnostic));

    CHECK(!preprocess::Preprocessor::parseTruthValue("0"));
    CHECK(!preprocess::Preprocessor::parseTruthValue("off"));
    CHECK(!preprocess::Preprocessor::parseTruthValue("False"));
    CHECK(preprocess::Preprocessor::parseTruthValue("1"));
    CHECK(preprocess::Preprocessor::parseTruthValue("yes"));
}

/**
 * @brief 命令行 `-D` 传进来的预定义符号。
 */
void testPredefinedSymbols()
{
    preprocess::Options options;
    options.defines.push_back(preprocess::Define{"DEBUG", ""});
    options.defines.push_back(preprocess::Define{"LEVEL", "2"});

    preprocess::Preprocessor preprocessor(options);
    preprocess::Result result;
    const std::string source =
        "%if DEBUG\n"
        "debug\n"
        "%endif\n"
        "%if LEVEL == 2\n"
        "level2\n"
        "%endif\n";
    CHECK(preprocessor.runText(source, "<t>", "", result));
    CHECK(result.text.find("debug") != std::string::npos);
    CHECK(result.text.find("level2") != std::string::npos);
}

/**
 * @brief include / import / include_once 与依赖表。
 */
void testInclude()
{
    const std::string inc = writeTemp("inc-a.inc", "A1\nA2\n");
    const std::string main =
        writeTemp("main-a.pp",
                  "%include \"inc-a.inc\"\n"
                  "%include \"inc-a.inc\"\n"
                  "%import \"inc-a.inc\"\n"
                  "after\n");

    preprocess::Preprocessor preprocessor;
    preprocess::Result result;
    CHECK(preprocessor.runFile(main, result));
    // 第一次展开，后两次被 include-once 跳过（跳过时补空行占位）。
    checkText("include 展开一次，之后去重", result.text, "A1\nA2\n\n\nafter\n");
    CHECK(result.dependencies.size() == 2u);
    CHECK(result.includeCount == 3u);

    // 关掉 include-once 就会重复展开。
    preprocess::Options options;
    options.includeOnceByDefault = false;
    preprocess::Preprocessor repeated(options);
    preprocess::Result repeatedResult;
    CHECK(repeated.runFile(main, repeatedResult));
    CHECK(repeatedResult.text.find("A1\nA2\nA1\nA2\n") != std::string::npos);

    removeTemp(inc);
    removeTemp(main);
}

/**
 * @brief 循环包含与深度限制。
 */
void testIncludeCycle()
{
    const std::string first = writeTemp("cycle-a.inc", "%include \"cycle-b.inc\"\n");
    const std::string second = writeTemp("cycle-b.inc", "%include \"cycle-a.inc\"\n");

    preprocess::Options options;
    options.includeOnceByDefault = false; // 关掉去重才能走到循环检测
    preprocess::Preprocessor preprocessor(options);
    preprocess::Result result;
    CHECK(!preprocessor.runFile(first, result));
    CHECK(result.count(preprocess::Diagnostic::Severity::Error) >= 1u);
    bool cycle = false;
    for (const preprocess::Diagnostic &diagnostic : result.diagnostics)
    {
        if (diagnostic.code == "include-cycle") cycle = true;
    }
    CHECK(cycle);

    removeTemp(first);
    removeTemp(second);
}

/**
 * @brief 找不到文件、未闭合条件、多余 else / endif。
 */
void testDiagnostics()
{
    {
        preprocess::Preprocessor preprocessor;
        preprocess::Result result;
        CHECK(!preprocessor.runText("%include \"nope.inc\"\n", "<t>", "", result));
        CHECK(result.diagnostics.size() == 1u);
        CHECK(result.diagnostics[0].code == "include-not-found");
        CHECK(result.diagnostics[0].line == 1u);
    }
    {
        preprocess::Preprocessor preprocessor;
        preprocess::Result result;
        CHECK(!preprocessor.runText("%if A\nx\n", "<t>", "", result));
        CHECK(result.diagnostics[0].code == "unterminated-if");
        CHECK(result.diagnostics[0].line == 1u);
    }
    {
        preprocess::Preprocessor preprocessor;
        preprocess::Result result;
        CHECK(!preprocessor.runText("x\n%endif\n", "<t>", "", result));
        CHECK(result.diagnostics[0].code == "unmatched-endif");
    }
    {
        preprocess::Preprocessor preprocessor;
        preprocess::Result result;
        CHECK(!preprocessor.runText("%else\n", "<t>", "", result));
        CHECK(result.diagnostics[0].code == "unmatched-else");
    }
    {
        preprocess::Preprocessor preprocessor;
        preprocess::Result result;
        CHECK(!preprocessor.runText("%if A\n%else\n%else\n%endif\n", "<t>", "", result));
        bool duplicate = false;
        for (const preprocess::Diagnostic &diagnostic : result.diagnostics)
        {
            if (diagnostic.code == "duplicate-else") duplicate = true;
        }
        CHECK(duplicate);
    }
    {
        preprocess::Preprocessor preprocessor;
        preprocess::Result result;
        CHECK(!preprocessor.runText("%frobnicate 1\n", "<t>", "", result));
        CHECK(result.diagnostics[0].code == "unknown-directive");
    }
    {
        preprocess::Preprocessor preprocessor;
        preprocess::Result result;
        CHECK(!preprocessor.runText("%error \"炸了\"\n", "<t>", "", result));
        CHECK(result.diagnostics[0].code == "error-directive");
        CHECK(result.diagnostics[0].message == "炸了");
    }
    {
        preprocess::Preprocessor preprocessor;
        preprocess::Result result;
        CHECK(preprocessor.runText("%warning \"小心\"\nx\n", "<t>", "", result));
        CHECK(result.count(preprocess::Diagnostic::Severity::Warning) == 1u);
        CHECK(result.ok()); // 警告不算失败
    }
}

/**
 * @brief 未知指令的三种策略：error / keep / ignore。
 */
void testUnknownPolicy()
{
    const std::string source = "%token NUMBER \"[0-9]+\"\n%define X \"y\"\n";

    preprocess::Options keep = preprocess::Preprocessor::forGrammar();
    preprocess::Preprocessor keepProcessor(keep);
    preprocess::Result keepResult;
    CHECK(keepProcessor.runText(source, "<t>", "", keepResult));
    checkText("keep：文法指令原样保留", keepResult.text, source);

    preprocess::Options ignore;
    ignore.unknownDirective = preprocess::Options::UnknownDirective::Ignore;
    preprocess::Preprocessor ignoreProcessor(ignore);
    preprocess::Result ignoreResult;
    CHECK(ignoreProcessor.runText(source, "<t>", "", ignoreResult));
    checkText("ignore：不认识的指令静默丢掉", ignoreResult.text, "\n\n");
}

/**
 * @brief 注释、字符串、不透明区域里的指令不当指令。
 */
void testNotDirectives()
{
    const std::string source =
        "// %if NOPE\n"
        "/*\n"
        "%if NOPE\n"
        "*/\n"
        "const char *s = \"%if NOPE\";\n"
        "%code\n"
        "%if NOPE\n"
        "%endcode\n"
        "%symbol OK\n"
        "%if OK\n"
        "yes\n"
        "%endif\n";

    // 用 .gram 预设：`%code … %endcode` 里的正文不解析。
    preprocess::Preprocessor preprocessor(preprocess::Preprocessor::forGrammar());
    preprocess::Result result;
    CHECK(preprocessor.runText(source, "<t>", "", result));
    CHECK(result.text.find("yes") != std::string::npos);
    CHECK(result.count(preprocess::Diagnostic::Severity::Error) == 0u);
}

/**
 * @brief 指令行末尾挂的注释要当注释：`%include "a.inc"   // 说明`。
 *
 * 引号里的行注释或块注释标记不算注释起点（路径里可能出现）。
 */
void testTrailingComment()
{
    const std::string includePath = writeTemp("trailing-comment.inc", "INCLUDED\n");
    const std::size_t slash = includePath.find_last_of("/\\");

    preprocess::Options options = preprocess::Preprocessor::forMetro();
    if (slash != std::string::npos) options.includeDirs.push_back(includePath.substr(0u, slash));

    const std::string source = "%include \"trailing-comment.inc\"   // 行注释\n"
                               "%include \"trailing-comment.inc\"   /* 块注释 */\n"
                               "%symbol NAME value // 注释\n"
                               "%if NAME == \"value\"\n"
                               "yes\n"
                               "%endif\n";

    preprocess::Preprocessor preprocessor(options);
    preprocess::Result result;
    CHECK(preprocessor.runText(source, "<t>", ".", result));
    CHECK(result.count(preprocess::Diagnostic::Severity::Error) == 0u);
    CHECK(result.text.find("INCLUDED") != std::string::npos);
    CHECK(result.text.find("yes") != std::string::npos);
}

/**
 * @brief #pragma once 与 metro 别名。
 */
void testPragmaAndMetroAliases()
{
    const std::string inc = writeTemp("once.inc", "#pragma once\nB1\n");
    const std::string main = writeTemp("main-once.pp",
                                       "#include \"once.inc\"\n"
                                       "#include \"once.inc\"\n"
                                       "tail\n");

    // 关掉默认去重，只有 #pragma once 生效时才只展开一次。
    preprocess::Options options;
    options.includeOnceByDefault = false;
    preprocess::Preprocessor preprocessor(options);
    preprocess::Result result;
    CHECK(preprocessor.runFile(main, result));
    // `#pragma once` 那一行本身变成空行，所以开头有一个空行。
    checkText("#pragma once 生效", result.text, "\nB1\n\ntail\n");

    // metro 预设：`%define` / `%undefine` 也算符号定义。
    preprocess::Preprocessor metroProcessor(preprocess::Preprocessor::forMetro());
    preprocess::Result metroResult;
    CHECK(metroProcessor.runText("%define M\n%ifdef M\nm\n%endif\n", "<t>", "", metroResult));
    CHECK(metroResult.text.find("m") != std::string::npos);

    removeTemp(inc);
    removeTemp(main);
}

/**
 * @brief `#line` 标记与行号映射里的来源文件。
 */
void testLineMarkers()
{
    const std::string inc = writeTemp("marker.inc", "IN\n");
    const std::string main = writeTemp("main-marker.pp",
                                      "top\n"
                                      "%include \"marker.inc\"\n"
                                      "bottom\n");

    preprocess::Options options;
    options.emitLineMarkers = true;
    preprocess::Preprocessor preprocessor(options);
    preprocess::Result result;
    CHECK(preprocessor.runFile(main, result));
    checkText("换文件时插 #line",
              result.text,
              "#line 1 \"" + normalized(main) + "\"\n"
              "top\n"
              "#line 1 \"" + normalized(inc) + "\"\n"
              "IN\n"
              "#line 3 \"" + normalized(main) + "\"\n"
              "bottom\n");

    removeTemp(inc);
    removeTemp(main);
}

/**
 * @brief 自定义 loader：让 `%import` 吃 CSV、`%include` 走默认、还能跳过与报错。
 *
 * 这里用一个最小的"CSV → 文本"回调，模拟 metro_editor 里
 * `%import "stations.csv" AS STATIONS` 那种"把数据源变成文法文本"的用法。
 */
void testIncludeLoader()
{
    const std::string csv = writeTemp("stations.csv", "name,lon,lat\nA,1,2\nB,3,4\n");
    const std::string main = writeTemp("main-loader.pp",
                                      "%import \"stations.csv\"\n"
                                      "%include \"plain.inc\"\n"
                                      "%import \"skip.csv\"\n"
                                      "%import \"broken.csv\"\n");
    writeTemp("plain.inc", "plain\n");
    writeTemp("skip.csv", "x\n");
    writeTemp("broken.csv", "y\n");

    preprocess::Options options;
    options.includeDirs.push_back(tempDir() + "/preprocess-tests");
    options.includeLoader = [](const preprocess::IncludeRequest &request) {
        // 1. 只接管 CSV：其它文件交回默认逻辑（读文件）。
        const bool isCsv = (request.path.size() >= 4u) &&
                           (request.path.compare(request.path.size() - 4u, 4u, ".csv") == 0);
        if (!isCsv) return preprocess::IncludeResult::useDefault();

        // 2. 按名字造出不同结果，覆盖 Skip / Error / 正常三种分支。
        if (request.path == "skip.csv") return preprocess::IncludeResult::skip();
        if (request.path == "broken.csv") return preprocess::IncludeResult::error("这份 CSV 坏了");
        if (!request.imported)
        {
            // `%include` 来的 CSV 不处理，交回默认（读原文）。
            return preprocess::IncludeResult::useDefault();
        }

        // 3. 真的把 CSV 转成文本：第一行是表头，后面每行变成 `列=值; 列=值`。
        std::ifstream stream(request.resolvedPath);
        std::vector<std::string> headers;
        std::vector<std::string> lines;
        std::string line;
        if (std::getline(stream, line))
        {
            std::string cell;
            for (char ch : line + ',')
            {
                if (ch == ',') { headers.push_back(cell); cell.clear(); }
                else if (ch != '\r') cell.push_back(ch);
            }
        }
        while (std::getline(stream, line))
        {
            std::vector<std::string> cells;
            std::string cell;
            for (char ch : line + ',')
            {
                if (ch == ',') { cells.push_back(cell); cell.clear(); }
                else if (ch != '\r') cell.push_back(ch);
            }
            std::string text = "row";
            for (std::size_t index = 0; index < cells.size(); ++index)
            {
                text += " " + (index < headers.size() ? headers[index] : std::string("col")) +
                        "=" + cells[index];
            }
            lines.push_back(text);
        }

        std::string generated;
        for (const std::string &item : lines) generated += item + "\n";
        return preprocess::IncludeResult::inlineText(generated, "<csv:" + request.path + ">");
    };

    preprocess::Preprocessor preprocessor(options);
    preprocess::Result result;
    CHECK(!preprocessor.runFile(main, result)); // broken.csv 故意报错

    // 1. CSV 被转成了文本，并且用回调给的 label 进依赖表。
    CHECK(result.text.find("row name=A lon=1 lat=2") != std::string::npos);
    CHECK(result.text.find("row name=B lon=3 lat=4") != std::string::npos);
    CHECK(result.text.find("plain") != std::string::npos);
    bool csvDependency = false;
    for (const std::string &dependency : result.dependencies)
    {
        if (dependency == "<csv:stations.csv>") csvDependency = true;
    }
    CHECK(csvDependency);

    // 2. Skip 分支什么都不做（那一行只留空行占位）。
    CHECK(result.text.find("skip") == std::string::npos);

    // 3. Error 分支给出 include-loader-error。
    bool loaderError = false;
    for (const preprocess::Diagnostic &diagnostic : result.diagnostics)
    {
        if ((diagnostic.code == "include-loader-error") &&
            (diagnostic.message == "这份 CSV 坏了"))
        {
            loaderError = true;
        }
    }
    CHECK(loaderError);

    removeTemp(csv);
    removeTemp(main);
    removeTemp(tempDir() + "/preprocess-tests/plain.inc");
    removeTemp(tempDir() + "/preprocess-tests/skip.csv");
    removeTemp(tempDir() + "/preprocess-tests/broken.csv");
}

/**
 * @brief 端到端：跑 examples/ 里的示例文件并与期望输出对照。
 * @param exampleDir 示例目录。
 */
void testExampleFixture(const std::string &exampleDir)
{
    const std::string main = exampleDir + "/main.pp";
    std::ifstream probe(main, std::ios::binary);
    if (!probe)
    {
        std::cerr << "  （跳过示例对照：读不到 " << main << "）\n";
        return;
    }
    probe.close();

    preprocess::Options options;
    options.includeDirs.push_back(exampleDir);
    options.defines.push_back(preprocess::Define{"DEBUG", ""});
    preprocess::Preprocessor preprocessor(options);
    preprocess::Result result;
    CHECK(preprocessor.runFile(main, result));

    std::string expected;
    std::ifstream expectedStream(exampleDir + "/expected-debug.txt", std::ios::binary);
    if (expectedStream)
    {
        expected.assign(std::istreambuf_iterator<char>(expectedStream),
                        std::istreambuf_iterator<char>());
        checkText("示例：与 expected-debug.txt 一致", result.text,
                  normalizeNewlines(expected));
    }

    CHECK(result.dependencies.size() == 3u); // main.pp + common.inc + debug.inc
}

} // namespace

/**
 * @brief 程序入口。
 * @param argc 参数个数。
 * @param argv 参数数组。
 * @return 0 全部通过，1 有失败。
 */
int main(int argc, char **argv)
{
    const std::string exampleDir = (argc > 1) ? argv[1] : "examples";

    testIdentity();
    testPreserveLines();
    testHashStyle();
    testExpressions();
    testNames();
    testPredefinedSymbols();
    testInclude();
    testIncludeCycle();
    testDiagnostics();
    testUnknownPolicy();
    testNotDirectives();
    testTrailingComment();
    testPragmaAndMetroAliases();
    testLineMarkers();
    testIncludeLoader();
    testExampleFixture(exampleDir);

    if (failures == 0)
    {
        std::cout << "preprocess-tests：全部通过（" << checks << " 项断言）\n";
        return 0;
    }
    std::cout << "preprocess-tests：" << failures << " / " << checks << " 项断言失败\n";
    return 1;
}
