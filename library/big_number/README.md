# big_number：任意精度大整数 + 十进制浮点（C++17）

> **谁在用**：`library/stackvm` 的 `bint` 类型（虚拟机的任意精度整数）后端就是这个库的
> `bigint::BigInt` —— 宿主语言（如 `magel` 的 Magel 语言）里的
> `bint a = bint("…"); a + b;` 走到虚拟机内部就是这边的运算符重载。
> 那一步是**可选的**：`make -C library/stackvm STACKVM_WITH_BIGINT=0` 能编出不带大整数的
> 轻量虚拟机。

`library/big_number` 是一个**只依赖标准库**的大数库：加 / 减 / 乘 / 除 / 取余 /
幂 / 最大公约数 / 最小公倍数 / 同余幂，以及十进制字符串互转。实现参考
`eigenmath-plus/eigenmath_bigint.c` 的整数部分（同一套"符号 + 小端 limb + 有效长度"的
结构），把手工内存管理换成 `std::vector`、把 C 接口换成 C++ 的运算符重载。

同一个库里还有 `bigfloat::BigFloat`（`bigfloat.hpp` / `bigfloat.cpp`）：**十进制浮点**，
`值 = 尾数 × 10^指数`、尾数就是上面的 `BigInt` —— 对应参考实现的 `BigFloat_T`。
所以 `0.1 + 0.2` 精确等于 `0.3`（二进制浮点那套表示误差这里不存在），
大整数乘法和除法直接吃到下面的 NTT 内核。

目录名是 `big_number`（一个库装下整数与浮点），C++ 里的名字仍按类型走：
整数是 `bigint::BigInt`（`bigint.hpp`）、十进制浮点是 `bigfloat::BigFloat`
（`bigfloat.hpp`）—— 头文件名与命名空间说的都是"类型"，不是库目录。

## 表示与约定

```text
value = sign × Σ limbs[i] × 2^(32 i)      limbs 是小端 uint32_t 数组
```

- 0 只有一种表示：`sign() == 0`、limb 个数为 0；
- 高位不存 0 limb（每次运算后规范化）；
- `a / b`、`a % b` 与 C / C++ 的整数除法一致：**向零截断**，
  `a == (a / b) * b + a % b`，余数符号跟被除数；
- 除以零：`divmod()` 返回 `false`；`operator/`、`operator%` 抛 `std::domain_error`；
- `int64_t` 构造只吃有符号范围，大于 `INT64_MAX` 的无符号值用
  **`BigInt::fromU64()`**（与参考实现的 `EigenMath_BigIntFromU64` 对应）。

## API

```cpp
using bigint::BigInt;

BigInt a("123456789012345678901234567890");   // 十进制串（可带 +/- 号）
BigInt b = BigInt::fromU64(18446744073709551615ull);
BigInt c(42), d(-7);

a + b;  a - b;  a * b;  a / b;  a % b;   -a;      // 运算符重载（值语义）
a += b; a -= b; a *= b; a /= b; a %= b; ++a; a--; // 就地运算
a == b; a != b; a <  b; a <= b; a > b; a >= b;    // 比较（含负数）

a.toString();          // 十进制字符串
a.decimalDigits();     // 十进制位数
a.limbCount();         // 32 位 limb 个数
a.isZero(); a.sign();  // 0 / ±1
a.abs();
a.toU64(); a.toI64(); a.toDouble();   // 截断（toDouble 会丢精度，符号跟着走）

a.pow(300);                          // 快速幂（二进制）
BigInt::gcd(a, b); BigInt::lcm(a, b); // 欧几里得 / 最小公倍数
a.powmod(300, modulus);              // 同余幂（逐步取模）

BigInt q, r;
if (a.divmod(b, &q, &r)) { /* a == q*b + r */ }   // 除零返回 false，不抛异常

bool ok = false;
BigInt parsed = BigInt::fromString("12a", &ok);   // 不抛异常的解析；ok == false
```

## BigFloat API

