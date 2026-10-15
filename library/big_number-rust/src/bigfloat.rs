//! 任意精度**十进制浮点** [`BigFloat`]：`mant × 10^exp10`，尾数复用 [`BigInt`]。
//!
//! 对应 C++ 版的 `bigfloat.hpp` + `bigfloat.cpp`。表示与参考实现
//! `eigenmath_bigint.c` 的 `BigFloat_T` 一致：尾数是任意精度整数、指数是**十进制**
//! —— 所以二进制浮点那套 `0.1 + 0.2 ≠ 0.3` 的表示误差这里不存在，大整数乘法直接吃到
//! [`BigInt`] 的 NTT 内核。
//!
//! 已实现：解析 / 输出（保留 N 位有效数字 + 恒定普通记法）、加减乘除、比较、舍入、平方根。
//! 没实现：`exp / log / π / sin / cos / tan / atan / asin / acos` —— 那些是另一层
//! （级数 + AGM），要做得单独一轮。**分数（大有理数）明确不做**。
//!
//! 精度口径与参考实现一致：**所有运算都在整数上做，多算几位再舍入**到目标有效位数。

use crate::bigint::{integer_sqrt, pow10, BigInt};
use crate::NumberError;
use std::cmp::Ordering;
use std::fmt;
use std::ops::{Add, AddAssign, Div, DivAssign, Mul, MulAssign, Neg, Sub, SubAssign};
use std::str::FromStr;

/// 默认有效位数（`/` 用；与 128 位浮点的十进制约 34 位同量级）。
const DEFAULT_DIVISION_DIGITS: i32 = 34;

/// `value / 10^shift`，四舍五入（远离零）。
///
/// `shift ≤ 0` 时原样返回。
fn divide_rounded(value: &BigInt, shift: i32) -> BigInt {
    if shift <= 0 {
        return value.clone();
    }
    let divisor = pow10(shift as u64);
    let (mut quotient, remainder) = value.divmod(&divisor).expect("除数非零");
    if remainder.abs() * BigInt::from_i64(2) >= divisor {
        quotient += BigInt::from_i64(if value.sign() < 0 { -1 } else { 1 });
    }
    quotient
}

/// 把一个「已经舍入好」的值写成十进制文本。
///
/// - `plain` 为真：恒定普通记法（`1e20` → `100000000000000000000`）；
/// - `plain` 为假：按 `%g` 口径，指数过大 / 过小时切科学记数法；
/// - `precision`：显示口径的有效位数（`%g` 里那个 P：科学记数法指数 `X` 满足
///   `X < -4 || X >= P` 时用科学记数法）。
fn format_decimal(mantissa: &BigInt, exp10: i32, plain: bool, precision: i32) -> String {
    if mantissa.is_zero() {
        return "0".to_string();
    }
    let negative = mantissa.sign() < 0;
    let digits = mantissa.abs().to_string();
    let digit_count = digits.len() as i32;
    let point_pos = digit_count + exp10; // 小数点位置：0 = 最左
    let exponent = point_pos - 1; // 科学记数法里的指数：值 = d.ddd × 10^exponent

    let mut text = String::new();
    if negative {
        text.push('-');
    }
    if !plain && (exponent < -4 || exponent >= precision) {
        text.push(digits.as_bytes()[0] as char);
        if digit_count > 1 {
            text.push('.');
            text.push_str(&digits[1..]);
        }
        text.push('e');
        if exponent >= 0 {
            text.push('+');
        }
        text.push_str(&exponent.to_string());
        return text;
    }
    if point_pos <= 0 {
        text.push_str("0.");
        for _ in 0..-point_pos {
            text.push('0');
        }
        text.push_str(&digits);
        return text;
    }
    if point_pos >= digit_count {
        text.push_str(&digits);
        for _ in 0..point_pos - digit_count {
            text.push('0');
        }
        return text;
    }
    text.push_str(&digits[..point_pos as usize]);
    text.push('.');
    text.push_str(&digits[point_pos as usize..]);
    text
}

/// 任意精度十进制浮点数：`值 = 尾数 × 10^exp10`（尾数带符号）。
///
/// 尾数去掉十进制尾随零是规范形（每次运算后 `normalize()`），所以 `1.50 == 1.5`。
#[derive(Clone, Default)]
pub struct BigFloat {
    /// 尾数（符号在里面）。
    mant: BigInt,
    /// 十进制指数。
    exp10: i32,
}

