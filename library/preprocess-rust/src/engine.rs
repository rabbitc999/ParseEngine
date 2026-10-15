//! 一次预处理过程的实现体（对应 `preprocess.cpp` 第二个匿名命名空间里的
//! `ScanState` / `ScanInfo` / `CondFrame` / `Directive` / `Engine`）。
//!
//! 生命周期就是一次 `run_file` / `run_text`：符号表、包含集合、输出都在这里。
//!
//! 注意：`Result` 在本 crate 里被占用了（= 预处理结果，与 C++ 版同名），
//! 所以这里**不** `use crate::Result`，需要写结果类型时用 `crate::Result`
//! 全路径；本模块里的裸 `Result<...>` 仍然是标准库那个。

use std::collections::{HashMap, HashSet};

use crate::expr::ExprParser;
use crate::text::{
    byte_at, dir_name, file_exists, is_name_part, is_name_start, is_space, join_path, matches_at,
    normalize_path, read_file, split_lines, strip_bom, strip_trailing_comment, to_lower, trim,
};
use crate::{
    Define, Diagnostic, IncludeAction, IncludeRequest, IncludeResult, LineOrigin, Options,
    Preprocessor, Severity, UnknownDirective,
};

// ---------------------------------------------------------------------------
//  扫描状态、条件帧、指令分类
// ---------------------------------------------------------------------------

/// 跨行保留的扫描状态。
#[derive(Debug, Default, Clone)]
struct ScanState {
    /// 是否正处在块注释里。
    in_block_comment: bool,
    /// 当前块注释的结束串。
    block_comment_end: String,
    /// 非空表示正处在某个不透明区域，值是它的结束标记。
    opaque_end: String,
}

/// 一行的扫描结果。
#[derive(Debug, Default, Clone, Copy)]
struct ScanInfo {
    /// 行内是否有“有意义”的字符。
    has_content: bool,
    /// 有意义字符的下标。
    content_index: usize,
    /// 行首是不透明区域的开始（此时不当指令）。
    opaque_start: bool,
}

/// 条件编译的一帧。
#[derive(Debug, Clone)]
struct CondFrame {
    /// `%if` 所在文件（报错用）。
    file: String,
    /// `%if` 所在行（报错用）。
    line: usize,
    /// 外层是否活跃。
    parent_active: bool,
    /// 本分支当前是否输出。
    active: bool,
    /// 是否已有分支被选中（`%elif` / `%else` 用）。
    taken: bool,
    /// 是否已经出现过 `%else`。
    saw_else: bool,
}

/// 指令种类。
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum Directive {
    /// `%symbol`（`%define` 别名）
    Symbol,
    /// `%unsymbol`（`%undefine` 别名）
    Unsymbol,
    /// `%if`
    If,
    /// `%ifdef NAME`（等价 `%if defined(NAME)`）
    IfDef,
    /// `%ifndef NAME`（等价 `%if !defined(NAME)`）
    IfNotDef,
    /// `%elif`
    Elif,
    /// `%else`
    Else,
    /// `%endif`
    Endif,
    /// `%include`
    Include,
    /// `%include_once` / `%import`
    IncludeOnce,
    /// `%once`
    Once,
    /// `%pragma once`
    Pragma,
    /// `%error`
    Error,
    /// `%warning`
    Warning,
    /// 其它
    Unknown,
}

/// 符号名 → 查表键（大小写不敏感时统一转小写）。
///
/// C++ 版用的是带比较器的 `std::map`；这里用普通哈希表 + 键归一化，
/// 效果一致（符号表只用来查，从不遍历，所以看不出区别）。
fn symbol_key<'k>(name: &'k str, case_insensitive: bool) -> std::borrow::Cow<'k, str> {
    if case_insensitive {
        std::borrow::Cow::Owned(name.to_ascii_lowercase())
    } else {
        std::borrow::Cow::Borrowed(name)
    }
}

/// 解析 `"路径"` / `'路径'`。
fn parse_quoted_path(argument: &str) -> Option<&str> {
    let bytes = argument.as_bytes();
    if bytes.len() < 2 {
        return None;
    }
    let quote = bytes[0];
    if quote != b'"' && quote != b'\'' {
        return None;
    }
    if bytes[bytes.len() - 1] != quote {
        return None;
    }
    let path = &argument[1..argument.len() - 1];
    if path.is_empty() {
        None
    } else {
        Some(path)
    }
}

