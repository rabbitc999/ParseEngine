# 扫 TODO / FIXME / NOTE（Python 产物 · `%mode lex` + `%commit`）

`project/` 下七个完整工程示例中**最小的一份**：只有词法器（`%mode lex`），**一条产生式都
不写**——因为「把待办标记抽出来」这件事本来就只发生在词法层。

它演示 `%commit` 最直接的用途：**匹配到的东西既不当 Token、也不丢掉，而是按
`{行号, 类型, 原文}` 交出来**。这里的「类型」就是 `%commit` 声明里写的关键字
（`todo` / `fixme` / `note`），正好是报告里的一列。

```
project/todo-scan/
├── todo_scan.gram        词法文法（无产生式）+ Python 报告驱动
├── Makefile              make（生成） / run / check / clean
├── check.sh              跑用例并比对期望输出
├── samples/*             3 份被扫的样例源码（app.js / util.py / clean.txt）
├── expected/*.md         正常用例的期望报告（已人工核对）
├── expected-error/*.txt  错误用例的期望报错首行
└── build/                生成产物：parser.py + main.py（.gitignore 忽略）
```

## 构建与运行

```sh
# 0) 先在仓库根目录构建生成器（本工程只用它，不改它）
make

# 1) 生成（Python 产物不需要编译）
make -C project/todo-scan

# 2) 扫源码 / 跑全部用例
make -C project/todo-scan run                    # 扫 samples/app.js 与 samples/util.py
make -C project/todo-scan check                  # 3 个正常用例 + 2 个错误用例
python project/todo-scan/build/main.py project/todo-scan/samples/app.js
```

输出是一份 Markdown 报告：开头一行总数与分类计数，下面一张表（类型 / 文件 / 行 / 内容）：

```markdown
# TODO 扫描报告

共 5 条（todo 2 / fixme 2 / note 1）

| 类型 | 文件 | 行 | 内容 |
| --- | --- | ---: | --- |
| todo | samples/app.js | 3 | TODO: 列表超过 200 条时改成虚拟滚动 |
...
```

`samples/clean.txt` 是「一条都没有」的样例，报告只有表头与 `共 0 条`——**空结果也要
稳定**，这份期望文件就是用来钉住它的。

## 文法（一共十几行）

```c
%mode lex
%class TodoScanner

%token regex CHAR "[^\n]"      /* 兜底：吃掉任意一个非换行字符 */
%token NL "\n"                 /* 换行单独一条，行号才对得上 */

%commit todo  "TODO[^\n]*"     /* 类型 = todo，收进容器 */
%commit fixme "FIXME[^\n]*"
%commit note  "NOTE[^\n]*"
```

两个关键点：

- **必须有一条 `CHAR` 兜底 Token。** 词法器只认它认识的规则，没被任何规则覆盖的
  字符会报「认不出来的字符」并停下；要让整份源码都能扫过去，就得有一条吃掉任意
  字符的兜底规则（换行单独一条，否则行号会算错）。这些 Token 我们不关心——扫出来
  的东西全在 `engine.commits` 里。
- **`%commit` 的类型名就是报告里的一列。** 三种标记各一条规则，容器里每条记录的
  `.type` 直接告诉你是哪一类，不用去猜原文前缀。

## 样例与期望输出

| 用例 | 命令行 | 期望文件 | 覆盖 |
| --- | --- | --- | --- |
| `all` | `samples/app.js samples/util.py` | `expected/all.md` | 多文件、三种类型、行号 |
| `single` | `samples/app.js` | `expected/single.md` | 单文件；注释在行尾（`// FIXME: …`）也能扫到 |
| `clean` | `samples/clean.txt` | `expected/clean.md` | 空结果 |
| `missing` | `samples/nope.js` | `expected-error/missing.txt` | 文件打不开（退出码 2） |
| `usage` | （不给参数） | `expected-error/usage.txt` | 用法提示（退出码 2） |

期望文件里的「文件」一列是命令行里给的路径，所以用例固定用 `samples/…` 这种相对
路径（`make -C` 在本目录下执行）。报告是 UTF-8：`%main` 把 stdout 固定成 UTF-8，
不受 Windows 控制台代码页影响，Windows / Linux 共用同一份期望文件。

加用例：在 `check.sh` 末尾加一行 `check_ok <名字> <文件…>` 或 `check_err <名字> …`，
再存一份对应的期望文件即可。

## 这个工程用到生成器的哪些特性

| 用到的特性 | 位置 | 作用 |
| --- | --- | --- |
| `%mode lex` | 文件头 | 只导出词法器，允许一条产生式都不写（产物里没有解析器） |
| `%class TodoScanner` | 文件头 | Python 产物里的词法器类名（默认 `Parser`） |
| `%token regex` | 词法段 | `CHAR` 兜底 + `NL`（换行当 Token，行号才准） |
| `%commit` | 词法段 | 三种标记各收一类，`engine.commits` 里是 `ParserCommit(line, type, text)` |
| `%code ... %endcode` | 用户代码块 | `render_report` / `report_rows` 两个纯函数 |
| `%main` | 文法末尾 | 命令行入口：逐个文件 `tokenize`，最后渲染一份 Markdown |
| 词法器公开 API | `%main` | `lexer.tokenize(text)` → `(status, tokens)`；`lexer.commits`；`lexer.last_error` |
