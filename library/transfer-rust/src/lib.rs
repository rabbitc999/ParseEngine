//! 四元式优化与教学目标代码生成器的 Rust 实现。
//!
//! 与 C++ 版本保持同一套模型：
//!   * 变量类型：i8/u8/i16/u16/i32/u32/i64/u64/i128/u128/f32/f64，
//!     未声明时按 i16；
//!   * 机器原生字长 `word_bits ∈ {8,16,32,64}`：
//!       W == M 一条原生指令；W < M 归一化到类型宽度；
//!       W > M 变量按机器字分片（`x#0` 是低字），加减走进位链，其余交给
//!       运行时伪指令（RCALL / RCALLW）；
//!   * 乘法和除法用**固定的命名寄存器对**（HI/LO，目标机上映射成
//!     HI:LO / DX:AX / regHi:regLo）；
//!   * 输入输出：read / write / print / putc / getc；
//!   * 调用与中断：param / call / ret / int / iret。
//!
//! 所有状态都属于调用对象或返回值，`Program` 可以安全地交给不同线程处理。

use std::collections::{HashMap, HashSet};
use std::fmt;

// ---------------------------------------------------------------------------
// 类型与数值
// ---------------------------------------------------------------------------

pub const TYPE_NAMES: [&str; 13] = [
    "i8", "u8", "i16", "u16", "i32", "u32", "i64", "u64", "i128", "u128", "f32", "f64", "str",
];

pub fn default_type() -> &'static str {
    "i16"
}

pub fn is_type_name(name: &str) -> bool {
    TYPE_NAMES.contains(&name.trim().to_ascii_lowercase().as_str())
}

fn normalized_type(name: &str) -> Result<String, String> {
    let lowered = name.trim().to_ascii_lowercase();
    if TYPE_NAMES.contains(&lowered.as_str()) {
        Ok(lowered)
    } else {
        Err(format!("unknown type {name}"))
    }
}

fn is_float_type(ty: &str) -> bool {
    ty == "f32" || ty == "f64"
}

fn is_string_type(ty: &str) -> bool {
    ty == "str"
}

fn is_unsigned_type(ty: &str) -> bool {
    ty.starts_with('u')
}

fn type_bits(ty: &str) -> u32 {
    if is_string_type(ty) {
        return 0;
    }
    ty[1..].parse().unwrap_or(16)
}

fn mask_of(bits: u32) -> u128 {
    if bits >= 128 {
        u128::MAX
    } else {
        (1u128 << bits) - 1
    }
}

fn sign_extend(pattern: u128, bits: u32) -> i128 {
    let masked = pattern & mask_of(bits);
    let sign = 1u128 << (bits - 1);
    if masked & sign != 0 {
        (masked | !mask_of(bits)) as i128
    } else {
        masked as i128
    }
}

/// 带类型的数值：整数用位宽内的二补码模式，浮点用 `real`。
#[derive(Clone, Debug, PartialEq)]
pub struct Value {
    pub ty: String,
    pub bits: u128,
    pub real: f64,
    /// `str` 类型的文本内容；其他类型不使用。
    pub text: String,
}

impl Value {
    pub fn is_float(&self) -> bool {
        is_float_type(&self.ty)
    }
    pub fn is_string(&self) -> bool {
        is_string_type(&self.ty)
    }
    pub fn zero(ty: &str) -> Value {
        Value {
            ty: ty.to_string(),
            bits: 0,
            real: 0.0,
            text: String::new(),
        }
    }
}

pub fn is_integer_literal(text: &str) -> bool {
    let text = text.trim();
    let body = text.strip_prefix(['+', '-']).unwrap_or(text);
    !body.is_empty() && body.chars().all(|c| c.is_ascii_digit())
}

pub fn is_float_literal(text: &str) -> bool {
    let text = text.trim();
    let body = text.strip_prefix(['+', '-']).unwrap_or(text);
    if body.is_empty() {
        return false;
    }
    let mut digits = false;
    let mut dot = false;
    let mut exponent = false;
    let mut chars = body.chars().peekable();
    while let Some(c) = chars.next() {
        if c.is_ascii_digit() {
            digits = true;
        } else if c == '.' {
            if dot || exponent {
                return false;
            }
            dot = true;
        } else if c == 'e' || c == 'E' {
            if exponent || !digits {
                return false;
            }
            exponent = true;
            digits = false;
            if let Some(sign) = chars.peek() {
                if *sign == '+' || *sign == '-' {
                    chars.next();
                }
            }
        } else {
            return false;
        }
    }
    digits && (dot || exponent)
}

pub fn is_numeric_literal(text: &str) -> bool {
    is_integer_literal(text) || is_float_literal(text)
}

/// 字符串字面量：形如 `"..."`，引号内允许 \n \t \r \\ \" 转义。
pub fn is_string_literal(text: &str) -> bool {
    let value = text.trim();
    let bytes = value.as_bytes();
    if bytes.len() < 2 || bytes[0] != b'"' || bytes[bytes.len() - 1] != b'"' {
        return false;
    }
    let mut chars = value[1..value.len() - 1].chars();
    while let Some(c) = chars.next() {
        if c == '\\' {
            chars.next();
        } else if c == '"' {
            return false;
        }
    }
    true
}

/// 去掉两端引号并还原转义；不是字符串字面量时原样返回。
pub fn string_literal_text(text: &str) -> String {
    if !is_string_literal(text) {
        return text.to_string();
    }
    let inner = &text.trim()[1..text.trim().len() - 1];
    let mut out = String::new();
    let mut chars = inner.chars();
    while let Some(c) = chars.next() {
        if c == '\\' {
            match chars.next() {
                Some('n') => out.push('\n'),
                Some('t') => out.push('\t'),
                Some('r') => out.push('\r'),
                Some(other) => out.push(other),
                None => out.push('\\'),
            }
        } else {
            out.push(c);
        }
    }
    out
}

