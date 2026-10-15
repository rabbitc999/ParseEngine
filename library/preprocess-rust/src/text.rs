//! 字符串与路径小工具（对应 `preprocess.cpp` 里那个匿名命名空间的第一节）。
//!
//! **一切都是按字节处理的**：C++ 版的 `std::string` 是字节序列，行内下标、
//! 列号、表达式里的列号全是**字节**下标。Rust 的 `String` 是 UTF-8，所以这里
//! 统一用 `as_bytes()` 取字节、用 `&text[begin..end]` 取子串——所有切点都落在
//! ASCII 分隔符（`/`、`\n`、`%`、`#`、`=`、空白、引号）上，必然是字符边界，
//! 对中文注释与中文字面量都安全。

use std::fs;
use std::path::Path;

/// 字符是不是空白（与 C++ 版一致：只认这几个 ASCII 空白，不认 Unicode 空白）。
pub(crate) fn is_space(ch: u8) -> bool {
    matches!(ch, b' ' | b'\t' | b'\r' | b'\n' | b'\x0c' | b'\x0b')
}

/// 取 `index` 处的字节；越界给 `0`（调用方本来就保证下标合法，这里只是不 panic）。
pub(crate) fn byte_at(text: &str, index: usize) -> u8 {
    text.as_bytes().get(index).copied().unwrap_or(0)
}

/// 去掉首尾空白。
pub(crate) fn trim(text: &str) -> &str {
    let bytes = text.as_bytes();
    let mut begin = 0usize;
    let mut end = bytes.len();
    while begin < end && is_space(bytes[begin]) {
        begin += 1;
    }
    while end > begin && is_space(bytes[end - 1]) {
        end -= 1;
    }
    &text[begin..end]
}

/// 去掉**指令参数末尾的注释**（行注释 `//`，或块注释那一对定界符）。
///
/// 指令行常常写成 `%include "a.inc"` 后面再跟一句说明 —— 注释不属于参数，不剥掉的话
/// `%include` 会看见一个“不是引号开头”的路径、`%symbol` 会把注释当成值。
/// 引号里的注释定界符当普通字符（路径 / 消息里可能出现），不剥；
/// 只认“参数里第一处注释起点”，从这里一刀切到行尾（不支持注释夹在表达式中间）。
///
/// # 参数
///
/// * `argument` — 参数原文（已 `trim`）。
///
/// # 返回
///
/// 去掉注释后（再 `trim`）的切片；没注释时原样返回。
pub(crate) fn strip_trailing_comment(argument: &str) -> &str {
    let bytes = argument.as_bytes();
    let mut quote: u8 = 0;
    let mut index = 0usize;
    while index < bytes.len() {
        let ch = bytes[index];
        if quote != 0 {
            if ch == quote {
                quote = 0;
            }
            index += 1;
            continue;
        }
        if ch == b'"' || ch == b'\'' {
            quote = ch;
            index += 1;
            continue;
        }
        if ch == b'/'
            && index + 1 < bytes.len()
            && (bytes[index + 1] == b'/' || bytes[index + 1] == b'*')
        {
            return trim(&argument[..index]);
        }
        index += 1;
    }
    argument
}

/// 转成小写（只处理 ASCII 的 `A-Z`，与 C++ 版一致）。
pub(crate) fn to_lower(text: &str) -> String {
    text.to_ascii_lowercase()
}

/// `index` 处是否匹配 `token`（空串视为不匹配）。
pub(crate) fn matches_at(text: &str, index: usize, token: &str) -> bool {
    if token.is_empty() {
        return false;
    }
    match text.get(index..) {
        Some(rest) => rest.starts_with(token),
        None => false,
    }
}

/// 名字首字符是否合法（`[A-Za-z_]`）。
pub(crate) fn is_name_start(ch: u8) -> bool {
    ch == b'_' || ch.is_ascii_lowercase() || ch.is_ascii_uppercase()
}

/// 名字后续字符是否合法（`[A-Za-z0-9_]`）。
pub(crate) fn is_name_part(ch: u8) -> bool {
    is_name_start(ch) || ch.is_ascii_digit()
}

