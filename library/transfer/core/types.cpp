#include "types.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace transfer {
namespace {

[[noreturn]] void fail(const std::string& message) { throw std::runtime_error(message); }

std::string lower(std::string text) {
    for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

std::string trimmed(const std::string& text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}

/** 64x64 -> 高 64 位的乘法，用 32 位分段实现，避免依赖 __int128。 */
std::uint64_t mulHigh(std::uint64_t a, std::uint64_t b) {
    const std::uint64_t a1 = a >> 32, a0 = a & 0xFFFFFFFFull;
    const std::uint64_t b1 = b >> 32, b0 = b & 0xFFFFFFFFull;
    const std::uint64_t t0 = a0 * b0;
    const std::uint64_t t1 = a1 * b0 + (t0 >> 32);
    const std::uint64_t t2 = a0 * b1 + (t1 & 0xFFFFFFFFull);
    return a1 * b1 + (t1 >> 32) + (t2 >> 32);
}

U128 maskOf(unsigned bits) {
    if (bits >= 128) return U128{~0ull, ~0ull};
    U128 value;
    if (bits >= 64) {
        value.lo = ~0ull;
        value.hi = bits == 64 ? 0 : ((1ull << (bits - 64)) - 1ull);
    } else {
        value.lo = (1ull << bits) - 1ull;
    }
    return value;
}

int signOf(const U128& twoComplement) { return (twoComplement.hi >> 63) != 0 ? -1 : 1; }

/** 把「符号 + 绝对值」变成位宽内的二补码模式。 */
U128 fromMagnitude(const U128& magnitude, bool negative, unsigned bits) {
    if (!negative) return u128Mask(magnitude, bits);
    const U128 minusOne = u128Sub(magnitude, U128{1, 0});
    return u128Mask(u128Sub(maskOf(bits), minusOne), bits);
}

/** 取整数值的绝对值（按源码类型解释），用于有符号乘除。 */
U128 signedMagnitude(const Value& value) {
    const unsigned bits = typeBits(value.type);
    const U128 extended = u128SignExtend(value.bits, bits);
    return signOf(extended) < 0 ? u128Sub(U128{0, 0}, extended) : extended;
}

double valueAsDouble(const Value& value) {
    if (isFloatType(value.type)) return value.real;
    const unsigned bits = typeBits(value.type);
    if (isUnsignedType(value.type)) return u128ToDouble(u128Mask(value.bits, bits));
    const U128 extended = u128SignExtend(value.bits, bits);
    if (signOf(extended) < 0) return -u128ToDouble(u128Sub(U128{0, 0}, extended));
    return u128ToDouble(extended);
}

Value makeFloat(double value, const std::string& type) {
    Value result;
    result.type = typeInfo(type).name;
    result.real = typeInfo(type).bits == 32 ? static_cast<double>(static_cast<float>(value)) : value;
    return result;
}

Value makeInteger(const U128& bits, const std::string& type) {
    Value result;
    result.type = typeInfo(type).name;
    result.bits = u128Mask(bits, typeBits(result.type));
    return result;
}

Value makeSignedInteger(const U128& magnitude, bool negative, const std::string& type) {
    return makeInteger(fromMagnitude(magnitude, negative, typeBits(type)), type);
}

int compareSigned(const Value& lhs, const Value& rhs) {
    const unsigned bits = typeBits(lhs.type);
    return u128CompareSigned(u128SignExtend(lhs.bits, bits), u128SignExtend(rhs.bits, bits));
}

int compareUnsigned(const Value& lhs, const Value& rhs) {
    return u128Compare(u128Mask(lhs.bits, typeBits(lhs.type)),
                       u128Mask(rhs.bits, typeBits(rhs.type)));
}

std::string formatFloat(double value, unsigned bits) {
    if (std::isnan(value)) return "nan";
    if (std::isinf(value)) return value < 0 ? "-inf" : "inf";
    std::ostringstream out;
    out << std::setprecision(bits == 32 ? 7 : 15) << std::defaultfloat << value;
    std::string text = out.str();
    if (text.find('.') != std::string::npos && text.find('e') == std::string::npos &&
        text.find('E') == std::string::npos) {
        while (text.size() > 1 && text.back() == '0' && text[text.size() - 2] != '.') text.pop_back();
        if (!text.empty() && text.back() == '.') text.pop_back();
    }
    return text;
}

/** 把任意值转换为运算类型下的整数值（浮点截断，整数按位宽重解释）。 */
Value asInteger(const Value& value, const std::string& type) {
    const TypeInfo target = typeInfo(type);
    if (target.kind == TypeKind::Float) fail("internal: expected integer type");
    if (isFloatType(value.type)) {
        const double real = std::trunc(value.real);
        if (!std::isfinite(real)) fail("Cannot convert non-finite float to " + target.name);
        if (target.kind == TypeKind::Unsigned) {
            if (real < 0.0 || real >= std::ldexp(1.0, static_cast<int>(target.bits))) {
                fail("Float out of range for " + target.name);
            }
            return makeInteger(u128FromDouble(real), target.name);
        }
        const double limit = std::ldexp(1.0, static_cast<int>(target.bits) - 1);
        if (real < -limit || real >= limit) fail("Float out of range for " + target.name);
        const bool negative = real < 0.0;
        return makeSignedInteger(u128FromDouble(std::fabs(real)), negative, target.name);
    }
    U128 pattern = value.bits;
    if (!isUnsignedType(value.type)) pattern = u128SignExtend(pattern, typeBits(value.type));
    return makeInteger(pattern, target.name);
}

char digitToChar(unsigned digit) { return static_cast<char>('0' + digit); }

} // namespace

const std::vector<TypeInfo>& types() {
    static const std::vector<TypeInfo> table{
        {"i8", TypeKind::Signed, 8},   {"u8", TypeKind::Unsigned, 8},
        {"i16", TypeKind::Signed, 16}, {"u16", TypeKind::Unsigned, 16},
        {"i32", TypeKind::Signed, 32}, {"u32", TypeKind::Unsigned, 32},
        {"i64", TypeKind::Signed, 64}, {"u64", TypeKind::Unsigned, 64},
        {"i128", TypeKind::Signed, 128}, {"u128", TypeKind::Unsigned, 128},
        {"f32", TypeKind::Float, 32},  {"f64", TypeKind::Float, 64},
        {"str", TypeKind::String, 0}};
    return table;
}

bool isTypeName(const std::string& name) {
    const auto wanted = lower(trimmed(name));
    const auto& all = types();
    return std::any_of(all.begin(), all.end(),
                       [&](const TypeInfo& info) { return info.name == wanted; });
}

TypeInfo typeInfo(const std::string& name) {
    const auto wanted = lower(trimmed(name));
    const auto& all = types();
    const auto found = std::find_if(all.begin(), all.end(),
                                    [&](const TypeInfo& info) { return info.name == wanted; });
    if (found == all.end()) fail("Unknown type: " + name);
    return *found;
}

const char* defaultTypeName() { return "i16"; }

bool isFloatType(const std::string& name) { return typeInfo(name).kind == TypeKind::Float; }

bool isUnsignedType(const std::string& name) { return typeInfo(name).kind == TypeKind::Unsigned; }

bool isStringType(const std::string& name) { return typeInfo(name).kind == TypeKind::String; }

unsigned typeBits(const std::string& name) { return typeInfo(name).bits; }

U128 u128FromU64(std::uint64_t value) { return U128{value, 0}; }

bool u128IsZero(const U128& value) { return value.lo == 0 && value.hi == 0; }

bool u128Equals(const U128& a, const U128& b) { return a.lo == b.lo && a.hi == b.hi; }

int u128Compare(const U128& a, const U128& b) {
    if (a.hi != b.hi) return a.hi < b.hi ? -1 : 1;
    if (a.lo != b.lo) return a.lo < b.lo ? -1 : 1;
    return 0;
}

int u128CompareSigned(const U128& a, const U128& b) {
    const bool negativeA = (a.hi >> 63) != 0, negativeB = (b.hi >> 63) != 0;
    if (negativeA != negativeB) return negativeA ? -1 : 1;
    return u128Compare(a, b);
}

U128 u128Add(const U128& a, const U128& b) {
    U128 result;
    result.lo = a.lo + b.lo;
    result.hi = a.hi + b.hi + (result.lo < a.lo ? 1ull : 0ull);
    return result;
}

U128 u128Sub(const U128& a, const U128& b) {
    U128 result;
    result.lo = a.lo - b.lo;
    result.hi = a.hi - b.hi - (a.lo < b.lo ? 1ull : 0ull);
    return result;
}

U128 u128Mul(const U128& a, const U128& b) {
    U128 result;
    result.lo = a.lo * b.lo;
    result.hi = mulHigh(a.lo, b.lo) + a.lo * b.hi + a.hi * b.lo;
    return result;
}

U128 u128And(const U128& a, const U128& b) { return U128{a.lo & b.lo, a.hi & b.hi}; }
U128 u128Or(const U128& a, const U128& b) { return U128{a.lo | b.lo, a.hi | b.hi}; }
U128 u128Xor(const U128& a, const U128& b) { return U128{a.lo ^ b.lo, a.hi ^ b.hi}; }
U128 u128Not(const U128& value) { return U128{~value.lo, ~value.hi}; }

U128 u128Shl(const U128& value, unsigned amount) {
    if (amount == 0) return value;
    if (amount >= 128) return U128{};
    U128 result;
    if (amount < 64) {
        result.hi = (value.hi << amount) | (value.lo >> (64 - amount));
        result.lo = value.lo << amount;
    } else {
        result.hi = value.lo << (amount - 64);
    }
    return result;
}

U128 u128Shr(const U128& value, unsigned amount) {
    if (amount == 0) return value;
    if (amount >= 128) return U128{};
    U128 result;
    if (amount < 64) {
        result.lo = (value.lo >> amount) | (value.hi << (64 - amount));
        result.hi = value.hi >> amount;
    } else {
        result.lo = value.hi >> (amount - 64);
    }
    return result;
}

void u128DivMod(const U128& a, const U128& b, U128& quotient, U128& remainder) {
    if (u128IsZero(b)) fail("Division by zero");
    quotient = U128{};
    remainder = U128{};
    for (int bit = 127; bit >= 0; --bit) {
        remainder = u128Shl(remainder, 1);
        const std::uint64_t next = bit >= 64 ? ((a.hi >> (bit - 64)) & 1ull) : ((a.lo >> bit) & 1ull);
        remainder.lo |= next;
        if (u128Compare(remainder, b) >= 0) {
            remainder = u128Sub(remainder, b);
            if (bit >= 64) quotient.hi |= (1ull << (bit - 64));
            else quotient.lo |= (1ull << bit);
        }
    }
}

std::string u128ToString(const U128& value) {
    if (u128IsZero(value)) return "0";
    std::string digits;
    U128 current = value;
    const U128 ten{10, 0};
    while (!u128IsZero(current)) {
        U128 quotient, remainder;
        u128DivMod(current, ten, quotient, remainder);
        digits.push_back(digitToChar(static_cast<unsigned>(remainder.lo)));
        current = quotient;
    }
    std::reverse(digits.begin(), digits.end());
    return digits;
}

bool u128FromString(const std::string& text, U128& value) {
    if (text.empty()) return false;
    const U128 limit{0x1999999999999999ull, 0x1999999999999999ull}; // (2^128-1)/10
    const U128 ten{10, 0};
    U128 current;
    for (char c : text) {
        if (c < '0' || c > '9') return false;
        if (u128Compare(current, limit) > 0) return false;
        const U128 scaled = u128Mul(current, ten);
        const U128 digit{c == '0' ? 0ull : static_cast<std::uint64_t>(c - '0'), 0};
        if (u128Compare(scaled, u128Not(digit)) > 0) return false;
        current = u128Add(scaled, digit);
    }
    value = current;
    return true;
}

double u128ToDouble(const U128& value) {
    return static_cast<double>(value.hi) * 18446744073709551616.0 + static_cast<double>(value.lo);
}

U128 u128FromDouble(double value) {
    if (!(value > 0.0)) return U128{};
    const double two64 = 18446744073709551616.0;
    if (value >= two64 * two64) return U128{~0ull, ~0ull};
    U128 result;
    result.hi = static_cast<std::uint64_t>(value / two64);
    result.lo = static_cast<std::uint64_t>(value - static_cast<double>(result.hi) * two64);
    return result;
}

U128 u128Mask(const U128& value, unsigned bits) {
    if (bits >= 128) return value;
    return u128And(value, maskOf(bits));
}

U128 u128SignExtend(const U128& value, unsigned bits) {
    if (bits == 0 || bits >= 128) return value;
    const U128 masked = u128Mask(value, bits);
    // 符号位在 lo 里（bits <= 64）还是 hi 里（bits > 64）要分开取。
    const bool negative = bits <= 64 ? ((masked.lo >> (bits - 1)) & 1ull) != 0
                                     : ((masked.hi >> (bits - 65)) & 1ull) != 0;
    if (!negative) return masked;
    return u128Or(masked, u128Not(maskOf(bits)));
}

U128 u128MinUnsigned(unsigned bits) { return maskOf(bits); }

bool Value::isFloat() const { return isFloatType(type); }

bool Value::isString() const { return isStringType(type); }

bool isIntegerLiteral(const std::string& text) {
    const std::string value = trimmed(text);
    if (value.empty()) return false;
    std::size_t i = (value[0] == '+' || value[0] == '-') ? 1 : 0;
    if (i == value.size()) return false;
    for (; i < value.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(value[i]))) return false;
    }
    return true;
}

