/**
 * @file main.cpp
 * @brief library/template 的示例：把 C++ 模型类渲染成任意目标语言的代码。
 *
 * 编译运行（只用标准库，C++17）：
 *
 * ```sh
 * g++ -std=c++17 main.cpp -o test && ./test
 * ```
 *
 * 演示的是编译器后端最常见的那条链路：
 *
 * ```
 * IR（语法树 / 符号表）
 *   → 转换层：把 IR 里的类、方法、字段抄进模型类（下面这两个 struct）
 *   → 模板渲染：tmpl::render(source, model)
 *   → 目标语言源码
 * ```
 */

#include "engine.hpp"

#include <iostream>
#include <memory>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
//  1. 模型类：直接照着 IR 的形状写，字段就是模板里要用到的名字
// ---------------------------------------------------------------------------

/** 一个方法（IR 里的函数声明投影过来的）。 */
struct MethodModel
{
    std::string name;          ///< 方法名。
    std::string ret_type;      ///< 返回类型（已经按目标语言渲染好的文本）。
    std::string args;          ///< 形参列表（已经拼好的文本，如 `int a, int b`）。
    bool is_static = false;    ///< 是否静态方法。
};

/** 一个类（IR 里的类型声明投影过来的）。 */
struct ClassModel
{
    std::string name;                  ///< 类名。
    bool has_ctor = false;             ///< IR 里有没有显式构造器。
    std::vector<MethodModel> methods;   ///< 方法表。
};

// ---------------------------------------------------------------------------
//  2. 绑定：字段名 → 成员指针
//
//  同一个类可以同时有 JSON 和 TEMPLATE 两份绑定，它们是两个命名空间里的
//  两个特化，各写各的、互不干扰（想给上面两个类也来一套 JSON 投射，就再写
//  JSON_MODEL(MethodModel, json::field("name", &MethodModel::name), ...)）。
//  注意 TEMPLATE_MODEL 展开的是显式特化，只能写在全局作用域。
// ---------------------------------------------------------------------------

TEMPLATE_MODEL(MethodModel,
               tmpl::field("name", &MethodModel::name),
               tmpl::field("ret_type", &MethodModel::ret_type),
               tmpl::field("args", &MethodModel::args),
               tmpl::field("is_static", &MethodModel::is_static));

TEMPLATE_MODEL(ClassModel,
               tmpl::field("name", &ClassModel::name),
               tmpl::field("has_ctor", &ClassModel::has_ctor),
               tmpl::field("methods", &ClassModel::methods));

// ---------------------------------------------------------------------------
//  3. 模板：同一个模型，两份目标语言模板（引擎不读文件，模板就是字符串；
//     真做产品时这些字符串通常来自 include/templates/*.template，由调用方读进来）
// ---------------------------------------------------------------------------

/** 类模板：条件、循环、同行的 if/endif、独占一行的标签都会按预期消失。 */
const std::string kClassTemplate = R"(
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
)";

/** C++ 模板：声明式头文件，顺便演示"循环体里访问外层变量"（{{ name }}）。 */
const std::string kCppTemplate = R"(
// 由 {{ name }} 自动生成，请勿手改
class {{ name }} {
public:
{% for m in methods %}
    {{ m.ret_type }} {{ m.name }}({{ m.args }});
{% endfor %}
};
)";

/**
 * 再加一个模板演示其余语法：注释、嵌套循环 + 嵌套 if、作用域链（内层循环里
 * 取外层变量）、缺失变量（输出空串而不是报错）。
 */
const std::string kFeatureTemplate = R"(
{# 这一整行是注释，独占一行，不会出现在输出里 #}
class {{ name }} {
{% for m in methods %}
    {% if m.is_static %}static {% endif %}{{ m.ret_type }} {{ m.name }}({{ m.args }});
    {# 下面这行演示"内层能看见外层"：m.name 来自循环，name 来自外层 #}
    // {{ name }}::{{ m.name }}{{ not_exist_field }}{{ m.not_exist }}
{% endfor %}
};
)";

/**
 * 片段：`{% include "名字" %}` 把大模板拆成几段，片段表由调用方给。
 *
 * 片段正文**按模板渲染**、用的是**同一个上下文**——所以下面 `methods` 那段能直接
 * 用 model.methods，`banner` 里也能用 model.name；片段里还能再 include（这里没演示，
 * 自检里有）。注意 banner 里那句 `{# … #}` 是注释：片段本身也是模板，注释会被丢掉。
 */
const std::string kFileTemplate = R"({% include "banner" %}#include <string>
#include <vector>

{% include "methods" %})";

