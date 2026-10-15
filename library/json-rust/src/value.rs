//! JSON 的运行时值：`Value` / `Array` / `Object` / `Member`。
//!
//! 这是 C++ 版 `library/json/json.hpp` 里那一层的 Rust 对应物。C++ 用
//! `std::variant<Null, bool, int64_t, double, string, Array, Object>` 装值、
//! 用「插入顺序的 `std::vector<Member>`」装对象；Rust 用 `enum` 装值、用
//! `Vec<Member>` 装对象——**约束是一样的**：类型互斥、对象保序。
//!
//! | C++ 版 | 本模块 |
//! | --- | --- |
//! | `json::Value`（`std::variant` + `isNull()` / `asInt()` …） | [`Value`]（`enum` + `is_null()` / `as_int()` …） |
//! | `json::Array` = `std::vector<Value>` | [`Array`] = `Vec<Value>` |
//! | `json::Object`（手写迭代器的类） | [`Object`]（`Deref<Target = [Member]>`，迭代器是切片的） |
//! | `json::Member{key, value}` 公开字段 | [`Member`] 公开字段 |
//! | `Value::asInt()` → `std::optional<int64_t>` | [`Value::as_int`] → `Option<i64>`，另有 `int_or` 兜底 |
//! | `asArray()` / `asObject()` 的 const 重载 | [`Value::as_array`] / [`Value::as_array_mut`] 两个方法 |
//! | 抛 `std::domain_error` / `std::out_of_range` | 返回 [`Error`]（[`Value::require_array`] / [`Value::at`]） |
//! | `Value::atPath("a.b[0]")` | [`Value::at_path`]（同一套路径语法与边界判定） |
//! | `for (auto& [key, value] : object)` 结构化绑定 | [`Entry`] / [`EntryMut`]（[`Value::entries`]、`for entry in &value`） |
//! | `operator==` 手写：1 == 1.0、对象不看顺序 | [`Value`] 手写 `PartialEq`，规则相同 |
//!
//! 设计要点：
//!
//!   * **对象保序**：[`Object`] 内部是 `Vec<Member>` 而不是 `HashMap`，查找是线性的
//!     ——配置 / 报文这类场景成员不多，保序（= 解析时的文件顺序、编码时的字段顺序）
//!     比 O(1) 更值；
//!   * **不 panic**：取不到就是 `None` / 兜底值 / `Err`，库内没有 `unwrap`；
//!   * **不共享**：`Value` 是普通的所有权类型（对应 C++ 的按值语义），克隆才是深拷贝。
//!
//! ```
//! use json::{parse, Value};
//!
//! let doc = parse(r#"{"name":"calc","tokens":["+","-"]}"#);
//! assert!(doc.ok());
//! let root = doc.value.as_ref().unwrap();
//! assert_eq!(root.get("name").unwrap().string_or("?"), "calc");
//! assert_eq!(root.at_path("tokens[1]").unwrap().string_or("?"), "-");
//!
//! let mut record = Value::object([("a", Value::int(1)), ("b", Value::string("x"))]);
//! assert_eq!(record.get("a").unwrap().int_or(0), 1);
//! record.get_or_insert("c").unwrap(); // 不存在的成员会被插成 null
//! assert_eq!(record.len(), 3);
//! ```

use std::fmt;
use std::ops::{Deref, DerefMut};

/// JSON 数组：一串 [`Value`]（对应 C++ 的 `std::vector<Value>`）。
///
/// 它是**裸 `Vec`**，所以 `for item in &array`、`array.iter().filter(..).count()`、
/// `array.len()`、`array.sort_by(...)` 这些标准容器玩法直接可用——这正是 C++ 版
/// 「迭代器给 STL 算法用」那一条在 Rust 里的对应物。
pub type Array = Vec<Value>;

/// 对象的一个成员：名字 + 值。
///
/// 两个字段都是公开的，所以可以 `for member in &object { member.key }`。
/// （Rust 没有结构化绑定，但 `Member { key, value }` 这样解构一样直接。）
#[derive(Clone, Debug, PartialEq)]
pub struct Member {
    /// 成员名。
    pub key: String,
    /// 成员值。
    pub value: Value,
}

/// JSON 对象：**保持插入顺序**的成员表。
///
/// 查找是线性的（配置 / 报文这类场景成员不多，保序比 O(1) 更值）；迭代顺序就是
/// 插入顺序（解析时即文件里的先后顺序，编码时即字段声明顺序）。
///
/// `Deref<Target = [Member]>` 让它**就是**一个成员切片：`object[0]`、`object.len()`、
/// `object.iter()`、`object.first()` 都来自切片，所以 `library/json` 里那套
/// 「对象也能直接喂 STL 算法」的用法原样成立。
#[derive(Clone, Debug, Default, PartialEq)]
pub struct Object {
    members: Vec<Member>,
}