bool isFloatLiteral(const std::string& text) {
    const std::string value = trimmed(text);
    if (value.empty()) return false;
    std::size_t i = (value[0] == '+' || value[0] == '-') ? 1 : 0;
    bool digit = false, dot = false, exponent = false;
    for (; i < value.size(); ++i) {
        const char c = value[i];
        if (std::isdigit(static_cast<unsigned char>(c))) {
            digit = true;
        } else if (c == '.') {
            if (dot || exponent) return false;
            dot = true;
        } else if (c == 'e' || c == 'E') {
            if (exponent || !digit) return false;
            exponent = true;
            digit = false;
            if (i + 1 < value.size() && (value[i + 1] == '+' || value[i + 1] == '-')) ++i;
        } else {
            return false;
        }
    }
    if (!digit) return false;
    return dot || exponent;
}

bool isNumericLiteral(const std::string& text) {
    return isIntegerLiteral(text) || isFloatLiteral(text);
}

bool isStringLiteral(const std::string& text) {
    const std::string value = trimmed(text);
    if (value.size() < 2 || value.front() != '"' || value.back() != '"') return false;
    // 引号内允许 \" 转义。
    for (std::size_t i = 1; i + 1 < value.size(); ++i) {
        if (value[i] == '\\') ++i;
        else if (value[i] == '"') return false;
    }
    return true;
}

