/**
 * @file simple_ast.cpp
 * @brief simple_ast.hpp 的实现：类型打印、AST 构造、四元式 / `.data` 生成。
 */

#include "simple_ast.hpp"

#include <cstdlib>
#include <cstring>
#include <cmath>
#include <functional>
#include <iomanip>
#include <limits>
#include <sstream>

namespace simple_ast
{
namespace
{

using Kind = Type::Kind;
using TypeEnv = std::map<std::string, TypePtr>;

// ---- .data 二进制导出的小工具 --------------------------------------------

/** 从 meta 里取 `key=数字`；没有就返回 fallback。 */
int metaInt(const std::string &meta, const std::string &key, int fallback)
{
    const std::size_t at = meta.find(key + "=");
    if (at == std::string::npos) return fallback;
    return std::atoi(meta.c_str() + at + key.size() + 1u);
}

/** 追加小端整数（8 字节）。 */
void putInt(std::string &out, std::int64_t value)
{
    const std::uint64_t bits = static_cast<std::uint64_t>(value);
    for (int index = 0; index < 8; ++index) out.push_back(static_cast<char>((bits >> (index * 8)) & 0xFFu));
}

/** 追加小端 32 位无符号（4 字节）。 */
void putU32(std::string &out, std::uint32_t value)
{
    for (int index = 0; index < 4; ++index) out.push_back(static_cast<char>((value >> (index * 8)) & 0xFFu));
}

/** 追加小端双精度（8 字节）。 */
void putDouble(std::string &out, double value)
{
    std::uint64_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value), "double 必须是 8 字节");
    std::memcpy(&bits, &value, sizeof(bits));
    for (int index = 0; index < 8; ++index) out.push_back(static_cast<char>((bits >> (index * 8)) & 0xFFu));
}

/** 文本是不是带引号的字符串。 */
bool isQuoted(const std::string &text)
{
    return text.size() >= 2u && text.front() == '"' && text.back() == '"';
}

/** 切顶层逗号（去括号，尊重引号）。 */
std::vector<std::string> splitTop(const std::string &body)
{
    // 先把最外层的 [] / {} / () 剥掉：它们让内容停在 depth=1，
    // 而下面的分隔判断只在 depth==0 生效——不剥就会把 `[10, 20, 30, 40]`
    // 当成一个元素（这正是数组在 .data 里只写 4 字节的原因）。
    std::string text = body;
    if (text.size() >= 2u)
    {
        const char front = text.front();
        const char back = text.back();
        const bool wrapped = (front == '[' && back == ']') || (front == '{' && back == '}') ||
                             (front == '(' && back == ')');
        if (wrapped) text = text.substr(1u, text.size() - 2u);
    }

    std::vector<std::string> parts;
    std::string current;
    int depth = 0;
    bool quoted = false;
    for (const char ch : text)
    {
        if (ch == '"') quoted = !quoted;
        if (!quoted)
        {
            if (ch == '[' || ch == '{' || ch == '(') { ++depth; continue; }
            if (ch == ']' || ch == '}' || ch == ')') { --depth; continue; }
            if (ch == ',' && depth == 0) { parts.push_back(current); current.clear(); continue; }
        }
        current += ch;
    }
    if (!current.empty()) parts.push_back(current);
    return parts;
}

/** 解析数字文本。 */
bool numberText(const std::string &text, bool &isFloat, std::int64_t &intValue, double &doubleValue)
{
    if (text.empty()) return false;
    char *end = nullptr;
    doubleValue = std::strtod(text.c_str(), &end);
    if (end == nullptr || *end != '\0') return false;
    isFloat = (text.find_first_of(".eE") != std::string::npos);
    intValue = static_cast<std::int64_t>(doubleValue);
    return true;
}

/** 生成期状态。 */
struct Emitter
{
    CodegenResult result;
    TypeEnv env;
    std::vector<std::string> dataLines;
    std::vector<std::string> lines;
    std::size_t tempIndex = 0;
    std::size_t labelIndex = 0;
    std::size_t lambdaIndex = 0;

    void error(const std::string &message) { result.diagnostics.push_back(message); }
    void emit(const std::string &text) { lines.push_back(text); }
    std::string temp() { return "t" + std::to_string(++tempIndex); }
    std::string label(const std::string &hint) { return hint + "_" + std::to_string(++labelIndex); }
    void data(const std::string &name, const std::string &body, const TypePtr &type)
    {
        /* 同名数据项只登记一次（`Node::namedStr` 要的是"同一个东西只有一份"）。 */
        for (const CodegenResult::Entry &entry : result.entries)
        {
            if (entry.name == name) return;
        }
        dataLines.push_back("# " + name + " : " + type->describe());
        if (!type->meta.empty()) dataLines.push_back("#   meta: " + type->meta);
        dataLines.push_back(name + ": " + body);
        result.entries.push_back(CodegenResult::Entry{name, type, body});
    }
};

/** 字面量的原文；不是字面量返回空串。 */
std::string literalText(const NodePtr &node)
{
    if (!node) return std::string();
    switch (node->kind)
    {
    case Node::Kind::IntLit: return std::to_string(node->intValue);
    case Node::Kind::FloatLit:
    {
        // 浮点字面量一律带小数点：`2.0` 写成 `2` 的话，四元式的文本形态会被当成整数，
        // 后端要么截断要么按整数压栈，浮点语义就丢了。
        //
        std::ostringstream buffer;
        buffer << std::setprecision(std::numeric_limits<double>::max_digits10)
               << node->floatValue;
        std::string text = buffer.str();
        if (text.find_first_of(".eE") == std::string::npos) text += ".0";
        return text;
    }
    case Node::Kind::BoolLit: return node->intValue != 0 ? "1" : "0";
    case Node::Kind::NullLit: return "0";
    default: return std::string();
    }
}

/**
 * @brief 数据区里的一个字面量：**浮点槽一定带小数点**。
 *
 * `2` 和 `2.0` 写进 `.data.bin` 是完全不同的 8 个字节：整型按整数写、浮点按
 * IEEE-754 写。所以只要槽位类型是浮点，整数写法（隐式转换来的 `0` / `2`）就补成
 * `0.0` / `2.0` —— 数据条目的正文既是给人看的，也决定了二进制怎么解释。
 *
 * @param node 字面量节点。
 * @param slot 槽位类型（元素 / 字段）。
 * @return 正文里的文本；不是字面量返回空串（调用方走 `emitExpr`）。
 */
std::string literalTextFor(const NodePtr &node, const TypePtr &slot)
{
    std::string text = literalText(node);
    if (!text.empty() && slot && (slot->kind == Type::Kind::Float) &&
        (text.find_first_of(".eE") == std::string::npos))
    {
        text += ".0";   // 隐式转换：整数写法落进浮点槽
    }
    return text;
}

