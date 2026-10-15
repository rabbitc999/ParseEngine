# 预处理器（`library/preprocess`）

把「源码进编译器之前」的那一步独立出来：处理 `%` / `#` 开头的预处理指令——
符号定义、条件编译、文件包含，**默认用空行占位保证行号不变**，出错一路收诊断。
只依赖 C++17 标准库，不参与 `ParseEngine` 主程序的构建。

流程参考 `metro_editor` 的 `MetroLang/Compiler/PreProcessor.cs`（指令语义、包含去重、
深度保护、逐条收集错误），但做成了**语言无关**的：前缀、注释、字符串、不透明区域
都可以按语言换，另外补了条件表达式、行号映射和可插拔的包含加载器。

## 指令

指令要求**单独一行、行首**（允许缩进）。前缀默认同时认 `%` 与 `#`：

| 指令 | 作用 |
| --- | --- |
| `%symbol NAME [VALUE]` / `%unsymbol NAME` | 定义 / 取消符号（中性写法，VALUE 用空格分隔） |
| `#define NAME [VALUE]` / `#undef NAME` | 同上，C 风格（始终可用；`--metro` 时 `%define` / `%undefine` 也可用） |
| `if EXPR` / `elif EXPR` / `else` / `endif` | 条件编译；不活跃的分支整块丢掉 |
| `ifdef NAME` / `ifndef NAME` | 等价 `if defined(NAME)` / `if !defined(NAME)` |
| `include "路径"` | 把文件内容原地展开（相对当前文件目录 → `includeDirs` → 原样） |
| `include_once "路径"` / `import "路径"` | 同上，但按"只展开一次"处理 |
| `once` / `pragma once` | 把当前文件登记成"只展开一次" |
| `error "消息"` / `warning "消息"` | 活跃分支里报一条错误 / 警告 |

指令行**末尾的注释会被忽略**（`%include "a.inc"   // 说明` / `/* 说明 */` 都行）——注释不属于参数，引号里的注释定界符当普通字符；注释夹在表达式中间的写法不支持。

包含（include / import）**同一个文件默认只展开一次**，并且带循环检测与最大深度
（`Options::maxIncludeDepth`，默认 32）。不认识的指令默认报错，也可以用
`Options::UnknownDirective::Keep` 原样保留（`.gram` 就用这个策略）。

## 条件表达式

优先级从低到高：

```text
或   := 与 ('||' 与)*
与   := 非 ('&&' 非)*
非   := '!' 非 | 比较
比较 := 原子 (('==' | '!=') 字面量)?
原子 := NAME | 'defined' '(' NAME ')' | '(' 或 ')'
```

未定义的符号为假；定义了但没值的符号为真；有值的按 `0 / false / no / off`
（大小写不敏感）与空串为假，其余为真。`NAME == 值` 比较符号的取值，值可以带引号。

```text
%if DEBUG && !QUIET
%if LEVEL == 2
%if defined(FEATURE) || defined(FALLBACK)
```

## 行号

默认「**丢掉的行用空行占位**」（`Options::preserveLines = true`）：指令行、条件为假的
行都会输出一个空行，于是输出的行号与输入一一对应，上层按原文件行号报错不用换算。
想紧凑输出就关掉它（CLI 的 `--drop-lines`）。

`%include` 会把被包含文件的行插进来，行数必然变化，所以还有两条路：

- `Result::lineMap`：输出第 i 行的来源（文件 + 行号），空行占位也记来源；
- `Options::emitLineMarkers`：换文件时插入 `#line 行号 "文件"`，直接喂给 C 编译器。

## 包含加载器（CSV、数据库、内存文本…）

`%include` / `%import` 都会先问 `Options::includeLoader`。回调可以自己解析路径、
直接给正文、跳过或报错；不处理就返回 `useDefault()` 交回库的默认逻辑。

```cpp
preprocess::Options options;
options.includeLoader = [](const preprocess::IncludeRequest &request) {
    // 1. 只接管 %import 的 CSV，别的交回默认（读文件）
    if (!request.imported || request.path.compare(request.path.size() - 4, 4, ".csv") != 0) {
        return preprocess::IncludeResult::useDefault();
    }
    // 2. 自己把 CSV 转成文法能吃的文本
    const std::string text = CsvToText(request.resolvedPath);
    // 3. label 进依赖表 / 去重 / 循环检测；留空则用 resolvedPath
    return preprocess::IncludeResult::inlineText(text, request.resolvedPath);
};
```

| 回调返回 | 含义 |
| --- | --- |
| `useDefault()` | 我不处理，走库的默认逻辑 |
| `useFile(label?)` | 路径我认了，正文仍从 `resolvedPath` 读 |
| `inlineText(text, label?)` | 正文我自己给（CSV / 数据库 / 内存表都行） |
| `skip()` | 这次包含什么都不做 |
| `error(message)` | 报一条 `include-loader-error` |

`request.imported` 区分 `%import` 与 `%include`，所以可以让 `import` 当数据源、
`include` 当代码片段。CLI 里的 `--csv-to-lines` 就是这个回调的最小实现（表头当列名，
每行数据生成 `row1 列=值 列=值`）：

```text
$ preprocess demo.pp --csv-to-lines
row1 name=人民广场 lon=121.47 lat=31.23
row2 name=南京东路 lon=121.49 lat=31.24
```

## 语言预设

不想逐个设选项时，用这三个预设（也可以照着改）：

