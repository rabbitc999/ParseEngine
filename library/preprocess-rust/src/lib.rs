//! 通用文本预处理器（Rust 版）：`%define` / `%undefine` / `%if` / `%include` 与行号映射。
//!
//! 这是 [`library/preprocess`](../../library/preprocess/README.md)（C++17）的 **Rust 移植**，
//! 行为与它逐字节对齐（同一套诊断短码、同一套默认值、同一套输出规则）。
//!
//! 这个 crate 把「源码进编译器之前的那一步」独立出来，流程参考 metro_editor 的
//! `MetroLang/Compiler/PreProcessor.cs`：
//!
//! - 指令一律「单独一行、行首（允许缩进）」。**语言无关**：默认同时认 `#`
//!   （C / C++ / Objective-C 家族）与 `%`（yacc / `.gram` / metro_editor 家族）；
//! - 符号：`#define NAME [VALUE]` / `#undef NAME`（C 风格），中性写法是
//!   `%symbol` / `%unsymbol`；`%define` 这种 yacc 风格要显式打开
//!   `percent_define_is_symbol`，免得把 `.gram` 自己的 `%define NAME "正则"` 吃掉；
//! - 条件：`if` / `ifdef` / `ifndef` / `elif` / `else` / `endif`（两个前缀都认）；
//! - 包含：`include`、`import`（等价 `include_once`）、`include_once`、`once`、
//!   `pragma once`；同一个文件默认只展开一次，并带循环与深度保护；
//! - 出错不中断：一路收诊断，最后一次性交给调用方（metro_editor 也是这样
//!   收集 `List<string> errors` 的）。
//!
//! 默认「**丢掉的每一行都用空行占位**」：指令行与条件为假的行都会输出一个空行，
//! 于是输出的行号与输入一一对应（`include` 展开的地方除外），上层按原文件行号
//! 报错时不用额外换算；想要紧凑输出把 [`Options::preserve_lines`] 关掉即可。
//!
//! 在此之上补了两件本仓库需要的东西：
//!
//! - 条件表达式支持 `|| && ! ()`、`NAME == VALUE`、`defined(NAME)`
//!   （与 ParseEngine 生成器的 `--define` 语义保持一致）；
//! - 输出带**行号映射**（第 i 行来自哪个文件的哪一行），需要时还能直接吐
//!   `#line` 标记，方便上层按原文件报错。
//!
//! **只用标准库，没有任何第三方依赖**（`Cargo.lock` 里只有本 crate 自己）。
//!
//! # 与 C++ 版的一处命名差别
//!
//! 结果类型**故意**叫 [`Result`]，与 C++ 版的 `preprocess::Result` 同名。
//! 代价是：一旦你 `use preprocess::Result;`，本模块里的裸 `Result<(), E>`
//! 就指向这个结构体了 —— 需要标准库那个时写 `std::result::Result`。

#![forbid(unsafe_code)]

mod engine;
mod expr;
mod text;

use std::collections::HashMap;
use std::fmt;

/// 一个符号定义（`-D NAME` 或 `%define NAME VALUE`）。
///
/// `value` 为空串表示「只定义、没有值」，此时符号为真。
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct Define {
    /// 符号名。
    pub name: String,
    /// 符号值（可以为空）。
    pub value: String,
}

impl Define {
    /// 造一个符号定义。
    pub fn new(name: impl Into<String>, value: impl Into<String>) -> Self {
        Define {
            name: name.into(),
            value: value.into(),
        }
    }
}

/// 一次包含请求（交给自定义 loader 判断怎么处理）。
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct IncludeRequest {
    /// 指令里写的原文路径（引号内），例如 `../data/stations.csv`。
    pub path: String,
    /// 库按惯例（当前文件目录 → `include_dirs` → 原样）找到的绝对路径；没找到为空。
    pub resolved_path: String,
    /// 包含方文件（绝对路径或 `run_text` 的标签）。
    pub from_file: String,
    /// `%include` 相对路径的基准目录。
    pub base_dir: String,
    /// true = `%import` 触发的，false = `%include` / `%include_once`。
    pub imported: bool,
    /// 这次是否按“只展开一次”处理（`%import` / `%include_once` / 默认打开时为 true）。
    pub once: bool,
}

