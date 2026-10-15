//! `library/big_number` 的 Rust 版：**任意精度大整数 + 十进制浮点**，只依赖标准库。
//!
//! 这是 C++17 版 `library/big_number`（`bigint.hpp/cpp` + `bigfloat.hpp/cpp`）的
//! **1:1 移植**：同一套表示、同一套算法、同一套语义与边界行为，只是换了语言。
//! 对应关系如下：
//!
//! | C++ | Rust |
//! | --- | --- |
//! | `bigint::BigInt` / `bigfloat::BigFloat` | [`BigInt`] / [`BigFloat`]（同一 crate 的两个模块） |
//! | `BigInt(42)` / `BigInt("…")` | `BigInt::from(42)` / `"…".parse::<BigInt>()` |
//! | `std::vector<std::uint32_t>` | `Vec<u32>` |
//! | 运算符重载（`a + b`） | `std::ops` 的四种组合（`a + b` / `a + &b` / `&a + b` / `&a + &b`）与 `+=` 系列 |
//! | `a.toString()` | `a.to_string()`（`Display`） |
//! | 抛 `std::domain_error` | `panic!`（每个会 panic 的方法都用 `# Panics` 写清） |
//! | `BigInt::fromString(s, &ok)` | [`BigInt::from_string`]（`Option`）/ [`FromStr`](std::str::FromStr) |
//! | 命名空间级全局阈值 | [`BigInt::set_multiplication_threshold`] / [`BigInt::set_karatsuba_threshold`] / [`BigInt::set_division_threshold`]（内部是 `AtomicUsize`） |
//!
//! 模块：
//!
//! - [`bigint`]：任意精度**整数**（加 / 减 / 乘 / 除 / 取余 / 幂 / 最大公约数 /
//!   最小公倍数 / 同余幂 / 十进制互转），三级乘法 + Burnikel-Ziegler 除法；
//! - [`bigfloat`]：**十进制浮点**（`值 = 尾数 × 10^指数`，尾数就是 [`BigInt`]）——
//!   四则 / 比较 / 舍入 / 平方根 / 输出，所以 `0.1 + 0.2` 精确等于 `0.3`。
//!
//! ```
//! use big_number::{BigFloat, BigInt};
//!
//! let a: BigInt = "123456789012345678901234567890".parse().unwrap();
//! let b: BigInt = "123456789012345678901234567891".parse().unwrap();
//! assert_eq!(a + BigInt::from(1), b);
//! assert_eq!(BigInt::from(2).pow(300).decimal_digits(), 91);
//! assert_eq!(BigInt::from(7).powmod(300, &BigInt::from(1_000_000_007)).to_string(), "678144625");
//!
//! // 十进制浮点：0.1 + 0.2 精确等于 0.3（这里没有二进制浮点的表示误差）
//! let sum = BigFloat::from_double(0.1) + BigFloat::from_double(0.2);
//! assert_eq!(sum.to_string(), "0.3");
//! assert_eq!(
//!     BigFloat::from_string("2").unwrap().sqrt(30).to_string_with_digits(30),
//!     "1.41421356237309504880168872421"
//! );
//! ```
//!
//! 上面这段就是 `library/big_number-rust/README.md` 里的那个例子，`cargo test` 会把它作为
//! doctest 真跑一遍。
//!
//! 明确不做（与 C++ 版一致）：**分数 / 大有理数**，以及 `BigFloat` 的
//! `exp / log / π / 三角函数`。

#![warn(missing_docs)]

pub mod bigfloat;
pub mod bigint;

pub use bigfloat::BigFloat;
pub use bigint::{integer_sqrt, pow10, BigInt};

use std::fmt;

/// 大数库的错误类型。
///
/// C++ 版用各式各样的异常（`std::domain_error` / `std::invalid_argument`）表达这些情况；
/// Rust 版统一成一个枚举。`Display` 的文案与 C++ 版抛出的消息一致（`bigfloat: 除以零`
/// 这种前缀由抛出的那一方补上），所以两版的
/// `#[should_panic(expected = "除以零")]` / `catch (const std::domain_error &)` 能对上。
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum NumberError {
    /// 除以零（`/`、`%`、`divide`）。
    DivisionByZero,
    /// 同余幂的模数为零。
    ZeroModulus,
    /// 负数开平方。
    NegativeSqrt,
    /// 有效位数不是正数（`round` / `divide` / `sqrt` 的 `digits` 参数）。
    InvalidPrecision,
    /// 十进制文本不是合法的数字。
    InvalidText,
}

impl fmt::Display for NumberError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        let text = match self {
            NumberError::DivisionByZero => "除以零",
            NumberError::ZeroModulus => "模数为零",
            NumberError::NegativeSqrt => "负数不能开平方",
            NumberError::InvalidPrecision => "有效位数必须是正数",
            NumberError::InvalidText => "不是合法的十进制文本",
        };
        formatter.write_str(text)
    }
}

impl std::error::Error for NumberError {}

/// 测试专用的小工具。
#[cfg(test)]
pub(crate) mod test_support {
    use std::sync::{Mutex, MutexGuard};

    /// 乘法阈值是**进程级全局状态**，而 `cargo test` 默认多线程跑：
    /// 动过阈值的测试必须串行，否则互相把对方的阈值改掉（断言会随机失败）。
    /// 拿到守卫即持锁，测试结束自动放开。
    pub(crate) fn threshold_guard() -> MutexGuard<'static, ()> {
        static LOCK: Mutex<()> = Mutex::new(());
        LOCK.lock().unwrap_or_else(|poisoned| poisoned.into_inner())
    }
}
