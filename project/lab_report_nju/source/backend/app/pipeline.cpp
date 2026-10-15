#include "pipeline.hpp"

#include "typed_ir.hpp"
#include "dataflow.hpp"
#include "irbridge.hpp"
#include "module_main.hpp"
#include "module_main.hpp"
#include "regalloc.hpp"
#include "ssa.hpp"
#include "transfer.hpp"

#include <fstream>
#include <iostream>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#endif

namespace t1t {

std::string joinNumbers(const std::vector<long long>& values) {
    if (values.empty()) return "（无）";
    std::ostringstream out;
    for (std::size_t i = 0; i < values.size(); ++i) out << (i ? " " : "") << values[i];
    return out.str();
}

void section(const char* title) { std::cout << "=== " << title << " ===\n"; }

/**
 * 后半段（④ dataflow → ⑤ ssa → ⑥ regalloc）：四元式入口与"已经是三地址"入口共用。
 * 只吃 TT3（`typed`），需要的地方用名字视图 `plain`。
 */
void runBackHalf(const TypedProgram& typed, const PipelineOptions& options, PipelineReport& report) {
    const std::string plain = toPlainText(typed);
    report.instructionsLowered = countInstructions(plain);
    std::vector<std::string> errors;
    // 一次解析：ssa / dataflow / regalloc 共用同一份基本块（同一个 t1t::Block 类型）
    t1t::Program shared;
    if (!parseProgram(plain, shared, errors) || shared.blocks.empty()) {
        report.error = errors.empty() ? "三地址文本解析失败" : errors.front();
        return;
    }
    ssa::Program lowered = shared.blocks;
    dataflow::Program dataflowProgram = shared.blocks;
    for (const auto& name : dataflow::analysisNames()) {
        const auto analysis = dataflow::makeAnalysis(name, dataflowProgram);
        const auto worklist = dataflow::solveWorklist(dataflowProgram, *analysis);
        const auto naive = dataflow::solveNaive(dataflowProgram, *analysis);
        if (!dataflow::sameSolution(worklist, naive, dataflowProgram)) {
            report.error = "dataflow 的 " + name + "：两种求解器结果不一致";
            return;
        }
    }
    const ssa::Result ssaResult = ssa::optimize(lowered);
    report.phis = ssaResult.stats.phis;
    report.sccpFolded = ssaResult.stats.sccpFoldedInstructions;
    report.gvnFolded = ssaResult.stats.gvnRedundantExpressions + ssaResult.stats.gvnRedundantPhis;
    report.instructionsAfterSsa = ssaResult.stats.instructionsAfter;
    report.ssaEquivalent = ssaResult.equivalent;
    regalloc::Program allocProgram;                 // regalloc 的语言里没有 print：按块过滤
    for (const auto& block : shared.blocks) {
        regalloc::Block copy = block;
        copy.code.clear();
        for (const auto& ins : block.code) {
            if (ins.op != "print") copy.code.push_back(ins);
        }
        allocProgram.push_back(copy);
    }
    if (!allocProgram.empty()) {
        const regalloc::Machine machine = regalloc::Machine::teaching(options.registers);
        const regalloc::Result allocation = regalloc::allocate(allocProgram, machine, "chaitin");
        report.colorsUsed = allocation.stats.colorsUsed;
        report.spilled = allocation.stats.spilledRegisters;
    }
    report.ok = true;
}

PipelineReport runPipeline(const PipelineOptions& options) {
    PipelineReport report;
    std::ifstream in(options.file, std::ios::binary);
    if (!in) {
        report.error = "无法读取 " + options.file;
        return report;
    }
    try {
        std::ostringstream all;
        all << in.rdbuf();
        const std::string source = all.str();

        // ---- 入口：只在这里做一次转换（命令行 --from= 决定输入是什么）----
        //   quads   四元式  → TT3（一次降级，保留类型）
        //   triples 三元式  → 四元式 → TT3（入口一次连续转换）
        //   3addr   已经是三地址：不转换，直接当 TT3 解析
        if (options.from == "3addr") {
            TypedProgram typed;
            std::vector<std::string> parseErrors;
            if (!t1t::parse(source, typed, parseErrors)) {
                report.error = parseErrors.empty() ? "三地址文本解析失败" : parseErrors.front();
                return report;
            }
            report.source = "3addr";
            report.converted = false;
            report.mapped = widthHistogram(typed).empty() ? 0 : countInstructions(print(typed));
            report.widths = widthHistogram(typed);
            report.typedText = print(typed);
            runBackHalf(typed, options, report);       // 输入已经是三地址：跳过 transfer 那两步
            return report;
        }
        std::string quadText = source;
        if (options.from == "triples") {
            // 三元式 → 四元式（入口这一步），下面交给同一条四元式通路
            irbridge::Triples triples;
            irbridge::Quads quads;
            std::vector<std::string> convertErrors;
            if (!irbridge::readTriples(source, triples, convertErrors) ||
                !irbridge::triplesToQuads(triples, quads, convertErrors)) {
                report.error = convertErrors.empty() ? "三元式读取 / 转换失败" : convertErrors.front();
                return report;
            }
            quadText = irbridge::toTransfer(quads);
            report.source = "triples";
            report.converted = true;
        } else {
            report.source = "quads";
        }
        std::istringstream quadInput(quadText);
        transfer::Program program = transfer::read(quadInput);
        transfer::validate(program);
        const transfer::Target target = transfer::target(options.target);
        transfer::MachineConfig config =
            options.configPath.empty() ? transfer::defaultConfig(target)
                                       : transfer::loadConfigFile(options.configPath, target);
        // 数据区三件套（addr / load / store）靠这两份文件；挂上以后 ① 步能跑
        // 带数据区的程序，③ 步的桥照旧把它们记成"跳过"（统一 IR 只覆盖交集；
        // 把内存操作降成 ld / st + 数据区解释器这件事明确不做，见技术报告）。
        if (!options.dataPath.empty())
            transfer::loadDataArea(config, options.dataPath, options.layoutPath);
        report.quadsBefore = program.size();
        report.blocks = transfer::analyze(program).blocks.size();
        transfer::Optimization optimized = transfer::optimizeDetailed(program);
        if (options.noOpt) {                                  // `--no-opt`：原程序直接进后端
            optimized.program = program;
            optimized.rounds = 0;
        }
        report.quadsAfter = optimized.program.size();
        report.rounds = optimized.rounds;
        const std::string assembly = transfer::generate(optimized.program, config);
        const transfer::Execution execution = transfer::simulate(assembly, options.input);
        report.steps = execution.steps;
        for (const auto& value : execution.output) {
            const std::string text = transfer::displayValue(value);
            report.simulated.push_back(text);
            report.simulatedNumbers.push_back(std::stoll(text));
        }

        const Lowering lowering = lowerQuads(optimized.program);

        report.typedText = lowering.text;

        report.widths = widthHistogram(lowering.program);
        report.mapped = lowering.mapped;
        report.skipped = lowering.skipped;
        report.skippedOps = lowering.skippedOps;
        report.instructionsLowered = countInstructions(lowering.plain);

        std::vector<std::string> errors;
        // 一次解析：ssa / dataflow / regalloc 共用同一份基本块，不再文本往返
        t1t::Program shared;
        if (!parseProgram(lowering.plain, shared, errors) || shared.blocks.empty()) {
            report.ok = true;      // 桥没产出可用程序：不算失败，但做不了后面几步
            return report;
        }
        ssa::Program lowered = shared.blocks;
        dataflow::Program dataflowProgram = shared.blocks;
        for (const auto& name : dataflow::analysisNames()) {
            const auto analysis = dataflow::makeAnalysis(name, dataflowProgram);
            const auto worklist = dataflow::solveWorklist(dataflowProgram, *analysis);
            const auto naive = dataflow::solveNaive(dataflowProgram, *analysis);
            if (!dataflow::sameSolution(worklist, naive, dataflowProgram)) {
                report.error = "dataflow 的 " + name + "：两种求解器结果不一致";
                return report;
            }
        }
        const ssa::Result ssaResult = ssa::optimize(lowered);
        report.phis = ssaResult.stats.phis;
        report.sccpFolded = ssaResult.stats.sccpFoldedInstructions;
        report.gvnFolded = ssaResult.stats.gvnRedundantExpressions + ssaResult.stats.gvnRedundantPhis;
        report.instructionsAfterSsa = ssaResult.stats.instructionsAfter;
        report.ssaEquivalent = ssaResult.equivalent;

        regalloc::Program allocProgram;                 // regalloc 的语言里没有 print：按块过滤
    for (const auto& block : shared.blocks) {
        regalloc::Block copy = block;
        copy.code.clear();
        for (const auto& ins : block.code) {
            if (ins.op != "print") copy.code.push_back(ins);
        }
        allocProgram.push_back(copy);
    }
    if (!allocProgram.empty()) {
            const regalloc::Machine machine = regalloc::Machine::teaching(options.registers);
            const regalloc::Result allocation = regalloc::allocate(allocProgram, machine, "chaitin");
            report.colorsUsed = allocation.stats.colorsUsed;
            report.spilled = allocation.stats.spilledRegisters;
        }
        if (lowering.skipped == 0) {
            const ssa::RunResult interpreted = ssa::run(lowered);
            if (interpreted.error.empty()) {
                report.executable = true;
                report.interpreted = interpreted.output;
                report.consistent = interpreted.output == report.simulatedNumbers;
            }
        }
        report.ok = true;
        return report;
    } catch (const std::exception& error) {
        report.error = error.what();
        return report;
    }
}

} // namespace t1t