/// [`IncludeResult`] 的处理方式（C++ 版是 `IncludeResult::Action`）。
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum IncludeAction {
    /// 交给库的默认逻辑。
    #[default]
    Default,
    /// 从 `resolved_path` 读文件。
    UseFile,
    /// 用 `text` 里的正文。
    Inline,
    /// 什么都不做。
    Skip,
    /// 报错。
    Error,
}

/// 自定义 loader 的返回值。
///
/// 常见用法：
///
/// - [`IncludeResult::use_default`]：我不处理，交回库的默认逻辑（读文件）；
/// - [`IncludeResult::use_file`]：路径我认了，正文仍从 `resolved_path` 读；
/// - [`IncludeResult::inline_text`]：正文我自己给（CSV、数据库、内存文本都行），
///   `label` 会进依赖表、去重集合与循环检测（留空则用 `resolved_path` / 原文路径）；
/// - [`IncludeResult::skip`]：这次包含什么都不做；
/// - [`IncludeResult::error`]：报一条 `include-loader-error` 错误。
///
/// C++ 版的 `useFile(label = "")` / `inlineText(text, label = "")` 有默认参数，
/// Rust 没有，所以拆成「不带 label」与「带 label」两个构造函数。
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct IncludeResult {
    /// 处理方式。
    pub action: IncludeAction,
    /// `Inline` 时展开的正文。
    pub text: String,
    /// `Inline` / `UseFile` 时的标识（依赖表、去重、循环检测都用它）。
    pub label: String,
    /// `Error` 时的消息。
    pub message: String,
}

impl IncludeResult {
    /// 构造“我不处理”的结果。
    pub fn use_default() -> Self {
        IncludeResult::default()
    }

    /// 构造“路径我认了、正文仍从文件读”的结果（`label` 留空 = 用 `resolved_path`）。
    pub fn use_file() -> Self {
        IncludeResult {
            action: IncludeAction::UseFile,
            ..IncludeResult::default()
        }
    }

    /// 同 [`IncludeResult::use_file`]，但显式给一个标识。
    pub fn use_file_as(label: impl Into<String>) -> Self {
        IncludeResult {
            action: IncludeAction::UseFile,
            label: label.into(),
            ..IncludeResult::default()
        }
    }

    /// 构造“正文我自己给”的结果（`label` 留空 = 用 `resolved_path` / 原文路径）。
    pub fn inline_text(text: impl Into<String>) -> Self {
        IncludeResult {
            action: IncludeAction::Inline,
            text: text.into(),
            ..IncludeResult::default()
        }
    }

    /// 同 [`IncludeResult::inline_text`]，但显式给一个标识。
    pub fn inline_text_as(text: impl Into<String>, label: impl Into<String>) -> Self {
        IncludeResult {
            action: IncludeAction::Inline,
            text: text.into(),
            label: label.into(),
            ..IncludeResult::default()
        }
    }

    /// 构造“跳过这次包含”的结果。
    pub fn skip() -> Self {
        IncludeResult {
            action: IncludeAction::Skip,
            ..IncludeResult::default()
        }
    }

    /// 构造“报错”的结果。
    pub fn error(message: impl Into<String>) -> Self {
        IncludeResult {
            action: IncludeAction::Error,
            message: message.into(),
            ..IncludeResult::default()
        }
    }
}

/// 自定义包含加载器的类型（对应 C++ 的
/// `std::function<IncludeResult(const IncludeRequest &)>`）。
///
/// 拿到一个 [`IncludeRequest`]，返回一个 [`IncludeResult`]。
pub type IncludeLoader = Box<dyn Fn(&IncludeRequest) -> IncludeResult>;

/// 一段「不解析」的区域（区域内的指令不当指令）。
///
/// 默认给的是 `.gram` 的两处：`%code … %endcode`、
/// `%class_body … %endbody`。处理别的语言时把 `opaque_regions` 换成自己的，
/// 或者清空。
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct OpaqueRegion {
    /// 起始标记（例如 `%code`）。
    pub begin: String,
    /// 结束标记（例如 `}}%`）。
    pub end: String,
    /// 起始标记是否必须单独占一行（允许缩进）。
    ///
    /// true：`%code` / `%class_body` 这种「段标记」；
    /// false：可出现在行内的起始标记。
    pub line_anchored: bool,
}