impl BigFloat {
    /// 造一个 0（等价于 C++ 的 `BigFloat()`）。
    #[must_use]
    pub fn zero() -> BigFloat {
        BigFloat { mant: BigInt::zero(), exp10: 0 }
    }

    /// 从十进制文本构造（不 panic 版）：`123.456` / `-0.5` / `.5` / `1e100` / `2.5E-3` 都行。
    ///
    /// 与 C++ 的 `BigFloat::fromString(text, &ok)` 对应：文本不合法时返回 `None`
    /// （C++ 那边是「返回 0 + `ok = false`」）。文本可以带首尾空白与 `+` / `-` 号。
    #[must_use]
    pub fn from_string(text: &str) -> Option<BigFloat> {
        let bytes = text.as_bytes();
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

        let mut integer_part = String::new();
        while index < bytes.len() && bytes[index].is_ascii_digit() {
            integer_part.push(bytes[index] as char);
            index += 1;
        }
        let mut fraction_part = String::new();
        if index < bytes.len() && bytes[index] == b'.' {
            index += 1;
            while index < bytes.len() && bytes[index].is_ascii_digit() {
                fraction_part.push(bytes[index] as char);
                index += 1;
            }
        }
        if integer_part.is_empty() && fraction_part.is_empty() {
            return None;
        }

        let mut exponent = 0i64;
        if index < bytes.len() && (bytes[index] == b'e' || bytes[index] == b'E') {
            index += 1;
            let mut exponent_sign = 1i64;
            if index < bytes.len() && (bytes[index] == b'+' || bytes[index] == b'-') {
                if bytes[index] == b'-' {
                    exponent_sign = -1;
                }
                index += 1;
            }
            if index >= bytes.len() || !bytes[index].is_ascii_digit() {
                return None;
            }
            while index < bytes.len() && bytes[index].is_ascii_digit() {
                exponent = exponent * 10 + i64::from(bytes[index] - b'0');
                index += 1;
                if exponent > 1_000_000 {
                    // 指数大得离谱就当语法错，别把内存吃光
                    return None;
                }
            }
            exponent *= exponent_sign;
        }
        while index < bytes.len() && (bytes[index] == b' ' || bytes[index] == b'\t') {
            index += 1;
        }
        if index != bytes.len() {
            return None;
        }

        let mut out = BigFloat::zero();
        // `BigInt` 的解析允许前导 0，整数部分与小数部分直接拼起来就是尾数
        out.mant = BigInt::from_string(&(integer_part + &fraction_part))?;
        if !out.mant.is_zero() && sign < 0 {
            out.mant = -out.mant;
        }
        let exponent_value = exponent - fraction_part.len() as i64;
        if exponent_value < i64::from(i32::MIN) || exponent_value > i64::from(i32::MAX) {
            return None;
        }
        out.exp10 = exponent_value as i32;
        out.normalize();
        Some(out)
    }

    /// 从整数构造（值就是那个整数，精确）。
    #[must_use]
    pub fn from_int(value: &BigInt) -> BigFloat {
        let mut out = BigFloat::zero();
        out.mant = value.clone();
        out.normalize();
        out
    }

    /// 从 `f64` 构造：取**能往返同一个 double 的最短十进制写法**。
    ///
    /// C++ 版是「从 1 位有效数字往上试 `%.*g`、用 `strtod` 读回来比对」；Rust 没有
    /// `printf`，这里用同样口径的 `{:.N e}`（科学记数法、N+1 位有效数字）配
    /// `str::parse::<f64>()` 做那一步往返比对。所以
    /// `from_double(0.1).to_string() == "0.1"`，而
    /// `from_double(0.1) + from_double(0.2)` 精确等于 `"0.3"`。
    ///
    /// 非有限值（NaN / ±inf）按 0 处理（与参考实现一致）。
    #[must_use]
    pub fn from_double(value: f64) -> BigFloat {
        BigFloat::from_double_with_limit(value, 17)
    }

