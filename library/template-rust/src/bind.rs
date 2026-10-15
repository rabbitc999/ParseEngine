//! 把 Rust 模型投射成模板引擎的 [`Value`]：[`Encode`]（外加 `encode_object!` /
//! `template_model!` 两个宏）。
//!
//! **C++ 版的 `TEMPLATE_MODEL` 靠成员指针表**（`tmpl::field("name", &T::name)`）把模型类
//! 接进引擎；Rust 没有成员指针，对应物是**实现一次 [`Encode`]**——把模型"投影"成
//! `Value`。所以：
//!
//!   * **已经有结构体**：手写 `impl Encode`，body 用 `encode_object!` 摆出字段表，
//!     顺序就是模板遍历顺序：
//!
//!     ```
//!     use template_engine::{encode_object, Encode, Value};
//!
//!     struct Token {
//!         name: String,
//!         priority: i64,
//!     }
//!
//!     impl Encode for Token {
//!         fn encode(&self) -> Value {
//!             encode_object! { "name" => &self.name, "priority" => self.priority }
//!         }
//!     }
//!     ```
//!
//!   * **还没写结构体**：`template_model!` 一次把结构体 + [`Encode`] 一起展开出来：
//!
//!     ```
//!     use template_engine::template_model;
//!
//!     template_model! {
//!         /// 一个变量声明。
//!         pub struct Variable {
//!             name: String,
//!             ty: String,
//!         }
//!     }
//!
//!     let variable = Variable::new("count", "int");
//!     assert_eq!(variable.name, "count");
//!     ```
//!
//! 字段类型不用改造成模板专用的类型：标量（bool / 各种整数 / char / String / &str）、
//! [`Value`] 本身、`Vec<T>`、`&[T]`、`[T; N]`、`Option<T>`、以及任何实现了 [`Encode`]
//! 的模型类都直接能填——`Vec<模型>` 会变成数组，模型会变成对象。
//!
//! 同一个结构体可以既实现本 crate 的 `Encode`、又实现别的投射（例如 `library/json`
//! 那套 JSON 绑定），两份互不干扰。

use crate::value::Value;
use std::sync::Arc;

/// 把模型投射成模板引擎的 [`Value`]。
///
/// 引擎只认 [`Value`]，所以"字段名 → 值"这一步必须由某一方声明：本 trait 就是那个
/// 声明点。实现体通常用 `encode_object!` 写，字段顺序 = 模板遍历顺序。
///
/// 已经内置实现的类型：
///
///   * [`Value`]（原样克隆）；
///   * `bool`、`i8` / `i16` / `i32` / `i64` / `isize` / `u8` / `u16` / `u32` / `u64` / `usize`
///     （统一收成 `i64`；超过 64 位的整数类型不在支持范围内）；
///   * `char`（变成单字符串）；
///   * `String` / `str` / `&T`；
///   * `Vec<T>` / `[T]` / `[T; N]`（变数组，元素可以是标量、模型，甚至嵌套容器）；
///   * `Option<T>`（`None` 变成 null，`{% if %}` 里判假）；
///   * 任何实现了 `Encode` 的模型类（变对象，可以嵌套、可以放进 `Vec`）。
pub trait Encode {
    /// 把自己的值投影成模板值。
    fn encode(&self) -> Value;
}

/// 把一个实现了 [`Encode`] 的值投影成 [`Value`]。
///
/// 存在的意义是给 `encode_object!` 这类宏用：宏里拿到的是表达式，统一走这里
/// 变成值，调用点就不用区分字段是标量还是模型。
pub fn encode_into<T: Encode + ?Sized>(value: &T) -> Value {
    value.encode()
}

impl Encode for Value {
    fn encode(&self) -> Value {
        self.clone()
    }
}

impl Encode for bool {
    fn encode(&self) -> Value {
        Value::Bool(*self)
    }
}

/// 把各种整数统一收成 `i64`（和 C++ 版 `TEMPLATE_MODEL` 的整数分支一致）。
macro_rules! encode_integer {
    ($($ty:ty),* $(,)?) => {
        $(
            impl Encode for $ty {
                fn encode(&self) -> Value {
                    Value::Int(*self as i64)
                }
            }
        )*
    };
}

encode_integer!(i8, i16, i32, i64, isize, u8, u16, u32, u64, usize);

impl Encode for char {
    fn encode(&self) -> Value {
        Value::Str(self.to_string())
    }
}

impl Encode for str {
    fn encode(&self) -> Value {
        Value::Str(self.to_string())
    }
}

impl Encode for String {
    fn encode(&self) -> Value {
        Value::Str(self.clone())
    }
}

/// 引用照常投影：`&String` / `&str` / `&&T` 都能当字段填。
impl<T: Encode + ?Sized> Encode for &T {
    fn encode(&self) -> Value {
        (**self).encode()
    }
}

impl<T: Encode> Encode for [T] {
    fn encode(&self) -> Value {
        Value::Array(Arc::new(self.iter().map(Encode::encode).collect()))
    }
}

impl<T: Encode, const N: usize> Encode for [T; N] {
    fn encode(&self) -> Value {
        Value::Array(Arc::new(self.iter().map(Encode::encode).collect()))
    }
}

impl<T: Encode> Encode for Vec<T> {
    fn encode(&self) -> Value {
        Value::Array(Arc::new(self.iter().map(Encode::encode).collect()))
    }
}

/// `Some(x)` 照常投影，`None` 变成 null（`{% if %}` 判假、`{{ }}` 输出空串）。
impl<T: Encode> Encode for Option<T> {
    fn encode(&self) -> Value {
        match self {
            Some(value) => value.encode(),
            None => Value::Null,
        }
    }
}