impl OpaqueRegion {
    /// 造一个区域标记。
    pub fn new(begin: impl Into<String>, end: impl Into<String>, line_anchored: bool) -> Self {
        OpaqueRegion {
            begin: begin.into(),
            end: end.into(),
            line_anchored,
        }
    }
}

/// 指令写法（按语言家族选）。
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Syntax {
    /// 同时认 `#` 与 `%`（默认）。
    Auto,
    /// 只认 `#`：C / C++ / Objective-C 等。
    Hash,
    /// 只认 `%`：yacc / `.gram` / metro_editor 等。
    Percent,
    /// 用 [`Options::directive_prefixes`] 里给的前缀集合。
    Custom,
}

/// 不认识的指令怎么处理。
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum UnknownDirective {
    /// 报一条 `unknown-directive` 错误，指令行丢掉。
    Error,
    /// 原样留在输出里——处理 `.gram` 这种「`%token` / `%define` 有自己的含义」
    /// 的文本时用这个。
    Keep,
    /// 静默丢掉。
    Ignore,
}

/// 预处理选项。
pub struct Options {
    /// 指令写法；决定哪些前缀会被当成指令行。
    pub syntax: Syntax,

    /// [`Syntax::Custom`] 使用的前缀字符集合（默认给了两个都常见的）。
    pub directive_prefixes: String,

    /// 预定义符号（`-D NAME[=VALUE]`）。
    pub defines: Vec<Define>,

    /// `%include` 的额外搜索目录（先查当前文件所在目录，再按顺序查这里）。
    pub include_dirs: Vec<String>,

    /// 自定义包含加载器：`%include` 与 `%import` 都会先问它。
    ///
    /// 留空时走默认逻辑（按 `include_dirs` 找文件、读成 UTF-8 文本）。设置了以后
    /// 可以把 CSV / JSON / 数据库之类的数据源接进来：
    ///
    /// ```
    /// use preprocess::{IncludeRequest, IncludeResult, Options};
    ///
    /// let mut options = Options::default();
    /// options.include_loader = Some(Box::new(|request: &IncludeRequest| {
    ///     // 1. 只接管 CSV，其它交回默认逻辑
    ///     if !request.path.ends_with(".csv") {
    ///         return IncludeResult::use_default();
    ///     }
    ///     // 2. 读 CSV 并拼成文法能吃的文本（这里只是示意）
    ///     let text = format!("row {}={}\n", request.resolved_path, request.path);
    ///     // 3. 用文件路径当标识：依赖表、去重、循环检测都认它
    ///     IncludeResult::inline_text_as(text, request.resolved_path.clone())
    /// }));
    /// ```
    ///
    /// `request.imported` 区分 `%import` 与 `%include`：想让它俩行为不同，
    /// 在回调里判断这个字段即可。
    pub include_loader: Option<IncludeLoader>,

    /// 符号名是否大小写不敏感（metro_editor 是不敏感的，默认按 C 习惯区分）。
    pub case_insensitive: bool,

    /// 同一个文件是否默认只展开一次（metro_editor 的行为）。
    pub include_once_by_default: bool,

    /// 指令行本身是否留在输出里（默认丢掉；打开便于肉眼比对）。
    pub keep_directive_lines: bool,

    /// 丢掉的行是否用空行占位（**默认打开**）。
    ///
    /// 打开：指令行、条件为假的行都输出一个空行，输出行号与输入一一对应
    /// （`%include` 展开处除外，那里靠 `line_map` / `#line` 追踪）。
    /// 关掉：整行删掉，输出更紧凑。
    pub preserve_lines: bool,

    /// 是否在输出里插入 `#line 行号 "文件"` 标记（换文件时插一条）。
    pub emit_line_markers: bool,

    /// `#line` 标记的前缀文本。
    pub line_marker_prefix: String,

    /// 最大嵌套展开深度，超过就报错（防循环包含）。
    pub max_include_depth: usize,

