// 端到端流水线自检：读 examples/*.quad，逐个跑 runPipeline，并要求
//
//   ① 整条流水线跑通；② 桥的跳过条数为 0；③ 降级后的程序能解释执行；
//   ④ transfer 的模拟输出与三地址解释输出一致；⑤ 与文件里 `# 期望:` 一致。
//
//   usage: t1t-pipeline-suite [目录]（默认 examples）
#include "pipeline.hpp"

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
        if (entry.path().extension() == ".quad") files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end());
    if (files.empty()) {
        std::cout << "目录 " << directory << " 里没有找到 .quad 示例。\n";
        return 1;
    }
    std::size_t passed = 0;
    std::size_t failed = 0;
    for (const auto& path : files) {
        std::ifstream in(path, std::ios::binary);
        std::ostringstream buffer;
        buffer << in.rdbuf();
        std::string name;
        std::string expect;
        std::istringstream text(buffer.str());
        for (std::string line; std::getline(text, line);) {
            line = trim(line);
            const auto colon = line.find(':');
            if (line.empty() || line[0] != '#' || colon == std::string::npos) continue;
            const std::string key = trim(line.substr(1, colon - 1));
            if (key == "名称") name = trim(line.substr(colon + 1));
            if (key == "期望") expect = trim(line.substr(colon + 1));
        }
        t1t::PipelineOptions options;
        options.file = path.string();
        const t1t::PipelineReport report = t1t::runPipeline(options);
        auto fail = [&](const std::string& reason) {
            ++failed;
            std::cout << "[失败] " << path.filename().string() << " —— " << reason << "\n";
        };
        if (!report.ok) { fail("流水线出错：" + report.error); continue; }
        if (report.skipped != 0) {
            fail("桥跳过了 " + std::to_string(report.skipped) + " 条四元式");
            continue;
        }
        if (!report.executable) { fail("降级后的程序不能解释执行"); continue; }
        if (!report.ssaEquivalent) { fail("SSA 的语义对照没通过"); continue; }
        if (!report.consistent) {
            fail("transfer 模拟 " + join(report.simulatedNumbers) + " ≠ 三地址解释 " +
                 join(report.interpreted));
            continue;
        }
        const std::string actual = join(report.interpreted);
        if (!expect.empty() && actual != expect) {
            fail("输出是 " + actual + "，期望 " + expect);
            continue;
        }
        ++passed;
        std::cout << "[通过] " << path.filename().string() << " —— "
                  << (name.empty() ? path.filename().string() : name)
                  << "（四元式 " << report.quadsBefore << " → " << report.quadsAfter << "，桥翻译 "
                  << report.mapped << "，三地址 " << report.instructionsLowered << " → φ 消除后 "
                  << report.instructionsAfterSsa << "，φ " << report.phis << "，用色 "
                  << report.colorsUsed << "，输出 " << actual << "，两种执行一致）\n";
    }
    std::cout << "共 " << files.size() << " 个示例：" << passed << " 通过，" << failed << " 失败。\n";
    return failed == 0 ? 0 : 1;
}
