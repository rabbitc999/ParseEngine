/**
 * @file bigint.hpp
 * @brief 任意精度大整数：加 / 减 / 乘 / 除 / 取余 / 幂 / 最大公约数，十进制字符串互转。
 *
 * 定位：`library/` 里的小工具库，C++17 + 只依赖标准库。实现参考
 * `eigenmath-plus/eigenmath_bigint.c`（同一套"符号 + limb 数组 + 有效位数"的结构），
 * 但把 C 的手工内存管理换成 `std::vector`，接口换成 C++ 的运算符重载。
 *
 * 表示：**基 2^32 的小端 limb 数组**（每个 limb 一个 `uint32_t`），
 *
 * ```text
 *   value = sign × Σ limbs[i] × 2^(32 i)
 * ```
 *
 * 约定（与参考实现一致）：
 *   - 0 只有一种表示：`sign == 0`、limbs 为空；
 *   - 高位不存 0 limb（每次运算后 `normalize()`）；
 *   - `a / b`、`a % b` 按**向零截断**（C / C++ 的整数除法语义），
 *     `a = (a / b) × b + a % b`，余数符号跟被除数；
 *   - 除以零：`divmod` 返回 false；`operator/`、`operator%` 抛 `std::domain_error`
 *     （不想处理异常就用 `divmod`）。
 *
 * 乘法内核按规模自动切换：小规模走学校算法，中等规模默认走 **Karatsuba**，超大规模走
 * **双素数 NTT + CRT**（O(n log n)）。
 * 除法默认走 **Burnikel-Ziegler**，除数不超过 56 limb 时回退移位相减长除法。
 *
 * 同一个库里还有十进制浮点 `bigfloat::BigFloat`（`bigfloat.hpp`，尾数就是这个 `BigInt`）。
 * 明确不做（参考实现里有、这里不做）：**分数 / 大有理数**
 * （`BigRational_T`）、以及 `BigFloat` 的 `exp / log / π / 三角函数`。
 */

