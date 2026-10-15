//! library/template-rust 的自检：变量、条件、循环、注释、空白规则、片段、报错。
//!
//! 不用任何测试框架（本 crate 不依赖第三方），就是一批 `#[test]` + `cargo test`。
//! 用例与 C++ 版 `library/template/test_template.cpp` 一一对应，**期望字符串
//! 逐字相同**——这是"翻译没走样"的主要证据。
//!
//! ```sh
//! cargo test --manifest-path library/template-rust/Cargo.toml
//! ```

use template_engine::{
    encode_object, no_snippets, render, render_values, render_with, snippet_table, template_model,
    to_camel_case, to_snake_case, Encode, MapModel, Object, TemplateEngine, TemplateError, Value,
    ValueModel,
};

// ---------------------------------------------------------------------------
//  测试用的模型
// ---------------------------------------------------------------------------

template_model! {
    /// 叶子节点。
    struct Leaf {
        name: String,
        size: i64,
    }
}

template_model! {
    /// 树枝：一段标题 + 一串叶子。
    struct Branch {
        title: String,
        leaves: Vec<Leaf>,
    }
}

/// 根模型：**手写 `Encode`**（对应 C++ 版"已有结构体 + `TEMPLATE_MODEL` 绑定"）——
/// 字段顺序就是模板遍历顺序。
#[derive(Clone, Debug)]
struct Root {
    name: String,
    yes: bool,
    no: bool,
    count: i64,
    empty_text: String,
    probe: Leaf,
    leaves: Vec<Leaf>,
    empty_leaves: Vec<Leaf>,
    branches: Vec<Branch>,
}

impl Encode for Root {
    fn encode(&self) -> Value {
        encode_object! {
            "name" => &self.name,
            "yes" => self.yes,
            "no" => self.no,
            "count" => self.count,
            "empty_text" => &self.empty_text,
            "probe" => &self.probe,
            "leaves" => &self.leaves,
            "empty_leaves" => &self.empty_leaves,
            "branches" => &self.branches,
        }
    }
}

/// 零字段模型：给"纯静态片段"用的占位模型（没有变量可填）。
struct NoFields;

impl Encode for NoFields {
    fn encode(&self) -> Value {
        encode_object! {}
    }
}

fn root() -> Root {
    Root {
        name: "root".to_string(),
        yes: true,
        no: false,
        count: 42,
        empty_text: String::new(),
        probe: Leaf::new("leafy", 7),
        leaves: vec![Leaf::new("a", 1), Leaf::new("b", 2)],
        empty_leaves: Vec::new(),
        branches: vec![
            Branch::new("B1", vec![Leaf::new("a", 1), Leaf::new("b", 2)]),
            Branch::new("B2", vec![Leaf::new("c", 3)]),
        ],
    }
}

/// 片段表：名字 → 片段正文（引擎不碰文件系统）。
fn snippets() -> template_engine::SnippetTable {
    snippet_table([
        ("head", "// {{ name }} 的头部\n"),
        ("inner", "<{{ name }}>"),
        ("outer", "[{% include \"inner\" %}]\n"),
        ("item", "- {{ leaf.name }} #{{ loop_index }}\n"),
        ("block", "{% if yes %}Y{% else %}N{% endif %}"),
        ("with space", "SP"),
    ])
}

/// 渲染成功就取值，失败直接把错误打出来（消息里带标签名）。
fn ok(result: Result<String, TemplateError>) -> String {
    match result {
        Ok(text) => text,
        Err(error) => panic!("渲染失败：{error}"),
    }
}

// ---------------------------------------------------------------------------
//  变量替换
// ---------------------------------------------------------------------------

#[test]
fn variables() {
    let root = root();
    assert_eq!(ok(render("A{{ name }}B", &root)), "ArootB", "变量");
    assert_eq!(ok(render("A{{  name  }}B", &root)), "ArootB", "变量：路径两端空白自动 trim");
    assert_eq!(ok(render("A{{ nope }}B", &root)), "AB", "变量：缺失 → 空串");
    assert_eq!(ok(render("{{ probe.name }}/{{ probe.size }}", &root)), "leafy/7", "变量：多级路径");
    assert_eq!(ok(render("A{{ name.nope }}B", &root)), "AB", "变量：路径断在非对象上");
    assert_eq!(ok(render("A{{ }}B", &root)), "AB", "变量：空路径");
    assert_eq!(ok(render("{{ yes }}/{{ no }}", &root)), "true/false", "变量：bool → true/false");
    assert_eq!(ok(render("{{ count }}", &root)), "42", "变量：整数 → 十进制");
    assert_eq!(ok(render("A{{ leaves }}B", &root)), "AB", "变量：数组/对象 → 空串");
}

