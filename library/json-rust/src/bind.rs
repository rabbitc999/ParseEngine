//! JSON ↔ 模型类的投射：`encode` / `decode`（对应 `library/json/bind.hpp`）。
//!
//! C++ 没有内置反射（静态反射要到 C++26 才进标准），所以"模型类 ↔ JSON"必须由某一方
//! 声明字段：C++ 版选的是**成员指针表**（`JSON_MODEL(T, json::field("x", &T::x))`）。
//! Rust 的选择更省事——**用 trait**：模型实现 [`JsonModel`]，字段名与成员就在
//! `impl` 里明说，取值 / 赋值靠 trait 分派，不需要成员指针，也不需要 `if constexpr`。
//!
//! | C++ 版 | 本模块 |
//! | --- | --- |
//! | `JSON_MODEL(T, json::field("name", &T::name), …)` | [`json_model!`](crate::json_model)`(T, "name" => name, …)`（生成 [`JsonModel`] + [`ToJson`] + [`FromJson`]） |
//! | `json::encode(model)` | [`encode`]（或直接 `model.to_json()`） |
//! | `json::decode<T>(value)` / `decode<T>(value, options)` | [`decode`] / [`decode_with`] |
//! | `json::hasBinding<T>::value` | 用 trait 约束代替：没实现 [`JsonModel`] 的模型**编译不过** |
//! | `std::optional<T>` 字段 | `Option<T>` 字段（`null` ↔ 空，**字段缺席也不算错**） |
//! | `std::vector<T>` / `std::map<string,T>` 字段 | `Vec<T>` / `BTreeMap<String,T>`（或 `HashMap`）字段 |
//! | `enum` 按底层整数（`std::is_enum`） | [`json_enum!`](crate::json_enum)（Rust 不能对"所有枚举"做统一分派，得点一下名） |
//! | `json::DecodeResult<T>::require()` 抛 `std::runtime_error` | [`DecodeResult::require`] 返回 `Result<T, Vec<Diagnostic>>` |
//! | 诊断带 JSON 路径（`points[1].x`） | 同样：路径由 [`Context`] 维护，逐段 push / restore |
//!
//! ```
//! use json::{decode, encode, json_enum, json_model, FromJson, JsonModel, ToJson, Value};
//!
//! #[derive(Clone, Debug, Default, PartialEq)]
//! struct Point {
//!     x: i64,
//!     y: i64,
//! }
//! json_model!(Point, "x" => x, "y" => y);
//!
//! #[derive(Clone, Debug, Default, PartialEq)]
//! struct Shape {
//!     id: String,
//!     origin: Point,
//!     tags: Vec<String>,
//!     scale: Option<f64>,
//! }
//! json_model!(Shape, "id" => id, "origin" => origin, "tags" => tags, "scale" => scale);
//!
//! let shape = Shape {
//!     id: "s1".into(),
//!     origin: Point { x: 1, y: 2 },
//!     tags: vec!["a".into()],
//!     scale: Some(2.5),
//! };
//! let text = encode(&shape);
//! assert_eq!(text.dump(), r#"{"id":"s1","origin":{"x":1,"y":2},"tags":["a"],"scale":2.5}"#);
//!
//! let back = decode::<Shape>(&text);
//! assert!(back.ok());
//! assert_eq!(back.value.unwrap(), shape);
//!
//! // 缺字段（可选字段豁免）+ 类型不对：诊断都带 JSON 路径。
//! let bad = decode::<Shape>(&json::parse(r#"{"id":"x","origin":{"x":1,"y":2},"tags":[],"scale":"no"}"#).value.unwrap());
//! assert!(!bad.ok());
//! assert_eq!(bad.diagnostics[0].path, "scale");
//! ```

use std::collections::{BTreeMap, HashMap};
use std::fmt;

use crate::parse::Diagnostic;
use crate::value::{Object, Value};

// ---------------------------------------------------------------------------
//  两个方向
// ---------------------------------------------------------------------------

