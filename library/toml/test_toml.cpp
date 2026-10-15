/**
 * @file test_toml.cpp
 * @brief toml 库的单元测试：解析 / 序列化 / 迭代器与 STL 算法 / 模型投射。
 *
 * 运行：
 *   toml-tests
 */

#include "bind.hpp"

#include <algorithm>
#include <iostream>
#include <limits>
#include <map>
#include <numeric>
#include <optional>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
//  模型与绑定
//
//  `TOML_MODEL` 展开的是显式特化，只能写在全局作用域。
// ---------------------------------------------------------------------------

/** 一个嵌套模型：点。 */
struct Point
{
    int x = 0;
    int y = 0;
};

/** 顶层模型：嵌套模型 + 容器 + optional + map + 日期时间。 */
struct Site
{
    std::string name;
    std::vector<Point> points;
    std::optional<double> scale;
    std::map<std::string, std::string> labels;
    toml::DateTime born;
    bool active = false;
};

TOML_MODEL(Point,
           toml::field("x", &Point::x),
           toml::field("y", &Point::y));

TOML_MODEL(Site,
           toml::field("name", &Site::name),
           toml::field("points", &Site::points),
           toml::field("scale", &Site::scale),
           toml::field("labels", &Site::labels),
           toml::field("born", &Site::born),
           toml::field("active", &Site::active));

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
 * @brief 断言两个字符串相等。
 * @param title 用例名。
 * @param actual 实际。
 * @param expected 期望。
 */
void checkText(const std::string &title, const std::string &actual, const std::string &expected)
{
    ++checks;
    if (actual == expected) return;
    ++failures;
    std::cerr << "  失败：" << title << "\n    实际：" << actual << "\n    期望：" << expected << "\n";
}

/**
 * @brief 解析一段文本并断言成功，返回顶层值。
 * @param text 文本。
 * @return 顶层值。
 */
toml::Value parseOk(std::string_view text)
{
    const toml::ParseResult doc = toml::parse(text);
    ++checks;
    if (!doc.ok())
    {
        ++failures;
        std::cerr << "  失败：应当解析成功，实际报错 " << doc.diagnostics.front().toString() << "\n";
        return toml::Value{};
    }
    return *doc.value;
}

/**
 * @brief 断言一段文本解析失败，并检查短码。
 * @param title 用例名。
 * @param text 文本。
 * @param code 期望的短码（空表示不检查）。
 */
void checkError(const std::string &title, std::string_view text, const std::string &code)
{
    ++checks;
    const toml::ParseResult doc = toml::parse(text);
    if (doc.ok())
    {
        ++failures;
        std::cerr << "  失败：" << title << " 应当解析失败，实际通过\n";
        return;
    }
    if (!code.empty() && (doc.diagnostics.front().code != code))
    {
        ++failures;
        std::cerr << "  失败：" << title << " 短码应为 " << code << "，实际 "
                  << doc.diagnostics.front().code << "\n";
    }
}

// ---------------------------------------------------------------------------
//  标量 / 数字 / 字符串 / 日期时间
// ---------------------------------------------------------------------------

/** 标量：bool 与三种字符串形式。 */
void testScalars()
{
    const toml::Value root = parseOk(
        "yes = true\n"
        "no = false\n"
        "basic = \"a\\tb\\n\"\n"
        "literal = 'C:\\dir\\x'\n"
        "multi = \"\"\"\nline1\nline2\"\"\"\n"
        "multi_lit = '''\nraw \\n stays'''\n");

    CHECK(root.find("yes")->asBool() == true);
    CHECK(root.find("no")->asBool() == false);
    CHECK(root.find("basic")->asString() == std::string("a\tb\n"));
    CHECK(root.find("literal")->asString() == std::string("C:\\dir\\x"));
    CHECK(root.find("multi")->asString() == std::string("line1\nline2"));
    CHECK(root.find("multi_lit")->asString() == std::string("raw \\n stays"));
    CHECK(root.typeName() == std::string("table"));
}

