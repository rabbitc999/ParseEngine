# `.gram` / `.mgl` 语言服务器（`library/lsp`）

给 ParseEngine 的文法文件用的 **LSP（Language Server Protocol）服务器**：
编辑器打开 `.gram` 就能得到诊断、悬停说明、补全、跳转定义、文档大纲、
语义高亮、保守格式化和快速修复。只用 C++17 标准库，自带最小 JSON 与
`Content-Length` 帧实现，不依赖任何第三方库。

打开 `.mgl` 时使用独立的 Magel 文档模型：识别 `$macro NAME(x, y)` / `$end_macro`
（宏名前的 `!` 与形参名前的反引号都只是展开修饰，不算名字的一部分），
以及单行宏 `$replace NAME(x, y) 宏体` / `$replace NAME 宏体`（没有 `$end_macro`），两者共用同一套宏模型，
提供宏块配对、参数列表与调用参数数目的诊断，以及宏补全、悬停、跳转定义、大纲和
语义 token；也认得 `$match` / `$when` / `$else` 与它们的 `$end_*`（高亮 + 补全 +
"只能写在 `$macro` 体内"的诊断，分支配对的细节由编译器报）。
它只做编辑期检查；实际宏展开与完整 Magel 编译错误仍由 magel 驱动负责。
`--check` 命令仍用于 `.gram` 文法。
`.gram` 的 `%mode` 校验、补全和悬停已包含全部 16 种模式（含 `earley` / `cyk`）；
LSP 只校验模式名，目标语言组合仍由生成器检查。

> 设计与实现的完整说明（架构图、协议时序、位置换算公式、实测结果）见
> [`技术报告.md`](技术报告.md)。

## 能力

| 能力 | 说明 |
| --- | --- |
| 诊断 | 未知指令、禁用的旧用户代码块、未知 `%mode` / `%lang` 取值、`%start` 指向不存在的产生式、`%sync` 用未声明 Token、未声明的非终结符、未使用的 Token（规则里没用到且不是 `%prec` 来源）、未闭合的注释 / 字符串 / 段标记 / 语义动作、隐式 Token 提示 |
| 悬停 | 全部 `%指令` 的中文说明（与 `../../doc/12-grammar-usage.md` 的指令表一致）、Token 的字面量 / 正则 / `%union` 成员 / CSV 别名来源（`%token NAME include "表.csv"`）/ `alias` 显示名、`%token` 的两个行内关键字 `include` 与 `alias`、非终结符的候选数与起始符号标记、`$n` / `$$` / `$line` / `$token` / `$code` |
| 补全 | 行首补 `%指令`；`%mode` / `%lang` 后补取值；`%token NAME ` 后补 `include`（CSV 别名表）与 `alias`（显示名）；规则区补 Token、非终结符、`%empty`、`%prec` |
| 跳转定义 | Token / 非终结符引用 → `%token` / `%define` / 规则左部的声明处（`%prec` 的参数也能跳） |
| 文档大纲 | `%指令`、Token（带 `include "表.csv"` / `alias "显示名"` 详情）、产生式（含候选数与"起始符号"标记） |
| 语义高亮 | `%指令`、`%token` 的行内关键字 `include` / `alias`、Token 名、非终结符、字面量、注释、数字、`$n`、`<成员>`、动作代码；多行块按行拆分（语义 token 不能跨行） |
| 格式化 | 只做保守整理：去行尾空白、折叠多余空行、保证单个结尾换行；动作正文（C / Python 代码）与缩进一律原样保留 |
| 快速修复 | 未显式声明的 Token → 在指令区末尾插入 `%token X`；未声明的非终结符 → 追加 `X := %empty;` |

诊断码是稳定的短码，便于测试与抑制：`unknown-directive`、`unknown-value`、
`unknown-start`、`unknown-sync`、`undefined-symbol`、`implicit-token`、
`unused-token`、`unused-rule`、`unterminated-comment`、`unterminated-string`、
`disabled-user-code`、`unterminated-section`、`unterminated-action`。

## 构建

```text
make -C library/lsp            # 构建 build/lsp/lsp-server 与 lsp-tests
make -C library/lsp check      # 单元测试 + 用 examples/client.py 驱动真服务器跑两份示例
```

产物在 `build/lsp`：`lsp-server`（服务器）、`lsp-tests`（单元测试）。
GCC 下加 `-finput-charset=UTF-8 -fexec-charset=UTF-8`，Windows 下标准输入输出
切二进制模式，保证中文诊断与 `Content-Length` 帧都不出错。

## 用法

```text
lsp-server --stdio            在标准输入输出上跑 LSP（编辑器默认用法）
lsp-server --check FILE...    直接校验文法，输出 FILE:行:列: 级别: 消息（CI 用）
lsp-server --version / --help
```

把它接到任何支持 LSP 的编辑器（stdio 传输），把 `.gram` 关联到语言 id `gram` 即可；
参考 `examples/client.py`（一个 100 来行的完整客户端：initialize → didOpen →
diagnostics → hover / completion / definition / documentSymbol / semanticTokens /
formatting / codeAction → shutdown）。

```text
python library/lsp/examples/client.py build/lsp/lsp-server library/lsp/examples/sample.gram
```

## 示例

| 文件 | 用途 |
| --- | --- |
| `examples/sample.gram` | 干净示例：诊断为空，演示悬停 / 补全 / 跳转 / 大纲 / 高亮 / 格式化；含一条 `%token KW_LET include "sample_keywords.csv"` 演示 CSV 字面量别名表 |
| `examples/sample_keywords.csv` | `sample.gram` 的关键字表：含表头行、"文法里没导入"与"根本不存在"的对照行、第 3 列噪音列 |
| `examples/sample_broken.gram` | 故意写错：演示六类诊断与快速修复 |
| `examples/client.py` | 最小 LSP 客户端，同时是冒烟测试（干净示例 0 error，坏例子 `--expect-errors`） |
| `examples/README.md` | 编辑器接入说明 |

## 不做什么

服务器是**纯文本模型**（`GrammarDocument` 手里只有全文，没有文件路径），所以
`%token NAME include "表.csv"` 只按语法解析：它认识 `include` / `alias` 这两个
行内关键字、知道 CSV 路径属于哪个 Token，**但不会打开 CSV**。CSV 不存在、
第一列为空、别名跨 Token 冲突这类问题由生成器在生成期报（`ParseEngine` 的
`src/token_dict.cpp`），编辑器这一侧只保证不再把 `include` 误当成 Token 名。
`%copy` 引用的文件与 `%code include "…"` 的路径同理。

## 目录

```text
library/lsp/
├── json.hpp / json.cpp         最小 JSON（对象保序、\uXXXX 与代理对）
├── grammar.hpp / grammar.cpp   `.gram` 容错扫描 + 模型 + 六项语言特性
├── lsp.hpp / lsp.cpp           LSP 帧、JSON-RPC 分派、能力声明
├── main.cpp                    服务器入口（--stdio / --check）
├── test_lsp.cpp                单元测试与一次完整会话
├── Makefile / CMakeLists.txt   构建与自检
└── examples/                   示例文法（含 CSV 关键字表）+ 最小客户端 + 接入说明
```
