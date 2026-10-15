# big_number（Rust 版）：任意精度大整数 + 十进制浮点

> **这是一个独立的 Rust crate：`big_number`。** 它是 C++17 版
> [`library/big_number`](../big_number) 的**逐函数移植**：同一套表示、同一套算法、
> 同一套语义与边界行为（向零截断、`0^0 = 1`、`gcd(0,0) = 0`、除以零的行为、
> `%.17g` 口径的输出……）。
>
> **只依赖标准库，没有任何第三方依赖**（`Cargo.lock` 里只有本 crate 自己）。
> 同一个 crate 里装两样东西：整数 `bigint::BigInt` 与十进制浮点 `bigfloat::BigFloat`
> （`值 = 尾数 × 10^指数`，尾数就是那个 `BigInt`）。
>
> C++ 版那份 README（以及三个算法阈值的实测表）在
> [`../big_number/README.md`](../big_number/README.md)，这里只讲 Rust 版怎么用、
> 与 C++ 版怎么对上、自检验过什么。

## 快速开始

```sh
# 演示程序（输出与 C++ 的 big_number-demo 逐行一致，可以直接 diff）
cargo run --manifest-path library/big_number-rust/Cargo.toml

# 单元测试（22 个测试函数 + 1 个 doctest）
cargo test --manifest-path library/big_number-rust/Cargo.toml
```

```rust
use big_number::{BigFloat, BigInt};

let a: BigInt = "123456789012345678901234567890".parse().unwrap();
let b: BigInt = "123456789012345678901234567891".parse().unwrap();
assert_eq!(a + BigInt::from(1), b);
assert_eq!(BigInt::from(2).pow(300).decimal_digits(), 91);
assert_eq!(BigInt::from(7).powmod(300, &BigInt::from(1_000_000_007)).to_string(), "678144625");

// 十进制浮点：0.1 + 0.2 精确等于 0.3（这里没有二进制浮点的表示误差）
let sum = BigFloat::from_double(0.1) + BigFloat::from_double(0.2);
assert_eq!(sum.to_string(), "0.3");
assert_eq!(
    BigFloat::from_string("2").unwrap().sqrt(30).to_string_with_digits(30),
    "1.41421356237309504880168872421"
);
```

编译产物：在 crate 目录里跑（`cd library/big_number-rust && cargo run`）时按
`.cargo/config.toml` 落到仓库的 `build/big_number/rust/`；从仓库根用 `--manifest-path`
跑时 cargo 读不到那份配置，落在 `library/big_number-rust/target/`。两者都在 `.gitignore` 里。

## 与 C++ 版的对应关系