std::string stringLiteralText(const std::string& text) {
    if (!isStringLiteral(text)) return text;
    const std::string inner = text.substr(1, text.size() - 2);
    std::string out;
    for (std::size_t i = 0; i < inner.size(); ++i) {
        if (inner[i] == '\\' && i + 1 < inner.size()) {
            const char next = inner[++i];
            if (next == 'n') out.push_back('\n');
            else if (next == 't') out.push_back('\t');
            else if (next == 'r') out.push_back('\r');
            else out.push_back(next);
        } else {
            out.push_back(inner[i]);
        }
    }
    return out;
}

Value literalValue(const std::string& text, const std::string& type) {
    const TypeInfo info = typeInfo(type);
    const std::string value = trimmed(text);
    if (info.kind == TypeKind::String) {
        Value result;
        result.type = info.name;
        result.text = stringLiteralText(value);
        return result;
    }
    if (info.kind == TypeKind::Float) {
        if (!isNumericLiteral(value)) fail("Expected numeric literal: " + text);
        double parsed = 0.0;
        try {
            std::size_t used = 0;
            parsed = std::stod(value, &used);
            if (used != value.size()) fail("Invalid float literal: " + text);
        } catch (const std::runtime_error&) {
            fail("Invalid float literal: " + text);
        }
        return makeFloat(parsed, info.name);
    }
    if (!isIntegerLiteral(value)) fail("Expected integer literal for " + info.name + ": " + text);
    const bool negative = value[0] == '-';
    const std::string digits = (value[0] == '+' || value[0] == '-') ? value.substr(1) : value;
    U128 magnitude;
    if (!u128FromString(digits, magnitude)) fail("Integer literal out of range: " + text);
    if (info.kind == TypeKind::Unsigned) {
        if (negative && !u128IsZero(magnitude)) fail("Negative literal for unsigned type: " + text);
        if (u128Compare(magnitude, maskOf(info.bits)) > 0) {
            fail("Literal out of range for " + info.name + ": " + text);
        }
        return makeInteger(magnitude, info.name);
    }
    const U128 maximum = maskOf(info.bits - 1); // 2^(bits-1) - 1
    if (negative) {
        const U128 limit = u128Add(maximum, U128{1, 0}); // 2^(bits-1)
        if (u128Compare(magnitude, limit) > 0) fail("Literal out of range for " + info.name + ": " + text);
    } else if (u128Compare(magnitude, maximum) > 0) {
        fail("Literal out of range for " + info.name + ": " + text);
    }
    return makeSignedInteger(magnitude, negative && !u128IsZero(magnitude), info.name);
}