impl Object {
    /// 空对象。
    pub fn new() -> Self {
        Object { members: Vec::new() }
    }

    /// 成员表（按插入顺序）。
    pub fn members(&self) -> &[Member] {
        &self.members
    }

    /// 成员表（可写）。
    pub fn members_mut(&mut self) -> &mut [Member] {
        &mut self.members
    }

    /// 按插入顺序遍历成员。
    pub fn iter(&self) -> std::slice::Iter<'_, Member> {
        self.members.iter()
    }

    /// 按插入顺序遍历成员（可写）。
    pub fn iter_mut(&mut self) -> std::slice::IterMut<'_, Member> {
        self.members.iter_mut()
    }

    /// 成员个数。
    pub fn len(&self) -> usize {
        self.members.len()
    }

    /// 是不是没有成员。
    pub fn is_empty(&self) -> bool {
        self.members.is_empty()
    }

    /// 清空。
    pub fn clear(&mut self) {
        self.members.clear();
    }

    /// 成员在表里的位置（没有返回 `None`）。
    fn position_of(&self, key: &str) -> Option<usize> {
        self.members.iter().position(|member| member.key == key)
    }

    /// 按名字找**成员**（对应 C++ `Object::find` 给出的迭代器）。
    pub fn find(&self, key: &str) -> Option<&Member> {
        self.position_of(key).map(|position| &self.members[position])
    }

    /// 按名字找成员（可写）。
    pub fn find_mut(&mut self, key: &str) -> Option<&mut Member> {
        match self.position_of(key) {
            Some(position) => Some(&mut self.members[position]),
            None => None,
        }
    }

    /// 按名字取**值**（对应 C++ `Object::tryGet` 的 `const` 重载）。
    pub fn get(&self, key: &str) -> Option<&Value> {
        self.find(key).map(|member| &member.value)
    }

    /// 按名字取值（可写，对应 C++ `Object::tryGet` 的非 const 重载）。
    pub fn get_mut(&mut self, key: &str) -> Option<&mut Value> {
        self.find_mut(key).map(|member| &mut member.value)
    }

    /// 有没有这个成员。
    pub fn contains(&self, key: &str) -> bool {
        self.position_of(key).is_some()
    }

    /// 成员个数（STL 风格，0 或 1；对应 C++ `Object::count`）。
    pub fn count(&self, key: &str) -> usize {
        usize::from(self.contains(key))
    }

    /// 取值，不存在就插一个 `null`（对应 C++ `Object::operator[]`）。
    pub fn get_or_insert(&mut self, key: impl Into<String>) -> &mut Value {
        let key = key.into();
        match self.position_of(&key) {
            Some(position) => &mut self.members[position].value,
            None => {
                self.members.push(Member { key, value: Value::Null });
                let last = self.members.len() - 1;
                &mut self.members[last].value
            }
        }
    }

    /// 插入或覆盖一个成员（对应 C++ `Object::insert`）。
    pub fn insert(&mut self, key: impl Into<String>, value: impl Into<Value>) -> &mut Member {
        let key = key.into();
        let value = value.into();
        match self.position_of(&key) {
            Some(position) => {
                self.members[position].value = value;
                &mut self.members[position]
            }
            None => {
                self.members.push(Member { key, value });
                let last = self.members.len() - 1;
                &mut self.members[last]
            }
        }
    }

    /// 删除一个成员（对应 C++ `Object::erase`）。
    pub fn erase(&mut self, key: &str) -> bool {
        match self.position_of(key) {
            Some(position) => {
                self.members.remove(position);
                true
            }
            None => false,
        }
    }

    /// 用一串「名字 → 值」造对象（保持给的顺序）。
    ///
    /// ```
    /// use json::{Object, Value};
    ///
    /// let object = Object::from_pairs([("name", Value::string("calc")), ("size", Value::int(3))]);
    /// assert_eq!(object.len(), 2);
    /// assert_eq!(object.get("name").unwrap().string_or(""), "calc");
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

impl Deref for Object {
    type Target = [Member];

    fn deref(&self) -> &[Member] {
        &self.members
    }
}

impl DerefMut for Object {
    fn deref_mut(&mut self) -> &mut [Member] {
        &mut self.members
    }
}

impl<'a> IntoIterator for &'a Object {
    type Item = &'a Member;
    type IntoIter = std::slice::Iter<'a, Member>;

    fn into_iter(self) -> Self::IntoIter {
        self.members.iter()
    }
}

impl<'a> IntoIterator for &'a mut Object {
    type Item = &'a mut Member;
    type IntoIter = std::slice::IterMut<'a, Member>;

