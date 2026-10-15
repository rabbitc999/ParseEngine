/**
 * @file test_stackvm.cpp
 * @brief stackvm 的单元测试：四元式解析、组装、栈式执行。
 *
 * 运行：
 *   stackvm-tests [示例目录]     示例目录缺省 `examples`
 *
 * 除了单元断言，还会把 `examples` 目录下的 `.quad` 逐个组装并执行，与文件里 `# 期望:`
 * 声明的内容对照——这些示例和 `library/transfer/core/example` 是同一批，
 * 因此这也验证了"同一份四元式，两种后端"的一致性。
 */

#include "stackvm.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace
{

int checks = 0;
int failures = 0;

/** 记一条断言。 */
#define CHECK(condition)                                                          \
    do                                                                           \
    {                                                                            \
        ++checks;                                                                \
        if (!(condition))                                                        \
        {                                                                        \
            ++failures;                                                          \
            std::cerr << "  失败：" #condition " （第 " << __LINE__ << " 行）\n"; \
        }                                                                        \
    } while (false)

/**
 * @brief 组装并执行一段四元式文本。
 * @param text 文本。
 * @param input 输入项。
 * @param output 输出。
 * @param program 组装结果（可选）。
 * @return 执行结果。
 */
stackvm::RunResult runText(const std::string &text, const std::vector<std::string> &input,
                           std::string &output, stackvm::Program *program = nullptr)
{
    const stackvm::QuadFile file = stackvm::parseQuads(text, "<test>");
    stackvm::Program assembled;
    stackvm::assemble(file, assembled);
    if (program != nullptr) *program = assembled;
    stackvm::Machine machine;
    const stackvm::RunResult result = machine.run(assembled, input);
    output = result.output;
    return result;
}

/** 类型与打印：整数回绕、无符号打印、浮点、有符号比较。 */
void testTypes()
{
    CHECK(stackvm::typeOf("i8").bits == 8);
    CHECK(stackvm::typeOf("u32").isUnsigned);
    CHECK(stackvm::typeOf("f64").isFloat);
    CHECK(stackvm::typeOf("").bits == 16); // 没声明按 i16

    std::string output;
    const stackvm::RunResult result = runText(
        "0: (decl, i8, global, si)\n"
        "1: (decl, u8, global, ui)\n"
        "2: (decl, i32, global, big)\n"
        "3: (:=, -1, _, si)\n"
        "4: (:=, 200, _, ui)\n"
        "5: (<:i8, si, 0, t1)\n"
        "6: (write, t1, _, _)\n"
        "7: (:=, -1, _, big)\n"
        "8: (write, big, _, _)\n"
        "9: (write:u8, ui, _, _)\n"
        "10: (halt, _, _, _)\n",
        {}, output);
    CHECK(result.ok);
    CHECK(output == std::string("1 -1 200")); // i8 的 -1 是有符号的，u8 的 200 按无符号打印

    // 无符号比较：u32 的 0xFFFFFFFF 大于 1（有符号才反过来）。
    const stackvm::RunResult compare = runText(
        "0: (decl, u32, global, u)\n"
        "1: (decl, i32, global, s)\n"
        "2: (:=, 4294967295, _, u)\n"
        "3: (:=, -1, _, s)\n"
        "4: (<:u32, u, 1, t1)\n"
        "5: (write, t1, _, _)\n"
        "6: (<:i32, s, 1, t2)\n"
        "7: (write, t2, _, _)\n"
        "8: (halt, _, _, _)\n",
        {}, output);
    CHECK(compare.ok);
    CHECK(output == std::string("0 1"));

    const stackvm::RunResult arithmetic = runText(
        "0: (+:i8, 127, 1, t1)\n"
        "1: (/:u32, 4294967294, 2, t2)\n"
        "2: (write, t1, _, _)\n"
        "3: (write:u32, t2, _, _)\n"
        "4: (halt, _, _, _)\n",
        {}, output);
    CHECK(arithmetic.ok);
    CHECK(output == std::string("-128 2147483647"));
}

/** 求值栈可按槽数限制，溢出走可诊断错误而不是宿主异常。 */
void testStackLimit()
{
    const stackvm::QuadFile file = stackvm::parseQuads(
        "0: (+, 1, 2, sum)\n"
        "1: (write, sum, _, _)\n"
        "2: (halt, _, _, _)\n",
        "<stack-limit>");
    stackvm::Program program;
    CHECK(stackvm::assemble(file, program));

    stackvm::Machine limited;
    limited.setStackLimit(1u);
    CHECK(limited.stackLimit() == 1u);
    const stackvm::RunResult overflow = limited.run(program);
    CHECK(!overflow.ok);
    CHECK(overflow.error.find("求值栈超过上限") != std::string::npos);

    stackvm::Machine enough;
    CHECK(enough.stackLimit() == 0u);
    enough.setStackLimit(2u);
    const stackvm::RunResult result = enough.run(program);
    CHECK(result.ok);
    CHECK(result.output == "3");
    CHECK(result.maxStackDepth == 2u);
}

/** 浮点与类型转换。 */
void testFloat()
{
    std::string output;
    const stackvm::RunResult result = runText(
        "0: (decl, f64, global, d)\n"
        "1: (decl, f32, global, f)\n"
        "2: (decl, i32, global, n)\n"
        "3: (:=, 1.5, _, d)\n"
        "4: (+:f64, d, 2.25, t1)\n"
        "5: (write:f64, t1, _, _)\n"
        "6: (cvt:f32, t1, _, f)\n"
        "7: (/:f32, f, 1.5, t2)\n"
        "8: (write:f32, t2, _, _)\n"
        "9: (cvt:i32, t1, _, n)\n"
        "10: (write, n, _, _)\n"
        "11: (halt, _, _, _)\n",
        {}, output);
    CHECK(result.ok);
    CHECK(output == std::string("3.75 2.5 3"));

    const stackvm::RunResult precision = runText(
        "0: (cvt:f32, 16777216, _, t1)\n"
        "1: (+:f32, t1, 1.0, t2)\n"
        "2: (cvt:f64, 16777216, _, t3)\n"
        "3: (+:f64, t3, 1.0, t4)\n"
        "4: (write:f64, t2, _, _)\n"
        "5: (write:f64, t4, _, _)\n"
        "6: (halt, _, _, _)\n",
        {}, output);
    CHECK(precision.ok);
    CHECK(output == std::string("16777216 16777217"));

    // 整数之间转型、赋值和参数传递不能经过 double（超过 2^53 会丢低位）。
    const stackvm::RunResult exactInteger = runText(
        "0: (decl, u64, global, value)\n"
        "1: (cvt:u64, 2305843009213693951, _, value)\n"
        "2: (write:u64, value, _, _)\n"
        "3: (halt, _, _, _)\n",
        {}, output);
    CHECK(exactInteger.ok);
    CHECK(output == std::string("2305843009213693951"));
}

/** 跳转与循环。 */
void testBranchAndLoop()
{
    std::string output;
    const stackvm::RunResult result = runText(
        "0: (decl, i32, global, i)\n"
        "1: (decl, i32, global, sum)\n"
        "2: (:=, 1, _, i)\n"
        "3: (:=, 0, _, sum)\n"
        "4: (label, _, _, loop)\n"
        "5: (<=:i32, i, 5, t1)\n"
        "6: (jz, t1, _, done)\n"
        "7: (+, sum, i, sum)\n"
        "8: (+, i, 1, i)\n"
        "9: (j, _, _, loop)\n"
        "10: (label, _, _, done)\n"
        "11: (write, sum, _, _)\n"
        "12: (halt, _, _, _)\n",
        {}, output);
    CHECK(result.ok);
    CHECK(output == std::string("15"));
}

/** 调用与递归：u32 阶乘。 */
void testCall()
{
    std::string output;
    const stackvm::RunResult result = runText(
        "0: (decl, u32, global, r)\n"
        "1: (decl, u32, global, s)\n"
        "2: (read, _, _, n)\n"
        "3: (param, n, _, k)\n"
        "4: (call, fact, _, r)\n"
        "5: (write:u32, r, _, _)\n"
        "6: (halt, _, _, _)\n"
        "7: (label, _, _, fact)\n"
        "8: (decl, u32, local, k)\n"
        "9: (<=:u32, k, 1, t)\n"
        "10: (jz, t, _, rec)\n"
        "11: (:=, 1, _, r)\n"
        "12: (ret:u32, r, _, _)\n"
        "13: (label, _, _, rec)\n"
        "14: (-:u32, k, 1, m)\n"
        "15: (param, m, _, k)\n"
        "16: (call, fact, _, s)\n"
        "17: (*:u32, k, s, r)\n"
        "18: (ret:u32, r, _, _)\n",
        {"6"}, output);
    CHECK(result.ok);
    CHECK(output == std::string("720")); // 6! = 720
}

/**
 * 局部变量按帧存放：`(decl, T, local, x)` 在每层调用里各开一个槽。
 *
 * 这是"递归能对"的前提：1) 每层调用的局部变量互不覆盖；2) 返回值写回的
 * 是**调用方那一帧**的局部变量；3) 已经绑好的形参不会被函数体里的同名声明清零。
 */
void testLocalFrames()
{
    // 1. 斐波那契：x / y 是局部变量，回溯时两个分支的结果都要还在。
    std::string output;
    const stackvm::RunResult fib = runText(
        "0: (decl, i32, global, res)\n"
        "1: (param, 10, _, n)\n"
        "2: (call, fib, _, res)\n"
        "3: (write, res, _, _)\n"
        "4: (halt, _, _, _)\n"
        "5: (label, _, _, fib)\n"
        "6: (decl, i32, local, a)\n"
        "7: (:=, 0, _, a)\n"
        "8: (decl, i32, local, b)\n"
        "9: (:=, 1, _, b)\n"
        "10: (decl, i32, local, x)\n"
        "11: (decl, i32, local, y)\n"
        "12: (==:i32, n, 0, t1)\n"
        "13: (jz, t1, _, one)\n"
        "14: (ret, a, _, _)\n"
        "15: (label, _, _, one)\n"
        "16: (==:i32, n, 1, t2)\n"
        "17: (jz, t2, _, rec)\n"
        "18: (ret, b, _, _)\n"
        "19: (label, _, _, rec)\n"
        "20: (-:i32, n, 1, t3)\n"
        "21: (param, t3, _, n)\n"
        "22: (call, fib, _, x)\n"
        "23: (-:i32, n, 2, t4)\n"
        "24: (param, t4, _, n)\n"
        "25: (call, fib, _, y)\n"
        "26: (+:i32, x, y, t5)\n"
        "27: (ret, t5, _, _)\n",
        {}, output);
    CHECK(fib.ok);
    CHECK(output == std::string("55")); // 每层的 a / b / x / y 都是自己的

    // 2. 函数体里的 `decl` 不能把调用时绑好的形参清零（`k` 先由 `param` 绑进本帧）。
    const stackvm::RunResult param = runText(
        "0: (param, 7, _, k)\n"
        "1: (call, f, _, r)\n"
        "2: (write, r, _, _)\n"
        "3: (halt, _, _, _)\n"
        "4: (label, _, _, f)\n"
        "5: (decl, i32, local, k)\n"
        "6: (decl, i32, local, n)\n"
        "7: (:=, 3, _, n)\n"
        "8: (+:i32, k, n, s)\n"
        "9: (ret, s, _, _)\n",
        {}, output);
    CHECK(param.ok);
    CHECK(output == std::string("10")); // k 仍是 7，而不是被 decl 清零

    // 3. 同一段代码调用两次、跨调用互不影响（每次调用都是新的一帧）：
    //    g 里 n 从 1 数到 3，第二次调用仍从 n 的初值开始，不会接着上一次的值。
    const stackvm::RunResult twice = runText(
        "0: (call, g, _, r)\n"
        "1: (call, g, _, r)\n"
        "2: (write, r, _, _)\n"
        "3: (halt, _, _, _)\n"
        "4: (label, _, _, g)\n"
        "5: (decl, i32, local, n)\n"
        "6: (:=, 1, _, n)\n"
        "7: (decl, i32, local, sum)\n"
        "8: (:=, 0, _, sum)\n"
        "9: (label, _, _, loop)\n"
        "10: (<=:i32, n, 3, t1)\n"
        "11: (jz, t1, _, done)\n"
        "12: (+:i32, sum, n, sum)\n"
        "13: (+:i32, n, 1, n)\n"
        "14: (j, _, _, loop)\n"
        "15: (label, _, _, done)\n"
        "16: (ret, sum, _, _)\n",
        {}, output);
    CHECK(twice.ok);
    CHECK(output == std::string("6")); // 两次都是 1+2+3，后一次不接前一次的 n
}

/** 双结果乘除与输出指令。 */
void testMulDivAndIo()
{
    std::string output;
    const stackvm::RunResult result = runText(
        "0: (decl, u16, global, a)\n"
        "1: (decl, u16, global, b)\n"
        "2: (decl, u16, global, lo)\n"
        "3: (decl, u16, global, hi)\n"
        "4: (decl, i16, global, q)\n"
        "5: (decl, i16, global, r)\n"
        "6: (:=, 40000, _, a)\n"
        "7: (:=, 3, _, b)\n"
        "8: (mul:u16, a, b, lo, hi)\n"
        "9: (write:u16, lo, _, _)\n"
        "10: (:=, -7, _, a)\n"
        "11: (div:i16, a, b, q, r)\n"
        "12: (write, q, _, _)\n"
        "13: (write, r, _, _)\n"
        "14: (halt, _, _, _)\n",
        {}, output);
    CHECK(result.ok);
    CHECK(output == std::string("54464 -2 -1")); // 40000*3 = 120000 = hi 1 / lo 54464；-7/3 = -2 余 -1

    // print / putc / getc / read。
    const stackvm::RunResult io = runText(
        "0: (decl, u16, global, n)\n"
        "1: (decl, u8, global, c)\n"
        "2: (read, _, _, n)\n"
        "3: (<<:u16, n, 1, t)\n"
        "4: (write:u16, t, _, _)\n"
        "5: (print, \"n=\", _, _)\n"
        "6: (write:u16, n, _, _)\n"
        "7: (getc, _, _, c)\n"
        "8: (print, \"|\", _, _)\n"
        "9: (putc, c, _, _)\n"
        "10: (halt, _, _, _)\n",
        {"21", "A"}, output);
    CHECK(io.ok);
    // 数值输出与文本输出分开收集（和 transfer 的 Execution.output / text 一致）。
    CHECK(output == std::string("42 21"));
    CHECK(io.text == std::string("n=|A"));
}

/** 解析诊断与不支持的指令。 */
void testDiagnostics()
{
    const stackvm::QuadFile broken = stackvm::parseQuads("0: (+, 1, 2\n", "<test>");
    CHECK(!broken.ok());

    const stackvm::QuadFile unsupported = stackvm::parseQuads(
        "0: (decl, u32, global, a)\n"
        "1: (+c:u16, a, a, b)\n"
        "2: (halt, _, _, _)\n",
        "<test>");
    CHECK(unsupported.ok());
    stackvm::Program program;
    CHECK(!stackvm::assemble(unsupported, program)); // 组装期报出不支持
    CHECK(!program.diagnostics.empty());
    CHECK(program.diagnostics.front().find("不支持") != std::string::npos);

    // 运行时错误：读未定义变量 / 除零。
    std::string output;
    const stackvm::RunResult undefined = runText("0: (+, x, 1, y)\n1: (halt, _, _, _)\n", {}, output);
    CHECK(!undefined.ok);
    CHECK(undefined.error.find("未定义") != std::string::npos);
    const stackvm::RunResult divide = runText(
        "0: (decl, i32, global, a)\n"
        "1: (:=, 0, _, a)\n"
        "2: (/:i32, 1, a, t)\n"
        "3: (halt, _, _, _)\n",
        {}, output);
    CHECK(!divide.ok);
    CHECK(divide.error.find("除零") != std::string::npos);
}

/** 反汇编文本可读（顺便固定指令编码）。 */
void testDisassembly()
{
    stackvm::Program program;
    std::string output;
    runText(
        "0: (decl, i32, global, x)\n"
        "1: (:=, 2, _, x)\n"
        "2: (+, x, 3, t1)\n"
        "3: (write, t1, _, _)\n"
        "4: (halt, _, _, _)\n",
        {}, output, &program);
    CHECK(output == std::string("5"));
    CHECK(program.code.size() == 9u); // PUSH/STORE/LOAD/PUSH/ADD/LOAD/PRINT/HALT + (:=) 展开
    CHECK(program.code[0].toString() == std::string("PUSH 2"));
    CHECK(program.code[1].toString() == std::string("STORE x"));
    CHECK(program.code[2].toString() == std::string("LOAD x"));
    CHECK(program.code[3].toString() == std::string("PUSH 3"));
    CHECK(program.code[4].toString() == std::string("ADD"));
}

/** 化简：常量折叠 / 代数化简 / 死分支 / 死代码，且不改变语义。 */
void testOptimize()
{
    // 1. 常量折叠 + 死代码：x 只写不读，赋值应被删；2+3 应折成 5。
    stackvm::QuadFile folded = stackvm::parseQuads(
        "0: (:=, 2, _, x)\n"
        "1: (+, x, 3, t1)\n"
        "2: (write, t1, _, _)\n"
        "3: (halt, _, _, _)\n",
        "<test>");
    const stackvm::OptimizationReport first = stackvm::optimize(folded);
    CHECK(first.constantsFolded >= 1u);
    CHECK(first.total() >= 1u);
    // 折叠出来的 5 可能被继续代入到 write，所以两种形态都接受。
    bool hasFive = false;
    for (const stackvm::Quad &quad : folded.quads)
    {
        if ((quad.op == ":=" || quad.op == "write") && quad.arg(0) == "5") hasFive = true;
    }
    CHECK(hasFive);

    // 2. 代数化简：x+0 / x*1 / x<<0 → 直接赋值。
    stackvm::QuadFile algebra = stackvm::parseQuads(
        "0: (decl, i32, global, x)\n"
        "1: (read, _, _, x)\n"
        "2: (+, x, 0, a)\n"
        "3: (write, a, _, _)\n"
        "4: (*:i32, x, 1, b)\n"
        "5: (write, b, _, _)\n"
        "6: (<<:i32, x, 0, c)\n"
        "7: (write, c, _, _)\n"
        "8: (halt, _, _, _)\n",
        "<test>");
    const stackvm::OptimizationReport second = stackvm::optimize(algebra);
    CHECK(second.algebraicSimplified >= 3u);

    // 3. 死分支：常量条件的 jz 折成 j。
    stackvm::QuadFile branch = stackvm::parseQuads(
        "0: (:=, 0, _, t)\n"
        "1: (jz, t, _, skip)\n"
        "2: (halt, _, _, _)\n"
        "3: (label, _, _, skip)\n"
        "4: (halt, _, _, _)\n",
        "<test>");
    const stackvm::OptimizationReport third = stackvm::optimize(branch);
    CHECK(third.deadBranches >= 1u);
    CHECK(third.deadCode >= 1u); // `j skip` 后面的 halt 不可达

    // 4. 语义不变：同一份四元式，化简前后输出一致。
    const std::string source =
        "0: (decl, i32, global, i)\n"
        "1: (decl, i32, global, sum)\n"
        "2: (:=, 1, _, i)\n"
        "3: (:=, 0, _, sum)\n"
        "4: (label, _, _, loop)\n"
        "5: (<=:i32, i, 5, t1)\n"
        "6: (jz, t1, _, done)\n"
        "7: (+, sum, i, sum)\n"
        "8: (+, i, 1, i)\n"
        "9: (j, _, _, loop)\n"
        "10: (label, _, _, done)\n"
        "11: (write, sum, _, _)\n"
        "12: (halt, _, _, _)\n";
    stackvm::QuadFile plain = stackvm::parseQuads(source, "<test>");
    stackvm::QuadFile optimized = stackvm::parseQuads(source, "<test>");
    const stackvm::OptimizationReport report = stackvm::optimize(optimized);
    CHECK(report.rounds > 0u); // 至少跑完一轮（这份程序本身没有可折的常量）

    stackvm::Program plainProgram;
    stackvm::Program optimizedProgram;
    stackvm::assemble(plain, plainProgram);
    stackvm::assemble(optimized, optimizedProgram);
    stackvm::Machine machine;
    const stackvm::RunResult plainResult = machine.run(plainProgram);
    const stackvm::RunResult optimizedResult = machine.run(optimizedProgram);
    CHECK(plainResult.ok && optimizedResult.ok);
    CHECK(plainResult.output == optimizedResult.output);
    CHECK(optimizedProgram.code.size() <= plainProgram.code.size());

    // 回归：形参的 `(decl …)` 排在引导 `(halt …)` 之后、函数标签之前——它是**类型元信息**，
    // 不能被"不可达代码"整段删掉（`simple_ast` 给每个函数发的形参声明就长这样）。
    stackvm::QuadFile guide = stackvm::parseQuads(
        "0: (param, 41, _, k)\n"
        "1: (call, f, _, _)\n"
        "2: (halt, _, _, _)\n"
        "3: (decl, u32, local, k)\n"
        "4: (label, _, _, f)\n"
        "5: (+:u32, k, 1, r)\n"
        "6: (write:u32, r, _, _)\n"
        "7: (ret, 0, _, _)\n",
        "<test>");
    const stackvm::OptimizationReport guideReport = stackvm::optimize(guide);
    (void)guideReport; // 只关心下面"decl 还在不在"，化简统计在这里不看
    bool keptDecl = false;
    for (const stackvm::Quad &quad : guide.quads)
    {
        if (quad.op == "decl" && quad.arg(2) == "k") keptDecl = true;
    }
    CHECK(keptDecl);
    stackvm::Program guided;
    CHECK(stackvm::assemble(guide, guided));
    stackvm::Machine guidedMachine;
    const stackvm::RunResult guidedResult = guidedMachine.run(guided, {}, 1000u);
    CHECK(guidedResult.ok);
    CHECK(guidedResult.output == std::string("42")); // k = 41（形参），r = k + 1
}

/**
 * 堆：`(alloc …)` / `(free …)`。
 *
 * 堆和数据区共用一套地址空间（数据区在前、堆跟在后面），但对象不在数据区里、
 * 也不在帧里：要多少字节由运行期决定，还回来由程序自己 `free`。
 */
void testHeap()
{
    std::string output;

    // 1. 分配 → 在块内写 / 读 → 释放；结束时统计里不该有活块。
    const stackvm::RunResult basics = runText(
        "0: (decl, i64, global, p)\n"
        "1: (alloc, 16, _, p)\n"
        "2: (store:i32, p, 42, _)\n"
        "3: (load:i32, p, _, t1)\n"
        "4: (write, t1, _, _)\n"
        "5: (+:i64, p, 8, t2)\n"
        "6: (store:i32, t2, 7, _)\n"
        "7: (load:i32, t2, _, t3)\n"
        "8: (write, t3, _, _)\n"
        "9: (free, p, _, _)\n"
        "10: (halt, _, _, _)\n",
        {}, output);
    CHECK(basics.ok);
    CHECK(output == std::string("42 7"));
    CHECK(basics.heapLiveBytes == 0u);   // 释放干净
    CHECK(basics.heapLiveBlocks == 0u);
    CHECK(basics.heapPeakBytes >= 16u);

    // 2. 没 free 就在统计里露出来。
    const stackvm::RunResult leak = runText(
        "0: (decl, i64, global, p)\n"
        "1: (alloc, 24, _, p)\n"
        "2: (halt, _, _, _)\n",
        {}, output);
    CHECK(leak.ok);
    CHECK(leak.heapLiveBytes == 24u);
    CHECK(leak.heapLiveBlocks == 1u);

    // 3. 本次运行中不复用已释放地址，旧指针不能命中新对象。
    const stackvm::RunResult reuse = runText(
        "0: (decl, i64, global, a)\n"
        "1: (decl, i64, global, b)\n"
        "2: (alloc, 8, _, a)\n"
        "3: (free, a, _, _)\n"
        "4: (alloc, 8, _, b)\n"
        "5: (==:i64, a, b, t1)\n"
        "6: (write, t1, _, _)\n"
        "7: (halt, _, _, _)\n",
        {}, output);
    CHECK(reuse.ok);
    CHECK(output == std::string("0"));
    CHECK(reuse.heapLiveBytes == 8u);

    const stackvm::RunResult staleAfterAllocate = runText(
        "0: (decl, i64, global, a)\n"
        "1: (decl, i64, global, b)\n"
        "2: (alloc, 8, _, a)\n"
        "3: (free, a, _, _)\n"
        "4: (alloc, 8, _, b)\n"
        "5: (load:i32, a, _, value)\n"
        "6: (halt, _, _, _)\n",
        {}, output);
    CHECK(!staleAfterAllocate.ok);
    CHECK(staleAfterAllocate.error.find("堆越界读") != std::string::npos);

    // 4. 双重释放 / 释放后读写：都要报出来，而不是悄悄读写脏内存。
    const stackvm::RunResult twice = runText(
        "0: (decl, i64, global, p)\n"
        "1: (alloc, 8, _, p)\n"
        "2: (free, p, _, _)\n"
        "3: (free, p, _, _)\n"
        "4: (halt, _, _, _)\n",
        {}, output);
    CHECK(!twice.ok);
    CHECK(twice.error.find("free 过了") != std::string::npos);

    const stackvm::RunResult useAfterFree = runText(
        "0: (decl, i64, global, p)\n"
        "1: (alloc, 8, _, p)\n"
        "2: (free, p, _, _)\n"
        "3: (load:i32, p, _, t1)\n"
        "4: (halt, _, _, _)\n",
        {}, output);
    CHECK(!useAfterFree.ok);
    CHECK(useAfterFree.error.find("堆越界读") != std::string::npos);

    // 5. 申请 0 字节 / 巨大块：给清楚的诊断。
    const stackvm::RunResult zero = runText(
        "0: (decl, i64, global, p)\n"
        "1: (alloc, 0, _, p)\n"
        "2: (halt, _, _, _)\n",
        {}, output);
    CHECK(!zero.ok);
    CHECK(zero.error.find("必须是正数") != std::string::npos);

    const stackvm::RunResult huge = runText(
        "0: (decl, i64, global, p)\n"
        "1: (alloc, 9223372036854775807, _, p)\n"
        "2: (halt, _, _, _)\n",
        {}, output);
    CHECK(!huge.ok);
    CHECK(huge.error.find("堆内存不足") != std::string::npos);

    const stackvm::RunResult fractional = runText(
        "0: (alloc, 1.0, _, p)\n"
        "1: (halt, _, _, _)\n",
        {}, output);
    CHECK(!fractional.ok);
    CHECK(fractional.error.find("必须是正数") != std::string::npos);
}

/** 数学库已移到宿主胶水，StackVM 不再暴露旧 `__vm_*` 数学指令。 */
void testRemovedMathNatives()
{
    std::string output;
    const stackvm::RunResult removed = runText(
        "0: (param, 0.0, _, a)\n"
        "1: (call, __vm_sin, _, t1)\n"
        "2: (halt, _, _, _)\n", {}, output);
    CHECK(!removed.ok);
    CHECK(removed.error.find("函数入口不存在") != std::string::npos);
}

/**
 * 数组下标检查：`(check, 下标, 上限, 标签)` 与 `(checkptr, 下标, 基址, 步长, 标签)`。
 *
 * 前者比"运行期算出来的上限"（运行时长度数组的长度就存成变量），后者按
 * "基址所在那一块"的容量比（堆块按分配大小、数据区按布局里的 `size=`）。
 */
void testBoundsCheck()
{
    std::string output;

    // 1. check：合法下标照常过。
    const stackvm::RunResult pass = runText(
        "0: (decl, i32, global, i)\n"
        "1: (:=, 3, _, i)\n"
        "2: (check, i, 4, m)\n"
        "3: (write, i, _, _)\n"
        "4: (halt, _, _, _)\n",
        {}, output);
    CHECK(pass.ok);
    CHECK(output == std::string("3"));

    // 2. check：下标 == 上限、下标为负，都要终止运行并说清是谁越界。
    const stackvm::RunResult over = runText(
        "0: (decl, i32, global, i)\n"
        "1: (:=, 4, _, i)\n"
        "2: (check, i, 4, m)\n"
        "3: (halt, _, _, _)\n",
        {}, output);
    CHECK(!over.ok);
    CHECK(over.error.find("数组下标越界") != std::string::npos);
    CHECK(over.error.find("m") != std::string::npos);

    const stackvm::RunResult negative = runText(
        "0: (decl, i32, global, i)\n"
        "1: (:=, -1, _, i)\n"
        "2: (check, i, 4, m)\n"
        "3: (halt, _, _, _)\n",
        {}, output);
    CHECK(!negative.ok);
    CHECK(negative.error.find("数组下标越界") != std::string::npos);

    // 3. checkptr：堆上一块 32 字节，第 3 个 8 字节元素放得下、第 4 个放不下。
    const stackvm::RunResult heapPass = runText(
        "0: (decl, i64, global, p)\n"
        "1: (alloc, 32, _, p)\n"
        "2: (checkptr, 3, p, 8, row)\n"
        "3: (store:i32, p, 7, _)\n"
        "4: (halt, _, _, _)\n",
        {}, output);
    CHECK(heapPass.ok);

    const stackvm::RunResult heapOver = runText(
        "0: (decl, i64, global, p)\n"
        "1: (alloc, 32, _, p)\n"
        "2: (checkptr, 4, p, 8, row)\n"
        "3: (halt, _, _, _)\n",
        {}, output);
    CHECK(!heapOver.ok);
    CHECK(heapOver.error.find("数组下标越界") != std::string::npos);

    // 4. checkptr：已经 free 掉之后再查 → 立刻说"地址不在任何一块已分配内存里"。
    const stackvm::RunResult afterFree = runText(
        "0: (decl, i64, global, p)\n"
        "1: (alloc, 32, _, p)\n"
        "2: (free, p, _, _)\n"
        "3: (checkptr, 0, p, 8, row)\n"
        "4: (halt, _, _, _)\n",
        {}, output);
    CHECK(!afterFree.ok);
    CHECK(afterFree.error.find("不在任何一块已分配内存里") != std::string::npos);

    // 5. checkptr：数据区里的数组按**符号大小**算容量（布局里的 `size=`）。
    stackvm::DataArea data;
    data.binary.assign(32u, 0u);
    data.symbols["arr"] = 0u;
    data.sizes["arr"] = 32u;

    stackvm::QuadFile inside = stackvm::parseQuads(
        "0: (decl, i64, global, p)\n"
        "1: (addr, arr, _, p)\n"
        "2: (checkptr, 3, p, 8, arr)\n"
        "3: (halt, _, _, _)\n",
        "<test>");
    stackvm::Program insideProgram;
    CHECK(stackvm::assemble(inside, insideProgram));
    stackvm::Machine insideMachine;
    insideMachine.setData(&data);
    CHECK(insideMachine.run(insideProgram).ok);

    stackvm::QuadFile outside = stackvm::parseQuads(
        "0: (decl, i64, global, p)\n"
        "1: (addr, arr, _, p)\n"
        "2: (checkptr, 4, p, 8, arr)\n"
        "3: (halt, _, _, _)\n",
        "<test>");
    stackvm::Program outsideProgram;
    CHECK(stackvm::assemble(outside, outsideProgram));
    stackvm::Machine outsideMachine;
    outsideMachine.setData(&data);
    const stackvm::RunResult caught = outsideMachine.run(outsideProgram);
    CHECK(!caught.ok);
    CHECK(caught.error.find("数组下标越界") != std::string::npos);
}

/**
 * 函数指针：`(callptr, 函数指针, _, 目标)`。
 *
 * 指针的值就是"数据区里存着函数标签的那串字节"的地址；实参**按位置**绑定，
 * 位置表是组装时从"函数标签前面那段 decl"扫出来的。
 */
void testFunctionPointer()
{
    std::string output;
    stackvm::DataArea data;
    // 地址 0 留给"空指针"（数据区从 8 起排），标签字符串都排在 8 之后。
    const std::string functionBytes =
        std::string(8u, '\0') + std::string("f\0g\0bin/str\0", 12u);
    data.binary.assign(functionBytes.begin(), functionBytes.end());
    data.symbols["fname"] = 8u;       // "f"
    data.symbols["gname"] = 10u;      // "g"
    data.symbols["bad"] = 12u;        // "bin/str"：不是函数名

    // 公共部分：两个函数。**形参声明排在函数标签前面**，间接调用就靠这段扫出位置表。
    const std::string prologue =
        "0: (call, entry, _, _)\n"
        "1: (halt, _, _, _)\n"
        "2: (decl, i32, local, x)\n"
        "3: (label, _, _, f)\n"
        "4: (*:i32, x, 2, t1)\n"
        "5: (ret, t1, _, _)\n"
        "6: (decl, i32, local, y)\n"
        "7: (label, _, _, g)\n"
        "8: (+:i32, y, 100, t2)\n"
        "9: (ret, t2, _, _)\n"
        "10: (decl, i64, global, p)\n"
        "11: (decl, i32, global, r)\n"
        "12: (label, _, _, entry)\n";

    auto runWith = [&](const std::string &tail, std::size_t yesParamCount) {
        stackvm::QuadFile file = stackvm::parseQuads(prologue + tail, "<test>");
        stackvm::Program program;
        stackvm::Machine machine;
        machine.setData(&data);
        CHECK(stackvm::assemble(file, program));
        CHECK(program.parameters.size() == yesParamCount);   // 标签 → 形参名扫出来了
        return machine.run(program);
    };

    // 1) 正常：`p = f` 调 f、`p = g` 调 g（实参按位置绑定，不看调用点写的名字）。
    const stackvm::RunResult result = runWith(
        "13: (addr, fname, _, p)\n"
        "14: (param, 21, _, a)\n"
        "15: (callptr, p, _, r)\n"
        "16: (write, r, _, _)\n"
        "17: (addr, gname, _, p)\n"
        "18: (param, 5, _, a)\n"
        "19: (callptr, p, _, r)\n"
        "20: (write, r, _, _)\n"
        "21: (halt, _, _, _)\n",
        2u);
    CHECK(result.ok);
    output = result.output;
    CHECK(output == std::string("42 105"));   // f(21) = 42、g(5) = 105

    // 2) 空函数指针（0）：调用时立刻报错。
    const stackvm::RunResult nil = runWith(
        "13: (:=, 0, _, p)\n"
        "14: (callptr, p, _, r)\n"
        "15: (halt, _, _, _)\n",
        2u);
    CHECK(!nil.ok);
    CHECK(nil.error.find("空函数指针") != std::string::npos);

    // 3) 指到一串不是函数名的字节上：也要报错。
    const stackvm::RunResult wrongName = runWith(
        "13: (addr, bad, _, p)\n"
        "14: (callptr, p, _, _)\n"
        "15: (halt, _, _, _)\n",
        2u);
    CHECK(!wrongName.ok);

    // 4) 实参个数与目标函数的形参表对不上：报出来（f 只吃 1 个）。
    const stackvm::RunResult wrongArity = runWith(
        "13: (addr, fname, _, p)\n"
        "14: (param, 1, _, a)\n"
        "15: (param, 2, _, b)\n"
        "16: (callptr, p, _, _)\n"
        "17: (halt, _, _, _)\n",
        2u);
    CHECK(!wrongArity.ok);
    CHECK(wrongArity.error.find("需要 1 个参数") != std::string::npos);
    // 零参数函数不产生形参 decl；普通与加载后的字节码仍允许通过函数指针调用。
    auto zeroArgument = [&](bool extra) {
        auto file = stackvm::parseQuads(
            "0: (call, entry, _, _)\n1: (halt, _, _, _)\n"
            "2: (label, _, _, f)\n3: (ret, 42, _, _)\n"
            "4: (label, _, _, entry)\n5: (addr, fname, _, p)\n" +
            std::string(extra ? "6: (param, 1, _, a)\n" : "") +
            "7: (callptr, p, _, r)\n8: (write, r, _, _)\n9: (halt, _, _, _)\n", "<zero-argument>");
        stackvm::Program program;
        CHECK(stackvm::assemble(file, program));
        CHECK(program.parameters.empty());
        stackvm::Machine machine; machine.setData(&data);
        return machine.run(program);
    };
    const auto zero = zeroArgument(false);
    CHECK(zero.ok && zero.output == "42");
    const auto extra = zeroArgument(true);
    CHECK(!extra.ok && extra.error.find("需要 0 个参数") != std::string::npos);

}

/** 协程拥有独立调用帧，可在 yield 后恢复，并由 wait 等待完成。 */
void testCoroutines()
{
    stackvm::DataArea data;
    const std::string functionBytes =
        std::string(8u, '\0') + std::string("task_a\0task_b\0", 14u);
    data.binary.assign(functionBytes.begin(), functionBytes.end());
    data.symbols["task_a_name"] = 8u;
    data.symbols["task_b_name"] = 15u;

    const stackvm::QuadFile file = stackvm::parseQuads(
        "0: (addr, task_a_name, _, task_a_ptr)\n"
        "1: (param, task_a_ptr, _, task)\n"
        "2: (call, lang::SharedCoroutine::create, _, first)\n"
        "3: (addr, task_b_name, _, task_b_ptr)\n"
        "4: (param, task_b_ptr, _, task)\n"
        "5: (call, lang::SharedCoroutine::create, _, second)\n"
        "6: (param, first, _, coroutine)\n"
        "7: (call, lang::SharedCoroutine::wait, _, _)\n"
        "8: (param, second, _, coroutine)\n"
        "9: (call, lang::SharedCoroutine::wait, _, _)\n"
        "10: (halt, _, _, _)\n"
        "11: (label, _, _, task_a)\n"
        "12: (putc, 65, _, _)\n"
        "13: (call, lang::SharedCoroutine::yield, _, _)\n"
        "14: (putc, 67, _, _)\n"
        "15: (ret, 0, _, _)\n"
        "16: (label, _, _, task_b)\n"
        "17: (putc, 66, _, _)\n"
        "18: (call, lang::SharedCoroutine::yelid, _, _)\n"
        "19: (putc, 68, _, _)\n"
        "20: (ret, 0, _, _)\n",
        "<coroutines>");
    stackvm::Program program;
    CHECK(stackvm::assemble(file, program));
    stackvm::Machine machine;
    machine.setData(&data);
    const stackvm::RunResult result = machine.run(program);
    CHECK(result.ok);
    CHECK(result.text == "ABCD");
    CHECK(result.coroutines.size() == 3u);
    CHECK(result.coroutines[0].state == "done");
    CHECK(result.coroutines[1].state == "done");
    CHECK(result.coroutines[2].state == "done");
}

void testCoroutineMessages()
{
    stackvm::DataArea data;
    const std::string bytes = std::string(8u, '\0') + std::string("worker\0hello\0", 13u);
    data.binary.assign(bytes.begin(), bytes.end());
    data.symbols["worker_name"] = 8u;
    data.symbols["greeting"] = 15u;
    const stackvm::QuadFile file = stackvm::parseQuads(
        "0: (addr, worker_name, _, task_ptr)\n"
        "1: (param, task_ptr, _, task)\n"
        "2: (call, lang::SharedCoroutine::create, _, child)\n"
        "3: (addr, greeting, _, greeting_ptr)\n"
        "4: (param, child, _, coroutine)\n"
        "5: (param, greeting_ptr, _, message)\n"
        "6: (call, lang::SharedCoroutine::send, _, _)\n"
        "7: (call, lang::SharedCoroutine::receive, _, reply)\n"
        "8: (param, child, _, coroutine)\n"
        "9: (call, lang::SharedCoroutine::wait, _, _)\n"
        "10: (param, reply, _, left)\n"
        "11: (param, greeting_ptr, _, right)\n"
        "12: (call, __vm_str_cmp, _, comparison)\n"
        "13: (write, comparison, _, _)\n"
        "14: (halt, _, _, _)\n"
        "15: (label, _, _, worker)\n"
        "16: (call, lang::SharedCoroutine::receive, _, incoming)\n"
        "17: (call, lang::SharedCoroutine::parent, _, parent)\n"
        "18: (param, parent, _, coroutine)\n"
        "19: (param, incoming, _, message)\n"
        "20: (call, lang::SharedCoroutine::send, _, _)\n"
        "21: (ret, 0, _, _)\n",
        "<coroutine messages>");
    stackvm::Program program;
    CHECK(stackvm::assemble(file, program));
    stackvm::Machine machine;
    machine.setData(&data);
    const stackvm::RunResult result = machine.run(program);
    CHECK(result.ok);
    CHECK(result.output == "0");
    CHECK(result.coroutines.size() == 2u);
    CHECK(result.coroutines[0].state == "done");
    CHECK(result.coroutines[1].state == "done");
}

/** 宿主时钟与每 N 条指令钩子使用稳定的累计指令计数。 */
void testTimingAndInstructionHook()
{
    const stackvm::QuadFile file = stackvm::parseQuads(
        "0: (:=, 2, _, x)\n"
        "1: (+, x, 3, t)\n"
        "2: (write, t, _, _)\n"
        "3: (halt, _, _, _)\n",
        "<hooks>");
    stackvm::Program program;
    CHECK(stackvm::assemble(file, program));

    std::uint64_t now = 100u;
    std::vector<stackvm::InstructionHookInfo> calls;
    stackvm::Machine machine;
    machine.setTimeCallback([&now]() {
        const std::uint64_t value = now;
        now += 75u;
        return value;
    });
    machine.setInstructionHook(2u, [&calls](const stackvm::InstructionHookInfo &info) {
        calls.push_back(info);
    });
    const stackvm::RunResult result = machine.run(program);
    CHECK(result.ok);
    CHECK(result.timingAvailable);
    CHECK(result.startedNanoseconds == 100u);
    CHECK(result.finishedNanoseconds == 175u);
    CHECK(result.elapsedNanoseconds == 75u);
    CHECK(!calls.empty());
    CHECK(calls.back().steps <= result.steps);
    for (const stackvm::InstructionHookInfo &call : calls)
    {
        CHECK(call.steps % 2u == 0u);
        CHECK(!call.opcode.empty());
    }
}

/** 数据区：`(addr …)` / `(load …)` / `(store …)` 真正读写那片内存。 */
void testDataArea()
{
    // 1. 造一份数据区：两个 8 字节槽（10、20），布局表给符号 arr 指向 0。
    stackvm::DataArea data;
    data.binary.assign(16u, 0u);
    data.symbols["arr"] = 0u;
    CHECK(data.writeInt(0u, 10));
    CHECK(data.writeInt(8u, 20));
    std::int64_t value = 0;
    CHECK(data.readInt(8u, value) && value == 20);
    CHECK(!data.readInt(9u, value));      // 越界
    CHECK(data.writeInt(0u, 10));

    // 2. 四元式：t1 = addr arr; t2 = t1 + 8; x = load t2; write x —— 期望 20。
    stackvm::QuadFile file = stackvm::parseQuads(
        "0: (addr, arr, _, t1)\n"
        "1: (+:i64, t1, 8, t2)\n"
        "2: (load, t2, _, x)\n"
        "3: (write, x, _, _)\n"
        "4: (halt, _, _, _)\n",
        "<test>");
    stackvm::Program program;
    CHECK(stackvm::assemble(file, program));
    CHECK(program.code[0].toString() == std::string("ADDR arr"));
    stackvm::Machine machine;
    machine.setData(&data);
    const stackvm::RunResult result = machine.run(program);
    CHECK(result.ok);
    CHECK(result.output == std::string("20"));

    // 3. 反向写：store t2 <- 99，再读回来。
    stackvm::QuadFile writer = stackvm::parseQuads(
        "0: (decl, i64, global, t1)\n"
        "1: (decl, i64, global, t2)\n"
        "2: (addr, arr, _, t1)\n"
        "3: (+:i64, t1, 8, t2)\n"
        "4: (store, t2, 99, _)\n"
        "5: (load, t2, _, x)\n"
        "6: (write, x, _, _)\n"
        "7: (halt, _, _, _)\n",
        "<test>");
    stackvm::Program writerProgram;
    CHECK(stackvm::assemble(writer, writerProgram));
    stackvm::Machine writerMachine;
    writerMachine.setData(&data);
    const stackvm::RunResult wrote = writerMachine.run(writerProgram);
    CHECK(wrote.ok && wrote.output == std::string("99"));

    // 4. 没挂数据区时给清楚的错误。
    stackvm::Machine bare;
    const stackvm::RunResult missing = bare.run(program);
    CHECK(!missing.ok);
    CHECK(missing.error.find("数据区") != std::string::npos);
}

/**
 * 装箱格子（`obj`）：`__vm_obj_*` 那批原生函数。
 *
 * 盒子就是"一个值 + 类型号"，也是**接口指针（胖指针）的运行时表示** ——
 * `__vm_obj_get` 后面跟的是一串"允许的类型号"，对不上就报运行期错误。
 * 类型名（第 5 个实参 / 第 2 个实参）只是个诊断用的字符串，放在数据区里。
 */
void testObj()
{
    // 数据区：地址 0 是 "int"，地址 4 是 "hi"（当"类型名"和"字符串值"用）。
    stackvm::DataArea data;
    data.binary.assign(32u, 0u);
    const char *names = "int\0hi\0";
    for (std::size_t index = 0; index < 7u; ++index)
        data.binary[index] = static_cast<std::uint8_t>(names[index]);
    data.symbols["tname"] = 0u;
    data.symbols["sname"] = 4u;   // "int" 之后就是 "hi"

    // 1. 空格子：类型号 0。
    {
        const stackvm::QuadFile file = stackvm::parseQuads(
            "0: (decl, i64, global, o)\n"
            "1: (call, __vm_obj_new, _, o)\n"
            "2: (param, o, _, a0)\n"
            "3: (call, __vm_obj_name, _, t)\n"
            "4: (write, t, _, _)\n"
            "5: (halt, _, _, _)\n",
            "<test>");
        stackvm::Program program;
        CHECK(stackvm::assemble(file, program));
        stackvm::Machine machine;
        machine.setData(&data);
        const stackvm::RunResult result = machine.run(program);
        CHECK(result.ok);
        CHECK(result.output == std::string("0"));   // 还没 set 过 → 类型号 0
        CHECK(result.liveObjects == 1u);            // 没释放 → 停机时还挂着
    }

    // 2. 装 int 42（类型号 1）→ name 给 1、get 取回 42；再复制一份，改副本不影响原件。
    {
        const stackvm::QuadFile file = stackvm::parseQuads(
            "0: (decl, i64, global, o)\n"
            "1: (decl, i64, global, p)\n"
            "2: (decl, i64, global, t)\n"
            "3: (call, __vm_obj_new, _, o)\n"
            "4: (param, o, _, a0)\n"
            "5: (param, 42, _, a1)\n"
            "6: (param, 1, _, a2)\n"     // 类型号 = 1
            "7: (param, 0, _, a3)\n"     // 值不是字符串
            "8: (param, 0, _, a4)\n"     // 不是对象句柄
            "9: (param, 0, _, a5)\n"     // 类型名地址 = tname
            "10: (call, __vm_obj_set, _, t)\n"
            "11: (param, o, _, a0)\n"
            "12: (call, __vm_obj_name, _, t)\n"
            "13: (write, t, _, _)\n"                       // 期望 1
            "14: (param, o, _, a0)\n"
            "15: (param, 0, _, a1)\n"                      // 要取的类型名地址
            "16: (param, 0, _, a2)\n"                      // 不是字符串
            "17: (param, 0, _, a3)\n"                      // 不是浮点
            "18: (param, 0, _, a4)\n"                      // 不是对象句柄
            "19: (param, 1, _, a5)\n"                      // 允许的类型号
            "20: (call, __vm_obj_get, _, t)\n"
            "21: (write, t, _, _)\n"                       // 期望 42
            "22: (param, o, _, a0)\n"
            "23: (call, __vm_object_copy, _, p)\n"         // 值语义：复制一格
            "24: (param, p, _, a0)\n"
            "25: (param, 7, _, a1)\n"
            "26: (param, 2, _, a2)\n"                      // 副本改成 类型号 2
            "27: (param, 0, _, a3)\n"
            "28: (param, 0, _, a4)\n"
            "29: (param, 0, _, a5)\n"
            "30: (call, __vm_obj_set, _, t)\n"
            "31: (param, o, _, a0)\n"
            "32: (call, __vm_obj_name, _, t)\n"
            "33: (write, t, _, _)\n"                       // 原件仍是 1
            "34: (param, p, _, a0)\n"
            "35: (call, __vm_obj_name, _, t)\n"
            "36: (write, t, _, _)\n"                       // 副本是 2
            "37: (halt, _, _, _)\n",
            "<test>");
        stackvm::Program program;
        CHECK(stackvm::assemble(file, program));
        stackvm::Machine machine;
        machine.setData(&data);
        const stackvm::RunResult result = machine.run(program);
        CHECK(result.ok);
        CHECK(result.output == std::string("1 42 1 2"));
    }

    // 3. 装字符串："hi"（值 = 数据区地址，按副本存）→ get 拿回一根**新的堆串**。
    {
        const stackvm::QuadFile file = stackvm::parseQuads(
            "0: (decl, i64, global, o)\n"
            "1: (decl, i64, global, t)\n"
            "2: (call, __vm_obj_new, _, o)\n"
            "3: (addr, sname, _, s)\n"
            "4: (param, o, _, a0)\n"
            "5: (param, s, _, a1)\n"      // 值 = "hi" 的地址
            "6: (param, 5, _, a2)\n"      // 类型号 = 5（string）
            "7: (param, 1, _, a3)\n"      // 值是字符串
            "8: (param, 0, _, a4)\n"
            "9: (param, 0, _, a5)\n"
            "10: (call, __vm_obj_set, _, t)\n"
            "11: (param, o, _, a0)\n"
            "12: (param, 0, _, a1)\n"
            "13: (param, 1, _, a2)\n"     // 取字符串
            "14: (param, 0, _, a3)\n"
            "15: (param, 0, _, a4)\n"
            "16: (param, 5, _, a5)\n"
            "17: (call, __vm_obj_get, _, t)\n"
            "18: (param, t, _, a0)\n"
            "19: (call, __vm_str_len, _, t)\n"
            "20: (write, t, _, _)\n"      // 期望 2（"hi" 两个字节）
            "21: (halt, _, _, _)\n",
            "<test>");
        stackvm::Program program;
        CHECK(stackvm::assemble(file, program));
        stackvm::Machine machine;
        machine.setData(&data);
        const stackvm::RunResult result = machine.run(program);
        CHECK(result.ok);
        CHECK(result.output == std::string("2"));
    }

    // 4. 类型号对不上：装上 1，按 5 取 → 运行期错误（文案里两个都要说清）。
    {
        const stackvm::QuadFile file = stackvm::parseQuads(
            "0: (decl, i64, global, o)\n"
            "1: (decl, i64, global, t)\n"
            "2: (call, __vm_obj_new, _, o)\n"
            "3: (param, o, _, a0)\n"
            "4: (param, 42, _, a1)\n"
            "5: (param, 1, _, a2)\n"
            "6: (param, 0, _, a3)\n"
            "7: (param, 0, _, a4)\n"
            "8: (param, 0, _, a5)\n"
            "9: (call, __vm_obj_set, _, t)\n"
            "10: (param, o, _, a0)\n"
            "11: (param, 8, _, a1)\n"     // 要取的类型名 = "hi"（随便一个名字）
            "12: (param, 0, _, a2)\n"
            "13: (param, 0, _, a3)\n"
            "14: (param, 0, _, a4)\n"
            "15: (param, 5, _, a5)\n"     // 只允许类型号 5
            "16: (call, __vm_obj_get, _, t)\n"
            "17: (halt, _, _, _)\n",
            "<test>");
        stackvm::Program program;
        CHECK(stackvm::assemble(file, program));
        stackvm::Machine machine;
        machine.setData(&data);
        const stackvm::RunResult result = machine.run(program);
        CHECK(!result.ok);
        CHECK(result.error.find("类型对不上") != std::string::npos);
    }

    // 5. 释放：还回去之后 `liveObjects` 归零，重复释放报"已经释放过了"。
    {
        const stackvm::QuadFile file = stackvm::parseQuads(
            "0: (decl, i64, global, o)\n"
            "1: (call, __vm_obj_new, _, o)\n"
            "2: (free, o, _, _)\n"
            "3: (halt, _, _, _)\n",
            "<test>");
        stackvm::Program program;
        CHECK(stackvm::assemble(file, program));
        stackvm::Machine machine;
        machine.setData(&data);
        const stackvm::RunResult result = machine.run(program);
        CHECK(result.ok);
        CHECK(result.liveObjects == 0u);
    }
    {
        const stackvm::QuadFile file = stackvm::parseQuads(
            "0: (decl, i64, global, o)\n"
            "1: (call, __vm_obj_new, _, o)\n"
            "2: (free, o, _, _)\n"
            "3: (free, o, _, _)\n"
            "4: (halt, _, _, _)\n",
            "<test>");
        stackvm::Program program;
        CHECK(stackvm::assemble(file, program));
        stackvm::Machine machine;
        machine.setData(&data);
        const stackvm::RunResult result = machine.run(program);
        CHECK(!result.ok);
        CHECK(result.error.find("已经释放过了") != std::string::npos);
    }

    // 6. 对象句柄装进 obj 时按副本存；取出来也再给一份副本，释放外层会递归释放里面那份。
    {
        const stackvm::QuadFile file = stackvm::parseQuads(
            "0: (decl, i64, global, inner)\n"
            "1: (decl, i64, global, outer)\n"
            "2: (decl, i64, global, copy)\n"
            "3: (decl, i64, global, t)\n"
            "4: (call, __vm_obj_new, _, inner)\n"
            "5: (call, __vm_obj_new, _, outer)\n"
            "6: (param, inner, _, a0)\n"
            "7: (param, 42, _, a1)\n"
            "8: (param, 1, _, a2)\n"
            "9: (param, 0, _, a3)\n"
            "10: (param, 0, _, a4)\n"
            "11: (param, 0, _, a5)\n"
            "12: (call, __vm_obj_set, _, t)\n"
            "13: (param, outer, _, a0)\n"
            "14: (param, inner, _, a1)\n"
            "15: (param, 8, _, a2)\n"
            "16: (param, 0, _, a3)\n"
            "17: (param, 1, _, a4)\n"
            "18: (param, 0, _, a5)\n"
            "19: (call, __vm_obj_set, _, t)\n"
            "20: (param, outer, _, a0)\n"
            "21: (param, 0, _, a1)\n"
            "22: (param, 0, _, a2)\n"
            "23: (param, 0, _, a3)\n"
            "24: (param, 1, _, a4)\n"
            "25: (param, 8, _, a5)\n"
            "26: (call, __vm_obj_get, _, copy)\n"
            "27: (param, copy, _, a0)\n"
            "28: (param, 0, _, a1)\n"
            "29: (param, 0, _, a2)\n"
            "30: (param, 0, _, a3)\n"
            "31: (param, 0, _, a4)\n"
            "32: (param, 1, _, a5)\n"
            "33: (call, __vm_obj_get, _, t)\n"
            "34: (write, t, _, _)\n"
            "35: (free, inner, _, _)\n"
            "36: (free, outer, _, _)\n"
            "37: (free, copy, _, _)\n"
            "38: (halt, _, _, _)\n",
            "<test>");
        stackvm::Program program;
        CHECK(stackvm::assemble(file, program));
        stackvm::Machine machine;
        machine.setData(&data);
        const stackvm::RunResult result = machine.run(program);
        CHECK(result.ok);
        CHECK(result.output == std::string("42"));
        CHECK(result.liveObjects == 0u);
    }

    // 7. 容器可以保存对象句柄类值：存进去是副本，查找仍按原句柄身份匹配。
    {
        const stackvm::QuadFile file = stackvm::parseQuads(
            "0: (decl, i64, global, inner)\n"
            "1: (decl, i64, global, v)\n"
            "2: (decl, i64, global, s)\n"
            "3: (decl, i64, global, m)\n"
            "4: (decl, i64, global, copy)\n"
            "5: (decl, i64, global, t)\n"
            "6: (call, __vm_obj_new, _, inner)\n"
            "7: (call, __vm_vec_new, _, v)\n"
            "8: (call, __vm_set_new, _, s)\n"
            "9: (call, __vm_map_new, _, m)\n"
            "10: (param, inner, _, a0)\n"
            "11: (param, 42, _, a1)\n"
            "12: (param, 1, _, a2)\n"
            "13: (param, 0, _, a3)\n"
            "14: (param, 0, _, a4)\n"
            "15: (param, 0, _, a5)\n"
            "16: (call, __vm_obj_set, _, t)\n"
            "17: (param, v, _, a0)\n"
            "18: (param, inner, _, a1)\n"
            "19: (param, 0, _, a2)\n"
            "20: (param, 1, _, a3)\n"
            "21: (call, __vm_vec_push, _, t)\n"
            "22: (param, v, _, a0)\n"
            "23: (param, inner, _, a1)\n"
            "24: (param, 0, _, a2)\n"
            "25: (param, 1, _, a3)\n"
            "26: (call, __vm_vec_find, _, t)\n"
            "27: (write, t, _, _)\n"
            "28: (param, v, _, a0)\n"
            "29: (param, 0, _, a1)\n"
            "30: (param, 0, _, a2)\n"
            "31: (param, 1, _, a3)\n"
            "32: (call, __vm_vec_at, _, copy)\n"
            "33: (param, copy, _, a0)\n"
            "34: (param, 0, _, a1)\n"
            "35: (param, 0, _, a2)\n"
            "36: (param, 0, _, a3)\n"
            "37: (param, 0, _, a4)\n"
            "38: (param, 1, _, a5)\n"
            "39: (call, __vm_obj_get, _, t)\n"
            "40: (write, t, _, _)\n"
            "41: (free, copy, _, _)\n"
            "42: (param, s, _, a0)\n"
            "43: (param, inner, _, a1)\n"
            "44: (param, 0, _, a2)\n"
            "45: (param, 1, _, a3)\n"
            "46: (call, __vm_set_add, _, t)\n"
            "47: (param, s, _, a0)\n"
            "48: (param, inner, _, a1)\n"
            "49: (param, 0, _, a2)\n"
            "50: (param, 1, _, a3)\n"
            "51: (call, __vm_set_has, _, t)\n"
            "52: (write, t, _, _)\n"
            "53: (param, s, _, a0)\n"
            "54: (param, 0, _, a1)\n"
            "55: (param, 0, _, a2)\n"
            "56: (param, 1, _, a3)\n"
            "57: (call, __vm_set_at, _, copy)\n"
            "58: (param, copy, _, a0)\n"
            "59: (param, 0, _, a1)\n"
            "60: (param, 0, _, a2)\n"
            "61: (param, 0, _, a3)\n"
            "62: (param, 0, _, a4)\n"
            "63: (param, 1, _, a5)\n"
            "64: (call, __vm_obj_get, _, t)\n"
            "65: (write, t, _, _)\n"
            "66: (free, copy, _, _)\n"
            "67: (param, m, _, a0)\n"
            "68: (param, inner, _, a1)\n"
            "69: (param, 0, _, a2)\n"
            "70: (param, 1, _, a3)\n"
            "71: (param, inner, _, a4)\n"
            "72: (param, 0, _, a5)\n"
            "73: (param, 1, _, a6)\n"
            "74: (call, __vm_map_put, _, t)\n"
            "75: (param, m, _, a0)\n"
            "76: (param, inner, _, a1)\n"
            "77: (param, 0, _, a2)\n"
            "78: (param, 1, _, a3)\n"
            "79: (call, __vm_map_has, _, t)\n"
            "80: (write, t, _, _)\n"
            "81: (param, m, _, a0)\n"
            "82: (param, inner, _, a1)\n"
            "83: (param, 0, _, a2)\n"
            "84: (param, 1, _, a3)\n"
            "85: (param, 0, _, a4)\n"
            "86: (param, 1, _, a5)\n"
            "87: (call, __vm_map_get, _, copy)\n"
            "88: (param, copy, _, a0)\n"
            "89: (param, 0, _, a1)\n"
            "90: (param, 0, _, a2)\n"
            "91: (param, 0, _, a3)\n"
            "92: (param, 0, _, a4)\n"
            "93: (param, 1, _, a5)\n"
            "94: (call, __vm_obj_get, _, t)\n"
            "95: (write, t, _, _)\n"
            "96: (free, copy, _, _)\n"
            "97: (free, inner, _, _)\n"
            "98: (free, v, _, _)\n"
            "99: (free, s, _, _)\n"
            "100: (free, m, _, _)\n"
            "101: (halt, _, _, _)\n",
            "<test>");
        stackvm::Program program;
        CHECK(stackvm::assemble(file, program));
        stackvm::Machine machine;
        machine.setData(&data);
        const stackvm::RunResult result = machine.run(program);
        CHECK(result.ok);
        CHECK(result.output == std::string("0 42 1 42 1 42"));
        CHECK(result.liveObjects == 0u);
    }
    // 8. map.clear 必须释放自己持有的对象键和值。
    {
        const stackvm::QuadFile file = stackvm::parseQuads(
            "0: (decl, i64, global, key)\n"
            "1: (decl, i64, global, value)\n"
            "2: (decl, i64, global, map)\n"
            "3: (decl, i64, global, tmp)\n"
            "4: (call, __vm_obj_new, _, key)\n"
            "5: (call, __vm_obj_new, _, value)\n"
            "6: (call, __vm_map_new, _, map)\n"
            "7: (param, map, _, a0)\n"
            "8: (param, key, _, a1)\n"
            "9: (param, 0, _, a2)\n"
            "10: (param, 1, _, a3)\n"
            "11: (param, value, _, a4)\n"
            "12: (param, 0, _, a5)\n"
            "13: (param, 1, _, a6)\n"
            "14: (call, __vm_map_put, _, tmp)\n"
            "15: (param, map, _, a0)\n"
            "16: (call, __vm_map_clear, _, tmp)\n"
            "17: (free, key, _, _)\n"
            "18: (free, value, _, _)\n"
            "19: (free, map, _, _)\n"
            "20: (halt, _, _, _)\n",
            "<test>");
        stackvm::Program program;
        CHECK(stackvm::assemble(file, program));
        stackvm::Machine machine;
        const stackvm::RunResult result = machine.run(program);
        CHECK(result.ok);
        CHECK(result.liveObjects == 0u);
    }
}

/** 二进制程序：完整往返、直接执行，以及校验和损坏诊断。 */
void testProgramBinary()
{
    const std::string path = "stackvm_test_program.svmb";
    const std::string corruptPath = "stackvm_test_program_corrupt.svmb";
    const std::string compactPath = "stackvm_test_program_compact.svmb";

    stackvm::Program source;
    source.name = "binary-roundtrip";
    stackvm::Instruction push;
    push.op = stackvm::Op::PushInt;
    push.name = "含中文的诊断名";
    push.name2 = "result";
    push.intValue = 42;
    push.doubleValue = 1.25;
    push.textValue = std::string("a\0b", 3u);
    push.unsignedOp = true;
    push.isFloat = true;
    push.format = stackvm::PrintFormat::Unsigned;
    push.quadIndex = 7u;
    source.code.push_back(push);
    stackvm::Instruction print;
    print.op = stackvm::Op::Print;
    source.code.push_back(print);
    stackvm::Instruction minimum;
    minimum.op = stackvm::Op::PushInt;
    minimum.intValue = std::numeric_limits<std::int64_t>::min();
    source.code.push_back(minimum);
    stackvm::Instruction halt;
    halt.op = stackvm::Op::Halt;
    source.code.push_back(halt);
    source.labels["entry"] = 0u;
    source.types["value"] = stackvm::TypeInfo{"u32", 32, true, false, 4u};
    source.parameters["entry"] = {"left", "right"};
    source.exports.insert("entry");
    stackvm::SourceLocation entryLocation;
    entryLocation.function = "entry";
    entryLocation.file = "demo.mgl";
    entryLocation.line = 12;
    entryLocation.quadLine = 30u;
    source.debug.add(0u, entryLocation);
    stackvm::SourceLocation laterLocation;
    laterLocation.function = "entry";
    laterLocation.file = "demo.mgl";
    laterLocation.line = 14;
    laterLocation.quadLine = 32u;
    source.debug.add(2u, laterLocation);

    std::vector<std::string> diagnostics;
    CHECK(stackvm::saveProgramBinary(source, path, &diagnostics));
    CHECK(diagnostics.empty());
    CHECK(stackvm::isProgramBinary(path));

    /* v4：沿用变长指令编码，末尾依次是导出标签表与调试信息表。 */
    stackvm::Program compact;
    compact.name = "compact";
    compact.code.push_back(halt);
    CHECK(stackvm::saveProgramBinary(compact, compactPath, &diagnostics));
    std::ifstream compactInput(compactPath, std::ios::binary);
    const std::vector<unsigned char> compactBytes((std::istreambuf_iterator<char>(compactInput)),
                                                   std::istreambuf_iterator<char>());
    compactInput.close();
    CHECK(compactBytes.size() < 64u);
    CHECK(compactBytes.size() > 10u);
    CHECK(compactBytes[8] == 4u && compactBytes[9] == 0u); /* 小端格式版本 4 */

    stackvm::Program restored;
    CHECK(stackvm::loadProgramBinary(path, restored, &diagnostics));
    CHECK(restored.name == source.name);
    CHECK(restored.code.size() == source.code.size());
    CHECK(restored.code[0].op == stackvm::Op::PushInt);
    CHECK(restored.code[0].name == push.name);
    CHECK(restored.code[0].name2 == push.name2);
    CHECK(restored.code[0].intValue == push.intValue);
    CHECK(restored.code[0].doubleValue == push.doubleValue);
    CHECK(restored.code[0].textValue == push.textValue);
    CHECK(restored.code[0].unsignedOp && restored.code[0].isFloat);
    CHECK(restored.code[0].format == stackvm::PrintFormat::Unsigned);
    CHECK(restored.code[0].quadIndex == 7u);
    CHECK(restored.code[2].intValue == std::numeric_limits<std::int64_t>::min());
    CHECK(restored.labels.at("entry") == 0u);
    CHECK(restored.types.at("value").name == "u32");
    CHECK(restored.parameters.at("entry").size() == 2u);
    /* 调试信息：整表往返，且查询取"不超过目标指令的最近一条"。 */
    CHECK(restored.debug.size() == 2u);
    const stackvm::SourceLocation *dumpedEntry = restored.debug.lookup(0u);
    CHECK(dumpedEntry != nullptr);
    if (dumpedEntry != nullptr)
    {
        CHECK(dumpedEntry->function == "entry");
        CHECK(dumpedEntry->file == "demo.mgl");
        CHECK(dumpedEntry->line == 12);
        CHECK(dumpedEntry->quadLine == 30u);
        CHECK(dumpedEntry->toString() == "demo.mgl:12");
    }
    const stackvm::SourceLocation *dumpedLater = restored.debug.lookup(3u);
    CHECK(dumpedLater != nullptr && dumpedLater->line == 14);
    CHECK(restored.debug.lookup(2u) != nullptr && restored.debug.lookup(2u)->line == 14);
    CHECK(restored.exports.count("entry") == 1u);

    stackvm::Machine machine;
    const stackvm::RunResult result = machine.run(restored);
    CHECK(result.ok);
    CHECK(result.output == "42");

    std::ifstream input(path, std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(input)),
                            std::istreambuf_iterator<char>());
    input.close();
    CHECK(!bytes.empty());
    if (!bytes.empty()) bytes.back() ^= 0x01;
    std::ofstream corrupt(corruptPath, std::ios::binary);
    corrupt.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    corrupt.close();
    diagnostics.clear();
    CHECK(!stackvm::loadProgramBinary(corruptPath, restored, &diagnostics));
    CHECK(!diagnostics.empty());

    std::remove(path.c_str());
    std::remove(corruptPath.c_str());
    std::remove(compactPath.c_str());
}