Value convertValue(const Value& value, const std::string& type) {
    const TypeInfo target = typeInfo(type);
    if (target.kind == TypeKind::String || isStringType(value.type)) {
        if (target.kind != TypeKind::String || !isStringType(value.type)) {
            fail("Cannot convert between " + value.type + " and " + target.name);
        }
        Value result;
        result.type = target.name;
        result.text = value.text;
        return result;
    }
    if (target.kind == TypeKind::Float) {
        if (isFloatType(value.type)) return makeFloat(value.real, target.name);
        return makeFloat(valueAsDouble(value), target.name);
    }
    return asInteger(value, target.name);
}

Value valueFromBits(const U128& bits, const std::string& type) {
    const TypeInfo info = typeInfo(type);
    if (info.kind == TypeKind::Float) {
        return makeFloat(u128ToDouble(u128Mask(bits, info.bits)), info.name);
    }
    return makeInteger(bits, info.name);
}

U128 valueBits(const Value& value) {
    if (isFloatType(value.type)) {
        const double truncated = std::trunc(value.real);
        if (!std::isfinite(truncated)) fail("Cannot take bits of a non-finite float");
        const U128 magnitude = u128FromDouble(std::fabs(truncated));
        return truncated < 0 ? u128Sub(U128{0, 0}, magnitude) : magnitude;
    }
    return u128Mask(value.bits, typeBits(value.type));
}

