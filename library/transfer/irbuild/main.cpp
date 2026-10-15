// irbuild —— 用构建器造 IR（四元式 / 三元式），并打印三种形态与运行结果。
//
//   t1t irbuild [--kind=quad|triple] [--quiet]
#include "irbuild.hpp"
#include "module_main.hpp"
#include "ssa.hpp"
#include "transfer.hpp"

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

/** 用四元式构建器造 `1..n` 累加：与 examples/03_loop.quad 同一段逻辑。 */
t1t::QuadBuilder loopQuads() {
    t1t::QuadBuilder builder;
    builder.declare("i", "i16").declare("s", "i16").declare("t", "i16");
    builder.assign("0", "i").assign("0", "s");
    builder.label("loop");
    builder.add("i", "1", "i").add("s", "i", "s");
    builder.sub("5", "i", "t");
    builder.jumpIfZero("t", "done");
    builder.jump("loop");
    builder.label("done");
    builder.write("s").write("i").halt();
    return builder;
}

/** 同样一段逻辑，用三元式构建器写：结果用 `(k)` 引用。 */
t1t::TripleBuilder loopTriples() {
    t1t::TripleBuilder builder;
    builder.assign("0", "i");
    builder.assign("0", "s");
    builder.label("loop");
    const std::size_t inc = builder.arithmetic("+", "i", "1");
    builder.assign(t1t::TripleBuilder::ref(inc), "i");
    const std::size_t add = builder.arithmetic("+", "s", t1t::TripleBuilder::ref(inc));
    builder.assign(t1t::TripleBuilder::ref(add), "s");
    const std::size_t cmp = builder.arithmetic("-", "5", "i");
    builder.jumpIfZero(t1t::TripleBuilder::ref(cmp), "done");
    builder.jump("loop");
    builder.label("done");
    builder.write("s");
    builder.write("i");
    builder.halt();
    return builder;
}

} // namespace

int irbuildMain(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    std::string kind = "quad";
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            std::cout << "用法：t1t irbuild [--kind=quad|triple]\n"
                         "\n"
                         "用构建器造一段 1..5 累加，打印：原 IR 文本 → 统一 IR（TT3）→ 运行结果。\n"
                         "库接口见 irbuild/irbuild.hpp（QuadBuilder / TripleBuilder）。\n";
            return 0;
        }
        if (arg.rfind("--kind=", 0) == 0) { kind = arg.substr(7); continue; }
    }
    try {
        std::string quadText;
        t1t::TypedProgram typed;
        if (kind == "triple") {
            const t1t::TripleBuilder builder = loopTriples();
            std::cout << "=== 三元式（构建器产出）===\n" << builder.text();
            const irbridge::Quads quads = builder.toQuads();
            quadText = irbridge::toTransfer(quads);
            typed = builder.toTT3();
        } else {
            const t1t::QuadBuilder builder = loopQuads();
            std::cout << "=== 四元式（构建器产出）===\n" << builder.text();
            quadText = builder.text();
            typed = builder.lower();
        }
        std::cout << "=== 统一 IR（TT3，带位数）===\n" << t1t::print(typed);
        std::istringstream text(quadText);
        const transfer::Program program = transfer::read(text);
        const std::string assembly = transfer::generate(transfer::optimize(program),
                                                        transfer::defaultConfig(transfer::Target::Mips));
        const transfer::Execution execution = transfer::simulate(assembly);
        std::cout << "=== 运行结果 ===\n";
        for (const auto& value : execution.output) std::cout << transfer::displayValue(value) << " ";
        std::cout << "\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "t1t irbuild: " << error.what() << "\n";
        return 1;
    }
}
