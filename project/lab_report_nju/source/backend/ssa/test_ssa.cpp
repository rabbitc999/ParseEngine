// 单元测试：解析、CFG、支配树 / 支配边界、φ 插入与改名、SCCP、GVN、
// φ 消除（含切临界边与复制环）以及"优化前后语义一致"的对照。
#include "ssa.hpp"

#include <iostream>
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

ssa::Program parseOrDie(const std::string& text) {
    ssa::Program program;
    std::vector<std::string> errors;
    if (!ssa::parse(text, program, errors)) {
        for (const auto& error : errors) std::cerr << "  解析错误: " << error << "\n";
        ++failures;
    }
    return program;
}

// 菱形：y 在两条路径上各定义一次，汇合点要插 φ。
const char* kDiamond = R"(
label entry
  mov x, 1
  jz x, L1
  mov y, 2
  jmp L2
label L1
  mov y, 3
label L2
  print y
  halt
)";

// 常量条件：SCCP 应该把 L1 判成不可达。
const char* kConstBranch = R"(
label entry
  mov x, 1
  jz x, dead
  mov y, 2
  jmp out
label dead
  mov y, 3
label out
  print y
  halt
)";

// 循环 + 交换：回边上的 φ 实参互相引用，φ 消除时必须借临时变量。
const char* kSwap = R"(
label entry
  mov flag, 5
  mov a, 1
  mov b, 2
  mov n, 0
  jz flag, done
label loop
  mov t, a
  mov a, b
  mov b, t
  add n, n, 1
  mov k, n
  sub k, k, 3
  jnz k, loop
label done
  print a
  print b
  print n
  halt
)";

// 菱形（条件来自 load，两条路径都活着）：汇合点的 φ 有一个前驱是"顺序落下"的块，
// SCCP 必须把顺序边也算成可执行边。
const char* kDiamondLoad = R"(
label entry
  load x, [slot]
  jz x, L1
  mov y, 2
  jmp L2
label L1
  mov y, 3
label L2
  print y
  halt
)";

// 重复表达式 + 复制 + 冗余 φ：值来自 load，SCCP 折不动，留给 GVN。
const char* kRedundant = R"(
label entry
  mov a, 7
  mov b, 5
  add s, a, b
  load x, [slot]
  jz x, L1
  mov t, s
  jmp L2
label L1
  mov t, s
label L2
  add r, t, s
  add q, s, s
  print r
  print q
  halt
)";

void testParse() {
    const auto program = parseOrDie(kDiamond);
    CHECK(program.size() == 4);
    CHECK(program[0].successors.size() == 2);            // jz 目标 + 顺序落下的块
    CHECK(program[3].predecessors.size() == 2);
    const std::string text = ssa::print(program);
    ssa::Program again;
    std::vector<std::string> errors;
    CHECK(ssa::parse(text, again, errors));
    CHECK(again.size() == program.size());
}

void testCfg() {
    const auto program = parseOrDie(kConstBranch);
    const auto cfg = ssa::buildCfg(program);
    CHECK(cfg.rpo.size() == program.size());             // 4 个块都可达
    CHECK(cfg.unreachableBlocks == 0);
    CHECK(cfg.edgeCount == 4);
}

void testDominators() {
    const auto program = parseOrDie(kDiamond);
    const auto cfg = ssa::buildCfg(program);
    const auto dom = ssa::analyzeDominators(program, cfg);
    CHECK(dom.idom[0] == 0);
    CHECK(dom.dominates(0, 3));                          // 入口支配汇合点
    CHECK(dom.dominates(3, 3));
    CHECK(!dom.dominates(1, 2));                         // 两条分支互不支配
    CHECK(dom.frontier[0].empty());                      // 入口的直接支配边界是空
    CHECK(dom.children[0].size() == 3);                  // 三个块都直接挂在入口下
    CHECK(dom.treeText(program).find("flowchart TD") == 0);
}

void testSsa() {
    const auto program = parseOrDie(kDiamond);
    const auto cfg = ssa::buildCfg(program);
    const auto dom = ssa::analyzeDominators(program, cfg);
    const auto info = ssa::buildSsa(program, cfg, dom);
    CHECK(info.phiCount == 1);                           // 只有 y 需要 φ
    CHECK(info.program[3].code[0].isPhi());              // L2 的第一条就是 φ
    CHECK(info.program[3].code[0].phiArgs.size() == 2);
    // x 只在入口定义一次 → 名字是 x_1；y 有两条定义 + 一条 φ
    const std::string text = ssa::print(info.program);
    CHECK(text.find("phi y_") != std::string::npos);
    CHECK(info.renamedVariables == 2);                   // x 与 y
    CHECK(info.renamedDefinitions >= 3);
}

void testSccp() {
    const auto program = parseOrDie(kConstBranch);
    const ssa::Result result = ssa::optimize(program);
    CHECK(result.stats.phis == 1);                       // out 里 y 的汇合 φ
    CHECK(result.stats.sccpFoldedBranches == 1);         // jz x, dead 条件已知
    CHECK(result.stats.sccpRemovedBlocks == 1);          // dead 块被删掉
    CHECK(result.stats.gvnRedundantPhis == 1);           // dead 删掉后 φ 只剩一个实参
    CHECK(result.sccp.constants.count("x_1") == 1);
    CHECK(result.sccp.constants.at("x_1") == 1);
    const std::string text = ssa::print(result.sccp.program);
    CHECK(text.find("label dead") == std::string::npos);
}

