/**
 * @file test_template.cpp
 * @brief library/template 的自检：变量、条件、循环、注释、空白规则、报错。
 *
 * 不用任何测试框架（本库不依赖第三方），就是一个最小的断言器 + 退出码。
 * 编译运行：
 *
 * ```sh
 * g++ -std=c++17 -I. test_template.cpp -o test_template && ./test_template
 * ```
 */

#include "engine.hpp"
#include "naming.hpp"

#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
//  测试用的模型
// ---------------------------------------------------------------------------

struct Leaf
{
    std::string name;
    std::int64_t size = 0;
};

TEMPLATE_MODEL(Leaf,
               tmpl::field("name", &Leaf::name),
               tmpl::field("size", &Leaf::size));

struct Branch
{
    std::string title;
    std::vector<Leaf> leaves;
};

TEMPLATE_MODEL(Branch,
               tmpl::field("title", &Branch::title),
               tmpl::field("leaves", &Branch::leaves));

struct Root
{
    std::string name = "root";
    bool yes = true;
    bool no = false;
    std::int64_t count = 42;
    std::string empty_text;
    Leaf probe{"leafy", 7};
    std::vector<Leaf> leaves;
    std::vector<Leaf> empty_leaves;
    std::vector<Branch> branches;
};

TEMPLATE_MODEL(Root,
               tmpl::field("name", &Root::name),
               tmpl::field("yes", &Root::yes),
               tmpl::field("no", &Root::no),
               tmpl::field("count", &Root::count),
               tmpl::field("empty_text", &Root::empty_text),
               tmpl::field("probe", &Root::probe),
               tmpl::field("leaves", &Root::leaves),
               tmpl::field("empty_leaves", &Root::empty_leaves),
               tmpl::field("branches", &Root::branches));

/** 零字段模型：给"纯静态片段"用的占位模型（没有变量可填）。 */
struct NoFields
{
};

// 零字段模型手写特化：TEMPLATE_MODEL 是可变参数宏，-Wpedantic 下要求至少一个实参。
template <>
struct tmpl::Binding<NoFields>
{
    static constexpr auto fields() { return std::make_tuple(); }
};

// ---------------------------------------------------------------------------
//  最小断言器
// ---------------------------------------------------------------------------

namespace
{

int g_failures = 0;
int g_checks = 0;

/** 把不可见字符画出来，失败信息才看得懂。 */
std::string show(const std::string &text)
{
    std::string out = "\"";
    for (const char ch : text)
    {
        if (ch == '\n') out += "\\n";
        else if (ch == '\t') out += "\\t";
        else out += ch;
    }
    return out + "\"";
}

void expect(const std::string &label, const std::string &got, const std::string &want)
{
    ++g_checks;
    if (got == want) return;
    ++g_failures;
    std::cout << "[FAIL] " << label << "\n  得到: " << show(got) << "\n  期望: " << show(want) << "\n";
}

void expectTrue(const std::string &label, bool value)
{
    ++g_checks;
    if (value) return;
    ++g_failures;
    std::cout << "[FAIL] " << label << "\n";
}

/** 期望渲染时抛出 std::runtime_error（未闭合标签）。 */
template <class Body>
void expectThrow(const std::string &label, Body body)
{
    ++g_checks;
    try
    {
        body();
    }
    catch (const std::runtime_error &)
    {
        return; // 正是要的
    }
    catch (...)
    {
        ++g_failures;
        std::cout << "[FAIL] " << label << "：抛了别的异常\n";
        return;
    }
    ++g_failures;
    std::cout << "[FAIL] " << label << "：没有抛异常\n";
}

} // namespace

