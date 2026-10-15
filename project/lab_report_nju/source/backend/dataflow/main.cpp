// dataflow —— 通用数据流框架（格 + 传递函数 + 工作队列 / 朴素的轮询）。
//
//   dataflow [--analysis=live|reach|avail|const|all] [--mode=summary|sets|mermaid|check]
//            [--order=fifo|lifo] [--seed=all|boundary] [--no-monotone] [file]
//   dataflow --help / --version
#include "dataflow.hpp"

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

std::string readAll(std::istream& in) {
    std::ostringstream out;
    out << in.rdbuf();
    std::string text = out.str();
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF) {
        text.erase(0, 3);
    }
    return text;
}

} // namespace

int dataflowMain(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    std::string analysisName = "all";
    std::string mode = "summary";
    dataflow::Options options;
    std::string file;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            std::cout << "用法：dataflow [--analysis=live|reach|avail|const|all]\n"
                         "                [--mode=summary|sets|mermaid|check]\n"
                         "                [--order=fifo|lifo] [--seed=all|boundary]\n"
                         "                [--no-monotone] [file]\n"
                         "\n"
                         "输入格式（每行一条，`;` / `#` / `//` 起注释）：\n"
                         "  label L1\n"
                         "  mov t1, 1        add t2, t1, 3     load t3, [x]\n"
                         "  store [x], t1    jz t2, L2         jmp L1      print t2   halt\n"
                         "\n"
                         "分析：live 活跃变量（后向 / 并集）、reach 到达定值（前向 / 并集）、\n"
                         "      avail 可用表达式（前向 / 交集）、const 常量传播（前向 / 映射格）。\n"
                         "模式：summary 指标、sets 每块 IN/OUT、mermaid 画 CFG（附集合）、\n"
                         "      check 两种求解器互相对照（不一致就返回 1，CI 用）。\n";
            return 0;
        }
        if (arg == "--version") { std::cout << "dataflow 1.0.0\n"; return 0; }
        if (arg.rfind("--analysis=", 0) == 0) { analysisName = arg.substr(11); continue; }
        if (arg.rfind("--mode=", 0) == 0) { mode = arg.substr(7); continue; }
        if (arg.rfind("--order=", 0) == 0) {
            const std::string order = arg.substr(8);
            if (order == "fifo") options.lifo = false;
            else if (order == "lifo") options.lifo = true;
            else { std::cerr << "dataflow: --order 只支持 fifo / lifo\n"; return 2; }
            continue;
        }
        if (arg.rfind("--seed=", 0) == 0) {
            const std::string seed = arg.substr(7);
            if (seed == "all") options.seedAll = true;
            else if (seed == "boundary") options.seedAll = false;
            else { std::cerr << "dataflow: --seed 只支持 all / boundary\n"; return 2; }
            continue;
        }
        if (arg == "--no-monotone") { options.checkMonotone = false; continue; }
        if (arg.rfind("--", 0) == 0) {
            std::cerr << "dataflow: 不认识的选项 " << arg << "\n";
            return 2;
        }
        file = arg;
    }
    std::string text;
    if (file.empty()) {
        text = readAll(std::cin);
    } else {
        std::ifstream in(file, std::ios::binary);
        if (!in) {
            std::cerr << "dataflow: 无法读取 " << file << "\n";
            return 2;
        }
        text = readAll(in);
    }
    dataflow::Program program;
    std::vector<std::string> errors;
    if (!dataflow::parse(text, program, errors)) {
        for (const auto& error : errors) std::cerr << "dataflow: " << error << "\n";
        return 1;
    }
    if (mode != "summary" && mode != "sets" && mode != "mermaid" && mode != "check") {
        std::cerr << "dataflow: 不认识的模式 " << mode << "\n";
        return 2;
    }
    std::vector<std::string> names;
    if (analysisName == "all") {
        names = dataflow::analysisNames();
    } else {
        names.push_back(analysisName);
    }
    bool ok = true;
    for (const auto& name : names) {
        const auto analysis = dataflow::makeAnalysis(name, program);
        if (!analysis) {
            std::cerr << "dataflow: 不认识的分析 " << name << "\n";
            return 2;
        }
        const dataflow::Solution worklist = dataflow::solveWorklist(program, *analysis, options);
        const dataflow::Solution naive = dataflow::solveNaive(program, *analysis, options);
        const bool same = dataflow::sameSolution(worklist, naive, program);
        if (mode == "summary") {
            std::cout << dataflow::report(program, *analysis, worklist, naive);
        } else if (mode == "sets") {
            std::cout << dataflow::dumpSets(program, *analysis, worklist);
        } else if (mode == "mermaid") {
            std::cout << dataflow::mermaid(program, *analysis, worklist);
        } else {
            std::cout << "分析 " << name << "：" << (same ? "两种求解器一致" : "两种求解器不一致！")
                      << "，单调 " << (worklist.monotone ? "✓" : "✗") << "，收敛 "
                      << (worklist.converged ? "✓" : "✗") << "（块访问 "
                      << worklist.blockVisits << " / " << naive.blockVisits << "）\n";
            if (!same || !worklist.monotone || !worklist.converged || !naive.converged) ok = false;
        }
        if (mode != "check") std::cout << "\n";
    }
    return ok ? 0 : 1;
}

#ifdef TRANSFER_MODULE_STANDALONE_MAIN
/*
 * 单模块构建（本目录的 Makefile / CMakeLists）用：把 dataflowMain 包成 main。
 * 合并库 `t1t` 由 `app/main.cpp` 统一分发，不定义这个宏，所以不编这一段。
 */
int main(int argc, char** argv) { return dataflowMain(argc, argv); }
#endif
