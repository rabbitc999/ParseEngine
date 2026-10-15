//! 解析：JSON 文本 → [`Value`]（对应 `library/json/json.cpp` 里的 `json::Parser`）。
//!
//! 手写的递归下降解析器，**遇到第一个错误就停下**并把行列信息记进诊断——这样错误
//! 信息最贴近人的直觉（"第 2 行第 8 列，这里应该是个值"）。
//!
//! 与 C++ 版的关系：
//!
//! | C++ 版 | 本模块 |
//! | --- | --- |
//! | `json::Parser`（`std::string_view` + 字节下标） | `Parser`（`&[u8]` + 字节下标） |
//! | `json::ParseOptions{allowComments, allowTrailingCommas}` | [`ParseOptions`]（同样两个开关，另有 [`ParseOptions::loose`]） |
//! | `json::ParseResult{value, diagnostics}` | [`ParseResult`] |
//! | `json::Diagnostic` 的 `toString()` | [`Diagnostic`] 的 `Display` |
//! | `std::from_chars` / `strtod` | `str::parse::<i64>` / `str::parse::<f64>` |
//!
//! **短码、消息、行列与字节偏移都与 C++ 版逐字一致**（自检里逐条比对），所以
//! 两版对同一份坏输入给出同一行诊断。三处刻意的差异：
//!
//!   1. 行列按**字节**算（与 C++ 版一致），列号不会因为前面有中文而"多算"；
//!   2. "不认识的字符 'x'"里，C++ 版直接印出那个**字节**（中文会印成乱码），
//!      Rust 版解码出真正的字符；
//!   3. 落单的代理码点（如 `"\udc00"`）C++ 版会写出非法 UTF-8 字节，Rust 版按
//!      `bad-surrogate` 报错——`String` 装不下非法 UTF-8，这是语言的硬约束。
//!
//! ```
//! use json::{parse, parse_with, ParseOptions};
//!
//! // 坏输入：给短码 + 行列。
//! let broken = parse("{\n  \"a\": ,\n}");
//! assert!(!broken.ok());
//! let first = &broken.diagnostics[0];
//! assert_eq!(first.code, "unexpected-token");
//! assert_eq!((first.line, first.column), (2, 8));
//!
//! // 宽松档：允许注释与尾逗号。
//! let loose = parse_with("// 注释\n{\"a\": [1, 2,], /* 块注释 */}", ParseOptions::loose());
//! assert!(loose.ok());
//! assert_eq!(loose.value.unwrap().at_path("a[1]").unwrap().int_or(0), 2);
//! ```

use std::fmt;

use crate::value::{Object, Value};

/// 解析选项（对应 C++ 的 `ParseOptions`）。
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct ParseOptions {
    /// 允许 `//` 行注释与 `/* … */` 块注释（配置文件的常见扩展）。
    pub allow_comments: bool,
    /// 允许数组 / 对象末尾多一个逗号。
    pub allow_trailing_commas: bool,
}

impl ParseOptions {
    /// 宽松档：注释与尾逗号都允许（配置文件里最常见的那两种"非标准写法"）。
    ///
    /// ```
    /// use json::{parse_with, ParseOptions};
    ///
    /// let doc = parse_with("{ /* 注释 */ \"a\": [1, 2,], }", ParseOptions::loose());
    /// assert!(doc.ok());
    /// ```
    pub fn loose() -> Self {
        ParseOptions { allow_comments: true, allow_trailing_commas: true }
    }
}

/// 一条诊断（对应 C++ 的 `Diagnostic`）。
///
/// 解析错误给**行列与字节偏移**；投射（[`crate::decode`]）错误给 **JSON 路径**。
/// 两者都有时，[`Display`](fmt::Display) 优先印路径（与 C++ 版相同）。
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Diagnostic {
    /// 行号（1 基）。
    pub line: usize,
    /// 列号（1 基，按 UTF-8 字节算）。
    pub column: usize,
    /// 字节偏移。
    pub offset: usize,
    /// 投射 / 解码诊断里的 JSON 路径，例如 `user.tags[2].name`。
    pub path: String,
    /// 短码，例如 `unexpected-token`。
    pub code: String,
    /// 说明。
    pub message: String,
}

impl Diagnostic {
    /// 一条**解析**诊断（带行列与偏移）。
    pub fn at(line: usize, column: usize, offset: usize, code: impl Into<String>, message: impl Into<String>) -> Self {
        Diagnostic {
            line,
            column,
            offset,
            path: String::new(),
            code: code.into(),
            message: message.into(),
        }
    }