/** 转义与 Unicode：`\uXXXX` / `\UXXXXXXXX`。 */
void testStrings()
{
    const toml::Value root = parseOk("a = \"\\u4e2d\\U0001F600\"\n");
    CHECK(root.find("a")->asString() == std::string("\u4e2d\U0001F600"));

    const toml::Value text(std::string("中\n\t\"x\""));
    checkText("字符串转义", text.inlineDump(), "\"中\\n\\t\\\"x\\\"\"");

    // 行尾反斜杠折行：连同后续空白一起裁掉。
    const toml::Value folded = parseOk("a = \"\"\"the quick \\\n    brown fox\"\"\"\n");
    CHECK(folded.find("a")->asString() == std::string("the quick brown fox"));
}

/** 整数：十进制 / 十六 / 八 / 二进制 / 下划线 / 负数。 */
void testIntegers()
{
    const toml::Value root = parseOk(
        "dec = 1_000_000\n"
        "hex = 0xDEAD_beef\n"
        "oct = 0o755\n"
        "bin = 0b1010_1010\n"
        "neg = -42\n"
        "plus = +7\n"
        "zero = 0\n");

    CHECK(root.find("dec")->asInt() == 1000000);
    CHECK(root.find("hex")->asInt() == 0xDEADBEEFLL);
    CHECK(root.find("oct")->asInt() == 493);
    CHECK(root.find("bin")->asInt() == 170);
    CHECK(root.find("neg")->asInt() == -42);
    CHECK(root.find("plus")->asInt() == 7);
    CHECK(root.find("zero")->asInt() == 0);
    CHECK(root.find("dec")->isInt());
    CHECK(!root.find("dec")->isDouble());
}

/** 浮点：小数 / 指数 / inf / nan。 */
void testFloats()
{
    const toml::Value root = parseOk(
        "a = 3.1415\n"
        "b = 5e2\n"
        "c = 6.02e23\n"
        "d = -0.0\n"
        "e = inf\n"
        "f = -inf\n"
        "g = nan\n");

    CHECK(root.find("a")->asDouble() == 3.1415);
    CHECK(root.find("b")->asDouble() == 500.0);
    CHECK(root.find("c")->asDouble() == 6.02e23);
    CHECK(root.find("d")->isDouble());
    CHECK(root.find("e")->asDouble() == std::numeric_limits<double>::infinity());
    CHECK(root.find("f")->asDouble() == -std::numeric_limits<double>::infinity());
    CHECK(root.find("g")->asDouble().has_value());
}

/** 日期时间：四种形态与偏移。 */
void testDateTime()
{
    const toml::Value root = parseOk(
        "utc = 1979-05-27T07:32:00Z\n"
        "offset = 1979-05-27T07:32:00+08:00\n"
        "local_dt = 1979-05-27T07:32:00.5\n"
        "date = 1979-05-27\n"
        "time = 07:32:00.999\n"
        "spaced = 1979-05-27 07:32:00\n");

    CHECK(root.find("utc")->asDateTime()->kind == toml::DateTimeKind::OffsetDateTime);
    CHECK(root.find("utc")->asDateTime()->offsetMinutes == 0);
    CHECK(root.find("utc")->asDateTime()->toString() == std::string("1979-05-27T07:32:00Z"));
    CHECK(root.find("offset")->asDateTime()->offsetMinutes == 480);
    CHECK(root.find("offset")->asDateTime()->toString() == std::string("1979-05-27T07:32:00+08:00"));
    CHECK(root.find("local_dt")->asDateTime()->kind == toml::DateTimeKind::LocalDateTime);
    CHECK(root.find("local_dt")->asDateTime()->nanosecond == 500000000u);
    CHECK(root.find("date")->asDateTime()->kind == toml::DateTimeKind::LocalDate);
    CHECK(root.find("date")->asDateTime()->toString() == std::string("1979-05-27"));
    CHECK(root.find("time")->asDateTime()->kind == toml::DateTimeKind::LocalTime);
    CHECK(root.find("time")->asDateTime()->toString() == std::string("07:32:00.999"));
    CHECK(root.find("spaced")->asDateTime()->toString() == std::string("1979-05-27T07:32:00"));
    CHECK(root.find("spaced")->typeName() == std::string("datetime"));
}

// ---------------------------------------------------------------------------
//  结构：表 / 点分键 / 表数组 / 内联表 / 数组
// ---------------------------------------------------------------------------

