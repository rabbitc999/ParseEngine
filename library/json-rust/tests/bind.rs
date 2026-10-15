//! 自检之二：模型投射（`json_model!` / `json_enum!` / `encode` / `decode`）。
//!
//! 用例与 C++ 版 `library/json/test_json.cpp` 的后半段一一对应：嵌套模型、
//! 容器字段、可选字段、枚举、字段顺序、往返稳定、以及带 JSON 路径的诊断。
//!
//! ```sh
//! cargo test --manifest-path library/json-rust/Cargo.toml
//! ```
//!
//! C++ 版里有两条 `hasBinding<T>` 断言（"没绑定的类型必须返回 false"），Rust 版
//! **没有对应写法**：绑定就是 trait 实现，没实现 [`JsonModel`] 的类型连编译都过不去
//! ——比运行期返回 `false` 更早拦住。这里改成正面覆盖：字段表内容、字段顺序、
//! 以及"每个字段类型都真的能编能解"。

use std::collections::BTreeMap;

use json::{
    decode, decode_with, encode, json_enum, json_model, parse, DecodeOptions, FromJson, JsonModel,
    ToJson, Value,
};

// ---------------------------------------------------------------------------
//  测试用的模型
// ---------------------------------------------------------------------------

/// 一个嵌套模型：点。
#[derive(Clone, Debug, Default, PartialEq)]
struct Point {
    x: i64,
    y: i64,
}
json_model!(Point, "x" => x, "y" => y);

/// 形状的类型（枚举按整数投射）。
///
/// `json_enum!` 顺带给它一个 `Default`（第一个变体），所以 `Shape` 也能
/// `#[derive(Default)]`。
#[derive(Clone, Copy, Debug, PartialEq)]
enum Kind {
    Line,
    Polygon,
}
json_enum!(Kind, Line = 0, Polygon = 1);

/// 一个模型：嵌套模型 + 容器 + optional + map。
#[derive(Clone, Debug, Default, PartialEq)]
struct Shape {
    id: String,
    kind: Kind,
    origin: Point,
    points: Vec<Point>,
    scale: Option<f64>,
    labels: BTreeMap<String, String>,
}
json_model!(
    Shape,
    "id" => id,
    "kind" => kind,
    "origin" => origin,
    "points" => points,
    "scale" => scale,
    "labels" => labels
);

/// 解析一份文本；失败就当测试失败。
fn value(text: &str) -> Value {
    let doc = parse(text);
    assert!(doc.ok(), "解析失败：{:?}", doc.diagnostics);
    doc.value.unwrap_or(Value::Null)
}

/// 造一份"完整"的形状，往返用例都用它。
fn sample() -> Shape {
    let mut labels = BTreeMap::new();
    labels.insert("cn".to_string(), "多边形".to_string());
    Shape {
        id: "shape-1".to_string(),
        kind: Kind::Polygon,
        origin: Point { x: 1, y: 2 },
        points: vec![Point { x: 0, y: 0 }, Point { x: 3, y: 4 }],
        scale: Some(2.5),
        labels,
    }
}

// ---------------------------------------------------------------------------
//  往返
// ---------------------------------------------------------------------------

