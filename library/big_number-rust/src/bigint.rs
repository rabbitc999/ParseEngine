//! 任意精度大整数 [`BigInt`]：加 / 减 / 乘 / 除 / 取余 / 幂 / 最大公约数 / 最小公倍数 /
//! 同余幂，以及十进制字符串互转。
//!
//! 对应 C++ 版的 `bigint.hpp` + `bigint.cpp`（同一套「符号 + 小端 limb + 有效长度」的
//! 结构、同一套算法），只是把手工内存管理换成 `Vec<u32>`、把运算符重载换成 `std::ops`。
//!
//! ```text
//! value = sign × Σ limbs[i] × 2^(32 i)      limbs 是小端 u32 数组
//! ```
//!
//! 约定（与 C++ 版逐条一致）：
//!
//! - 0 只有一种表示：`sign() == 0`、limb 个数为 0；
//! - 高位不存 0 limb（每次运算后 `normalize()`）；
//! - `a / b`、`a % b` **向零截断**（C / C++ / Rust 的整数除法语义）：
//!   `a == (a / b) * b + a % b`，余数符号跟被除数；
//! - 除以零：`divmod()` 返回 `None`；`/`、`%` 直接 panic（不想处理 panic 就用 `divmod`）；
//! - `i64` 构造只吃有符号范围，大于 `i64::MAX` 的无符号值用 [`BigInt::from_u64`]
//!   （对应参考实现的 `EigenMath_BigIntFromU64`）。
//!
//! 乘法内核按规模自动切换：小规模走学校算法，中等规模默认走 **Karatsuba**，超大规模走
//! **双素数 NTT + CRT**（O(n log n)）。
//! 除法默认走 **Burnikel-Ziegler**，除数不超过 56 limb 时回退移位相减长除法。

use crate::NumberError;
use std::cmp::Ordering;
use std::fmt;
use std::ops::{
    Add, AddAssign, Div, DivAssign, Mul, MulAssign, Neg, Rem, RemAssign, Sub, SubAssign,
};
use std::str::FromStr;
use std::sync::atomic::{AtomicUsize, Ordering as AtomicOrdering};

/// 一个 limb 的基数：2^32。
const LIMB_BASE: u64 = 1 << 32;

/// 十进制字符串一次处理 9 位（`10^9 < 2^32`）。
const DECIMAL_CHUNK: u32 = 1_000_000_000;

/// NTT 素数 1：`119 × 2^23 + 1 = 998244353`，原根 3（支持长度到 `2^23`）。
const NTT_PRIME1: u32 = 998_244_353;

/// NTT 素数 2：`479 × 2^21 + 1 = 1004535809`，原根 3（支持长度到 `2^21`）。
const NTT_PRIME2: u32 = 1_004_535_809;

/// 两个 NTT 素数的原根。
const NTT_ROOT: u32 = 3;

/// NTT 用的「15 位数字」：`15 + 15 + log2(n) < log2(p1 × p2)`，CRT 重建不会溢出。
const DIGIT_BITS: usize = 15;

/// 默认 NTT 阈值：两边合计 48,000 limb（平衡乘法每边约 23 万位十进制）。
const DEFAULT_MULTIPLICATION_THRESHOLD: usize = 48_000;
const DEFAULT_KARATSUBA_THRESHOLD: usize = 96;
const DEFAULT_DIVISION_THRESHOLD: usize = 56;

/// 乘法内核的切换阈值：两边 limb 个数之和达到它就改用 NTT。
///
/// C++ 版是个 `std::size_t` 全局量；这里用原子量，多线程下改 / 读也不会撕裂。
static MULTIPLICATION_THRESHOLD: AtomicUsize = AtomicUsize::new(DEFAULT_MULTIPLICATION_THRESHOLD);

/// Karatsuba 基线阈值：较短操作数超过它才递归，以下仍以学校算法收尾。
static KARATSUBA_THRESHOLD: AtomicUsize = AtomicUsize::new(DEFAULT_KARATSUBA_THRESHOLD);

/// Burnikel-Ziegler 的递归基线；更小的除数回退到移位相减长除法。
static DIVISION_THRESHOLD: AtomicUsize = AtomicUsize::new(DEFAULT_DIVISION_THRESHOLD);

/// `(base ^ exponent) mod MODULUS`（快速幂，`u32` 版）。
///
/// 模数走**编译期常量泛型**（C++ 版是模板参数）：蝶形里的 `% MODULUS` 于是能被编成
/// 「乘高位 + 移位」而不是真正的除法指令 —— 这是 NTT 快慢的关键。
fn pow_mod_u32<const MODULUS: u32>(base: u32, exponent: u64) -> u32 {
    let modulus = MODULUS as u64;
    let mut result = 1u64;
    let mut value = (base as u64) % modulus;
    let mut remaining = exponent;
    while remaining != 0 {
        if remaining & 1 != 0 {
            result = (result * value) % modulus;
        }
        value = (value * value) % modulus;
        remaining >>= 1;
    }
    result as u32
}

/// 原地 NTT（`invert` 为真时做逆变换并乘上 `n^{-1}`）。
fn ntt<const MODULUS: u32>(values: &mut [u32], invert: bool) {
    let modulus = MODULUS as u64;
    let count = values.len();

    // 位反转置换
    let mut reverse = 0usize;
    for index in 1..count {
        let mut bit = count >> 1;
        while reverse & bit != 0 {
            reverse ^= bit;
            bit >>= 1;
        }
        reverse ^= bit;
        if index < reverse {
            values.swap(index, reverse);
        }
    }

    let mut length = 2usize;
    while length <= count {
        let mut root = pow_mod_u32::<MODULUS>(NTT_ROOT, (modulus - 1) / length as u64);
        if invert {
            root = pow_mod_u32::<MODULUS>(root, modulus - 2); // 逆元
        }
        let half = length / 2;
        let mut start = 0usize;
        while start < count {
            let mut weight = 1u64;
            for offset in 0..half {
                let even = values[start + offset];
                let odd = ((values[start + offset + half] as u64 * weight) % modulus) as u32;
                // 加减各是「两个 < p 的数」，用条件减代替取模（省掉除法）。
                // 两个操作数都 < p < 2^30，所以 u32 加法不会溢出。
                let mut sum = even + odd;
                if sum >= MODULUS {
                    sum -= MODULUS;
                }
                let mut difference = even + MODULUS - odd;
                if difference >= MODULUS {
                    difference -= MODULUS;
                }
                values[start + offset] = sum;
                values[start + offset + half] = difference;
                weight = (weight * root as u64) % modulus;
            }
            start += length;
        }
        length <<= 1;
    }

    if invert {
        let inverse = pow_mod_u32::<MODULUS>(count as u32, modulus - 2);
        for value in values.iter_mut() {
            *value = ((*value as u64 * inverse as u64) % modulus) as u32;
        }
    }
}

/// 把幅值的 32 位 limb 切成 15 位一段的数字（小端）。
fn to_base15(limbs: &[u32]) -> Vec<u32> {
    if limbs.is_empty() {
        return Vec::new();
    }
    let total_bits = (limbs.len() - 1) * 32;
    let mut top_bits = 0usize;
    let mut top = limbs[limbs.len() - 1];
    while top != 0 {
        top_bits += 1;
        top >>= 1;
    }
    let bit_count = total_bits + top_bits;
    let digit_count = bit_count.div_ceil(DIGIT_BITS);
    let mut digits = vec![0u32; digit_count];
    for bit in 0..bit_count {
        if (limbs[bit / 32] >> (bit % 32)) & 1 == 0 {
            continue;
        }
        digits[bit / DIGIT_BITS] |= 1 << (bit % DIGIT_BITS);
    }
    digits
}

/// 一次 NTT 卷积，模 `MODULUS`（结果长度是补到 2 的幂之后的长度）。
fn convolution<const MODULUS: u32>(left: &[u32], right: &[u32]) -> Vec<u32> {
    let modulus = MODULUS as u64;
    let mut count = 1usize;
    while count < left.len() + right.len() {
        count <<= 1;
    }
    let mut a = vec![0u32; count];
    let mut b = vec![0u32; count];
    // 15 位数字一定 < 2^15 < 两个素数，所以夹一下就够了（不用除法）
    for (index, &value) in left.iter().enumerate() {
        a[index] = if value >= MODULUS {
            value - MODULUS
        } else {
            value
        };
    }
    for (index, &value) in right.iter().enumerate() {
        b[index] = if value >= MODULUS {
            value - MODULUS
        } else {
            value
        };
    }
    ntt::<MODULUS>(&mut a, false);
    ntt::<MODULUS>(&mut b, false);
    for index in 0..count {
        a[index] = ((a[index] as u64 * b[index] as u64) % modulus) as u32;
    }
    ntt::<MODULUS>(&mut a, true);
    a
}

