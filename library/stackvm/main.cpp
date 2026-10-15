/**
 * @file main.cpp
 * @brief stackvm 命令行工具：编译、运行、反汇编和检查 `.quad` / `.svmb`。
 *
 * 用法见 `--help`；退出码：0 成功，1 运行失败，2 用法错误。
 */

#include "stackvm.hpp"

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
        << "用法：" << program << " <命令> <file.quad|file.svmb> [选项]\n"
        << "\n"
        << "命令：\n"
        << "  run      组装并执行（默认命令）\n"
        << "  compile  把四元式组装成可直接加载的 .svmb 二进制程序\n"
        << "  disasm   显示组装后的栈指令（支持 .quad / .svmb）\n"
        << "  check    只组装或加载，检查程序是否合法\n"
        << "\n"
        << "选项：\n"
        << "  --input \"1 2\"   read / getc 的输入项（空格或逗号分隔）\n"
        << "  --trace         打印每条执行的指令\n"
        << "  --debug         按源码位置逐行打印执行的指令（需要产物带调试信息）\n"
        << "  --limit N       指令条数上限（缺省不设上限，防死循环可传正整数）\n"
        << "  --no-opt        不做四元式化简（默认会先化简）\n"
        << "  --data FILE     加载数据区二进制（simple_ast 生成的 .data.bin）\n"
        << "  --layout FILE   数据区布局表（simple_ast 的 exportDataLayout 输出）\n"
        << "  --encrypt       compile 时加密载荷区（ChaCha20，密钥见 stackvm_crypto.hpp）\n"
        << "  --out FILE      compile 的输出路径（缺省 <输入>.svmb）\n"
        << "  --stats         打印化简统计\n"
        << "  --expected      把 '成功' 或 '失败' 当输出的比对结果（示例套件用）\n"
        << "  -h, --help      显示这份帮助\n";
}

/**
 * @brief 把输入串切成输入项（空格 / 逗号分隔）。
 * @param text 原文。
 * @return 输入项。
 */