/**
 * 调试：组装时自动登记源码位置、断点、单步、源码级调用栈、出错位置。
 */
void testDebugger()
{
    const stackvm::QuadFile file = stackvm::parseQuads(
        "0: (label, _, _, main)\n"
        "1: (:=, 3, _, base)\n"
        "2: (param, base, _, value)\n"
        "3: (call, twice, _, result)\n"
        "4: (write, result, _, _)\n"
        "5: (halt, _, _, _)\n"
        "6: (decl, u32, local, value)\n"
        "7: (label, _, _, twice)\n"
        "8: (+:u32, value, value, doubled)\n"
        "9: (ret:u32, doubled, _, _)\n",
        "demo.quad");
    stackvm::Program program;
    CHECK(stackvm::assemble(file, program));
    CHECK(program.ok());
    CHECK(!program.debug.empty());

    /* 组装期自动登记：函数名来自最近的 (label …)，行号来自四元式文本行。 */
    const std::size_t mainEntry = program.labels.at("main");
    const std::size_t twiceEntry = program.labels.at("twice");
    const stackvm::SourceLocation *mainLocation = program.debug.lookup(mainEntry);
    const stackvm::SourceLocation *twiceLocation = program.debug.lookup(twiceEntry);
    CHECK(mainLocation != nullptr && mainLocation->function == "main");
    CHECK(twiceLocation != nullptr && twiceLocation->function == "twice");
    if (twiceLocation != nullptr)
    {
        /* 标签本身不产生指令，"标签"与"紧跟它的第一条语句"落在同一个下标上，
         * 按约定后者胜出：所以这里是 `8: (+:u32 …)` 那一行的文本行号。 */
        CHECK(twiceLocation->quadLine == 9u);
        CHECK(twiceLocation->toString() == "demo.quad:9");
    }
    CHECK(program.debug.lookup(program.code.size()) != nullptr);
    CHECK(program.debug.entries.size() < program.code.size()); /* 稀疏：比指令数少 */

    /* 断点：停在 twice 入口（**执行之前**），并能看到调用者。 */
    {
        stackvm::Machine machine;
        machine.setBreakpoints({twiceEntry});
        CHECK(machine.breakpointCount() == 1u);
        std::vector<stackvm::DebugEvent> stops;
        machine.setDebugHook([&](const stackvm::DebugEvent &event) {
            stops.push_back(event);
            return stackvm::DebugAction::Continue;
        });
        const stackvm::RunResult result = machine.run(program);
        CHECK(result.ok);
        CHECK(stops.size() == 1u);
        if (stops.size() == 1u)
        {
            CHECK(stops[0].breakpoint);
            CHECK(stops[0].instruction == twiceEntry);
            CHECK(stops[0].location.function == "twice");
            CHECK(stops[0].frames.size() == 2u); /* twice 自己 + 调用它的 main */
            CHECK(stops[0].frames.front().function == "main");
            CHECK(stops[0].frames.back().function == "twice");
        }
        CHECK(machine.currentLocation().function == "twice");
        /* 停机后的快照：twice 已经返回，只剩主程序那一帧。 */
        CHECK(machine.callStack().size() == 1u);
    }

    /* 单步：每条指令执行前回调一次，次数与执行条数一致。 */
    {
        stackvm::Machine machine;
        machine.setSingleStep(true);
        std::vector<std::size_t> visited;
        machine.setDebugHook([&](const stackvm::DebugEvent &event) {
            visited.push_back(event.instruction);
            return stackvm::DebugAction::StepInto;
        });
        const stackvm::RunResult result = machine.run(program);
        CHECK(result.ok);
        CHECK(!visited.empty());
        CHECK(visited.size() == result.steps);
    }

    /* 中止：结果里带中止位置与源码级调用栈。 */
    {
        stackvm::Machine machine;
        machine.setBreakpoints({twiceEntry});
        machine.setDebugHook([](const stackvm::DebugEvent &) { return stackvm::DebugAction::Abort; });
        const stackvm::RunResult result = machine.run(program);
        CHECK(!result.ok);
        CHECK(result.error.find("demo.quad:9") != std::string::npos);
        CHECK(result.failureLocation.function == "twice");
        CHECK(result.sourceTrace.size() == 2u);
    }

    /* 运行期出错：错误位置直接指到出错那条四元式所属的源码位置。 */
    {
        const stackvm::QuadFile broken = stackvm::parseQuads(
            "0: (label, _, _, main)\n"
            "1: (:=, undefinedValue, _, target)\n"
            "2: (halt, _, _, _)\n",
            "broken.quad");
        stackvm::Program brokenProgram;
        CHECK(stackvm::assemble(broken, brokenProgram));
        stackvm::Machine machine;
        const stackvm::RunResult result = machine.run(brokenProgram);
        CHECK(!result.ok);
        CHECK(result.error.find("undefinedValue") != std::string::npos);
        CHECK(!result.sourceTrace.empty());
        CHECK(result.failureLocation.function == "main");
        CHECK(result.failureLocation.toString() == "broken.quad:2");
    }

    /* 外部挂表优先于程序自带的那张。 */
    {
        stackvm::DebugInfo external;
        stackvm::SourceLocation location;
        location.function = "外部表";
        location.file = "override.mgl";
        location.line = 7;
        external.add(0u, location);
        stackvm::Machine machine;
        machine.attachDebugInfo(&external);
        CHECK(machine.debugInfo() == nullptr); /* 还没跑过，表要等 run() 才生效 */
        const stackvm::RunResult result = machine.run(program);
        CHECK(result.ok);
        CHECK(machine.debugInfo() == &external);
        CHECK(machine.currentLocation().file == "override.mgl");
        CHECK(external.lookup(1u)->line == 7);
    }
}