/// 去掉 `%error "…"` 里的引号；没引号就原样返回。
fn unquote(argument: &str) -> &str {
    parse_quoted_path(argument).unwrap_or(argument)
}

// ---------------------------------------------------------------------------
//  引擎
// ---------------------------------------------------------------------------

/// 一次预处理过程的实现体。
pub(crate) struct Engine<'a> {
    /// 选项。
    options: &'a Options,
    /// 结果。
    out: &'a mut crate::Result,
    /// 实际生效的指令前缀（选项不会在一次处理中变，构造时算一次）。
    prefixes: Vec<u8>,
    /// 符号表。
    symbols: HashMap<String, String>,
    /// 包含栈（循环检测 + 报错链）。
    include_stack: Vec<String>,
    /// 已经展开过的“只展开一次”文件。
    included_once: HashSet<String>,
    /// 写了 `%once` 的文件。
    once_files: HashSet<String>,
    /// 上一次插 `#line` 时的文件。
    last_marker_file: String,
}

impl<'a> Engine<'a> {
    /// 构造。
    pub(crate) fn new(options: &'a Options, out: &'a mut crate::Result) -> Self {
        Engine {
            prefixes: options.prefixes().into_bytes(),
            options,
            out,
            symbols: HashMap::new(),
            include_stack: Vec::new(),
            included_once: HashSet::new(),
            once_files: HashSet::new(),
            last_marker_file: String::new(),
        }
    }

    // ---- 入口 -------------------------------------------------------------

    /// 处理一个文件。
    pub(crate) fn run_file(&mut self, path: &str) -> bool {
        self.seed_defines();

        // 1. 主文件必须能读出来。
        let absolute = normalize_path(path);
        let text = match read_file(&absolute) {
            Some(text) => text,
            None => {
                self.report(
                    Severity::Error,
                    "open-failed",
                    path,
                    1,
                    1,
                    &format!("打不开文件：{absolute}"),
                );
                return false;
            }
        };

        // 2. 处理；主文件也记进依赖表。
        self.add_dependency(&absolute);
        self.include_stack.push(absolute.clone());
        let base = dir_name(&absolute);
        self.process_source(strip_bom(&text), &absolute, &base, 0);
        self.include_stack.pop();
        !self.has_errors()
    }

    /// 处理一段文本。
    pub(crate) fn run_text(&mut self, text: &str, label: &str, base_dir: &str) -> bool {
        self.seed_defines();
        self.process_source(strip_bom(text), label, base_dir, 0);
        !self.has_errors()
    }

    // ---- 主循环 -----------------------------------------------------------

    /// 把 `options.defines` 灌进符号表。
    fn seed_defines(&mut self) {
        let case_insensitive = self.options.case_insensitive;
        for define in &self.options.defines {
            let key = symbol_key(&define.name, case_insensitive).into_owned();
            self.symbols.insert(key, define.value.clone());
        }
    }

    /// 处理一份文本（主文件或 `%include` 进来的文件）。
    fn process_source(&mut self, text: &str, file: &str, base_dir: &str, depth: usize) {
        let lines = split_lines(text);
        self.out.lines_in += lines.len();

        let mut state = ScanState::default();
        let mut frames: Vec<CondFrame> = Vec::new();

        for (index, line) in lines.iter().enumerate() {
            let line = *line;
            let line_number = index + 1;
            let info = self.scan_line(line, &mut state);

            // 1. 空行、纯注释行、不透明区域内部：按内容行处理。
            let is_directive = info.has_content
                && !info.opaque_start
                && self.prefixes.contains(&byte_at(line, info.content_index))
                && info.content_index + 1 < line.len()
                && is_name_start(byte_at(line, info.content_index + 1));

            if !is_directive {
                if active(&frames) {
                    self.emit_line(line, file, line_number);
                } else {
                    // 条件为假：默认输出一个空行占位，保证行号与输入一致。
                    self.emit_dropped(file, line_number);
                }
                continue;
            }

            // 2. 指令行本身不输出（除非 keep_directive_lines），同样补空行占位。
            let emitted =
                self.handle_directive(line, &info, file, line_number, base_dir, depth, &mut frames);
            if !emitted {
                if self.options.keep_directive_lines {
                    self.emit_line(line, file, line_number);
                } else {
                    self.emit_dropped(file, line_number);
                }
            }
        }

        // 3. 文件结束时条件栈必须是空的（与 metro_editor 一样报出来）。
        for frame in &frames {
            self.report(
                Severity::Error,
                "unterminated-if",
                &frame.file,
                frame.line,
                1,
                "这个 %if 没有对应的 %endif",
            );
        }
    }