    /// 行注释起始串列表（默认 `//`）。
    ///
    /// 处理别的语言时按需要加：SQL / Lua 的 `--`、Python / shell 的 `#`、
    /// ini 的 `;` 都可以往这里塞；空列表表示不识别行注释。
    pub line_comments: Vec<String>,

    /// 块注释「起始 / 结束」串对（默认 `/* … */`）。
    ///
    /// 处理别的语言时可以加 `(* … *)`、`--[[ … ]]` 之类；空列表表示不识别块注释。
    pub block_comments: Vec<(String, String)>,

    /// 字符串定界符集合（`"'` 表示单双引号都识别；空串表示不识别）。
    pub string_delimiters: String,

    /// 不解析的区域（区域内的指令忽略）。
    ///
    /// 默认是空的——这是语言无关的默认值；处理 `.gram` 时用
    /// [`Preprocessor::for_grammar`] 会带上 `%code … %endcode`、
    /// `%class_body … %endbody` 两处。
    pub opaque_regions: Vec<OpaqueRegion>,

    /// 不认识指令的策略（见 [`UnknownDirective`]）。
    pub unknown_directive: UnknownDirective,

    /// 是否接受 metro_editor 的 `%define` / `%undefine` 写法。
    ///
    /// 默认关闭：中性写法是 `%symbol` / `%unsymbol`，这样处理 `.gram` 时不会把
    /// 文法自己的 `%define NAME "正则"` 吃掉。注意 `#define` / `#undef` 始终可用
    /// （C 风格），不受这个开关影响；metro_editor 那种 `%define` 可以打开它。
    pub percent_define_is_symbol: bool,
}

impl Default for Options {
    fn default() -> Self {
        Options {
            syntax: Syntax::Auto,
            directive_prefixes: "#%".to_string(),
            defines: Vec::new(),
            include_dirs: Vec::new(),
            include_loader: None,
            case_insensitive: false,
            include_once_by_default: true,
            keep_directive_lines: false,
            preserve_lines: true,
            emit_line_markers: false,
            line_marker_prefix: "#line".to_string(),
            max_include_depth: 32,
            line_comments: vec!["//".to_string()],
            block_comments: vec![("/*".to_string(), "*/".to_string())],
            string_delimiters: "\"'".to_string(),
            opaque_regions: Vec::new(),
            unknown_directive: UnknownDirective::Error,
            percent_define_is_symbol: false,
        }
    }
}

impl Options {
    /// 实际生效的指令前缀集合。
    pub fn prefixes(&self) -> String {
        match self.syntax {
            Syntax::Auto => "#%".to_string(),
            Syntax::Hash => "#".to_string(),
            Syntax::Percent => "%".to_string(),
            Syntax::Custom => self.directive_prefixes.clone(),
        }
    }
}

/// 诊断级别。
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum Severity {
    /// 错误：调用方应当视为预处理失败。
    #[default]
    Error,
    /// 警告：不阻断流程。
    Warning,
    /// 说明：用来补充上下文（例如包含链）。
    Note,
}

/// 一条诊断。
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Diagnostic {
    /// 级别。
    pub severity: Severity,
    /// 稳定的诊断短码，便于测试与抑制（例如 `include-not-found`）。
    pub code: String,
    /// 出问题的文件（`run_text` 时是调用方给的标签）。
    pub file: String,
    /// 行号（1 基；0 表示与具体行无关）。
    pub line: usize,
    /// 列号（1 基；0 表示与具体列无关）。
    pub column: usize,
    /// 给人看的消息。
    pub message: String,
}

impl Default for Diagnostic {
    fn default() -> Self {
        Diagnostic {
            severity: Severity::Error,
            code: String::new(),
            file: String::new(),
            line: 0,
            column: 0,
            message: String::new(),
        }
    }
}

impl Diagnostic {
    /// 拼成 `文件:行:列: 级别: 消息 [短码]` 的形式（C++ 版的 `toString()`）。
    pub fn to_text(&self) -> String {
        let mut text = if self.file.is_empty() {
            "<memory>".to_string()
        } else {
            self.file.clone()
        };
        text.push(':');
        text.push_str(&self.line.to_string());
        if self.column > 0 {
            text.push(':');
            text.push_str(&self.column.to_string());
        }
        text.push_str(": ");
        text.push_str(text::severity_text(self.severity));
        text.push_str(": ");
        text.push_str(&self.message);
        if !self.code.is_empty() {
            text.push_str(" [");
            text.push_str(&self.code);
            text.push(']');
        }
        text
    }
}

