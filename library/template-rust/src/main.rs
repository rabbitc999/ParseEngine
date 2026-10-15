//! library/template-rust 的示例：把模型渲染成任意目标语言的代码。
//!
//! 编译运行（只用标准库）：
//!
//! ```sh
//! cargo run --manifest-path library/template-rust/Cargo.toml
//! ```
//!
//! 演示的是编译器后端最常见的那条链路：
//!
//! ```text
//! IR（语法树 / 符号表）
//!   → 转换层：把 IR 里的类、方法、字段抄进模型结构体（下面那两个 struct）
//!   → 模板渲染：render(source, &model)
//!   → 目标语言源码
//! ```

use template_engine::{
    no_snippets, render, render_with, snippet_table, template_model, Object, TemplateEngine, Value,
    ValueModel,
};

// ---------------------------------------------------------------------------
//  1. 模型：直接照着 IR 的形状写，字段就是模板里要用到的名字
// ---------------------------------------------------------------------------

template_model! {
    /// 一个方法（IR 里的函数声明投影过来的）。
    struct MethodModel {
        /// 方法名。
        name: String,
        /// 返回类型（已经按目标语言渲染好的文本）。
        ret_type: String,
        /// 形参列表（已经拼好的文本，如 `int a, int b`）。
        args: String,
        /// 是否静态方法。
        is_static: bool,
    }
}

template_model! {
    /// 一个类（IR 里的类型声明投影过来的）。
    struct ClassModel {
        /// 类名。
        name: String,
        /// IR 里有没有显式构造器。
        has_ctor: bool,
        /// 方法表。
        methods: Vec<MethodModel>,
    }
}

// 说明：`template_model!` 一次把「结构体 + `Encode` 实现」展开出来，等价于 C++ 版的
// `struct` + `TEMPLATE_MODEL(...)`。已经有结构体、只想加绑定时，手写
// `impl Encode` + `encode_object!` 即可（模板里的字段名还可以跟 Rust 字段名不同）。

// ---------------------------------------------------------------------------
//  2. 模板：同一个模型，两份目标语言模板（引擎不读文件，模板就是字符串；
//     真做产品时这些字符串通常来自 include/templates/*.tmpl，由调用方读进来）
// ---------------------------------------------------------------------------

/// 类模板：条件、循环、同行的 if/endif、独占一行的标签都会按预期消失。
const CLASS_TEMPLATE: &str = r#"
{% if has_ctor %}public {% endif %}class {{ name }} {
{% if has_ctor %}
    public {{ name }}() {
        // 构造器
    }
{% endif %}
{% for m in methods %}
    {{ m.ret_type }} {{ m.name }}({{ m.args }}) {
        return 0;
    }
{% endfor %}
}
"#;

/// C++ 模板：声明式头文件，顺便演示"循环体里访问外层变量"（`{{ name }}`）。
const CPP_TEMPLATE: &str = r#"
// 由 {{ name }} 自动生成，请勿手改
class {{ name }} {
public:
{% for m in methods %}
    {{ m.ret_type }} {{ m.name }}({{ m.args }});
{% endfor %}
};
"#;