| C++（`library/big_number`） | Rust（本 crate） |
| --- | --- |
| `bigint::BigInt` / `bigfloat::BigFloat` | `BigInt` / `BigFloat`（`bigint.rs` / `bigfloat.rs`） |
| `BigInt(42)` / `BigInt("…")` | `BigInt::from(42)` / `"…".parse::<BigInt>()` |
| `BigInt::fromU64(v)` | `BigInt::from_u64(v)`（也可以 `BigInt::from(v)`） |
| `BigInt::fromString(s, &ok)` | `BigInt::from_string(s) -> Option<BigInt>` |
| `a.toString()` | `a.to_string()`（`impl Display`） |
| `a.decimalDigits()` / `limbCount()` / `isZero()` / `sign()` / `abs()` | `a.decimal_digits()` / `limb_count()` / `is_zero()` / `sign()` / `abs()` |
| `a.compare(b)` | `a.compare(&b)`（另有 `Ord`：`a < b`、`sort()` 都能用） |
| `a.toU64()` / `toI64()` / `toDouble()` | `a.to_u64()` / `to_i64()` / `to_f64()` |
| `a + b`（值语义）、`a += b`、`++a` / `a++` | `a + b` / `a + &b` / `&a + b` / `&a + &b`、`a += b`、`a += 1` |
| `BigInt::gcd(a, b)` / `lcm` / `a.pow(e)` / `a.powmod(e, m)` | `BigInt::gcd(&a, &b)` / `lcm` / `a.pow(e)` / `a.powmod(e, &m)` |
| `a.divmod(b, &q, &r)` → `bool` | `a.divmod(&b) -> Option<(BigInt, BigInt)>` |
| `integerSqrt(v)` / `pow10(e)` | `integer_sqrt(&v)` / `pow10(e)` |
| `BigFloat("…")` / `fromString(s, &ok)` | `BigFloat::from_string(s)` / `"…".parse::<BigFloat>()` |
| `fromInt` / `fromDouble(v)` / `fromDouble(v, n)` | `from_int(&v)` / `from_double(v)` / `from_double_with_limit(v, n)` |
| `x.toString(n)` / `toPlainString(n)` | `x.to_string_with_digits(n)` / `to_plain_string_with_digits(n)`（`to_plain_string()` = 不舍入） |
| `x.round(n)` / `divide(y, n)` / `sqrt(n)` | `x.round(n)` / `x.divide(&y, n)` / `x.sqrt(n)` |
| `x.mantissa()` / `exponent10()` | `x.mantissa()` / `exponent10()` |
| `setMultiplicationThreshold` / `multiplicationThreshold` | `BigInt::set_multiplication_threshold` / `multiplication_threshold`（NTT 阈值） |
| `setKaratsubaThreshold` / `karatsubaThreshold` | `BigInt::set_karatsuba_threshold` / `karatsuba_threshold`（Karatsuba 基线） |
| `setDivisionThreshold` / `divisionThreshold` | `BigInt::set_division_threshold` / `division_threshold`（Burnikel-Ziegler 基线） |
| 抛 `std::domain_error` / `std::invalid_argument` | `panic!`（每个方法都写了 `# Panics`）或 `try_divide` / `try_sqrt` 拿 `Result` |

**刻意的差异**（都是语言层面的，语义没动）：

1. **除以零**：C++ 的 `divmod` 返回 `false`、`/` 与 `%` 抛异常；Rust 的 `divmod` 返回
   `None`（不 panic），`/` 与 `%` panic，文案与 C++ 一样（`bigfloat: 除以零` 这种前缀都保留了，
   所以 `#[should_panic(expected = "除以零")]` 与 C++ 的 `catch` 能对上）。
   `BigFloat::divide` / `sqrt` 额外给了不 panic 的 `try_divide` / `try_sqrt`。
2. **错误类型**：C++ 的各种异常在 Rust 里统一成 `NumberError`
   （`DivisionByZero` / `ZeroModulus` / `NegativeSqrt` / `InvalidPrecision` / `InvalidText`），
   `Display` 的文案抄的就是 C++ 异常消息。
3. **没有 `++` / `--`**：C++ 的 `operator++/--` 在 Rust 里就是 `a += 1` / `a -= 1`
   （单元测试里两者逐条对应）。
4. **三个算法阈值都是原子量**：C++ 是 `static std::size_t`，Rust 用 `AtomicUsize`（多线程下改 / 读不会撕裂）。
5. **学校算法的缓冲多留一格**：C++ 的进位回写循环靠「乘积一定塞得下」的推论省了边界判断，
   Rust 版把中间数组开成 `len_l + len_r + 1` 再 `normalize()` 收掉多出来的那个零 limb
   （结果完全一样，只是不想让 debug 构建有机会越界 panic）。
6. **`bit_at` 放成公开方法**：C++ 里是 private 辅助函数，Rust 版当查询接口开放
   （`bit_length` 仍是 private）。

## 自检结果（实测）

```text
$ cargo test                        # 在 library/big_number-rust 里跑
test result: ok. 24 passed; 0 failed; 0 ignored
   Doc-tests big_number: running 1 test ... ok
$ cargo clippy --all-targets        # 0 条告警
```

