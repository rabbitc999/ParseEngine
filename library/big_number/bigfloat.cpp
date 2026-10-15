/**
 * @file bigfloat.cpp
 * @brief `BigFloat` 的实现：尾数 `BigInt` × 10^exp10，四则 / 比较 / 舍入 / 平方根。
 *
 * 策略与参考实现一致：所有运算都在整数上做，**多算几位再舍入**到目标有效位数，
 * 所以 `0.1 + 0.2` 精确等于 `0.3`（十进制浮点，不是二进制浮点）。
 * 大规模乘法直接吃到 `BigInt` 的 NTT 内核（O(n log n)）。
 */

#include "bigfloat.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <stdexcept>

namespace bigfloat
{
namespace
{
/** @brief 默认有效位数（`operator/` 用；与 128 位浮点的十进制约 34 位同量级）。 */
constexpr int kDefaultDivisionDigits = 34;

/** @brief 十进制位数（0 算 1 位）。 */
int decimalDigits(const bigint::BigInt &value)
{
    return static_cast<int>(value.decimalDigits());
}

/**
 * @brief `value / 10^shift`，四舍五入（远离零）。
 * @param shift 移掉的十进制位数（≤ 0 时原样返回）。
 */
bigint::BigInt divideRounded(const bigint::BigInt &value, int shift)
{
    if (shift <= 0) return value;
    const bigint::BigInt divisor = bigint::pow10(static_cast<std::uint64_t>(shift));
    bigint::BigInt quotient;
    bigint::BigInt remainder;
    value.divmod(divisor, &quotient, &remainder);
    if (remainder.abs() * bigint::BigInt(2) >= divisor)
    {
        quotient += bigint::BigInt(value.sign() < 0 ? -1 : 1);
    }
    return quotient;
}

/** @brief 字符是不是数字。 */
bool isDigit(char ch) { return ch >= '0' && ch <= '9'; }

/**
 * @brief 把一个"已经舍入好"的值写成十进制文本。
 * @param mantissa 尾数（符号在里面）。
 * @param exp10 十进制指数。
 * @param plain `true` = 恒定普通记法；`false` = 按 `%g` 口径在指数过大 / 过小时切科学记数法。
 * @param precision 显示口径的有效位数（`%g` 里那个 P：指数 `X` 满足 `X < -4 || X >= P` 时用科学记数法）。
 */
std::string formatDecimal(const bigint::BigInt &mantissa, int exp10, bool plain, int precision)
{
    if (mantissa.isZero()) return "0";
    const bool negative = mantissa.sign() < 0;
    const std::string digits = mantissa.abs().toString();
    const int digitCount = static_cast<int>(digits.size());
    const int pointPos = digitCount + exp10;   /* 小数点位置：0 = 最左 */
    const int exponent = pointPos - 1;         /* 科学记数法里的指数：值 = d.ddd × 10^exponent */

    std::string text;
    if (negative) text.push_back('-');
    if (!plain && (exponent < -4 || exponent >= precision))
    {
        text.push_back(digits[0]);
        if (digitCount > 1)
        {
            text.push_back('.');
            text += digits.substr(1u);
        }
        text.push_back('e');
        if (exponent >= 0) text.push_back('+');
        text += std::to_string(exponent);
        return text;
    }
    if (pointPos <= 0)
    {
        text += "0.";
        text.append(static_cast<std::size_t>(-pointPos), '0');
        text += digits;
        return text;
    }
    if (pointPos >= digitCount)
    {
        text += digits;
        text.append(static_cast<std::size_t>(pointPos - digitCount), '0');
        return text;
    }
    text += digits.substr(0u, static_cast<std::size_t>(pointPos));
    text.push_back('.');
    text += digits.substr(static_cast<std::size_t>(pointPos));
    return text;
}
} // namespace

// ---------------------------------------------------------------------------
//  解析 / 规范化
// ---------------------------------------------------------------------------

BigFloat::BigFloat(const std::string &text)
{
    *this = fromString(text, nullptr);
}

BigFloat BigFloat::fromString(const std::string &text, bool *ok)
{
    auto report = [&](bool success) {
        if (ok != nullptr) *ok = success;
    };

    std::size_t index = 0u;
    while (index < text.size() && (text[index] == ' ' || text[index] == '\t')) ++index;
    int sign = 1;
    if (index < text.size() && (text[index] == '+' || text[index] == '-'))
    {
        if (text[index] == '-') sign = -1;
        ++index;
    }

    std::string integerPart;
    while (index < text.size() && isDigit(text[index])) integerPart.push_back(text[index++]);
    std::string fractionPart;
    if (index < text.size() && text[index] == '.')
    {
        ++index;
        while (index < text.size() && isDigit(text[index])) fractionPart.push_back(text[index++]);
    }
    if (integerPart.empty() && fractionPart.empty())
    {
        report(false);
        return BigFloat();
    }
    long long exponent = 0;
    if (index < text.size() && (text[index] == 'e' || text[index] == 'E'))
    {
        ++index;
        int exponentSign = 1;
        if (index < text.size() && (text[index] == '+' || text[index] == '-'))
        {
            if (text[index] == '-') exponentSign = -1;
            ++index;
        }
        if (index >= text.size() || !isDigit(text[index]))
        {
            report(false);
            return BigFloat();
        }
        while (index < text.size() && isDigit(text[index]))
        {
            exponent = exponent * 10 + (text[index++] - '0');
            if (exponent > 1000000)   /* 指数大得离谱就当语法错，别把内存吃光 */
            {
                report(false);
                return BigFloat();
            }
        }
        exponent *= exponentSign;
    }
    while (index < text.size() && (text[index] == ' ' || text[index] == '\t')) ++index;
    if (index != text.size())
    {
        report(false);
        return BigFloat();
    }

    BigFloat out;
    out.mant_ = bigint::BigInt(integerPart + fractionPart);   /* 允许前导 0 */
    if (!out.mant_.isZero()) out.mant_ = sign < 0 ? -out.mant_ : out.mant_;
    const long long exponentValue =
        exponent - static_cast<long long>(fractionPart.size());
    if (exponentValue < std::numeric_limits<int>::min() ||
        exponentValue > std::numeric_limits<int>::max())
    {
        report(false);
        return BigFloat();
    }
    out.exp10_ = static_cast<int>(exponentValue);
    out.normalize();
    report(true);
    return out;
}

BigFloat BigFloat::fromInt(const bigint::BigInt &value)
{
    BigFloat out;
    out.mant_ = value;
    out.normalize();
    return out;
}

BigFloat BigFloat::fromDouble(double value, int digits)
{
    if (!std::isfinite(value)) return BigFloat();   /* NaN / ±inf 按 0 处理 */
    const int limit = (digits > 0 && digits < 17) ? digits : 17;
    char buffer[64];
    bool ok = false;
    BigFloat out;
    /* 从 1 位有效数字往上试：第一个"读回来还是同一个 double"的写法就是最短写法。
     * `%.17g` 一定能往返，所以循环最坏也就 17 轮。 */
    for (int precision = 1; precision <= limit; ++precision)
    {
        std::snprintf(buffer, sizeof(buffer), "%.*g", precision, value);
        out = fromString(buffer, &ok);
        if (!ok) return BigFloat();   /* 理论上不会发生；兜底成 0 */
        if (std::strtod(buffer, nullptr) == value) break;
    }
    return out;
}

void BigFloat::normalize()
{
    if (mant_.isZero())
    {
        exp10_ = 0;
        return;
    }
    /* 先按 10^9 一档剥尾随零（比一位一位剥快得多），最后再用 10 收尾 */
    for (;;)
    {
        bigint::BigInt quotient;
        bigint::BigInt remainder;
        mant_.divmod(bigint::pow10(9u), &quotient, &remainder);
        if (!remainder.isZero()) break;
        mant_ = quotient;
        exp10_ += 9;
    }
    for (;;)
    {
        bigint::BigInt quotient;
        bigint::BigInt remainder;
        mant_.divmod(bigint::BigInt(10), &quotient, &remainder);
        if (!remainder.isZero()) break;
        mant_ = quotient;
        exp10_ += 1;
    }
}

// ---------------------------------------------------------------------------
//  舍入 / 输出 / 比较
// ---------------------------------------------------------------------------

BigFloat BigFloat::round(int digits) const
{
    if (digits <= 0) throw std::invalid_argument("bigfloat: 有效位数必须是正数");
    BigFloat out = *this;
    if (out.mant_.isZero()) return out;
    const int have = decimalDigits(out.mant_);
    if (have <= digits) return out;
    const int shift = have - digits;
    out.mant_ = divideRounded(out.mant_, shift);
    out.exp10_ += shift;
    out.normalize();
    return out;
}

std::string BigFloat::toString(int digits) const
{
    /* `digits ≤ 0` = 不舍入，但显示口径仍按缺省的 17 位（跟 `%.17g` 一致） */
    const int precision = (digits > 0) ? digits : 17;
    if (digits <= 0) return formatDecimal(mant_, exp10_, false, precision);
    const BigFloat value = round(digits);
    return formatDecimal(value.mant_, value.exp10_, false, precision);
}

std::string BigFloat::toPlainString(int digits) const
{
    if (digits <= 0) return formatDecimal(mant_, exp10_, true, 17);   /* 不舍入 */
    const BigFloat value = round(digits);
    return formatDecimal(value.mant_, value.exp10_, true, digits);
}

int BigFloat::compare(const BigFloat &other) const
{
    if (mant_.sign() != other.mant_.sign())
    {
        return (mant_.sign() < other.mant_.sign()) ? -1 : 1;
    }
    if (mant_.isZero()) return 0;
    /* 同号：比 尾数 × 10^exp10 的绝对值 —— 对齐到更小的指数，再比整数 */
    const int exponent = std::min(exp10_, other.exp10_);
    const bigint::BigInt left =
        mant_.abs() * bigint::pow10(static_cast<std::uint64_t>(exp10_ - exponent));
    const bigint::BigInt right =
        other.mant_.abs() * bigint::pow10(static_cast<std::uint64_t>(other.exp10_ - exponent));
    const int magnitude = left.compare(right);
    return (mant_.sign() >= 0) ? magnitude : -magnitude;
}

// ---------------------------------------------------------------------------
//  四则
// ---------------------------------------------------------------------------

BigFloat operator+(const BigFloat &left, const BigFloat &right)
{
    if (left.isZero()) return right;
    if (right.isZero()) return left;
    const int exponent = std::min(left.exp10_, right.exp10_);
    BigFloat out;
    out.mant_ = left.mant_ * bigint::pow10(static_cast<std::uint64_t>(left.exp10_ - exponent)) +
                right.mant_ * bigint::pow10(static_cast<std::uint64_t>(right.exp10_ - exponent));
    out.exp10_ = exponent;
    out.normalize();
    return out;
}

BigFloat operator-(const BigFloat &left, const BigFloat &right) { return left + (-right); }

BigFloat BigFloat::operator-() const
{
    BigFloat out = *this;
    out.mant_ = -out.mant_;
    return out;
}

BigFloat operator*(const BigFloat &left, const BigFloat &right)
{
    BigFloat out;
    out.mant_ = left.mant_ * right.mant_;
    out.exp10_ = left.exp10_ + right.exp10_;
    out.normalize();
    return out;
}

BigFloat BigFloat::divide(const BigFloat &divisor, int digits) const
{
    if (divisor.isZero()) throw std::domain_error("bigfloat: 除以零");
    if (digits <= 0) throw std::invalid_argument("bigfloat: 有效位数必须是正数");
    if (isZero()) return BigFloat();
    /* 把被除数放大，让商有 digits + 2 位有效数字，再舍入回 digits 位 */
    const int numeratorDigits = decimalDigits(mant_);
    const int denominatorDigits = decimalDigits(divisor.mant_);
    const int shift = digits + 2 + denominatorDigits - numeratorDigits;
    bigint::BigInt numerator = mant_;
    int exponent = exp10_ - divisor.exp10_;
    if (shift > 0)
    {
        numerator *= bigint::pow10(static_cast<std::uint64_t>(shift));
        exponent -= shift;
    }
    bigint::BigInt quotient;
    bigint::BigInt remainder;
    numerator.divmod(divisor.mant_, &quotient, &remainder);
    BigFloat out;
    out.mant_ = quotient;
    out.exp10_ = exponent;
    out.normalize();
    return out.round(digits);
}

BigFloat operator/(const BigFloat &left, const BigFloat &right)
{
    return left.divide(right, kDefaultDivisionDigits);
}

BigFloat &BigFloat::operator+=(const BigFloat &other) { *this = *this + other; return *this; }
BigFloat &BigFloat::operator-=(const BigFloat &other) { *this = *this - other; return *this; }
BigFloat &BigFloat::operator*=(const BigFloat &other) { *this = *this * other; return *this; }
BigFloat &BigFloat::operator/=(const BigFloat &other) { *this = *this / other; return *this; }

bool operator==(const BigFloat &left, const BigFloat &right)
{
    return left.compare(right) == 0;
}
bool operator!=(const BigFloat &left, const BigFloat &right)
{
    return left.compare(right) != 0;
}
bool operator<(const BigFloat &left, const BigFloat &right)
{
    return left.compare(right) < 0;
}
bool operator<=(const BigFloat &left, const BigFloat &right)
{
    return left.compare(right) <= 0;
}
bool operator>(const BigFloat &left, const BigFloat &right)
{
    return left.compare(right) > 0;
}
bool operator>=(const BigFloat &left, const BigFloat &right)
{
    return left.compare(right) >= 0;
}

// ---------------------------------------------------------------------------
//  平方根
// ---------------------------------------------------------------------------

BigFloat BigFloat::sqrt(int digits) const
{
    if (digits <= 0) throw std::invalid_argument("bigfloat: 有效位数必须是正数");
    if (mant_.sign() < 0) throw std::domain_error("bigfloat: 负数不能开平方");
    if (isZero()) return BigFloat();
    /* 值 = m × 10^e：先把 e 凑成偶数（不够就 m × 10、e − 1），
     * 再把 m 放大到 2×digits 位，做整数平方根，最后舍入。 */
    bigint::BigInt m = mant_.abs();
    long long exponent = exp10_;
    if ((exponent % 2) != 0)
    {
        m *= bigint::BigInt(10);
        exponent -= 1;
    }
    /* 多算 2 位再舍回（尾数误差 < 1 个最低位，2 位余量足够躲开四舍五入的边界） */
    const long long want = 2ll * digits + 2ll;
    const long long have = static_cast<long long>(m.decimalDigits());
    if (have < want)
    {
        long long extra = want - have;
        /* 放大的位数要跟指数**同奇偶**，这样 `exponent / 2` 才是精确的一半
         * —— 否则 sqrt(4) 会算出 6.324…×10⁻⁹ 这种结果。 */
        if (((extra + exponent) % 2) != 0) extra += 1;
        m *= bigint::pow10(static_cast<std::uint64_t>(extra));
        exponent -= extra;
    }
    BigFloat out;
    out.mant_ = bigint::integerSqrt(m);
    out.exp10_ = static_cast<int>(exponent / 2);
    out.normalize();
    return out.round(digits);
}

} // namespace bigfloat