void testGvn() {
    const auto program = parseOrDie(kRedundant);
    const auto cfg = ssa::buildCfg(program);
    const auto dom = ssa::analyzeDominators(program, cfg);
    const auto info = ssa::buildSsa(program, cfg, dom);
    // φ 插入与改名不改变块结构，所以原程序的 cfg / dom 可以直接复用。
    const auto simplified = ssa::gvn(info.program, cfg, dom);
    CHECK(simplified.redundantPhis == 1);                // 两条路径的实参都归并到 s
    CHECK(simplified.redundantCopies == 2);              // 两条 mov t, s
    CHECK(simplified.redundantExpressions == 1);         // add q, s, s
    CHECK(simplified.replacedUses >= 1);
    // 整条流水线跑一遍，语义必须一致。
    const ssa::Result result = ssa::optimize(program);
    CHECK(result.equivalent);
}

void testFallthroughPhi() {
    // 回归：顺序落下的边漏标成"可执行"时，φ 会少算一个前驱，
    // 常量传播就会把 y_3 错误地定成 2（而不是 ⊥）。
    const ssa::Result result = ssa::optimize(parseOrDie(kDiamondLoad));
    CHECK(result.stats.phis == 1);
    CHECK(result.sccp.executableBlocks.size() == 4);
    CHECK(result.sccp.executableEdges.size() == 4);
    CHECK(result.sccp.constants.count("y_3") == 0);      // φ 的两个实参是 2 和 3
    CHECK(result.sccp.overdefined.count("y_3") == 1);
    CHECK(result.equivalent);
    CHECK(result.after.output.size() == 1);
    CHECK(result.after.output[0] == 3);                  // slot 默认 0 → jz 跳到 L1
}

void testLoopEdgeReprocess() {
    // 回归：循环出口条件"第一次看是常量、后来变成 ⊥"时，新可执行边必须让目标块
    // 重新入队，否则 φ 少算回边前驱，jz 会被错误地折死（死循环）。
    const char* loop = R"(
  mov i, 0
  mov s, 0
label loop
  add i, i, 1
  add s, s, i
  sub t, 5, i
  jz t, done
  jmp loop
label done
  print s
  print i
  halt
)";
    const ssa::Result result = ssa::optimize(parseOrDie(loop));
    CHECK(result.stats.phis == 2);          // i 与 s 的回边 φ
    CHECK(result.equivalent);               // 优化前后解释执行必须一致
    CHECK(result.after.error.empty());
    CHECK(result.after.output.size() == 2);
    CHECK(result.after.output[0] == 15);    // 1+2+3+4+5
    CHECK(result.after.output[1] == 5);
}

void testDestroy() {
    const ssa::Result result = ssa::optimize(parseOrDie(kSwap));
    CHECK(result.stats.phis >= 2);                       // a、b（可能还有 n）的回边 φ
    CHECK(result.stats.destroyedParallelCopies >= 2);
    CHECK(result.stats.destroyedSplitEdges >= 1);        // 入口到循环头的临界边
    CHECK(result.stats.destroyedTemporaries >= 1);       // a、b 交换 → 复制环
    const std::string text = ssa::print(result.destruction.program);
    CHECK(text.find("phi ") == std::string::npos);
    CHECK(result.equivalent);
    CHECK(result.after.output.size() == 3);
    CHECK(result.after.output[0] == 2);                  // 交换 3 次 → a = 2
    CHECK(result.after.output[1] == 1);
    CHECK(result.after.output[2] == 3);
}

void testSemantics() {
    // 每个测试程序都要"优化前后跑出来一样"。
    const char* programs[] = {kDiamond, kConstBranch, kSwap, kRedundant, kDiamondLoad};
    for (const char* text : programs) {
        const ssa::Result result = ssa::optimize(parseOrDie(text));
        CHECK(result.before.error.empty());
        CHECK(result.after.error.empty());
        CHECK(result.equivalent);
    }
}

void testErrors() {
    ssa::Program program;
    std::vector<std::string> errors;
    CHECK(!ssa::parse("label\n", program, errors));
    CHECK(!errors.empty());
    errors.clear();
    CHECK(!ssa::parse("add t1, t2\n", program, errors));
    errors.clear();
    CHECK(!ssa::parse("frob t1\n", program, errors));
    errors.clear();
    CHECK(!ssa::parse("\n", program, errors));            // 空程序
}

} // namespace

int main() {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    testParse();
    testCfg();
    testDominators();
    testSsa();
    testSccp();
    testGvn();
    testFallthroughPhi();
    testLoopEdgeReprocess();
    testDestroy();
    testSemantics();
    testErrors();
    if (failures == 0) {
        std::cout << "ssa-tests：全部通过\n";
        return 0;
    }
    std::cout << "ssa-tests：" << failures << " 个断言失败\n";
    return 1;
}
