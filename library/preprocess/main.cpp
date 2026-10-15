/**
 * @file main.cpp
 * @brief preprocess 命令行工具：把预处理结果写到文件或标准输出。
 *
 * 用法见 `--help`；退出码：0 成功，1 有错误诊断，2 用法错误。
 */

#include "preprocess.hpp"

#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace
{

/**
 * @brief 打印用法。
 * @param program 程序名。
 */
void printUsage(const std::string &program)
{
    std::cout
        << "用法：" << program << " <输入文件> [选项]\n"
        << "\n"
        << "输出：\n"
        << "  -o, --out FILE         输出文件（缺省写到标准输出）\n"
        << "      --keep-directives  指令行原样保留（默认用空行占位）\n"
        << "      --drop-lines       被丢掉的行直接删除（不补空行，输出更紧凑）\n"
        << "      --line-markers     换文件时插入 #line 标记\n"
        << "      --check            只做检查，不输出正文\n"
        << "      --deps             只列出依赖文件，每行一个\n"
        << "\n"
        << "符号与条件：\n"
        << "  -D, --define NAME[=V]  预定义符号（可重复）\n"
        << "  -U, --undef NAME       取消一个预定义符号（可重复）\n"
        << "      --case-insensitive 符号名大小写不敏感\n"
        << "      --hash             只认 # 前缀（C / C++ 家族）\n"
        << "      --percent          只认 % 前缀（yacc / .gram / metro 家族）\n"
        << "      --metro            等价 --percent 且把 %define 当符号定义\n"
        << "      --grammar          等价 --percent --unknown=keep 并带上 .gram 的不透明区域\n"
        << "      --syntax MODE      auto / hash / percent / custom，缺省 auto\n"
        << "\n"
        << "包含：\n"
        << "  -I, --include-dir DIR  额外搜索目录（可重复）\n"
        << "      --no-include-once  同一个文件允许重复展开\n"
        << "      --max-depth N      最大嵌套深度，缺省 32\n"
        << "      --csv-to-lines     演示自定义 loader：%import 的 .csv 按表头转成\n"
        << "                         `行名 列=值 列=值` 的文本行\n"
        << "\n"
        << "其它：\n"
        << "      --prefix CHARS     --syntax custom 时的指令前缀集合\n"
        << "      --unknown MODE     不认识的指令：error / keep / ignore，缺省 error\n"
        << "      --stats            把输入行数 / 输出行数 / 依赖数写到标准错误\n"
        << "  -h, --help             显示这份帮助\n"
        << "  -v, --version          显示版本\n";
}

/**
 * @brief 解析 `--unknown=` 的取值。
 * @param text 取值。
 * @param out 结果。
 * @return 合法返回 true。
 */
bool parseUnknownMode(const std::string &text, preprocess::Options::UnknownDirective &out)
{
    if (text == "error") { out = preprocess::Options::UnknownDirective::Error; return true; }
    if (text == "keep") { out = preprocess::Options::UnknownDirective::Keep; return true; }
    if (text == "ignore") { out = preprocess::Options::UnknownDirective::Ignore; return true; }
    return false;
}

/**
 * @brief 造一个演示用的 CSV loader。
 *
 * 规则：只接管 `%import` 的 `.csv`；第一行当表头，之后每行生成
 * `row 列=值 列=值 …` 一行文本。想接别的数据源（JSON / 数据库 / 内存表）时，
 * 照这个形状改 `options.includeLoader` 即可。
 *
 * @return 可挂到 `Options::includeLoader` 的回调。
 */
std::function<preprocess::IncludeResult(const preprocess::IncludeRequest &)> makeCsvLoader()
{
    return [](const preprocess::IncludeRequest &request) {
        // 1. 只接管 `%import` 的 .csv，其余交回默认逻辑。
        const bool isCsv = (request.path.size() >= 4u) &&
                           (request.path.compare(request.path.size() - 4u, 4u, ".csv") == 0);
        if (!isCsv || !request.imported) return preprocess::IncludeResult::useDefault();
        if (request.resolvedPath.empty())
        {
            return preprocess::IncludeResult::error("找不到 CSV：" + request.path);
        }

        // 2. 读文件：第一行当表头。
        std::ifstream stream(request.resolvedPath, std::ios::binary);
        if (!stream) return preprocess::IncludeResult::error("读不了 CSV：" + request.resolvedPath);

        /**
         * 按逗号切一行（够演示用；真正的 CSV 要处理引号与转义）。
         *
         * @param line 行内容。
         * @return 单元格列表。
         */
        auto splitCsv = [](std::string line) {
            std::vector<std::string> cells;
            if ((!line.empty()) && (line.back() == '\r')) line.pop_back();
            std::string cell;
            std::istringstream stream(line);
            while (std::getline(stream, cell, ',')) cells.push_back(cell);
            return cells;
        };

        std::string header;
        std::vector<std::string> headers;
        if (std::getline(stream, header)) headers = splitCsv(header);
        // 表头可能带 UTF-8 BOM（Windows 记事本另存为 UTF-8 时常见），去掉它。
        if (!headers.empty() && headers[0].size() >= 3u &&
            static_cast<unsigned char>(headers[0][0]) == 0xEFu &&
            static_cast<unsigned char>(headers[0][1]) == 0xBBu &&
            static_cast<unsigned char>(headers[0][2]) == 0xBFu)
        {
            headers[0] = headers[0].substr(3u);
        }

        std::string generated;
        std::size_t rowNumber = 0;
        std::string line;
        while (std::getline(stream, line))
        {
            const std::vector<std::string> cells = splitCsv(line);
            if (cells.empty()) continue;
            ++rowNumber;
            generated += "row" + std::to_string(rowNumber);
            for (std::size_t index = 0; index < cells.size(); ++index)
            {
                generated += " ";
                generated += (index < headers.size() && !headers[index].empty())
                                 ? headers[index]
                                 : ("col" + std::to_string(index + 1));
                generated += "=";
                generated += cells[index];
            }
            generated += "\n";
        }

        // 3. 用文件路径当标识：依赖表、去重、循环检测都认它。
        return preprocess::IncludeResult::inlineText(generated, request.resolvedPath);
    };
}

/**
 * @brief 把 `-U` 指定的符号从预定义里去掉。
 * @param options 选项。
 * @param names 要取消的名字。
 */
void removeDefines(preprocess::Options &options, const std::vector<std::string> &names)
{
    for (const std::string &name : names)
    {
        for (std::size_t index = 0; index < options.defines.size();)
        {
            if (options.defines[index].name == name) options.defines.erase(options.defines.begin() + index);
            else ++index;
        }
    }
}

} // namespace

