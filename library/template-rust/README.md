# library/template-rust —— 纯标准库文本模板引擎（crate `template_engine`）

> **这是一个独立的 Rust crate：`template_engine`。** 给 Rust 项目用的轻量文本
> 模板引擎：`{{ 变量 }}` / `{% if %}` / `{% for %}` / `{% include %}` / `{% raw %}` /
> `{# 注释 #}`，**只用标准库，没有任何第三方依赖**（`Cargo.lock` 里只有本 crate 自己）。
>
> 它是 [`../template/`](../template/README.md)（C++17 纯头文件版，`library/template`）的
> Rust 版：**同一套语法、同一套语义、同一份自检用例**——96 项检查逐条对照着搬过来，
> 期望字符串逐字相同，demo 输出与 C++ 版逐行一致。
>
> 设计、实现与取舍见 [技术报告.md](技术报告.md)。

适合这些场景：快速拼一段 DSL、生成代码 / 配置 / 报告文本、把"结构体 → 文本"
的那一步从手写 `format!` 里拿出来。

## 文件

| 文件 | 内容 |
| --- | --- |
| `src/engine.rs` | 渲染引擎：`TemplateEngine::render` / `render_block`、标签解析、空白规则、片段（`include`） |
| `src/value.rs` | 动态值 `Value`（null / string / int64 / bool / array / object）与 `Object` / `Array` |
| `src/model.rs` | `Model` trait、`ValueModel`（动态值当模型）、`MapModel`（名字→值）、`EncodedModel`、`resolve` |
| `src/bind.rs` | `Encode` trait：结构体 → 模板值（含标量 / 字符串 / 容器 / `Option` 的内置实现） |
| `src/macros.rs` | `encode_object!`（字段表）、`template_model!`（结构体 + 投射一步到位） |
| `src/naming.rs` | `to_snake_case` / `to_camel_case` |
| `src/main.rs` | 演示：同一份模型渲染成两份目标语言代码（含 `{% include %}` 拼片段） |
| `tests/engine.rs` | 自检：14 个 `#[test]` / 100 处断言，与 C++ 版用例一一对应 |

## 快速开始

在别的 crate 里用 path 依赖：

```toml
[dependencies]
template_engine = { path = "library/template-rust" }
```

```rust
use template_engine::{render, template_model};

template_model! {
    struct Method {
        ret: String,
        name: String,
    }
}

template_model! {
    struct Class {
        name: String,
        methods: Vec<Method>,
    }
}

fn main() {
    let class = Class::new(
        "Calc",
        vec![Method::new("int", "add"), Method::new("void", "reset")],
    );

    let source = r#"
class {{ name }} {
{% for m in methods %}
    {{ m.ret }} {{ m.name }}();
{% endfor %}
}
"#;
    print!("{}", render(source, &class).unwrap());
}
```

`template_model!` 一次把「结构体 + `Encode` 实现」展开出来，等价于 C++ 版的
`struct` + `TEMPLATE_MODEL(...)`。已经有结构体、只想加投射时，手写 `impl Encode`
+ `encode_object!`（C++ 版 `TEMPLATE_MODEL` 的直译，模板里的字段名还可以跟
Rust 字段名不同）：

```rust
use template_engine::{encode_object, Encode, Value};

struct Token {
    name: String,
    priority: i64,
}

impl Encode for Token {
    fn encode(&self) -> Value {
        encode_object! { "name" => &self.name, "priority" => self.priority }
    }
}
```

三种给数据的方式，按需要挑：

1. **模型结构体**：`template_model!` 或 `impl Encode` +
   `render(source, &model)`（最正式，编译期就能查字段）；
2. **名字 → 值**：`render_values(source, &[("mode_name", Value::text("LALR(1)")), ...])`
   （临时拼一段文本时最省事）；
3. **动态值**：`Object` + `ValueModel`（值在运行时才知道，比如读配置）。

模板拆成多段时再加一层片段表（`{% include %}` 的名字 → 片段正文，见下一节）；
它是 `render_with(source, &model, &snippets)` / `render_values_with(...)` 的第三个参数，
不传就是空表（`render` / `render_values`）。

## 语法