/** 推断表达式类型（字面量 / 变量 / 下标 / 成员）。 */
TypePtr infer(const NodePtr &node, const TypeEnv &env)
{
    if (!node) return Type::i32();
    switch (node->kind)
    {
    case Node::Kind::IntLit: return Type::i32();
    case Node::Kind::FloatLit: return Type::f64();
    case Node::Kind::BoolLit: return Type::boolean();
    case Node::Kind::StringLit: return Type::stringType();
    case Node::Kind::Var:
    {
        const auto found = env.find(node->text);
        return found == env.end() ? Type::i32() : found->second;
    }
    case Node::Kind::Index:
    {
        const TypePtr target = infer(node->kids.empty() ? nullptr : node->kids[0], env);
        return target->element ? target->element : Type::i32();
    }
    case Node::Kind::CheckIndex:
    case Node::Kind::CheckPointer:
        return infer(node->kids.empty() ? nullptr : node->kids[0], env);   // 值就是下标
    case Node::Kind::LoadInt:
        return Type::i64();
    case Node::Kind::Offset:
    {
        const TypePtr target = infer(node->kids.empty() ? nullptr : node->kids[0], env);
        return target->element ? target->element : Type::i32();
    }
    case Node::Kind::Member:
    {
        const TypePtr target = infer(node->kids.empty() ? nullptr : node->kids[0], env);
        for (const auto &field : target->fields) if (field.first == node->text) return field.second;
        return Type::i32();
    }
    case Node::Kind::StructNew:
    case Node::Kind::EnumNew:
    {
        // 类型名也放在 env 里（生成前会把声明的复合类型登记进去），这样 `Vec{…} + Vec{…}`
        // 才能查到运算符重载。
        const auto found = env.find(node->typeName.empty() ? node->text : node->typeName);
        return found == env.end() ? Type::i32() : found->second;
    }
    case Node::Kind::Alloc: return Type::i64();   // 堆地址（句柄）
    case Node::Kind::Call:
    {
        // 普通函数 / lambda 的返回类型：generate() 把 `函数标签 → 返回类型` 也放进 env。
        const auto found = env.find(node->text);
        return found == env.end() ? Type::i32() : found->second;
    }
    case Node::Kind::CallPointer:
        return Type::i32();   // 返回类型由调用方（前端）自己掌握
    case Node::Kind::MethodCall:
    {
        // 方法调用：`Type.method` → 返回类型；这样 `(a + b) + c` 才能继续脱糖。
        const TypePtr owner = infer(node->kids.empty() ? nullptr : node->kids[0], env);
        const auto found = env.find(owner->name + "." + node->text);
        return found == env.end() ? Type::i32() : found->second;
    }
    case Node::Kind::Binary:
    {
        const std::string &op = node->text;
        const bool compare = (op == "==" || op == "!=" || op == "<" || op == "<=" ||
                              op == ">" || op == ">=" || op == "&&" || op == "||");
        if (compare) return Type::boolean();
        // 复合类型参与运算时，结果是运算符方法声明的返回类型（链式重载要用）。
        const TypePtr left = infer(node->kids.empty() ? nullptr : node->kids[0], env);
        for (const auto &item : left->operators)
        {
            if (item.first != op) continue;
            const auto found = env.find(left->name + "." + item.second);
            if (found != env.end()) return found->second;
        }
        return left;
    }
    case Node::Kind::Unary:
    {
        if (node->text == "!") return Type::boolean();
        return infer(node->kids.empty() ? nullptr : node->kids[0], env);
    }
    case Node::Kind::Cast: return node->declared ? node->declared : Type::i32();
    case Node::Kind::Sequence:
        return infer(node->kids.empty() ? nullptr : node->kids.back(), env);
    case Node::Kind::Declare: return node->declared ? node->declared : Type::i32();
    default: return Type::i32();
    }
}

std::string emitExpr(const NodePtr &node, Emitter &emitter);
void emitStmt(const NodePtr &node, Emitter &emitter);

/** lambda：抽成独立函数，返回数据区里的闭包句柄。 */
std::string emitLambda(const NodePtr &node, Emitter &emitter)
{
    const std::string name = "lambda_" + std::to_string(++emitter.lambdaIndex);
    const std::vector<std::string> saved = std::move(emitter.lines);
    emitter.lines.clear();
    if (!node->kids.empty()) emitStmt(node->kids[0], emitter);
    emitter.emit("(ret, 0, _, _)");
    std::ostringstream body;
    body << "{ " << name;
    for (const std::string &line : emitter.lines) body << " | " << line;
    body << " }";
    emitter.lines = saved;
    emitter.data(name, body.str(), Type::i32());
    return name;
}

/** 运算符重载：查复合类型的 operators 表，找到就脱糖成方法调用。 */
std::string overloadTarget(const TypePtr &type, const std::string &op)
{
    if (!type) return std::string();
    for (const auto &item : type->operators) if (item.first == op) return item.second;
    return std::string();
}