    /// 一条**投射**诊断（带 JSON 路径）。
    ///
    /// ```
    /// use json::Diagnostic;
    ///
    /// let item = Diagnostic::in_path("points[1].x", "type-mismatch", "期望整数，实际是 string");
    /// assert_eq!(item.to_string(), "points[1].x: 期望整数，实际是 string [type-mismatch]");
    /// ```
    pub fn in_path(path: impl Into<String>, code: impl Into<String>, message: impl Into<String>) -> Self {
        Diagnostic {
            line: 1,
            column: 1,
            offset: 0,
            path: path.into(),
            code: code.into(),
            message: message.into(),
        }
    }
}

impl Default for Diagnostic {
    /// 与 C++ 版 `Diagnostic` 的成员初值一致：行列从 1 起。
    fn default() -> Self {
        Diagnostic {
            line: 1,
            column: 1,
            offset: 0,
            path: String::new(),
            code: String::new(),
            message: String::new(),
        }
    }
}

/// 拼成一行：有路径时是 `路径: 消息 [短码]`，否则是 `行:列: 消息 [短码]`（对应 C++ `toString`）。
impl fmt::Display for Diagnostic {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        if self.path.is_empty() {
            write!(formatter, "{}:{}: ", self.line, self.column)?;
        } else {
            write!(formatter, "{}: ", self.path)?;
        }
        formatter.write_str(&self.message)?;
        if !self.code.is_empty() {
            write!(formatter, " [{}]", self.code)?;
        }
        Ok(())
    }
}

/// 解析结果（对应 C++ 的 `ParseResult`）。
///
/// ```
/// use json::parse;
///
/// let doc = parse("{}");
/// assert!(doc.ok());
/// assert!(doc.value.is_some());
///
/// let bad = parse("{");
/// assert!(!bad.ok());
/// assert_eq!(bad.diagnostics[0].code, "expected-key");
/// ```
#[derive(Clone, Debug, Default)]
pub struct ParseResult {
    /// 解析成功时的值。
    pub value: Option<Value>,
    /// 诊断（失败时至少一条）。
    pub diagnostics: Vec<Diagnostic>,
}

impl ParseResult {
    /// 是否解析成功。
    pub fn ok(&self) -> bool {
        self.value.is_some()
    }

    /// 取出解析好的值；失败时把诊断一起还回去（不想 `unwrap` 时用这个）。
    pub fn into_value(self) -> Result<Value, Vec<Diagnostic>> {
        match self.value {
            Some(value) => Ok(value),
            None => Err(self.diagnostics),
        }
    }

    /// 第一条诊断（失败时最该看的就是它）。
    pub fn first_error(&self) -> Option<&Diagnostic> {
        self.diagnostics.first()
    }
}

/// 解析 JSON 文本，用缺省选项（对应 C++ `parse(text)`）。
///
/// 输入按 UTF-8 处理；允许带 BOM。解析失败时 [`ParseResult::diagnostics`] 里带行列信息，
/// [`ParseResult::value`] 为空。
pub fn parse(text: &str) -> ParseResult {
    parse_with(text, ParseOptions::default())
}

/// 解析 JSON 文本（对应 C++ `parse(text, options)`）。
pub fn parse_with(text: &str, options: ParseOptions) -> ParseResult {
    Parser::new(text, options).run()
}

/// JSON 解析器（递归下降）。
struct Parser<'a> {
    /// 输入文本（按字节看：行列、偏移、切片边界都按字节算，与 C++ 版一致）。
    text: &'a [u8],
    /// 选项。
    options: ParseOptions,
    /// 当前字节偏移。
    index: usize,
    /// 当前行号。
    line: usize,
    /// 当前列号。
    column: usize,
    /// 第一条诊断。
    diagnostic: Diagnostic,
}

impl<'a> Parser<'a> {
    /// 构造（跳过 UTF-8 BOM）。
    fn new(text: &'a str, options: ParseOptions) -> Self {
        let bytes = text.as_bytes();
        let index = if bytes.len() >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF {
            3
        } else {
            0
        };
        Parser { text: bytes, options, index, line: 1, column: 1, diagnostic: Diagnostic::default() }
    }

