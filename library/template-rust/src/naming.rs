//! 命名转换：驼峰 ↔ 下划线（模板模块自带，生成器拿它派生文件名 / 标识符）。
//!
//! 代码生成经常要做这种转换：模型里的 `MyParser` 要变成文件名 `my_parser`、
//! 环境变量 `MY_PARSER`，或者回头变回 `MyParser`。放在模板模块里而不是各个后端
//! 各自写一份，规则才是统一的。
//!
//! 规则只看 ASCII 字母 / 数字，非 ASCII 字符（中文等）原样透传——与 C++ 版一致。
//!
//! ```
//! use template_engine::{to_camel_case, to_snake_case};
//!
//! assert_eq!(to_snake_case("MyParser"), "my_parser");
//! assert_eq!(to_snake_case("HTTPParser"), "http_parser");
//! assert_eq!(to_snake_case("Parser2Value"), "parser2_value");
//! assert_eq!(to_camel_case("my_parser"), "MyParser");
//! ```

/// 是不是小写字母或数字。
fn is_lower_or_digit(ch: char) -> bool {
    ch.is_ascii_lowercase() || ch.is_ascii_digit()
}

/// 是不是大写字母。
fn is_upper(ch: char) -> bool {
    ch.is_ascii_uppercase()
}

/// 是不是小写字母。
fn is_lower(ch: char) -> bool {
    ch.is_ascii_lowercase()
}

/// 是不是分隔符（一律当词边界，连续分隔符合并成一个下划线）。
fn is_separator(ch: char) -> bool {
    matches!(ch, '_' | '-' | ' ' | '.')
}

/// 驼峰 / 帕斯卡转下划线：`MyParser` → `my_parser`。
///
/// 规则（生成文件名、宏名前缀时都按这一套）：
///
///   * 遇到大写字母，前面是小写字母或数字，或者它后面跟着小写字母且自己前面
///     也是大写字母（缩写的最后一个字母），就插一个下划线；
///   * `_`、`-`、空格、`.` 一律当分隔符，连续分隔符合并成一个下划线；
///   * 结果全小写，首尾不留分隔符。
///
/// ```
/// use template_engine::to_snake_case;
///
/// assert_eq!(to_snake_case("MyParser"), "my_parser");
/// assert_eq!(to_snake_case("my-parser.Parser"), "my_parser_parser");
/// ```
pub fn to_snake_case(name: &str) -> String {
    let chars: Vec<char> = name.chars().collect();
    let mut out = String::with_capacity(name.len() + 4);
    for (index, &ch) in chars.iter().enumerate() {
        if is_upper(ch) {
            let after_word = index > 0 && is_lower_or_digit(chars[index - 1]);
            let acronym_end = index > 0
                && is_upper(chars[index - 1])
                && (index + 1) < chars.len()
                && is_lower(chars[index + 1]);
            if (after_word || acronym_end) && !out.is_empty() && !out.ends_with('_') {
                out.push('_');
            }
            out.push(ch.to_ascii_lowercase());
        } else if is_separator(ch) {
            if !out.is_empty() && !out.ends_with('_') {
                out.push('_');
            }
        } else {
            out.push(ch);
        }
    }
    while out.ends_with('_') {
        out.pop();
    }
    out
}

/// 下划线转帕斯卡（大驼峰）：`my_parser` → `MyParser`。
///
/// 只有紧跟在分隔符之后的**小写字母**会被抬成大写，其它字符原样保留。
///
/// ```
/// use template_engine::to_camel_case;
///
/// assert_eq!(to_camel_case("my_parser"), "MyParser");
/// ```
pub fn to_camel_case(name: &str) -> String {
    let mut out = String::with_capacity(name.len());
    let mut upper_next = true;
    for ch in name.chars() {
        if is_separator(ch) {
            upper_next = true;
            continue;
        }
        if upper_next && is_lower(ch) {
            out.push(ch.to_ascii_uppercase());
        } else {
            out.push(ch);
        }
        upper_next = false;
    }
    out
}