/// 解析字面量并按类型做范围检查。
pub fn literal_value(text: &str, ty: &str) -> Result<Value, String> {
    let ty = normalized_type(ty)?;
    let text = text.trim();
    if is_string_type(&ty) {
        let mut value = Value::zero(&ty);
        value.text = string_literal_text(text);
        return Ok(value);
    }
    if is_float_type(&ty) {
        if !is_numeric_literal(text) {
            return Err(format!("expected numeric literal: {text}"));
        }
        let parsed: f64 = text
            .parse()
            .map_err(|_| format!("invalid float literal: {text}"))?;
        return Ok(make_float(parsed, &ty));
    }
    if !is_integer_literal(text) {
        return Err(format!("expected integer literal for {ty}: {text}"));
    }
    let negative = text.starts_with('-');
    let digits = text.trim_start_matches(['+', '-']);
    let magnitude: u128 = digits
        .parse()
        .map_err(|_| format!("integer literal out of range: {text}"))?;
    let bits = type_bits(&ty);
    if is_unsigned_type(&ty) {
        if negative && magnitude != 0 {
            return Err(format!("negative literal for unsigned type: {text}"));
        }
        if magnitude > mask_of(bits) {
            return Err(format!("literal out of range for {ty}: {text}"));
        }
        return Ok(make_integer(magnitude, &ty));
    }
    let limit = 1u128 << (bits - 1); // 2^(bits-1)
    if negative {
        if magnitude > limit {
            return Err(format!("literal out of range for {ty}: {text}"));
        }
    } else if magnitude >= limit {
        return Err(format!("literal out of range for {ty}: {text}"));
    }
    let pattern = if negative && magnitude != 0 {
        mask_of(bits) - (magnitude - 1)
    } else {
        magnitude
    };
    Ok(make_integer(pattern, &ty))
}

/// 无符号上下文里的负数字面量按同宽有符号解析再转换（`-1` -> 0xFFFF…）。
pub fn literal_for(text: &str, ty: &str) -> Result<Value, String> {
    match literal_value(text, ty) {
        Ok(value) => Ok(value),
        Err(error) => {
            if is_unsigned_type(ty) {
                let signed = format!("i{}", type_bits(ty));
                if is_type_name(&signed) {
                    let parsed = literal_value(text, &signed)?;
                    return convert_value(&parsed, ty);
                }
            }
            Err(error)
        }
    }
}

fn make_float(value: f64, ty: &str) -> Value {
    let rounded = if type_bits(ty) == 32 {
        (value as f32) as f64
    } else {
        value
    };
    Value {
        ty: ty.to_string(),
        bits: 0,
        real: rounded,
        text: String::new(),
    }
}

fn make_integer(pattern: u128, ty: &str) -> Value {
    Value {
        ty: ty.to_string(),
        bits: pattern & mask_of(type_bits(ty)),
        real: 0.0,
        text: String::new(),
    }
}

fn value_as_f64(value: &Value) -> f64 {
    if value.is_float() {
        return value.real;
    }
    if is_unsigned_type(&value.ty) {
        value.bits as f64
    } else {
        sign_extend(value.bits, type_bits(&value.ty)) as f64
    }
}

/// 转换到目标类型（整数按位宽截断，整数↔浮点按 C 语义截断 / 舍入）。
pub fn convert_value(value: &Value, ty: &str) -> Result<Value, String> {
    let ty = normalized_type(ty)?;
    if is_string_type(&ty) || value.is_string() {
        if !is_string_type(&ty) || !value.is_string() {
            return Err(format!("cannot convert between {} and {ty}", value.ty));
        }
        let mut result = Value::zero(&ty);
        result.text = value.text.clone();
        return Ok(result);
    }
    if is_float_type(&ty) {
        return Ok(make_float(value_as_f64(value), &ty));
    }
    let bits = type_bits(&ty);
    if value.is_float() {
        let truncated = value.real.trunc();
        if !truncated.is_finite() {
            return Err(format!("cannot convert non-finite float to {ty}"));
        }
        if is_unsigned_type(&ty) {
            if truncated < 0.0 || truncated >= 2f64.powi(bits as i32) {
                return Err(format!("float out of range for {ty}"));
            }
            return Ok(make_integer(truncated as u128, &ty));
        }
        let limit = 2f64.powi(bits as i32 - 1);
        if truncated < -limit || truncated >= limit {
            return Err(format!("float out of range for {ty}"));
        }
        let magnitude = truncated.abs() as u128;
        let pattern = if truncated < 0.0 {
            mask_of(bits) - (magnitude - 1)
        } else {
            magnitude
        };
        return Ok(make_integer(pattern, &ty));
    }
    let pattern = if is_unsigned_type(&value.ty) {
        value.bits & mask_of(type_bits(&value.ty))
    } else {
        sign_extend(value.bits, type_bits(&value.ty)) as u128
    };
    Ok(make_integer(pattern, &ty))
}

/// 十进制显示（浮点按可读精度）。
pub fn display_value(value: &Value) -> String {
    if value.is_string() {
        return value.text.clone();
    }
    if value.is_float() {
        return format_float(value.real, type_bits(&value.ty));
    }
    if is_unsigned_type(&value.ty) {
        return (value.bits & mask_of(type_bits(&value.ty))).to_string();
    }
    sign_extend(value.bits, type_bits(&value.ty)).to_string()
}

fn format_float(value: f64, bits: u32) -> String {
    if value.is_nan() {
        return "nan".to_string();
    }
    if value.is_infinite() {
        return if value < 0.0 {
            "-inf".to_string()
        } else {
            "inf".to_string()
        };
    }
    let text = if bits == 32 {
        format!("{:.7}", value)
    } else {
        format!("{:.15}", value)
    };
    let text = text.trim_end_matches('0').trim_end_matches('.').to_string();
    if text.is_empty() {
        "0".to_string()
    } else {
        text
    }
}

pub fn value_is_zero(value: &Value) -> bool {
    if value.is_string() {
        return value.text.is_empty();
    }
    if value.is_float() {
        value.real == 0.0
    } else {
        (value.bits & mask_of(type_bits(&value.ty))) == 0
    }
}

pub fn value_low_byte(value: &Value) -> u8 {
    if value.is_float() {
        value.real.trunc() as i64 as u8
    } else {
        (value.bits & 0xFF) as u8
    }
}

fn bool_value(value: bool, ty: &str) -> Value {
    if is_float_type(ty) {
        make_float(if value { 1.0 } else { 0.0 }, ty)
    } else {
        make_integer(if value { 1 } else { 0 }, ty)
    }
}

fn as_integer(value: &Value, ty: &str) -> Result<Value, String> {
    convert_value(value, ty)
}

/// 按类型求值一个运算符；比较与 `odd` 的结果类型是 i8。
pub fn string_value(value: &Value) -> Result<&str, String> {
    if !value.is_string() {
        return Err(format!("expected a string value, got {}", value.ty));
    }
    Ok(&value.text)
}

