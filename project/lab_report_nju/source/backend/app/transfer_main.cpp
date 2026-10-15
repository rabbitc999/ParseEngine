// `t1t transfer`：读四元式 → 优化 → 生成目标代码 → 模拟执行。
#include "module_main.hpp"
#include "transfer.hpp"

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

std::vector<std::string> splitCommas(const std::string& text) {
    std::vector<std::string> out;
    std::string current;
    for (char c : text) {
        if (c == ',') {
            if (!current.empty()) out.push_back(current);
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    if (!current.empty()) out.push_back(current);
    return out;
}

} // namespace

int transferMain(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    std::string file;
    std::string targetName = "mips";
    std::string configPath;
    std::string inputText;
    std::string dataPath;                                   // `--data=`：数据区字节镜像
    std::string layoutPath;                                 // `--layout=`：数据区符号基址表
    bool showAssembly = false;
    bool noOpt = false;                                     // `--no-opt`：跳过优化（调试 / 后端限制时用）
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            std::cout << "用法：t1t transfer <file.quad> [--target=mips|8086|arm]\n"
                         "                  [--config=file.ini] [--input=1,2] [--asm] [--no-opt]\n"
                         "                  [--data=x.data.bin] [--layout=x.layout]\n"
                         "\n"
                         "  --data   数据区字节镜像（simple_ast 的 .data.bin），供 addr / load / store 读写\n"
                         "  --layout 数据区符号表（simple_ast 的 .layout）；留空时按 <data>.layout 找\n";
            return 0;
        }
        if (arg.rfind("--target=", 0) == 0) { targetName = arg.substr(9); continue; }
        if (arg.rfind("--config=", 0) == 0) { configPath = arg.substr(9); continue; }
        if (arg.rfind("--input=", 0) == 0) { inputText = arg.substr(8); continue; }
        if (arg.rfind("--data=", 0) == 0) { dataPath = arg.substr(7); continue; }
        if (arg.rfind("--layout=", 0) == 0) { layoutPath = arg.substr(9); continue; }
        if (arg == "--asm") { showAssembly = true; continue; }
        if (arg == "--no-opt") { noOpt = true; continue; }
        if (arg.rfind("--", 0) == 0) {
            std::cerr << "t1t transfer: 不认识的选项 " << arg << "\n";
            return 2;
        }
        file = arg;
    }
    try {
        const transfer::Target target = transfer::target(targetName);
        std::string text;
        if (file.empty()) {
            std::ostringstream all;
            all << std::cin.rdbuf();
            text = all.str();
        } else {
            std::ifstream in(file, std::ios::binary);
            if (!in) {
                std::cerr << "t1t transfer: 无法读取 " << file << "\n";
                return 2;
            }
            std::ostringstream all;
            all << in.rdbuf();
            text = all.str();
        }
        std::istringstream buffer(text);
        transfer::Program program = transfer::read(buffer);
        transfer::validate(program);
        transfer::MachineConfig config = configPath.empty()
                                             ? transfer::defaultConfig(target)
                                             : transfer::loadConfigFile(configPath, target);
        // 数据区三件套（addr / load / store）靠这两份文件：字节镜像 + 符号基址表。
        if (!dataPath.empty()) transfer::loadDataArea(config, dataPath, layoutPath);
        const transfer::Analysis analysis = transfer::analyze(program);
        transfer::Optimization optimized = transfer::optimizeDetailed(program);
        if (noOpt) {                                          // 关优化：原程序直接进后端
            optimized.program = program;
            optimized.rounds = 0;
            optimized.trace.clear();
            optimized.trace.push_back("--no-opt：跳过优化");
        }
        std::cout << "四元式：" << program.size() << " 条（基本块 " << analysis.blocks.size() << "）\n"
                  << "优化：" << program.size() << " → " << optimized.program.size() << " 条（"
                  << optimized.rounds << " 轮）\n";
        for (const auto& line : optimized.trace) std::cout << "  " << line << "\n";
        const std::string assembly = transfer::generate(optimized.program, config);
        std::size_t lines = 0;
        std::istringstream assemblyStream(assembly);
        for (std::string line; std::getline(assemblyStream, line);) {
            if (!line.empty()) ++lines;
        }
        std::cout << "目标机：" << transfer::name(config.target) << "（字长 " << config.wordBits
                  << "）；汇编 " << lines << " 行\n";
        if (showAssembly) std::cout << assembly;
        const transfer::Execution execution = transfer::simulate(assembly, splitCommas(inputText));
        std::cout << "模拟：步数 " << execution.steps << "\n输出：";
        if (execution.output.empty()) std::cout << "（无）";
        for (std::size_t i = 0; i < execution.output.size(); ++i) {
            std::cout << (i ? " " : "") << transfer::displayValue(execution.output[i]);
        }
        std::cout << "\n";
        if (!execution.text.empty()) std::cout << "文本输出：" << execution.text << "\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "t1t transfer: " << error.what() << "\n";
        return 1;
    }
}
