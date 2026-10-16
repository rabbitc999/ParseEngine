---
name: ParseEngine-analyze
description: 用 ParseEngine 给日志、DSL、配置等文本格式写一份小 .gram 文法，生成免编译的 Python 解析器，几条命令就能拿到解析结果、错误行列号与语法树；适合 AI 代理自己跑命令核对语法，而不是把整份日志读进上下文；达到止损信号时立即停止扩文法，转状态机 / 逆向 / 样本对比。
---

# 用 ParseEngine 快速分析日志 / DSL

## 这个技能解决什么

手上有一批**结构化文本**要理解：日志、DSL、配置文件、协议报文、测试期望文件…… 手写正则很快失控（嵌套、优先级、转义、多义），而把整份日志读进上下文又太贵。

ParseEngine 是仓库根目录这个 yacc 风格生成器：写一份**几十行**的 `.gram` 文法描述这个格式，生成一份**免编译**的 Python 解析器，然后用两条命令拿到「解析成功 / 失败 + 第几行第几列 + 语法树 / 语义值」。

对 AI 代理来说这等价于：**用几十 token 的输出换掉把整份日志读进上下文**。改文法 → 重新生成 → 喂一行输入看结果」这个循环可以在很小的上下文里转很多圈。

## 止损说明（什么时候停）

这个技能只回答「文本长什么样」，不回答「文本为什么这样、命令是否有效、数值从哪来」。出现下面任意两条，就停止扩文法，转回状态机 / 逆向 / 样本对比。

**立即止损信号**

1. **目标在语义层、状态层或逆向层**：要追常量来源、判断命令是否配对、检查协议约束、还原生成公式、验证设备行为——这些不是语法问题。
2. **输入太“平”**：逐行命令、`key=value`、固定参数、没有嵌套 / 表达式 / 优先级 / 多义。小型词法 + 显式状态机通常更直接。
3. **真正要看的字段在注释里**：文法已经跳过注释，`;CUT DISTANCE 54.8` 这类内容根本不进解析器。
4. **需要命令表、固件差异、状态机、回归样本**：这些必须人工确定和实现，不会因为用了生成器就自动获得。
5. **一次性任务，复用次数 N 很小**：接入 + 维护 + 额外验证成本高于节省，净节省为负。
6. **连续 2~3 轮「改文法 → 重生成 → 喂样本」仍不能把有效 / 无效输入分开**。
7. **你开始写大动作、大 `%{{...}}%`、大 `%main` 来补业务规则**：这说明问题不在语法层，继续写 `.gram` 收益很低。

**止损后保留什么**

- 保留已有 `.gram` 和生成的 `parser.py`，只做辅助语法检查。
- 把后续投入放到：命令表、状态检查、数值传播追踪、逆向证据、受控样本对比、回归用例。

**一句话**

语法通过 ≠ 命令有效；能解析 ≠ 能归因。到这一步就该换工具。

## 三条命令的循环

```sh
# 0. 没有生成器时先构建一次（需要 C++17 编译器）
make                                   # 产出 build/bin/ParseEngine

# 1. 写一份小文法（见下面的模板）
#    输入格式 = 文法里的产生式，字面量 = %define / %token

# 2. 生成 Python 解析器（不编译、不联网、只用标准库）
./build/bin/ParseEngine logfmt.gram --out-dir /tmp/logfmt --lang python
# （文法文件直接写在命令行上就行，等价于 --grammar-file logfmt.gram；
#   不写文件名时从标准输入读：cat logfmt.gram | ./build/bin/ParseEngine …）

# 3. 喂输入（演示程序读标准输入一行，读到换行或 EOF）
printf 'a=1,b=[c=2,d=3]' | python3 /tmp/logfmt/main.py

# 3'. 或者当库调：只回一行结果，最省上下文
python3 -c "import sys; sys.path.insert(0, '/tmp/logfmt'); import parser; \
e = parser.Parser(); print(e.run_text(open('a.log').read()), e.result, \
[str(x) for x in e.errors])"
```

产出的 `/tmp/logfmt/` 里只有 `parser.py`（解析器）、`main.py`（演示入口）与 `RESULT.md`（生成信息与冲突消解：状态数、消解记录；加 `--full-report` 才附上 FIRST/FOLLOW、ACTION/GOTO 那份完整报告）。

## 最小可用模板

以「`key=value`，值是数字 / 标识符 / 方括号嵌套」的格式为例，整份文法十来行：

```gram
%lang python
%mode ll1

%define EQ     "="
%define COMMA  ","
%define LBRACK "["
%define RBRACK "]"
%token regex NUMBER "[0-9]+"          /* 正则 Token：生成期编译成 DFA */
%identifier "[A-Za-z_][A-Za-z0-9_]*"  /* 标识符 → 内建 VARIABLE */

%tree                                 /* 只要结构：直接打印语法树，不写动作 */

line      := pair_list;
pair_list := pair_list COMMA pair | pair;
pair      := VARIABLE EQ value;
value     := NUMBER | VARIABLE | LBRACK pair_list RBRACK;
```

