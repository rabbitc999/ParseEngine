//! 模板引擎自己的动态值：`Value` / `Array` / `Object`。
//!
//! 模板里的 `{{ path.to.value }}` 需要一个"运行时才知道类型"的值，这就是
//! [`Value`]。它**完全独立于** `library/json` 里的 `json::Value`：本 crate 不引用
//! 仓库里任何别的工程，两份实现可以各自演化（一份关心 JSON 的解析 / 序列化，
//! 一份只关心"模板里能不能取值、真不真、怎么变字符串"）。
//!
//! 设计要点：
//!
//!   * **六种类型互斥**：null / string / int64 / bool / array / object —— 只支持整数，
//!     代码生成里够用，省掉浮点格式化的一堆坑；
//!   * **共享而不是深拷贝**：数组与对象用 `Arc` 持有（对应 C++ 版的 `std::shared_ptr`），
//!     拷贝一个 `Value` 只是加一次引用计数；顺带让 `Value` 满足 `Send + Sync`；
//!   * **对象保序**：[`Object`] 内部是 `Vec<(String, Value)>` 而不是 `HashMap`，
//!     因为模板遍历时**顺序必须稳定**——模型字段的声明顺序就是生成代码的顺序；
//!   * **不 panic**：取不到就是 `None` / 空串 / 假值，没有 `unwrap` 式的接口。
//!
//! ```
//! use template_engine::{Object, Value};
//!
//! let text = Value::text("calc");
//! assert!(text.is_string());
//! assert_eq!(text.as_str(), Some("calc"));
//! assert_eq!(text.to_text(), "calc");
//!
//! let mut object = Object::new();
//! object.insert("name", "calc");
//! let document = Value::object(object);
//! assert_eq!(document.as_object().unwrap().get("name").unwrap().to_text(), "calc");
//! ```

use std::fmt;
use std::sync::Arc;

/// 数组：一串 [`Value`]。
///
/// `{% for x in list %}` 只认它（[`Value::as_array`] 拿到的就是本类型）。
pub type Array = Vec<Value>;

/// 模板引擎的值：null / string / int64 / bool / array / object。
///
/// 六种类型互斥：`is_xxx()` 判定、`as_xxx()` 取值（取不到返回 `None`）、
/// [`truthy`](Value::truthy) 给 `{% if %}` 用、[`to_text`](Value::to_text) 给
/// `{{ }}` 用。
///
/// ```
/// use template_engine::Value;
///
/// assert!(Value::Int(3).is_int());
/// assert_eq!(Value::Int(3).as_int(), Some(3));
/// assert_eq!(Value::Bool(true).to_text(), "true");
/// assert!(!Value::Int(0).truthy());
/// assert_eq!(Value::text("x"), Value::text("x"));
/// ```
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub enum Value {
    /// null：`{{ }}` 里渲染成空串，`{% if %}` 里判假。
    #[default]
    Null,
    /// 字符串。
    Str(String),
    /// 64 位整数。
    Int(i64),
    /// 布尔。
    Bool(bool),
    /// 数组（和其它 `Value` 共享同一份数据）。
    Array(Arc<Array>),
    /// 对象（和其它 `Value` 共享同一份数据）。
    Object(Arc<Object>),
}

impl Value {
    /// 造一个字符串值。
    pub fn text(text: impl Into<String>) -> Self {
        Value::Str(text.into())
    }

    /// 造一个整数值。
    pub fn int(number: impl Into<i64>) -> Self {
        Value::Int(number.into())
    }

    /// 造一个布尔值。
    pub fn flag(flag: bool) -> Self {
        Value::Bool(flag)
    }

    /// 造一个数组值（拿走所有权，之后与克隆体共享）。
    pub fn array(items: Array) -> Self {
        Value::Array(Arc::new(items))
    }

    /// 造一个对象值（拿走所有权，之后与克隆体共享）。
    pub fn object(members: Object) -> Self {
        Value::Object(Arc::new(members))
    }

    // ---- 类型判断 ---------------------------------------------------------

    /// 是不是 null。
    pub fn is_null(&self) -> bool {
        matches!(self, Value::Null)
    }

    /// 是不是字符串。
    pub fn is_string(&self) -> bool {
        matches!(self, Value::Str(_))
    }

    /// 是不是整数。
    pub fn is_int(&self) -> bool {
        matches!(self, Value::Int(_))
    }

    /// 是不是布尔。
    pub fn is_bool(&self) -> bool {
        matches!(self, Value::Bool(_))
    }

    /// 是不是数组。
    pub fn is_array(&self) -> bool {
        matches!(self, Value::Array(_))
    }

    /// 是不是对象。
    pub fn is_object(&self) -> bool {
        matches!(self, Value::Object(_))
    }

    // ---- 取值（取不到就是 None） -------------------------------------------

    /// 取字符串；不是字符串返回 `None`。
    pub fn as_str(&self) -> Option<&str> {
        match self {
            Value::Str(text) => Some(text),
            _ => None,
        }
    }

    /// 取整数；不是整数返回 `None`。
    pub fn as_int(&self) -> Option<i64> {
        match self {
            Value::Int(number) => Some(*number),
            _ => None,
        }
    }

    /// 取布尔；不是布尔返回 `None`。
    pub fn as_bool(&self) -> Option<bool> {
        match self {
            Value::Bool(flag) => Some(*flag),
            _ => None,
        }
    }

    /// 取数组；不是数组返回 `None`。
    pub fn as_array(&self) -> Option<&Array> {
        match self {
            Value::Array(items) => Some(items),
            _ => None,
        }
    }

