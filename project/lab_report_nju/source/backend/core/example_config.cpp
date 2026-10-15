// 外部配置示例：读 example/config.quad 与 example_machine.ini（寄存器、
// 指令模板、Bin 数据区、只读区），打印 CFG / 数据流报告，再生成 ARM 汇编
// 并模拟执行。
//
//   usage: transfer-config-example [四元式文件] [配置文件]
#include "example/quad_file.hpp"

#include <iostream>
#include <string>

int main(int argc, char** argv) {
    example::useUtf8Console();
    try {
        const std::string quadPath = argc > 1 ? argv[1] : "example/config.quad";
        const std::string configPath = argc > 2 ? argv[2] : "example_machine.ini";
        const auto source = example::loadQuadFile(quadPath);
        const auto config = transfer::loadConfigFile(configPath);
        const auto analysis = transfer::analyze(source.program);
        const auto optimized = transfer::optimizeDetailed(source.program);

        std::cout << "【示例】" << source.name << "（" << quadPath << "）\n"
                  << "【机器配置】" << configPath << "（目标机 " << transfer::name(config.target)
                  << "，字长 " << config.wordBits << " 位）\n";
        std::cout << "【CFG 与数据流报告】\n"
                  << transfer::report(source.program, analysis)
                  << "\n【优化后四元式】\n"
                  << transfer::write(optimized.program);

        const auto assembly = transfer::generate(optimized.program, config);
        std::cout << "【按配置生成的 ARM 汇编】\n" << assembly;

        const auto execution = transfer::simulate(assembly, source.input);
        std::cout << "【运行结果】" << example::renderOutput(execution) << "\n";
        if (!execution.text.empty()) std::cout << "【文本输出】" << execution.text << "\n";
        return 0;
    } catch (const std::exception& error) {
        std::cout << "出错：" << error.what() << "\n";
        return 1;
    }
}