    /// [`BigFloat::from_double`] 的带位数上限版本（对应 C++ 的 `fromDouble(value, digits)`）。
    ///
    /// `digits` 是有效位数**上限**，只在 `1..17` 内生效，缺省 17 = double 的往返精度。
    #[must_use]
    pub fn from_double_with_limit(value: f64, digits: i32) -> BigFloat {
        if !value.is_finite() {
            return BigFloat::zero(); // NaN / ±inf 按 0 处理
        }
        let limit = if digits > 0 && digits < 17 { digits } else { 17 };
        let mut out = BigFloat::zero();
        // 从 1 位有效数字往上试：第一个「读回来还是同一个 double」的写法就是最短写法。
        // 17 位一定能往返，所以循环最坏也就 17 轮。
        for precision in 1..=limit {
            let text = format!("{:.*e}", (precision - 1) as usize, value);
            out = match BigFloat::from_string(&text) {
                Some(value) => value,
                None => return BigFloat::zero(), // 理论上不会发生；兜底成 0
            };
            if text.parse::<f64>() == Ok(value) {
                break;
            }
        }
        out
    }

    /// 尾数（符号在里面）。
    #[must_use]
    pub fn mantissa(&self) -> &BigInt {
        &self.mant
    }

    /// 十进制指数：`值 = 尾数 × 10^指数`。
    #[must_use]
    pub fn exponent10(&self) -> i32 {
        self.exp10
    }

    /// 是不是 0。
    #[must_use]
    pub fn is_zero(&self) -> bool {
        self.mant.is_zero()
    }

    /// 符号：-1 / 0 / 1。
    #[must_use]
    pub fn sign(&self) -> i32 {
        self.mant.sign()
    }

    /// 舍入到 `digits` 位有效数字（四舍五入、远离零）。
    ///
    /// # Panics
    ///
    /// `digits ≤ 0` 时 panic（对应 C++ 抛 `std::invalid_argument`）。
    #[must_use]
    pub fn round(&self, digits: i32) -> BigFloat {
        if digits <= 0 {
            panic!("bigfloat: {}", NumberError::InvalidPrecision);
        }
        let mut out = self.clone();
        if out.mant.is_zero() {
            return out;
        }
        let have = out.mant.decimal_digits() as i32;
        if have <= digits {
            return out;
        }
        let shift = have - digits;
        out.mant = divide_rounded(&out.mant, shift);
        out.exp10 += shift;
        out.normalize();
        out
    }

    /// 十进制文本，保留 `digits` 位有效数字（对应 C++ 的 `toString(digits)`）。
    ///
    /// 指数不大时输出普通记法（`123.456`、`0.00123`），指数太大或太小改用科学记数法
    /// （`1.23e+100`、`4.5e-30`）—— 与 `%.17g` 同一口径。
    ///
    /// `digits ≤ 0` 表示**不舍入**（按值本身精确输出，显示阈值仍按缺省的 17 位）。
    #[must_use]
    pub fn to_string_with_digits(&self, digits: i32) -> String {
        let precision = if digits > 0 { digits } else { 17 };
        if digits <= 0 {
            return format_decimal(&self.mant, self.exp10, false, precision);
        }
        let value = self.round(digits);
        format_decimal(&value.mant, value.exp10, false, precision)
    }

    /// 十进制文本，**恒定用普通记法**（不切科学记数法），按值精确输出。
    ///
    /// 对应参考实现的 `EigenMath_BigFloatToDecimalString`（「供 infix / LaTeX 输出用」）：
    /// `BigFloat("1e20").to_plain_string()` → `"100000000000000000000"`，
    /// `(BigFloat("1e20") + BigFloat("1")).to_plain_string()` → `"100000000000000000001"`。
    #[must_use]
    pub fn to_plain_string(&self) -> String {
        format_decimal(&self.mant, self.exp10, true, 17)
    }

    /// [`BigFloat::to_plain_string`] 的带位数版本（对应 C++ 的 `toPlainString(digits)`）；
    /// `digits ≤ 0` 与 [`BigFloat::to_plain_string`] 等价（不舍入）。
    #[must_use]
    pub fn to_plain_string_with_digits(&self, digits: i32) -> String {
        if digits <= 0 {
            return format_decimal(&self.mant, self.exp10, true, 17);
        }
        let value = self.round(digits);
        format_decimal(&value.mant, value.exp10, true, digits)
    }