impl fmt::Display for Diagnostic {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(&self.to_text())
    }
}

/// 输出里某一行的来源。
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct LineOrigin {
    /// 来源文件（绝对路径或 `run_text` 的标签）。
    pub file: String,
    /// 来源行号（1 基）。
    pub line: usize,
}

/// 预处理结果。
///
/// > 名字与 C++ 版的 `preprocess::Result` 一致；**与 `std::result::Result` 同名**，
/// > 一旦 `use` 进来，裸 `Result<(), E>` 就指向这个结构体了。
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct Result {
    /// 处理后的正文。
    pub text: String,

    /// 行号映射：`line_map[i - 1]` 是输出第 i 行的来源。
    ///
    /// 空表示「没插过 `#line` 标记，也没有 include」——此时行号与主文件一致。
    pub line_map: Vec<LineOrigin>,

    /// 按首次出现顺序排列的依赖文件（含主文件，绝对路径）。
    pub dependencies: Vec<String>,

    /// 诊断列表（按发现顺序）。
    pub diagnostics: Vec<Diagnostic>,

    /// 读入的总行数（含所有被展开的文件）。
    pub lines_in: usize,

    /// 写出的总行数。
    pub lines_out: usize,

    /// 展开过的 `include` 次数（被去重跳过的也算，便于观察）。
    pub include_count: usize,
}

impl Result {
    /// 是否没有错误级诊断。
    pub fn ok(&self) -> bool {
        self.count(Severity::Error) == 0
    }

    /// 指定级别的诊断条数。
    pub fn count(&self, severity: Severity) -> usize {
        self.diagnostics
            .iter()
            .filter(|diagnostic| diagnostic.severity == severity)
            .count()
    }
}

/// 预处理器。
///
/// 一个实例可以反复用；每次 `run_file` / `run_text` 都是独立的一次处理，
/// 不共享符号表或包含状态。
#[derive(Default)]
pub struct Preprocessor {
    options: Options,
}

impl Preprocessor {
    /// 构造预处理器。
    pub fn new(options: Options) -> Self {
        Preprocessor { options }
    }

    /// 看一眼当前的选项。
    pub fn options(&self) -> &Options {
        &self.options
    }

    /// 处理一个文件。
    ///
    /// `out` 在成功与失败时都会填诊断；失败时正文可能不完整。
    /// 返回“没有错误诊断”。
    pub fn run_file(&self, path: &str, out: &mut Result) -> bool {
        engine::Engine::new(&self.options, out).run_file(path)
    }

    /// 处理一段文本。
    ///
    /// * `text` — 文本内容。
    /// * `label` — 诊断里显示的文件名（例如 `<memory>`）。
    /// * `base_dir` — `%include` 相对路径的基准目录。
    pub fn run_text(&self, text: &str, label: &str, base_dir: &str, out: &mut Result) -> bool {
        engine::Engine::new(&self.options, out).run_text(text, label, base_dir)
    }

    /// 求值一个条件表达式。
    ///
    /// 语法（优先级从低到高）：
    ///
    /// ```text
    /// 或  := 与 ('||' 与)*
    /// 与  := 非 ('&&' 非)*
    /// 非  := '!' 非 | 比较
    /// 比较 := 原子 (('==' | '!=') 字面量)?
    /// 原子 := NAME | 'defined' '(' NAME ')' | '(' 或 ')'
    /// ```
    ///
    /// 未定义的符号为假；定义了但没值的符号为真；有值的按
    /// `0/false/no/off`（大小写不敏感）与空串为假、其余为真。
    ///
    /// 成功返回 Ok(值)；失败返回 Err(诊断)，诊断的 `column` 相对表达式原文。
    pub fn evaluate(
        &self,
        expression: &str,
        symbols: &HashMap<String, String>,
    ) -> std::result::Result<bool, Diagnostic> {
        match expr::ExprParser::new(expression, |name| symbols.get(name).cloned()).run() {
            Ok(value) => Ok(value),
            Err(error) => Err(Diagnostic {
                severity: Severity::Error,
                code: "bad-expression".to_string(),
                column: error.column,
                message: error.message,
                ..Diagnostic::default()
            }),
        }
    }

