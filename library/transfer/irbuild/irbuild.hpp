#pragma once
// 四元式 / 三元式**构建库**：用链式调用直接造 IR，不必拼文本再解析。
//
//   QuadBuilder   造带类型四元式 → 文本（喂 transfer）/ TT3（喂 dataflow / ssa / regalloc）
//   TripleBuilder 造三元式（结果用下标 `(k)` 引用）→ 四元式 → TT3
//
// 字段惯例与 `irbridge` 的 fixture 一致：
//   四元式 `(op, a, b, result)`：`(:=, 值, _, 目标)`、`(jz, 值, _, 标号)`、`(label, _, _, L)`
//   三元式 `(op, a, b)`：`(:=, 值, 目标)`、`(jz, 值, 标号)`、`(label, L, _)`，结果引用写 `(k)`
#include "irbridge.hpp"
#include "transfer.hpp"
#include "typed_ir.hpp"

#include <cstddef>
#include <string>

namespace t1t {

/**
 * 四元式构建器（`transfer::Program` 的链式写入器）。
 *
 * 每个方法都往程序末尾追加一条四元式并返回自身，因此可以一路点下去；
 * `text()` 得到与仓库 `.quad` 示例同形的文本，`lower()` 得到统一 IR（TT3）。
 *
 * ```cpp
 * t1t::QuadBuilder q;
 * q.declare("x", "i32").assign("2", "x").add("x", "3", "t").write("t").halt();
 * std::istringstream in(q.text());
 * const transfer::Program program = transfer::read(in);   // 与手写 .quad 等价
 * ```
 *
 * 构建器**不做语义检查**：操作名是否合法、标号是否存在、类型是否匹配，
 * 仍由 `transfer::validate` 与生成 / 模拟阶段负责，这样"故意写错"的测试程序也能造。
 */
class QuadBuilder {
public:
    /**
     * 追加一条变量声明：`(decl, 类型, global, 名字)`。
     *
     * 声明决定该名字在后端被当作哪种类型；没有声明的名字按 `i16` 处理。
     * 建议先声明再写指令——构建器只按调用顺序追加，不替你重排。
     *
     * @param name 变量名（字母 / 数字 / 下划线，不以数字开头）
     * @param type 类型名（见 types.hpp 的类型表）；空串等同 `"i16"`
     * @return `*this`
     */
    QuadBuilder& declare(const std::string& name, const std::string& type = "i16");

    /**
     * 追加一条赋值：`(:=, 值, _, 目标)`。
     *
     * @param value  右值（整型 / 浮点 / 字符串字面量，或另一个变量名）
     * @param target 左值变量名
     * @return `*this`
     */
    QuadBuilder& assign(const std::string& value, const std::string& target);

    /**
     * 追加任意二元运算：`(op, a, b, 目标)`，`type` 非空时写成 `(op:类型, a, b, 目标)`。
     *
     * 这是通用入口：加减乘除、比较、按位、移位、浮点都能用；接受哪些 `op`
     * 由 transfer 的校验与生成阶段决定。双结果指令（乘法的另一半、除法的余数）
     * 请用 `raw()`。
     *
     * @param op     操作名（`"+"`、`"*"`、`"<="`、`"<<"` …）
     * @param a      左操作数
     * @param b      右操作数
     * @param target 结果变量名
     * @param type   可选类型后缀（`"u16"`、`"f64"` …）
     * @return `*this`
     */
    QuadBuilder& op(const std::string& op, const std::string& a, const std::string& b,
                    const std::string& target, const std::string& type = {});

    /**
     * 追加加法 `(+, a, b, 目标)`；`type` 非空时带类型后缀。
     */
    QuadBuilder& add(const std::string& a, const std::string& b, const std::string& t, const std::string& type = {});

    /** 追加减法 `(-, a, b, 目标)`。 */
    QuadBuilder& sub(const std::string& a, const std::string& b, const std::string& t, const std::string& type = {});