`printf 'a=1,b=[c=2,d=3]' | python3 /tmp/logfmt/main.py` 会打出缩进语法树；
把输入改成 `a=1,b=[` 则打出
`解析失败: code=-27 line=1 column=8 message=语法错误：期望 VARIABLE，实际是 END`。

要点：

- **固定字面量用 `%define 名字 "字面量"`**（一个名字可以写多个候选：`%define EQ "=|=="`）；正则 Token 用 `%token regex 名字 "正则"`。
- **`%identifier` 只负责「标识符怎么切」**，文法里写的 Token 名是内建`VARIABLE`（`%identifier` 与 `%token regex VARIABLE ...` 二选一）。
- `%tree` 让演示程序直接打印 CST，不用写语义动作——**先看结构再谈值**。
- 规则写法两种都行：一行一条，或 yacc 风格的 `|` 分支（整条以 `;` 结束）。
- **空白默认被跳过**：想让换行 / 空格进语法层（一行一条的日志、按空白分隔的字段）就写 `%token NL "\n"`（换行成为真实 Token）、`%token SP "\b"`（`\b` = 一个空白字符，空格 / `\t` / 换行都命中），再用 `%space " \t"`决定取 Token 前跳过哪些空白；`%token regex` 的正则里用 `\s`。换行 Token 只在多行输入里才有意义，试的时候别用内建 demo main（它只读一行），照上面 3' 那样把整份文本喂给 `run_text`。

## 按格式特点选套路

| 场景 | 写法 |
| --- | --- |
| 固定关键字 / 前缀（`INFO`、`GET`、`begin`） | `%define` 或 `%token NAME "字面量"` |
| 数字、时间戳、IP、UUID | `%token regex`（正则 `{m,n}` 上限 32，不能匹配空串） |
| 标识符 + 关键字表 | `%identifier "..."`，再用 `%define` 声明关键字（同长度时字面量优先） |
| 表达式 / 运算符优先级 | `%left` / `%right` / `%nonassoc` + `%prec`（二义文法不用改写） |
| 嵌套结构（括号、JSON 风格） | 递归产生式；LL(1) 请避免左递归，或直接用默认的 LR 家族 |
| 只关心结构 | `%tree` |
| 想一次看完所有错 | `error` 产生式 + `yyerrok`，或 `%sync TOKEN` panic mode；`%max_errors N` |
| 一行一条 / 按空白分隔（日志、表格） | `%token NL "\n"` + `%token SP "\b"`（`\b` = 一个空白字符）+ `%space " \t"` |
| 要自己的输出格式 | `%{{ ... }}%` 里写辅助函数，动作 / `%main` 里直接调 |

模式（`%mode` / `--mode`）默认是 IELR(1)（能覆盖全部 LR(1) 文法）； LL(1) 适合已经显式消除左递归的 DSL。

## 输出怎么读

| 拿到什么 | 怎么取 |
| --- | --- |
| 状态码 | `run_text(text)` / `run(tokens)` / `parse_tree(text)` 的返回值；`0` 是 `parser.STATUS_OK`，负数见 `STATUS_ERR_*` |
| 错误 | `engine.errors`（列表）/ `engine.last_error`；`str(error)` 形如「第 3 行第 12 列：…」 |
| 语义值 | `engine.result`（动作里 `$$` 写出来的任意 Python 对象） |
| 语法树 | `engine.parse_tree(text)` → `tree.print()` 出缩进文本，`tree.children` 可继续遍历 |

演示程序 `main.py` 与 `parser_demo` 行为一致：打印 `解析输入:`，成功打印 `解析成功`（`result` 不是 `None` 时再打印一行 `解析结果: <repr>`），失败打印 `解析失败: code=... line=... column=... message=...`；`%tree` 文法打印语法树。

## 分析一整份日志（省上下文的关键）

**不要**把日志内容或全部输出贴进上下文，让命令自己收敛：

```sh
# 只喂前 20 行
head -n 20 a.log | python3 /tmp/logfmt/main.py

# 逐行解析整份日志，只打印失败的行号与原因（成功一行都不打）
python3 - <<'PY'
import sys
sys.path.insert(0, '/tmp/logfmt')
import parser

engine = parser.Parser()
bad = 0
for number, line in enumerate(open('a.log', encoding='utf-8'), 1):
    if engine.run_text(line.rstrip('\n')) != parser.STATUS_OK:
        bad += 1
        print(number, engine.last_error)
print('失败行数:', bad)
PY
```

需要一次读多行（而不是一行一条记录）时，在文法里用 `%main` 写自己的入口：

```gram
%main
engine = Parser()
if engine.run_text(sys.stdin.read()) == parser.STATUS_OK:
    print(engine.result)
else:
    print(engine.last_error)
```

