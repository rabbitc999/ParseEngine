# 预处理器（Rust 版，`library/preprocess-rust`）

> **这是一个独立的 Rust crate：`preprocess`。** [`library/preprocess`](../preprocess/README.md)
> （C++17）的**逐字节对齐移植**：把「源码进编译器之前」的那一步独立出来——处理 `%` / `#`
> 开头的预处理指令：符号定义、条件编译、文件包含，**默认用空行占位保证行号不变**，
> 出错一路收诊断。
>
> **只用 Rust 标准库，没有任何第三方依赖**（`Cargo.lock` 里只有本 crate 自己）。
> 指令语义、诊断短码、默认选项、输出规则与 C++ 版完全一致——**56 组命令行参数的
> 实测对照全部相同**（见下面「与 C++ 版的关系」）。
>
> 内部设计与实现细节见 [技术报告.md](技术报告.md)。

```text
文本 → 逐行扫描（注释 / 字符串 / 不透明区域）→ 指令分类
     → 符号表 + 条件栈 → include / import 递归展开（去重 / 循环 / 深度保护）
     → 输出（空行占位）+ 行号映射 + 诊断
```

## 快速开始

```sh
cd library/preprocess-rust

cargo test                       # 自检：16 个 #[test]（102 处断言）+ 7 个单元测试 + 1 个文档示例
cargo run -- examples/main.pp -D DEBUG --stats
cargo build                      # 产物按 .cargo/config.toml 落到仓库 build/preprocess/rust/
cargo clippy --all-targets       # 无提示
```

从仓库根跑也一样（cargo 读不到 crate 目录下的 `.cargo/config.toml`，产物落在
`library/preprocess-rust/target/`，两处都在 `.gitignore` 里）：

```sh
cargo test --manifest-path library/preprocess-rust/Cargo.toml
```

`cargo run`（= `preprocess-cli`）读 `examples/main.pp`，输出与
`examples/expected-debug.txt` 逐字节相同。

## 指令

指令要求**单独一行、行首**（允许缩进）。前缀默认同时认 `%` 与 `#`：

| 指令 | 作用 |
| --- | --- |
| `%symbol NAME [VALUE]` / `%unsymbol NAME` | 定义 / 取消符号（中性写法，VALUE 用空格分隔） |
| `#define NAME [VALUE]` / `#undef NAME` | 同上，C 风格（始终可用；`--metro` 时 `%define` / `%undefine` 也可用） |
| `if EXPR` / `elif EXPR` / `else` / `endif` | 条件编译；不活跃的分支整块丢掉 |
| `ifdef NAME` / `ifndef NAME` | 等价 `if defined(NAME)` / `if !defined(NAME)` |
| `include "路径"` | 把文件内容原地展开（相对当前文件目录 → `include_dirs` → 原样） |
| `include_once "路径"` / `import "路径"` | 同上，但按“只展开一次”处理 |
| `once` / `pragma once` | 把当前文件登记成“只展开一次” |
| `error "消息"` / `warning "消息"` | 活跃分支里报一条错误 / 警告 |

指令行**末尾的注释会被忽略**（`%include "a.inc"   // 说明` / `/* 说明 */` 都行）——注释不属于
参数，引号里的注释定界符当普通字符；注释夹在表达式中间的写法不支持。

包含（include / import）**同一个文件默认只展开一次**，并且带循环检测与最大深度
（`Options::max_include_depth`，默认 32）。不认识的指令默认报错，也可以用
`UnknownDirective::Keep` 原样保留（`.gram` 就用这个策略）。

## 条件表达式

优先级从低到高：

```text
或   := 与 ('||' 与)*
与   := 非 ('&&' 非)*
非   := '!' 非 | 比较
比较 := 原子 (('==' | '!=') 字面量)?
原子 := NAME | 'defined' '(' NAME ')' | '(' 或 ')'
```

未定义的符号为假；定义了但没值的符号为真；有值的按 `0 / false / no / off`
（大小写不敏感）与空串为假，其余为真。`NAME == 值` 比较符号的取值，值可以带引号。