    /**
     * 追加乘法 `(*, a, b, 目标)`。
     *
     * 宽类型或 `mul` 指令族在生成阶段会拆成高低半（`HI` / `LO`）；
     * 需要显式拿另一半时用 `raw("mul:类型", a, b, lo, hi)`。
     */
    QuadBuilder& mul(const std::string& a, const std::string& b, const std::string& t, const std::string& type = {});

    /**
     * 追加除法 `(/, a, b, 目标)`；需要余数时用 `raw("div:类型", a, b, 商, 余数)`。
     */
    QuadBuilder& div(const std::string& a, const std::string& b, const std::string& t, const std::string& type = {});

    /**
     * 追加一个标号：`(label, _, _, 名字)`。标号既是跳转目标，也是基本块边界。
     *
     * @param name 标号名（与变量名同一套字符规则，但两者互不冲突）
     * @return `*this`
     */
    QuadBuilder& label(const std::string& name);

    /**
     * 追加无条件跳转：`(j, _, _, 标号)`。
     *
     * @param label 目标标号；是否存在由 `transfer::validate` 检查
     * @return `*this`
     */
    QuadBuilder& jump(const std::string& label);

    /**
     * 追加"为零则跳转"：`(jz, 值, _, 标号)`。
     *
     * @param value 被判断的值（字面量或变量名）
     * @param label 值为 0 时跳到的标号
     * @return `*this`
     */
    QuadBuilder& jumpIfZero(const std::string& value, const std::string& label);

    /**
     * 追加输出：`(write, 值, _, _)`，按该值的类型打印到 STDOUT。
     *
     * @param value 要输出的值
     * @return `*this`
     */
    QuadBuilder& write(const std::string& value);

    /**
     * 追加停机指令 `(halt, _, _, _)`：模拟器执行到这里就返回结果。
     */
    QuadBuilder& halt();

    /**
     * 原样追加一条四元式，用于上面没提供便捷方法的指令族。
     *
     * ```cpp
     * q.raw("mul:u16", "a", "b", "lo", "hi");   // (mul:u16, a, b, lo, hi)
     * q.raw("cvt", "src", "i32", "dst");        // (cvt, src, i32, dst)
     * ```
     *
     * @param op     操作名（写什么就是什么，可带 `:类型`）
     * @param a      第一操作数
     * @param b      第二操作数（没有写 `"_"`）
     * @param result 结果（没有写 `"_"`）
     * @param extra  双结果指令的第二目标（没有写 `"_"`）
     * @return `*this`
     */
    QuadBuilder& raw(const std::string& op, const std::string& a, const std::string& b,
                     const std::string& result, const std::string& extra = "_");

    /**
     * 已攒出来的程序（顺序即调用顺序）；生命周期与构建器相同。
     */
    const transfer::Program& program() const { return program_; }

    /** 当前已追加多少条四元式（含 `decl`）。 */
    std::size_t size() const { return program_.size(); }

    /**
     * 按仓库的稳定格式写出四元式文本：每条形如 `  3: (+, x, 1, t)`，末尾带换行。
     * 可以直接落成 `.quad` 文件，也可以喂给 `transfer::read`（逐字往返）。
     */
    std::string text() const;

    /**
     * 转成统一 IR（TT3，带位数信息）：四元式 → `decl` 表 + 三地址指令。
     * 需要 SSA / 数据流 / 寄存器分配时用这个，而不是把它们再解析一遍文本。
     */
    TypedProgram lower() const;

