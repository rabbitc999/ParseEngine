# library/template —— 纯头文件模板引擎（`tmpl`）

给 C++17 项目用的轻量文本模板引擎：`{{ 变量 }}` / `{% if %}` / `{% for %}` /
`{% include %}` / `{% raw %}` / `{# 注释 #}`，**纯头文件、只用标准库**，不依赖
`library/json`，也不依赖仓库里其它任何东西。

适合这些场景：快速拼一段 DSL、生成代码 / 配置 / 报告文本、把"模型类 → 文本"
的那一步从手写 `<<` 拼串里拿出来。

## 文件

| 文件 | 内容 |
| --- | --- |
| `engine.hpp` | 渲染引擎：`tmpl::render` / `renderValues` / `renderBlock`、标签解析、空白规则 |
| `value.hpp` | 动态值 `tmpl::Value`（null / string / int64 / bool / array / object） |
| `model.hpp` | `Model` 基类与 `ValueModel`（把动态值当模型用） |
| `bind.hpp` | `TEMPLATE_MODEL(...)` / `tmpl::field(...)`：模型类字段绑定 |
| `naming.hpp` | `toSnakeCase` / `toCamelCase` |
| `main.cpp` | 演示：同一份模型渲染成两份目标语言代码（含 `{% include %}` 拼片段） |
| `test_template.cpp` | 自检（变量、条件、循环、片段、注释、空白规则、报错） |

## 快速开始

```cpp
#include "template/engine.hpp"   // 从仓库根起算（构建时加 -Ilibrary）

#include <iostream>
#include <string>
#include <vector>

struct Method
{
    std::string ret;
    std::string name;
};

struct Class
{
    std::string name;
    std::vector<Method> methods;
};

TEMPLATE_MODEL(Method, tmpl::field("ret", &Method::ret),
               tmpl::field("name", &Method::name));
TEMPLATE_MODEL(Class, tmpl::field("name", &Class::name),
               tmpl::field("methods", &Class::methods));

int main()
{
    Class model;
    model.name = "Calc";
    model.methods.push_back(Method{"int", "add"});
    model.methods.push_back(Method{"void", "reset"});

    const std::string source = R"(
class {{ name }} {
{% for m in methods %}
    {{ m.ret }} {{ m.name }}();
{% endfor %}
}
)";
    std::cout << tmpl::render(source, model);
}
```

三种给数据的方式，按需要挑：

1. **模型类**：`TEMPLATE_MODEL(类型, tmpl::field("字段名", &类型::成员))` +
   `tmpl::render(source, model)`（最正式，编译期就能查字段名）；
2. **名字 → 值**：`tmpl::renderValues(source, {{"mode_name", "LALR(1)"}, ...})`
   （临时拼一段文本时最省事）；
3. **动态值**：`tmpl::Object` + `tmpl::ValueModel`（值在运行时才知道，比如读配置）。

模板拆成多段时再加一层 `tmpl::SnippetTable`（`{% include %}` 的片段表，见下一节）；
它是这三个调用的**第三个可选实参**，不传就是空表。

## 语法

| 写法 | 含义 |
| --- | --- |
| `{{ a.b.c }}` | 取值（对象 / 数组下标 / 嵌套字段）；取不到就渲染成空串 |
| `{% if x %}…{% else %}…{% endif %}` | 条件；`x` 可以是值、比较、`not` / `and` / `or` |
| `{% for x in list %}…{% endfor %}` | 循环；循环内可用 `loop_index` / `loop_first` / `loop_last` / `loop_count` |
| `{% raw %}…{% endraw %}` | 原样输出（里面的 `{{ }}` / `{% %}` 不当标签） |
| `{# 注释 #}` | 注释，不输出 |
| `{% include "名字" %}` | 插入另一段模板：片段表（名字 → 片段正文）由调用方给，片段里还能再 include |

- **块标签独占一行时整行删除**（不会留下空行）；想让空段保留一行，把标签写在行内。
  这条对 `{% include %}` 同样有效——独占一行的 include 就是"这一行换成片段正文"。
- 模板里要写目标语言自己的 `{{` / `}}`（C++ 初始化器、C# 字典等）时，写成 `{ {` / `} }`
  或包进 `{% raw %}…{% endraw %}`，否则会被当成变量标签吃掉。
- 模板**不读文件**：模板字符串由调用方给（从文件读、从内嵌资源取、命令行传入都行）。
- 缺字段、空列表、缺片段、不认识的指令都不会崩，只是输出空串；标签不闭合（或
  include 嵌套过深）会抛 `std::runtime_error`，消息里带着那个没配对的标签名。