/// 编码 / 解码往返。
#[test]
fn bind_round_trip() {
    let shape = sample();

    // 1. 模型 → JSON：字段顺序就是声明顺序。
    let encoded = encode(&shape);
    assert!(encoded.is_object());
    assert_eq!(encoded.get("id").unwrap().as_str(), Some("shape-1"));
    assert_eq!(encoded.get("kind").unwrap().as_int(), Some(1));
    assert!(encoded.get("origin").unwrap().is_object());
    assert_eq!(encoded.get("origin").unwrap().get("x").unwrap().as_int(), Some(1));
    assert_eq!(encoded.get("origin").unwrap().get("y").unwrap().as_int(), Some(2));
    assert!(encoded.get("points").unwrap().is_array());
    assert_eq!(encoded.get("points").unwrap().len(), 2);
    assert_eq!(encoded.at_path("points[1]").unwrap().get("y").unwrap().as_int(), Some(4));
    assert_eq!(encoded.get("scale").unwrap().as_double(), Some(2.5));
    assert_eq!(encoded.at_path("labels.cn").unwrap().as_str(), Some("多边形"));
    let order: String = encoded
        .as_object()
        .unwrap()
        .iter()
        .map(|member| format!("{},", member.key))
        .collect();
    assert_eq!(order, "id,kind,origin,points,scale,labels,");

    // 2. JSON → 模型。
    let decoded = decode::<Shape>(&encoded);
    assert!(decoded.ok());
    let back = decoded.value.unwrap_or_default();
    assert_eq!(back, shape);
    assert_eq!(back.id, "shape-1");
    assert_eq!(back.kind, Kind::Polygon);
    assert_eq!(back.origin.x, 1);
    assert_eq!(back.origin.y, 2);
    assert_eq!(back.points.len(), 2);
    assert_eq!(back.points[1].x, 3);
    assert_eq!(back.scale, Some(2.5));
    assert_eq!(back.labels.get("cn").map(String::as_str), Some("多边形"));

    // 3. 再编一遍：与第一次逐字节相同（往返稳定）。
    assert_eq!(encode(&back).dump_indent(2), encoded.dump_indent(2));

    // 4. JSON 文本直接解到模型：数字字符串当枚举、缺可选字段算空。
    let from_text = decode::<Shape>(&value(
        r#"{
        "id": "s2", "kind": "1", "origin": {"x": 5, "y": 6},
        "points": [], "labels": {}
    }"#,
    ));
    assert!(from_text.ok());
    let from_text = from_text.value.unwrap_or_default();
    assert_eq!(from_text.origin.x, 5);
    assert_eq!(from_text.kind, Kind::Polygon);
    assert_eq!(from_text.scale, None);

    // 5. `require()`：成功给值、失败给诊断（对应 C++ 抛异常的 require）。
    assert_eq!(decode::<Shape>(&encoded).require().map(|item| item.id), Ok("shape-1".to_string()));
    assert!(decode::<Shape>(&value("[]")).require().is_err());
}