/// 把一个 Rust 值编码成 JSON（对应 C++ 版 `json::encode`）。
///
/// 已备好的实现：`bool` / 各种整数 / `f32` / `f64` / `String` / `str` / [`Value`] /
/// `Option<T>` / `Vec<T>` / `BTreeMap<String,T>` / `HashMap<String,T>` / 用
/// [`json_enum!`](crate::json_enum) 点过名的枚举 / 实现了 [`JsonModel`] 的模型。
pub trait ToJson {
    /// 编码。
    fn to_json(&self) -> Value;
}

/// 从一个 JSON 值解码出 Rust 值（对应 C++ 版 `json::decode` 里那一层）。
///
/// 目标类型还必须是 `Sized`；要能**造出缺省实例**的（模型走 [`JsonModel`] 的
/// `Default`，容器与标量天然可以）。
///
/// 为什么 `Err` 里不带东西：失败细节（短码、消息、JSON 路径）都在 [`Context`] 里
/// 攒着——`Err(())` 只是"这条路走不通，别往下走了"。这样每条诊断只构造一次，
/// 也不会因为多包一层错误类型而把路径信息拆成两处。
pub trait FromJson: Sized {
    /// 解码。
    #[allow(clippy::result_unit_err)]
    fn from_json(value: &Value, ctx: &mut Context<'_>) -> Result<Self, ()>;

    /// 对象里**没有这个字段**时怎么办。
    ///
    /// 缺省行为：按 [`DecodeOptions::strict_missing`] 决定报不报 `missing-field`
    /// （C++ 版对非 `optional` 字段就是这么做的）。[`Option`] 覆盖成"置空且成功"
    /// ——所以可选字段缺席是正常的，不算错。
    fn from_missing(_out: &mut Self, ctx: &mut Context<'_>, name: &str) -> bool {
        if ctx.options.strict_missing {
            ctx.error("missing-field", format!("缺少字段 \"{name}\""));
            false
        } else {
            true
        }
    }
}

// ---------------------------------------------------------------------------
//  选项与结果
// ---------------------------------------------------------------------------

/// 解码选项（对应 C++ 的 `DecodeOptions`）。
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct DecodeOptions {
    /// 缺字段是否算错误（默认算：模型字段就是协议的一部分）。
    pub strict_missing: bool,
    /// 多出来的字段是否算错误（默认不算：方便前端多传字段）。
    pub strict_unknown: bool,
    /// 字符串形式的数字（`"12"`）是否允许喂给整数 / 浮点 / 枚举字段。
    pub allow_string_numbers: bool,
}

impl Default for DecodeOptions {
    fn default() -> Self {
        DecodeOptions { strict_missing: true, strict_unknown: false, allow_string_numbers: true }
    }
}

impl DecodeOptions {
    /// 宽松档：缺字段不报错、多出来的字段也不报错（适合"能读到多少算多少"的场景）。
    pub fn lenient() -> Self {
        DecodeOptions { strict_missing: false, strict_unknown: false, allow_string_numbers: true }
    }
}

/// 解码结果（对应 C++ 的 `DecodeResult<T>`）。
///
/// ```
/// use json::{decode, json_model};
///
/// #[derive(Default)]
/// struct Config {
///     version: i64,
/// }
/// json_model!(Config, "version" => version);
///
/// let error = decode::<Config>(&json::parse(r#"{"version":"x"}"#).value.unwrap());
/// assert!(!error.ok());
/// assert_eq!(error.diagnostics[0].path, "version");
/// ```
pub struct DecodeResult<T> {
    /// 成功时的模型。
    pub value: Option<T>,
    /// 失败时的诊断（带 JSON 路径）。
    pub diagnostics: Vec<Diagnostic>,
}

impl<T> DecodeResult<T> {
    /// 是否成功。
    pub fn ok(&self) -> bool {
        self.value.is_some()
    }

    /// 成功时取值；失败时把诊断还回去（对应 C++ 抛 `runtime_error` 的 `require()`，
    /// 但 Rust 侧不抛，改成 `Result`）。
    pub fn require(self) -> Result<T, Vec<Diagnostic>> {
        match self.value {
            Some(value) => Ok(value),
            None => Err(self.diagnostics),
        }
    }