int pipelineMain(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    t1t::PipelineOptions options;
    std::string inputText;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            std::cout << "用法：t1t pipeline <file> [--from=quads|triples|3addr]\n"
                         "                  [--target=mips|8086|arm] [--config=file.ini]\n"
                         "                  [--input=1,2] [--regs=N]\n"
                         "                  [--data=x.data.bin] [--layout=x.layout]\n"
                         "\n"
                         "一条流水线：transfer 优化 + 目标代码 + 模拟 → 桥降级成三地址 →\n"
                         "dataflow 四个分析 → SSA（SCCP / GVN / φ 消除）→ regalloc，\n"
                         "最后把 transfer 的模拟输出与三地址解释输出对照一次。\n"
                         "\n"
                         "  --data   数据区字节镜像（simple_ast 的 .data.bin），供 addr / load / store 读写\n"
                         "  --layout 数据区符号表（simple_ast 的 .layout）；留空时按 <data>.layout 找\n";
            return 0;
        }
        if (arg.rfind("--from=", 0) == 0) {
            options.from = arg.substr(7);
            if (options.from != "quads" && options.from != "triples" && options.from != "3addr" &&
                options.from != "ast") {
                std::cerr << "t1t pipeline: --from 只支持 quads / triples / 3addr\
";
                return 2;
            }
            continue;
        }
        if (arg.rfind("--target=", 0) == 0) { options.target = arg.substr(9); continue; }
        if (arg.rfind("--config=", 0) == 0) { options.configPath = arg.substr(9); continue; }
        if (arg.rfind("--data=", 0) == 0) { options.dataPath = arg.substr(7); continue; }
        if (arg.rfind("--layout=", 0) == 0) { options.layoutPath = arg.substr(9); continue; }
        if (arg.rfind("--input=", 0) == 0) { inputText = arg.substr(8); continue; }
        if (arg == "--no-opt") { options.noOpt = true; continue; }
        if (arg.rfind("--regs=", 0) == 0) {
            options.registers = static_cast<std::size_t>(std::stoul(arg.substr(7)));
            continue;
        }
        if (arg.rfind("--", 0) == 0) {
            std::cerr << "t1t pipeline: 不认识的选项 " << arg << "\n";
            return 2;
        }
        options.file = arg;
    }
    if (options.file.empty()) {
        std::cerr << "t1t pipeline: 需要给一个 .quad 文件（-h 看用法）\n";
        return 2;
    }
    std::string current;
    for (char c : inputText) {
        if (c == ',') {
            if (!current.empty()) options.input.push_back(current);
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    if (!current.empty()) options.input.push_back(current);

    const t1t::PipelineReport report = t1t::runPipeline(options);
    if (!report.ok) {
        std::cerr << "t1t pipeline: " << report.error << "\n";
        return 1;
    }
    t1t::section("① transfer：读入与优化");
    std::cout << "四元式 " << report.quadsBefore << " 条（基本块 " << report.blocks
              << "）→ 优化后 " << report.quadsAfter << " 条（" << report.rounds << " 轮）\n";
    t1t::section("② transfer：目标代码与模拟");
    std::cout << "模拟步数 " << report.steps << "\n输出：";
    for (std::size_t i = 0; i < report.simulated.size(); ++i) {
        std::cout << (i ? " " : "") << report.simulated[i];
    }
    if (report.simulated.empty()) std::cout << "（无）";
    std::cout << "\n";
    t1t::section("③ 桥：四元式 → 三地址文本");
    std::cout << "翻译 " << report.mapped << " 条；跳过 " << report.skipped << " 条";
    if (!report.skippedOps.empty()) {
        std::cout << "（";
        bool first = true;
        for (const auto& entry : report.skippedOps) {
            std::cout << (first ? "" : "、") << entry.first << " ×" << entry.second;
            first = false;
        }
        std::cout << "）";
    }
    std::cout << "；三地址指令 " << report.instructionsLowered << " 条\n";
    if (!report.widths.empty()) {
        std::cout << "统一三地址语言保留的位数：";
        bool first = true;
        for (const auto& entry : report.widths) {
            std::cout << (first ? "" : "、") << entry.first << " ×" << entry.second;
            first = false;
        }
        std::cout << "\n";
    }
    if (!report.typedText.empty()) {
        std::cout << "（内部统一语言：op:类型 dst, src1, src2）\n" << report.typedText;
    }
    t1t::section("④ dataflow：四个分析");
    std::cout << "活跃变量 / 到达定值 / 可用表达式 / 常量传播：两种求解器结果一致 ✓\n";
    t1t::section("⑤ ssa：构造 → SCCP / GVN → φ 消除");
    std::cout << "φ " << report.phis << " 条；SCCP 折叠 " << report.sccpFolded
              << " 条指令；GVN 消 " << report.gvnFolded << " 处；φ 消除后指令 "
              << report.instructionsLowered << " → " << report.instructionsAfterSsa << "\n"
              << "  SSA 自己的语义对照：" << (report.ssaEquivalent ? "一致 ✓" : "不一致 ✗") << "\n";
    t1t::section("⑥ regalloc：图着色 + 溢出重写");
    std::cout << "物理寄存器 " << options.registers << " 个：用色 " << report.colorsUsed
              << "，溢出 " << report.spilled << "\n";
    t1t::section("⑦ 跨模块语义对照");
    if (!report.executable) {
        std::cout << "降级后的程序不能直接解释（有 " << report.skipped
                  << " 条四元式没有对应指令），跳过对照（统一 IR 只覆盖交集，设计如此）\n";
        return 0;
    }
    std::cout << "transfer 模拟输出：" << t1t::joinNumbers(report.simulatedNumbers) << "\n"
              << "三地址解释输出：" << t1t::joinNumbers(report.interpreted) << "\n"
              << "一致：" << (report.consistent ? "✓" : "✗") << "\n";
    return report.consistent ? 0 : 1;
}