/// 解码诊断：缺字段、类型不对、多余字段，都要带 JSON 路径。
#[test]
fn bind_diagnostics() {
    // 1. 缺必填字段。
    let missing = decode::<Shape>(&value(r#"{"id":"x"}"#));
    assert!(!missing.ok());
    assert!(
        missing
            .diagnostics
            .iter()
            .any(|item| item.code == "missing-field" && item.path == "kind"),
        "{:?}",
        missing.diagnostics
    );
    assert!(missing.diagnostics.iter().any(|item| item.path == "points"));
    assert_eq!(missing.first_error().map(|item| item.code.as_str()), Some("missing-field"));

    // 2. 数组元素里类型不对：路径带下标。
    let broken = decode::<Shape>(&value(
        r#"{
        "id": "x", "kind": 0, "origin": {"x": 1, "y": 2},
        "points": [{"x": 0, "y": 0}, {"x": "bad", "y": 1}], "labels": {}
    }"#,
    ));
    assert!(!broken.ok());
    assert_eq!(broken.diagnostics[0].path, "points[1].x");
    assert_eq!(broken.diagnostics[0].code, "type-mismatch");
    assert_eq!(
        broken.diagnostics[0].to_string(),
        "points[1].x: 期望整数，实际是 string [type-mismatch]"
    );

    // 3. 多出来的字段：`strict_unknown` 才报。
    let extra = r#"{
        "id": "x", "kind": 0, "origin": {}, "points": [], "labels": {}, "extra": 1
    }"#;
    let strict = DecodeOptions { strict_unknown: true, strict_missing: false, ..Default::default() };
    let result = decode_with::<Shape>(&value(extra), strict);
    assert!(!result.ok());
    assert_eq!(result.diagnostics[0].path, "extra");
    assert_eq!(result.diagnostics[0].code, "unknown-field");
    assert_eq!(
        result.diagnostics[0].to_string(),
        "extra: 模型里没有字段 \"extra\" [unknown-field]"
    );

    let lenient = DecodeOptions { strict_unknown: false, ..strict };
    assert!(decode_with::<Shape>(&value(extra), lenient).ok());

    // 4. 整体类型不对：报在根路径。
    let wrong = decode::<Shape>(&value("[]"));
    assert!(!wrong.ok());
    assert_eq!(wrong.diagnostics[0].path, "$");
    assert_eq!(wrong.diagnostics[0].code, "type-mismatch");
    assert!(wrong.diagnostics[0].message.contains("期望对象"));
    assert_eq!(wrong.diagnostics[0].line, 1);

    // 5. 严格档下缺字段报错、宽松档下不报。
    let incomplete = r#"{"id":"x","kind":0,"origin":{"x":1,"y":2},"points":[],"labels":{}}"#;
    assert!(decode::<Shape>(&value(incomplete)).ok());
    assert!(decode_with::<Shape>(&value(incomplete), DecodeOptions::lenient()).ok());
    assert!(decode::<Shape>(&value(r#"{"id":"x"}"#)).diagnostics.len() >= 3);
}

/// 投射之后的模型照样能喂迭代器适配器（对应 C++ 那句"模型也能用 STL 算法"）。
#[test]
fn bind_with_algorithms() {
    let shapes = decode::<Vec<Shape>>(&value(
        r#"[
        {"id":"a","kind":0,"origin":{"x":0,"y":0},"points":[{"x":1,"y":1}],"labels":{}},
        {"id":"b","kind":1,"origin":{"x":0,"y":0},"points":[{"x":2,"y":2},{"x":3,"y":3}],"labels":{}}
    ]"#,
    ));
    assert!(shapes.ok());
    let shapes = shapes.value.unwrap_or_default();
    assert_eq!(shapes.len(), 2);

    // 直接用适配器处理解出来的模型。
    let polygons = shapes.iter().filter(|shape| shape.kind == Kind::Polygon).count();
    assert_eq!(polygons, 1);
    let points: usize = shapes.iter().map(|shape| shape.points.len()).sum();
    assert_eq!(points, 3);
    assert!(shapes.iter().any(|shape| shape.id == "b"));

    // 容器也能被投射成 JSON 数组。
    let encoded = encode(&shapes);
    assert!(encoded.is_array());
    assert_eq!(encoded.len(), 2);
    // 注意用 `at` 而不是 `at_path("[1].id")`：`[n]` 后面跟 `.` 是找不到的
    // （与 C++ 版实测一致，见 `技术报告.md`）。
    assert_eq!(encoded.at(1).unwrap().get("id").unwrap().as_str(), Some("b"));
    assert!(encoded.at_path("[1].id").is_none());
}

/// 字段类型矩阵：每种支持的类型都真的能编能解。
#[test]
fn field_type_matrix() {
    #[derive(Clone, Debug, Default, PartialEq)]
    struct Everything {
        flag: bool,
        count: i64,
        small: u32,
        ratio: f64,
        approx: f32,
        text: String,
        tags: Vec<String>,
        lookup: BTreeMap<String, i64>,
        maybe: Option<i64>,
        kind: Kind,
        raw: Value,
        inner: Point,
    }
    json_model!(
        Everything,
        "flag" => flag,
        "count" => count,
        "small" => small,
        "ratio" => ratio,
        "approx" => approx,
        "text" => text,
        "tags" => tags,
        "lookup" => lookup,
        "maybe" => maybe,
        "kind" => kind,
        "raw" => raw,
        "inner" => inner
    );

    // `field_names()` 就是声明顺序（`strict_unknown` 靠它）。
    assert_eq!(
        <Everything as JsonModel>::field_names(),
        ["flag", "count", "small", "ratio", "approx", "text", "tags", "lookup", "maybe", "kind", "raw", "inner"]
    );

    let mut lookup = BTreeMap::new();
    lookup.insert("k".to_string(), 9);
    let value_of = Everything {
        flag: true,
        count: -3,
        small: 7,
        ratio: 1.5,
        approx: 0.5,
        text: "hi".to_string(),
        tags: vec!["a".to_string(), "b".to_string()],
        lookup,
        maybe: None,
        kind: Kind::Polygon,
        raw: Value::object([("z", Value::array([1, 2]))]),
        inner: Point { x: 1, y: 2 },
    };

    // 编码：每种类型写成对应的 JSON 形状；`None` → null；`Value` 原样透传。
    assert_eq!(
        encode(&value_of).dump(),
        r#"{"flag":true,"count":-3,"small":7,"ratio":1.5,"approx":0.5,"text":"hi","tags":["a","b"],"lookup":{"k":9},"maybe":null,"kind":1,"raw":{"z":[1,2]},"inner":{"x":1,"y":2}}"#
    );

    // 解码：整数 / 浮点 / 枚举都认数字字符串；`null` → 空；缺可选字段也不算错。
    let parsed = decode::<Everything>(&value(
        r#"{
        "flag": true, "count": "-3", "small": 7, "ratio": "1.5", "approx": 0.5,
        "text": "hi", "tags": ["a", "b"], "lookup": {"k": 9},
        "kind": "1", "raw": {"z": [1, 2]}, "inner": {"x": 1, "y": 2}
    }"#,
    ));
    assert!(parsed.ok(), "{:?}", parsed.diagnostics);
    let parsed = parsed.value.unwrap_or_default();
    assert_eq!(parsed.count, -3);
    assert_eq!(parsed.ratio, 1.5);
    assert_eq!(parsed.approx, 0.5);
    assert_eq!(parsed.maybe, None);
    assert_eq!(parsed.kind, Kind::Polygon);
    assert_eq!(parsed.raw, value_of.raw);
    assert_eq!(parsed.inner, value_of.inner);

    // 布尔不认字符串（与 C++ 版一致）。
    let bad_bool = decode::<Everything>(&value(
        r#"{"flag":"true","count":1,"small":1,"ratio":1.0,"approx":0.5,"text":"x","tags":[],
            "lookup":{},"maybe":null,"kind":0,"raw":null,"inner":{"x":1,"y":2}}"#,
    ));
    assert!(!bad_bool.ok());
    assert_eq!(bad_bool.diagnostics[0].path, "flag");
    assert!(bad_bool.diagnostics[0].message.contains("期望布尔"));
}

