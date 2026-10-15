#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

/**
 * 四元式后端的类型与数值层。
 *
 * 类型表固定为 12 种数值类型：i8/u8、i16/u16、i32/u32、i64/u64、i128/u128
 * 与 f32/f64。整数用「按位宽截断 + 二补码」表示，无符号/有符号只影响
 * 乘除、右移、比较和打印的解释；浮点按 IEEE-754 求值（f32 每步舍入到
 * float）。没有声明类型的变量沿用 i16，保证旧的 16 位教学程序语义不变。
 *
 * 128 位整数的加减乘除、移位和比较由本文件自带的可移植实现完成，不依赖
 * 编译器的 `__int128` 扩展，因此在 MSVC / GCC / Clang 上行为一致。
 */
namespace transfer {

/** 类型种类：有符号 / 无符号整数、浮点、字符串。 */
enum class TypeKind { Signed, Unsigned, Float, String };

/** 一种数值类型：名称、种类和位宽。 */
struct TypeInfo {
    std::string name;
    TypeKind kind = TypeKind::Signed;
    unsigned bits = 16;
};

/** 内建类型表（顺序固定，便于文档与测试引用）。 */
const std::vector<TypeInfo>& types();
/**
 * 名称是否是内建类型（大小写不敏感，前后空白忽略）。
 */
bool isTypeName(const std::string& name);
/**
 * 查询类型；未知名称抛出 std::runtime_error。
 */
TypeInfo typeInfo(const std::string& name);
/** 未声明类型时的默认类型名。 */
const char* defaultTypeName();
/** 该类型是否按浮点表示。 */
bool isFloatType(const std::string& name);
/** 该类型是否无符号。 */
bool isUnsignedType(const std::string& name);
/** 该类型是否是字符串（`str`）。 */
bool isStringType(const std::string& name);
/** 类型的位宽；未知名称抛出异常。 */
unsigned typeBits(const std::string& name);

// ---------------------------------------------------------------------------
// 可移植 128 位无符号整数（lo 为低 64 位，hi 为高 64 位）
// ---------------------------------------------------------------------------

struct U128 {
    std::uint64_t lo = 0;
    std::uint64_t hi = 0;
};

/**
 * 由 64 位整数构造 128 位值（高半清零）。。
 *
 * @param low 低 64 位
 * @return 128 位值
 */
U128 u128FromU64(std::uint64_t value);
/**
 * 判断 128 位值是否为零。。
 *
 * @param value 待判断的值
 * @return 全 0 返回 true
 */
bool u128IsZero(const U128& value);
/**
 * 比较两个 128 位值是否相等。。
 *
 * @param a 左值
 * @param b 右值
 * @return 相等返回 true
 */
bool u128Equals(const U128& a, const U128& b);
/**
 * 无符号比较：a<b 返回负数，a==b 返回 0，a>b 返回正数。
 */
int u128Compare(const U128& a, const U128& b);
/** 有符号比较：两个值需先按位宽符号扩展。 */
int u128CompareSigned(const U128& a, const U128& b);
/**
 * 无符号 128 位加法（进位丢弃，按位宽回绕）。。
 *
 * @param a 左值
 * @param b 右值
 * @return a+b 的低 128 位
 */
U128 u128Add(const U128& a, const U128& b);
/**
 * 无符号 128 位减法（借位丢弃）。。
 *
 * @param a 左值
 * @param b 右值
 * @return a-b 的低 128 位
 */
U128 u128Sub(const U128& a, const U128& b);
/**
 * 无符号 128 位乘法（取低 128 位；超出部分丢弃）。。
 *
 * @param a 左值
 * @param b 右值
 * @return 积的低 128 位
 */
U128 u128Mul(const U128& a, const U128& b);
/**
 * 128 位按位与。。
 *
 * @param a 左值
 * @param b 右值
 * @return 逐位与的结果
 */
U128 u128And(const U128& a, const U128& b);
/**
 * 128 位按位或。。
 *
 * @param a 左值
 * @param b 右值
 * @return 逐位或的结果
 */
U128 u128Or(const U128& a, const U128& b);
/**
 * 128 位按位异或。。
 *
 * @param a 左值
 * @param b 右值
 * @return 逐位异或的结果
 */
U128 u128Xor(const U128& a, const U128& b);
/**
 * 128 位按位取反。。
 *
 * @param value 操作数
 * @return 逐位取反的结果
 */
U128 u128Not(const U128& value);
/** 左移；移位量 >= 128 时结果为 0。 */
U128 u128Shl(const U128& value, unsigned amount);
/** 逻辑右移；移位量 >= 128 时结果为 0。 */
U128 u128Shr(const U128& value, unsigned amount);
/** 无符号除法与取余；除数为 0 时抛出异常。 */
void u128DivMod(const U128& a, const U128& b, U128& quotient, U128& remainder);
/** 十进制输出。 */
std::string u128ToString(const U128& value);
/** 解析十进制（只接受数字，溢出返回 false）。 */
bool u128FromString(const std::string& text, U128& value);
/**
 * 近似转换成 double（超出 2^53 后按 IEEE 舍入）。
 */
double u128ToDouble(const U128& value);
/**
 * 截断转换；要求 0 <= value < 2^128。
 */
U128 u128FromDouble(double value);
/**
 * 保留低 bits 位；bits 为 1..128。
 */
U128 u128Mask(const U128& value, unsigned bits);
/**
 * 按给定符号位宽做符号扩展，得到 128 位二补码模式。
 */
U128 u128SignExtend(const U128& value, unsigned bits);
/** 带位宽的无符号最小值比较。 */
U128 u128MinUnsigned(unsigned bits);

// ---------------------------------------------------------------------------
// 类型化数值
// ---------------------------------------------------------------------------

/**
 * 一个带类型的数值。整数类型时 `bits` 保存位宽内的二补码模式（不做符号
 * 扩展），`real` 不使用；浮点类型时 `real` 保存值（f32 已舍入到单精度）。
 */
struct Value {
    std::string type = "i16";
    U128 bits{};
    double real = 0.0;
    /** `str` 类型的文本内容；其他类型不使用。 */
    std::string text;
    /** 该值是否按浮点解释。 */
    bool isFloat() const;
    /** 该值是否是字符串。 */
    bool isString() const;
};

/** 字面量是否匹配整数写法（可带正负号）。 */
bool isIntegerLiteral(const std::string& text);
/**
 * 字面量是否匹配浮点写法（小数点或指数至少出现一个）。
 */
bool isFloatLiteral(const std::string& text);
/** 整数或浮点字面量。 */
bool isNumericLiteral(const std::string& text);
/**
 * 字符串字面量：形如 `"..."`（引号内支持 \n \t \r \\ \" 转义）。
 */
bool isStringLiteral(const std::string& text);
/**
 * 去掉两端引号并做转义还原；不是字符串字面量时原样返回。
 */
std::string stringLiteralText(const std::string& text);

/**
 * 按类型解析字面量并做范围检查；失败抛出 std::runtime_error。
 */
Value literalValue(const std::string& text, const std::string& type);
/** 用位模式构造整数类型的值（按类型位宽截断）。 */
Value valueFromBits(const U128& bits, const std::string& type);
/**
 * 取整数位模式；浮点值返回截断后的整数位模式（供分片/存储使用）。
 */
U128 valueBits(const Value& value);
/**
 * 把值转换成目标类型（整数按位宽截断，浮点/整数互转按 C 语义）。
 */
Value convertValue(const Value& value, const std::string& type);
/** 值的十进制显示文本（浮点用最短可读表示）。 */
std::string displayValue(const Value& value);
/**
 * 值按 `int8_t` 语义截断的低 8 位（putc / getc 用）。
 */
std::uint32_t valueLowByte(const Value& value);
/** 值是否为 0（用于 jz / 折叠判断）。 */
bool valueIsZero(const Value& value);
/** 生成布尔结果（0 或 1）并按目标类型表示。 */
Value boolValue(bool value, const std::string& type);

/**
 * 按给定类型求值一个运算符，返回带类型的结果。
 *
 * `base` 是去掉类型后缀的运算符（`+ - * / % << >> & | ^ ~ neg odd == != < <= > >= cvt`）；
 * `type` 是运算类型（也是结果类型）；`cvt` 的 `type` 是转换目标类型。
 * 比较和 `odd` 的结果类型固定为 i8。除零、除溢出、浮点定义域错误会抛出
 * std::runtime_error。
 */
Value evalTyped(const std::string& base, const std::string& type, const Value& lhs,
                const Value& rhs);

// ---------------------------------------------------------------------------
// 字符串指令
// ---------------------------------------------------------------------------

/**
 * 字符串值（按字节比较 / 计数；UTF-8 文本按字节处理）。
 */
const std::string& stringValue(const Value& value);
/**
 * `slen`：字节长度，结果按 resultType 表示。
 */
Value stringLength(const Value& value, const std::string& resultType);
/**
 * `scmp`：字典序比较，返回 -1 / 0 / 1（按 resultType 表示）。
 */
Value stringCompare(const Value& lhs, const Value& rhs, const std::string& resultType);
/** `scat`：拼接。 */
Value stringConcat(const Value& lhs, const Value& rhs);
/**
 * `ssub`：从 start 取 length 个字节（越界自动截断）。
 */
Value substring(const Value& value, int start, int length);
/**
 * `sfind`：子串首次出现的位置，找不到返回 -1。
 */
Value stringFind(const Value& haystack, const Value& needle, const std::string& resultType);

/**
 * 进位加法：`result = a + b + carryIn`，并把进位输出写到 `carryOut`。
 * 只有整数类型有意义；用于多字长加法链（进位加法）。普通 `+` 不读也不写进位。
 */
Value addWithCarry(const std::string& type, const Value& lhs, const Value& rhs, bool carryIn,
                   bool& carryOut);
/**
 * 借位减法：`result = a - b - borrowIn`，并把借位输出写到 `borrowOut`。
 * 用于多字长减法链；普通 `-` 不读也不写借位。
 */
Value subWithBorrow(const std::string& type, const Value& lhs, const Value& rhs, bool borrowIn,
                    bool& borrowOut);
/**
 * 双寄存器乘法：把 `a * b` 的 2N 位结果拆成高半 `hi` 与低半 `lo`（各 N 位）。
 * 只支持位宽不超过 64 的整数类型（乘积恰好放进 128 位），否则抛出异常。
 */
void multiplyWide(const std::string& type, const Value& lhs, const Value& rhs, Value& hi,
                  Value& lo);
/**
 * 双寄存器除法：`quotient = a / b`、`remainder = a % b`，两者类型都是 `type`。
 * 只有整数类型有意义；除零与有符号溢出抛出异常。
 */
void divideWide(const std::string& type, const Value& lhs, const Value& rhs, Value& quotient,
                Value& remainder);

/**
 * 拆出 `*:u32` 的基名（无后缀时返回原串）。
 */
std::string opBase(const std::string& op);
/**
 * 拆出 `*:u32` 的类型后缀；没有后缀时返回空串。
 */
std::string opType(const std::string& op);
/**
 * 组合基名与类型后缀；type 为空或默认 i16 时返回基名。
 */
std::string typedOp(const std::string& base, const std::string& type);

} // namespace transfer