    /// 跑一遍解析。
    fn run(&mut self) -> ParseResult {
        let mut result = ParseResult::default();
        self.skip_space();
        let mut value = Value::Null;
        if !self.parse_value(&mut value) {
            result.diagnostics.push(self.diagnostic.clone());
            return result;
        }
        self.skip_space();
        if !self.at_end() {
            self.fail("unexpected-token", "JSON 值后面还有多余内容");
            result.diagnostics.push(self.diagnostic.clone());
            return result;
        }
        result.value = Some(value);
        result
    }

    /// 是否到末尾。
    fn at_end(&self) -> bool {
        self.index >= self.text.len()
    }

    /// 当前字节（不前进）；到末尾返回 `0`（对应 C++ 的 `'\0'`）。
    fn peek(&self) -> u8 {
        if self.at_end() {
            0
        } else {
            self.text[self.index]
        }
    }

    /// 当前**字符**（只给诊断文案用；不能解码时退回 `'\0'`）。
    fn peek_char(&self) -> char {
        if self.at_end() {
            return '\0';
        }
        std::str::from_utf8(&self.text[self.index..])
            .ok()
            .and_then(|rest| rest.chars().next())
            .unwrap_or('\0')
    }

    /// 取一个字节并推进位置（到末尾时返回 `0`，不前进也不 panic）。
    fn get(&mut self) -> u8 {
        if self.at_end() {
            return 0;
        }
        let ch = self.text[self.index];
        self.index += 1;
        if ch == b'\n' {
            self.line += 1;
            self.column = 1;
        } else {
            self.column += 1;
        }
        ch
    }

    /// 记一条诊断（只留第一条，与 C++ 版一致）。
    fn fail(&mut self, code: &str, message: impl Into<String>) {
        if !self.diagnostic.message.is_empty() {
            return;
        }
        self.diagnostic.line = self.line;
        self.diagnostic.column = self.column;
        self.diagnostic.offset = self.index;
        self.diagnostic.code = code.to_string();
        self.diagnostic.message = message.into();
    }

    /// 自己是不是正对着给定字面量（`true` / `false` / `null`）。
    fn matches(&self, literal: &[u8]) -> bool {
        self.text.len() >= self.index + literal.len()
            && &self.text[self.index..self.index + literal.len()] == literal
    }

    /// 跳过空白与注释。
    fn skip_space(&mut self) {
        while !self.at_end() {
            let ch = self.peek();
            if ch == b' ' || ch == b'\t' || ch == b'\r' || ch == b'\n' {
                self.get();
                continue;
            }
            if self.options.allow_comments && ch == b'/' && self.index + 1 < self.text.len() {
                let next = self.text[self.index + 1];
                // 1. 行注释。
                if next == b'/' {
                    while !self.at_end() && self.peek() != b'\n' {
                        self.get();
                    }
                    continue;
                }
                // 2. 块注释。
                if next == b'*' {
                    self.get();
                    self.get();
                    while !self.at_end()
                        && !(self.peek() == b'*'
                            && self.index + 1 < self.text.len()
                            && self.text[self.index + 1] == b'/')
                    {
                        self.get();
                    }
                    if self.at_end() {
                        self.fail("unterminated-comment", "块注释没有闭合");
                        return;
                    }
                    self.get();
                    self.get();
                    continue;
                }
            }
            break;
        }
    }

    /// 解析一个值。
    fn parse_value(&mut self, out: &mut Value) -> bool {
        self.skip_space();
        if self.at_end() {
            self.fail("unexpected-end", "内容在值之前就结束了");
            return false;
        }

        match self.peek() {
            b'{' => self.parse_object(out),
            b'[' => self.parse_array(out),
            b'"' => self.parse_string(out),
            b't' | b'f' | b'n' => self.parse_literal(out),
            _ => self.parse_number(out),
        }
    }