std::string emitExpr(const NodePtr &node, Emitter &emitter)
{
    if (!node) return "0";
    const std::string literal = literalText(node);
    if (!literal.empty()) return literal;

    switch (node->kind)
    {
    case Node::Kind::Sequence:
        if (node->kids.empty()) return "0";
        for (std::size_t index = 0; index + 1u < node->kids.size(); ++index)
            emitStmt(node->kids[index], emitter);
        return emitExpr(node->kids.back(), emitter);
    case Node::Kind::StringLit:
    {
        /* 具名的（`namedStr`）沿用给的名字 → 同名只有一份数据项；普通的每次新开一个。 */
        const std::string name = node->typeName.empty()
                                     ? ("str_" + std::to_string(++emitter.labelIndex))
                                     : node->typeName;
        emitter.data(name, "\"" + node->text + "\"", Type::stringType());
        // 字符串在数据区里是一串以 `\0` 结尾的字节；表达式的值是它的**基址**
        // （i64 句柄），这样才能放进变量、当实参传出去、再按地址逐字节取。
        const std::string address = emitter.temp();
        emitter.emit("(addr, " + name + ", _, " + address + ")");
        return address;
    }
    case Node::Kind::Var:
        if (emitter.env.find(node->text) == emitter.env.end())
            emitter.error("引用了未声明的变量：" + node->text);
        return node->text;

    case Node::Kind::Binary:
    {
        const NodePtr leftNode = node->kids.size() > 0u ? node->kids[0] : nullptr;
        const NodePtr rightNode = node->kids.size() > 1u ? node->kids[1] : nullptr;
        const TypePtr leftType = infer(leftNode, emitter.env);
        const TypePtr rightType = infer(rightNode, emitter.env);
        const std::string op = node->text;
        const bool compare = (op == "==" || op == "!=" || op == "<" || op == "<=" ||
                              op == ">" || op == ">=");

        // 1. 引用只允许判空，不做指针运算。
        if ((leftType->kind == Kind::Ref || rightType->kind == Kind::Ref) &&
            !(compare && (op == "==" || op == "!=")))
        {
            emitter.error("引用只支持 == / !=（可空判断），不做指针运算：运算符 " + op);
        }

        // 2. 运算符重载：左操作数是复合类型且声明了该运算符 → `a.add(b)`。
        const std::string method = overloadTarget(leftType, op);
        if (!method.empty())
        {
            const std::string self = emitExpr(leftNode, emitter);
            const std::string other = emitExpr(rightNode, emitter);
            const std::string target = emitter.temp();
            emitter.emit("(param, " + self + ", _, self)");
            emitter.emit("(param, " + other + ", _, other)");
            emitter.emit("(call, " + leftType->name + "." + method + ", _, " + target + ")");
            return target;
        }

        // 3. 逻辑短路：右侧表达式必须放在条件跳转之后生成，否则函数调用、
        // 越界检查等副作用会在 `false && rhs` / `true || rhs` 中错误发生。
        if (op == "&&" || op == "||")
        {
            const std::string left = emitExpr(leftNode, emitter);
            const std::string target = emitter.temp();
            const std::string done = emitter.label(op == "&&" ? "and_done" : "or_done");
            const bool conjunction = (op == "&&");
            emitter.emit("(:=, " + std::string(conjunction ? "0" : "1") + ", _, " + target + ")");
            emitter.emit("(" + std::string(conjunction ? "jz" : "jnz") + ", " + left +
                         ", _, " + done + ")");
            const std::string right = emitExpr(rightNode, emitter);
            emitter.emit("(!=:i32, " + right + ", 0, " + target + ")");
            emitter.emit("(label, _, _, " + done + ")");
            return target;
        }

        // 4. 其余内建运算。
        const std::string left = emitExpr(leftNode, emitter);
        const std::string right = emitExpr(rightNode, emitter);
        const std::string target = emitter.temp();
        TypePtr valueType = compare ? Type::i32() : leftType;
        if (!compare && (leftType->kind == Kind::Float || rightType->kind == Kind::Float))
        {
            const int leftBits = leftType->kind == Kind::Float ? leftType->bits : 0;
            const int rightBits = rightType->kind == Kind::Float ? rightType->bits : 0;
            valueType = Type::make(Kind::Float, std::max(leftBits, rightBits));
        }
        else if (!compare &&
                 (leftType->kind == Kind::Int || leftType->kind == Kind::Uint) &&
                 (rightType->kind == Kind::Int || rightType->kind == Kind::Uint))
        {
            const Kind kind = (leftType->kind == Kind::Uint || rightType->kind == Kind::Uint)
                                  ? Kind::Uint : Kind::Int;
            valueType = Type::make(kind, std::max(leftType->bits, rightType->bits));
        }
        emitter.emit("(" + op + ":" + valueType->quadType() + ", " + left + ", " + right + ", " +
                     target + ")");
        return target;
    }

    case Node::Kind::Unary:
    {
        const NodePtr operandNode = node->kids.empty() ? nullptr : node->kids[0];
        const std::string operand = emitExpr(operandNode, emitter);
        const std::string target = emitter.temp();
        if (node->text == "!") emitter.emit("(==:i32, " + operand + ", 0, " + target + ")");
        // 一元负号走专门的 `neg`（只有一个操作数）：写成 `(-:i32, x, _, t)` 会被当成二元减法，
        // 第二个操作数缺失，两个后端都会报"缺少操作数"。
        else if (node->text == "-")
            emitter.emit("(neg:" + infer(operandNode, emitter.env)->quadType() + ", " + operand +
                         ", _, " + target + ")");
        else emitter.emit("(" + node->text + ":" + infer(operandNode, emitter.env)->quadType() +
                          ", " + operand + ", _, " + target + ")");
        return target;
    }

    case Node::Kind::Cast:
    {
        // 强转就是 `cvt`：位宽 / 符号 / 浮点由目标类型决定（栈机与 transfer 都实现它）。
        const NodePtr source = node->kids.empty() ? nullptr : node->kids[0];
        const std::string value = emitExpr(source, emitter);
        const TypePtr targetType = node->declared ? node->declared : Type::i32();
        const std::string target = emitter.temp();
        emitter.emit("(cvt:" + targetType->quadType() + ", " + value + ", _, " + target + ")");
        return target;
    }

    case Node::Kind::Index:
    {
        const NodePtr baseNode = node->kids.size() > 0u ? node->kids[0] : nullptr;
        const std::string base = emitExpr(baseNode, emitter);
        const std::string position = emitExpr(node->kids.size() > 1u ? node->kids[1] : nullptr,
                                             emitter);
        // 降级成"基址 + 偏移"：每个元素一个 8 字节槽位 → 先 位置×8，再加基址。
        const std::string scaled = emitter.temp();
        emitter.emit("(*:i64, " + position + ", 8, " + scaled + ")");
        const std::string address = emitter.temp();
        emitter.emit("(+:i64, " + base + ", " + scaled + ", " + address + ")");
        // rvalue：算出地址后再取一次值（本 AST 里赋值目标只能是变量名，所以下标总是 rvalue）。
        // `load` 带上**元素类型**：数据区的槽位是 8 字节，整型按整数解释、浮点按
        // IEEE-754 双精度解释（两个后端都认这个后缀）。
        const TypePtr container = infer(baseNode, emitter.env);
        const TypePtr element = container->element ? container->element : Type::i32();
        const std::string value = emitter.temp();
        emitter.emit("(load:" + element->quadType() + ", " + address + ", _, " + value + ")");
        return value;
    }

    case Node::Kind::Member:
    {
        const NodePtr baseNode = node->kids.empty() ? nullptr : node->kids[0];
        const std::string base = emitExpr(baseNode, emitter);
        // 结构体按同样的 8 字节槽位扁平化：偏移 = 字段序号 × 8（顺序即声明顺序）。
        const TypePtr baseType = infer(baseNode, emitter.env);
        std::size_t fieldIndex = 0u;
        for (std::size_t index = 0; index < baseType->fields.size(); ++index)
        {
            if (baseType->fields[index].first == node->text) { fieldIndex = index; break; }
        }
        const std::string target = emitter.temp();
        emitter.emit("(+:i64, " + base + ", " + std::to_string(fieldIndex * 8u) + ", " + target + ")");
        // 成员访问同样是 rvalue：取该槽位的值。
        const TypePtr fieldType = (fieldIndex < baseType->fields.size())
                                      ? baseType->fields[fieldIndex].second
                                      : Type::i32();
        const std::string value = emitter.temp();
        emitter.emit("(load:" + fieldType->quadType() + ", " + target + ", _, " + value + ")");
        return value;
    }

    case Node::Kind::Offset:
    {
        // 按自定义步长算地址，**不取值**：结果是"元素句柄"（结构体数组用）
        const std::string base = emitExpr(node->kids.size() > 0u ? node->kids[0] : nullptr, emitter);
        const std::string position = emitExpr(node->kids.size() > 1u ? node->kids[1] : nullptr,
                                             emitter);
        const std::string scaled = emitter.temp();
        emitter.emit("(*:i64, " + position + ", " + node->text + ", " + scaled + ")");
        const std::string address = emitter.temp();
        emitter.emit("(+:i64, " + base + ", " + scaled + ", " + address + ")");
        return address;
    }

    case Node::Kind::ArrayNew:
    {
        const std::string name = "arr_" + std::to_string(++emitter.labelIndex);
        std::ostringstream body;
        body << "[";
        // 数据条目的元素类型：`arrayNew` 带了类型就用它（浮点数组要写对描述），
        // 老写法（只给 meta 字符串）退回 i32。
        const TypePtr arrayType = node->declared ? node->declared
                                                 : Type::array(Type::i32(), node->text);
        const TypePtr elementType = arrayType->element;
        for (std::size_t index = 0; index < node->kids.size(); ++index)
        {
            if (index != 0u) body << ", ";
            const std::string item = literalTextFor(node->kids[index], elementType);
            body << (item.empty() ? emitExpr(node->kids[index], emitter) : item);
        }
        body << "]";
        emitter.data(name, body.str(), arrayType);
        // 复合值在四元式里是"地址"：先取数据区里该条目的基址，再返回它。
        const std::string address = emitter.temp();
        emitter.emit("(addr, " + name + ", _, " + address + ")");
        return address;
    }

    case Node::Kind::Alloc:
    {
        // 堆分配：字节数先求值，`(alloc, 字节数, _, t)` 把地址放进 t，表达式的值就是 t。
        const std::string bytes = emitExpr(node->kids.empty() ? nullptr : node->kids[0], emitter);
        const std::string address = emitter.temp();
        emitter.emit("(alloc, " + bytes + ", _, " + address + ")");
        return address;
    }

    case Node::Kind::LoadInt:
    {
        // 按地址读 8 字节：默认按整数读（运行时数组的长度就是这么读的）；带了
        // `declared` 就按那个类型读（`loadFrom`：字段地址 + 字段类型）。
        const std::string address = emitExpr(node->kids.empty() ? nullptr : node->kids[0], emitter);
        const std::string value = emitter.temp();
        const TypePtr type = node->declared ? node->declared : Type::i64();
        emitter.emit("(load:" + type->quadType() + ", " + address + ", _, " + value + ")");
        return value;
    }

    case Node::Kind::CheckIndex:
    {
        // 下标陷阱：表达式值 = 下标本身，所以能直接当 `index()` / `offset()` 的位置参数用。
        const std::string index = emitExpr(node->kids.empty() ? nullptr : node->kids[0], emitter);
        const std::string limit = emitExpr(node->kids.size() > 1u ? node->kids[1] : nullptr, emitter);
        emitter.emit("(check, " + index + ", " + limit + ", " + node->text + ")");
        return index;
    }

    case Node::Kind::CheckPointer:
    {
        const std::string index = emitExpr(node->kids.empty() ? nullptr : node->kids[0], emitter);
        const std::string base = emitExpr(node->kids.size() > 1u ? node->kids[1] : nullptr, emitter);
        emitter.emit("(checkptr, " + index + ", " + base + ", " +
                     std::to_string(node->intValue) + ", " + node->text + ")");
        return index;
    }

    case Node::Kind::StructNew:
    {
        const std::string name = "obj_" + std::to_string(++emitter.labelIndex);
        std::ostringstream body;
        body << "{";
        /* 字段类型从类型环境里取（结构体类型在 generate 之前就登记进 env 了），
         * 这样浮点字段里的整数写法也能补成小数点。 */
        const TypePtr selfType = [&] {
            const auto found = emitter.env.find(node->text);
            return found == emitter.env.end() ? TypePtr() : found->second;
        }();
        for (std::size_t index = 0; index < node->kids.size(); ++index)
        {
            if (index != 0u) body << ", ";
            const TypePtr fieldType =
                (selfType && index < selfType->fields.size()) ? selfType->fields[index].second
                                                              : TypePtr();
            const std::string item = literalTextFor(node->kids[index], fieldType);
            body << (item.empty() ? emitExpr(node->kids[index], emitter) : item);
        }
        body << "}";
        emitter.data(name, body.str(), selfType ? selfType : Type::structOf(node->text, {}));
        const std::string address = emitter.temp();
        emitter.emit("(addr, " + name + ", _, " + address + ")");
        return address;
    }

    case Node::Kind::EnumNew:
    {
        const std::string name = "enum_" + std::to_string(++emitter.labelIndex);
        // 保留类型名：数据条目要用真实的枚举类型（含分支表），二进制里才能写对 tag。
        const auto declared = emitter.env.find(node->typeName);
        const TypePtr type = (declared == emitter.env.end())
                                 ? Type::enumOf(node->typeName, {})
                                 : declared->second;
        std::ostringstream body;
        body << node->text << "(";
        for (std::size_t index = 0; index < node->kids.size(); ++index)
        {
            if (index != 0u) body << ", ";
            body << emitExpr(node->kids[index], emitter);
        }
        body << ")";
        emitter.data(name, body.str(), type);
        const std::string address = emitter.temp();
        emitter.emit("(addr, " + name + ", _, " + address + ")");
        return address;
    }

    case Node::Kind::Call:
    {
        const std::string target = emitter.temp();
        // 先把实参**全部求值**，再发 `param`：`param` 是"挂起参数表"，要是在求值
        // 过程中又碰到一次调用（`f(g(x))`），内层 call 会把外层已经挂上的参数一起
        // 消耗掉 —— 先算完再挂就不会串。
        std::vector<std::string> values;
        for (const NodePtr &argument : node->kids) values.push_back(emitExpr(argument, emitter));
        // 形参名：显式给了就逐个用（`param` 是按名字绑定的），没给就沿用旧的 `arg`。
        for (std::size_t index = 0; index < values.size(); ++index)
        {
            const std::string parameter = (index < node->names.size() && !node->names[index].empty())
                                              ? node->names[index]
                                              : "arg";
            emitter.emit("(param, " + values[index] + ", _, " + parameter + ")");
        }
        emitter.emit("(call, " + node->text + ", _, " + target + ")");
        return target;
    }

    case Node::Kind::CallPointer:
    {
        // 函数指针调用：目标（那个地址）与实参都先求值，再挂 param，最后 `callptr`。
        const NodePtr targetNode = node->kids.empty() ? nullptr : node->kids[0];
        const std::string pointer = emitExpr(targetNode, emitter);
        std::vector<std::string> values;
        for (std::size_t index = 1; index < node->kids.size(); ++index)
            values.push_back(emitExpr(node->kids[index], emitter));
        for (std::size_t index = 0; index < values.size(); ++index)
        {
            const std::string parameter = (index < node->names.size() && !node->names[index].empty())
                                              ? node->names[index]
                                              : "arg";
            emitter.emit("(param, " + values[index] + ", _, " + parameter + ")");
        }
        const std::string result = emitter.temp();
        emitter.emit("(callptr, " + pointer + ", _, " + result + ")");
        return result;
    }

    case Node::Kind::MethodCall:
    {
        const NodePtr object = node->kids.empty() ? nullptr : node->kids[0];
        const std::string target = emitter.temp();
        // 同上：接收者与实参先全部求值，再依次挂 `param`。
        const std::string self = emitExpr(object, emitter);
        std::vector<std::string> values;
        for (std::size_t index = 1; index < node->kids.size(); ++index)
            values.push_back(emitExpr(node->kids[index], emitter));
        emitter.emit("(param, " + self + ", _, self)");
        for (const std::string &value : values)
        {
            emitter.emit("(param, " + value + ", _, arg)");
        }
        const std::string owner = infer(object, emitter.env)->name;
        emitter.emit("(call, " + owner + "." + node->text + ", _, " + target + ")");
        return target;
    }

    case Node::Kind::Lambda: return emitLambda(node, emitter);
    default: break;
    }
    return "0";
}

