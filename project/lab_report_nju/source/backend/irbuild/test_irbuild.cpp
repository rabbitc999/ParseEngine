// 单元测试：两个构建器的产出（文本 / 统一 IR / 转换 / 运行结果）与往返稳定性。
#include "irbuild.hpp"
#include "ssa.hpp"
#include "transfer.hpp"

#include <iostream>
#include <sstream>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

int failures = 0;
#define CHECK(condition)                                                          \
    do {                                                                          \
        if (!(condition)) {                                                       \
            std::cerr << "FAIL 第 " << __LINE__ << " 行: " #condition "\n";        \
            ++failures;                                                           \
        }                                                                         \
    } while (0)

/** 1..5 累加，四元式构建器。 */
t1t::QuadBuilder quads() {
    t1t::QuadBuilder builder;
    builder.declare("i", "i16").declare("s", "i16").declare("t", "i16");
    builder.assign("0", "i").assign("0", "s");
    builder.label("loop");
    builder.add("i", "1", "i").add("s", "i", "s").sub("5", "i", "t");
    builder.jumpIfZero("t", "done");
    builder.jump("loop");
    builder.label("done");
    builder.write("s").write("i").halt();
    return builder;
}

/** 同一段逻辑，三元式构建器。 */
t1t::TripleBuilder triples() {
    t1t::TripleBuilder builder;
    builder.assign("0", "i");
    builder.assign("0", "s");
    builder.label("loop");
    const std::size_t inc = builder.arithmetic("+", "i", "1");
    builder.assign(t1t::TripleBuilder::ref(inc), "i");
    const std::size_t sum = builder.arithmetic("+", "s", t1t::TripleBuilder::ref(inc));
    builder.assign(t1t::TripleBuilder::ref(sum), "s");
    const std::size_t cmp = builder.arithmetic("-", "5", "i");
    builder.jumpIfZero(t1t::TripleBuilder::ref(cmp), "done");
    builder.jump("loop");
    builder.label("done");
    builder.write("s");
    builder.write("i");
    builder.halt();
    return builder;
}

std::vector<std::string> run(const std::string& quadText) {
    std::istringstream text(quadText);
    const transfer::Program program = transfer::read(text);
    const std::string assembly =
        transfer::generate(transfer::optimize(program), transfer::defaultConfig(transfer::Target::Mips));
    std::vector<std::string> output;
    for (const auto& value : transfer::simulate(assembly).output) {
        output.push_back(transfer::displayValue(value));
    }
    return output;
}

void testQuadBuilder() {
    const t1t::QuadBuilder builder = quads();
    CHECK(builder.size() == 15);                       // 3 decl + 2 assign + 10 指令
    const std::string text = builder.text();
    CHECK(text.find("(label, _, _, loop)") != std::string::npos);
    CHECK(text.find("(jz, t, _, done)") != std::string::npos);
    // 文本 → 读回 → 再写回：格式稳定
    std::istringstream in(text);
    CHECK(transfer::write(transfer::read(in)) == text);
    // 运行结果
    const std::vector<std::string> output = run(text);
    CHECK(output.size() == 2);
    CHECK(output[0] == "15");
    CHECK(output[1] == "5");
    // 统一 IR：声明与位数都在
    const t1t::TypedProgram typed = builder.lower();
    const std::string lower = t1t::print(typed);
    CHECK(lower.find("decl i, i16") != std::string::npos);
    CHECK(lower.find("add:i16 i, i, 1") != std::string::npos);
    CHECK(t1t::widthHistogram(typed).count("i16") == 1);
}

void testTripleBuilder() {
    const t1t::TripleBuilder builder = triples();
    CHECK(builder.size() == 14);
    const std::string text = builder.text();
    CHECK(text.find("(:=, 0, i)") != std::string::npos);        // 三元的赋值：目标在第三位
    CHECK(text.find("(jz, (7), done)") != std::string::npos);   // 结果用下标引用
    CHECK(text.find("(label, loop, _)") != std::string::npos);
    // 三元式 → 四元式 → 文本 → 运行
    const std::string quadText = irbridge::toTransfer(builder.toQuads());
    const std::vector<std::string> output = run(quadText);
    CHECK(output.size() == 2);
    CHECK(output[0] == "15");
    CHECK(output[1] == "5");
    // 三元式 → 统一 IR：位数、指令都对得上
    const t1t::TypedProgram typed = builder.toTT3();
    const std::string lower = t1t::print(typed);
    CHECK(lower.find("jz:i16 t") != std::string::npos);
    CHECK(lower.find("jmp:i16 loop") != std::string::npos);
    CHECK(t1t::countInstructions(t1t::toPlainText(typed)) >= 10);
    CHECK(t1t::countInstructions(quads().lowerText()) >= 10);
}

void testTripleErrors() {
    t1t::TripleBuilder builder;
    builder.jumpIfZero("(99)", "L1");      // 引用越界：转四元式必须报错
    bool threw = false;
    try {
        builder.toQuads();
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);
}

} // namespace

int main() {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    testQuadBuilder();
    testTripleBuilder();
    testTripleErrors();
    if (failures == 0) {
        std::cout << "irbuild-tests：全部通过\n";
        return 0;
    }
    std::cout << "irbuild-tests：" << failures << " 个断言失败\n";
    return 1;
}
