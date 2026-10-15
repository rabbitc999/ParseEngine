/**
 * @file simple_ast.hpp
 * @brief 简单 AST 构建：常用类型 / 运算符 / 数组（带 meta）/ 可空引用 / 复合类型 /
 *        简单泛型 / lambda / 带载荷枚举 / 字符串，并生成四元式与 `.data` 文件。
 *
 * 定位：`library/transfer` 与 `library/stackvm` 吃的都是**四元式**；本模块站在更上层，
 * 提供"用 C++ 代码搭一棵 AST，然后把它落成四元式 + 数据文件"的最小前端。
 * 只依赖 C++17 标准库。
 *
 * 复合类型（struct）还可以**定义方法**并**重载运算符**：
 *
 * ```cpp
 * auto vec = Type::structOf("Vec", {{"x", Type::f64()}, {"y", Type::f64()}});
 * program.addMethod("Vec", method("add", {"other"}, Type::i64(),
 *                                Node::block({ Node::ret(Node::binary("+", ...)) })));
 * program.addOperator("Vec", "+", "add");        // `a + b` 脱糖成 `a.add(b)`
 * ```
 *
 * 生成的四元式里，方法就是一个普通函数 `label`（名字是 `Vec.add`），隐式形参 `self`
 * 由调用方先 `param` 进来；运算符重载在生成期就完成脱糖，四元式层面看不到区别。
 *
 * **数组与结构体降级成"基址 + 偏移"**：生成的四元式里没有 `idx` / `field` 这类伪指令，
 * 复合值统一是 `i64` 句柄，数据区按"每个槽位 8 字节"扁平铺开：
 *
 * ```text
 * obj.field      →  t = obj + 字段序号×8
 * arr[index]     →  t1 = index * 8;  t = arr + t1
 * ```
 *
 * 成员与下标都只是标量乘加；真实布局与 meta（`count=` / `align=`）写在 `.data` 里。
 */

