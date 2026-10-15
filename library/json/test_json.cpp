/**
 * @file test_json.cpp
 * @brief json 库的单元测试：解析 / 序列化 / 迭代器与 STL 算法 / 模型投射。
 *
 * 运行：
 *   json-tests
 */

#include "bind.hpp"

#include <algorithm>
#include <iostream>
#include <numeric>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
//  模型与绑定
//
//  注意：`JSON_MODEL` 展开的是显式特化，只能写在全局作用域。
//  类型本身可以放进命名空间（写全限定名即可），这里为了简短就放在全局。
// ---------------------------------------------------------------------------

/** 一个嵌套模型：点。 */
struct Point
{
    int x = 0;
    int y = 0;
};

/** 形状的类型（枚举按整数投射）。 */
enum class Kind
{
    Line = 0,
    Polygon = 1,
};

/** 一个模型：嵌套模型 + 容器 + optional + map。 */
struct Shape
{
    std::string id;
    Kind kind = Kind::Line;
    Point origin;
    std::vector<Point> points;
    std::optional<double> scale;
    std::map<std::string, std::string> labels;
};

/** 声明绑定：这就是"投射"里唯一要手写的东西。 */
JSON_MODEL(Point,
           json::field("x", &Point::x),
           json::field("y", &Point::y));

JSON_MODEL(Shape,
           json::field("id", &Shape::id),
           json::field("kind", &Shape::kind),
           json::field("origin", &Shape::origin),
           json::field("points", &Shape::points),
           json::field("scale", &Shape::scale),
           json::field("labels", &Shape::labels));

/** 没有绑定的类型：`hasBinding` 必须是 false。 */
struct Unbound
{
    int value = 0;
};

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
void checkText(const std::string &title, const std::string &actual,
               const std::string &expected)
{
    ++checks;
    if (actual == expected) return;
    ++failures;
    std::cerr << "  失败：" << title << "\n    实际：" << actual
              << "\n    期望：" << expected << "\n";
}

// ---------------------------------------------------------------------------
//  解析与序列化
// ---------------------------------------------------------------------------

/** 标量：null / bool / 整数 / 浮点 / 字符串。 */
void testScalars()
{
    const json::ParseResult doc = json::parse(R"({
        "nothing": null, "yes": true, "no": false,
        "count": 42, "big": 9007199254740993, "ratio": 1.5, "text": "hi"
    })");
    CHECK(doc.ok());
    const json::Value &root = *doc.value;
    CHECK(root.isObject());
    CHECK(root.find("nothing")->isNull());
    CHECK(root.find("yes")->asBool() == true);
    CHECK(root.find("no")->asBool() == false);
    CHECK(root.find("count")->isInt());
    CHECK(root.find("count")->asInt() == 42);
    CHECK(root.find("big")->asInt() == 9007199254740993LL); // 64 位整数不丢精度
    CHECK(root.find("ratio")->isDouble());
    CHECK(root.find("ratio")->asDouble() == 1.5);
    CHECK(root.find("text")->asString() == "hi");
    CHECK(root.find("text")->stringOr("?") == "hi");
    CHECK(root.find("count")->stringOr("?") == "?");
    CHECK(root.find("count")->intOr(7) == 42);
    CHECK(root.typeName() == std::string("object"));
}

/** 转义与 Unicode：`\uXXXX`、代理对、控制字符。 */
void testStrings()
{
    const json::ParseResult doc = json::parse("\"a\\n\\t\\\"\\u4e2d\\ud83d\\ude00\"");
    CHECK(doc.ok());
    CHECK(doc.value->asString() == std::string("a\n\t\"\u4e2d\U0001F600"));

    // 序列化时控制字符要转义，中文原样透传。
    const json::Value text(std::string("中\n\t\"x\""));
    checkText("字符串转义", text.dump(), "\"中\\n\\t\\\"x\\\"\"");

    // 往返一致。
    const json::ParseResult again = json::parse(text.dump());
    CHECK(again.ok() && (*again.value == text));
}

/** 数字：整数 / 浮点 / 大数 / 往返。 */
void testNumbers()
{
    CHECK(json::parse("1").value->isInt());
    CHECK(json::parse("-0").value->asInt() == 0);
    CHECK(json::parse("1e3").value->isDouble());
    CHECK(json::parse("1e3").value->asDouble() == 1000.0);
    CHECK(json::parse("0.5").value->asDouble() == 0.5);

    // 浮点往返：1.5 与 0.1（二进制不能精确表示，但必须往返一致）。
    const json::Value ratio(0.1);
    const json::ParseResult back = json::parse(ratio.dump());
    CHECK(back.ok() && (back.value->asDouble() == 0.1));
    checkText("整数值的浮点带 .0", json::Value(2.0).dump(), "2.0");

    // 超过 int64 的整数退化成浮点。
    const json::ParseResult huge = json::parse("123456789012345678901234567890");
    CHECK(huge.ok() && huge.value->isDouble());
}

