/**
 * @file main.cpp
 * @brief simple_ast 演示：把一份用到全部特性的程序生成 `<out>.quad` 与 `<out>.data`。
 *
 *   simple_ast <输出前缀>        （缺省 out/simple_ast_demo）
 */

#include "simple_ast.hpp"

#include <fstream>
#include <iostream>
#include <string>

int main(int argc, char **argv)
{
    using namespace simple_ast;

    Program program;

    // 1. 复合类型 + 方法 + 运算符重载。
    auto vec = Type::structOf("Vec", {{"x", Type::f64()}, {"y", Type::f64()}});
    auto box = Type::structOf("Box", {{"value", Type::i32()}});           // 复合类型：不可空
    auto maybe = Type::enumOf("Maybe", {{"None", nullptr}, {"Some", Type::i32()}}); // 带载荷枚举
    program.types = {vec, box, maybe};

    Function add;
    add.name = "add";
    add.parameters = {{"other", Type::i64()}};
    add.result = Type::i64();
    add.body = Node::block({Node::ret(Node::var("self"))});               // 演示：返回 self
    program.addMethod("Vec", add);
    program.addOperator("Vec", "+", "add");                               // `a + b` → `a.add(b)`
    program.addOperator("Vec", "==", "add");

    // 2. 泛型（简单泛型，不做特化）：`Pair<T>` 只是记号，生成期按 i64 句柄处理。
    program.types.push_back(Type::structOf("Pair",
        {{"first", Type::generic("T")}, {"second", Type::generic("T")}}));

    // 3. 常量 / 数组（带 meta）/ 可空引用。
    program.addConstant("LIMIT", Type::i32(), "4");
    program.addVariable("numbers", Type::array(Type::u16(), "count=4;align=2;label=numbers"));
    program.addVariable("current", Type::ref(Type::i32()));               // 可空引用，不做指针运算
    program.addVariable("b", Type::i32());

    // 4. lambda：抽成独立函数 + 数据区闭包句柄。
    Function entry;
    entry.name = "main";
    entry.result = Type::i32();
    entry.body = Node::block({
            Node::assign("numbers", Node::arrayNew(
                Type::array(Type::u16(), "count=4;align=2;label=numbers"),
                {Node::i(10), Node::i(20), Node::i(30), Node::i(40)})),
            Node::assign("b", Node::index(Node::var("numbers"), Node::i(2))),
            Node::assign("current", Node::null()),                        // 引用可以为空
            Node::ifThenElse(Node::binary("==", Node::var("current"), Node::null()),
                             Node::exprStmt(Node::binary("==", Node::var("b"), Node::var("b"))),
                             Node::exprStmt(Node::binary("!=", Node::var("b"), Node::var("b")))),
            Node::exprStmt(Node::structNew("Box", {Node::var("b")})),
            Node::exprStmt(Node::enumNew("Maybe", "Some", {Node::i(7)})),
            Node::exprStmt(Node::methodCall(Node::structNew("Vec", {Node::f(1.5), Node::f(2.5)}),
                                            "add",
                                            {Node::structNew("Vec", {Node::f(0.5), Node::f(0.5)})})),
            Node::exprStmt(Node::binary("+", Node::structNew("Vec", {Node::f(1.0), Node::f(2.0)}),
                                        Node::structNew("Vec", {Node::f(3.0), Node::f(4.0)}))),
            Node::exprStmt(Node::lambda(Node::block({}))),
            Node::write(Node::var("b")),   // 先打印从数据区取出的 numbers[2]（应为 30）
            Node::whileLoop(Node::binary("<", Node::var("b"), Node::var("LIMIT")),
                            Node::assign("b", Node::binary("+", Node::var("b"), Node::i(1)))),
            Node::write(Node::var("b")),   // 再打印累加到 LIMIT 之后的值（应为 4）
            Node::ret(Node::i(0)),
        });
    program.functions.push_back(std::move(entry));

    const std::string prefix = (argc > 1) ? argv[1] : "out/simple_ast_demo";
    const CodegenResult result = generate(program, "simple_ast_demo");

    std::ofstream quads(prefix + ".quad", std::ios::binary);
    std::ofstream data(prefix + ".data", std::ios::binary);
    std::ofstream binary(prefix + ".data.bin", std::ios::binary);
    std::ofstream header(prefix + ".data.h", std::ios::binary);
    std::ofstream layout(prefix + ".layout", std::ios::binary);
    quads << result.quads;
    data << result.data;
    binary << result.exportDataBinary();
    header << result.exportDataHeader("simple_ast_demo");
    // 布局表：`符号 offset=… size=… type=…`，stackvm --layout 直接读它。
    for (const std::string &line : result.exportDataLayout()) layout << line << "\n";

    for (const std::string &message : result.diagnostics) std::cerr << "诊断：" << message << "\n";
    std::cout << "已生成 " << prefix << ".quad（" << result.quads.size() << " 字节）与 "
              << prefix << ".data（" << result.data.size() << " 字节），诊断 "
              << result.diagnostics.size() << " 条\n";
    return result.ok() ? 0 : 1;
}