fn string_result(text: impl Into<String>) -> Value {
    let mut value = Value::zero("str");
    value.text = text.into();
    value
}

fn index_result(value: i64, result_type: &str) -> Result<Value, String> {
    let ty = if is_type_name(result_type) {
        normalized_type(result_type)?
    } else {
        default_type().to_string()
    };
    if is_string_type(&ty) {
        return Err("string instruction needs a numeric result type".into());
    }
    if is_float_type(&ty) {
        return Ok(make_float(value as f64, &ty));
    }
    let bits = type_bits(&ty);
    let pattern = if value < 0 {
        mask_of(bits).wrapping_sub((value.unsigned_abs() as u128).wrapping_sub(1))
    } else {
        value as u128
    };
    Ok(make_integer(pattern, &ty))
}

/// `slen`：字节长度。
pub fn string_length(value: &Value, result_type: &str) -> Result<Value, String> {
    index_result(string_value(value)?.len() as i64, result_type)
}

/// `scmp`：字典序比较，返回 -1 / 0 / 1。
pub fn string_compare(lhs: &Value, rhs: &Value, result_type: &str) -> Result<Value, String> {
    let a = string_value(lhs)?;
    let b = string_value(rhs)?;
    let order = if a < b {
        -1
    } else if a > b {
        1
    } else {
        0
    };
    index_result(order, result_type)
}

/// `scat`：拼接。
pub fn string_concat(lhs: &Value, rhs: &Value) -> Result<Value, String> {
    Ok(string_result(format!(
        "{}{}",
        string_value(lhs)?,
        string_value(rhs)?
    )))
}

/// `ssub`：从 start 取 length 个字节（越界截断）。
pub fn substring(value: &Value, start: i64, length: i64) -> Result<Value, String> {
    let text = string_value(value)?.to_string();
    let size = text.len() as i64;
    let start = start.clamp(0, size);
    let count = length.clamp(0, size - start);
    Ok(string_result(
        &text[start as usize..(start + count) as usize],
    ))
}

/// `sfind`：子串首次出现的位置，找不到返回 -1。
pub fn string_find(haystack: &Value, needle: &Value, result_type: &str) -> Result<Value, String> {
    let text = string_value(haystack)?;
    let target = string_value(needle)?;
    let position = text.find(target).map(|index| index as i64).unwrap_or(-1);
    index_result(position, result_type)
}

/// 按类型求值一个运算符；比较与 `odd` 的结果类型是 i8。
pub fn eval_typed(base: &str, ty: &str, lhs: &Value, rhs: &Value) -> Result<Value, String> {
    let ty = normalized_type(ty)?;
    if is_string_type(&ty) {
        return Err(format!(
            "string operator {base} must use the string routines"
        ));
    }
    if is_float_type(&ty) {
        let a = value_as_f64(lhs);
        let b = value_as_f64(rhs);
        return Ok(match base {
            "+" => make_float(a + b, &ty),
            "-" => make_float(a - b, &ty),
            "*" => make_float(a * b, &ty),
            "/" => {
                if b == 0.0 {
                    return Err("float division by zero".into());
                }
                make_float(a / b, &ty)
            }
            "neg" => make_float(-a, &ty),
            "==" => bool_value(a == b, "i8"),
            "!=" => bool_value(a != b, "i8"),
            "<" => bool_value(a < b, "i8"),
            "<=" => bool_value(a <= b, "i8"),
            ">" => bool_value(a > b, "i8"),
            ">=" => bool_value(a >= b, "i8"),
            "cvt" => convert_value(lhs, &ty)?,
            _ => return Err(format!("operator {base} is not defined for {ty}")),
        });
    }
    let a = as_integer(lhs, &ty)?;
    let b = as_integer(rhs, &ty)?;
    let bits = type_bits(&ty);
    let unsigned = is_unsigned_type(&ty);
    let signed_a = sign_extend(a.bits, bits);
    let signed_b = sign_extend(b.bits, bits);
    Ok(match base {
        "+" => make_integer(a.bits.wrapping_add(b.bits), &ty),
        "-" => make_integer(a.bits.wrapping_sub(b.bits), &ty),
        "*" => make_integer(a.bits.wrapping_mul(b.bits), &ty),
        "&" => make_integer(a.bits & b.bits, &ty),
        "|" => make_integer(a.bits | b.bits, &ty),
        "^" => make_integer(a.bits ^ b.bits, &ty),
        "~" => make_integer(!a.bits, &ty),
        "neg" => make_integer(0u128.wrapping_sub(a.bits), &ty),
        "odd" => bool_value((a.bits & 1) != 0, "i8"),
        "cvt" => convert_value(lhs, &ty)?,
        "<<" | ">>" => {
            let amount = (b.bits & (bits as u128 - 1)) as u32;
            if base == "<<" {
                make_integer(a.bits.wrapping_shl(amount), &ty)
            } else if unsigned {
                make_integer((a.bits & mask_of(bits)) >> amount, &ty)
            } else {
                make_integer((signed_a >> amount) as u128, &ty)
            }
        }
        "==" => bool_value(
            if unsigned {
                a.bits == b.bits
            } else {
                signed_a == signed_b
            },
            "i8",
        ),
        "!=" => bool_value(
            if unsigned {
                a.bits != b.bits
            } else {
                signed_a != signed_b
            },
            "i8",
        ),
        "<" => bool_value(
            if unsigned {
                a.bits < b.bits
            } else {
                signed_a < signed_b
            },
            "i8",
        ),
        "<=" => bool_value(
            if unsigned {
                a.bits <= b.bits
            } else {
                signed_a <= signed_b
            },
            "i8",
        ),
        ">" => bool_value(
            if unsigned {
                a.bits > b.bits
            } else {
                signed_a > signed_b
            },
            "i8",
        ),
        ">=" => bool_value(
            if unsigned {
                a.bits >= b.bits
            } else {
                signed_a >= signed_b
            },
            "i8",
        ),
        "/" | "%" => {
            if b.bits == 0 {
                return Err("division by zero".into());
            }
            if unsigned {
                let (q, r) = (a.bits / b.bits, a.bits % b.bits);
                make_integer(if base == "/" { q } else { r }, &ty)
            } else {
                if signed_a == i128::MIN >> (128 - bits) && signed_b == -1 {
                    return Err("division overflow".into());
                }
                let (q, r) = (signed_a / signed_b, signed_a % signed_b);
                make_integer(if base == "/" { q as u128 } else { r as u128 }, &ty)
            }
        }
        _ => return Err(format!("unknown operator {base}")),
    })
}

