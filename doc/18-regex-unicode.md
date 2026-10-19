# 18 Unicode 正则模式

在正则开头写 `(?u)`，启用按 UTF-8 Unicode 标量值匹配的模式；写 `(?iu)` 或 `(?ui)`
同时启用 Unicode 简单大小写折叠。也可以分开写成 `(?i)(?u)`。
该模式支持 C99、C++17 `std` 和 C++17 `dfa`，使用相同的有序运行时。
完整文法示例见 [regex_unicode.gram](../example/regex_unicode.gram)。

## 模式与字符单位

| 写法 | 语义 |
| --- | --- |
| `.` | 字节模式；UTF-8 多字节字符可以只匹配首字节 |
| `(?u).` | 消耗一个完整 Unicode 标量值；例如中文占 3 字节，常见 emoji 占 4 字节 |
| `(?u).{2}` | 消耗两个码点；`中🙂` 的接受长度是 7 字节 |
| `(?u)[一-龥]+` | 按码点比较区间端点 |
| `(?u)[中🙂]+` | 中文或 emoji 的字符集合 |
| `(?u)\u{1F642}` | 匹配码点 U+1F642 |
| `(?iu)σ+` | 通过简单折叠匹配 `Σ`、`σ`、`ς` |

重复次数按码点计数；捕获、返回的匹配长度、Token 词素和偏移仍使用 UTF-8 字节。
行列号沿用既有词法器的位置计数规则。组合字符、emoji ZWJ 序列中的每个码点独立计数，
此模式的匹配单位不是用户感知的字形簇。

运行时严格验证被匹配或检查的 UTF-8：拒绝过长编码、孤立续字节、截断序列、
UTF-8 编码的代理项和大于 U+10FFFF 的值。不会把非法字节折成 ASCII。
一个合法接受前缀可以终止在后续非法字节之前；后续输入仍由词法器继续处理。

## 字符类与属性

Unicode 数据固定为 **17.0.0**，来自 Unicode 官方的
[UnicodeData.txt](https://www.unicode.org/Public/17.0.0/ucd/UnicodeData.txt)、
[CaseFolding.txt](https://www.unicode.org/Public/17.0.0/ucd/CaseFolding.txt) 和
[PropList.txt](https://www.unicode.org/Public/17.0.0/ucd/PropList.txt)。
源数据、许可和生成结果都在仓库内，构建和运行无需联网。

| 写法 | Unicode 模式中的定义 |
| --- | --- |
| `\d` / `\D` | `Nd` 十进制数字及其补集，包括阿拉伯、全角等数字 |
| `\w` / `\W` | `L`、`M`、`N`、`Pc` 及 U+200C/U+200D 的并集及其补集 |
| `\s` / `\S` | Unicode `White_Space` 属性及其补集，包括 U+3000、U+00A0 等空白 |
| `\p{L}` / `\P{L}` | Unicode 字母及其补集 |
| `\p{Lu}` | 大写字母；也支持 `Uppercase_Letter` 等类别长名 |
| `\p{gc=Nd}` | 通用类别缩写前缀 |
| `\p{General_Category=So}` | 通用类别长前缀 |
| `\p{White_Space}`、`\p{Word}`、`\p{ASCII}`、`\p{Any}` | 空白、上述单词集合、ASCII 和全部码点 |

支持全部 30 个通用类别及 `L/M/N/P/S/Z/C` 七个组合类别。
POSIX 类如 `[[:alpha:]]`、`[[:digit:]]`、`[[:upper:]]` 也使用对应的 Unicode 类别；
`alnum` 是 `L+N`，`word` 使用上述单词集合，`xdigit` 保留 ASCII 十六进制数字定义。
当前属性语法覆盖通用类别和表中列出的集合；不提供 Script、Block 或 Emoji 专项属性。
未知 Unicode 属性在生成期报错，C++ 也不会把它延迟给标准库构造。

`.` 在 Unicode 模式下排除 CR、LF、U+2028 和 U+2029。
`\b` / `\B` 按上述 Unicode 单词集合检查左右码点，组合标记也算单词字符。
词法规则每次以当前位置的剩余文本为匹配输入，边界上下文以这次匹配为准。

## 大小写、捕获和断言

`(?iu)` 使用 Unicode 17.0.0 的默认简单大小写折叠（CaseFolding 的 C/S 映射），
覆盖希腊字母、西里尔字母、Deseret 等字符以及 `K/k`、`ſ/s` 等等价关系。
字符类先完成折叠闭包，再取补集；例如 `(?iu)[^k]` 不能匹配 Kelvin 符号 `K`。

Unicode 模式可以与[扩展正则](17-regex-extensions.md)的捕获、数字反向引用、
正负前瞻和非贪婪量词组合：

```text
%token regex PAIR "(?iu)\\b(\\p{L}+)-\\1\\b(?=;)"
```

它可匹配 `Σ-ς;` 中的 `Σ-ς`，也能匹配 `K-k;` 和 `k-K;`。
反向引用逐码点比较，接受字节数由实际输入决定，因此捕获与引用可以占不同字节数。
前瞻不消耗输入；捕获和回溯继续保留原来的分支顺序。

此模式不做 Unicode 规范化，也不做多码点的完整大小写折叠。
因此 `é` 与 `e` 加组合重音是不同的序列，`ß` 也不会因为忽略大小写而匹配 `SS`。

## 生成与维护

默认字节模式的 C++ 规则继续使用 `<regex>` 或可证明等价的 DFA。
显式 Unicode 规则由生成期编译的共享程序执行，可与同一文法中的字节规则并存。
字符表、折叠表和运行时都写进生成源文件，产物不依赖系统 locale、外部 Unicode 库或磁盘模板。

源数据目录是 `source/unicode/17.0.0/`；生成表带有源文件 SHA-256 和 Unicode 许可全文。
更新数据后依次运行：

```sh
python tools/embed_unicode_regex.py
tools/lock.sh build -j4
python tests/regex_unicode.py
python tests/regex_extended.py
python tests/cpp_dfa_std.py
```

Unicode 专项验证码点长度、属性、边界、非 BMP 字符、17.0 新字符、简单折叠、
不同字节长度的反向引用、非法 UTF-8、长输入，以及三种后端的实际词法与解析产物。