/**
 * @brief 算一个左值（下标 / 成员）的**地址**；`a[i] = v`、`p.x = v` 用它。
 *
 * 与 `emitExpr` 里 rvalue 的地址计算逐字一致：下标 = 基址 + 位置×8，
 * 成员 = 基址 + 字段序号×8，只是不再补最后那次 `load`。
 */
std::string emitAddress(const NodePtr &node, Emitter &emitter)
{
    if (!node) return "0";
    if (node->kind == Node::Kind::Index)
    {
        const std::string base = emitExpr(node->kids.size() > 0u ? node->kids[0] : nullptr, emitter);
        const std::string position = emitExpr(node->kids.size() > 1u ? node->kids[1] : nullptr,
                                             emitter);
        const std::string scaled = emitter.temp();
        emitter.emit("(*:i64, " + position + ", 8, " + scaled + ")");
        const std::string address = emitter.temp();
        emitter.emit("(+:i64, " + base + ", " + scaled + ", " + address + ")");
        return address;
    }
    if (node->kind == Node::Kind::Offset)
    {
        // 本身就是地址计算，左值用它时直接复用同一条路径
        const std::string base = emitExpr(node->kids.size() > 0u ? node->kids[0] : nullptr, emitter);
        const std::string position = emitExpr(node->kids.size() > 1u ? node->kids[1] : nullptr,
                                             emitter);
        const std::string scaled = emitter.temp();
        emitter.emit("(*:i64, " + position + ", " + node->text + ", " + scaled + ")");
        const std::string address = emitter.temp();
        emitter.emit("(+:i64, " + base + ", " + scaled + ", " + address + ")");
        return address;
    }
    if (node->kind == Node::Kind::Member)
    {
        const NodePtr baseNode = node->kids.empty() ? nullptr : node->kids[0];
        const std::string base = emitExpr(baseNode, emitter);
        const TypePtr baseType = infer(baseNode, emitter.env);
        std::size_t fieldIndex = 0u;
        for (std::size_t index = 0; index < baseType->fields.size(); ++index)
        {
            if (baseType->fields[index].first == node->text) { fieldIndex = index; break; }
        }
        const std::string address = emitter.temp();
        emitter.emit("(+:i64, " + base + ", " + std::to_string(fieldIndex * 8u) + ", " + address + ")");
        return address;
    }
    emitter.error("赋值目标只能是变量、下标或成员");
    return "0";
}

