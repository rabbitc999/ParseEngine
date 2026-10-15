// 最小示例：从 .quad 文件读四元式 -> 优化 -> 生成 MIPS -> 模拟执行。
//
//   usage: transfer-example [四元式文件]   默认 example/minimal.quad
#include "example/quad_file.hpp"

#include <iostream>
#include <string>

int main(int argc, char** argv) {
    example::useUtf8Console();
    try {
        const std::string path = argc > 1 ? argv[1] : "example/minimal.quad";
        const auto source = example::loadQuadFile(path);

        std::cout << "【读入的示例】" << source.name << "（" << path << "）\n"
                  << transfer::write(source.program);

        const auto optimized = transfer::optimizeDetailed(source.program);
        std::cout << "【优化后四元式】\n" << transfer::write(optimized.program);

        const auto assembly = transfer::generate(optimized.program, transfer::Target::Mips);
        std::cout << "【生成的 MIPS 汇编】\n" << assembly;

        const auto execution = transfer::simulate(assembly, source.input);
        std::cout << "【运行结果】" << example::renderOutput(execution) << "\n";
        if (!execution.text.empty()) std::cout << "【文本输出】" << execution.text << "\n";
        return 0;
    } catch (const std::exception& error) {
        std::cout << "出错：" << error.what() << "\n";
        return 1;
    }
}