/// 进位加法：`a + b + carry_in`，同时给出进位输出。
pub fn add_with_carry(
    ty: &str,
    lhs: &Value,
    rhs: &Value,
    carry_in: bool,
) -> Result<(Value, bool), String> {
    let ty = normalized_type(ty)?;
    if is_float_type(&ty) {
        return Err("carry addition requires an integer type".into());
    }
    let a = as_integer(lhs, &ty)?;
    let b = as_integer(rhs, &ty)?;
    let bits = type_bits(&ty);
    let (sum1, carry1) = a.bits.overflowing_add(b.bits);
    let (sum, carry2) = if carry_in {
        sum1.overflowing_add(1)
    } else {
        (sum1, false)
    };
    let carry_out = if bits >= 128 {
        carry1 || carry2
    } else {
        (sum & mask_of(bits)) != sum || (sum1 & mask_of(bits)) != sum1
    };
    Ok((make_integer(sum, &ty), carry_out))
}

/// 借位减法：`a - b - borrow_in`，同时给出借位输出。
pub fn sub_with_borrow(
    ty: &str,
    lhs: &Value,
    rhs: &Value,
    borrow_in: bool,
) -> Result<(Value, bool), String> {
    let ty = normalized_type(ty)?;
    if is_float_type(&ty) {
        return Err("borrow subtraction requires an integer type".into());
    }
    let a = as_integer(lhs, &ty)?;
    let b = as_integer(rhs, &ty)?;
    let borrow_first = a.bits < b.bits;
    let partial = a.bits.wrapping_sub(b.bits);
    let total = if borrow_in {
        partial.wrapping_sub(1)
    } else {
        partial
    };
    let borrow_second = borrow_in && partial == 0;
    Ok((make_integer(total, &ty), borrow_first || borrow_second))
}

/// 双寄存器乘法：返回 (高半, 低半)，支持到 64 位类型。
pub fn multiply_wide(ty: &str, lhs: &Value, rhs: &Value) -> Result<(Value, Value), String> {
    let ty = normalized_type(ty)?;
    if is_float_type(&ty) {
        return Err("two-register multiplication requires an integer type".into());
    }
    let bits = type_bits(&ty);
    if bits > 64 {
        return Err(format!(
            "two-register multiplication supports up to 64-bit types; use * for {ty}"
        ));
    }
    let a = as_integer(lhs, &ty)?;
    let b = as_integer(rhs, &ty)?;
    let product = (a.bits & mask_of(bits)).wrapping_mul(b.bits & mask_of(bits));
    Ok((
        make_integer(product >> bits, &ty),
        make_integer(product, &ty),
    ))
}

/// 双寄存器除法：返回 (商, 余数)。
pub fn divide_wide(ty: &str, lhs: &Value, rhs: &Value) -> Result<(Value, Value), String> {
    Ok((
        eval_typed("/", ty, lhs, rhs)?,
        eval_typed("%", ty, lhs, rhs)?,
    ))
}

/// 拆出 `*:u32` 的基名；`:=` 的冒号属于运算符本身。
pub fn op_base(op: &str) -> &str {
    if let Some(rest) = op.strip_prefix(":=") {
        return if rest.is_empty() { ":=" } else { ":=" };
    }
    match op.find(':') {
        Some(index) if index + 1 < op.len() => &op[..index],
        _ => op,
    }
}

/// 拆出 `*:u32` 的类型后缀；没有后缀时返回空串。
pub fn op_type(op: &str) -> &str {
    if let Some(rest) = op.strip_prefix(":=") {
        return rest.strip_prefix(':').unwrap_or("");
    }
    match op.find(':') {
        Some(index) if index + 1 < op.len() => &op[index + 1..],
        _ => "",
    }
}

fn typed_op(base: &str, ty: &str) -> String {
    if ty.is_empty() || ty == default_type() {
        base.to_string()
    } else {
        format!("{base}:{ty}")
    }
}

// ---------------------------------------------------------------------------
// 四元式
// ---------------------------------------------------------------------------

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Quad {
    pub op: String,
    pub a: String,
    pub b: String,
    pub result: String,
    /// 双结果指令的第二目标（`mul` 的高半、`div` 的余数）。
    pub extra: String,
}

impl Quad {
    pub fn new(op: &str, a: &str, b: &str, result: &str) -> Quad {
        Quad {
            op: op.into(),
            a: a.into(),
            b: b.into(),
            result: result.into(),
            extra: "_".into(),
        }
    }
}

pub type Program = Vec<Quad>;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Target {
    Mips,
    I8086,
    Arm,
}

#[derive(Clone, Debug)]
pub struct MachineConfig {
    pub target: Target,
    /// 机器原生字长（8 / 16 / 32 / 64），不是变量类型宽度。
    pub word_bits: u32,
    pub reg_a: String,
    pub reg_b: String,
    pub reg_c: String,
    pub reg_hi: String,
    pub reg_lo: String,
    pub data: HashMap<String, String>,
    pub rodata: HashMap<String, String>,
    pub data_image: Vec<u8>,
    /// 数据区字节镜像（`addr` / `load` / `store` 三件套用；与 stackvm 的 DataArea 同一份约定）。
    pub data_area_bytes: Vec<u8>,
    /// 数据区符号基址：符号名 -> 字节偏移（来自 `.layout` 里的 `offset=`）。
    pub data_area_symbols: HashMap<String, u64>,
    pub stack_words: usize,
    pub commands: HashMap<String, String>,
    /// 这台机器实现了哪些指令族（`features=` / `feature.<族>=off`）。
    pub features: HashSet<String>,
}

#[derive(Clone, Debug, Default)]
pub struct Block {
    pub begin: usize,
    pub end: usize,
    pub successors: Vec<usize>,
    pub predecessors: Vec<usize>,
    pub reachable: bool,
    pub reaching_in: HashSet<usize>,
    pub reaching_out: HashSet<usize>,
    pub live_in: HashSet<String>,
    pub live_out: HashSet<String>,
    pub available_in: HashSet<String>,
    pub available_out: HashSet<String>,
}

#[derive(Clone, Debug, Default)]
pub struct Analysis {
    pub blocks: Vec<Block>,
    pub instruction_block: Vec<usize>,
}