    fn into_iter(self) -> Self::IntoIter {
        self.members.iter_mut()
    }
}

impl IntoIterator for Object {
    type Item = Member;
    type IntoIter = std::vec::IntoIter<Member>;

    fn into_iter(self) -> Self::IntoIter {
        self.members.into_iter()
    }
}

impl FromIterator<Member> for Object {
    fn from_iter<T: IntoIterator<Item = Member>>(iter: T) -> Self {
        Object { members: iter.into_iter().collect() }
    }
}

/// JSON 值：null / bool / int64 / double / string / array / object。
///
/// 七种类型互斥，与 C++ 版的 `std::variant` 一一对应。几个**刻意与 C++ 对齐**的点：
///
///   * [`as_int`](Value::as_int) 对「正好是整数的浮点」也成立（`3.0` → `3`），
///     但 `2^63` 那个边界不算（和 C++ 版的上下界判断逐字相同）；
///   * 相等按 JSON 语义：[`PartialEq`] 是手写的——数字跨 int / double 按数值比
///     （`1` == `1.0`），对象按「成员集合」比（顺序不影响相等）；
///   * [`dump`](Value::dump) 是紧凑单行，[`dump_indent`](Value::dump_indent) 管缩进
///     （缩进为负 = 紧凑）。
///
/// ```
/// use json::Value;
///
/// assert!(Value::int(3).is_int());
/// assert_eq!(Value::double(3.0).as_int(), Some(3));
/// assert_eq!(Value::Double(3.5).as_int(), None);
/// assert_eq!(Value::int(1), Value::double(1.0));
/// assert_eq!(Value::int(2).dump(), "2");
/// ```
#[derive(Clone, Debug, Default)]
pub enum Value {
    /// JSON 的 `null`。
    #[default]
    Null,
    /// 布尔。
    Bool(bool),
    /// 64 位整数。
    Int(i64),
    /// 双精度浮点。
    Double(f64),
    /// 字符串（UTF-8）。
    String(String),
    /// 数组。
    Array(Array),
    /// 对象。
    Object(Object),
}

impl Value {
    // ---- 构造 -------------------------------------------------------------

    /// 造一个 `null`。
    pub fn null() -> Self {
        Value::Null
    }

    /// 造一个布尔值。
    pub fn bool(flag: bool) -> Self {
        Value::Bool(flag)
    }

    /// 造一个整数值。
    pub fn int(number: impl Into<i64>) -> Self {
        Value::Int(number.into())
    }

    /// 造一个浮点值。
    pub fn double(number: f64) -> Self {
        Value::Double(number)
    }

    /// 造一个字符串值。
    pub fn string(text: impl Into<String>) -> Self {
        Value::String(text.into())
    }

    /// 用一串元素造数组（对应 C++ `Value::array`）。
    ///
    /// ```
    /// use json::Value;
    ///
    /// let items = Value::array([1, 2, 3]);
    /// assert!(items.is_array());
    /// assert_eq!(items.len(), 3);
    /// ```
    pub fn array<I, V>(items: I) -> Self
    where
        I: IntoIterator<Item = V>,
        V: Into<Value>,
    {
        Value::Array(items.into_iter().map(Into::into).collect())
    }

    /// 用一串「名字 → 值」造对象（对应 C++ `Value::object`）。
    ///
    /// ```
    /// use json::Value;
    ///
    /// let record = Value::object([("a", Value::int(1)), ("b", Value::string("x"))]);
    /// assert!(record.is_object());
    /// assert_eq!(record.get("b").unwrap().string_or("?"), "x");
    /// ```
    pub fn object<I, K, V>(pairs: I) -> Self
    where
        I: IntoIterator<Item = (K, V)>,
        K: Into<String>,
        V: Into<Value>,
    {
        let mut object = Object::new();
        for (key, value) in pairs {
            object.insert(key, value);
        }
        Value::Object(object)
    }

    // ---- 类型判断 ---------------------------------------------------------

    /// 是不是 `null`。
    pub fn is_null(&self) -> bool {
        matches!(self, Value::Null)
    }

    /// 是不是布尔。
    pub fn is_bool(&self) -> bool {
        matches!(self, Value::Bool(_))
    }

    /// 是不是整数。
    pub fn is_int(&self) -> bool {
        matches!(self, Value::Int(_))
    }

    /// 是不是浮点。
    pub fn is_double(&self) -> bool {
        matches!(self, Value::Double(_))
    }

    /// 是不是数字（整数或浮点）。
    pub fn is_number(&self) -> bool {
        self.is_int() || self.is_double()
    }

    /// 是不是字符串。
    pub fn is_string(&self) -> bool {
        matches!(self, Value::String(_))
    }

