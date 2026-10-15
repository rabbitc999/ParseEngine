# JSON 库（`library/json`）

现代 C++17 的 JSON：解析、序列化，**数组与对象都能直接用 `for` 和 `<algorithm>` 遍历**，
外加一层「JSON ↔ 模型类」的投射（`bind.hpp`）。只依赖标准库，不参与 ParseEngine
主程序的构建。

## 特点

| 特点 | 说明 |
| --- | --- |
| 现代 C++ | `std::variant` 存储、`std::visit` / `if constexpr`、`std::optional` 取值、`std::string_view`、成员指针 + 折叠表达式、结构化绑定、`[[nodiscard]]` |
| 迭代器 | `Array` / `Object` 暴露标准迭代器；`Value` 也能迭代（数组给元素、对象给成员） |
| STL 友好 | `std::for_each` / `count_if` / `find_if` / `any_of` / `none_of` / `transform` / `accumulate` 直接可用 |
| 对象保序 | `Object` 按插入（解析时即文件）顺序保存成员，`dump` 也按这个顺序写回 |
| 模型投射 | `JSON_MODEL` 声明字段，`encode` / `decode` 双向转换，支持嵌套模型、`vector`、`map`、`optional`、枚举 |
| 诊断 | 解析错误给行列与短码；投射错误给 **JSON 路径**（`points[1].x`） |
| 选项 | 允许注释（`//`、`/* */`）与尾逗号（配置文件的常见写法） |

## 快速开始

```cpp
#include "json.hpp"

const json::ParseResult doc = json::parse(R"({"name":"calc","count":2,"tokens":["+","-"]})");
if (!doc.ok()) {
    std::cerr << doc.diagnostics.front().toString() << "\n";   // 1:5: 期望... [code]
    return 1;
}

const json::Value &root = *doc.value;
const std::string name = std::string(root.find("name")->stringOr("?"));
const int count = static_cast<int>(root.find("count")->intOr(0));
const json::Value *first = root.atPath("tokens[0]");           // 路径查询

std::cout << root.dump(2) << "\n";                             // 缩进 2 空格；dump() 是单行
```

## 遍历与 STL 算法

这是本库最当"一等公民"的地方：解析出来的东西就是容器，不需要先转成别的类型。

```cpp
const json::ParseResult doc = json::parse(R"({"scores":[3,1,2]})");
json::Value &root = *doc.value;
json::Array &scores = *root.find("scores")->asArray();

// 1. 范围 for
for (const json::Value &item : scores) total += static_cast<int>(item.asInt().value());

// 2. STL 算法
const auto threes = std::count_if(scores.begin(), scores.end(),
                                 [](const json::Value &v) { return v.asInt() == 3; });
const auto found  = std::find_if(scores.begin(), scores.end(),
                                 [](const json::Value &v) { return v.asInt() == 1; });
std::vector<int> numbers(scores.size());
std::transform(scores.begin(), scores.end(), numbers.begin(),
               [](const json::Value &v) { return static_cast<int>(v.asInt().value()); });
```

对象同样可以直接遍历，成员是**公开字段**，所以结构化绑定可用：

```cpp
for (const auto &[key, value] : *root.asObject()) {
    std::cout << key << " = " << value.dump() << "\n";
}
std::for_each(object.begin(), object.end(),
              [](const json::Member &member) { /* member.key / member.value */ });
```

`Value` 自身也能迭代，适合"不关心是数组还是对象"的通用遍历（写统计、格式化、
校验都不必分两套代码）：

```cpp
json::Value &value = *doc.value;
for (const auto &entry : value) {
    // 数组元素：entry.index 有值、entry.key 为空
    // 对象成员：entry.key 是成员名、entry.index 是 npos
    std::cout << entry.key << " → " << entry.get().typeName() << "\n";
}
```

> `Value::begin()/end()` 只提供非 const 版本（迭代给出的引用可写）；只读遍历请用
> `asArray()` / `asObject()`，它们有 const 重载。

## 模型投射（`bind.hpp`）

C++ 没有内置反射（静态反射要到 C++26 才进标准），所以"模型类 ↔ JSON"必须由某一方
声明字段。本库选**成员指针表**，C++17 的语法就够：