#[derive(Clone, Debug, Default)]
pub struct Optimization {
    pub program: Program,
    pub trace: Vec<String>,
    pub rounds: usize,
}

#[derive(Clone, Debug, Default)]
pub struct Execution {
    pub memory: HashMap<String, Value>,
    /// `write` 的输出（保留类型）。
    pub output: Vec<Value>,
    /// `print` / `putc` 的文本输出。
    pub text: String,
    /// 其他流（非 STDOUT）的文本输出：流名 -> 文本。
    pub stream_text: HashMap<String, String>,
    /// 其他流（非 STDOUT）的数值输出：流名 -> 值序列。
    pub streams: HashMap<String, Vec<Value>>,
    /// `system exit` 的退出码。
    pub exit_code: i32,
    pub steps: usize,
}

fn split_fields(line: &str) -> Vec<String> {
    let mut fields = Vec::new();
    let mut current = String::new();
    let mut quoted = false;
    let mut chars = line.chars().peekable();
    while let Some(c) = chars.next() {
        if quoted {
            current.push(c);
            if c == '\\' {
                if let Some(next) = chars.next() {
                    current.push(next);
                }
            } else if c == '"' {
                quoted = false;
            }
        } else if c == '"' {
            quoted = true;
            current.push(c);
        } else if c == ',' || c.is_whitespace() {
            if !current.is_empty() {
                fields.push(std::mem::take(&mut current));
            }
        } else {
            current.push(c);
        }
    }
    if !current.is_empty() {
        fields.push(current);
    }
    fields
}

fn strip_quotes(text: &str) -> String {
    let bytes = text.as_bytes();
    if bytes.len() >= 2 && bytes[0] == b'"' && bytes[bytes.len() - 1] == b'"' {
        text[1..text.len() - 1].to_string()
    } else {
        text.to_string()
    }
}

fn unescape(text: &str) -> String {
    let mut out = String::new();
    let mut chars = text.chars();
    while let Some(c) = chars.next() {
        if c == '\\' {
            match chars.next() {
                Some('n') => out.push('\n'),
                Some('t') => out.push('\t'),
                Some('r') => out.push('\r'),
                Some(other) => out.push(other),
                None => out.push('\\'),
            }
        } else {
            out.push(c);
        }
    }
    out
}

fn escape(text: &str) -> String {
    let mut out = String::new();
    for c in text.chars() {
        match c {
            '\n' => out.push_str("\\n"),
            '\t' => out.push_str("\\t"),
            '\r' => out.push_str("\\r"),
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            other => out.push(other),
        }
    }
    out
}

fn is_binary_base(base: &str) -> bool {
    matches!(
        base,
        "+" | "-"
            | "*"
            | "/"
            | "%"
            | "=="
            | "!="
            | "<"
            | "<="
            | ">"
            | ">="
            | "<<"
            | ">>"
            | "&"
            | "|"
            | "^"
            | "+c"
            | "-b"
            | "mul"
            | "div"
    )
}

fn is_compare_base(base: &str) -> bool {
    matches!(base, "==" | "!=" | "<" | "<=" | ">" | ">=")
}

fn is_unary_base(base: &str) -> bool {
    matches!(base, "neg" | "odd" | "~" | "cvt")
}

fn is_pair_base(base: &str) -> bool {
    matches!(base, "mul" | "div")
}

/// 字符串 / 流 / 向量 / 文件指令族与 `system` 系统调用。
fn is_string_op(base: &str) -> bool {
    matches!(base, "scopy" | "slen" | "scmp" | "scat" | "ssub" | "sfind")
}

fn is_stream_op(base: &str) -> bool {
    matches!(base, "stream" | "sread" | "swrite" | "sclose")
}

fn is_vector_op(base: &str) -> bool {
    matches!(
        base,
        "vload" | "vstore" | "vcopy" | "vfill" | "vsum" | "vmax" | "vmin"
    )
}

fn is_file_op(base: &str) -> bool {
    matches!(base, "fopen" | "fread" | "fwrite" | "fclose" | "feof")
}

fn identifier(text: &str) -> bool {
    let mut chars = text.chars();
    match chars.next() {
        Some(first) if first.is_alphabetic() || first == '_' || first == '$' => {}
        _ => return false,
    }
    chars.all(|c| c.is_alphanumeric() || c == '_' || c == '.' || c == '$')
}

fn value_operand(text: &str) -> bool {
    text != "_" && (is_numeric_literal(text) || is_string_literal(text) || identifier(text))
}

/// 收集 `decl` 声明的变量类型。
pub fn variable_types(program: &Program) -> HashMap<String, String> {
    let mut out = HashMap::new();
    for quad in program {
        if op_base(&quad.op) == "decl" {
            if let Ok(ty) = normalized_type(&quad.a) {
                out.insert(quad.result.clone(), ty);
            }
        }
    }
    // 数据区三件套的结果：基址与取出来的槽位都是**8 字节句柄**（i64），
    // 窄机器上按机器字分片存放 —— 和 stackvm 的 8 字节槽位一致。
    for quad in program {
        let base = op_base(&quad.op);
        if (base == "addr" || base == "load")
            && identifier(&quad.result)
            && !out.contains_key(&quad.result)
        {
            out.insert(quad.result.clone(), "i64".to_string());
        }
    }
    out
}

fn operation_type(quad: &Quad, declared: &HashMap<String, String>) -> String {
    let suffix = op_type(&quad.op);
    if !suffix.is_empty() {
        if let Ok(ty) = normalized_type(suffix) {
            return ty;
        }
    }
    if let Some(ty) = declared.get(&quad.result) {
        return ty.clone();
    }
    if let Some(ty) = declared.get(&quad.a) {
        return ty.clone();
    }
    if let Some(ty) = declared.get(&quad.b) {
        return ty.clone();
    }
    default_type().to_string()
}

fn result_type(quad: &Quad, declared: &HashMap<String, String>) -> String {
    if let Some(ty) = declared.get(&quad.result) {
        return ty.clone();
    }
    let base = op_base(&quad.op);
    if is_compare_base(base) || base == "odd" {
        return "i8".to_string();
    }
    operation_type(quad, declared)
}

/// 求一条四元式的运算类型。
pub fn quad_type(quad: &Quad, declared: &HashMap<String, String>) -> String {
    operation_type(quad, declared)
}

