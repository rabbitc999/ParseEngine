# JSON 库（Rust 版）

> **这是一个独立的 Rust crate：`json`。** 它是 [`library/json`](../json/README.md) 的
> Rust 移植：解析、序列化、**数组与对象都能直接用 `for` 和迭代器适配器遍历**，
> 外加一层「JSON ↔ 模型类」的投射。**只用 Rust 标准库，没有任何第三方依赖**
> （`Cargo.lock` 里只有本 crate 自己），也不依赖仓库里别的 crate；
> **不参与 ParseEngine 主程序的构建**。
>
> 与 C++ 版的关系是「**同一套语义的两个实现**」：短码、消息、行列、字段顺序、
> 相等规则、命令行文案都逐条对齐，并且用一份[对照脚本](#与-c-版逐条比对)
> 把两版的输出直接比过。内部设计与实现细节见 [技术报告.md](技术报告.md)。

```text
文本 ──parse──▶ Value（null / bool / int / double / string / array / object）
                  │
                  ├──dump（紧凑 / 缩进）──▶ 文本
                  ├──at_path("a.b[0]") ──▶ 取值
                  └──decode::<T> / encode(&t) ──▶ 模型类（json_model!）
```

## 特点

| 特点 | 说明 |
| --- | --- |
| 纯标准库 | 没有 `serde`、没有 `regex`、没有任何 `[dependencies]`；`#![forbid(unsafe_code)]` |
| 对象保序 | `Object` 是「插入顺序的成员表」（不是 `HashMap`）：解析顺序、编码顺序、`for` 顺序是同一份顺序 |
| 容器就是容器 | `Array` 就是 `Vec<Value>`；`Object` 能 `Deref` 成 `[Member]`，`iter` / `filter` / `find` / `sum` / `collect` 直接可用 |
| 通用迭代 | `value.entries()` 一次迭代同时给「键、下标、值」——不关心是数组还是对象时只写一套代码 |
| 模型投射 | `json_model!` 声明字段，`encode` / `decode` 双向转换；支持嵌套模型、`Vec`、`BTreeMap` / `HashMap`、`Option`、枚举 |
| 诊断 | 解析错误给行列与短码（与 C++ 版逐字相同）；投射错误给 **JSON 路径**（`points[1].x`） |
| 不 panic | 取值给 `Option`、类型不对给 `Err`（`Error::NotArray` / `NotObject` / `OutOfRange`），库内没有 `unwrap` |
| 选项 | 允许注释（`//`、`/* */`）与尾逗号（`ParseOptions::loose()`，命令行 `--comments` / `--trailing-commas`） |

## 快速开始

```sh
# 编译：在 crate 里跑 → 产物落 build/json/rust；从仓库根带 --manifest-path 跑 → 落 crate 里的 target/
cd library/json-rust && cargo build
```

```rust
use json::parse;

let doc = parse(r#"{"name":"calc","count":2,"tokens":["+","-"]}"#);
if !doc.ok() {
    eprintln!("{}", doc.diagnostics[0]);   // 例如 2:8: 不认识的字符 ',' [unexpected-token]
}
let root = doc.value.unwrap();

let name = root.get("name").unwrap().string_or("?");   // 取不到就用兜底值
let count = root.get("count").unwrap().int_or(0);
let first = root.at_path("tokens[0]");                 // 路径查询

println!("{}", root.dump());                           // 紧凑单行
println!("{}", root.dump_indent(2));                   // 缩进 2 空格
```

## 遍历：容器就是容器

C++ 版主打「解析出来的东西能直接喂 STL 算法」；Rust 这边对应的就是**标准迭代器**，
不需要先转成别的类型：

```rust
use json::parse;

let doc = parse(r#"{"scores":[3,1,2]}"#);
let root = doc.value.unwrap();
let scores = root.get("scores").unwrap().as_array().unwrap();

// 1. 范围 for + 求和
let total: i64 = scores.iter().filter_map(|item| item.as_int()).sum();

// 2. 迭代器适配器
let threes = scores.iter().filter(|item| item.as_int() == Some(3)).count();
let found = scores.iter().find(|item| item.as_int() == Some(1));
let numbers: Vec<i64> = scores.iter().map(|item| item.int_or(0)).collect();
let any_big = scores.iter().any(|item| item.int_or(0) > 2);

// 3. 对象：成员是公开字段，顺序就是文件里的顺序
for member in root.as_object().unwrap().iter() {
    println!("{} = {}", member.key, member.value.dump());
}

// 4. Value 自己也能迭代：数组给元素、对象给成员、标量是空区间
for entry in &root {
    println!("{} → {}", entry.key.unwrap_or("?"), entry.type_name());
}
```

`Entry` 把 C++ 版 `Value::Reference` 的三样东西都带出来了：`key`（对象成员名，
数组元素是 `None`）、`index`（数组下标，对象成员是 `None`）、`value`（值本身）。
要改就用 `entries_mut()` / `for entry in &mut value`。

## 模型投射（`json_model!`）

C++ 没有内置反射，所以那边用**成员指针表**（`JSON_MODEL(T, json::field("x", &T::x))`）。
Rust 有 trait，用不着成员指针：

```rust
use json::{decode, encode, json_enum, json_model};
use std::collections::BTreeMap;

#[derive(Clone, Debug, Default, PartialEq)]
struct Token {
    name: String,
    priority: i64,
    pattern: Option<String>,     // 可选字段：JSON 里没有也不算错
}
json_model!(Token, "name" => name, "priority" => priority, "pattern" => pattern);

#[derive(Clone, Copy, Debug, PartialEq)]
enum Kind { Line, Polygon }
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
assert_eq!(config.tokens[0].pattern, None);          // 缺席的可选字段 → 空

let encoded = encode(&config);                        // 字段顺序 = 声明顺序
assert_eq!(encoded.dump(), r#"{"name":"calc","kind":1,"tokens":[{"name":"NUMBER","priority":10,"pattern":null}],"labels":{}}"#);
```

字段名与成员名**不必相同**（`"maxDepth" => max_depth` 这么写就行，
`src/bin/model_demo.rs` 就是这么映射 `limits` 的）。支持的类型：

| 类型 | 编码 | 解码 |
| --- | --- | --- |
| `bool` / 整数 / `f32` / `f64` | 对应的 JSON 标量 | 类型不符报 `type-mismatch`；`allow_string_numbers` 时 `"12"` 也能喂给整数 / 浮点 / 枚举 |
| `String` | 字符串 | 必须是字符串 |
| `&str` | 字符串 | **只用于编码**（Rust 里没有"解到借用"这回事，要借就用 `String`） |
| `Option<T>` | 空 → `null` | `null` → 空；**字段缺席也不算错** |
| `Vec<T>` | 数组 | 必须是数组，逐元素解码（路径带 `[i]`） |
| `BTreeMap<String,T>` / `HashMap<String,T>` | 对象 | 必须是对象 |
| 枚举（`json_enum!`） | 整数（想写名字就 `as_json_name()`） | 整数或数字字符串；对不上退回第一个变体 |
| `Value` | 原样透传 | 原样透传 |
| 有 `json_model!` 的模型 | 对象（字段顺序 = 声明顺序） | 必须是对象 |

解码选项（`DecodeOptions`）：

| 选项 | 默认 | 作用 |
| --- | --- | --- |
| `strict_missing` | `true` | 缺字段报 `missing-field`（`Option` 字段豁免） |
| `strict_unknown` | `false` | 多出来的字段报 `unknown-field` |
| `allow_string_numbers` | `true` | `"12"` 允许解成整数 / 浮点 / 枚举 |

失败时 `DecodeResult::diagnostics` 里每条都带 **JSON 路径**，`require()` 则把
「要么给值、要么给诊断」摊平成一个 `Result`：

```rust
use json::{decode, json_model, parse, DecodeOptions};

#[derive(Debug, Default)]
struct Point { x: i64, y: i64 }
json_model!(Point, "x" => x, "y" => y);

#[derive(Debug, Default)]
struct Shape { points: Vec<Point> }
json_model!(Shape, "points" => points);

let doc = parse(r#"{"points":[{"x":0,"y":0},{"x":"bad","y":1}]}"#);
let broken = decode::<Shape>(&doc.value.unwrap());
for item in &broken.diagnostics {
    eprintln!("{item}");   // points[1].x: 期望整数，实际是 string [type-mismatch]
}

let _ = DecodeOptions::lenient();      // 缺字段、多字段都不报错（能读到多少算多少）
```

## 命令行

```text
json-cli <命令> [文件] [选项]

  fmt              格式化（--indent N / --compact）
  check            校验，打印诊断
  get <路径> [文件] 按路径取值（--raw：字符串不加引号）
  keys [路径] [文件] 列出成员名
  stats            统计各类型节点数与最大深度

  --out FILE       输出写文件；--comments / --trailing-commas 放宽解析
```

命令、选项、文案与退出码（0 成功 / 1 解析或取值失败 / 2 用法错误）**与 C++ 版一致**：

```console
$ cargo run -q --manifest-path library/json-rust/Cargo.toml -- fmt library/json/examples/config.json --compact
{"name":"pl0-super","version":2,...}
$ cargo run -q --manifest-path library/json-rust/Cargo.toml -- get limits.maxDepth library/json/examples/config.json
64
$ cargo run -q --manifest-path library/json-rust/Cargo.toml -- stats library/json/examples/config.json
array: 2
array.items: 4
array.nested: 2
bool: 1
double: 1
int: 5
object: 5
object.members: 18
string: 9
maxdepth: 4
```

文件缺省从标准输入读；`get` / `keys` 的路径语法是 `a.b[0]`
（**注意**：`[n]` 只能收尾，`a[0].b` 两版都判为"找不到"，见下面的差异一节）。

## 构建与自检

```sh
# 1. 自检：52 个测试 = 单元（core 10 + bind 6 + readme 4）+ 命令行端到端（cli 10）+ 文档示例（22）
cargo test --manifest-path library/json-rust/Cargo.toml

# 2. 投射演示：读 library/json/examples/config.json → 模型 → 再编回去
cargo run --manifest-path library/json-rust/Cargo.toml --bin json-model-demo

# 3. 与 C++ 版逐条比对（要先 make -C library/json）
python library/json-rust/tools/diff_with_cpp.py
```

`cargo test` 里的 30 个用例与 C++ 版 `test_json.cpp` 的 **174 条断言一一对应**，
期望字符串逐字相同（`readme` 那 4 个是上面几段示例的可运行副本）；
`cli` 那几个还会把 `json-cli` / `json-model-demo` 真跑一遍
（对应 C++ 版 `make check` 里"单元测试 + CLI 演示 + 投射演示"三件事）。

**实测**（本机 Windows / rustc 1.98.1；示例数据就是 C++ 版那份
`library/json/examples/config.json`）：

| 项目 | 结果 |
| --- | --- |
| `cargo test` | 52 个测试全过（0 失败）；`cargo clippy --all-targets` 无告警 |
| 语料比对（91 条 JSON 文本，含坏输入） | 一致 **86**，有意差异 5（3 条浮点写法 + 2 条落单代理） |
| 命令行比对（25 条命令 / 选项 / 错误分支） | 一致 **24**，有意差异 1（用法行里的程序名本来就不同） |
| 实质差异 | **0** |

### 与 C++ 版逐条比对

```sh
python library/json-rust/tools/diff_with_cpp.py          # 全跑（语料 + 命令行）
python library/json-rust/tools/diff_with_cpp.py 语料      # 只跑 91 条语料
python library/json-rust/tools/diff_with_cpp.py 命令      # 只跑命令行矩阵
```

脚本会把两版的标准输出、标准错误、退出码一起比，并把差异归类成
「浮点写法（值相同）」「落单代理」「用法行的程序名」和**真差异**——
只要出现真差异就退出码非 0。

## 与 C++ 版的差异

只有两条，而且都不改语义：

1. **浮点写法 —— 差异的根在 C++ 那一侧的实现里。** C++ 的 `formatDouble`（`json.cpp`）
   是「从 17 位有效数字逐级缩短到能往返为止」：`%.*g` 打印、`strtod` 回读比对。
   可 `f64` 打满 17 位有效数字**必定**能往返 —— 于是循环第一轮就 `break`，
   实际印出来的永远是 17 位：`0.1` → `0.10000000000000001`。Rust 这边不用那个循环，
   直接用标准库 `format!` 的规范写法（语言保证**最短且能往返**）：`0.1` → `0.1`。
   两串是**同一个 `f64`**，互相解析回来都不变；差别只是长短与可读性，而**长的那串
   来自 C++ 的内部实现细节**（Rust 不是"简化了"，是"没去照抄一个更啰嗦的算法"）。
   量级很大 / 很小时两边都切指数写法（`1e+300`）。
2. **落单代理**：输入 `"\udc00"`（单独的代理码点）C++ 会把 `ED B0 80` 这几个
   **非法 UTF-8 字节**写进产物，Rust 直接报 `bad-surrogate` —— **这是 Rust 字符串
   类型的固有约束**（`String` 只能装合法 UTF-8，没有"装着非法字节的字符串"这种状态），
   不是实现取舍。合法代理对（`"\ud83d\ude00"`）两版完全一样。

平台层面的换行（Windows 上 C++ 的 stdout 走文本模式给 CRLF、Rust 一律 LF）
**不算差异**：`--out` 写文件时两边都是 LF，比对脚本也会先把 CRLF 归一。
`用法：<argv[0]> …` 里的程序名各是各的，同理。

`library/json` 的头注释与 README 里有几处**写着支持、实测做不到**的写法
（`Value::atPath("[0].name")`、`DecodeOptions::enumAsString`），Rust 版按实测行为
对齐，并在 [技术报告](技术报告.md) 里逐条记了账——**没有照抄宣传，只照抄行为**。

## 目录

```text
library/json-rust/
├── src/
│   ├── lib.rs             模块装配与总览
│   ├── value.rs           Value / Array / Object / Member / 路径 / 迭代 / 相等
│   ├── parse.rs           解析器、选项、诊断（短码与 C++ 版逐字相同）
│   ├── text.rs            序列化（dump / dump_indent / 转义 / 浮点写法）
│   ├── bind.rs            投射：ToJson / FromJson / JsonModel / DecodeResult
│   ├── macros.rs          json_model! / json_enum!
│   ├── main.rs            json-cli：命令行工具
│   └── bin/model_demo.rs  json-model-demo：JSON ↔ 模型 的演示
├── tests/                 自检：core（解析 / 值 / 迭代）+ bind（投射）+ cli（端到端）+ readme（示例）
├── tools/diff_with_cpp.py 与 C++ 版逐条比对（语料 + 命令行矩阵）
├── Cargo.toml / .cargo/config.toml
└── README.md / 技术报告.md
```

## 与 `library/lsp/json.hpp` 的关系

和 C++ 版那份说明一样：`library/lsp` 里的 JSON 是**为语言服务器写的最小实现**
（够用就好、内嵌在服务器里）。本 crate 是独立、通用的一份——多了迭代器、
路径查询、模型投射、注释 / 尾逗号选项与更细的诊断。三者互不依赖，
`.gram` 语言服务器继续用它自己那份，行为不因为本 crate 而改变。