    /// 解析对象。
    fn parse_object(&mut self, out: &mut Value) -> bool {
        self.get(); // '{'
        let mut object = Object::new();
        self.skip_space();
        if self.peek() == b'}' {
            self.get();
            *out = Value::Object(object);
            return true;
        }

        loop {
            self.skip_space();
            if self.peek() != b'"' {
                self.fail("expected-key", "对象的成员名必须是字符串");
                return false;
            }
            let mut key = Value::Null;
            if !self.parse_string(&mut key) {
                return false;
            }
            let key_text = key.as_str().unwrap_or_default().to_string();

            self.skip_space();
            if self.peek() != b':' {
                self.fail("expected-colon", "成员名后面要跟冒号");
                return false;
            }
            self.get();

            let mut member = Value::Null;
            if !self.parse_value(&mut member) {
                return false;
            }
            object.insert(key_text, member);

            self.skip_space();
            if self.peek() == b',' {
                self.get();
                self.skip_space();
                if self.peek() == b'}' {
                    if !self.options.allow_trailing_commas {
                        self.fail("trailing-comma", "对象末尾多了一个逗号（可用 --trailing-commas 允许）");
                        return false;
                    }
                    self.get();
                    break;
                }
                continue;
            }
            if self.peek() == b'}' {
                self.get();
                break;
            }
            self.fail("expected-comma", "对象成员之间要有逗号，结尾要有 '}'");
            return false;
        }

        *out = Value::Object(object);
        true
    }

    /// 解析数组。
    fn parse_array(&mut self, out: &mut Value) -> bool {
        self.get(); // '['
        let mut items = Vec::new();
        self.skip_space();
        if self.peek() == b']' {
            self.get();
            *out = Value::Array(items);
            return true;
        }

        loop {
            let mut item = Value::Null;
            if !self.parse_value(&mut item) {
                return false;
            }
            items.push(item);

            self.skip_space();
            if self.peek() == b',' {
                self.get();
                self.skip_space();
                if self.peek() == b']' {
                    if !self.options.allow_trailing_commas {
                        self.fail("trailing-comma", "数组末尾多了一个逗号（可用 --trailing-commas 允许）");
                        return false;
                    }
                    self.get();
                    break;
                }
                continue;
            }
            if self.peek() == b']' {
                self.get();
                break;
            }
            self.fail("expected-comma", "数组元素之间要有逗号，结尾要有 ']'");
            return false;
        }

        *out = Value::Array(items);
        true
    }

    /// 解析字符串（含 `\uXXXX` 与代理对）。
    fn parse_string(&mut self, out: &mut Value) -> bool {
        self.get(); // '"'
        let mut buffer: Vec<u8> = Vec::new();
        while !self.at_end() {
            let ch = self.get();
            if ch == b'"' {
                return match String::from_utf8(buffer) {
                    Ok(text) => {
                        *out = Value::String(text);
                        true
                    }
                    // 输入本身是合法 UTF-8、切点又都在 ASCII 定界符上，所以这里
                    // 理论上到不了；留着是为了不 panic。
                    Err(_) => {
                        self.fail("bad-utf8", "字符串里出现了非法的 UTF-8 序列");
                        false
                    }
                };
            }
            if ch < 0x20 {
                self.fail("control-char", "字符串里出现了未转义的控制字符");
                return false;
            }
            if ch != b'\\' {
                buffer.push(ch);
                continue;
            }

            // 转义序列。
            if self.at_end() {
                self.fail("bad-escape", "转义序列不完整");
                return false;
            }
            let escape = self.get();
            match escape {
                b'"' => buffer.push(b'"'),
                b'\\' => buffer.push(b'\\'),
                b'/' => buffer.push(b'/'),
                b'b' => buffer.push(0x08),
                b'f' => buffer.push(0x0C),
                b'n' => buffer.push(b'\n'),
                b'r' => buffer.push(b'\r'),
                b't' => buffer.push(b'\t'),
                b'u' => {
                    let mut code = 0u32;
                    if !self.parse_hex4(&mut code) {
                        return false;
                    }
                    // 代理对：高位后面必须跟 \uDC00..\uDFFF。
                    if (0xD800..=0xDBFF).contains(&code) {
                        if self.at_end() || self.peek() != b'\\' {
                            self.fail("bad-surrogate", "高位代理后面缺少 \\uXXXX 低位代理");
                            return false;
                        }
                        self.get();
                        if self.at_end() || self.get() != b'u' {
                            self.fail("bad-surrogate", "高位代理后面缺少 \\uXXXX 低位代理");
                            return false;
                        }
                        let mut low = 0u32;
                        if !self.parse_hex4(&mut low) {
                            return false;
                        }
                        if !(0xDC00..=0xDFFF).contains(&low) {
                            self.fail("bad-surrogate", "低位代理不在 DC00..DFFF 范围内");
                            return false;
                        }
                        code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                    }
                    match char::from_u32(code) {
                        Some(character) => {
                            let mut encoded = [0u8; 4];
                            buffer.extend_from_slice(character.encode_utf8(&mut encoded).as_bytes());
                        }
                        None => {
                            self.fail("bad-surrogate", "落单的代理码点无法表示成 UTF-8");
                            return false;
                        }
                    }
                }
                other => {
                    self.fail("bad-escape", format!("不认识的转义：\\{}", describe_byte(other)));
                    return false;
                }
            }
        }

        self.fail("unterminated-string", "字符串没有闭合");
        false
    }

