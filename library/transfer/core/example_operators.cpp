// 移位 / 按位运算示例：读 example/operators.quad，在 MIPS / 8086 / ARM 三种
// 目标机上生成代码并模拟，验证结果一致（与文件里的"期望"行比对）。
//
//   usage: transfer-operators-example [四元式文件]
#include "example/quad_file.hpp"

#include <iostream>
#include <string>

namespace {

bool run(transfer::Target target, const example::QuadFile& source,
         const transfer::Program& program) {
    const auto assembly = transfer::generate(program, target);
    const auto execution = transfer::simulate(assembly, source.input);
    const std::string actual = example::renderOutput(execution);
    const bool ok = actual == source.expect;
    std::cout << "\n———— " << transfer::name(target) << " ————\n" << assembly;
    std::cout << "运行结果：" << actual << "（指令数 " << execution.steps << "）"
              << (ok ? "  [与期望一致]" : "  [与期望不一致]") << "\n";
    return ok;
}

} // namespace

int main(int argc, char** argv) {
    example::useUtf8Console();
    try {
        const std::string path = argc > 1 ? argv[1] : "example/operators.quad";
        const auto source = example::loadQuadFile(path);
        const auto optimized = transfer::optimizeDetailed(source.program);

        std::cout << "【示例】" << source.name << "（" << path << "）\n";
        std::cout << "【优化后四元式】\n";
        for (const auto& step : optimized.trace) std::cout << "  " << step << "\n";
        std::cout << transfer::write(optimized.program);

        bool ok = true;
        for (const auto target :
             {transfer::Target::Mips, transfer::Target::I8086, transfer::Target::Arm}) {
            ok = run(target, source, optimized.program) && ok;
        }
        std::cout << (ok ? "\n三种目标机结果一致。\n" : "\n存在不一致结果。\n");
        return ok ? 0 : 1;
    } catch (const std::exception& error) {
        std::cout << "出错：" << error.what() << "\n";
        return 1;
    }
}