    /// 取对象；不是对象返回 `None`。
    pub fn as_object(&self) -> Option<&Object> {
        match self {
            Value::Object(members) => Some(members),
            _ => None,
        }
    }

    // ---- 模板语义 ---------------------------------------------------------

    /// `{% if cond %}` 的真值判断。
    ///
    /// 规则：bool 按值；string 非空为真；array 非空为真；object 非空为真；
    /// 整数 0 为假；null 为假。
    pub fn truthy(&self) -> bool {
        match self {
            Value::Bool(flag) => *flag,
            Value::Str(text) => !text.is_empty(),
            Value::Int(number) => *number != 0,
            Value::Array(items) => !items.is_empty(),
            Value::Object(members) => !members.is_empty(),
            Value::Null => false,
        }
    }

    /// `{{ value }}` 的文本化（对应 C++ 版的 `toString()`）。
    ///
    /// 规则：string 原样；bool → `"true"` / `"false"`；int → 十进制；
    /// 其它（null / array / object）→ 空串。
    pub fn to_text(&self) -> String {
        self.to_string()
    }
}

/// `{{ value }}` 的文本化，规则与 [`Value::to_text`] 相同。
///
/// 实现出来是为了让 `resolve(...).to_string()` 这种写法自然可用。
impl fmt::Display for Value {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Value::Str(text) => formatter.write_str(text),
            Value::Bool(flag) => formatter.write_str(if *flag { "true" } else { "false" }),
            Value::Int(number) => write!(formatter, "{number}"),
            // null / array / object 都不产生文本。
            _ => Ok(()),
        }
    }
}

/// 对象：**保持插入顺序**的成员表。
///
/// 查找是线性的——模型字段就那几个，保序（= 字段声明顺序）比 O(1) 更值：
/// 内部用 `Vec<(String, Value)>` 而不是 `HashMap`，模板遍历顺序才是稳定的。
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Object {
    members: Vec<(String, Value)>,
}

impl Object {
    /// 空对象。
    pub fn new() -> Self {
        Object { members: Vec::new() }
    }

    /// 追加一个成员（同名不去重——与 C++ 版一致，重名时按先找到的那个算）。
    ///
    /// 返回 `&mut Self`，可以链式地摆出一整个对象。
    pub fn insert(&mut self, key: impl Into<String>, value: impl Into<Value>) -> &mut Self {
        self.members.push((key.into(), value.into()));
        self
    }

    /// 按名字找成员；没有返回 `None`。
    pub fn get(&self, key: &str) -> Option<&Value> {
        self.members.iter().find(|(name, _)| name == key).map(|(_, value)| value)
    }

    /// 有没有这个成员。
    pub fn contains_key(&self, key: &str) -> bool {
        self.get(key).is_some()
    }

    /// 是不是没有成员。
    pub fn is_empty(&self) -> bool {
        self.members.is_empty()
    }

    /// 成员个数。
    pub fn len(&self) -> usize {
        self.members.len()
    }

    /// 成员表（按插入顺序）。
    pub fn members(&self) -> &[(String, Value)] {
        &self.members
    }

    /// 按插入顺序遍历成员。
    pub fn iter(&self) -> impl Iterator<Item = (&str, &Value)> {
        self.members.iter().map(|(name, value)| (name.as_str(), value))
    }

    /// 用一串"名字 → 值"造对象。
    ///
    /// ```
    /// use template_engine::{Object, Value};
    ///
    /// let object = Object::from_pairs([("name", Value::text("calc")), ("size", Value::Int(3))]);
    /// assert_eq!(object.len(), 2);
    /// ```
    pub fn from_pairs<K, V, I>(pairs: I) -> Self
    where
        K: Into<String>,
        V: Into<Value>,
        I: IntoIterator<Item = (K, V)>,
    {
        let mut object = Object::new();
        for (key, value) in pairs {
            object.insert(key, value);
        }
        object
    }
}

impl FromIterator<(String, Value)> for Object {
    fn from_iter<T: IntoIterator<Item = (String, Value)>>(iter: T) -> Self {
        let mut object = Object::new();
        for (key, value) in iter {
            object.insert(key, value);
        }
        object
    }
}

// ---------------------------------------------------------------------------
//  便捷转换：`Value::from(...)` / `.into()`
// ---------------------------------------------------------------------------

impl From<&Value> for Value {
    fn from(value: &Value) -> Self {
        value.clone()
    }
}

impl From<String> for Value {
    fn from(text: String) -> Self {
        Value::Str(text)
    }
}

impl From<&str> for Value {
    fn from(text: &str) -> Self {
        Value::Str(text.to_string())
    }
}

impl From<bool> for Value {
    fn from(flag: bool) -> Self {
        Value::Bool(flag)
    }
}

impl From<i64> for Value {
    fn from(number: i64) -> Self {
        Value::Int(number)
    }
}

impl From<i32> for Value {
    fn from(number: i32) -> Self {
        Value::Int(i64::from(number))
    }
}

impl From<u32> for Value {
    fn from(number: u32) -> Self {
        Value::Int(i64::from(number))
    }
}

impl From<usize> for Value {
    fn from(number: usize) -> Self {
        Value::Int(number as i64)
    }
}

impl From<Array> for Value {
    fn from(items: Array) -> Self {
        Value::Array(Arc::new(items))
    }
}

impl From<Object> for Value {
    fn from(members: Object) -> Self {
        Value::Object(Arc::new(members))
    }
}
