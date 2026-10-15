// 一个最小但完整的 `.gram` 语言服务器。
//
//   lsp-server --stdio            在标准输入输出上跑 LSP（编辑器默认用法）
//   lsp-server --check FILE       直接校验一份文法（CI / 命令行）
//   lsp-server --version | --help
#include "lsp.hpp"

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace {

void useBinaryStdio() {
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
}

std::string readFile(const std::string& path, bool& ok) {
    std::ifstream in(path, std::ios::binary);
    if (!in) { ok = false; return {}; }
    std::ostringstream out;
    out << in.rdbuf();
    ok = true;
    std::string text = out.str();
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF) {
        text.erase(0, 3);
    }
    return text;
}

void printHelp() {
    std::cout << "parseengine-lsp 1.0.0 —— `.gram` 文法语言服务器\n"
                 "\n"
                 "用法：\n"
                 "  lsp-server --stdio            在标准输入输出上跑 LSP（编辑器默认用法）\n"
                 "  lsp-server --check FILE...    直接校验文法，输出 FILE:行:列: 级别: 消息\n"
                 "  lsp-server --version          版本\n"
                 "  lsp-server --help             本帮助\n"
                 "\n"
                 "能力：诊断、悬停、补全、跳转定义、文档大纲、语义高亮、保守格式化、快速修复。\n";
}

} // namespace

int main(int argc, char** argv) {
    useBinaryStdio();
    std::string mode = "--stdio";
    std::vector<std::string> files;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") { printHelp(); return 0; }
        if (arg == "--version" || arg == "-v") { std::cout << "parseengine-lsp 1.0.0\n"; return 0; }
        if (arg == "--check") { mode = "--check"; continue; }
        if (arg == "--stdio") { mode = "--stdio"; continue; }
        files.push_back(arg);
    }
    if (mode == "--check") {
        if (files.empty()) {
            std::cerr << "用法：lsp-server --check FILE...\n";
            return 2;
        }
        int errors = 0;
        for (const auto& file : files) {
            bool ok = false;
            const std::string text = readFile(file, ok);
            if (!ok) {
                std::cout << file << ": error: 无法读取文件\n";
                ++errors;
                continue;
            }
            const std::string report = lsp::Server::checkText(text, file);
            std::cout << report;
            for (const auto& line : report) if (line == '\n') { /* 计数交给下面的统计 */ }
            if (report.find(": error:") != std::string::npos) ++errors;
        }
        return errors == 0 ? 0 : 1;
    }
    lsp::Server server;
    return lsp::runStdioServer(server, std::cin, std::cout, std::cerr);
}