pub fn read_quads(text: &str) -> Result<Program, String> {
    let mut program = Vec::new();
    for (index, raw) in text.lines().enumerate() {
        // 兼容 Windows 编辑器写出的 UTF-8 BOM。
        let raw = if index == 0 {
            raw.trim_start_matches('\u{feff}')
        } else {
            raw
        };
        let mut line = raw.split('#').next().unwrap_or("").trim().to_string();
        if line.is_empty() {
            continue;
        }
        if let Some(open) = line.find('(') {
            let prefix = line[..open].trim().to_string();
            if !prefix.is_empty() && !prefix.ends_with(':') {
                return Err(format!("invalid quadruple index at line {}", index + 1));
            }
            if !line.ends_with(')') {
                return Err(format!("missing ')' at line {}", index + 1));
            }
            line = line[open + 1..line.len() - 1].to_string();
        }
        let fields = split_fields(&line);
        if fields.len() != 4 && fields.len() != 5 {
            return Err(format!(
                "expected four or five fields at line {}",
                index + 1
            ));
        }
        let mut op = fields[0].clone();
        if op == "=" {
            op = "==".into();
        }
        if op == "#" || op == "<>" {
            op = "!=".into();
        }
        let a = if op_base(&op) == "print" {
            strip_quotes(&fields[1])
        } else {
            fields[1].clone()
        };
        program.push(Quad {
            op,
            a,
            b: fields[2].clone(),
            result: fields[3].clone(),
            extra: fields.get(4).cloned().unwrap_or_else(|| "_".to_string()),
        });
    }
    validate(&program)?;
    Ok(program)
}

pub fn write_quads(program: &Program) -> String {
    let mut out = String::new();
    for (index, quad) in program.iter().enumerate() {
        let a = if op_base(&quad.op) == "print" {
            format!("\"{}\"", escape(&quad.a))
        } else {
            quad.a.clone()
        };
        out.push_str(&format!(
            "{index}: ({}, {}, {}, {}",
            quad.op, a, quad.b, quad.result
        ));
        if !quad.extra.is_empty() && quad.extra != "_" {
            out.push_str(&format!(", {}", quad.extra));
        }
        out.push_str(")\n");
    }
    out
}

pub fn validate(program: &Program) -> Result<(), String> {
    let mut labels: HashSet<String> = HashSet::new();
    let mut declared: HashMap<String, String> = HashMap::new();
    for (index, quad) in program.iter().enumerate() {
        let base = op_base(&quad.op);
        let fail = |what: &str| Err(format!("invalid quadruple {index} ({}): {what}", quad.op));
        if base == "decl" {
            if !is_type_name(&quad.a) {
                return fail("expected a type name");
            }
            if quad.b != "_" && quad.b != "global" && quad.b != "local" {
                return fail("scope must be global / local / _");
            }
            if !identifier(&quad.result) {
                return fail("expected a variable name");
            }
            let ty = normalized_type(&quad.a)?;
            if let Some(previous) = declared.get(&quad.result) {
                if *previous != ty {
                    return fail("declared twice with different types");
                }
            }
            declared.insert(quad.result.clone(), ty);
            continue;
        }
        let suffix = op_type(&quad.op);
        if !suffix.is_empty() && !is_type_name(suffix) {
            return fail(&format!("unknown type '{suffix}'"));
        }
        let ok = if base == "label" || base == "j" {
            if quad.a != "_" || quad.b != "_" || !identifier(&quad.result) {
                return fail("expected (_, _, label)");
            }
            if base == "label" && !labels.insert(quad.result.clone()) {
                return fail("duplicate label");
            }
            true
        } else if base == "jz" || base == "jnz" {
            value_operand(&quad.a) && quad.b == "_" && identifier(&quad.result)
        } else if base == "read" || base == "getc" {
            quad.a == "_" && quad.b == "_" && identifier(&quad.result)
        } else if base == "write" || base == "putc" {
            value_operand(&quad.a) && quad.b == "_" && quad.result == "_"
        } else if base == "print" {
            quad.a != "_" && quad.b == "_" && quad.result == "_"
        } else if base == "addr" || base == "load" || base == "store" {
            // 数据区三件套（语义与 library/stackvm 的 ADDR / LOADAT / STOREAT 相同）：
            //   (addr, 符号, _, 目标)   把数据区里该符号的基址写进目标变量
            //   (load, 指针, _, 目标)   按指针读 8 字节（小端）
            //   (store, 指针, 值, _)    按指针写 8 字节（小端）
            // `addr` 的第一个操作数是数据区符号名（不是变量），不参与变量类型推断；
            // 地址与取值都按 8 字节句柄处理，指令由三套目标机生成、由模拟器执行。
            if base == "addr" {
                identifier(&quad.a) && quad.b == "_" && identifier(&quad.result)
            } else if base == "load" {
                value_operand(&quad.a) && quad.b == "_" && identifier(&quad.result)
            } else {
                value_operand(&quad.a) && value_operand(&quad.b) && quad.result == "_"
            }
        } else if base == "param" {
            value_operand(&quad.a) && quad.b == "_" && identifier(&quad.result)
        } else if base == "call" || base == "int" {
            identifier(&quad.a) && quad.b == "_" && (quad.result == "_" || identifier(&quad.result))
        } else if base == "ret" {
            (quad.a == "_" || value_operand(&quad.a)) && quad.b == "_" && quad.result == "_"
        } else if base == "iret" || base == "halt" {
            quad.a == "_" && quad.b == "_" && quad.result == "_"
        } else if base == ":=" {
            value_operand(&quad.a) && quad.b == "_" && identifier(&quad.result)
        } else if is_pair_base(base) {
            value_operand(&quad.a)
                && value_operand(&quad.b)
                && identifier(&quad.result)
                && identifier(&quad.extra)
                && quad.extra != quad.result
        } else if is_unary_base(base) {
            if base == "cvt" && suffix.is_empty() {
                return fail("cvt needs a target type, e.g. cvt:f64");
            }
            value_operand(&quad.a) && quad.b == "_" && identifier(&quad.result)
        } else if is_binary_base(base) {
            value_operand(&quad.a) && value_operand(&quad.b) && identifier(&quad.result)
        } else if is_string_op(base) {
            if base == "scopy" || base == "slen" {
                value_operand(&quad.a) && quad.b == "_" && identifier(&quad.result)
            } else if base == "ssub" {
                value_operand(&quad.a)
                    && value_operand(&quad.b)
                    && identifier(&quad.result)
                    && value_operand(&quad.extra)
            } else {
                value_operand(&quad.a) && value_operand(&quad.b) && identifier(&quad.result)
            }
        } else if is_stream_op(base) {
            if base == "stream" || base == "sread" {
                value_operand(&quad.a) && quad.b == "_" && identifier(&quad.result)
            } else if base == "swrite" {
                value_operand(&quad.a) && value_operand(&quad.b) && quad.result == "_"
            } else {
                value_operand(&quad.a) && quad.b == "_" && quad.result == "_"
            }
        } else if is_vector_op(base) {
            if base == "vload" || base == "vsum" || base == "vmax" || base == "vmin" {
                identifier(&quad.a) && value_operand(&quad.b) && identifier(&quad.result)
            } else if base == "vcopy" {
                identifier(&quad.a) && value_operand(&quad.b) && identifier(&quad.result)
            } else {
                identifier(&quad.a) && value_operand(&quad.b) && value_operand(&quad.result)
            }
        } else if is_file_op(base) {
            if base == "fopen" {
                value_operand(&quad.a) && value_operand(&quad.b) && identifier(&quad.result)
            } else if base == "fread" || base == "feof" {
                value_operand(&quad.a) && quad.b == "_" && identifier(&quad.result)
            } else if base == "fwrite" {
                value_operand(&quad.a) && value_operand(&quad.b) && quad.result == "_"
            } else {
                value_operand(&quad.a) && quad.b == "_" && quad.result == "_"
            }
        } else if base == "system" {
            identifier(&quad.a)
                && (quad.b == "_" || value_operand(&quad.b))
                && (quad.result == "_" || identifier(&quad.result))
        } else {
            // 数据区三件套 `addr` / `load` / `store` 也落在这里：它们只在 C++17
            // 主库实现，Rust 版明确不移植（见 core/技术报告.md 的边界一节）。
            return Err(format!(
                "第 {index} 条四元式的操作 '{}' 不存在（未知四元式）",
                quad.op
            ));
        };
        if !ok {
            return fail("operand shape mismatch");
        }
    }
    for quad in program {
        let base = op_base(&quad.op);
        if matches!(base, "j" | "jz" | "jnz" | "call" | "int") {
            let target = if base == "call" || base == "int" {
                &quad.a
            } else {
                &quad.result
            };
            if !labels.contains(target) {
                return Err(format!("undefined label: {target}"));
            }
        }
    }
    Ok(())
}

