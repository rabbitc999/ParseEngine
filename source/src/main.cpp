/**
 * @file main.cpp
 * @brief Yacc 风格解析器生成器的命令行入口。
 */

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "ParseEngine/analysis.hpp"
#include "ParseEngine/chart_grammar.hpp"
#include "ParseEngine/error.hpp"
#include "ParseEngine/grammar_loader.hpp"
#include "ParseEngine/manual.hpp"
#include "ParseEngine/mermaid.hpp"
#include "ParseEngine/preprocessor.hpp"
#include "ParseEngine/project.hpp"
#include "ParseEngine/utils.hpp"
#include "ParseEngine/i18n.hpp"
#include "ParseEngine/package.hpp"
#include "lsp/lsp.hpp"

namespace
{

/// 命令行参数解析结果。
struct CommandLine
{
    std::string outDir = ".";         ///< 输出目录（默认当前目录，不再多套一层 out/）
    std::optional<std::string> grammarFile; ///< 文法文件
    std::optional<std::string> report;      ///< 报告另存路径
    std::optional<std::string> mode;        ///< 命令行模式
    std::optional<std::string> lang;        ///< 命令行目标语言
    std::optional<std::string> cppNamespace; ///< 命令行命名空间（--namespace）
    std::optional<std::string> cPrefix; ///< 命令行 C 符号前缀（--prefix）
    std::optional<std::string> cppLexer; ///< 命令行 C++17 词法引擎（--cpp-lexer）
    std::optional<std::string> pythonClass; ///< 命令行解析器类名（--class）
    std::optional<bool> teach;              ///< 教学模式（--teach / --no-teach）
    std::optional<bool> mermaid;            ///< 状态机图示（--mermaid / --no-mermaid）
    /// 是否导出工程构建文件（--build-files / --no-build-files；缺省打开）。
    /// 关闭时只写目标语言源码与 RESULT.md，CMakeLists / Makefile / Cargo.toml /
    /// *.csproj / pom.xml / package.json 等留给用户自己写。
    std::optional<bool> buildFiles;
    std::optional<int> lookahead;           ///< LR(k) / LALR(k) 的 k（--lookahead）
    ParseEngine::DefineMap defines;        ///< 条件编译变量（--define NAME[=VALUE]）
    bool dumpLrAutomaton = false;     ///< 打印 LR 状态 / 项集 / 转移
    bool dumpSlrTables = false;       ///< 打印 ACTION / GOTO 表
    bool dumpMermaid = false;         ///< 打印教学模式的 Mermaid 状态机图
    bool preprocessOnly = false;      ///< [调试] 只跑条件编译预处理并打印结果
    bool dumpSections = false;        ///< [调试] 只打印收集到的 %epilogue / %main 段
    bool syntaxOnly = false;          ///< 只保留词法/产生式，供解析预览生成纯语法树
    /// RESULT.md 里带完整分析报告（--full-report；缺省简化版，省略末尾那一节）
    bool fullReport = false;
    bool clearCommand = false;        ///< 使用位置子命令 clear
    bool help = false;                ///< 是否请求帮助
};

/**
 * @brief 打印用法说明。
 * @param program 程序名。
 */
void printUsage(const std::string &program)
{
    if(ParseEngine::I18n::language()!=ParseEngine::I18n::Language::Zh){
        std::cout << PE_TEXT("用法：") << program << PE_TEXT(" [文法文件] [选项]\n")
                  << PE_TEXT("ParseEngine 帮助正文");
        return;
    }
    std::cout
        << "用法：" << program << " [文法文件] [选项]\n"
        << "       " << program << " clear --out-dir 目录\n"
        << "       文法文件可以直接写在命令行上（等价于 --grammar-file 文件）：\n"
        << "         " << program << " example/calc.gram --out-dir /tmp/calc --lang c\n"
        << "       不写文法文件时从标准输入读（管线里最常见：cat x.gram | "
        << program << " --lang python）\n"
        << "  --out-dir 目录        输出目录（默认 . 当前目录）\n"
        << "  --grammar-file 文件   文法文件（与位置参数等价，二者只能给一个）\n"
        << "  --report 文件         把文法分析报告另存为 UTF-8 文本\n"
        << "  --full-report         RESULT.md 里带上完整分析报告：默认是简化版，写到\n"
        << "                        「未消解冲突」就收尾，省略末尾的「## 文法分析报告」\n"
        << "                        （那一节的正文随时可以用 --report 文件单独导出）\n"
        << "  --mode {ll1,lr0,slr,lalr,lr1,ielr,lrk,lalrk,glr,earley,cyk,dd} 生成模式（默认 ielr）\n"
        << "                        文法没写 %mode 时用它。lr0 / lr1 算规范 LR(0) / LR(1)\n"
        << "                        自动机与分析表，不用任何开关就能用，未消解冲突\n"
        << "                        只报不中止；dd 是带回溯的递归下降（不建表，\n"
        << "                        直接照文法生成每个非终结符的解析函数）\n"
        << "                        clear 是维护模式：不读取文法，只删除 --out-dir\n"
        << "                        内产物清单登记的生成文件；等价于位置子命令 clear\n"
        << "  --lookahead K         LR(k) / LALR(k) 的 k（也可用 %lookahead 指定；\n"
        << "                        命令行优先）。K 取 1..3、默认 2，只对 --mode lrk /\n"
        << "                        lalrk 有意义；k = 1 时两者分别等于 lr1 / lalr\n"
        << "  --lang {c,cpp17,python,csharp,rust,go,java,js,gram}  目标语言（默认 cpp17；\n"
        << "                        也可用 %lang 指定）\n"
        << "                        python 产物是 parser.py + main.py（类名可用\n"
        << "                        %class / --class 改），不需要编译\n"
        << "                        csharp 产物是解析器 + Token + 入口 +\n"
        << "                        构建脚本（dotnet build / dotnet run）\n"
        << "                        rust 产物是 Cargo 工程（src/parser.rs +\n"
        << "                        src/token.rs + src/main.rs），只用标准库，\n"
        << "                        crate 名可用 %package 改（cargo run）\n"
        << "                        go 产物是 Go 模块（parser.go + token.go +\n"
        << "                        main.go + go.mod），只用标准库，module path\n"
        << "                        可用 %package 改（go build / go run .）\n"
        << "                        java 与 js 当前支持 dd / earley；java 产物是\n"
        << "                        Parser.java，js 产物是带 TypeScript 类型标注的\n"
        << "                        parser.ts（Node.js 22+ 可直接运行）\n"
        << "                        gram 档不生成解析器：只把文法原文件与分析\n"
        << "                        报告（RESULT.md）导出到 --out-dir\n"
        << "  --namespace NAME      C++17 产物的命名空间名与文件名前缀（也可用\n"
        << "                        %namespace 指定；命令行优先）。NAME 取\n"
        << "                        [A-Za-z_][A-Za-z0-9_]*、不能是 C++ 关键字；\n"
        << "                        不传或传空串 = 命名空间 Parser、文件名不带前缀\n"
        << "  --cpp-lexer {dfa,std} C++17 产物的词法引擎（也可用 %cpp_lexer 指定；\n"
        << "                        命令行优先）。dfa（缺省）在生成期把正则编译成 DFA\n"
        << "                        表写进产物，产物不依赖 <regex>，每个位置只扫一遍\n"
        << "                        输入（大文法更快）；std 把正则交给 <regex>\n"
        << "  --prefix NAME        C99 产物的公开符号前缀（也可用 %prefix 指定；\n"
        << "                        命令行优先；不能以 Parser/PARSER 开头）\n"
        << "  --class NAME          Python 产物的解析器类名（也可用 %class 指定；\n"
        << "                        命令行优先）。NAME 取 [A-Za-z_][A-Za-z0-9_]*、\n"
        << "                        不能是 Python 关键字；不传 = 类名 Parser\n"
        << "  --teach / --no-teach  教学模式开关（也可用 %teach 指定；默认关闭）\n"
        << "                        开启后 slr / lalr / ielr 的未消解冲突不再中止生成，\n"
        << "                        报告多出 [教学提示] 与 [教学对照]\n"
        << "  --mermaid / --no-mermaid  生成 Mermaid 状态机图示（也可用 %mermaid 指定；\n"
        << "                        默认关闭），写进产物的 mermaid/ 目录；与教学模式无关\n"
        << "  --build-files / --no-build-files  是否导出工程构建文件（也可用 %build_files\n"
        << "                        指定；默认打开）。关闭时只写目标语言源码与 RESULT.md，\n"
        << "                        CMakeLists / Makefile / Cargo.toml / *.csproj / pom.xml /\n"
        << "                        package.json / go.mod 留给用户自己写（命令行优先）\n"
        << "  --define NAME[=VALUE] 条件编译变量（%if 用）：NAME 等价 NAME=1；\n"
        << "                        0/false/no/off 为假（大小写不敏感），其余非空为真；\n"
        << "                        可重复，同名后传覆盖先传\n"
        << "  --syntax-only        预览生成：强制启用语法树，忽略语义动作、用户代码与\n"
        << "                        目标语言专属类型；用于 Token / 语法树可视化\n"
        << "  --preprocess-only     [调试] 只跑 %if 条件编译预处理，打印处理后的文本\n"
        << "                        与行号映射，不生成产物\n"
        << "  --dump-sections       [调试] 只打印收集到的 %epilogue / %main 段（含行号），\n"
        << "                        不生成产物\n"
        << "  --dump-lr-automaton   打印 LR(0) / SLR(1) / LALR(1) / LR(1) / IELR(1)\n"
        << "                        状态、项集与转移\n"
        << "  --dump-slr-tables     打印 ACTION / GOTO 表\n"
        << "  --dump-mermaid        打印 Mermaid 状态机图（文法 LR 自动机与词法 DFA）\n"
        << "  -h, --help            显示本帮助\n"
        << "  --ui-lang zh|en|auto   显示语言；默认自动检测，与 --lang 目标语言独立\n"
        << "  lsp [--stdio|--check FILE...]  内嵌的 `.gram` 语言服务器（编辑器用法；\n"
        << "                        与 library/lsp 独立构建的 lsp-server 同参数同输出）\n"
        << "  man [目标] [--json]   内嵌手册：列出文档页、文法指令、生成模式、目标语言\n"
        << "                        与示例；`man <页>` 显示正文，`man --search <文本>` 搜\n"
        << "                        关键词，`man --help` 看全部用法（--lang 选正文语言）\n"
        << "  zip [out FILE.zip]    调用 Python 3.11+ 发布脚本打包当前可执行文件\n"
        << "\n"
        << "文法指令（%token / %define / %mode / %if …，含 %token NAME include \"csv\"\n"
        << "这种从 CSV 追加字面量别名的写法）见 doc/12-grammar-usage.md 与 doc/05-grammar.md。\n";
}

/**
 * @brief 打印一条错误（格式与提示语和旧版一致）。
 * @param error 错误值。
 */
void printError(const ParseEngine::Error &error)
{
    std::cerr << PE_TEXT("错误：");
    if (error.line > 0u)
    {
        std::cerr << PE_TEXT("第 ") << error.line << PE_TEXT(" 行：");
    }
    std::cerr << ParseEngine::I18n::diagnostic(error.message) << "\n";
}

/**
 * @brief 产物目录里那份「生成结果」文档的路径（控制台提示用）。
 *
 * 产物清单里的 `RESULT.md` 是唯一不带前缀改名的文档，但这里仍以**实际写出
 * 的路径**为准；万一清单里没有（理论上不会），按输出目录拼一个。
 * @param outDir 输出目录。
 * @param written 实际写出的文件路径列表。
 * @return `RESULT.md` 的路径。
 */
std::string resultDocPath(const std::string &outDir,
                          const std::vector<std::string> &written)
{
    const std::string name = "RESULT.md";
    for (const std::string &path : written)
    {
        if ((path.size() >= name.size()) &&
            (path.compare(path.size() - name.size(), name.size(), name) == 0))
        {
            return path;
        }
    }
    if (outDir.empty())
    {
        return name;
    }
    if ((outDir.back() == '/') || (outDir.back() == '\\'))
    {
        return outDir + name;
    }
    return outDir + "/" + name;
}

/**
 * @brief 读一个文本文件的全部内容（二进制模式，不去动行尾）。
 * @param path 文件路径。
 * @param out 输出文件内容（成功时写入）。
 * @param error 出错时写入错误说明。
 * @return 成功返回 true；打不开文件返回 false。
 */
bool readGrammarText(const std::string &path, std::string &out,
                     ParseEngine::Error &error)
{
    std::ifstream stream(std::filesystem::u8path(path), std::ios::binary);
    if (!stream)
    {
        return ParseEngine::fail(error, PE_TEXT("无法打开文法文件: ") + path);
    }
    out = std::string(std::istreambuf_iterator<char>(stream),
                      std::istreambuf_iterator<char>());
    return true;
}

/**
 * @brief 解析 argv。
 * @param argc 参数个数。
 * @param argv 参数数组。
 * @param parsed 输出参数。
 * @return 解析成功返回 true；选项非法时返回 false。
 */
bool parseArguments(int argc, char **argv, CommandLine &parsed)
{
    for (int index = 1; index < argc; ++index)
    {
        const std::string arg = argv[index];
        const auto takeValue = [&argc, &argv, &index, &arg]() -> std::string
        {
            const std::size_t equal = arg.find('=');
            if (equal != std::string::npos)
            {
                return arg.substr(equal + 1);
            }
            if (index + 1 >= argc)
            {
                return std::string();
            }
            ++index;
            return argv[index];
        };
        if (arg == "-h" || arg == "--help")
        {
            parsed.help = true;
        }
        else if (arg == "--dump-lr-automaton")
        {
            parsed.dumpLrAutomaton = true;
        }
        else if (arg == "--dump-mermaid")
        {
            parsed.dumpMermaid = true;
        }
        else if (arg == "--mermaid")
        {
            parsed.mermaid = true;
        }
        else if (arg == "--no-mermaid")
        {
            parsed.mermaid = false;
        }
        else if (arg == "--build-files" || arg == "--build-files=1" ||
                 arg == "--build-files=true")
        {
            parsed.buildFiles = true;
        }
        else if (arg == "--no-build-files" || arg == "--build-files=0" ||
                 arg == "--build-files=false")
        {
            parsed.buildFiles = false;
        }
        else if (arg == "--dump-slr-tables")
        {
            parsed.dumpSlrTables = true;
        }
        else if (arg == "--teach")
        {
            parsed.teach = true;
        }
        else if (arg == "--no-teach")
        {
            parsed.teach = false;
        }
        else if (arg == "--full-report")
        {
            parsed.fullReport = true;
        }
        else if (arg.rfind("--out-dir", 0) == 0)
        {
            parsed.outDir = takeValue();
        }
        else if (arg.rfind("--grammar-file", 0) == 0)
        {
            const std::string value = takeValue();
            if (value.empty())
            {
                std::cerr << PE_TEXT("错误：--grammar-file 后面要跟文法文件名\n");
                return false;
            }
            if (parsed.grammarFile.has_value())
            {
                std::cerr << PE_TEXT("错误：文法文件给了两个（") << *parsed.grammarFile
                          << PE_TEXT(" 与 ") << value << PE_TEXT("），只能留一个\n");
                return false;
            }
            parsed.grammarFile = value;
        }
        else if (arg.rfind("--report", 0) == 0)
        {
            parsed.report = takeValue();
        }
        else if (arg.rfind("--mode", 0) == 0)
        {
            parsed.mode = takeValue();
        }
        else if (arg.rfind("--lookahead", 0) == 0)
        {
            const std::string value = takeValue();
            int parsedValue = 0;
            if ((value.empty()) ||
                (value.find_first_not_of("0123456789") != std::string::npos))
            {
                std::cerr << PE_TEXT("错误：--lookahead 后应跟非负整数 k，实际得到 '")
                          << value << "'\n";
                return false;
            }
            // 手工累加而不是 std::stoi：生成器不开异常（`-fno-exceptions` 也要
            // 能构建），stoi 在超范围时会抛 out_of_range；逐位累加并在乘 10
            // 之前检查 int 上限，既能拦住溢出、也不必引入异常。
            bool overflow = false;
            for (const char ch : value)
            {
                const int digit = ch - '0';
                if (parsedValue >
                    ((std::numeric_limits<int>::max)() - digit) / 10)
                {
                    overflow = true;
                    break;
                }
                parsedValue = parsedValue * 10 + digit;
            }
            if (overflow)
            {
                std::cerr << PE_TEXT("错误：--lookahead 超出了 int 的范围：'") << value
                          << "'\n";
                return false;
            }
            parsed.lookahead = parsedValue;
        }
        else if (arg.rfind("--lang", 0) == 0)
        {
            parsed.lang = takeValue();
        }
        else if (arg.rfind("--namespace", 0) == 0)
        {
            const std::string name = takeValue();
            // 空串 = 退化成现状（命名空间 Parser、文件名不带前缀）
            if (!name.empty() &&
                !ParseEngine::isValidCppNamespaceName(name))
            {
                if (name.find("::") != std::string::npos)
                {
                    std::cerr << PE_TEXT("错误：--namespace 只支持单级名字，不支持 "
                                 "'::'（实际得到 '") << name << "'）\n";
                }
                else
                {
                    std::cerr << PE_TEXT("错误：--namespace 的名字必须是 C++ 标识符"
                                 "（[A-Za-z_][A-Za-z0-9_]*）且不能是 C++ "
                                 "关键字，实际得到 '") << name << "'\n";
                }
                return false;
            }
            parsed.cppNamespace = name;
        }
        else if (arg.rfind("--cpp-lexer", 0) == 0)
        {
            const std::string value = takeValue();
            if ((value != "std") && (value != "dfa"))
            {
                std::cerr << PE_TEXT("错误：--cpp-lexer 只接受 std 或 dfa，实际得到 '")
                          << value << "'\n";
                return false;
            }
            parsed.cppLexer = value;
        }
        else if (arg.rfind("--prefix", 0) == 0)
        {
            const std::string name = takeValue();
            if (!name.empty() && !ParseEngine::isValidCPrefixName(name))
            {
                std::cerr << PE_TEXT("错误：--prefix 的名字必须是 C 标识符且不能以 Parser/PARSER 开头，实际得到 '") << name << "'\n";
                return false;
            }
            parsed.cPrefix = name;
        }
        else if (arg.rfind("--class", 0) == 0)
        {
            const std::string name = takeValue();
            // 空串 = 用缺省类名 Parser
            if (!name.empty() &&
                !ParseEngine::isValidPythonClassName(name))
            {
                std::cerr << PE_TEXT("错误：--class 的名字必须是标识符"
                             "（[A-Za-z_][A-Za-z0-9_]*）且不能是 Python "
                             "关键字，实际得到 '") << name << "'\n";
                return false;
            }
            parsed.pythonClass = name;
        }
        else if (arg.rfind("--define", 0) == 0)
        {
            const std::string text = takeValue();
            std::string name;
            bool value = false;
            ParseEngine::Error defineError;
            if (!ParseEngine::Preprocessor::parseDefineArgument(
                    text, name, value, defineError))
            {
                printError(defineError);
                return false;
            }
            // 同名后传覆盖先传
            parsed.defines[name] = value;
        }
        else if (arg == "--preprocess-only")
        {
            parsed.preprocessOnly = true;
        }
        else if (arg == "--syntax-only")
        {
            parsed.syntaxOnly = true;
        }
        else if (arg == "--dump-sections")
        {
            parsed.dumpSections = true;
        }
        else if (arg == "clear" && index == 1 &&
                 !parsed.grammarFile.has_value())
        {
            parsed.clearCommand = true;
        }
        else if (!arg.empty() && (arg[0] != '-'))
        {
            // 位置参数就是文法文件，等价于 --grammar-file；两者只能给一个
            if (parsed.grammarFile.has_value())
            {
                std::cerr << PE_TEXT("错误：文法文件给了两个（--grammar-file ")
                          << *parsed.grammarFile << PE_TEXT(" 与位置参数 ") << arg
                          << PE_TEXT("），只能留一个\n");
                return false;
            }
            parsed.grammarFile = arg;
        }
        else
        {
            std::cerr << PE_TEXT("错误：无法识别的参数 ") << arg << "\n";
            return false;
        }
    }
    if (parsed.clearCommand)
    {
        std::string mode = parsed.mode.value_or("clear");
        std::transform(mode.begin(), mode.end(), mode.begin(),
                       [](unsigned char ch)
                       { return static_cast<char>(std::tolower(ch)); });
        if (mode != "clear")
        {
            std::cerr << PE_TEXT("错误：位置子命令 clear 不能与 --mode ") << mode
                      << PE_TEXT(" 同时使用\n");
            return false;
        }
        parsed.mode = "clear";
    }
    return true;
}

} // namespace

