//! 两个宏：`encode_object!`（写 [`Encode`](crate::Encode) 实现体的字段表）与
//! `template_model!`（结构体 + `Encode` 一次展开）。
//!
//! 放在单独一个模块里只是为了让 `lib.rs` 清爽，宏本身都 `#[macro_export]` 到 crate
//! 根，用的时候 `template_engine::encode_object!` 即可。

/// 把一串"名字 → 值"摆成 `Value::Object`。
///
/// 这是 C++ 版 `TEMPLATE_MODEL(T, tmpl::field("x", &T::x), ...)` 的对应物：
/// **顺序就是模板遍历顺序**，所以字段的书写顺序别随手改。
///
/// ```
/// use template_engine::{encode_object, Encode, Value};
///
/// struct Token {
///     name: String,
///     priority: i64,
/// }
///
/// impl Encode for Token {
///     fn encode(&self) -> Value {
///         encode_object! { "name" => &self.name, "priority" => self.priority }
///     }
/// }
/// ```
///
/// 值的部分走 [`Encode`](crate::Encode)：标量、`&str`、`Vec<T>`、`Option<T>`、
/// 别的模型都能直接填。空表（`encode_object! {}`）得到一个空对象——模板里
/// 任何变量都取不到，正是"纯静态片段"要的占位模型。
#[macro_export]
macro_rules! encode_object {
    ($($key:expr => $value:expr),* $(,)?) => {{
        #[allow(unused_mut)]
        let mut members = $crate::Object::new();
        $(
            members.insert($key, $crate::encode_into(&$value));
        )*
        $crate::Value::object(members)
    }};
}

/// 声明一个"模板模型"结构体，并顺手实现 [`Encode`](crate::Encode)。
///
/// C++ 版是"先有结构体、再写 `TEMPLATE_MODEL` 绑定"；Rust 里没有成员指针，
/// 干脆一步到位：结构体（字段 `pub`、`#[derive(Clone, Debug)]`）+ `new()`
/// + `Encode`。
///
/// ```
/// use template_engine::{render, template_model};
///
/// template_model! {
///     /// 一个方法。
///     pub struct Method {
///         /// 方法名。
///         name: String,
///         /// 返回类型。
///         ret: String,
///         /// 是不是静态方法。
///         is_static: bool,
///     }
/// }
///
/// let method = Method::new("add", "int", false);
/// assert_eq!(render("{{ ret }} {{ name }}();", &method).unwrap(), "int add();");
/// ```
///
/// 生成的结构体带 `new()`（参数顺序 = 字段声明顺序，每项收 `impl Into<字段类型>`）。
/// 想给已有的结构体加绑定，别用这个宏，直接手写 `impl Encode` + `encode_object!`
/// 就行——那才是 C++ 版 `TEMPLATE_MODEL` 的等价写法（可以给字段起跟 Rust 字段名
/// 不同的模板名）。
///
/// 模板里的名字取的是**字段名本身**（`stringify!` 展开）；字段名不合法的模板键
/// （带连字符之类）请改用手写 `impl`。
#[macro_export]
macro_rules! template_model {
    (
        $(#[$meta:meta])*
        $vis:vis struct $name:ident {
            $(
                $(#[$field_meta:meta])*
                $field:ident : $ty:ty
            ),* $(,)?
        }
    ) => {
        $(#[$meta])*
        #[derive(Clone, Debug)]
        $vis struct $name {
            $(
                $(#[$field_meta])*
                pub $field: $ty,
            )*
        }

        impl $name {
            /// 按字段声明顺序造一个模型。
            ///
            /// 每个参数收 `impl Into<字段类型>`，所以 `String` 字段可以直接传
            /// `&str`（`ClassModel::new("Calc", true, methods)`）。
            pub fn new($($field: impl Into<$ty>),*) -> Self {
                Self { $($field: $field.into()),* }
            }
        }

        impl $crate::Encode for $name {
            fn encode(&self) -> $crate::Value {
                $crate::encode_object! {
                    $( ::std::stringify!($field) => &self.$field ),*
                }
            }
        }
    };
}
