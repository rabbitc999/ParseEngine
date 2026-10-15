//! 序列化：把 [`Value`] 写回 JSON 文本（对应 `library/json/json.cpp` 里
//! `formatInt` / `formatDouble` / `escapeInto` / `appendIndent` / `dumpInto` 那一块）。
//!
//! 输出规则与 C++ 版逐条对齐：
//!
//!   * **缩进**：`indent < 0` 单行紧凑（`{"a":1}`）；`indent == 0` 换行但不缩进；
//!     `indent > 0` 每层缩进这么多空格。紧凑时冒号后面不加空格，带缩进时加。
//!   * **字符串转义**：`"` / `\` / `\b` / `\f` / `\n` / `\r` / `\t` 用短转义，
//!     其余 U+0000..U+001F 写成 `\u00xx`（小写十六进制）；**非 ASCII 原样透传**
//!     （不做 `\uXXXX` 化，中文还是中文）。
//!   * **数字**：整数原样十进制；浮点若是 `NaN` / `Infinity` 写 `null`（JSON 没有
//!     这两个值）；整数值的浮点补 `.0`，保证再解析回来还是浮点。
//!   * **对象顺序**：按成员表的插入顺序写回——解析时就是文件里的先后顺序。
//!
//! 与 C++ 版**有意的差异**只有两处，都是"同值不同写法"或"语言的硬约束"：
//!
//! | | C++ 版 | Rust 版 |
//! | --- | --- | --- |
//! | 浮点写法 | `formatDouble` 从 17 位有效数字逐级缩短到能往返为止（`%.*g` + `strtod` 回读）——而 17 位对 `f64` **必定**能往返，所以那个循环第一轮就退出，永远印 17 位 | `format!` 直接给**最短且能往返**的那串（`0.1` → `0.1`，而不是 `0.10000000000000001`） |
//! | 落单代理 | `"\udc00"` 会把 `ED B0 80` 这几个**非法 UTF-8 字节**写进产物 | `String` 装不下非法 UTF-8，直接按 `bad-surrogate` 报错（只影响输入本身非法的情形） |
//!
//! 这两处差异的**归属**要说清楚：浮点那处的根在 **C++ 的实现里**（更多位数、
//! 更难读，信息量却没多——还是同一个 `f64`），Rust 只是用了语言标准库的规范写法；
//! 落单代理那处是 **Rust 字符串类型的固有约束**（`String` 只能装合法 UTF-8）。
//!
//! 除了这两处，同一份输入的两版输出**逐字节相同**（自检里用 91 条语料比对过，
//! 见 `README.md` 的实测一段）。浮点数两条路都保证"再解析回来还是同一个 `f64`"。

use std::fmt;

use crate::value::Value;

impl Value {
    /// 序列化成 JSON 文本，**单行紧凑**（对应 C++ 的 `dump()`）。
    ///
    /// ```
    /// use json::parse;
    ///
    /// let doc = parse(r#"{"a":[1,2],"b":{"c":true},"d":"x"}"#);
    /// assert_eq!(doc.value.unwrap().dump(), r#"{"a":[1,2],"b":{"c":true},"d":"x"}"#);
    /// ```
    pub fn dump(&self) -> String {
        self.dump_indent(-1)
    }

    /// 序列化成 JSON 文本，缩进自定（对应 C++ 的 `dump(int indent)`）。
    ///
    /// `indent < 0` 单行紧凑；`0` 换行但不缩进；`> 0` 每层缩进这么多空格。
    ///
    /// ```
    /// use json::parse;
    ///
    /// let doc = parse(r#"{"a":1}"#);
    /// assert_eq!(doc.value.unwrap().dump_indent(2), "{\n  \"a\": 1\n}");
    /// ```
    pub fn dump_indent(&self, indent: i32) -> String {
        let mut out = String::with_capacity(64);
        dump_into(&mut out, self, indent, 0);
        out
    }
}

/// `{{ value }}` 式的文本化：与 [`Value::dump`] 相同（对应 C++ 的 `operator<<`）。
impl fmt::Display for Value {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(&self.dump())
    }
}