/** 解析错误：短码与行列信息。 */
void testErrors()
{
    // 1. 空输入 / 半截输入。
    CHECK(json::parse("").diagnostics.front().code == "unexpected-end");
    CHECK(json::parse("{").diagnostics.front().code == "expected-key");
    CHECK(json::parse("{\"a\" 1}").diagnostics.front().code == "expected-colon");
    CHECK(json::parse("{\"a\":1 \"b\":2}").diagnostics.front().code == "expected-comma");
    CHECK(json::parse("[1,2").diagnostics.front().code == "expected-comma");
    CHECK(json::parse("nul").diagnostics.front().code == "unexpected-token");
    CHECK(json::parse("01").diagnostics.front().code == "bad-number");
    CHECK(json::parse("1.").diagnostics.front().code == "bad-number");
    CHECK(json::parse("\"abc").diagnostics.front().code == "unterminated-string");
    CHECK(json::parse("\"\\x\"").diagnostics.front().code == "bad-escape");
    CHECK(json::parse("\"\\u12\"").diagnostics.front().code == "bad-escape");
    CHECK(json::parse("\"\\ud83d\"").diagnostics.front().code == "bad-surrogate");
    CHECK(json::parse("{} {}").diagnostics.front().code == "unexpected-token");
    CHECK(json::parse("[1,]").diagnostics.front().code == "trailing-comma");
    CHECK(json::parse("// c\n{}").diagnostics.front().code == "unexpected-token");

    // 2. 行列信息要对得上。
    const json::ParseResult broken = json::parse("{\n  \"a\": ,\n}");
    CHECK(!broken.ok());
    CHECK(broken.diagnostics.front().line == 2u);
    CHECK(broken.diagnostics.front().column == 8u);

    // 3. 打开选项后注释与尾逗号都能过。
    json::ParseOptions options;
    options.allowComments = true;
    options.allowTrailingCommas = true;
    const json::ParseResult loose = json::parse("// 注释\n{\"a\": [1, 2,], /* 块注释 */}", options);
    CHECK(loose.ok());
    CHECK(loose.value->find("a")->at(1).asInt() == 2);
}

/** 对象：保序、查找、增删改。 */
void testObject()
{
    const json::ParseResult doc = json::parse(R"({"b":1,"a":2,"c":3})");
    CHECK(doc.ok());
    const json::Object &object = *doc.value->asObject();

    // 1. 保序：遍历顺序就是文件里的顺序。
    std::string order;
    for (const json::Member &member : object) order += member.key;
    CHECK(order == std::string("bac"));

    // 2. 查找。
    CHECK(object.contains("a"));
    CHECK(!object.contains("z"));
    CHECK(object.count("a") == 1u);
    CHECK(object.find("a") != object.end());
    CHECK(object.find("z") == object.end());
    CHECK(object.tryGet("c")->asInt() == 3);
    CHECK(object.tryGet("z") == nullptr);

    // 3. 修改。
    json::Value copy = *doc.value;
    json::Object &mutableObject = *copy.asObject();
    mutableObject["a"] = json::Value(9);                 // 覆盖
    CHECK(mutableObject["a"].asInt() == 9);
    mutableObject["new"] = json::Value(std::string("x")); // 新增
    CHECK(mutableObject.size() == 4u);
    CHECK(mutableObject.contains("new"));
    CHECK(mutableObject.erase("new"));
    CHECK(!mutableObject.erase("new"));
    CHECK(mutableObject.size() == 3u);
    mutableObject.insert("z", json::Value(true));
    CHECK(mutableObject.tryGet("z")->asBool() == true);
    CHECK(mutableObject.size() == 4u);
}

