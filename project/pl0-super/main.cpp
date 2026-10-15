// pl0-super 命令行：源码 → 简化 → 目标机汇编 → 目标机执行。
//
//   pl0-super 文件.p0s              编译并运行（打印输出）
//   pl0-super --asm 文件.p0s        只打印汇编
//   pl0-super --stats 文件.p0s      打印各趟简化各做了多少次
//   pl0-super --expect=1 2 3 文件.p0s  跑完对照期望（记号序列），不符返回 1

#include "pl0super.hpp"

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

/** 读整个文件（二进制读，换行自己处理）。 */
bool readFile(const std::string& path, std::string& text) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream buffer;
    buffer << in.rdbuf();
    text = buffer.str();
    return true;
}

/** 期待的输出：空白分开的记号。 */
std::vector<std::string> splitTokens(const std::string& text) {
    std::vector<std::string> out;
    std::istringstream in(text);
    std::string token;
    while (in >> token) out.push_back(token);
    return out;
}

} // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    std::string path;
    std::string expect;
    bool showAsm = false;
    bool showStats = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--asm") showAsm = true;
        else if (arg == "--stats") showStats = true;
        else if (arg.rfind("--expect=", 0) == 0) expect = arg.substr(9);
        else if (!arg.empty() && arg[0] == '-') {
            std::cerr << "不认的选项：" << arg << "\n";
            return 2;
        } else {
            path = arg;
        }
    }
    if (path.empty()) {
        std::cerr << "用法：pl0-super [--asm] [--stats] [--expect=...] 文件.p0s\n";
        return 2;
    }
    std::string source;
    if (!readFile(path, source)) {
        std::cerr << "打不开文件：" << path << "\n";
        return 2;
    }
    pl0s::CompileResult result;
    if (!pl0s::compile(source, result)) {
        std::cerr << path << "：" << result.error << "\n";
        return 1;
    }
    if (showStats) {
        std::cout << "简化：折叠 " << result.stats.folded << "，代数 " << result.stats.algebra
                  << "，死分支 " << result.stats.deadBranch << "，窥孔 " << result.stats.peephole
                  << "\n";
    }
    if (showAsm) {
        std::cout << result.code.asmText;
        return 0;
    }
    pl0s::VmResult run;
    if (!pl0s::runAsm(result.code.asmText, run)) {
        std::cerr << path << "：运行时错误：" << run.error << "\n";
        return 1;
    }
    std::cout << run.output;
    if (!expect.empty()) {
        const std::vector<std::string> wanted = splitTokens(expect);
        if (wanted != run.tokens) {
            std::cerr << "输出与期望不符：期望 " << expect << "，实际";
            for (const std::string& token : run.tokens) std::cerr << " " << token;
            std::cerr << "\n";
            return 1;
        }
    }
    return 0;
}