/// 把字符串按 JSON 转义规则写进输出串（对应 C++ `escapeInto`）。
fn escape_into(out: &mut String, text: &str) {
    out.push('"');
    for ch in text.chars() {
        match ch {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            '\u{8}' => out.push_str("\\b"),
            '\u{c}' => out.push_str("\\f"),
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            '\t' => out.push_str("\\t"),
            // 其余控制字符（U+0000..U+001F）写成 \u00xx。
            ch if (ch as u32) < 0x20 => {
                out.push_str(&format!("\\u{:04x}", ch as u32));
            }
            // 非 ASCII 原样透传（UTF-8 直出），与 C++ 版一致。
            ch => out.push(ch),
        }
    }
    out.push('"');
}

/// 把浮点写成文本（对应 C++ `formatDouble`）。
///
/// `NaN` / `±Infinity` → `null`；其余用标准库的**最短往返**写法，必要时补 `.0`
/// （保证 `2.0` 再解析回来还是浮点而不是整数）。
///
/// 量级很大 / 很小时改用指数写法（`1e-300`、`1.2345678901234568e29`）——这一点与
/// C++ 版 `%g` 的行为一致（`%g` 在指数 `< -4` 或 `>= 精度` 时也会切指数），
/// 只是边界取在 `1e-4` / `1e17`。
///
/// 与 C++ 版的差异只在**位数**，而且根在 C++ 侧：它的 `formatDouble` 从 17 位有效
/// 数字起逐级缩短，对 `f64` 来说 17 位必定能往返，于是循环第一轮就退出、永远印
/// 17 位（`0.1` → `0.10000000000000001`）。这里用标准库给的**最短**那串就够了，
/// 两边都是同一个 `f64`。
fn format_double(value: f64) -> String {
    if !value.is_finite() {
        return "null".to_string();
    }
    let magnitude = value.abs();
    if magnitude != 0.0 && !(1e-4..1e17).contains(&magnitude) {
        // `{:e}` 给的就是最短往返的指数写法；指数补上符号位，与 C++ `%g` 的
        // `1e+300` 写法对齐（JSON 两种都认）。
        let text = format!("{value:e}");
        return match text.find('e') {
            Some(position) => {
                let (mantissa, exponent) = text.split_at(position + 1);
                match exponent.as_bytes().first() {
                    Some(b'+') | Some(b'-') => text,
                    _ => format!("{mantissa}+{exponent}"),
                }
            }
            None => text,
        };
    }
    let mut text = format!("{value}");
    if !text.contains('.') && !text.contains('e') && !text.contains('E') {
        text.push_str(".0");
    }
    text
}

/// 追加「换行 + 缩进」（对应 C++ `appendIndent`）。
fn append_indent(out: &mut String, indent: i32, depth: usize) {
    if indent < 0 {
        return;
    }
    out.push('\n');
    for _ in 0..(indent as usize).saturating_mul(depth) {
        out.push(' ');
    }
}

/// 递归写出一个值（对应 C++ `dumpInto`）。
fn dump_into(out: &mut String, value: &Value, indent: i32, depth: usize) {
    // 1. 标量直接写。
    match value {
        Value::Null => {
            out.push_str("null");
            return;
        }
        Value::Bool(flag) => {
            out.push_str(if *flag { "true" } else { "false" });
            return;
        }
        Value::Int(number) => {
            out.push_str(&number.to_string());
            return;
        }
        Value::Double(number) => {
            out.push_str(&format_double(*number));
            return;
        }
        Value::String(text) => {
            escape_into(out, text);
            return;
        }
        _ => {}
    }

    // 2. 数组。
    if let Value::Array(items) = value {
        if items.is_empty() {
            out.push_str("[]");
            return;
        }
        out.push('[');
        for (index, item) in items.iter().enumerate() {
            if index != 0 {
                out.push(',');
            }
            if indent >= 0 {
                append_indent(out, indent, depth + 1);
            }
            dump_into(out, item, indent, depth + 1);
        }
        if indent >= 0 {
            append_indent(out, indent, depth);
        }
        out.push(']');
        return;
    }

    // 3. 对象（剩下的只可能是对象）。
    if let Value::Object(object) = value {
        if object.is_empty() {
            out.push_str("{}");
            return;
        }
        out.push('{');
        for (index, member) in object.iter().enumerate() {
            if index != 0 {
                out.push(',');
            }
            if indent >= 0 {
                append_indent(out, indent, depth + 1);
            }
            escape_into(out, &member.key);
            out.push_str(if indent >= 0 { ": " } else { ":" });
            dump_into(out, &member.value, indent, depth + 1);
        }
        if indent >= 0 {
            append_indent(out, indent, depth);
        }
        out.push('}');
    }
}