/**
 * `__vm_panic`：唯一的中止原语。
 *
 * `__vm_exit` 与 `__vm_assert` 已删除 —— Magel 侧的中止只有 `std::panic`，
 * `std::assert` 是标准库对它的包装，虚拟机里不再有第二条停机路径。
 */
void testPanic()
{
    std::string output;
    stackvm::DataArea data;
    data.binary.resize(32u, 0u);
    const char message[] = "fatal state";
    CHECK(data.writeBytes(8u, message, sizeof(message)));
    const stackvm::QuadFile panicFile = stackvm::parseQuads(
        "0: (param, 8, _, __magel_value)\n"
        "1: (call, __vm_panic, _, t)\n"
        "2: (write, 99, _, _)\n"
        "3: (halt, _, _, _)\n",
        "<panic-test>");
    stackvm::Program panicProgram;
    CHECK(stackvm::assemble(panicFile, panicProgram));
    stackvm::Machine panicMachine;
    panicMachine.setData(&data);
    const stackvm::RunResult panicked = panicMachine.run(panicProgram);
    CHECK(!panicked.ok);
    CHECK(panicked.error == "panic：fatal state");
    CHECK(panicked.output.empty());
}

/** C++ 胶水：整数/浮点槽、字符串内存访问和参数个数诊断。 */
void testNativeRegistry()
{
    stackvm::NativeRegistry registry;
    std::string registrationError;
    CHECK(registry.add(
        "cpp_measure", 2u,
        [](stackvm::NativeCallContext &context, const std::vector<stackvm::NativeValue> &args) {
            std::string text;
            if (!context.readString(args[0].asInt(), text))
            {
                context.fail("宿主读字符串失败");
                return stackvm::NativeValue::fromInt(0);
            }
            return stackvm::NativeValue::fromInt(
                static_cast<std::int64_t>(text.size()) + args[1].asInt());
        },
        &registrationError));
    CHECK(registrationError.empty());
    CHECK(!registry.add("cpp_measure", 2u, {}, &registrationError));

    stackvm::DataArea data;
    data.binary.resize(14u, 0u);
    const char word[] = "hello";
    CHECK(data.writeBytes(8u, word, sizeof(word)));
    const stackvm::QuadFile file = stackvm::parseQuads(
        "0: (param, 8, _, text)\n"
        "1: (param, 7, _, extra)\n"
        "2: (call, cpp_measure, _, value)\n"
        "3: (write, value, _, _)\n"
        "4: (halt, _, _, _)\n",
        "<native-registry>");
    stackvm::Program program;
    CHECK(stackvm::assemble(file, program));
    stackvm::Machine machine;
    machine.setData(&data);
    machine.setNativeRegistry(&registry);
    const stackvm::RunResult result = machine.run(program);
    CHECK(result.ok);
    CHECK(result.output == "12");

    /* `.svmb` 只序列化调用符号；加载后重新挂同一注册表即可运行。 */
    const std::string binaryPath = "stackvm_test_native_registry.svmb";
    std::vector<std::string> diagnostics;
    CHECK(stackvm::saveProgramBinary(program, binaryPath, &diagnostics));
    stackvm::Program restored;
    CHECK(stackvm::loadProgramBinary(binaryPath, restored, &diagnostics));
    stackvm::Machine restoredMachine;
    restoredMachine.setData(&data);
    restoredMachine.setNativeRegistry(&registry);
    const stackvm::RunResult restoredResult = restoredMachine.run(restored);
    CHECK(restoredResult.ok);
    CHECK(restoredResult.output == "12");
    std::remove(binaryPath.c_str());
}