// ---------------------------------------------------------------------------
//  条件
// ---------------------------------------------------------------------------

#[test]
fn conditionals() {
    let root = root();
    assert_eq!(ok(render("{% if yes %}Y{% endif %}", &root)), "Y", "if：真");
    assert_eq!(ok(render("{% if no %}Y{% endif %}", &root)), "", "if：假");
    assert_eq!(ok(render("{% if no %}Y{% else %}N{% endif %}", &root)), "N", "if：假 → else");
    assert_eq!(ok(render("{% if yes %}Y{% else %}N{% endif %}", &root)), "Y", "if：真 → 不走 else");
    assert_eq!(
        ok(render("{% if empty_text %}Y{% else %}N{% endif %}", &root)),
        "N",
        "if：空字符串为假"
    );
    assert_eq!(ok(render("{% if leaves %}Y{% else %}N{% endif %}", &root)), "Y", "if：非空数组为真");
    assert_eq!(ok(render("{% if count %}Y{% endif %}", &root)), "Y", "if：整数 0 为假（count=42 为真）");
    assert_eq!(
        ok(render("{% if yes %}1{% if no %}a{% else %}b{% endif %}2{% else %}Z{% endif %}", &root)),
        "1b2",
        "if：嵌套 + 内层 else"
    );
    assert_eq!(
        ok(render("{% if no %}1{% if yes %}a{% else %}b{% endif %}2{% else %}Z{% endif %}", &root)),
        "Z",
        "if：嵌套（外层假时整块跳过）"
    );
    assert_eq!(ok(render("{%if yes%}Y{%else%}N{%endif%}", &root)), "Y", "if：紧贴写法 + 空白变体");
    assert_eq!(ok(render("{%  if yes  %}Y{%  endif  %}", &root)), "Y", "if：空白变体");
}

// ---------------------------------------------------------------------------
//  独占一行的标签：整行删除
// ---------------------------------------------------------------------------

#[test]
fn standalone_line_tags() {
    let root = root();
    assert_eq!(
        ok(render("before\n{% if yes %}\nkeep\n{% endif %}\nafter\n", &root)),
        "before\nkeep\nafter\n",
        "独占一行的 if（真）"
    );
    assert_eq!(
        ok(render("before\n{% if no %}\nkeep\n{% endif %}\nafter\n", &root)),
        "before\nafter\n",
        "独占一行的 if（假）"
    );
    assert_eq!(ok(render("x {% if yes %}Y{% endif %} z", &root)), "x Y z", "同一行内有别的内容 → 只删标签");
    assert_eq!(ok(render("{% if no %}Y{% endif %}z", &root)), "z", "同一行内有别的内容（假）");
}

// ---------------------------------------------------------------------------
//  循环 + 作用域链
// ---------------------------------------------------------------------------

#[test]
fn loops() {
    let root = root();
    assert_eq!(
        ok(render("{% for leaf in leaves %}[{{ leaf.name }}]{% endfor %}", &root)),
        "[a][b]",
        "for：基本"
    );
    assert_eq!(
        ok(render("list:\n{% for leaf in leaves %}\n- {{ leaf.name }}\n{% endfor %}\ndone\n", &root)),
        "list:\n- a\n- b\ndone\n",
        "for：独占一行"
    );
    assert_eq!(
        ok(render("A{% for leaf in nope %}{{ leaf.name }}{% endfor %}B", &root)),
        "AB",
        "for：缺失列表 → 整块跳过"
    );
    assert_eq!(ok(render("{% for leaf %}{{ name }}{% endfor %}C", &root)), "C", "for：写坏的 for → 整块跳过，后面照常");
    assert_eq!(
        ok(render("A{% for leaf in empty_leaves %}{{ leaf.name }}{% endfor %}B", &root)),
        "AB",
        "for：空列表 → 整块跳过"
    );

    assert_eq!(
        ok(render(
            "{% for b in branches %}{{ b.title }}\
             {% for leaf in b.leaves %}({{ name }}/{{ b.title }}:{{ leaf.name }}){% endfor %};\
             {% endfor %}",
            &root
        )),
        "B1(root/B1:a)(root/B1:b);B2(root/B2:c);",
        "嵌套 for：内层能取到外层变量（name / b.title）"
    );
    assert_eq!(
        ok(render(
            "{% for b in branches %}{% for leaf in leaves %}{{ b.title }}{{ leaf.name }}{% endfor %}{% endfor %}",
            &root
        )),
        "B1aB1bB2aB2b",
        "嵌套 for：外层循环变量在内层仍可用"
    );
}