void emitStmt(const NodePtr &node, Emitter &emitter)
{
    if (!node) return;
    switch (node->kind)
    {
    case Node::Kind::Block:
        for (const NodePtr &statement : node->kids) emitStmt(statement, emitter);
        return;
    case Node::Kind::ExprStmt:
        (void)emitExpr(node->kids.empty() ? nullptr : node->kids[0], emitter);
        return;
    case Node::Kind::Assign:
        if (node->kids.size() >= 2u)   // 左值是节点（下标 / 成员）：先算地址再写
        {
            const NodePtr target = node->kids[0];
            const std::string value = emitExpr(node->kids[1], emitter);
            if (target && target->kind == Node::Kind::Var)
            {
                emitter.emit("(:=, " + value + ", _, " + target->text + ")");
                return;
            }
            // `store` 带上**元素类型**：数据区槽位是 8 字节，浮点要按 IEEE-754 写成
            // 位模式（整数路径会把 1.5 截成 1，读回来就不是浮点了）。
            const std::string storedType = infer(target, emitter.env)->quadType();
            emitter.emit("(store:" + storedType + ", " + emitAddress(target, emitter) + ", " +
                         value + ", _)");
            return;
        }
        emitter.emit("(:=, " + emitExpr(node->kids.empty() ? nullptr : node->kids[0], emitter) +
                     ", _, " + node->text + ")");
        return;
    case Node::Kind::Declare:
    {
        // 带类型的局部变量：先登记类型，再发一条 `(decl, T, local, name)`。
        const TypePtr type = node->declared ? node->declared : Type::i32();
        emitter.env[node->text] = type;
        emitter.emit("(decl, " + type->quadType() + ", local, " + node->text + ")");
        return;
    }
    case Node::Kind::If:
    {
        const std::string otherwise = emitter.label("else");
        const std::string done = emitter.label("endif");
        emitter.emit("(jz, " + emitExpr(node->kids.empty() ? nullptr : node->kids[0], emitter) +
                     ", _, " + otherwise + ")");
        emitStmt(node->kids.size() > 1u ? node->kids[1] : nullptr, emitter);
        emitter.emit("(j, _, _, " + done + ")");
        emitter.emit("(label, _, _, " + otherwise + ")");
        emitStmt(node->kids.size() > 2u ? node->kids[2] : nullptr, emitter);
        emitter.emit("(label, _, _, " + done + ")");
        return;
    }
    case Node::Kind::While:
    {
        const std::string loop = emitter.label("while");
        const std::string done = emitter.label("endwhile");
        emitter.emit("(label, _, _, " + loop + ")");
        emitter.emit("(jz, " + emitExpr(node->kids.empty() ? nullptr : node->kids[0], emitter) +
                     ", _, " + done + ")");
        emitStmt(node->kids.size() > 1u ? node->kids[1] : nullptr, emitter);
        emitter.emit("(j, _, _, " + loop + ")");
        emitter.emit("(label, _, _, " + done + ")");
        return;
    }
    case Node::Kind::DoWhile:
    {
        // do body while (cond)：先做一遍 body，再在末尾判条件往回跳
        const std::string loop = emitter.label("do");
        emitter.emit("(label, _, _, " + loop + ")");
        emitStmt(node->kids.empty() ? nullptr : node->kids[0], emitter);
        emitter.emit("(jnz, " + emitExpr(node->kids.size() > 1u ? node->kids[1] : nullptr, emitter) +
                     ", _, " + loop + ")");
        return;
    }
    case Node::Kind::Label:
        emitter.emit("(label, _, _, " + node->text + ")");
        return;
    case Node::Kind::Jump:
        emitter.emit("(j, _, _, " + node->text + ")");
        return;
    case Node::Kind::Return:
        emitter.emit("(ret, " + emitExpr(node->kids.empty() ? nullptr : node->kids[0], emitter) +
                     ", _, _)");
        return;
    case Node::Kind::Write:
        emitter.emit("(write, " + emitExpr(node->kids.empty() ? nullptr : node->kids[0], emitter) +
                     ", _, _)");
        return;
    case Node::Kind::Free:
        emitter.emit("(free, " + emitExpr(node->kids.empty() ? nullptr : node->kids[0], emitter) +
                     ", _, _)");
        return;
    case Node::Kind::StoreAt:
    {
        // 地址自己算好的时候写值：`(store:T, 地址, 值, _)`（浮点按位写，整数按整数写）。
        //
        // **值的表达式先求**，地址后求：表达式临时量在本 IR 里是普通变量名（没有
        // "栈帧局部"这回事），递归时同一段代码的临时量会被内层覆盖 —— 先算地址、
        // 再调用（值里含递归调用）的话，回来时那个地址已经被内层改掉了，就会写到
        // 别处去（`node.left = rebuild(...)` 这种写法踩过）。
        const std::string value = emitExpr(node->kids.size() > 1u ? node->kids[1] : nullptr, emitter);
        const std::string address = emitExpr(node->kids.empty() ? nullptr : node->kids[0], emitter);
        emitter.emit("(store:" + node->text + ", " + address + ", " + value + ", _)");
        return;
    }
    case Node::Kind::Drop:
        // 软释放：地址不是活跃堆块（0 / 数据区 / 已经还过）就什么都不做。
        emitter.emit("(drop, " + emitExpr(node->kids.empty() ? nullptr : node->kids[0], emitter) +
                     ", _, _)");
        return;
    case Node::Kind::Read:
        // `(read, _, _, 变量)`：按顺序从输入取一项，按变量声明的类型收进去。
        emitter.emit("(read, _, _, " + node->text + ")");
        return;
    case Node::Kind::Print:
    {
        // 文本里的换行 / 引号 / 反斜杠按四元式的转义规则写出来（transfer 的 read() 认这些）。
        std::string escaped;
        for (const char ch : node->text)
        {
            if (ch == '\n') escaped += "\\n";
            else if (ch == '\t') escaped += "\\t";
            else if (ch == '"') escaped += "\\\"";
            else if (ch == '\\') escaped += "\\\\";
            else escaped.push_back(ch);
        }
        emitter.emit("(print, \"" + escaped + "\", _, _)");
        return;
    }
    case Node::Kind::Putc:
    {
        // `(putc, v, _, _)`：把 v 的低 8 位当字符码输出（数值转文本的最小手段）。
        emitter.emit("(putc, " + emitExpr(node->kids.empty() ? nullptr : node->kids[0], emitter) +
                     ", _, _)");
        return;
    }
    default:
        (void)emitExpr(node, emitter);
        return;
    }
}

} // namespace

// ---- 类型 -----------------------------------------------------------------

TypePtr Type::make(Kind kind, int bits)
{
    auto type = std::make_shared<Type>();
    type->kind = kind;
    type->bits = bits;
    return type;
}

TypePtr Type::i8() { return make(Kind::Int, 8); }
TypePtr Type::i16() { return make(Kind::Int, 16); }
TypePtr Type::i32() { return make(Kind::Int, 32); }
TypePtr Type::i64() { return make(Kind::Int, 64); }
TypePtr Type::u8() { return make(Kind::Uint, 8); }
TypePtr Type::u16() { return make(Kind::Uint, 16); }
TypePtr Type::u32() { return make(Kind::Uint, 32); }
TypePtr Type::u64() { return make(Kind::Uint, 64); }
TypePtr Type::f32() { return make(Kind::Float, 32); }
TypePtr Type::f64() { return make(Kind::Float, 64); }
TypePtr Type::boolean() { return make(Kind::Bool, 1); }
TypePtr Type::stringType() { return make(Kind::String, 0); }

TypePtr Type::array(TypePtr element, std::string meta)
{
    auto type = make(Kind::Array);
    type->element = std::move(element);
    type->meta = std::move(meta);
    return type;
}

TypePtr Type::ref(TypePtr target)
{
    auto type = make(Kind::Ref);
    type->element = std::move(target);
    type->meta = "nullable";
    return type;
}

TypePtr Type::structOf(std::string name, std::vector<std::pair<std::string, TypePtr>> fields)
{
    auto type = make(Kind::Struct);
    type->name = std::move(name);
    type->fields = std::move(fields);
    type->meta = "value";
    return type;
}