/// 把 15 位数字（小端）打包回 32 位 limb。
///
/// 用一个 64 位小缓冲按 15 位往里塞、够 32 位就吐出一个 limb —— 线性一趟，
/// 不做「每个数字左移 15i 再相加」那种 O(n²) 的笨办法。
fn pack_base15(digits: &[u64]) -> Vec<u32> {
    // 真正的位长度：从最高那个非零的 15 位数字数出来
    let mut bit_count = 0usize;
    for index in (0..digits.len()).rev() {
        if digits[index] == 0 {
            continue;
        }
        bit_count = index * DIGIT_BITS;
        let mut top = digits[index];
        while top != 0 {
            bit_count += 1;
            top >>= 1;
        }
        break;
    }
    if bit_count == 0 {
        return Vec::new(); // 0
    }
    let mut limbs = vec![0u32; bit_count.div_ceil(32)];
    let mut limb_index = 0usize;
    let mut buffer = 0u64;
    let mut buffered = 0usize;
    for &digit in digits {
        buffer |= (digit & 0x7FFF) << buffered;
        buffered += DIGIT_BITS;
        while buffered >= 32 {
            if limb_index < limbs.len() {
                limbs[limb_index] = buffer as u32;
            }
            limb_index += 1;
            buffer >>= 32;
            buffered -= 32;
        }
    }
    if buffered != 0 && limb_index < limbs.len() {
        limbs[limb_index] = buffer as u32;
    }
    limbs
}

fn trim_limbs(limbs: &mut Vec<u32>) {
    while limbs.last() == Some(&0) {
        limbs.pop();
    }
}

fn add_limbs(left: &[u32], right: &[u32]) -> Vec<u32> {
    let mut out = vec![0u32; left.len().max(right.len()) + 1];
    let mut carry = 0u64;
    for index in 0..out.len() - 1 {
        let a = left.get(index).copied().unwrap_or(0) as u64;
        let b = right.get(index).copied().unwrap_or(0) as u64;
        let value = a + b + carry;
        out[index] = value as u32;
        carry = value >> 32;
    }
    let last = out.len() - 1;
    out[last] = carry as u32;
    trim_limbs(&mut out);
    out
}

/// limb 幅值相减；调用方保证 `left >= right`。
fn sub_limbs(left: &[u32], right: &[u32]) -> Vec<u32> {
    let mut out = vec![0u32; left.len()];
    let mut borrow = 0u64;
    for index in 0..left.len() {
        let a = left[index] as u64;
        let b = right.get(index).copied().unwrap_or(0) as u64;
        let subtrahend = b + borrow;
        if a < subtrahend {
            out[index] = (LIMB_BASE + a - subtrahend) as u32;
            borrow = 1;
        } else {
            out[index] = (a - subtrahend) as u32;
            borrow = 0;
        }
    }
    trim_limbs(&mut out);
    out
}

fn schoolbook_multiply(left: &[u32], right: &[u32]) -> Vec<u32> {
    if left.is_empty() || right.is_empty() {
        return Vec::new();
    }
    let mut out = vec![0u32; left.len() + right.len() + 1];
    for (index, &left_limb) in left.iter().enumerate() {
        let mut carry = 0u64;
        for (inner, &right_limb) in right.iter().enumerate() {
            let value = left_limb as u64 * right_limb as u64 + out[index + inner] as u64 + carry;
            out[index + inner] = value as u32;
            carry = value >> 32;
        }
        let mut cursor = index + right.len();
        while carry != 0 {
            let value = out[cursor] as u64 + carry;
            out[cursor] = value as u32;
            carry = value >> 32;
            cursor += 1;
        }
    }
    trim_limbs(&mut out);
    out
}

fn add_shifted(target: &mut Vec<u32>, value: &[u32], shift: usize) {
    if value.is_empty() {
        return;
    }
    if target.len() < shift + value.len() + 1 {
        target.resize(shift + value.len() + 1, 0);
    }
    let mut carry = 0u64;
    for (index, &limb) in value.iter().enumerate() {
        let sum = target[shift + index] as u64 + limb as u64 + carry;
        target[shift + index] = sum as u32;
        carry = sum >> 32;
    }
    let mut cursor = shift + value.len();
    while carry != 0 {
        let sum = target[cursor] as u64 + carry;
        target[cursor] = sum as u32;
        carry = sum >> 32;
        cursor += 1;
        if cursor == target.len() && carry != 0 {
            target.push(0);
        }
    }
}

fn karatsuba_multiply(left: &[u32], right: &[u32], threshold: usize) -> Vec<u32> {
    if left.is_empty() || right.is_empty() {
        return Vec::new();
    }
    if left.len().min(right.len()) <= threshold {
        return schoolbook_multiply(left, right);
    }

    let half = left.len().max(right.len()) / 2;
    let left_split = half.min(left.len());
    let right_split = half.min(right.len());
    let (left_low, left_high) = left.split_at(left_split);
    let (right_low, right_high) = right.split_at(right_split);

    let low_product = karatsuba_multiply(left_low, right_low, threshold);
    let high_product = karatsuba_multiply(left_high, right_high, threshold);
    let left_sum = add_limbs(left_low, left_high);
    let right_sum = add_limbs(right_low, right_high);
    let mut middle_product = karatsuba_multiply(&left_sum, &right_sum, threshold);
    middle_product = sub_limbs(&middle_product, &low_product);
    middle_product = sub_limbs(&middle_product, &high_product);

    let mut out = vec![0u32; left.len() + right.len() + 1];
    add_shifted(&mut out, &low_product, 0);
    add_shifted(&mut out, &middle_product, half);
    add_shifted(&mut out, &high_product, half * 2);
    trim_limbs(&mut out);
    out
}

/// 任意精度**有符号**大整数。
///
/// 值语义：所有运算都返回新值，不修改操作数（`+=` 系列除外）。
/// 所有比较 / 算术运算符都实现了「值 / 引用」的四种组合，写 `a + &b` 也能编过。
#[derive(Clone, Default)]
pub struct BigInt {
    /// 符号：-1 / 0 / 1。
    sign: i32,
    /// 小端 32 位 limb（高位不存 0）。
    limbs: Vec<u32>,
}

impl BigInt {
    /// 造一个 0（等价于 C++ 的 `BigInt()`）。
    #[must_use]
    pub fn zero() -> BigInt {
        BigInt {
            sign: 0,
            limbs: Vec::new(),
        }
    }

    /// 从 64 位有符号整数构造（`i64::MIN` 也正确）。
    #[must_use]
    pub fn from_i64(value: i64) -> BigInt {
        let mut out = BigInt::zero();
        if value == 0 {
            return out;
        }
        out.sign = if value < 0 { -1 } else { 1 };
        // i64::MIN 取负会溢出，所以先按无符号取幅值
        let mut magnitude = value.unsigned_abs();
        while magnitude != 0 {
            out.limbs.push(magnitude as u32);
            magnitude >>= 32;
        }
        out
    }

    /// 从 64 位**无符号**整数构造（`u64::MAX` 也不会被当成 -1）。
    ///
    /// 与参考实现的 `EigenMath_BigIntFromU64` 对应：`i64` 构造只吃有符号范围，
    /// 大于 `i64::MAX` 的无符号值请显式用这个。
    #[must_use]
    pub fn from_u64(value: u64) -> BigInt {
        let mut out = BigInt::zero();
        let mut magnitude = value;
        while magnitude != 0 {
            out.limbs.push(magnitude as u32);
            magnitude >>= 32;
        }
        if !out.limbs.is_empty() {
            out.sign = 1;
        }
        out
    }

    /// 从十进制文本构造（可带 `+` / `-` 号、允许前导 0 与首尾空白）。
    ///
    /// 等价于 C++ 的 `BigInt::fromString(decimal, &ok)`：文本不合法时返回 `None`
    /// （C++ 那边是「返回 0 + `ok = false`」，用 `unwrap_or_default()` 就能拿到那个 0）。
    #[must_use]
    pub fn from_string(decimal: &str) -> Option<BigInt> {
        BigInt::parse_decimal(decimal)
    }

    /// 是不是 0。
    #[must_use]
    pub fn is_zero(&self) -> bool {
        self.sign == 0
    }

    /// 符号：-1 / 0 / 1。
    #[must_use]
    pub fn sign(&self) -> i32 {
        self.sign
    }

    /// 十进制位数（0 算 1 位）。
    #[must_use]
    pub fn decimal_digits(&self) -> usize {
        if self.is_zero() {
            return 1;
        }
        let mut magnitude = self.abs();
        let mut digits = 0usize;
        while !magnitude.is_zero() {
            let remainder = magnitude.div_small(DECIMAL_CHUNK);
            digits += if magnitude.is_zero() {
                remainder.to_string().len()
            } else {
                9
            };
        }
        digits
    }

