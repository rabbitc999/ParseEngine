//! 条件表达式的递归下降求值器（对应 `preprocess.cpp` 里的 `ExprParser`）。
//!
//! 优先级：`||` < `&&` < `!` < 比较 < 原子。
//!
//! 求值失败时用 `ExprError` 带回「说明 + 列号（1 基，相对表达式原文）」，
//! 与 C++ 版的两个出参 `error` / `column` 一一对应（`fail` 只记第一处失败）。

use crate::text::{byte_at, is_name_part, is_name_start, is_space, matches_at};
use crate::Preprocessor;

/// 求值失败时的说明与列号。
#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) struct ExprError {
    /// 给人看的说明。
    pub message: String,
    /// 出错位置（1 基列号，相对表达式原文）。
    pub column: usize,
}

/// 条件表达式的解析器。
///
/// * `F` — 符号查询回调：找到返回 `Some(值)`，没定义返回 `None`。
///   C++ 版是 `std::function<bool(const std::string &, std::string *)>`，
///   第二个参数是“顺便取值”的出参、`nullptr` 表示只问“在不在”；
///   `Option<String>` 把这两件事合成一件。
pub(crate) struct ExprParser<'a, F>
where
    F: Fn(&str) -> Option<String>,
{
    /// 表达式原文。
    text: &'a str,
    /// 符号查询。
    lookup: F,
    /// 当前下标（字节）。
    index: usize,
    /// 最近一次查到的符号值（比较运算用）。
    last_value: String,
    /// 第一处失败（只记一次）。
    error: Option<ExprError>,
}