TypePtr Type::enumOf(std::string name, std::vector<std::pair<std::string, TypePtr>> variants)
{
    auto type = make(Kind::Enum);
    type->name = std::move(name);
    type->variants = std::move(variants);
    type->meta = "tagged";
    return type;
}

TypePtr Type::generic(std::string name)
{
    auto type = make(Kind::Generic);
    type->name = std::move(name);
    type->meta = "generic";
    return type;
}

std::string Type::quadType() const
{
    switch (kind)
    {
    case Kind::Bool: return "i8";
    case Kind::Int: return "i" + std::to_string(bits);
    case Kind::Uint: return "u" + std::to_string(bits);
    case Kind::Float: return "f" + std::to_string(bits);
    default: return "i64"; // 字符串 / 数组 / 引用 / 结构体 / 枚举 / 泛型：i64 句柄
    }
}

std::string Type::describe() const
{
    std::string text;
    switch (kind)
    {
    case Kind::Bool: text = "bool"; break;
    case Kind::Int: text = "i" + std::to_string(bits); break;
    case Kind::Uint: text = "u" + std::to_string(bits); break;
    case Kind::Float: text = "f" + std::to_string(bits); break;
    case Kind::String: text = "string"; break;
    case Kind::Generic: text = "generic<" + name + ">"; break;
    case Kind::Array: text = "array<" + (element ? element->describe() : "?") + ">"; break;
    case Kind::Ref: text = "ref<" + (element ? element->describe() : "?") + (meta == "nonnull" ? ">" : ">?"); break;
    case Kind::Struct:
        text = "struct " + name + " {";
        for (std::size_t index = 0; index < fields.size(); ++index)
        {
            if (index != 0u) text += ", ";
            text += fields[index].first + ": " + fields[index].second->describe();
        }
        if (!methods.empty())
        {
            text += " | methods:";
            for (const std::string &method : methods) text += " " + method;
        }
        if (!operators.empty())
        {
            text += " | operators:";
            for (const auto &item : operators) text += " " + item.first + "→" + item.second;
        }
        text += "}";
        break;
    case Kind::Enum:
        text = "enum " + name + " {";
        for (std::size_t index = 0; index < variants.size(); ++index)
        {
            if (index != 0u) text += ", ";
            text += variants[index].first;
            if (variants[index].second) text += "(" + variants[index].second->describe() + ")";
        }
        text += "}";
        break;
    default: text = "void"; break;
    }
    if (!meta.empty()) text += "{" + meta + "}";
    return text;
}

// ---- 节点 -----------------------------------------------------------------

NodePtr Node::i(std::int64_t value)
{
    auto node = std::make_shared<Node>();
    node->kind = Kind::IntLit;
    node->intValue = value;
    return node;
}

NodePtr Node::f(double value)
{
    auto node = std::make_shared<Node>();
    node->kind = Kind::FloatLit;
    node->floatValue = value;
    return node;
}

NodePtr Node::str(std::string text)
{
    auto node = std::make_shared<Node>();
    node->kind = Kind::StringLit;
    node->text = std::move(text);
    return node;
}

NodePtr Node::boolean(bool value)
{
    auto node = std::make_shared<Node>();
    node->kind = Kind::BoolLit;
    node->intValue = value ? 1 : 0;
    return node;
}

NodePtr Node::null()
{
    auto node = std::make_shared<Node>();
    node->kind = Kind::NullLit;
    return node;
}

NodePtr Node::var(std::string name)
{
    auto node = std::make_shared<Node>();
    node->kind = Kind::Var;
    node->text = std::move(name);
    return node;
}

NodePtr Node::binary(std::string op, NodePtr left, NodePtr right)
{
    auto node = std::make_shared<Node>();
    node->kind = Kind::Binary;
    node->text = std::move(op);
    node->kids = {std::move(left), std::move(right)};
    return node;
}

NodePtr Node::sequence(std::vector<NodePtr> statements, NodePtr value)
{
    NodePtr node = std::make_shared<Node>();
    node->kind = Kind::Sequence;
    node->kids = std::move(statements);
    node->kids.push_back(std::move(value));
    return node;
}

NodePtr Node::unary(std::string op, NodePtr operand)
{
    auto node = std::make_shared<Node>();
    node->kind = Kind::Unary;
    node->text = std::move(op);
    node->kids = {std::move(operand)};
    return node;
}

NodePtr Node::index(NodePtr target, NodePtr position)
{
    auto node = std::make_shared<Node>();
    node->kind = Kind::Index;
    node->kids = {std::move(target), std::move(position)};
    return node;
}

NodePtr Node::offset(NodePtr base, NodePtr position, std::int64_t stride)
{
    auto node = std::make_shared<Node>();
    node->kind = Kind::Offset;
    node->text = std::to_string(stride);
    node->kids = {std::move(base), std::move(position)};
    return node;
}

NodePtr Node::member(NodePtr target, std::string field)
{
    auto node = std::make_shared<Node>();
    node->kind = Kind::Member;
    node->text = std::move(field);
    node->kids = {std::move(target)};
    return node;
}

NodePtr Node::call(std::string callee, std::vector<NodePtr> args)
{
    auto node = std::make_shared<Node>();
    node->kind = Kind::Call;
    node->text = std::move(callee);
    node->kids = std::move(args);
    return node;
}

NodePtr Node::call(std::string callee, std::vector<std::string> parameterNames,
                   std::vector<NodePtr> args)
{
    NodePtr node = call(std::move(callee), std::move(args));
    node->names = std::move(parameterNames);
    return node;
}

NodePtr Node::methodCall(NodePtr object, std::string method, std::vector<NodePtr> args)
{
    auto node = std::make_shared<Node>();
    node->kind = Kind::MethodCall;
    node->text = std::move(method);
    node->kids.push_back(std::move(object));
    for (NodePtr &argument : args) node->kids.push_back(std::move(argument));
    return node;
}

NodePtr Node::lambda(NodePtr body)
{
    auto node = std::make_shared<Node>();
    node->kind = Kind::Lambda;
    if (body) node->kids = {std::move(body)};
    return node;
}

NodePtr Node::arrayNew(TypePtr type, std::vector<NodePtr> items)
{
    auto node = std::make_shared<Node>();
    node->kind = Kind::ArrayNew;
    node->text = type ? type->meta : std::string();
    node->declared = type;   // 元素类型进 .data 的描述（浮点数组也要写对）
    node->kids = std::move(items);
    return node;
}

NodePtr Node::structNew(std::string typeName, std::vector<NodePtr> fields)
{
    auto node = std::make_shared<Node>();
    node->kind = Kind::StructNew;
    node->text = std::move(typeName);
    node->kids = std::move(fields);
    return node;
}

NodePtr Node::enumNew(std::string typeName, std::string variant, std::vector<NodePtr> payload)
{
    auto node = std::make_shared<Node>();
    node->kind = Kind::EnumNew;
    node->text = std::move(variant);
    node->typeName = std::move(typeName); // 保留类型名（二进制 tag / 类型推断都要用）
    node->kids = std::move(payload);
    return node;
}

NodePtr Node::alloc(NodePtr bytes)
{
    auto node = std::make_shared<Node>();
    node->kind = Kind::Alloc;
    if (bytes) node->kids = {std::move(bytes)};
    return node;
}

NodePtr Node::loadFrom(NodePtr address, TypePtr type)
{
    auto node = std::make_shared<Node>();
    node->kind = Kind::LoadInt;
    node->declared = type ? std::move(type) : Type::i64();
    if (address) node->kids = {std::move(address)};
    return node;
}

NodePtr Node::storeAt(NodePtr address, NodePtr value, TypePtr type)
{
    auto node = std::make_shared<Node>();
    node->kind = Kind::StoreAt;
    node->text = (type ? type : Type::i64())->quadType();   // 写的时候按这个类型写
    if (address) node->kids.push_back(std::move(address));
    if (value) node->kids.push_back(std::move(value));
    return node;
}