/** 值：类型判断、optional 取值、数组操作、路径查询。 */
void testValueApi()
{
    const json::ParseResult doc = json::parse(R"({
        "user": {"name": "糖糖", "tags": ["a", "b", "c"], "scores": [1, 2, 3]}
    })");
    CHECK(doc.ok());
    const json::Value &root = *doc.value;

    // 1. 路径查询。
    CHECK(root.atPath("user.name")->asString() == "糖糖");
    CHECK(root.atPath("user.tags[1]")->asString() == "b");
    CHECK(root.atPath("user.scores[2]")->asInt() == 3);
    CHECK(root.atPath("user.nope") == nullptr);
    CHECK(root.atPath("user.tags[9]") == nullptr);
    CHECK(root.atPath("user.tags[]") == nullptr);
    CHECK(root.atPath("user..name") == nullptr);

    // 2. 可选取值。
    CHECK(root.find("user")->asObject() != nullptr);
    CHECK(root.find("user")->asArray() == nullptr);
    CHECK(root.find("user")->asString() == std::nullopt);
    CHECK(root.find("user")->intOr(-1) == -1);
    CHECK(json::Value(3.0).asInt() == 3);       // 整数浮点也能取到整数
    CHECK(json::Value(3.5).asInt() == std::nullopt);

    // 3. 构造：初始化列表。
    json::Value list = json::Value::array({1, 2, 3});
    CHECK(list.isArray() && list.size() == 3u);
    json::Value record = json::Value::object({{"a", 1}, {"b", "x"}});
    CHECK(record.isObject() && record.find("b")->asString() == "x");

    // 4. 数组修改。
    list.push_back(json::Value(4));
    list.emplace_back(std::string("five"));
    CHECK(list.size() == 5u);
    CHECK(list.at(3).asInt() == 4);
    CHECK(list.at(4).asString() == "five");
    list.clear();
    CHECK(list.empty());

    // 5. 类型不对时的行为。
    bool threw = false;
    try { (void)list.at(0); } catch (const std::out_of_range &) { threw = true; }
    CHECK(threw);
    threw = false;
    try { (void)root.requireArray(); } catch (const std::domain_error &) { threw = true; }
    CHECK(threw);
}

/** 序列化：紧凑与缩进两种形态。 */
void testDump()
{
    const json::ParseResult doc = json::parse(R"({"a":[1,2],"b":{"c":true},"d":"x"})");
    CHECK(doc.ok());
    checkText("紧凑输出", doc.value->dump(), R"({"a":[1,2],"b":{"c":true},"d":"x"})");
    checkText("缩进输出", doc.value->dump(2),
              "{\n"
              "  \"a\": [\n"
              "    1,\n"
              "    2\n"
              "  ],\n"
              "  \"b\": {\n"
              "    \"c\": true\n"
              "  },\n"
              "  \"d\": \"x\"\n"
              "}");
    checkText("空数组与空对象", json::parse("[{},[]]").value->dump(), "[{},[]]");

    // 往返：dump → parse 之后再 dump，逐字节一致。
    const std::string once = doc.value->dump(2);
    const json::ParseResult again = json::parse(once);
    CHECK(again.ok());
    checkText("往返一致", again.value->dump(2), once);
}

/** 比较：数字跨类型、对象不看顺序。 */
void testCompare()
{
    CHECK(json::parse("1").value == json::parse("1.0").value);
    CHECK(*json::parse(R"({"a":1,"b":2})").value ==
          *json::parse(R"({"b":2,"a":1})").value); // 对象顺序不影响相等
    CHECK(*json::parse("[1,2]").value != *json::parse("[2,1]").value);
    CHECK(*json::parse("null").value == *json::parse("null").value);
    CHECK(*json::parse(R"("a")").value != *json::parse(R"("b")").value);
}

// ---------------------------------------------------------------------------
//  迭代器与 STL 算法
// ---------------------------------------------------------------------------

