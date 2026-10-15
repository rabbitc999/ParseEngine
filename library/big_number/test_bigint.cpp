/**
 * @file test_bigint.cpp
 * @brief `bigint` 单元测试：构造 / 互转、比较、四则、幂、gcd、同余幂与边界。
 */

#include "bigint.hpp"

#include <iostream>
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

using bigint::BigInt;

/** 构造 / 十进制互转 / 截断。 */
void testConstruct()
{
    CHECK(BigInt().toString() == std::string("0"));
    CHECK(BigInt(0).isZero());
    CHECK(BigInt(42).toString() == std::string("42"));
    CHECK(BigInt(-42).toString() == std::string("-42"));
    CHECK(BigInt(4294967296ll).toString() == std::string("4294967296"));   // 2^32
    CHECK(BigInt(-9223372036854775807ll - 1).toString() ==
          std::string("-9223372036854775808"));                            // INT64_MIN

    CHECK(BigInt("0").isZero());
    CHECK(BigInt("+123").toString() == std::string("123"));
    CHECK(BigInt("-000123").toString() == std::string("-123"));
    CHECK(BigInt("1606938044258990275541962092341162602522202993782792835301376").toString() ==
          std::string("1606938044258990275541962092341162602522202993782792835301376"));  // 2^200

    bool ok = false;
    CHECK(BigInt::fromString("12a", &ok).isZero());
    CHECK(!ok);
    CHECK(BigInt::fromString("", &ok).isZero());
    CHECK(!ok);
    CHECK(BigInt::fromString("-", &ok).isZero());
    CHECK(!ok);
    CHECK(BigInt::fromString("9999999999999999999999999999", &ok).toString() ==
          std::string("9999999999999999999999999999"));
    CHECK(ok);

    CHECK(BigInt("1606938044258990275541962092341162602522202993782792835301376").toU64() == 0u);
    CHECK(BigInt::fromU64(18446744073709551615ull).toString() ==
          std::string("18446744073709551615"));
    CHECK(BigInt::fromU64(18446744073709551615ull).toU64() == 18446744073709551615ull);
    CHECK(BigInt::fromU64(0u).isZero());
    CHECK(BigInt::fromU64(4294967296ull).toString() == std::string("4294967296"));
    CHECK(BigInt(-1).toU64() == 1u);                    // 只看幅值
    CHECK(BigInt(12345).toDouble() == 12345.0);
    CHECK(BigInt(-12345).toDouble() == -12345.0);
    CHECK(BigInt("1606938044258990275541962092341162602522202993782792835301376").decimalDigits() ==
          61u);
    CHECK(BigInt(0).decimalDigits() == 1u);
    CHECK(BigInt(1).limbCount() == 1u);
    CHECK(BigInt(0).limbCount() == 0u);
}

/** 比较与符号。 */
void testCompare()
{
    CHECK(BigInt(-5).compare(BigInt(3)) == -1);
    CHECK(BigInt(5).compare(BigInt(-3)) == 1);
    CHECK(BigInt(-5).compare(BigInt(-3)) == -1);
    CHECK(BigInt(5).compare(BigInt(5)) == 0);
    CHECK(BigInt(0).compare(BigInt(0)) == 0);
    CHECK(BigInt(0) < BigInt(1));
    CHECK(BigInt(-1) < BigInt(0));
    CHECK(BigInt("100000000000000000000000") > BigInt("99999999999999999999999"));
    CHECK(BigInt(7) != BigInt(8));
    CHECK(BigInt(7) >= BigInt(7));
    CHECK(BigInt(-7) <= BigInt(-7));
    CHECK(BigInt(-9).abs() == BigInt(9));
    CHECK(BigInt(9).sign() == 1);
    CHECK(BigInt(-9).sign() == -1);
    CHECK(BigInt(0).sign() == 0);
}

/** 加减乘（含进位 / 借位 / 符号交叉）。 */
void testArithmetic()
{
    CHECK((BigInt(2) + BigInt(3)).toString() == std::string("5"));
    CHECK((BigInt(4294967295ll) + BigInt(1)).toString() == std::string("4294967296"));
    CHECK((BigInt(-5) + BigInt(3)).toString() == std::string("-2"));
    CHECK((BigInt(5) + BigInt(-3)).toString() == std::string("2"));
    CHECK((BigInt(-5) + BigInt(5)).isZero());
    CHECK((BigInt(0) + BigInt(-7)).toString() == std::string("-7"));

    CHECK((BigInt(2) - BigInt(3)).toString() == std::string("-1"));
    CHECK((BigInt(-2) - BigInt(-3)).toString() == std::string("1"));
    CHECK((BigInt(4294967296ll) - BigInt(1)).toString() == std::string("4294967295"));
    CHECK((BigInt(3) - BigInt(3)).isZero());

    CHECK((BigInt(6) * BigInt(7)).toString() == std::string("42"));
    CHECK((BigInt(-6) * BigInt(7)).toString() == std::string("-42"));
    CHECK((BigInt(-6) * BigInt(-7)).toString() == std::string("42"));
    CHECK((BigInt(0) * BigInt(999)).isZero());
    CHECK((BigInt(65536) * BigInt(65536)).toString() == std::string("4294967296"));
    CHECK((BigInt("12345678901234567890") * BigInt("98765432109876543210")).toString() ==
          std::string("1219326311370217952237463801111263526900"));
    CHECK((BigInt("99999999999999999999") * BigInt("99999999999999999999")).toString() ==
          std::string("9999999999999999999800000000000000000001"));

    BigInt value(10);
    CHECK((value += BigInt(5)).toString() == std::string("15"));
    CHECK((value -= BigInt(20)).toString() == std::string("-5"));
    CHECK((value *= BigInt(-2)).toString() == std::string("10"));
    CHECK((++value).toString() == std::string("11"));
    CHECK((value++).toString() == std::string("11"));
    CHECK(value.toString() == std::string("12"));
    CHECK((--value).toString() == std::string("11"));
    CHECK((value--).toString() == std::string("11"));
    CHECK(value.toString() == std::string("10"));
}