    /// 是不是数组。
    pub fn is_array(&self) -> bool {
        matches!(self, Value::Array(_))
    }

    /// 是不是对象。
    pub fn is_object(&self) -> bool {
        matches!(self, Value::Object(_))
    }

    /// 类型名（诊断 / 统计用）。
    ///
    /// 返回值与 C++ 版 `typeName()` 逐字相同：`null` / `bool` / `int` / `double` /
    /// `string` / `array` / `object`。
    pub fn type_name(&self) -> &'static str {
        match self {
            Value::Null => "null",
            Value::Bool(_) => "bool",
            Value::Int(_) => "int",
            Value::Double(_) => "double",
            Value::String(_) => "string",
            Value::Array(_) => "array",
            Value::Object(_) => "object",
        }
    }

    // ---- 取值（取不到就是 None） -------------------------------------------

    /// 取布尔。
    pub fn as_bool(&self) -> Option<bool> {
        match self {
            Value::Bool(flag) => Some(*flag),
            _ => None,
        }
    }

    /// 取整数（浮点且**正好是整数**时也能取到，`3.0` → `3`）。
    ///
    /// 边界与 C++ 版一致：`±2^63` 之外的浮点取不到（`2^63` 本身也取不到，
    /// `-2^63` 取得到）。
    pub fn as_int(&self) -> Option<i64> {
        match self {
            Value::Int(number) => Some(*number),
            Value::Double(number) => {
                if number.is_finite()
                    && number.trunc() == *number
                    && *number >= -9_223_372_036_854_775_808.0
                    && *number < 9_223_372_036_854_775_808.0
                {
                    Some(*number as i64)
                } else {
                    None
                }
            }
            _ => None,
        }
    }

    /// 取浮点（整数会转成浮点）。
    pub fn as_double(&self) -> Option<f64> {
        match self {
            Value::Double(number) => Some(*number),
            Value::Int(number) => Some(*number as f64),
            _ => None,
        }
    }

    /// 取字符串（借用，对应 C++ 的 `std::string_view` 取值）。
    pub fn as_str(&self) -> Option<&str> {
        match self {
            Value::String(text) => Some(text),
            _ => None,
        }
    }

    /// 取字符串，不是字符串就用兜底值（对应 C++ `stringOr`）。
    pub fn string_or<'a>(&'a self, fallback: &'a str) -> &'a str {
        self.as_str().unwrap_or(fallback)
    }

    /// 取整数，取不到就用兜底值（对应 C++ `intOr`）。
    pub fn int_or(&self, fallback: i64) -> i64 {
        self.as_int().unwrap_or(fallback)
    }

    /// 取浮点，取不到就用兜底值（对应 C++ `doubleOr`）。
    pub fn double_or(&self, fallback: f64) -> f64 {
        self.as_double().unwrap_or(fallback)
    }

    /// 取数组；不是数组返回 `None`。
    pub fn as_array(&self) -> Option<&Array> {
        match self {
            Value::Array(items) => Some(items),
            _ => None,
        }
    }

    /// 取数组（可写）；不是数组返回 `None`。
    pub fn as_array_mut(&mut self) -> Option<&mut Array> {
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

    /// 取对象（可写）；不是对象返回 `None`。
    pub fn as_object_mut(&mut self) -> Option<&mut Object> {
        match self {
            Value::Object(members) => Some(members),
            _ => None,
        }
    }

    /// 取数组，不是数组就报 [`Error::NotArray`]（对应 C++ 的 `requireArray()` 抛异常）。
    pub fn require_array(&self) -> Result<&Array, Error> {
        self.as_array().ok_or(Error::NotArray)
    }

    /// 取数组（可写），不是数组就报 [`Error::NotArray`]。
    pub fn require_array_mut(&mut self) -> Result<&mut Array, Error> {
        self.as_array_mut().ok_or(Error::NotArray)
    }

    /// 取对象，不是对象就报 [`Error::NotObject`]。
    pub fn require_object(&self) -> Result<&Object, Error> {
        self.as_object().ok_or(Error::NotObject)
    }

    /// 取对象（可写），不是对象就报 [`Error::NotObject`]。
    pub fn require_object_mut(&mut self) -> Result<&mut Object, Error> {
        self.as_object_mut().ok_or(Error::NotObject)
    }

    // ---- 访问与修改 -------------------------------------------------------

    /// 取数组元素。
    ///
    /// 不是数组报 [`Error::NotArray`]，下标越界报 [`Error::OutOfRange`]
    /// （对应 C++ 的 `domain_error` / `out_of_range`）。
    pub fn at(&self, index: usize) -> Result<&Value, Error> {
        let items = self.as_array().ok_or(Error::NotArray)?;
        items
            .get(index)
            .ok_or(Error::OutOfRange { index, len: items.len() })
    }

    /// 取数组元素（可写）。
    pub fn at_mut(&mut self, index: usize) -> Result<&mut Value, Error> {
        let items = self.as_array_mut().ok_or(Error::NotArray)?;
        let len = items.len();
        items.get_mut(index).ok_or(Error::OutOfRange { index, len })
    }

    /// 取对象成员；不存在就插一个 `null`（对应 C++ `Value::operator[]`）。
    pub fn get_or_insert(&mut self, key: impl Into<String>) -> Result<&mut Value, Error> {
        let object = self.as_object_mut().ok_or(Error::NotObject)?;
        Ok(object.get_or_insert(key))
    }

    /// 查找对象成员（对应 C++ `Value::find` 的 `const` 重载）。
    ///
    /// 不是对象或没找到都返回 `None`。
    pub fn get(&self, key: &str) -> Option<&Value> {
        self.as_object()?.get(key)
    }

    /// 查找对象成员（可写）。
    pub fn get_mut(&mut self, key: &str) -> Option<&mut Value> {
        self.as_object_mut()?.get_mut(key)
    }

    /// 追加一个数组元素，返回新元素（对应 C++ `push_back` / `emplace_back`）。
    pub fn push(&mut self, value: impl Into<Value>) -> Result<&mut Value, Error> {
        let items = self.as_array_mut().ok_or(Error::NotArray)?;
        items.push(value.into());
        let last = items.len() - 1;
        Ok(&mut items[last])
    }

    /// 元素个数（数组 / 对象 / 字符串的字节长度；标量是 0）。
    ///
    /// 对应 C++ 的 `size()`（C++ 里叫 `size` / `empty`，Rust 侧按习惯叫 `len` /
    /// `is_empty`，语义相同）。
    pub fn len(&self) -> usize {
        match self {
            Value::Array(items) => items.len(),
            Value::Object(members) => members.len(),
            Value::String(text) => text.len(),
            _ => 0,
        }
    }

    /// 是否为空（空数组 / 空对象 / 空字符串；`null` 也算空）。
    pub fn is_empty(&self) -> bool {
        match self {
            Value::Array(items) => items.is_empty(),
            Value::Object(members) => members.is_empty(),
            Value::String(text) => text.is_empty(),
            _ => self.is_null(),
        }
    }

    /// 清空数组 / 对象（标量变成 `null`）。
    pub fn clear(&mut self) {
        match self {
            Value::Array(items) => items.clear(),
            Value::Object(members) => members.clear(),
            Value::Null => {}
            _ => *self = Value::Null,
        }
    }

    /// 按路径取值：`a.b[0]`（对应 C++ `Value::atPath`）。
    ///
    /// 路径语法（**与 C++ 版实测行为逐条一致**）：
    ///
    ///   * 名字用 `.` 连接：`a.b.c`；
    ///   * 数组下标写 `[n]`：`a.b[0]`，`[n]` 也可以直接跟在根上（`[0]`）；
    ///   * `[n]` 只能出现在**路径末尾**或后面紧跟另一个 `[`：`a[0].b` 会判成
    ///     "找不到"。（C++ 版头注释里写着 `[0].name` 可用，**实测不行**——两版
    ///     都对同一路径返回"找不到"。这是照搬的既存行为，见 `技术报告.md`。）
    ///   * 空段（`a..b`、`.a`、`a.`）、没闭合的 `[`、非数字的 `[]` 都算"找不到"。
    ///
    /// ```
    /// use json::parse;
    ///
    /// let doc = parse(r#"{"user":{"tags":["a","b"]}}"#);
    /// let root = doc.value.unwrap();
    /// assert_eq!(root.at_path("user.tags[1]").unwrap().string_or("?"), "b");
    /// assert!(root.at_path("user.tags[1].x").is_none());
    /// assert!(root.at_path("user..tags").is_none());
    /// ```
    pub fn at_path(&self, path: &str) -> Option<&Value> {
        let bytes = path.as_bytes();
        let mut current = self;
        let mut index = 0usize;

        while index < bytes.len() {
            // 1. `[n]`：数组下标。
            if bytes[index] == b'[' {
                index += 1;
                let mut number = 0usize;
                let mut digits = false;
                while index < bytes.len() && bytes[index].is_ascii_digit() {
                    let digit = usize::from(bytes[index] - b'0');
                    number = number.checked_mul(10)?.checked_add(digit)?;
                    index += 1;
                    digits = true;
                }
                if index >= bytes.len() || bytes[index] != b']' || !digits {
                    return None;
                }
                index += 1;
                current = current.as_array()?.get(number)?;
                continue;
            }

            // 2. 段首出现 `.`：`a..b` / `.a` 这类空段，判为找不到。
            if bytes[index] == b'.' {
                return None;
            }

            // 3. 成员名：读到下一个 `.` 或 `[`。
            let start = index;
            while index < bytes.len() && bytes[index] != b'.' && bytes[index] != b'[' {
                index += 1;
            }
            let key = std::str::from_utf8(&bytes[start..index]).ok()?;
            if key.is_empty() {
                return None;
            }
            current = current.get(key)?;

            // 4. 名字后面要么结束、要么跟 `[`，要么是单个 `.`（不能是空段或结尾的点）。
            if index < bytes.len() && bytes[index] == b'.' {
                index += 1;
                if index >= bytes.len() || bytes[index] == b'.' {
                    return None;
                }
            }
        }

        Some(current)
    }

    /// 按路径取值的可写版本（对应 `value.at_path(p)`，但给的是 `&mut Value`）。
    ///
    /// 路径语法与 [`Value::at_path`] 完全相同（包括 `[n]` 之后不能跟 `.` 这一条）。
    ///
    /// ```
    /// use json::parse;
    ///
    /// let mut doc = parse(r#"{"user":{"tags":["a","b"]}}"#);
    /// let root = doc.value.as_mut().unwrap();
    /// *root.at_path_mut("user.tags[0]").unwrap() = "A".into();
    /// assert_eq!(root.at_path("user.tags[0]").unwrap().string_or("?"), "A");
    /// assert!(root.at_path_mut("user.tags[9]").is_none());
    /// ```
    pub fn at_path_mut(&mut self, path: &str) -> Option<&mut Value> {
        let bytes = path.as_bytes();
        let mut current = self;
        let mut index = 0usize;

        while index < bytes.len() {
            // 1. `[n]`：数组下标。
            if bytes[index] == b'[' {
                index += 1;
                let mut number = 0usize;
                let mut digits = false;
                while index < bytes.len() && bytes[index].is_ascii_digit() {
                    let digit = usize::from(bytes[index] - b'0');
                    number = number.checked_mul(10)?.checked_add(digit)?;
                    index += 1;
                    digits = true;
                }
                if index >= bytes.len() || bytes[index] != b']' || !digits {
                    return None;
                }
                index += 1;
                let len = current.as_array().map(Array::len)?;
                if number >= len {
                    return None;
                }
                current = &mut current.as_array_mut()?[number];
                continue;
            }

            // 2. 段首出现 `.`：空段，判为找不到。
            if bytes[index] == b'.' {
                return None;
            }

            // 3. 成员名：读到下一个 `.` 或 `[`。
            let start = index;
            while index < bytes.len() && bytes[index] != b'.' && bytes[index] != b'[' {
                index += 1;
            }
            let key = std::str::from_utf8(&bytes[start..index]).ok()?;
            if key.is_empty() {
                return None;
            }
            current = current.get_mut(key)?;

            // 4. 名字后面要么结束、要么跟 `[`，要么是单个 `.`。
            if index < bytes.len() && bytes[index] == b'.' {
                index += 1;
                if index >= bytes.len() || bytes[index] == b'.' {
                    return None;
                }
            }
        }

        Some(current)
    }

    // ---- 迭代 -------------------------------------------------------------

    /// 通用迭代：数组给元素、对象给成员、标量是空区间。
    ///
    /// 每一轮拿到一个 [`Entry`]（`key` / `index` / `value`），对应 C++ 版
    /// `Value::Reference` 那三样东西——区别只是 Rust 用 `Option` 表示「没有这一项」，
    /// 而不是 `npos` / 空 `string_view`。
    ///
    /// ```
    /// use json::parse;
    ///
    /// let doc = parse(r#"{"a":1,"b":[2,3]}"#);
    /// let root = doc.value.as_ref().unwrap();
    /// let names: Vec<&str> = root.entries().filter_map(|entry| entry.key).collect();
    /// assert_eq!(names, ["a", "b"]);
    ///
    /// let items: Vec<i64> = root
    ///     .get("b")
    ///     .unwrap()
    ///     .entries()
    ///     .filter_map(|entry| entry.value.as_int())
    ///     .collect();
    /// assert_eq!(items, [2, 3]);
    /// ```
    pub fn entries(&self) -> Entries<'_> {
        match self {
            Value::Array(items) => Entries {
                inner: EntriesInner::Array { items: items.iter(), position: 0 },
            },
            Value::Object(members) => Entries { inner: EntriesInner::Object(members.iter()) },
            _ => Entries { inner: EntriesInner::Empty },
        }
    }

    /// 通用迭代（可写）：数组给元素、对象给成员、标量是空区间。
    ///
    /// ```
    /// use json::parse;
    ///
    /// let mut doc = parse(r#"{"a":1,"b":2}"#);
    /// let root = doc.value.as_mut().unwrap();
    /// for entry in root.entries_mut() {
    ///     entry.value.clear();
    /// }
    /// assert_eq!(root.dump(), r#"{"a":null,"b":null}"#);
    /// ```
    pub fn entries_mut(&mut self) -> EntriesMut<'_> {
        match self {
            Value::Array(items) => EntriesMut {
                inner: EntriesInnerMut::Array { items: items.iter_mut(), position: 0 },
            },
            Value::Object(members) => EntriesMut { inner: EntriesInnerMut::Object(members.iter_mut()) },
            _ => EntriesMut { inner: EntriesInnerMut::Empty },
        }
    }
}