impl fmt::Display for Target {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(match self {
            Target::Mips => "mips",
            Target::I8086 => "8086",
            Target::Arm => "arm",
        })
    }
}

pub fn default_config(target: Target) -> MachineConfig {
    let (a, b, c, hi, lo, load, store, immediate) = match target {
        Target::Mips => (
            "$t0",
            "$t1",
            "$t2",
            "HI",
            "LO",
            "LH {dst}, {name}",
            "SH {src}, {name}",
            "LI {dst}, {value}",
        ),
        Target::I8086 => (
            "AX",
            "BX",
            "CX",
            "DX",
            "AX",
            "MOV {dst}, [{name}]",
            "MOV [{name}], {src}",
            "MOV {dst}, {value}",
        ),
        Target::Arm => (
            "r0",
            "r1",
            "r2",
            "r12",
            "r14",
            "LDRSH {dst}, [{name}]",
            "STRH {src}, [{name}]",
            "MOV {dst}, #{value}",
        ),
    };
    let mut commands = HashMap::new();
    let table: [(&str, &str); 16] = [
        ("add", "ADD {dst}, {lhs}, {rhs}"),
        ("sub", "SUB {dst}, {lhs}, {rhs}"),
        ("mul", "MUL {dst}, {lhs}, {rhs}"),
        ("div", "SDIV {dst}, {lhs}, {rhs}"),
        ("shl", "LSL {dst}, {lhs}, {rhs}"),
        ("shr", "ASR {dst}, {lhs}, {rhs}"),
        ("shru", "LSR {dst}, {lhs}, {rhs}"),
        ("and", "AND {dst}, {lhs}, {rhs}"),
        ("or", "ORR {dst}, {lhs}, {rhs}"),
        ("xor", "EOR {dst}, {lhs}, {rhs}"),
        ("not", "MVN {dst}, {lhs}"),
        ("fadd", "VADD {dst}, {lhs}, {rhs}"),
        ("fsub", "VSUB {dst}, {lhs}, {rhs}"),
        ("fmul", "VMUL {dst}, {lhs}, {rhs}"),
        ("fdiv", "VDIV {dst}, {lhs}, {rhs}"),
        ("fneg", "VNEG {dst}, {lhs}"),
    ];
    for (key, value) in table {
        commands.insert(key.to_string(), value.to_string());
    }
    MachineConfig {
        target,
        word_bits: 16,
        reg_a: a.into(),
        reg_b: b.into(),
        reg_c: c.into(),
        reg_hi: hi.into(),
        reg_lo: lo.into(),
        data: HashMap::new(),
        rodata: HashMap::new(),
        data_image: Vec::new(),
        data_area_bytes: Vec::new(),
        data_area_symbols: HashMap::new(),
        stack_words: 1024,
        commands,
        features: [
            "carry", "shift", "bitwise", "mul", "div", "float", "string", "vector", "stream",
            "file", "system", "data",
        ]
        .iter()
        .map(|name| name.to_string())
        .collect(),
    }
    .with_memory(load, store, immediate)
}

impl MachineConfig {
    /// 这台机器是否实现了某个指令族。
    pub fn supports(&self, feature: &str) -> bool {
        self.features.contains(feature)
    }

    fn with_memory(mut self, load: &str, store: &str, immediate: &str) -> MachineConfig {
        self.commands.insert("memory_load".into(), load.into());
        self.commands.insert("memory_store".into(), store.into());
        self.commands
            .insert("immediate_load".into(), immediate.into());
        self
    }
}

pub fn load_data_bin(path: impl AsRef<std::path::Path>) -> Result<Vec<u8>, String> {
    std::fs::read(path).map_err(|e| e.to_string())
}

