// 示例集运行器：读 examples/*.ssa，做四件事并全部比对：
//
//   1. 解释执行原程序，输出必须与文件里 `# 期望:` 一致；
//   2. SSA 构造 / SCCP / GVN / φ 消除的指标必须与 `# 期望 …:` 一致；
//   3. φ 消除之后的程序解释执行，输出必须和原程序**完全一样**（语义不变）；
//   4. 删掉不可达块等结构变化也要和声明一致。
//
//   usage: ssa-examples [目录]（默认 examples）
#include "ssa.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

std::string trim(const std::string& text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}

struct Expect {
    std::string name;
    std::string note;
    std::string output;          // 期望的 print 输出，空格分隔
    std::size_t phis = 0;
    std::size_t folded = 0;
    std::size_t unreachable = 0;
    std::size_t gvn = 0;
};

Expect readExpect(const std::string& text) {
    Expect expect;
    std::istringstream in(text);
    for (std::string line; std::getline(in, line);) {
        line = trim(line);
        if (line.empty() || line[0] != '#') continue;
        const auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        const std::string key = trim(line.substr(1, colon - 1));
        const std::string value = trim(line.substr(colon + 1));
        if (key == "名称") expect.name = value;
        else if (key == "说明") expect.note = value;
        else if (key == "期望") expect.output = value;
        else if (key == "期望 φ") expect.phis = static_cast<std::size_t>(std::stoull(value));
        else if (key == "期望 折叠") expect.folded = static_cast<std::size_t>(std::stoull(value));
        else if (key == "期望 不可达") expect.unreachable = static_cast<std::size_t>(std::stoull(value));
        else if (key == "期望 GVN") expect.gvn = static_cast<std::size_t>(std::stoull(value));
    }
    return expect;
}

std::string join(const std::vector<long long>& values) {
    if (values.empty()) return "（无）";
    std::ostringstream out;
    for (std::size_t i = 0; i < values.size(); ++i) out << (i ? " " : "") << values[i];
    return out.str();
}

} // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    const std::string directory = argc > 1 ? argv[1] : "examples";
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (entry.path().extension() == ".ssa") files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end());
    if (files.empty()) {
        std::cout << "目录 " << directory << " 里没有找到 .ssa 示例。\n";
        return 1;
    }
    std::size_t passed = 0;
    std::size_t failed = 0;
    for (const auto& path : files) {
        std::ifstream in(path, std::ios::binary);
        std::ostringstream buffer;
        buffer << in.rdbuf();
        const std::string text = buffer.str();
        const Expect expect = readExpect(text);
        const std::string shown = expect.name.empty() ? path.filename().string() : expect.name;
        auto fail = [&](const std::string& reason) {
            ++failed;
            std::cout << "[失败] " << path.filename().string() << " —— " << reason << "\n";
        };
        ssa::Program program;
        std::vector<std::string> errors;
        if (!ssa::parse(text, program, errors)) {
            fail(errors.empty() ? "解析失败" : errors.front());
            continue;
        }
        const ssa::RunResult before = ssa::run(program);
        if (!before.error.empty()) {
            fail("原程序执行出错：" + before.error);
            continue;
        }
        if (join(before.output) != (expect.output.empty() ? "（无）" : expect.output)) {
            fail("解释输出是 " + join(before.output) + "，期望 " + expect.output);
            continue;
        }
        const ssa::Result result = ssa::optimize(program);
        std::string mismatch;
        if (result.stats.phis != expect.phis) {
            mismatch += " φ=" + std::to_string(result.stats.phis) + "（期望 " +
                        std::to_string(expect.phis) + "）";
        }
        if (result.stats.sccpFoldedInstructions != expect.folded) {
            mismatch += " 折叠=" + std::to_string(result.stats.sccpFoldedInstructions) + "（期望 " +
                        std::to_string(expect.folded) + "）";
        }
        if (result.stats.sccpRemovedBlocks != expect.unreachable) {
            mismatch += " 不可达=" + std::to_string(result.stats.sccpRemovedBlocks) + "（期望 " +
                        std::to_string(expect.unreachable) + "）";
        }
        if (result.stats.gvnRedundantExpressions != expect.gvn) {
            mismatch += " GVN=" + std::to_string(result.stats.gvnRedundantExpressions) + "（期望 " +
                        std::to_string(expect.gvn) + "）";
        }
        if (!mismatch.empty()) {
            fail("指标不符：" + mismatch);
            continue;
        }
        if (!result.equivalent) {
            fail("φ 消除前后语义不一致：" + join(result.after.output));
            continue;
        }
        ++passed;
        std::cout << "[通过] " << path.filename().string() << " —— " << shown;
        if (!expect.note.empty()) std::cout << "：" << expect.note;
        std::cout << "（φ " << result.stats.phis << "，折叠 " << result.stats.sccpFoldedInstructions
                  << "，不可达 " << result.stats.sccpRemovedBlocks << "，GVN "
                  << result.stats.gvnRedundantExpressions << "，输出 " << join(before.output) << "）\n";
    }
    std::cout << "共 " << files.size() << " 个示例：" << passed << " 通过，" << failed << " 失败。\n";
    return failed == 0 ? 0 : 1;
}
