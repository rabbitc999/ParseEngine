// `t1t transfer`：读四元式 → 优化 → 生成目标代码 → 模拟执行。
#include "module_main.hpp"
#include "hydra86.hpp"
#include "llvm_hydra.hpp"
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
    std::string sourceFormat = "quads";
    std::string configPath;
    std::string inputText;
    std::string dataPath;                                   // `--data=`：数据区字节镜像
    std::string layoutPath;                                 // `--layout=`：数据区符号基址表
    std::string assemblyOutPath;                            // `--asm-out=`：纯目标汇编
    std::string binaryOutPath;                              // `--bin-out=`：Hydra86 .bin
    bool showAssembly = false;
    bool noOpt = false;                                     // `--no-opt`：跳过优化（调试 / 后端限制时用）
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            std::cout << "用法：t1t transfer <file.quad|file.ll> [--from=quads|llvm]\n"
                         "                  [--target=mips|8086|arm|hydra86|llvm]\n"
                         "                  [--config=file.ini] [--input=1,2] [--asm] [--no-opt]\n"
                         "                  [--asm-out=file.asm] [--bin-out=file.bin]\n"
                         "                  [--data=x.data.bin] [--layout=x.layout]\n"
                         "\n"
                         "  --data   数据区字节镜像（simple_ast 的 .data.bin），供 addr / load / store 读写\n"
                         "  --layout 数据区符号表（simple_ast 的 .layout）；留空时按 <data>.layout 找\n"
                         "  --from=llvm 读取本工具生成且保留 t1t 载荷的 LLVM IR，仅可导出 Hydra86\n";
            return 0;
        }
        if (arg.rfind("--target=", 0) == 0) { targetName = arg.substr(9); continue; }
        if (arg.rfind("--from=", 0) == 0) { sourceFormat = arg.substr(7); continue; }
        if (arg.rfind("--config=", 0) == 0) { configPath = arg.substr(9); continue; }
        if (arg.rfind("--input=", 0) == 0) { inputText = arg.substr(8); continue; }
        if (arg.rfind("--data=", 0) == 0) { dataPath = arg.substr(7); continue; }
        if (arg.rfind("--layout=", 0) == 0) { layoutPath = arg.substr(9); continue; }
        if (arg.rfind("--asm-out=", 0) == 0) { assemblyOutPath = arg.substr(10); continue; }
        if (arg.rfind("--bin-out=", 0) == 0) { binaryOutPath = arg.substr(10); continue; }
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
        if (sourceFormat == "llvm") {
            if (target != transfer::Target::Hydra86)
                throw std::runtime_error("--from=llvm 当前只支持 --target=hydra86");
            if (!configPath.empty() || !dataPath.empty() || !layoutPath.empty() || noOpt ||
                !inputText.empty())
                throw std::runtime_error(
                    "LLVM → Hydra86 不接受 --config/--data/--layout/--no-opt/--input");
            const std::string assembly = transfer::generateHydra86FromLlvm(text);
            if (showAssembly) std::cout << assembly;
            if (!assemblyOutPath.empty()) {
                std::ofstream out(assemblyOutPath, std::ios::binary);
                if (!out || !(out << assembly))
                    throw std::runtime_error("无法写入 Hydra86 汇编 " + assemblyOutPath);
                std::cout << "Hydra86 汇编：" << assemblyOutPath << "\n";
            }
            transfer::Hydra86Binary binary;
            if (!binaryOutPath.empty()) {
                binary = transfer::compileHydra86(assembly);
                if (!binary.available)
                    throw std::runtime_error("Hydra86 二进制输出不可用：" + binary.error);
                if (!binary.ok) throw std::runtime_error("Hydra86 汇编失败：" + binary.error);
                std::ofstream out(binaryOutPath, std::ios::binary);
                if (!out || !out.write(reinterpret_cast<const char*>(binary.bytes.data()),
                                       static_cast<std::streamsize>(binary.bytes.size())))
                    throw std::runtime_error("无法写入 Hydra86 镜像 " + binaryOutPath);
                std::cout << "Hydra86 镜像：" << binaryOutPath << "（" << binary.bytes.size()
                          << " 字节）\n";
            }
            const transfer::Hydra86Execution execution =
                binary.ok ? transfer::runHydra86Binary(binary.bytes)
                          : transfer::runHydra86(assembly);
            if (!execution.available) {
                std::cout << "LLVM → Hydra86：汇编已生成；" << execution.error << "\n";
                return 0;
            }
            if (!execution.ok) throw std::runtime_error("Hydra86 执行失败：" + execution.error);
            std::cout << "LLVM → Hydra86：周期 " << execution.cycles << "，退出码 "
                      << execution.exitCode << "\n输出：";
            if (execution.output.empty()) std::cout << "（无）";
            else std::cout << execution.output;
            if (execution.output.empty() || execution.output.back() != '\n') std::cout << "\n";
            return 0;
        }
        if (sourceFormat != "quads")
            throw std::runtime_error("未知输入格式 " + sourceFormat + "；可选 quads / llvm");
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
                  << "）；目标代码 " << lines << " 行\n";
        if (showAssembly) std::cout << assembly;
        if (!assemblyOutPath.empty()) {
            std::ofstream out(assemblyOutPath, std::ios::binary);
            if (!out || !(out << assembly))
                throw std::runtime_error("无法写入目标代码文件 " + assemblyOutPath);
            std::cout << "目标代码产物：" << assemblyOutPath << "\n";
        }
        if (config.target == transfer::Target::Hydra86) {
            if (!inputText.empty())
                throw std::runtime_error("Hydra86 后端暂不支持 --input；当前已支持整数输出");
            transfer::Hydra86Binary binary;
            if (!binaryOutPath.empty()) {
                binary = transfer::compileHydra86(assembly);
                if (!binary.available)
                    throw std::runtime_error("Hydra86 二进制输出不可用：" + binary.error);
                if (!binary.ok)
                    throw std::runtime_error("Hydra86 汇编失败：" + binary.error);
                std::ofstream out(binaryOutPath, std::ios::binary);
                if (!out || !out.write(reinterpret_cast<const char*>(binary.bytes.data()),
                                       static_cast<std::streamsize>(binary.bytes.size())))
                    throw std::runtime_error("无法写入 Hydra86 镜像 " + binaryOutPath);
                std::cout << "Hydra86 镜像：" << binaryOutPath << "（" << binary.bytes.size()
                          << " 字节）\n";
            }
            const transfer::Hydra86Execution execution =
                binary.ok ? transfer::runHydra86Binary(binary.bytes)
                          : transfer::runHydra86(assembly);
            if (!execution.available) {
                std::cout << "Hydra86：汇编已生成；" << execution.error << "\n";
                return 0;
            }
            if (!execution.ok) throw std::runtime_error("Hydra86 执行失败：" + execution.error);
            std::cout << "Hydra86：周期 " << execution.cycles << "，退出码 "
                      << execution.exitCode << "\n输出：";
            if (execution.output.empty()) std::cout << "（无）";
            else std::cout << execution.output;
            if (execution.output.empty() || execution.output.back() != '\n') std::cout << "\n";
            return 0;
        }
        if (!binaryOutPath.empty())
            throw std::runtime_error("--bin-out 仅适用于 --target=hydra86");
        if (config.target == transfer::Target::Llvm) {
            std::cout << "LLVM：IR 已生成；使用 --asm-out=file.ll 保存后交给 clang"
                         "（或带 lli 的完整发行版）\n";
            return 0;
        }
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