    /// 有效 limb（32 位）个数；0 是 0 个。
    #[must_use]
    pub fn limb_count(&self) -> usize {
        self.limbs.len()
    }

    /// NTT 切换阈值：两边 limb 个数之和达到它就改用 NTT。
    ///
    /// 默认 48000；测试和调参可以直接改（`1` = 永远走 NTT）。
    pub fn set_multiplication_threshold(limbs: usize) {
        MULTIPLICATION_THRESHOLD.store(limbs, AtomicOrdering::Relaxed);
    }

    /// 当前阈值。
    #[must_use]
    pub fn multiplication_threshold() -> usize {
        MULTIPLICATION_THRESHOLD.load(AtomicOrdering::Relaxed)
    }

    /// Karatsuba 基线阈值：较短操作数超过该 limb 数时使用 Karatsuba。
    ///
    /// 默认 96；设置为很大可强制使用学校算法，0 会按最小安全值 1 处理。
    pub fn set_karatsuba_threshold(limbs: usize) {
        KARATSUBA_THRESHOLD.store(limbs.max(1), AtomicOrdering::Relaxed);
    }

    /// 当前 Karatsuba 基线阈值。
    #[must_use]
    pub fn karatsuba_threshold() -> usize {
        KARATSUBA_THRESHOLD.load(AtomicOrdering::Relaxed)
    }

    /// Burnikel-Ziegler 除法的递归基线；除数不超过该 limb 数时使用长除法。
    ///
    /// 默认 56、最小 32；设置为很大可强制使用长除法。
    pub fn set_division_threshold(limbs: usize) {
        DIVISION_THRESHOLD.store(limbs.max(32), AtomicOrdering::Relaxed);
    }

    /// 当前 Burnikel-Ziegler 递归基线。
    #[must_use]
    pub fn division_threshold() -> usize {
        DIVISION_THRESHOLD.load(AtomicOrdering::Relaxed)
    }

    /// 截断成 64 位无符号整数（只取低 64 位，符号位丢弃）。
    #[must_use]
    pub fn to_u64(&self) -> u64 {
        let mut value = 0u64;
        let count = self.limbs.len().min(2);
        for index in (0..count).rev() {
            value = (value << 32) | self.limbs[index] as u64;
        }
        value
    }

    /// 截断成 64 位有符号整数（按位截断，与 C 的强转一致）。
    #[must_use]
    pub fn to_i64(&self) -> i64 {
        self.to_u64() as i64
    }

    /// 转双精度浮点（可能丢精度；符号跟着走）。
    #[must_use]
    pub fn to_f64(&self) -> f64 {
        let mut value = 0.0f64;
        for index in (0..self.limbs.len()).rev() {
            value = value * 4294967296.0 + self.limbs[index] as f64;
        }
        if self.sign < 0 {
            -value
        } else {
            value
        }
    }

    /// 绝对值。
    #[must_use]
    pub fn abs(&self) -> BigInt {
        let mut value = self.clone();
        if !value.is_zero() {
            value.sign = 1;
        }
        value
    }

    /// 比较。
    ///
    /// 小于 / 等于 / 大于分别返回 -1 / 0 / 1（与 C++ 版同）。Rust 的 `Ord` 走的是同一个
    /// 实现，所以 `<`、`>`、`sort()` 都能直接用。
    #[must_use]
    pub fn compare(&self, other: &BigInt) -> i32 {
        if self.sign != other.sign {
            return if self.sign < other.sign { -1 } else { 1 };
        }
        let magnitude = BigInt::compare_magnitude(self, other);
        if self.sign >= 0 {
            magnitude
        } else {
            -magnitude
        }
    }

    /// 同号幂：`self ^ exponent`（二进制快速幂，`0^0 = 1`）。
    #[must_use]
    pub fn pow(&self, exponent: u64) -> BigInt {
        let mut result = BigInt::from_i64(1);
        let mut base = self.clone();
        let mut remaining = exponent;
        while remaining != 0 {
            if remaining & 1 != 0 {
                result *= &base;
            }
            remaining >>= 1;
            if remaining != 0 {
                base = &base * &base;
            }
        }
        result
    }

    /// 除取整：`self / divisor` 与余数（向零截断，`self == q * divisor + r`）。
    ///
    /// 除数为零返回 `None`（此时不写输出），**不 panic** —— 与 C++ 的 `divmod` 同。
    #[must_use]
    pub fn divmod(&self, divisor: &BigInt) -> Option<(BigInt, BigInt)> {
        if divisor.is_zero() {
            return None;
        }
        if self.is_zero() {
            return Some((BigInt::zero(), BigInt::zero()));
        }
        let (mut quotient, mut remainder) = BigInt::divmod_magnitude(self, divisor);
        // 向零截断：商的符号是两边符号的积，余数符号跟被除数
        if !quotient.is_zero() {
            quotient.sign = self.sign * divisor.sign;
        }
        if !remainder.is_zero() {
            remainder.sign = self.sign;
        }
        Some((quotient, remainder))
    }

    /// 最大公约数（欧几里得，取绝对值；`gcd(0, 0) = 0`）。
    #[must_use]
    pub fn gcd(left: &BigInt, right: &BigInt) -> BigInt {
        let mut a = left.abs();
        let mut b = right.abs();
        while !b.is_zero() {
            let remainder = a.divmod(&b).expect("gcd: 除数非零").1;
            a = b;
            b = remainder;
        }
        a
    }

    /// 最小公倍数（`lcm(0, x) = 0`）。
    #[must_use]
    pub fn lcm(left: &BigInt, right: &BigInt) -> BigInt {
        if left.is_zero() || right.is_zero() {
            return BigInt::zero();
        }
        let divisor = BigInt::gcd(left, right);
        (left.abs() / divisor) * right.abs()
    }

    /// 同余幂：`self ^ exponent mod modulus`（快速幂，逐步取模）。
    ///
    /// # Panics
    ///
    /// 模数为零时 panic（对应 C++ 抛 `std::domain_error("bigint: 模数为零")`）。
    #[must_use]
    pub fn powmod(&self, exponent: u64, modulus: &BigInt) -> BigInt {
        if modulus.is_zero() {
            panic!("bigint: {}", NumberError::ZeroModulus);
        }
        let mut result = BigInt::from_i64(1);
        result %= modulus;
        let mut base = self % modulus;
        if !base.is_zero() && base.sign < 0 {
            base += modulus.abs();
        }
        let mut remaining = exponent;
        while remaining != 0 {
            if remaining & 1 != 0 {
                result = (result * &base) % modulus;
            }
            remaining >>= 1;
            if remaining != 0 {
                base = (&base * &base) % modulus;
            }
        }
        if !result.is_zero() && result.sign < 0 {
            result += modulus.abs();
        }
        result
    }

    // -----------------------------------------------------------------------
    //  规范化 / 幅值运算（C++ 版的 private 成员）
    // -----------------------------------------------------------------------

    /// 去掉高位 0 并同步符号（0 时 limbs 清空）。
    fn normalize(&mut self) {
        while self.limbs.last() == Some(&0) {
            self.limbs.pop();
        }
        if self.limbs.is_empty() {
            self.sign = 0;
        }
    }

    /// 幅值比较：`|left|` 与 `|right|`。
    fn compare_magnitude(left: &BigInt, right: &BigInt) -> i32 {
        if left.limbs.len() != right.limbs.len() {
            return if left.limbs.len() < right.limbs.len() {
                -1
            } else {
                1
            };
        }
        for index in (0..left.limbs.len()).rev() {
            if left.limbs[index] != right.limbs[index] {
                return if left.limbs[index] < right.limbs[index] {
                    -1
                } else {
                    1
                };
            }
        }
        0
    }

    /// 幅值相加（都按非负处理）。
    fn add_magnitude(left: &BigInt, right: &BigInt) -> BigInt {
        let mut out = BigInt::zero();
        let count = left.limbs.len().max(right.limbs.len());
        out.limbs = vec![0u32; count];
        let mut carry = 0u64;
        for index in 0..count {
            let a = if index < left.limbs.len() {
                left.limbs[index] as u64
            } else {
                0
            };
            let b = if index < right.limbs.len() {
                right.limbs[index] as u64
            } else {
                0
            };
            let value = a + b + carry;
            out.limbs[index] = value as u32;
            carry = value >> 32;
        }
        if carry != 0 {
            out.limbs.push(carry as u32);
        }
        out.normalize();
        if !out.limbs.is_empty() {
            out.sign = 1;
        }
        out
    }