NodePtr Node::release(NodePtr pointer)
{
    auto node = std::make_shared<Node>();
    node->kind = Kind::Drop;
    node->kids.push_back(std::move(pointer));
    return node;
}

NodePtr Node::freeMemory(NodePtr pointer)
{
    auto node = std::make_shared<Node>();
    node->kind = Kind::Free;
    if (pointer) node->kids = {std::move(pointer)};
    return node;
}

NodePtr Node::callPointer(NodePtr target, std::vector<std::string> parameterNames,
                           std::vector<NodePtr> args)
{
    auto node = std::make_shared<Node>();
    node->kind = Kind::CallPointer;
    if (target) node->kids.push_back(std::move(target));
    for (NodePtr &argument : args) node->kids.push_back(std::move(argument));
    node->names = std::move(parameterNames);
    return node;
}

NodePtr Node::namedStr(std::string name, std::string text)
{
    auto node = std::make_shared<Node>();
    node->kind = Kind::StringLit;
    node->text = std::move(text);
    node->typeName = std::move(name);   // 数据项名字（普通字符串字面量这里为空）
    return node;
}

NodePtr Node::loadInt(NodePtr address)
{
    auto node = std::make_shared<Node>();
    node->kind = Kind::LoadInt;
    if (address) node->kids = {std::move(address)};
    return node;
}

NodePtr Node::checkIndex(NodePtr index, NodePtr limit, std::string label)
{
    auto node = std::make_shared<Node>();
    node->kind = Kind::CheckIndex;
    node->text = std::move(label);
    if (index) node->kids.push_back(std::move(index));
    node->kids.push_back(std::move(limit));
    return node;
}

NodePtr Node::checkPointer(NodePtr index, NodePtr base, std::int64_t stride, std::string label)
{
    auto node = std::make_shared<Node>();
    node->kind = Kind::CheckPointer;
    node->text = std::move(label);
    node->intValue = stride;
    if (index) node->kids.push_back(std::move(index));
    node->kids.push_back(std::move(base));
    return node;
}

NodePtr Node::assign(std::string target, NodePtr value)
{
    auto node = std::make_shared<Node>();
    node->kind = Kind::Assign;
    node->text = std::move(target);
    node->kids = {std::move(value)};
    return node;
}

NodePtr Node::assign(NodePtr target, NodePtr value)
{
    auto node = std::make_shared<Node>();
    node->kind = Kind::Assign;
    node->kids = {std::move(target), std::move(value)};
    return node;
}

NodePtr Node::declare(std::string name, TypePtr type)
{
    auto node = std::make_shared<Node>();
    node->kind = Kind::Declare;
    node->text = std::move(name);
    node->declared = std::move(type);
    return node;
}

NodePtr Node::cast(NodePtr value, TypePtr type)
{
    auto node = std::make_shared<Node>();
    node->kind = Kind::Cast;
    node->declared = std::move(type);
    node->kids = {std::move(value)};
    return node;
}

NodePtr Node::ifThenElse(NodePtr condition, NodePtr thenBranch, NodePtr elseBranch)
{
    auto node = std::make_shared<Node>();
    node->kind = Kind::If;
    node->kids = {std::move(condition), std::move(thenBranch), std::move(elseBranch)};
    return node;
}

NodePtr Node::whileLoop(NodePtr condition, NodePtr body)
{
    auto node = std::make_shared<Node>();
    node->kind = Kind::While;
    node->kids = {std::move(condition), std::move(body)};
    return node;
}

NodePtr Node::doWhileLoop(NodePtr body, NodePtr condition)
{
    auto node = std::make_shared<Node>();
    node->kind = Kind::DoWhile;
    node->kids = {std::move(body), std::move(condition)};
    return node;
}

NodePtr Node::label(std::string name)
{
    auto node = std::make_shared<Node>();
    node->kind = Kind::Label;
    node->text = std::move(name);
    return node;
}

NodePtr Node::jump(std::string target)
{
    auto node = std::make_shared<Node>();
    node->kind = Kind::Jump;
    node->text = std::move(target);
    return node;
}

NodePtr Node::ret(NodePtr value)
{
    auto node = std::make_shared<Node>();
    node->kind = Kind::Return;
    if (value) node->kids = {std::move(value)};
    return node;
}

NodePtr Node::exprStmt(NodePtr expression)
{
    auto node = std::make_shared<Node>();
    node->kind = Kind::ExprStmt;
    node->kids = {std::move(expression)};
    return node;
}

NodePtr Node::write(NodePtr value)
{
    auto node = std::make_shared<Node>();
    node->kind = Kind::Write;
    if (value) node->kids = {std::move(value)};
    return node;
}

NodePtr Node::block(std::vector<NodePtr> statements)
{
    auto node = std::make_shared<Node>();
    node->kind = Kind::Block;
    node->kids = std::move(statements);
    return node;
}

NodePtr Node::print(std::string text)
{
    auto node = std::make_shared<Node>();
    node->kind = Kind::Print;
    node->text = std::move(text);
    return node;
}

NodePtr Node::putc(NodePtr value)
{
    auto node = std::make_shared<Node>();
    node->kind = Kind::Putc;
    node->kids = {std::move(value)};
    return node;
}

NodePtr Node::read(std::string variable)
{
    auto node = std::make_shared<Node>();
    node->kind = Kind::Read;
    node->text = std::move(variable);
    return node;
}

// ---- 程序与生成 -----------------------------------------------------------

void Program::addConstant(const std::string &name, TypePtr type, const std::string &literal)
{
    auto stored = std::make_shared<Type>(*type);
    stored->meta = literal; // 常量值暂存在 meta 里，生成时直接展开
    constants.emplace_back(name, stored);
    variables.emplace_back(name, type);
}

void Program::addVariable(const std::string &name, TypePtr type)
{
    variables.emplace_back(name, type);
}

void Program::addMethod(const std::string &typeName, Function function)
{
    function.owner = typeName;
    functions.push_back(std::move(function));
    for (TypePtr &type : types)
    {
        if (type->name == typeName) type->methods.push_back(functions.back().name);
    }
}

void Program::addOperator(const std::string &typeName, const std::string &op,
                          const std::string &methodName)
{
    for (TypePtr &type : types)
    {
        if (type->name == typeName) type->operators.emplace_back(op, methodName);
    }
}