```text
%if DEBUG && !QUIET
%if LEVEL == 2
%if defined(FEATURE) || defined(FALLBACK)
```

## 行号

默认「**丢掉的行用空行占位**」（`Options::preserve_lines = true`）：指令行、条件为假的
行都会输出一个空行，于是输出的行号与输入一一对应，上层按原文件行号报错不用换算。
想紧凑输出就关掉它（CLI 的 `--drop-lines`）。

`%include` 会把被包含文件的行插进来，行数必然变化，所以还有两条路：

- `Result::line_map`：输出第 i 行的来源（文件 + 行号），空行占位也记来源；
- `Options::emit_line_markers`：换文件时插入 `#line 行号 "文件"`，直接喂给 C 编译器。

## 包含加载器（CSV、数据库、内存文本…）

`%include` / `%import` 都会先问 `Options::include_loader`。回调可以自己解析路径、
直接给正文、跳过或报错；不处理就返回 `use_default()` 交回库的默认逻辑。

```rust
use preprocess::{IncludeRequest, IncludeResult, Options};

let mut options = Options::default();
options.include_loader = Some(Box::new(|request: &IncludeRequest| {
    // 1. 只接管 %import 的 CSV，别的交回默认（读文件）
    if !request.imported || !request.path.ends_with(".csv") {
        return IncludeResult::use_default();
    }
    // 2. 自己把 CSV 转成文法能吃的文本
    let text = format!("row name={}\n", request.resolved_path);
    // 3. label 进依赖表 / 去重 / 循环检测；留空则用 resolved_path
    IncludeResult::inline_text_as(text, request.resolved_path.clone())
}));
```

| 回调返回 | 含义 |
| --- | --- |
| `use_default()` | 我不处理，走库的默认逻辑 |
| `use_file()` / `use_file_as(label)` | 路径我认了，正文仍从 `resolved_path` 读 |
| `inline_text(text)` / `inline_text_as(text, label)` | 正文我自己给（CSV / 数据库 / 内存表都行） |
| `skip()` | 这次包含什么都不做 |
| `error(message)` | 报一条 `include-loader-error` |

C++ 版的 `useFile(label = "")` / `inlineText(text, label = "")` 有默认参数，Rust 没有，
所以拆成「不带 label」与「带 label」两个构造函数——行为一一对应。

`request.imported` 区分 `%import` 与 `%include`，所以可以让 `import` 当数据源、
`include` 当代码片段。CLI 里的 `--csv-to-lines` 就是这个回调的最小实现（表头当列名，
每行数据生成 `row1 列=值 列=值`）：

```text
$ preprocess-cli demo.pp --metro --csv-to-lines
row1 name=人民广场 lon=121.47 lat=31.23
row2 name=南京东路 lon=121.49 lat=31.24
```

## 语言预设

不想逐个设选项时，用这三个预设（也可以照着改）：

| 预设 | 前缀 | 说明 |
| --- | --- | --- |
| `Preprocessor::for_c()` | `#` | C / C++ / Objective-C：`#define`、`#ifdef`、`#pragma once`、`#import` |
| `Preprocessor::for_metro()` | `%` | metro_editor 那种 `%define` / `%if` / `%include` / `%import` |
| `Preprocessor::for_grammar()` | `%` | `.gram`：`%token` 等非预处理指令原样保留，`%code … %endcode` / `%class_body … %endbody` 里不解析 |

换别的语言时按需改 `Options`：

| 选项 | 作用 |
| --- | --- |
| `syntax` / `directive_prefixes` | 指令写法：`Auto`（默认，`#` 与 `%`）/ `Hash` / `Percent` / `Custom` |
| `line_comments` / `block_comments` | 注释起始串（默认 `//` 与 `/* … */`；SQL 的 `--`、Python 的 `#` 都可以加） |
| `string_delimiters` | 字符串定界符（默认单双引号；注释与字符串里的指令不当指令） |
| `opaque_regions` | 成对的“不解析区域”（默认空，`.gram` 预设里带三处） |
| `percent_define_is_symbol` | `%define NAME [VALUE]` 是否也算符号定义（yacc / metro 风格） |
| `unknown_directive` | 不认识的指令：`Error` / `Keep` / `Ignore` |
| `case_insensitive` | 符号名是否大小写不敏感 |