    /// 幅值相减：要求 `|left| >= |right|`。
    fn sub_magnitude(left: &BigInt, right: &BigInt) -> BigInt {
        let mut out = BigInt::zero();
        out.limbs = vec![0u32; left.limbs.len()];
        let mut borrow = 0i64; // 0 或 -1
        for index in 0..left.limbs.len() {
            let a = left.limbs[index] as i64;
            let b = if index < right.limbs.len() {
                right.limbs[index] as i64
            } else {
                0
            };
            let mut value = a - b + borrow;
            if value < 0 {
                value += LIMB_BASE as i64;
                borrow = -1;
            } else {
                borrow = 0;
            }
            out.limbs[index] = value as u32;
        }
        out.normalize();
        if !out.limbs.is_empty() {
            out.sign = 1;
        }
        out
    }

    /// 幅值乘法（学校算法 / Karatsuba / NTT 三级分派）。
    fn mul_magnitude(left: &BigInt, right: &BigInt) -> BigInt {
        let mut out = BigInt::zero();
        if left.limbs.is_empty() || right.limbs.is_empty() {
            return out; // 0
        }

        // 规模够大就换 NTT（O(n log n)）：15 位数字 → 双素数卷积 → CRT → 打包回 limb。
        if left.limbs.len() + right.limbs.len()
            >= MULTIPLICATION_THRESHOLD.load(AtomicOrdering::Relaxed)
        {
            let a = to_base15(&left.limbs);
            let b = to_base15(&right.limbs);
            let residue1 = convolution::<NTT_PRIME1>(&a, &b);
            let residue2 = convolution::<NTT_PRIME2>(&a, &b);
            let prime1 = NTT_PRIME1 as u64;
            let prime2 = NTT_PRIME2 as u64;
            // CRT：x ≡ r1 (mod p1)、x ≡ r2 (mod p2) → x = r1 + p1 × t，
            // t = (r2 − r1) × p1^{-1} (mod p2)。
            let inverse = pow_mod_u32::<NTT_PRIME2>((prime1 % prime2) as u32, prime2 - 2) as u64;
            let count = residue1.len().max(residue2.len());
            let mut coefficients: Vec<u64> = Vec::with_capacity(count + 2);
            let mut carry = 0u64;
            for index in 0..count {
                let value1 = if index < residue1.len() {
                    residue1[index] as u64
                } else {
                    0
                };
                let value2 = if index < residue2.len() {
                    residue2[index] as u64
                } else {
                    0
                };
                let difference = (value2 + prime2 - (value1 % prime2)) % prime2;
                let factor = (difference * inverse) % prime2;
                let exact = value1 + prime1 * factor; // < p1 × p2 ≈ 2^60
                let current = exact + carry;
                coefficients.push(current & 0x7FFF); // 低 15 位
                carry = current >> DIGIT_BITS;
            }
            while carry != 0 {
                coefficients.push(carry & 0x7FFF);
                carry >>= DIGIT_BITS;
            }
            out.limbs = pack_base15(&coefficients);
            out.normalize();
            if !out.limbs.is_empty() {
                out.sign = 1;
            }
            return out;
        }

        let karatsuba_threshold = KARATSUBA_THRESHOLD.load(AtomicOrdering::Relaxed);
        out.limbs = if left.limbs.len().min(right.limbs.len()) > karatsuba_threshold {
            karatsuba_multiply(&left.limbs, &right.limbs, karatsuba_threshold)
        } else {
            schoolbook_multiply(&left.limbs, &right.limbs)
        };
        out.normalize();
        if !out.limbs.is_empty() {
            out.sign = 1;
        }
        out
    }

    /// 取一段小端 limb。
    fn limb_slice(value: &BigInt, offset: usize, count: usize) -> BigInt {
        if offset >= value.limbs.len() || count == 0 {
            return BigInt::zero();
        }
        let end = value.limbs.len().min(offset + count);
        let mut out = BigInt { sign: 1, limbs: value.limbs[offset..end].to_vec() };
        out.normalize();
        out
    }

    /// `high * base^low_width + low`。
    fn combine_blocks(high: &BigInt, low: &BigInt, low_width: usize) -> BigInt {
        let mut limbs = vec![0u32; low_width + high.limbs.len()];
        let low_count = low_width.min(low.limbs.len());
        limbs[..low_count].copy_from_slice(&low.limbs[..low_count]);
        limbs[low_width..].copy_from_slice(&high.limbs);
        let mut out = BigInt { sign: 1, limbs };
        out.normalize();
        out
    }

    /// 移位相减长除法：Burnikel-Ziegler 的小规模递归基线。
    fn divmod_base(left: &BigInt, right: &BigInt) -> (BigInt, BigInt) {
        let mut quotient = BigInt::zero();
        let mut remainder = left.abs();
        let divisor = right.abs();
        if BigInt::compare_magnitude(&remainder, &divisor) >= 0 {
            let shift = remainder.bit_length() - divisor.bit_length();
            let mut shifted = divisor.clone();
            shifted.shift_left(shift);
            let mut step = shift + 1;
            while step > 0 {
                step -= 1;
                if BigInt::compare_magnitude(&remainder, &shifted) >= 0 {
                    remainder = BigInt::sub_magnitude(&remainder, &shifted);
                    quotient.set_bit(step);
                }
                shifted.shift_right(1);
            }
            quotient.normalize();
            if !quotient.limbs.is_empty() {
                quotient.sign = 1;
            }
        }
        (quotient, remainder)
    }

    /// Burnikel-Ziegler 的 3n/2n 子问题。
    fn div_three_by_two(
        dividend: &BigInt,
        divisor: &BigInt,
        block_size: usize,
    ) -> (BigInt, BigInt) {
        let a3 = BigInt::limb_slice(dividend, 0, block_size);
        let a2 = BigInt::limb_slice(dividend, block_size, block_size);
        let a1 = BigInt::limb_slice(dividend, block_size * 2, block_size);
        let b2 = BigInt::limb_slice(divisor, 0, block_size);
        let b1 = BigInt::limb_slice(divisor, block_size, block_size);

        let (mut quotient, remainder_high) = if BigInt::compare_magnitude(&a1, &b1) < 0 {
            BigInt::div_two_by_one(
                &BigInt::combine_blocks(&a1, &a2, block_size),
                &b1,
                block_size,
            )
        } else {
            let quotient = BigInt { sign: 1, limbs: vec![u32::MAX; block_size] };
            let remainder = BigInt::combine_blocks(
                &BigInt::sub_magnitude(&a1, &b1),
                &a2,
                block_size,
            ) + &b1;
            (quotient, remainder)
        };

        let mut remainder = BigInt::combine_blocks(&remainder_high, &a3, block_size)
            - BigInt::mul_magnitude(&quotient, &b2);
        while remainder.sign < 0 {
            remainder += divisor;
            quotient -= BigInt::from_i64(1);
        }
        (quotient, remainder)
    }

    /// Burnikel-Ziegler 的 2n/1n 子问题。
    fn div_two_by_one(
        dividend: &BigInt,
        divisor: &BigInt,
        block_size: usize,
    ) -> (BigInt, BigInt) {
        if BigInt::compare_magnitude(dividend, divisor) < 0 {
            return (BigInt::zero(), dividend.clone());
        }
        let threshold = DIVISION_THRESHOLD.load(AtomicOrdering::Relaxed);
        if block_size <= threshold || block_size & 1 != 0 {
            return BigInt::divmod_base(dividend, divisor);
        }

        let half = block_size / 2;
        let low = BigInt::limb_slice(dividend, 0, half);
        let upper = BigInt::limb_slice(dividend, half, block_size + half);
        let (quotient_high, remainder_high) =
            BigInt::div_three_by_two(&upper, divisor, half);
        let (quotient_low, remainder) = BigInt::div_three_by_two(
            &BigInt::combine_blocks(&remainder_high, &low, half),
            divisor,
            half,
        );
        (
            BigInt::combine_blocks(&quotient_high, &quotient_low, half),
            remainder,
        )
    }

    /// 幅值除取整（默认 Burnikel-Ziegler，小规模回退长除法）。
    fn divmod_magnitude(left: &BigInt, right: &BigInt) -> (BigInt, BigInt) {
        let dividend = left.abs();
        let divisor = right.abs();
        if BigInt::compare_magnitude(&dividend, &divisor) < 0 {
            return (BigInt::zero(), dividend);
        }
        let threshold = DIVISION_THRESHOLD.load(AtomicOrdering::Relaxed);
        if divisor.limbs.len() <= threshold {
            return BigInt::divmod_base(&dividend, &divisor);
        }

        let mut block_size = 1usize;
        while block_size < divisor.limbs.len() {
            block_size <<= 1;
        }
        let padding = block_size - divisor.limbs.len();
        let top_shift = divisor.limbs.last().expect("除数非零").leading_zeros() as usize;
        let normalization_bits = padding * 32 + top_shift;
        let mut normalized_dividend = dividend;
        let mut normalized_divisor = divisor;
        normalized_dividend.shift_left(normalization_bits);
        normalized_divisor.shift_left(normalization_bits);

        let mut quotient = BigInt::zero();
        let mut remainder = BigInt::zero();
        let block_count = normalized_dividend.limbs.len().div_ceil(block_size);
        for block in (0..block_count).rev() {
            let chunk = BigInt::limb_slice(&normalized_dividend, block * block_size, block_size);
            let window = BigInt::combine_blocks(&remainder, &chunk, block_size);
            let (quotient_block, next_remainder) =
                BigInt::div_two_by_one(&window, &normalized_divisor, block_size);
            quotient = BigInt::combine_blocks(&quotient, &quotient_block, block_size);
            remainder = next_remainder;
        }
        remainder.shift_right(normalization_bits);
        quotient.normalize();
        remainder.normalize();
        (quotient, remainder)
    }