| 写法 | 含义 |
| --- | --- |
| `{{ a.b.c }}` | 取值（逐级取对象成员）；取不到就渲染成空串 |
| `{% if x %}…{% else %}…{% endif %}` | 条件；`x` 是一条路径，按真值判断（可嵌套、可只写 `{% if x %}`） |
| `{% for x in list %}…{% endfor %}` | 循环；循环内可用 `loop_index` / `loop_first` / `loop_last` / `loop_count` |
| `{% raw %}…{% endraw %}` | 原样输出（里面的 `{{ }}` / `{% %}` 不当标签） |
| `{# 注释 #}` | 注释，不输出 |
| `{% include "名字" %}` | 插入另一段模板：片段表（名字 → 片段正文）由调用方给，片段里还能再 include |

- **块标签独占一行时整行删除**（不会留下空行）；想让空段保留一行，把标签写在行内。
  这条对 `{% include %}` 同样有效——独占一行的 include 就是"这一行换成片段正文"。
- 模板里要写目标语言自己的 `{{` / `}}`（C++ 初始化器、C# 字典等）时，包进
  `{% raw %}…{% endraw %}`，否则会被当成变量标签吃掉。
- 模板**不读文件**：模板字符串由调用方给（从文件读、从内嵌资源取、命令行传入都行）。
- 缺字段、空列表、缺片段、不认识的指令都不会崩，只是输出空串；**只有两类情况返回
  错误**：标签不闭合（`TemplateError::UnclosedTag(_)` / `UnclosedVariable` /
  `UnclosedComment`）与 include 嵌套过深（`TemplateError::IncludeTooDeep(_)`），
  错误消息里带着那个出事的标签名 / 片段名。

## 片段：`{% include "名字" %}`

把大模板拆成几段、或者复用一段公共前缀，就用它：片段表是**调用方**的东西
（`SnippetTable`，就是 `名字 → 片段正文` 的 `BTreeMap`），引擎只负责在渲染到那一行时查表。

```rust
use template_engine::{render_with, snippet_table};

let snippets = snippet_table([
    ("header", "// 由 {{ name }} 自动生成，请勿手改\n"),
    ("methods", "{% for m in methods %}    {{ m.ret }} {{ m.name }}();\n{% endfor %}"),
    ("whole", "{% include \"header\" %}class {{ name }} {\n{% include \"methods\" %}};\n"),
]);

let text = render_with("{% include \"whole\" %}", &class, &snippets).unwrap();
```

几条约定：

| 事情 | 行为 |
| --- | --- |
| 片段名怎么写 | `{% include "a" %}` / `{% include 'a' %}` / `{% include a %}` 都认；名字里不认转义，所以 `"a b"` 这种带空格的名字要加引号 |
| 会读到哪些数据 | 片段正文**按模板渲染**，用的是**同一个上下文**：片段里能用外层变量，放进 `{% for %}` 里还能取到循环变量、`loop_*` |
| 嵌套 | 片段里再写 `{% include %}` 就是嵌套包含；层数上限 `TemplateEngine::MAX_INCLUDE_DEPTH`（32），超了返回错误——片段互相包含不会把栈转爆 |
| 片段里标签没闭合 | 和顶层模板一样返回错误（消息里带标签名） |
| 表里没有 / 没给表 / 名字写坏了 | 输出空串，不报错（和"缺变量、空列表"一个口径） |

片段表的键就是名字，值是**模板正文**（不是文件路径）——想按名字去磁盘上找文件，
就在渲染前自己把文件读进来填表；想做懒加载也可以，引擎只在渲染到那一行时才查表。

## 构建与自检

```sh
# 在 crate 目录里跑：产物按 .cargo/config.toml 落到仓库 build/template/rust/
cd library/template-rust

cargo test      # 自检：14 个 #[test] + 20 个文档示例
cargo run       # 跑 demo（把模型渲染成两份目标语言代码）
cargo clippy --all-targets
```

或者从仓库根用 `--manifest-path`：

```sh
cargo test --manifest-path library/template-rust/Cargo.toml
cargo run  --manifest-path library/template-rust/Cargo.toml
```