| 项 | 数量 | 说明 |
| --- | --- | --- |
| 测试函数 | 24 | `bigint` 15 个（构造 / 比较 / 四则 / Burnikel-Ziegler 与长除法差分 / 位辅助 / 三条乘法内核 / **多组随机形状的内核比对** / 幂与 gcd / 平方根 / 解析输出 + 5 个 `#[should_panic]`）、`bigfloat` 9 个（解析输出 / 四则 / 舍入比较 / 平方根 / 不 panic 入口 / 大规模精度 + 3 个 `#[should_panic]`） |
| 断言 | 逐项 + 差分 | C++ 基础语义逐条对应，另加位辅助、失败入口、整数平方根及乘除内核随机形状差分 |
| doctest | 1 | `lib.rs` 的 crate 文档示例是真跑的（就是本文件上面那段 `use big_number::…`） |

**演示程序与 C++ 逐行一致**：`build/big_number/big_number-demo.exe` 与
`cargo run --manifest-path library/big_number-rust/Cargo.toml` 的输出
`diff --strip-trailing-cr` 为 0（只差 Windows 文本模式的 `\r\n`）。

**差分自检**（`tmp/big_number-diff/`，不进仓库）：让 C++ 版与 Rust 版跑同一份 **562 行**
「剧本」——两边的每一行都是 `名字 = 值`，再逐行比对，结果是 **0 处差异**。剧本覆盖：

- 两个类型的**合法 / 非法文本**（`"12a"` / `"1e"` / `"1e99999999"` / `""` / `"-"` / `"42 "` …）
  与全部格式化重载（`toString(n)` / `toPlainString(n)` / `round(n)` / `divmod` 的商与余数）；
- 数字边界（`0`、`±1`、`u64::MAX`、`i64::MIN`、`2^32±1`、`INT64_MIN`、`2^200`）；
- **跨过乘法内核切换阈值**的大数：`2^120000 × 3^100000` —— NTT / Karatsuba /
  学校算法可分别强制执行并互比，**36,154 位十进制 / 8,704 个 limb** 逐位一致；
- 大数除法恒等式 `q × b + r == a`、余数 `<` 除数、`c / c`、小整数除 / 取余，以及
  多组不规则 limb 长度的 Burnikel-Ziegler / 长除法差分；
- 浮点的加减乘除 / 比较 / 舍入 / 开方（含 `sqrt(2)` 到 60 位）、
  `fromDouble` 的 16 个值（含 `1e300`、`1e-300`、`f64::MAX`、`-0.0`、`0.1`）。

一份 `f64` 的往返比对是这套里最值得留意的：C++ 用 `%.*g` + `strtod` 逐位试出「最短往返
十进制」，Rust 用 `format!("{:.*e}", p - 1, v)` + `str::parse::<f64>()` —— 同样 p 位有效数字、
同样的四舍五入规则，所以两个实现挑出来的尾数 / 指数逐位相同（`fromDouble 尾数/指数`
那几行就是这条的证据）。

## 目录与模块

| 文件 | 作用 |
| --- | --- |
| `src/lib.rs` | crate 文档 + `NumberError` + doctest |
| `src/bigint.rs` | `BigInt`：构造 / 互转 / 位操作 / 幅值运算 / 四则与比较的运算符 / 幂与 gcd / 三级乘法 / Burnikel-Ziegler 除法 / 单元测试 |
| `src/bigfloat.rs` | `BigFloat`：解析 / 输出（普通与科学记数法）/ 四则 / 比较 / 舍入 / 平方根 / 单元测试 |
| `src/main.rs` | 演示程序 `big-number-demo`（与 C++ 的 `big_number-demo` 同输出） |
| `.cargo/config.toml` | 产物落到 `build/big_number/rust/` |

算法与复杂度与 C++ 版完全一致（学校算法 / Karatsuba / 双素数 NTT + CRT /
Burnikel-Ziegler + 小规模长除法 / 二进制快速幂 / 欧几里得 / 十进制互转），默认阈值同为
Karatsuba 96 limb、NTT 48,000 limb 和、除法递归基线 56 limb，
细节见 [`../big_number/README.md`](../big_number/README.md) 的「算法」一节。