    /// 扫描一行：跳过注释、字符串、不透明区域，找第一个有意义的字符。
    fn scan_line(&self, line: &str, state: &mut ScanState) -> ScanInfo {
        let mut info = ScanInfo::default();
        let mut index = 0usize;

        // 1. 还处在不透明区域里：先找结束标记。
        if !state.opaque_end.is_empty() {
            match line.find(state.opaque_end.as_str()) {
                None => return info,
                Some(end) => {
                    index = end + state.opaque_end.len();
                    state.opaque_end.clear();
                }
            }
        }

        while index < line.len() {
            // 2. 块注释里：找结束标记。
            if state.in_block_comment {
                match line[index..].find(state.block_comment_end.as_str()) {
                    None => return info,
                    Some(relative) => {
                        index += relative + state.block_comment_end.len();
                        state.in_block_comment = false;
                        continue;
                    }
                }
            }

            let ch = byte_at(line, index);
            if is_space(ch) {
                index += 1;
                continue;
            }

            // 3. 行注释：命中任意一个起始串，本行后面就没有内容了。
            if self
                .options
                .line_comments
                .iter()
                .any(|marker| matches_at(line, index, marker))
            {
                return info;
            }

            // 4. 块注释开始：记住是哪一对，跨行时用它的结束串。
            let mut is_block_comment = false;
            for (begin, end) in &self.options.block_comments {
                if !matches_at(line, index, begin) {
                    continue;
                }
                state.in_block_comment = true;
                state.block_comment_end = end.clone();
                index += begin.len();
                is_block_comment = true;
                break;
            }
            if is_block_comment {
                continue;
            }

            // 5. 不透明区域开始（行锚定的标记必须顶格）。
            let mut entered_opaque = false;
            for region in &self.options.opaque_regions {
                if !matches_at(line, index, &region.begin) {
                    continue;
                }
                if region.line_anchored && index != 0 {
                    continue;
                }
                // `%code` 不能匹配 `%codex`：标记以名字字符结尾时，后面必须是分隔符。
                let after = index + region.begin.len();
                if !region.begin.is_empty()
                    && is_name_part(byte_at(&region.begin, region.begin.len() - 1))
                    && after < line.len()
                    && is_name_part(byte_at(line, after))
                {
                    continue;
                }
                state.opaque_end = region.end.clone();
                index = after;
                info.has_content = true;
                info.content_index = 0;
                info.opaque_start = true;
                entered_opaque = true;
                break;
            }
            if entered_opaque {
                // 区域可能在同行结束，结束后继续往后扫。
                match line[index..].find(state.opaque_end.as_str()) {
                    None => return info,
                    Some(relative) => {
                        index += relative + state.opaque_end.len();
                        state.opaque_end.clear();
                        continue;
                    }
                }
            }

            // 6. 字符串：整段跳过（里面的指令不当指令）。
            if self.options.string_delimiters.as_bytes().contains(&ch) {
                let quote = ch;
                index += 1;
                while index < line.len() {
                    let current = byte_at(line, index);
                    index += 1;
                    if current == b'\\' && index < line.len() {
                        index += 1;
                        continue;
                    }
                    if current == quote {
                        break;
                    }
                }
                continue;
            }

            // 7. 找到有意义的内容。
            info.has_content = true;
            info.content_index = index;
            return info;
        }

        info
    }

    // ---- 指令 -------------------------------------------------------------