/// 迭代里的「当前一项」：对象成员给 `key`，数组元素给 `index`（对应 C++ `Value::Reference`）。
#[derive(Clone, Copy, Debug)]
pub struct Entry<'a> {
    /// 对象成员名；数组元素是 `None`。
    pub key: Option<&'a str>,
    /// 数组下标；对象成员是 `None`。
    pub index: Option<usize>,
    /// 元素 / 成员值的引用。
    pub value: &'a Value,
}

impl<'a> Entry<'a> {
    /// 值的类型名（写统计 / 校验时不关心是元素还是成员时方便用）。
    pub fn type_name(&self) -> &'static str {
        self.value.type_name()
    }
}

/// 迭代里的「当前一项」（可写）。
#[derive(Debug)]
pub struct EntryMut<'a> {
    /// 对象成员名；数组元素是 `None`。
    pub key: Option<&'a str>,
    /// 数组下标；对象成员是 `None`。
    pub index: Option<usize>,
    /// 元素 / 成员值的可变引用。
    pub value: &'a mut Value,
}

impl<'a> EntryMut<'a> {
    /// 值的类型名。
    pub fn type_name(&self) -> &'static str {
        self.value.type_name()
    }
}

enum EntriesInner<'a> {
    Array { items: std::slice::Iter<'a, Value>, position: usize },
    Object(std::slice::Iter<'a, Member>),
    Empty,
}

/// [`Value::entries`] 给出的只读迭代器。
pub struct Entries<'a> {
    inner: EntriesInner<'a>,
}

