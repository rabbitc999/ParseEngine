//! 文本模板引擎（`library/template` 的 Rust 版）。
//!
//! 支持 `{{ 变量 }}` / `{% if %}` / `{% for %}` / `{% include %}` / `{% raw %}` /
//! `{# 注释 #}`，**只用 Rust 标准库**，没有任何第三方依赖，也不依赖仓库里别的 crate。
//!
//! ```
//! use template_engine::{encode_object, render, Encode, Value};
//!
//! struct Method {
//!     ret: String,
//!     name: String,
//! }
//!
//! impl Encode for Method {
//!     fn encode(&self) -> Value {
//!         encode_object! { "ret" => &self.ret, "name" => &self.name }
//!     }
//! }
//!
//! let method = Method { ret: "int".into(), name: "add".into() };
//! let source = "{{ ret }} {{ name }}();";
//! assert_eq!(render(source, &method).unwrap(), "int add();");
//! ```
//!
//! 与 C++ 版（`library/template`）的对照：
//!
//! | C++ 版 | 本 crate |
//! | --- | --- |
//! | `tmpl::Value` / `Object` / `Array` | [`Value`] / [`Object`] / [`Array`] |
//! | `tmpl::Model` 虚接口 + `ModelAdapter` | [`Model`] trait + [`Encode`] trait（`EncodedModel` 做适配） |
//! | `TEMPLATE_MODEL(T, tmpl::field(...))` | [`Encode`] 实现 + `encode_object!` / `template_model!` |
//! | `tmpl::render(source, model)` | [`render`]（带片段表是 [`render_with`]） |
//! | `tmpl::renderValues(source, {...})` | [`render_values`]（带片段表是 [`render_values_with`]） |
//! | 抛 `std::runtime_error` | 返回 `Result<String, `[`TemplateError`]`>` |
//!
//! 设计要点：
//!
//!   * **无状态**：[`TemplateEngine`] 全是关联函数，渲染不落任何全局状态，同一份模板
//!     可以并发渲染（[`Value`] 内部用 `Arc`，`Send + Sync`）；
//!   * **引擎不读文件**：模板是调用方给的字符串，`include` 的片段表也是调用方给的
//!     （[`SnippetTable`]，名字 → 片段正文）；
//!   * **不 panic**：缺变量、空列表、缺片段、不认识的指令一律安全跳过（输出空串），
//!     只有"标签没闭合"和"include 嵌套过深"返回错误。

#![forbid(unsafe_code)]

mod bind;
mod engine;
mod macros;
mod model;
mod naming;
mod value;

pub use bind::{encode_into, Encode};
pub use engine::{
    no_snippets, render, render_values, render_values_with, render_with, snippet_table,
    ChildModel, SnippetTable, TemplateEngine, TemplateError,
};
pub use model::{resolve, EncodedModel, MapModel, Model, ValueModel};
pub use naming::{to_camel_case, to_snake_case};
pub use value::{Array, Object, Value};