（从仓库根跑时 cargo 读不到 `.cargo/config.toml`，产物落在 `library/template-rust/target/`；
两者都在 `.gitignore` 范围内。）

## 自检结果（实测）

```text
     Running tests\engine.rs
running 14 tests
test result: ok. 14 passed; 0 failed; 0 ignored; 0 measured; 0 filtered out

   Doc-tests template_engine
running 20 tests
test result: ok. 20 passed; 0 failed; 0 ignored; 0 measured; 0 filtered out
```

`cargo clippy --all-targets` 无提示；`cargo build --release` 无警告。

与 C++ 版自检的对应关系（`library/template/test_template.cpp` 跑出 **96 项检查**）：

| 一侧 | 数量 | 说明 |
| --- | --- | --- |
| C++ 自检 | 96 | `expect` / `expectTrue` / `expectThrow` |
| Rust 自检 | 100 | 96 项全部覆盖 + 4 项新增/拆分 |

4 项差额都记账在这里：`encode：对象保序` 拆成 3 条断言（+2）、`MapModel::set`
同名覆盖的补充用例（+1）、错误信息里带标签名的校验（+1）。6 处**措辞**改写是
等价替换，不是漏项：`Value：string_view` → `Value：字符串`、`size_t` → `usize`、
`vector` → `Vec`、`include` 报错措辞、`renderValues` → `render_values`（标识符按
Rust 的下划线命名）。
核对脚本：`tmp/template_rust_case_check.py`（比对两侧的用例标签集合）。

demo 输出与 C++ 版 demo 逐行一致——两边各跑一次再用 `diff --strip-trailing-cr`
比对，没有内容差异；唯一的字节差异是换行符（C++ demo 的模板字符串来自 CRLF 的
`.cpp`，本 crate 的 `.rs` 在磁盘上是 LF），与引擎无关。

## 与 C++ 版（`library/template`）的差异

语义完全对齐，**接口按 Rust 的习惯改写**：

| 事情 | C++ 版 | 本 crate |
| --- | --- | --- |
| 值 | `tmpl::Value`（`std::variant` + `shared_ptr`） | `Value` 枚举（`Arc` 共享，`Send + Sync`） |
| 取值 | `isString()` / `asString()` 返回 `optional<string_view>` | `is_string()` / `as_str()` 返回 `Option<&str>` |
| 模型投射 | `TEMPLATE_MODEL` + 成员指针表 | `impl Encode` + `encode_object!` / `template_model!` |
| 取数接口 | `Model` 虚基类 + `ModelAdapter<T>` | `Model` trait + `EncodedModel`（投影一次并缓存） |
| 报错 | 抛 `std::runtime_error`（`-fno-exceptions` 下 `abort`） | 返回 `Result<_, TemplateError>` |
| 片段表 | `std::map`（键有序） | `BTreeMap`（键有序） |
| 便捷入口 | `render(src, model, snippets = {})` 默认实参 | `render` / `render_with`、`render_values` / `render_values_with`（Rust 没有默认实参） |
| 无状态 | 全静态方法 | 全关联函数 / 自由函数，无全局状态 |

一处**实现**上的差异：C++ 的 `ModelAdapter<T>` 每次 `get` 只投射命中的那一个字段
（靠成员指针表），Rust 没有成员指针，`EncodedModel` 改成构造时投射一次、之后每次
`get` 在缓存好的对象里查——同一个适配器可以反复用，不会重复投射。

## 和 ParseEngine 生成器的关系

**目前生成流程不用它。** ParseEngine 的六档后端渲染 `include/templates/*.tmpl`
走的是 C++ 版的 `library/template`（`src/project.cpp` / `code_gen_*.cpp` 里
`#include "template/engine.hpp"`）。本 crate 是**独立的一份 Rust 实现**：语法与语义
对齐、自检用例对齐，可以单独拿去别的 Rust 项目用，也可以当成"把生成器搬到 Rust"
时的现成组件——但**它现在与生成流程没有任何接线**，改它不会影响生成的产物。

这个库同时是给**用户**用的：想在别的 Rust 项目里快速拼一段 DSL、生成配置或报告
文本，直接把 path 依赖指到 `library/template-rust/` 即可，不用拉别的依赖。