/** 表、点分键、表数组与内联表。 */
void testStructure()
{
    const toml::Value root = parseOk(
        "owner.name = \"甲\"\n"
        "owner.age = 30\n"
        "point = { x = 1, y = 2 }\n"
        "\n"
        "[server]\n"
        "host = \"localhost\"\n"
        "port = 8080\n"
        "\n"
        "[server.tls]\n"
        "cert = \"a.pem\"\n"
        "\n"
        "[[users]]\n"
        "name = \"乙\"\n"
        "[[users]]\n"
        "name = \"丙\"\n"
        "[[users.pets]]\n"
        "kind = \"猫\"\n");

    CHECK(root.find("server")->isTable());
    CHECK(root.atPath("server.port")->asInt() == 8080);
    CHECK(root.atPath("server.tls.cert")->asString() == std::string("a.pem"));
    CHECK(root.atPath("owner.name")->asString() == std::string("甲"));
    CHECK(root.atPath("point.x")->asInt() == 1);
    CHECK(root.atPath("users")->isArray());
    CHECK(root.atPath("users")->size() == 2u);
    CHECK(root.atPath("users[0].name")->asString() == std::string("乙"));
    CHECK(root.atPath("users[1].pets[0].kind")->asString() == std::string("猫"));
}

/** 数组：跨行、注释、尾逗号、混合类型。 */
void testArrays()
{
    const toml::Value root = parseOk(
        "nums = [\n  1, # 第一个\n  2,\n  3,\n]\n"
        "mixed = [1, \"two\", true]\n"
        "nested = [[1, 2], [3]]\n"
        "empty = []\n");

    CHECK(root.find("nums")->asArray()->size() == 3u);
    CHECK(root.find("mixed")->asArray()->size() == 3u);
    CHECK(root.find("mixed")->asArray()->at(1).asString() == std::string("two"));
    CHECK(root.atPath("nested[1][0]")->asInt() == 3);
    CHECK(root.find("empty")->asArray()->empty());
}

// ---------------------------------------------------------------------------
//  错误
// ---------------------------------------------------------------------------

/** 各类语法 / 语义错误。 */
void testErrors()
{
    checkError("重复键", "a = 1\na = 2\n", "duplicate-key");
    checkError("重复表", "[a]\n[a]\n", "duplicate-table");
    checkError("键与表冲突", "a = 1\n[a]\n", "key-conflict");
    checkError("内联表被追加", "a = {x = 1}\n[a.y]\nz = 1\n", "inline-table");
    checkError("点分键表被重开", "a.b = 1\n[a]\n", "duplicate-table");
    checkError("表数组被普通表覆盖", "[[a]]\n[a]\n", "key-conflict");
    checkError("前导零", "a = 01\n", "bad-number");
    checkError("十六进制带符号", "a = -0x1\n", "bad-number");
    checkError("下划线位置不对", "a = 1__2\n", "bad-number");
    checkError("坏转义", "a = \"\\q\"\n", "bad-escape");
    checkError("未闭合字符串", "a = \"abc\n", "unterminated-string");
    checkError("单行字符串换行", "a = \"abc\ndef\"\n", "unterminated-string");
    checkError("坏日期", "a = 1979-13-45\n", "bad-datetime");
    checkError("内联表尾逗号", "a = {x = 1,}\n", "trailing-comma");
    checkError("缺等号", "a 1\n", "expected-equals");
    checkError("多余内容", "a = 1 b = 2\n", "expected-newline");
    checkError("数组未闭合", "a = [1, 2\n", "unterminated-array");
    checkError("表头语法", "[a\n", "expected-bracket");
    checkError("空键", "= 1\n", "expected-key");
    checkError("未知字面量", "a = truthy\n", "unexpected-token");
    checkError("整数溢出", "a = 99999999999999999999\n", "bad-number");

    // 行列信息：第 3 行第 3 列。
    const toml::ParseResult doc = toml::parse("a = 1\nb = 2\n  = 3\n");
    CHECK(!doc.ok());
    CHECK(doc.diagnostics.front().line == 3u);
    CHECK(doc.diagnostics.front().column == 3u);

    // allowDuplicateKeys：后写覆盖。
    toml::ParseOptions options;
    options.allowDuplicateKeys = true;
    const toml::ParseResult relaxed = toml::parse("a = 1\na = 2\n", options);
    CHECK(relaxed.ok());
    CHECK(relaxed.value->find("a")->asInt() == 2);
}

