#pragma once
// 示例程序共用的小工具：从 .quad 文件读取四元式与元信息（名称 / 输入 /
// 期望输出），并把模拟结果渲染成可比较的文本。
//
// .quad 文件就是普通四元式文本，元信息写在 `#` 注释里：
//     # 名称: 无符号乘法
//     # 输入: 300
//     # 期望: 75
//     # 期望文本: ok
//     # 配置: example/arm32.ini      （机器配置，可选）
//     # 数据: example/x.data.bin     （数据区字节镜像，可选）
//     # 布局: example/x.layout       （数据区符号表，可选）
#include "transfer.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace example {

struct QuadFile {
    /** 这个 .quad 文件自己的路径（`# 配置:` 的相对路径按它解析）。 */
    std::string path;
    std::string name;
    std::string expect;
    std::string expectText;
    /** 可选的机器配置文件（`# 配置: example/arm32.ini`）。 */
    std::string configPath;
    /** 可选的数据区字节镜像（`# 数据: example/x.data.bin`，给 addr / load / store 用）。 */
    std::string dataPath;
    /** 可选的数据区符号表（`# 布局: example/x.layout`）；留空时按 `<数据>.layout` 找。 */
    std::string layoutPath;
    /** 期望报错：生成 / 校验失败且信息包含这个子串时算通过。 */
    std::string expectError;
    /** 期望的 STDERR 文本（非 STDOUT 的流输出）。 */
    std::string expectErrorStream;
    /** 可选的目标机 / 字长覆盖（`# 目标: arm`、`# 字长: 32`）。 */
    std::string targetName;
    unsigned wordBits = 0;
    /** `# 优化: 否`：这份示例跑后端时**跳过优化**（后端优化器对某些地址模型会过度折叠）。 */
    bool skipOptimization = false;
    std::vector<std::string> input;
    transfer::Program program;
};

inline std::string trimCopy(const std::string& text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}

/**
 * `# 配置:` / `# 数据:` / `# 布局:` 里写的是相对路径时：先按当前目录找，
 * 找不到就相对这个 .quad 文件所在目录再找一次。合并进 transfer-one-thread
 * 之后示例放在 `core/example/`，元信息里的路径写法保持不变，靠这一步解析。
 */
inline std::string resolveRelativePath(const std::string& quadFile, const std::string& path) {
    if (path.empty()) return path;
    {
        std::ifstream direct(path, std::ios::binary);
        if (direct) return path;
    }
    const std::filesystem::path base(quadFile);
    const std::filesystem::path parent = base.has_parent_path() ? base.parent_path()
                                                                : std::filesystem::path();
    // 依次试：相对 .quad 目录的原路径、以及只用文件名的形式
    //（示例里的写法是 `example/arm32.ini`，合并后文件就在同一个目录下）。
    for (const auto& candidate : {parent / path, parent / std::filesystem::path(path).filename()}) {
        std::ifstream alternative(candidate.string(), std::ios::binary);
        if (alternative) return candidate.string();
    }
    return path;
}

/** `# 配置:` 的路径解析，规则同 resolveRelativePath。 */
inline std::string resolveConfigPath(const std::string& quadFile, const std::string& configPath) {
    return resolveRelativePath(quadFile, configPath);
}

/** Windows 控制台切到 UTF-8，中文输出才不会乱码。 */
inline void useUtf8Console() {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif
}

/** 读取 .quad 文件：注释里的元信息由本函数解析，正文交给 transfer::read。 */
inline QuadFile loadQuadFile(const std::string& path, bool parseProgram = true) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("无法打开四元式文件：" + path);
    QuadFile out;
    out.path = path;
    std::ostringstream body;
    for (std::string line; std::getline(in, line);) {
        const std::string trimmed = trimCopy(line);
        if (!trimmed.empty() && trimmed[0] == '#') {
            const auto colon = trimmed.find(':');
            if (colon != std::string::npos) {
                const std::string key = trimCopy(trimmed.substr(1, colon - 1));
                const std::string value = trimCopy(trimmed.substr(colon + 1));
                if (key == "名称") out.name = value;
                else if (key == "期望") out.expect = value;
                else if (key == "期望文本") out.expectText = value;
                else if (key == "配置") out.configPath = value;
                else if (key == "数据") out.dataPath = value;
                else if (key == "布局") out.layoutPath = value;
                else if (key == "期望错误") out.expectError = value;
                else if (key == "期望错误流") out.expectErrorStream = value;
                else if (key == "目标") out.targetName = value;
                else if (key == "优化") out.skipOptimization = (value == "否" || value == "no");
                else if (key == "字长") {
                    try {
                        out.wordBits = static_cast<unsigned>(std::stoul(value));
                    } catch (const std::exception&) {
                        out.wordBits = 0;
                    }
                }
                else if (key == "输入") {
                    std::istringstream items(value);
                    for (std::string item; items >> item;) out.input.push_back(item);
                }
            }
        }
        body << line << '\n';
    }
    if (parseProgram) {
        std::istringstream text(body.str());
        out.program = transfer::read(text);
    }
    if (out.name.empty()) out.name = path;
    return out;
}

/** 数值输出渲染成空格分隔的文本。 */
inline std::string renderOutput(const transfer::Execution& execution) {
    std::string text;
    for (const auto& value : execution.output) {
        if (!text.empty()) text += ' ';
        text += transfer::displayValue(value);
    }
    return text;
}

} // namespace example