（`%main` 正文整段替换 `main.py` 的演示体；外壳的 `import parser` 与类名别名 `Parser` 保留。`%{{ ... }}%` / `%code … %endcode` 里的辅助函数则插在 `parser.py` 的 import 之后，动作里可以直接调用；正文按 Python 缩进规则归一化，照文法排版缩进着写也不会报 `IndentationError`。）

## Python 产物（`--lang python`）

- **`%commit`：把注释 / 标注收集起来给 AI 看（推荐）**：日志或 DSL 里常有一段「识别出来但不该进语法」的文本（注释、时间戳标注、`@TODO` 之类）。用 `%commit <类型> "<模式>"` 收进容器就不会打断解析，`%commit skip`（或等价的 `%trivia`）则匹配即丢：

  ```gram
  %trivia "\s*#.*"                         /* # 开头的注释：丢掉 */
  %commit note "@TODO:[^\n]*"              /* 收进容器，类型 note */
  ```
  产物里读 `engine.commits`（`ParserCommit(line, type, text)` 列表）就能只把这些片段抽出来喂给模型，而不用把整份日志读进上下文；位置信息用 `engine.current_location()`（`ParserLocation(line, column, offset)`）。`%commit` / `%trivia` 与位置信息 API 六档（C / C++17 / Python / C# / Rust / Go）都有。

- **一键构建**：根目录 `sh build-all.sh`（= `make build-all`）把生成器 + 8 个 C++ 库 + 5 个 Rust crate + example 产物 + 7 个工程编一遍，**不跑测试**；`--list` 只看清单。它自己检测 g++/gcc/make/python（必需）与 cargo/rustc/dotnet/go（可选，缺了跳过）。
- 支持：`%mode`（十六种：ll0 / ll1 / llk / lr0 / slr / slrk / lalr / lalrk / lr1 / lrk / ielr / glr / earley / cyk / dd / lex；
  **glr 给 `cpp17` / `python` / `csharp` / `rust` / `go`，earley / cyk 给 `cpp17` / `python` / `csharp` / `rust`**；各语言与 `gram` 导出的限制统一见 [15](doc/15-language-mode-support.md)）、`%start`、`%define` / `%token` / `%token regex` /`%identifier`、`%tree`、`%sync`、`error` 产生式、`%left` /`%right` / `%nonassoc` / `%prec`、`%expect`、`%max_*`、`%teach`、`%mermaid`、`%copy`、`%if`、`%main` / `%epilogue`、`%space`、`%{{ ... }}%` / `%code … %endcode`、`%class NAME` / `--class`。
- `%package` / `%import` / `%class_body … %endbody` 是 `--lang csharp` / `--lang rust` / `--lang go` 专属，Python 下**提示一句并忽略**（不报错），所以带这三个指令的文法照样能用 `--lang python` 快速试跑。
- 语义动作写 Python 表达式：`$$` 是 `result.value`，`$1` 是 `values[0]`（`$n` 就是右部第 n 个符号的值），`&&` / `||` / `!` 请写 `and` / `or` / `not`，语句结尾不写分号。
- 换类名用 `%class` / `--class`（缺省 `Parser`）；产物文件名固定是 `parser.py` / `main.py`。

## 例子（照抄就行）

| 想学什么 | 看哪份文法 |
| --- | --- |
| LL(1) + `%tree`（列表 / 可选字段） | `example/json.gram`、`example/ini.gram` |
| 语句 + 表达式 + 左递归 | `example/pl0.gram`、`example/calc_prec.gram` |
| 正则 Token / `%identifier` / 关键字 | `example/lex_demo.gram` |
| 一次报多个错 | `example/error_recovery.gram`、`example/sync_recovery.gram` |
| Python 产物 + `%class` + `%{{...}}%` | `example/python_demo.gram` |
| 空白 / 换行当 Token（一行一条） | `example/space_demo.gram` |
| 编译原理课的三个实验（文法类型判断 / 词法分析 / 语法分析） | `project/lab_report_axhu/lab1_grammar_type/`、`project/lab_report_axhu/lab2_lexer/`、`project/lab_report_axhu/lab3_parser/` |

完整文法规范见 `doc/05-grammar.md`，Python 产物 API 见 `doc/08-generated-code.md` §11。

## 别做的事

- 别把整份日志、整份产物或完整解析输出读进上下文——先 `head`、先只打印失败行。
- 别为了「以后可能用到」写大文法：先 `%tree` 把结构跑通，再按需加动作。
- 别用 Python 产物做交付（它是分析工具）：要交付就给 C / C++17 产物。
- 别把语法通过当成命令有效：文法接受只说明文本形式匹配，不说明协议、状态、固件或数值来源正确。
- 别为一次性分析扩展通用引擎：先算净节省工时，为负就保留现有解析器做辅助检查，把投入转回业务规则、逆向证据和回归样本。
