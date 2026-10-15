// regalloc —— 寄存器分配（活跃变量 → 干涉图 → 图着色 / 线性扫描 → 溢出重写）。
//
//   regalloc [--regs=N] [--algorithm=chaitin|linear] [--no-coalesce] [file]
//   regalloc --help / --version
#include "regalloc.hpp"

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

} // namespace

int regallocMain(int argc, char** argv) {
    useUtf8();
    std::size_t registers = 4;
    std::string algorithm = "chaitin";
    regalloc::Options options;
    std::string file;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            std::cout << "用法：regalloc [--regs=N] [--algorithm=chaitin|linear] [--no-coalesce] [file]\n"
                         "\n"
                         "输入格式（每行一条，`;` 或 `#` 起注释）：\n"
                         "  label L1\n"
                         "  mov t1, 10        add t2, t1, 3      load t3, [x]\n"
                         "  store [x], t1     jz t2, L2         jmp L1        call f\n"
                         "  ret / halt\n"
                         "\n"
                         "输出：分配结果、需要的溢出重写、以及指令数 / 访存次数对比。\n"
                         "物理寄存器缺省 4 个（A、B 被调用者保存，C、D 调用者保存）。\n";
            return 0;
        }
        if (arg == "--version") { std::cout << "regalloc 1.0.0\n"; return 0; }
        if (arg.rfind("--regs=", 0) == 0) { registers = static_cast<std::size_t>(std::stoul(arg.substr(7))); continue; }
        if (arg.rfind("--algorithm=", 0) == 0) { algorithm = arg.substr(12); continue; }
        if (arg == "--no-coalesce") { options.coalesce = false; continue; }
        if (arg.rfind("--", 0) == 0) { std::cerr << "regalloc: 不认识的选项 " << arg << "\n"; return 2; }
        file = arg;
    }
    std::string text;
    if (file.empty()) {
        text = readAll(std::cin);
    } else {
        std::ifstream in(file, std::ios::binary);
        if (!in) { std::cerr << "regalloc: 无法读取 " << file << "\n"; return 2; }
        text = readAll(in);
    }
    regalloc::Program program;
    std::vector<std::string> errors;
    if (!regalloc::parse(text, program, errors)) {
        for (const auto& error : errors) std::cerr << "regalloc: " << error << "\n";
        return 1;
    }
    if (program.empty()) { std::cerr << "regalloc: 程序为空\n"; return 1; }
    const regalloc::Machine machine = regalloc::Machine::teaching(registers);
    const regalloc::Result result = regalloc::allocate(program, machine, algorithm, options);
    std::cout << "=== 分配结果 ===\n";
    for (const auto& entry : result.allocation.assignment)
        std::cout << "  " << entry.first << " -> " << entry.second << "\n";
    for (const auto& name : result.allocation.spilled) std::cout << "  " << name << " -> 溢出\n";
    std::cout << "\n=== 重写后的程序 ===\n" << regalloc::print(result.rewritten.program);
    std::cout << "\n=== 指标 ===\n" << regalloc::report(result, machine);
    return 0;
}

#ifdef TRANSFER_MODULE_STANDALONE_MAIN
/*
 * 单模块构建（本目录的 Makefile / CMakeLists）用：把 regallocMain 包成 main。
 * 合并库 `t1t` 由 `app/main.cpp` 统一分发，不定义这个宏，所以不编这一段。
 */
int main(int argc, char** argv) { return regallocMain(argc, argv); }
#endif