```cpp
using bigfloat::BigFloat;

BigFloat x("123.456");            // 十进制文本：123.456 / -0.5 / .5 / 1e100 / 2.5E-3
BigFloat y = BigFloat::fromInt(BigInt("123456789012345678901234567890"));  // 整数 → 浮点（精确）
BigFloat z = BigFloat::fromDouble(0.1);   // double → 最短往返十进制：z.toString() == "0.1"

x + y; x - y; x * y; x / y;  -x;          // 运算符重载（operator/ 结果保留 34 位）
x += y; x -= y; x *= y; x /= y;
x == y; x != y; x < y; x <= y; x > y; x >= y;

x.mantissa(); x.exponent10();  // 值 = 尾数 × 10^指数（尾数带符号）
x.sign(); x.isZero();

x.round(10);            // 舍入到 10 位有效数字（四舍五入、远离零）
x.toString();           // 十进制文本，缺省 17 位（%.17g 口径，指数大/小切科学记数法）
x.toString(30);         // 30 位有效数字
x.toString(0);          // ≤ 0 = 不舍入（按值精确输出，显示阈值仍按 17 位）
x.toPlainString();      // 恒定普通记法：1e20 → "100000000000000000000"
x.divide(y, 50);        // 除法并指定有效位数
x.sqrt(30);             // 平方根，保留 30 位（负数抛 std::domain_error）
```

精度口径与参考实现一致：**所有运算都在整数上做，多算几位再舍入**到目标有效位数。

输出记法与 `%g` 同一口径：设 P 为显示位数（`digits > 0` 时就是 `digits`，否则 17）、
X 为科学记数法指数（`值 = d.ddd × 10^X`），则 `X < -4 || X >= P` 用科学记数法，
否则用普通记法。所以 `BigFloat("1e20").toString()` 是 `"1e+20"`（20 ≥ 17），
要看全 21 位就用 `toPlainString()`。

## 算法

| 操作 | 做法 | 复杂度 |
| --- | --- | --- |
| 加 / 减 | limb 级带进位 / 借位（符号交叉在 `operator±` 里分派） | O(n) |
| 乘（小规模） | **学校算法**（`uint64_t` 承接 32×32 位乘积） | O(n×m) |
| 乘（默认） | **Karatsuba**：`z0 + ((z1-z0-z2) << m) + (z2 << 2m)`，递归基线为较短操作数 96 limb | O(n^log2(3)) |
| 乘（超大规模） | **双素数 NTT + CRT 重建**（15 位数字，`p1 = 998244353`、`p2 = 1004535809`，原根 3） | **O(n log n)** |
| 除 / 取余（默认） | **Burnikel-Ziegler**：递归求解 `2n/1n` 与 `3n/2n` 子问题 | 分治复杂度，取决于乘法内核 |
| 除 / 取余（小规模） | 移位相减长除法，作为不超过 56 limb 的默认递归基线 | O(位数 × n) |
| 幂 / 同余幂 | 二进制快速幂 | O(log e) 次乘法 |
| gcd | 欧几里得（相除取余） | O(log min(a,b)) 次除法 |
| 十进制互转 | 每 9 位一段，反复乘 / 除 10^9 | O(段数 × n) |
| 浮点加 / 减 | 对齐到较小的十进制指数、整数加减、再规范化 | O(位数) |
| 浮点乘 | 尾数相乘（默认 Karatsuba，超大规模走 NTT）+ 指数相加 | O(n^log2(3)) / O(n log n) |
| 浮点除 | 放大被除数到 `digits + 2` 位有效数字，整数除 + 舍入 | 一次大整数除法 |
| 浮点平方根 | 指数凑成偶数、尾数放大到 `2×digits + 2` 位，`integerSqrt` + 舍入 | 一次整数开方 |

乘法内核现在是三级分派：较短操作数不超过 **96 limb** 时走学校算法，超过后默认走
Karatsuba；两边 limb 个数之和达到 **48,000**（等长时每边约 23 万位十进制）再切到 NTT。
`BigInt::setKaratsubaThreshold()` 调整递归基线，`setMultiplicationThreshold()` 调整 NTT
阈值。单元测试会分别强制三条路径并**逐位对比**，还覆盖
`2^4096 × 2^4096 == 2^8192` 与大数除法 `q×b+r == a`。

