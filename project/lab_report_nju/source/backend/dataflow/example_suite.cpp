// 示例集运行器：读 examples/*.df，做两类检查：
//
//   1. 通用检查：每个示例 × 每个分析，跑两种求解器（worklist / naive），
//      结果必须逐块一致、必须收敛、每次更新必须沿格往上（单调性）；
//   2. 逐条检查文件里声明的期望：
//        # 期望 <分析> <块> <in|out>: <集合>
//      块名用 label，没有 label 的块写 B<下标>。
//
//   usage: dataflow-examples [目录]（默认 examples）
#include "dataflow.hpp"

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

struct Expectation {
    std::string analysis;
    std::string block;
    bool wantIn = true;
    std::string value;
};

struct Meta {
    std::string name;
    std::string note;
    std::vector<Expectation> expectations;
};

Meta readMeta(const std::string& text) {
    Meta meta;
    std::istringstream in(text);
    for (std::string line; std::getline(in, line);) {
        line = trim(line);
        if (line.empty() || line[0] != '#') continue;
        const auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        const std::string key = trim(line.substr(1, colon - 1));
        const std::string value = trim(line.substr(colon + 1));
        if (key == "名称") { meta.name = value; continue; }
        if (key == "说明") { meta.note = value; continue; }
        // `期望 <分析> <块> <in|out>`
        std::istringstream keyStream(key);
        std::string first;
        keyStream >> first;
        if (first != "期望") continue;
        Expectation expectation;
        keyStream >> expectation.analysis >> expectation.block;
        std::string where;
        keyStream >> where;
        expectation.wantIn = (where == "in");
        expectation.value = value;
        meta.expectations.push_back(expectation);
    }
    return meta;
}

/** 块名：有 label 用 label，否则 B<下标>。 */
std::string blockName(const dataflow::Program& program, std::size_t index) {
    if (!program[index].label.empty()) return program[index].label;
    return "B" + std::to_string(index);
}

} // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    const std::string directory = argc > 1 ? argv[1] : "examples";
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (entry.path().extension() == ".df") files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end());
    if (files.empty()) {
        std::cout << "目录 " << directory << " 里没有找到 .df 示例。\n";
        return 1;
    }
    std::size_t passed = 0;
    std::size_t failed = 0;
    for (const auto& path : files) {
        std::ifstream in(path, std::ios::binary);
        std::ostringstream buffer;
        buffer << in.rdbuf();
        const std::string text = buffer.str();
        const Meta meta = readMeta(text);
        auto fail = [&](const std::string& reason) {
            ++failed;
            std::cout << "[失败] " << path.filename().string() << " —— " << reason << "\n";
        };
        dataflow::Program program;
        std::vector<std::string> errors;
        if (!dataflow::parse(text, program, errors)) {
            fail(errors.empty() ? "解析失败" : errors.front());
            continue;
        }
        // 1) 通用检查：两种求解器一致 + 单调 + 收敛
        std::string problem;
        std::size_t visits = 0;
        std::size_t naiveVisits = 0;
        for (const auto& name : dataflow::analysisNames()) {
            const auto analysis = dataflow::makeAnalysis(name, program);
            const auto worklist = dataflow::solveWorklist(program, *analysis);
            const auto naive = dataflow::solveNaive(program, *analysis);
            visits += worklist.blockVisits;
            naiveVisits += naive.blockVisits;
            if (!dataflow::sameSolution(worklist, naive, program)) {
                problem += name + " 两种求解器结果不一致；";
            }
            if (!worklist.monotone) problem += name + " 违反单调性；";
            if (!worklist.converged || !naive.converged) problem += name + " 没收敛；";
        }
        if (!problem.empty()) {
            fail(problem);
            continue;
        }
        // 2) 逐条比对期望
        bool wrong = false;
        for (const auto& expectation : meta.expectations) {
            const auto analysis = dataflow::makeAnalysis(expectation.analysis, program);
            if (!analysis) {
                fail("不认识的分析 " + expectation.analysis);
                wrong = true;
                break;
            }
            std::size_t index = program.size();
            for (std::size_t b = 0; b < program.size(); ++b) {
                if (blockName(program, b) == expectation.block) { index = b; break; }
            }
            if (index == program.size()) {
                fail("找不到块 " + expectation.block);
                wrong = true;
                break;
            }
            const auto solution = dataflow::solveWorklist(program, *analysis);
            const dataflow::Element& element = expectation.wantIn ? solution.in[index] : solution.out[index];
            const std::string actual = dataflow::formatElement(element);
            if (actual != expectation.value) {
                fail(expectation.analysis + " " + expectation.block + " " +
                     (expectation.wantIn ? "in" : "out") + " 是 " + actual + "，期望 " +
                     expectation.value);
                wrong = true;
                break;
            }
        }
        if (wrong) continue;
        ++passed;
        std::cout << "[通过] " << path.filename().string() << " —— "
                  << (meta.name.empty() ? path.filename().string() : meta.name);
        if (!meta.note.empty()) std::cout << "：" << meta.note;
        std::cout << "（" << meta.expectations.size() << " 条期望；4 个分析自洽，块访问 "
                  << visits << " vs 朴素 " << naiveVisits << "）\n";
    }
    std::cout << "共 " << files.size() << " 个示例：" << passed << " 通过，" << failed << " 失败。\n";
    return failed == 0 ? 0 : 1;
}