std::string displayValue(const Value& value) {
    const TypeInfo info = typeInfo(value.type);
    if (info.kind == TypeKind::String) return value.text;
    if (info.kind == TypeKind::Float) return formatFloat(value.real, info.bits);
    if (info.kind == TypeKind::Unsigned) return u128ToString(u128Mask(value.bits, info.bits));
    const U128 extended = u128SignExtend(value.bits, info.bits);
    if (signOf(extended) > 0) return u128ToString(extended);
    return "-" + u128ToString(u128Sub(U128{0, 0}, extended));
}

std::uint32_t valueLowByte(const Value& value) {
    if (isFloatType(value.type)) {
        const double truncated = std::trunc(value.real);
        if (!std::isfinite(truncated)) return 0;
        const long long signed_value = static_cast<long long>(truncated);
        return static_cast<std::uint32_t>(static_cast<unsigned long long>(signed_value) & 0xFFull);
    }
    return static_cast<std::uint32_t>(value.bits.lo & 0xFFull);
}

bool valueIsZero(const Value& value) {
    if (isStringType(value.type)) return value.text.empty();
    if (isFloatType(value.type)) return value.real == 0.0;
    return u128IsZero(u128Mask(value.bits, typeBits(value.type)));
}

Value boolValue(bool value, const std::string& type) {
    const TypeInfo target = typeInfo(type);
    if (target.kind == TypeKind::Float) return makeFloat(value ? 1.0 : 0.0, target.name);
    return makeInteger(u128FromU64(value ? 1ull : 0ull), target.name);
}

