/**
 * @file bigint.cpp
 * @brief `BigInt` 的实现：基 2^32 的 limb 数组 + 三级乘法内核 + Burnikel-Ziegler 除法。
 *
 * 参考 `eigenmath-plus/eigenmath_bigint.c` 的整数部分：同样是"符号 + 小端 limb +
 * 有效长度 + 每次运算后规范化"，只是把手工 `malloc` 换成 `std::vector`。
 * 乘法按规模在学校算法、Karatsuba 与 NTT 之间切换（阈值与实测数字见 README）。
 */

#include "bigint.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace bigint
{
namespace
{
constexpr std::uint64_t kLimbBase = 1ull << 32;   ///< 一个 limb 的基数。
constexpr std::uint32_t kDecimalChunk = 1000000000u;  ///< 十进制串一段 9 位。

/* ---- NTT 内核（参考实现里"大规模乘法走 NTT"那部分的 C++ 版）----
 *
 * 两个 NTT 友好的素数：p = c × 2^k + 1，都有原根 3：
 *   p1 = 119 × 2^23 + 1 = 998244353      （支持长度到 2^23）
 *   p2 = 479 × 2^21 + 1 = 1004535809     （支持长度到 2^21）
 * 每个系数的真值 < p1 × p2 ≈ 2^60，而 15 位数字乘法的系数上界是
 * 15+15+log2(n) 位（n ≤ 2^20 时约 50 位），所以 CRT 重建不会溢出。
 */
constexpr std::uint32_t kNttPrime1 = 998244353u;
constexpr std::uint32_t kNttPrime2 = 1004535809u;
constexpr std::uint32_t kNttRoot = 3u;
constexpr std::size_t kDigitBits = 15u;      ///< NTT 用的"15 位数字"。
/* 三级乘法实测阈值（MinGW g++ 11.2 -O2）：学校算法 / Karatsuba 约 96 limb，
 * Karatsuba / NTT 的首个平衡交叉区约为两边合计 48,000 limb。详见 README。 */
constexpr std::size_t kDefaultThreshold = 48000u;
constexpr std::size_t kDefaultKaratsubaThreshold = 96u;
constexpr std::size_t kDefaultDivisionThreshold = 56u;

std::size_t g_multiplicationThreshold = kDefaultThreshold;
std::size_t g_karatsubaThreshold = kDefaultKaratsubaThreshold;
std::size_t g_divisionThreshold = kDefaultDivisionThreshold;

/**
 * @brief (base ^ exponent) mod Modulus（快速幂，uint32 版）。
 *
 * `Modulus` 走模板参数（编译期常量）：蝶形里的 `%` 于是能编成"乘高位 + 移位"
 * 的定长除法替代，而不是很慢的 64 位除法指令 —— 这是 NTT 快慢的关键。
 */
template <std::uint32_t Modulus>
std::uint32_t powModU32(std::uint32_t base, std::uint64_t exponent)
{
    constexpr std::uint64_t modulus = Modulus;
    std::uint64_t result = 1u;
    std::uint64_t value = base % modulus;
    while (exponent != 0u)
    {
        if ((exponent & 1u) != 0u) result = (result * value) % modulus;
        value = (value * value) % modulus;
        exponent >>= 1u;
    }
    return static_cast<std::uint32_t>(result);
}

/** @brief 原地 NTT（invert = true 时做逆变换并乘上 n^{-1}）。 */
template <std::uint32_t Modulus>
void ntt(std::vector<std::uint32_t> &values, bool invert)
{
    constexpr std::uint64_t modulus = Modulus;
    const std::size_t count = values.size();
    for (std::size_t index = 1u, reverse = 0u; index < count; ++index)
    {
        std::size_t bit = count >> 1u;
        for (; (reverse & bit) != 0u; bit >>= 1u) reverse ^= bit;
        reverse ^= bit;
        if (index < reverse) std::swap(values[index], values[reverse]);
    }
    for (std::size_t length = 2u; length <= count; length <<= 1u)
    {
        std::uint32_t root = powModU32<Modulus>(kNttRoot, (modulus - 1u) / length);
        if (invert) root = powModU32<Modulus>(root, modulus - 2u);   /* 逆元 */
        for (std::size_t start = 0u; start < count; start += length)
        {
            std::uint64_t weight = 1u;
            const std::size_t half = length / 2u;
            for (std::size_t offset = 0u; offset < half; ++offset)
            {
                const std::uint32_t even = values[start + offset];
                const std::uint32_t odd = static_cast<std::uint32_t>(
                    (values[start + offset + half] * weight) % modulus);
                /* 加减各是"两个 < p 的数"，用条件减代替取模（省掉除法） */
                std::uint32_t sum = even + odd;
                if (sum >= modulus) sum -= static_cast<std::uint32_t>(modulus);
                std::uint32_t difference = even + static_cast<std::uint32_t>(modulus) - odd;
                if (difference >= modulus) difference -= static_cast<std::uint32_t>(modulus);
                values[start + offset] = sum;
                values[start + offset + half] = difference;
                weight = (weight * root) % modulus;
            }
        }
    }
    if (invert)
    {
        const std::uint32_t inverse =
            powModU32<Modulus>(static_cast<std::uint32_t>(count), modulus - 2u);
        for (std::uint32_t &value : values)
        {
            value = static_cast<std::uint32_t>(
                (static_cast<std::uint64_t>(value) * inverse) % modulus);
        }
    }
}

/** @brief 把幅值切成 15 位一段的数字（小端）。 */
std::vector<std::uint32_t> toBase15(const std::vector<std::uint32_t> &limbs)
{
    std::vector<std::uint32_t> digits;
    if (limbs.empty()) return digits;
    const std::size_t totalBits = (limbs.size() - 1u) * 32u;
    std::size_t topBits = 0u;
    for (std::uint32_t top = limbs.back(); top != 0u; top >>= 1u) ++topBits;
    const std::size_t bitCount = totalBits + topBits;
    const std::size_t digitCount = (bitCount + kDigitBits - 1u) / kDigitBits;
    digits.assign(digitCount, 0u);
    for (std::size_t bit = 0u; bit < bitCount; ++bit)
    {
        if (((limbs[bit / 32u] >> (bit % 32u)) & 1u) == 0u) continue;
        digits[bit / kDigitBits] |= (1u << (bit % kDigitBits));
    }
    return digits;
}

/** @brief 一次 NTT 卷积，模 `Modulus`（结果长度 = a.size() + b.size() - 1）。 */
template <std::uint32_t Modulus>
std::vector<std::uint32_t> convolution(const std::vector<std::uint32_t> &left,
                                       const std::vector<std::uint32_t> &right)
{
    constexpr std::uint64_t modulus = Modulus;
    std::size_t count = 1u;
    while (count < left.size() + right.size()) count <<= 1u;
    std::vector<std::uint32_t> a(count, 0u);
    std::vector<std::uint32_t> b(count, 0u);
    /* 15 位数字一定 < 2^15 < 两个素数，所以只要夹一下就够了（不用除法） */
    for (std::size_t index = 0u; index < left.size(); ++index)
    {
        a[index] = (left[index] >= modulus) ? left[index] - static_cast<std::uint32_t>(modulus)
                                            : left[index];
    }
    for (std::size_t index = 0u; index < right.size(); ++index)
    {
        b[index] = (right[index] >= modulus) ? right[index] - static_cast<std::uint32_t>(modulus)
                                             : right[index];
    }
    ntt<Modulus>(a, false);
    ntt<Modulus>(b, false);
    for (std::size_t index = 0u; index < count; ++index)
    {
        a[index] = static_cast<std::uint32_t>(
            (static_cast<std::uint64_t>(a[index]) * b[index]) % modulus);
    }
    ntt<Modulus>(a, true);
    return a;
}

/**
 * @brief 把 15 位数字（小端）打包回 BigInt 的 32 位 limb。
 *
 * 用一个 64 位小缓冲按 15 位往里塞、够 32 位就吐出一个 limb —— 线性一趟，
 * 不做"每个数字左移 15i 再相加"那种 O(n²) 的笨办法。
 */
std::vector<std::uint32_t> packBase15(const std::vector<std::uint64_t> &digits)
{
    /* 真正的位长度：从最高那个非零的 15 位数字数出来 */
    std::size_t bitCount = 0u;
    for (std::size_t index = digits.size(); index-- > 0u;)
    {
        if (digits[index] == 0u) continue;
        bitCount = index * kDigitBits;
        for (std::uint64_t top = digits[index]; top != 0u; top >>= 1u) ++bitCount;
        break;
    }
    if (bitCount == 0u) return std::vector<std::uint32_t>();   /* 0 */
    std::vector<std::uint32_t> limbs((bitCount + 31u) / 32u, 0u);
    std::size_t limbIndex = 0u;
    std::uint64_t buffer = 0u;
    std::size_t buffered = 0u;
    for (const std::uint64_t digit : digits)
    {
        buffer |= (digit & 0x7FFFull) << buffered;
        buffered += kDigitBits;
        while (buffered >= 32u)
        {
            if (limbIndex < limbs.size()) limbs[limbIndex] = static_cast<std::uint32_t>(buffer);
            ++limbIndex;
            buffer >>= 32u;
            buffered -= 32u;
        }
    }
    if (buffered != 0u && limbIndex < limbs.size())
    {
        limbs[limbIndex] = static_cast<std::uint32_t>(buffer);
    }
    return limbs;
}

using Limbs = std::vector<std::uint32_t>;

void trimLimbs(Limbs &limbs)
{
    while (!limbs.empty() && limbs.back() == 0u) limbs.pop_back();
}

Limbs addLimbs(const Limbs &left, const Limbs &right)
{
    Limbs out(std::max(left.size(), right.size()) + 1u, 0u);
    std::uint64_t carry = 0u;
    for (std::size_t index = 0u; index + 1u < out.size(); ++index)
    {
        const std::uint64_t a = (index < left.size()) ? left[index] : 0u;
        const std::uint64_t b = (index < right.size()) ? right[index] : 0u;
        const std::uint64_t value = a + b + carry;
        out[index] = static_cast<std::uint32_t>(value);
        carry = value >> 32u;
    }
    out.back() = static_cast<std::uint32_t>(carry);
    trimLimbs(out);
    return out;
}

/** @brief limb 幅值相减；调用方保证 `left >= right`。 */
Limbs subLimbs(const Limbs &left, const Limbs &right)
{
    Limbs out(left.size(), 0u);
    std::uint64_t borrow = 0u;
    for (std::size_t index = 0u; index < left.size(); ++index)
    {
        const std::uint64_t a = left[index];
        const std::uint64_t b = (index < right.size()) ? right[index] : 0u;
        const std::uint64_t subtrahend = b + borrow;
        if (a < subtrahend)
        {
            out[index] = static_cast<std::uint32_t>(kLimbBase + a - subtrahend);
            borrow = 1u;
        }
        else
        {
            out[index] = static_cast<std::uint32_t>(a - subtrahend);
            borrow = 0u;
        }
    }
    trimLimbs(out);
    return out;
}

Limbs schoolbookMultiply(const Limbs &left, const Limbs &right)
{
    if (left.empty() || right.empty()) return Limbs();
    Limbs out(left.size() + right.size() + 1u, 0u);
    for (std::size_t index = 0u; index < left.size(); ++index)
    {
        std::uint64_t carry = 0u;
        const std::uint64_t a = left[index];
        for (std::size_t inner = 0u; inner < right.size(); ++inner)
        {
            const std::uint64_t value = a * right[inner] + out[index + inner] + carry;
            out[index + inner] = static_cast<std::uint32_t>(value);
            carry = value >> 32u;
        }
        std::size_t cursor = index + right.size();
        while (carry != 0u)
        {
            const std::uint64_t value = out[cursor] + carry;
            out[cursor] = static_cast<std::uint32_t>(value);
            carry = value >> 32u;
            ++cursor;
        }
    }
    trimLimbs(out);
    return out;
}

void addShifted(Limbs &target, const Limbs &value, std::size_t shift)
{
    if (value.empty()) return;
    if (target.size() < shift + value.size() + 1u) target.resize(shift + value.size() + 1u, 0u);
    std::uint64_t carry = 0u;
    std::size_t index = 0u;
    for (; index < value.size(); ++index)
    {
        const std::uint64_t sum = target[shift + index] +
                                  static_cast<std::uint64_t>(value[index]) + carry;
        target[shift + index] = static_cast<std::uint32_t>(sum);
        carry = sum >> 32u;
    }
    std::size_t cursor = shift + index;
    while (carry != 0u)
    {
        const std::uint64_t sum = target[cursor] + carry;
        target[cursor] = static_cast<std::uint32_t>(sum);
        carry = sum >> 32u;
        ++cursor;
        if (cursor == target.size() && carry != 0u) target.push_back(0u);
    }
}

Limbs karatsubaMultiply(const Limbs &left, const Limbs &right, std::size_t threshold)
{
    if (left.empty() || right.empty()) return Limbs();
    if (std::min(left.size(), right.size()) <= threshold)
        return schoolbookMultiply(left, right);

    const std::size_t half = std::max(left.size(), right.size()) / 2u;
    const auto split = [half](const Limbs &value, bool high) {
        const std::size_t begin = high ? std::min(half, value.size()) : 0u;
        const std::size_t end = high ? value.size() : std::min(half, value.size());
        return Limbs(value.begin() + static_cast<std::ptrdiff_t>(begin),
                     value.begin() + static_cast<std::ptrdiff_t>(end));
    };
    const Limbs leftLow = split(left, false);
    const Limbs leftHigh = split(left, true);
    const Limbs rightLow = split(right, false);
    const Limbs rightHigh = split(right, true);

    const Limbs lowProduct = karatsubaMultiply(leftLow, rightLow, threshold);
    const Limbs highProduct = karatsubaMultiply(leftHigh, rightHigh, threshold);
    Limbs middleProduct = karatsubaMultiply(addLimbs(leftLow, leftHigh),
                                            addLimbs(rightLow, rightHigh), threshold);
    middleProduct = subLimbs(middleProduct, lowProduct);
    middleProduct = subLimbs(middleProduct, highProduct);

    Limbs out(left.size() + right.size() + 1u, 0u);
    addShifted(out, lowProduct, 0u);
    addShifted(out, middleProduct, half);
    addShifted(out, highProduct, half * 2u);
    trimLimbs(out);
    return out;
}

/** @brief 十进制字符是不是数字。 */
bool isDigit(char ch) { return ch >= '0' && ch <= '9'; }
} // namespace

// ---------------------------------------------------------------------------
//  构造 / 规范化
// ---------------------------------------------------------------------------

BigInt::BigInt(std::int64_t value)
{
    if (value == 0) return;
    sign_ = (value < 0) ? -1 : 1;
    // INT64_MIN 取负会溢出，所以先按无符号取幅值
    std::uint64_t magnitude = (value < 0) ? (~static_cast<std::uint64_t>(value) + 1ull)
                                         : static_cast<std::uint64_t>(value);
    while (magnitude != 0)
    {
        limbs_.push_back(static_cast<std::uint32_t>(magnitude & 0xFFFFFFFFull));
        magnitude >>= 32u;
    }
}

BigInt::BigInt(const std::string &decimal)
{
    *this = parseDecimal(decimal, nullptr);
}

BigInt BigInt::fromU64(std::uint64_t value)
{
    BigInt out;
    while (value != 0u)
    {
        out.limbs_.push_back(static_cast<std::uint32_t>(value & 0xFFFFFFFFull));
        value >>= 32u;
    }
    if (!out.limbs_.empty()) out.sign_ = 1;
    return out;
}

BigInt BigInt::fromString(const std::string &decimal, bool *ok)
{
    bool parsed = false;
    BigInt value = parseDecimal(decimal, &parsed);
    if (ok != nullptr) *ok = parsed;
    return parsed ? value : BigInt();
}

BigInt BigInt::parseDecimal(const std::string &decimal, bool *ok)
{
    auto report = [&](bool success) {
        if (ok != nullptr) *ok = success;
    };

    std::size_t index = 0u;
    while (index < decimal.size() &&
           (decimal[index] == ' ' || decimal[index] == '\t')) ++index;
    int sign = 1;
    if ((index < decimal.size()) && (decimal[index] == '+' || decimal[index] == '-'))
    {
        if (decimal[index] == '-') sign = -1;
        ++index;
    }
    if (index >= decimal.size())   /* 只有符号 / 空串 */
    {
        report(false);
        return BigInt();
    }
    for (std::size_t cursor = index; cursor < decimal.size(); ++cursor)
    {
        if (!isDigit(decimal[cursor]))
        {
            report(false);
            return BigInt();
        }
    }

    /* 从高位往低位：每 9 位凑一段，`value = value × 10^9 + 段`。 */
    BigInt value;
    std::size_t cursor = index;
    while (cursor < decimal.size())
    {
        const std::size_t take = std::min<std::size_t>(9u, decimal.size() - cursor);
        std::uint32_t chunk = 0u;
        std::uint32_t scale = 1u;
        for (std::size_t offset = 0u; offset < take; ++offset)
        {
            chunk = chunk * 10u + static_cast<std::uint32_t>(decimal[cursor + offset] - '0');
            scale *= 10u;
        }
        value.mulSmallAdd(scale, chunk);
        cursor += take;
    }
    if (!value.isZero()) value.sign_ = sign;
    report(true);
    return value;
}

void BigInt::normalize()
{
    while (!limbs_.empty() && limbs_.back() == 0u) limbs_.pop_back();
    if (limbs_.empty()) sign_ = 0;
}

BigInt BigInt::abs() const
{
    BigInt value = *this;
    if (!value.isZero()) value.sign_ = 1;
    return value;
}

// ---------------------------------------------------------------------------
//  位操作 / 小整数除法（除法与十进制输出用）
// ---------------------------------------------------------------------------

std::size_t BigInt::bitLength() const
{
    if (limbs_.empty()) return 0u;
    std::size_t bits = (limbs_.size() - 1u) * 32u;
    std::uint32_t top = limbs_.back();
    while (top != 0u)
    {
        ++bits;
        top >>= 1u;
    }
    return bits;
}

bool BigInt::bitAt(std::size_t bit) const
{
    const std::size_t index = bit / 32u;
    if (index >= limbs_.size()) return false;
    return ((limbs_[index] >> (bit % 32u)) & 1u) != 0u;
}

void BigInt::setBit(std::size_t bit)
{
    const std::size_t index = bit / 32u;
    if (index >= limbs_.size()) limbs_.resize(index + 1u, 0u);
    limbs_[index] |= (1u << (bit % 32u));
}

void BigInt::shiftLeft(std::size_t bits)
{
    if (limbs_.empty() || bits == 0u) return;
    const std::size_t whole = bits / 32u;
    const std::size_t part = bits % 32u;
    std::vector<std::uint32_t> out(limbs_.size() + whole + 1u, 0u);
    for (std::size_t index = 0u; index < limbs_.size(); ++index)
    {
        const std::uint64_t value = static_cast<std::uint64_t>(limbs_[index]) << part;
        out[index + whole] |= static_cast<std::uint32_t>(value & 0xFFFFFFFFull);
        out[index + whole + 1u] |= static_cast<std::uint32_t>(value >> 32u);
    }
    limbs_.swap(out);
    normalize();
}

void BigInt::shiftRight(std::size_t bits)
{
    if (limbs_.empty() || bits == 0u) return;
    const std::size_t whole = bits / 32u;
    const std::size_t part = bits % 32u;
    if (whole >= limbs_.size())
    {
        limbs_.clear();
        sign_ = 0;
        return;
    }
    std::vector<std::uint32_t> out(limbs_.size() - whole, 0u);
    for (std::size_t index = 0u; index + whole < limbs_.size(); ++index)
    {
        std::uint64_t value = limbs_[index + whole] >> part;
        if (part != 0u && index + whole + 1u < limbs_.size())
        {
            value |= static_cast<std::uint64_t>(limbs_[index + whole + 1u]) << (32u - part);
        }
        out[index] = static_cast<std::uint32_t>(value & 0xFFFFFFFFull);
    }
    limbs_.swap(out);
    normalize();
}

void BigInt::mulSmallAdd(std::uint32_t small, std::uint32_t add)
{
    std::uint64_t carry = add;
    for (std::size_t index = 0u; index < limbs_.size(); ++index)
    {
        const std::uint64_t value =
            static_cast<std::uint64_t>(limbs_[index]) * small + carry;
        limbs_[index] = static_cast<std::uint32_t>(value & 0xFFFFFFFFull);
        carry = value >> 32u;
    }
    while (carry != 0u)
    {
        limbs_.push_back(static_cast<std::uint32_t>(carry & 0xFFFFFFFFull));
        carry >>= 32u;
    }
    normalize();
    if (!limbs_.empty() && sign_ == 0) sign_ = 1;
}

std::uint32_t BigInt::divSmall(std::uint32_t small)
{
    std::uint64_t remainder = 0u;
    for (std::size_t index = limbs_.size(); index-- > 0u;)
    {
        const std::uint64_t value = (remainder << 32u) | limbs_[index];
        limbs_[index] = static_cast<std::uint32_t>(value / small);
        remainder = value % small;
    }
    normalize();
    return static_cast<std::uint32_t>(remainder);
}

// ---------------------------------------------------------------------------
//  幅值运算
// ---------------------------------------------------------------------------

int BigInt::compareMagnitude(const BigInt &left, const BigInt &right)
{
    if (left.limbs_.size() != right.limbs_.size())
    {
        return (left.limbs_.size() < right.limbs_.size()) ? -1 : 1;
    }
    for (std::size_t index = left.limbs_.size(); index-- > 0u;)
    {
        if (left.limbs_[index] != right.limbs_[index])
        {
            return (left.limbs_[index] < right.limbs_[index]) ? -1 : 1;
        }
    }
    return 0;
}

BigInt BigInt::addMagnitude(const BigInt &left, const BigInt &right)
{
    BigInt out;
    const std::size_t count = std::max(left.limbs_.size(), right.limbs_.size());
    out.limbs_.assign(count, 0u);
    std::uint64_t carry = 0u;
    for (std::size_t index = 0u; index < count; ++index)
    {
        const std::uint64_t a = (index < left.limbs_.size()) ? left.limbs_[index] : 0u;
        const std::uint64_t b = (index < right.limbs_.size()) ? right.limbs_[index] : 0u;
        const std::uint64_t value = a + b + carry;
        out.limbs_[index] = static_cast<std::uint32_t>(value & 0xFFFFFFFFull);
        carry = value >> 32u;
    }
    if (carry != 0u) out.limbs_.push_back(static_cast<std::uint32_t>(carry));
    out.normalize();
    if (!out.limbs_.empty()) out.sign_ = 1;
    return out;
}

BigInt BigInt::subMagnitude(const BigInt &left, const BigInt &right)
{
    BigInt out;
    out.limbs_.assign(left.limbs_.size(), 0u);
    std::int64_t borrow = 0;   /* 0 或 -1 */
    for (std::size_t index = 0u; index < left.limbs_.size(); ++index)
    {
        const std::int64_t a = static_cast<std::int64_t>(left.limbs_[index]);
        const std::int64_t b = (index < right.limbs_.size())
                                   ? static_cast<std::int64_t>(right.limbs_[index])
                                   : 0;
        std::int64_t value = a - b + borrow;
        if (value < 0)
        {
            value += static_cast<std::int64_t>(kLimbBase);
            borrow = -1;
        }
        else
        {
            borrow = 0;
        }
        out.limbs_[index] = static_cast<std::uint32_t>(value);
    }
    out.normalize();
    if (!out.limbs_.empty()) out.sign_ = 1;
    return out;
}

BigInt BigInt::mulMagnitude(const BigInt &left, const BigInt &right)
{
    BigInt out;
    if (left.limbs_.empty() || right.limbs_.empty()) return out;   /* 0 */
    /* 规模够大就换 NTT（O(n log n)）：15 位数字 → 双素数卷积 → CRT → 打包回 limb。 */
    if (left.limbs_.size() + right.limbs_.size() >= g_multiplicationThreshold)
    {
        const std::vector<std::uint32_t> a = toBase15(left.limbs_);
        const std::vector<std::uint32_t> b = toBase15(right.limbs_);
        const std::vector<std::uint32_t> residue1 = convolution<kNttPrime1>(a, b);
        const std::vector<std::uint32_t> residue2 = convolution<kNttPrime2>(a, b);
        const std::uint64_t prime1 = kNttPrime1;
        const std::uint64_t prime2 = kNttPrime2;
        /* CRT：x ≡ r1 (mod p1)、x ≡ r2 (mod p2) → x = r1 + p1 × t，
         * t = (r2 − r1) × p1^{-1} (mod p2)。 */
        const std::uint64_t inverse =
            powModU32<kNttPrime2>(static_cast<std::uint32_t>(prime1 % prime2), prime2 - 2u);
        const std::size_t count = std::max(residue1.size(), residue2.size());
        std::vector<std::uint64_t> coefficients(count, 0u);
        std::uint64_t carry = 0u;
        for (std::size_t index = 0u; index < count; ++index)
        {
            const std::uint64_t value1 = (index < residue1.size()) ? residue1[index] : 0u;
            const std::uint64_t value2 = (index < residue2.size()) ? residue2[index] : 0u;
            const std::uint64_t difference = (value2 + prime2 - (value1 % prime2)) % prime2;
            const std::uint64_t factor = (difference * inverse) % prime2;
            const std::uint64_t exact = value1 + prime1 * factor;   /* < p1 × p2 ≈ 2^60 */
            const std::uint64_t current = exact + carry;
            coefficients[index] = current & 0x7FFFull;              /* 低 15 位 */
            carry = current >> kDigitBits;
        }
        while (carry != 0u)
        {
            coefficients.push_back(carry & 0x7FFFull);
            carry >>= kDigitBits;
        }
        out.limbs_ = packBase15(coefficients);
        out.normalize();
        if (!out.limbs_.empty()) out.sign_ = 1;
        return out;
    }
    if (std::min(left.limbs_.size(), right.limbs_.size()) > g_karatsubaThreshold)
        out.limbs_ = karatsubaMultiply(left.limbs_, right.limbs_, g_karatsubaThreshold);
    else
        out.limbs_ = schoolbookMultiply(left.limbs_, right.limbs_);
    out.normalize();
    if (!out.limbs_.empty()) out.sign_ = 1;
    return out;
}

void BigInt::setMultiplicationThreshold(std::size_t limbs) { g_multiplicationThreshold = limbs; }
std::size_t BigInt::multiplicationThreshold() { return g_multiplicationThreshold; }
void BigInt::setKaratsubaThreshold(std::size_t limbs)
{
    g_karatsubaThreshold = std::max<std::size_t>(1u, limbs);
}
std::size_t BigInt::karatsubaThreshold() { return g_karatsubaThreshold; }
void BigInt::setDivisionThreshold(std::size_t limbs)
{
    g_divisionThreshold = std::max<std::size_t>(32u, limbs);
}
std::size_t BigInt::divisionThreshold() { return g_divisionThreshold; }

BigInt integerSqrt(const BigInt &value)
{
    if (value.sign() < 0) throw std::domain_error("bigint: 负数没有整数平方根");
    if (value.isZero()) return BigInt();
    /* 二分：先找一个 high 使 high² > value，再不断折半逼近 */
    BigInt low(0);
    BigInt high(1);
    while (high * high <= value) high *= BigInt(2);
    while (high - low > BigInt(1))
    {
        const BigInt middle = (low + high) / BigInt(2);
        if (middle * middle <= value) low = middle;
        else high = middle;
    }
    return low;
}

BigInt pow10(std::uint64_t exponent) { return BigInt(10).pow(exponent); }

BigInt BigInt::limbSlice(const BigInt &value, std::size_t offset, std::size_t count)
{
    BigInt out;
    if (offset >= value.limbs_.size() || count == 0u) return out;
    const std::size_t end = std::min(value.limbs_.size(), offset + count);
    out.limbs_.assign(value.limbs_.begin() + static_cast<std::ptrdiff_t>(offset),
                      value.limbs_.begin() + static_cast<std::ptrdiff_t>(end));
    out.normalize();
    if (!out.limbs_.empty()) out.sign_ = 1;
    return out;
}

BigInt BigInt::combineBlocks(const BigInt &high, const BigInt &low, std::size_t lowWidth)
{
    BigInt out;
    out.limbs_.assign(lowWidth + high.limbs_.size(), 0u);
    const std::size_t lowCount = std::min(lowWidth, low.limbs_.size());
    std::copy(low.limbs_.begin(), low.limbs_.begin() + static_cast<std::ptrdiff_t>(lowCount),
              out.limbs_.begin());
    std::copy(high.limbs_.begin(), high.limbs_.end(),
              out.limbs_.begin() + static_cast<std::ptrdiff_t>(lowWidth));
    out.normalize();
    if (!out.limbs_.empty()) out.sign_ = 1;
    return out;
}

void BigInt::divmodBase(const BigInt &left, const BigInt &right,
                        BigInt *quotient, BigInt *remainder)
{
    BigInt quotientValue;   /* 0 */
    BigInt remainderValue = left.abs();
    const BigInt divisor = right.abs();
    if (compareMagnitude(remainderValue, divisor) >= 0)
    {
        const std::size_t shift = remainderValue.bitLength() - divisor.bitLength();
        BigInt shifted = divisor;
        shifted.shiftLeft(shift);
        for (std::size_t step = shift + 1u; step-- > 0u;)
        {
            if (compareMagnitude(remainderValue, shifted) >= 0)
            {
                remainderValue = subMagnitude(remainderValue, shifted);
                quotientValue.setBit(step);
            }
            shifted.shiftRight(1u);
        }
        quotientValue.normalize();
        if (!quotientValue.limbs_.empty()) quotientValue.sign_ = 1;
    }
    if (quotient != nullptr) *quotient = quotientValue;
    if (remainder != nullptr) *remainder = remainderValue;
}

void BigInt::divThreeByTwo(const BigInt &dividend, const BigInt &divisor,
                           std::size_t blockSize, BigInt *quotient, BigInt *remainder)
{
    const BigInt a3 = limbSlice(dividend, 0u, blockSize);
    const BigInt a2 = limbSlice(dividend, blockSize, blockSize);
    const BigInt a1 = limbSlice(dividend, blockSize * 2u, blockSize);
    const BigInt b2 = limbSlice(divisor, 0u, blockSize);
    const BigInt b1 = limbSlice(divisor, blockSize, blockSize);

    BigInt quotientValue;
    BigInt remainderHigh;
    if (compareMagnitude(a1, b1) < 0)
    {
        divTwoByOne(combineBlocks(a1, a2, blockSize), b1, blockSize,
                    &quotientValue, &remainderHigh);
    }
    else
    {
        quotientValue.limbs_.assign(blockSize, std::numeric_limits<std::uint32_t>::max());
        quotientValue.sign_ = 1;
        remainderHigh = combineBlocks(subMagnitude(a1, b1), a2, blockSize) + b1;
    }

    BigInt remainderValue = combineBlocks(remainderHigh, a3, blockSize) -
                            mulMagnitude(quotientValue, b2);
    while (remainderValue.sign_ < 0)
    {
        remainderValue += divisor;
        quotientValue -= BigInt(1);
    }
    if (quotient != nullptr) *quotient = quotientValue;
    if (remainder != nullptr) *remainder = remainderValue;
}

void BigInt::divTwoByOne(const BigInt &dividend, const BigInt &divisor,
                         std::size_t blockSize, BigInt *quotient, BigInt *remainder)
{
    if (compareMagnitude(dividend, divisor) < 0)
    {
        if (quotient != nullptr) *quotient = BigInt();
        if (remainder != nullptr) *remainder = dividend;
        return;
    }
    if (blockSize <= g_divisionThreshold || (blockSize & 1u) != 0u)
    {
        divmodBase(dividend, divisor, quotient, remainder);
        return;
    }

    const std::size_t half = blockSize / 2u;
    const BigInt low = limbSlice(dividend, 0u, half);
    const BigInt upper = limbSlice(dividend, half, blockSize + half);
    BigInt quotientHigh;
    BigInt remainderHigh;
    divThreeByTwo(upper, divisor, half, &quotientHigh, &remainderHigh);
    BigInt quotientLow;
    BigInt remainderValue;
    divThreeByTwo(combineBlocks(remainderHigh, low, half), divisor, half,
                  &quotientLow, &remainderValue);
    if (quotient != nullptr) *quotient = combineBlocks(quotientHigh, quotientLow, half);
    if (remainder != nullptr) *remainder = remainderValue;
}

void BigInt::divmodMagnitude(const BigInt &left, const BigInt &right,
                             BigInt *quotient, BigInt *remainder)
{
    const BigInt dividend = left.abs();
    const BigInt divisor = right.abs();
    if (compareMagnitude(dividend, divisor) < 0)
    {
        if (quotient != nullptr) *quotient = BigInt();
        if (remainder != nullptr) *remainder = dividend;
        return;
    }
    if (divisor.limbs_.size() <= g_divisionThreshold)
    {
        divmodBase(dividend, divisor, quotient, remainder);
        return;
    }

    std::size_t blockSize = 1u;
    while (blockSize < divisor.limbs_.size()) blockSize <<= 1u;
    const std::size_t padding = blockSize - divisor.limbs_.size();
    std::size_t topShift = 0u;
    for (std::uint32_t top = divisor.limbs_.back(); (top & 0x80000000u) == 0u; top <<= 1u)
        ++topShift;
    const std::size_t normalizationBits = padding * 32u + topShift;
    BigInt normalizedDividend = dividend;
    BigInt normalizedDivisor = divisor;
    normalizedDividend.shiftLeft(normalizationBits);
    normalizedDivisor.shiftLeft(normalizationBits);

    BigInt quotientValue;
    BigInt remainderValue;
    const std::size_t blockCount =
        (normalizedDividend.limbs_.size() + blockSize - 1u) / blockSize;
    for (std::size_t block = blockCount; block-- > 0u;)
    {
        const BigInt chunk = limbSlice(normalizedDividend, block * blockSize, blockSize);
        const BigInt window = combineBlocks(remainderValue, chunk, blockSize);
        BigInt quotientBlock;
        divTwoByOne(window, normalizedDivisor, blockSize, &quotientBlock, &remainderValue);
        quotientValue = combineBlocks(quotientValue, quotientBlock, blockSize);
    }
    remainderValue.shiftRight(normalizationBits);
    quotientValue.normalize();
    remainderValue.normalize();
    if (!quotientValue.limbs_.empty()) quotientValue.sign_ = 1;
    if (!remainderValue.limbs_.empty()) remainderValue.sign_ = 1;
    if (quotient != nullptr) *quotient = quotientValue;
    if (remainder != nullptr) *remainder = remainderValue;
}

// ---------------------------------------------------------------------------
//  比较 / 算术
// ---------------------------------------------------------------------------

int BigInt::compare(const BigInt &other) const
{
    if (sign_ != other.sign_) return (sign_ < other.sign_) ? -1 : 1;
    const int magnitude = compareMagnitude(*this, other);
    return (sign_ >= 0) ? magnitude : -magnitude;
}

BigInt operator+(const BigInt &left, const BigInt &right)
{
    if (left.isZero()) return right;
    if (right.isZero()) return left;
    if (left.sign_ == right.sign_)
    {
        BigInt out = BigInt::addMagnitude(left, right);
        out.sign_ = left.sign_;
        return out;
    }
    const int magnitude = BigInt::compareMagnitude(left, right);
    if (magnitude == 0) return BigInt();
    if (magnitude > 0)
    {
        BigInt out = BigInt::subMagnitude(left, right);
        out.sign_ = left.sign_;
        return out;
    }
    BigInt out = BigInt::subMagnitude(right, left);
    out.sign_ = right.sign_;
    return out;
}

BigInt operator-(const BigInt &left, const BigInt &right) { return left + (-right); }

BigInt BigInt::operator-() const
{
    BigInt out = *this;
    out.sign_ = -out.sign_;
    return out;
}

BigInt operator*(const BigInt &left, const BigInt &right)
{
    if (left.isZero() || right.isZero()) return BigInt();
    BigInt out = BigInt::mulMagnitude(left, right);
    out.sign_ = left.sign_ * right.sign_;
    return out;
}

bool BigInt::divmod(const BigInt &divisor, BigInt *quotient, BigInt *remainder) const
{
    if (divisor.isZero()) return false;
    if (isZero())
    {
        if (quotient != nullptr) *quotient = BigInt();
        if (remainder != nullptr) *remainder = BigInt();
        return true;
    }
    BigInt q;
    BigInt r;
    BigInt::divmodMagnitude(*this, divisor, &q, &r);
    /* 向零截断：商的符号是两边符号的积，余数符号跟被除数 */
    if (!q.isZero())
    {
        q.sign_ = sign_ * divisor.sign_;
        /* |a| ≥ |b| 时商的幅值正确；否则上面已经是 0 */
    }
    if (!r.isZero()) r.sign_ = sign_;
    if (quotient != nullptr) *quotient = q;
    if (remainder != nullptr) *remainder = r;
    return true;
}

BigInt operator/(const BigInt &left, const BigInt &right)
{
    BigInt quotient;
    if (!left.divmod(right, &quotient, nullptr))
    {
        throw std::domain_error("bigint: 除以零");
    }
    return quotient;
}

BigInt operator%(const BigInt &left, const BigInt &right)
{
    BigInt remainder;
    if (!left.divmod(right, nullptr, &remainder))
    {
        throw std::domain_error("bigint: 除以零");
    }
    return remainder;
}

BigInt &BigInt::operator+=(const BigInt &other) { *this = *this + other; return *this; }
BigInt &BigInt::operator-=(const BigInt &other) { *this = *this - other; return *this; }
BigInt &BigInt::operator*=(const BigInt &other) { *this = *this * other; return *this; }
BigInt &BigInt::operator/=(const BigInt &other) { *this = *this / other; return *this; }
BigInt &BigInt::operator%=(const BigInt &other) { *this = *this % other; return *this; }

BigInt &BigInt::operator++() { *this += BigInt(1); return *this; }
BigInt BigInt::operator++(int) { BigInt before = *this; ++(*this); return before; }
BigInt &BigInt::operator--() { *this -= BigInt(1); return *this; }
BigInt BigInt::operator--(int) { BigInt before = *this; --(*this); return before; }

bool operator==(const BigInt &left, const BigInt &right) { return left.compare(right) == 0; }
bool operator!=(const BigInt &left, const BigInt &right) { return left.compare(right) != 0; }
bool operator<(const BigInt &left, const BigInt &right) { return left.compare(right) < 0; }
bool operator<=(const BigInt &left, const BigInt &right) { return left.compare(right) <= 0; }
bool operator>(const BigInt &left, const BigInt &right) { return left.compare(right) > 0; }
bool operator>=(const BigInt &left, const BigInt &right) { return left.compare(right) >= 0; }

// ---------------------------------------------------------------------------
//  幂 / 公约数 / 同余
// ---------------------------------------------------------------------------

BigInt BigInt::pow(std::uint64_t exponent) const
{
    BigInt result(1);
    BigInt base = *this;
    std::uint64_t remaining = exponent;
    while (remaining != 0u)
    {
        if ((remaining & 1u) != 0u) result *= base;
        remaining >>= 1u;
        if (remaining != 0u) base *= base;
    }
    return result;
}

BigInt BigInt::gcd(const BigInt &left, const BigInt &right)
{
    BigInt a = left.abs();
    BigInt b = right.abs();
    while (!b.isZero())
    {
        BigInt remainder;
        a.divmod(b, nullptr, &remainder);
        a = b;
        b = remainder;
    }
    return a;
}

BigInt BigInt::lcm(const BigInt &left, const BigInt &right)
{
    if (left.isZero() || right.isZero()) return BigInt();
    const BigInt divisor = gcd(left, right);
    BigInt out = (left.abs() / divisor) * right.abs();
    return out;
}

BigInt BigInt::powmod(std::uint64_t exponent, const BigInt &modulus) const
{
    if (modulus.isZero()) throw std::domain_error("bigint: 模数为零");
    BigInt result(1);
    result %= modulus;
    BigInt base = *this % modulus;
    if (!base.isZero() && base.sign_ < 0) base += modulus.abs();
    std::uint64_t remaining = exponent;
    while (remaining != 0u)
    {
        if ((remaining & 1u) != 0u) result = (result * base) % modulus;
        remaining >>= 1u;
        if (remaining != 0u) base = (base * base) % modulus;
    }
    if (!result.isZero() && result.sign_ < 0) result += modulus.abs();
    return result;
}

// ---------------------------------------------------------------------------
//  十进制互转 / 截断
// ---------------------------------------------------------------------------

std::string BigInt::toString() const
{
    if (isZero()) return "0";
    BigInt magnitude = abs();
    std::vector<std::uint32_t> chunks;
    while (!magnitude.isZero())
    {
        chunks.push_back(magnitude.divSmall(kDecimalChunk));
    }
    std::string text;
    if (sign_ < 0) text.push_back('-');
    text += std::to_string(chunks.back());
    for (std::size_t index = chunks.size() - 1u; index-- > 0u;)
    {
        std::string part = std::to_string(chunks[index]);
        text.append(9u - part.size(), '0');
        text += part;
    }
    return text;
}

std::size_t BigInt::decimalDigits() const
{
    if (isZero()) return 1u;
    BigInt magnitude = abs();
    std::size_t digits = 0u;
    while (!magnitude.isZero())
    {
        const std::uint32_t remainder = magnitude.divSmall(kDecimalChunk);
        digits += (magnitude.isZero()) ? std::to_string(remainder).size() : 9u;
    }
    return digits;
}

std::uint64_t BigInt::toU64() const
{
    std::uint64_t value = 0u;
    for (std::size_t index = std::min<std::size_t>(limbs_.size(), 2u); index-- > 0u;)
    {
        value = (value << 32u) | limbs_[index];
    }
    return value;
}

std::int64_t BigInt::toI64() const { return static_cast<std::int64_t>(toU64()); }

double BigInt::toDouble() const
{
    double value = 0.0;
    for (std::size_t index = limbs_.size(); index-- > 0u;)
    {
        value = value * 4294967296.0 + static_cast<double>(limbs_[index]);
    }
    return (sign_ < 0) ? -value : value;
}

} // namespace bigint
