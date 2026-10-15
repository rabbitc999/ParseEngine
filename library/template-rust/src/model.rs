//! 模板引擎的"取数"抽象：`Model` / `ValueModel` / `MapModel` / `EncodedModel` / `resolve`。
//!
//! 模板引擎不关心数据是 Rust 结构体、是动态拼出来的 [`Value`]、还是别的什么，
//! 它只认一个抽象：**给一个名字，返回一个 [`Value`]**。这就是 [`Model`]：
//!
//! ```
//! use template_engine::{Model, Value};
//!
//! struct FromEnv;
//!
//! impl Model for FromEnv {
//!     fn get(&self, key: &str) -> Value {
//!         Value::text(format!("<{key}>"))
//!     }
//! }
//! ```
//!
//! 四种现成的实现：
//!
//!   * [`EncodedModel`]：把实现了 [`Encode`](crate::Encode) 的模型类包成 [`Model`]
//!     （**投影一次**、之后反复取字段——C++ 版的 `ModelAdapter<T>` 是每次 `get`
//!     现查字段表，Rust 没有成员指针，所以改成一次投影 + 缓存）；
//!   * [`ValueModel`]：把已经造好的 [`Value`] 对象包成 [`Model`]——`a.b.c` 这种
//!     多级路径中间那几级就靠它逐级下降；
//!   * [`MapModel`]：用"名字 → 值"的表当模型，给还没做模型结构体的模板用；
//!   * [`ChildModel`](crate::ChildModel)（在 `engine`）：循环体的子上下文，带作用域链。
//!
//! [`resolve`] 是路径求值：`"a.b.c"` 按点号切段，逐级向下，任何一级不是对象
//! 就直接返回 null——模板里 `{{ 不存在的字段 }}` 只会输出空串，不报错、
//! 更不会 panic。

use crate::bind::Encode;
use crate::value::Value;

/// 模板引擎的取数接口：一个名字 → 一个值。
///
/// 引擎只依赖这个 trait，所以模型类（[`EncodedModel`]）、动态值（[`ValueModel`]）、
/// 名字值表（[`MapModel`]）、循环子上下文（`ChildModel`）可以用同一套模板语法。
///
/// 约定：**没有这个字段就返回 [`Value::Null`]**，不要 panic。
pub trait Model {
    /// 按名字取值。
    fn get(&self, key: &str) -> Value;
}

/// 把实现了 [`Encode`](crate::Encode) 的模型包成 [`Model`]。
///
/// 与 C++ 版 `ModelAdapter<T>` 的差别：那边靠成员指针表做到"每次 `get` 只投影命中的
/// 那一个字段"，Rust 没有成员指针，这个适配器改成**构造时投影一次**、之后每次
/// `get` 都在缓存好的对象里查。所以同一个适配器可以反复（跨多次 `get`、跨多个
/// `{% for %}` 轮次）使用，不会重复投影。
///
/// ```
/// use template_engine::{render, EncodedModel, Model, TemplateEngine};
///
/// struct Point { x: i64, y: i64 }
/// // （Point 的 Encode 实现略）
/// # impl template_engine::Encode for Point {
/// #     fn encode(&self) -> template_engine::Value {
/// #         template_engine::encode_object! { "x" => self.x, "y" => self.y }
/// #     }
/// # }
///
/// let point = Point { x: 3, y: 4 };
/// let model = EncodedModel::new(&point);
/// assert_eq!(model.get("x").as_int(), Some(3));
/// ```
#[derive(Clone, Debug)]
pub struct EncodedModel {
    value: Value,
}

impl EncodedModel {
    /// 投影一个模型。
    pub fn new<T: Encode + ?Sized>(model: &T) -> Self {
        EncodedModel { value: model.encode() }
    }

    /// 已经有一个值了，直接包起来。
    pub fn from_value(value: Value) -> Self {
        EncodedModel { value }
    }

    /// 里面缓存的那个值。
    pub fn value(&self) -> &Value {
        &self.value
    }
}

impl Model for EncodedModel {
    fn get(&self, key: &str) -> Value {
        match self.value.as_object().and_then(|members| members.get(key)) {
            Some(member) => member.clone(),
            None => Value::Null,
        }
    }
}

/// 把一个已经造好的 [`Value`] 包成 [`Model`]。
///
/// 用途：给动态值当根上下文（C++ 版 `main.cpp` / 自检里就是这种用法），
/// 以及 `resolve()` 处理 `"a.b.c"` 时中间那几级的逐级下降。
#[derive(Clone, Debug, Default)]
pub struct ValueModel<'a> {
    value: Option<&'a Value>,
}