// ---------------------------------------------------------------------------
// 字符串指令
// ---------------------------------------------------------------------------

const std::string& stringValue(const Value& value) {
    if (!isStringType(value.type)) fail("Expected a string value, got " + value.type);
    return value.text;
}

static Value stringResult(const std::string& text) {
    Value result;
    result.type = "str";
    result.text = text;
    return result;
}

static Value indexResult(long long value, const std::string& resultType) {
    const std::string type = isTypeName(resultType) ? typeInfo(resultType).name
                                                    : std::string(defaultTypeName());
    if (isStringType(type)) fail("String instruction needs a numeric result type");
    if (isFloatType(type)) return makeFloat(static_cast<double>(value), type);
    // 结果可能是 -1（比较 / 查找失败），按二补码直接构造位模式。
    Value result;
    result.type = type;
    const unsigned bits = typeBits(type);
    const unsigned long long magnitude = static_cast<unsigned long long>(value < 0 ? -value : value);
    result.bits = value < 0 ? u128Sub(maskOf(bits), U128{magnitude - 1, 0}) : U128{magnitude, 0};
    result.bits = u128Mask(result.bits, bits);
    return result;
}

Value stringLength(const Value& value, const std::string& resultType) {
    return indexResult(static_cast<long long>(stringValue(value).size()), resultType);
}

Value stringCompare(const Value& lhs, const Value& rhs, const std::string& resultType) {
    const auto& a = stringValue(lhs);
    const auto& b = stringValue(rhs);
    const int order = a < b ? -1 : a > b ? 1 : 0;
    return indexResult(order, resultType);
}

Value stringConcat(const Value& lhs, const Value& rhs) {
    return stringResult(stringValue(lhs) + stringValue(rhs));
}

Value substring(const Value& value, int start, int length) {
    const auto& text = stringValue(value);
    const long long size = static_cast<long long>(text.size());
    if (start < 0) start = 0;
    if (length < 0) length = 0;
    if (start > size) start = static_cast<int>(size);
    long long count = std::min<long long>(length, size - start);
    return stringResult(text.substr(static_cast<std::size_t>(start),
                                   static_cast<std::size_t>(count)));
}

Value stringFind(const Value& haystack, const Value& needle, const std::string& resultType) {
    const auto& text = stringValue(haystack);
    const auto& target = stringValue(needle);
    const auto position = text.find(target);
    return indexResult(position == std::string::npos ? -1
                                                     : static_cast<long long>(position),
                       resultType);
}