| 预设 | 前缀 | 说明 |
| --- | --- | --- |
| `Preprocessor::forC()` | `#` | C / C++ / Objective-C：`#define`、`#ifdef`、`#pragma once`、`#import` |
| `Preprocessor::forMetro()` | `%` | metro_editor 那种 `%define` / `%if` / `%include` / `%import` |
| `Preprocessor::forGrammar()` | `%` | `.gram`：`%token` 等非预处理指令原样保留，`%code … %endcode` / `%class_body … %endbody` 里不解析 |

换别的语言时按需改 `Options`：

| 选项 | 作用 |
| --- | --- |
| `syntax` / `directivePrefixes` | 指令写法：`Auto`（默认，`#` 与 `%`）/ `Hash` / `Percent` / `Custom` |
| `lineComments` / `blockComments` | 注释起始串（默认 `//` 与 `/* … */`；SQL 的 `--`、Python 的 `#` 都可以加） |
| `stringDelimiters` | 字符串定界符（默认单双引号；注释与字符串里的指令不当指令） |
| `opaqueRegions` | 成对的"不解析区域"（默认空，`.gram` 预设里带三处） |
| `percentDefineIsSymbol` | `%define NAME [VALUE]` 是否也算符号定义（yacc / metro 风格） |
| `unknownDirective` | 不认识的指令：`Error` / `Keep` / `Ignore` |
| `caseInsensitive` | 符号名是否大小写不敏感 |

## API

```cpp
#include "preprocess.hpp"

preprocess::Options options;              // 或 forC() / forMetro() / forGrammar()
options.defines.push_back({"DEBUG", ""}); // 等价命令行 -D DEBUG
options.includeDirs.push_back("shared");  // 等价 -I shared

preprocess::Preprocessor preprocessor(options);
preprocess::Result result;
if (!preprocessor.runFile("main.src", result)) {
    for (const preprocess::Diagnostic &item : result.diagnostics) {
        std::cerr << item.toString() << "\n";   // 文件:行:列: error: 消息 [短码]
    }
}
result.text;          // 处理后的正文
result.lineMap;       // 输出第 i 行的来源（文件 + 行号）
result.dependencies;  // 依赖文件（含主文件，按首次出现排序）
result.linesIn / linesOut / includeCount;
```

`runText(text, label, baseDir, result)` 用来处理内存里的文本（编辑器场景很方便），
`evaluate()` / `parseDefine()` / `parseTruthValue()` / `isValidName()` 是给上层复用的
静态工具。

诊断短码：`unknown-directive`、`bad-argument`、`bad-expression`、`bad-path`、
`include-not-found`、`include-cycle`、`include-depth`、`include-loader-error`、
`unterminated-if`、`unmatched-elif`、`unmatched-else`、`unmatched-endif`、
`duplicate-else`、`elif-after-else`、`error-directive`、`warning-directive`、
`unknown-pragma`、`open-failed`。

## 命令行

```text
preprocess <输入文件> [选项]

  -o, --out FILE         输出文件（缺省 stdout）
  -D, --define NAME[=V]  预定义符号（可重复）    -U, --undef NAME  取消预定义
  -I, --include-dir DIR  额外搜索目录（可重复）  --no-include-once 允许重复展开
      --keep-directives  指令行原样保留          --drop-lines      被丢掉的行直接删（不补空行）
      --line-markers     输出 #line 标记         --max-depth N     最大嵌套深度
      --hash/--percent/--metro/--grammar        语言预设
      --syntax MODE      auto / hash / percent / custom
      --prefix CHARS     custom 模式下的前缀集合
      --unknown MODE     error / keep / ignore
      --csv-to-lines     演示自定义 loader（%import 的 CSV 转成文本行）
      --check            只检查不输出            --deps            只列依赖文件
      --stats            把行数 / include 次数 / 依赖数写到 stderr
```

退出码：`0` 成功，`1` 有错误诊断，`2` 用法错误。诊断一律走 stderr，所以
`preprocess a.pp > a.out` 拿到的只有正文。

## 构建与自检

```text
make -C library/preprocess            # libpreprocess.a + preprocess + preprocess-tests
make -C library/preprocess check      # 单元测试 + 跑一遍 examples/
```

产物在 `build/preprocess`。`check` 会跑 102 条断言（单跑测试二进制是 99 条，另外几条来自示例对照）：条件编译、表达式、符号、
包含去重与循环、注释与字符串里的伪指令、`#pragma once`、自定义 loader（CSV）、
行号映射与 `#line` 标记、指令行末尾的注释，最后把 `examples/main.pp` 的输出和 `examples/expected-debug.txt`
对照一遍。

## 目录

```text
library/preprocess/
├── preprocess.hpp / preprocess.cpp   库本体（Options / Preprocessor / Result / Diagnostic）
├── main.cpp                          命令行工具
├── test_preprocess.cpp               单元测试（含 CSV loader 用例）
├── examples/                         示例：main.pp 包含 common.inc 与 debug.inc
├── Makefile / CMakeLists.txt         两种构建方式
└── README.md
```

## 与生成器自带预处理的关系

`ParseEngine` 主程序里本来就有一段条件编译（`src/preprocessor.cpp`，只处理
`%if` / `%elif` / `%else` / `%endif` 与 `--define`，并且知道 `.gram` 的
`%code ... %endcode` 段落）。这个库是**独立、通用**的那一份：多了 `include` / `import`、
符号定义、自定义加载器、行号映射与 `#line`，供编辑器、构建脚本或别的 DSL 使用。
两者行为对齐（表达式语法、变量真假判定一致），但互不依赖——生成器主程序不链接本库，
本库也不依赖主程序。

现在还有一个真实用户：[`magel`](../../magel/README.md) 把本库当成"源码进解析器之前"的那一步（`%include` 共享定义、`%if` 条件编译、`-D` 预定义），并用 `lineMap` 把解析错误翻回真正写错的那个文件。