CodegenResult generate(const Program &program, const std::string &moduleName)
{
    Emitter emitter;

    std::ostringstream quads;
    quads << "# module: " << moduleName << "\n";
    for (const TypePtr &type : program.types) quads << "# type " << type->describe() << "\n";
    for (const TypePtr &type : program.types) emitter.env[type->name] = type;
    // 函数 / 方法的返回类型也进 env：推断 `f(...)` 与 `obj.m(...)` 的结果类型用。
    for (const Function &function : program.functions)
    {
        emitter.env[function.quadLabel()] = function.result ? function.result : Type::i32();
        emitter.env[function.name] = function.result ? function.result : Type::i32();
    }
    for (const auto &item : program.constants)
    {
        emitter.env[item.first] = item.second;
        quads << "# const " << item.first << " : " << item.second->meta << "\n";
    }
    for (const auto &item : program.variables) emitter.env[item.first] = item.second;

    std::size_t quadIndex = 0;
    auto flush = [&](std::vector<std::string> &lines) {
        for (const std::string &line : lines) quads << quadIndex++ << ": " << line << "\n";
        lines.clear();
    };

    // 1. 声明 + 常量初始化。
    for (const auto &item : program.variables)
    {
        emitter.emit("(decl, " + item.second->quadType() + ", global, " + item.first + ")");
    }
    flush(emitter.lines);
    for (const auto &item : program.constants)
    {
        emitter.emit("(:=, " + (item.second->meta.empty() ? std::string("0") : item.second->meta) +
                     ", _, " + item.first + ")");
    }
    flush(emitter.lines);

    // 2. 函数与方法：方法标签是 `Type.method`，隐式形参 `self` 由调用方 param 进来。
    //
    // 顺序很关键：虚拟机从第 0 条指令开始执行，所以先把入口 `main` 调到最前面——
    // 先发 `(call, main, _, _)` 引导，再排 main 的函数体，其余函数 / 方法跟在后面。
    // （否则方法体排在前面会被当成程序起点，读没绑定的 `self` 直接报错。）
    std::vector<const Function *> ordered;
    for (const Function &function : program.functions)
    {
        if (function.name == "main") ordered.insert(ordered.begin(), &function);
        else ordered.push_back(&function);
    }
    const bool hasMain = !ordered.empty() && ordered.front()->name == "main";
    if (hasMain)
    {
        // 引导：调用 main；`main` 返回后必须立刻停机，否则 pc 会落回函数体再跑一遍。
        quads << quadIndex++ << ": (call, main, _, _)\n";
        quads << quadIndex++ << ": (halt, _, _, _)\n";
    }

    for (const Function *entry : ordered)
    {
        const Function &function = *entry;
        for (const auto &parameter : function.parameters)
        {
            emitter.env[parameter.first] = parameter.second;
            emitter.emit("(decl, " + parameter.second->quadType() + ", local, " + parameter.first + ")");
        }
        if (!function.owner.empty())
        {
            emitter.emit("(decl, i64, local, self)");
            const auto owner = emitter.env.find(function.owner);
            emitter.env["self"] = (owner == emitter.env.end()) ? Type::i64() : owner->second;
        }
        emitter.emit("(label, _, _, " + function.quadLabel() + ")");
        emitStmt(function.body, emitter);
        emitter.emit("(ret, 0, _, _)");
        flush(emitter.lines);
    }

    quads << "(halt, _, _, _)\n";
    emitter.result.quads = quads.str();
    emitter.result.data = "# simple_ast data: " + moduleName + "\n";
    for (const std::string &line : emitter.dataLines) emitter.result.data += line + "\n";
    return emitter.result;
}

namespace
{

/**
 * @brief 把数据条目铺成二进制；可选输出布局表。
 * @param entries 条目。
 * @param layout 非空时写入 `名字 offset=… size=… type=…`。
 * @return 二进制内容。
 */
std::string buildDataBinary(const std::vector<CodegenResult::Entry> &entries,
                            std::vector<std::string> *layout)
{
    // 1. 句柄表：数组元素里出现字符串 / 复合对象时写的是"条目序号"。
    std::map<std::string, std::uint32_t> handles;
    for (std::size_t index = 0; index < entries.size(); ++index)
    {
        handles[entries[index].name] = static_cast<std::uint32_t>(index);
    }

    std::string out;
    // **地址 0 留给"空指针"**：数据区从偏移 8 开始排，这样任何真实对象的地址都不为 0，
    // `p == null` / 函数指针判空才有意义（堆那边本来也是从数据区之后开始长的）。
    out.assign(8u, '\0');
    for (const CodegenResult::Entry &entry : entries)
    {
        // 2. 对齐（`align=` 缺省 8 字节）。
        const int align = metaInt(entry.type->meta, "align", 8);
        while ((out.size() % static_cast<std::size_t>(align > 0 ? align : 1)) != 0u) out.push_back('\0');
        const std::size_t start = out.size();

        /**
         * 写一个值：字符串 → 字节 + `\0`；数字 → 8 字节；已知标签 → 4 字节句柄。
         */
        const std::function<void(const std::string &)> writeValue = [&](const std::string &token) {
            std::string text = token;
            while (!text.empty() && (text.front() == ' ')) text.erase(text.begin());
            while (!text.empty() && (text.back() == ' ')) text.pop_back();
            if (isQuoted(text))
            {
                out += text.substr(1u, text.size() - 2u);
                out.push_back('\0');
                return;
            }
            bool isFloat = false;
            std::int64_t intValue = 0;
            double doubleValue = 0.0;
            if (numberText(text, isFloat, intValue, doubleValue))
            {
                if (isFloat) putDouble(out, doubleValue);
                else putInt(out, intValue);
                return;
            }
            const auto found = handles.find(text);
            putU32(out, found == handles.end() ? 0u : found->second);
        };

        // 3. 按类型写内容。
        if (entry.type->kind == Type::Kind::String)
        {
            writeValue(entry.body);
        }
        else if (entry.type->kind == Type::Kind::Array || entry.type->kind == Type::Kind::Struct)
        {
            for (const std::string &item : splitTop(entry.body)) writeValue(item);
        }
        else if (entry.type->kind == Type::Kind::Enum)
        {
            // 枚举：4 字节 tag（分支序号，按类型的 variants 顺序）+ 载荷。
            const std::size_t open = entry.body.find('(');
            const std::string variant =
                (open == std::string::npos) ? entry.body : entry.body.substr(0u, open);
            std::uint32_t tag = 0u;
            for (std::size_t index = 0; index < entry.type->variants.size(); ++index)
            {
                if (entry.type->variants[index].first == variant) tag = static_cast<std::uint32_t>(index);
            }
            putU32(out, tag);
            if (open != std::string::npos)
            {
                const std::size_t close = entry.body.rfind(')');
                const std::string payload = entry.body.substr(open + 1u, close - open - 1u);
                for (const std::string &item : splitTop(payload)) writeValue(item);
            }
        }
        else
        {
            writeValue(entry.body);
        }

        if (layout != nullptr)
        {
            // 带上 stride=8：数据区里每个槽位固定 8 字节，和四元式里 `(*:i64, index, 8, t)`
            // 用的步长一致；stackvm 的 DataArea 会把它读进来备查。
            layout->push_back(entry.name + " offset=" + std::to_string(start) + " size=" +
                              std::to_string(out.size() - start) + " stride=8 type=" +
                              entry.type->describe());
        }
    }
    // 5. 尾部补 8 个零字节：数据区是按地址按 8 字节整块读的（`load`），
    //    最后一个条目末尾也要有"可读满 8 字节"的余量，逐字节扫字符串才不会越界。
    while ((out.size() % 8u) != 0u) out.push_back('\0');
    out.append(8u, '\0');
    return out;
}

} // namespace

std::string CodegenResult::exportDataBinary() const
{
    return buildDataBinary(entries, nullptr);
}

std::vector<std::string> CodegenResult::exportDataLayout() const
{
    std::vector<std::string> layout;
    (void)buildDataBinary(entries, &layout);
    return layout;
}

std::string CodegenResult::exportDataHeader(const std::string &moduleName) const
{
    const std::string binary = buildDataBinary(entries, nullptr);
    std::ostringstream header;
    header << "/* " << moduleName << " 的 .data 二进制（小端）。由 simple_ast 生成。 */\n";
    header << "#ifndef SIMPLE_AST_DATA_" << moduleName << "_H\n";
    header << "#define SIMPLE_AST_DATA_" << moduleName << "_H\n\n";
    header << "#include <stdint.h>\n\n";
    for (const std::string &line : exportDataLayout())
    {
        const std::size_t space = line.find(' ');
        const std::string name = line.substr(0u, space);
        const std::size_t offsetAt = line.find("offset=");
        header << "#define DATA_" << name << "_OFFSET "
               << line.substr(offsetAt + 7u, line.find(' ', offsetAt) - offsetAt - 7u) << "u\n";
    }
    header << "\nstatic const unsigned char " << moduleName << "_data[] = {";
    for (std::size_t index = 0; index < binary.size(); ++index)
    {
        if (index % 12u == 0u) header << "\n    ";
        header << static_cast<int>(static_cast<unsigned char>(binary[index])) << ",";
    }
    header << "\n};\n\n#endif\n";
    return header.str();
}

} // namespace simple_ast