## 片段：`{% include "名字" %}`

把大模板拆成几段、或者复用一段公共前缀，就用它：片段表是**调用方**的东西
（`tmpl::SnippetTable`，就是 `名字 → 片段正文`），引擎只负责在渲染到那一行时查表。

```cpp
const tmpl::SnippetTable snippets = {
    {"header",  "// 由 {{ name }} 自动生成，请勿手改\n"},
    {"methods", "{% for m in methods %}    {{ m.ret }} {{ m.name }}();\n{% endfor %}"},
    {"whole",   "{% include \"header\" %}class {{ name }} {\n{% include \"methods\" %}};\n"},
};

// 第三个别参数就是片段表；不传就是空表（模板里的 include 一律输出空串）
std::cout << tmpl::render("{% include \"whole\" %}", model, snippets);
```

几条约定：

| 事情 | 行为 |
| --- | --- |
| 片段名怎么写 | `{% include "a" %}` / `{% include 'a' %}` / `{% include a %}` 都认；名字里不认转义，所以 `"a b"` 这种带空格的名字要加引号 |
| 会读到哪些数据 | 片段正文**按模板渲染**，用的是**同一个上下文**：片段里能用外层变量，放进 `{% for %}` 里还能取到循环变量、`loop_*` |
| 嵌套 | 片段里再写 `{% include %}` 就是嵌套包含；层数上限 `tmpl::TemplateEngine::k_maxIncludeDepth`（32），超了抛 `std::runtime_error`——片段互相包含不会把栈转爆 |
| 片段里标签没闭合 | 和顶层模板一样抛 `std::runtime_error`（错误信息里带标签名） |
| 表里没有 / 没给表 / 名字写坏了 | 输出空串，不报错（和"缺变量、空列表"一个口径） |

片段表的键就是名字，值是**模板正文**（不是文件路径）——想按名字去磁盘上找文件，
就在渲染前自己把文件读进来填表；想做懒加载也可以，引擎只在渲染到那一行时才查表。


## 构建与自检

```sh
make -C library/template          # 编译 demo + 自检，并各跑一遍
make -C library/template check    # 同上
make -C library/template clean
```

或者用 CMake：

```sh
cmake -S library/template -B build/template
cmake --build build/template
ctest --test-dir build/template --output-on-failure
```

## 和 ParseEngine 生成器的关系

**已被生成器用来渲染产物模板**：`src/project.cpp` 与 `src/code_gen_cpp17.cpp`
include 了 `template/engine.hpp`（`-Ilibrary` 提供的搜索路径）。目前用到的地方：

- **C++17 的 23 份代码模板**（`cpp17_*.tmpl`，全部）——`{{ 变量 }}` 填生成期算好的
  表 / 片段，`{% if lex %}` 选解析产物还是词法产物的措辞，`{% if parse_tree %}` /
    `{% if tree_class %}` 决定 `Engine::parseTree` 与 `Tree` 的实现要不要出现；
- 构建脚本：`CMakeLists_{lex,lex_cpp17,custom,custom_cpp17}.txt.tmpl` 与
  `Makefile_{lex,lex_cpp17,custom,custom_cpp17}.tmpl`（`{% for %}` 展开 `%copy`
  清单、`{% if user_header %}` 决定要不要 `parser_user.h`）；
- 生成说明：`RESULT.md.tmpl`（`{{ 变量 }}` 填生成信息 + `{% for %}` 展开产物
  清单）与 `result_*.md.tmpl` 那批小节片段。
- **`{% include %}` 唯一的实际用户**是 `RESULT.md.tmpl`：它的三段静态文案
  （「编译运行」「运行时与内存」「语法树」）分别放在
  `result_run_section_{python,c_cpp17}.md.tmpl`、
  `result_runtime_section_{custom,cpp17,python}.md.tmpl`、
  `result_tree_section_{custom,cpp17,python}.md.tmpl` 里，由
  `{% include "run_section" %}` / `"runtime_section"` / `"tree_section"` 拼进来；
  片段名固定，取哪一份由 `generatorResultMd` 往片段表里放哪一份决定。

  C99 / Python / C# 的**代码**模板仍走 `include/templates/*.template` 的
  `__占位符__` + `replaceAll`——那批还没迁。

这个库同时是给**用户**用的：想在别的项目里快速拼一段 DSL、生成配置或报告文本，
直接把 `include` 路径指到 `library/template/` 即可，不用拉别的依赖。