/** 除取整（向零截断）、余数符号、除零。 */
void testDivision()
{
    BigInt quotient;
    BigInt remainder;
    CHECK(BigInt(7).divmod(BigInt(2), &quotient, &remainder));
    CHECK(quotient.toString() == std::string("3"));
    CHECK(remainder.toString() == std::string("1"));
    CHECK((BigInt(-7) / BigInt(2)).toString() == std::string("-3"));    // 向零截断
    CHECK((BigInt(-7) % BigInt(2)).toString() == std::string("-1"));    // 余数跟被除数
    CHECK((BigInt(7) / BigInt(-2)).toString() == std::string("-3"));
    CHECK((BigInt(7) % BigInt(-2)).toString() == std::string("1"));
    CHECK((BigInt(0) / BigInt(5)).isZero());
    CHECK((BigInt(3) / BigInt(1000)).isZero());
    CHECK((BigInt(3) % BigInt(1000)).toString() == std::string("3"));

    CHECK(BigInt(12345).divmod(BigInt(), &quotient, &remainder) == false);   // 除零
    bool threw = false;
    try { (void)(BigInt(1) / BigInt(0)); } catch (const std::domain_error &) { threw = true; }
    CHECK(threw);
    threw = false;
    try { (void)(BigInt(1) % BigInt(0)); } catch (const std::domain_error &) { threw = true; }
    CHECK(threw);

    const BigInt big("1606938044258990275541962092341162602522202993782792835301376");  // 2^200
    const BigInt divisor("12345678901234567890");
    CHECK((big / divisor).toString() ==
          std::string("130161982756436057245270848995428623856352"));
    CHECK((big % divisor).toString() == std::string("6540610052683564096"));
    CHECK((big / divisor) * divisor + (big % divisor) == big);   // 恒等式
    CHECK((big / big).toString() == std::string("1"));
    CHECK((big % big).isZero());
    CHECK((big / BigInt("999999999999999999999999999999")) * BigInt("999999999999999999999999999999") +
              (big % BigInt("999999999999999999999999999999")) ==
          big);

    /* 强制 Burnikel-Ziegler 与长除法分别计算同一组非 2 次幂长度数据。 */
    const BigInt bzDivisor = BigInt(2).pow(2050u) + BigInt(123456789);
    const BigInt expectedQuotient = BigInt(3).pow(800u) + BigInt(77);
    const BigInt expectedRemainder = BigInt(2).pow(1024u) + BigInt(99);
    const BigInt bzDividend = bzDivisor * expectedQuotient + expectedRemainder;
    BigInt::setDivisionThreshold(32u);
    BigInt bzQuotient;
    BigInt bzRemainder;
    CHECK(bzDividend.divmod(bzDivisor, &bzQuotient, &bzRemainder));
    BigInt::setDivisionThreshold(1000000u);
    BigInt baseQuotient;
    BigInt baseRemainder;
    CHECK(bzDividend.divmod(bzDivisor, &baseQuotient, &baseRemainder));
    CHECK(bzQuotient == baseQuotient && bzQuotient == expectedQuotient);
    CHECK(bzRemainder == baseRemainder && bzRemainder == expectedRemainder);
    CHECK((-bzDividend) / bzDivisor == -expectedQuotient);
    CHECK((-bzDividend) % bzDivisor == -expectedRemainder);
    BigInt::setDivisionThreshold(56u);
    CHECK(BigInt::divisionThreshold() == 56u);
}

