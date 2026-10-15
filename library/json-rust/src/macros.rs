//! 两个宏：`json_model!`（模型字段表）与 `json_enum!`（枚举 ↔ 整数）。
//!
//! 它们对应 C++ 版 `bind.hpp` 里的 `JSON_MODEL(Type, json::field(...), ...)`：
//! 都是"由某一方声明字段"的那一步，也是投射里**唯一要手写的东西**。
//!
//! 生成的代码不做任何反射：`json_model!` 直接展开成「逐字段 encode / decode」的
//! 普通函数，编译期就全部确定，运行时零查找开销（除了 JSON 对象本身的线性查找）。

/// 把一个模型的字段表补上（生成 [`JsonModel`](crate::JsonModel) +
/// [`ToJson`](crate::ToJson) + [`FromJson`](crate::FromJson) 三个实现）。
///
/// 语法：`json_model!(类型, "JSON 里的名字" => 成员名, …)`；字段顺序 = 声明顺序
/// （编码时就是这个顺序写进对象）。
///
/// 模型要 `#[derive(Default)]`——解码时先造一个缺省实例再逐字段覆盖，和 C++ 版
/// `U out{}` 的做法一样。
///
/// ```
/// use json::{decode, encode, json_model, Value};
///
/// #[derive(Clone, Debug, Default, PartialEq)]
/// struct Token {
///     name: String,
///     priority: i64,
///     pattern: Option<String>,
/// }
/// json_model!(Token, "name" => name, "priority" => priority, "pattern" => pattern);
///
/// // 字段顺序 = 声明顺序；`Option` 是空 → null。
/// let token = Token { name: "NUMBER".into(), priority: 10, pattern: None };
/// assert_eq!(encode(&token).dump(), r#"{"name":"NUMBER","priority":10,"pattern":null}"#);
///
/// // 可选字段缺席不算错。
/// let parsed = decode::<Token>(&json::parse(r#"{"name":"IDENT","priority":5}"#).value.unwrap());
/// assert!(parsed.ok());
/// assert_eq!(parsed.value.unwrap(), Token { name: "IDENT".into(), priority: 5, pattern: None });
///
/// // 缺必填字段：诊断带字段名。
/// let broken = decode::<Token>(&Value::object([("priority", Value::int(5))]));
/// assert!(!broken.ok());
/// assert_eq!(broken.diagnostics[0].path, "name");
/// assert_eq!(broken.diagnostics[0].code, "missing-field");
/// ```
#[macro_export]
macro_rules! json_model {
    ($model:ty, $( $name:literal => $field:ident ),+ $(,)?) => {
        impl $crate::JsonModel for $model {
            fn field_names() -> &'static [&'static str] {
                &[ $( $name ),+ ]
            }

            /// 模型 → JSON 对象：字段顺序就是声明顺序。
            fn to_json_object(&self) -> $crate::Value {
                let mut object = $crate::Object::new();
                $( object.insert($name, $crate::encode_field(&self.$field)); )+
                $crate::Value::Object(object)
            }

            /// JSON 对象 → 模型：逐字段解，缺字段交给 `FromJson::from_missing`
            /// （可选字段豁免、必填字段按 `strict_missing` 报错）。
            fn from_json_object(
                object: &$crate::Object,
                ctx: &mut $crate::Context<'_>,
            ) -> ::std::result::Result<Self, ()> {
                let mut out = <$model as ::std::default::Default>::default();
                let mut ok = true;
                $(
                    {
                        let saved = ctx.push_key($name);
                        match object.get($name) {
                            Some(value) => match $crate::FromJson::from_json(value, ctx) {
                                Ok(parsed) => out.$field = parsed,
                                Err(()) => ok = false,
                            },
                            None => {
                                if !$crate::FromJson::from_missing(&mut out.$field, ctx, $name) {
                                    ok = false;
                                }
                            }
                        }
                        ctx.restore(saved);
                    }
                )+
                if ctx.options.strict_unknown {
                    let known = <Self as $crate::JsonModel>::field_names();
                    for member in object.iter() {
                        if known.contains(&member.key.as_str()) {
                            continue;
                        }
                        let saved = ctx.push_key(&member.key);
                        ctx.error("unknown-field", format!("模型里没有字段 \"{}\"", member.key));
                        ctx.restore(saved);
                        ok = false;
                    }
                }
                if ok {
                    ::std::result::Result::Ok(out)
                } else {
                    ::std::result::Result::Err(())
                }
            }
        }

        impl $crate::ToJson for $model {
            fn to_json(&self) -> $crate::Value {
                <Self as $crate::JsonModel>::to_json_object(self)
            }
        }

        impl $crate::FromJson for $model {
            fn from_json(
                value: &$crate::Value,
                ctx: &mut $crate::Context<'_>,
            ) -> ::std::result::Result<Self, ()> {
                match value.as_object() {
                    Some(object) => <Self as $crate::JsonModel>::from_json_object(object, ctx),
                    None => {
                        let message = format!("期望对象，实际是 {}", value.type_name());
                        ctx.error("type-mismatch", message);
                        ::std::result::Result::Err(())
                    }
                }
            }
        }
    };
}