/** `for` 与 `<algorithm>` 直接遍历数组 / 对象。 */
void testIteration()
{
    // 注意：`Value` 的迭代器给出可写引用，所以这里用非 const 的文档。
    json::ParseResult doc = json::parse(R"({"scores":[3,1,2],"names":["a","b"]})");
    CHECK(doc.ok());
    json::Value &root = *doc.value;
    json::Array &scores = *root.find("scores")->asArray();

    // 1. 范围 for：数组。
    int sum = 0;
    for (const json::Value &item : scores) sum += static_cast<int>(item.asInt().value());
    CHECK(sum == 6);

    // 2. STL 算法：数组。
    const auto threes = std::count_if(scores.begin(), scores.end(),
                                     [](const json::Value &item) { return item.asInt() == 3; });
    CHECK(threes == 1);
    const auto found = std::find_if(scores.begin(), scores.end(),
                                    [](const json::Value &item) { return item.asInt() == 1; });
    CHECK(found != scores.end() && found->asInt() == 1);
    CHECK(std::any_of(scores.begin(), scores.end(),
                      [](const json::Value &item) { return item.asInt() > 2; }));
    CHECK(std::accumulate(scores.begin(), scores.end(), 0,
                          [](int total, const json::Value &item) {
                              return total + static_cast<int>(item.asInt().value());
                          }) == 6);
    CHECK(std::none_of(scores.begin(), scores.end(),
                       [](const json::Value &item) { return item.isNull(); }));

    // 3. std::transform：把 JSON 数组变成 C++ 容器。
    std::vector<int> numbers(scores.size());
    std::transform(scores.begin(), scores.end(), numbers.begin(),
                   [](const json::Value &item) { return static_cast<int>(item.asInt().value()); });
    CHECK((numbers == std::vector<int>{3, 1, 2}));

    // 4. 对象：结构化绑定 + 算法。
    const json::ParseResult recordDoc = json::parse(R"({"a":1,"b":2,"c":3})");
    const json::Object &record = *recordDoc.value->asObject();
    int keyLength = 0;
    for (const auto &[key, value] : record)
    {
        CHECK(!key.empty());
        CHECK(value.isInt());
        keyLength += static_cast<int>(key.size());
    }
    CHECK(keyLength == 3);
    const auto twos = std::count_if(record.begin(), record.end(),
                                    [](const json::Member &member) {
                                        return member.value.asInt() == 2;
                                    });
    CHECK(twos == 1);
    std::for_each(record.begin(), record.end(), [](const json::Member &member) {
        CHECK(!member.key.empty());
    });

    // 5. `Value` 自己也能迭代：数组给元素、对象给成员、标量是空区间。
    std::size_t visited = 0;
    for (const auto &entry : root)
    {
        CHECK(entry.key == "scores" || entry.key == "names");
        CHECK(entry.get().isArray());
        ++visited;
    }
    CHECK(visited == 2u);
    for (const auto &entry : *root.find("scores"))
    {
        CHECK(entry.key.empty());
        CHECK(entry.index < scores.size());
        CHECK(entry.get().isInt());
        ++visited;
    }
    CHECK(visited == 5u);
    json::Value scalar(42);
    CHECK(scalar.begin() == scalar.end());
}

/** 编码 / 解码往返。 */
void testBindRoundTrip()
{
    CHECK(json::hasBinding<Point>::value);
    CHECK(json::hasBinding<Shape>::value);
    CHECK(!json::hasBinding<Unbound>::value);
    CHECK(!json::hasBinding<int>::value);

    Shape shape;
    shape.id = "shape-1";
    shape.kind = Kind::Polygon;
    shape.origin = Point{1, 2};
    shape.points = {Point{0, 0}, Point{3, 4}};
    shape.scale = 2.5;
    shape.labels["cn"] = "多边形";

    // 1. 模型 → JSON：字段顺序就是声明顺序。
    const json::Value encoded = json::encode(shape);
    CHECK(encoded.isObject());
    CHECK(encoded.find("id")->asString() == "shape-1");
    CHECK(encoded.find("kind")->asInt() == 1);
    CHECK(encoded.find("points")->isArray() && encoded.find("points")->size() == 2u);
    CHECK(encoded.find("points")->at(1).find("y")->asInt() == 4);
    CHECK(encoded.find("scale")->asDouble() == 2.5);
    CHECK(encoded.find("labels")->find("cn")->asString() == "多边形");
    std::string order;
    for (const json::Member &member : *encoded.asObject()) order += member.key + ",";
    CHECK(order == std::string("id,kind,origin,points,scale,labels,"));

    // 2. JSON → 模型。
    const json::DecodeResult<Shape> decoded = json::decode<Shape>(encoded);
    CHECK(decoded.ok());
    const Shape &back = *decoded.value;
    CHECK(back.id == "shape-1");
    CHECK(back.kind == Kind::Polygon);
    CHECK(back.origin.x == 1 && back.origin.y == 2);
    CHECK(back.points.size() == 2u && back.points[1].x == 3);
    CHECK(back.scale.has_value() && back.scale.value() == 2.5);
    CHECK(back.labels.at("cn") == "多边形");

    // 3. 再编一遍：与第一次逐字节相同（往返稳定）。
    checkText("投射往返稳定", json::encode(back).dump(2), encoded.dump(2));

    // 4. JSON 文本直接解到模型。
    const json::ParseResult doc = json::parse(R"({
        "id": "s2", "kind": "1", "origin": {"x": 5, "y": 6},
        "points": [], "labels": {}
    })");
    CHECK(doc.ok());
    const json::DecodeResult<Shape> fromText = json::decode<Shape>(*doc.value);
    CHECK(fromText.ok());
    if (fromText.ok())
    {
        CHECK(fromText.value->origin.x == 5);
        CHECK(!fromText.value->scale.has_value()); // 缺可选字段 → 空
    }
}