```cpp
struct Token { std::string name; int priority = 0; std::optional<std::string> pattern; };
struct Config {
    std::string name;
    int version = 0;
    std::vector<Token> tokens;
    std::map<std::string, std::string> labels;
};

// 显式特化只能写在全局作用域（类型可以放在命名空间里，写全限定名）
JSON_MODEL(Token,
           json::field("name", &Token::name),
           json::field("priority", &Token::priority),
           json::field("pattern", &Token::pattern));

JSON_MODEL(Config,
           json::field("name", &Config::name),
           json::field("version", &Config::version),
           json::field("tokens", &Config::tokens),
           json::field("labels", &Config::labels));

const json::Value json = json::encode(config);                    // 模型 → JSON
const json::DecodeResult<Config> back = json::decode<Config>(json); // JSON → 模型
if (!back.ok()) {
    for (const json::Diagnostic &item : back.diagnostics) {
        std::cerr << item.toString() << "\n";   // 例如：tokens[1].priority: 期望整数，实际是 string
    }
}
```

支持的类型：

| 类型 | 编码 | 解码 |
| --- | --- | --- |
| `bool` / 整数 / 浮点 | 对应的 JSON 标量 | 类型不符报 `type-mismatch`；`allowStringNumbers` 时 `"12"` 也能喂给整数 |
| `std::string` | 字符串 | 必须是字符串 |
| `std::string_view` / `const char *` | 字符串 | 只能编码（解码器拒绝，避免悬垂） |
| `std::optional<T>` | 空 → `null` | `null` → 空；**字段缺席也不算错** |
| `std::vector<T>` | 数组 | 必须是数组，逐元素解码（路径带 `[i]`） |
| `std::map<std::string,T>` / `std::unordered_map<std::string,T>` | 对象 | 必须是对象 |
| `enum` | 底层整数 | 整数或数字字符串 |
| `json::Value` | 原样透传 | 原样透传 |
| 有 `JSON_MODEL` 的模型 | 对象（字段顺序 = 声明顺序） | 必须是对象 |

解码选项（`json::DecodeOptions`）：

| 选项 | 默认 | 作用 |
| --- | --- | --- |
| `strictMissing` | `true` | 缺字段报 `missing-field`（`optional` 字段豁免） |
| `strictUnknown` | `false` | 多出来的字段报 `unknown-field` |
| `allowStringNumbers` | `true` | `"12"` 允许解成整数 / 浮点 / 枚举 |
| `enumAsString` | `false` | 编码枚举时用 `to_string`（解码两种都认） |

## 命令行

```text
json <命令> [文件] [选项]

  fmt              格式化（--indent N / --compact）
  check            校验，打印诊断
  get <路径> [文件] 按路径取值（--raw：字符串不加引号）
  keys [路径] [文件] 列出成员名
  stats            统计各类型节点数与最大深度

  --out FILE       输出写文件；--comments / --trailing-commas 放宽解析
```

```text
$ json fmt examples/config.json --compact
{"name":"pl0-super","version":2,...}
$ json get limits.maxDepth examples/config.json
64
$ json stats examples/config.json
object: 5
array.items: 4
maxdepth: 4
```

文件缺省从标准输入读；`get` / `keys` 的路径语法是 `a.b[0].c`。

## 构建与自检

```text
make -C library/json            # libjson.a + json + json-tests + model-demo
make -C library/json check      # 单元测试 + CLI 演示 + 投射演示
```

产物在 `build/json`。`check` 跑 170 多条断言：标量 / 转义 / Unicode 代理对 / 数字
往返 / 十几类解析错误（含行列）/ 对象保序 / 路径查询 / dump 与往返 / 比较语义 /
迭代器与 STL 算法 / 模型投射与诊断路径，最后用 CLI 与 `model-demo` 各跑一遍
`examples/config.json`。

## 目录

```text
library/json/
├── json.hpp / json.cpp          值 / 数组 / 对象 / 解析 / 序列化 / 路径 / 迭代器
├── bind.hpp                     模型投射：JSON_MODEL、encode、decode
├── main.cpp                     命令行工具
├── test_json.cpp                单元测试（含 STL 算法与投射用例）
├── examples/
│   ├── config.json              示例数据
│   └── model_demo.cpp           投射演示：JSON → 模型 → JSON
├── Makefile / CMakeLists.txt    两种构建方式
└── README.md
```

## 与 `library/lsp/json.hpp` 的关系

`library/lsp` 里那份 JSON 是**为语言服务器写的最小实现**（够用就好，直接内嵌在
服务器里）。本库是独立、通用的那一份：多了迭代器、STL 友好容器、路径查询、
模型投射、注释 / 尾逗号选项与更细的诊断。两者互不依赖，`.gram` 语言服务器继续
用它自己那份，不因为本库而改变行为。