std::vector<std::string> splitInput(const std::string &text)
{
    std::vector<std::string> items;
    std::string current;
    for (const char ch : text)
    {
        if (ch == ' ' || ch == '\t' || ch == ',' || ch == '\n' || ch == '\r')
        {
            if (!current.empty())
            {
                items.push_back(current);
                current.clear();
            }
            continue;
        }
        current += ch;
    }
    if (!current.empty()) items.push_back(current);
    return items;
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
    const std::string program = (argc > 0) ? argv[0] : "stackvm";

    std::string command = "run";
    std::string path;
    std::string inputText;
    std::string dataPath;
    std::string layoutPath;
    std::string outPath;
    bool trace = false;
    bool debug = false;
    bool encryptPayload = false;
    bool expectCheck = false;
    bool optimizeQuads = true;
    bool showStats = false;
    std::size_t limit = 0u; /* 0 = 不设上限 */

    for (int index = 1; index < argc; ++index)
    {
        const std::string argument = argv[index];
        if (argument == "-h" || argument == "--help")
        {
            printUsage(program);
            return 0;
        }
        if (argument == "--trace") { trace = true; continue; }
        if (argument == "--debug") { debug = true; continue; }
        if (argument == "--encrypt") { encryptPayload = true; continue; }
        if (argument == "--expected") { expectCheck = true; continue; }
        if (argument == "--no-opt") { optimizeQuads = false; continue; }
        if (argument == "--stats") { showStats = true; continue; }
        if (argument == "--data")
        {
            if (index + 1 >= argc) { std::cerr << "--data 缺少取值\n"; return 2; }
            dataPath = argv[++index];
            continue;
        }
        if (argument == "--layout")
        {
            if (index + 1 >= argc) { std::cerr << "--layout 缺少取值\n"; return 2; }
            layoutPath = argv[++index];
            continue;
        }
        if (argument == "--out")
        {
            if (index + 1 >= argc) { std::cerr << "--out 缺少取值\n"; return 2; }
            outPath = argv[++index];
            continue;
        }
        if (argument == "--input")
        {
            if (index + 1 >= argc) { std::cerr << "--input 缺少取值\n"; return 2; }
            inputText = argv[++index];
            continue;
        }
        if (argument == "--limit")
        {
            if (index + 1 >= argc) { std::cerr << "--limit 缺少取值\n"; return 2; }
            limit = static_cast<std::size_t>(std::stoul(argv[++index]));
            continue;
        }
        if (!argument.empty() && argument[0] == '-')
        {
            std::cerr << "不认识的选项：" << argument << "\n";
            return 2;
        }
        if (path.empty() && (argument == "run" || argument == "compile" ||
                             argument == "disasm" || argument == "check"))
        {
            command = argument;
            continue;
        }
        if (path.empty()) path = argument;
        else
        {
            std::cerr << "参数太多了：" << argument << "\n";
            return 2;
        }
    }

    if (path.empty())
    {
        printUsage(program);
        return 2;
    }

    const bool binaryInput = stackvm::isProgramBinary(path) ||
                             (path.size() >= stackvm::kProgramBinaryExtension.size() &&
                              path.compare(path.size() - stackvm::kProgramBinaryExtension.size(),
                                           stackvm::kProgramBinaryExtension.size(),
                                           stackvm::kProgramBinaryExtension) == 0);
    if (command == "compile" && binaryInput)
    {
        std::cerr << "compile 的输入应是 .quad；.svmb 已经是组装后的二进制程序\n";
        return 2;
    }

    stackvm::QuadFile file;
    stackvm::Program assembled;
    stackvm::OptimizationReport optimization;
    if (binaryInput)
    {
        if (!optimizeQuads)
        {
            std::cerr << "--no-opt 只对 .quad 或 compile 生效；.svmb 已经完成组装\n";
            return 2;
        }
        if (expectCheck)
        {
            std::cerr << "--expected 依赖 .quad 注释，不能用于 .svmb\n";
            return 2;
        }
        std::vector<std::string> diagnostics;
        if (!stackvm::loadProgramBinary(path, assembled, &diagnostics))
        {
            for (const std::string &message : diagnostics) std::cerr << message << "\n";
            return 1;
        }
    }
    else
    {
        // 1. 读 + 解析四元式。
        file = stackvm::loadQuadFile(path);
        if (!file.ok())
        {
            for (const std::string &message : file.diagnostics) std::cerr << message << "\n";
            return 1;
        }

        // 2. 化简（保守四趟：常量折叠 / 代数化简 / 死分支 / 死代码）。
        if (optimizeQuads)
        {
            optimization = stackvm::optimize(file);
            if (showStats)
            {
                std::cerr << "; 化简：常量折叠 " << optimization.constantsFolded
                          << "，代数化简 " << optimization.algebraicSimplified
                          << "，死分支 " << optimization.deadBranches
                          << "，死代码 " << optimization.deadCode
                          << "（共 " << optimization.rounds << " 轮）\n";
            }
        }

        // 3. 组装。
        const bool assembledOk = stackvm::assemble(file, assembled);
        if (!assembled.diagnostics.empty())
        {
            for (const std::string &message : assembled.diagnostics) std::cerr << message << "\n";
            if (!assembledOk) return 1;
        }
    }

    if (command == "compile")
    {
        if (outPath.empty()) outPath = path + std::string(stackvm::kProgramBinaryExtension);
        std::vector<std::string> diagnostics;
        if (!stackvm::saveProgramBinary(assembled, outPath, &diagnostics,
                                                    encryptPayload))
        {
            for (const std::string &message : diagnostics) std::cerr << message << "\n";
            return 1;
        }
        std::cout << "已生成 " << outPath << "：" << assembled.code.size() << " 条栈指令，"
                  << assembled.labels.size() << " 个标签，" << assembled.types.size()
                  << " 个变量声明，" << assembled.exports.size() << " 个公开入口\n";
        return 0;
    }

    if (command == "disasm")
    {
        std::cout << "; " << (assembled.name.empty() ? path : assembled.name) << "\n";
        for (const std::string &label : assembled.exports)
            std::cout << "; export " << label << "\n";
        for (std::size_t index = 0; index < assembled.code.size(); ++index)
        {
            std::cout << index << ": " << assembled.code[index].toString() << "\n";
        }
        return 0;
    }
    if (command == "check")
    {
        std::cout << "可组装：" << assembled.code.size() << " 条栈指令，"
                  << assembled.labels.size() << " 个标签，"
                  << assembled.types.size() << " 个变量声明，"
                  << assembled.exports.size() << " 个公开入口\n";
        return 0;
    }

    // 4. 执行。
    std::vector<std::string> input = splitInput(
        inputText.empty() ? file.input : inputText);
    stackvm::Machine machine(trace);
    stackvm::DataArea dataArea;
    if (!dataPath.empty())
    {
        std::vector<std::string> problems;
        if (!dataArea.load(dataPath, layoutPath.empty() ? (dataPath + ".layout") : layoutPath,
                           &problems))
        {
            for (const std::string &message : problems) std::cerr << message << "\n";
            return 1;
        }
        machine.setData(&dataArea);
    }
    /*
     * `--debug`：按**源码位置**逐行打印。每执行一条指令都回调，但只在位置变化时输出，
     * 所以看到的是一条条"源代码行"在跑；输出走 stderr，不污染程序自己的 stdout。
     */
    std::string lastLocation;
    if (debug)
    {
        if (assembled.debug.empty())
        {
            std::cerr << "提示：这个程序没有带调试信息，只能显示指令下标\n";
        }
        machine.setSingleStep(true);
        machine.setDebugHook([&lastLocation](const stackvm::DebugEvent &event) {
            const std::string text = event.location.toString();
            if (text != lastLocation)
            {
                lastLocation = text;
                std::cerr << "调试[" << event.steps << "] " << text << "\n";
            }
            return stackvm::DebugAction::StepInto;
        });
    }
    const stackvm::RunResult result = machine.run(assembled, input, limit);

    if (trace)
    {
        for (const std::string &line : result.trace) std::cout << line << "\n";
    }
    if (!result.output.empty()) std::cout << result.output << "\n";
    if (!result.text.empty()) std::cout << result.text << "\n";

    if (!result.ok)
    {
        std::cerr << "运行失败：" << result.error << "\n";
        /* 程序带调试信息（或刚用 --debug 跑过）时，顺手给出源码位置与调用栈。 */
        if (result.failureLocation.known())
        {
            std::cerr << "出错位置：" << result.failureLocation.toString();
            if (!result.failureLocation.function.empty())
                std::cerr << "（" << result.failureLocation.function << "）";
            std::cerr << "\n";
        }
        if (!result.sourceTrace.empty())
        {
            std::cerr << "调用栈（外 → 内）：\n";
            for (const stackvm::DebugFrame &frame : result.sourceTrace)
            {
                const std::string name = frame.function.empty() ? "(根)" : frame.function;
                std::cerr << "  " << name << "  " << frame.location.toString() << "\n";
            }
        }
        return 1;
    }

    // 4. `--expected`：按文件里的 `# 期望:` 比对（示例套件用）。
    if (expectCheck && !file.expected.empty())
    {
        const bool same = (result.output == file.expected ||
                           result.output == file.expected + "\n");
        std::cout << (same ? "成功" : "失败") << "\n";
        if (!same)
        {
            std::cerr << "  期望：" << file.expected << "\n  实际：" << result.output << "\n";
        }
    }
    std::cout << "; 执行 " << result.steps << " 条指令，栈最深 " << result.maxStackDepth
              << "，堆峰值 " << result.heapPeakBytes << " 字节";
    if (result.heapLiveBytes != 0u)
    {
        std::cout << "，**没释放 " << result.heapLiveBytes << " 字节 / " << result.heapLiveBlocks
                  << " 块**";
    }
    std::cout << "\n";
    return result.ok ? 0 : 1;
}
