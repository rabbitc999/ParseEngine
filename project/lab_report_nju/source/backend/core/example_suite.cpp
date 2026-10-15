// 示例集运行器：读取 example/*.quad，逐个优化 + 生成目标代码 + 模拟执行，
// 并与文件里声明的期望输出比对。
//
//   usage: transfer-examples [目录] [目标机]
//          （默认目录 example，默认目标机 mips；目标机取 mips / 8086 / arm）
//
// .quad 文件里可以用注释覆盖运行方式：
//   `# 配置: example/arm32.ini` 用配置文件生成（寄存器 / 字长 / 数据区）；
//   `# 目标: 8086`、`# 字长: 32` 直接构造一份默认配置；
//   `# 数据: x.data.bin` + `# 布局: x.layout` 装载数据区（addr / load / store 用）。
#include "example/quad_file.hpp"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {

transfer::MachineConfig configFor(const example::QuadFile& source, transfer::Target fallback) {
    if (!source.configPath.empty()) {
        return transfer::loadConfigFile(example::resolveConfigPath(source.path, source.configPath),
                                        fallback);
    }
    transfer::MachineConfig config = transfer::defaultConfig(
        source.targetName.empty() ? fallback : transfer::target(source.targetName));
    if (source.wordBits != 0) config.wordBits = source.wordBits;
    return config;
}

} // namespace

int main(int argc, char** argv) {
    example::useUtf8Console();
    const std::string directory = argc > 1 ? argv[1] : "example";
    const std::string targetName = argc > 2 ? argv[2] : "mips";
    const auto target = transfer::target(targetName);

    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (entry.path().extension() == ".quad") files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end());
    if (files.empty()) {
        std::cout << "目录 " << directory << " 里没有找到 .quad 示例。\n";
        return 1;
    }

    std::size_t passed = 0, failed = 0;
    std::cout << "示例目录：" << directory << "，目标机：" << transfer::name(target) << "\n";
    for (const auto& file : files) {
        try {
            const auto example = example::loadQuadFile(file.string());
            // `# 优化: 否` 的示例直接进后端（地址模型相关的收敛在优化器里还没做）
            transfer::Optimization optimized = transfer::optimizeDetailed(example.program);
            if (example.skipOptimization) {
                optimized.program = example.program;
                optimized.rounds = 0;
                optimized.trace.clear();
            }
            transfer::MachineConfig config = configFor(example, target);
            if (!example.dataPath.empty()) {
                transfer::loadDataArea(
                    config, example::resolveRelativePath(example.path, example.dataPath),
                    example.layoutPath.empty()
                        ? std::string()
                        : example::resolveRelativePath(example.path, example.layoutPath));
            }
            const auto assembly = transfer::generate(optimized.program, config);
            const auto execution = transfer::simulate(assembly, example.input);
            const std::string actual = example::renderOutput(execution);
            const auto errorStream = execution.streamText.find("STDERR");
            const std::string actualError =
                errorStream == execution.streamText.end() ? std::string() : errorStream->second;
            if (!example.expectError.empty()) {
                std::cout << "[失败] " << file.filename().string() << " —— 期望报错但运行成功\n";
                ++failed;
                continue;
            }
            const bool ok = actual == example.expect &&
                            (example.expectText.empty() || execution.text == example.expectText) &&
                            (example.expectErrorStream.empty() ||
                             actualError == example.expectErrorStream);
            std::cout << (ok ? "[通过] " : "[失败] ") << file.filename().string() << " —— "
                      << example.name;
            if (!example.configPath.empty()) std::cout << "（配置 " << example.configPath << "）";
            else if (example.wordBits != 0 || !example.targetName.empty())
                std::cout << "（目标机 " << transfer::name(config.target) << "，" << config.wordBits
                          << " 位）";
            std::cout << "\n";
            if (!ok) {
                std::cout << "        期望输出：" << example.expect << "，实际输出：" << actual << "\n";
                if (!example.expectText.empty() && execution.text != example.expectText)
                    std::cout << "        期望文本：" << example.expectText << "，实际文本："
                              << execution.text << "\n";
                if (!example.expectErrorStream.empty() && actualError != example.expectErrorStream)
                    std::cout << "        期望错误流：" << example.expectErrorStream
                              << "，实际错误流：" << actualError << "\n";
            }
            ok ? ++passed : ++failed;
        } catch (const std::exception& error) {
            // 期望报错的示例：错误信息包含关键字才算通过（用来区分
            // "四元式存在但机器没有该指令"和"未知四元式"两类错误）。
            const std::string message = error.what();
            std::string expected;
            try {
                expected = example::loadQuadFile(file.string(), false).expectError;
            } catch (const std::exception&) {
                expected.clear();
            }
            if (!expected.empty() && message.find(expected) != std::string::npos) {
                std::cout << "[通过] " << file.filename().string() << " —— 报了预期错误：" << message
                          << "\n";
                ++passed;
            } else {
                std::cout << "[失败] " << file.filename().string() << " —— " << message << "\n";
                ++failed;
            }
        }
    }
    std::cout << "共 " << files.size() << " 个示例：" << passed << " 通过，" << failed << " 失败。\n";
    return failed == 0 ? 0 : 1;
}