    /// 处理一条指令行（返回“已经自己输出过了”，调用方就不再补空行占位）。
    ///
    /// 参数多是因为这里就是 C++ 那个 `handleDirective` 的签名：它把「行 + 扫描结果 +
    /// 文件 + 行号 + 基准目录 + 深度 + 条件栈」全收在手里。拆成结构体只会让两边对照
    /// 变难，所以留着。
    #[allow(clippy::too_many_arguments)]
    fn handle_directive(
        &mut self,
        line: &str,
        info: &ScanInfo,
        file: &str,
        line_number: usize,
        base_dir: &str,
        depth: usize,
        frames: &mut Vec<CondFrame>,
    ) -> bool {
        let prefix = info.content_index;
        let prefix_char = byte_at(line, prefix);
        let mut cursor = prefix + 1;
        let name_start = cursor;
        while cursor < line.len() && is_name_part(byte_at(line, cursor)) {
            cursor += 1;
        }

        let name = to_lower(&line[name_start..cursor]);
        // 参数末尾挂的注释不属于参数：`%include "a.inc"  // 说明` 也要能用。
        let argument = strip_trailing_comment(trim(&line[cursor..]));
        let argument_column = cursor + 1;
        let active_now = active(frames);

        match self.classify(&name, prefix_char) {
            Directive::Symbol | Directive::Unsymbol => {
                // 1. `%symbol NAME [VALUE]` / `%unsymbol NAME`。
                let mut define = Define::default();
                if let Err(diagnostic) = Preprocessor::parse_define(argument, &mut define) {
                    self.report(
                        Severity::Error,
                        "bad-argument",
                        file,
                        line_number,
                        argument_column,
                        &diagnostic.message,
                    );
                    return false;
                }
                if active_now {
                    let key = symbol_key(&define.name, self.options.case_insensitive).into_owned();
                    if self.classify(&name, prefix_char) == Directive::Symbol {
                        self.symbols.insert(key, define.value);
                    } else {
                        self.symbols.remove(&key);
                    }
                }
                false
            }

            Directive::If | Directive::IfDef | Directive::IfNotDef => {
                // 2. `%if EXPR` / `%ifdef NAME` / `%ifndef NAME`：
                //    外层不活跃时不再求值，只记一帧保持嵌套配平。
                let mut value = false;
                if active_now {
                    let kind = self.classify(&name, prefix_char);
                    let expression = match kind {
                        Directive::If => argument.to_string(),
                        Directive::IfDef => format!("defined({argument})"),
                        _ => format!("!defined({argument})"),
                    };
                    value = self.evaluate_expression(&expression, file, line_number, argument_column);
                }
                frames.push(CondFrame {
                    file: file.to_string(),
                    line: line_number,
                    parent_active: active_now,
                    active: active_now && value,
                    taken: active_now && value,
                    saw_else: false,
                });
                false
            }

            Directive::Elif => {
                // 3. `%elif EXPR`：只在“外层活跃 + 还没分支命中”时求值。
                if frames.is_empty() {
                    self.report(
                        Severity::Error,
                        "unmatched-elif",
                        file,
                        line_number,
                        prefix + 1,
                        "多余的 %elif，缺少匹配的 %if",
                    );
                    return false;
                }
                if frames[frames.len() - 1].saw_else {
                    self.report(
                        Severity::Error,
                        "elif-after-else",
                        file,
                        line_number,
                        prefix + 1,
                        "%elif 不能写在 %else 之后",
                    );
                    return false;
                }
                let mut value = false;
                {
                    let frame = &frames[frames.len() - 1];
                    if frame.parent_active && !frame.taken {
                        value =
                            self.evaluate_expression(argument, file, line_number, argument_column);
                    }
                }
                let frame = match frames.last_mut() {
                    Some(frame) => frame,
                    None => return false, // 上面已经判过非空
                };
                frame.active = frame.parent_active && !frame.taken && value;
                if frame.active {
                    frame.taken = true;
                }
                false
            }

            Directive::Else => {
                // 4. `%else`：翻转当前帧。
                if frames.is_empty() {
                    self.report(
                        Severity::Error,
                        "unmatched-else",
                        file,
                        line_number,
                        prefix + 1,
                        "多余的 %else，缺少匹配的 %if",
                    );
                    return false;
                }
                let frame = match frames.last_mut() {
                    Some(frame) => frame,
                    None => return false, // 上面已经判过非空
                };
                if frame.saw_else {
                    self.report(
                        Severity::Error,
                        "duplicate-else",
                        file,
                        line_number,
                        prefix + 1,
                        "同一个 %if 里出现了两次 %else",
                    );
                    return false;
                }
                frame.saw_else = true;
                frame.active = frame.parent_active && !frame.taken;
                frame.taken = true;
                false
            }

            Directive::Endif => {
                // 5. `%endif`：收掉一帧。
                if frames.is_empty() {
                    self.report(
                        Severity::Error,
                        "unmatched-endif",
                        file,
                        line_number,
                        prefix + 1,
                        "多余的 %endif，缺少匹配的 %if",
                    );
                    return false;
                }
                frames.pop();
                false
            }

            Directive::Include | Directive::IncludeOnce => {
                // 6. 包含：不活跃的分支不展开（语法仍然检查）。
                //    展开成功时，被包含内容顶替这一行（不再补空行）。
                //    `%import` 与 `%include_once` 都按“只展开一次”处理，区别只有
                //    `imported` 标记——自定义 loader 可以据此走不同分支。
                let imported = name == "import";
                let once_requested = self.classify(&name, prefix_char) == Directive::IncludeOnce;
                self.handle_include(
                    argument,
                    imported,
                    once_requested,
                    file,
                    line_number,
                    argument_column,
                    base_dir,
                    depth,
                    active_now,
                )
            }

            Directive::Once => {
                // 7. `%once`：把当前文件登记成“只展开一次”。
                self.once_files.insert(file.to_string());
                false
            }

            Directive::Pragma => {
                // 8. `%pragma once`：与 `%once` 等价；别的 pragma 只给一句警告。
                if to_lower(trim(argument)) == "once" {
                    self.once_files.insert(file.to_string());
                } else if active_now {
                    self.report(
                        Severity::Warning,
                        "unknown-pragma",
                        file,
                        line_number,
                        prefix + 1,
                        "只认识 %pragma once，其余的忽略",
                    );
                }
                false
            }

            Directive::Error => {
                // 9. `%error "…"`：活跃分支里直接报错（与 #error 一致）。
                if active_now {
                    self.report(
                        Severity::Error,
                        "error-directive",
                        file,
                        line_number,
                        prefix + 1,
                        unquote(argument),
                    );
                }
                false
            }

            Directive::Warning => {
                if active_now {
                    self.report(
                        Severity::Warning,
                        "warning-directive",
                        file,
                        line_number,
                        prefix + 1,
                        unquote(argument),
                    );
                }
                false
            }

            Directive::Unknown => {
                // 10. 不认识的指令：按选项报错 / 原样保留 / 静默丢掉。
                match self.options.unknown_directive {
                    UnknownDirective::Keep => {
                        // 原样保留：活跃分支里把原文写出去，非活跃分支仍然补空行占位。
                        if active_now {
                            self.emit_line(line, file, line_number);
                        } else {
                            self.emit_dropped(file, line_number);
                        }
                        true
                    }
                    UnknownDirective::Error => {
                        self.report(
                            Severity::Error,
                            "unknown-directive",
                            file,
                            line_number,
                            prefix + 1,
                            &format!("不认识的预处理指令 {}", &line[prefix..]),
                        );
                        false
                    }
                    UnknownDirective::Ignore => false,
                }
            }
        }
    }

