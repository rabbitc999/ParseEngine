/**
 * @file bigfloat.hpp
 * @brief 任意精度**十进制浮点**：`mant × 10^exp10`，尾数复用 `bigint::BigInt`。
 *
 * 表示与参考实现 `eigenmath-plus/eigenmath_bigint.c` 的 `BigFloat_T` 一致：
 * 尾数是任意精度整数、指数是十进制（**二进制浮点不会有 0.1 + 0.2 ≠ 0.3 那种事**），
 * 符号存在尾数里。
 *
 * 已实现：解析 / 输出（保留 N 位有效数字 + 恒定位数记法）、加减乘除、比较、
 * 舍入、平方根。没实现：`exp / log / π / sin / cos / tan / atan / asin /
 * acos` —— 那些是另一层（级数 + AGM），要做得单独一轮。
 * **分数（`BigRational`）明确不做**（人拍板）：要精确有理数就是另一个类，
 * 本库的定位是"整数 + 十进制浮点"。
 */

#ifndef PARSE_ENGINE_BIGFLOAT_HPP
#define PARSE_ENGINE_BIGFLOAT_HPP

#include "bigint.hpp"

#include <cstdint>
#include <string>

namespace bigfloat
{

/**
 * @brief 任意精度十进制浮点数。
 */
class BigFloat
{
public:
    /** @brief 造一个 0。 */
    BigFloat() = default;

    /**
     * @brief 从十进制文本构造：`123.456` / `-0.5` / `.5` / `1e100` / `2.5E-3` 都行。
     * @param text 文本。
     * @throws std::invalid_argument 文本不是合法十进制小数时。
     */
    explicit BigFloat(const std::string &text);

    /**
     * @brief 从十进制文本构造（不抛异常版）。
     * @param text 文本。
     * @param ok 非空时写入是否解析成功。
     */
    static BigFloat fromString(const std::string &text, bool *ok = nullptr);

    /** @brief 从整数构造（值就是那个整数）。 */
    static BigFloat fromInt(const bigint::BigInt &value);

    /**
     * @brief 从 `double` 构造：取**能往返同一个 double 的最短十进制写法**。
     *
     * 参考实现的 `EigenMath_BigFloatFromDouble` 直接 `%.17g`，于是 `0.1` 会变成
     * `0.10000000000000001`；这里改成从 1 位起逐位试 `%.*g`、用 `strtod` 读回来
     * 比对，第一个能往返的位数就是结果 —— 所以 `fromDouble(0.1).toString() == "0.1"`，
     * 而 `fromDouble(0.1) + fromDouble(0.2)` 精确等于 `"0.3"`。
     * 非有限值（NaN / ±inf）按 0 处理（与参考实现一致）。
     * 注意：内部用 `snprintf` / `strtod`，跟 `printf` 一样**认当前 locale 的小数点**
     * （缺省的 `"C"` locale 下就是 `.`）；要跨 locale 稳定就自己先解析好文本。
     * @param value 值。
     * @param digits 有效位数**上限**（缺省 17 = double 的往返精度；只在 1..17 内生效）。
     */
    static BigFloat fromDouble(double value, int digits = 17);

    /** @brief 尾数（符号在里面）。 */
    [[nodiscard]] const bigint::BigInt &mantissa() const { return mant_; }
    /** @brief 十进制指数：值 = 尾数 × 10^指数。 */
    [[nodiscard]] int exponent10() const { return exp10_; }

    /** @brief 是不是 0。 */
    [[nodiscard]] bool isZero() const { return mant_.isZero(); }
    /** @brief 符号：-1 / 0 / 1。 */
    [[nodiscard]] int sign() const { return mant_.sign(); }

    /**
     * @brief 舍入到 `digits` 位有效数字（四舍五入、远离零；`digits ≤ 0` 抛异常）。
     * @param digits 有效位数。
     */
    [[nodiscard]] BigFloat round(int digits) const;

    /**
     * @brief 十进制文本，保留 `digits` 位有效数字。
     *
     * 指数不大时输出普通记法（`123.456`、`0.00123`），指数太大或太小改用
     * 科学记数法（`1.23e+100`、`4.5e-30`）—— 与 `%.17g` 同一口径。
     * @param digits 有效位数；`≤ 0` 表示**不舍入**（按值本身精确输出）。
     *        缺省 17，与 `%.17g` 同量级（所以 `1e20 + 1` 会显示成 `1e+20`）。
     */
    [[nodiscard]] std::string toString(int digits = 17) const;

    /**
     * @brief 十进制文本，**恒定用普通记法**（不切科学记数法）。
     *
     * 对应参考实现的 `EigenMath_BigFloatToDecimalString`（"供 infix / LaTeX 输出用"）：
     * `BigFloat("1e20").toPlainString()` → `"100000000000000000000"`，
     * `(BigFloat("1e20") + BigFloat("1")).toPlainString()` → `"100000000000000000001"`。
     * @param digits 有效位数；`≤ 0`（缺省）表示不舍入、按值本身精确输出。
     */
    [[nodiscard]] std::string toPlainString(int digits = 0) const;

    /** @brief 比较：-1 / 0 / 1。 */
    [[nodiscard]] int compare(const BigFloat &other) const;

    friend BigFloat operator+(const BigFloat &left, const BigFloat &right);
    friend BigFloat operator-(const BigFloat &left, const BigFloat &right);
    friend BigFloat operator*(const BigFloat &left, const BigFloat &right);
    /** @brief 除法：结果保留 `digits` 位有效数字（除数为 0 抛 `std::domain_error`）。 */
    friend BigFloat operator/(const BigFloat &left, const BigFloat &right);

    /** @brief 一元负号。 */
    [[nodiscard]] BigFloat operator-() const;

    BigFloat &operator+=(const BigFloat &other);
    BigFloat &operator-=(const BigFloat &other);
    BigFloat &operator*=(const BigFloat &other);
    BigFloat &operator/=(const BigFloat &other);

    friend bool operator==(const BigFloat &left, const BigFloat &right);
    friend bool operator!=(const BigFloat &left, const BigFloat &right);
    friend bool operator<(const BigFloat &left, const BigFloat &right);
    friend bool operator<=(const BigFloat &left, const BigFloat &right);
    friend bool operator>(const BigFloat &left, const BigFloat &right);
    friend bool operator>=(const BigFloat &left, const BigFloat &right);

    /**
     * @brief 除法并指定有效位数（`operator/` 用 34 位）。
     * @param divisor 除数。
     * @param digits 商保留的有效位数。
     */
    [[nodiscard]] BigFloat divide(const BigFloat &divisor, int digits) const;

    /**
     * @brief 平方根，结果保留 `digits` 位有效数字。
     * @param digits 有效位数。
     * @throws std::domain_error 负数开方时。
     */
    [[nodiscard]] BigFloat sqrt(int digits) const;

    /** @brief 调整成"尾数去掉十进制尾随零"的规范形（内部用）。 */
    void normalize();

private:
    bigint::BigInt mant_;    ///< 尾数（符号在里面）。
    int exp10_ = 0;          ///< 十进制指数。
};

} // namespace bigfloat

#endif // PARSE_ENGINE_BIGFLOAT_HPP
