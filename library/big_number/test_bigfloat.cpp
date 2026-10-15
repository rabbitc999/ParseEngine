/**
 * @file test_bigfloat.cpp
 * @brief `BigFloat` 单元测试：解析 / 输出、四则、比较、舍入、平方根与边界。
 */

#include "bigfloat.hpp"

#include <iostream>
#include <limits>
#include <string>

namespace
{
int checks = 0;
int failures = 0;
#define CHECK(condition)                                                          \
    do                                                                           \
    {                                                                            \
        ++checks;                                                                \
        if (!(condition))                                                        \
        {                                                                        \
            ++failures;                                                          \
            std::cerr << "  失败：" #condition "（第 " << __LINE__ << " 行）\n";  \
        }                                                                        \
    } while (false)

using bigfloat::BigFloat;

/** 解析与输出。 */
void testParseAndPrint()
{
    CHECK(BigFloat("0").isZero());
    CHECK(BigFloat("123.456").toString() == std::string("123.456"));
    CHECK(BigFloat("-0.5").toString() == std::string("-0.5"));
    CHECK(BigFloat(".5").toString() == std::string("0.5"));
    CHECK(BigFloat("1.").toString() == std::string("1"));
    CHECK(BigFloat("+42").toString() == std::string("42"));
    CHECK(BigFloat("  7  ").toString() == std::string("7"));
    CHECK(BigFloat("0.00123").toString() == std::string("0.00123"));
    CHECK(BigFloat("00123.4500").toString() == std::string("123.45"));   /* 尾随零剥掉 */
    CHECK(BigFloat("1e3").toString() == std::string("1000"));
    CHECK(BigFloat("2.5E-3").toString() == std::string("0.0025"));
    CHECK(BigFloat("1e100").toString() == std::string("1e+100"));
    CHECK(BigFloat("-4.5e-30").toString() == std::string("-4.5e-30"));
    CHECK(BigFloat("1.23456789e21").toString() == std::string("1.23456789e+21"));
    CHECK(BigFloat("1.23456789e21").toPlainString() == std::string("1234567890000000000000"));
    CHECK(BigFloat("0").toPlainString() == std::string("0"));
    CHECK(BigFloat("-0.5").toPlainString() == std::string("-0.5"));
    CHECK(BigFloat("1e20").toPlainString() == std::string("100000000000000000000"));
    CHECK(BigFloat("1e20").toString(0) == std::string("1e+20"));   /* 0 = 不舍入，仍按 %g 口径 */
    CHECK(BigFloat("00123.4500").toPlainString() == std::string("123.45"));
    CHECK(BigFloat::fromDouble(1.5).toString() == std::string("1.5"));
    CHECK(BigFloat::fromDouble(0.1).toString() == std::string("0.1"));
    CHECK(BigFloat::fromDouble(1.0 / 3.0).toString() == std::string("0.3333333333333333"));
    CHECK(BigFloat::fromDouble(1e300).toString() == std::string("1e+300"));
    CHECK(BigFloat::fromDouble(std::numeric_limits<double>::infinity()).isZero());
    CHECK(BigFloat::fromDouble(std::numeric_limits<double>::quiet_NaN()).isZero());

    bool ok = false;
    CHECK(BigFloat::fromString("12a", &ok).isZero());
    CHECK(!ok);
    CHECK(BigFloat::fromString("", &ok).isZero());
    CHECK(!ok);
    CHECK(BigFloat::fromString("1e", &ok).isZero());
    CHECK(!ok);
    CHECK(BigFloat::fromString("1e99999999", &ok).isZero());
    CHECK(!ok);
    CHECK(BigFloat::fromString("-0.0", &ok).isZero());
    CHECK(ok);

    CHECK(BigFloat::fromInt(bigint::BigInt("123456789012345678901234567890")).toPlainString() ==
          std::string("123456789012345678901234567890"));
    CHECK(BigFloat::fromInt(bigint::BigInt("-42")).toString() == std::string("-42"));
}

/** 十进制浮点的"精确"特性与四则。 */
void testArithmetic()
{
    /* 二进制浮点的经典坑：这里是十进制，精确等于 0.3 */
    CHECK((BigFloat("0.1") + BigFloat("0.2")).toString() == std::string("0.3"));
    CHECK((BigFloat::fromDouble(0.1) + BigFloat::fromDouble(0.2)).toString() ==
          std::string("0.3"));
    CHECK((BigFloat("1.5") + BigFloat("2.25")).toString() == std::string("3.75"));
    CHECK((BigFloat("1") - BigFloat("0.999")).toString() == std::string("0.001"));
    CHECK((BigFloat("-5") + BigFloat("3")).toString() == std::string("-2"));
    /* 1e20 + 1 精确等于 100000000000000000001（21 位）：缺省只显示 17 位所以是 1e+20，
     * 要看全就得换恒定位数记法（或显式给位数）。 */
    CHECK((BigFloat("1e20") + BigFloat("1")).toString() == std::string("1e+20"));
    CHECK((BigFloat("1e20") + BigFloat("1")).toPlainString() ==
          std::string("100000000000000000001"));
    CHECK((BigFloat("2.5") * BigFloat("4")).toString() == std::string("10"));
    CHECK((BigFloat("-2.5") * BigFloat("4")).toString() == std::string("-10"));
    CHECK((BigFloat("0.001") * BigFloat("1000")).toString() == std::string("1"));
    CHECK((BigFloat("1e100") * BigFloat("1e-100")).toString() == std::string("1"));
    CHECK((BigFloat("123456789012345678901234567890") * BigFloat("2")).toPlainString() ==
          std::string("246913578024691357802469135780"));

    CHECK((BigFloat("1") / BigFloat("3")).toString(20) ==
          std::string("0.33333333333333333333"));
    CHECK((BigFloat("2") / BigFloat("7")).toString(30) ==
          std::string("0.285714285714285714285714285714"));
    CHECK((BigFloat("10") / BigFloat("4")).toString() == std::string("2.5"));
    CHECK(BigFloat("6").divide(BigFloat("3"), 10).toString() == std::string("2"));

    BigFloat value("10.5");
    CHECK((value += BigFloat("0.25")).toString() == std::string("10.75"));
    CHECK((value *= BigFloat("2")).toString() == std::string("21.5"));
    CHECK((value /= BigFloat("2")).toString() == std::string("10.75"));
    CHECK((value -= BigFloat("0.75")).toString() == std::string("10"));
}

/** 舍入、比较与边界。 */
void testRoundCompare()
{
    CHECK(BigFloat("123.4567").round(5).toString() == std::string("123.46"));   // 四舍五入
    CHECK(BigFloat("123.4549").round(5).toString() == std::string("123.45"));
    CHECK(BigFloat("-1.25").round(2).toString() == std::string("-1.3"));        // 远离零
    CHECK(BigFloat("0.000999").toString(2) == std::string("0.001"));
    CHECK(BigFloat("999.9").round(3).toString() == std::string("1000"));        // 进位跨位

    CHECK(BigFloat("1.5") == BigFloat("1.50"));
    CHECK(BigFloat("-1.5") < BigFloat("1.5"));
    CHECK(BigFloat("0.001") < BigFloat("0.01"));
    CHECK(BigFloat("-1e100") < BigFloat("-1e99"));
    CHECK(BigFloat("1e100") > BigFloat("9.99e99"));
    CHECK(BigFloat("0") == BigFloat("-0.0"));
    CHECK(BigFloat("0") != BigFloat("1e-1000"));
    CHECK(BigFloat("2") >= BigFloat("2"));

    bool threw = false;
    try { (void)(BigFloat("1") / BigFloat("0")); } catch (const std::domain_error &) { threw = true; }
    CHECK(threw);
}

/** 平方根（保留 N 位有效数字，多算几位再舍入）。 */
void testSqrt()
{
    CHECK(BigFloat("4").sqrt(10).toString() == std::string("2"));
    CHECK(BigFloat("2").sqrt(11).toString() == std::string("1.4142135624"));
    CHECK(BigFloat("2").sqrt(12).toString() == std::string("1.41421356237"));
    CHECK(BigFloat("2").sqrt(30).toString(30) ==
          std::string("1.41421356237309504880168872421"));
    CHECK(BigFloat("0").sqrt(10).isZero());
    CHECK(BigFloat("1e100").sqrt(20).toString() == std::string("1e+50"));
    CHECK(BigFloat("0.0001").sqrt(10).toString() == std::string("0.01"));
    CHECK(BigFloat("6.25").sqrt(10).toString() == std::string("2.5"));

    bool threw = false;
    try { (void)BigFloat("-1").sqrt(10); } catch (const std::domain_error &) { threw = true; }
    CHECK(threw);
}

/** 大规模尾数运算：确认 BigFloat 真的吃到 `BigInt` 的 NTT 内核（与学校算法逐位比对）。 */
void testLargePrecision()
{
    const bigint::BigInt x = bigint::pow10(1500u) + bigint::BigInt(3);    /* 1501 位十进制 */
    const bigint::BigInt y = bigint::pow10(1400u) - bigint::BigInt(5);
    /* (10^1500 + 3) × (10^1400 − 5) = 10^2900 − 5×10^1500 + 3×10^1400 − 15 */
    const bigint::BigInt expected = bigint::pow10(2900u) - bigint::BigInt(5) * bigint::pow10(1500u) +
                                    bigint::BigInt(3) * bigint::pow10(1400u) - bigint::BigInt(15);

    bigint::BigInt::setMultiplicationThreshold(1u);          /* 永远走 NTT */
    const BigFloat ntt = BigFloat::fromInt(x) * BigFloat::fromInt(y);
    const BigFloat nttRoot = BigFloat::fromInt(bigint::pow10(200u)).sqrt(60);
    bigint::BigInt::setMultiplicationThreshold(1000000u);    /* 先关闭 NTT */
    bigint::BigInt::setKaratsubaThreshold(1000000u);         /* 再关闭 Karatsuba */
    const BigFloat schoolbook = BigFloat::fromInt(x) * BigFloat::fromInt(y);

    CHECK(ntt == schoolbook);
    CHECK(ntt.toPlainString() == expected.toString());
    CHECK(nttRoot.toPlainString() == bigint::pow10(100u).toString());   /* sqrt(10^200) == 10^100 */
    CHECK(ntt.mantissa().limbCount() >= 150u);   /* 尾数确实很大（约 2900 位十进制） */

    bigint::BigInt::setMultiplicationThreshold(48000u);   /* 换回实测缺省值 */
    bigint::BigInt::setKaratsubaThreshold(96u);
    CHECK(bigint::BigInt::multiplicationThreshold() == 48000u);
    CHECK(bigint::BigInt::karatsubaThreshold() == 96u);
}

} // namespace

int main()
{
    testParseAndPrint();
    testArithmetic();
    testRoundCompare();
    testSqrt();
    testLargePrecision();
    if (failures == 0)
    {
        std::cout << "bigfloat-tests：全部通过（" << checks << " 项断言）\n";
        return 0;
    }
    std::cout << "bigfloat-tests：" << failures << " / " << checks << " 项断言失败\n";
    return 1;
}