除法默认使用 Burnikel-Ziegler：先把除数补齐到 2 的幂 limb 块，再通过标准的
`2n/1n`、`3n/2n` 递归分解计算商块；除数不超过 **56 limb** 时回退到移位相减的长除法。
`BigInt::setDivisionThreshold()` 可调整递归基线。测试会强制两条路径处理同一组非 2 次幂
长度及不对称输入，并核对商、余数、`q×b+r == a` 和负数向零截断语义。

三个默认值在当前机器上用 MinGW g++ 11.2 `-O2`、稠密平衡操作数、5 轮中位数重新标定。
学校算法与 Karatsuba 在约 **96–120 limb** 进入交叉区（下表单位为毫秒）：

| 每边 limb | 学校算法 | Karatsuba（基线 96） |
| ---: | ---: | ---: |
| 96 | 0.0050 | 0.0050 |
| 104 | 0.0059 | **0.0051** |
| 120 | 0.0065 | **0.0063** |
| 129 | 0.0091 | **0.0070** |
| 161 | 0.0137 | **0.0104** |

Karatsuba 与 NTT 的首个明确交叉落在每边约 **24K limb**、两边合计约 **48K limb**。
NTT 长度取 2 的幂，因此耗时会阶梯式跳变，不存在对所有形状都完美的单一阈值：

| 每边 limb | Karatsuba（基线 96） | NTT |
| ---: | ---: | ---: |
| 20,481 | **26.6** | 29.8 |
| 24,576 | 35.4 | **30.4** |
| 32,768 | **55.4** | 58.8 |
| 40,961 | 79.3 | **60.4** |

除法的平衡 `2n / n` 交叉区在除数 **48–56 limb**；默认取 56，避免短商场景过早进入
分治。除数为 41 / 49 / 57 limb 时，长除法与 Burnikel-Ziegler 分别为
`0.242/0.252`、`0.321/0.296`、`0.411/0.330` ms。不同 CPU、编译器与操作数比例会移动
交叉点，所以三个阈值都保留公开调节接口。

NTT 内部还有一处关键写法：`ntt` / `convolution` 把模数写成
**模板参数**（编译期常量），蝶形里的 `% modulus` 才能被编成"乘高位 + 移位"而不是
很慢的 64 位除法指令 —— 同一台机器上这一处就让 400,000 位的乘法从 43 ms 掉到 23 ms。

NTT 的细节（与参考实现的思路一致）：

```text
32 位 limb  →  15 位数字（小端）
    →  两个素数各做一次长度 2^k 的 NTT 卷积
    →  CRT：x = r1 + p1 × ((r2 − r1) × p1^{-1} mod p2)
    →  15 位数字内部进位  →  线性打包回 32 位 limb
```

15 位数字保证"系数真值 < p1×p2 ≈ 2^60"，两个素数都支持到 2^21 的长度，
所以几个兆比特以内的乘法都不会溢出。

## 构建与自检

```text
make -C library/big_number          # libbig_number.a + big_number-demo + bigint-tests + bigfloat-tests
make -C library/big_number check    # 跑单元测试（119 + 86 项断言）与演示
make -C library/big_number clean
```

产物在 `build/big_number/`。演示（`big_number-demo`）打印固定内容，`check` 会顺带跑它：

```text
2^100      = 1267650600228229401496703205376
30!        = 265252859812191058636308480000000
F(200)     = 280571172992510140037611932413038677189525
a           = 123456789012345678901234567890
b           = 98765432109876543210
a / b      = 1249999988
a % b      = 60185185207253086410
gcd(a, b)  = 90
lcm(12, 18) = 36
7^300 mod 1000000007 = 678144625
位十进制位数 = 30（limb 4 个）

BigFloat（尾数 × 10^指数，十进制浮点）：
0.1 + 0.2    = 0.3（同样的 double 在二进制浮点里是 0.30000000000000004）
1 / 3 (30 位)= 0.333333333333333333333333333333
sqrt(2) (30) = 1.41421356237309504880168872421
1e20 + 1     = 100000000000000000001（精确，没有丢低位）
2^200 精确   = 1606938044258990275541962092341162602522202993782792835301376
2^200 17 位  = 1.6069380442589903e+60
```

也可以用 CMake：`cmake -S library/big_number -B build/big_number-cmake && cmake --build build/big_number-cmake && ctest --test-dir build/big_number-cmake -C Debug`。