    /// 符号名是否合法（`[A-Za-z_][A-Za-z0-9_]*`）。
    pub fn is_valid_name(name: &str) -> bool {
        let bytes = name.as_bytes();
        if bytes.is_empty() || !text::is_name_start(bytes[0]) {
            return false;
        }
        bytes[1..].iter().all(|&ch| text::is_name_part(ch))
    }

    /// 把 `NAME` / `NAME=VALUE` 原文拆成一个定义。
    ///
    /// 失败时返回带 `bad-name` 短码的诊断。
    pub fn parse_define(
        argument: &str,
        out: &mut Define,
    ) -> std::result::Result<(), Diagnostic> {
        let trimmed = text::trim(argument);
        let equal = trimmed.find('=');
        let space = trimmed.find([' ', '\t']);

        // 1. 两种写法都认：
        //      NAME=VALUE      （命令行 -D / #define 的等号写法）
        //      NAME VALUE…     （%symbol NAME VALUE 的空格写法）
        let (name, value) = match (equal, space) {
            (Some(equal), space) if space.is_none() || equal < space.unwrap() => (
                text::trim(&trimmed[..equal]).to_string(),
                text::trim(&trimmed[equal + 1..]).to_string(),
            ),
            _ => {
                let split = space.unwrap_or(trimmed.len());
                (
                    text::trim(&trimmed[..split]).to_string(),
                    text::trim(&trimmed[split..]).to_string(),
                )
            }
        };

        if !Preprocessor::is_valid_name(&name) {
            return Err(Diagnostic {
                severity: Severity::Error,
                code: "bad-name".to_string(),
                message: if name.is_empty() {
                    "这里要写一个符号名，例如 %symbol DEBUG".to_string()
                } else {
                    format!("符号名不合法：{name}")
                },
                ..Diagnostic::default()
            });
        }

        out.name = name;
        out.value = value;
        Ok(())
    }

    /// 把 `NAME=VALUE` 的 `VALUE` 解释成真假。
    pub fn parse_truth_value(value: &str) -> bool {
        let lowered = text::to_lower(text::trim(value));
        if lowered.is_empty() {
            return false;
        }
        !matches!(lowered.as_str(), "0" | "false" | "no" | "off")
    }

    /// 处理 `.gram` 文本的推荐选项。
    ///
    /// 指令前缀只留 `%`（`.gram` 的语义动作里可能出现 `#`）；`%token` /
    /// `%define` 等不属于预处理器的指令原样保留；顺带带上 `.gram` 的三处不透明区域。
    pub fn for_grammar() -> Options {
        // `.gram` 的语义动作里可能出现 `#`，所以只认 `%` 前缀。
        Options {
            syntax: Syntax::Percent,
            unknown_directive: UnknownDirective::Keep,
            percent_define_is_symbol: false,
            // `.gram` 里这两处的正文不解析（正文里可能出现 `%if` 之类的字面量）。
            opaque_regions: vec![
                OpaqueRegion::new("%code", "%endcode", true),
                OpaqueRegion::new("%class_body", "%endbody", true),
            ],
            ..Options::default()
        }
    }

    /// 处理 C / C++ 文本的推荐选项。
    ///
    /// 只认 `#` 前缀，注释按 `//` 与 `/* … */`，不认识的指令按错误处理。
    pub fn for_c() -> Options {
        Options {
            syntax: Syntax::Hash,
            line_comments: vec!["//".to_string()],
            block_comments: vec![("/*".to_string(), "*/".to_string())],
            unknown_directive: UnknownDirective::Error,
            ..Options::default()
        }
    }