/// 路径规范化：`\` → `/`，消掉 `.` 与 `..`，保留盘符与绝对路径前缀。
pub(crate) fn normalize_path(path: &str) -> String {
    let raw: String = path
        .chars()
        .map(|ch| if ch == '\\' { '/' } else { ch })
        .collect();
    let bytes = raw.as_bytes();

    // 1. 拆出前缀：`C:/` 或 `/`。
    let mut prefix = String::new();
    let mut index = 0usize;
    if raw.len() >= 2 && bytes[1] == b':' && bytes[0].is_ascii_alphabetic() {
        prefix.push_str(&raw[0..2]);
        index = 2;
        if index < raw.len() && bytes[index] == b'/' {
            index += 1;
        }
        prefix.push('/');
    } else if !raw.is_empty() && bytes[0] == b'/' {
        prefix.push('/');
        index = 1;
    }

    // 2. 逐段处理 `.` 与 `..`。
    let mut parts: Vec<&str> = Vec::new();
    while index <= raw.len() {
        let slash = match raw[index..].find('/') {
            Some(relative) => index + relative,
            None => raw.len(),
        };
        let part = &raw[index..slash];
        if part.is_empty() || part == "." {
            // 跳过
        } else if part == ".." {
            if !parts.is_empty() && parts.last() != Some(&"..") {
                parts.pop();
            } else if prefix.is_empty() {
                parts.push("..");
            }
        } else {
            parts.push(part);
        }
        if slash == raw.len() {
            break;
        }
        index = slash + 1;
    }

    // 3. 拼回去。
    let mut out = prefix;
    for (position, part) in parts.iter().enumerate() {
        if position != 0 {
            out.push('/');
        }
        out.push_str(part);
    }
    if out.is_empty() {
        ".".to_string()
    } else {
        out
    }
}

/// 取目录部分（不含结尾斜杠）；没有目录部分时返回空串。
pub(crate) fn dir_name(path: &str) -> String {
    let normalized = normalize_path(path);
    match normalized.rfind('/') {
        None => String::new(),
        Some(0) => "/".to_string(),
        Some(slash) => normalized[..slash].to_string(),
    }
}

/// 拼一个相对路径到目录上（`relative` 是绝对路径时原样返回）。
pub(crate) fn join_path(dir: &str, relative: &str) -> String {
    if relative.is_empty() {
        return normalize_path(dir);
    }
    let bytes = relative.as_bytes();
    let absolute =
        bytes[0] == b'/' || bytes[0] == b'\\' || (relative.len() >= 2 && bytes[1] == b':');
    if absolute || dir.is_empty() {
        return normalize_path(relative);
    }
    normalize_path(&format!("{dir}/{relative}"))
}

/// 读整个文件（按字节读，再按 UTF-8 解释；非法字节按 U+FFFD 替换，不失败）。
pub(crate) fn read_file(path: &str) -> Option<String> {
    let bytes = fs::read(path).ok()?;
    Some(String::from_utf8_lossy(&bytes).into_owned())
}

/// 文件是否存在（“能不能打开”，与 C++ 版的 `std::ifstream` 判定一致）。
pub(crate) fn file_exists(path: &str) -> bool {
    fs::File::open(Path::new(path)).is_ok()
}

/// 去掉 UTF-8 BOM。
pub(crate) fn strip_bom(text: &str) -> &str {
    if text.as_bytes().starts_with(&[0xEF, 0xBB, 0xBF]) {
        &text[3..]
    } else {
        text
    }
}

/// 把文本切成「行」（每行末尾的 `\r` 已去掉；不保留结尾换行信息）。
pub(crate) fn split_lines(text: &str) -> Vec<&str> {
    let mut lines = Vec::new();
    if text.is_empty() {
        return lines;
    }

    let bytes = text.as_bytes();
    let mut index = 0usize;
    while index < text.len() {
        let (end, last) = match bytes[index..].iter().position(|&byte| byte == b'\n') {
            Some(relative) => (index + relative, false),
            None => (text.len(), true),
        };
        let mut line = &text[index..end];
        if line.ends_with('\r') {
            line = &line[..line.len() - 1];
        }
        lines.push(line);
        if last {
            break;
        }
        index = end + 1;
    }
    lines
}

/// 诊断级别 → 文本（`error` / `warning` / `note`）。
pub(crate) fn severity_text(severity: crate::Severity) -> &'static str {
    match severity {
        crate::Severity::Error => "error",
        crate::Severity::Warning => "warning",
        crate::Severity::Note => "note",
    }
}