// ---------------------------------------------------------------------------
//  序列化
// ---------------------------------------------------------------------------

/** dump 的往返稳定与键 / 字符串写法。 */
void testDump()
{
    const std::string source =
        "title = \"x\"\n"
        "list = [1, 2, 3]\n"
        "[a]\n"
        "b = \"c\"\n"
        "[a.d]\n"
        "e = true\n"
        "[[items]]\n"
        "k = 1\n"
        "[[items]]\n"
        "k = 2\n";

    const toml::Value first = parseOk(source);
    const std::string dumped = first.dump();
    const toml::ParseResult second = toml::parse(dumped);
    CHECK(second.ok());
    CHECK(*second.value == first);

    // 标量要排在子表之前。
    CHECK(dumped.find("title = \"x\"") < dumped.find("[a]"));

    // 带特殊字符的键要加引号。
    const toml::Value quoted = parseOk("\"a b\" = 1\n");
    CHECK(quoted.dump() == std::string("\"a b\" = 1\n"));

    // 内联表与数组的写法。
    checkText("内联表", parseOk("p = { x = 1, y = 2 }\n").find("p")->inlineDump(),
              "{ x = 1, y = 2 }");
    checkText("空内联表", parseOk("p = {}\n").find("p")->inlineDump(), "{}");
    checkText("数组", parseOk("a = [1, 2, 3]\n").find("a")->inlineDump(), "[1, 2, 3]");
}

/** 相等语义。 */
void testCompare()
{
    CHECK(parseOk("a = 1\n") == parseOk("a = 1.0\n"));           // 数字按数值比
    CHECK(!(parseOk("a = 1\n") == parseOk("a = 2\n")));
    CHECK(parseOk("a = { x = 1, y = 2 }\n") == parseOk("a = { y = 2, x = 1 }\n")); // 表按集合比
    CHECK(!(parseOk("a = [1, 2]\n") == parseOk("a = [2, 1]\n")));  // 数组有序
}

// ---------------------------------------------------------------------------
//  Value API / 迭代
// ---------------------------------------------------------------------------

/** Value 的构造、取值与修改。 */
void testValueApi()
{
    toml::Value table = toml::Value::table({{"a", 1}, {"b", "x"}});
    CHECK(table.isTable());
    CHECK(table.size() == 2u);
    table["c"] = true;
    CHECK(table.find("c")->asBool() == true);
    CHECK(table.find("nope") == nullptr);

    toml::Value items = toml::Value::array({1, 2, 3});
    items.push_back(4);
    CHECK(items.size() == 4u);
    CHECK(items.at(3).asInt() == 4);
    CHECK(items.atPath("[2]")->asInt() == 3);

    bool threw = false;
    try { (void)table.at(0u); } catch (const std::domain_error &) { threw = true; }
    CHECK(threw);
}

/** 迭代器与 STL 算法。 */
void testIteration()
{
    const toml::Value root = parseOk("scores = [3, 1, 2]\n[user]\nname = \"x\"\nage = 9\n");

    toml::Value copy = root;
    toml::Array &scores = *copy.find("scores")->asArray();
    const auto threes = std::count_if(scores.begin(), scores.end(),
                                      [](const toml::Value &v) { return v.asInt() == 3; });
    CHECK(threes == 1);

    const int total = std::accumulate(scores.begin(), scores.end(), 0,
                                      [](int sum, const toml::Value &v) {
                                          return sum + static_cast<int>(v.asInt().value_or(0));
                                      });
    CHECK(total == 6);

    // 结构化绑定遍历表。
    int members = 0;
    for (const auto &[key, value] : *copy.find("user")->asTable())
    {
        (void)value;
        CHECK(key == "name" || key == "age");
        ++members;
    }
    CHECK(members == 2);

    // Value 自身迭代：表走成员、数组走元素。
    int visited = 0;
    for (const auto &entry : *copy.find("user"))
    {
        CHECK(entry.value != nullptr);
        ++visited;
    }
    CHECK(visited == 2);
}

