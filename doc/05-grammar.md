# 02 `.gram` 文法规范

`%lang` 与 `%mode` 的可用组合见 [12 各语言支持的模式](15-language-mode-support.md)；
前端识别某种模式不代表每个后端都能生成该模式。

## 1. 词法层面的写法

- 文件是 UTF-8 文本，允许 BOM；`//` 行注释与 `/* ... */` 块注释会被跳过。
- 指令以 `%` 开头，一行一条；用户代码只能写在 `%code` 与 `%endcode` 之间，
  两个标记都必须单独成行。
- 规则有两种等价写法：

```gram
/* 写法一：一行一条，同名左部写多行 */
expr := expr PLUS term { $$ = $1 + $3; }
expr := term           { $$ = $1; }

/* 写法二：yacc 风格 | 分支，整条规则必须以 ; 结束 */
expr := expr PLUS term { $$ = $1 + $3; }
      | term           { $$ = $1; };
```

- 空产生式写 `%empty`，也可以写在 `|` 分支里：`opt := X | %empty;`。
- 动作可以带花括号 `{ ... }`，也可以写到行尾不带花括号。

## 2. 指令一览

### 模式、语言与开关

| 指令 | 作用 |
| --- | --- |
| `%start NAME` | 起始非终结符（缺省取第一条规则的左部） |
| `%example "文本"` | 为本文法声明一条示例输入，生成的 `RESULT.md` 会把它用于命令行、API 与绘图示例；最多声明一次。支持普通字符串转义（如 `\\`、`\"`、`\n`、`\t`），但拒绝无法通过命令行传递的 `\0`；未声明时显示 `<符合文法的语言>` |
| `%mode ll0 \| ll1 \| llk \| lr0 \| slr \| slrk \| lalr \| lalrk \| lr1 \| lrk \| ielr \| lex` | 生成模式；与命令行 `--mode` 不一致时以命令行为准。**都不写时缺省 `ielr`（IELR(1)，能覆盖全部 LR(1) 文法）**。`lr0` / `lr1` 是规范 LR(0) / LR(1) 自动机模式，**不需要任何开关**就能用，只是从不因未消解冲突中止。带 k 的四种（`lrk` / `lalrk` / `slrk` / `llk`）配 `%lookahead N`，也可以把 k 直接写进模式名（`LL(2)`），见下面「k 写在哪儿」一节。`lex` 只导出词法器（可以一条产生式都不写，产物里没有解析器），见 §6 |
| `%lookahead N`（等价命令行 `--lookahead N`） | 展望符长度 k：N 取 **0..INT_MAX**、缺省 **2**——**没有策略上限，大 k 代价自负**（见下面「k 的取值」），只对 `ll0` / `llk` / `lrk` / `lalrk` / `slrk` 有意义（N = 0 只有 LL 家族有意义，就是 LL(0)），写成别的模式直接报错。N = 1 时这几种分别等价于 `ll1` / `ll1` / `lr1` / `lalr` / `slr` |
| `%teach`（等价命令行 `--teach` / `--no-teach`） | 打开教学模式：`slr` / `lalr` / `ielr` 的未消解冲突只报不中止，报告多出 `[教学提示]` / `[教学对照]`；缺省**关闭**，`--no-teach` 可覆盖文法里的 `%teach`；`ll1` 下被忽略并提示一句。与 `%mermaid` 相互独立。见 [`06-analysis.md`](06-analysis.md) §5 |
| `%mermaid`（等价命令行 `--mermaid` / `--no-mermaid`） | 生成 [Mermaid 状态机图](https://mermaid.js.org/)：产物里多出 `mermaid/` 目录（文法 LR 自动机 + 词法 DFA）；缺省**关闭**，`--no-mermaid` 可覆盖文法里的 `%mermaid`；与教学模式无关。见 [`07-codegen.md`](07-codegen.md) |
| `%lang c \| cpp17 \| python \| csharp \| rust \| go \| gram` | 产物语言；与命令行 `--lang` 不一致时以命令行为准，都缺省时取 `cpp17`。`python` 出免编译的 `parser.py` / `main.py`（见下面的 `%class` 一节），`csharp` 出 `Parser.cs` / `Token.cs` / `Program.cs` / `parser.csproj`，`rust` 出 Cargo 工程 `Cargo.toml` + `src/parser.rs` / `src/token.rs` / `src/main.rs`，`go` 出 Go 模块 `go.mod` + `parser.go` / `token.go` / `main.go`（三者都见下面的 `%package` / `%import` / `%class_body` 一节）；`gram` 是**文法导出档**：不生成任何代码，只把 `.gram` 原文件与分析报告 `RESULT.md` 导出到 `--out-dir`（见 [`14-languages-and-products.md`](14-languages-and-products.md#文法导出--lang-gram)） |
| `%namespace NAME` | C++17 产物的命名空间名与文件名前缀（命令行 `--namespace` 优先）。NAME 取 `[A-Za-z_][A-Za-z0-9_]*`、不能是 C++ 关键字、不支持 `::`；不写时用默认 `Parser` + 不带前缀的文件名 |
| `%prefix NAME` | C99 公开符号前缀（命令行 `--prefix` 优先）；不能以 `Parser` / `PARSER` 开头；只对 `--lang c` 生效，其它语言提示并忽略 |
| `%class NAME` | Python 产物的解析器类名（命令行 `--class` 优先；不写是 `Parser`）。NAME 取 `[A-Za-z_][A-Za-z0-9_]*`、不能是 Python 关键字；只对 `--lang python` 有效，配 C / C++17 直接报错 |
| `%package NAME` | C# 产物：生成文件级 `namespace NAME;`；Rust 产物：`Cargo.toml` 的 crate 名（`.` 换 `_`）；Go 产物：`go.mod` 的 module path（`::` 归一成 `/`）。NAME 取分段标识符（`[A-Za-z_][A-Za-z0-9_]*` 用 `.` 或 `::` 连接，例如 `MyApp.Compiler`）、最多写一次；只对 `--lang csharp` / `rust` / `go` 有效，别的目标语言下提示一句并忽略。见下面「`%package` / `%import` / `%class_body`」一节 |
| `%import NAME` | C# 产物：生成 `using NAME;`（一律排在 `namespace` 之前）；Rust 产物：`use NAME;`；Go 产物：进 `import ( … )` 块（`::` 当归一成 `/` 的路径分隔符，与标准库重复的包会被跳过；带引号的写法原样贴）。可重复；NAME 取分段标识符（`.` 或 `::` 连接）、末段可以是 `*`（如 `System.Collections.*`）；只对 csharp / rust / go 有效，别处提示并忽略。见下面「`%package` / `%import` / `%class_body`」一节 |
| `%class_body` … `%endbody` | 段标记：两个标记各自单独成行，之间的正文**原样**插进解析器类内部（C# 放辅助方法 / 字段 / 内部类）、`impl Parser { … }`（Rust）或解析器代码之后的一组 `func (p *Parser) …` 方法（Go）。只对 csharp / rust / go 有效；别处提示并忽略。见下面「`%package` / `%import` / `%class_body`」一节 |

### 语义值、Token 与词法

| 指令 | 作用 |
| --- | --- |
| `%value_type 类型` | 声明语义值类型（如 `%value_type double`、`%value_type struct AstNode *`）：生成器往 `parser_user.h` 追加 `typedef <类型> ParserValue_T;`，最多写一次；与 `%union` 二选一。见 §2.1 |
| `%union { ... }` | 声明语义值联合体：成员声明**原样**贴进产物 `parser_user.h` 的 `typedef union { ... } ParserValue_T;`，最多写一次。见下节与 [`07-codegen.md`](07-codegen.md) §4.1 |
| `%type <成员> A B` | 给非终结符绑定 `%union` 成员（一个指令可带多个符号） |
| `%token <成员> A B` / `%token <成员> NAME "字面量"` / `%token <成员> regex NAME "正则"` | 给 Token 绑定 `%union` 成员；标签作用于后面**所有**符号，直到出现下一个标签 |
| `%destructor <成员> { ... }` | 给 `%union` 成员登记清理代码：该成员的值被丢弃（错误恢复弹栈 / 解析失败收尾清栈）时执行，片段里的 `$$` 就是那个值。见下面「`%destructor`」一节 |
| `%define NAME "字面量"` | 固定字符串 Token；`\|` 分隔多个候选，`\|` 转义为普通竖线 |
| `%space "空白"` | 取 Token 前跳过的空白字符集（缺省 `"\b"`，即全部空白）；`\b` 在这里代表全部空白，`%space ""` 表示一个都不跳过。见下面的「空白 / 换行当 Token」一节 |
| `%token A B C` | 声明没有固定字面量的 Token |
| `%token A B C [alias "显示名"]` | 声明没有固定字面量的 Token；别名应用到紧邻的 Token 声明 |
| `%token NAME "..." [alias "显示名"]` | 等价于 `%define`（不带 `regex` 时只拆字面量）；`alias` 只改变显示名称，不改变 Token 身份 |
| `%token NAME include "词表.csv" [alias "显示名"]` | **追加**字面量别名：CSV 里第一列等于 `NAME` 的那些行，第二列作为字面量追加到 `NAME` 的别名列表。只追加别名、不新建 Token；第二列一律当**字面量**（不编译正则）；带 `<成员>` 也能用。见下面「用 CSV 维护字面量别名表」一节 |
| `%token regex NAME "正则" [alias "显示名"]` | 正则 Token 的可选显示别名；正则匹配规则和 Token 身份保持不变 |
| `%token regex NAME "正则"` | 用正则定义 Token，生成期编译成 DFA |
| `%identifier "正则"` | 声明标识符正则，生成标识符识别与关键字查表接口 |
| `%identifier "正则" [alias "显示名"]` | 声明标识符正则；别名显示在内建 `VARIABLE` 名称上 |
| `%commit [类型] "模式" [alias "显示名"]` / `%commit skip "模式"` | 把「识别但不进语法层」的文本收进容器（元素 `{line, type, text}`）；不写类型时是 `"default"`，`skip` 表示匹配即丢、不进容器（**等价于 `%trivia`，推荐用 `%trivia`**）。`alias` 只用于类型表和诊断显示，不改变 `type` 字段。模式按 DFA 正则编译（没有元字符的写法就是固定字面量）；与 `%token` / 内建注释规则同一份最长匹配顺序，同长度按声明先后。见下面「`%commit`」一节 |
| `%trivia "模式" [alias "显示名"]` | 匹配即丢：不进容器、不进语法层。与 `%commit skip "模式"` **完全等价**（两种写法生成的词法表逐字相同），别名只记录在类型表中；语义更清楚，推荐用它声明 trivia。模式与排序规则同 `%commit` |

### 运行时限制、恢复与用户代码

| 指令 | 作用 |
| --- | --- |
| `%max_stack_depth N` | 分析栈深度上限 `PARSER_MAX_STACK_DEPTH`，N 取 1..65535、缺省 64（栈下标是 `uint16_t`，再大没意义） |
| `%max_errors N` | 一次解析最多保留几条错 `PARSER_MAX_ERRORS`，N 取 1..4096、缺省 8 |
| `%error_recovery_tokens N` | 错误恢复后静默几个 Token `PARSER_ERROR_RECOVERY_TOKENS`，N 取 0..4096、缺省 3；0 表示恢复后立刻报下一个错 |
| `%error_context`（等价命令行 `-DPARSER_ERROR_CONTEXT`） | 让错误记录里多一段「出错那一行原文 + `^` 指示」（`ParserError_T::context`，长度 `PARSER_ERROR_CONTEXT_LENGTH`，缺省 120）。**缺省关闭**——这是纯内存开销，而且只有文本模式（`Parser_RunText`）能拿到源文。见 [`08-generated-code.md`](08-generated-code.md) |
| `%epilogue` | 段标记：之后的正文原样贴到生成的 `parser.c`（C++17 是 `parser.cpp`，Python 是 `parser.py`）末尾 |
| `%main` | 段标记：之后的正文替换产物 `main.c`（C++17 是 `main.cpp`，Python 是 `main.py` 的演示体）里的内建 demo main |
| `%cpp_only` | 声明「这份文法只能生成 C++17 产物」：命令行再写 `--lang c` / `--lang python` / `--lang csharp` / `--lang rust` / `--lang go` 会直接报错（`%lang` 里写那几个也一样）。文法依赖 C++17 产物才有的东西（`Engine`、`std::string_view`、兼容层）时用它提前拦住误用 |
| `%cpp_lexer std\|dfa` | C++17 产物的词法引擎，等价命令行 `--cpp-lexer`（命令行优先）。`std`（**缺省**）把 `%token regex` / `%identifier` / `%commit` 的正则交给 `std::regex`，产物行为与标准库一致；`dfa` 在生成期把它们编译成 DFA 表写进产物，产物不依赖 `<regex>`，每个位置只扫一遍输入。配非 `--lang cpp17` 的目标时，写指令报错、写命令行只提示并忽略 |
| `%tree` | 生成 CST 接口，纯 `%tree` 文法可以不写语义动作 |
| `%left` / `%right` / `%nonassoc` | 声明 Token 结合性与优先级，行越靠下优先级越高 |
| `%prec TOKEN` | 写在分支右部末尾，覆盖该产生式的优先级来源 |
| `%expect N` | 允许保留 N 个未消解的移进/归约冲突 |
| `%sync TOKEN...` | 声明 panic mode 的恢复锚点 Token；LR 家族在 `error` 产生式找不到恢复点时兜底；LL 家族本来就靠 FOLLOW 集恢复，它只是额外汇进来的锚点（`ll0` 用不上）。见 §3.2 |
| `%copy [file\|dir] 路径` | 把文法目录下的文件/目录拷进产物目录，缺省 `file` |
| `%include "路径"` | 文件级包含：把被引 `.gram` 的内容**内联**进来（也接受 `%include='路径'` 与单引号，必须单独成行）。相对路径按包含它的文件所在目录解析，支持递归与循环检测；`%code` / `%class_body` / `%epilogue` / `%main` 段内不展开。内联后报错行号指向拼接后的文本 |
| `%build_files on\|off` | 是否导出工程构建文件（`Makefile` / `CMakeLists.txt`），等价命令行 `--build-files` / `--no-build-files`（命令行优先），只能声明一次 |
| `%code` … `%endcode` | 用户代码块：`%code` 与 `%endcode` 各自单独成行，中间内容原样拼进 `parser_user.h`；可写多段，按出现顺序拼接。旧 `%{{ ... }}%` 语法已禁用 |
| `%if` / `%elif` / `%else` / `%endif` | 条件编译（由预处理层处理，不是解析器的指令）：条件来自命令行 `--define`，条件为假的整行丢掉。见下节 |

### `%value_type`：一行声明语义值类型

```text
%value_type double                 /* → typedef double ParserValue_T; */
%value_type struct AstNode *       /* 任意 C 类型：指针、结构体、联合体都行 */
%value_type uint32_t
```

类型原文原样贴进产物（`%code` 用户代码之后，所以可以引用用户代码里定义的
结构体），生成期不做任何解析，交给 C 编译器。限定条件：

- 最多写一次；与 `%union` 不能同时写（两者都在定义 `ParserValue_T`）。
- 写了 `%value_type` 时，`%code` 里不能再定义 `ParserValue_T`。
- 都不写、又没写 `%code` 时，生成器给一个
  `typedef int ParserValue_T;` 兜底；写了用户代码就由用户自己给，生成器不再补
  ——C99 与 C++17 一致（C++17 产物的 `parser_user.h` 在打开命名空间之前包含，
  所以用户给的类型在命名空间里同样可见）。

### `%union` / `%type` / `%token <成员>` 的写法

```text
%union {
    int    ival;
    double dval;
    char  *sval;
    struct { int x; float y; } point;      /* 嵌套 struct 定义 */
    union  { int i; double d; } num;       /* 嵌套 union 定义 */
    int  (*callback)(int, char *);         /* 函数指针成员 */
    int    arr[16];                        /* 数组成员 */
    const char *name;
}

%type  <ival> expr term
%type  <sval> name
%token <ival> NUMBER
%token <sval> regex KEYWORD "[a-z]+"
```

生成期**不解析 C 类型**，只做三件事，剩下的交给 C 编译器：

1. **括号配平找最外层 `}`**：从 `%union` 后面第一个 `{` 开始，同时维护
   `{}` / `[]` / `()` 三种括号的深度，三种深度都为 0 时遇到的第一个 `}` 才是
   `%union` 的结束标记——所以 `struct { int x; } p;` 里那个 `}` 不会被当成结尾。
2. **只在顶层按 `;` 切分成员段落**（同样按三种括号配平），因此
   `int (*fn)(int, char *);` 里的 `;` 不会切段。**顶层逗号直接报错**：一段只能
   有一个声明符，`int a, b;` 要拆成两行写。
3. **从段落尾部提取成员名**：先按需剥掉尾部的 `[...]`（数组）、`(...)`（函数
   指针的参数列表）与 `: 位宽`（位域），再向前取一段连续标识符。看不出来就报错
   并附上出问题的段落原文，提示先用 `typedef` 起个名字再作为成员类型。

因此成员声明可以随便写：嵌套 `struct` / `union` / `enum` 定义、函数指针成员、
数组成员、位域、`const` 指针都行。其它规则：

- `%union` 最多一次，成员按声明顺序保存。
- 终结符与非终结符**共用一张「符号名 → 成员名」的全局映射**（yacc 语义：
  `%type` 是全局的，同一个符号不能在不同产生式里用不同成员）。
- 成员名重复、同一符号绑到两个成员、`%type` / `%token <成员>` 引用了未声明的
  成员，都在生成期报错；写了 `%union` 时用户 `%code` 里不允许再定义
  `ParserValue_T`（这个类型由生成器追加）。
- 想让所有符号共用同一个类型，用 `%value_type` 一行声明就够了（见 §2.1）；
  `%union` 是「每个符号一种 C 类型」的进阶写法。
- `%union` 可以引用用户 `%code` 里定义的类型（生成器把联合体追加在用户代码
  之后）；完整示例见 `example/union_demo.gram`。
- 写了 `%union` 时 `ParserToken_T.value` 本身也是 `ParserValue_T`，词法器按
  `%token <成员>` 把词素直接写进对应成员（数字写解析出的数值，双引号字符串、
  标识符与 `%token regex` 写匹配到的文本），所以终结符的语义值通常不用再写
  `PARSER_USER_TOKEN_VALUE`。细节见 [`07-codegen.md`](07-codegen.md) §4.1 与
  [`08-generated-code.md`](08-generated-code.md) §9。
- 漏绑成员的终结符词法器不写语义值（联合体里没地方放），`value` 保持零值；内建
  `NUMBER` 漏绑时（写了 `%union` 却没写 `%token <成员> NUMBER`，而文法里又用到
  `NUMBER`）生成期会补一条 `警告：`，产物照常能编译。

### `%destructor`：按成员登记清理代码

```text
%union {
    char        *str;
    struct Node *node;
    int          ival;
}
%type  <str>  list item
%token <node> VAR

%destructor <str>  { free($$); }
%destructor <node> { Node_Free($$); }
```

语义值的内存归用户所有，运行时不会替你猜「哪个成员需要释放」——`%destructor`
就是把这个信息告诉它：**按 `%union` 成员**登记一段清理代码，丢弃该成员的值时执行。

写法与校验：

- 片段是 `{ ... }` 里原样收集的 C 语句（与 `%union` 一样只做括号配平，不解析
  C）；片段里只认 `$$`，展开成「正在被丢弃的那个值」（C 产物 `pValue->成员`，
  C++17 产物同样）。写 `$1` / `$line` 这类需要右部符号的写法在生成期报错。
- `%destructor` 需要先写 `%union`；`<成员>` 必须是 `%union` 里声明过的成员；
  同一个成员只能写一条。三条违反都在生成期报错并指回原文件行号。
- `%lang python` / `rust` / `go` 下写 `%destructor` 直接报错：Python 的语义值靠引用
  计数回收、Rust 靠所有权、Go 靠 GC，都没有「丢弃时调清理代码」这一层。LL(0) /
  LL(1) / LL(k) 与 LR 家族都支持。
- 没写 `%destructor` 的成员保持原样：C 产物仍然对每个被丢掉的值调
  `PARSER_USER_DISCARD_VALUE` 钩子（老写法不受影响），C++17 产物没有这个钩子。

什么时候调（与 bison 同规矩）：

| 时机 | 说明 |
| --- | --- |
| 错误恢复弹栈 | `%sync` / `error` 产生式恢复时被弹掉的槽位；LL 家族是弹掉的语义帧槽位 |
| 解析失败收尾 | 返回失败前把还留在栈上的语义值逐项丢弃 |
| 成功解析 | **不清栈**：起始符号的结果交还调用方，结果里的指针归调用方所有 |

运行时只在「真的写过值」的槽位上调 `%destructor`（LL 的语义帧会记录每个槽位属于
哪个文法符号），所以不会对没初始化的槽位执行 `free` 之类的代码。

完整示例（带计数器自检）见 `example/destructor_demo.gram`；产物形态见
[`08-generated-code.md`](08-generated-code.md) §9.1。

### `%if` / `%elif` / `%else` / `%endif`：条件编译

同一份文法按场景产出不同内容，条件来自命令行 `--define NAME[=VALUE]`，不传的变量
一律为假。条件编译在**解析文法之前**由预处理层按行处理：条件为假的整行丢掉，条件
为真的整行原样保留（所以丢掉的行不占原文件行号）。

```text
%if DEBUG
%token <ival> TRACE        /* 调试版才有的 Token */
%endif

%if LOG && !QUIET
%define BANG "!"
%elif TRACE
%define BANG "!"
%else
/* 普通版一个都不加 */
%endif
```

条件表达式（只有这四个运算符与括号，没有「值」的概念，只有真 / 假）：

| 写法 | 含义 |
| --- | --- |
| `NAME` | 变量为真 |
| `!NAME` | 取反 |
| `A && B` | 与 |
| `A \|\| B` | 或 |
| `( ... )` | 括号分组 |

优先级从低到高：`\|\|` < `&&` < `!` < 括号 < 变量名。`==` / `!=` / `<` / 算术 /
函数调用 / 字符串字面量都**不支持**（`%if A == B` 报「条件表达式里有非法字符 '='」）。

`--define` 的取值（可重复，同名**后传覆盖先传**）：

| 写法 | 真假 |
| --- | --- |
| `--define NAME` | 真（等价 `NAME=1`） |
| `--define NAME=1` / `=yes` / `=true` / `=on` / 其它非空值 | 真 |
| `--define NAME=0` / `=false` / `=no` / `=off`（大小写不敏感） | 假 |
| `--define NAME=`（空值）或干脆不传 | 假 |

生效范围：

- **文法层**：`%if` 可以包住任何文法内容——`%token` / `%define` / `%left` /
  `%start` / 产生式 / 整块 `%code ... %endcode`；
- **动作层**：`%if` 可以写在语义动作 `{ ... }` 里，决定哪几行 C 代码进产物；
- **不支持**：用 `%if` 决定产物清单（比如包住 `%copy` / `%tree`）、在条件里引用
  文法符号（比如「某个 Token 有没有被声明」）。

边界情况（都会报错，且行号指回**原文件**）：

- `%if` 没有 `%endif`（含嵌套 `%if` 不配对）、`%endif` 没有对应的 `%if`；
- `%elif` / `%else` 出现在 `%if` 之外、一个 `%if` 块里多个 `%else`、`%elif` 出现在
  `%else` 之后；
- 条件表达式语法错误：括号不配平、运算符缺操作数、非法字符；
- `%if` 后面没写条件；`%else` / `%endif` 后面多写了内容；
- `%ifdef` / `%ifndef` 不支持，请写 `%if NAME` / `%if !NAME`。

另外几条约定：

- 条件必须写在 `%if` **同一行**（跨行请用嵌套 `%if`）；
- `%code ... %endcode` 代码块对 `%if` 是**不透明**的（里面的 `%if` 当普通代码看），
  要按条件注入用户代码得把整块 `%code` 包在 `%if` 里；
- 注释（`//`、`/* */`）与字符串字面量里的 `%if` 不当指令；
- 变量名**区分大小写**（`DEBUG` 与 `debug` 是两个变量），引用不到的变量为假、不报错；
- 条件为假而没进产物的那段代码，出错时 C 编译器也看不到它——这是「按场景裁剪」的
  正常现象。

示例：`example/if_demo.gram`。只想看预处理结果的话可以加
`--preprocess-only`（不生成产物，打印处理后的文本与行号映射）。

### `%code` … `%endcode`：用户代码块

```text
%code
typedef struct MyNode MyNode;
#define PARSER_USER_TOKEN_VALUE(pToken) ((ParserValue_T)((pToken)->value))
%endcode
```

`%code ... %endcode` 是当前唯一的用户代码块语法：正文进产物 `parser_user.h`
（C++17 下是 `<前缀>_parser_user.h`），参与同样的校验（`%value_type` / `%union` 与
「用户代码里自己 typedef `ParserValue_T`」的冲突检测）、同样带 `#line`（前向指向正文首行）。

规则：

- `%code` 与 `%endcode` 都**单独成行**（前后只允许空白），正文从 `%code` 的下一行开始、
  到 `%endcode` 那一行为止，**原样收集**（不解析、不转义、不用花括号包——`%code { … }`
  这种写法不支持：花括号会和用户 C 代码里的花括号纠缠）；
- 可以写多段，按出现顺序拼进 `parser_user.h`；
- `%code` 段对**条件编译层是不透明区**：段里的 `%if` / `%endif`
  只是普通用户代码，不会被当成指令；
- 错误分支：`%code` 缺少 `%endcode`（报 `%code` 那一行）、`%endcode` 没有对应的
  `%code`（报 `%endcode` 那一行）。

可以放声明、`typedef`、宏、`static inline` 函数；非 `inline`
的函数定义请用 `%epilogue`（`parser_user.h` 会被多个 `.c` include，重复定义会在链接期炸）。
示例：`example/code_demo.gram`。

### `%namespace`：C++17 产物的命名空间与文件名前缀

```text
%namespace myapp
```

C++17 产物（`--lang cpp17`）的命名空间名与文件名前缀都用这个名字：

| 默认（不写） | 写了 `%namespace myapp` |
| --- | --- |
| `namespace Parser { ... }` | `namespace myapp { ... }` |
| `Parser::Engine::runText` / `Parser::Token` | `myapp::Engine::runText` / `myapp::Token` |
| `parser.hpp` / `parser.cpp` | `myapp_parser.hpp` / `myapp_parser.cpp` |
| `main.cpp` / `parser_user.h` | `myapp_main.cpp` / `myapp_parser_user.h` |
| main.cpp 里 `using namespace Parser;` | `using namespace myapp;` |

规则：

- 名字必须匹配 `[A-Za-z_][A-Za-z0-9_]*`、不能是 C++ 关键字、**不接受多级**
  （`%namespace my::lang` 报错）；
- 命令行 `--namespace NAME` 覆盖 `%namespace`（与 `--mode` 覆盖 `%mode` 一致，不报错）；
- **空名字 = 退化成默认**：不写 `%namespace`、不传 `--namespace`（或传空串）时，
  命名空间是 `Parser`、文件名不带前缀；
- `Makefile` / `CMakeLists.txt` / `RESULT.md` 不加前缀（构建脚本与文档）；
  `%copy` 拷进来的文件保持原名；`libparser.a` 与 `parser_demo` 这两个目标名也不变；
- 头文件保护宏按前缀生成（`ALPHA_PARSER_H`），所以两份产物的头可以同时 include；
  生成代码里的 `#include "…"` 与 `#line` 的恢复行都用带前缀的名字；
- **C99 产物用不了它**：`%namespace` 是 C++17 专属（C 产物没有命名空间、文件名
  也不带前缀），所以「写了 `%namespace` 或传了 `--namespace`」再配 `--lang c` 会
  直接报错，而不是静默忽略；`--lang python` 同样报错（Python 里改的类名用 `%class`）。

用途是多实例共存：C++ 下同时链接两个生成器产物时，命名空间与文件名都不再撞车
（细节与注意事项见 [`14-languages-and-products.md`](14-languages-and-products.md)
的「换掉命名空间与文件名前缀」一节）。示例：
`example/namespace_demo.gram`。

### `%class`：Python 产物的解析器类名（`--lang python`）

```text
%lang python
%class PyCalc
```

Python 产物只有 `parser.py` / `main.py` 两个文件：`parser.py` 里是模块级常量
（状态码、Token、`Tree`、`ParserError`）加一个普通 Python 类，类名就取 `%class`
（不写是 `Parser`）；`main.py` 里 `import parser` 之后用 `Parser = parser.<类名>`
这个别名，所以演示正文换了类名也不用改。

规则：

- 名字必须匹配 `[A-Za-z_][A-Za-z0-9_]*`、**不能是 Python 关键字**（`%class class`
  报「不能用 Python 关键字」）；
- 命令行 `--class NAME` 覆盖 `%class`（与 `--mode` 覆盖 `%mode` 一致，不报错），
  空串等于没写（退回 `Parser`）；
- **只对 Python 产物有效**：C / C++17 产物的接口名字是固定的（要改命名空间用
  `%namespace` / `--namespace`），所以配 `--lang c` / `--lang cpp17` 会直接报错；
- 产物文件名与类名无关，永远是 `parser.py` / `main.py`。

用户代码在 Python 产物里的落点（与 C / C++17 一样是「四个地方」）：

| 放什么 | 放哪 |
| --- | --- |
| 辅助函数、常量、类 | `%code` … `%endcode`：插在 `parser.py` 的 `import` 之后、生成的常量之前，按出现顺序拼接 |
| 更多辅助函数 | `%epilogue`：追加到 `parser.py` 末尾（与解析器类同一个模块，能直接调它的内部方法） |
| 自己的入口 | `%main`：整段替换 `main.py` 的演示体（头注释、`import parser` 与类名别名 `Parser` 保留） |
| 独立文件 | `%copy`：原样拷进产物目录（Python 里要用自己 `import`） |

插进 Python 模块的正文按 Python 的缩进规则**归一化**（`%code` /
`%epilogue` / `%main` 都做）：先去掉首尾空行，再按所有非空行的**最小前导空白**
去掉公共缩进，最后把剩下的行首 tab 换成 4 个空格。所以照文法排版缩进着写也不会
变成 `IndentationError`，行首空白之外的字符一个都不改。

Python 产物**不支持**这些 C 专属写法，写了在生成期直接报错（不静默忽略）：

- `%value_type` / `%union` / `%type <成员>` / `%token <成员>`——Python 的语义值是
  任意对象，没有「联合体成员」这回事；
- `%namespace` / `--namespace`（见上一节）。

其余指令都能用：`%mode`（十六种都支持）、`%start`、`%define` / `%token` /
`%token regex` / `%identifier`、`%tree`、`%sync`、`error` 产生式与
`yyerrok` / `yyclearin`、`%max_*`、`%expect`、`%left` /
`%right` / `%nonassoc` / `%prec`、`%teach`、`%mermaid`、`%copy`、`%if`、
`%main` / `%epilogue`、`%lookahead`。示例：`example/python_demo.gram`。

### `%package` / `%import` / `%class_body`：C#、Rust 与 Go 产物专属

这三个指令只对 `--lang csharp` / `--lang rust` / `--lang go` 生效（C# 落成
`namespace NAME;` / `using NAME;` / 类内部成员，Rust 落成 crate 名 / `use NAME;` /
`impl` 里的东西，Go 落成 module path / import 块 / 一组 `func (p *Parser) …` 方法）；
别的目标语言下生成期给一句「只对 --lang csharp / rust / go 有意义，已忽略」的提示，
然后照常生成（不会中止，也不静默变脸）。

```text
%package MyApp.Compiler          /* C#: namespace MyApp.Compiler; */
%import System.Collections       /* C#: using System.Collections; */
%import System.Collections.*     /* 末段可以是 * */

%class_body                      /* 正文原样插进解析器类内部 */
    /** 辅助方法：把一个语义值翻倍（这里写目标语言自己的代码） */
    public double doubled()
    {
        result();                /* Parser.cs 里的公开入口 */
        return 0.0;
    }
%endbody
```

| 指令 | 规则 |
| --- | --- |
| `%package NAME` | NAME 是分段标识符（`a`、`a.b.c`、`a::b`），空 / 非法直接报错；同一个文法只能写一次。产物里是**文件级** `namespace NAME;`（写在 `using` 之后） |
| `%import NAME` | 可重复，按出现顺序生成 `using NAME;`；NAME 是分段标识符（`.` 或 `::` 连接），末段允许是 `*`。`using` 一律排在 `namespace` 声明之前（C# 的语法要求） |
| `%class_body` … `%endbody` | 与 `%code` … `%endcode` 同一套写法：两个标记**各自单独成行**，之间的正文不解析、不转义、不配平花括号，原样插到解析器类内部（放辅助方法 / 字段 / 内部类）；只允许一个 `%class_body`，缺 `%endbody` 或多余的 `%endbody` 都在生成期报错 |

### `%lookahead` 与 k：LR(k) / LALR(k) / SLR(k) / LL(k) / LL(0)

```text
%mode lrk            /* 规范 LR(k)：不合并同心状态 */
%lookahead 2         /* k = 2；不写时缺省也是 2 */
```

**k 写在哪儿都行。** 模式名大小写随意，也可以把 k 直接写进括号或贴在后面
（命令行 `--mode` 与文法里 `%mode` 共用同一套写法；写 k 就不用再写 `%lookahead`）：

| 写法 | 等价于 |
| --- | --- |
| `LL(0)` / `LL0` / `ll0` | LL(0)：不看 lookahead |
| `LL(1)` / `LL1` / `ll1` | LL(1) |
| `LL(k)` / `LLK` / `llk` | LL(k)，k 由 `%lookahead` / `--lookahead` 给（缺省 2） |
| `LL(2)` / `LL2`（`LL3` 同理） | LL(k) 且 k = 2（/3） |
| `SLR` / `SLR(1)` / `slr` | SLR(1) |
| `SLR(k)` / `SLRK` / `slrk`、`SLR(2)` / `SLR2` | SLR(k)，k 同上 / 写死 2 |
| `LALR` / `LALR(1)`、`LALR(k)` / `LALRK`、`LALR(2)` / `LALR2` | 同理 |
| `LR(0)` / `LR0`、`LR(1)` / `LR1`、`LR(k)` / `LRK`、`LR(2)` / `LR2` | 同理（`LR(0)` 就是 `lr0`，`LR(1)` 就是 `lr1`） |
| `IELR` / `IELR(1)` / `ielr` | IELR(1) |

> 命令行里带括号的写法在 shell 里要加引号：`--mode 'LL(2)'`。

**k 的取值。** `%lookahead N` 取 **0..INT_MAX**、缺省 **2**——**没有任何策略上限**，
只有一条「编码放不下就报错」的物理下限：

- **N = 1**：`lrk` / `lalrk` / `slrk` / `llk` 分别等价于 `lr1` / `lalr` / `slr` /
  `ll1`（分析结果与产物都一样，只有 `[生成模式]` 标题与虚拟起始符号名不同）。
- **N = 0**：只有 LL 家族有意义，就是 **LL(0)**——不看任何 lookahead，所以每个
  非终结符只能有一条产生式。
- **没有上限**：k 想写多大都行（0..INT_MAX）。但两个代价要知道：
  1. `FIRST_k` / `FOLLOW_k` 的集合大小与 k 元组列数都随 k **指数级膨胀**——
     k = 3 以上基本只有教学或玩具文法撑得住。k > 3 时生成期会提醒一句，
     但**不会拦你**；
  2. 列编码是 $\sum_i \mathrm{Token号}_i\cdot(\text{Token 个数})^i$，`Token 个数^k`
     放不进 64 位时产物会算错，所以生成期会**报错并给出这个文法能用的最大 k**
     （`--lookahead 1000000000` 会告诉你「这个文法最多 k = 15」这种）。这是编码
     宽度的限制，不是我们给 k 定的策略上限。
- 表的大小按**用到的 k 元组个数**走，不是 `Token 数^k`（但上一条的两项仍然指数级）。
- `%lookahead` 配别的模式（含 `ll1` / `lalr` …）**直接报错**，不静默忽略。
- 命令行 `--lookahead N` 覆盖 `%lookahead`（与 `--mode` 覆盖 `%mode` 一致）。
- 模式名里写了 k、又写 `%lookahead` 且两者不一致 → 报错（选一处写就够了）。

**四种带 k 的模式。** 它们两两对应「LR 侧 / LL 侧」两条路线：

| 模式 | 分析 | 展望符从哪来 | k = 1 时等价于 |
| --- | --- | --- | --- |
| `lrk` | 规范 LR(k) 项集（不合并同心状态） | 项自带的 k 元组 | `lr1` |
| `lalrk` | 同上的项集 + 同心合并 | 合并后的 k 元组集合 | `lalr` |
| `slrk` | LR(0) 项集 | **FOLLOW_k（只看产生式左部）** | `slr` |
| `llk` | 自顶向下预测分析（含左递归消除） | **SELECT_k(A→α) = FIRST_k(α·FOLLOW_k(A))** | `ll1` |

`lrk` / `lalrk` / `slrk` 都是自底向上、按 k 元组展望符决定归约（三者的表与运行时
共用一份），`llk` 是自顶向下、按 k 个 Token 决定展开哪条产生式。**SLR(k) 严格弱于
LR(k)**：它归约时只看左部的 FOLLOW_k，不看上下文——`example/lrk_demo.gram` 上
SLR(1) / SLR(2) / SLR(3) 都是 2 个冲突，LR(2) 是 0 个（FOLLOW_k(X) 与 FOLLOW_k(Y)
恰好相等，加长 k 也分不开）。

**LL(0)。** 零个展望符推不出任何选择信息，所以 `M[A, ε]` 上只能有一条产生式：
LL(0) 文法就是「每个非终结符恰好一条产生式」，整个语言只剩一条串。它是
LL(1) ⊆ LL(2) ⊆ … 这条链的起点，写成 `%mode LL(0)` / `%mode ll0` /
`%lookahead 0`（配 LL 家族）都行，示例见 `example/ll0_demo.gram`。

什么时候值得用：**LALR(1) 有冲突、而 LR(k) 没有**的文法。经典例子是
`example/lrk_demo.gram`（`S -> A X C | A Y D | B Y C | B X D; X -> E; Y -> E`）：

| 模式 | 未消解冲突 |
| --- | --- |
| `%mode lr0` | 9 |
| `%mode slr` / `%mode lalr` | 2 |
| `%mode lr1` / `%mode ielr` / `%mode lrk`（k = 1/2/3） | **0** |
| `%mode lalrk`（k = 1/2/3） | 2（这个文法对任何 k 都不是 LALR(k)） |

最后一行值得记住：**LALR(k) 严格弱于 LR(k)**。同心合并丢掉的区分度，加长 k 也补不
回来——那份信息在合并那一刻就没了。要解掉就别合并状态（`lrk`），或让 IELR 按需
拆分（`ielr`）；自顶向下那条路可以试 `llk`（示例 `example/llk_demo.gram`：一份
LL(2) 而不是 LL(1) 的文法）。分析层的构造见 [`06-analysis.md`](06-analysis.md)
§2.12 / §2.13，产物里多出来的那一层表见 [`07-codegen.md`](07-codegen.md)。

### 容量宏：`%max_stack_depth` / `%max_errors` / `%error_recovery_tokens`

三个容量上限既能在文法里定，也能在编译期用 `-D` 覆盖：

```text
%max_stack_depth 256        /* PARSER_MAX_STACK_DEPTH = 256 */
%max_errors 16              /* PARSER_MAX_ERRORS = 16 */
%error_recovery_tokens 0    /* PARSER_ERROR_RECOVERY_TOKENS = 0：恢复后立刻报下一个错 */
```

| 指令 | 宏 | 取值范围 | 缺省 | 作用 |
| --- | --- | --- | --- | --- |
| `%max_stack_depth N` | `PARSER_MAX_STACK_DEPTH` | 1..65535 | 64 | 分析栈深度上限，撞上就报 `PARSER_ERR_STACK`（-20） |
| `%max_errors N` | `PARSER_MAX_ERRORS` | 1..4096 | 8 | 一次解析最多保留几条错（见 [`08-generated-code.md`](08-generated-code.md) §2.2） |
| `%error_recovery_tokens N` | `PARSER_ERROR_RECOVERY_TOKENS` | 0..4096 | 3 | 错误恢复后「压制重复报错」的 Token 数（bison 的 3 Token 规则）；0 = 不压制 |

约定：

- 产物里仍然是 `#ifndef <宏> / #define <宏> <值>u / #endif`，所以**编译期 `-D` 优先**，
  改模板也可以；
- 每条指令只能写一次，值必须是整数，超出范围 / 写非数字 / 后面有多余内容都会在生成期
  报错（带原文件行号）；
- 写了任何一条，产物的 `RESULT.md` 里会多一节「容量宏」，列出三个宏的当前值与来源；
- 三条都不写时用的就是模板里的原值。

不写任何一条时三个宏都取缺省值。

### `%epilogue`：往生成代码末尾贴一段用户代码

```text
%code
/* 声明、宏、static inline 函数 */
%endcode

%epilogue
/* 普通函数定义写在这里：原样贴到 parser.c 末尾 */
double MyHelper(double x)
{
    return x * 2.0;
}
```

规则：

- `%epilogue` **单独成行**（前后只允许空白），之后的内容**原样收集**（不解析、
  不转义、不用 `%code` 包），直到下一个「行首是 `%`」的行或文件末尾；
- 一份文法最多一个 `%epilogue`，写第二个报「%epilogue 只允许一个」；
- 贴的位置是生成的 `parser.c`（C++17 是 `parser.cpp`）**最后**，在 `parser_user.h`
  已经被 include 之后，所以能直接用 `%code` 里声明的类型、宏与函数声明；
- **Python 产物下没有 `parser_user.h`，也没有 `#line`**（Python 没这条指令）；
  正文按 Python 缩进规则归一化（去公共缩进、行首 tab 换 4 空格）后追加到
  `parser.py` 末尾，前面贴一行 `# 语法文件 <文法文件> 行 <N>` 的注释标出它在
  `.gram` 里的位置（与 C 目标 `#line` 指向同一行，见
  [`07-codegen.md`](07-codegen.md) §4.2），正文与解析器类同一个模块，可以直接叫
  类里的内部方法；
- 不新增产物文件——它就是 `parser.c` 的一部分；
- **C++17 下这段代码在命名空间之外**：你写的是普通 C++ 函数，能被其它编译单元
  按普通 C++ 链接规则链到。正文前生成器会插一行 `using namespace <命名空间>;`
  （默认 `Parser`，`%namespace` 时是那个名字），正文**原样贴、不做任何改写**；
  里面照样能写 `Parser_RunText(...)` / `Parser_GetLastError()` / `ParserError_T`
  这些 C 名字，靠的是 C++17 产物里自带的一层兼容入口（见
  [`08-generated-code.md`](08-generated-code.md) §1.2）。新代码也可以直接用
  `Engine`：`engine.runText(text, &result)` / `engine.lastError()`。

`%epilogue` 里**不允许定义 `main`**：生成器会做一次简单词法扫描（行首忽略空白与
注释后是 `main(...)` / `int main(...)` 这类声明），命中就报
「%epilogue 里不允许定义 main；请把 main 放到 %main 段里」。宁可误报也不漏报——
漏报会在链接期炸出两份 `main`。检测在条件编译**之后**做，所以 `%if` 包住的 `main`
一样会被抓到。

### `%main`：用自己的 main 替换内建 demo main

```text
%main
int main(void)
{
    ParserValue_T result;
    if (Parser_RunText("1+2*3", &result) != PARSER_OK) {
        return 1;
    }
    printf("%g\n", (double)result);
    return 0;
}
```

规则：

- `%main` **单独成行**，正文原样收集到下一个「行首是 `%`」的行或文件末尾；
- 正文进产物的 `main.c`（C++17 是 `main.cpp`），**替换内建 demo main 的正文**，
  外壳（`#include <stdio.h>` / `<string.h>` / `"parser.h"`）不变，产物清单不变；
- C++17 产物的 `main.cpp` 里，模板会在最后一个 `#include` 之后插一行
  `using namespace <命名空间>;`，正文原样贴、不做改写，所以里面照样能写 C 名字的
  `Parser_RunText`（兼容层），也能直接写 `Engine engine; engine.runText(...)`；
- Python 产物下正文整段替换 `main.py` 的**演示体**（内建的 `_read_line()` /
  `main()` 与 `if __name__ == "__main__": sys.exit(main())` 全在这里），外壳的
  头注释、`import sys` / `import parser` 与类名别名 `Parser` 保留；正文原样贴、
  只按 Python 缩进规则归一化，所以入口调用要自己写（顶层直接跑，或自己加
  `if __name__` 段）；
- `%main` 可以写多段（配合 `%if` 表达多分支），但**同时生效的只能有一个**：

| 条件编译之后生效的 `%main` 段数 | 结果 |
| --- | --- |
| 0 | 用生成器内建的 demo main |
| 1 | 用这一段替换 `main.c` 的正文 |
| >1 | 生成期报错「多个 %main 同时生效」，并指回第二段所在的原文件行号 |

- 和 `%if` 的两种配合都支持（预处理层已经处理掉 `%if`）：`%main` 段内部写 `%if`
  只影响段内内容；`%if` 包住整个 `%main` 时条件为假就整段不生效；
- 想覆盖 `main` 也可以走 `%copy file my_main.c`（见 §7）：`%main` 适合短的入口，
  `%copy` 适合要 include 一堆自己头文件的长入口。**两条路不要同时用**，否则两份
  `main` 链接失败。

### 空白 / 换行当 Token：`\b` 与 `%space`

生成式词法器默认在取 Token 前跳过空白（空格 / `\t` / `\n` / `\v` / `\f` / `\r`），
所以想按「行」或按「空白分隔」写文法时，得先把空白变成 Token：

```gram
%token NL "\n"      /* 换行是真实 Token：可以在产生式里直接写 NL */
%token SP "\b"      /* \b = 一个空白字符（空格 / \t / \n / \v / \f / \r 都命中） */
%space " \t"        /* 取 Token 前只跳过空格与制表；换行归 NL 管 */
```

三个写法各管一段：

| 写法 | 作用 |
| --- | --- |
| `%token NAME "\n"` / `"\t"` / `" "` | 字面量里写了空白，这些字符就**不再被跳过**：对得上就产出这个 Token，可以对它写产生式 |
| `%token NAME "\b"` | `\b` 是「一个空白字符」的通配：空格、制表、换行都命中，词素是输入里真实的那一个字符。既是**解析对象**，也是「强制一个空白」 |
| `%space "空白字符集"` | 声明**取 Token 前跳过哪些空白**：缺省等价于 `%space "\b"`（全部空白），`%space ""` 表示一个都不跳过 |

规则细节：

- 取 Token 前遇到空白时，词法器先看「以空白 / `\b` 开头的固定字面量能不能在
  当前位置命中」：命中就停手，交给词法匹配当 Token 用；否则按 `%space` 的集合
  跳过。所以**声明了空白 Token 就自动生效**，不用额外开关；没声明空白 Token
  的文法跳空白逻辑与从前一样，词法行为完全不变（字面量规则表只是多了两个
  标记字段）。
- `\b` 只在固定字面量（`%define` / `%token NAME "..."`）里是这个意思。
  `%token regex` 的正则里要写 `\s`（正则的字符类转义），`\b` 在正则里仍是普通
  字符 `b`。
- `%space` 的参数只能是空白字符与 `\b`，`|` 分隔多个候选（写法与 `%define`
  一致）；出现别的字符、或写两次都直接报错。
- 固定字面量（包括带 `\b` 的）命中后词素写进 `pName` / `%tree` 的 `pText`，
  但**不写 `%union` 成员**（与其它固定字面量一致）；要值就自己读词素，或改用
  `%token regex`。
- 不在 `%space` 集合里、又没有 Token 覆盖的空白会让词法器报「词法解析失败」，
  这正是「这种空白不许出现」的写法。
- 内建 demo main 只喂**一行**输入（行尾换行会被吃掉），所以要试 `NL` 这类换行
  Token 得自己写 `%main` 读整个标准输入，或直接调 API：
  `Parser_RunText("a\nb\n", &result)` / `engine.runText("a\nb\n", &result)` /
  `engine.run_text("a\nb\n")`。

完整例子见 `example/space_demo.gram`（配 [`10-examples.md`](10-examples.md)）。

### 用 CSV 维护字面量别名表：`%token NAME include "..."`

关键字表 / 指令表 / 枚举名表这类「一个 Token 有海量字面量」的场合，把别名列表放进
CSV、用一条 `%token` 指令追加进来：

```gram
%token A include "kw.csv"                 /* A 的别名来自 kw.csv */
%token <ival> NUM include "numbers.csv"   /* 带成员的 Token 也能用 */
```

语义只有一条：

> 打开 CSV，把它当作一份两列别名表；只取**第一列**等于 `A`（或 `NUM`）的行，
> 把每行的**第二列**作为**字面量**追加到该 Token 的别名列表末尾。

等价于逐行展开成 `%token A "…"`——同一份 CSV、同一批行的手写写法是
`%token A "let|print|while"`（一个 Token 的多个字面量在文法里就是这么写的）。

**只追加别名，不是声明 Token：**

- Token 必须已经或同时由普通 `%token` 声明；本指令只增加别名，不创建新 Token
  （`%token A include "…"` 自己也算一条声明）。
- CSV 第一列出现文法里**根本不存在的** token 名 → 整行**完全忽略**，不报错、
  不创建、不影响其它 Token。
- CSV 第一列出现某个 Token 名，但文法里**没有**对那个 Token 使用 `include`
  → 同样忽略（这就是「表头 / 无关行不校验」的机制）。
- 不改变 Token 的类别：`%token regex A "…"` 与 `%token A include "…"` 可共存，
  后者只追加字面量别名。
- 带成员的 Token 允许 `include`：`%token <ival> NUM include "…"` 只为 `NUM`
  追加字面量别名，成员类型 `<ival>` 不变，别名匹配到的 Token 仍是 `NUM`。

多种组合都支持，结果按声明出现顺序合并：

```gram
%token A "x"
%token A include "kw.csv"        /* 手写在前、CSV 在后 → 别名顺序 x, <kw.csv 的行…> */
%token A include "kw2.csv"       /* 同一 Token 引用多份 CSV */
```

```gram
%token A include "kw.csv"        /* 两个 Token 共享同一份 CSV，各取各的 */
%token B include "kw.csv"
```

**别名唯一性**（硬规则）：

| 情形 | 结果 |
| --- | --- |
| 同一 Token 下别名之间重复 | 静默去重，保留首次出现的顺序 |
| 不同 Token 之间同一个字面量作为别名 | **报错**，指出两个来源（CSV 行 + 首次分配处） |
| 别名与 Token 名相同（如 CSV 给 `A` 追加别名 `"B"` 而文法里已有 `%token B`） | **报错** |
| 别名与 `%token regex` 模式 | **不冲突**：正则 Token 不参与字面量别名唯一性判定，只走现有最长匹配 / 声明顺序规则 |

唯一性在**展开阶段**做一次全量扫描：把文法里所有 `%token … include` 收集齐再统一
判定，所以与「先展开后到」的顺序无关。

**CSV 格式**（最小 RFC 4180 风格，**表头忽略、不校验、只读前两列**）：

- 只读每行的**第 1 列**（Token 名）与**第 2 列**（字面量）；第 3 列及以后一律忽略。
- 不要求第一行是表头，也不校验第一行内容；第一行恰为表头（如 `token,word`）时，
  其第一列不匹配任何被导入的 Token 名，**自然被忽略**。
- 空行跳过；行首 `#` 当注释跳过。
- 第 2 列要写成**双引号字符串字面量**（与 `%token NAME "…"` 的写法一致）：支持
  `""` 表示一个双引号、字段内逗号；**不支持跨行字段**。
- 第 1 列 / 第 2 列为空都报错；第 1 / 2 列前后空白**不自动裁剪**（字面量里的
  空格有意义）。
- CSV 路径相对**声明它的那份 `.gram` 文件所在目录**解析，允许子目录。
- 同一 `.gram` 内多处引用同一份 CSV 时按规范化路径缓存读盘结果。

**第 2 列一律按字面量处理，绝不解析成正则**：要正则 Token 继续用
`%token regex NAME "…"`；本指令不提供正则通道，也不读取 CSV 里的正则列，
更不要在 CSV 里新增 `regex` 列。

**错误格式**（都带文件、行、列）：

```text
1.csv:5:2: 第 2 列（字面量）不能为空
1.csv:7:1: 第 1 列（Token 名）不能为空
1.csv:5:2: 别名 "x" 已被 token A 使用，不能重复分配给 token B（首次出现于 2.csv:3:2）
example/calc.gram:8:1: 找不到 CSV 文件 1.csv
```

CSV 内的错误用**相对 `.gram` 目录的路径**；`%token … include` 这一行本身的
错误（缺文件）用文法路径 + 行列。最终都由 `main` 打印成 `错误：…`，退出码 1。

**与其它指令的关系**：

- 展开发生在 `%if` 条件编译**之后**、`grammar_loader` 正式解析之前——被
  `%if` 假分支包住的 `%token … include` **不读 CSV、不报缺文件**。
- 展开出来的别名声明位于该条指令的**原始位置**，按 CSV 行顺序排列。
- `--lang gram` 的导出行为**不变**：它导出的仍是输入的 `.gram` 原文件，展开只
  发生在内存里。
- **不实现**：`%commit include` / `%trivia include` / `%identifier include`、
  整个 Token 表的 CSV 外置、CSV 里的正则列、别名「新建 Token」语义。

示例：`example/token_dict_demo.gram`（含带 `<ival>` 成员的 Token、CSV 里未导入
的对照行、第 3 / 第 4 列噪音）。

### `%commit`：把识别但不进语法层的文本收进容器

`%commit` 是词法层的第三条通道：匹配到的文本**既不进语法分析、也不当 Token**，
而是按「行号 + 类型 + 原文」收进解析器的容器；典型用途是把注释留给后处理
（文档提取、AI 分析日志等）。三种形式：

```gram
%commit block "/\*([^*]|\*+[^*/])*\*+/"   /* 块注释：类型 block */
%commit line  "//[^\n]*"                  /* 行注释：类型 line */
%commit "0x[0-9a-fA-F]+"                  /* 类型缺省 = "default" */
%commit skip  "@[A-Za-z]+"                /* 匹配即丢，不进容器 */
```

文法层解析规则：

- 写法是 `%commit [类型] "模式"`：类型是一个标识符（`[A-Za-z_][A-Za-z0-9_]*`），
  不写时缺省为字符串 `default`；`%commit skip "模式"` 是「匹配即丢」的
  简写（`skip` 是保留的类型名，不写模式的 `%commit skip` 会报错）。
- 模式必须是一个双引号字面量，**按 DFA 正则编译**（正则 → NFA → 子集构造 →
  最小化）：没有正则元字符的写法（如 `"//"`）就是固定字面量。DFA 不支持
  非贪婪，块注释要写 DFA 友好形式（上面的例子）；模式能匹配空串、
  正则语法有误都在生成期报错并指回这一行。
- `%commit` 的匹配与 `%token`（固定字面量 / `%token regex`）以及内建注释规则
  走**同一份词法匹配顺序**：先按最长匹配，长度相同时按声明先后（先声明优先；
  同一行内按书写顺序）。**同一个模式不能既当 Token 又当 `%commit` /
  `%trivia`**：与已声明的固定字面量或正则重复时生成期报错。
- 匹配从「跳过 `%space` 空白之后」的位置开始；`%commit` 命中的文本会被吃掉
  （词法位置前进），`skip` 的片段不产生容器元素，其余片段按出现顺序追加。
- 行号是片段**起始位置**的 1-based 行号；`text` 是匹配到的原文（含字面量
  本身），跨行片段里的换行原样保留。列号 / 偏移量不在 `%commit` 元素里
  （语法层的位置 API 见 [`08-generated-code.md`](08-generated-code.md)）。
- `%commit` 是词法层的东西，不改变文法（`analysis_*` 完全看不到它）；
  `%tree`、LL / LR / `%mode lex` 都能用。
- 语言边界：六档都支持（C99 `Parser_GetCommits()`、C++17 `Engine::commits()`、
  Python `engine.commits`、C# `Commits`、Rust `engine.commits()`、
  Go `engine.Commits()`）。容器的字段与
  所有权细节见
  [`14-languages-and-products.md`](14-languages-and-products.md)。

完整例子见 `example/commit_demo.gram`。

#### 「匹配即丢」写成 `%trivia`

`%commit skip "<模式>"` 与 `%trivia "<模式>"` 是同一条规则的两个写法：都表示
「匹配到的文本既不进容器、也不当 Token」。区别只在文法层可读性——
`%commit skip` 把「丢」藏在 `%commit` 的类型位置，容易和「收进容器」混在一起，
所以新文法推荐写：

```gram
%trivia "//[^\n]*"                     /* 行注释：匹配即丢 */
%trivia "/\*([^*]|\*+[^*/])*\*+/"      /* 块注释：匹配即丢 */
```

两者共用同一个「词法声明序」，所以和 `%token` / `%commit` 的最长匹配、同长度
先声明优先的规则完全一致；`%trivia` 的模式同样不能与 `%token` 重复。
`example/commit_demo.gram` 里两种写法各留了一条（`%commit skip "@[A-Za-z]+"`
与 `%trivia "#[A-Za-z]+"`），把 `%trivia` 那一行改成 `%commit skip` 重新生成，
产物与运行结果逐字节相同。

#### 已知的 DFA 构造坑：否定类写在「分组 + 交替 + 星号」里

只有 **C99 目标**的正则走 `source/src/regex_dfa.cpp` 编译成 DFA 并写进产物。有**一类写法会
编译成功、却永不匹配**（实测，`%token regex` / `%commit` / `%trivia` 都复现）：
否定类同时排掉 `*` 与结束符那个字符，并且放在 `(A|B)*` 的分支里。写块注释时
最容易撞上：

```gram
%trivia "\(\*([^*]|\*+[^*)])\*+\)"     /* ✗ 永不匹配：命中不了任何 (* ... *) */
%trivia "\(\*([^*]|\*+[^)]+)*\*+\)"    /* ✓ 语义等价，正常匹配 */
```

两条正则语言相同（注释里有 `*`、有 `)`、有中文、跨行都能配对），差别只在
`\*+[^*)]` 还是 `\*+[^)]+`。踩中时的表现是**静默不匹配**：词法器把注释开头的
`(` 当成「认不出来的字符」，既不提示正则有问题，也不报生成期错误。所以：

- 块注释照上面 `\*+[^)]+` 这一族写法写（C 风格 `[^*/]`、花括号 `[^}]` 不受影响）；
- 拿不准就把正则单独写一份小文法，`%mode lex` + 一行输入跑一下，看 Token 列表里
  有没有你期望的那条——5 秒就能验，别在生成器里猜。

这不是推荐写法而是引擎现状：修 `regex_dfa` 会改动所有正则产物（六档的词法表），
本批按「既有行为不变」冻结，所以先以文档 + 写法规避。完整例子见
`project/pl0-ir/pl0.gram` 的 `%trivia` 两条。

> **C++17 / Python / C# / Rust / Go 目标不受这条影响**：它们的词法器不用生成的 DFA，
> 而是把正则原样交给各自语言自带的正则引擎（C++17 的 `std::regex`、Python 的 `re`、
> C# 的 `Regex`、Rust 官方的 `regex` crate、Go 标准库的 `regexp`，见
> [`08-generated-code.md`](08-generated-code.md) 的 §10.4 / §12.2 / §14.4 / §15.5 与
> [`07-codegen.md`](07-codegen.md) §13），上面那两条写法都能正常匹配。
> 反过来说，**同一个文法在 C++17 / Python / C# / Rust / Go 与 C99 产物上可能出现「一个
> 匹配上了、一个没匹配上」**——要跨目标一致就照上面的推荐写法写。与方言有关的另一处差异是
> `\b`（见本节开头「规则细节」）。

### 四个「写用户代码的地方」怎么分工

| 放什么 | 放哪 |
| --- | --- |
| 声明、`typedef`、宏、`static inline` 函数 | `%code ... %endcode`（进 `parser_user.h`，被 `parser.c` / `parser_token.c` 等 include） |
| 普通函数定义、`static` 辅助函数 | `%epilogue`（进 `parser.c` 末尾） |
| `main` | `%main`（进 `main.c`） |
| 独立编译单元（自己的 `.c` / `.h`） | `%copy`（拷进产物目录并进 Makefile / CMakeLists） |

反例：把非 `inline` 的函数定义塞进 `%code`——`parser_user.h` 会被多个 `.c`
包含，链接时就是多份重复定义。正例见 `example/epilogue_demo.gram`（声明在
`%code`、实现在 `%epilogue`、动作里调用）与 `example/main_demo.gram`。

## 3. 语义动作

| 写法 | 含义 |
| --- | --- |
| `$$` | 本条产生式的返回值 |
| `$1` `$2` … | 第 n 个右部符号的值 |
| `$$.<字段>` | 语义值是结构体时按字段赋值（`$$` 可作左值） |
| `int32(0)` / `float(1.5)` | 转成 C 强制转换 `(int32_t)(0)` / `(float)(1.5)` |
| `$1.<字段>` | 普通 C 成员访问（`$n` 不能出现在赋值左边） |
| `sqrt($3)` / `pow($1,$3)` | 按同名 C 函数调用生成，自己 `#include <math.h>` |
| `$1` `$3`（`%union` 下） | 照旧写运算，生成器按符号自动补成员：`$1` 是 `term`（`<ival>`）就展开成 `values[1].ival` |
| `$$`（`%union` 下） | 展开成 `result.value.<左部符号的成员>` |
| `$n.<成员名>` | 手写成员的写法原样穿过 |

算术/比较/位运算按普通 C 运算符生成；生成期不做 C 类型检查（`$$ = $1` 类型
不匹配也照发，交给 C 编译器）。`$n = 表达式`（给右部符号赋值）报错，信息里带
产生式编号与动作里的字符位置。动作里不支持的写法一律在生成期报错，清单见
§5「生成期校验」。

`%union` 下语义值是联合体：`$$ = $1 + $3;` 这种写法不用改，生成器按符号绑定的
成员补上 `.成员`。

LL(1) 与 LR 系列（SLR/LALR/IELR）的 `$n` 取值方式不同，由 `ActionTranslator` 按模式翻译，见
[`07-codegen.md`](07-codegen.md)。

### 3.1 `error` 产生式（yacc 风格的恢复锚点）

`error` 是保留的伪终结符，像普通 Token 一样写在产生式**右部**：

```gram
%mode lalr

stmt_list := stmt_list stmt { $$ = $2; } | stmt { $$ = $1; };

stmt := expr SEMI  { $$ = $1; }
      | error SEMI { yyerrok; $$ = 0; };   /* 坏语句整条丢掉，接着解析下一条 */
```

运行时遇到语法错误时，先弹栈到某个状态能移进 `error`，把 `error` 当伪 Token
移进（`errstatus = 3`），**不消耗当前 lookahead**，用同一个 Token 回到主循环继续
解析。移进 `error` 时压入的是零初始化的 `ParserValue_T`，所以它的 `$n` 是垃圾
占位值，不要依赖内容。

允许 / 禁止的位置：

| 位置 | 结果 |
| --- | --- |
| 产生式右部 | 允许 |
| 产生式左部（`error : ...`） | 报错 |
| `%type` / `%token` / `%token <成员>` / `%define` / `%token regex` / `%start` / `%prec` 的参数 | 报错 |
| 用户自己定义的符号名（非终结符或 Token 与 `error` 同名） | 报错（`error` 是保留字） |

模式限制：

- **LR 家族**（`lr0` / `slr` / `lalr` / `lr1` / `ielr`）支持；`error` 加进终结符
  集合，参与项集构造与 ACTION 表，ACTION 表多一列 `error`（见
  [`06-analysis.md`](06-analysis.md) §2.6）。
- **LL 家族**（`ll0` / `ll1` / `llk`）下写 `error` 产生式生成期报错：自顶向下
  没有「弹栈到最近恢复点」这个动作，语义与 yacc 不兼容。自顶向下有自己的恢复
  手段——**FOLLOW 集恢复**（见 §3.2），不写任何指令就能跳过坏片段；`%sync`
  只是额外汇进来的锚点。
- FOLLOW 集合**不含 `error`**：`expect` 集合与 LL(1) 的同步判定都不会把 `error`
  当成正常终结符传播。

`yyerrok` / `yyclearin` 写在 `error` 规则的动作里，必须**单独成句**：

| 写法 | 含义 |
| --- | --- |
| `yyerrok;` | 清掉「正在恢复」状态，让后面的错误照常上报 |
| `yyclearin;` | 丢掉当前 lookahead Token（下一个 Token 由主循环再取） |

生成出来的调用在 C99 产物里是 `Parser_LrYyErrOk` / `Parser_LrYyClearIn`，在 C++17
产物里是 `Engine::Private` 的成员函数 `yyErrOk()` / `yyClearIn()`。两者写在表达式位置
报错，在 LL(1) 模式下使用也报错（只有 LR 家族有「错误状态」这个概念）。不写
`yyerrok` 时，恢复后 3 个 Token（`PARSER_ERROR_RECOVERY_TOKENS`）内不再重复报错，
与 bison 的 3 Token 规则一致。

### 3.2 三种模式的错误恢复：`%sync` / FOLLOW 集 / 吞 Token

```gram
%mode lalr
%sync SEMI RBRACE        /* 恢复锚点，按声明序记录 */
```

#### 3.2.1 自底向上（LR 家族）：`%sync` + `error` 产生式

先试 `error` 产生式那一层；找不到 `error` 列动作时，`%sync` 兜底——弹栈（弹栈前
先做掉栈上待归约的项，panic reduce——错误常常出现在归约到一半的构造里，例如
`1 + + 2` 卡在 `expr PLUS . term`）到某个能移进同步 Token 的状态，丢掉输入直到
那个 Token，移进它，再接着解析。

#### 3.2.2 自顶向下（LL 家族）：FOLLOW 集恢复

LL 家族**不写任何恢复指令也有恢复**——运行时会自动按下面的顺序对齐（这就是
「FOLLOW 集恢复」，与 ANTLR / 教科书里的 panic mode 一致）：

1. 栈顶那一项**能取**当前 Token（终结符相等，或非终结符在这一列的预测表里有
   产生式）→ 已经对齐，主循环接着走；
2. 当前 Token 是用户声明的 `%sync` 锚点 → 弹栈到能取它的位置；
3. 栈顶那一项「该结束了」（**当前 Token 在它的 FOLLOW 里**），或者栈里更深处
   有项能取它 → **放弃「栈顶那一项所属的构造」**，连同它里面已经展开的内层构造
   一起退栈（内层帧在退栈时按 §3.1 的规则清掉语义值），把当前 Token 交给外层；
4. 谁都不认它 → 丢掉这个 Token，看下一个。

这就是「两层都没写也能恢复」的自顶向下版本：`%sync` 在 LL 下只是第 2 步的额外
锚点，真正的默认手段是第 3 步的 FOLLOW 集。弹栈与丢输入共用一份 256 步预算，
病态输入不会把解析拖死。

**LL(0) 例外**：只有 ε 一列，没有「当前 Token 属于谁」的信息，FOLLOW 恢复无从
判起，仍是「一错即停」（这是自顶向下的定义决定的）。


约束：

- `%sync` 里的 Token 必须是已声明的 Token（`%define` / `%token` / `%token regex`）；
  未声明报错。
- LR 家族两层都写时**以 `error` 产生式为准**，`%sync` 只在找不到 `error` 列动作
  时兜底；LR 家族两层都没写才保持老行为（报一个错、返回负错误码，不恢复）。
- `%sync` 不参与分析层：它不进终结符集合、不进 ACTION 表，只是运行时的一个锚点
  集合，另外在分析报告（`--report`）与产物 `RESULT.md` 里单列一节「同步 Token」。
- LL 家族下 `error` 产生式不存在（见 §3.1），恢复靠 FOLLOW 集 +（可选的）
  `%sync`。`ll0` 是唯一例外：它连「当前 Token」都看不到，两条路都用不上，
  运行时保持一错即停（写了 `%sync` 也不会生效，生成期不报错）。

## 4. 词法匹配顺序

取 Token 之前先按 `%space` 跳过空白（缺省全部空白；有以空白 / `\b` 开头的固定
字面量能在当前位置命中时不跳过，见上面「空白 / 换行当 Token」一节）。之后的
匹配顺序（长匹配优先，同长按顺序）：

1. 双引号字符串 `"..."` → `STRING`
2. 若声明了 `%token regex`：正则 Token 与固定字面量一起做最长匹配，
   **同长度时字面量优先**（关键字不会被正则吃掉）
3. 内建数字规则（整数 / `1.2` / `1e3` / `u`、`d` 后缀）
4. 剩下的固定字面量（长的优先 + 关键字边界检查）
5. 标识符：有 `%identifier` 用它的 DFA，否则用内建 C 标识符规则，
   命中 `VARIABLE`；有关键字表时先查关键字

关键字边界检查的含义：固定字面量 `"b"` 后面紧跟字母/数字/下划线时不匹配，
所以 `"bc"` 不会被切成 `b` + `c`，要写成 `"b c"`（见 `example/or_branch.gram`）。

固定字面量命中后词素会写进 `pToken->pName`，因此 `%tree` 里显示
`COLON(:)` 而不是 `COLON(COLON)`。

## 5. 生成期校验（节选）

- `%mode` 与命令行 `--mode` 不一致时以命令行为准。
- 每条产生式必须写动作，或整个文法启用 `%tree`。
- `%start` 必须是已定义的非终结符。
- **动作里不支持的写法**（这里是**唯一**一份清单，其它文档只指过来）：
  `%var` / `%var_max_count` 指令，以及动作里的 `[$n]`（动态变量引用）、
  `def[...]` / `delete[...]`（变量定义与删除）、`$n.int32`（内置的标量类型字段）、
  `max(...)`（内置函数）——写了都在生成期报错，并提示「请在 `%code` 里用 C
  自己实现」。语义值只有 `custom` 一种（`%value_type` / `%union`，见 §2 与 §3）；
  需要变量表之类的语义，自己在 `%code` / `%epilogue` 里用 C 写，见 `example/calc.gram`。
- `%space` 只能声明一次，参数只能是空白字符（空格 / `\t` / `\n` / `\v` / `\f` /
  `\r`）或 `\b`（代表全部空白），写成别的字符直接报错；`%space ""` 是合法的
  「一个空白都不跳过」。
- `%token NAME include "csv"`：CSV 打不开报错（文法路径 + 行列）；第 1 / 2 列为空、
  第 2 列不是双引号字符串、引号没闭合都报错（CSV 路径 + 行列）；别名跨 Token 重复、
  别名撞上已声明 Token 名也报错。CSV 第一列对不上任何被导入 Token 名的行
  **静默忽略**，表头**不校验**。详见上文「用 CSV 维护字面量别名表」。
  - 语义值运行时只有 `custom`，也是缺省值（不写就等于 `custom`）；写了 `%runtime`
    会在生成期报错并提示删掉那一行。
- `error` 只能出现在产生式右部：出现在左部，或写进 `%type` / `%token` /
  `%token regex` / `%define` / `%start` / `%prec` 的参数都报错；与用户符号
  同名也报错。
  - `%sync` 里的 Token 必须已声明；`error`
    产生式在 `%mode ll1` 下报错；`yyerrok` / `yyclearin` 在
    `%mode ll1` 下报错。
  - `error` 产生式在 LL 家族（`ll1` / `llk` / `ll0`）下都报错，`yyerrok` /
    `yyclearin` 同理；展望符长度只对 `lrk` / `lalrk` / `slrk` / `llk` / `ll0`
    有意义（别的模式写了直接报错），模式名里的 k 与 `%lookahead` 冲突也报错。
  - **归约/归约冲突不再中止生成**：确定性 LR 模式（`lr0` / `slr` / `lalr` /
    `lr1` / `ielr` / `lrk` / `lalrk` / `slrk`）按「优先级高的胜出，否则取文法里更早
    出现的产生式」自动选一条，并打一条醒目的大警告；细节见
    [`06-analysis.md` §10](06-analysis.md)。
- 条件编译：`%if` / `%elif` / `%else` / `%endif` 必须配对，条件表达式只能写
  `||` / `&&` / `!` / 括号 / 变量名；指令与条件的错误都带**原文件**行号。
- `%epilogue` 最多一个；`%epilogue` 里不能定义 `main`（指路 `%main`）；
  `%main` 段里不能出现 `%epilogue`（两段都到文件末尾，`%epilogue` 要写在前面）；
  条件编译之后同时生效的 `%main` 超过一个就报错。
- `%union` 最多一次、成员名不能重复；`%type` / `%token <成员>` 引用的成员必须
  在 `%union` 里，同一个符号不能绑两个成员，顶层逗号与提取不出成员名都报错。
- 目标语言相关：`%lang` 只接受 `c` / `cpp17` / `python` / `csharp` / `rust` / `go` /
  `gram`；`%class` 只对
  `--lang python` 有效（配 C / C++17 报错），名字按标识符规则 + Python 关键字表
  校验；`%namespace` 只对 C++17 有效（配 C / Python 报错）；`%cpp_only` 配
  `--lang c` / `--lang python` / `--lang csharp` / `--lang rust` / `--lang go` 报错；Python 产物
  另外拒绝 `%value_type` /
  `%union` / `%type <成员>` / `%token <成员>`（见上文 `%class` 一节）；Rust 产物
  拒绝 `%destructor`（靠所有权回收）。
- 写了 `%union` 时，动作里引用到的符号（`$n` 对应符号、`$$` 的左部符号）都必须
  绑定成员；`$n` 不允许出现在赋值左边；`%union` / `%type` / `%token <成员>`
  只在 custom 运行时下可用（这是唯一的语义值运行时）。
- `$n` 越界报错：右部只有 k 个符号时写 `$k+1`（或 `$0`）在生成期报
  `产生式 P? 的 $n 越界：这条产生式右部只有 k 个符号`，并带上动作所在行号——
  不拦的话越界处取不到成员后缀，最后只会得到一句看不懂的 C 类型错误。
- `%type <成员>` / `%token <成员>` 绑的符号必须真的存在（已声明的 Token 或某条
  规则的左部）：名字打错时在生成期报 `符号 X 没有定义`（带指令行号），而不是
  静默把它当成「一个没用的绑定」。
- 正则不能匹配空串（六档一律报错：哪个语言的 Lexer 都没法前进）；`{m,n}` 的重复
  上限是 1000（`{m}` / `{m,}` / `{m,n}` 都按这条算，超过报错并提示改成 `*` / `+`
  或拆成两段）；C99 与 C++17 的 `dfa` 词法器用的是本仓库自带的正则编译器，其余
  各档交给目标语言的正则库，同一套写法在六档里都能用（见
  [`07-codegen.md`](07-codegen.md) §13）。
- LL(1) 直接左递归会自动消除，检测到间接左递归报错；SLR/LALR/IELR 的未消解冲突
  按 `%expect` 规则处理（IELR 会先尝试拆分冲突状态，拆不掉才报错）；

## 6. 词法模式（`%mode lex`）

`%mode lex` 只导出词法器：不建分析栈 / 分析表，也没有解析入口，所以**允许一条
产生式都没有**（其它模式必须有规则，否则报「没有找到任何文法规则」）。

- 词法相关的写法照常生效：`%token`（固定字面量 / `regex`）、`%define`、
  `%identifier`、`%space`、`%union` + `%token <成员>`、`%code`、`%main`、
  `%epilogue`、`%copy`、`%if`。
- 解析相关的写法会被**忽略**（不是报错）：产生式、`%start`、`%tree`、
  `%sync`、`%destructor`、`%error_context`——相应的生成期校验也一并跳过；入口会
  打一句 `提示：%mode lex 只导出词法器，已忽略：…`。`%left` / `%right` /
  `%nonassoc` 仍然按 bison 的规矩声明 Token。
- 词法相关的校验照旧：`%token <成员>` 的成员必须在 `%union` 里、`%identifier`
  与 `VARIABLE` 不能同时声明、右部未定义符号、`$n` 越界（见 §5）。
- 六种目标语言都支持，产物里都只有词法器：
  - C（`--lang c`）：`lexer.h` / `lexer.c` / `parser_token.h` / `parser_token.c` /
    `main.c`（+ `%code` 时的 `parser_user.h`），**不生成** `parser.h` /
    `parser.c` / `parser_table.c`；入口是 `Parser_LexInit` + `Parser_NextToken`，
    错误用 `Parser_GetLastError()` / `Parser_GetErrorCount()` / `Parser_GetError(i)`；
  - C++17（`--lang cpp17`，缺省）：`lexer.hpp` / `lexer.cpp` / `main.cpp`
    （+ `%code` 时带前缀的 `parser_user.h`），**不生成** `parser.hpp` /
    `parser.cpp`；入口是 `Engine::tokenize(std::string_view, std::vector<Token> *)`，
    错误用 `engine.errors()` / `lastError()`；
  - Python（`--lang python`）：`parser.py`（词法器 + `tokenize`）/ `main.py`，
    模块里没有分析表 / 分析栈 / `run_text`；入口是 `Parser().tokenize(text)`，
    返回 `(状态码, Token 列表)`。
  - C#（`--lang csharp`）：`Parser.cs` / `Token.cs` / `Program.cs` /
    `parser.csproj`，入口是 `new Parser().tokenize(text, tokens)`（`List<Token>`）。
  - Rust（`--lang rust`）：`Cargo.toml` + `src/parser.rs` / `src/token.rs` /
    `src/main.rs`，模块里没有分析表 / 分析栈；入口是
    `Parser::new().tokenize(text)` → `(i32, Vec<Token>)`，错误用
    `engine.errors()` / `engine.last_error()`。
  六档都带 `RESULT.md`；C / C++17 另有 `Makefile` / `CMakeLists.txt`，
  C# 用 `dotnet build` 直接编译，
  Rust 用 `cargo build`（计算只用标准库，词法正则用官方的 `regex`），
  Python 不需要编译。
- CLI（C 的 `main.c` / C++17 的 `main.cpp` / Python 的 `main.py` / C# 的
  `Program.cs`）
  都是「输入 Token
  判断是什么类型」：把标准
  输入读到 EOF，逐个打印 `<类型>  "<词素>"  <行>:<列>`，
  最后一行是总数；词法错误打印一行说明并以退出码 1 结束（`-29` 认不出来的字符、
  `-28` 字符串没有闭合）。
- C / C++17 交出去的 Token 词素指向内部复用缓冲，只在下一次取 Token 之前有效；
  Python / C# 侧是普通字符串（不用管）。

示例：`example/lexer_only.gram`（只有词法声明，没有产生式）。

## 7. 相关实现

- 加载与校验：`source/src/grammar_loader.cpp`
- `%token NAME include "csv"` 的展开：`source/src/token_dict.cpp`（`TokenDict::expand`，
  在 `Preprocessor` 之后、`GrammarLoader` 之前跑）
- 数据模型：`source/include/ParseEngine/grammar_types.hpp`
- 动作翻译：`source/src/action_translator.cpp`
- 正则编译：`source/src/regex_dfa.cpp`