/** 解码诊断：缺字段、类型不对、多余字段，都要带 JSON 路径。 */
void testBindDiagnostics()
{
    {
        const json::DecodeResult<Shape> result =
            json::decode<Shape>(*json::parse(R"({"id":"x"})").value);
        CHECK(!result.ok());
        bool missing = false;
        for (const json::Diagnostic &item : result.diagnostics)
        {
            if ((item.code == "missing-field") && (item.path == "kind")) missing = true;
        }
        CHECK(missing);
    }
    {
        const json::ParseResult doc = json::parse(R"({
            "id": "x", "kind": 0, "origin": {"x": 1, "y": 2},
            "points": [{"x": 0, "y": 0}, {"x": "bad", "y": 1}], "labels": {}
        })");
        const json::DecodeResult<Shape> result = json::decode<Shape>(*doc.value);
        CHECK(!result.ok());
        CHECK(result.diagnostics.front().path == std::string("points[1].x"));
        CHECK(result.diagnostics.front().code == std::string("type-mismatch"));
    }
    {
        const json::ParseResult doc = json::parse(R"({
            "id": "x", "kind": 0, "origin": {}, "points": [], "labels": {}, "extra": 1
        })");
        json::DecodeOptions options;
        options.strictUnknown = true;
        options.strictMissing = false;
        const json::DecodeResult<Shape> result = json::decode<Shape>(*doc.value, options);
        CHECK(!result.ok());
        CHECK(result.diagnostics.front().path == std::string("extra"));
        CHECK(result.diagnostics.front().code == std::string("unknown-field"));

        options.strictUnknown = false;
        const json::DecodeResult<Shape> lenient = json::decode<Shape>(*doc.value, options);
        CHECK(lenient.ok());
    }
    {
        // 整体类型不对：报在根路径。
        const json::DecodeResult<Shape> result = json::decode<Shape>(*json::parse("[]").value);
        CHECK(!result.ok());
        CHECK(result.diagnostics.front().path == std::string("$"));
        CHECK(result.diagnostics.front().message.find("期望对象") != std::string::npos);
    }
}

/** 投射之后的模型照样能喂 STL 算法。 */
void testBindWithAlgorithms()
{
    const json::ParseResult doc = json::parse(R"([
        {"id":"a","kind":0,"origin":{"x":0,"y":0},"points":[{"x":1,"y":1}],"labels":{}},
        {"id":"b","kind":1,"origin":{"x":0,"y":0},"points":[{"x":2,"y":2},{"x":3,"y":3}],"labels":{}}
    ])");
    CHECK(doc.ok());
    const json::DecodeResult<std::vector<Shape>> shapes = json::decode<std::vector<Shape>>(*doc.value);
    CHECK(shapes.ok());
    if (shapes.ok())
    {
        CHECK(shapes.value->size() == 2u);

        // 直接用 STL 算法处理解出来的模型。
        const auto polygons = std::count_if(shapes.value->begin(), shapes.value->end(),
                                            [](const Shape &shape) {
                                                return shape.kind == Kind::Polygon;
                                            });
        CHECK(polygons == 1);
        const int total = std::accumulate(shapes.value->begin(), shapes.value->end(), 0,
                                          [](int sum, const Shape &shape) {
                                              return sum + static_cast<int>(shape.points.size());
                                          });
        CHECK(total == 3);

        // 容器也能被投射成 JSON 数组。
        const json::Value encoded = json::encode(*shapes.value);
        CHECK(encoded.isArray() && encoded.size() == 2u);
    }
}

} // namespace

/**
 * @brief 程序入口。
 * @return 0 全部通过，1 有失败。
 */
int main()
{
    CHECK(!json::Value(9223372036854775808.0).asInt().has_value());
    CHECK(json::Value(-9223372036854775808.0).asInt().has_value());
    testScalars();
    testStrings();
    testNumbers();
    testErrors();
    testObject();
    testValueApi();
    testDump();
    testCompare();
    testIteration();
    testBindRoundTrip();
    testBindDiagnostics();
    testBindWithAlgorithms();

    if (failures == 0)
    {
        std::cout << "json-tests：全部通过（" << checks << " 项断言）\n";
        return 0;
    }
    std::cout << "json-tests：" << failures << " / " << checks << " 项断言失败\n";
    return 1;
}
