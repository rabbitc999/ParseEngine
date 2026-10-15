/**
 * @file test_simple_ast.cpp
 * @brief simple_ast 单元测试：类型、AST 构造、四元式 / `.data` 生成、运算符重载与引用限制。
 */

#include "simple_ast.hpp"

#include <iostream>
#include <string>

namespace
{
int checks = 0;
int failures = 0;
#define CHECK(condition)                                                          \
    do                                                                           \
    {                                                                            \
        ++checks;                                                                \
        if (!(condition))                                                        \
        {                                                                        \
            ++failures;                                                          \
            std::cerr << "  失败：" #condition "（第 " << __LINE__ << " 行）\n";  \
        }                                                                        \
    } while (false)

/** 类型描述里要能看到 meta / 方法 / 运算符。 */
void testTypes()
{
    using namespace simple_ast;
    const TypePtr array = Type::array(Type::u16(), "count=4;align=2");
    CHECK(array->quadType() == std::string("i64"));
    CHECK(array->describe().find("count=4") != std::string::npos);
    CHECK(Type::ref(Type::i32())->describe().find(">?") != std::string::npos); // 可空
    CHECK(Type::structOf("S", {{"x", Type::i32()}})->describe().find("struct S") != std::string::npos);
    CHECK(Type::enumOf("E", {{"Some", Type::i32()}})->describe().find("Some(i32)") != std::string::npos);
    CHECK(Type::generic("T")->describe().find("generic<T>") != std::string::npos);
}

/** 生成四元式与数据文件：数组 meta / 字符串 / 结构体 / 带载荷枚举 / 方法 / 运算符重载。 */
void testGenerate()
{
    using namespace simple_ast;
    Program program;
    program.types.push_back(Type::structOf("Vec", {{"x", Type::f64()}}));
    program.addOperator("Vec", "+", "add");
    program.addConstant("K", Type::i32(), "3");
    program.addVariable("arr", Type::array(Type::u16(), "count=2;align=2"));
    program.addVariable("p", Type::ref(Type::i32()));

    Function add;
    add.name = "add";
    add.body = Node::block({Node::ret(Node::var("self"))});
    program.addMethod("Vec", add);

    Function entry;
    entry.name = "main";
    entry.result = Type::i32();
    entry.body = Node::block({Node::assign("arr", Node::arrayNew(Type::array(Type::u16(), "count=2"),
                                                       {Node::i(1), Node::i(2)})),
                     Node::assign("p", Node::null()),
                     Node::exprStmt(Node::call("f", {Node::str("hi")})),
                     Node::exprStmt(Node::enumNew("Maybe", "Some", {Node::i(9)})),
                     Node::exprStmt(Node::binary("+", Node::structNew("Vec", {Node::f(1.0)}),
                                                 Node::structNew("Vec", {Node::f(2.0)}))),
                     Node::exprStmt(Node::methodCall(Node::structNew("Vec", {Node::f(1.0)}), "add",
                                                     {Node::structNew("Vec", {Node::f(2.0)})})),
                     Node::exprStmt(Node::lambda(Node::block({}))),
                     Node::ret(Node::i(0))});
    program.functions.push_back(std::move(entry));

    const CodegenResult result = generate(program, "t");
    CHECK(result.ok());
    CHECK(result.quads.find("(label, _, _, Vec.add)") != std::string::npos); // 方法标签
    CHECK(result.quads.find("(call, Vec.add, _, ") != std::string::npos);    // 运算符重载脱糖
    CHECK(result.quads.find("(call, f, _, ") != std::string::npos);
    CHECK(result.quads.find("(:=, 0, _, p)") != std::string::npos);          // 引用置空
    CHECK(result.quads.find("(decl, i64, local, self)") != std::string::npos);
    CHECK(result.data.find("#   meta: count=2") != std::string::npos);       // 数组 meta
    CHECK(result.data.find("str_") != std::string::npos);                    // 字符串进数据区
    CHECK(result.data.find("Some(") != std::string::npos);                   // 带载荷枚举
    CHECK(result.data.find("lambda_") != std::string::npos);                 // lambda 闭包句柄
}

/** 引用不允许指针运算；声明引用后做加法要报诊断。 */
void testReferenceRules()
{
    using namespace simple_ast;
    Program program;
    program.addVariable("p", Type::ref(Type::i32()));
    program.addVariable("q", Type::ref(Type::i32()));
    Function entry;
    entry.name = "main";
    entry.result = Type::i32();
    entry.body = Node::block({Node::exprStmt(Node::binary("+", Node::var("p"), Node::var("q"))),
                              Node::ret(Node::i(0))});
    program.functions.push_back(std::move(entry));
    const CodegenResult result = generate(program, "t");
    CHECK(!result.ok());
    CHECK(result.diagnostics.front().find("不做指针运算") != std::string::npos);
}

/** `&&` / `||` 的右侧必须出现在条件跳转之后，不能被提前求值。 */
void testLogicalShortCircuit()
{
    using namespace simple_ast;
    Program program;
    Function entry;
    entry.name = "main";
    entry.result = Type::i32();
    entry.body = Node::block({
        Node::exprStmt(Node::binary("&&", Node::i(0), Node::call("right_and", {}))),
        Node::exprStmt(Node::binary("||", Node::i(1), Node::call("right_or", {}))),
        Node::ret(Node::i(0)),
    });
    program.functions.push_back(std::move(entry));

    const CodegenResult result = generate(program, "short_circuit");
    CHECK(result.ok());
    const std::size_t andJump = result.quads.find("(jz, 0, _, and_done_");
    const std::size_t andCall = result.quads.find("(call, right_and, _, ");
    const std::size_t orJump = result.quads.find("(jnz, 1, _, or_done_");
    const std::size_t orCall = result.quads.find("(call, right_or, _, ");
    CHECK(andJump != std::string::npos && andCall != std::string::npos && andJump < andCall);
    CHECK(orJump != std::string::npos && orCall != std::string::npos && orJump < orCall);
    CHECK(result.quads.find("(&:bool") == std::string::npos);
    CHECK(result.quads.find("(|:bool") == std::string::npos);
}

/** 复合类型专项：嵌套结构体 / 数组字段带 meta / 可空引用字段 / 方法带参数 /
 *  运算符重载 / 泛型字段 / 带载荷枚举字段 / 成员链与下标链。 */
void testComplexTypes()
{
    using namespace simple_ast;
    Program program;

    const auto point = Type::structOf("Point", {{"x", Type::i32()}, {"y", Type::i32()}});
    const auto tag = Type::enumOf("Tag", {{"Empty", nullptr}, {"Named", Type::stringType()}});
    const auto pair = Type::structOf("Pair",
                                     {{"first", Type::generic("T")}, {"second", Type::generic("T")}});
    const auto points = Type::array(point, "count=3;align=8;label=shape_points");
    const auto shape = Type::structOf("Shape",
                                      {{"id", Type::stringType()},
                                       {"origin", point},
                                       {"points", points},
                                       {"parent", Type::ref(point)},   // 可空引用
                                       {"tag", tag},
                                       {"pair", pair}});
    program.types = {point, tag, pair, shape};

    // 方法：带两个参数，改自己的字段并返回 self；再把它挂成 `+`。
    Function move;
    move.name = "move";
    move.parameters = {{"dx", Type::i32()}, {"dy", Type::i32()}};
    move.result = Type::i64();
    move.body = Node::block({
        Node::assign("x", Node::binary("+", Node::member(Node::var("self"), "x"), Node::var("dx"))),
        Node::ret(Node::var("self")),
    });
    program.addMethod("Point", move);
    program.addOperator("Point", "+", "move");

    Function entry;
    entry.name = "main";
    entry.result = Type::i32();
    entry.body = Node::block({
        // 嵌套构造：Shape 里塞 Point / 数组 / 空引用 / 带载荷枚举 / 泛型结构体。
        Node::exprStmt(Node::structNew("Shape",
                                       {Node::str("s0"),
                                        Node::structNew("Point", {Node::i(1), Node::i(2)}),
                                        Node::arrayNew(points, {Node::structNew("Point", {Node::i(3), Node::i(4)}),
                                                                Node::structNew("Point", {Node::i(5), Node::i(6)})}),
                                        Node::null(),
                                        Node::enumNew("Tag", "Named", {Node::str("t")}),
                                        Node::structNew("Pair", {Node::i(1), Node::str("a")})})),
        // 成员链 + 下标链：shape.origin.x、shape.points[0].y。
        Node::exprStmt(Node::member(Node::structNew("Shape", {Node::str("s1")}), "origin")),
        Node::exprStmt(Node::member(Node::index(Node::arrayNew(points,
                                                              {Node::structNew("Point", {Node::i(7), Node::i(8)})}),
                                                 Node::i(0)),
                                    "y")),
        // 可空引用判空。
        Node::exprStmt(Node::binary("==", Node::null(), Node::null())),
        // 方法调用 + 运算符重载（都是 Point.move）。
        Node::exprStmt(Node::methodCall(Node::structNew("Point", {Node::i(1), Node::i(1)}), "move",
                                        {Node::i(10), Node::i(20)})),
        Node::exprStmt(Node::binary("+", Node::structNew("Point", {Node::i(1), Node::i(1)}),
                                    Node::structNew("Point", {Node::i(2), Node::i(2)}))),
        Node::ret(Node::i(0)),
    });
    program.functions.push_back(std::move(entry));

    const CodegenResult result = generate(program, "complex");
    CHECK(result.ok());
    // 1. 嵌套类型都进了文件头注释。
    CHECK(result.quads.find("struct Shape") != std::string::npos);
    CHECK(result.quads.find("origin: struct Point") != std::string::npos);
    // 引用描述里包含完整的目标类型描述（`ref<struct Point {…}>?{nullable}`），只验证关键标记。
    CHECK(result.quads.find("ref<struct Point") != std::string::npos);
    CHECK(result.quads.find("nullable") != std::string::npos);
    CHECK(result.quads.find("enum Tag") != std::string::npos);
    CHECK(result.quads.find("generic<T>") != std::string::npos);
    CHECK(result.quads.find("methods: move") != std::string::npos);
    CHECK(result.quads.find("operators: +→move") != std::string::npos);
    // 2. 方法体：label + 隐式 self + 带参 + 返回。
    CHECK(result.quads.find("(label, _, _, Point.move)") != std::string::npos);
    CHECK(result.quads.find("(decl, i64, local, self)") != std::string::npos);
    CHECK(result.quads.find("(decl, i32, local, dx)") != std::string::npos);
    // 3. 成员链 / 下标链 / 运算符脱糖。
    CHECK(result.quads.find("(field, ") == std::string::npos);
    CHECK(result.quads.find("(idx, ") == std::string::npos);
    const std::size_t calls = result.quads.find("(call, Point.move, _, ");
    CHECK(calls != std::string::npos);
    CHECK(result.quads.find("(call, Point.move, _, ", calls + 1u) != std::string::npos); // 方法 + 重载
    // 4. 数据区：数组 meta、嵌套对象、枚举载荷、字符串。
    CHECK(result.data.find("#   meta: count=3;align=8;label=shape_points") != std::string::npos);
    CHECK(result.data.find("Tag(") != std::string::npos || result.data.find("Named(") != std::string::npos);
    CHECK(result.data.find("str_") != std::string::npos);
    CHECK(result.data.find("struct Shape") != std::string::npos);
    CHECK(result.data.find("struct Pair") != std::string::npos);
    // 5. 二进制导出：字符串字节在、条目按 align 对齐、枚举 tag 按分支序号。
    const std::string binary = result.exportDataBinary();
    CHECK(!binary.empty());
    CHECK(binary.find("s0") != std::string::npos); // 字符串字节 + \0 进二进制
    const std::vector<std::string> layout = result.exportDataLayout();
    CHECK(layout.size() == result.entries.size());
    CHECK(layout.front().find("offset=8") != std::string::npos);
    CHECK(result.exportDataHeader("t").find("_data[]") != std::string::npos);
    // Tag::Named 的 tag 是 1（Empty 在前）。
    bool sawTagOne = false;
    for (std::size_t index = 0; index + 4u <= binary.size(); ++index)
    {
        if (static_cast<unsigned char>(binary[index]) == 1u &&
            static_cast<unsigned char>(binary[index + 1u]) == 0u &&
            static_cast<unsigned char>(binary[index + 2u]) == 0u &&
            static_cast<unsigned char>(binary[index + 3u]) == 0u)
        {
            sawTagOne = true;
        }
    }
    CHECK(sawTagOne);
}

/** 链式运算符重载：`(a + b) + c` 里第二处 `+` 也要脱糖（靠返回类型推断）。 */
void testChainedOverload()
{
    using namespace simple_ast;
    Program program;
    const auto point = Type::structOf("Point", {{"x", Type::i32()}});
    program.types = {point};

    Function add;
    add.name = "add";
    add.parameters = {{"other", Type::i64()}};
    add.result = point;                       // 返回本类型，才能继续参与运算
    add.body = Node::block({Node::ret(Node::var("self"))});
    program.addMethod("Point", add);
    program.addOperator("Point", "+", "add");

    Function entry;
    entry.name = "main";
    entry.result = Type::i32();
    entry.body = Node::block({
        Node::exprStmt(Node::binary("+",
                                    Node::binary("+", Node::structNew("Point", {Node::i(1)}),
                                                 Node::structNew("Point", {Node::i(2)})),
                                    Node::structNew("Point", {Node::i(3)}))),
        Node::ret(Node::i(0)),
    });
    program.functions.push_back(std::move(entry));

    const CodegenResult result = generate(program, "chain");
    CHECK(result.ok());
    // 两处 `+` 都要生成 Point.add 调用。
    const std::size_t first = result.quads.find("(call, Point.add, _, ");
    CHECK(first != std::string::npos);
    CHECK(result.quads.find("(call, Point.add, _, ", first + 1u) != std::string::npos);
    // 复合类型在四元式里统一是 i64 句柄，所以 `self` 的声明就是 `(decl, i64, local, self)`；
    // 真正证明"按 Point 处理"的是上面两处 Point.add 都成功脱糖。
    CHECK(result.quads.find("(decl, i64, local, self)") != std::string::npos);
    CHECK(result.quads.find("(param, ") != std::string::npos);
}

/**
 * 前端（C 子集那类工程）需要的最小能力：带类型的局部变量、左值赋值、
 * 强转、常量文本输出 —— 四条都只看生成的四元式。
 */
void testFrontendHelpers()
{
    using namespace simple_ast;
    Program program;
    program.addVariable("a", Type::array(Type::i32(), "count=3;align=8;label=a"));
    Function helper;
    helper.name = "helper";
    helper.parameters = {{"x", Type::i32()}, {"y", Type::i32()}};
    helper.result = Type::i32();
    helper.body = Node::block({Node::ret(Node::binary("+", Node::var("x"), Node::var("y")))});
    program.functions.push_back(helper);
    Function f;
    f.name = "main";
    f.body = Node::block({
        Node::declare("i", Type::i32()),                                  // 局部变量带类型
        Node::assign(Node::index(Node::var("a"), Node::i(1)), Node::i(7)), // 左值：a[1] = 7
        Node::assign("i", Node::cast(Node::f(2.5), Type::i32())),          // (int)2.5
        Node::assign("i", Node::unary("-", Node::var("i"))),               // 一元负号 → neg
        Node::exprStmt(Node::call("helper", {"x", "y"}, {Node::i(1), Node::i(2)})), // 形参按名字绑定
        Node::print("ok\n"),                                               // 常量文本
        Node::putc(Node::i(10)),                                           // 数值转文本：putc
        Node::read("i"),                                                   // 输入：从输入取一项
        Node::doWhileLoop(Node::assign("i", Node::binary("+", Node::var("i"), Node::i(1))),
                          Node::binary("<", Node::var("i"), Node::i(3))),  // do-while
        Node::label("again"),                                              // 标签
        Node::jump("again"),                                               // goto
        Node::write(Node::offset(Node::var("a"), Node::i(2), 16)),         // 结构体数组：按 stride 取地址
        Node::write(Node::var("i")),
    });
    program.functions.push_back(f);
    const CodegenResult result = generate(program, "helpers");
    CHECK(result.ok());
    CHECK(result.quads.find("(decl, i32, local, i)") != std::string::npos);
    // 左值赋值带元素类型：`a[1] = 7`（i32 数组）→ `(store:i32, 地址, 值, _)`，
    // 浮点元素就是 `(store:f64 …)`，后端按这个后缀决定写整数还是写 IEEE-754 位模式。
    CHECK(result.quads.find("(store:i32, ") != std::string::npos);
    CHECK(result.quads.find("(cvt:i32, ") != std::string::npos);
    CHECK(result.quads.find("(neg:i32, ") != std::string::npos);
    CHECK(result.quads.find("(param, ") != std::string::npos &&
          result.quads.find(", _, x)") != std::string::npos &&
          result.quads.find(", _, y)") != std::string::npos);
    CHECK(result.quads.find("(call, helper, _, ") != std::string::npos);
    CHECK(result.quads.find("(putc, 10, _, _)") != std::string::npos);
    CHECK(result.quads.find("(read, _, _, i)") != std::string::npos);
    CHECK(result.quads.find("(jnz, ") != std::string::npos);
    CHECK(result.quads.find("(label, _, _, again)") != std::string::npos);
    CHECK(result.quads.find("(j, _, _, again)") != std::string::npos);
    CHECK(result.quads.find("(*:i64, 2, 16, ") != std::string::npos);
    CHECK(result.quads.find("(print, \"ok\\n\", _, _)") != std::string::npos);
}

} // namespace

int main()
{
    testTypes();
    testGenerate();
    testReferenceRules();
    testLogicalShortCircuit();
    testComplexTypes();
    testChainedOverload();
    testFrontendHelpers();
    if (failures == 0)
    {
        std::cout << "simple_ast-tests：全部通过（" << checks << " 项断言）\n";
        return 0;
    }
    std::cout << "simple_ast-tests：" << failures << " / " << checks << " 项断言失败\n";
    return 1;
}
