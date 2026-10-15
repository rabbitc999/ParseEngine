# 09 用 `.gram` 写文法（用户手册）

本章从 [`README.md`](../README.md) 拆出：`.gram` 指令表、语义动作、语义值类型与用户代码注入、
冲突消解、错误恢复、条件编译（`%if`）、自定义入口（`%main` / `%epilogue`）与报错定位（`#line`）。

规范与生成期校验的细节见 [`05-grammar.md`](05-grammar.md)，翻译与模板的实现见
[`07-codegen.md`](07-codegen.md)，产物侧的 API 见 [`08-generated-code.md`](08-generated-code.md)。
选择目标语言与模式前，先查 [12 各语言支持的模式与增量 API](15-language-mode-support.md)。

## `.gram` 文法写法

规则有两种写法：**一行一条**（同名左部写多行即可），或 yacc 风格的 `|` 分支
（同一左部的多个分支写在一起）。分支写法每个分支各带自己的动作，整条规则**必须
以 `;` 结束**：

```text
initializer := assignment_expression        { $$ = $1; }
             | LBRACE initializer_list RBRACE      { $$ = $2; }
             | LBRACE initializer_list COMMA RBRACE { $$ = $2; }
             | LBRACE RBRACE                       { $$ = NULL; };
```

动作可写 `{ ... }`，也可省掉花括号写到行尾。`%empty` 表示空产生式。文件里的
`//` 与 `/* */` 注释会被跳过。

### 模式、语言与代码落点