## API

```rust
use preprocess::{Define, Options, Preprocessor, Result};

let mut options = Options::default();          // 或 for_c() / for_metro() / for_grammar()
options.defines.push(Define::new("DEBUG", "")); // 等价命令行 -D DEBUG
options.include_dirs.push("shared".into());     // 等价 -I shared

let preprocessor = Preprocessor::new(options);
let mut result = Result::default();
if !preprocessor.run_file("main.src", &mut result) {
    for item in &result.diagnostics {
        eprintln!("{item}");   // Display = 文件:行:列: error: 消息 [短码]
    }
}
result.text;          // 处理后的正文
result.line_map;      // 输出第 i 行的来源（文件 + 行号）
result.dependencies;  // 依赖文件（含主文件，按首次出现排序）
result.lines_in;      // 还有 lines_out / include_count
result.ok();          // 没有错误诊断
result.count(Severity::Error);
```

`run_text(text, label, base_dir, &mut result)` 用来处理内存里的文本（编辑器场景很方便），
`evaluate()` / `parse_define()` / `parse_truth_value()` / `is_valid_name()` 是给上层复用的
关联函数。

诊断短码：`unknown-directive`、`bad-argument`、`bad-name`、`bad-expression`、`bad-path`、
`include-not-found`、`include-cycle`、`include-depth`、`include-loader-error`、
`unterminated-if`、`unmatched-elif`、`unmatched-else`、`unmatched-endif`、
`duplicate-else`、`elif-after-else`、`error-directive`、`warning-directive`、
`unknown-pragma`、`open-failed`。

> **命名提醒**：结果类型故意叫 `Result`，与 C++ 版的 `preprocess::Result` 同名。
> 代价是一旦 `use preprocess::Result;`，本文件里的裸 `Result<(), E>` 就指向它了——
> 需要标准库那个时写 `std::result::Result`。这是**唯一**为了「两边对得上号」而保留的
> 非惯用命名。

## 命令行

```text
preprocess-cli <输入文件> [选项]

  -o, --out FILE         输出文件（缺省 stdout）
  -D, --define NAME[=V]  预定义符号（可重复）    -U, --undef NAME  取消预定义
  -I, --include-dir DIR  额外搜索目录（可重复）  --no-include-once 允许重复展开
      --keep-directives  指令行原样保留          --drop-lines      被丢掉的行直接删（不补空行）
      --line-markers     输出 #line 标记         --max-depth N     最大嵌套深度
      --hash/--percent/--metro/--grammar        语言预设
      --syntax MODE      auto / hash / percent / custom
      --prefix CHARS     custom 模式下的前缀集合
      --unknown MODE     error / keep / ignore
      --csv-to-lines     演示自定义 loader（%import 的 CSV 转成文本行）
      --check            只检查不输出            --deps            只列依赖文件
      --stats            把行数 / include 次数 / 依赖数写到 stderr
```

退出码：`0` 成功，`1` 有错误诊断，`2` 用法错误。诊断一律走 stderr，所以
`preprocess-cli a.pp > a.out` 拿到的只有正文。

**与 C++ 版有意不同的两处**（实测，见下节）：

| 项 | C++ 版 | Rust 版 |
| --- | --- | --- |
| `--max-depth abc` | `std::stoul` 抛异常没人接 → 直接崩（退出码 3） | 当成用法错误：`--max-depth 只接受非负整数`，退出码 2 |
| 写 stdout 的换行 | Windows 上 `std::cout` 是文本模式 → `CRLF` | Rust 不翻译换行 → `LF` |