/// 给枚举装上「变体 ↔ 整数」的投射，并顺带记下每个变体的**名字**。
///
/// 语法：`json_enum!(类型, 变体 = 整数, 变体 = 整数, …)`（整数要写字面量）。
///
/// C++ 版靠 `std::is_enum` 对**所有**枚举统一分派；Rust 没有这回事，所以枚举要
/// 这样点一下名。生成的东西：
///
///   * `as_json_int()` / `as_json_name()` / `from_json_name()`——对应 C++ 的
///     `static_cast<底层类型>` 与 `to_string` / `from_string`；
///   * [`ToJson`](crate::ToJson)：按整数写（与 C++ 版默认一致）；
///   * [`FromJson`](crate::FromJson)：认整数，也认（`allow_string_numbers` 时的）
///     数字字符串；找不到对应变体就退回**第一个变体**（C++ 版这里是 `static_cast`
///     强转，数字对不上时结果不由语言保证）；
///   * `Default`：就是**第一个变体**——省得每个模型都给枚举写 `#[default]`。
///
/// > C++ 版 `DecodeOptions::enumAsString` 声明了却从来没被读过（`encode` 不收选项，
/// > 那个开关一直没接线）。Rust 版不搬这个空开关：想把枚举写成字符串，直接调
/// > `as_json_name()`（或自己写 `to_json`）。
///
/// ```
/// use json::{decode, encode, json_enum, ToJson};
///
/// #[derive(Clone, Copy, Debug, PartialEq)]
/// enum Kind {
///     Line,
///     Polygon,
/// }
/// json_enum!(Kind, Line = 0, Polygon = 1);
///
/// assert_eq!(encode(&Kind::Polygon).dump(), "1");
/// assert_eq!(encode(&Kind::Polygon.as_json_name()).dump(), "\"Polygon\"");
/// assert_eq!(Kind::from_json_name("Line"), Some(Kind::Line));
/// assert_eq!(decode::<Kind>(&json::parse("1").value.unwrap()).value, Some(Kind::Polygon));
/// assert_eq!(decode::<Kind>(&json::parse("\"1\"").value.unwrap()).value, Some(Kind::Polygon));
/// ```
#[macro_export]
macro_rules! json_enum {
    ($name:ty, $first:ident = $first_value:expr $(, $variant:ident = $value:expr )* $(,)?) => {
        impl $name {
            /// 枚举值 → 整数。
            pub fn as_json_int(&self) -> i64 {
                match self {
                    <$name>::$first => $first_value,
                    $( <$name>::$variant => $value, )*
                }
            }

            /// 整数 → 枚举值；对不上就退回第一个变体。
            ///
            /// （这里用 `if` 串而不是 `match`：宏里的 `expr` 片段放不进模式位置。）
            pub fn from_json_int(number: i64) -> Self {
                if number == $first_value {
                    return <$name>::$first;
                }
                $( if number == $value { return <$name>::$variant; } )*
                <$name>::$first
            }

            /// 枚举值 → 名字。
            pub fn as_json_name(&self) -> &'static str {
                match self {
                    <$name>::$first => stringify!($first),
                    $( <$name>::$variant => stringify!($variant), )*
                }
            }

            /// 名字 → 枚举值。
            pub fn from_json_name(name: &str) -> ::std::option::Option<Self> {
                if name == stringify!($first) {
                    return ::std::option::Option::Some(<$name>::$first);
                }
                $( if name == stringify!($variant) {
                    return ::std::option::Option::Some(<$name>::$variant);
                } )*
                ::std::option::Option::None
            }
        }

        /// 缺省值 = 第一个变体（对应 C++ 版枚举值 0 的习惯写法）。
        impl ::std::default::Default for $name {
            fn default() -> Self {
                <$name>::$first
            }
        }

        impl $crate::ToJson for $name {
            /// 按**整数**写（想按名字写就自己调 `as_json_name()`）。
            fn to_json(&self) -> $crate::Value {
                $crate::Value::Int(self.as_json_int())
            }
        }

        impl $crate::FromJson for $name {
            fn from_json(
                value: &$crate::Value,
                ctx: &mut $crate::Context<'_>,
            ) -> ::std::result::Result<Self, ()> {
                if let $crate::Value::Int(number) = value {
                    return ::std::result::Result::Ok(<$name>::from_json_int(*number));
                }
                if ctx.options.allow_string_numbers {
                    if let $crate::Value::String(text) = value {
                        if let ::std::result::Result::Ok(number) = text.parse::<i64>() {
                            return ::std::result::Result::Ok(<$name>::from_json_int(number));
                        }
                        let message = format!("期望枚举，实际是字符串 \"{text}\"");
                        ctx.error("type-mismatch", message);
                        return ::std::result::Result::Err(());
                    }
                }
                let message = format!("期望枚举，实际是 {}", value.type_name());
                ctx.error("type-mismatch", message);
                ::std::result::Result::Err(())
            }
        }
    };
}