    /// 二进制位长度（0 返回 0）。
    fn bit_length(&self) -> usize {
        if self.limbs.is_empty() {
            return 0;
        }
        let mut bits = (self.limbs.len() - 1) * 32;
        let mut top = self.limbs[self.limbs.len() - 1];
        while top != 0 {
            bits += 1;
            top >>= 1;
        }
        bits
    }

    /// 取第 `bit` 位（二进制、小端）是不是 1；越界一律 `false`。
    ///
    /// C++ 里是 private 的辅助函数，Rust 版放出来当查询接口（长除法用它试探，
    /// 外部想按位看大数也省得自己拆 limb）。
    #[must_use]
    pub fn bit_at(&self, bit: usize) -> bool {
        let index = bit / 32;
        if index >= self.limbs.len() {
            return false;
        }
        (self.limbs[index] >> (bit % 32)) & 1 != 0
    }

    /// 把第 bit 位置 1。
    fn set_bit(&mut self, bit: usize) {
        let index = bit / 32;
        if index >= self.limbs.len() {
            self.limbs.resize(index + 1, 0);
        }
        self.limbs[index] |= 1 << (bit % 32);
    }

    /// 左移 `bits` 位（就地）。
    fn shift_left(&mut self, bits: usize) {
        if self.limbs.is_empty() || bits == 0 {
            return;
        }
        let whole = bits / 32;
        let part = bits % 32;
        let mut out = vec![0u32; self.limbs.len() + whole + 1];
        for index in 0..self.limbs.len() {
            let value = (self.limbs[index] as u64) << part;
            out[index + whole] |= value as u32;
            out[index + whole + 1] |= (value >> 32) as u32;
        }
        self.limbs = out;
        self.normalize();
    }

    /// 右移 `bits` 位（就地）。
    fn shift_right(&mut self, bits: usize) {
        if self.limbs.is_empty() || bits == 0 {
            return;
        }
        let whole = bits / 32;
        let part = bits % 32;
        if whole >= self.limbs.len() {
            self.limbs.clear();
            self.sign = 0;
            return;
        }
        let mut out = vec![0u32; self.limbs.len() - whole];
        for (index, slot) in out.iter_mut().enumerate() {
            let mut value = self.limbs[index + whole] >> part;
            if part != 0 && index + whole + 1 < self.limbs.len() {
                value |= self.limbs[index + whole + 1] << (32 - part);
            }
            *slot = value;
        }
        self.limbs = out;
        self.normalize();
    }

    /// 幅值 = 幅值 × `small` + `add`（`small`、`add` 都是 32 位以内）。
    fn mul_small_add(&mut self, small: u32, add: u32) {
        let mut carry = add as u64;
        for index in 0..self.limbs.len() {
            let value = self.limbs[index] as u64 * small as u64 + carry;
            self.limbs[index] = value as u32;
            carry = value >> 32;
        }
        while carry != 0 {
            self.limbs.push(carry as u32);
            carry >>= 32;
        }
        self.normalize();
        if !self.limbs.is_empty() && self.sign == 0 {
            self.sign = 1;
        }
    }

    /// 幅值 / `small` 并返回余数（`small != 0`）。
    fn div_small(&mut self, small: u32) -> u32 {
        let mut remainder = 0u64;
        for index in (0..self.limbs.len()).rev() {
            let value = (remainder << 32) | self.limbs[index] as u64;
            self.limbs[index] = (value / small as u64) as u32;
            remainder = value % small as u64;
        }
        self.normalize();
        remainder as u32
    }

    /// 把十进制文本按 `10^9` 一段一段地吃进来。
    fn parse_decimal(decimal: &str) -> Option<BigInt> {
        let bytes = decimal.as_bytes();
        let mut index = 0usize;
        while index < bytes.len() && (bytes[index] == b' ' || bytes[index] == b'\t') {
            index += 1;
        }
        let mut sign = 1i32;
        if index < bytes.len() && (bytes[index] == b'+' || bytes[index] == b'-') {
            if bytes[index] == b'-' {
                sign = -1;
            }
            index += 1;
        }
        if index >= bytes.len() {
            // 只有符号 / 空串
            return None;
        }
        for &byte in &bytes[index..] {
            if !byte.is_ascii_digit() {
                return None;
            }
        }

        // 从高位往低位：每 9 位凑一段，`value = value × 10^9 + 段`。
        let mut value = BigInt::zero();
        let mut cursor = index;
        while cursor < bytes.len() {
            let take = 9.min(bytes.len() - cursor);
            let mut chunk = 0u32;
            let mut scale = 1u32;
            for offset in 0..take {
                chunk = chunk * 10 + u32::from(bytes[cursor + offset] - b'0');
                scale *= 10;
            }
            value.mul_small_add(scale, chunk);
            cursor += take;
        }
        if !value.is_zero() {
            value.sign = sign;
        }
        Some(value)
    }

    // -----------------------------------------------------------------------
    //  运算符的「核心」实现（`std::ops` 的各个组合都转发到这里）
    // -----------------------------------------------------------------------

    /// `left + right`。
    fn core_add(left: &BigInt, right: &BigInt) -> BigInt {
        if left.is_zero() {
            return right.clone();
        }
        if right.is_zero() {
            return left.clone();
        }
        if left.sign == right.sign {
            let mut out = BigInt::add_magnitude(left, right);
            out.sign = left.sign;
            return out;
        }
        let magnitude = BigInt::compare_magnitude(left, right);
        if magnitude == 0 {
            return BigInt::zero();
        }
        if magnitude > 0 {
            let mut out = BigInt::sub_magnitude(left, right);
            out.sign = left.sign;
            return out;
        }
        let mut out = BigInt::sub_magnitude(right, left);
        out.sign = right.sign;
        out
    }

    /// `left - right`。
    fn core_sub(left: &BigInt, right: &BigInt) -> BigInt {
        BigInt::core_add(left, &-right)
    }

    /// `left * right`。
    fn core_mul(left: &BigInt, right: &BigInt) -> BigInt {
        if left.is_zero() || right.is_zero() {
            return BigInt::zero();
        }
        let mut out = BigInt::mul_magnitude(left, right);
        out.sign = left.sign * right.sign;
        out
    }

    /// `left / right`（除零 panic）。
    fn core_div(left: &BigInt, right: &BigInt) -> BigInt {
        match left.divmod(right) {
            Some((quotient, _)) => quotient,
            None => panic!("bigint: {}", NumberError::DivisionByZero),
        }
    }

    /// `left % right`（除零 panic）。
    fn core_rem(left: &BigInt, right: &BigInt) -> BigInt {
        match left.divmod(right) {
            Some((_, remainder)) => remainder,
            None => panic!("bigint: {}", NumberError::DivisionByZero),
        }
    }
}

/// 为五个二元运算符生成「值 / 引用」四种组合 + 两种 `op=` 实现。
macro_rules! impl_arithmetic {
    ($trait:ident, $method:ident, $core:ident, $assign_trait:ident, $assign_method:ident) => {
        impl $trait<&BigInt> for &BigInt {
            type Output = BigInt;
            fn $method(self, other: &BigInt) -> BigInt {
                BigInt::$core(self, other)
            }
        }
        impl $trait<BigInt> for &BigInt {
            type Output = BigInt;
            fn $method(self, other: BigInt) -> BigInt {
                BigInt::$core(self, &other)
            }
        }
        impl $trait<&BigInt> for BigInt {
            type Output = BigInt;
            fn $method(self, other: &BigInt) -> BigInt {
                BigInt::$core(&self, other)
            }
        }
        impl $trait<BigInt> for BigInt {
            type Output = BigInt;
            fn $method(self, other: BigInt) -> BigInt {
                BigInt::$core(&self, &other)
            }
        }
        impl $assign_trait<BigInt> for BigInt {
            fn $assign_method(&mut self, other: BigInt) {
                *self = BigInt::$core(self, &other);
            }
        }
        impl $assign_trait<&BigInt> for BigInt {
            fn $assign_method(&mut self, other: &BigInt) {
                *self = BigInt::$core(self, other);
            }
        }
    };
}