impl<'a> Iterator for Entries<'a> {
    type Item = Entry<'a>;

    fn next(&mut self) -> Option<Self::Item> {
        match &mut self.inner {
            EntriesInner::Array { items, position } => {
                let value = items.next()?;
                let index = *position;
                *position += 1;
                Some(Entry { key: None, index: Some(index), value })
            }
            EntriesInner::Object(members) => {
                let member = members.next()?;
                Some(Entry { key: Some(member.key.as_str()), index: None, value: &member.value })
            }
            EntriesInner::Empty => None,
        }
    }

    fn size_hint(&self) -> (usize, Option<usize>) {
        match &self.inner {
            EntriesInner::Array { items, .. } => items.size_hint(),
            EntriesInner::Object(members) => members.size_hint(),
            EntriesInner::Empty => (0, Some(0)),
        }
    }
}

impl ExactSizeIterator for Entries<'_> {}

enum EntriesInnerMut<'a> {
    Array { items: std::slice::IterMut<'a, Value>, position: usize },
    Object(std::slice::IterMut<'a, Member>),
    Empty,
}

/// [`Value::entries_mut`] 给出的可写迭代器。
pub struct EntriesMut<'a> {
    inner: EntriesInnerMut<'a>,
}

impl<'a> Iterator for EntriesMut<'a> {
    type Item = EntryMut<'a>;