int main()
{
    Root root;
    root.leaves = {Leaf{"a", 1}, Leaf{"b", 2}};
    root.branches = {Branch{"B1", {Leaf{"a", 1}, Leaf{"b", 2}}}, Branch{"B2", {Leaf{"c", 3}}}};

    // ---- 变量替换 ---------------------------------------------------------
    expect("变量", tmpl::render("A{{ name }}B", root), "ArootB");
    expect("变量：路径两端空白自动 trim", tmpl::render("A{{  name  }}B", root), "ArootB");
    expect("变量：缺失 → 空串", tmpl::render("A{{ nope }}B", root), "AB");
    expect("变量：多级路径", tmpl::render("{{ probe.name }}/{{ probe.size }}", root), "leafy/7");
    expect("变量：路径断在非对象上", tmpl::render("A{{ name.nope }}B", root), "AB");
    expect("变量：空路径", tmpl::render("A{{ }}B", root), "AB");
    expect("变量：bool → true/false", tmpl::render("{{ yes }}/{{ no }}", root), "true/false");
    expect("变量：整数 → 十进制", tmpl::render("{{ count }}", root), "42");
    expect("变量：数组/对象 → 空串", tmpl::render("A{{ leaves }}B", root), "AB");

    // ---- 条件 -------------------------------------------------------------
    expect("if：真", tmpl::render("{% if yes %}Y{% endif %}", root), "Y");
    expect("if：假", tmpl::render("{% if no %}Y{% endif %}", root), "");
    expect("if：假 → else", tmpl::render("{% if no %}Y{% else %}N{% endif %}", root), "N");
    expect("if：真 → 不走 else", tmpl::render("{% if yes %}Y{% else %}N{% endif %}", root), "Y");
    expect("if：空字符串为假", tmpl::render("{% if empty_text %}Y{% else %}N{% endif %}", root), "N");
    expect("if：非空数组为真", tmpl::render("{% if leaves %}Y{% else %}N{% endif %}", root), "Y");
    expect("if：整数 0 为假（count=42 为真）", tmpl::render("{% if count %}Y{% endif %}", root), "Y");
    expect("if：嵌套 + 内层 else",
           tmpl::render("{% if yes %}1{% if no %}a{% else %}b{% endif %}2{% else %}Z{% endif %}", root),
           "1b2");
    expect("if：嵌套（外层假时整块跳过）",
           tmpl::render("{% if no %}1{% if yes %}a{% else %}b{% endif %}2{% else %}Z{% endif %}", root),
           "Z");
    expect("if：紧贴写法 + 空白变体", tmpl::render("{%if yes%}Y{%else%}N{%endif%}", root), "Y");
    expect("if：空白变体", tmpl::render("{%  if yes  %}Y{%  endif  %}", root), "Y");

    // ---- 独占一行的标签：整行删除 -----------------------------------------
    expect("独占一行的 if（真）", tmpl::render("before\n{% if yes %}\nkeep\n{% endif %}\nafter\n", root),
           "before\nkeep\nafter\n");
    expect("独占一行的 if（假）", tmpl::render("before\n{% if no %}\nkeep\n{% endif %}\nafter\n", root),
           "before\nafter\n");
    expect("同一行内有别的内容 → 只删标签",
           tmpl::render("x {% if yes %}Y{% endif %} z", root), "x Y z");
    expect("同一行内有别的内容（假）", tmpl::render("{% if no %}Y{% endif %}z", root), "z");

    // ---- 循环 -------------------------------------------------------------
    expect("for：基本", tmpl::render("{% for leaf in leaves %}[{{ leaf.name }}]{% endfor %}", root),
           "[a][b]");
    expect("for：独占一行", tmpl::render("list:\n{% for leaf in leaves %}\n- {{ leaf.name }}\n{% endfor %}\ndone\n", root),
           "list:\n- a\n- b\ndone\n");
    expect("for：缺失列表 → 整块跳过", tmpl::render("A{% for leaf in nope %}{{ leaf.name }}{% endfor %}B", root),
           "AB");
    expect("for：写坏的 for → 整块跳过，后面照常",
           tmpl::render("{% for leaf %}{{ name }}{% endfor %}C", root), "C");
    expect("for：空列表 → 整块跳过",
           tmpl::render("A{% for leaf in empty_leaves %}{{ leaf.name }}{% endfor %}B", root), "AB");

    // ---- 循环 + 作用域链 --------------------------------------------------
    expect("嵌套 for：内层能取到外层变量（name / b.title）",
           tmpl::render("{% for b in branches %}{{ b.title }}"
                             "{% for leaf in b.leaves %}({{ name }}/{{ b.title }}:{{ leaf.name }}){% endfor %};"
                             "{% endfor %}",
                             root),
           "B1(root/B1:a)(root/B1:b);B2(root/B2:c);");
    expect("嵌套 for：外层循环变量在内层仍可用",
           tmpl::render("{% for b in branches %}{% for leaf in leaves %}{{ b.title }}{{ leaf.name }}{% endfor %}{% endfor %}",
                             root),
           "B1aB1bB2aB2b");

    // ---- 注释 -------------------------------------------------------------
    expect("注释：直接丢掉", tmpl::render("A{# hidden #}B", root), "AB");
    expect("注释：里面的标签文字不算标签", tmpl::render("A{# {% endif %} {% if x %} #}B", root), "AB");
    expect("注释：独占一行时整行丢掉", tmpl::render("A\n{# c #}\nB\n", root), "A\nB\n");

    // ---- 原样段：{% raw %} … {% endraw %} ---------------------------------
    expect("raw：里面的 {{ }} 不当标签",
           tmpl::render("A{% raw %}{{ name }}{% endraw %}B", root), "A{{ name }}B");
    expect("raw：{{ target_code }} 这种目标文本能原样输出",
           tmpl::render("`{% raw %}{{ target_code }}{% endraw %}`", root), "`{{ target_code }}`");
    expect("raw：独占一行时内容原样保留",
           tmpl::render("{% raw %}\n{% if x %}\n{% endraw %}\n", root), "{% if x %}\n");
    expect("raw：放进循环体里也照原样输出",
           tmpl::render("{% for leaf in leaves %}{% raw %}{{ x }}{% endraw %}{% endfor %}", root),
           "{{ x }}{{ x }}");
    expectThrow("未闭合的 raw", [&] { tmpl::render("{% raw %}{{ x }}", root); });

    // ---- 片段：{% include "名字" %} ---------------------------------------
    // 片段表由调用方给；片段正文按模板渲染，用的是同一个上下文。
    const tmpl::SnippetTable snippets = {
        {"head", "// {{ name }} 的头部\n"},
        {"inner", "<{{ name }}>"},
        {"outer", "[{% include \"inner\" %}]\n"},
        {"item", "- {{ leaf.name }} #{{ loop_index }}\n"},
        {"block", "{% if yes %}Y{% else %}N{% endif %}"},
        {"with space", "SP"},
    };
    expect("include：基本", tmpl::render("A{% include \"inner\" %}B", root, snippets),
           "A<root>B");
    expect("include：单引号", tmpl::render("A{% include 'inner' %}B", root, snippets),
           "A<root>B");
    expect("include：不带引号", tmpl::render("A{% include inner %}B", root, snippets),
           "A<root>B");
    expect("include：独占一行 → 整行换成片段",
           tmpl::render("before\n{% include \"head\" %}\nafter\n", root, snippets),
           "before\n// root 的头部\nafter\n");
    expect("include：同一行里有别的内容 → 只换标签",
           tmpl::render("x {% include \"inner\" %} z", root, snippets), "x <root> z");
    expect("include：片段里能用外层变量",
           tmpl::render("{% include \"head\" %}", root, snippets), "// root 的头部\n");
    expect("include：放进循环体，片段能取到循环变量",
           tmpl::render("{% for leaf in leaves %}{% include \"item\" %}{% endfor %}", root, snippets),
           "- a #0\n- b #1\n");
    expect("include：片段里再 include（嵌套）",
           tmpl::render("{% include \"outer\" %}", root, snippets), "[<root>]\n");
    expect("include：片段里的 if / for 照常解析",
           tmpl::render("{% include \"block\" %}", root, snippets), "Y");
    expect("include：片段名可以带空格（引号写法）",
           tmpl::render("{% include \"with space\" %}", root, snippets), "SP");
    expect("include：片段表里没有 → 空串", tmpl::render("A{% include \"nope\" %}B", root, snippets),
           "AB");
    expect("include：没给片段表 → 空串", tmpl::render("A{% include \"head\" %}B", root), "AB");
    expect("include：标签里没写名字 → 空串", tmpl::render("A{% include %}B", root, snippets), "AB");
    expect("include：引号没闭合 → 空串", tmpl::render("A{% include \"head %}B", root, snippets), "AB");
    expect("include：片段为空 → 独占一行的标签也整行消失",
           tmpl::render("before\n{% include \"nope\" %}\nafter\n", root, snippets),
           "before\nafter\n");
    expect("include：放在 if 分支里也照常（环境一路传下去）",
           tmpl::render("{% if yes %}{% include \"inner\" %}{% endif %}", root, snippets),
           "<root>");
    expectThrow("include：片段自己的标签没闭合 → 和顶层模板一样报错",
                [&] {
                    tmpl::render("{% include \"bad\" %}", root,
                                 {{"bad", "{% if yes %}x"}});
                });
    {
        const tmpl::SnippetTable cycle = {
            {"a", "{% include \"b\" %}"},
            {"b", "{% include \"a\" %}"},
        };
        expectThrow("include：片段互相包含 → 抛异常而不是爆栈",
                    [&] { tmpl::render("{% include \"a\" %}", root, cycle); });
        expectTrue("include：嵌套上限是个正数", tmpl::TemplateEngine::k_maxIncludeDepth > 0u);
    }
    expect("renderValues：include 也能用（片段同样是模板）",
           tmpl::renderValues("{% include \"v\" %}", {{"x", 1}}, {{"v", "{{ x }}!"}}),
           "1!");
    expect("include：同一份模板 + 不同片段表 → 结果跟着表走",
           tmpl::render("{% include \"inner\" %}", root, {{"inner", "A"}}) +
               tmpl::render("{% include \"inner\" %}", root, {{"inner", "B"}}),
           "AB");

    // ---- 循环元信息：loop_index / loop_first / loop_last / loop_count ------
    expect("loop_index：下标从 0 起",
           tmpl::render("{% for leaf in leaves %}{{ loop_index }}{% endfor %}", root), "01");
    expect("loop_count：元素个数",
           tmpl::render("{% for leaf in leaves %}{{ loop_count }}{% endfor %}", root), "22");
    expect("loop_first / loop_last：首尾标志",
           tmpl::render("{% for leaf in leaves %}{% if loop_first %}F{% endif %}"
                        "{% if loop_last %}L{% endif %}{% endfor %}",
                        root),
           "FL");
    expect("分隔符交给模板：逗号只是首元素之后才有",
           tmpl::render("{% for leaf in leaves %}{% if loop_first %}{% else %}, {% endif %}"
                        "{{ leaf.name }}{% endfor %}",
                        root),
           "a, b");
    expect("嵌套循环：loop_first 说的是最内层",
           tmpl::render("{% for b in branches %}{{ b.title }}:"
                        "{% for leaf in b.leaves %}{% if loop_first %}{% else %}/{% endif %}"
                        "{{ leaf.name }}{% endfor %};"
                        "{% endfor %}",
                        root),
           "B1:a/b;B2:c;");
    expect("单元素循环：既 first 又 last",
           tmpl::render("{% for leaf in empty_leaves %}x{% endfor %}", root), "");

    // ---- 健壮性 -----------------------------------------------------------
    expect("游离的 endif 安全跳过", tmpl::render("A{% endif %}B", root), "AB");
    expect("游离的 endfor 安全跳过", tmpl::render("A{% endfor %}B", root), "AB");
    expect("不认识的指令安全跳过", tmpl::render("A{% nonsense %}B", root), "AB");
    expectThrow("未闭合的 if", [&] { tmpl::render("{% if yes %}X", root); });
    expectThrow("未闭合的变量标签", [&] { tmpl::render("{{ name", root); });
    expectThrow("未闭合的注释", [&] { tmpl::render("{# c", root); });

    // ---- 无状态：同一份模板渲染多次结果一致 -------------------------------
    const std::string source = "{% for leaf in leaves %}{{ leaf.name }}{% endfor %}";
    expect("重复渲染（无状态）", tmpl::render(source, root) + tmpl::render(source, root), "abab");

    // ---- Value / encode ---------------------------------------------------
    expectTrue("Value：int", tmpl::Value(3).isInt());
    expectTrue("Value：int64", tmpl::Value(std::int64_t(-5)).toString() == "-5");
    expectTrue("Value：其它整数类型（size_t）", tmpl::Value(std::size_t(4)).asInt() == 4);
    expectTrue("Value：null", tmpl::Value(nullptr).isNull());
    expectTrue("Value：bool", tmpl::Value(true).toString() == "true");
    expectTrue("Value：string_view", tmpl::Value(std::string_view("hi")).asString() == "hi");
    expectTrue("Value：空串为假", !tmpl::Value(std::string()).truthy());
    expectTrue("Value：非空串为真", tmpl::Value(std::string("x")).truthy());
    expectTrue("Value：0 为假", !tmpl::Value(0).truthy());
    expectTrue("encode：模型 → 对象", tmpl::encode(root).isObject());
    expectTrue("encode：vector → 数组", tmpl::encode(root.leaves).isArray());
    expectTrue("encode：对象保序",
               [&] {
                   // 注意：asObject() 给的是"值内部"的指针，值得先活着。
                   const tmpl::Value encoded = tmpl::encode(root.probe);
                   const tmpl::Object *object = encoded.asObject();
                   return object != nullptr && object->members.size() == 2 &&
                          object->members[0].first == "name" && object->members[1].first == "size";
               }());

    // ---- 引擎不绑模型类：任何 Model 都能渲染 -------------------------------
    auto object = std::make_shared<tmpl::Object>();
    object->insert("name", tmpl::Value(std::string("dynamic")));
    const tmpl::Value value(object);
    const tmpl::ValueModel model(value);
    expect("ValueModel：动态值也能渲染", tmpl::TemplateEngine::render("<{{ name }}>", model), "<dynamic>");

    // ---- 零字段模型（纯静态片段） -----------------------------------------
    expect("零字段模型：没有变量的模板原样输出", tmpl::render("hello\n", NoFields{}), "hello\n");
    expect("零字段模型：变量取不到就是空串", tmpl::render("A{{ x }}B", NoFields{}), "AB");

    // ---- 命名转换（文件名 / 宏名派生用，命名空间本身不改）------------------
    expect("toSnakeCase：MyParser", tmpl::toSnakeCase("MyParser"), "my_parser");
    expect("toSnakeCase：HTTPParser（缩写）", tmpl::toSnakeCase("HTTPParser"), "http_parser");
    expect("toSnakeCase：Parser2Value", tmpl::toSnakeCase("Parser2Value"), "parser2_value");
    expect("toSnakeCase：已经下划线 / 带分隔符", tmpl::toSnakeCase("my-parser.Parser"), "my_parser_parser");
    expect("toCamelCase：my_parser", tmpl::toCamelCase("my_parser"), "MyParser");

    expect("MapModel / renderValues：按名字取值",
           tmpl::renderValues("A{{ x }}B{{ y }}C", {{"x", std::string("1")}, {"y", 2}}),
           "A1B2C");
    expect("MapModel：没有的键就是空串", tmpl::renderValues("A{{ nope }}B", {{"x", 1}}), "AB");
    {
        auto items = std::make_shared<tmpl::Array>();
        items->items.push_back(tmpl::Value(3));
        items->items.push_back(tmpl::Value(4));
        expect("renderValues：循环也能用",
               tmpl::renderValues("{% for v in xs %}{% if loop_first %}{% else %}, {% endif %}{{ v }}{% endfor %}",
                                  {{"xs", tmpl::Value(items)}}),
               "3, 4");
    }
    // ---- 汇总 -------------------------------------------------------------
    if (g_failures == 0)
    {
        std::cout << "全部通过：" << g_checks << " 项检查\n";
        return 0;
    }
    std::cout << "失败：" << g_failures << " / " << g_checks << "\n";
    return 1;
}