impl_arithmetic!(Add, add, core_add, AddAssign, add_assign);
impl_arithmetic!(Sub, sub, core_sub, SubAssign, sub_assign);
impl_arithmetic!(Mul, mul, core_mul, MulAssign, mul_assign);
impl_arithmetic!(Div, div, core_div, DivAssign, div_assign);
impl_arithmetic!(Rem, rem, core_rem, RemAssign, rem_assign);

impl Neg for BigInt {
    type Output = BigInt;

    fn neg(mut self) -> BigInt {
        self.sign = -self.sign;
        self
    }
}

impl Neg for &BigInt {
    type Output = BigInt;

    fn neg(self) -> BigInt {
        -self.clone()
    }
}

impl PartialEq for BigInt {
    fn eq(&self, other: &BigInt) -> bool {
        self.compare(other) == 0
    }
}

impl Eq for BigInt {}

impl PartialOrd for BigInt {
    fn partial_cmp(&self, other: &BigInt) -> Option<Ordering> {
        Some(self.cmp(other))
    }
}

impl Ord for BigInt {
    fn cmp(&self, other: &BigInt) -> Ordering {
        match self.compare(other) {
            -1 => Ordering::Less,
            1 => Ordering::Greater,
            _ => Ordering::Equal,
        }
    }
}

impl fmt::Display for BigInt {
    /// 十进制字符串（负数带 `-`；0 输出 `"0"`）。对应 C++ 的 `toString()`。
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        if self.is_zero() {
            return formatter.write_str("0");
        }
        let mut magnitude = self.abs();
        let mut chunks = Vec::new();
        while !magnitude.is_zero() {
            chunks.push(magnitude.div_small(DECIMAL_CHUNK));
        }
        let mut text = String::new();
        if self.sign < 0 {
            text.push('-');
        }
        text.push_str(&chunks[chunks.len() - 1].to_string());
        for index in (0..chunks.len() - 1).rev() {
            let part = chunks[index].to_string();
            for _ in 0..9 - part.len() {
                text.push('0');
            }
            text.push_str(&part);
        }
        formatter.write_str(&text)
    }
}

impl fmt::Debug for BigInt {
    /// 十进制形式（大数的内部 limb 数组对人没什么用，出错了想看的也是十进制）。
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(formatter, "BigInt({})", self)
    }
}

impl FromStr for BigInt {
    type Err = NumberError;

    /// 十进制文本 → `BigInt`（可带 `+` / `-` 号、允许前导 0 与首尾空白）。
    fn from_str(decimal: &str) -> Result<BigInt, NumberError> {
        BigInt::from_string(decimal).ok_or(NumberError::InvalidText)
    }
}

macro_rules! impl_from_integer {
    ($($type:ty => $method:ident),+ $(,)?) => {
        $(
            impl From<$type> for BigInt {
                fn from(value: $type) -> BigInt {
                    BigInt::$method(value as _)
                }
            }
        )+
    };
}

impl_from_integer!(i32 => from_i64, i64 => from_i64, u32 => from_u64, u64 => from_u64);

/// 整数平方根（向下取整）：二分逼近，只用大整数的加减乘除。
///
/// 大浮点的 `sqrt` 靠它（先放大到 `2 × digits` 位有效数字再开方）。
///
/// # Panics
///
/// `value` 为负时 panic（对应 C++ 抛 `std::domain_error`）。
#[must_use]
pub fn integer_sqrt(value: &BigInt) -> BigInt {
    if value.sign() < 0 {
        panic!("bigint: {}", NumberError::NegativeSqrt);
    }
    if value.is_zero() {
        return BigInt::zero();
    }
    // 二分：先找一个 high 使 high² > value，再不断折半逼近
    let mut low = BigInt::zero();
    let mut high = BigInt::from_i64(1);
    while &high * &high <= *value {
        high *= BigInt::from_i64(2);
    }
    while &high - &low > BigInt::from_i64(1) {
        let middle = (&low + &high) / BigInt::from_i64(2);
        if &middle * &middle <= *value {
            low = middle;
        } else {
            high = middle;
        }
    }
    low
}

