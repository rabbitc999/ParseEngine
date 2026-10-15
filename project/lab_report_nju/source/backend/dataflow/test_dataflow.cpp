// 单元测试：解析与 CFG、格的代数律、四个实例的结果、两种求解器的一致性、
// 单调性检查（含"故意写错的格"必须被发现）。
#include "dataflow.hpp"

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

dataflow::Program parseOrDie(const std::string& text) {
    dataflow::Program program;
    std::vector<std::string> errors;
    if (!dataflow::parse(text, program, errors)) {
        for (const auto& error : errors) std::cerr << "  解析错误: " << error << "\n";
        ++failures;
    }
    return program;
}

// 菱形：d 在两条路径上各定义一次，汇合点上两个定值都到达。
const char* kDiamond = R"(
label entry
  mov a, 1
  mov b, 2
  add c, a, b
  jz c, L1
  mov d, a
  jmp L2
label L1
  mov d, b
label L2
  print d
  halt
)";

// 循环：i、s 跨回边活跃；i 有两个定值到达循环头。
const char* kLoop = R"(
label entry
  mov i, 0
  mov s, 0
label L1
  add s, s, i
  add i, i, 1
  mov k, i
  sub k, k, 3
  jnz k, L1
label L2
  print s
  halt
)";

std::string setsOf(const dataflow::Element& element) { return dataflow::formatElement(element); }

void testParse() {
    const auto program = parseOrDie(kDiamond);
    CHECK(program.size() == 4);
    CHECK(program[0].successors.size() == 2);      // jz 目标 + 顺序落下的块
    CHECK(program[3].predecessors.size() == 2);
    const std::string text = dataflow::print(program);
    dataflow::Program again;
    std::vector<std::string> errors;
    CHECK(dataflow::parse(text, again, errors));
    CHECK(again.size() == program.size());
    CHECK(dataflow::expressions(program).size() == 1);   // 只有 a add b
}

void testLatticeLaws() {
    const auto program = parseOrDie(kDiamond);
    for (const auto& name : dataflow::analysisNames()) {
        const auto analysis = dataflow::makeAnalysis(name, program);
        const dataflow::Solution solution = dataflow::solveWorklist(program, *analysis);
        for (const auto& element : solution.out) {
            // 单位元：merge(⊤, x) == x == merge(x, ⊤)
            CHECK(analysis->equal(analysis->merge(analysis->identity(), element), element));
            CHECK(analysis->equal(analysis->merge(element, analysis->identity()), element));
            // 幂等：merge(x, x) == x
            CHECK(analysis->equal(analysis->merge(element, element), element));
            // 格序自洽：x ⪯ x
            CHECK(analysis->lessEqual(element, element));
        }
        // 交换律：merge(x, y) == merge(y, x)
        for (std::size_t i = 0; i + 1 < solution.out.size(); ++i) {
            CHECK(analysis->equal(analysis->merge(solution.out[i], solution.out[i + 1]),
                                  analysis->merge(solution.out[i + 1], solution.out[i])));
        }
    }
}

void testLive() {
    const auto program = parseOrDie(kDiamond);
    const auto analysis = dataflow::makeAnalysis("live", program);
    const auto solution = dataflow::solveWorklist(program, *analysis);
    CHECK(analysis->direction() == dataflow::Direction::Backward);
    // 格序：并集格上更大就是"更靠上"，缩回去必须被判成不单调。
    CHECK(!analysis->lessEqual(dataflow::Element::ofSet({"a", "b"}),
                               dataflow::Element::ofSet({"a"})));
    CHECK(analysis->lessEqual(dataflow::Element::ofSet({"a"}),
                              dataflow::Element::ofSet({"a", "b"})));
    CHECK(setsOf(solution.in[0]) == "{}");
    CHECK(setsOf(solution.out[0]) == "{a, b}");
    CHECK(setsOf(solution.in[1]) == "{a}");
    CHECK(setsOf(solution.out[1]) == "{d}");
    CHECK(setsOf(solution.in[2]) == "{b}");
    CHECK(setsOf(solution.in[3]) == "{d}");
    CHECK(setsOf(solution.out[3]) == "{}");
}

void testReach() {
    const auto program = parseOrDie(kDiamond);
    const auto analysis = dataflow::makeAnalysis("reach", program);
    const auto solution = dataflow::solveWorklist(program, *analysis);
    // 汇合点：d 的两个定值都到达。
    CHECK(setsOf(solution.in[3]) == "{a@0, b@1, c@2, d@4, d@6}");
    // 定义会杀掉同一变量在本块之前的定值。
    CHECK(setsOf(solution.out[2]) == "{a@0, b@1, c@2, d@6}");
}

void testAvail() {
    const auto program = parseOrDie(kDiamond);
    const auto analysis = dataflow::makeAnalysis("avail", program);
    const auto solution = dataflow::solveWorklist(program, *analysis);
    CHECK(setsOf(solution.in[0]) == "{}");                 // 入口没有可用表达式
    CHECK(setsOf(solution.out[1]) == "{a add b}");
    CHECK(setsOf(solution.in[3]) == "{a add b}");          // 两条路径都算过 → 可用
    CHECK(analysis->equal(analysis->identity(),
                          dataflow::Element::ofSet({"a add b"})));   // 交集格的单位元是全集
}

