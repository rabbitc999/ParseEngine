//! 自检之四：README 里那几段示例的**可运行副本**。
//!
//! README 是给人看的，代码块不会参与编译；这里把 README「快速开始 / 遍历 /
//! 模型投射 / 诊断」四段原样搬过来跑一遍，保证文档里的写法和断言都是真的
//! （改 README 的示例就顺手改这里，两边都留着 `assert`，跑不过会立刻发现）。
//!
//! ```sh
//! cargo test --manifest-path library/json-rust/Cargo.toml
//! ```

/// README「快速开始」。
#[test]
fn readme_quick_start() {
    use json::parse;

    let doc = parse(r#"{"name":"calc","count":2,"tokens":["+","-"]}"#);
    if !doc.ok() {
        eprintln!("{}", doc.diagnostics[0]);
    }
    let root = doc.value.unwrap();

    let name = root.get("name").unwrap().string_or("?");
    let count = root.get("count").unwrap().int_or(0);
    let first = root.at_path("tokens[0]");

    assert_eq!(name, "calc");
    assert_eq!(count, 2);
    assert_eq!(first.unwrap().string_or("?"), "+");
    assert_eq!(root.dump(), r#"{"name":"calc","count":2,"tokens":["+","-"]}"#);
    assert!(root.dump_indent(2).starts_with("{\n  \"name\": \"calc\","));
}

/// README「遍历：容器就是容器」。
#[test]
fn readme_iteration() {
    use json::parse;

    let doc = parse(r#"{"scores":[3,1,2]}"#);
    let root = doc.value.unwrap();
    let scores = root.get("scores").unwrap().as_array().unwrap();

    let total: i64 = scores.iter().filter_map(|item| item.as_int()).sum();
    let threes = scores.iter().filter(|item| item.as_int() == Some(3)).count();
    let found = scores.iter().find(|item| item.as_int() == Some(1));
    let numbers: Vec<i64> = scores.iter().map(|item| item.int_or(0)).collect();
    let any_big = scores.iter().any(|item| item.int_or(0) > 2);

    assert_eq!(total, 6);
    assert_eq!(threes, 1);
    assert_eq!(found.map(|item| item.int_or(0)), Some(1));
    assert_eq!(numbers, [3, 1, 2]);
    assert!(any_big);

    let keys: Vec<&str> = root.as_object().unwrap().iter().map(|item| item.key.as_str()).collect();
    assert_eq!(keys, ["scores"]);

    let entries: Vec<Option<&str>> = (&root).into_iter().map(|entry| entry.key).collect();
    assert_eq!(entries, [Some("scores")]);
}

/// README「模型投射」。
#[test]
fn readme_model_projection() {
    use json::{decode, encode, json_enum, json_model};
    use std::collections::BTreeMap;

    #[derive(Clone, Debug, Default, PartialEq)]
    struct Token {
        name: String,
        priority: i64,
        pattern: Option<String>,
    }
    json_model!(Token, "name" => name, "priority" => priority, "pattern" => pattern);

    #[derive(Clone, Copy, Debug, PartialEq)]
    enum Kind {
        Line,
        Polygon,
    }
    json_enum!(Kind, Line = 0, Polygon = 1);

    #[derive(Clone, Debug, Default, PartialEq)]
    struct Config {
        name: String,
        kind: Kind,
        tokens: Vec<Token>,
        labels: BTreeMap<String, String>,
    }
    json_model!(Config,
                "name" => name,
                "kind" => kind,
                "tokens" => tokens,
                "labels" => labels);

    let text = r#"{"name":"calc","kind":1,"tokens":[{"name":"NUMBER","priority":10}],"labels":{}}"#;
    let config = decode::<Config>(&json::parse(text).value.unwrap()).require().unwrap();
    assert_eq!(config.kind, Kind::Polygon);
    assert_eq!(config.tokens[0].name, "NUMBER");
    assert_eq!(config.tokens[0].pattern, None);

    let encoded = encode(&config);
    assert_eq!(
        encoded.dump(),
        r#"{"name":"calc","kind":1,"tokens":[{"name":"NUMBER","priority":10,"pattern":null}],"labels":{}}"#
    );
}

/// README「诊断带 JSON 路径」。
#[test]
fn readme_diagnostics() {
    use json::{decode, json_model, parse, DecodeOptions};

    #[derive(Debug, Default)]
    struct Point {
        x: i64,
        y: i64,
    }
    json_model!(Point, "x" => x, "y" => y);

    #[derive(Debug, Default)]
    struct Shape {
        points: Vec<Point>,
    }
    json_model!(Shape, "points" => points);

    let doc = parse(r#"{"points":[{"x":0,"y":0},{"x":"bad","y":1}]}"#);
    let broken = decode::<Shape>(&doc.value.unwrap());
    let first = broken.diagnostics[0].to_string();
    assert_eq!(first, "points[1].x: 期望整数，实际是 string [type-mismatch]");

    assert!(!DecodeOptions::lenient().strict_missing, "宽松档不查缺字段");
    assert!(DecodeOptions::default().strict_missing, "缺省档查缺字段");
}