| 指令 | 说明 |
| --- | --- |
| `%define NAME "字面量"` | 固定字符串 Token，用 `\|` 分隔多个候选：`%define EQ "==\|="`（`%token NAME "字面量"` 完全等价） |
| `%token A B C` | 声明没有固定字面量的 Token（由词法层外部产生） |
| `%token NAME "a\|b"` | 固定字符串 Token；字符串里只有 `\|` 有特殊含义（分隔候选，`\\\|` 为普通竖线） |
| `%space "空白字符集"` | 取 Token 前跳过哪些空白（缺省 `"\b"` = 全部空白；`%space ""` = 一个都不跳过）。`\b` 写在 `%token` 里是「一个空白字符」的通配，写在 `%space` 里代表全部空白 |
| `%start name` | 起始非终结符（缺省取第一条规则的左部） |
| `%example "文本"` | 生成文档所用的示例输入；最多一次，支持 `\\`、`\"`、`\n`、`\t` 等字符串转义，`\0` 因无法通过命令行传递而拒绝。八种代码后端的 `RESULT.md` 会在命令行与 API 示例中使用它；没写时显示 `<符合文法的语言>` |
| `%mode ll1 \| ll0 \| llk \| lr0 \| slr \| slrk \| lalr \| lalrk \| lr1 \| lrk \| ielr \| glr \| earley \| cyk \| dd \| lex` | 生成模式；与命令行 `--mode` 不一致时以命令行为准，缺省 `ielr`。`earley` / `cyk` 支持 C++17、Python、C#、Rust 产物；各模式说明见[生成模式与运行时](13-modes.md) |
| `%lookahead N`（或写进模式名 `LL(2)`；命令行 `--lookahead N`） | 展望符长度 k，取 0..INT_MAX、缺省 2；只对 `lrk` / `lalrk` / `slrk` / `llk` / `ll0` 有意义（别的模式写了直接报错）。N = 1 时分别等价于 `lr1` / `lalr` / `slr` / `ll1`。**没有策略上限**，但 k 一大 `FIRST_k` / `FOLLOW_k` 与列数就指数级膨胀（列编码放不进 64 位时生成期会报错并给出本文法上限） |
| `%teach` | 打开[教学模式](13-modes.md#教学模式教学提示与教学对照)；等价于命令行 `--teach`（`--no-teach` 可反向覆盖），缺省**关闭** |
| `%mermaid` | 生成 [Mermaid 状态机图](13-modes.md#状态机图示mermaid)；等价于命令行 `--mermaid`（`--no-mermaid` 可反向覆盖），缺省**关闭**；与 `%teach` 相互独立 |
| `%lang c \| cpp17 \| python \| csharp \| rust \| go \| gram` | 产物语言；与命令行 `--lang` 不一致时以命令行为准，都缺省时取 `cpp17`。`gram` 不生成代码：只导出文法原文件 + 分析报告，见 [`14-languages-and-products.md`](14-languages-and-products.md#文法导出--lang-gram) |
| `%class NAME` | Python 产物的解析器类名（等价于 `--class`，命令行优先；缺省 `Parser`）；NAME 取 `[A-Za-z_][A-Za-z0-9_]*`、不能是 Python 关键字；只对 `--lang python` 有效，配 C / C++17 会报错 |
| `%package NAME` | C# 的 `namespace NAME;` / Rust 的 crate 名（`Cargo.toml` 的 `package.name`）/ Go 的 module path（`go.mod` 的 `module`，`::` 归一成 `/`）；NAME 取分段标识符（`.` 或 `::` 连接，例如 `MyApp.Compiler`）；只对 `--lang csharp` / `--lang rust` / `--lang go` 有效，别的目标语言下提示一句并忽略 |
| `%import NAME` | C# 的 `using NAME;` / Rust 的 `use NAME;` / Go 的 `import ( … )`（`::` 归一成 `/`，与标准库重复的包跳过）；可重复，末段可以是 `*`（如 `System.Collections.*`）；只对 csharp / rust / go 有效，别的目标语言下提示一句并忽略 |
| `%class_body` … `%endbody` | 段标记：两个标记各自单独成行，之间的正文**原样**插进解析器类 / 实现内部（C# 放辅助方法 / 字段 / 内部类，Rust 放 `impl Parser { … }` 里的东西，Go 贴成一组 `func (p *Parser) …` 方法）；只对 csharp / rust / go 有效 |
| `%namespace NAME` | C++17 产物的命名空间名与文件名前缀（等价于 `--namespace`，命令行优先；缺省不传 = 命名空间 `Parser`、文件名不带前缀）；NAME 取 `[A-Za-z_][A-Za-z0-9_]*`、不能是 C++ 关键字；只对 `--lang cpp17` 有效，配 `--lang c` 会报错 |

### 语义值、类型绑定与清理

| 指令 | 说明 |
| --- | --- |
| `%value_type 类型` | 声明语义值类型（如 `%value_type double`、`%value_type struct Node *`），生成器补 `typedef … ParserValue_T;` |
| `%code ... %endcode` | 用户 C 代码块，可跨行、可写多段；按出现顺序原样拼接进 `parser_user.h` |
| `%code` … `%endcode` | 用户代码块：两个标记各自单独成行，中间内容原样拼接进 `parser_user.h`；可写多段 |
| `%code include "路径"` | 同上，但正文来自外部文件：标记行直接写 `include "路径"`（也接受 `include="路径"`），**不用**写 `%endcode`。`%epilogue` / `%main` / `%class_body` 同样支持，见[把大段用户代码放外部文件](#把大段用户代码放外部文件include) |
| `%union { ... }` | 语义值联合体（与 `%value_type` 二选一）：成员声明原样贴成 `typedef union { ... } ParserValue_T;`，追加在用户代码之后；见 [`%union` + `%type`](#union--type按符号分-c-类型的语义值) |
| `%type <成员> a b` | 给非终结符绑定 `%union` 成员（一个指令可带多个符号） |
| `%token <成员> A B` | 给 Token 绑定 `%union` 成员；标签作用于后面所有符号，可与 `regex` / `"字面量"` 写法叠加 |
| `%token NAME include "词表.csv"` | 从 CSV **追加**字面量别名：只取第一列等于 `NAME` 的行的第二列，作为字面量接到 `NAME` 的别名列表末尾。只追加别名、不新建 Token；第二列一律当字面量（不编译正则）；表头不校验、只读前两列；别名必须唯一；带 `<成员>` 的 Token 也能用。见[用 CSV 维护字面量别名表](#用-csv-维护字面量别名表) |
| `%destructor <成员> { ... }` | 给 `%union` 成员登记清理代码：语义值被丢弃（错误恢复弹栈 / 解析失败收尾清栈）时执行，片段里的 `$$` 就是那个值；见 [`%destructor`](#destructor丢弃语义值时按成员清理) |

### 恢复、资源限制与其余指令

| 指令 | 说明 |
| --- | --- |
| `%max_stack_depth N` | 分析栈深度上限（`PARSER_MAX_STACK_DEPTH`，1..65535，缺省 64） |
| `%max_errors N` | 一次解析最多报几条错（`PARSER_MAX_ERRORS`，1..4096，缺省 8） |
| `%error_recovery_tokens N` | 错误恢复后静默几个 Token（`PARSER_ERROR_RECOVERY_TOKENS`，0..4096，缺省 3；0 = 恢复后立刻报下一个错） |
| `%sync TOKEN...` | 声明错误恢复的同步锚点 Token（见[错误恢复](#错误恢复error-产生式--sync--ll-的-follow-集)）：LR 家族在 `error` 产生式找不到恢复点时兜底，LL 家族是额外汇进来的锚点；Token 必须是已声明的，没声明报错 |
| `%error_context` | 错误记录里多一段「出错那一行原文 + `^` 指示」（`ParserError_T::context`，长度 `PARSER_ERROR_CONTEXT_LENGTH` 缺省 120）。等价于编译期 `-DPARSER_ERROR_CONTEXT`；**缺省关闭**（有内存开销，且只有 `Parser_RunText` 这类文本模式拿得到源文） |
| `%empty` | 产生式右部写 `%empty` 表示空产生式（等价于右部什么都不写，但更显眼） |
| `%if` / `%elif` / `%else` / `%endif` | 条件编译：按 `--define` 决定哪些行进产物，见[条件编译](#条件编译if) |
| `%epilogue` | 段标记：之后的正文原样贴到 `parser.c`（C++17 是 `parser.cpp`，Python 是 `parser.py`）末尾；Python 下会先按 Python 缩进规则归一化，见[自定义入口与 epilogue](#自定义入口与-epiloguemain--epilogue) |
| `%main` | 段标记：之后的正文替换产物 `main.c`（C++17 是 `main.cpp`，Python 是 `main.py` 的演示体）里的内建 demo main；多段时用 `%if` 保证只有一段生效 |
| `%tree` | 生成通用语法树 API（除 `%mode lex` 外的所有模式都支持，`lex` 下被忽略并提示一句） |
| `%left` / `%right` / `%nonassoc` | 声明 Token 的结合性与优先级，行内自左向右递增，后声明的行更高 |
| `%prec TOKEN` | 写在产生式右部末尾，覆盖该产生式的优先级来源 |
| `%expect N` | 允许保留 N 个未消解的移进/归约冲突 |
| `%token regex NAME "正则"` | 用正则定义 Token（**C99** 在生成期编译成 DFA 写进产物；C++17 / Python / C# / Rust / Go 把正则原样交给各自语言的正则库） |
| `%identifier "正则"` | 声明标识符正则，生成标识符识别与关键字查表接口 |
| `%commit [类型] "模式"` / `%commit skip "模式"` | 把「识别但不进语法层」的文本收进容器（元素 `{line, type, text}`）；类型缺省 `default`，`skip` 匹配即丢。模式按正则编译，与 `%token` 同一份最长匹配顺序（同长度按声明先后）；见 [`%commit`](#commit把识别但不进语法层的文本收进容器)。**六档都支持**（C99 / C++17 / Python / C# / Rust / Go），各档的容器类型与所有权见 [`08-generated-code.md`](08-generated-code.md) |
| `%trivia "模式"` | 匹配即丢：不进容器、不进语法层。与 `%commit skip "模式"` 完全等价（两种写法生成的词法表逐字相同），推荐用 `%trivia` 表达 trivia；排序与冲突规则同 `%commit` |
| `%copy [file\|dir] 路径` | 把文法目录下的库文件 / 目录拷进产物目录；缺省 `file`，拷进来的 `.c`/`.h` 会进 `Makefile` / `CMakeLists.txt` |
| `%cpp_only` | 声明「这份文法只能生成 C++17 产物」：配 `--lang c` / `--lang python` / `--lang csharp` / `--lang rust` / `--lang go`（或 `%lang` 里写那几个）直接报错，见[只能出 C++17 的文法](14-languages-and-products.md#只能出-c17-的文法cpp_only) |

### 用 CSV 维护字面量别名表

关键字表 / 指令表 / 枚举名表这类「一个 Token 有几百个字面量」的场合，把别名列表
放进 CSV，用一条 `%token` 指令追加进来（不用在 `.gram` 里堆一长串 `"a\|b\|c"`）：

```gram
%token A include "kw.csv"                 /* A 的别名来自 kw.csv */
%token <ival> NUM include "numbers.csv"   /* 带成员的 Token 也能用 */
```

语义：打开 CSV，只取**第一列**等于 `A`（或 `NUM`）的行，把每行的**第二列**作为
**字面量**追加到该 Token 的别名列表末尾。等价于手写
`%token A "let|print|while"`。

CSV 长这样（**表头随便写、不校验**，第 3 列及以后一律忽略）：

```csv
token,word,note,extra
A,"let",关键字,噪音列
A,"print",关键字,噪音列
B,"这一行会被忽略",文法没导入 B,
NOT_TOKENS,"这一行也会被忽略",不在文法里,
```

- **只追加别名、不新建 Token**：`A` 必须已经或同时由普通 `%token` 声明；
  CSV 第一列写了个文法里不存在的名字（如上面的 `NOT_TOKENS`），或写了某个 Token
  名但文法没对它写 `include`（如上面的 `B`），**整行静默忽略**，不报错。
- **第二列一律按字面量处理**，绝不编译成正则——要正则 Token 继续用
  `%token regex NAME "..."`。
- **表头不校验、只读前两列**：不要求第一行是表头；只读每行的第 1 / 2 列，
  第 3 列及以后忽略。
- **别名必须唯一**：同一个 Token 下重复别名静默去重；不同 Token 用同一个别名
  报错（错误指向 CSV 行与首次分配处）；别名撞上已声明的 Token 名也报错。
  `%token regex` 不参与这个判定。
- 路径相对**声明它的那份 `.gram` 文件所在目录**解析，允许子目录；同一份 CSV
  被多处引用时只读一次。
- `%if` 假分支包住的 `%token ... include` **不读 CSV**（条件编译在展开之前跑）。
- **编辑器也认识这条**：`library/lsp/` 与跟着它的 VS Code 插件
  把 `include`（以及 `alias`）当 `%token` 的行内关键字——不会错误地多出一个
  叫 `include` 的 Token，悬停 `include` 有写法说明、悬停 Token 名会告诉你别名
  来自哪份 CSV。但 LSP 是纯文本模型、**不打开 CSV**：CSV 不存在、第一列为空、
  别名跨 Token 冲突这些仍由生成器在生成期报。

CSV 内错误带文件行列（相对 `.gram` 目录的路径）：

```text
1.csv:5:2: 第 2 列（字面量）不能为空
1.csv:9:2: 别名 "x" 已被 token A 使用，不能重复分配给 token B（首次出现于 1.csv:5:2）
example/calc.gram:8:1: 找不到 CSV 文件 1.csv
```

现成例子：[`example/token_dict_demo.gram`](../example/token_dict_demo.gram)
（`printf 'let num2 + num3 manual_b' | ./parser_demo` → 打印语法树，
`A(let)` 与 `NUM(num2)` 都是 CSV 里的别名，`B(manual_b)` 是手写字面量；
CSV 里未导入的 `B` / `NOT_A_TOKEN` 行一个都没进产物）。

> 小坑：**内置 `NUMBER` 规则在「剩下的固定字面量」之前匹配**，纯数字串会被先吃成
> `NUMBER`。要用纯数字当别名，先 `%token NUMBER ...` 之类把内置规则接管掉；
> 示例里因此把 `NUM` 的别名写成了 `num2` / `num3`。

### `%commit`：把识别但不进语法层的文本收进容器

想把**注释**这类「识别出来、但不参与语法分析」的文本留下来（后处理、生成
文档、交给 AI 分析日志），用 `%commit` 把它们收进解析器的容器：

```gram
%commit block "/\*([^*]|\*+[^*/])*\*+/"   /* 块注释，类型 block */
%commit line  "//[^\n]*"                  /* 行注释，类型 line */
%commit "0x[0-9a-fA-F]+"                  /* 类型缺省 = default */
%commit skip  "@[A-Za-z]+"                /* 匹配即丢，不进容器 */
%trivia       "#[A-Za-z]+"                /* 同上：%commit skip 的推荐写法 */

%start program
program := NUMBER "+" NUMBER;
```

规则：模式是正则（没有元字符的写法就是固定字面量；**只对 C99 目标**要写 DFA
友好形式——DFA 不支持非贪婪，其余五档的目标正则库支持，见
[`05-grammar.md`](05-grammar.md) §「已知的 DFA 构造坑」）；匹配到的文本**不进语法层、
也不当 Token**，
而是按出现顺序追加进容器，元素是 `{line, type, text}`——`line` 是片段起始的
1-based 行号，`text` 是原文（含字面量本身，跨行片段保留换行）。匹配与
`%token` / 内建注释规则共用同一份最长匹配顺序：**最长匹配优先，同长度按声明
先后**；同一个模式不能既当 Token 又当 `%commit` / `%trivia`。

各语言产物怎么取容器（**六档都支持**：C99 / C++17 / Python / C# / Rust / Go，
这里给 C++17 与 Python 两个例子；Rust 是 `engine.commits()`、Go 是 `engine.Commits()`，各档的容器与所有权细节见
[`07-codegen.md`](07-codegen.md) §13，位置信息 API 见
[`08-generated-code.md`](08-generated-code.md) §1.5）：

```cpp
// C++17（parser.hpp）
const std::vector<ParserCommit> &commits = engine.commits();
for (const ParserCommit &entry : commits) { /* entry.line / type / text */ }
```

```python
# Python（parser.py）
for entry in engine.commits:      # ParserCommit(line, type, text)
    print(entry.line, entry.type, entry.text)
```

`example/commit_demo.gram` 把四种写法放在一起，六档（C / C++17 / Python /
C# / Rust / Go）生成的输出逐字一致（C# 与 Rust 在 Windows 上是
CRLF 行尾）。

**「匹配即丢」推荐写 `%trivia`**：`%commit skip "<模式>"` 与
`%trivia "<模式>"` 完全等价——都是「匹配到的文本不进容器、也不当 Token」，
共用同一个词法声明序（最长匹配、同长度先声明优先）和同一条「模式不能与
`%token` 重复」的校验，两种写法生成的词法表与产物**逐字相同**；`%commit skip`
只为兼容保留。`example/commit_demo.gram` 里两种写法各留了一条（`@tag` 走
`%commit skip`、`#tag` 走 `%trivia`），两条片段都不会进容器。

#### 正则的一个已知坑（块注释最容易撞上）

**C99** 的所有正则（`%token regex` / `%commit` / `%trivia`）都由 `source/src/regex_dfa.cpp`
编成 DFA 写进产物，其中有一类写法**编译得过、却永不匹配**：否定类同时排掉 `*` 和
结束符那个字符，又放在 `(A|B)*` 的分支里——`(* … *)` 注释就是典型
（**C++17 / Python / C# / Rust / Go 不受这条影响**：它们把正则原样交给各自语言的正则
库）：

```gram
%trivia "\(\*([^*]|\*+[^*)])\*+\)"     /* ✗ 永远认不出 (* ... *) */
%trivia "\(\*([^*]|\*+[^)]+)*\*+\)"    /* ✓ 等价、能用（推荐这一族写法） */
```

踩中时没任何提示：词法器只是把 `(` 报成「认不出来的字符」。所以块注释一律照
`\*+[^)]+` 这种写法来（C 风格 `[^*/]`、`{ … }` 的 `[^}]` 都没事）；拿不准就把
正则单独写一份 `%mode lex` 小文法、喂一行输入看 Token 列表，5 秒能验。
细节与来龙去脉见 [`05-grammar.md`](05-grammar.md) §「已知的 DFA 构造坑」，现成例子见
`project/pl0-ir/pl0.gram`。

## 语义动作

| 写法 | 含义 |
| --- | --- |
| `$$` | 本条产生式的返回值 |
| `$1` `$2` … | 第 n 个右部符号的返回值 |
| `$$.字段 = $3;` | 语义值是结构体时按字段赋值（`$$` 可作左值，`$n` 不可以） |
| `int32(0)` / `float(1.5)` | 转成 C 强制转换 `(int32_t)(0)` / `(float)(1.5)` |
| `$1.字段` | 普通 C 成员访问；`$n.<标量类型名>`（如 `$1.int32`）会报错 |
| `$1` / `$$`（写了 `%union`） | 照旧写 `$1 + $3` / `$$ = $1`，生成器按符号绑定的成员自动补 `.成员`（手写 `$1.ival` 也原样穿过） |
| `sqrt($3)` / `pow($1,$3)` | 就是同名 C 函数，自己 `#include <math.h>` |

算术/逻辑运算符（`+ - * / % && || 比较 位运算`）**原样生成 C 运算符**，生成期不做
类型检查，交给 C 编译器。不在支持范围内的写法会在生成期报错，并提示改在 `%code` 里用 C
自己实现——清单见 [`05-grammar.md`](05-grammar.md) 的「生成期校验」一节。

每种模式都要求“每条产生式要么写动作，要么文法启用 `%tree`”，否则报
“规则 X 缺少语义动作；请写动作或启用 %tree”。

## 语义值类型与用户代码注入

### `%value_type`：一行声明语义值类型

最省事的写法是在文法开头写一行 `%value_type`，生成器会补
`typedef <类型> ParserValue_T;`（追加在 `%code ... %endcode` 用户代码之后，所以类型可以
引用用户代码里定义的结构体）：

```text
%value_type double                 /* → typedef double ParserValue_T; */
%value_type struct AstNode *       /* 也可以是指针 / 结构体等任意 C 类型 */
```

| 声明方式 | 语义值类型 | 适用 |
| --- | --- | --- |
| `%value_type <C 类型>` | 所有符号共用这一个类型 | 简单、推荐；结构体 / 指针 / 标量都行 |
| `%union { ... }` + `%type <成员>` | 联合体，按符号绑定不同成员 | 想要 yacc/bison 那样「每个符号一种 C 类型」 |
| 自己在 `%code ... %endcode`（或 `%copy` 进来的头文件）里 typedef | 用户完全接管 | 需要把类型定义集中在自己的头文件里 |
| 都不写（又没有 `%code ... %endcode`） | 生成器补 `typedef int ParserValue_T;` | 只想先跑通 |

`%value_type` 与 `%union` 不能同时写（都在定义 `ParserValue_T`，生成期报错）；
写了 `%value_type` 时用户代码里也不能再定义 `ParserValue_T`。

各档产物的语义值类型对照（C# 要写目标语言自己的类型名）见
[`14-languages-and-products.md`](14-languages-and-products.md)：C# 都没写时
是 `object`；`%union` 在 C# 生成带成员字段的类
（成员按各语言的字段写法写，段与段之间照旧用 `;` 分隔）。

### `%code ... %endcode` 代码块

`%code` 与 `%endcode` 之间的内容**原样**收集（不解析、不转义、可跨行），按出现顺序
拼接进生成的 `parser_user.h`。两个标记必须单独成行；代码块可以放在文件任何位置，不会影响其它指令
的解析；没有代码块、也没有 `%value_type` / `%union` 时不生成 `parser_user.h`。
生成的 `parser.c` 与 `parser.h` 都会包含该头文件。

旧 `%{{ ... }}%` 语法已禁用，加载时会提示改用 `%code ... %endcode`。

Python 产物（`--lang python`）下这段正文不生成 `parser_user.h`，而是插在
`parser.py` 的 `import` 之后（缩进按 Python 规则归一化，见
[Python 产物](14-languages-and-products.md#python-产物给文法做快速分析--lang-python)）。

```text
%code
#include "mylib.h"

/* 辅助函数 / 宏 / 全局表都放这里；语义值类型用一行 %value_type 更省事 */
static double My_Clamp(double value) { ... }
%endcode
```

### `%union` + `%type`：按符号分 C 类型的语义值

想要 yacc/bison 那样**每个符号一种 C 类型**的语义值，就用 `%union` 配合
`%type` / `%token <成员>`：

```text
%union {
    int          ival;   /* 成员声明原样贴进产物，生成期不解析 C 类型 */
    double       dval;
    struct Node *node;   /* 可以引用 %code ... %endcode 里 struct / typedef 出来的类型 */
}
%code
/* 用户代码（排在联合体定义之前） */
%endcode

%token <ival> NUMBER
%type  <ival> expr term
expr := expr PLUS term { $$ = $1 + $3; }   /* → result.value.ival = values[1].ival + values[3].ival; */
      | term           { $$ = $1; }
      ;
```

生成器把 `%union` 的成员原样贴成 `typedef union { ... } ParserValue_T;`（追加在
用户代码之后），动作里的 `$$` / `$n` 按符号绑定的成员自动补 `.成员`，所以
`$1 + $3`、`$$ = $1` 这些写法一个字都不用改。生成期只做括号配平（找最外层 `}`、
按顶层 `;` 切段）与成员名提取，完整的声明符解析交给 C 编译器，因此嵌套
`struct` / `union` / `enum` 定义、函数指针成员、数组、位域、`const` 指针都能写；
类型不匹配不拦，由 C 编译器报警告/报错。

终结符也不用再手写 `PARSER_USER_TOKEN_VALUE`：写了 `%union` 时
`ParserToken_T.value` 本身就是 `ParserValue_T`，词法器按 `%token <ival> NUMBER`
把数字词素写进 `pToken->value.ival`（双引号字符串、标识符与 `%token regex` 写
匹配到的文本）。只有想自己接管转换——例如把正则匹配到的数字文本转成 `int`——
时才需要定义那个宏，它优先。

要是 `%union` 下漏了 `%token <成员> NUMBER`，联合体里就没有放数字的地方：生成器
不会硬写一句编译不过的 `pToken->value = …`，而是不写赋值——数字 Token 的语义值
恒为零值，生成期会打一条 `警告：` 提醒（要用数字的值就补上那条 `%token`）。

一个坑：C++17 产物在命名空间里带了一层兼容层，`Parser_RunText` / `ParserError_T`
这类名字被它占用；`%code ... %endcode` 与 `%epilogue` 里的用户代码生成器一个字都不改写，
所以在 `%code ... %endcode` 里定义的函数 / 类型**不要**用 `Parser_` 开头，否则会跟兼容层的
声明撞名。换个前缀（例如 `Demo_MakeNode`）即可。C99 产物同理：
`Parser_` 是生成器保留的前缀。

**设计取舍**：只有「所有符号共用同一个 `ParserValue_T`」与「`%union` 按符号分
成员」两条路，前者用 `%value_type` 一行搞定。没写 `%union` 时生成期完全不碰
成员展开这条路径——改生成期时可以拿一份老版本的产物对一遍，确认「没写 `%union`
就不该动产物」。

指令细节见 [`05-grammar.md`](05-grammar.md) §2，翻译规则见
[`07-codegen.md`](07-codegen.md) §4.1，完整例子见
`example/union_demo.gram`。

### `%destructor`：丢弃语义值时按成员清理

`%union` 的成员如果是裸指针这类**需要手工释放**的类型，就用 `%destructor`
按成员登记清理代码（写法与 bison 一致）：

```text
%union {
    char        *str;    /* 堆内存：丢弃时要 free */
    struct Node *node;   /* 堆节点：丢弃时要 Node_Free */
    int          ival;   /* 标量：没什么可清的 */
}

%type  <str>  list item
%token <node> VAR

%destructor <str>  { free($$); }
%destructor <node> { Node_Free($$); }
```

片段里的 `$$` 指**正在被丢弃的那个值**，按成员展开（C 产物里是 `pValue->str`，
C++17 产物里同样）。片段是原样收集的 C 语句，只认 `$$`；写 `$1` / `$line`
这类需要「右部符号」的写法会在生成期报错——清理现场没有右部符号可指。

**什么时候调**（与 bison 同规矩）：

| 时机 | 说明 |
| --- | --- |
| 错误恢复弹栈 | `%sync` / `error` 产生式恢复、以及 LL 的 FOLLOW 集恢复（放弃一层构造）时被弹掉的那些槽位 |
| 解析失败收尾 | 返回失败前把还留在栈上的语义值逐项丢弃 |
| 成功解析 | **不清栈**：起始符号的结果交还调用方（结果里的指针归调用方所有） |

**谁来走**：用 `%type <成员>` / `%token <成员>` 绑到该成员的符号。同一个成员只能
写一条 `%destructor`；没写 `%destructor` 的成员保持原样——C 产物仍然对每个被丢掉
的值调 `PARSER_USER_DISCARD_VALUE` 钩子（兼容老写法），C++17 产物本来就是 RAII
语义值、没有这个钩子。粒度是**成员**，不是具体符号名。LL 家族的语义值放在
「语义帧」里，运行时同样按槽位记了符号号，丢弃时查同一张表。

**限制**：`--lang python` 下写 `%destructor` 直接报错（Python 的语义值是任意
对象，靠引用计数回收）。

完整例子与自检：[`example/destructor_demo.gram`](../example/destructor_demo.gram)
（两个计数器分别数「释放了几次堆内存」「丢弃了几个数值」，喂 4 组输入打印计数）。

### `%tree` 的语法树

`%tree` 与语义值类型完全解耦：节点类型是 `ParserTree_T`，**只保存符号名与
Token 词素**，不保存语义值。

```c
typedef struct ParserTree_T {
    const char *pName;                 /* 符号名（终结符 / 非终结符） */
    char *pText;                       /* 终结符词素；非终结符为 NULL */
    struct ParserTree_T **ppChildren;  /* 子节点数组 */
    uint32_t childCount;
    uint32_t childCapacity;
} ParserTree_T;

ParserTree_T *Parser_ParseTreeText(const char *pText);
uint32_t      Parser_TreeChildCount(const ParserTree_T *pNode);
const ParserTree_T *Parser_TreeGetChild(const ParserTree_T *pNode, uint32_t index);
const char   *Parser_TreeGetName(const ParserTree_T *pNode);
const char   *Parser_TreeGetText(const ParserTree_T *pNode);
void          Parser_TreePrint(const ParserTree_T *pRoot);
void          Parser_TreeFree(ParserTree_T *pRoot);
```

### `%copy` 拷贝库文件

`%copy [file|dir] 路径` 把文法目录下的库文件 / 目录拷进产物目录：

- `%copy file lib/value.h` 或简写 `%copy lib/value.h`（缺省就是 `file`）：
  按文件名拷到输出目录，即 `out/value.h`；
- `%copy dir lib`：递归拷贝，保持目录层次，即 `out/lib/...`；
- 相对路径以文法文件所在目录为基准；
- 拷进来的 `.c` / `.h` 会自动加进生成 `Makefile` 的 `LIB_SRCS` / `HDRS` 与
  `CMakeLists.txt` 的源文件列表，可直接参与编译。

```text
%mode lalr
%tree
%start expr
%copy file lib/parser_value.h     /* 拷进产物目录并算进 HDRS */

%code
#include "parser_value.h"
%endcode
```

完整示例见 `example/custom_tree.gram`。

## 冲突处理与优先级

- **默认（不写优先级指令）**：SLR(1) / LALR(1) 遇到**移进/归约**冲突在生成期抛错，
  不消解、不产出代码；IELR(1) 会先尝试拆分冲突状态，拆不掉才报错。
- **写了优先级指令后**：只消解移进/归约冲突——移进 Token 的优先级来自
  `%left/%right/%nonassoc`；归约产生式的优先级取 `%prec` 指定的 Token，没写就取
  右部最右终结符。优先级高者胜；同级时 `%left` 归约、`%right` 移进、
  `%nonassoc` 视为错误（生成期报错）。
- **归约/归约冲突自动选一条 + 一条醒目的大警告**（确定性 LR 模式，bison 也是这么
  做的）：先看两条产生式的优先级（`%prec` 或右部最右终结符），**不一样就优先级高
  的胜出**（这就是「用户推荐的产生式」）；一样就取**文法里更早出现的产生式**。
  选中项写进 ACTION 表、被放弃的记进 `[冲突消解]`，生成期再打一条横幅警告，
  说清「选了谁、弃了谁、依据是什么」（最多列 8 条、其余汇总，免得 LR(0) 刷屏）。
  IELR 仍然会先按 LR(1) 上下文拆分状态，拆到头还冲突才走这条。
- **`lr0` / `lr1` 是例外，任何模式下都不中止**：这两个模式本来就要把「文法不够
  强」摆出来，未消解冲突只写进报告的 `[未消解冲突]`，表项按 yacc 默认（移进
  优先）继续写（见 [LR(0) / LR(1) 规范自动机模式](13-modes.md#lr0--lr1-规范自动机模式)）。
- **`slr` / `lalr` / `ielr` 开了教学模式（`%teach` / `--teach`）才放行**：这时
  不因未消解冲突中止，只在报告里写清「该文法不是 X 文法」与冲突清单，表项按
  移进优先继续生成。不写 `%teach` 时这三个模式严格：未消解冲突直接报错、
  不产出代码。
- **`%expect N`**：未消解的移进/归约冲突数量与 N 完全一致时放行（输出警告，并按
  移进优先写表），否则报错。
- 消解结果与未消解冲突都会写进分析报告的 `[冲突消解]` / `[未消解冲突]` 小节，
  分文件布局还会同步写进 `RESULT.md`。
- **LL(1) 不受影响**：新指令能解析但被忽略，产物与不使用该指令时一致。

```gram
%mode lalr
%left  PLUS MINUS
%left  STAR SLASH
%right UMINUS
%right POW

expr := expr PLUS expr  { $$ = $1 + $3; }
expr := expr MINUS expr { $$ = $1 - $3; }
expr := expr STAR expr  { $$ = $1 * $3; }
expr := expr SLASH expr { $$ = $1 / $3; }
expr := expr POW expr   { $$ = pow($1, $3); }
expr := MINUS expr %prec UMINUS { $$ = -$2; }
expr := NUMBER { $$ = $1; }
```

## 错误恢复（error 产生式 / %sync / LL 的 FOLLOW 集）

默认行为不变：语法错误只记一条 `ParserError_T`、返回负错误码。想让解析器**跳过
坏掉的片段继续解析**（一次解析报多个错），三种模式各有一条路：

| 写法 | 适用模式 | 恢复策略 |
| --- | --- | --- |
| `stmt := error SEMI { yyerrok; $$ = 0; }` | LR(0) / SLR(1) / LALR(1) / LR(1) / IELR(1) | 弹栈到能移进 `error` 的状态，把 `error` 当伪 Token 移进，再用同一个 lookahead 继续 |
| `%sync SEMI RBRACE` | LR 五种 + LL 家族 | LR：弹栈（先做掉栈上待归约的项）到能移进某个同步 Token 的状态，丢掉输入直到那个 Token，再接着解析；LL：额外补充的锚点 |
| （什么都不用写） | LL(1) / LL(k) | **FOLLOW 集恢复**：出错时先看栈顶能不能取当前 Token，再看它是不是 `%sync` 锚点，再看栈顶那一项「该结束了」（当前 Token 在它的 FOLLOW 里）或更深处有项能取它，就放弃栈顶那一项所属的**构造**（连同它里面已展开的内层构造一起退栈）、把 Token 交给外层；谁都不认才丢掉 Token。LL(0) 没有可用的 FOLLOW 信息，仍是「一错即停」 |

- **`error` 是保留字**：只能出现在产生式右部；放左部，或写进 `%type` / `%token` /
  `%define` / `%start` / `%prec` 都在生成期报错。它的 `$n` 是零初始化的
  占位值，内容是垃圾，别依赖。
- **`yyerrok` / `yyclearin`** 写在 error 规则的动作里（单独成句）：`yyerrok` 清掉
  「正在恢复」状态，让后面的错误照常上报；`yyclearin` 丢掉当前 lookahead。不写
  `yyerrok` 时，恢复后的 3 个 Token（`PARSER_ERROR_RECOVERY_TOKENS`）内不再重复报错
  ——与 bison 的 3 Token 规则一致。两者只在 LR 家族可用，LL(1) 下报错。
- **两者都写时以 `error` 产生式为准**，`%sync` 只在找不到 `error` 列动作时兜底；
  LR 下两层都没写就是老行为（报一个错、返回失败）。
- **LL 的 FOLLOW 集恢复不用写 `%sync`**：`%sync` 在 LL 下只是「额外补充的锚点」，
  把当前 Token 换成它时按「弹栈到能取它的位置」恢复；真正的默认手段是 FOLLOW 集
  ——出错的那一层该结束时（当前 Token 落在它的 FOLLOW 里）直接放弃这一层。所以
  LL(1) / LL(k) 不写 `%sync` 也能一次解析报多个错。
- **模式限制**：`error` 产生式在 LL(1) 下生成期报错——自顶向下没有「弹栈到最近
  恢复点」这个动作。
- **一次解析最多报 `PARSER_MAX_ERRORS`（8）条错**：用 `Parser_GetErrorCount()` /
  `Parser_GetError(index)` 逐条取，`Parser_GetLastError()` 仍然是最后一条。恢复过程中
  报过错但最终解析成功时返回值仍是 `PARSER_OK`，结果可能不完整，调用方看
  `Parser_GetErrorCount()` 决定要不要信任它（见[生成代码的 API](14-languages-and-products.md#生成代码的-api)）。
- **容量宏可以在文法里定**：`%max_errors N` / `%error_recovery_tokens N`（还有
  `%max_stack_depth N`）改的就是产物里 `#ifndef` 兜底的那个值，编译期 `-D` 仍然优先。
- **弹栈丢掉的语义值由用户清理**：写了 `%union` 时首选 `%destructor <成员> { ... }`
  （按符号绑定的成员调对应片段，见 [`%destructor`](#destructor丢弃语义值时按成员清理)）；
  没写 `%union`、或成员没登记 `%destructor` 时，C 产物退回在 `%code ... %endcode` 里定义的
  `PARSER_USER_DISCARD_VALUE(pValue)` 钩子，运行时对每个被丢掉的值调一次；两者都没有
  就不调（用户自己分配的内存会泄漏）。C++17 产物靠 RAII，没有这个钩子。`%tree` 的
  树节点由运行时自己释放。

```gram
%mode lalr
%sync SEMI                     /* 没写 error 产生式时的兜底锚点 */

stmt_list := stmt_list stmt { $$ = $2; } | stmt { $$ = $1; };

stmt := expr SEMI  { $$ = $1; }
      | error SEMI { yyerrok; $$ = 0; };   /* 坏语句整条丢掉，接着解析下一条 */
```

三个示例：[`example/error_recovery.gram`](../example/error_recovery.gram)（`error` +
`yyerrok`，`1+2; 3; 4 5; 6 7; 8;` 报 2 条错后仍解析到 8）、
[`example/sync_recovery.gram`](../example/sync_recovery.gram)（只有 `%sync`，
`1 + + 2 ; 3` 命中 `;` 锚点后继续）、
[`example/ll_follow_recovery.gram`](../example/ll_follow_recovery.gram)（LL(1) 的
FOLLOW 集恢复，什么恢复指令都没写，坏语句靠 FOLLOW 集丢掉后继续）。

## 条件编译（%if）

同一份文法按场景产出不同内容：条件来自命令行 `--define NAME[=VALUE]`，条件编译在
**解析文法之前**按行处理——条件为假的整行直接丢掉（连解析都不做），条件为真的整行
原样保留。

```text
%if DEBUG
%token <ival> TRACE        /* 调试版才有的 Token */
%endif

%if LOG && !QUIET
%define BANG "!"
%else
/* 普通版一个都不加 */
%endif

factor := NUMBER {
    $$ = $1;
%if TRACE
    IfDemoTrace($1);       /* 动作片段也能用 %if 包 */
%endif
}
        ;
```

| 条件写法 | 含义 |
| --- | --- |
| `NAME` | 变量为真 |
| `!NAME` | 取反 |
| `A && B` | 与 |
| `A \|\| B` | 或 |
| `( ... )` | 括号分组 |

优先级从低到高：`\|\|` < `&&` < `!` < 括号 < 变量名。没有「值」的概念，只有真 / 假；
`==` / `!=` / 算术 / 函数调用 / 字符串字面量都不支持。

`--define` 的取值（可重复，同名**后传覆盖先传**）：

| 写法 | 真假 |
| --- | --- |
| `--define NAME` / `--define NAME=1` / `=yes` / `=true` / `=on` / 其它非空值 | 真 |
| `--define NAME=0` / `=false` / `=no` / `=off`（大小写不敏感）、`NAME=`、不传 | 假 |

生效范围与约定：

- **文法层**：`%if` 可以包住任何文法内容（`%token` / `%define` / `%left` / `%start` /
  产生式 / 整块 `%code ... %endcode`）；**动作层**：可以写在语义动作 `{ ... }` 里。
- **不支持**：用 `%if` 决定产物清单（比如包住 `%copy` / `%tree`）；在条件里引用文法
  符号（比如「某个 Token 有没有被声明」）。
- 条件必须写在 `%if` 同一行；变量名区分大小写；不传的变量为假、不报错；
  `%code ... %endcode` 代码块对 `%if` 不透明（要条件注入就把整块包起来）。
- 指令不配对（未闭合 / 多余 `%endif` / 多个 `%else` / `%elif` 在 `%else` 之后）、
  条件表达式写错（括号不配平、运算符缺操作数、非法字符）都会在生成期报错，
  而且**行号指回原文件**（条件为假被丢掉的行不占号）。

示例 [`example/if_demo.gram`](../example/if_demo.gram)：`--define LOG` 打开一个 `!` 后缀
Token 与日志，`--define TRACE` 打开 trace（`TRACE && !QUIET` 控制），
`--define LOG=off` 与不传等价。

## 自定义入口与 epilogue（%main / %epilogue）

写用户代码有四个地方，各有各的落点：

| 放什么 | 放哪 | 为什么 |
| --- | --- | --- |
| 声明、`typedef`、宏、`static inline` 函数 | `%code ... %endcode` → `parser_user.h` | 这个头会被 `parser.c` / `parser_token.c` 一起 include，只能放重复包含无害的东西 |
| 普通函数定义、`static` 辅助函数 | `%epilogue` → `parser.c`（C++17 是 `parser.cpp`，Python 是 `parser.py`）末尾 | 只出现一次；C++17 下定义在命名空间之外（别的编译单元也能链到），正文前自动补一行 `using namespace`，正文原样保留 |
| `main` | `%main` → `main.c`（C++17 是 `main.cpp`，Python 是 `main.py` 的演示体） | 与内建 demo main 二选一，避免两份 `main` |
| 独立编译单元（自己的 `.c` / `.h`） | `%copy` | 拷进产物目录并进 `Makefile` / `CMakeLists.txt` |

反例：把非 `inline` 的函数定义塞进 `%code ... %endcode`——头文件被多个 `.c` 包含，链接时
就是多份重复定义。正例见 [`example/epilogue_demo.gram`](../example/epilogue_demo.gram)：
声明放 `%code ... %endcode`、实现在 `%epilogue`、语义动作里调用它。

Python 产物（`--lang python`）下这张表换成：`%code ... %endcode` 插
在 `parser.py` 的 `import` 之后、`%epilogue` 贴到 `parser.py` 末尾、`%main` 替换
`main.py` 的演示体（正文都按 Python 缩进规则归一化）。

```text
%code ... %endcode           /* 声明放这里 */

%epilogue             /* 函数定义贴到 parser.c 末尾 */
double MyHelper(double x) { return x * 2.0; }

%if PROD              /* 入口可以按场景切换 */
%main
int main(void) { ParserValue_T r; return Parser_RunText("1+2*3", &r); }
%endif
```

`%epilogue` / `%main` 都是**单独成行**的段标记，正文原样收集到下一个「行首是 `%`」
的行或文件末尾。要点：

### 把大段用户代码放外部文件（include）

段正文也可以从文件读，`.gram` 只留文法：

```text
%code include "demo_user.h"        /* 等价于 %code … %endcode 里贴上这个文件 */
%code include="demo_user.h"        /* 带等号的写法也认 */

%epilogue include "demo_epilogue.cpp"
%main     include "demo_main.cpp"
```

- `%code ... %endcode` 与 `%code include "…"` 可以混用，按出现顺序拼接；
- 相对路径以**文法文件所在目录**为基准（与 `%copy` 同一套规则），绝对路径原样用；
- 被 include 的文件**按原样收进产物**：不参与 `%if` 条件编译、不会被当成 `.gram`
  递归解析；文件打不开会在生成期报出解析后的完整路径；
- 报错定位仍指向 `.gram` 里的那个标记行（不是被包含文件里的行）；
- 完整例子见 `example/include_demo.gram`（`%code` / `%epilogue` / `%main` 三段都走 include，
  c 与 cpp17 两个语言档的输出一致）。

- `%epilogue` 最多一个，且**不能定义 `main`**（会报错并指路 `%main`，检测在条件编译
  之后做，`%if` 包住的 `main` 也拦得住）；
- `%main` 可以写多段（配合 `%if` / `%elif` / `%else`），但**同时生效的只能有一段**：
  0 段 → 内建 demo main，1 段 → 用它替换 `main.c` 正文，多于 1 段 → 生成期报错；
- **`%epilogue` 与 `%main` 里都按 C99 的名字调生成器函数**：C++17 下这两段都贴在
  生成的命名空间**之外**（普通 C++ 函数，别的编译单元也能链到），正文前生成器会插
  一行 `using namespace <命名空间>;`（默认 `Parser`，写了 `%namespace` 就是那个
  名字），正文本身**原样保留、一个字都不改写**。所以两边都写
  `Parser_RunText(...)`、`Parser_GetLastError()` 即可，`--lang c` 与 C++17 通用
  ——C++17 产物在命名空间里自带一层兼容层（`Parser_RunText` / `PARSER_OK` /
  `ParserError_T` 这些名字都在，底下包着一个进程级 `Engine`，**非线程安全**）；
  `%epilogue` 和 `parser.cpp` 同属一个编译单元，还能调那里的内部 helper，
  `%main` 是独立编译单元，只能用公开 API。新代码建议直接写 `Engine` /
  `engine.runText(...)`；
- 想覆盖 `main` 也可以走 `%copy file my_main.c`；两条路别同时用（两份 `main` 链接
  失败）。
- **Python 产物下这两段照旧，只是没有 `#line` 也没有命名空间**：`%epilogue`
  正文贴到 `parser.py` 末尾（和解析器类同一个模块，能直接调它的内部方法），
  `%main` 正文整段替换 `main.py` 的演示体（内建的 `_read_line()` / `main()` 与
  `if __name__` 段都在被替换之列，所以入口调用要自己写；头注释、`import parser`
  与类名别名 `Parser` 保留）。两段正文都按 Python 的缩进规则归一化（去掉公共
  缩进、行首 tab 换成 4 个空格），行首空白之外**一字不改**。

两个示例：[`example/epilogue_demo.gram`](../example/epilogue_demo.gram)（`1+2*3` → 13，
乘积被 epilogue 里的函数乘了 2）、[`example/main_demo.gram`](../example/main_demo.gram)
（`--define PROD` 换成生产入口，不传则回落到内建 demo main）。

## 报错定位（#line）

生成的代码里，凡是**来自 `.gram` 的部分**前后都有一对 `#line`：

```c
        case 3u:
        {
#line 10 "example/calc.gram"          /* 下一行属于 .gram 第 10 行 */
            result.value = stack->values[stack->top - 1u + 0u];
#line 511 "parser.c"                  /* 行号还给自己 */
            break;
        }
```

于是 `gcc -std=c99 -c parser.c`（C++17 下 `g++ -std=c++17 -c parser.cpp`）报错时看到的是
`.gram` 的行号与原文，而不是生成文件里的位置：

```text
example/calc.gram:10:28: error: 'NoSuchGlobal' undeclared (first use in this function)
   10 | expr := expr PLUS term { $$ = NoSuchGlobal; }
```

覆盖范围与约定：

| 内容 | 落点 | 前向 `#line` 指向 |
| --- | --- | --- |
| 语义动作 | `parser.c`（C++17 是 `parser.cpp`） | 动作 `{` 那一行 |
| `%code ... %endcode` | `parser_user.h` | 正文第一行 |
| `%union` 正文 | `parser_user.h`（C++17 是 `parser.hpp` 里） | `%union` 那一行 |
| `%epilogue` 正文 | `parser.c`（C++17 是 `parser.cpp`）末尾 | `%epilogue` 的下一行 |
| `%main` 正文 | `main.c`（C++17 是 `main.cpp`） | `%main` 的下一行 |

- 行号是**原文件行号**：`%if` 丢掉整行不会让行号漂移（用预处理层的行号映射表换算）；
- 恢复行的行号按产物实际行号回填，所以后续报错不会整体偏移；
- `.gram` 路径用的是你传给 `--grammar-file` 的那个路径（相对就相对），Windows 反斜杠
  会转义成 `\\`；
- **纯生成内容不带 `#line`**：`Makefile` / `CMakeLists.txt` / `RESULT.md` 里一行都没有
  （C99 产物里 `parser_table.c` / `parser_token.c` / `parser.h` 同样没有；`pl0.gram`
  这类没有语义动作的 `%tree` 示例的 `parser.c` 也没有）；
- **Python 产物用注释代替 `#line`**：Python 没有 `#line` 指令（报错只会给
  `parser.py` / `main.py` 自己的行号），所以同样的位置改成一行注释标出出处——
  见下面「Python 产物的行标记」；
- 跨行的动作只标起始行，后面的行由 C 编译器自行递增。

示例 [`example/line_demo.gram`](../example/line_demo.gram) 的注释里写了「怎么故意写错、
期望看到什么报错」；上面每一条都能用真实的 `gcc` / `g++` 报错行号核对。

### Python 产物的行标记

`--lang python` / `%lang python` 时，上面每个位置改成一行**注释**，形式是
`# 语法文件 <文法文件> 行 <N>`（指向的行号与 C 的 `#line` **完全一致**，方便两种
产物对着看）：

```python
        if production == 3:
            # 语法文件 example/calc.gram 行 10
            result.value = values[0] + values[2]
            return STATUS_OK
```

| 内容 | 落点 | 标记指向 |
| --- | --- | --- |
| 语义动作 | `parser.py` 的 `_apply_action`（LL 是展开分支） | 动作 `{` 那一行 |
| `%code ... %endcode` | `parser.py` 模块顶部（import 之后） | 正文第一行 |
| `%epilogue` | `parser.py` 末尾 | `%epilogue` 的下一行 |
| `%main` | `main.py` 演示体开头 | `%main` 的下一行 |

几点约定：

- 注释**行尾照样换行**、缩进跟着所在块走（动作里是 12 个空格），所以不影响 Python
  的缩进规则，`python -m py_compile parser.py` 照常通过；
- 路径用的是你传给 `--grammar-file` 的那个路径（相对就相对），**不做 C 的转义**
  （注释里不需要），Windows 反斜杠原样写出来；
- 它只是**给人看的导航**：Python 的 traceback 仍然报 `parser.py` 自己的行号，
  这一点和 C 的 `#line` 不同——想要精确对应就顺着注释回到 `.gram` 看。
# 别名示例

`example/alias_demo.gram` 集中演示 `%token`、`%token regex`、`%identifier`、`%commit` 和 `%trivia` 的 `alias` 写法；它是 `%mode lex` 文法，六种目标语言都列入 `example/manifest.tsv`。

# CSV 别名表示例

`example/token_dict_demo.gram` 演示 `%token NAME include "csv"`：别名列表放在
`example/token_dict_kw.csv` / `example/token_dict_num.csv` 里，一条指令追加进来。
它是 `%tree` 文法（列 `c,cpp17`），细节见上文[用 CSV 维护字面量别名表](#用-csv-维护字面量别名表)。
