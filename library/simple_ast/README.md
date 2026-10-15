# 简单 AST 构建（`library/simple_ast`）

用 C++ 代码搭一棵 AST，再落成 **四元式**（喂给 `library/transfer` 或 `library/stackvm`）
与 **`.data`**（文本 + 二进制 + C 头）。只依赖 C++17 标准库。

## 覆盖的语法特性

| 特性 | 写法 |
| --- | --- |
| 常用类型 | `Type::i8() … Type::u64()`、`f32/f64`、`boolean()`、`stringType()` |
| 运算符 | 内建 `+ - * / % & \| ^ << >> == != < <= > >= && \|\|`、一元 `! - ~` |
| 数组（带 meta） | `Type::array(elem, "count=4;align=8;label=…")`，meta 进 `.data` 注释头与二进制对齐 |
| 引用指针 | `Type::ref(T)`：**可为空**（`null` → 0），**不做指针运算**（`p + q` 直接报诊断） |
| 复合类型 | `Type::structOf("Vec", {{"x", f64}, …}})`，值语义、不可空 |
| 复合类型定义函数 | `Program::addMethod("Vec", f)`：四元式里是 `(label, _, _, Vec.add)` + 隐式 `self` |
| 运算符重载 | `Program::addOperator("Vec", "+", "add")`：`a + b` 脱糖成 `a.add(b)`，**支持链式**（靠方法返回类型推断） |
| 泛型记号 | `Type::generic("T")` 这个记号与它的断言（`Pair<T>` 的字段类型），不实例化 / 不特化 |
| lambda | `Node::lambda(body)`：抽成独立函数，表达式值是数据区里的闭包句柄 `lambda_N` |
| 枚举（带载荷） | `Type::enumOf("Maybe", {{"None", nullptr}, {"Some", i32}})` + `Node::enumNew(...)` |
| 字符串 | `Node::str("…")` → 数据区 `str_N: "…"`（二进制里是字节 + `\0`）；表达式的值是这串字节的**基址**，能放进变量、当实参传出去、再按地址逐字节取 |
| 地址空间 | **数据区从偏移 8 开始排，地址 0 留给「空指针」** —— 任何真实对象的地址都不为 0，`p == null` 判空才有意义（堆本来也是从数据区之后开始长） |
| 常量 | `Program::addConstant("LIMIT", i32, "4")` |
| 流程 | `assign` / `ifThenElse` / `whileLoop` / `return` / `block` |
| 逻辑短路 | `binary("&&" / "||", lhs, rhs)` → 条件跳转；仅在需要时执行 `rhs` |
| 带类型的局部变量 | `Node::declare("i", Type::i32())` → `(decl, i32, local, i)`（前端做作用域与类型检查用） |
| 左值赋值 | `Node::assign(Node::index(Node::var("a"), Node::i(1)), Node::i(7))` → 先算地址再 `(store, …)`；`Node::assign(Node::member(...), v)` 同理 |
| 强转 | `Node::cast(Node::f(2.5), Type::i32())` → `(cvt:i32, …)`；指针 ↔ 地址也走它 |
| 常量文本输出 | `Node::print("ok\n")` → `(print, "ok\\n", _, _)`（数值输出仍是 `Node::write`） |
| 单字符输出 | `Node::putc(Node::i(10))` → `(putc, 10, _, _)`：数值转文本时逐字符打 |
| 带形参名的调用 | `Node::call("f", {"a", "b"}, {x, y})` → `(param, x, _, a)` `(param, y, _, b)` `(call, f, _, t)`；`param` 是**按名字**绑定的，多参调用必须把形参名逐个写出来 |
| 间接调用（函数指针） | `Node::callPointer(值, {"__magel_p0", …}, {实参})` → `(param …)` + `(callptr, 值, _, t)`：值是「数据区里存着函数标签那串字节」的地址，栈机按**位置**绑实参 |
| 浮点字面量 | `Node::f(2.0)` 在四元式里一定写成 `2.0`（不是 `2`），免得被后端当成整数槽 |
| 堆：分配 / 释放 | `Node::alloc(字节数)` → `(alloc, 字节数, _, t)`（表达式的值是拿到的地址）；`Node::freeMemory(地址)` → `(free, 地址, _, _)`（当语句用）。给"运行期才知道要多少"的对象用，与数据区（编译期布局）分开 |
| 按地址读整数 | `Node::loadInt(地址)` → `(load:i64, 地址, _, t)`：`index()` 的取值宽度跟着基址的**元素类型**走，而"运行时数组的长度"这类值必须按整数读，所以单独给一个原语 |
| 按地址读（指定类型） | `Node::loadFrom(地址, 类型)` → `(load:T, 地址, _, t)`：地址自己算好（比如结构体字段的槽位偏移）时用它，别用 `member()` 那种"按名字查下标"的路 |
| 按地址写 | `Node::storeAt(地址, 值, 类型)` → `(store:T, 地址, 值, _)`：左值不是"变量 / 下标 / 成员"三种时用它；**值的表达式先求、地址后求**（表达式临时量是全局名，递归会互相覆盖） |
| 软释放 | `Node::release(地址)` → `(drop, 地址, _, _)`：只有"地址是活跃堆块"才还，其余（0 / 数据区 / 已经还过）跳过；给作用域自动释放用（严格释放是 `freeMemory`） |
| 下标越界陷阱 | `Node::checkIndex(下标, 上限, 标签)` → `(check, …)`：越界就终止运行，表达式的值仍是下标，可以直接当 `index()` / `offset()` 的位置参数；`Node::checkPointer(下标, 基址, 步长, 标签)` → `(checkptr, …)`：按"基址所在那一块"的容量查（堆块按分配大小、数据区按布局里的 `size=`） |