/** 幂、gcd / lcm、同余幂。 */
/** 乘法内核：学校算法、Karatsuba 与 NTT 三条路径必须逐位一致（阈值可调）。 */
void testNtt()
{
    /* 造几个"够大"的数：2^1000、10^100、随机形状的 limb 序列 */
    const BigInt big = BigInt(2).pow(1000);
    const BigInt ten = BigInt(10).pow(100);
    BigInt randomish("1234567890123456789012345678901234567890123456789012345678901234567890"
                     "9876543210987654321098765432109876543210987654321098765432109876543210");

    /* 依次强制 NTT、Karatsuba、学校算法，三批结果逐位比对。 */
    BigInt::setMultiplicationThreshold(1u);
    const BigInt ntt1 = big * ten;
    const BigInt ntt2 = big * randomish;
    const BigInt ntt3 = randomish * randomish;
    const BigInt ntt4 = BigInt(2).pow(4096) * BigInt(2).pow(4096);
    BigInt::setMultiplicationThreshold(1000000u);
    BigInt::setKaratsubaThreshold(1u);
    const BigInt karatsuba1 = big * ten;
    const BigInt karatsuba2 = big * randomish;
    const BigInt karatsuba3 = randomish * randomish;
    const BigInt karatsuba4 = BigInt(2).pow(4096) * BigInt(2).pow(4096);
    BigInt::setKaratsubaThreshold(1000000u);
    const BigInt schoolbook1 = big * ten;
    const BigInt schoolbook2 = big * randomish;
    const BigInt schoolbook3 = randomish * randomish;
    const BigInt schoolbook4 = BigInt(2).pow(4096) * BigInt(2).pow(4096);
    CHECK(ntt1 == karatsuba1 && karatsuba1 == schoolbook1);
    CHECK(ntt2 == karatsuba2 && karatsuba2 == schoolbook2);
    CHECK(ntt3 == karatsuba3 && karatsuba3 == schoolbook3);
    CHECK(ntt4 == karatsuba4 && karatsuba4 == schoolbook4);

    /* NTT 路径也要和"人工已知值"一致 */
    BigInt::setMultiplicationThreshold(1u);
    CHECK(ntt4 == BigInt(2).pow(8192));
    CHECK(ntt1 == BigInt(2).pow(1000) * BigInt(10).pow(100));
    CHECK((ntt3 / randomish) == randomish);
    CHECK((ntt3 % randomish).isZero());

    /* 大数除法 / 取余在 NTT 乘法下依然自洽：q × b + r == a */
    const BigInt divisor = BigInt(3).pow(200);
    const BigInt quotient = ntt2 / divisor;
    const BigInt remainder = ntt2 % divisor;
    CHECK(quotient * divisor + remainder == ntt2);
    CHECK(remainder < divisor);

    /* 换回默认阈值，后面的用例不受影响 */
    BigInt::setMultiplicationThreshold(48000u);
    BigInt::setKaratsubaThreshold(96u);
    CHECK(BigInt::multiplicationThreshold() == 48000u);
    CHECK(BigInt::karatsubaThreshold() == 96u);
}

void testPowerAndGcd()
{
    CHECK(BigInt(2).pow(10).toString() == std::string("1024"));
    CHECK(BigInt(2).pow(0).toString() == std::string("1"));
    CHECK(BigInt(0).pow(0).toString() == std::string("1"));
    CHECK(BigInt(0).pow(5).isZero());
    CHECK(BigInt(-3).pow(3).toString() == std::string("-27"));
    CHECK(BigInt(2).pow(200).toString() ==
          std::string("1606938044258990275541962092341162602522202993782792835301376"));
    CHECK(BigInt(3).pow(40).toString() == std::string("12157665459056928801"));

    const BigInt left = BigInt(2).pow(100) * BigInt(3);
    const BigInt right = BigInt(3).pow(50) * BigInt(5);
    CHECK(BigInt::gcd(left, right).toString() == std::string("3"));
    CHECK(BigInt::gcd(BigInt(0), BigInt(0)).isZero());
    CHECK(BigInt::gcd(BigInt(0), BigInt(-12)).toString() == std::string("12"));
    CHECK(BigInt::gcd(BigInt(-18), BigInt(24)).toString() == std::string("6"));
    CHECK(BigInt::lcm(BigInt(4), BigInt(6)).toString() == std::string("12"));
    CHECK(BigInt::lcm(BigInt(0), BigInt(7)).isZero());
    CHECK(BigInt::lcm(left, right).toString() ==
          std::string("4550219075001074886663792637671283162463576301628293120"));

    CHECK(BigInt(7).powmod(300, BigInt(1000000007)).toString() == std::string("678144625"));
    CHECK(BigInt(2).powmod(10, BigInt(1000)).toString() == std::string("24"));
    CHECK(BigInt(-2).powmod(3, BigInt(5)).toString() == std::string("2"));   // (−8) mod 5
    CHECK(BigInt(123).powmod(0, BigInt(97)).toString() == std::string("1"));
}

} // namespace

int main()
{
    testConstruct();
    testCompare();
    testArithmetic();
    testDivision();
    testNtt();
    testPowerAndGcd();
    if (failures == 0)
    {
        std::cout << "bigint-tests：全部通过（" << checks << " 项断言）\n";
        return 0;
    }
    std::cout << "bigint-tests：" << failures << " / " << checks << " 项断言失败\n";
    return 1;
}