    /// 比较：小于 / 等于 / 大于分别返回 -1 / 0 / 1（`Ord` 走同一个实现）。
    #[must_use]
    pub fn compare(&self, other: &BigFloat) -> i32 {
        if self.mant.sign() != other.mant.sign() {
            return if self.mant.sign() < other.mant.sign() { -1 } else { 1 };
        }
        if self.mant.is_zero() {
            return 0;
        }
        // 同号：比 `尾数 × 10^exp10` 的绝对值 —— 对齐到更小的指数，再比整数
        let exponent = self.exp10.min(other.exp10);
        let left = self.mant.abs() * pow10((self.exp10 - exponent) as u64);
        let right = other.mant.abs() * pow10((other.exp10 - exponent) as u64);
        let magnitude = left.compare(&right);
        if self.mant.sign() >= 0 {
            magnitude
        } else {
            -magnitude
        }
    }

    /// 除法并指定有效位数（`/` 用 34 位）。
    ///
    /// # Panics
    ///
    /// 除数为零或 `digits ≤ 0` 时 panic（对应 C++ 抛 `std::domain_error` /
    /// `std::invalid_argument`）；不想处理 panic 就用 [`BigFloat::try_divide`]。
    #[must_use]
    pub fn divide(&self, divisor: &BigFloat, digits: i32) -> BigFloat {
        match self.try_divide(divisor, digits) {
            Ok(value) => value,
            Err(error) => panic!("bigfloat: {error}"),
        }
    }

    /// [`BigFloat::divide`] 的不 panic 版：除零 / 位数非法返回 `Err`。
    pub fn try_divide(&self, divisor: &BigFloat, digits: i32) -> Result<BigFloat, NumberError> {
        if divisor.is_zero() {
            return Err(NumberError::DivisionByZero);
        }
        if digits <= 0 {
            return Err(NumberError::InvalidPrecision);
        }
        if self.is_zero() {
            return Ok(BigFloat::zero());
        }
        // 把被除数放大，让商有 digits + 2 位有效数字，再舍入回 digits 位
        let numerator_digits = self.mant.decimal_digits() as i32;
        let denominator_digits = divisor.mant.decimal_digits() as i32;
        let shift = digits + 2 + denominator_digits - numerator_digits;
        let mut numerator = self.mant.clone();
        let mut exponent = self.exp10 - divisor.exp10;
        if shift > 0 {
            numerator *= pow10(shift as u64);
            exponent -= shift;
        }
        let (quotient, _) = numerator.divmod(&divisor.mant).expect("除数非零");
        let mut out = BigFloat::zero();
        out.mant = quotient;
        out.exp10 = exponent;
        out.normalize();
        Ok(out.round(digits))
    }

    /// 平方根，结果保留 `digits` 位有效数字。
    ///
    /// # Panics
    ///
    /// 负数开方或 `digits ≤ 0` 时 panic（对应 C++ 抛 `std::domain_error` /
    /// `std::invalid_argument`）；不想处理 panic 就用 [`BigFloat::try_sqrt`]。
    #[must_use]
    pub fn sqrt(&self, digits: i32) -> BigFloat {
        match self.try_sqrt(digits) {
            Ok(value) => value,
            Err(error) => panic!("bigfloat: {error}"),
        }
    }

    /// [`BigFloat::sqrt`] 的不 panic 版：负数 / 位数非法返回 `Err`。
    pub fn try_sqrt(&self, digits: i32) -> Result<BigFloat, NumberError> {
        if digits <= 0 {
            return Err(NumberError::InvalidPrecision);
        }
        if self.mant.sign() < 0 {
            return Err(NumberError::NegativeSqrt);
        }
        if self.is_zero() {
            return Ok(BigFloat::zero());
        }
        // 值 = m × 10^e：先把 e 凑成偶数（不够就 m × 10、e − 1），
        // 再把 m 放大到 2 × digits 位，做整数平方根，最后舍入。
        let mut m = self.mant.abs();
        let mut exponent = i64::from(self.exp10);
        if exponent % 2 != 0 {
            m *= BigInt::from_i64(10);
            exponent -= 1;
        }
        // 多算 2 位再舍回（尾数误差 < 1 个最低位，2 位余量足够躲开四舍五入的边界）
        let want = 2 * i64::from(digits) + 2;
        let have = m.decimal_digits() as i64;
        if have < want {
            let mut extra = want - have;
            // 放大的位数要跟指数**同奇偶**，这样 `exponent / 2` 才是精确的一半
            // —— 否则 sqrt(4) 会算出 6.324…×10⁻⁹ 这种结果。
            if (extra + exponent) % 2 != 0 {
                extra += 1;
            }
            m *= pow10(extra as u64);
            exponent -= extra;
        }
        let mut out = BigFloat::zero();
        out.mant = integer_sqrt(&m);
        out.exp10 = (exponent / 2) as i32;
        out.normalize();
        Ok(out.round(digits))
    }