#ifndef PARSE_ENGINE_BIGINT_HPP
#define PARSE_ENGINE_BIGINT_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace bigint
{

/**
 * @brief 任意精度有符号大整数。
 */
class BigInt
{
public:
    /** @brief 造一个 0。 */
    BigInt() = default;

    /**
     * @brief 从 64 位有符号整数构造（`INT64_MIN` 也正确）。
     * @param value 值。
     */
    BigInt(std::int64_t value);

    /**
     * @brief 从 64 位**无符号**整数构造（`uint64_t` 上界也不会被当成 -1）。
     *
     * 与参考实现的 `EigenMath_BigIntFromU64` 对应：`int64_t` 构造只吃有符号范围，
     * 大于 `INT64_MAX` 的无符号值请显式用这个。
     * @param value 值。
     */
    static BigInt fromU64(std::uint64_t value);

    /**
     * @brief 从十进制字符串构造（可带 `+` / `-` 号，允许前导 0）。
     * @param decimal 十进制文本。
     * @throws std::invalid_argument 文本不是合法十进制整数时。
     */
    explicit BigInt(const std::string &decimal);

    /**
     * @brief 从十进制字符串构造（不抛异常版）。
     * @param decimal 十进制文本。
     * @param ok 非空时写入是否解析成功。
     * @return 解析成功时的值；失败返回 0。
     */
    static BigInt fromString(const std::string &decimal, bool *ok = nullptr);

    /** @brief 十进制字符串（负数带 `-`；0 输出 `"0"`）。 */
    [[nodiscard]] std::string toString() const;

    /** @brief 是不是 0。 */
    [[nodiscard]] bool isZero() const { return sign_ == 0; }

    /** @brief 符号：-1 / 0 / 1。 */
    [[nodiscard]] int sign() const { return sign_; }

    /** @brief 十进制位数（0 算 1 位）。 */
    [[nodiscard]] std::size_t decimalDigits() const;

    /** @brief 有效 limb（32 位）个数；0 是 0 个。 */
    [[nodiscard]] std::size_t limbCount() const { return limbs_.size(); }

    /**
     * @brief NTT 切换阈值：两边 limb 个数之和达到它就改用 NTT。
     *
     * 默认 48000；测试和调参可以直接改（`1` = 永远走 NTT）。
     */
    static void setMultiplicationThreshold(std::size_t limbs);
    /** @brief 当前 NTT 阈值。 */
    [[nodiscard]] static std::size_t multiplicationThreshold();

    /**
     * @brief Karatsuba 基线阈值：较短操作数超过该 limb 数时使用 Karatsuba。
     *
     * 默认 96；设置为很大可强制使用学校算法，0 会按最小安全值 1 处理。
     */
    static void setKaratsubaThreshold(std::size_t limbs);
    /** @brief 当前 Karatsuba 基线阈值。 */
    [[nodiscard]] static std::size_t karatsubaThreshold();

    /**
     * @brief Burnikel-Ziegler 除法的递归基线；除数不超过该 limb 数时使用长除法。
     *
     * 默认 56、最小 32；设置为很大可强制使用长除法。
     */
    static void setDivisionThreshold(std::size_t limbs);
    /** @brief 当前 Burnikel-Ziegler 递归基线。 */
    [[nodiscard]] static std::size_t divisionThreshold();

    /** @brief 截断成 64 位无符号整数（只取低 64 位，符号位丢弃）。 */
    [[nodiscard]] std::uint64_t toU64() const;

    /** @brief 截断成 64 位有符号整数（按位截断，与 C 的强转一致）。 */
    [[nodiscard]] std::int64_t toI64() const;

    /** @brief 转双精度浮点（可能丢精度；符号跟着走）。 */
    [[nodiscard]] double toDouble() const;

    /** @brief 绝对值。 */
    [[nodiscard]] BigInt abs() const;

    /**
     * @brief 比较。
     * @return 小于 / 等于 / 大于分别返回 -1 / 0 / 1。
     */
    [[nodiscard]] int compare(const BigInt &other) const;

    // ---- 算术（都是值语义，不修改操作数）----
    friend BigInt operator+(const BigInt &left, const BigInt &right);
    friend BigInt operator-(const BigInt &left, const BigInt &right);
    friend BigInt operator*(const BigInt &left, const BigInt &right);
    /** @throws std::domain_error 除数为零时。 */
    friend BigInt operator/(const BigInt &left, const BigInt &right);
    /** @throws std::domain_error 除数为零时。 */
    friend BigInt operator%(const BigInt &left, const BigInt &right);

    /** @brief 一元负号。 */
    [[nodiscard]] BigInt operator-() const;

    BigInt &operator+=(const BigInt &other);
    BigInt &operator-=(const BigInt &other);
    BigInt &operator*=(const BigInt &other);
    BigInt &operator/=(const BigInt &other);
    BigInt &operator%=(const BigInt &other);

    BigInt &operator++();      ///< 前置 ++（`++a`）。
    BigInt operator++(int);    ///< 后置 ++（`a++`）。
    BigInt &operator--();      ///< 前置 --。
    BigInt operator--(int);    ///< 后置 --。

    friend bool operator==(const BigInt &left, const BigInt &right);
    friend bool operator!=(const BigInt &left, const BigInt &right);
    friend bool operator<(const BigInt &left, const BigInt &right);
    friend bool operator<=(const BigInt &left, const BigInt &right);
    friend bool operator>(const BigInt &left, const BigInt &right);
    friend bool operator>=(const BigInt &left, const BigInt &right);

    /**
     * @brief 同号幂：`*this ^ exponent`（二进制快速幂，`0^0 = 1`）。
     * @param exponent 指数（无符号）。
     */
    [[nodiscard]] BigInt pow(std::uint64_t exponent) const;

    /**
     * @brief 除取整：`this / divisor` 与余数（向零截断，`this = q × divisor + r`）。
     * @param divisor 除数。
     * @param quotient 非空时写入商。
     * @param remainder 非空时写入余数。
     * @return 除数为零返回 false（此时不写输出）。
     */
    bool divmod(const BigInt &divisor, BigInt *quotient, BigInt *remainder) const;

    /**
     * @brief 最大公约数（欧几里得，取绝对值；`gcd(0, 0) = 0`）。
     */
    [[nodiscard]] static BigInt gcd(const BigInt &left, const BigInt &right);

    /** @brief 最小公倍数（`lcm(0, x) = 0`）。 */
    [[nodiscard]] static BigInt lcm(const BigInt &left, const BigInt &right);

    /**
     * @brief 同余幂：`base ^ exponent mod modulus`（快速幂，逐步取模；模为零抛异常）。
     * @param exponent 指数（无符号）。
     * @param modulus 模数。
     */
    [[nodiscard]] BigInt powmod(std::uint64_t exponent, const BigInt &modulus) const;

private:
    int sign_ = 0;                        ///< -1 / 0 / 1
    std::vector<std::uint32_t> limbs_;    ///< 小端 32 位 limb（高位不存 0）

    /** @brief 去掉高位 0 并同步符号（0 时 limbs 清空）。 */
    void normalize();

    /** @brief 幅值比较：|a| 与 |b|。 */
    static int compareMagnitude(const BigInt &left, const BigInt &right);
    /** @brief 幅值相加（都按非负处理）。 */
    static BigInt addMagnitude(const BigInt &left, const BigInt &right);
    /** @brief 幅值相减：要求 |left| ≥ |right|。 */
    static BigInt subMagnitude(const BigInt &left, const BigInt &right);
    /** @brief 幅值乘法（学校算法，O(n×m)）。 */
    static BigInt mulMagnitude(const BigInt &left, const BigInt &right);
    /** @brief 幅值除取整（默认 Burnikel-Ziegler，小规模回退长除法）。 */
    static void divmodMagnitude(const BigInt &left, const BigInt &right,
                               BigInt *quotient, BigInt *remainder);
    /** @brief 长除法递归基线。 */
    static void divmodBase(const BigInt &left, const BigInt &right,
                           BigInt *quotient, BigInt *remainder);
    /** @brief Burnikel-Ziegler 的 2n/1n 子问题。 */
    static void divTwoByOne(const BigInt &dividend, const BigInt &divisor,
                            std::size_t blockSize, BigInt *quotient, BigInt *remainder);
    /** @brief Burnikel-Ziegler 的 3n/2n 子问题。 */
    static void divThreeByTwo(const BigInt &dividend, const BigInt &divisor,
                              std::size_t blockSize, BigInt *quotient, BigInt *remainder);
    /** @brief 取一段小端 limb。 */
    static BigInt limbSlice(const BigInt &value, std::size_t offset, std::size_t count);
    /** @brief `high * base^lowWidth + low`。 */
    static BigInt combineBlocks(const BigInt &high, const BigInt &low, std::size_t lowWidth);

    /** @brief 二进制位长度（0 返回 0）。 */
    [[nodiscard]] std::size_t bitLength() const;
    /** @brief 取第 bit 位是不是 1。 */
    [[nodiscard]] bool bitAt(std::size_t bit) const;
    /** @brief 把第 bit 位置 1。 */
    void setBit(std::size_t bit);
    /** @brief 左移 bits 位（就地）。 */
    void shiftLeft(std::size_t bits);
    /** @brief 右移 bits 位（就地）。 */
    void shiftRight(std::size_t bits);
    /** @brief 幅值 = 幅值 × small + add（small < 2^32）。 */
    void mulSmallAdd(std::uint32_t small, std::uint32_t add);
    /** @brief 幅值 / small 并返回余数（small ≠ 0）。 */
    std::uint32_t divSmall(std::uint32_t small);

    /** @brief 把十进制文本按 10^9 一段一段地吃进来（`ok` 见 fromString）。 */
    static BigInt parseDecimal(const std::string &decimal, bool *ok);
};

/**
 * @brief 整数平方根（向下取整）：二分 / 牛顿迭代，只用大整数的加减乘除。
 *
 * 大浮点的 `sqrt` 靠它（先放大到 2×digits 位有效数字再开方）。
 * @param value 非负整数；为负时抛 `std::domain_error`。
 */
[[nodiscard]] BigInt integerSqrt(const BigInt &value);

/** @brief 10 的 `exponent` 次幂（大浮点对齐指数时大量使用；幂用 NTT 乘法）。 */
[[nodiscard]] BigInt pow10(std::uint64_t exponent);

} // namespace bigint

#endif // PARSE_ENGINE_BIGINT_HPP
