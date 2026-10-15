# TOML 库（`library/toml`）

现代 C++17 的 TOML v1.0：解析、序列化，**数组与表都能直接用 `for` 和 `<algorithm>` 遍历**，
外加一层「TOML ↔ 模型类」的投射（`bind.hpp`）。设计对齐同目录的 [`library/json`](../json/README.md)，
只依赖标准库，不参与 ParseEngine 主程序的构建。

## 特点

| 特点 | 说明 |
| --- | --- |
| 现代 C++ | `std::variant` 存储（含 `DateTime`）、`std::visit` / `if constexpr`、`std::optional` 取值、`std::string_view`、成员指针 + 折叠表达式、结构化绑定、`[[nodiscard]]` |
| 迭代器 | `Array` / `Object` 暴露标准迭代器；`Value` 也能迭代（数组给元素、表给成员） |
| STL 友好 | `std::for_each` / `count_if` / `find_if` / `any_of` / `none_of` / `transform` / `accumulate` 直接可用 |
| 表保序 | `Object` 按插入（解析时即文件）顺序保存成员，`dump` 也按这个顺序写回 |
| 完整语法 | 四种字符串、四种整数、浮点 / `inf` / `nan`、四种日期时间、数组、内联表、标准表、表数组、点分键、注释、`\uXXXX` / `\UXXXXXXXX` |
| 冲突检查 | 重复键、表被重定义、内联表被追加、点分键表被具名表头重开、表与值互相占用等都会报错并给行列 |
| 模型投射 | `TOML_MODEL` 声明字段，`encode` / `decode` 双向转换，支持嵌套模型、`vector`、`map`、`optional`、枚举、`DateTime` |
| 诊断 | 解析错误给行列与短码；投射错误给 **TOML 路径**（`points[1].x`） |

## 快速开始

```cpp
#include "toml.hpp"

const toml::ParseResult doc = toml::parse(R"(
    title = "calc"
    count = 2
    tags = ["+", "-"]

    [server]
    host = "localhost"
    port = 8080
)");
if (!doc.ok()) {
    std::cerr << doc.diagnostics.front().toString() << "\n";   // 3:5: 期望... [code]
    return 1;
}

const toml::Value &root = *doc.value;
const std::string title = std::string(root.find("title")->stringOr("?"));
const std::int64_t count = root.find("count")->intOr(0);
const toml::Value *host = root.atPath("server.host");          // 路径查询

std::cout << root.dump() << "\n";                              // 规范化 TOML 文档
```

## 遍历与 STL 算法

解析出来的东西就是容器，不需要先转成别的类型。

```cpp
const toml::ParseResult doc = toml::parse("scores = [3, 1, 2]\n");
toml::Value &root = *doc.value;
toml::Array &scores = *root.find("scores")->asArray();

for (const toml::Value &item : scores) total += static_cast<int>(item.asInt().value());

const auto threes = std::count_if(scores.begin(), scores.end(),
                                  [](const toml::Value &v) { return v.asInt() == 3; });
```

表同样可以直接遍历，成员是**公开字段**，所以结构化绑定可用：

```cpp
for (const auto &[key, value] : *root.find("server")->asTable()) {
    std::cout << key << " = " << value.inlineDump() << "\n";
}
```

`Value` 自身也能迭代（数组给元素、表给成员），适合"不关心是数组还是表"的通用遍历。

> `Value::begin()/end()` 只提供非 const 版本（迭代给出的引用可写）；只读遍历请用
> `asArray()` / `asTable()`，它们有 const 重载。

## 模型投射（`bind.hpp`）

C++ 没有内置反射，所以"模型类 ↔ TOML"由成员指针表声明字段，C++17 的语法就够：

```cpp
struct Token { std::string name; int priority = 0; std::optional<std::string> pattern; };
struct Config {
    std::string name;
    int version = 0;
    std::vector<Token> tokens;
    std::map<std::string, std::string> labels;
    toml::DateTime released;
};

// 显式特化只能写在全局作用域（类型可以放在命名空间里，写全限定名）
TOML_MODEL(Token,
           toml::field("name", &Token::name),
           toml::field("priority", &Token::priority),
           toml::field("pattern", &Token::pattern));

TOML_MODEL(Config,
           toml::field("name", &Config::name),
           toml::field("version", &Config::version),
           toml::field("tokens", &Config::tokens),
           toml::field("labels", &Config::labels),
           toml::field("released", &Config::released));

const toml::Value toml = toml::encode(config);                       // 模型 → TOML
const toml::DecodeResult<Config> back = toml::decode<Config>(toml);  // TOML → 模型
if (!back.ok()) {
    for (const toml::Diagnostic &item : back.diagnostics) {
        std::cerr << item.toString() << "\n";   // 例如：tokens[1].priority: 期望整数，实际是 string
    }
}
```