    /// 调整成「尾数去掉十进制尾随零」的规范形（内部用，也开放给外部）。
    pub fn normalize(&mut self) {
        if self.mant.is_zero() {
            self.exp10 = 0;
            return;
        }
        // 先按 10^9 一档剥尾随零（比一位一位剥快得多），最后再用 10 收尾
        let billion = pow10(9);
        loop {
            match self.mant.divmod(&billion) {
                Some((quotient, remainder)) if remainder.is_zero() => {
                    self.mant = quotient;
                    self.exp10 += 9;
                }
                _ => break,
            }
        }
        let ten = BigInt::from_i64(10);
        loop {
            match self.mant.divmod(&ten) {
                Some((quotient, remainder)) if remainder.is_zero() => {
                    self.mant = quotient;
                    self.exp10 += 1;
                }
                _ => break,
            }
        }
    }

    /// `left + right`（对齐到较小的十进制指数、整数加减、再规范化）。
    fn core_add(left: &BigFloat, right: &BigFloat) -> BigFloat {
        if left.is_zero() {
            return right.clone();
        }
        if right.is_zero() {
            return left.clone();
        }
        let exponent = left.exp10.min(right.exp10);
        let mut out = BigFloat::zero();
        out.mant = &left.mant * pow10((left.exp10 - exponent) as u64)
            + &right.mant * pow10((right.exp10 - exponent) as u64);
        out.exp10 = exponent;
        out.normalize();
        out
    }

    /// `left - right`。
    fn core_sub(left: &BigFloat, right: &BigFloat) -> BigFloat {
        BigFloat::core_add(left, &-right)
    }

    /// `left * right`（尾数相乘、指数相加）。
    fn core_mul(left: &BigFloat, right: &BigFloat) -> BigFloat {
        let mut out = BigFloat::zero();
        out.mant = &left.mant * &right.mant;
        out.exp10 = left.exp10 + right.exp10;
        out.normalize();
        out
    }

    /// `left / right`（结果保留 34 位有效数字；除零 panic）。
    fn core_div(left: &BigFloat, right: &BigFloat) -> BigFloat {
        left.divide(right, DEFAULT_DIVISION_DIGITS)
    }
}

/// 为四个二元运算符生成「值 / 引用」四种组合 + 两种 `op=` 实现。
macro_rules! impl_arithmetic {
    ($trait:ident, $method:ident, $core:ident, $assign_trait:ident, $assign_method:ident) => {
        impl $trait<&BigFloat> for &BigFloat {
            type Output = BigFloat;
            fn $method(self, other: &BigFloat) -> BigFloat {
                BigFloat::$core(self, other)
            }
        }
        impl $trait<BigFloat> for &BigFloat {
            type Output = BigFloat;
            fn $method(self, other: BigFloat) -> BigFloat {
                BigFloat::$core(self, &other)
            }
        }
        impl $trait<&BigFloat> for BigFloat {
            type Output = BigFloat;
            fn $method(self, other: &BigFloat) -> BigFloat {
                BigFloat::$core(&self, other)
            }
        }
        impl $trait<BigFloat> for BigFloat {
            type Output = BigFloat;
            fn $method(self, other: BigFloat) -> BigFloat {
                BigFloat::$core(&self, &other)
            }
        }
        impl $assign_trait<BigFloat> for BigFloat {
            fn $assign_method(&mut self, other: BigFloat) {
                *self = BigFloat::$core(self, &other);
            }
        }
        impl $assign_trait<&BigFloat> for BigFloat {
            fn $assign_method(&mut self, other: &BigFloat) {
                *self = BigFloat::$core(self, other);
            }
        }
    };
}

impl_arithmetic!(Add, add, core_add, AddAssign, add_assign);
impl_arithmetic!(Sub, sub, core_sub, SubAssign, sub_assign);
impl_arithmetic!(Mul, mul, core_mul, MulAssign, mul_assign);
impl_arithmetic!(Div, div, core_div, DivAssign, div_assign);

impl Neg for BigFloat {
    type Output = BigFloat;

    fn neg(mut self) -> BigFloat {
        self.mant = -self.mant;
        self
    }
}

impl Neg for &BigFloat {
    type Output = BigFloat;

