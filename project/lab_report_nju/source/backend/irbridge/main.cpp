// irbridge —— 三元式 ↔ 四元式 互转，并导出 library/transfer 可读的四元式。
//
//   irbridge triples2quads [in.triples] [-o out.quads]
//   irbridge quads2triples [in.quads]   [-o out.triples]
//   irbridge to-transfer   [in]         [-o out.quads] [--no-decl]
//   irbridge stats         [in]
//   irbridge --check FILE...            只解析并报告错误
//
// 不给输入文件时读标准输入；不给 -o 时写标准输出。
#include "irbridge.hpp"

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#endif

namespace {

void useUtf8() {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
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

void printErrors(const std::vector<std::string>& errors) {
    for (const auto& error : errors) std::cerr << "irbridge: " << error << "\n";
}

bool writeOut(const std::string& path, const std::string& text) {
    if (path.empty()) { std::cout << text; return true; }
    std::ofstream out(path, std::ios::binary);
    if (!out) { std::cerr << "irbridge: 无法写入 " << path << "\n"; return false; }
    out << text;
    return true;
}

} // namespace

int irbridgeMain(int argc, char** argv) {
    useUtf8();
    std::vector<std::string> args(argv + 1, argv + argc);
    if (args.empty() || args[0] == "--help" || args[0] == "-h") {
        std::cout << "用法：\n"
                     "  irbridge triples2quads [in.triples] [-o out.quads]\n"
                     "  irbridge quads2triples [in.quads]   [-o out.triples]\n"
                     "  irbridge to-transfer   [in]         [-o out.quads] [--no-decl]\n"
                     "  irbridge stats         [in]\n"
                     "  irbridge --check FILE...\n";
        return args.empty() ? 1 : 0;
    }
    const std::string mode = args[0];
    std::string input, output;
    bool withDecl = true;
    std::vector<std::string> files;
    for (std::size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "-o" && i + 1 < args.size()) { output = args[++i]; continue; }
        if (args[i] == "--no-decl") { withDecl = false; continue; }
        files.push_back(args[i]);
    }
    if (!files.empty()) input = files.front();

    if (mode == "--check") {
        if (files.empty()) { std::cerr << "irbridge: --check 需要一个文件\n"; return 2; }
        int failed = 0;
        for (const auto& file : files) {
            std::ifstream in(file, std::ios::binary);
            if (!in) { std::cerr << "irbridge: 无法读取 " << file << "\n"; ++failed; continue; }
            const std::string text = readAll(in);
            irbridge::Triples triples;
            irbridge::Quads quads;
            std::vector<std::string> errors;
            if (!irbridge::readQuads(text, quads, errors)) {
                errors.clear();
                if (!irbridge::readTriples(text, triples, errors)) {
                    std::cerr << file << ":\n";
                    printErrors(errors);
                    ++failed;
                    continue;
                }
            }
            std::cout << file << ": ok\n";
        }
        return failed == 0 ? 0 : 1;
    }

    std::istringstream stdinText;
    std::string text;
    if (input.empty()) {
        text = readAll(std::cin);
    } else {
        std::ifstream in(input, std::ios::binary);
        if (!in) { std::cerr << "irbridge: 无法读取 " << input << "\n"; return 2; }
        text = readAll(in);
    }

    irbridge::Triples triples;
    irbridge::Quads quads;
    std::vector<std::string> errors;

    if (mode == "triples2quads") {
        if (!irbridge::readTriples(text, triples, errors)) { printErrors(errors); return 1; }
        if (!irbridge::triplesToQuads(triples, quads, errors)) { printErrors(errors); return 1; }
        return writeOut(output, irbridge::writeQuads(quads)) ? 0 : 2;
    }
    if (mode == "quads2triples") {
        if (!irbridge::readQuads(text, quads, errors)) { printErrors(errors); return 1; }
        if (!irbridge::quadsToTriples(quads, triples, errors)) { printErrors(errors); return 1; }
        return writeOut(output, irbridge::writeTriples(triples)) ? 0 : 2;
    }
    if (mode == "to-transfer") {
        // 输入可能是四元式也可能是三元式：先按四元式读，失败再按三元式读。
        if (!irbridge::readQuads(text, quads, errors)) {
            if (!irbridge::readTriples(text, triples, errors)) { printErrors(errors); return 1; }
            return writeOut(output, irbridge::toTransferFromTriples(triples, withDecl)) ? 0 : 2;
        }
        return writeOut(output, irbridge::toTransfer(quads, withDecl)) ? 0 : 2;
    }
    if (mode == "stats") {
        if (irbridge::readQuads(text, quads, errors) && !quads.empty()) {
            return writeOut(output, irbridge::statistics(quads)) ? 0 : 2;
        }
        errors.clear();
        if (!irbridge::readTriples(text, triples, errors)) { printErrors(errors); return 1; }
        return writeOut(output, irbridge::statistics(triples)) ? 0 : 2;
    }
    std::cerr << "irbridge: 不认识的模式 " << mode << "（--help 看用法）\n";
    return 2;
}

#ifdef TRANSFER_MODULE_STANDALONE_MAIN
/*
 * 单模块构建（本目录的 Makefile / CMakeLists）用：把 irbridgeMain 包成 main。
 * 合并库 `t1t` 由 `app/main.cpp` 统一分发，不定义这个宏，所以不编这一段。
 */
int main(int argc, char** argv) { return irbridgeMain(argc, argv); }
#endif