TOML 没有 `null`：**编码时空的 `std::optional` 字段直接省略**，解码时缺字段就是空值，
`strictMissing` 不会为难可选字段。

## 支持的 TOML 语法

- **字符串**：基本 `"..."`、字面量 `'...'`、多行 `"""..."""` / `'''...'''`（含行尾 `\` 折行、
  开头换行裁剪、连续引号处理），转义 `\b \t \n \f \r \" \\ \uXXXX \UXXXXXXXX`。
- **整数**：十进制（允许 `_`）、`0x` / `0o` / `0b`、前导 `+` / `-`；前导 0 报错。
- **浮点**：小数 / 指数、`_` 分隔、`inf` / `-inf` / `nan`。
- **布尔**：`true` / `false`。
- **日期时间**：带偏移 `1979-05-27T07:32:00Z` / `+08:00`、本地日期时间、纯日期、纯时间（含小数秒）。
- **结构**：数组（跨行、注释、尾逗号、混合类型）、内联表（单行、不允许尾逗号）、
  标准表 `[a.b]`、表数组 `[[a.b]]`、点分键 `a.b.c = 1`。
- **注释**：`#` 到行尾；顶层允许空行、CRLF 与 BOM。

定义冲突（重复键、`[a]` 重复、`a = {}` 之后 `[a.b]`、点分键表被 `[a]` 重开等）都会报错。

## 命令行工具

```text
toml fmt   config.toml            规范化输出
toml check config.toml            只校验
toml get server.port config.toml  路径取值（--raw 去引号）
toml keys  server.tls config.toml 列出成员名
toml stats config.toml            统计类型分布与最大深度
```

文件缺省从标准输入读；`get` / `keys` 的路径语法是 `a.b[0].c`。

## 构建与自检

```text
make -C library/toml            # libtoml.a + toml + toml-tests + model-demo
make -C library/toml check      # 单元测试 + CLI 演示 + 投射演示
```

产物在 `build/toml`。`check` 跑 150 多条断言：标量 / 转义 / Unicode / 四种整数 /
浮点 / 四种日期时间 / 结构 / 冲突与错误（含行列）/ dump 往返 / 比较语义 / 迭代器与
STL 算法 / 模型投射与诊断路径，最后用 CLI 与 `model-demo` 各跑一遍 `examples/config.toml`。

## 目录

```text
library/toml/
├── toml.hpp / toml.cpp          值 / 数组 / 表 / 日期时间 / 解析 / 序列化 / 路径 / 迭代器
├── bind.hpp                     模型投射：TOML_MODEL、encode、decode
├── main.cpp                     命令行工具
├── test_toml.cpp                单元测试（含 STL 算法与投射用例）
├── examples/
│   ├── config.toml              示例数据
│   └── model_demo.cpp           投射演示：TOML → 模型 → TOML
├── Makefile / CMakeLists.txt    两种构建方式
└── README.md
```

## 与 `library/json` 的关系

两者是**同一套设计的两份实现**：`Value` / `Array` / `Object` / `Member` 的结构、
迭代器、`ParseResult` / `Diagnostic`、模型投射（`JSON_MODEL` ↔ `TOML_MODEL`）都一一对应，
换库基本是换命名空间。差别来自格式本身：

- TOML 有**日期时间**，多了 `toml::DateTime`；没有 `null`，`Null` 在 JSON 里承担的角色由
  "空 `optional` 编码时省略"取代；
- TOML 是**文档式**（表头 / 表数组 / 点分键），`dump()` 会写出规范的 TOML 文档，JSON 的
  `dump(indent)` 则写出缩进的对象文本；
- TOML 的解析器额外维护定义冲突检查，`Diagnostic` 的短码如 `duplicate-table` /
  `inline-table` / `duplicate-key` 都对应规范里的具体违规。

两者互不依赖。
