//! JSON 库（`library/json` 的 Rust 版）。
//!
//! 解析、序列化、**数组与对象都能直接用 `for` 与迭代器适配器遍历**，外加一层
//! 「JSON ↔ 模型类」的投射。**只用 Rust 标准库**，没有任何第三方依赖，也不依赖仓库里
//! 别的 crate；不参与 ParseEngine 主程序的构建。
//!
//! ```text
//! 文本 ──parse──▶ Value（null/bool/int/double/string/array/object）
//!                   │
//!                   ├──dump──▶ 文本（紧凑 / 缩进）
//!                   ├──at_path("a.b[0]")──▶ 取值
//!                   └──decode::<T> / encode(&t)──▶ 模型类（json_model!）
//! ```
//!
//! 与 C++ 版（`library/json`）的对照：
//!
//! | C++ 版 | 本 crate |
//! | --- | --- |
//! | `json::Value`（`std::variant`）| [`Value`]（`enum`） |
//! | `json::Array` / `json::Object` / `json::Member` | [`Array`] / [`Object`] / [`Member`] |
//! | `json::parse(text, options)` | [`parse`] / [`parse_with`] |
//! | `json::ParseResult` / `json::Diagnostic` | [`ParseResult`] / [`Diagnostic`] |
//! | `Value::dump(indent)` | [`Value::dump`]（紧凑）/ [`Value::dump_indent`] |
//! | `Value::atPath` / `operator[]` / `find` | [`Value::at_path`] / [`Value::get_or_insert`] / [`Value::get`] |
//! | `Value::Reference` 与手写迭代器 | [`Entry`] / [`EntryMut`]（[`Value::entries`]） |
//! | 抛 `std::domain_error` / `std::out_of_range` | [`Error`]（[`Value::require_array`] / [`Value::at`]） |
//! | `JSON_MODEL` + 成员指针表 | [`JsonModel`] + [`json_model!`]（trait 分派，不需要成员指针） |
//! | `json::encode` / `json::decode<T>` | [`encode`] / [`decode`] / [`decode_with`] |
//! | `json::DecodeResult<T>` | [`DecodeResult`] |
//! | 命令行 `json`（`main.cpp`） | `json-cli`（`src/main.rs`，命令与选项相同） |
//!
//! 设计要点：
//!
//!   * **对象保序**：[`Object`] 是「插入顺序的成员表」，不是哈希表——解析顺序、
//!     编码顺序、`for` 顺序都是同一份顺序；
//!   * **容器就是容器**：[`Array`] 就是 `Vec<Value>`，[`Object`] 能 `Deref` 成
//!     `[Member]`，所以迭代器适配器（`iter` / `filter` / `map` / `sum`）直接可用；
//!   * **不 panic**：取值给 `Option`，类型不对给 `Err`，库内没有 `unwrap`；
//!   * **诊断带位置**：解析错误给行列与字节偏移（短码与 C++ 版逐字一致），
//!     投射错误给 JSON 路径（`points[1].x`）。
//!
//! ```
//! use json::{decode, encode, json_model, parse};
//!
//! // 1. 文本 → 值：可以直接 for，也可以按路径取。
//! let doc = parse(r#"{"name":"calc","tokens":["+","-"],"retry":2}"#);
//! assert!(doc.ok());
//! let root = doc.value.as_ref().unwrap();
//! assert_eq!(root.at_path("tokens[1]").unwrap().string_or("?"), "-");
//!
//! let tokens: Vec<&str> = root
//!     .get("tokens")
//!     .unwrap()
//!     .entries()
//!     .filter_map(|entry| entry.value.as_str())
//!     .collect();
//! assert_eq!(tokens, ["+", "-"]);
//!
//! // 2. 值 ↔ 模型类。
//! #[derive(Clone, Debug, Default, PartialEq)]
//! struct Config {
//!     name: String,
//!     tokens: Vec<String>,
//!     retry: i64,
//!     pattern: Option<String>, // 可选字段：缺席不算错
//! }
//! json_model!(Config, "name" => name, "tokens" => tokens, "retry" => retry, "pattern" => pattern);
//!
//! let config = decode::<Config>(root).require().unwrap();
//! assert_eq!(config.tokens, ["+", "-"]);
//! assert_eq!(config.pattern, None);
//! assert_eq!(encode(&config).dump(),
//!            r#"{"name":"calc","tokens":["+","-"],"retry":2,"pattern":null}"#);
//! ```

#![forbid(unsafe_code)]

mod bind;
mod macros;
mod parse;
mod text;
mod value;

pub use bind::{
    decode, decode_with, encode, encode_field, Context, DecodeOptions, DecodeResult, FromJson,
    JsonModel, ToJson,
};
pub use parse::{parse, parse_with, Diagnostic, ParseOptions, ParseResult};
pub use value::{
    Array, Entries, EntriesMut, Entry, EntryMut, Error, Member, Object, Value,
};