    /// 处理 metro_editor 那类 `%define` / `%if` / `%include` 文本的推荐选项。
    ///
    /// 只认 `%` 前缀，并把 `%define` / `%undefine` 也当作符号定义。
    pub fn for_metro() -> Options {
        Options {
            syntax: Syntax::Percent,
            percent_define_is_symbol: true,
            line_comments: vec!["//".to_string()],
            block_comments: vec![("/*".to_string(), "*/".to_string())],
            unknown_directive: UnknownDirective::Error,
            ..Options::default()
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn prefixes_follow_syntax() {
        let mut options = Options::default();
        assert_eq!(options.prefixes(), "#%");
        options.syntax = Syntax::Hash;
        assert_eq!(options.prefixes(), "#");
        options.syntax = Syntax::Percent;
        assert_eq!(options.prefixes(), "%");
        options.syntax = Syntax::Custom;
        options.directive_prefixes = "@".to_string();
        assert_eq!(options.prefixes(), "@");
    }

    #[test]
    fn valid_names() {
        assert!(Preprocessor::is_valid_name("DEBUG"));
        assert!(Preprocessor::is_valid_name("_x1"));
        assert!(!Preprocessor::is_valid_name(""));
        assert!(!Preprocessor::is_valid_name("1x"));
        assert!(!Preprocessor::is_valid_name("a-b"));
    }

    #[test]
    fn defines_parse_both_ways() {
        let mut define = Define::default();
        assert!(Preprocessor::parse_define("NAME=value", &mut define).is_ok());
        assert_eq!(define.name, "NAME");
        assert_eq!(define.value, "value");

        assert!(Preprocessor::parse_define("ONLY", &mut define).is_ok());
        assert_eq!(define.name, "ONLY");
        assert!(define.value.is_empty());

        assert!(Preprocessor::parse_define("LEVEL 2", &mut define).is_ok());
        assert_eq!(define.value, "2");

        let error = Preprocessor::parse_define("=x", &mut define).unwrap_err();
        assert_eq!(error.code, "bad-name");
    }

    #[test]
    fn truth_values() {
        for falsy in ["", "0", "off", "False", "no", " 0 "] {
            assert!(!Preprocessor::parse_truth_value(falsy), "{falsy}");
        }
        for truthy in ["1", "yes", "ON", "value"] {
            assert!(Preprocessor::parse_truth_value(truthy), "{truthy}");
        }
    }

    #[test]
    fn expressions_through_public_api() {
        let preprocessor = Preprocessor::default();
        let symbols: HashMap<String, String> = [
            ("A".to_string(), String::new()),
            ("B".to_string(), "0".to_string()),
            ("C".to_string(), "hello".to_string()),
        ]
        .into_iter()
        .collect();

        assert_eq!(preprocessor.evaluate("A", &symbols), Ok(true));
        assert_eq!(preprocessor.evaluate("B", &symbols), Ok(false));
        assert_eq!(preprocessor.evaluate("!B", &symbols), Ok(true));
        assert_eq!(preprocessor.evaluate("A && !B", &symbols), Ok(true));
        assert_eq!(preprocessor.evaluate("B || (A && C == hello)", &symbols), Ok(true));
        assert_eq!(
            preprocessor.evaluate("defined(A) && !defined(NOPE)", &symbols),
            Ok(true)
        );
        assert_eq!(preprocessor.evaluate("C != hello", &symbols), Ok(false));

        // 表达式不完整：只断言短码（列号在下面那条用例里单独盯）。
        let error = preprocessor.evaluate("A &&", &symbols).unwrap_err();
        assert_eq!(error.code, "bad-expression");
        assert_eq!(error.message, "条件表达式不完整");
        assert_eq!(error.column, 5);
    }

    #[test]
    fn text_identity_and_line_map() {
        let preprocessor = Preprocessor::default();
        let mut result = Result::default();
        assert!(preprocessor.run_text("a\nb\n\nc\n", "<t>", "", &mut result));
        assert_eq!(result.text, "a\nb\n\nc\n");
        assert!(result.diagnostics.is_empty());
        assert_eq!(result.lines_in, 4);
        assert_eq!(result.lines_out, 4);
    }

    #[test]
    fn diagnostic_display() {
        let diagnostic = Diagnostic {
            severity: Severity::Warning,
            code: "unknown-pragma".to_string(),
            file: "a.pp".to_string(),
            line: 3,
            column: 2,
            message: "忽略了".to_string(),
        };
        assert_eq!(
            diagnostic.to_text(),
            "a.pp:3:2: warning: 忽略了 [unknown-pragma]"
        );
    }
}
