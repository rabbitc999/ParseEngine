# 结构化日志 → JSON（Python 产物 · `%commit` 与位置 API）

`project/` 下七个完整工程示例之一（总入口见 [`../README.md`](../README.md)）。
这一份用**免编译的 Python
产物**，把「生成器当脚本 / 当接口」那条路走通：
一份 `.gram` + 一段 Python 驱动，把半结构化日志变成 JSON。

它专门演示两件别处没演示过的事：

- **`%commit`**：注释不是丢掉，而是按 `{line, type, text}` 收进容器交出来
  （这里当「记录的备注」用，正好是「把注释喂给 AI 看」那个场景）；
- **位置信息**：报错按「第 N 行第 M 列：…」打，记录的行号也进 JSON。

```
project/log-ir/
├── log.gram              文法 + Python 驱动（%code ... %endcode / 动作 / %main）
├── Makefile              make（生成） / run / check / clean
├── check.sh              跑全部样例并比对期望输出
├── samples/*.log         3 个正常样例
├── samples-error/*.log   2 个出错样例
├── expected/*.json       正常样例的期望输出（已人工核对）
├── expected-error/*.txt  出错样例的期望报错首行
└── build/                生成产物：parser.py + main.py（.gitignore 忽略）
```

## 构建与运行

```sh
# 0) 先在仓库根目录构建生成器（本工程只用它，不改它）
make

# 1) 生成（Python 产物不需要编译）
make -C project/log-ir

# 2) 跑样例 / 全部样例
make -C project/log-ir run                       # 跑 samples/app.log
make -C project/log-ir check                     # 3 个正常 + 2 个错误
python project/log-ir/build/main.py project/log-ir/samples/app.log
cat project/log-ir/samples/app.log | python project/log-ir/build/main.py
```

输出是一行 JSON（每份输入一行），键排过序、UTF-8 编码：

```json
{"entries": [{"fields": {"action": "login", "ip": "10.0.0.1", "user": "alice"},
              "level": "INFO", "line": 2, "time": "2024-09-10T12:00:01"}, ...],
 "notes": [{"line": 1, "text": "# 一份普通日志…", "type": "note"}, ...]}
```

`entries` 是解析出来的记录，`notes` 是 `%commit` 收上来的注释——两者都有行号，
想「让模型只看注释」就只喂 `notes`，想「按注释给记录找原因」就按行号对齐。

## 文法范围

一行一条记录：`时间戳 级别 若干 key=value`，行尾可以跟 `# 注释`。

| 部分 | 写法 |
| --- | --- |
| 时间戳 | `%token regex STAMP "[0-9]{4}-[0-9]{2}-[0-9]{2}[T ][0-9]{2}:[0-9]{2}:[0-9]{2}"` |
| 级别 | `%define INFO "INFO"` / `WARN` / `ERROR` / `DEBUG`（固定字面量，最长匹配 + 关键字优先） |
| 字段 | `名字=值`，值可以是标识符、整数、IP（`%token regex IP`）或另一个时间戳 |
| 注释 | `%commit note "#[^\n]*"` —— 收进容器，不进语法层 |

几个刻意的取舍：

- **IP 单列一条正则**。不写它的话 `10.0.0.1` 会先被 `NUMBER`（`[0-9]+`）吃掉
  `10`，然后卡在 `.` 上报「认不出来的字符」——最长匹配是按**已声明的正则**比的，
  没声明的写法谁也救不了。
- **动作里不写字符串字面量**（DSL 的约定）：级别直接取词素 `$1`；`[]` / `($1, $2)`
  这类写法也有坑——前者是已移除的 `[$n]` 变量表语法，后者会被当成占位符解析，
  所以字段表用 `no_fields()` / `add_field()` / `field_pair()` 三个小函数包一下
  （都写在 `%code ... %endcode` 里）。
- **行号在 `STAMP` 归约时取**：动作里的 `$line` 是**当前 lookahead** 的行，等
  `entry` 整条归约时 lookahead 已经是下一条记录了，会报成下一行。所以
  `entry_start := STAMP` 先把 `($1, $line)` 存下来，`entry` 再用它。
- **`%main` 要自己补 `if __name__ == "__main__":`**：写 `%main` 时整段演示正文
  （含那行收尾）都会被替换掉，外壳只保留 docstring、`import parser` 与类名别名。
- **stdout 固定成 UTF-8**：Windows 控制台默认代码页是 GBK，不 `reconfigure` 的话
  中文会按 GBK 写进文件，期望文件就没法跨平台共用。

## 样例与期望输出

`samples/`（成功 + 与 `expected/*.json` 逐行比对）：

| 样例 | 覆盖 |
| --- | --- |
| `app.log` | 4 行日志（含文件头注释）：INFO / WARN / ERROR、IP、数字字段、行尾注释 |
| `levels.log` | DEBUG 级别、没有注释的记录、只有一条注释的记录 |
| `one.log` | 最小输入：一条记录、零个注释 |

`samples-error/`（必须报错退出，stderr 首行与 `expected-error/*.txt` 一致）：

| 样例 | 报错 |
| --- | --- |
| `bad_level.log` | 级别写了 `FATAL`：`期望 INFO / WARN / ERROR / DEBUG 之一，实际是 VARIABLE` |
| `bad_field.log` | 字段少了 `=`：`期望 ASSIGN，实际是 END` |

加样例就是往 `samples/` 放 `.log`、把跑出来的 JSON 存进 `expected/<名字>.json`
（先人工核对内容，再固化）；错误样例同理放 `samples-error/` +
`expected-error/<名字>.txt`（只比首行）。

## 这个工程用到生成器的哪些特性

| 用到的特性 | 位置 | 作用 |
| --- | --- | --- |
| `%mode lalr` | 文件头 | 一行一条记录，LALR(1) 足够，也不需要错误恢复 |
| `%class LogParser` | 文件头 | Python 产物里的解析器类名（默认 `Parser`） |
| `%token regex` | 词法段 | 时间戳、数字、IP 三条正则 Token |
| `%define` | 词法段 | 四个级别关键字（固定字面量，优先于标识符） |
| `%identifier` | 词法段 | 字段名与标识符取值 |
| `%commit` | 词法段 | `# …` 注释收进容器：`engine.commits` 里是 `ParserCommit(line, type, text)` |
| `$line` | 语义动作 | 记录行号（在 `STAMP` 归约时取，见上面「刻意的取舍」） |
| `current_location()` / `last_error` | `%main` | 报错打「第 N 行第 M 列：…」 |
| `%code ... %endcode` | 用户代码块 | `ENTRY_LIST`、`emit_entry`、`render` 等 Python 辅助函数与 `import json` |
| `%main` | 文法末尾 | 命令行入口：读文件或 stdin → 解析 → 打印一行 JSON |
| Python 免编译 | 全流程 | `make check` 只跑解释器，CI 与 AI 分析都不需要 C 工具链 |