    /**
     * `lower()` 的文本形式（`decl i, i16` / `add:i16 i, i, 1` …），便于打印与比对。
     */
    std::string lowerText() const { return print(lower()); }

private:
    transfer::Program program_;
};

/**
 * 三元式构建器（`irbridge::Triples` 的链式写入器）。
 *
 * 三元式不写结果名：`add()` 等方法返回**本条三元式的下标**，引用时用
 * `ref(k)` 得到 `(k)`。这正是三元式与四元式的本质差别——引用绑定在指令位置上。
 *
 * ```cpp
 * t1t::TripleBuilder t;
 * t.assign("0", "i");
 * const std::size_t inc = t.arithmetic("+", "i", "1");   // (+, i, 1)
 * t.assign(t.ref(inc), "i");                             // (:=, (k), i)
 * t.toQuads();   // 三元式 → 四元式（(k) → 名字）
 * t.toTT3();     // 三元式 → 四元式 → 统一 IR
 * ```
 */
class TripleBuilder {
public:
    /**
     * 追加一条三元式 `(op, a, b)`。
     *
     * @param op 操作名（`"+"`、`":="`、`"jz"`、`"label"` …）
     * @param a  第一操作数（名字、字面量，或 `ref(k)` 产生的 `(k)`）
     * @param b  第二操作数；没有就传 `"_"`
     * @return 本条三元式的下标（后续用 `ref()` 引用它的结果）
     */
    std::size_t add(const std::string& op, const std::string& a, const std::string& b = "_");

    /**
     * 追加赋值 `(:=, 值, 目标)`——三元式里**目标写在第三个字段**，与四元式不同。
     *
     * @param value  右值（字面量、变量名，或 `ref(k)`）
     * @param target 目标变量名
     * @return 本条下标
     */
    std::size_t assign(const std::string& value, const std::string& target);

    /**
     * 追加二元运算 `(op, a, b)`；结果用返回的下标引用。
     *
     * @param op 操作名（`"+"`、`"<="` …）
     * @return 本条下标
     */
    std::size_t arithmetic(const std::string& op, const std::string& a, const std::string& b);

    /** 追加标号 `(label, 名字, _)`。 */
    std::size_t label(const std::string& name);

    /** 追加无条件跳转 `(j, _, 标号)`。 */
    std::size_t jump(const std::string& label);

    /**
     * 追加条件跳转 `(jz, 值, 标号)`。
     *
     * @param value 被判断的值（变量名或 `ref(k)`）
     * @param label 值为 0 时跳到的标号
     */
    std::size_t jumpIfZero(const std::string& value, const std::string& label);

    /** 追加输出 `(write, 值, _)`。 */
    std::size_t write(const std::string& value);

    /** 追加停机 `(halt, _, _)`。 */
    std::size_t halt();

    /**
     * 第 `index` 条三元式的结果引用：`ref(3)` → `"(3)"`。
     *
     * 这是三元式的"结果名"：把 `(k)` 写进后续指令的操作数位置即可；
     * 转四元式时 `irbridge` 会把它解析回对应的名字。
     */
    static std::string ref(std::size_t index);

    /** 已攒出来的三元式序列；生命周期与构建器相同。 */
    const irbridge::Triples& triples() const { return triples_; }

    /** 当前已追加多少条三元式。 */
    std::size_t size() const { return triples_.size(); }

    /**
     * 三元式文本：表头 `三元式：共 N 条` + `  k: (op, a, b)`，与仓库 fixture 同形。
     */
    std::string text() const;

    /**
     * 转成四元式（`(k)` 引用变成结果名字）。
     *
     * 内部先经过一次文本往返，让 `irbridge` 自己的读取器解析 `(k)` 引用，
     * 保证与命令行 `irbridge triples2quads` 完全同一套规则。
     *
     * @return 四元式序列
     * @throws std::runtime_error 引用越界、引用不产生值的条目等转换错误
     */
    irbridge::Quads toQuads() const;

    /**
     * 转成统一 IR（TT3）：三元式 → 四元式 → 三地址。
     *
     * @return 带位数信息的 TT3 程序（`decl` 表 + 指令）
     * @throws std::runtime_error 同 `toQuads()`
     */
    TypedProgram toTT3() const;

private:
    irbridge::Triples triples_;
};

} // namespace t1t