/// 再加一个模板演示其余语法：注释、嵌套 if、作用域链（循环里取外层变量）、
/// 缺失变量（输出空串而不是报错）。
const FEATURE_TEMPLATE: &str = r#"
{# 这一整行是注释，独占一行，不会出现在输出里 #}
class {{ name }} {
{% for m in methods %}
    {% if m.is_static %}static {% endif %}{{ m.ret_type }} {{ m.name }}({{ m.args }});
    {# 下面这行演示"内层能看见外层"：m.name 来自循环，name 来自外层 #}
    // {{ name }}::{{ m.name }}{{ not_exist_field }}{{ m.not_exist }}
{% endfor %}
};
"#;

/// 片段：`{% include "名字" %}` 把大模板拆成几段，片段表由调用方给。
///
/// 片段正文**按模板渲染**、用的是**同一个上下文**——所以下面 `methods` 那段能直接
/// 用 model.methods，`banner` 里也能用 model.name。注意 banner 里那句 `{# … #}`
/// 是注释：片段本身也是模板，注释会被丢掉。
const FILE_TEMPLATE: &str = r#"{% include "banner" %}#include <string>
#include <vector>

{% include "methods" %}"#;

fn main() {
    // ---- 3. 构造模型：真实项目里这一步是"IR → 模型"的转换层干的 ----
    let model = ClassModel::new(
        "Calc",
        true,
        vec![
            MethodModel::new("add", "int", "int lhs, int rhs", false),
            MethodModel::new("reset", "void", "", false),
            MethodModel::new("version", "string", "", true),
        ],
    );

    // ---- 4. 渲染 ----
    // 用 `print!` 而不是 `println!`：输出与 C++ 版 demo 逐行对齐（模板自己已经带换行）。
    print!("===== 类 =====\n{}\n", render(CLASS_TEMPLATE, &model).unwrap());
    print!("===== C++ =====\n{}\n", render(CPP_TEMPLATE, &model).unwrap());
    print!("===== 语法特性 =====\n{}\n", render(FEATURE_TEMPLATE, &model).unwrap());

    // 条件为假时：整个 if 块（含独占一行的 {% if %} / {% endif %} 两行）一起消失。
    let plain = ClassModel::new("Plain", false, vec![MethodModel::new("noop", "void", "", false)]);
    print!("===== has_ctor = false =====\n{}\n", render(CLASS_TEMPLATE, &plain).unwrap());

    // 引擎也不绑定模型结构体：任何 Model 都能渲染（ValueModel 把动态值包成 Model）。
    let mut object = Object::new();
    object.insert("name", "Dynamic");
    let dynamic = Value::object(object);
    let dynamic_model = ValueModel::new(&dynamic);
    print!(
        "===== 动态值（不经过模型类）=====\n{}",
        TemplateEngine::render("class {{ name }} { {{ missing }} }\n", &dynamic_model, no_snippets())
            .unwrap()
    );

    // ---- 5. 片段：{% include %} 把几段拼成一份文件（独占一行的 include 整行换成片段）
    let snippets = snippet_table([
        ("banner", "{# 模块头注释：示范「片段也是模板」 #}\n// {{ name }} —— 由模板引擎拼出来\n"),
        (
            "methods",
            "{% for m in methods %}{{ m.ret_type }} {{ m.name }}({{ m.args }});\n{% endfor %}",
        ),
    ]);
    print!(
        "===== 片段（{{% include %}}）=====\n{}",
        render_with(FILE_TEMPLATE, &model, &snippets).unwrap()
    );
}

// ---------------------------------------------------------------------------
//  接入已有的 IR 转换层
//
//  本引擎只做"模型 → 文本"，所以接入点非常窄，三步就够：
//
//    1) 在 IR → 目标语言的转换层里，把 IR 节点投影成模型结构体（就是本文件里的
//       MethodModel / ClassModel 这种"哑结构体"），字段全部用 String / bool /
//       Vec，别把 IR 的引用和生命周期带进来；
//    2) 给模型结构体写一次投射：`template_model!`（结构体 + Encode 一起展开）或
//       手写 `impl Encode` + `encode_object!`（已经有结构体时用这个）。同一份模型
//       可以挂多份目标语言模板（cpp_tmpl / python_tmpl / csharp_tmpl），换语言
//       只是换模板字符串；
//    3) 渲染：
//
//           let code = render(source, &class_model)?;
//
//       模板字符串从哪来由调用方决定——读文件、从内嵌资源里取、或者命令行传入
//       都行，引擎自己不碰文件系统。模板拆成了几段就把片段表带上：
//
//           let code = render_with(source, &class_model, &snippets)?;
//
//   现有的 code_gen_*.cpp 里是"占位符 replaceAll"，换成这条链路以后，条件分支
//   和循环交给模板（{% if %} / {% for %}），生成逻辑会短很多；迁移可以一份模板
//   一份模板地做，不要求一次换完。
//
//   调试提示：模板写错（比如漏了 {% endif %}）会返回 TemplateError，消息里带着
//   那个没配对的标签名；缺字段、空列表、缺片段、不认识的指令都不会崩，只是输出空。
//   片段互相包含（{% include %} 转圈）也会报错，不会爆栈。
// ---------------------------------------------------------------------------
