/**
 * @file main.cpp
 * @brief `library/big_number` 演示：阶乘、斐波那契、幂、gcd / lcm、同余幂、字符串互转，
 *        以及 `BigFloat` 的十进制浮点（0.1 + 0.2 == 0.3、1/3、sqrt(2)、大整数精确输出）。
 *
 * 输出是**确定**的，`make check` 会跑它做冒烟。
 */

#include "bigint.hpp"
#include "bigfloat.hpp"

#include <iostream>
#include <string>

namespace
{
using bigint::BigInt;
using bigfloat::BigFloat;

/** n!（迭代，全部用大整数乘）。 */
BigInt factorial(unsigned n)
{
    BigInt result(1);
    for (unsigned value = 2u; value <= n; ++value) result *= BigInt(static_cast<std::int64_t>(value));
    return result;
}

/** 第 n 个斐波那契数（F(0) = 0、F(1) = 1）。 */
BigInt fibonacci(unsigned n)
{
    BigInt a(0);
    BigInt b(1);
    for (unsigned index = 0u; index < n; ++index)
    {
        BigInt next = a + b;
        a = b;
        b = next;
    }
    return a;
}
} // namespace

int main()
{
    std::cout << "2^100      = " << BigInt(2).pow(100).toString() << "\n";
    std::cout << "30!        = " << factorial(30u).toString() << "\n";
    std::cout << "F(200)     = " << fibonacci(200u).toString() << "\n";

    const BigInt a("123456789012345678901234567890");
    const BigInt b("98765432109876543210");
    std::cout << "a           = " << a.toString() << "\n";
    std::cout << "b           = " << b.toString() << "\n";
    std::cout << "a / b       = " << (a / b).toString() << "\n";
    std::cout << "a % b       = " << (a % b).toString() << "\n";
    std::cout << "gcd(a, b)   = " << BigInt::gcd(a, b).toString() << "\n";
    std::cout << "lcm(12, 18) = " << BigInt::lcm(BigInt(12), BigInt(18)).toString() << "\n";
    std::cout << "7^300 mod 1000000007 = "
              << BigInt(7).powmod(300u, BigInt(1000000007)).toString() << "\n";
    std::cout << "位十进制位数 = " << a.decimalDigits() << "（limb " << a.limbCount() << " 个）\n";

    std::cout << "\nBigFloat（尾数 × 10^指数，十进制浮点）：\n";
    const BigFloat tenth = BigFloat::fromDouble(0.1);
    const BigFloat fifth = BigFloat::fromDouble(0.2);
    std::cout << "0.1 + 0.2    = " << (tenth + fifth).toString()
              << "（同样的 double 在二进制浮点里是 0.30000000000000004）\n";
    std::cout << "1 / 3 (30 位)= " << (BigFloat("1") / BigFloat("3")).toString(30) << "\n";
    std::cout << "sqrt(2) (30) = " << BigFloat("2").sqrt(30).toString(30) << "\n";
    std::cout << "1e20 + 1     = " << (BigFloat("1e20") + BigFloat("1")).toPlainString()
              << "（精确，没有丢低位）\n";
    std::cout << "2^200 精确   = " << BigFloat::fromInt(BigInt(2).pow(200)).toPlainString() << "\n";
    std::cout << "2^200 17 位  = " << BigFloat::fromInt(BigInt(2).pow(200)).toString() << "\n";
    return 0;
}
