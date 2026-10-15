// ssa —— SSA 与稀疏优化（构造 / 常量传播 / 值编号 / φ 消除）。
//
//   ssa [--mode=summary|ssa|dom|sccp|gvn|destroy|run] [--steps=N] [file]
//   ssa --help / --version
#include "ssa.hpp"

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

void useUtf8() {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
}

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

void printRun(const ssa::RunResult& run) {
    if (!run.error.empty()) {
        std::cout << "运行出错：" << run.error << "\n";
        return;
    }
    std::cout << "执行步数：" << run.steps << "\n输出：";
    if (run.output.empty()) std::cout << "（无）";
    for (std::size_t i = 0; i < run.output.size(); ++i) {
        std::cout << (i ? " " : "") << run.output[i];
    }
    std::cout << "\n内存：";
    if (run.memory.empty()) std::cout << "（空）";
    bool first = true;
    for (const auto& entry : run.memory) {
        std::cout << (first ? "" : " ") << entry.first << "=" << entry.second;
        first = false;
    }
    std::cout << "\n";
}

} // namespace

int ssaMain(int argc, char** argv) {
    useUtf8();
    std::string mode = "summary";
    std::size_t steps = 200000;
    std::string file;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            std::cout << "用法：ssa [--mode=summary|ssa|dom|sccp|gvn|destroy|run] [--steps=N] [file]\n"
                         "\n"
                         "输入格式（每行一条，`;` / `#` / `//` 起注释）：\n"
                         "  label L1\n"
                         "  mov t1, 1        add t2, t1, 3     load t3, [x]\n"
                         "  store [x], t1    jz t2, L2         jmp L1\n"
                         "  print t2         halt\n"
                         "\n"
                         "各模式：\n"
                         "  summary（缺省）只看指标；ssa 打印 SSA 形式；dom 打印支配树（Mermaid）；\n"
                         "  sccp 打印常量传播后的程序；gvn 打印值编号后的程序；\n"
                         "  destroy 打印 φ 消除后的可执行形式；run 直接解释执行原程序。\n";
            return 0;
        }
        if (arg == "--version") { std::cout << "ssa 1.0.0\n"; return 0; }
        if (arg.rfind("--mode=", 0) == 0) { mode = arg.substr(7); continue; }
        if (arg.rfind("--steps=", 0) == 0) {
            steps = static_cast<std::size_t>(std::stoull(arg.substr(8)));
            continue;
        }
        if (arg.rfind("--", 0) == 0) {
            std::cerr << "ssa: 不认识的选项 " << arg << "\n";
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
            std::cerr << "ssa: 无法读取 " << file << "\n";
            return 2;
        }
        text = readAll(in);
    }
    ssa::Program program;
    std::vector<std::string> errors;
    if (!ssa::parse(text, program, errors)) {
        for (const auto& error : errors) std::cerr << "ssa: " << error << "\n";
        return 1;
    }
    const ssa::Cfg cfg = ssa::buildCfg(program);
    const ssa::Dominators dom = ssa::analyzeDominators(program, cfg);
    if (mode == "run") {
        printRun(ssa::run(program, steps));
        return 0;
    }
    if (mode == "dom") {
        std::cout << dom.treeText(program);
        return 0;
    }
    const ssa::Result result = ssa::optimize(program);
    if (mode == "ssa") {
        std::cout << ssa::print(result.ssa.program);
        return 0;
    }
    if (mode == "sccp") {
        std::cout << ssa::print(result.sccp.program);
        return 0;
    }
    if (mode == "gvn") {
        std::cout << ssa::print(result.gvn.program);
        return 0;
    }
    if (mode == "destroy") {
        std::cout << ssa::print(result.destruction.program);
        return 0;
    }
    if (mode != "summary") {
        std::cerr << "ssa: 不认识的模式 " << mode << "\n";
        return 2;
    }
    std::cout << ssa::report(result);
    return 0;
}

#ifdef TRANSFER_MODULE_STANDALONE_MAIN
/*
 * 单模块构建（本目录的 Makefile / CMakeLists）用：把 ssaMain 包成 main。
 * 合并库 `t1t` 由 `app/main.cpp` 统一分发，不定义这个宏，所以不编这一段。
 */
int main(int argc, char** argv) { return ssaMain(argc, argv); }
#endif