/// 10 的 `exponent` 次幂（大浮点对齐指数时大量使用；幂用 NTT 乘法）。
#[must_use]
pub fn pow10(exponent: u64) -> BigInt {
    BigInt::from_i64(10).pow(exponent)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::test_support::threshold_guard;

    /// `bi(42)` ↔ C++ 测试里的 `BigInt(42)`。
    fn bi(value: i64) -> BigInt {
        BigInt::from_i64(value)
    }

    /// `dec("…")` ↔ C++ 测试里的 `BigInt("…")`（文本一定是合法的）。
    fn dec(text: &str) -> BigInt {
        BigInt::from_string(text).expect("测试文本应当合法")
    }

    /// 2^200 的十进制写法（C++ 测试里反复出现）。
    const TWO_POW_200: &str = "1606938044258990275541962092341162602522202993782792835301376";

    /// C++ `testConstruct()`：构造 / 十进制互转 / 截断。
    #[test]
    fn construct() {
        assert_eq!(BigInt::default().to_string(), "0");
        assert!(bi(0).is_zero());
        assert_eq!(bi(42).to_string(), "42");
        assert_eq!(bi(-42).to_string(), "-42");
        assert_eq!(bi(4294967296).to_string(), "4294967296"); // 2^32
        assert_eq!(bi(i64::MIN).to_string(), "-9223372036854775808"); // INT64_MIN

        assert!(dec("0").is_zero());
        assert_eq!(dec("+123").to_string(), "123");
        assert_eq!(dec("-000123").to_string(), "-123");
        assert_eq!(dec(TWO_POW_200).to_string(), TWO_POW_200); // 2^200

        // `None` 一举对应 C++ 的两个断言：值是 0（unwrap_or_default）且 ok == false
        assert_eq!(BigInt::from_string("12a"), None);
        assert_eq!(BigInt::from_string(""), None);
        assert_eq!(BigInt::from_string("-"), None);
        assert_eq!(
            BigInt::from_string("9999999999999999999999999999")
                .unwrap()
                .to_string(),
            "9999999999999999999999999999"
        );

        assert_eq!(dec(TWO_POW_200).to_u64(), 0);
        assert_eq!(
            BigInt::from_u64(u64::MAX).to_string(),
            "18446744073709551615"
        );
        assert_eq!(BigInt::from_u64(u64::MAX).to_u64(), u64::MAX);
        assert!(BigInt::from_u64(0).is_zero());
        assert_eq!(BigInt::from_u64(4294967296).to_string(), "4294967296");
        assert_eq!(bi(-1).to_u64(), 1); // 只看幅值
        assert_eq!(bi(12345).to_f64(), 12345.0);
        assert_eq!(bi(-12345).to_f64(), -12345.0);
        assert_eq!(dec(TWO_POW_200).decimal_digits(), 61);
        assert_eq!(bi(0).decimal_digits(), 1);
        assert_eq!(bi(1).limb_count(), 1);
        assert_eq!(bi(0).limb_count(), 0);
    }

    /// C++ `testCompare()`：比较与符号。
    #[test]
    fn compare_and_sign() {
        assert_eq!(bi(-5).compare(&bi(3)), -1);
        assert_eq!(bi(5).compare(&bi(-3)), 1);
        assert_eq!(bi(-5).compare(&bi(-3)), -1);
        assert_eq!(bi(5).compare(&bi(5)), 0);
        assert_eq!(bi(0).compare(&bi(0)), 0);
        assert!(bi(0) < bi(1));
        assert!(bi(-1) < bi(0));
        assert!(dec("100000000000000000000000") > dec("99999999999999999999999"));
        assert!(bi(7) != bi(8));
        assert!(bi(7) >= bi(7));
        assert!(bi(-7) <= bi(-7));
        assert_eq!(bi(-9).abs(), bi(9));
        assert_eq!(bi(9).sign(), 1);
        assert_eq!(bi(-9).sign(), -1);
        assert_eq!(bi(0).sign(), 0);
    }

    /// C++ `testArithmetic()`：加减乘（含进位 / 借位 / 符号交叉）与就地运算。
    #[test]
    fn arithmetic() {
        assert_eq!((bi(2) + bi(3)).to_string(), "5");
        assert_eq!((bi(4294967295) + bi(1)).to_string(), "4294967296");
        assert_eq!((bi(-5) + bi(3)).to_string(), "-2");
        assert_eq!((bi(5) + bi(-3)).to_string(), "2");
        assert_eq!(bi(-5) + bi(5), BigInt::zero());
        assert_eq!((bi(0) + bi(-7)).to_string(), "-7");

        assert_eq!((bi(2) - bi(3)).to_string(), "-1");
        assert_eq!((bi(-2) - bi(-3)).to_string(), "1");
        assert_eq!((bi(4294967296) - bi(1)).to_string(), "4294967295");
        assert_eq!(bi(3) - bi(3), BigInt::zero());

        assert_eq!((bi(6) * bi(7)).to_string(), "42");
        assert_eq!((bi(-6) * bi(7)).to_string(), "-42");
        assert_eq!((bi(-6) * bi(-7)).to_string(), "42");
        assert_eq!(bi(0) * bi(999), BigInt::zero());
        assert_eq!((bi(65536) * bi(65536)).to_string(), "4294967296");
        assert_eq!(
            (dec("12345678901234567890") * dec("98765432109876543210")).to_string(),
            "1219326311370217952237463801111263526900"
        );
        assert_eq!(
            (dec("99999999999999999999") * dec("99999999999999999999")).to_string(),
            "9999999999999999999800000000000000000001"
        );

        // 「值 / 引用」四种组合都要能编过
        let left = bi(6);
        let right = bi(7);
        assert_eq!(left.clone() + right.clone(), &left + &right);
        assert_eq!(&left + right.clone(), left.clone() + &right);
        assert_eq!(&left * &right, bi(42));

        let mut value = bi(10);
        value += bi(5);
        assert_eq!(value.to_string(), "15");
        value -= bi(20);
        assert_eq!(value.to_string(), "-5");
        value *= bi(-2);
        assert_eq!(value.to_string(), "10");
        value += &bi(0);
        assert_eq!(value.to_string(), "10");
        assert_eq!((&value + &bi(1)).to_string(), "11");
    }

    /// C++ `testDivision()`：除取整（向零截断）、余数符号、除零。
    #[test]
    fn division() {
        let (quotient, remainder) = bi(7).divmod(&bi(2)).expect("除数非零");
        assert_eq!(quotient.to_string(), "3");
        assert_eq!(remainder.to_string(), "1");
        assert_eq!((bi(-7) / bi(2)).to_string(), "-3"); // 向零截断
        assert_eq!((bi(-7) % bi(2)).to_string(), "-1"); // 余数跟被除数
        assert_eq!((bi(7) / bi(-2)).to_string(), "-3");
        assert_eq!((bi(7) % bi(-2)).to_string(), "1");
        assert_eq!(bi(0) / bi(5), BigInt::zero());
        assert_eq!(bi(3) / bi(1000), BigInt::zero());
        assert_eq!((bi(3) % bi(1000)).to_string(), "3");

        // 除零：`divmod` 给 None（不 panic）
        assert_eq!(bi(12345).divmod(&BigInt::zero()), None);

        let big = dec(TWO_POW_200); // 2^200
        let divisor = dec("12345678901234567890");
        assert_eq!(
            (big.clone() / divisor.clone()).to_string(),
            "130161982756436057245270848995428623856352"
        );
        assert_eq!(
            (big.clone() % divisor.clone()).to_string(),
            "6540610052683564096"
        );
        assert_eq!(
            &big / &divisor * &divisor + &big % &divisor,
            big,
            "恒等式 a == (a / b) * b + a % b"
        );
        assert_eq!((big.clone() / big.clone()).to_string(), "1");
        assert_eq!(big.clone() % big.clone(), BigInt::zero());
        let third = dec("999999999999999999999999999999");
        assert_eq!(&big / &third * &third + &big % &third, big);

        // 强制 Burnikel-Ziegler 与长除法分别计算同一组非 2 次幂长度数据。
        let bz_divisor = bi(2).pow(2050) + bi(123456789);
        let expected_quotient = bi(3).pow(800) + bi(77);
        let expected_remainder = bi(2).pow(1024) + bi(99);
        let bz_dividend = &bz_divisor * &expected_quotient + &expected_remainder;
        BigInt::set_division_threshold(32);
        let (bz_quotient, bz_remainder) = bz_dividend.divmod(&bz_divisor).expect("除数非零");
        BigInt::set_division_threshold(1_000_000);
        let (base_quotient, base_remainder) = bz_dividend.divmod(&bz_divisor).expect("除数非零");
        assert_eq!(bz_quotient, base_quotient);
        assert_eq!(bz_quotient, expected_quotient);
        assert_eq!(bz_remainder, base_remainder);
        assert_eq!(bz_remainder, expected_remainder);
        assert_eq!(-bz_dividend.clone() / &bz_divisor, -expected_quotient);
        assert_eq!(-bz_dividend % &bz_divisor, -expected_remainder);
        BigInt::set_division_threshold(56);
        assert_eq!(BigInt::division_threshold(), 56);
    }

    /// 多组不规则 limb 长度：Burnikel-Ziegler 与长除法必须给出相同商余数。
    #[test]
    fn division_kernels_agree_on_many_shapes() {
        let _guard = threshold_guard();
        let shapes = [
            (33usize, 2usize, 9usize),
            (34, 3, 20),
            (63, 2, 31),
            (65, 4, 32),
            (100, 3, 50),
            (129, 5, 64),
        ];
        let mut state = 0xD1A1_DE00_BADC_0FFEu64;
        for (divisor_limbs, quotient_limbs, remainder_limbs) in shapes {
            let mut divisor = random_bigint(&mut state, divisor_limbs);
            let top = divisor.limbs.last_mut().expect("随机除数非零");
            *top = (*top & 0x0000_FFFF) | 1; // 特意留下前导零，覆盖按位规范化。
            let expected_quotient = random_bigint(&mut state, quotient_limbs);
            let expected_remainder = random_bigint(&mut state, remainder_limbs);
            let dividend = &divisor * &expected_quotient + &expected_remainder;

            BigInt::set_division_threshold(32);
            let burnikel = dividend.divmod(&divisor).expect("除数非零");
            BigInt::set_division_threshold(usize::MAX);
            let base = dividend.divmod(&divisor).expect("除数非零");
            assert_eq!(burnikel, base, "divisor limb {divisor_limbs}");
            assert_eq!(burnikel.0, expected_quotient);
            assert_eq!(burnikel.1, expected_remainder);
        }
        BigInt::set_division_threshold(56);
        assert_eq!(BigInt::division_threshold(), 56);
    }

    /// C++ `testDivision()` 里那两个 `try / catch` 的 Rust 写法。
    #[test]
    #[should_panic(expected = "bigint: 除以零")]
    fn divide_by_zero_panics() {
        let _ = bi(1) / BigInt::zero();
    }

    /// 取余的除零同样 panic（C++ 是同一个 `std::domain_error`）。
    #[test]
    #[should_panic(expected = "bigint: 除以零")]
    fn remainder_by_zero_panics() {
        let _ = bi(1) % BigInt::zero();
    }

    /// C++ `testNtt()`：三条乘法内核（NTT / Karatsuba / 学校算法）必须逐位一致。
    ///
    /// 阈值是全局状态，这里持锁跑，免得和 `bigfloat` 那边动阈值的用例互相干扰。
    #[test]
    fn multiplication_kernels_agree() {
        let _guard = threshold_guard();

        // 造几个「够大」的数：2^1000、10^100、随机形状的 limb 序列
        let big = bi(2).pow(1000);
        let ten = bi(10).pow(100);
        let randomish = dec(
            "1234567890123456789012345678901234567890123456789012345678901234567890\
             9876543210987654321098765432109876543210987654321098765432109876543210",
        );

        // 依次强制 NTT、Karatsuba、学校算法，三批结果逐位比对
        BigInt::set_multiplication_threshold(1);
        let ntt1 = &big * &ten;
        let ntt2 = &big * &randomish;
        let ntt3 = &randomish * &randomish;
        let ntt4 = bi(2).pow(4096) * bi(2).pow(4096);
        BigInt::set_multiplication_threshold(1_000_000);
        BigInt::set_karatsuba_threshold(1);
        let karatsuba1 = &big * &ten;
        let karatsuba2 = &big * &randomish;
        let karatsuba3 = &randomish * &randomish;
        let karatsuba4 = bi(2).pow(4096) * bi(2).pow(4096);
        BigInt::set_karatsuba_threshold(1_000_000);
        let schoolbook1 = &big * &ten;
        let schoolbook2 = &big * &randomish;
        let schoolbook3 = &randomish * &randomish;
        let schoolbook4 = bi(2).pow(4096) * bi(2).pow(4096);
        assert_eq!(ntt1, karatsuba1);
        assert_eq!(karatsuba1, schoolbook1);
        assert_eq!(ntt2, karatsuba2);
        assert_eq!(karatsuba2, schoolbook2);
        assert_eq!(ntt3, karatsuba3);
        assert_eq!(karatsuba3, schoolbook3);
        assert_eq!(ntt4, karatsuba4);
        assert_eq!(karatsuba4, schoolbook4);

        // NTT 路径也要和「人工已知值」一致
        BigInt::set_multiplication_threshold(1);
        assert_eq!(ntt4, bi(2).pow(8192));
        assert_eq!(ntt1, bi(2).pow(1000) * bi(10).pow(100));
        assert_eq!(&ntt3 / &randomish, randomish);
        assert!(ntt3.divmod(&randomish).expect("除数非零").1.is_zero());

        // 大数除法 / 取余在 NTT 乘法下依然自洽：q × b + r == a
        let divisor = bi(3).pow(200);
        let (quotient, remainder) = ntt2.divmod(&divisor).expect("除数非零");
        assert_eq!(&quotient * &divisor + &remainder, ntt2);
        assert!(remainder < divisor);

        // 换回默认阈值，后面的用例不受影响
        BigInt::set_multiplication_threshold(48_000);
        BigInt::set_karatsuba_threshold(96);
        assert_eq!(BigInt::multiplication_threshold(), 48_000);
        assert_eq!(BigInt::karatsuba_threshold(), 96);
    }

    /// 固定种子的 LCG：造「随机形状」的大数（两边语言都能复现同一串数）。
    fn random_bigint(state: &mut u64, limbs: usize) -> BigInt {
        let base = BigInt::from_u64(1 << 32);
        let mut value = BigInt::zero();
        for _ in 0..limbs {
            *state = state
                .wrapping_mul(6364136223846793005)
                .wrapping_add(1442695040888963407);
            value = value * &base + BigInt::from_u64(*state >> 32);
        }
        value
    }

    /// 多组「随机形状」的 limb 数：三条乘法内核必须逐位一致，且乘除自洽。
    ///
    /// 这是 C++ 那边没写、但最值得补的一条 —— 学校算法的进位回写与 NTT 的
    /// CRT 重建都容易被「长度不整」的输入咬到（进位跨到最后一格、补零、进位链长度）。
    #[test]
    fn multiplication_kernels_agree_on_many_shapes() {
        let _guard = threshold_guard();
        let shapes = [
            (1usize, 1usize),
            (2, 3),
            (5, 7),
            (31, 33),
            (64, 65),
            (127, 128),
            (200, 7),
            (3, 512),
            (256, 256),
        ];
        let mut state = 0x1234_5678_9ABC_DEF0u64;
        for (left_limbs, right_limbs) in shapes {
            let left = random_bigint(&mut state, left_limbs);
            let right = random_bigint(&mut state, right_limbs);
            BigInt::set_multiplication_threshold(1); // 永远走 NTT
            let ntt = &left * &right;
            BigInt::set_multiplication_threshold(usize::MAX);
            BigInt::set_karatsuba_threshold(1); // 强制 Karatsuba（单 limb 是递归基线）
            let karatsuba = &left * &right;
            BigInt::set_karatsuba_threshold(usize::MAX); // 永远走学校算法
            let schoolbook = &left * &right;
            assert_eq!(ntt, karatsuba, "Karatsuba limb {left_limbs}×{right_limbs}");
            assert_eq!(ntt, schoolbook, "limb {left_limbs}×{right_limbs}");
            assert_eq!(&left * &right, &right * &left);

            // 乘除自洽：乘积除以任一个因数应当回到另一个因数（除法也顺手验了）
            assert_eq!(&ntt / &left, right, "除法 limb {left_limbs}×{right_limbs}");
            assert!((&ntt % &left).is_zero());
            assert_eq!(&ntt / &right, left);
        }
        BigInt::set_multiplication_threshold(48_000); // 换回实测缺省值
        BigInt::set_karatsuba_threshold(96);
        assert_eq!(BigInt::multiplication_threshold(), 48_000);
        assert_eq!(BigInt::karatsuba_threshold(), 96);
    }

    /// 幅值的位操作（`bit_at` / `bit_length`）—— 长除法与打包都靠它们。
    #[test]
    fn bit_helpers() {
        let value = bi(0b1010_0110);
        assert_eq!(value.bit_length(), 8);
        assert!(value.bit_at(1));
        assert!(value.bit_at(2));
        assert!(!value.bit_at(0));
        assert!(!value.bit_at(9));
        assert_eq!(BigInt::zero().bit_length(), 0);

        let mut shifted = bi(1);
        shifted.shift_left(64);
        assert_eq!(shifted.to_string(), "18446744073709551616"); // 2^64
        shifted.shift_right(32);
        assert_eq!(shifted.to_string(), "4294967296"); // 2^32
        shifted.shift_right(64);
        assert!(shifted.is_zero());
    }

    /// C++ `testPowerAndGcd()`：幂、gcd / lcm、同余幂。
    #[test]
    fn power_and_gcd() {
        assert_eq!(bi(2).pow(10).to_string(), "1024");
        assert_eq!(bi(2).pow(0).to_string(), "1");
        assert_eq!(bi(0).pow(0).to_string(), "1");
        assert!(bi(0).pow(5).is_zero());
        assert_eq!(bi(-3).pow(3).to_string(), "-27");
        assert_eq!(bi(2).pow(200).to_string(), TWO_POW_200);
        assert_eq!(bi(3).pow(40).to_string(), "12157665459056928801");

        let left = bi(2).pow(100) * bi(3);
        let right = bi(3).pow(50) * bi(5);
        assert_eq!(BigInt::gcd(&left, &right).to_string(), "3");
        assert!(BigInt::gcd(&bi(0), &bi(0)).is_zero());
        assert_eq!(BigInt::gcd(&bi(0), &bi(-12)).to_string(), "12");
        assert_eq!(BigInt::gcd(&bi(-18), &bi(24)).to_string(), "6");
        assert_eq!(BigInt::lcm(&bi(4), &bi(6)).to_string(), "12");
        assert!(BigInt::lcm(&bi(0), &bi(7)).is_zero());
        assert_eq!(
            BigInt::lcm(&left, &right).to_string(),
            "4550219075001074886663792637671283162463576301628293120"
        );

        assert_eq!(bi(7).powmod(300, &bi(1000000007)).to_string(), "678144625");
        assert_eq!(bi(2).powmod(10, &bi(1000)).to_string(), "24");
        assert_eq!(bi(-2).powmod(3, &bi(5)).to_string(), "2"); // (−8) mod 5
        assert_eq!(bi(123).powmod(0, &bi(97)).to_string(), "1");
    }

    /// 同余幂的模数为零 —— C++ 是 `std::domain_error("bigint: 模数为零")`。
    #[test]
    #[should_panic(expected = "bigint: 模数为零")]
    fn powmod_with_zero_modulus_panics() {
        let _ = bi(7).powmod(3, &BigInt::zero());
    }

    /// 整数平方根与 `10^n`。
    #[test]
    fn integer_square_root() {
        assert_eq!(integer_sqrt(&bi(0)).to_string(), "0");
        assert_eq!(integer_sqrt(&bi(1)).to_string(), "1");
        assert_eq!(integer_sqrt(&bi(15)).to_string(), "3");
        assert_eq!(integer_sqrt(&bi(16)).to_string(), "4");
        assert_eq!(integer_sqrt(&pow10(122)).to_string(), pow10(61).to_string());

        assert_eq!(pow10(0).to_string(), "1");
        assert_eq!(pow10(9).to_string(), "1000000000");
        assert_eq!(pow10(20).to_string(), "100000000000000000000");
    }

    /// 负数开方 —— C++ 是 `std::domain_error("bigint: 负数没有整数平方根")`。
    #[test]
    #[should_panic(expected = "bigint: 负数不能开平方")]
    fn integer_square_root_of_negative_panics() {
        let _ = integer_sqrt(&bi(-1));
    }

    /// 文本解析与 `Display` 的边界。
    #[test]
    fn parsing_and_display() {
        assert_eq!(dec("  42").to_string(), "42"); // 前导空白可以
        assert_eq!(dec("007").to_string(), "7");
        assert_eq!("+0".parse::<BigInt>().unwrap(), BigInt::zero());
        assert_eq!(
            "12a".parse::<BigInt>().unwrap_err(),
            NumberError::InvalidText
        );
        assert_eq!(
            "12 3".parse::<BigInt>().unwrap_err(),
            NumberError::InvalidText
        );
        assert_eq!(
            "42 ".parse::<BigInt>().unwrap_err(),
            NumberError::InvalidText
        );
        assert_eq!(
            BigInt::from_string("12a").unwrap_or_default(),
            BigInt::zero()
        );
        assert_eq!(format!("{:?}", bi(-42)), "BigInt(-42)");

        // 十进制的「9 位一段」输出：跨段补零不能丢
        assert_eq!(
            dec("1000000000000000000").to_string(),
            "1000000000000000000"
        );
        assert_eq!(dec("1000000000001").to_string(), "1000000000001");
        assert_eq!(bi(i64::MAX).to_string(), "9223372036854775807");
        assert_eq!(BigInt::from(u64::MAX).to_string(), u64::MAX.to_string());
        assert_eq!(BigInt::from(7u32).to_string(), "7");
        assert_eq!(BigInt::from(-7i32).to_string(), "-7");
    }
}
