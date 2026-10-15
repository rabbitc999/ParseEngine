#pragma once
// 端到端流水线：四元式 → transfer 优化 / 目标代码 / 模拟 → 桥 → 三地址 →
// dataflow → ssa → regalloc，最后把 transfer 的模拟输出与三地址解释输出对照。
//
// 逻辑放在函数里（不只是写在命令行里），examples/pipeline_suite.cpp 直接调用
// 同一份实现做自检。
#include <cstddef>
#include <map>
#include <string>
#include <vector>

namespace t1t {

struct PipelineOptions {
    std::string file;
    /**
     * 输入语言：quads（四元式，缺省）/ triples（三元式）/ 3addr（已经是三地址）。
     */
    std::string from = "quads";
    /** `--no-opt`：跳过 transfer 的优化（地址模型相关的收敛还没做，调试时用）。 */
    bool noOpt = false;
    std::string target = "mips";
    std::string configPath;
    /** `--data=`：数据区字节镜像（`simple_ast` 的 `.data.bin`），供 `addr` / `load` / `store` 用。 */
    std::string dataPath;
    /** `--layout=`：数据区符号基址表（`.layout`）；留空时按 `<data>.layout` 找。 */
    std::string layoutPath;
    std::vector<std::string> input;
    std::size_t registers = 4;
};

struct PipelineReport {
    bool ok = false;                        // 整条流水线跑通（没有异常）
    std::string error;
    std::string source;                     // 这次实际用的输入语言
    bool converted = false;                 // 入口是否做过一次转换
    std::size_t blocks = 0;
    std::size_t quadsBefore = 0;
    std::size_t quadsAfter = 0;
    std::size_t rounds = 0;
    std::size_t steps = 0;                  // transfer 模拟步数
    std::vector<std::string> simulated;     // transfer 的 write 输出（文本）
    std::vector<long long> simulatedNumbers;
    std::size_t mapped = 0;                 // 桥翻译成功的四元式条数
    std::size_t skipped = 0;                // 桥跳过的条数
    std::map<std::string, std::size_t> skippedOps;
    std::string typedText;                  // TT3 文本（带 :类型）
    std::map<std::string, std::size_t> widths;   // 类型 → 指令条数
    std::size_t phis = 0;
    std::size_t sccpFolded = 0;
    std::size_t gvnFolded = 0;
    std::size_t instructionsLowered = 0;
    std::size_t instructionsAfterSsa = 0;
    bool ssaEquivalent = false;
    std::size_t colorsUsed = 0;
    std::size_t spilled = 0;
    bool executable = false;                // 降级后的程序能不能解释执行
    std::vector<long long> interpreted;
    bool consistent = false;                // 两种执行结果是否一致
};

/**
 * 跑完整条流水线：transfer 优化 + 目标代码 + 模拟 → 桥降级 → dataflow → ssa → regalloc，并做跨模块语义对照。。
 *
 * @param options 输入文件、目标机、寄存器数等选项
 * @return 各步指标与对照结果
 */
PipelineReport runPipeline(const PipelineOptions& options);

} // namespace t1t