// ---------------------------------------------------------------------------
//  注释
// ---------------------------------------------------------------------------

#[test]
fn comments() {
    let root = root();
    assert_eq!(ok(render("A{# hidden #}B", &root)), "AB", "注释：直接丢掉");
    assert_eq!(ok(render("A{# {% endif %} {% if x %} #}B", &root)), "AB", "注释：里面的标签文字不算标签");
    assert_eq!(ok(render("A\n{# c #}\nB\n", &root)), "A\nB\n", "注释：独占一行时整行丢掉");
}

// ---------------------------------------------------------------------------
//  原样段：{% raw %} … {% endraw %}
// ---------------------------------------------------------------------------

#[test]
fn raw_sections() {
    let root = root();
    assert_eq!(ok(render("A{% raw %}{{ name }}{% endraw %}B", &root)), "A{{ name }}B", "raw：里面的 {{ }} 不当标签");
    assert_eq!(ok(render("`{% raw %}{{ target_code }}{% endraw %}`", &root)), "`{{ target_code }}`", "raw：{{ target_code }} 这种目标文本能原样输出");
    assert_eq!(
        ok(render("{% raw %}\n{% if x %}\n{% endraw %}\n", &root)),
        "{% if x %}\n",
        "raw：独占一行时内容原样保留"
    );
    assert_eq!(
        ok(render("{% for leaf in leaves %}{% raw %}{{ x }}{% endraw %}{% endfor %}", &root)),
        "{{ x }}{{ x }}",
        "raw：放进循环体里也照原样输出"
    );
    assert!(render("{% raw %}{{ x }}", &root).is_err(), "未闭合的 raw");
}

// ---------------------------------------------------------------------------
//  片段：{% include "名字" %}
// ---------------------------------------------------------------------------

#[test]
fn includes() {
    let root = root();
    let snippets = snippets();

    assert_eq!(ok(render_with("A{% include \"inner\" %}B", &root, &snippets)), "A<root>B", "include：基本");
    assert_eq!(ok(render_with("A{% include 'inner' %}B", &root, &snippets)), "A<root>B", "include：单引号");
    assert_eq!(ok(render_with("A{% include inner %}B", &root, &snippets)), "A<root>B", "include：不带引号");
    assert_eq!(
        ok(render_with("before\n{% include \"head\" %}\nafter\n", &root, &snippets)),
        "before\n// root 的头部\nafter\n",
        "include：独占一行 → 整行换成片段"
    );
    assert_eq!(
        ok(render_with("x {% include \"inner\" %} z", &root, &snippets)),
        "x <root> z",
        "include：同一行里有别的内容 → 只换标签"
    );
    assert_eq!(
        ok(render_with("{% include \"head\" %}", &root, &snippets)),
        "// root 的头部\n",
        "include：片段里能用外层变量"
    );
    assert_eq!(
        ok(render_with("{% for leaf in leaves %}{% include \"item\" %}{% endfor %}", &root, &snippets)),
        "- a #0\n- b #1\n",
        "include：放进循环体，片段能取到循环变量"
    );
    assert_eq!(
        ok(render_with("{% include \"outer\" %}", &root, &snippets)),
        "[<root>]\n",
        "include：片段里再 include（嵌套）"
    );
    assert_eq!(
        ok(render_with("{% include \"block\" %}", &root, &snippets)),
        "Y",
        "include：片段里的 if / for 照常解析"
    );
    assert_eq!(
        ok(render_with("{% include \"with space\" %}", &root, &snippets)),
        "SP",
        "include：片段名可以带空格（引号写法）"
    );
    assert_eq!(ok(render_with("A{% include \"nope\" %}B", &root, &snippets)), "AB", "include：片段表里没有 → 空串");
    assert_eq!(ok(render("A{% include \"head\" %}B", &root)), "AB", "include：没给片段表 → 空串");
    assert_eq!(ok(render_with("A{% include %}B", &root, &snippets)), "AB", "include：标签里没写名字 → 空串");
    assert_eq!(ok(render_with("A{% include \"head %}B", &root, &snippets)), "AB", "include：引号没闭合 → 空串");
    assert_eq!(
        ok(render_with("before\n{% include \"nope\" %}\nafter\n", &root, &snippets)),
        "before\nafter\n",
        "include：片段为空 → 独占一行的标签也整行消失"
    );
    assert_eq!(
        ok(render_with("{% if yes %}{% include \"inner\" %}{% endif %}", &root, &snippets)),
        "<root>",
        "include：放在 if 分支里也照常（环境一路传下去）"
    );

    assert!(
        render_with("{% include \"bad\" %}", &root, &snippet_table([("bad", "{% if yes %}x")])).is_err(),
        "include：片段自己的标签没闭合 → 和顶层模板一样报错"
    );

    let cycle = snippet_table([("a", "{% include \"b\" %}"), ("b", "{% include \"a\" %}")]);
    let error = render_with("{% include \"a\" %}", &root, &cycle).unwrap_err();
    assert!(
        matches!(error, TemplateError::IncludeTooDeep(_)),
        "include：片段互相包含 → 报错而不是爆栈，实际得到 {error}"
    );
    // 编译期断言：嵌套上限是个正数（C++ 自检里对应 `k_maxIncludeDepth > 0u`）。
    const {
        assert!(TemplateEngine::MAX_INCLUDE_DEPTH > 0, "include：嵌套上限是个正数");
    }

    assert_eq!(
        ok(render_values_with_fixture("{% include \"v\" %}", &[("x", Value::Int(1))], &snippet_table([("v", "{{ x }}!")]))),
        "1!",
        "render_values：include 也能用（片段同样是模板）"
    );
    assert_eq!(
        ok(render_with("{% include \"inner\" %}", &root, &snippet_table([("inner", "A")])))
            + &ok(render_with("{% include \"inner\" %}", &root, &snippet_table([("inner", "B")]))),
        "AB",
        "include：同一份模板 + 不同片段表 → 结果跟着表走"
    );
}

