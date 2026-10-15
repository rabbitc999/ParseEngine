//! 自检之一：解析 / 序列化 / 对象与值 / 路径 / 比较 / 迭代。
//!
//! 用例与 C++ 版 `library/json/test_json.cpp` 一一对应，**期望字符串逐字相同**
//! ——这是"翻译没走样"的主要证据。
//!
//! ```sh
//! cargo test --manifest-path library/json-rust/Cargo.toml
//! ```
//!
//! 两版写法的对应关系：
//!
//! | C++ 版（`test_json.cpp`） | 本文件 |
//! | --- | --- |
//! | `CHECK(cond)` / `checkText(title, actual, expected)` | `assert!` / `assert_eq!`（每组一个 `#[test]`，失败时 cargo 直接点出是哪一条） |
//! | `std::count_if` / `find_if` / `any_of` / `none_of` | `iter().filter().count()` / `.find()` / `.any()` / `!….any()` |
//! | `std::accumulate` / `transform` / `for_each` | `.map().sum()` / `.map().collect()` / `for` |
//! | 抛 `std::domain_error` / `std::out_of_range` 再 `catch` | `Result` 的 `Err(Error::NotArray / OutOfRange { .. })` |
//! | `doc.value->…` / `object.find(k)->…` | `doc.value.unwrap().…` / `root.get(k).unwrap().…` |

use json::{parse, parse_with, Error, ParseOptions, Value};

// ---------------------------------------------------------------------------
//  小工具
// ---------------------------------------------------------------------------

/// 解析一份文本；失败就当测试失败。
fn value(text: &str) -> Value {
    let doc = parse(text);
    assert!(doc.ok(), "解析失败：{:?}", doc.diagnostics);
    doc.value.unwrap_or(Value::Null)
}

/// 带选项解析。
fn value_with(text: &str, options: ParseOptions) -> Value {
    let doc = parse_with(text, options);
    assert!(doc.ok(), "解析失败：{:?}", doc.diagnostics);
    doc.value.unwrap_or(Value::Null)
}

/// 第一条诊断（对应 C++ 版 `doc.diagnostics.front()`）。
fn first_error(text: &str) -> json::Diagnostic {
    let doc = parse(text);
    assert!(!doc.ok(), "这份输入本该解析失败：{text}");
    doc.diagnostics.into_iter().next().unwrap_or_default()
}

/// 第一条诊断的短码（对应 C++ 版 `doc.diagnostics.front().code`）。
fn first_code(text: &str) -> String {
    first_error(text).code
}

// ---------------------------------------------------------------------------
//  解析与序列化
// ---------------------------------------------------------------------------

/// 标量：null / bool / 整数 / 浮点 / 字符串。
#[test]
fn scalars() {
    let root = value(
        r#"{
        "nothing": null, "yes": true, "no": false,
        "count": 42, "big": 9007199254740993, "ratio": 1.5, "text": "hi"
    }"#,
    );
    assert!(root.is_object());
    assert!(root.get("nothing").unwrap().is_null());
    assert_eq!(root.get("yes").unwrap().as_bool(), Some(true));
    assert_eq!(root.get("no").unwrap().as_bool(), Some(false));
    assert!(root.get("count").unwrap().is_int());
    assert_eq!(root.get("count").unwrap().as_int(), Some(42));
    // 64 位整数不丢精度。
    assert_eq!(root.get("big").unwrap().as_int(), Some(9_007_199_254_740_993));
    assert!(root.get("ratio").unwrap().is_double());
    assert_eq!(root.get("ratio").unwrap().as_double(), Some(1.5));
    assert_eq!(root.get("text").unwrap().as_str(), Some("hi"));
    assert_eq!(root.get("text").unwrap().string_or("?"), "hi");
    assert_eq!(root.get("count").unwrap().string_or("?"), "?");
    assert_eq!(root.get("count").unwrap().int_or(7), 42);
    assert_eq!(root.type_name(), "object");

    // 类型判断彼此互斥。
    assert!(root.get("count").unwrap().is_number());
    assert!(!root.get("count").unwrap().is_double());
    assert!(!root.get("text").unwrap().is_number());
    assert!(root.get("nothing").unwrap().is_empty());
}