void testExamples(const std::string &directory)
{
    const std::vector<std::string> names = {
        "minimal.quad", "01_types.quad", "03_float.quad",
        "06_muldiv.quad", "07_io.quad", "08_call.quad", "10_signed.quad",
        "13_heap.quad",
    };

    for (const std::string &name : names)
    {
        const std::string path = directory + "/" + name;
        const stackvm::QuadFile file = stackvm::loadQuadFile(path);
        if (!file.ok())
        {
            std::cerr << "  （跳过 " << name << "：" << file.diagnostics.front() << "）\n";
            continue;
        }

        stackvm::Program program;
        if (!stackvm::assemble(file, program))
        {
            std::cerr << "  （跳过 " << name << "：" << program.diagnostics.front() << "）\n";
            continue;
        }

        std::vector<std::string> input;
        {
            std::string current;
            for (const char ch : file.input)
            {
                if (ch == ' ' || ch == '\t' || ch == ',')
                {
                    if (!current.empty()) { input.push_back(current); current.clear(); }
                    continue;
                }
                current += ch;
            }
            if (!current.empty()) input.push_back(current);
        }

        stackvm::Machine machine;
        const stackvm::RunResult result = machine.run(program, input);
        ++checks;
        if (!result.ok)
        {
            ++failures;
            std::cerr << "  失败：" << name << " 运行出错：" << result.error << "\n";
            continue;
        }
        if (file.expected.empty()) continue;

        if (!file.expectedText.empty() && result.text != file.expectedText)
        {
            ++failures;
            std::cerr << "  失败：" << name << " 文本输出\n    期望：" << file.expectedText
                      << "\n    实际：" << result.text << "\n";
            continue;
        }
        if (result.output != file.expected)
        {
            ++failures;
            std::cerr << "  失败：" << name << "\n    期望：" << file.expected
                      << "\n    实际：" << result.output << "\n";
        }
    }
}

} // namespace

/**
 * @brief 程序入口。
 * @param argc 参数个数。
 * @param argv 参数数组。
 * @return 0 全部通过，1 有失败。
 */
int main(int argc, char **argv)
{
    const std::string directory = (argc > 1) ? argv[1] : "examples";

    testTypes();
    testStackLimit();
    testFloat();
    testBranchAndLoop();
    testCall();
    testLocalFrames();
    testMulDivAndIo();
    testDiagnostics();
    testDisassembly();
    testOptimize();
    testHeap();
    testRemovedMathNatives();
    testBoundsCheck();
    testFunctionPointer();
    testCoroutines();
    testCoroutineMessages();
    testTimingAndInstructionHook();
    testDataArea();
    testObj();
    testProgramBinary();
    testDebugger();
    testPanic();
    testNativeRegistry();
    testExamples(directory);

    if (failures == 0)
    {
        std::cout << "stackvm-tests：全部通过（" << checks << " 项断言）\n";
        return 0;
    }
    std::cout << "stackvm-tests：" << failures << " / " << checks << " 项断言失败\n";
    return 1;
}
