// 单元测试：解析、活跃变量、干涉图、图着色、线性扫描、溢出重写与指标。
#include "regalloc.hpp"

#include <iostream>
#include <string>

namespace {

int failures = 0;
#define CHECK(condition)                                                          \
    do {                                                                          \
        if (!(condition)) {                                                       \
            std::cerr << "FAIL 第 " << __LINE__ << " 行: " #condition "\n";        \
            ++failures;                                                           \
        }                                                                         \
    } while (0)

regalloc::Program parseOrDie(const std::string& text) {
    regalloc::Program program;
    std::vector<std::string> errors;
    if (!regalloc::parse(text, program, errors)) {
        for (const auto& error : errors) std::cerr << "  解析错误: " << error << "\n";
        ++failures;
    }
    return program;
}

// 两个变量同时活跃：3 个寄存器够用，2 个就溢出。
const char* kPressure = R"(
label entry
  mov t1, 1
  mov t2, 2
  mov t3, 3
  add t4, t1, t2
  add t5, t4, t3
  store [r], t5
)";

// 复制链：mov 应该被合并，不额外占寄存器。
const char* kMoves = R"(
label entry
  mov a, 1
  mov b, a
  mov c, b
  add d, c, 1
  store [r], d
)";

// 循环：t1 跨回边活跃，活跃区间长，容易被溢出。
const char* kLoop = R"(
label L1
  add i, i, 1
  add s, s, i
  jz i, L2
  jmp L1
label L2
  store [r], s
)";

void testParse() {
    const auto program = parseOrDie(kPressure);
    CHECK(program.size() == 1);
    CHECK(program[0].code.size() == 6);
    const auto names = regalloc::virtualRegisters(program);
    CHECK(names.size() == 5); // t1..t5（内存符号 r 不算）
    const std::string text = regalloc::print(program);
    regalloc::Program again;
    std::vector<std::string> errors;
    CHECK(regalloc::parse(text, again, errors));
    CHECK(again.size() == program.size());
}

void testLiveness() {
    const auto program = parseOrDie(kPressure);
    const auto live = regalloc::liveness(program);
    CHECK(live.liveIn.size() == 1);
    CHECK(live.peakLive >= 3); // t1 t2 t3 同时活跃
    const auto graph = regalloc::buildInterference(program, live);
    CHECK(graph.degree("t1") >= 1);      // t1 与 t2 / t3 干涉
    CHECK(graph.degree("t2") >= 1);
    CHECK(graph.edgeCount() >= 2);
}

void testColors() {
    const auto program = parseOrDie(kPressure);
    // 4 个寄存器：应当没有溢出。
    const auto four = regalloc::allocate(program, regalloc::Machine::teaching(4), "chaitin");
    CHECK(four.allocation.spilled.empty());
    CHECK(four.stats.colorsUsed >= 3);
    CHECK(four.stats.instructionsAfter == four.stats.instructionsBefore);
    // 2 个寄存器：三个变量同时活跃 → 必须溢出并插入访存指令。
    const auto two = regalloc::allocate(program, regalloc::Machine::teaching(2), "chaitin");
    CHECK(two.stats.spilledRegisters > 0);
    CHECK(two.stats.spillLoads + two.stats.spillStores > 0);
    CHECK(two.stats.instructionsAfter > two.stats.instructionsBefore);
    CHECK(two.stats.memoryOpsAfter > two.stats.memoryOpsBefore);
}

void testCoalescing() {
    const auto program = parseOrDie(kMoves);
    const auto withCoalesce = regalloc::allocate(program, regalloc::Machine::teaching(2), "chaitin");
    regalloc::Options options;
    options.coalesce = false;
    const auto without = regalloc::allocate(program, regalloc::Machine::teaching(2), "chaitin", options);
    CHECK(withCoalesce.stats.coalescedMoves >= 1);
    CHECK(without.stats.coalescedMoves == 0);
    CHECK(withCoalesce.allocation.spilled.size() <= without.allocation.spilled.size());
    // 合并掉了就不该再把 `mov X, X` 留在输出里：指令数必须真的变少。
    const std::string text = regalloc::print(withCoalesce.rewritten.program);
    CHECK(text.find("mov A, A") == std::string::npos);
    CHECK(text.find("mov B, B") == std::string::npos);
    CHECK(withCoalesce.stats.eliminatedMoves >= 1);
    CHECK(withCoalesce.stats.instructionsAfter < withCoalesce.stats.instructionsBefore);
}

void testLinearScan() {
    const auto program = parseOrDie(kLoop);
    const auto chaitin = regalloc::allocate(program, regalloc::Machine::teaching(3), "chaitin");
    const auto linear = regalloc::allocate(program, regalloc::Machine::teaching(3), "linear");
    CHECK(chaitin.stats.algorithm == "chaitin");
    CHECK(linear.stats.algorithm == "linear");
    // 两种算法都必须给出可用的分配（溢出的用 load/store 补齐）。
    CHECK(linear.stats.instructionsAfter >= linear.stats.instructionsBefore);
    CHECK(!regalloc::print(chaitin.rewritten.program).empty());
}

void testRewrite() {
    const auto program = parseOrDie(kPressure);
    const auto result = regalloc::allocate(program, regalloc::Machine::teaching(2), "chaitin");
    const std::string text = regalloc::print(result.rewritten.program);
    // 重写后的程序里不应再出现被溢出的虚拟寄存器作为运算目标。
    for (const auto& name : result.allocation.spilled) {
        CHECK(text.find("add " + name) == std::string::npos);
    }
    CHECK(text.find("load SCRATCH") != std::string::npos ||
          text.find("store [spill.") != std::string::npos);
    const std::string table = regalloc::report(result, regalloc::Machine::teaching(2));
    CHECK(table.find("溢出寄存器：") != std::string::npos);
    CHECK(table.find("访存次数：") != std::string::npos);
}

void testErrors() {
    regalloc::Program program;
    std::vector<std::string> errors;
    CHECK(!regalloc::parse("label\n", program, errors));
    CHECK(!errors.empty());
    errors.clear();
    CHECK(!regalloc::parse("add t1, t2\n", program, errors));
    errors.clear();
    CHECK(!regalloc::parse("frob t1\n", program, errors));
}

} // namespace

int main() {
    testParse();
    testLiveness();
    testColors();
    testCoalescing();
    testLinearScan();
    testRewrite();
    testErrors();
    if (failures == 0) {
        std::cout << "regalloc-tests：全部通过\n";
        return 0;
    }
    std::cout << "regalloc-tests：" << failures << " 个断言失败\n";
    return 1;
}