/// 枚举的另外几种用法：整数映射、名字、对不上的数字。
#[test]
fn enum_bindings() {
    assert_eq!(Kind::Line.as_json_int(), 0);
    assert_eq!(Kind::Polygon.as_json_int(), 1);
    assert_eq!(Kind::Line.as_json_name(), "Line");
    assert_eq!(Kind::Polygon.as_json_name(), "Polygon");
    assert_eq!(Kind::from_json_name("Polygon"), Some(Kind::Polygon));
    assert_eq!(Kind::from_json_name("polygon"), None);
    assert_eq!(<Kind as Default>::default(), Kind::Line);
    assert_eq!(encode(&Kind::Polygon).dump(), "1");
    // 想按名字写就显式调 `as_json_name()`（C++ 版那个从没接线的 enumAsString 开关的替代）。
    assert_eq!(encode(&Kind::Polygon.as_json_name()).dump(), "\"Polygon\"");
    // 数字对不上 → 退回第一个变体（C++ 那边是 static_cast 强转）。
    assert_eq!(Kind::from_json_int(9), Kind::Line);
    assert_eq!(decode::<Kind>(&value("9")).value, Some(Kind::Line));
    // 非数字的字符串报诊断（消息与 C++ 版逐字相同）。
    let broken = decode::<Kind>(&value(r#""polygon""#));
    assert!(!broken.ok());
    assert_eq!(broken.diagnostics[0].message, "期望枚举，实际是字符串 \"polygon\"");
    let array = decode::<Kind>(&value("[]"));
    assert_eq!(array.diagnostics[0].message, "期望枚举，实际是 array");
}

/// 只用于**编码**的类型：C++ 版对 `string_view` 字段能编不能解（解码报
/// `unsupported-target`）；Rust 版干脆不给 `&str` 实现 `FromJson`——编译期就拦住。
#[test]
fn encode_only_fields() {
    /// 借用式模型（`&str` 字段）。
    struct Borrowed<'a> {
        name: &'a str,
        size: i64,
    }

    impl ToJson for Borrowed<'_> {
        fn to_json(&self) -> Value {
            Value::object([
                ("name", Value::string(self.name)),
                ("size", Value::int(self.size)),
            ])
        }
    }

    let model = Borrowed { name: "calc", size: 3 };
    assert_eq!(encode(&model).dump(), r#"{"name":"calc","size":3}"#);

    // 字符串字段按值解：`String` 有 `FromJson`，借用没有。
    let text: Result<String, ()> = FromJson::from_json(&value(r#""hi""#), &mut json::Context {
        options: DecodeOptions::default(),
        path: String::new(),
        diagnostics: &mut Vec::new(),
    });
    assert_eq!(text, Ok("hi".to_string()));
}