/** 片段表：名字 → 片段正文（键是名字，不是文件路径；引擎不碰文件系统）。 */
const tmpl::SnippetTable kFileSnippets = {
    {"banner", "{# 模块头注释：示范「片段也是模板」 #}\n// {{ name }} —— 由模板引擎拼出来\n"},
    {"methods", "{% for m in methods %}{{ m.ret_type }} {{ m.name }}({{ m.args }});\n{% endfor %}"},
};

int main()
{
    // ---- 4. 构造模型：真实项目里这一步是"IR → 模型类"的转换层干的 ----
    ClassModel model;
    model.name = "Calc";
    model.has_ctor = true;
    model.methods.push_back(MethodModel{"add", "int", "int lhs, int rhs", false});
    model.methods.push_back(MethodModel{"reset", "void", "", false});
    model.methods.push_back(MethodModel{"version", "string", "", true});

    // ---- 5. 渲染 ----
    std::cout << "===== 类 =====\n" << tmpl::render(kClassTemplate, model) << '\n';
    std::cout << "===== C++ =====\n" << tmpl::render(kCppTemplate, model) << '\n';
    std::cout << "===== 语法特性 =====\n" << tmpl::render(kFeatureTemplate, model) << '\n';

    // 条件为假时：整个 if 块（含独占一行的 {% if %} / {% endif %} 两行）一起消失。
    ClassModel plain;
    plain.name = "Plain";
    plain.has_ctor = false;
    plain.methods.push_back(MethodModel{"noop", "void", "", false});
    std::cout << "===== has_ctor = false =====\n" << tmpl::render(kClassTemplate, plain) << '\n';

    // 引擎也不绑定模型类：任何 Model 都能渲染（ValueModel 把动态值包成 Model）。
    auto object = std::make_shared<tmpl::Object>();
    object->insert("name", tmpl::Value(std::string("Dynamic")));
    const tmpl::Value dynamic(object);
    const tmpl::ValueModel dynamicModel(dynamic);
    std::cout << "===== 动态值（不经过模型类）=====\n"
              << tmpl::TemplateEngine::render("class {{ name }} { {{ missing }} }\n", dynamicModel);

    // ---- 6. 片段：{% include %} 把几段拼成一份文件（独占一行的 include 整行换成片段）
    std::cout << "===== 片段（{% include %}）=====\n"
              << tmpl::render(kFileTemplate, model, kFileSnippets);

    return 0;
}

// ---------------------------------------------------------------------------
//  接入已有的 IR 转换层
//
//  本引擎只做"模型类 → 文本"，所以接入点非常窄，三步就够：
//
//    1) 在 IR → 目标语言的转换层里，把 IR 节点投影成模型类（就是本文件里的
//       MethodModel / ClassModel 这种"哑结构体"），字段全部用 std::string /
//       bool / std::vector，别把 IR 的指针和生命周期带进来；
//    2) 给模型类写一次 TEMPLATE_MODEL 绑定；同一份模型可以挂多份目标语言模板
//       （cpp_tmpl / python_tmpl / csharp_tmpl），换语言只是换模板字符串；
//    3) 渲染：
//
//           const std::string code = tmpl::render(source, classModel);
//
//       模板字符串从哪来由调用方决定——读文件、从内嵌资源里取、或者命令行传入
//       都行，引擎自己不碰文件系统。模板拆成了几段就用第三个参数把片段表带上：
//
//           const std::string code = tmpl::render(source, classModel, snippets);
//
//   现有的 code_gen_*.cpp 里是"占位符 replaceAll"，换成这条链路以后，条件分支
//   和循环交给模板（{% if %} / {% for %}），生成逻辑会短很多；迁移可以一份模板
//   一份模板地做，不要求一次换完。
//
//   调试提示：模板写错（比如漏了 {% endif %}）会抛 std::runtime_error，消息里
//   带着那个没配对的标签名；缺字段、空列表、缺片段、不认识的指令都不会崩，
//   只是输出空。片段互相包含（{% include %} 转圈）也会抛异常，不会爆栈。
// ---------------------------------------------------------------------------