/**
 * @brief 程序入口。
 * @param argc 参数个数。
 * @param argv 参数数组。
 * @return 退出码。
 */
int main(int argc, char **argv)
{
    const std::string program = (argc > 0) ? argv[0] : "preprocess";

    preprocess::Options options;
    std::string input;
    std::string output;
    std::vector<std::string> undefines;
    bool checkOnly = false;
    bool listDeps = false;
    bool showStats = false;

    // 1. 解析参数：`--opt=value` 与 `--opt value` 都接受。
    for (int index = 1; index < argc; ++index)
    {
        const std::string argument = argv[index];
        const std::size_t equal = argument.find('=');
        const std::string key = (equal == std::string::npos) ? argument : argument.substr(0, equal);
        const std::string inlineValue = (equal == std::string::npos) ? std::string()
                                                                     : argument.substr(equal + 1u);

        /**
         * 取选项的值：`--opt=v` 直接用，`--opt v` 取下一个参数。
         */
        auto takeValue = [&](const std::string &name, std::string &out) -> bool {
            if (!inlineValue.empty()) { out = inlineValue; return true; }
            if (index + 1 >= argc) { std::cerr << "选项 " << name << " 缺少取值\n"; return false; }
            out = argv[++index];
            return true;
        };

        if ((key == "-h") || (key == "--help"))
        {
            printUsage(program);
            return 0;
        }
        if ((key == "-v") || (key == "--version"))
        {
            std::cout << "preprocess 1.0（ParseEngine library/preprocess）\n";
            return 0;
        }
        if ((key == "-o") || (key == "--out"))
        {
            if (!takeValue(key, output)) return 2;
            continue;
        }
        if ((key == "-D") || (key == "--define"))
        {
            std::string text;
            if (!takeValue(key, text)) return 2;
            preprocess::Define define;
            preprocess::Diagnostic diagnostic;
            if (!preprocess::Preprocessor::parseDefine(text, define, &diagnostic))
            {
                std::cerr << "选项 -D： " << diagnostic.message << "\n";
                return 2;
            }
            options.defines.push_back(define);
            continue;
        }
        if ((key == "-U") || (key == "--undef"))
        {
            std::string text;
            if (!takeValue(key, text)) return 2;
            undefines.push_back(text);
            continue;
        }
        if ((key == "-I") || (key == "--include-dir"))
        {
            std::string text;
            if (!takeValue(key, text)) return 2;
            options.includeDirs.push_back(text);
            continue;
        }
        if (key == "--prefix")
        {
            std::string text;
            if (!takeValue(key, text)) return 2;
            if (text.empty()) { std::cerr << "--prefix 不能为空\n"; return 2; }
            options.directivePrefixes = text;
            continue;
        }
        if (key == "--unknown")
        {
            std::string text;
            if (!takeValue(key, text)) return 2;
            if (!parseUnknownMode(text, options.unknownDirective))
            {
                std::cerr << "--unknown 只接受 error / keep / ignore\n";
                return 2;
            }
            continue;
        }
        if (key == "--max-depth")
        {
            std::string text;
            if (!takeValue(key, text)) return 2;
            options.maxIncludeDepth = static_cast<std::size_t>(std::stoul(text));
            continue;
        }
        if (key == "--metro")
        {
            options.syntax = preprocess::Options::Syntax::Percent;
            options.percentDefineIsSymbol = true;
            continue;
        }
        if (key == "--hash") { options.syntax = preprocess::Options::Syntax::Hash; continue; }
        if (key == "--percent") { options.syntax = preprocess::Options::Syntax::Percent; continue; }
        if (key == "--grammar")
        {
            options.syntax = preprocess::Options::Syntax::Percent;
            options.unknownDirective = preprocess::Options::UnknownDirective::Keep;
            options.opaqueRegions = {
                {"%code", "%endcode", true},
                {"%class_body", "%endbody", true},
            };
            continue;
        }
        if (key == "--syntax")
        {
            std::string text;
            if (!takeValue(key, text)) return 2;
            if (text == "auto") options.syntax = preprocess::Options::Syntax::Auto;
            else if (text == "hash") options.syntax = preprocess::Options::Syntax::Hash;
            else if (text == "percent") options.syntax = preprocess::Options::Syntax::Percent;
            else if (text == "custom") options.syntax = preprocess::Options::Syntax::Custom;
            else
            {
                std::cerr << "--syntax 只接受 auto / hash / percent / custom\n";
                return 2;
            }
            continue;
        }
        if (key == "--case-insensitive") { options.caseInsensitive = true; continue; }
        if (key == "--no-include-once") { options.includeOnceByDefault = false; continue; }
        if (key == "--keep-directives") { options.keepDirectiveLines = true; continue; }
        if (key == "--drop-lines") { options.preserveLines = false; continue; }
        if (key == "--line-markers") { options.emitLineMarkers = true; continue; }
        if (key == "--csv-to-lines") { options.includeLoader = makeCsvLoader(); continue; }
        if (key == "--check") { checkOnly = true; continue; }
        if (key == "--deps") { listDeps = true; continue; }
        if (key == "--stats") { showStats = true; continue; }

        if (!argument.empty() && (argument[0] == '-') && (input.empty()))
        {
            std::cerr << "不认识的选项：" << argument << "\n";
            return 2;
        }
        if (input.empty()) input = argument;
        else
        {
            std::cerr << "只能给一个输入文件（多余的是 " << argument << "）\n";
            return 2;
        }
    }

    if (input.empty())
    {
        printUsage(program);
        return 2;
    }

    removeDefines(options, undefines);

    // 2. 跑预处理。
    preprocess::Preprocessor preprocessor(options);
    preprocess::Result result;
    const bool ok = preprocessor.runFile(input, result);

    // 3. 诊断一律写到 stderr，方便管道里只拿正文。
    for (const preprocess::Diagnostic &diagnostic : result.diagnostics)
    {
        std::cerr << diagnostic.toString() << "\n";
    }

    if (listDeps)
    {
        for (const std::string &dependency : result.dependencies)
        {
            std::cout << dependency << "\n";
        }
    }
    else if (!checkOnly)
    {
        if (output.empty())
        {
            std::cout << result.text;
        }
        else
        {
            std::ofstream stream(output, std::ios::binary);
            if (!stream)
            {
                std::cerr << "写不了输出文件：" << output << "\n";
                return 2;
            }
            stream << result.text;
        }
    }

    if (showStats)
    {
        std::cerr << "输入 " << result.linesIn << " 行，输出 " << result.linesOut
                  << " 行，include " << result.includeCount << " 次，依赖 "
                  << result.dependencies.size() << " 个，诊断 "
                  << result.count(preprocess::Diagnostic::Severity::Error) << " 错 / "
                  << result.count(preprocess::Diagnostic::Severity::Warning) << " 警\n";
    }

    return ok ? 0 : 1;
}