/// 读数据区**符号布局表**：`simple_ast` 的 `exportDataLayout()` 输出，一行一个符号，
/// 形如
///
/// ```text
/// arr_1 offset=0 size=32 stride=8 type=array<i32>{count=4;align=2;label=numbers}
/// ```
///
/// 只认 `符号名 offset=N` 这两个字段（`size` / `stride` / `type` 归别处用），
/// 与 `library/stackvm` 的 DataArea 取同一套字段，两个后端因此看到同一份布局。
pub fn load_data_layout(
    path: impl AsRef<std::path::Path>,
) -> Result<HashMap<String, u64>, String> {
    let path = path.as_ref();
    let text = std::fs::read_to_string(path)
        .map_err(|error| format!("无法打开数据布局 {}：{error}", path.display()))?;
    let mut symbols = HashMap::new();
    for line in text.lines() {
        let line = line.trim_end_matches('\r');
        let (Some(space), Some(offset_at)) = (line.find(' '), line.find("offset=")) else {
            continue;
        };
        let name = &line[..space];
        let rest = &line[offset_at + "offset=".len()..];
        let end = rest.find(' ').unwrap_or(rest.len());
        let value = rest[..end]
            .parse::<u64>()
            .map_err(|_| format!("数据布局里 '{name}' 的 offset 非法：{}", &rest[..end]))?;
        symbols.insert(name.to_string(), value);
    }
    Ok(symbols)
}

/// 一次装好数据区：字节镜像 + 符号表（`layout_path` 留空时按 `<bin>.layout` 找，
/// 与 `library/stackvm` 的默认一致，这样 `simple_ast` 生成的 `.data.bin` +
/// `.data.bin.layout` 两件套能直接对接）。
pub fn load_data_area(
    config: &mut MachineConfig,
    bin_path: &str,
    layout_path: &str,
) -> Result<(), String> {
    config.data_area_bytes = load_data_bin(bin_path)?;
    let layout = if layout_path.is_empty() {
        format!("{bin_path}.layout")
    } else {
        layout_path.to_string()
    };
    config.data_area_symbols = load_data_layout(layout)?;
    Ok(())
}

pub fn load_config(text: &str, fallback: Target) -> Result<MachineConfig, String> {
    load_config_with_base(text, fallback, None)
}

/// 读取机器配置文件，并允许 `data_bin=` 相对配置文件目录解析。
pub fn load_config_file(
    path: impl AsRef<std::path::Path>,
    fallback: Target,
) -> Result<MachineConfig, String> {
    let path = path.as_ref();
    let text = std::fs::read_to_string(path)
        .map_err(|error| format!("无法读取配置 {}：{error}", path.display()))?;
    load_config_with_base(&text, fallback, path.parent())
}

fn resolve_data_bin(value: &str, base: Option<&std::path::Path>) -> std::path::PathBuf {
    let direct = std::path::Path::new(value);
    if direct.is_file() {
        return direct.to_path_buf();
    }
    if let Some(base) = base {
        let file_name = direct.file_name().unwrap_or_default();
        for candidate in [base.join(direct), base.join(file_name)] {
            if candidate.is_file() {
                return candidate;
            }
        }
    }
    direct.to_path_buf()
}

fn load_config_with_base(
    text: &str,
    fallback: Target,
    base: Option<&std::path::Path>,
) -> Result<MachineConfig, String> {
    let mut config = default_config(fallback);
    for line in text.lines() {
        let line = line.trim();
        if line.is_empty() || line.starts_with('#') || line.starts_with(';') {
            continue;
        }
        let (key, value) = line.split_once('=').ok_or("invalid config line")?;
        let key = key.trim();
        let value = value.trim();
        match key {
            "target" => {
                config.target = match value {
                    "mips" => Target::Mips,
                    "8086" => Target::I8086,
                    "arm" => Target::Arm,
                    _ => return Err("unknown target".into()),
                }
            }
            "word_bits" => {
                config.word_bits = value.parse().map_err(|_| "invalid word_bits")?;
                if ![8, 16, 32, 64].contains(&config.word_bits) {
                    return Err("word_bits must be 8, 16, 32 or 64".into());
                }
            }
            "reg_a" => config.reg_a = value.into(),
            "reg_b" => config.reg_b = value.into(),
            "reg_c" => config.reg_c = value.into(),
            "reg_hi" => config.reg_hi = value.into(),
            "reg_lo" => config.reg_lo = value.into(),
            "stack_words" => {
                config.stack_words = value.parse().map_err(|_| "invalid stack_words")?
            }
            "memory_load" => {
                config.commands.insert("memory_load".into(), value.into());
            }
            "memory_store" => {
                config.commands.insert("memory_store".into(), value.into());
            }
            "immediate_load" => {
                config
                    .commands
                    .insert("immediate_load".into(), value.into());
            }
            other if other.starts_with("cmd.") => {
                config.commands.insert(other[4..].into(), value.into());
            }
            other if other.starts_with("data.") => {
                config.data.insert(other[5..].into(), value.into());
            }
            other if other.starts_with("rodata.") => {
                config.rodata.insert(other[7..].into(), value.into());
            }
            // 指令族开关：features=a,b,c 整份替换；feature.xxx=off 单独关掉。
            "features" => {
                config.features.clear();
                for item in value.split([',', ' ', '\t']) {
                    let item = item.trim();
                    if !item.is_empty() {
                        config.features.insert(item.to_string());
                    }
                }
            }
            other if other.starts_with("feature.") => {
                let name = &other[8..];
                if matches!(value.trim(), "off" | "0" | "false") {
                    config.features.remove(name);
                } else {
                    config.features.insert(name.to_string());
                }
            }
            "data_bin" => {
                // C++ 版的 loadConfigFile 会自动装载 Bin；Rust 这里同样处理。
                config.data_image = load_data_bin(resolve_data_bin(value, base))?;
            }
            _ => return Err(format!("unknown config key {key}")),
        }
    }
    Ok(config)
}

fn command(config: &MachineConfig, key: &str, fallback: &str) -> String {
    config
        .commands
        .get(key)
        .cloned()
        .unwrap_or_else(|| fallback.to_string())
}

fn with_type(text: &str, ty: &str) -> String {
    if ty.is_empty() || ty == default_type() {
        return text.to_string();
    }
    match text.find([' ', '\t']) {
        Some(index) => format!("{}.{}{}", &text[..index], ty, &text[index..]),
        None => format!("{text}.{ty}"),
    }
}

// 优化、代码生成与模拟执行分文件实现，拼进同一模块。
include!("optimize.rs");
include!("generate.rs");
include!("simulate.rs");
include!("quadfile.rs");