    /// 第一条诊断。
    pub fn first_error(&self) -> Option<&Diagnostic> {
        self.diagnostics.first()
    }
}

impl<T: fmt::Debug> fmt::Debug for DecodeResult<T> {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter
            .debug_struct("DecodeResult")
            .field("value", &self.value)
            .field("diagnostics", &self.diagnostics)
            .finish()
    }
}

/// 解码上下文：路径、选项与诊断列表。
///
/// 路径是"当前走到哪儿"：进对象字段 push 一下、进数组元素 push 一下，出来 restore
/// 回去——与 C++ 版 `detail::Context` 的做法一样（只是 Rust 里用不着手写 RAII）。
pub struct Context<'a> {
    /// 选项。
    pub options: DecodeOptions,
    /// 当前 JSON 路径（例如 `points[1].x`）。
    pub path: String,
    /// 诊断输出。
    pub diagnostics: &'a mut Vec<Diagnostic>,
}

impl Context<'_> {
    /// 记一条诊断（路径为空时写成 `$`，表示"根"）。
    pub fn error(&mut self, code: impl Into<String>, message: impl Into<String>) {
        let path = if self.path.is_empty() { "$".to_string() } else { self.path.clone() };
        self.diagnostics.push(Diagnostic::in_path(path, code, message));
    }

    /// 进入一个对象字段，返回原来的路径（出来时用 [`Context::restore`] 还回去）。
    pub fn push_key(&mut self, key: &str) -> String {
        let saved = self.path.clone();
        if !self.path.is_empty() {
            self.path.push('.');
        }
        self.path.push_str(key);
        saved
    }

    /// 进入一个数组元素，返回原来的路径。
    pub fn push_index(&mut self, index: usize) -> String {
        let saved = self.path.clone();
        self.path.push_str(&format!("[{index}]"));
        saved
    }

    /// 还原路径。
    pub fn restore(&mut self, saved: String) {
        self.path = saved;
    }
}

// ---------------------------------------------------------------------------
//  模型接口
// ---------------------------------------------------------------------------

/// 模型：一段"字段名 ↔ 成员"的说明。
///
/// 一般不用手写，交给 [`json_model!`](crate::json_model) 就行；手写的场合是"字段名与
/// 成员名不一致"或"要在编解码中间插逻辑"。
///
/// 三个方法各管一段：
///
///   * [`field_names`](JsonModel::field_names)：声明顺序的字段名（`strict_unknown` 要用）；
///   * [`to_json_object`](JsonModel::to_json_object)：模型 → JSON 对象（顺序 = 声明顺序）；
///   * [`from_json_object`](JsonModel::from_json_object)：JSON 对象 → 模型（带路径的诊断）。
pub trait JsonModel: Default + Sized {
    /// 字段名，按声明顺序。
    fn field_names() -> &'static [&'static str];

    /// 模型 → JSON 对象。
    fn to_json_object(&self) -> Value;

    /// JSON 对象 → 模型。
    #[allow(clippy::result_unit_err)]
    fn from_json_object(object: &Object, ctx: &mut Context<'_>) -> Result<Self, ()>;
}

// ---------------------------------------------------------------------------
//  入口
// ---------------------------------------------------------------------------

/// 模型 → JSON（对应 C++ `json::encode`）。
///
/// ```
/// use json::{encode, json_model};
///
/// #[derive(Default)]
/// struct Point {
///     x: i64,
/// }
/// json_model!(Point, "x" => x);
///
/// assert_eq!(encode(&Point { x: 3 }).dump(), r#"{"x":3}"#);
/// ```
pub fn encode<T: ToJson + ?Sized>(value: &T) -> Value {
    value.to_json()
}

/// 编码一个字段（`json_model!` 生成的代码就是调它）。
///
/// 单独拎出来是为了让宏里的调用点类型推断简单一点：字段是 `&str` 这类借用时，
/// `encode_field(&self.field)` 拿到的是 `&&str`，靠 [`ToJson`] 的引用实现穿过去。
pub fn encode_field<T: ToJson + ?Sized>(value: &T) -> Value {
    value.to_json()
}