/**
 * @brief 命令行主流程。
 * @param argc 参数个数。
 * @param argv 参数数组。
 * @return 进程退出码；0 表示成功。
 */
int main(int argc, char **argv)
{
    const tool_console::Session console;
    auto arguments=tool_console::arguments(argc,argv);
    std::string localeError;
    if(!ParseEngine::I18n::configure(arguments,localeError)){std::cerr<<PE_TEXT("错误：")<<localeError<<"\n";return 2;}
    std::vector<char*> pointers;for(auto &argument:arguments)pointers.push_back(argument.data());
    argc=static_cast<int>(pointers.size());argv=pointers.data();
    if(argc>1 && arguments[1]=="zip"){
        if(argc==3 && (arguments[2]=="help" || arguments[2]=="--help" || arguments[2]=="-h")){printUsage(argv[0]);return 0;}
        std::string output;
        if(argc==4 && (arguments[2]=="out" || arguments[2]=="--out"))output=arguments[3];
        else if(argc!=2){std::cerr<<PE_TEXT("错误：zip 只接受 out FILE.zip 或 --out FILE.zip\n");return 2;}
        return ParseEngine::packageExecutable(argv[0],output);
    }
    // LSP 内嵌在生成器里：library/lsp 的 json/grammar/lsp 三份源码一起编进这个
    // 可执行文件，`ParseEngine lsp --stdio` 就是一份完整的 `.gram` 语言服务器，
    // 参数、退出码与独立 lsp-server 一致（见 library/lsp/README.md）。放在
    // parseArguments 之前：`lsp` 是场景子命令，后面的选项归它自己解析。
    //
    // 传参要错开一位：lsp::runCommandLine 是按"argv[0] 是可执行文件名"写的，
    // 从 argv[1] 开始扫选项。整体传进去的话它会把 `lsp` 当成待校验的文件名，
    // 于是多报一条"lsp: error: 无法读取文件"、退出码也永远是 1。
    if(argc>1 && arguments[1]=="lsp"){
        return lsp::runCommandLine(argc-1,argv+1);
    }
    // 内嵌手册同理：doc/ 的正文在构建期被 tools/embed_manual.py 压进
    // source/src/manual_data.cpp，`ParseEngine man` 运行时不需要 doc/ 目录。
    if(argc>1 && arguments[1]=="man"){
        return ParseEngine::Manual::runCommandLine(argc,argv);
    }
    CommandLine options;
    if (!parseArguments(argc, argv, options))
    {
        return 2;
    }
    if (options.help || argc <= 1)
    {
        printUsage(argv[0]);
        return 0;
    }

    std::string commandMode = options.mode.value_or("");
    std::transform(commandMode.begin(), commandMode.end(), commandMode.begin(),
                   [](unsigned char ch)
                   { return static_cast<char>(std::tolower(ch)); });
    if (commandMode == "clear")
    {
        if (options.grammarFile.has_value())
        {
            std::cerr << PE_TEXT("错误：clear 模式不读取文法文件；请只指定 --out-dir\n");
            return 2;
        }
        ParseEngine::Error clearError;
        std::size_t removed = 0u;
        if (!ParseEngine::Project::clearParserProject(
                options.outDir, removed, clearError))
        {
            printError(clearError);
            return 1;
        }
        std::cout << PE_TEXT("清理完成，共删除 ") << removed << PE_TEXT(" 个生成文件\n");
        return 0;
    }

    // 主流程不再用 try/catch：所有可能失败的下层函数都改成「bool 返回 +
    // Error 出参」，这里只在每个调用点检查一次并统一打印。
    ParseEngine::Error error;
    {
        using namespace ParseEngine;
        // [调试] 只跑条件编译预处理：打印处理后的文本与行号映射，不生成产物
        if (options.preprocessOnly)
        {
            std::string rawText = GrammarLoader::defaultLoadText();
            if (options.grammarFile.has_value() &&
                !readGrammarText(*options.grammarFile, rawText, error))
            {
                printError(error);
                return 1;
            }
            PreprocessResult result;
            if (!Preprocessor::run(rawText, options.defines, result, error))
            {
                printError(error);
                return 1;
            }
            std::cout << PE_TEXT("[预处理后文本]\n") << result.text;
            if (result.text.empty() || (result.text.back() != '\n'))
            {
                std::cout << "\n";
            }
            std::cout << PE_TEXT("[行号映射]\n");
            for (std::size_t index = 0u; index < result.lineMap.size(); ++index)
            {
                std::cout << (index + 1u) << " " << result.lineMap[index]
                          << "\n";
            }
            return 0;
        }
        GrammarConfig cfg;
        const bool loaded =
            options.grammarFile.has_value()
                ? GrammarLoader::loadGrammarFile(*options.grammarFile,
                                                 options.defines, cfg, error)
                : GrammarLoader::loadGrammarText(
                      GrammarLoader::defaultLoadText(), options.defines, cfg,
                      error);
        if (!loaded)
        {
            printError(error);
            return 1;
        }
        // [调试] 只看两个段被收集成什么样（不生成产物）
        if (options.dumpSections)
        {
            std::cout << "[%epilogue]"
                      << (cfg.hasEpilogue ? PE_TEXT(" 有") : PE_TEXT(" 无")) << "\n";
            if (cfg.hasEpilogue)
            {
                std::cout << cfg.epilogue;
                if (cfg.epilogue.empty() || (cfg.epilogue.back() != '\n'))
                {
                    std::cout << "\n";
                }
            }
            std::cout << PE_TEXT("[%main] 共 ") << cfg.mainBlocks.size() << PE_TEXT(" 段\n");
            for (std::size_t index = 0u; index < cfg.mainBlocks.size(); ++index)
            {
                const std::size_t line =
                    (index < cfg.mainBlockLines.size()) ? cfg.mainBlockLines[index]
                                                        : 0u;
                std::cout << "--- %main #" << (index + 1u) << PE_TEXT("（第 ") << line
                          << PE_TEXT(" 行）---\n")
                          << cfg.mainBlocks[index];
                if (cfg.mainBlocks[index].empty() ||
                    (cfg.mainBlocks[index].back() != '\n'))
                {
                    std::cout << "\n";
                }
            }
            // #line 用到的行号（动作 / %code / %union），便于核对
            std::cout << "[%union] ";
            if (cfg.unionLine > 0u)
            {
                std::cout << PE_TEXT("第 ") << cfg.unionLine << PE_TEXT(" 行\n");
            }
            else
            {
                std::cout << PE_TEXT("无\n");
            }
            std::cout << PE_TEXT("[%code] 共 ") << cfg.userBlockLines.size()
                      << PE_TEXT(" 块:");
            for (const std::size_t line : cfg.userBlockLines)
            {
                std::cout << " " << line;
            }
            std::cout << "\n";
            std::cout << PE_TEXT("[动作行号]\n");
            for (const auto &rule : cfg.grammar.rules)
            {
                for (std::size_t index = 0u; index < rule.second.size(); ++index)
                {
                    const Production &production = rule.second[index];
                    if (production.actionLine == 0u)
                    {
                        continue;
                    }
                    std::cout << "  " << rule.first << "[" << index << PE_TEXT("] 第 ")
                              << production.actionLine << PE_TEXT(" 行\n");
                }
            }
            return 0;
        }

        // 编辑器解析预览只需要词法结果和具体语法树。清掉所有会进入目标代码的
        // 用户片段与语义类型，让同一份 C/C++/C#/Rust 文法都能生成纯 Python
        // 预览器；产生式、优先级、正则和错误恢复规则保持不变。
        if (options.syntaxOnly)
        {
            cfg.tree = true;
            cfg.cppOnly = false;
            cfg.userCode.clear();
            cfg.userBlocks.clear();
            cfg.userBlockLines.clear();
            cfg.hasEpilogue = false;
            cfg.epilogue.clear();
            cfg.epilogueLine = 0u;
            cfg.mainBlocks.clear();
            cfg.mainBlockLines.clear();
            cfg.hasClassBody = false;
            cfg.classBody.clear();
            cfg.classBodyLine = 0u;
            cfg.packageName.clear();
            cfg.packageDeclared = false;
            cfg.imports.clear();
            cfg.importLines.clear();
            cfg.destructors.clear();
            cfg.destructorLines.clear();
            cfg.valueType.reset();
            cfg.hasUnion = false;
            cfg.unionBody.clear();
            cfg.unionMembers.clear();
            cfg.symbolValueMember.clear();
            cfg.copies.clear();
            for (auto &rule : cfg.grammar.rules)
            {
                for (Production &production : rule.second)
                {
                    production.action.clear();
                    production.actionLine = 0u;
                }
            }
        }
        const Grammar &grammar = cfg.grammar;
        const std::vector<std::string> &tokens = cfg.tokens;

        // 模式策略：命令行优先。--mode 与 .gram 里的 %mode 不一致时用 --mode；
        // 文法没写 %mode 时以命令行为准（缺省仍是 ll1）。
        std::string mode = cfg.mode.empty() ? "ll1" : cfg.mode;
        int modeLookahead = -1;
        if (options.mode.has_value())
        {
            // 命令行 --mode 也接受 LL(1) / LL(2) / LALR(k) 这类写法
            std::string normalized;
            int inlineK = -1;
            if (!normalizeModeName(*options.mode, normalized, inlineK))
            {
                std::cerr << PE_TEXT("错误：未知的生成模式 ") << *options.mode
                          << PE_TEXT("（支持 lex / ll0 / ll1 / llk / lr0 / slr / "
                             "slrk / lalr / lalrk / lr1 / lrk / ielr / glr / "
                             "earley / cyk / dd，也接受 LL(1) / LL(2) / LALR(k) 这种带 k "
                             "的写法）\n");
                return 2;
            }
            mode = normalized;
            modeLookahead = inlineK;
        }
        if ((mode != "ll1") && (mode != "llk") && (mode != "ll0") &&
            !isLrMode(mode) && !isLexMode(mode) && !isDdMode(mode))
        {
            std::cerr << PE_TEXT("错误：未知的生成模式 ") << mode
                      << PE_TEXT("（只支持 lex / ll1 / lr0 / slr / lalr / lr1 / "
                         "ielr / glr / earley / cyk / dd / lrk / lalrk / slrk / llk）\n");
            return 2;
        }
        // %mode lex：只导出词法器。解析相关的东西（产生式、%start / %tree /
        // %sync / %destructor 等）不参与产物，提醒一句别让人以为
        // 「写了就会生效」。
        if (isLexMode(mode))
        {
            std::size_t ruleCount = 0u;
            for (const auto &rule : cfg.grammar.rules)
            {
                ruleCount += rule.second.size();
            }
            std::string ignored;
            const auto addIgnored = [&ignored](const std::string &what)
            {
                if (!ignored.empty())
                {
                    ignored += "、";
                }
                ignored += what;
            };
            if (ruleCount > 0u)
            {
                addIgnored(PE_TEXT("产生式 ") + std::to_string(ruleCount) + PE_TEXT(" 条"));
            }
            if (cfg.tree)
            {
                addIgnored("%tree");
            }
            if (cfg.usesSync)
            {
                addIgnored("%sync");
            }
            if (!cfg.destructors.empty())
            {
                addIgnored("%destructor");
            }
            if (cfg.errorContextDeclared)
            {
                addIgnored(PE_TEXT("%error_context（词法产物的错误没有上下文行）"));
            }
            if (!ignored.empty())
            {
                std::cerr << PE_TEXT("提示：%mode lex 只导出词法器，已忽略：") << ignored
                          << PE_TEXT("（产物里没有解析器；要解析请换成别的 --mode）\n");
            }
        }
        // LR(k) / LALR(k) 的 k：命令行 --lookahead 覆盖 %lookahead，缺省 2。
        // 只有 lrk / lalrk 用得上它，别的模式（LL(1) 是自顶向下、其余 LR
        // 模式固定 k = 1）写了就报错，不静默忽略。
        const bool lookaheadMode = (mode == "lrk") || (mode == "lalrk") ||
                                   (mode == "slrk") || (mode == "llk") ||
                                   (mode == "ll0");
        // 只有 lrk / lalrk 用 k；其余模式一律 k = 1（别的取值上面已经报错了）
        int lookaheadK = 1;
        if (lookaheadMode)
        {
            if (options.lookahead.has_value())
            {
                lookaheadK = *options.lookahead;
            }
            else if (modeLookahead >= 0)
            {
                lookaheadK = modeLookahead;
            }
            else
            {
                lookaheadK = cfg.lookaheadDeclared ? cfg.lookahead : 2;
            }
        }
        if (mode == "ll0")
        {
            lookaheadK = 0; // ll0 就是「不看 lookahead」，写别的值没意义
        }
        // k 没有策略上限（0..INT_MAX 都收），但两件事得说清楚：
        // ① 列编码是 Σ Token号 × Token个数^i，T^k 放不进 64 位时产物会算错，
        //    所以在生成期拦一下并给出这个文法的上限；
        // ② k 一大 FIRST_k / FOLLOW_k 与列数就指数级膨胀，提醒一句（不拦）。
        if (lookaheadMode)
        {
            const int encodingMax = lookaheadEncodingMaxK(tokens.size());
            if (lookaheadK > encodingMax)
            {
                std::cerr << PE_TEXT("错误：k = ") << lookaheadK << PE_TEXT(" 太大——")
                          << tokens.size()
                          << PE_TEXT(" 个 Token 的 k 元组列编码放不进 64 位（这个文法"
                             "最多 k = ")
                          << encodingMax << "）\n";
                return 2;
            }
        }
        if (!isLexMode(mode) && !lookaheadMode &&
            (options.lookahead.has_value() || cfg.lookaheadDeclared))
        {
            std::cerr << PE_TEXT("错误：展望符长度（%lookahead / --lookahead）只对 "
                         "lrk / lalrk / slrk / llk 有意义，当前模式是 ")
                      << modeLabelText(mode)
                      << PE_TEXT("：要按 k 元组建表请用 --mode lrk / lalrk / slrk / "
                         "llk\n");
            return 2;
        }

        // error 产生式 / %sync 的模式限制要按「实际生效的模式」判：文法没写
        // %mode 时命令行 --mode 说了算，所以文法层只拦得住自己声明过模式的
        // 情况，这里补上剩下的（ll1 不能用 error）。
        if (cfg.usesError && (mode == "ll1"))
        {
            fail(error,
                 PE_TEXT("当前模式是 LL(1)，不支持 error 产生式：自顶向下分析没有"
                 "「弹栈到最近的恢复点」这个动作，请改用 --mode lr0 / slr / "
                 "lalr / lr1 / ielr，或把恢复改成 %sync SEMI 这种同步 Token"));
            printError(error);
            return 1;
        }
        // DD 是「带回溯的递归下降」：既没有 LR 的弹栈恢复（error 产生式没有
        // 落点），也没有「丢弃栈上语义值」这一层（%destructor 没有落点）。
        // 文法自己写了 %mode 时 grammar_loader 已经拦过，这里补上「模式由
        // 命令行决定」的那一半。
        if (isDdMode(mode) && cfg.usesError)
        {
            fail(error,
                 PE_TEXT("当前模式是 DD（递归下降），不支持 error 产生式：递归下降是"
                 "「照产生式右部往下调用」，没有「弹栈到最近的恢复点」这个"
                 "动作；请改用 --mode lalr / ielr，或把恢复改成 %sync SEMI "
                 "这种同步 Token（DD 下 %sync 是允许的）"));
            printError(error);
            return 1;
        }
        if (isDdMode(mode) && !cfg.destructors.empty())
        {
            fail(error,
                 PE_TEXT("当前模式是 DD（递归下降），不支持 %destructor：回溯式解析会"
                 "把同一条产生式试很多遍，哪些值被丢弃、丢几次都没有确定答案；"
                 "请改用 --mode lalr / ielr"));
            printError(error);
            return 1;
        }
        // 目标语言策略同样命令行优先：--lang 覆盖 %lang，缺省 cpp17。
        std::string lang = cfg.lang.empty() ? "cpp17" : cfg.lang;
        if (options.lang.has_value())
        {
            lang = *options.lang;
        }
        if ((lang != "c") && (lang != "cpp17") && (lang != "python") &&
            (lang != "csharp") && (lang != "rust") && (lang != "go") &&
            (lang != "java") && (lang != "js") && (lang != "gram"))
        {
            std::cerr << PE_TEXT("错误：未知的目标语言 ") << lang
                      << PE_TEXT("（只支持 c / cpp17 / python / csharp / rust / go / "
                         "java / js / gram）\n");
            return 2;
        }
        // 生成器自带的正则编译器（source/src/regex_dfa.cpp）没认下的正则：只有 C99
        // 真的要拿它编 DFA 表，所以那边与从前一样是硬错误（报错文案逐字不变）；
        // 其余档的正则原样交给各自语言自带的正则库，只警告、照常往下生成。
        if (!cfg.regexDiagnostics.empty())
        {
            if (lang == "c")
            {
                printError(cfg.regexDiagnostics.front());
                return 1;
            }
            for (const ParseEngine::Error &item : cfg.regexDiagnostics)
            {
                std::cerr << PE_TEXT("警告：");
                if (item.line > 0u)
                {
                    std::cerr << PE_TEXT("第 ") << item.line << PE_TEXT(" 行：");
                }
                std::cerr << item.message << "\n";
            }
            std::cerr << PE_TEXT("      以上是生成器自带的正则编译器（C99 用来编表的那一个）"
                         "给的判断：\n"
                         "      ")
                      << lang
                      << PE_TEXT(" 的产物把正则原样交给该语言自带的正则库（C++ 的 "
                         "std::regex / Python 的 re / C# 的 Regex / Rust 的 "
                         "regex / Java 的 Pattern / JS 的 RegExp），\n"
                         "      它不认识不等于目标语言不认——若你对该语言的正则语法"
                         "十分了解、确认这条正则没问题，可以忽略本警告。\n");
        }
        // --lang gram：只导出「文法原文件 + 分析报告」，没有解析器 / 词法器
        // 代码，所以后面那些「某个产物的专属写法」都不适用（全部按现有的
        // 「指令不适用」方式提示一句并忽略）。
        const bool gramTarget = (lang == "gram");
        // %cpp_only（文法层声明）只拦命令行显式要 C 的情况：命令行 --lang c
        // 覆盖不掉它，否则这条指令就没意义了。
        if (cfg.cppOnly && (lang != "cpp17") && !gramTarget)
        {
            fail(error,
                 PE_TEXT("这份文法写了 %cpp_only：只能生成 C++17 产物，不能用 "
                 "--lang ") + lang + PE_TEXT("；要么换目标语言，要么删掉文法里的 "
                 "%cpp_only"));
            printError(error);
            return 1;
        }
        if (cfg.cppOnly && gramTarget)
        {
            std::cerr << PE_TEXT("提示：%cpp_only 约束的是代码生成，--lang gram 只导出"
                         "文法与分析报告，已忽略\n");
        }

        // Python 产物没有 C 系列的语义值模型：这些写法一律直接报错（不静默
        // 忽略），错误信息在生成阶段之前就给，免得用户先看到动作翻译器的
        // 报错而摸不着头脑。（%code … %endcode 是支持的：正文会
        // 归一化后插在 parser.py 的 import 之后。）
        if (lang == "python")
        {
            const std::string pyReject =
                PE_TEXT("Python 产物不支持 ");
            if (cfg.valueType.has_value())
            {
                fail(error, pyReject +
                                PE_TEXT("%value_type：Python 的语义值是任意对象"
                                "（词法器直接给出 str / int / float），"
                                "不需要声明类型"));
                printError(error);
                return 1;
            }
            if (cfg.hasUnion)
            {
                fail(error, pyReject +
                                PE_TEXT("%union / %token <成员>：Python 的语义值是"
                                "任意对象，没有联合体成员这一层"));
                printError(error);
                return 1;
            }
        }

        // %namespace / --namespace 同样是 C++17 专属：C 产物没有命名空间，
        // 文件名也不带前缀，静默忽略只会让人以为生效了。
        const std::string nameSpace =
            options.cppNamespace.value_or(cfg.cppNamespace);
        if ((lang == "c") && !nameSpace.empty())
        {
            fail(error,
                 PE_TEXT("命名空间（%namespace / --namespace ") + nameSpace +
                     PE_TEXT("）只对 C++17 产物有效：C 产物没有命名空间，文件名也"
                     "不带前缀；要么去掉 --lang c，要么删掉命名空间设置"));
            printError(error);
            return 1;
        }
        // %class / --class 反过来只对 Python 产物有效：C / C++17 的类名是
        // 固定的（Engine），改名要用 %namespace / --namespace。
        const std::string pyClass =
            options.pythonClass.value_or(cfg.pythonClass);
        if ((lang != "python") && !gramTarget && !pyClass.empty())
        {
            fail(error,
                 PE_TEXT("解析器类名（%class / --class ") + pyClass +
                     PE_TEXT("）只对 Python 产物有效：C / C++17 产物的接口是固定"
                     "名字，要改命名空间请用 %namespace / --namespace"));
            printError(error);
            return 1;
        }
        if (gramTarget && !pyClass.empty())
        {
            std::cerr << PE_TEXT("提示：解析器类名（%class / --class ") << pyClass
                      << PE_TEXT("）只对 Python 产物有效，--lang gram 下已忽略\n");
        }

        // %package / %import / %class_body 是「csharp / rust / go 后端」专属
        // 指令：别的目标语言下按现有的「指令不适用」方式提示一句并忽略
        // （与「教学模式只对……有意义，……下已忽略」同一套口径）。
        {
            const bool pkgLike = (lang == "csharp") || (lang == "rust") ||
                                 (lang == "go");
            std::string ignored;
            if (cfg.packageDeclared)
            {
                ignored += "%package";
            }
            if (!cfg.imports.empty())
            {
                ignored += (ignored.empty() ? "" : " / ");
                ignored += "%import";
            }
            if (cfg.hasClassBody)
            {
                ignored += (ignored.empty() ? "" : " / ");
                ignored += "%class_body";
            }
            if (!pkgLike && !ignored.empty())
            {
                std::cerr << PE_TEXT("提示：") << ignored
                          << PE_TEXT(" 只对 --lang csharp / rust / go 有意义，")
                          << lang << PE_TEXT(" 下已忽略\n");
            }
            if ((pkgLike || gramTarget) && !nameSpace.empty())
            {
                std::cerr << PE_TEXT("提示：命名空间（%namespace / --namespace ")
                          << nameSpace
                          << PE_TEXT("）只对 C++17 产物有效，") << lang
                          << PE_TEXT(" 下已忽略（请改用 %package）\n");
            }
            // %destructor 靠 C 的「手工释放」语义：C# / Rust 由 GC / 所有权
            // 自动回收，没有落点，直接报错（不静默忽略）
            if (pkgLike && !cfg.destructors.empty())
            {
                fail(error,
                     PE_TEXT("%destructor 只对 C / C++17 产物有效：") + lang +
                         ((lang == "rust")
                              ? PE_TEXT(" 的语义值按值拷贝、由所有权回收，"
                                "不需要手工释放（这条边界写在 README 与 "
                                "doc/07-codegen.md 里）")
                              : PE_TEXT(" 的语义值靠 GC 回收，不需要手工释放（这条"
                                "边界写在 README 与 doc/07-codegen.md 里）")));
                printError(error);
                return 1;
            }
            // --lang gram 不生成代码：%destructor 没有落点，提示一句并忽略
            if (gramTarget && !cfg.destructors.empty())
            {
                std::cerr << PE_TEXT("提示：%destructor 只对 C / C++17 产物有效，"
                             "--lang gram 下已忽略\n");
            }
        }

        // 教学模式策略：命令行优先（--teach / --no-teach 覆盖 %teach），
        // 缺省关闭——不开教学模式时 slr / lalr / ielr 对未消解冲突报错，
        // 报告里也不会出现教学小节（lr0 / lr1 本来就不中止）。
        bool teach = cfg.teach;
        if (options.teach.has_value())
        {
            teach = *options.teach;
        }
        // 教学模式只对确定性 LR 模式有意义：LL(1) 没有冲突可言，
        // 按未开启处理并说明一声。
        if (teach && (mode == "ll1"))
        {
            std::cerr << PE_TEXT("提示：教学模式只对 lr0 / slr / lalr / lr1 / ielr "
                         "有意义，")
                      << modeLabelText(mode) << PE_TEXT(" 下已忽略\n");
            teach = false;
        }

        // 状态机图示的开关（命令行优先，覆盖 %mermaid）；它和教学模式无关，
        // LL(1) 下没有 LR 状态机可画，但词法 DFA 一样能画。
        bool mermaid = cfg.mermaid;
        if (options.mermaid.has_value())
        {
            mermaid = *options.mermaid;
        }
        // 工程构建文件的开关：命令行优先（--build-files / --no-build-files
        // 覆盖 %build_files）；关闭时只写目标语言源码与 RESULT.md，构建工程
        // 由用户自己维护。生成阶段过滤 + 写盘阶段不删用户已有的构建文件。
        bool buildFiles = cfg.buildFiles;
        if (options.buildFiles.has_value())
        {
            buildFiles = *options.buildFiles;
        }

        const PriorityConfig priority = GrammarLoader::priorityFromConfig(cfg);
        const LexSpec lexSpec = GrammarLoader::lexSpecFromConfig(cfg);

        AnalysisResult analysis;
        std::string report;
        bool analyzed = true;
        if (isLexMode(mode))
        {
            // 词法模式没有分析层：不建表、不算 FIRST/FOLLOW、不做冲突检查，
            // 产物里只有词法器（详见 project.cpp 的 generateLexProject）
            report = gramTarget
                         ? "词法模式（%mode lex）：只做词法分析（Token 表 / "
                           "最长匹配），不生成解析器\n"
                         : "词法模式（%mode lex）：只导出词法器，不生成解析器\n";
        }
        else if (isGlrMode(mode))
        {
            analyzed = Analysis::analyze(grammar, tokens, mode, &priority,
                                         teach, analysis, error);
            if (analyzed && ((mode == "earley") || (mode == "cyk")))
            {
                const ChartGrammar chart = buildChartGrammar(analysis);
                report = "[生成模式] " + modeLabelText(mode) + " 图表解析\n";
                if (mode == "earley")
                {
                    report += "[图表] 对原文法做预测、扫描与完成；产生式 " +
                              std::to_string(chart.lhs.size()) + " 条\n";
                }
                else
                {
                    report += "[CNF] 生成期消除空产生式与单元规则并二分长规则：终结符规则 " +
                              std::to_string(chart.terminalRules.size()) +
                              " 条，二元规则 " +
                              std::to_string(chart.binaryRules.size()) +
                              " 条；空串" +
                              (chart.acceptsEmpty ? "可接受" : "不可接受") + "\n";
                }
                if ((lang == "java") || (lang == "js"))
                {
                    report += "[结果] Earley 图表保留第一棵派生树；窄后端不执行语义动作\n";
                }
                else
                {
                    report += "[结果] 图表判定后复用 GLR 运行时重建歧义树与执行原语义动作\n";
                }
            }
        }
        else if (mode == "dd")
        {
            analyzed = Analysis::ddAnalyze(grammar, tokens, &priority, analysis,
                                           error);
            report = formatDdReport(analysis);
            // 递归下降是「按产生式声明顺序逐条试」，不存在「同一格里有几个
            // 动作要挑一个」的场合，所以优先级指令与 %expect 在这里都没有
            // 落点——按 LL(1) 那套口径提示一句（不静默忽略）
            if (!priority.precedence.empty() ||
                priority.expectConflicts.has_value())
            {
                report +=
                    "\n[提示] DD（递归下降）忽略 %left / %right / %nonassoc / "
                    "%prec 优先级指令与 %expect：回溯式解析按产生式的声明顺序"
                    "逐条试，没有「同一格里有几个动作要挑一个」的场合；要控制"
                    "歧义文法的取法，请把想要的产生式写在前面\n";
            }
        }
        else if (mode == "ielr")
        {
            analyzed = Analysis::ielrAnalyze(grammar, tokens, &priority, teach,
                                             analysis, error);
            report = formatIelrReport(analysis);
        }
        else if (mode == "lalr")
        {
            analyzed = Analysis::lalrAnalyze(grammar, tokens, &priority, teach,
                                             analysis, error);
            report = formatLalrReport(analysis);
        }
        else if (mode == "lrk")
        {
            analyzed = Analysis::lrkAnalyze(grammar, tokens, lookaheadK,
                                            &priority, teach, analysis, error);
            report = formatLrkReport(analysis);
        }
        else if (mode == "lalrk")
        {
            analyzed = Analysis::lalrkAnalyze(grammar, tokens, lookaheadK,
                                              &priority, teach, analysis, error);
            report = formatLalrkReport(analysis);
        }
        else if (mode == "slrk")
        {
            analyzed = Analysis::slrkAnalyze(grammar, tokens, lookaheadK,
                                             &priority, teach, analysis, error);
            report = formatSlrkReport(analysis);
        }
        else if (mode == "llk")
        {
            analyzed = Analysis::llkAnalyze(grammar, tokens, lookaheadK, teach,
                                            analysis, error);
            report = formatLlkReport(analysis);
        }
        else if (mode == "ll0")
        {
            analyzed = Analysis::llkAnalyze(grammar, tokens, 0, teach, analysis,
                                            error);
            report = formatLlkReport(analysis);
        }
        else if (mode == "lr1")
        {
            analyzed = Analysis::lr1Analyze(grammar, tokens, &priority, teach,
                                            analysis, error);
            report = formatLr1Report(analysis);
        }
        else if (mode == "slr")
        {
            analyzed = Analysis::slrAnalyze(grammar, tokens, &priority, teach,
                                            analysis, error);
            report = formatSlrReport(analysis);
        }
        else if (mode == "lr0")
        {
            analyzed = Analysis::lr0Analyze(grammar, tokens, &priority, teach,
                                            analysis, error);
            report = formatLr0Report(analysis);
        }
        else
        {
            analyzed = Analysis::prepareGrammar(grammar, tokens, analysis,
                                                error);
            report = formatGrammarReport(analysis);
            if (!priority.precedence.empty() ||
                priority.expectConflicts.has_value())
            {
                report +=
                    "\n[提示] LL(1) 模式忽略 %left / %right / %nonassoc / "
                    "%prec 优先级指令与 %expect\n";
            }
        }
        if (!analyzed)
        {
            printError(error);
            return 1;
        }
        // %sync 不进分析层（它不参与项集与 ACTION 表），但报告里单列一节，
        // 让人一眼看到运行时会在哪些 Token 上做 panic mode 恢复
        if (!isLexMode(mode) && !cfg.syncTokens.empty())
        {
            report += "\n[同步 Token]\n%sync 恢复锚点（运行时按声明顺序查）:";
            for (const std::string &token : cfg.syncTokens)
            {
                report += " " + token;
            }
            report += "\n";
        }
        // 报告全文不往控制台倒：它写在产物目录的生成结果文档里（--report
        // 可以另存一份），控制台只保留下面这些提示与最后那一行
        // 生成期警告：error 列上的冲突（按 yacc 惯例取移进优先，不中止生成）
        for (const std::string &message : analysis.warnings)
        {
            std::cerr << PE_TEXT("警告：") << message << "\n";
        }
        // %union 下没给 NUMBER 绑成员时，内建数字词法解析出的数值没地方放
        // （ParserValue_T 是联合体，见 code_gen.cpp 的 customizeLexerText），
        // 产物能编译但数字 Token 的语义值恒为零值；说一声免得对着零值查半天。
        // 只有「NUMBER 走内建数字词法」时才提：NUMBER 被 %token NUMBER "..."
        // / %token regex NUMBER 改写过时，它是别的 Token，不适用这句提示。
        const bool numberFromBuiltin =
            !cfg.tokenDefs.count("NUMBER") &&
            !lexSpec.regex.count("NUMBER");
        // NUMBER 是内建 Token（没写也在终结符表里），所以只看它有没有真的
        // 出现在某条产生式的右部
        const bool usesNumber =
            std::any_of(analysis.productions.begin(),
                        analysis.productions.end(),
                        [](const Production &production)
                        {
                            return std::find(production.rhs.begin(),
                                             production.rhs.end(),
                                             "NUMBER") !=
                                   production.rhs.end();
                        });
        if (cfg.hasUnion && numberFromBuiltin && usesNumber &&
            !cfg.symbolValueMember.count("NUMBER"))
        {
            std::cerr << PE_TEXT("警告：%union 下 NUMBER 没有绑语义值成员，数字 Token "
                         "的语义值恒为零值；要用它的值请写 %token <成员> "
                         "NUMBER\n");
        }
        // 冲突不中止生成时（lr0 / lr1 无条件，其余 LR 模式在教学模式下），
        // 在终端上明确提醒一声，别让人以为产物是「干净」的
        const bool allowConflicts =
            (mode == "lr0") || (mode == "lr1") ||
            (teach && isLrMode(mode));
        if (allowConflicts && !analysis.unresolved.empty())
        {
            std::cerr << PE_TEXT("警告：该文法不是 ") << modeLabelText(mode)
                      << PE_TEXT(" 文法，表里有 ") << analysis.unresolved.size()
                      << PE_TEXT(" 处未消解冲突；已按移进优先写表继续生成")
                      << (teach ? PE_TEXT("（详见报告里的「教学提示」）")
                                : PE_TEXT("（详见报告里的「未消解冲突」）"))
                      << "\n";
        }
        if (options.report.has_value())
        {
            std::ofstream handle(std::filesystem::u8path(*options.report),
                                 std::ios::binary | std::ios::trunc);
            if (!handle)
            {
                std::cerr << PE_TEXT("错误：无法写入报告文件 ")
                          << *options.report << "\n";
                return 2;
            }
            handle.write(report.data(),
                         static_cast<std::streamsize>(report.size()));
        }
        if (options.dumpLrAutomaton || options.dumpSlrTables)
        {
            if (isLexMode(mode))
            {
                std::cerr << PE_TEXT("错误：--dump-lr-automaton / --dump-slr-tables "
                             "需要语法分析，%mode lex 只导出词法器、没有表可看\n");
                return 2;
            }
            if (!isLrMode(mode))
            {
                std::cerr << PE_TEXT("错误：--dump-lr-automaton / --dump-slr-tables "
                             "只在 --mode lr0 / slr / lalr / lr1 / ielr / "
                             "lrk / lalrk "
                             "下可用\n");
                return 2;
            }
            if (options.dumpLrAutomaton)
            {
                if (mode == "lr1")
                {
                    std::cout << formatLr1Automaton(analysis);
                }
                else if (mode == "lrk")
                {
                    std::cout << formatLrkAutomaton(analysis);
                }
                else if (mode == "lalrk")
                {
                    std::cout << formatLalrkAutomaton(analysis);
                }
                else if (mode == "slrk")
                {
                    std::cout << formatSlrkAutomaton(analysis);
                }
                else if (mode == "lalr")
                {
                    std::cout << formatLalrAutomaton(analysis);
                }
                else if (mode == "ielr")
                {
                    std::cout << formatIelrAutomaton(analysis);
                }
                else if (mode == "lr0")
                {
                    std::cout << formatLr0Automaton(analysis);
                }
                else
                {
                    std::cout << formatSlrAutomaton(analysis);
                }
            }
            if (options.dumpSlrTables)
            {
                if (mode == "lr1")
                {
                    std::cout << formatLr1Tables(analysis);
                }
                else if ((mode == "lrk") || (mode == "lalrk"))
                {
                    std::cout << formatLrkTables(analysis);
                }
                else if (mode == "slrk")
                {
                    std::cout << formatLrkTables(analysis);
                }
                else if (mode == "ielr")
                {
                    std::cout << formatIelrTables(analysis);
                }
                else if (mode == "lalr")
                {
                    std::cout << formatLalrTables(analysis);
                }
                else if (mode == "lr0")
                {
                    std::cout << formatLr0Tables(analysis);
                }
                else
                {
                    std::cout << formatSlrTables(analysis);
                }
            }
        }

        if (options.dumpMermaid)
        {
            // lex 模式没有自动机图，只画词法 DFA
            std::vector<MermaidDiagram> diagrams =
                isLexMode(mode) ? std::vector<MermaidDiagram>()
                                : Mermaid::grammarDiagrams(analysis);
            const std::vector<MermaidDiagram> lexer =
                Mermaid::lexerDiagrams(lexSpec, error);
            if (!error.empty())
            {
                printError(error);
                return 1;
            }
            diagrams.insert(diagrams.end(), lexer.begin(), lexer.end());
            if (diagrams.empty())
            {
                std::cerr << PE_TEXT("提示：") << modeLabelText(mode) << PE_TEXT(" 没有可画的"
                             "状态机（LL(1) 的预测分析表不是状态机），文法里"
                             "也没有 %token regex / %identifier 的 DFA\n");
            }
            std::cout << Mermaid::markdownFences(diagrams);
        }

        ProjectRequest request;
        request.grammar = &grammar;
        request.tokens = &tokens;
        request.tokenDefs = &cfg.tokenDefs;
        request.tree = cfg.tree;
        request.maxStackDepth = cfg.maxStackDepth;
        request.maxErrors = cfg.maxErrors;
        request.errorRecoveryTokens = cfg.errorRecoveryTokens;
        request.errorContext = cfg.errorContext;
        request.destructors = &cfg.destructors;
        request.hasEpilogue = cfg.hasEpilogue;
        request.epilogue = &cfg.epilogue;
        request.spaceChars = cfg.spaceDeclared ? &cfg.spaceChars : nullptr;
        request.mainBlocks = &cfg.mainBlocks;
        request.mainBlockLines = &cfg.mainBlockLines;
        request.mode = mode;
        request.lookaheadK = lookaheadK;
        request.lang = lang;
        request.exampleInput = cfg.exampleInput;
        // 命令行 --namespace 覆盖 %namespace；都没有就是空串（退化成现状）
        request.cppNamespace =
            options.cppNamespace.value_or(cfg.cppNamespace);
        request.cppLexer = options.cppLexer.value_or(cfg.cppLexer);
        if (lang != "cpp17")
        {
            // 非 C++17 目标根本不用这个字段；只在用户**显式**指定过时提一句，
            // 免得缺省的 dfa 让每种语言都刷一条提示。
            if (options.cppLexer.has_value() || cfg.cppLexerDeclared)
            {
                std::cerr << PE_TEXT("提示：%cpp_lexer / --cpp-lexer 只对 --lang cpp17 "
                                     "有意义，")
                          << lang << PE_TEXT(" 下已忽略\n");
            }
            request.cppLexer = "dfa";
        }
        request.cPrefix = options.cPrefix.value_or(cfg.cPrefix);
        if (!request.cPrefix.empty() && lang != "c")
        {
            std::cerr << PE_TEXT("提示：%prefix / --prefix 只对 --lang c 有意义，") << lang
                      << PE_TEXT(" 下已忽略\n");
            request.cPrefix.clear();
        }
        // 命令行 --class 覆盖 %class；都没有就是空串（缺省类名 Parser）
        request.pythonClass = options.pythonClass.value_or(cfg.pythonClass);
        // %package / %import / %class_body：只对 --lang csharp / rust / go 生效
        request.packageName = cfg.packageName;
        request.imports = &cfg.imports;
        request.classBody = &cfg.classBody;
        request.classBodyLine = cfg.classBodyLine;
        request.sourceName = options.grammarFile.value_or("");
        // #line 的行号信息（.gram 路径 + 各段的起始行）—— 必须在
        // request.sourceName 赋好之后建
        ProjectRequest::LineInfo lineInfo;
        lineInfo.grammarPath = request.sourceName;
        lineInfo.unionLine = cfg.unionLine;
        lineInfo.epilogueLine = cfg.epilogueLine;
        lineInfo.userBlockLines = &cfg.userBlockLines;
        lineInfo.userBlocks = &cfg.userBlocks;
        lineInfo.mainLines = &cfg.mainBlockLines;
        request.lines = &lineInfo;
        request.reportText = report;
        // RESULT.md 默认是简化版（到「未消解冲突」为止）；--full-report 才把
        // 分析报告全文也钉进文档末尾。报告本身照旧生成：--report 要写文件、
        // 教学提示的终端提醒也看它。
        request.fullReport = options.fullReport;
        // --lang gram：把文法原文件导出到产物目录，所以这里留一份原文
        // （读的是 --grammar-file / 位置参数给的原始文件；没给文件时就是
        // 内建的 default_grammar.gram）。别的目标语言不用这一份。
        std::string grammarSourceText;
        if (gramTarget)
        {
            if (options.grammarFile.has_value())
            {
                if (!readGrammarText(*options.grammarFile, grammarSourceText,
                                     error))
                {
                    printError(error);
                    return 1;
                }
            }
            else
            {
                grammarSourceText = GrammarLoader::defaultLoadText();
            }
            request.grammarText = &grammarSourceText;
        }
        request.priority = &priority;
        request.teach = teach;
        request.mermaid = mermaid;
        request.usesError = cfg.usesError;
        request.syncTokens = &cfg.syncTokens;
        request.lexSpec = &lexSpec;
        request.userCode = cfg.userCode;
        if (cfg.valueType.has_value())
        {
            // %value_type：语义值类型由生成器补 typedef，用户不用自己写
            request.valueType = &(*cfg.valueType);
        }
        if (cfg.hasUnion)
        {
            // %union：语义值类型由生成器追加到 parser_user.h，$n / $$ 按符号成员展开
            request.unionBody = &cfg.unionBody;
            request.unionMembers = &cfg.unionMembers;
            request.symbolValueMember = &cfg.symbolValueMember;
        }
        // 分析表已经为报告算过一次，生成阶段直接复用（lex 模式没有分析表）
        request.analysis = isLexMode(mode) ? nullptr : &analysis;
        request.copies = &cfg.copies;

        std::map<std::string, std::string> project;
        if (!Project::generateParserProject(request, project, error))
        {
            printError(error);
            return 1;
        }
        if (!buildFiles)
        {
            // 只写目标语言源码与 RESULT.md：构建工程文件（Makefile / CMakeLists /
            // Cargo.toml / *.csproj / pom.xml / package.json / go.mod 等）不导出，
            // 用户自己维护；写盘时也不把它们当过期产物删掉。
            for (auto it = project.begin(); it != project.end();)
            {
                if (Project::isBuildFileName(it->first))
                {
                    it = project.erase(it);
                }
                else
                {
                    ++it;
                }
            }
            std::cout << PE_TEXT("已按 --no-build-files / %build_files off 跳过工程构建文件\n");
        }
        std::vector<std::string> written;
        if (!Project::writeParserProject(options.outDir, project, written,
                                         error, !buildFiles))
        {
            printError(error);
            return 1;
        }
        // 控制台只留一行：分析报告与产物清单都写在产物目录的生成结果文档
        // 里，要单独存一份可以加 --report FILE，要看自动机/表用 --dump-*
        std::cout << PE_TEXT("解析完成，解析详情参考 ")
                  << resultDocPath(options.outDir, written) << "\n";
        return 0;
    }
}
