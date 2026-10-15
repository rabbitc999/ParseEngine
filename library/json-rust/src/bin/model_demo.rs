//! `json-model-demo`：演示「JSON ↔ 模型类」的投射（对应 C++ 版 `examples/model_demo.cpp`）。
//!
//! ```sh
//! cargo run --manifest-path library/json-rust/Cargo.toml --bin json-model-demo
//! ```
//!
//! 链路是：读 `library/json/examples/config.json`（**和 C++ 版共用同一份示例数据**）→
//! 解析成 [`Value`] → 投射成 `Config` 模型 → 打印摘要 → 再编回 JSON。
//! 参数给一个路径就换成读那个文件。

use std::collections::BTreeMap;
use std::process::ExitCode;

use json::{decode, encode, json_model, parse};

/// 一个词法规则。
#[derive(Clone, Debug, Default, PartialEq)]
struct Token {
    /// 词法名。
    name: String,
    /// 优先级。
    priority: i64,
    /// 正则（可选字段：JSON 里没有这一项也不算错）。
    pattern: Option<String>,
}
json_model!(Token, "name" => name, "priority" => priority, "pattern" => pattern);

/// 编译限制。
#[derive(Clone, Debug, Default, PartialEq)]
struct Limits {
    max_depth: i64,
    max_errors: i64,
    ratio: f64,
}
json_model!(Limits, "maxDepth" => max_depth, "maxErrors" => max_errors, "ratio" => ratio);

/// 顶层配置。
///
/// 注意字段名与 JSON 里的名字**不一样**（`max_depth` ↔ `maxDepth`）——`json_model!`
/// 第一个参数是 JSON 里的名字，第二个是 Rust 成员名，两边不必相同。
#[derive(Clone, Debug, Default, PartialEq)]
struct Config {
    name: String,
    version: i64,
    debug: bool,
    targets: Vec<String>,
    limits: Limits,
    tokens: Vec<Token>,
    labels: BTreeMap<String, String>,
}
json_model!(
    Config,
    "name" => name,
    "version" => version,
    "debug" => debug,
    "targets" => targets,
    "limits" => limits,
    "tokens" => tokens,
    "labels" => labels
);

/// 程序入口。
fn main() -> ExitCode {
    // 默认示例：C++ 版放在 library/json/examples/config.json，两版共用一份。
    let default_path = std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
        .join("../json/examples/config.json");
    let path = std::env::args()
        .nth(1)
        .map(std::path::PathBuf::from)
        .unwrap_or(default_path);

    // 1. 文本 → JSON。
    let Ok(bytes) = std::fs::read(&path) else {
        eprintln!("打不开文件：{}", path.display());
        return ExitCode::from(1);
    };
    let Ok(text) = String::from_utf8(bytes) else {
        eprintln!("输入不是合法的 UTF-8：{}", path.display());
        return ExitCode::from(1);
    };
    let doc = parse(&text);
    let Some(root) = doc.value else {
        eprintln!("解析失败：{}", doc.diagnostics[0]);
        return ExitCode::from(1);
    };

    // 2. JSON → 模型。
    let config = match decode::<Config>(&root).require() {
        Ok(config) => config,
        Err(diagnostics) => {
            eprintln!("投射失败：");
            for diagnostic in &diagnostics {
                eprintln!("  {diagnostic}");
            }
            return ExitCode::from(1);
        }
    };

    println!("配置：{} v{}", config.name, config.version);
    println!(
        "目标：{} 个，限制 maxDepth={}",
        config.targets.len(),
        config.limits.max_depth
    );
    let top_priority = config.tokens.iter().map(|token| token.priority).max().unwrap_or(0);
    println!("词法：{} 条，最高优先级={}", config.tokens.len(), top_priority);
    for (key, text) in &config.labels {
        println!("标签 {key} = {text}");
    }

    // 3. 模型 → JSON（紧凑输出，方便对比）。
    println!("\n重新编码：{}", encode(&config).dump());
    ExitCode::SUCCESS
}