Value evalTyped(const std::string& base, const std::string& type, const Value& lhs,
                const Value& rhs) {
    const TypeInfo info = typeInfo(type);
    if (info.kind == TypeKind::String) {
        fail("String operator " + base + " must be handled by the string routines");
    }

    if (info.kind == TypeKind::Float) {
        const double a = valueAsDouble(lhs), b = valueAsDouble(rhs);
        if (base == "+") return makeFloat(a + b, info.name);
        if (base == "-") return makeFloat(a - b, info.name);
        if (base == "*") return makeFloat(a * b, info.name);
        if (base == "/") {
            if (b == 0.0) fail("Float division by zero");
            return makeFloat(a / b, info.name);
        }
        if (base == "neg") return makeFloat(-a, info.name);
        if (base == "==") return boolValue(a == b, "i8");
        if (base == "!=") return boolValue(a != b, "i8");
        if (base == "<") return boolValue(a < b, "i8");
        if (base == "<=") return boolValue(a <= b, "i8");
        if (base == ">") return boolValue(a > b, "i8");
        if (base == ">=") return boolValue(a >= b, "i8");
        if (base == "cvt") return convertValue(lhs, info.name);
        fail("Operator " + base + " is not defined for " + info.name);
    }

    const Value a = asInteger(lhs, info.name);
    const Value b = asInteger(rhs, info.name);
    const unsigned bits = info.bits;
    const bool isUnsigned = info.kind == TypeKind::Unsigned;

    if (base == "+") return makeInteger(u128Add(a.bits, b.bits), info.name);
    if (base == "-") return makeInteger(u128Sub(a.bits, b.bits), info.name);
    if (base == "*") return makeInteger(u128Mul(a.bits, b.bits), info.name);
    if (base == "&") return makeInteger(u128And(a.bits, b.bits), info.name);
    if (base == "|") return makeInteger(u128Or(a.bits, b.bits), info.name);
    if (base == "^") return makeInteger(u128Xor(a.bits, b.bits), info.name);
    if (base == "~") return makeInteger(u128Not(a.bits), info.name);
    if (base == "neg") return makeInteger(u128Sub(U128{0, 0}, a.bits), info.name);
    if (base == "odd") return boolValue((a.bits.lo & 1ull) != 0, "i8");
    if (base == "cvt") return convertValue(lhs, info.name);

    if (base == "<<" || base == ">>") {
        const unsigned amount = static_cast<unsigned>(b.bits.lo & (bits - 1));
        if (base == "<<") return makeInteger(u128Shl(a.bits, amount), info.name);
        if (isUnsigned) return makeInteger(u128Shr(a.bits, amount), info.name);
        return makeInteger(u128Shr(u128SignExtend(a.bits, bits), amount), info.name);
    }

    if (base == "==") return boolValue(isUnsigned ? !u128Equals(a.bits, b.bits) == false
                                                  : compareSigned(a, b) == 0,
                                       "i8");
    if (base == "!=") return boolValue(isUnsigned ? !u128Equals(a.bits, b.bits)
                                                  : compareSigned(a, b) != 0,
                                       "i8");
    if (base == "<") return boolValue(isUnsigned ? compareUnsigned(a, b) < 0 : compareSigned(a, b) < 0, "i8");
    if (base == "<=") return boolValue(isUnsigned ? compareUnsigned(a, b) <= 0 : compareSigned(a, b) <= 0, "i8");
    if (base == ">") return boolValue(isUnsigned ? compareUnsigned(a, b) > 0 : compareSigned(a, b) > 0, "i8");
    if (base == ">=") return boolValue(isUnsigned ? compareUnsigned(a, b) >= 0 : compareSigned(a, b) >= 0, "i8");

    if (base == "/" || base == "%") {
        if (u128IsZero(b.bits)) fail("Division by zero");
        U128 quotient, remainder;
        if (isUnsigned) {
            u128DivMod(a.bits, b.bits, quotient, remainder);
            return makeInteger(base == "/" ? quotient : remainder, info.name);
        }
        const U128 magnitudeA = signedMagnitude(a), magnitudeB = signedMagnitude(b);
        const bool negativeA = (u128SignExtend(a.bits, bits).hi >> 63) != 0;
        const bool negativeB = (u128SignExtend(b.bits, bits).hi >> 63) != 0;
        if (negativeA && !negativeB && u128Equals(magnitudeB, U128{1, 0}) &&
            u128Equals(magnitudeA, u128Add(maskOf(bits - 1), U128{1, 0}))) {
            fail("Division overflow");
        }
        u128DivMod(magnitudeA, magnitudeB, quotient, remainder);
        if (base == "/") return makeSignedInteger(quotient, negativeA != negativeB, info.name);
        return makeSignedInteger(remainder, negativeA, info.name);
    }
    fail("Unknown operator: " + base);
}