**要跟 C++ 版逐字节比就用 `-o`**（两边都是二进制写，输出完全一致）。`--version` 打的是
`preprocess-rust 1.0（ParseEngine library/preprocess-rust）`，这是版本串本身的差别。

## 构建与自检

```sh
cd library/preprocess-rust
cargo test --all-targets         # 16 个 #[test]（102 处断言）+ 7 个单元测试 + 1 个文档示例
cargo clippy --all-targets       # 无提示
cargo build --release            # 干净构建约 1.5 秒，产物 245 KB
```

`tests/preprocess.rs` 的用例与断言条数**与 C++ 版 `test_preprocess.cpp` 一一对应（102 处）**：
条件编译、表达式、符号、包含去重与循环、注释与字符串里的伪指令、`#pragma once`、
自定义 loader（CSV）、行号映射与 `#line` 标记、指令行末尾的注释；最后一条
`example_fixture` 把 `examples/main.pp` 的输出与 `examples/expected-debug.txt` 逐字节对照。

## 目录

```text
library/preprocess-rust/
├── src/lib.rs        公开 API（Define / IncludeRequest / IncludeResult / OpaqueRegion /
│                     Options / Diagnostic / LineOrigin / Result / Preprocessor）
├── src/text.rs       字符串与路径工具（按字节处理，与 C++ 版同一套语义）
├── src/expr.rs       条件表达式的递归下降求值器
├── src/engine.rs     一次预处理的实现体（行扫描 / 条件栈 / include 展开 / 行号映射 / 诊断）
├── src/main.rs       命令行工具（preprocess-cli）
├── tests/preprocess.rs  自检（16 个 #[test]，102 处断言）
├── examples/         示例：main.pp 包含 common.inc 与 debug.inc（+ expected-debug.txt）
├── Cargo.toml / .cargo/config.toml
├── 技术报告.md
└── README.md
```

## 与 C++ 版的关系

**两边是同一套行为的两份实现**，不是各自演化：

| | C++（`library/preprocess`） | Rust（`library/preprocess-rust`） |
| --- | --- | --- |
| 库名 | `libpreprocess.a`，命名空间 `preprocess` | crate/lib `preprocess`，`use preprocess::…` |
| 命令行 | `build/preprocess/preprocess` | `preprocess-cli` |
| 自检 | `preprocess-tests`（102 处断言） | `tests/preprocess.rs`（16 个 `#[test]`，102 处断言） |
| 构建 | `make -C library/preprocess` | `cargo build`（产物落 `build/preprocess/rust/`） |
| 依赖 | C++17 标准库 | Rust 标准库 |

验收办法（可重复）：`bash tmp/pp_rust_parity.sh` 在**同一批输入、同一批参数**下分别跑两个
可执行文件，比较 `-o` 产物（逐字节）、stdout、stderr 与退出码。当前结果：

```text
对照结果：56 组一致，0 组不一致
```

覆盖：两份 `examples/`、条件与表达式（含 `%elif` 链、`defined()`、带引号比较、表达式报错）、
`--grammar`（不透明区域与 `%codex` 边界）、`--metro` / `--hash` / `--syntax custom --prefix`、
大小写敏感开关、未知指令三种策略、中文与中文符号名、诊断 7 类、循环包含、深度限制、
`%once` / `#pragma once` 去重、绝对路径包含、CSV 自定义 loader、CRLF + BOM、无结尾换行、
空文件、`-U` 与多种 `-D` 写法，以及 `--deps` / `--stats` / `--help` 与 6 种用法错误。

`ParseEngine` 主程序里本来就有一段条件编译（`src/preprocessor.cpp`，只处理 `%if` 系列
与 `--define`，并且知道 `.gram` 的 `%code ... %endcode` 段落）。这个 crate 是**独立、通用**的
那一份（与 C++ 版对齐）：多了 `include` / `import`、符号定义、自定义加载器、行号映射
与 `#line`，供编辑器、构建脚本或别的 DSL 使用。生成器主程序不链接本 crate，本 crate
也不依赖主程序。