#ifndef PARSE_ENGINE_SIMPLE_AST_HPP
#define PARSE_ENGINE_SIMPLE_AST_HPP

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace simple_ast
{

struct Type;
using TypePtr = std::shared_ptr<Type>;

/**
 * @brief 类型。
 *
 * 常见类型直接 `Type::i32()` 这样构造；复合类型用 `array()` / `ref()` / `structOf()` /
 * `enumOf()` / `generic()`。`meta` 用来携带附加信息（数组长度、对齐、数据标签…），
 * 会原样写进 `.data` 文件的注释头。
 */
struct Type
{
    enum class Kind
    {
        Void, Bool, Int, Uint, Float, String, Array, Ref, Struct, Enum, Generic,
    };

    Kind kind = Kind::Void;                                   ///< 种类。
    int bits = 0;                                             ///< 整数 / 浮点位宽。
    std::string name;                                         ///< 复合类型名 / 泛型参数名。
    TypePtr element;                                          ///< 数组元素 / 引用目标。
    std::vector<std::pair<std::string, TypePtr>> fields;      ///< 结构体字段。
    std::vector<std::pair<std::string, TypePtr>> variants;    ///< 枚举分支（载荷类型可为空）。
    std::string meta;                                         ///< 附加信息（数组等）。
    std::vector<std::pair<std::string, std::string>> operators; ///< 运算符重载：运算符 → 方法名。
    std::vector<std::string> methods;                         ///< 复合类型上定义的方法名。

    static TypePtr make(Kind kind, int bits = 0);             ///< 造一个基础类型。
    static TypePtr i8();  static TypePtr i16();  static TypePtr i32();  static TypePtr i64();
    static TypePtr u8();  static TypePtr u16();  static TypePtr u32();  static TypePtr u64();
    static TypePtr f32(); static TypePtr f64();
    static TypePtr boolean(); static TypePtr stringType();
    static TypePtr array(TypePtr element, std::string meta = {});   ///< 数组（元素 + meta）。
    static TypePtr ref(TypePtr target);                             ///< 可空引用（不做指针运算）。
    static TypePtr structOf(std::string name,
                            std::vector<std::pair<std::string, TypePtr>> fields);
    static TypePtr enumOf(std::string name,
                          std::vector<std::pair<std::string, TypePtr>> variants);
    static TypePtr generic(std::string name);                       ///< 简单泛型参数。

    /** 四元式里的类型名（复合 / 引用统一用 i64 句柄，真实布局在 `.data` 里）。 */
    [[nodiscard]] std::string quadType() const;
    /** 人类可读的类型名，例如 `array<u16>[4]{meta:align=2}`。 */
    [[nodiscard]] std::string describe() const;
};

struct Node;
using NodePtr = std::shared_ptr<Node>;

/**
 * @brief 表达式 / 语句节点（紧凑的通用节点：种类 + 文本 + 数字 + 子节点）。
 */
struct Node
{
    enum class Kind
    {
        IntLit, FloatLit, StringLit, BoolLit, NullLit,
        Var, Binary, Unary, Sequence, Index, Member, Call, Lambda,
        StructNew, EnumNew, ArrayNew,
        MethodCall,
        Assign, If, While, DoWhile, Return, ExprStmt, Block, Write, Read, Offset,
        Label, Jump,
        Declare, Cast, Print, Putc, Alloc, Free, Drop, LoadInt, StoreAt, CheckIndex,
        CheckPointer, CallPointer,
    };

    Kind kind = Kind::IntLit;      ///< 种类。
    std::string text;              ///< 名字 / 运算符 / 类型名 / 字符串内容。
    std::string typeName;          ///< 构造类节点（结构体 / 枚举）的类型名。
    std::int64_t intValue = 0;     ///< 整数字面量。
    double floatValue = 0.0;       ///< 浮点字面量。
    TypePtr declared;              ///< `Declare` 节点：被声明变量的类型。
    std::vector<std::string> names;///< `Call` 节点：实参对应的**形参名**（空 = 一律叫 `arg`）。
    std::vector<NodePtr> kids;     ///< 子节点。

    /** 造一个整数字面量。 */
    static NodePtr i(std::int64_t value);
    /** 造一个浮点字面量。 */
    static NodePtr f(double value);
    /** 造一个字符串字面量（内容进 `.data`）。 */
    static NodePtr str(std::string text);
    /**
     * @brief **具名**字符串数据项：同名只登记一次，表达式的值是它的地址。
     *
     * `str()` 每次发射都新开一个数据项（同一个内容出现两次就两份）；要"同一个东西
     * 地址始终一致"就得用这个（编译器给函数指针发的"函数标签"就是它 —— 于是
     * `p == 函数名` 能按地址比）。
     * @param name 数据项名字（要唯一地对应一份内容）。
     * @param text 内容（不含引号）。
     */
    static NodePtr namedStr(std::string name, std::string text);
    /** 造布尔 / null 字面量。 */
    static NodePtr boolean(bool value);
    static NodePtr null();
    /** 变量引用。 */
    static NodePtr var(std::string name);
    /** 二元 / 一元运算。 */
    static NodePtr binary(std::string op, NodePtr left, NodePtr right);
    /** 在表达式所在的控制流位置依次执行语句，再求最后的表达式。 */
    static NodePtr sequence(std::vector<NodePtr> statements, NodePtr value);
    static NodePtr unary(std::string op, NodePtr operand);
    /** 下标 / 成员访问。 */
    static NodePtr index(NodePtr target, NodePtr position);
    /**
     * @brief 按**自定义步长**算元素地址（不取值）：`base + position × stride`。
     *
     * `index` 固定按 8 字节槽位取值，适合标量数组；元素是结构体时每个元素占
     * `字段数 × 8` 字节、而表达式要的是**句柄**（地址）而不是取出来的值，
     * 这时用 `offset`（结构体数组 `arr[i].f` 就靠它）。
     */
    static NodePtr offset(NodePtr base, NodePtr position, std::int64_t stride);
    static NodePtr member(NodePtr target, std::string field);
    /** 调用 lambda / 函数。 */
    static NodePtr call(std::string callee, std::vector<NodePtr> args);
    /**
     * @brief 带形参名的调用：第 i 个实参绑定到 `parameterNames[i]`。
     *
     * 四元式里 `(param, 实参, _, 形参名)` 是**按名字**绑定的（见 stackvm 的 `Op::Call`），
     * 所以多参数调用必须把 callee 的形参名逐个写出来；只写 `arg` 会让所有实参挤在同一个槽。
     * 方法调用也可以走这里：形参名写 `self` + 各实参名，callee 写 `Type.方法名`。
     */
    static NodePtr call(std::string callee, std::vector<std::string> parameterNames,
                        std::vector<NodePtr> args);
    /**
     * @brief 间接调用（函数指针）：`(param …)` + `(callptr, 函数指针, _, 目标)`。
     *
     * `target` 的值是"数据区里存着函数标签的那串字节"的地址（0 = 空指针）。
     * 栈机侧实参**按位置**绑定：位置表由组装时从"函数标签前面那段 decl"扫出来，
     * 所以这里的 `parameterNames` 只是占位（走 `(param, 值, _, 名字)` 的格式而已）。
     */
    static NodePtr callPointer(NodePtr target, std::vector<std::string> parameterNames,
                               std::vector<NodePtr> args);
    /** lambda：`body` 会被抽成独立函数，表达式值是闭包句柄。 */
    static NodePtr lambda(NodePtr body);
    /** 方法调用：`object.method(args)`（复合类型定义的方法）。 */
    static NodePtr methodCall(NodePtr object, std::string method, std::vector<NodePtr> args);
    /** 数组 / 结构体 / 带载荷枚举的构造。 */
    static NodePtr arrayNew(TypePtr type, std::vector<NodePtr> items);
    static NodePtr structNew(std::string typeName, std::vector<NodePtr> fields);
    static NodePtr enumNew(std::string typeName, std::string variant,
                           std::vector<NodePtr> payload);
    /**
     * @brief 堆分配：`(alloc, 字节数, _, t)`，表达式的值是拿到的地址（`i64`）。
     *
     * 与 `arrayNew` / `structNew` 的区别：那两个把对象放进**数据区**（每个声明点一块、
     * 静态布局），这里要的是**运行期才知道大小**的对象，走虚拟机的堆；什么时候还回去
     * 由程序自己 `freeMemory`（栈机那边是 `(free, 地址, _, _)`）。返回 0 表示没分配到。
     */
    static NodePtr alloc(NodePtr bytes);
    /** 堆释放：`(free, 地址, _, _)`，当语句用（没有返回值）。地址不对就**报错**。 */
    static NodePtr freeMemory(NodePtr pointer);
    /**
     * @brief **软释放**：`(drop, 地址, _, _)`，当语句用。
     *
     * 与 `freeMemory` 的区别是"还不了就当没这回事"：地址是 0（空指针）、指向数据区
     * （静态字面量）、或者那块堆内存**已经还过**了，都不算错。编译器在语句块出口插的
     * 自动释放走这一条（见 magel 的"生命周期"），显式 `delete` 仍用 `freeMemory`。
     */
    static NodePtr release(NodePtr pointer);
    /**
     * @brief 按地址读 8 个字节，当**整数**用（`(load:i64, 地址, _, t)`）。
     *
     * `index()` 的取值宽度跟着**基址的元素类型**走（浮点元素就按 IEEE-754 解释），
     * 而"运行时数组的长度"这类值必须按整数读 —— 所以单独给一个原语。
     */
    static NodePtr loadInt(NodePtr address);
    /**
     * @brief 按地址读一个**指定类型**的值：`(load:T, 地址, _, t)`。
     *
     * 与 `loadInt` 的区别是"这 8 个字节怎么解释"由 `type` 决定（`f64` 按 IEEE-754
     * 读，`i64` 按整数读）。字段访问用它：字段地址是自己按槽位算出来的，不能走
     * `member()` 那种"按名字在基类型里查下标"的路（基类型是内层结构体时会查错）。
     * @param address 地址（i64 表达式）。
     * @param type 值的类型。
     * @return 表达式节点。
     */
    static NodePtr loadFrom(NodePtr address, TypePtr type);
    /**
     * @brief 往一个**算好的地址**写值：`(store:T, 地址, 值, _)`，当语句用。
     *
     * `assign(左值, 值)` 只认"变量 / 下标 / 成员"三种左值；地址已经自己算好（比如
     * 结构体字段的槽位偏移）时用这一条。
     * @param address 地址。
     * @param value 值。
     * @param type 值的类型（决定浮点按位写还是整数写）。
     * @return 语句节点。
     */
    static NodePtr storeAt(NodePtr address, NodePtr value, TypePtr type);
    /**
     * @brief 下标陷阱：`(check, 下标, 上限, 标签)`，越界就终止运行。
     *
     * 表达式的值就是**下标本身**，所以可以直接塞进 `index()` / `offset()` 的位置参数。
     */
    static NodePtr checkIndex(NodePtr index, NodePtr limit, std::string label);
    /**
     * @brief 按区块容量查下标：`(checkptr, 下标, 基址, 步长, 标签)`。
     *
     * 校验"第 index 个元素（`基址 + index × 步长`，共 `步长` 字节）"落在基址所在的
     * **那一块**里（堆块按分配大小、数据区按符号 `size=`）；基址不在任何一块里也报错。
     */
    static NodePtr checkPointer(NodePtr index, NodePtr base, std::int64_t stride,
                                std::string label);
    /** 语句。 */
    /** 声明一个局部变量（`int i;` → `(decl, i32, local, i)`，同时登记类型）。 */
    static NodePtr declare(std::string name, TypePtr type);
    /** 类型转换（`cvt`）：`(int)x`、浮点 ↔ 整数、指针 ↔ 地址。 */
    static NodePtr cast(NodePtr value, TypePtr type);
    static NodePtr assign(std::string target, NodePtr value);
    /** 赋值到下标 / 成员这类左值：`a[i] = v`、`p.x = v`。 */
    static NodePtr assign(NodePtr target, NodePtr value);
    static NodePtr ifThenElse(NodePtr condition, NodePtr thenBranch, NodePtr elseBranch);
    static NodePtr whileLoop(NodePtr condition, NodePtr body);
    /** do … while：先做一遍 body，再判 condition；`(label, …)` + `(jnz, …)` 实现。 */
    static NodePtr doWhileLoop(NodePtr body, NodePtr condition);
    /** 标签与 `goto`（跳转目标的标签名自己保证唯一，生成器原样写进四元式）。 */
    static NodePtr label(std::string name);
    static NodePtr jump(std::string label);
    static NodePtr ret(NodePtr value);
    static NodePtr exprStmt(NodePtr expression);
    /** 输出一个数值（生成 `(write, v, _, _)`，便于端到端验证数据区取值）。 */
    static NodePtr write(NodePtr value);
    /**
     * @brief 从输入读一个值写进变量（生成 `(read, _, _, 变量)`）。
     *
     * 输入项由调用方通过 `Machine::run(program, input)` 按顺序提供（每项一个
     * 数字字面量），类型按变量声明的类型收（整数 / 浮点都行）。
     * @param variable 变量名（要先 `Node::declare` 或当形参声明过）。
     */
    static NodePtr read(std::string variable);
    /** 输出一段文本（生成 `(print, "…", _, _)`）：格式串、换行这类常量文本用它。 */
    static NodePtr print(std::string text);
    /** 输出一个字符（生成 `(putc, v, _, _)`，值按低 8 位当字符码）：数值转文本时用它。 */
    static NodePtr putc(NodePtr value);
    static NodePtr block(std::vector<NodePtr> statements);
};

/**
 * @brief 一个函数 / lambda。
 */
struct Function
{
    std::string name;                                                    ///< 名字（同时也是 quad 标签）。
    std::string owner;                                                   ///< 所属复合类型名；空 = 普通函数。
    std::vector<std::pair<std::string, TypePtr>> parameters;              ///< 形参。
    TypePtr result = Type::i32();                                        ///< 返回类型。
    NodePtr body;                                                        ///< 函数体（`Block`）。
    std::vector<TypePtr> genericParams;                                  ///< 简单泛型参数（不做特化）。

    /** 四元式里的标签：方法是 `Type.name`，普通函数就是 `name`。 */
    [[nodiscard]] std::string quadLabel() const
    {
        return owner.empty() ? name : (owner + "." + name);
    }
};

/**
 * @brief 一份"程序"：类型声明 + 常量 + 变量 + 函数。
 */
struct Program
{
    std::vector<TypePtr> types;                                              ///< 复合类型声明。
    std::vector<std::pair<std::string, TypePtr>> constants;                   ///< 常量。
    std::vector<std::pair<std::string, TypePtr>> variables;                   ///< 全局变量。
    std::vector<std::pair<std::string, TypePtr>> constantValues;              ///< 常量的字面量原文。
    std::vector<Function> functions;                                          ///< 函数。

    /**
     * @brief 声明一个常量（值必须是字面量）。
     * @param name 名字。
     * @param type 类型。
     * @param literal 字面量原文（例如 `42`、`"hi"`）。
     */
    void addConstant(const std::string &name, TypePtr type, const std::string &literal);

    /**
     * @brief 声明一个全局变量。
     * @param name 名字。
     * @param type 类型。
     */
    void addVariable(const std::string &name, TypePtr type);

    /**
     * @brief 给复合类型加一个方法（结构体 / 枚举上的函数）。
     * @param typeName 复合类型名。
     * @param function 方法（`owner` 会自动填成 `typeName`，形参表里请不要重复写 `self`）。
     */
    void addMethod(const std::string &typeName, Function function);

    /**
     * @brief 声明一个运算符重载：`a OP b` 脱糖成 `a.method(b)`。
     * @param typeName 复合类型名。
     * @param op 运算符（`+ - * / % == != < <= > >= []`）。
     * @param methodName 处理这个运算符的方法名。
     */
    void addOperator(const std::string &typeName, const std::string &op,
                     const std::string &methodName);
};

/**
 * @brief 生成结果。
 */
struct CodegenResult
{
    std::string quads;                         ///< 四元式文本（可直接喂给 transfer / stackvm）。
    std::string data;                          ///< `.data` 文件内容（数组 / 字符串 / 枚举载荷）。
    std::vector<std::string> diagnostics;      ///< 诊断（类型不对、引用参与运算等）。

    /** 数据区条目（二进制导出按这个顺序排布）。 */
    struct Entry
    {
        std::string name;      ///< 数据标签（`arr_1` / `str_4` / `obj_5` / `enum_6`）。
        TypePtr type;          ///< 类型（meta 里带 `count=` / `align=` 等）。
        std::string body;      ///< 文本形态的值：`[1, 2]` / `"hi"` / `{1, 2}` / `Some(7)`。
    };

    std::vector<Entry> entries;                ///< 数据区条目。

    /** 是否没有诊断。 */
    [[nodiscard]] bool ok() const { return diagnostics.empty(); }

    /**
     * @brief 导出 `.data` 的**二进制**形态（小端）。
     *
     * 布局规则（简单、够嵌入式用）：
     *
     *   - 每个条目按 `align=` 对齐（缺省 8 字节），不足补 0；
     *   - 字符串：UTF-8 字节 + `\0`；
     *   - 数值：8 字节有符号 / 浮点（按条目类型：f* 用 IEEE-754 双精度）；
     *   - 数组 / 结构体：元素依次排布；
     *   - 枚举：4 字节 tag（分支序号）+ 载荷；
     *   - 引用 / 字符串 / 复合对象出现在数组里时写 4 字节"条目序号"当句柄。
     *
     * @return 二进制内容。
     */
    [[nodiscard]] std::string exportDataBinary() const;

    /**
     * @brief 导出配套的 C 头文件（偏移表 + 字节数组）。
     * @param moduleName 模块名（写进注释与宏前缀）。
     * @return 头文件文本。
     */
    [[nodiscard]] std::string exportDataHeader(const std::string &moduleName) const;

    /**
     * @brief 每个条目在二进制里的偏移与大小（便于查表 / 测试）。
     * @return 一行一条：`名字 offset=… size=… type=…`。
     */
    [[nodiscard]] std::vector<std::string> exportDataLayout() const;
};

/**
 * @brief 把程序落成四元式 + 数据文件。
 * @param program 程序。
 * @param moduleName 模块名（写进文件头注释）。
 * @return 生成结果。
 */
[[nodiscard]] CodegenResult generate(const Program &program, const std::string &moduleName = "module");

} // namespace simple_ast

#endif // PARSE_ENGINE_SIMPLE_AST_HPP
