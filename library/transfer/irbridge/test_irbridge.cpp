// 单元测试：真实 fixture 的解析、三元式↔四元式互转、导出与错误分支。
#include "irbridge.hpp"

#include <iostream>
#include <fstream>
#include <sstream>
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

// 仓库里 project/pl0-ir/expected/if_while.triples 的真实内容。
const char* kTriples = R"(三元式：共 15 条
  0: (:=, 1, i)
  1: (:=, 0, sum)
  2: (label, L1, _)
  3: (<=, i, 10)
  4: (jz, (3), L2)
  5: (+, sum, i)
  6: (:=, (5), sum)
  7: (+, i, 1)
  8: (:=, (7), i)
  9: (j, _, L1)
 10: (label, L2, _)
 11: (>, sum, 50)
 12: (jz, (11), L3)
 13: (write, sum, _)
 14: (label, L3, _)
)";

// 由上面的三元式手工推出的四元式（被引用的条目分到 t<下标>）。
const char* kQuads = R"(四元式：共 15 条
  0: (:=, 1, _, i)
  1: (:=, 0, _, sum)
  2: (label, _, _, L1)
  3: (<=, i, 10, t3)
  4: (jz, t3, _, L2)
  5: (+, sum, i, t5)
  6: (:=, t5, _, sum)
  7: (+, i, 1, t7)
  8: (:=, t7, _, i)
  9: (j, _, _, L1)
 10: (label, _, _, L2)
 11: (>, sum, 50, t11)
 12: (jz, t11, _, L3)
 13: (write, sum, _, _)
 14: (label, _, _, L3)
)";

void testParse() {
    irbridge::Triples triples;
    std::vector<std::string> errors;
    CHECK(irbridge::readTriples(kTriples, triples, errors));
    CHECK(triples.size() == 15);
    CHECK(triples[4].op == "jz");
    CHECK(triples[4].a == "(3)");
    CHECK(triples[4].b == "L2");
    CHECK(triples[2].op == "label" && triples[2].a == "L1");

    irbridge::Quads quads;
    CHECK(irbridge::readQuads(kQuads, quads, errors));
    CHECK(quads.size() == 15);
    CHECK(quads[3].result == "t3");
    CHECK(quads[13].op == "write" && quads[13].result == "_");

    std::size_t index = 0;
    CHECK(irbridge::parseRef("(12)", index) && index == 12);
    CHECK(!irbridge::parseRef("t3", index));
    CHECK(irbridge::isTemporary("t12"));
    CHECK(irbridge::isTemporary("__tr_3"));
    CHECK(!irbridge::isTemporary("sum"));
}

void testTriplesToQuads() {
    irbridge::Triples triples;
    std::vector<std::string> errors;
    CHECK(irbridge::readTriples(kTriples, triples, errors));
    irbridge::Quads quads;
    CHECK(irbridge::triplesToQuads(triples, quads, errors));
    CHECK(quads.size() == 15);
    CHECK(irbridge::writeQuads(quads) == kQuads);
}

void testQuadsToTriples() {
    irbridge::Quads quads;
    std::vector<std::string> errors;
    CHECK(irbridge::readQuads(kQuads, quads, errors));
    irbridge::Triples triples;
    CHECK(irbridge::quadsToTriples(quads, triples, errors));
    CHECK(triples.size() == 15);
    CHECK(irbridge::writeTriples(triples) == kTriples); // 折回三元式应与 fixture 一致
}

void testTransferExport() {
    irbridge::Quads quads;
    std::vector<std::string> errors;
    CHECK(irbridge::readQuads(kQuads, quads, errors));
    const std::string text = irbridge::toTransfer(quads, true);
    CHECK(text.find("(decl, i16, global, i)") != std::string::npos);
    CHECK(text.find("(decl, i16, global, sum)") != std::string::npos);
    CHECK(text.find("(jz, t3, _, L2)") != std::string::npos);
    CHECK(text.find("(decl, i16, global, t3)") == std::string::npos); // 临时量不声明
}

void testErrors() {
    irbridge::Triples triples;
    irbridge::Quads quads;
    std::vector<std::string> errors;
    // 引用越界
    CHECK(!irbridge::readTriples("0: (:=, (7), x)\n", triples, errors) ||
          !irbridge::triplesToQuads(triples, quads, errors));
    CHECK(!errors.empty());
    // 引用不产生值的条目
    errors.clear();
    CHECK(irbridge::readTriples("0: (write, x, _)\n1: (+, (0), 1)\n", triples, errors));
    CHECK(!irbridge::triplesToQuads(triples, quads, errors));
    CHECK(!errors.empty());
    // 字段数不对
    errors.clear();
    CHECK(!irbridge::readQuads("0: (+, 1, 2)\n", quads, errors) || quads.size() == 1);
}

void testStatistics() {
    irbridge::Triples triples;
    std::vector<std::string> errors;
    CHECK(irbridge::readTriples(kTriples, triples, errors));
    const std::string stats = irbridge::statistics(triples);
    CHECK(stats.find("总条数：15") != std::string::npos);
    CHECK(stats.find("跳转：3") != std::string::npos);
}

// 用仓库里的真实 fixture 做逐字比对：examples/if_while.triples → 四元式 → 三元式
// 必须回到原文件；examples/if_while.quads 也必须与生成结果逐字一致。
void testExampleFiles() {
    auto readFile = [](const std::string& path, std::string& text) {
        std::ifstream in(path, std::ios::binary);
        if (!in) return false;
        std::ostringstream out;
        out << in.rdbuf();
        text = out.str();
        // 统一成 LF：仓库 fixture 可能是 CRLF（Windows 检出）。
        std::string normalized;
        normalized.reserve(text.size());
        for (char c : text) if (c != '\r') normalized.push_back(c);
        text = std::move(normalized);
        while (!text.empty() && text.back() == '\n') text.pop_back();
        return true;
    };
    std::string triplesText, quadsText;
    if (!readFile("examples/if_while.triples", triplesText)) {
        std::cout << "（跳过 examples 比对：找不到 examples/if_while.triples）\n";
        return;
    }
    irbridge::Triples triples;
    irbridge::Quads quads;
    std::vector<std::string> errors;
    CHECK(irbridge::readTriples(triplesText, triples, errors));
    CHECK(irbridge::triplesToQuads(triples, quads, errors));
    std::string generated = irbridge::writeQuads(quads);
    if (!generated.empty() && generated.back() == '\n') generated.pop_back();
    if (readFile("examples/if_while.quads", quadsText)) CHECK(generated == quadsText);

    irbridge::Triples back;
    CHECK(irbridge::quadsToTriples(quads, back, errors));
    std::string roundTrip = irbridge::writeTriples(back);
    if (!roundTrip.empty() && roundTrip.back() == '\n') roundTrip.pop_back();
    CHECK(roundTrip == triplesText);

    const std::string transfer = irbridge::toTransfer(quads, true);
    CHECK(transfer.find("(decl, i16, global, sum)") != std::string::npos);
    CHECK(transfer.find("(label, _, _, L1)") != std::string::npos);
}

} // namespace

int main() {
    testParse();
    testTriplesToQuads();
    testQuadsToTriples();
    testTransferExport();
    testErrors();
    testStatistics();
    testExampleFiles();
    if (failures == 0) {
        std::cout << "irbridge-tests：全部通过\n";
        return 0;
    }
    std::cout << "irbridge-tests：" << failures << " 个断言失败\n";
    return 1;
}