Value addWithCarry(const std::string& type, const Value& lhs, const Value& rhs, bool carryIn,
                   bool& carryOut) {
    const TypeInfo info = typeInfo(type);
    if (info.kind == TypeKind::Float) fail("Carry addition requires an integer type");
    const Value a = asInteger(lhs, info.name), b = asInteger(rhs, info.name);
    const unsigned bits = info.bits;
    // 位宽内的进位判断：128 位用「和比加数小」检测回绕，其余比较最大值。
    const auto overflowed = [&](const U128& sum, const U128& base) {
        return bits == 128 ? u128Compare(sum, base) < 0 : u128Compare(sum, maskOf(bits)) > 0;
    };
    const U128 partial = u128Add(a.bits, b.bits);
    const bool carryFirst = overflowed(partial, a.bits);
    const U128 total = carryIn ? u128Add(partial, U128{1, 0}) : partial;
    const bool carrySecond = carryIn && overflowed(total, partial);
    carryOut = carryFirst || carrySecond;
    return makeInteger(total, info.name);
}

Value subWithBorrow(const std::string& type, const Value& lhs, const Value& rhs, bool borrowIn,
                    bool& borrowOut) {
    const TypeInfo info = typeInfo(type);
    if (info.kind == TypeKind::Float) fail("Borrow subtraction requires an integer type");
    const Value a = asInteger(lhs, info.name), b = asInteger(rhs, info.name);
    const bool borrowFirst = u128Compare(a.bits, b.bits) < 0;
    const U128 partial = u128Sub(a.bits, b.bits);
    const U128 total = borrowIn ? u128Sub(partial, U128{1, 0}) : partial;
    const bool borrowSecond = borrowIn && u128IsZero(partial);
    borrowOut = borrowFirst || borrowSecond;
    return makeInteger(total, info.name);
}

void multiplyWide(const std::string& type, const Value& lhs, const Value& rhs, Value& hi,
                  Value& lo) {
    const TypeInfo info = typeInfo(type);
    if (info.kind == TypeKind::Float) fail("Two-register multiplication requires an integer type");
    if (info.bits > 64) {
        fail("Two-register multiplication supports up to 64-bit types; use * for " + info.name);
    }
    const Value a = asInteger(lhs, info.name), b = asInteger(rhs, info.name);
    // 二补码乘法的低 2N 位与符号无关：先做 N 位无符号乘，再拆高低半。
    const U128 product = u128Mul(a.bits, b.bits);
    hi = makeInteger(u128Shr(product, info.bits), info.name);
    lo = makeInteger(product, info.name);
}

void divideWide(const std::string& type, const Value& lhs, const Value& rhs, Value& quotient,
                Value& remainder) {
    const TypeInfo info = typeInfo(type);
    if (info.kind == TypeKind::Float) fail("Two-register division requires an integer type");
    quotient = evalTyped("/", info.name, lhs, rhs);
    remainder = evalTyped("%", info.name, lhs, rhs);
}

std::string opBase(const std::string& op) {
    // `:=` 的冒号是运算符本身；`:u32` 之类才是类型后缀（如 `:=:u32`）。
    if (op.rfind(":=", 0) == 0) return ":=";
    const auto colon = op.find(':');
    if (colon == std::string::npos || colon + 1 == op.size()) return op;
    return op.substr(0, colon);
}

std::string opType(const std::string& op) {
    if (op.rfind(":=", 0) == 0) return op.size() > 2 ? op.substr(3) : std::string();
    const auto colon = op.find(':');
    if (colon == std::string::npos || colon + 1 == op.size()) return std::string();
    return op.substr(colon + 1);
}

std::string typedOp(const std::string& base, const std::string& type) {
    if (type.empty() || type == defaultTypeName()) return base;
    return base + ":" + type;
}

} // namespace transfer