## 生成产物

```text
simple_ast <输出前缀>            # 缺省 out/simple_ast_demo

<前缀>.quad        四元式（transfer / stackvm 直接能读）
<前缀>.data        文本数据区（数组 / 字符串 / 结构体 / 枚举载荷 + meta 注释头）
<前缀>.data.bin    二进制数据区（小端）
<前缀>.data.h      C 头：偏移宏 + static const unsigned char <module>_data[]
```

二进制布局规则：条目按 `align=`（缺省 8）对齐补 0；字符串 = UTF-8 + `\0`；数值 = 8 字节
（`f*` 用 IEEE-754 双精度）；数组 / 结构体按元素铺；数组里出现的字符串 / 复合对象写
**4 字节条目序号**当句柄；枚举 = 4 字节 tag（`variants` 声明顺序）+ 载荷。

## 代码骨架

```cpp
using namespace simple_ast;
Program program;

auto point = Type::structOf("Point", {{"x", Type::i32()}, {"y", Type::i32()}});
program.types = {point};

Function move;                       // 方法：复合类型上的函数
move.name = "move";
move.parameters = {{"dx", Type::i32()}, {"dy", Type::i32()}};
move.result = point;                 // 返回本类型 → `(a + b) + c` 也能脱糖
move.body = Node::block({
    Node::assign("x", Node::binary("+", Node::member(Node::var("self"), "x"), Node::var("dx"))),
    Node::ret(Node::var("self")),
});
program.addMethod("Point", move);
program.addOperator("Point", "+", "move");     // a + b → a.add(b)

Function entry;
entry.name = "main";
entry.body = Node::block({
    Node::exprStmt(Node::binary("+", Node::structNew("Point", {Node::i(1), Node::i(1)}),
                                Node::structNew("Point", {Node::i(2), Node::i(2)}))),
    Node::ret(Node::i(0)),
});
program.functions.push_back(std::move(entry));

const CodegenResult result = generate(program, "demo");
// result.quads / result.data / result.exportDataBinary() / exportDataHeader("demo")
```

## 构建与自检

```text
make -C library/simple_ast            # libsimple_ast.a + simple_ast + simple_ast-tests
make -C library/simple_ast check      # 64 项断言 + 生成一份全特性演示
```

断言覆盖：类型描述与 `quadType()` 映射、引用限制、数组 meta、方法标签与隐式 `self`、
运算符重载（含链式）、泛型记号字段、带载荷枚举的 tag、字符串进数据区、二进制长度 / 对齐 /
偏移表 / C 头，以及 `lambda_N` 闭包句柄。