impl<'a, F> ExprParser<'a, F>
where
    F: Fn(&str) -> Option<String>,
{
    /// 造一个解析器。
    pub(crate) fn new(text: &'a str, lookup: F) -> Self {
        ExprParser {
            text,
            lookup,
            index: 0,
            last_value: String::new(),
            error: None,
        }
    }

    /// 解析并求值（对应 C++ 的 `bool run(value, error, column)`）。
    pub(crate) fn run(mut self) -> std::result::Result<bool, ExprError> {
        self.skip_space();
        if self.at_end() {
            self.fail("条件表达式是空的", 1);
            return Err(self.take_error());
        }
        let value = self.parse_or();
        if self.error.is_some() {
            return Err(self.take_error());
        }
        self.skip_space();
        if !self.at_end() {
            self.fail("条件表达式后面还有多余内容", self.index + 1);
            return Err(self.take_error());
        }
        Ok(value)
    }

    /// 取出第一处失败。
    fn take_error(&mut self) -> ExprError {
        self.error.take().unwrap_or(ExprError {
            message: String::new(),
            column: 0,
        })
    }

    /// 跳过空白。
    fn skip_space(&mut self) {
        while self.index < self.text.len() && is_space(byte_at(self.text, self.index)) {
            self.index += 1;
        }
    }

    /// 是否到末尾。
    fn at_end(&self) -> bool {
        self.index >= self.text.len()
    }

    /// 记一次失败（只在还没有失败时记）。
    fn fail(&mut self, message: &str, column: usize) {
        if self.error.is_none() {
            self.error = Some(ExprError {
                message: message.to_string(),
                column,
            });
        }
    }

    /// 尝试吃掉一个运算符。
    ///
    /// 与 C++ 版一致：**无论成功与否都会先跳过空白**（失败了也不回退空白），
    /// 所以调用方想“没匹配就当没发生”时，得自己存一份 `index`。
    fn eat(&mut self, token: &str) -> bool {
        self.skip_space();
        if !matches_at(self.text, self.index, token) {
            return false;
        }
        self.index += token.len();
        true
    }

    /// 或运算。
    fn parse_or(&mut self) -> bool {
        let mut value = self.parse_and();
        if self.error.is_some() {
            return false;
        }
        while self.eat("||") {
            let right = self.parse_and();
            if self.error.is_some() {
                return false;
            }
            value = value || right;
        }
        value
    }

    /// 与运算。
    fn parse_and(&mut self) -> bool {
        let mut value = self.parse_not();
        if self.error.is_some() {
            return false;
        }
        while self.eat("&&") {
            let right = self.parse_not();
            if self.error.is_some() {
                return false;
            }
            value = value && right;
        }
        value
    }

    /// 非运算。
    fn parse_not(&mut self) -> bool {
        if self.eat("!") {
            return !self.parse_not();
        }
        self.parse_compare()
    }

    /// 比较：`NAME == 值` / `NAME != 值`。
    fn parse_compare(&mut self) -> bool {
        let left = self.parse_atom();
        if self.error.is_some() {
            return false;
        }

        let save = self.index;
        let negate = if self.eat("==") {
            false
        } else if self.eat("!=") {
            true
        } else {
            self.index = save;
            return left;
        };

        // 右边只接受一个字面量（带引号或不带引号的单词）。
        self.skip_space();
        let literal = match self.parse_literal() {
            Some(text) => text,
            None => return false,
        };
        let equal = self.last_value == literal;
        if negate {
            !equal
        } else {
            equal
        }
    }

    /// 原子：`defined(NAME)`、括号、符号名。
    fn parse_atom(&mut self) -> bool {
        self.skip_space();
        if self.at_end() {
            self.fail("条件表达式不完整", self.index + 1);
            return false;
        }

        // 1. defined(NAME)
        if byte_at(self.text, self.index) == b'd' && matches_at(self.text, self.index, "defined") {
            let start = self.index;
            self.index += 7;
            self.skip_space();
            if self.at_end() || byte_at(self.text, self.index) != b'(' {
                self.fail("defined 后面要跟 (名字)", start + 1);
                return false;
            }
            self.index += 1;
            self.skip_space();
            let name = match self.parse_name() {
                Some(name) => name,
                None => return false,
            };
            self.skip_space();
            if self.at_end() || byte_at(self.text, self.index) != b')' {
                self.fail("defined(名字) 缺少右括号", self.index + 1);
                return false;
            }
            self.index += 1;
            return (self.lookup)(&name).is_some();
        }

        // 2. 括号
        if byte_at(self.text, self.index) == b'(' {
            self.index += 1;
            let value = self.parse_or();
            if self.error.is_some() {
                return false;
            }
            self.skip_space();
            if self.at_end() || byte_at(self.text, self.index) != b')' {
                self.fail("条件表达式缺少右括号", self.index + 1);
                return false;
            }
            self.index += 1;
            return value;
        }

        // 3. 符号名：未定义 = 假；定义了没值 = 真；有值按值判真假。
        let name = match self.parse_name() {
            Some(name) => name,
            None => return false,
        };
        match (self.lookup)(&name) {
            None => {
                self.last_value.clear();
                false
            }
            Some(value) => {
                let truth = if value.is_empty() {
                    true
                } else {
                    Preprocessor::parse_truth_value(&value)
                };
                self.last_value = value;
                truth
            }
        }
    }

    /// 解析一个字面量（带引号的原样取内容，不带引号取到空白/运算符为止）。
    fn parse_literal(&mut self) -> Option<String> {
        self.skip_space();
        if self.at_end() {
            self.fail("比较运算缺少右操作数", self.index + 1);
            return None;
        }

        let first = byte_at(self.text, self.index);
        if first == b'"' || first == b'\'' {
            self.index += 1;
            // C++ 是逐个 `push_back(char)`（字节），这里同样按字节收，
            // 最后再整体按 UTF-8 解释 —— 直接往 String 里 push 单个续字节会切坏字符。
            let mut value: Vec<u8> = Vec::new();
            let mut closed = false;
            while !self.at_end() {
                let ch = byte_at(self.text, self.index);
                self.index += 1;
                if ch == b'\\' && !self.at_end() {
                    value.push(byte_at(self.text, self.index));
                    self.index += 1;
                    continue;
                }
                if ch == first {
                    closed = true;
                    break;
                }
                value.push(ch);
            }
            if !closed {
                self.fail("字面量缺少收尾引号", self.index + 1);
                return None;
            }
            return Some(String::from_utf8_lossy(&value).into_owned());
        }

        let start = self.index;
        while !self.at_end() {
            let ch = byte_at(self.text, self.index);
            if is_space(ch) || ch == b')' || ch == b'(' {
                break;
            }
            if ch == b'&' || ch == b'|' || ch == b'=' || ch == b'!' {
                break;
            }
            self.index += 1;
        }
        if self.index == start {
            self.fail("比较运算缺少右操作数", self.index + 1);
            return None;
        }
        Some(self.text[start..self.index].to_string())
    }

    /// 解析一个符号名。
    fn parse_name(&mut self) -> Option<String> {
        self.skip_space();
        if self.at_end() || !is_name_start(byte_at(self.text, self.index)) {
            self.fail("这里要写一个符号名", self.index + 1);
            return None;
        }
        let start = self.index;
        while !self.at_end() && is_name_part(byte_at(self.text, self.index)) {
            self.index += 1;
        }
        Some(self.text[start..self.index].to_string())
    }
}