    fn next(&mut self) -> Option<Self::Item> {
        match &mut self.inner {
            EntriesInnerMut::Array { items, position } => {
                let value = items.next()?;
                let index = *position;
                *position += 1;
                Some(EntryMut { key: None, index: Some(index), value })
            }
            EntriesInnerMut::Object(members) => {
                // 把 `&mut Member` 拆成「名字（只读）+ 值（可写）」两个不相交的借用。
                let Member { key, value } = members.next()?;
                Some(EntryMut { key: Some(key.as_str()), index: None, value })
            }
            EntriesInnerMut::Empty => None,
        }
    }

    fn size_hint(&self) -> (usize, Option<usize>) {
        match &self.inner {
            EntriesInnerMut::Array { items, .. } => items.size_hint(),
            EntriesInnerMut::Object(members) => members.size_hint(),
            EntriesInnerMut::Empty => (0, Some(0)),
        }
    }
}

impl ExactSizeIterator for EntriesMut<'_> {}

impl<'a> IntoIterator for &'a Value {
    type Item = Entry<'a>;
    type IntoIter = Entries<'a>;

    fn into_iter(self) -> Self::IntoIter {
        self.entries()
    }
}

impl<'a> IntoIterator for &'a mut Value {
    type Item = EntryMut<'a>;
    type IntoIter = EntriesMut<'a>;