    fn neg(self) -> BigFloat {
        -self.clone()
    }
}

impl PartialEq for BigFloat {
    fn eq(&self, other: &BigFloat) -> bool {
        self.compare(other) == 0
    }
}

impl Eq for BigFloat {}

impl PartialOrd for BigFloat {
    fn partial_cmp(&self, other: &BigFloat) -> Option<Ordering> {
        Some(self.cmp(other))
    }
}

impl Ord for BigFloat {
    fn cmp(&self, other: &BigFloat) -> Ordering {
        match self.compare(other) {
            -1 => Ordering::Less,
            1 => Ordering::Greater,
            _ => Ordering::Equal,
        }
    }
}

impl fmt::Display for BigFloat {
    /// 十进制文本，缺省 17 位有效数字（对应 C++ 的 `toString()` / `%.17g` 口径）。
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(&self.to_string_with_digits(17))
    }
}

impl fmt::Debug for BigFloat {
    /// 十进制形式（内部是「尾数 + 指数」，出错了想看的还是十进制的值）。
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(formatter, "BigFloat({})", self)
    }
}

impl FromStr for BigFloat {
    type Err = NumberError;

    /// 十进制文本 → `BigFloat`（`123.456` / `-0.5` / `.5` / `1e100` / `2.5E-3`）。
    fn from_str(text: &str) -> Result<BigFloat, NumberError> {
        BigFloat::from_string(text).ok_or(NumberError::InvalidText)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::test_support::threshold_guard;

    /// `dec("…")` ↔ C++ 测试里的 `BigFloat("…")`（文本一定是合法的）。
    fn dec(text: &str) -> BigFloat {
        BigFloat::from_string(text).expect("测试文本应当合法")
    }

    /// C++ `testParseAndPrint()`：解析、输出、`fromDouble`、`fromInt`。
    #[test]
    fn parse_and_print() {
        assert!(dec("0").is_zero());
        assert_eq!(dec("123.456").to_string(), "123.456");
        assert_eq!(dec("-0.5").to_string(), "-0.5");
        assert_eq!(dec(".5").to_string(), "0.5");
        assert_eq!(dec("1.").to_string(), "1");
        assert_eq!(dec("+42").to_string(), "42");
        assert_eq!(dec("  7  ").to_string(), "7");
        assert_eq!(dec("0.00123").to_string(), "0.00123");
        assert_eq!(dec("00123.4500").to_string(), "123.45"); // 尾随零剥掉
        assert_eq!(dec("1e3").to_string(), "1000");
        assert_eq!(dec("2.5E-3").to_string(), "0.0025");
        assert_eq!(dec("1e100").to_string(), "1e+100");
        assert_eq!(dec("-4.5e-30").to_string(), "-4.5e-30");
        assert_eq!(dec("1.23456789e21").to_string(), "1.23456789e+21");
        assert_eq!(dec("1.23456789e21").to_plain_string(), "1234567890000000000000");
        assert_eq!(dec("0").to_plain_string(), "0");
        assert_eq!(dec("-0.5").to_plain_string(), "-0.5");
        assert_eq!(dec("1e20").to_plain_string(), "100000000000000000000");
        assert_eq!(dec("1e20").to_string_with_digits(0), "1e+20"); // 0 = 不舍入，仍按 %g 口径
        assert_eq!(dec("00123.4500").to_plain_string(), "123.45");
        assert_eq!(BigFloat::from_double(1.5).to_string(), "1.5");
        assert_eq!(BigFloat::from_double(0.1).to_string(), "0.1");
        assert_eq!(BigFloat::from_double(1.0 / 3.0).to_string(), "0.3333333333333333");
        assert_eq!(BigFloat::from_double(1e300).to_string(), "1e+300");
        assert!(BigFloat::from_double(f64::INFINITY).is_zero());
        assert!(BigFloat::from_double(f64::NAN).is_zero());

        // `None` 一举对应 C++ 的两个断言：值是 0（unwrap_or_default）且 ok == false
        assert_eq!(BigFloat::from_string("12a"), None);
        assert_eq!(BigFloat::from_string(""), None);
        assert_eq!(BigFloat::from_string("1e"), None);
        assert_eq!(BigFloat::from_string("1e99999999"), None);
        assert!(BigFloat::from_string("-0.0").unwrap().is_zero());

        let integer = BigInt::from_string("123456789012345678901234567890").expect("合法");
        assert_eq!(
            BigFloat::from_int(&integer).to_plain_string(),
            "123456789012345678901234567890"
        );
        assert_eq!(
            BigFloat::from_int(&BigInt::from_string("-42").expect("合法")).to_string(),
            "-42"
        );
    }

    /// C++ `testArithmetic()`：十进制浮点的「精确」特性与四则。
    #[test]
    fn arithmetic() {
        // 二进制浮点的经典坑：这里是十进制，精确等于 0.3
        assert_eq!((dec("0.1") + dec("0.2")).to_string(), "0.3");
        assert_eq!(
            (BigFloat::from_double(0.1) + BigFloat::from_double(0.2)).to_string(),
            "0.3"
        );
        assert_eq!((dec("1.5") + dec("2.25")).to_string(), "3.75");
        assert_eq!((dec("1") - dec("0.999")).to_string(), "0.001");
        assert_eq!((dec("-5") + dec("3")).to_string(), "-2");
        // 1e20 + 1 精确等于 100000000000000000001（21 位）：缺省只显示 17 位所以是 1e+20，
        // 要看全就得换恒定位数记法（或显式给位数）。
        assert_eq!((dec("1e20") + dec("1")).to_string(), "1e+20");
        assert_eq!((dec("1e20") + dec("1")).to_plain_string(), "100000000000000000001");
        assert_eq!((dec("2.5") * dec("4")).to_string(), "10");
        assert_eq!((dec("-2.5") * dec("4")).to_string(), "-10");
        assert_eq!((dec("0.001") * dec("1000")).to_string(), "1");
        assert_eq!((dec("1e100") * dec("1e-100")).to_string(), "1");
        assert_eq!(
            (dec("123456789012345678901234567890") * dec("2")).to_plain_string(),
            "246913578024691357802469135780"
        );

        assert_eq!(
            (dec("1") / dec("3")).to_string_with_digits(20),
            "0.33333333333333333333"
        );
        assert_eq!(
            (dec("2") / dec("7")).to_string_with_digits(30),
            "0.285714285714285714285714285714"
        );
        assert_eq!((dec("10") / dec("4")).to_string(), "2.5");
        assert_eq!(dec("6").divide(&dec("3"), 10).to_string(), "2");

        let mut value = dec("10.5");
        value += dec("0.25");
        assert_eq!(value.to_string(), "10.75");
        value *= dec("2");
        assert_eq!(value.to_string(), "21.5");
        value /= dec("2");
        assert_eq!(value.to_string(), "10.75");
        value -= dec("0.75");
        assert_eq!(value.to_string(), "10");
    }

    /// C++ `testRoundCompare()`：舍入、比较与边界。
    #[test]
    fn round_and_compare() {
        assert_eq!(dec("123.4567").round(5).to_string(), "123.46"); // 四舍五入
        assert_eq!(dec("123.4549").round(5).to_string(), "123.45");
        assert_eq!(dec("-1.25").round(2).to_string(), "-1.3"); // 远离零
        assert_eq!(dec("0.000999").to_string_with_digits(2), "0.001");
        assert_eq!(dec("999.9").round(3).to_string(), "1000"); // 进位跨位

        assert_eq!(dec("1.5"), dec("1.50"));
        assert!(dec("-1.5") < dec("1.5"));
        assert!(dec("0.001") < dec("0.01"));
        assert!(dec("-1e100") < dec("-1e99"));
        assert!(dec("1e100") > dec("9.99e99"));
        assert_eq!(dec("0"), dec("-0.0"));
        assert!(dec("0") != dec("1e-1000"));
        assert!(dec("2") >= dec("2"));
        assert_eq!(format!("{:?}", dec("-1.5")), "BigFloat(-1.5)");

        assert_eq!(dec("1").compare(&dec("1.000")), 0);
        assert_eq!(dec("1").compare(&dec("2")), -1);
        assert_eq!(dec("2").compare(&dec("1")), 1);
    }

    /// 除零 panic（C++ 是 `std::domain_error("bigfloat: 除以零")`）。
    #[test]
    #[should_panic(expected = "bigfloat: 除以零")]
    fn divide_by_zero_panics() {
        let _ = dec("1") / dec("0");
    }

    /// 有效位数非法 panic（C++ 是 `std::invalid_argument`）。
    #[test]
    #[should_panic(expected = "bigfloat: 有效位数必须是正数")]
    fn round_with_bad_precision_panics() {
        let _ = dec("1.5").round(0);
    }

    /// 不 panic 的入口：`try_divide` / `try_sqrt` 把错误当返回值给出来。
    #[test]
    fn fallible_entries() {
        assert_eq!(
            dec("1").try_divide(&dec("0"), 10).unwrap_err(),
            NumberError::DivisionByZero
        );
        assert_eq!(
            dec("1").try_divide(&dec("2"), 0).unwrap_err(),
            NumberError::InvalidPrecision
        );
        assert_eq!(dec("1").try_divide(&dec("2"), 10).unwrap().to_string(), "0.5");
        assert_eq!(dec("0").try_divide(&dec("2"), 10).unwrap().to_string(), "0");
        assert_eq!(dec("-1").try_sqrt(10).unwrap_err(), NumberError::NegativeSqrt);
        assert_eq!(dec("4").try_sqrt(0).unwrap_err(), NumberError::InvalidPrecision);
        assert_eq!(dec("4").try_sqrt(10).unwrap().to_string(), "2");
        assert_eq!("1.5".parse::<BigFloat>().unwrap(), dec("1.5"));
        assert_eq!("x".parse::<BigFloat>().unwrap_err(), NumberError::InvalidText);
    }

    /// C++ `testSqrt()`：平方根（保留 N 位有效数字，多算几位再舍入）。
    #[test]
    fn sqrt() {
        assert_eq!(dec("4").sqrt(10).to_string(), "2");
        assert_eq!(dec("2").sqrt(11).to_string(), "1.4142135624");
        assert_eq!(dec("2").sqrt(12).to_string(), "1.41421356237");
        assert_eq!(
            dec("2").sqrt(30).to_string_with_digits(30),
            "1.41421356237309504880168872421"
        );
        assert!(dec("0").sqrt(10).is_zero());
        assert_eq!(dec("1e100").sqrt(20).to_string(), "1e+50");
        assert_eq!(dec("0.0001").sqrt(10).to_string(), "0.01");
        assert_eq!(dec("6.25").sqrt(10).to_string(), "2.5");
    }

    /// 负数开方 panic（C++ 是 `std::domain_error("bigfloat: 负数不能开平方")`）。
    #[test]
    #[should_panic(expected = "bigfloat: 负数不能开平方")]
    fn sqrt_of_negative_panics() {
        let _ = dec("-1").sqrt(10);
    }

    /// C++ `testLargePrecision()`：大规模尾数运算确实吃到 `BigInt` 的 NTT 内核。
    ///
    /// 阈值是全局状态，这里持锁跑（见 `crate::test_support`）。
    #[test]
    fn large_precision() {
        let _guard = threshold_guard();

        let x = pow10(1500) + BigInt::from_i64(3); // 1501 位十进制
        let y = pow10(1400) - BigInt::from_i64(5);
        // (10^1500 + 3) × (10^1400 − 5) = 10^2900 − 5×10^1500 + 3×10^1400 − 15
        let expected = pow10(2900) - BigInt::from_i64(5) * pow10(1500)
            + BigInt::from_i64(3) * pow10(1400)
            - BigInt::from_i64(15);

        BigInt::set_multiplication_threshold(1); // 永远走 NTT
        let ntt = BigFloat::from_int(&x) * BigFloat::from_int(&y);
        let ntt_root = BigFloat::from_int(&pow10(200)).sqrt(60);
        BigInt::set_multiplication_threshold(1_000_000); // 先关闭 NTT
        BigInt::set_karatsuba_threshold(1_000_000); // 再关闭 Karatsuba
        let schoolbook = BigFloat::from_int(&x) * BigFloat::from_int(&y);

        assert_eq!(ntt, schoolbook);
        assert_eq!(ntt.to_plain_string(), expected.to_string());
        assert_eq!(ntt_root.to_plain_string(), pow10(100).to_string()); // sqrt(10^200) == 10^100
        assert!(ntt.mantissa().limb_count() >= 150); // 尾数确实很大（约 2900 位十进制）

        BigInt::set_multiplication_threshold(48_000); // 换回实测缺省值
        BigInt::set_karatsuba_threshold(96);
        assert_eq!(BigInt::multiplication_threshold(), 48_000);
        assert_eq!(BigInt::karatsuba_threshold(), 96);
    }
}