    /// 指令名 → 种类。
    ///
    /// `prefix` 是该行用到的前缀字符（决定 `define` / `undef` 是否算别名）。
    fn classify(&self, name: &str, prefix: u8) -> Directive {
        if name == "symbol" {
            return Directive::Symbol;
        }
        if name == "unsymbol" {
            return Directive::Unsymbol;
        }
        // `#define` / `#undef` 是 C 风格写法，始终支持；
        // `%define` / `%undefine` 是 metro_editor 风格，要显式打开。
        if prefix == b'#' || self.options.percent_define_is_symbol {
            if name == "define" {
                return Directive::Symbol;
            }
            if name == "undef" || name == "undefine" {
                return Directive::Unsymbol;
            }
        }
        match name {
            "if" => Directive::If,
            "ifdef" => Directive::IfDef,
            "ifndef" => Directive::IfNotDef,
            "elif" => Directive::Elif,
            "else" => Directive::Else,
            "endif" => Directive::Endif,
            "include" => Directive::Include,
            "include_once" => Directive::IncludeOnce,
            "import" => Directive::IncludeOnce,
            "once" => Directive::Once,
            "pragma" => Directive::Pragma,
            "error" => Directive::Error,
            "warning" => Directive::Warning,
            _ => Directive::Unknown,
        }
    }