// ---------------------------------------------------------------------------
//  模型投射
// ---------------------------------------------------------------------------

/** 解码 / 编码往返。 */
void testBindRoundTrip()
{
    const toml::Value root = parseOk(
        "name = \"site\"\n"
        "active = true\n"
        "scale = 2.5\n"
        "born = 2020-01-02T03:04:05Z\n"
        "[[points]]\n"
        "x = 1\n"
        "y = 2\n"
        "[[points]]\n"
        "x = 3\n"
        "y = 4\n"
        "[labels]\n"
        "cn = \"甲\"\n");

    const toml::DecodeResult<Site> site = toml::decode<Site>(root);
    CHECK(site.ok());
    CHECK(site.value->name == std::string("site"));
    CHECK(site.value->active == true);
    CHECK(site.value->scale == 2.5);
    CHECK(site.value->points.size() == 2u);
    CHECK(site.value->points[1].y == 4);
    CHECK(site.value->labels.at("cn") == std::string("甲"));
    CHECK(site.value->born.toString() == std::string("2020-01-02T03:04:05Z"));

    // 编码回 TOML，再解析，值应一致。
    const toml::Value encoded = toml::encode(*site.value);
    const toml::ParseResult again = toml::parse(encoded.dump());
    CHECK(again.ok());
    const toml::DecodeResult<Site> back = toml::decode<Site>(*again.value);
    CHECK(back.ok());
    CHECK(back.value->points.size() == 2u);
    CHECK(back.value->born == site.value->born);
}

/** 解码诊断：缺字段 / 类型不对 / 多余字段，路径要准。 */
void testBindDiagnostics()
{
    // 缺字段（strictMissing 默认开）。
    const toml::Value missing = parseOk("name = \"x\"\nborn = 1979-05-27\n");
    const toml::DecodeResult<Site> bad1 = toml::decode<Site>(missing);
    CHECK(!bad1.ok());
    CHECK(bad1.diagnostics.front().code == std::string("missing-field"));

    // 类型不对：points 应是数组。
    const toml::Value wrong = parseOk(
        "name = \"x\"\nborn = 1979-05-27\npoints = 5\n");
    const toml::DecodeResult<Site> bad2 = toml::decode<Site>(wrong);
    CHECK(!bad2.ok());
    CHECK(bad2.diagnostics.front().path == std::string("points"));

    // 多余字段：严格模式下要报 unknown-field。
    toml::DecodeOptions options;
    options.strictUnknown = true;
    const toml::Value extra = parseOk(
        "name = \"x\"\nborn = 1979-05-27\npoints = []\nactive = false\nlabels = {}\nextra = 1\n");
    const toml::DecodeResult<Site> bad3 = toml::decode<Site>(extra, options);
    CHECK(!bad3.ok());
    bool sawUnknown = false;
    for (const toml::Diagnostic &item : bad3.diagnostics)
    {
        if (item.code == std::string("unknown-field")) sawUnknown = true;
    }
    CHECK(sawUnknown);

    // 可选字段缺席不算错。
    const toml::Value noScale = parseOk(
        "name = \"x\"\nborn = 1979-05-27\npoints = []\nactive = false\nlabels = {}\n");
    const toml::DecodeResult<Site> ok = toml::decode<Site>(noScale);
    CHECK(ok.ok());
    CHECK(!ok.value->scale.has_value());
}

} // namespace

/**
 * @brief 程序入口。
 * @return 0 全部通过，1 有失败。
 */
int main()
{
    testScalars();
    testStrings();
    testIntegers();
    testFloats();
    testDateTime();
    testStructure();
    testArrays();
    testErrors();
    testDump();
    testCompare();
    testValueApi();
    testIteration();
    testBindRoundTrip();
    testBindDiagnostics();

    if (failures == 0)
    {
        std::cout << "toml-tests：全部通过（" << checks << " 项断言）\n";
        return 0;
    }
    std::cout << "toml-tests：" << failures << " / " << checks << " 项断言失败\n";
    return 1;
}