impl<'a> ValueModel<'a> {
    /// 绑定一个值。
    pub fn new(value: &'a Value) -> Self {
        ValueModel { value: Some(value) }
    }

    /// 还没绑任何值（取什么都得到 null）。
    pub fn unbound() -> Self {
        ValueModel { value: None }
    }

    /// 改绑另一个值。
    pub fn bind(&mut self, value: &'a Value) {
        self.value = Some(value);
    }
}

impl Model for ValueModel<'_> {
    fn get(&self, key: &str) -> Value {
        match self.value.and_then(|value| value.as_object()).and_then(|members| members.get(key)) {
            Some(member) => member.clone(),
            None => Value::Null,
        }
    }
}

/// 用"名字 → 值"的表当模型：给还没做专门模型结构体的模板用。
///
/// 生成器里最常见的形态是"模板写好了，但字段还是临时拼的"——[`render_values`]
/// 就是给这种场合准备的：不用先声明 struct + `Encode`，直接把名字和值摆出来渲染，
/// 模板里的 `{{ 名字 }}` 照样能用。
///
/// ```
/// use template_engine::{render_values, Value};
///
/// let text = render_values(
///     "{{ mode_name }} / {{ ns }}",
///     &[("mode_name", Value::text("LALR(1)")), ("ns", Value::text("calc"))],
/// )
/// .unwrap();
/// assert_eq!(text, "LALR(1) / calc");
/// ```
///
/// [`render_values`]: crate::render_values
#[derive(Clone, Debug, Default)]
pub struct MapModel {
    entries: Vec<(String, Value)>,
}

impl MapModel {
    /// 空表。
    pub fn new() -> Self {
        MapModel { entries: Vec::new() }
    }

    /// 用一串"名字 → 值"建模型。
    ///
    /// **同名不去重**：按表的顺序取先找到的那个（与 C++ 版 `MapModel` 的
    /// initializer_list 构造函数一致）。要"后加覆盖前加"请用 [`MapModel::set`]。
    pub fn from_pairs<K, V, I>(pairs: I) -> Self
    where
        K: Into<String>,
        V: Into<Value>,
        I: IntoIterator<Item = (K, V)>,
    {
        MapModel {
            entries: pairs.into_iter().map(|(key, value)| (key.into(), value.into())).collect(),
        }
    }

    /// 加一项（同名时**覆盖**前面那个，与 C++ 版 `MapModel::set` 一致）。
    pub fn set(&mut self, key: impl Into<String>, value: impl Into<Value>) -> &mut Self {
        let key = key.into();
        let value = value.into();
        for entry in &mut self.entries {
            if entry.0 == key {
                entry.1 = value;
                return self;
            }
        }
        self.entries.push((key, value));
        self
    }

    /// 表里有多少项。
    pub fn len(&self) -> usize {
        self.entries.len()
    }

    /// 表是不是空的。
    pub fn is_empty(&self) -> bool {
        self.entries.is_empty()
    }
}

impl Model for MapModel {
    fn get(&self, key: &str) -> Value {
        match self.entries.iter().find(|(name, _)| name == key) {
            Some((_, value)) => value.clone(),
            None => Value::Null,
        }
    }
}

/// 求值一条点号路径：`"a.b.c"`。
///
/// 逐段调用 `current.get(segment)`；除了最后一段之外，每一段的结果都必须是
/// 对象，否则说明路径走不通，直接返回 null。空路径、空段、缺失字段一律返回
/// null——模板里只会输出空串，不报错。
///
/// 注意：**只按点号切段**，不做下标 / 表达式求值（`a[0]` 这种不认，整段当一个
/// 名字）。这是引擎刻意的边界：模板只管排版，不管逻辑。
///
/// ```
/// use template_engine::{resolve, ValueModel, Value, Object};
///
/// let nested = Object::from_pairs([
///     ("probe", Value::object(Object::from_pairs([("name", Value::text("leafy"))]))),
/// ]);
/// let root = Value::object(nested);
/// let model = ValueModel::new(&root);
///
/// assert_eq!(resolve(&model, "probe.name").as_str(), Some("leafy"));
/// assert_eq!(resolve(&model, "probe.missing"), Value::Null);
/// ```
pub fn resolve(root: &dyn Model, path: &str) -> Value {
    if path.is_empty() {
        return Value::Null;
    }

    let mut segments = path.split('.');
    let first = match segments.next() {
        Some(segment) => segment,
        None => return Value::Null,
    };
    let mut value = root.get(first);

    for segment in segments {
        // 后面还有段：这一级必须是对象，否则路径断开。
        let found = value.as_object().and_then(|members| members.get(segment)).cloned();
        match found {
            Some(member) => value = member,
            None => return Value::Null,
        }
    }
    value
}