/// JSON → 模型，用缺省选项（对应 C++ `json::decode<T>(value)`）。
pub fn decode<T: FromJson>(value: &Value) -> DecodeResult<T> {
    decode_with(value, DecodeOptions::default())
}

/// JSON → 模型（对应 C++ `json::decode<T>(value, options)`）。
pub fn decode_with<T: FromJson>(value: &Value, options: DecodeOptions) -> DecodeResult<T> {
    let mut result = DecodeResult { value: None, diagnostics: Vec::new() };
    let mut context = Context { options, path: String::new(), diagnostics: &mut result.diagnostics };
    if let Ok(parsed) = T::from_json(value, &mut context) {
        result.value = Some(parsed);
    }
    result
}

// ---------------------------------------------------------------------------
//  标量、字符串、Value
// ---------------------------------------------------------------------------

impl ToJson for Value {
    /// 原样透传。
    fn to_json(&self) -> Value {
        self.clone()
    }
}

/// 引用自动穿过一层：`&T` 与 `T` 编码结果相同。
///
/// 有了它，`&str` 这样的借用字段也能直接编码（`&&str` 会被这条实现接住）。
impl<T: ToJson + ?Sized> ToJson for &T {
    fn to_json(&self) -> Value {
        (**self).to_json()
    }
}

impl FromJson for Value {
    /// 原样透传。
    fn from_json(value: &Value, _ctx: &mut Context<'_>) -> Result<Self, ()> {
        Ok(value.clone())
    }
}

impl ToJson for bool {
    fn to_json(&self) -> Value {
        Value::Bool(*self)
    }
}

impl FromJson for bool {
    fn from_json(value: &Value, ctx: &mut Context<'_>) -> Result<Self, ()> {
        match value.as_bool() {
            Some(flag) => Ok(flag),
            None => {
                let message = format!("期望布尔，实际是 {}", value.type_name());
                ctx.error("type-mismatch", message);
                Err(())
            }
        }
    }
}

/// 给一批整数类型生成「编码成 JSON 整数 / 从 JSON 整数解码」的实现。
///
/// 解码规则与 C++ 版一致：先按整数取，取不到且 [`DecodeOptions::allow_string_numbers`]
/// 打开时再试字符串形式的数字；都不行就报 `type-mismatch`。
macro_rules! impl_integer_binding {
    ($($name:ty),+ $(,)?) => {
        $(
            impl ToJson for $name {
                fn to_json(&self) -> Value {
                    Value::Int(*self as i64)
                }
            }

            impl FromJson for $name {
                fn from_json(value: &Value, ctx: &mut Context<'_>) -> Result<Self, ()> {
                    if let Some(number) = value.as_int() {
                        return Ok(number as $name);
                    }
                    if ctx.options.allow_string_numbers {
                        if let Some(text) = value.as_str() {
                            if let Ok(number) = text.parse::<i64>() {
                                return Ok(number as $name);
                            }
                        }
                    }
                    let message = format!("期望整数，实际是 {}", value.type_name());
                    ctx.error("type-mismatch", message);
                    Err(())
                }
            }
        )+
    };
}

impl_integer_binding!(i8, i16, i32, i64, u8, u16, u32, usize);

impl ToJson for f32 {
    fn to_json(&self) -> Value {
        Value::Double(f64::from(*self))
    }
}

impl ToJson for f64 {
    fn to_json(&self) -> Value {
        Value::Double(*self)
    }
}

impl FromJson for f32 {
    fn from_json(value: &Value, ctx: &mut Context<'_>) -> Result<Self, ()> {
        f64::from_json(value, ctx).map(|number| number as f32)
    }
}

impl FromJson for f64 {
    fn from_json(value: &Value, ctx: &mut Context<'_>) -> Result<Self, ()> {
        if let Some(number) = value.as_double() {
            return Ok(number);
        }
        if ctx.options.allow_string_numbers {
            if let Some(text) = value.as_str() {
                if let Ok(number) = text.parse::<f64>() {
                    return Ok(number);
                }
            }
        }
        let message = format!("期望浮点，实际是 {}", value.type_name());
        ctx.error("type-mismatch", message);
        Err(())
    }
}