    /// 展开一个 `%include`（返回“真的展开了”，调用方就不再补空行占位）。
    #[allow(clippy::too_many_arguments)]
    fn handle_include(
        &mut self,
        argument: &str,
        imported: bool,
        once_requested: bool,
        file: &str,
        line_number: usize,
        column: usize,
        base_dir: &str,
        depth: usize,
        active_now: bool,
    ) -> bool {
        // 1. 语法：路径必须带引号。
        let path = match parse_quoted_path(argument) {
            Some(path) => path,
            None => {
                self.report(
                    Severity::Error,
                    "bad-path",
                    file,
                    line_number,
                    column,
                    "include 后面要跟一个带引号的路径，例如 %include \"common.inc\"",
                );
                return false;
            }
        };
        if !active_now {
            return false;
        }

        self.out.include_count += 1;

        // 2. 先按惯例找一遍路径：即使有自定义 loader，也能把结果给它参考。
        let resolved_path = self.resolve_include(path, file, base_dir);
        let once_by_default = once_requested || self.options.include_once_by_default;

        // 3. 问自定义 loader：它可以自己给正文（CSV / 数据库 / 内存文本）、
        //    可以跳过、可以报错，也可以交回默认逻辑。
        let custom = match self.options.include_loader.as_ref() {
            None => IncludeResult::use_default(),
            Some(loader) => {
                let request = IncludeRequest {
                    path: path.to_string(),
                    resolved_path: resolved_path.clone(),
                    from_file: file.to_string(),
                    base_dir: base_dir.to_string(),
                    imported,
                    once: once_by_default || self.once_files.contains(&resolved_path),
                };
                loader(&request)
            }
        };

        if custom.action == IncludeAction::Error {
            let message = if custom.message.is_empty() {
                format!("自定义 loader 拒绝加载 {path}")
            } else {
                custom.message
            };
            self.report(
                Severity::Error,
                "include-loader-error",
                file,
                line_number,
                column,
                &message,
            );
            return false;
        }
        if custom.action == IncludeAction::Skip {
            return false;
        }

        // 4. 定下这次展开的正文与它的标识（标识进依赖表 / 去重 / 循环检测）。
        let label;
        let mut text = String::new();
        let mut from_memory = false;
        match custom.action {
            IncludeAction::Inline => {
                text = custom.text;
                label = if custom.label.is_empty() {
                    if resolved_path.is_empty() {
                        path.to_string()
                    } else {
                        resolved_path.clone()
                    }
                } else {
                    custom.label
                };
                from_memory = true;
            }
            IncludeAction::UseFile => {
                label = if custom.label.is_empty() {
                    resolved_path.clone()
                } else {
                    custom.label
                };
            }
            _ => {
                label = resolved_path.clone();
            }
        }

        if label.is_empty() {
            let mut search_dirs = if base_dir.is_empty() {
                dir_name(file)
            } else {
                base_dir.to_string()
            };
            for dir in &self.options.include_dirs {
                search_dirs.push_str(", ");
                search_dirs.push_str(dir);
            }
            self.report(
                Severity::Error,
                "include-not-found",
                file,
                line_number,
                column,
                &format!("找不到被包含的文件 \"{path}\"（搜索目录：{search_dirs}）"),
            );
            return false;
        }

        // 5. 深度限制与循环检测。
        if depth >= self.options.max_include_depth {
            self.report(
                Severity::Error,
                "include-depth",
                file,
                line_number,
                column,
                &format!("包含层级超过 {} 层", self.options.max_include_depth),
            );
            return false;
        }
        if self.include_stack.iter().any(|item| item == &label) {
            let mut chain = String::new();
            for item in &self.include_stack {
                chain.push_str(item);
                chain.push_str(" → ");
            }
            chain.push_str(&label);
            self.report(
                Severity::Error,
                "include-cycle",
                file,
                line_number,
                column,
                &format!("循环包含：{chain}"),
            );
            return false;
        }

        // 6. 去重：同一个文件默认只展开一次。无论这次是否去重，都要记下
        //    “已经展开过”——否则文件在内容里写 `#pragma once` / `%once` 时，
        //    第二次包含会漏掉去重。
        let once = once_by_default || self.once_files.contains(&label);
        if once && self.included_once.contains(&label) {
            return false;
        }
        self.included_once.insert(label.clone());

        // 7. 取正文：loader 给了就用手上的，否则读文件。
        if !from_memory {
            match read_file(&label) {
                Some(content) => text = content,
                None => {
                    self.report(
                        Severity::Error,
                        "include-not-found",
                        file,
                        line_number,
                        column,
                        &format!("读不了被包含的文件：{label}"),
                    );
                    return false;
                }
            }
        }

        // 8. 递归展开：正文里的相对包含以被包含文件所在目录为基准。
        self.add_dependency(&label);
        self.include_stack.push(label.clone());
        self.last_marker_file.clear(); // 强制给被包含内容插一条 #line
        let child_base = if label.contains('/') {
            dir_name(&label)
        } else {
            base_dir.to_string()
        };
        self.process_source(strip_bom(&text), &label, &child_base, depth + 1);
        self.include_stack.pop();
        self.last_marker_file.clear(); // 回到上一层后，下一行也要重新插 #line
        true
    }