/// `render_values` + 片段表（对应 C++ 的 `renderValues(src, values, snippets)`）。
fn render_values_with_fixture(
    source: &str,
    values: &[(&str, Value)],
    snippets: &template_engine::SnippetTable,
) -> Result<String, TemplateError> {
    template_engine::render_values_with(source, values, snippets)
}

// ---------------------------------------------------------------------------
//  循环元信息：loop_index / loop_first / loop_last / loop_count
// ---------------------------------------------------------------------------

#[test]
fn loop_metadata() {
    let root = root();
    assert_eq!(ok(render("{% for leaf in leaves %}{{ loop_index }}{% endfor %}", &root)), "01", "loop_index：下标从 0 起");
    assert_eq!(ok(render("{% for leaf in leaves %}{{ loop_count }}{% endfor %}", &root)), "22", "loop_count：元素个数");
    assert_eq!(
        ok(render(
            "{% for leaf in leaves %}{% if loop_first %}F{% endif %}\
             {% if loop_last %}L{% endif %}{% endfor %}",
            &root
        )),
        "FL",
        "loop_first / loop_last：首尾标志"
    );
    assert_eq!(
        ok(render(
            "{% for leaf in leaves %}{% if loop_first %}{% else %}, {% endif %}{{ leaf.name }}{% endfor %}",
            &root
        )),
        "a, b",
        "分隔符交给模板：逗号只是首元素之后才有"
    );
    assert_eq!(
        ok(render(
            "{% for b in branches %}{{ b.title }}:\
             {% for leaf in b.leaves %}{% if loop_first %}{% else %}/{% endif %}{{ leaf.name }}{% endfor %};\
             {% endfor %}",
            &root
        )),
        "B1:a/b;B2:c;",
        "嵌套循环：loop_first 说的是最内层"
    );
    assert_eq!(ok(render("{% for leaf in empty_leaves %}x{% endfor %}", &root)), "", "单元素循环：既 first 又 last");
}

// ---------------------------------------------------------------------------
//  健壮性 + 无状态
// ---------------------------------------------------------------------------

#[test]
fn robustness_and_statelessness() {
    let root = root();
    assert_eq!(ok(render("A{% endif %}B", &root)), "AB", "游离的 endif 安全跳过");
    assert_eq!(ok(render("A{% endfor %}B", &root)), "AB", "游离的 endfor 安全跳过");
    assert_eq!(ok(render("A{% nonsense %}B", &root)), "AB", "不认识的指令安全跳过");

    assert!(render("{% if yes %}X", &root).is_err(), "未闭合的 if");
    assert!(render("{{ name", &root).is_err(), "未闭合的变量标签");
    assert!(render("{# c", &root).is_err(), "未闭合的注释");

    // 报错消息带上了那个没配对的标签名。
    let error = render("{% if yes %}X", &root).unwrap_err();
    assert_eq!(error.to_string(), "未闭合标签: {% endif %}", "错误信息带标签名");

    let source = "{% for leaf in leaves %}{{ leaf.name }}{% endfor %}";
    assert_eq!(ok(render(source, &root)) + &ok(render(source, &root)), "abab", "重复渲染（无状态）");
}