    /// 读 4 位十六进制。
    fn parse_hex4(&mut self, out: &mut u32) -> bool {
        let mut value = 0u32;
        for _ in 0..4 {
            if self.at_end() {
                self.fail("bad-escape", "\\u 后面要跟 4 位十六进制");
                return false;
            }
            let ch = self.get();
            value <<= 4;
            match ch {
                b'0'..=b'9' => value |= u32::from(ch - b'0'),
                b'a'..=b'f' => value |= u32::from(ch - b'a' + 10),
                b'A'..=b'F' => value |= u32::from(ch - b'A' + 10),
                _ => {
                    self.fail("bad-escape", "\\u 后面要跟 4 位十六进制");
                    return false;
                }
            }
        }
        *out = value;
        true
    }

    /// 解析数字（整数优先，放不下才当浮点）。
    fn parse_number(&mut self, out: &mut Value) -> bool {
        let start = self.index;
        let mut is_double = false;

        // 1. 符号。
        if self.peek() == b'-' {
            self.get();
        }

        // 2. 整数部分（JSON 不允许前导 0）。
        if self.peek() == b'0' {
            self.get();
            if self.peek().is_ascii_digit() {
                self.fail("bad-number", "数字不允许前导 0");
                return false;
            }
        } else if (b'1'..=b'9').contains(&self.peek()) {
            while self.peek().is_ascii_digit() {
                self.get();
            }
        } else {
            self.fail("unexpected-token", format!("不认识的字符 '{}'", self.peek_char()));
            return false;
        }

        // 3. 小数部分。
        if self.peek() == b'.' {
            is_double = true;
            self.get();
            if !self.peek().is_ascii_digit() {
                self.fail("bad-number", "小数点后面要有数字");
                return false;
            }
            while self.peek().is_ascii_digit() {
                self.get();
            }
        }

        // 4. 指数部分。
        if self.peek() == b'e' || self.peek() == b'E' {
            is_double = true;
            self.get();
            if self.peek() == b'+' || self.peek() == b'-' {
                self.get();
            }
            if !self.peek().is_ascii_digit() {
                self.fail("bad-number", "指数后面要有数字");
                return false;
            }
            while self.peek().is_ascii_digit() {
                self.get();
            }
        }

        // 数字只用 ASCII 组成，所以这段解码必然成功；失败分支只为不 panic。
        let Ok(token) = std::str::from_utf8(&self.text[start..self.index]) else {
            self.fail("bad-number", "数字里出现了非法的 UTF-8 序列");
            return false;
        };

        // 5. 整数优先：放不下就当浮点。
        if !is_double {
            if let Ok(integer) = token.parse::<i64>() {
                *out = Value::Int(integer);
                return true;
            }
        }

        match token.parse::<f64>() {
            Ok(number) => {
                *out = Value::Double(number);
                true
            }
            Err(_) => {
                self.fail("bad-number", format!("数字格式不对：{token}"));
                false
            }
        }
    }

    /// 解析 `true` / `false` / `null`。
    fn parse_literal(&mut self, out: &mut Value) -> bool {
        if self.matches(b"true") {
            for _ in 0..4 {
                self.get();
            }
            *out = Value::Bool(true);
            return true;
        }
        if self.matches(b"false") {
            for _ in 0..5 {
                self.get();
            }
            *out = Value::Bool(false);
            return true;
        }
        if self.matches(b"null") {
            for _ in 0..4 {
                self.get();
            }
            *out = Value::Null;
            return true;
        }
        self.fail("unexpected-token", "这里只认 true / false / null");
        false
    }
}

/// 把一个字节写成诊断文案里的样子（ASCII 直接给字符，其余给 `\xNN`）。
fn describe_byte(ch: u8) -> String {
    if ch.is_ascii_graphic() || ch == b' ' {
        (ch as char).to_string()
    } else {
        format!("\\x{ch:02x}")
    }
}