    // ---- 表达式、路径、输出、诊断 ------------------------------------------

    /// 求值一个条件表达式，失败时记诊断。
    fn evaluate_expression(
        &mut self,
        expression: &str,
        file: &str,
        line: usize,
        column: usize,
    ) -> bool {
        let case_insensitive = self.options.case_insensitive;
        let symbols = &self.symbols;
        let outcome = ExprParser::new(expression, |name| {
            symbols
                .get(symbol_key(name, case_insensitive).as_ref())
                .cloned()
        })
        .run();

        match outcome {
            Ok(value) => value,
            Err(error) => {
                let shift = if error.column > 0 { error.column - 1 } else { 0 };
                self.report(
                    Severity::Error,
                    "bad-expression",
                    file,
                    line,
                    column + shift,
                    &error.message,
                );
                false
            }
        }
    }

    /// 解析包含路径（返回存在的绝对路径；都找不到返回空串）。
    fn resolve_include(&self, path: &str, from_file: &str, base_dir: &str) -> String {
        let mut candidates: Vec<String> = Vec::new();
        if !base_dir.is_empty() {
            candidates.push(join_path(base_dir, path));
        }
        let base = dir_name(from_file);
        if !base.is_empty() {
            candidates.push(join_path(&base, path));
        }
        for dir in &self.options.include_dirs {
            candidates.push(join_path(dir, path));
        }
        candidates.push(normalize_path(path));

        for candidate in candidates {
            if file_exists(&candidate) {
                return candidate;
            }
        }
        String::new()
    }

    /// 输出一行（必要时先插 `#line` 标记）。
    fn emit_line(&mut self, line: &str, file: &str, line_number: usize) {
        if self.options.emit_line_markers && self.last_marker_file != file {
            let marker = format!(
                "{} {} \"{}\"\n",
                self.options.line_marker_prefix, line_number, file
            );
            self.out.text.push_str(&marker);
            self.out.line_map.push(LineOrigin {
                file: file.to_string(),
                line: line_number,
            });
            self.out.lines_out += 1;
            self.last_marker_file = file.to_string();
        }
        self.out.text.push_str(line);
        self.out.text.push('\n');
        self.out.line_map.push(LineOrigin {
            file: file.to_string(),
            line: line_number,
        });
        self.out.lines_out += 1;
    }

    /// 丢掉一行（按选项决定是否补一个空行）。
    fn emit_dropped(&mut self, file: &str, line_number: usize) {
        if !self.options.preserve_lines {
            return;
        }
        self.out.text.push('\n');
        self.out.line_map.push(LineOrigin {
            file: file.to_string(),
            line: line_number,
        });
        self.out.lines_out += 1;
    }

    /// 记一条诊断。
    fn report(
        &mut self,
        severity: Severity,
        code: &str,
        file: &str,
        line: usize,
        column: usize,
        message: &str,
    ) {
        self.out.diagnostics.push(Diagnostic {
            severity,
            code: code.to_string(),
            file: file.to_string(),
            line,
            column,
            message: message.to_string(),
        });
    }

    /// 是否已经有错误。
    fn has_errors(&self) -> bool {
        self.out.count(Severity::Error) > 0
    }

    /// 记一个依赖文件（去重、保持首次出现顺序）。
    fn add_dependency(&mut self, path: &str) {
        if self.out.dependencies.iter().any(|item| item == path) {
            return;
        }
        self.out.dependencies.push(path.to_string());
    }
}

/// 当前是否处于活跃分支。
fn active(frames: &[CondFrame]) -> bool {
    frames.is_empty() || frames[frames.len() - 1].active
}