void testConst() {
    const auto program = parseOrDie(kDiamond);
    const auto analysis = dataflow::makeAnalysis("const", program);
    const auto solution = dataflow::solveWorklist(program, *analysis);
    CHECK(setsOf(solution.out[0]) == "{a=1, b=2, c=3}");
    CHECK(setsOf(solution.out[1]) == "{a=1, b=2, c=3, d=1}");
    CHECK(setsOf(solution.out[2]) == "{a=1, b=2, c=3, d=2}");
    CHECK(setsOf(solution.in[3]) == "{a=1, b=2, c=3, d=⊥}");   // 1 与 2 合并 → 非常量
}

void testSolversAgree() {
    const char* programs[] = {kDiamond, kLoop};
    for (const char* text : programs) {
        const auto program = parseOrDie(text);
        for (const auto& name : dataflow::analysisNames()) {
            const auto analysis = dataflow::makeAnalysis(name, program);
            dataflow::Options options;
            const auto worklist = dataflow::solveWorklist(program, *analysis, options);
            const auto naive = dataflow::solveNaive(program, *analysis, options);
            CHECK(worklist.converged);
            CHECK(naive.converged);
            CHECK(worklist.monotone);
            CHECK(naive.monotone);
            CHECK(dataflow::sameSolution(worklist, naive, program));
            // 工作队列不应该比朴素轮询更费（默认参数下）。
            CHECK(worklist.transferApplications <= naive.transferApplications);
        }
    }
}

void testLoopResults() {
    const auto program = parseOrDie(kLoop);
    const auto live = dataflow::makeAnalysis("live", program);
    const auto liveSolution = dataflow::solveWorklist(program, *live);
    CHECK(setsOf(liveSolution.in[1]) == "{i, s}");          // 循环头：i、s 活跃
    CHECK(setsOf(liveSolution.in[2]) == "{s}");
    const auto reach = dataflow::makeAnalysis("reach", program);
    const auto reachSolution = dataflow::solveWorklist(program, *reach);
    CHECK(setsOf(reachSolution.in[1]) == "{i@0, i@3, k@5, s@1, s@2}");   // 入口与回边都到达
    const auto avail = dataflow::makeAnalysis("avail", program);
    const auto availSolution = dataflow::solveWorklist(program, *avail);
    CHECK(setsOf(availSolution.out[1]) == "{1 add i, k sub 3}");
    CHECK(setsOf(availSolution.in[1]) == "{}");             // 汇合点交集 → 都不算"可用"
}

/**
 * 故意写错的格：并集格上的传递函数，输入攒到 3 个元素时却"清空重来"。
 * 循环头第二次被处理时输入从 2 个元素涨到 3 个，输出反而从两个元素缩成 {"z"}，
 * 单调性检查必须发现。
 */
class BrokenAnalysis : public dataflow::Analysis {
public:
    std::string name() const override { return "broken"; }
    std::string title() const override { return "故意写错的格"; }
    dataflow::Direction direction() const override { return dataflow::Direction::Forward; }
    dataflow::Element identity() const override { return dataflow::Element::ofSet({}); }
    dataflow::Element boundary() const override { return dataflow::Element::ofSet({}); }
    dataflow::Element merge(const dataflow::Element& a, const dataflow::Element& b) const override {
        dataflow::Element result = a;
        result.items.insert(b.items.begin(), b.items.end());
        return result;
    }
    dataflow::Element transfer(const dataflow::Program&, std::size_t, std::size_t,
                               const dataflow::Instruction& ins,
                               const dataflow::Element& state) const override {
        dataflow::Element result = state;
        if (state.items.size() >= 3) {
            result.items.clear();                 // 非单调：输入越大输出越小
            result.items.insert("z");
            return result;
        }
        result.items.insert(ins.op + "!");        // 正常的一步：往并集里加东西
        return result;
    }
};

void testMonotonicityCheck() {
    // 循环：L1 的输入会随着 L2 的输出长大，最终触发"输入变大、输出变小"。
    const char* loop = R"(
label entry
  mov i, 0
label L1
  add i, i, 1
label L2
  jnz i, L1
  halt
)";
    const auto program = parseOrDie(loop);
    BrokenAnalysis broken;
    const auto solution = dataflow::solveWorklist(program, broken);
    CHECK(!solution.monotone);       // 框架必须报出来，而不是默默算完
}

void testErrors() {
    dataflow::Program program;
    std::vector<std::string> errors;
    CHECK(!dataflow::parse("label\n", program, errors));
    CHECK(!errors.empty());
    errors.clear();
    CHECK(!dataflow::parse("add t1, t2\n", program, errors));
    errors.clear();
    CHECK(!dataflow::parse("frob t1\n", program, errors));
    errors.clear();
    CHECK(!dataflow::parse("\n", program, errors));
}

} // namespace

int main() {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    testParse();
    testLatticeLaws();
    testLive();
    testReach();
    testAvail();
    testConst();
    testSolversAgree();
    testLoopResults();
    testMonotonicityCheck();
    testErrors();
    if (failures == 0) {
        std::cout << "dataflow-tests：全部通过\n";
        return 0;
    }
    std::cout << "dataflow-tests：" << failures << " 个断言失败\n";
    return 1;
}