/// 字符串：`str` 只用于**编码**（Rust 里想解码到"借用"是做不到的，得用 `String`）。
impl ToJson for str {
    fn to_json(&self) -> Value {
        Value::String(self.to_string())
    }
}

impl ToJson for String {
    fn to_json(&self) -> Value {
        Value::String(self.clone())
    }
}

impl FromJson for String {
    fn from_json(value: &Value, ctx: &mut Context<'_>) -> Result<Self, ()> {
        match value.as_str() {
            Some(text) => Ok(text.to_string()),
            None => {
                let message = format!("期望字符串，实际是 {}", value.type_name());
                ctx.error("type-mismatch", message);
                Err(())
            }
        }
    }
}

// ---------------------------------------------------------------------------
//  optional / vector / map
// ---------------------------------------------------------------------------

impl<T: ToJson> ToJson for Option<T> {
    /// 空 → `null`。
    fn to_json(&self) -> Value {
        match self {
            Some(value) => value.to_json(),
            None => Value::Null,
        }
    }
}

impl<T: FromJson> FromJson for Option<T> {
    /// `null` → 空。
    fn from_json(value: &Value, ctx: &mut Context<'_>) -> Result<Self, ()> {
        if value.is_null() {
            return Ok(None);
        }
        T::from_json(value, ctx).map(Some)
    }

    /// 字段缺席 → 空，且**不算错**（C++ 版对 `std::optional` 字段就是这么豁免的）。
    fn from_missing(out: &mut Self, _ctx: &mut Context<'_>, _name: &str) -> bool {
        *out = None;
        true
    }
}

impl<T: ToJson> ToJson for Vec<T> {
    /// 数组。
    fn to_json(&self) -> Value {
        Value::Array(self.iter().map(ToJson::to_json).collect())
    }
}

impl<T: FromJson> FromJson for Vec<T> {
    /// 必须是数组，逐元素解码（路径带 `[i]`）。
    fn from_json(value: &Value, ctx: &mut Context<'_>) -> Result<Self, ()> {
        let Some(items) = value.as_array() else {
            let message = format!("期望数组，实际是 {}", value.type_name());
            ctx.error("type-mismatch", message);
            return Err(());
        };
        let mut result = Vec::with_capacity(items.len());
        let mut ok = true;
        for (index, item) in items.iter().enumerate() {
            let saved = ctx.push_index(index);
            match T::from_json(item, ctx) {
                Ok(parsed) => result.push(parsed),
                Err(()) => ok = false,
            }
            ctx.restore(saved);
        }
        if ok {
            Ok(result)
        } else {
            Err(())
        }
    }
}

/// 以字符串为键的 map：数组式的重复代码太多，这里用宏统一生成。
macro_rules! impl_string_map_binding {
    ($($name:ident),+ $(,)?) => {
        $(
            impl<T: ToJson> ToJson for $name<String, T> {
                /// 对象（成员顺序 = map 的遍历顺序：`BTreeMap` 按名字升序，`HashMap` 不定）。
                fn to_json(&self) -> Value {
                    let mut object = Object::new();
                    for (key, value) in self {
                        object.insert(key.clone(), value.to_json());
                    }
                    Value::Object(object)
                }
            }

            impl<T: FromJson> FromJson for $name<String, T> {
                /// 必须是对象。
                fn from_json(value: &Value, ctx: &mut Context<'_>) -> Result<Self, ()> {
                    let Some(object) = value.as_object() else {
                        let message = format!("期望对象，实际是 {}", value.type_name());
                        ctx.error("type-mismatch", message);
                        return Err(());
                    };
                    let mut result = $name::new();
                    let mut ok = true;
                    for member in object.iter() {
                        let saved = ctx.push_key(&member.key);
                        match T::from_json(&member.value, ctx) {
                            Ok(parsed) => {
                                result.insert(member.key.clone(), parsed);
                            }
                            Err(()) => ok = false,
                        }
                        ctx.restore(saved);
                    }
                    if ok {
                        Ok(result)
                    } else {
                        Err(())
                    }
                }
            }
        )+
    };
}

impl_string_map_binding!(BTreeMap, HashMap);