// ---------------------------------------------------------------------------
//  Value / encode
// ---------------------------------------------------------------------------

#[test]
fn values_and_encode() {
    let root = root();
    assert!(Value::Int(3).is_int(), "Value：int");
    assert_eq!(Value::Int(-5).to_text(), "-5", "Value：int64");
    assert_eq!(Value::from(4usize).as_int(), Some(4), "Value：其它整数类型（usize）");
    assert!(Value::Null.is_null(), "Value：null");
    assert_eq!(Value::Bool(true).to_text(), "true", "Value：bool");
    assert_eq!(Value::text("hi").as_str(), Some("hi"), "Value：字符串");
    assert!(!Value::text("").truthy(), "Value：空串为假");
    assert!(Value::text("x").truthy(), "Value：非空串为真");
    assert!(!Value::Int(0).truthy(), "Value：0 为假");

    assert!(root.encode().is_object(), "encode：模型 → 对象");
    assert!(root.leaves.encode().is_array(), "encode：Vec → 数组");

    // 注意：as_object() 借的是值内部的对象，值得先活着。
    let encoded = root.probe.encode();
    let object = match encoded.as_object() {
        Some(object) => object,
        None => panic!("encode：对象保序：probe 应该是对象"),
    };
    assert_eq!(object.len(), 2, "encode：对象保序：两个字段");
    assert_eq!(object.members()[0].0, "name", "encode：对象保序：第一个是 name");
    assert_eq!(object.members()[1].0, "size", "encode：对象保序：第二个是 size");
}

// ---------------------------------------------------------------------------
//  引擎不绑模型结构体：任何 Model 都能渲染
// ---------------------------------------------------------------------------

#[test]
fn value_model_adapter() {
    let mut object = Object::new();
    object.insert("name", "dynamic");
    let value = Value::object(object);
    let model = ValueModel::new(&value);
    assert_eq!(
        ok(TemplateEngine::render("<{{ name }}>", &model, no_snippets())),
        "<dynamic>",
        "ValueModel：动态值也能渲染"
    );
}

// ---------------------------------------------------------------------------
//  零字段模型（纯静态片段）
// ---------------------------------------------------------------------------

#[test]
fn zero_field_model() {
    assert_eq!(ok(render("hello\n", &NoFields)), "hello\n", "零字段模型：没有变量的模板原样输出");
    assert_eq!(ok(render("A{{ x }}B", &NoFields)), "AB", "零字段模型：变量取不到就是空串");
}

// ---------------------------------------------------------------------------
//  命名转换（文件名 / 宏名派生用，命名空间本身不改）
// ---------------------------------------------------------------------------

#[test]
fn naming() {
    assert_eq!(to_snake_case("MyParser"), "my_parser", "toSnakeCase：MyParser");
    assert_eq!(to_snake_case("HTTPParser"), "http_parser", "toSnakeCase：HTTPParser（缩写）");
    assert_eq!(to_snake_case("Parser2Value"), "parser2_value", "toSnakeCase：Parser2Value");
    assert_eq!(to_snake_case("my-parser.Parser"), "my_parser_parser", "toSnakeCase：已经下划线 / 带分隔符");
    assert_eq!(to_camel_case("my_parser"), "MyParser", "toCamelCase：my_parser");
}

// ---------------------------------------------------------------------------
//  MapModel / renderValues：按名字取值
// ---------------------------------------------------------------------------

#[test]
fn map_model_and_render_values() {
    assert_eq!(
        ok(render_values("A{{ x }}B{{ y }}C", &[("x", Value::text("1")), ("y", Value::Int(2))])),
        "A1B2C",
        "MapModel / renderValues：按名字取值"
    );
    assert_eq!(ok(render_values("A{{ nope }}B", &[("x", Value::Int(1))])), "AB", "MapModel：没有的键就是空串");

    let items = Value::array(vec![Value::Int(3), Value::Int(4)]);
    assert_eq!(
        ok(render_values(
            "{% for v in xs %}{% if loop_first %}{% else %}, {% endif %}{{ v }}{% endfor %}",
            &[("xs", items)]
        )),
        "3, 4",
        "renderValues：循环也能用"
    );

    // 补充（C++ 自检里没有这条）：`set` 是同名覆盖语义，用同一个表改绑时看得到。
    let mut model = MapModel::new();
    model.set("x", 1).set("x", 2);
    assert_eq!(
        ok(TemplateEngine::render("{{ x }}", &model, no_snippets())),
        "2",
        "MapModel::set：同名覆盖"
    );
}