    fn into_iter(self) -> Self::IntoIter {
        self.entries_mut()
    }
}

// ---------------------------------------------------------------------------
//  操作错误
// ---------------------------------------------------------------------------

/// 取值失败的原因（对应 C++ 版 `requireArray` / `at` 抛出的异常）。
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Error {
    /// 当前值是数组之外的类型。
    NotArray,
    /// 当前值是对象之外的类型。
    NotObject,
    /// 数组下标越界。
    OutOfRange {
        /// 请求的下标。
        index: usize,
        /// 数组长度。
        len: usize,
    },
}

impl fmt::Display for Error {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Error::NotArray => formatter.write_str("当前 JSON 值不是数组"),
            Error::NotObject => formatter.write_str("当前 JSON 值不是对象"),
            Error::OutOfRange { index, len } => {
                write!(formatter, "JSON 数组下标越界：{index}（长度 {len}）")
            }
        }
    }
}

impl std::error::Error for Error {}

// ---------------------------------------------------------------------------
//  相等：按 JSON 语义（数字跨类型、对象不看顺序）
// ---------------------------------------------------------------------------

/// 相等比较（对应 C++ 的 `operator==`）。
///
/// 规则与 C++ 版逐条相同：
///
///   1. 两个数字：统一按 `f64` 比，所以 `1` == `1.0`；
///   2. 类型不同（`std::variant` 的 index 不同）就不等；
///   3. 数组逐元素比；对象按「成员集合」比（**顺序不影响相等**）；
///   4. 字符串、布尔各自按值比；剩下的（都是 `null`）相等。
///
/// ```
/// use json::{parse, Value};
///
/// assert_eq!(parse("1").value.unwrap(), parse("1.0").value.unwrap());
/// assert_eq!(parse(r#"{"a":1,"b":2}"#).value.unwrap(), parse(r#"{"b":2,"a":1}"#).value.unwrap());
/// assert_ne!(parse("[1,2]").value.unwrap(), parse("[2,1]").value.unwrap());
/// assert_eq!(Value::null(), Value::null());
/// ```
impl PartialEq for Value {
    fn eq(&self, other: &Self) -> bool {
        equals_value(self, other)
    }
}

/// [`Value`] 的相等判定（数字跨 int / double，对象不看顺序）。
fn equals_value(left: &Value, right: &Value) -> bool {
    // 1. 数字：统一按 double 比（1 与 1.0 相等）。
    if left.is_number() && right.is_number() {
        return left.as_double() == right.as_double();
    }

    // 2. 类型不同就不等。
    if std::mem::discriminant(left) != std::mem::discriminant(right) {
        return false;
    }

    // 3. 逐个类型比。
    match (left, right) {
        (Value::Array(items), Value::Array(others)) => {
            items.len() == others.len()
                && items.iter().zip(others.iter()).all(|(a, b)| equals_value(a, b))
        }
        (Value::Object(object), Value::Object(other)) => {
            // 对象按「成员集合」比：顺序不影响相等。
            object.len() == other.len()
                && object.iter().all(|member| match other.get(&member.key) {
                    Some(peer) => equals_value(&member.value, peer),
                    None => false,
                })
        }
        (Value::String(text), Value::String(other)) => text == other,
        (Value::Bool(flag), Value::Bool(other)) => flag == other,
        // 剩下的只可能是两个 null。
        _ => true,
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

impl From<bool> for Value {
    fn from(flag: bool) -> Self {
        Value::Bool(flag)
    }
}

impl From<i32> for Value {
    fn from(number: i32) -> Self {
        Value::Int(i64::from(number))
    }
}

impl From<i64> for Value {
    fn from(number: i64) -> Self {
        Value::Int(number)
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

impl From<f32> for Value {
    fn from(number: f32) -> Self {
        Value::Double(f64::from(number))
    }
}

impl From<f64> for Value {
    fn from(number: f64) -> Self {
        Value::Double(number)
    }
}

impl From<String> for Value {
    fn from(text: String) -> Self {
        Value::String(text)
    }
}

impl From<&str> for Value {
    fn from(text: &str) -> Self {
        Value::String(text.to_string())
    }
}

impl From<Array> for Value {
    fn from(items: Array) -> Self {
        Value::Array(items)
    }
}

impl From<Object> for Value {
    fn from(members: Object) -> Self {
        Value::Object(members)
    }
}