/// 转义与 Unicode：`\uXXXX`、代理对、控制字符。
#[test]
fn strings() {
    let doc = parse(r#""a\n\t\"\u4e2d\ud83d\ude00""#);
    assert!(doc.ok());
    assert_eq!(doc.value.as_ref().unwrap().as_str(), Some("a\n\t\"\u{4e2d}\u{1F600}"));

    // 序列化时控制字符要转义，中文原样透传。
    let text = Value::string("中\n\t\"x\"");
    assert_eq!(text.dump(), "\"中\\n\\t\\\"x\\\"\"");

    // 其余控制字符写成 \u00xx。
    assert_eq!(Value::string("\u{1}\u{1f}").dump(), "\"\\u0001\\u001f\"");

    // 往返一致。
    assert_eq!(value(&text.dump()), text);
}

/// 数字：整数 / 浮点 / 大数 / 往返。
#[test]
fn numbers() {
    assert!(value("1").is_int());
    assert_eq!(value("-0").as_int(), Some(0));
    assert!(value("1e3").is_double());
    assert_eq!(value("1e3").as_double(), Some(1000.0));
    assert_eq!(value("0.5").as_double(), Some(0.5));

    // 浮点往返：0.1 二进制不能精确表示，但必须往返一致。
    let ratio = Value::double(0.1);
    let back = value(&ratio.dump());
    assert_eq!(back.as_double(), Some(0.1));
    assert_eq!(Value::double(2.0).dump(), "2.0");

    // 大 / 极小量级换成指数写法（和 C++ 的 %g 一样切），也是合法 JSON。
    assert_eq!(value("1e300").dump(), "1e+300");
    assert_eq!(value("0.0001").dump(), "0.0001");

    // 超过 int64 的整数退化成浮点。
    let huge = value("123456789012345678901234567890");
    assert!(huge.is_double());

    // JSON 里没有 NaN / Infinity：写成 null。
    assert_eq!(Value::double(f64::NAN).dump(), "null");
    assert_eq!(Value::double(f64::INFINITY).dump(), "null");
}

/// `as_int` 的边界（对应 C++ 版 `main()` 开头那两条断言）。
#[test]
fn int_boundaries() {
    assert_eq!(Value::double(9_223_372_036_854_775_808.0).as_int(), None);
    assert!(Value::double(-9_223_372_036_854_775_808.0).as_int().is_some());
}

/// 解析错误：短码与行列信息。
#[test]
fn errors() {
    // 1. 空输入 / 半截输入。
    assert_eq!(first_code(""), "unexpected-end");
    assert_eq!(first_code("{"), "expected-key");
    assert_eq!(first_code(r#"{"a" 1}"#), "expected-colon");
    assert_eq!(first_code(r#"{"a":1 "b":2}"#), "expected-comma");
    assert_eq!(first_code("[1,2"), "expected-comma");
    assert_eq!(first_code("nul"), "unexpected-token");
    assert_eq!(first_code("01"), "bad-number");
    assert_eq!(first_code("1."), "bad-number");
    assert_eq!(first_code("\"abc"), "unterminated-string");
    assert_eq!(first_code(r#""\x""#), "bad-escape");
    assert_eq!(first_code(r#""\u12""#), "bad-escape");
    assert_eq!(first_code(r#""\ud83d""#), "bad-surrogate");
    assert_eq!(first_code("{} {}"), "unexpected-token");
    assert_eq!(first_code("[1,]"), "trailing-comma");
    assert_eq!(first_code("// c\n{}"), "unexpected-token");
    assert_eq!(first_code("\"a\nb\""), "control-char");

    // 2. 行列信息要对得上（列号按字节算）。
    let broken = first_error("{\n  \"a\": ,\n}");
    assert_eq!(broken.line, 2);
    assert_eq!(broken.column, 8);
    assert_eq!(broken.offset, 9);
    assert_eq!(broken.to_string(), "2:8: 不认识的字符 ',' [unexpected-token]");

    // 3. 打开选项后注释与尾逗号都能过，块注释没闭合照样报错。
    let loose = ParseOptions::loose();
    let parsed = value_with("// 注释\n{\"a\": [1, 2,], /* 块注释 */}", loose);
    assert_eq!(parsed.at_path("a[1]").unwrap().as_int(), Some(2));
    // 注释没打开时 `/` 只是普通字符；打开了才谈得上"没闭合"。
    assert_eq!(first_code("/* never"), "unexpected-token");
    let unterminated = parse_with("/* never", loose);
    assert_eq!(unterminated.diagnostics.first().map(|item| item.code.as_str()),
               Some("unterminated-comment"));
}

/// 对象：保序、查找、增删改。
#[test]
fn object() {
    let doc = value(r#"{"b":1,"a":2,"c":3}"#);
    let object = doc.as_object().unwrap();

    // 1. 保序：遍历顺序就是文件里的顺序。
    let mut order = String::new();
    for member in object.iter() {
        order.push_str(&member.key);
    }
    assert_eq!(order, "bac");
    assert_eq!(object.len(), 3);
    assert!(!object.is_empty());

    // 2. 查找。
    assert!(object.contains("a"));
    assert!(!object.contains("z"));
    assert_eq!(object.count("a"), 1);
    assert_eq!(object.count("z"), 0);
    assert!(object.find("a").is_some());
    assert!(object.find("z").is_none());
    assert_eq!(object.get("c").unwrap().as_int(), Some(3));
    assert!(object.get("z").is_none());
    assert_eq!(object.get("c").unwrap().type_name(), "int");

    // 3. 修改。
    let mut copy = doc.clone();
    let mutable = copy.as_object_mut().unwrap();
    *mutable.get_or_insert("a") = Value::int(9); // 覆盖
    assert_eq!(mutable.get("a").unwrap().as_int(), Some(9));
    *mutable.get_or_insert("new") = Value::string("x"); // 新增
    assert_eq!(mutable.len(), 4);
    assert!(mutable.contains("new"));
    assert!(mutable.erase("new"));
    assert!(!mutable.erase("new"));
    assert_eq!(mutable.len(), 3);
    mutable.insert("z", true);
    assert_eq!(mutable.get("z").unwrap().as_bool(), Some(true));
    assert_eq!(mutable.len(), 4);
    mutable.clear();
    assert!(mutable.is_empty());
}

/// 值：类型判断、可选取值、数组操作、路径查询。
#[test]
fn value_api() {
    let root = value(
        r#"{
        "user": {"name": "糖糖", "tags": ["a", "b", "c"], "scores": [1, 2, 3]}
    }"#,
    );

    // 1. 路径查询。
    assert_eq!(root.at_path("user.name").unwrap().as_str(), Some("糖糖"));
    assert_eq!(root.at_path("user.tags[1]").unwrap().as_str(), Some("b"));
    assert_eq!(root.at_path("user.scores[2]").unwrap().as_int(), Some(3));
    assert!(root.at_path("user.nope").is_none());
    assert!(root.at_path("user.tags[9]").is_none());
    assert!(root.at_path("user.tags[]").is_none());
    assert!(root.at_path("user..name").is_none());
    assert!(root.at_path("user.").is_none());
    assert!(root.at_path(".user").is_none());
    assert!(root.at_path("user.tags[1").is_none());
    assert_eq!(root.at_path("").unwrap(), &root);

    // 下标只能收尾：`[n]` 后面跟 `.` 判为找不到（C++ 版实测也一样，见技术报告）。
    assert_eq!(value("[1,2]").at_path("[1]").unwrap().as_int(), Some(2));
    assert!(value("[1,2]").at_path("[2]").is_none());
    assert!(value(r#"{"a":[{"b":1}]}"#).at_path("a[0].b").is_none());
    assert_eq!(
        value(r#"{"a":[{"b":1}]}"#).at_path("a[0]").unwrap().get("b").unwrap().as_int(),
        Some(1)
    );

    // 2. 可选取值。
    assert!(root.get("user").unwrap().as_object().is_some());
    assert!(root.get("user").unwrap().as_array().is_none());
    assert!(root.get("user").unwrap().as_str().is_none());
    assert_eq!(root.get("user").unwrap().int_or(-1), -1);
    assert_eq!(Value::double(3.0).as_int(), Some(3));
    assert_eq!(Value::double(3.5).as_int(), None);
    assert_eq!(Value::int(3).as_double(), Some(3.0));
    assert_eq!(Value::int(3).double_or(0.5), 3.0); // 整数也算"取得到浮点"
    assert_eq!(Value::string("x").double_or(0.5), 0.5); // 取不到才用兜底

    // 3. 构造：数组 / 对象。
    let mut list = Value::array([1, 2, 3]);
    assert!(list.is_array());
    assert_eq!(list.len(), 3);
    let record = Value::object([("a", Value::int(1)), ("b", Value::string("x"))]);
    assert!(record.is_object());
    assert_eq!(record.get("b").unwrap().as_str(), Some("x"));

    // 4. 数组修改。
    list.push(4).unwrap();
    list.push("five").unwrap();
    assert_eq!(list.len(), 5);
    assert_eq!(list.at(3).unwrap().as_int(), Some(4));
    assert_eq!(list.at(4).unwrap().as_str(), Some("five"));
    assert_eq!(list.as_array().unwrap()[4].as_str(), Some("five"));
    list.clear();
    assert!(list.is_empty());

    // 5. 类型不对时的行为（对应 C++ 抛 out_of_range / domain_error）。
    assert_eq!(list.at(0), Err(Error::OutOfRange { index: 0, len: 0 }));
    assert_eq!(root.require_array(), Err(Error::NotArray));
    assert_eq!(Value::int(1).require_object(), Err(Error::NotObject));
    assert_eq!(root.at(0), Err(Error::NotArray));
    assert_eq!(Error::NotArray.to_string(), "当前 JSON 值不是数组");
    assert_eq!(Error::NotObject.to_string(), "当前 JSON 值不是对象");

    // 6. 路径查询的可写版本。
    let mut mutable = root.clone();
    *mutable.at_path_mut("user.tags[0]").unwrap() = Value::string("A");
    assert_eq!(mutable.at_path("user.tags[0]").unwrap().as_str(), Some("A"));
    *mutable.get_or_insert("user").unwrap().get_or_insert("extra").unwrap() = Value::int(7);
    assert_eq!(mutable.at_path("user.extra").unwrap().as_int(), Some(7));
    assert!(mutable.at_path_mut("user.tags[9]").is_none());
}

/// 序列化：紧凑与缩进两种形态。
#[test]
fn dump() {
    let doc = value(r#"{"a":[1,2],"b":{"c":true},"d":"x"}"#);
    assert_eq!(doc.dump(), r#"{"a":[1,2],"b":{"c":true},"d":"x"}"#);
    let expected = r#"{
  "a": [
    1,
    2
  ],
  "b": {
    "c": true
  },
  "d": "x"
}"#;
    assert_eq!(doc.dump_indent(2), expected);
    assert_eq!(value("[{},[]]").dump(), "[{},[]]");
    assert_eq!(value("{}").dump(), "{}");
    assert_eq!(value("[]").dump(), "[]");

    // 缩进 0：换行但不缩进。
    assert_eq!(value(r#"{"a":1}"#).dump_indent(0), "{\n\"a\": 1\n}");

    // 往返：dump → parse 之后再 dump，逐字节一致。
    let once = doc.dump_indent(2);
    let again = value(&once);
    assert_eq!(again.dump_indent(2), once);

    // `Display` 就是紧凑输出（对应 C++ 的 `operator<<`）。
    assert_eq!(doc.to_string(), doc.dump());
}

/// 比较：数字跨类型、对象不看顺序。
#[test]
fn compare() {
    assert_eq!(value("1"), value("1.0"));
    assert_eq!(value(r#"{"a":1,"b":2}"#), value(r#"{"b":2,"a":1}"#));
    assert_ne!(value("[1,2]"), value("[2,1]"));
    assert_eq!(value("null"), value("null"));
    assert_ne!(value(r#""a""#), value(r#""b""#));
    assert_ne!(value("[]"), value("{}"));
    assert_ne!(value(r#"{"a":1}"#), value(r#"{"a":"1"}"#));
    assert_ne!(value("true"), value("1"));
    assert_ne!(value("null"), value("0"));
}

// ---------------------------------------------------------------------------
//  迭代器（对应 C++ 版「与 STL 算法一起用」那一段）
// ---------------------------------------------------------------------------

/// `for` 与迭代器适配器直接用在数组 / 对象上。
#[test]
fn iteration() {
    let root = value(r#"{"scores":[3,1,2],"names":["a","b"]}"#);
    let scores = root.get("scores").unwrap().as_array().unwrap();

    // 1. 范围 for：数组。
    let sum: i64 = scores.iter().filter_map(Value::as_int).sum();
    assert_eq!(sum, 6);

    // 2. `count_if` / `find_if` / `any_of` / `accumulate` / `none_of`。
    let threes = scores.iter().filter(|item| item.as_int() == Some(3)).count();
    assert_eq!(threes, 1);
    let found = scores.iter().find(|item| item.as_int() == Some(1)).unwrap();
    assert_eq!(found.as_int(), Some(1));
    assert!(scores.iter().any(|item| item.int_or(0) > 2));
    let total: i64 = scores.iter().map(|item| item.int_or(0)).sum();
    assert_eq!(total, 6);
    assert!(!scores.iter().any(Value::is_null));

    // 3. `transform`：把 JSON 数组变成 Rust 容器。
    let numbers: Vec<i64> = scores.iter().map(|item| item.int_or(0)).collect();
    assert_eq!(numbers, [3, 1, 2]);

    // 4. 对象：`Member` 公开字段 + 适配器。
    let record = value(r#"{"a":1,"b":2,"c":3}"#);
    let object = record.as_object().unwrap();
    let mut key_length = 0usize;
    for member in object.iter() {
        assert!(!member.key.is_empty());
        assert!(member.value.is_int());
        key_length += member.key.len();
    }
    assert_eq!(key_length, 3);
    let twos = object.iter().filter(|member| member.value.as_int() == Some(2)).count();
    assert_eq!(twos, 1);
    for member in object.iter() {
        assert!(!member.key.is_empty());
    }

    // 5. `Value` 自己也能迭代：数组给元素、对象给成员、标量是空区间。
    let mut visited = 0usize;
    for entry in &root {
        assert!(entry.key == Some("scores") || entry.key == Some("names"));
        assert!(entry.value.is_array());
        assert!(entry.index.is_none());
        visited += 1;
    }
    assert_eq!(visited, 2);
    for entry in root.get("scores").unwrap() {
        assert!(entry.key.is_none());
        assert!(entry.index.unwrap() < scores.len());
        assert!(entry.value.is_int());
        visited += 1;
    }
    assert_eq!(visited, 5);
    let scalar = Value::int(42);
    assert_eq!(scalar.entries().count(), 0);
    assert!(scalar.entries().next().is_none());
    assert_eq!(root.entries().count(), 2);
    assert_eq!(root.entries().len(), 2);

    // 6. 可写迭代：数组元素与对象成员都能改。
    let mut mutable = value(r#"{"a":1,"b":2}"#);
    for entry in &mut mutable {
        *entry.value = Value::int(entry.value.int_or(0) * 10);
    }
    assert_eq!(mutable.dump(), r#"{"a":10,"b":20}"#);
    let mut items = value("[1,2]");
    for entry in items.entries_mut() {
        assert!(entry.index.is_some());
        entry.value.clear();
    }
    assert_eq!(items.dump(), "[null,null]");
}
