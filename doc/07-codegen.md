# 04 代码生成与模板系统

各语言的模式与增量 API 支持范围统一见 [12 支持矩阵](15-language-mode-support.md)。

## 1. 产物是怎么拼出来的

`Project::generateParserProject(ProjectRequest)` 决定产物清单，再按目标语言逐个调用
`CodeGenC99` / `CodeGenCpp17` / `CodeGenPython` / `CodeGenCsharp` / `CodeGenRust` /
`CodeGenGo` 生成完整后端内容；DD/Earley/CYK 专用的 Java、JS 则由 `CodeGenJava` /
`CodeGenJs` 与共用的 `code_gen_narrow.cpp` 生成，最后交给
`Project::writeParserProject` 建目录写文件。
`ProjectRequest` 把文法、词法、分析结果、优先级、`%code ... %endcode` 用户代码、
`%value_type` 声明的语义值类型、`%union`、`%copy` 列表等一次性传进来
（见 `source/include/ParseEngine/project.hpp`）。

产物清单写死在 `source/src/project.cpp`：

```cpp
const std::vector<std::string> k_projectFiles = {
    "parser.h",      "parser.c",       "parser_token.h", "parser_token.c",
    "parser_user.h", "parser_table.c", "main.c",         "Makefile",
    "CMakeLists.txt", "RESULT.md"};
```

`%code ... %endcode` / `%value_type` / `%union` 都没有时，`parser_user.h` 从清单里移除
（`parser.h` 里给一个默认的 `typedef int ParserValue_T;`）。`%copy` 拷进来的
文件追加到清单末尾。

**`%mermaid` / `--mermaid` 会在清单里加一组 `mermaid/` 状态机图示**
（`Mermaid::projectFiles`，见 [`06-analysis.md`](06-analysis.md) §5.4）。它和教学
模式（`%teach`）无关：`%teach` 只改报告内容，`%mermaid` 只管这一组文件；两个
开关都缺省关闭，命令行可以各自覆盖文法里的写法：

| 文件 | 内容 |
| --- | --- |
| `mermaid/index.html` | 总览页：所有图内嵌在一页，带图例与互链 |
| `mermaid/grammar.html` | 文法 LR 自动机（LR 项集 + GOTO + 状态配色） |
| `mermaid/lexer.html` | 每个 `%token regex` / `%identifier` 的 DFA（没有正则 Token 时不生成） |
| `mermaid/grammar.mmd` / `mermaid/lexer.mmd` | 同上两张图的 Mermaid 源码 |

它们是**新增文件**，与目标语言无关（各档产物里的内容一样、扩展名原样），
`parser.c` / `parser.cpp` / `Parser.cs` 等产物不受影响。`%mermaid` 未开启、或两张图都画不出来（LL(1) 且没有正则 Token）
时这一组文件根本不会出现在清单里；`--dump-mermaid` 走标准输出，不经过产物清单。

## 2. 模板内联

产物里的每一段 C 代码、`Makefile`、`RESULT.md` 骨架都在 `source/templates/`
（**共 384 份，全部 `.tmpl`**）：

- **`.tmpl`** —— `library/template` 模板引擎的格式（`{{ 变量 }}` / `{% if %}` /
  `{% for %}` / `{% include %}` / `{# 注释 #}` / `{% raw %}`），
  生成器调 `tmpl::renderValues()` 渲染（见 §3）。**没有变量的纯静态片段也用它**
  （例如 `csharp_csproj.xml.tmpl`：目标框架号 `net8.0` 直接写在模板里，生成器只
  负责取出来，一个字都不改；但**文件头带 `{# … #}` 说明块的例外**——那种必须过一遍
  引擎，否则注释会原样漏进产物）。**全部走 `.tmpl`**：C99 的 `c99_*`（50）、
  C++17 的 `cpp17_*`（68）、Python 的 `python_*`（51）、C# 的 `csharp_*`（49）、
  Rust 的 `rust_*`（53）、Go 的 `go_*`（55）、Java 的 `java_*`（2）、JS 的
  `js_*`（3）、共用片段 `code_gen_*`（3）、
  构建脚本 8 份（`Makefile_*` 4 + `CMakeLists_*` 4）、`RESULT.md`（1）与
  21 份 `result_*.md` 片段与 20 份 `mermaid_*` 模板。
  - **`.template`** —— 占位符格式（模板里写 `__占位符__`，生成器用 `replaceAll` 填）。生成器仍支持它；
    仓库自带模板已经全部是 `.tmpl`。

> C99 那批的文件名是 `c99_<原名>`，其中 `c99_lexer.c` / `c99_parser_token.c` /
> `c99_parser_table.c` 与**产物文件名**同名——改模板名时别把产物名一起改了。

两者按「去掉后缀的名字」共享同一个键（`Utils::loadTemplate("RESULT.md")` 两种
后缀都命中，同名时 `.tmpl` 优先并打一句提醒）。构建期由
`tools/embed_templates.py` 把它们生成为 `source/src/templates_data.cpp`：

- 按文件名排序，去掉后缀作为 key（同名时 `.tmpl` 覆盖 `.template`）；
- 用原始字符串字面量 `R"PGX(...)PGX"` 存内容，并断言内容里不含 `)PGX"`；
- 生成 `ParseEngine::embeddedTemplates()`，返回 `unordered_map<string,string>`。

运行期 `Utils::loadTemplate(name)` 先查内联表，再用
`SCRIPT_ENGINE_TEMPLATES=<目录>` 覆盖（开发时可直接读磁盘模板，不必重新内联）。
`source/src/templates_data.cpp` 是生成文件，不要手改；改了模板后跑：

```sh
python3 tools/embed_templates.py     # 等价于 make -C source templates
```

## 3. 占位符与分段

两种模板的填值方式不同：

- **`.template`（占位符格式）**：
  - **整体替换**：`__XXX__` 形式的占位符，由本地 `replaceAll` 一次性替换成生成内容。
  - **分段抽取**：模板用 `__SEC_NAME_BEGIN__` / `__SEC_NAME_END__` 包住一段，
    `CodeGen::extractSection(template, "NAME")` 取出这段（例如
    `parser_prologue.c` 里的 `DEFINES`、`ERROR_TYPE`、`TOKEN_TYPES`）。
      只剩 C 后端还在用这套。
- **`.tmpl`（模板引擎格式）**：模板自己写 `{{ 名字 }}` 取值、`{% if %}` 分支、
  `{% for %}` 循环、`{% include "名字" %}` 拼片段，生成器只把数据摆成「名字 → 值」
  （`source/src/project.cpp` include 了 `template/engine.hpp`，走 `tmpl::renderValues` /
  `tmpl::encode`；`{% include %}` 的片段表是 `tmpl::renderValues` 的第三个实参）。
  产物里要写目标语言自己的 `{{` / `}}`（例如注释里的 `%code ... %endcode`）时包进
  `{% raw %}…{% endraw %}`，否则会被当成变量标签吃掉；块标签独占一行时整行删除，
  所以模板里不用为了少一行空行去写 `{% if %}` 的同行变体。

**共用部分（`source/src/code_gen.cpp`）也有三份片段模板**——各档后端都要用的那几段
C/C++ 代码不再以字符串字面量的形式写在 C++ 里：

| 模板 | 渲染出来的东西 | 用到的引擎特性 |
| --- | --- | --- |
| `code_gen_action_cases.cpp.tmpl` | 每个产生式一个 `case Nu: { … break; }` 语义动作分支（`CodeGen::actionCases`） | `{% for %}` 遍历产生式、`{% if %}` 选 `#line` 变体与空动作的默认语句 |
| `code_gen_line_back.txt.tmpl` | 「恢复行」`#line <下一行> "<产物名>"`（`resolveLineDirectives`） | `{{ }}` |
| `code_gen_section_marker.txt.tmpl` | 分段标记 `__SEC_<名字>_<BEGIN/END>__`（`sectionMarker`） | `{{ }}` |

`code_gen_action_cases.cpp.tmpl` 的 `cases` 是**对象数组**：`source/src/code_gen.cpp` 里的
`ActionCase` 用 `TEMPLATE_MODEL(...)` 绑了 `id` / `has_line` / `line` / `empty` /
`action` 五个字段，模板里写 `{{ c.id }}`、`{% if c.has_line %}`。这是 `source/src/` 里
第一处用上「模型类 + `{% for %}`」的地方（其余产物模板传的都是现成的字符串或
`tmpl::encode` 出来的数组）。

两条经验（写这类片段模板时会踩）：

- **换行属于谁**：`{% endfor %}` 与上一行内容写在同一行时，那一行的换行在标签
  *之后*、属于循环体外，每轮之间就会少一个分隔换行；要让 `{% endfor %}` 独占一行
  （整行删除）才有「每轮以换行收尾」的效果。
- **片段模板的结尾换行**：模板文件按惯例以换行收尾，而片段是**插进别的产物当值**
  用的，多带一个换行就多一行，所以渲染完要把结尾的换行收掉——`code_gen.cpp` 里
  统一走 `renderValueTemplate()`（渲染 + 收掉结尾换行）。

  替换顺序有讲究：占位符名字互为前缀时（例如 `__CUSTOM_LR_LOOP_TREE_ACCEPT__` 与
  `__CUSTOM_LR_LOOP_ACCEPT__`），先替换长的或改成不会前缀撞车的名字，否则会被截断错替。

## 4. 唯一的语义值运行时

语义值运行时只有一种，也是缺省值（写 `%runtime` 会报错）。它的要点：

| 项目 | 说明 |
| --- | --- |
| 额外产物 | 有 `%code ... %endcode` / `%value_type` / `%union` 时多一个 `parser_user.h` |
| 语义值类型 | 用户提供的 `ParserValue_T`（POD）：`%value_type` 声明，或 `%union` 按符号分成员，或自己在 `%code ... %endcode` 里 typedef |
| 解析入口 | `int32_t Parser_RunText(const char *, ParserValue_T *)` |
| 动作翻译 | 按普通 C 表达式/运算符生成，生成期不做类型检查 |
| 内存 | 语义值由用户负责；`%tree` 的树节点走 `malloc/realloc/free` |
| `%tree` | 节点是 `ParserTree_T`，只存名字与词素，与语义值类型解耦 |

动作里只认普通 C 表达式与运算符，生成期不做类型检查；不在支持范围内的写法会在生成期报错，
指向「请在 `%code ... %endcode` 里用 C 自己实现」。生成期校验的完整清单见
[`05-grammar.md`](05-grammar.md) §5。


**错误恢复**是运行时的第二层逻辑，和分析层解耦（见
[`06-analysis.md` §2.11](06-analysis.md#211-error-列与-sync错误恢复在分析层的落点)）：

- `c99_custom_lr_runtime.c.tmpl` 里的 `Parser_LRRecover` 实现两层策略——`error`
  产生式（`#ifdef PARSER_TOKEN_ERROR`，占位值零初始化后压栈、不消耗 lookahead）优先，
  `%sync` panic mode（`#ifdef PARSER_HAS_SYNC_TOKENS`，弹栈前先 `Parser_LRPanicReduce`
  做掉栈上待归约的项，再丢输入到同步 Token）兜底；
- `c99_custom_ll_loop.c.tmpl` 做自顶向下的 panic mode：① 栈顶能取当前 Token →
  对齐完成；② 当前 Token 是 `%sync` 锚点 → 弹栈到能取它的位置；③ 栈顶那一项
  「该结束了」（当前 Token 在其 FOLLOW 里）或更深处有项能取它 → 放弃栈顶那一项
  所属的构造（连内层帧一起退栈，`Parser_LLDiscardFramesFrom`）；④ 谁都不认就丢
  Token。②③④ 共用 256 步预算。FOLLOW 由生成器发的 `s_ParserFollow`
  （非终结符 × 终结符的 0/1 表，C++17 是 `kLlFollow`、Python 是 `_LL_FOLLOW`）
  提供；LL(0) 没有「当前 Token 属于谁」的信息，退回一错即停。
- 生成器侧的 `syncPredicateC` 按 `%sync` 生成 `PARSER_HAS_SYNC_TOKENS` /
  `Parser_IsSyncToken`（LR 模式还多一个 `Parser_LRCanShiftSyncToken`）
  片段（占位符 `__CUSTOM_SYNC_PREDICATE__`，LR / LL 两份运行时都收），
  `%sync` 本身不进分析层；
- 多错误表在 `c99_parser_c_custom.c.tmpl`（`s_ParserErrors` + `Parser_SetError` /
  `Parser_GetErrorCount` / `Parser_GetError`），容量宏
  `PARSER_MAX_ERRORS` / `PARSER_ERROR_RECOVERY_TOKENS` 在
  `c99_parser_prologue.c.tmpl`；弹栈丢弃的语义值先查 `%destructor` 的分派表
  （`Parser_DiscardValue`，写在 `generatorDestructorDispatchC` 里），该符号没绑
  `%destructor` 时才退回可选的 `PARSER_USER_DISCARD_VALUE` 钩子；
- `c99_parser_prologue.c.tmpl` 的 `__SEC_DEFINES__` 段同时给出内存钩子：
  生成代码里的清零 / 拷贝一律写 `PARSER_MEMSET` / `PARSER_MEMCPY`，
  缺省 `#ifndef` 兜底成 `<string.h>` 的 `memset` / `memcpy`，用户可在
  `%code ... %endcode` 或编译期 `-D` 里换成自己的实现（C++17 产物不生成这两个宏）。

**`%epilogue` 的插入位置**（在 `generateCustomProject` 的收尾循环里）：

- 拼装顺序是「模板渲染 `parser.c` → `collapseBlankLines` + `__MODE_NAME__` 替换 →
  最后追加 `%epilogue` 正文」。追加点在右花括号**之后**，所以用户定义的函数留在
  全局作用域、别的编译单元能照常链到（和 `parser_user.h` 里的声明对得上）；
  C++17 走 `generateCpp17Project`：`CodeGenCpp17::parserCpp()` 出 `parser.cpp` 之后
  由 `appendEpilogue()` 追加正文，正文前插一行 `using namespace <命名空间>;`，
  正文**原样贴**（不改写），于是 `%epilogue` 里用 C99 的名字（`Parser_RunText` /
  `ParserError_T`）也能调到生成器产出的函数——C++17 产物自带兼容层；
- 因此不需要在 `parser.c` 里插内部边界标记：追加在最后等价于「两段式」，而且
  标记不会泄漏进产物、也不受 `collapseBlankLines` 影响；
- `%main` 走另一条路：`CodeGenC99::customMainCWithBody()` 用 `main_custom.c` 模板
  （`stdio` / `string` / `parser.h` 外壳不变）把 `__DEMO_MAIN__` 换成用户正文；
  0 段时仍走 `customMainC()`。

四个「写用户代码的地方」的分工（放错位置最常见的后果是链接期重复定义或多份
`main`）：

| 放什么 | 放哪 | 为什么 |
| --- | --- | --- |
| 声明、`typedef`、宏、`static inline` 函数 | `%code ... %endcode` → `parser_user.h` | 会被 `parser.c` / `parser_token.c` 一起 include，只能放「重复包含无害」的东西 |
| 普通函数定义、`static` 辅助函数 | `%epilogue` → `parser.c`（C++17 是 `parser.cpp`）末尾 | 只出现一次；C++17 下在命名空间之外（别的编译单元能链到），正文前补 `using namespace` 且正文原样贴，靠兼容层直接调生成器函数 |
| `main` | `%main` → `main.c` | 与内建 demo main 二选一，避免两份 `main` |
| 独立编译单元（`.c` / `.h`） | `%copy` | 拷进产物目录并进 Makefile / CMakeLists，自己 include 自己的头 |

反例：非 `inline` 的函数定义放进 `%code ... %endcode` → `parser_user.h` 被多个 `.c` 包含 →
multiple definition。正例：`example/epilogue_demo.gram`。

### 4.1 custom + `%union`：按符号分 C 类型的语义值

写了 `%union` 时，`ParserValue_T` 由生成器给出，
并且**追加在用户 `%code ... %endcode` 代码之后**：
  函数 / 类型不要用 `Parser_` 开头，否则动作里的调用在 `parser.cpp` 侧会找不到声明；
  换个前缀即可（`example/union_demo.gram` 用的是 `Demo_MakeNode`）。
- 没写 `%union` 时这套代码完全不碰：产物与加 `%union` 之前一样。

#### 终结符与词法器的联动

`ParserToken_T.value` 在有 `%union` 时也换成 `ParserValue_T`（见
[`08-generated-code.md`](08-generated-code.md) §10 的完整形态表），词法器于是能
按 `%token <成员>` 把词素直接写进成员：数字 Token 写解析出的数值，双引号字符串、
标识符（含 `%identifier`）与 `%token regex` 写匹配到的文本，固定字面量不写。
`Parser_UserTokenValue()` 没定义 `PARSER_USER_TOKEN_VALUE` 时改成直接
`return pToken->value;`，用户定义了宏仍然优先。

生成期的实现要点：

- 成员名由 `lexMemberOf()` 从「符号 → 成员」表里取；`NUMBER` / `STRING` /
  `VARIABLE` 三个内置终结符按名字查，`%token regex` 按 `s_ParserRegexTypes` 的
  下标同序生成一个 `switch (bestType)`。
- 数字 Token 不再套 `(int32_t)`：`pToken->value.<成员> = (value);`，让 C 的隐式
  转换把 `long` / `double` 落到成员自己的类型上。
- 写了 `%union` 却没给 `NUMBER` 绑成员时，联合体里没有放数字的地方，模板里那四
  句 `pToken->value = (int32_t)(…)`（往联合体赋 `int` 编译不过）整行删掉：
  `PARSER_MEMSET` 已经把 Token 清零，数字 Token 的语义值因此恒为零值，C++17
  产物在同一个位置留一句注释说明。生成期还会打一条 `警告：`（只在文法真的用到
  `NUMBER`、且 `NUMBER` 仍走内建数字词法时）。没写 `%union` 时 `value` 是
  `int32_t`，这四句照旧保留。
- 正则与标识符 Token 手上只有文本，所以写的是 `pCtx->lexeme` 的地址——它是词法器
  复用的缓冲区，下一枚 Token 会覆盖；要留存请自己拷贝。
- `parser_token.h` 需要在 `ParserToken_T` 之前看到 `ParserValue_T`：它用
  `#ifndef PARSER_USER_HEADER_INCLUDED / #define … 1 / #include "parser_user.h" /
  #endif` 包含用户头，`parser.h` 在 `%union` 下也补上同样的 `#define`，两个入口
  谁先展开谁生效。
- 没有 `%union` 时这些全都不生成：`value` 还是 `int32_t`，`parser_token.h` 一个
  字节都不变。


```c
/* parser_user.h —— 用户代码在前：%union 可以引用这里定义的类型 */
struct Node { int value; struct Node *next; };
#define PARSER_USER_TOKEN_VALUE(pToken) ((ParserValue_T){ (int)((pToken)->value) })

/* —— 生成器追加的语义值类型：body 原样来自文法 */
typedef union {
    int          ival;
    struct Node *node;
} ParserValue_T;
```

顺序很关键：`%union` 的成员可以引用用户在 `%code ... %endcode` 里 `typedef` / `struct`
出来的类型，所以用户代码必须排在前面。只有 `%union` 而没有 `%code ... %endcode` 时也会
生成 `parser_user.h`，并且不再注入缺省的 `typedef int ParserValue_T;`；两者都
没有时行为与以前一致。

`$n` / `$` 按符号绑定的成员自动展开（`ActionTranslator` + `UnionValueSpec` 把
「符号 → 成员」和槽位对应关系传进翻译器）：

| 写法 | 无 `%union`（老样子） | 有 `%union` |
| --- | --- | --- |
| `$`（LL(1)） | `result.value` | `result.value.<左部符号的成员>` |
| `$`（SLR / LALR / LR(1) / IELR） | `result.value` | `result.value.<左部符号的成员>` |
| `$n`（LL(1)） | `values[n]` | `values[n].<第 n 个符号的成员>` |
| `$n`（SLR / LALR / LR(1) / IELR） | `stack->values[stack->top - k + n - 1]` | 同上再加 `.<第 n 个符号的成员>` |
| `+ - * / %`、比较、位运算 | 直接生成 C 运算符 | 不变（成员访问后是标量，运算符照常） |

`%destructor <成员> { ... }` 走另一条翻译入口：片段同样原样收集，但只认 `$$`
（`translateDestructorBody` 按 token 替换，字符串字面量里的 `$$` 不动），展开成
「正在被丢弃的那个值」——C 产物渲染成 `pValue->成员`，C++17 产物同样。写
`$1` / `$line` 这类需要右部符号的写法在生成期报错。分派表本身由
`generatorDestructorDispatchC`（C）/ `generatorDestructorDispatchCpp17`（C++17）
生成，两边共用 `CodeGen::destructorSymbolGroups` 算出的「成员 → 绑了它的符号
（符号名 + 统一符号号）」分组。产物形态见
[`08-generated-code.md`](08-generated-code.md) §10.1。

几个边角：

- 生成期不做 C 类型检查：`$$ = $1` 类型不匹配不拦，交给 C 编译器报警告/报错。
- `$n.<成员名>` / `$n.<字段名>` 这种手写成员访问原样穿过（只有
  `$n.<标量类型名>` 会被拦下并提示），所以用户既可以用自动展开，也可以自己写。
- `$n = 表达式`（给右部符号赋值）报错，信息里带产生式编号与动作
  里的字符位置——右部符号的语义值归它自己，`yacc` 也不允许这么写。
- builtin 运行时的那批内置写法已经移除，写了会报错（清单见
  [`05-grammar.md`](05-grammar.md) §5）；写了 `%union` 时错误信息里会多一句提示
  （请直接写 `$n.<成员名>`）。
- LL(1) 的左递归消除会把左递归那一步搬进 `LEFT_LOOP`：`left_continue` 产生式里
  `$1` 是累加值（符号是左部非终结符而不是改写后的右部首符号），`$2` 起才对应
  改写后的右部符号。成员查找按这个槽位对应关系走，见
  `generatorActionCases` 里对 `production.transform == "left_continue"` 的处理。
- 空动作的产生式（LR 的虚拟起始产生式就是这样）在 `%union` 下生成
  `memset(&result.value, 0, sizeof(result.value));`——联合体不能用 `= 0`。
- C++17 产物里的动作生成的是 `result.value` / `stack->values[...]`（空动作从
  `memset` 换成值初始化 `result.value = ParserValue_T{};`），与 C 产物逐条对应；
  这一处差异由 `CodeGen::actionCases(..., cpp17)` 的一个开关控制，C 与 C++17
  共用同一份翻译器；Python 走 `ActionTranslator` 的方言开关（见 §10.2）。
- 用户代码（`%code ... %endcode` / `%epilogue`）生成器一个字都不改写，所以里面定义的函数 /
  类型**不要**用 `Parser_` 开头（那是生成器与 C++17 兼容层占用的前缀）。换个前缀
  （例如 `Demo_MakeNode`）即可。

### 4.2 `#line` 指令：让编译器报错指回 `.gram`

生成器给所有「来自 `.gram` 的代码」前后各插一对 `#line`：

```c
        case 3u:
        {
#line 10 "example/calc.gram"
            result.value = stack->values[stack->top - 1u + 0u];
#line 511 "parser.c"
            break;
        }
```

前一行把「下一行」标成 `.gram` 的第 10 行，后一行把行号还给 `parser.c` 自己。
插入位置：

| 内容 | 落点 | 前向 `#line` 指向 |
| --- | --- | --- |
| 语义动作 | `parser.c` 的动作分支里 | 动作 `{` 所在行 |
| `%code ... %endcode` | `parser_user.h` | 正文第一行 |
| `%union` 正文 | `parser_user.h` 的 `typedef union { … }` 里 | `%union` 那一行 |
| `%epilogue` 正文 | `parser.c`（C++17 是 `parser.cpp`）末尾；C++17 下在命名空间之外，正文前多一行 `using namespace` | `%epilogue` 的下一行 |
| `%main` 正文 | `main.c`（C++17 是 `main.cpp`）正文 | `%main` 的下一行 |

行号来源与实现要点：

1. **行号是原文件行号**：`GrammarLoader` 在解析时用 `SourceLines`（字节偏移 →
   预处理行 → 原文件行）换算，`%if` 丢掉整行不会让行号漂移；跨行的动作只标起始行，
   后面的行交给 C 编译器自己递增。
2. **占位符 + 回填**：拼装阶段只写 `@@SE_GRAMMAR_FILE@@`（`.gram` 路径）与
   `@@SE_LINE_BACK@@`（恢复行），收尾循环最后调
   `CodeGen::resolveLineDirectives(text, 文法路径, 产物名)`：
   - 前向行换成用户传给 `--grammar-file` 的那个路径（`\` 与 `"` 转义），保持相对 /
     绝对形式不变；
   - 恢复行换成「该指令所在行 + 1」与**最终产物名**（`parser.cpp` / `main.cpp`；
     `parser_user.h` 两个语言下同名）。
3. **回填在最后一步**：`generateCpp17Project` 把 `parser.hpp` / `parser.cpp` /
   `main.cpp` / `parser_user.h` 都拼完之后（含 `%epilogue`）才逐个调
   `resolveLineDirectives`，这样行号与产物名都是最终的；`.gram` 路径从来不会被
   改名逻辑碰到。
4. **纯生成内容不加**：`Makefile` / `CMakeLists.txt` / `RESULT.md` 里一行 `#line`
   都没有（用 `pl0.gram` 这类没有语义动作的示例可以直接验证）；C 产的
   `parser_table.c` / `parser_token.c` / `parser.h` 同理。

上面每一条都能用 `gcc` / `g++` 的真实报错行号核对。示例：
`example/line_demo.gram`。

**Python 产物：注释代替 `#line`。** Python 没有 `#line` 指令（报错只会给产物自己
的行号），所以同样的位置改成一行注释 `# 语法文件 <文法文件> 行 <N>`，指向的行号与
C 的 `#line` 完全一致：

```python
        if production == 3:
            # 语法文件 example/calc.gram 行 10
            result.value = values[0] + values[2]
            return STATUS_OK
```

落点四处：语义动作（`_apply_action` 的分支里，缩进跟着动作体走 12 个空格）、
`%code`（`parser.py` 模块顶部）、`%epilogue`（`parser.py` 末尾）、
`%main`（`main.py` 演示体开头）。生成侧只有两个函数：
`CodeGenPython::grammarLineMarker(line)` 拼那一行注释（路径同样先写
`@@SE_GRAMMAR_FILE@@` 占位符），收尾时由 `generatePythonProject` /
`generateLexPythonProject` 直接把占位符换成真实路径——**不走
`resolveLineDirectives`**，因为那是给 C 字符串字面量做转义的，注释里不需要
（Windows 反斜杠原样保留），也不能让它把恢复行改写成 `#line`（Python 会语法错误）。

### 4.3 空白 Token 与 `%space` 的词法生成

三套后端的词法器都长这样：取 Token 前先跳空白，再按「字符串 → 正则 + 固定字面量
最长匹配 → 数字 → 标识符」的顺序匹配。要点在于**跳空白那一步也要认字面量**——
否则 `%token NL "\n"` 声明了也永远匹配不到（空白早就被跳掉了）。生成方式：

- 固定字面量规则表（C 的 `s_ParserLexRules` / C++17 的 `kLexRules` / Python 的
  `_LEX_RULES`）每条多带两个标记：`blankFirst`（首字节是空白或 `\b` 通配）与
  `wildcard`（含 `\b` 通配）。前者给跳空白循环用，后者决定命中后词素要不要按
  输入抄一份（字面量里存的是占位字节 `0x1F`，不是真实字符）。
- 跳空白循环是**生成**的，不是固定模板：没写 `%space`、也没有以空白 / `\b` 开头
  的字面量时，原样输出经典的 `isspace` 循环（这类文法的词法行为保持一致，
  只是字面量规则表多了两个标记字段）；
  否则换成「按 `%space` 集合判断 + 命中以空白开头的字面量就停手」的循环，并把
  集合（C / C++17 是 `strchr` 常量串，Python 是 `_LEX_SKIP_CHARS`）与
  命中判断函数（`Parser_LexRuleHit` / `lexRuleHit` / `_lex_rule_hit`）一起生成。
- `\b` 在解码字面量时写成占位字节 `k_blankWildcardChar`（`0x1F`），命中判断函数
  遇到它就改成「任意一个空白字符」；C / C++17 的 `varCString` 会把控制字节写成
  三位八进制（`\037`），避免 C 的 `\x` 转义贪心吃掉后一个字符。
- `%space` 的三个取值：不写（缺省全部空白）、写了（用声明的集合，`\b` 展开成六
  个空白字符）、写 `""`（空集，一个都不跳过）。生成期已经把 `\b` 展开，运行期
  只查普通字符表。

`%space` 只在 C / C++17 / Python 的词法器里出现，语义值、分析表、动作翻译都不
受影响；`%tree` 里空白 Token 的词素就是输入里真实的那个空白字符（比如
`NL(\n)`、`SP(\t)`）。

## 5. `%tree`

- 节点是 `ParserTree_T { pName, pText, ppChildren, childCount, childCapacity }`，
  **只保存符号名与 Token 词素**，不保存语义值，用 `malloc/realloc/free` 管理；
  模板是 `custom_tree_api.h`、`custom_tree_helpers.c`、`custom_tree_ll1.c`、
  `custom_lr_tree_body.c`、`custom_lr_tree_api.c`、`lr_tree_header.c`。
- 树与语义值类型完全解耦：`%value_type` / `%union` 怎么改都不影响树接口。

## 6. `%copy`

`GrammarLoader` 把 `%copy [file|dir] 路径` 解析成 `CopySpec`，相对路径以文法
文件所在目录为基准：

- `file`（缺省）：拷到产物目录，保留 basename；
- `dir`：递归拷贝，保留目录层次；
- 拷进来的 `.c` / `.h`（C++17 产物另加 `.cpp` / `.hpp`）分别追加到生成
  `Makefile` 的 `LIB_SRCS` / `HDRS`。
- 拷进来的源文件同时追加到生成 `CMakeLists.txt` 的 `add_library(parser ...)`
  源文件列表（模板里由 `{% for src in copy_sources %}` 展开）。

产物写出时 `writeParserProject` 会自动为带子目录的路径建父目录。

## 7. 生成 `Makefile` / `CMakeLists.txt` 与 `RESULT.md`

- `source/templates/Makefile_custom.tmpl`（C）与 `Makefile_custom_cpp17.tmpl`
  （C++17）：`make` 出 `libparser.a` + `parser_demo`；另有 `lib` / `demo` /
  `run` / `clean`；演示程序从标准输入读一行，所以 `make run` 更常用
  `make run < 输入.txt`（`ARGS` 仍会当 argv 传下去）。模板里 `LIB_SRCS` /
  `HDRS` 用 `{% for %}` 展开 `%copy` 拷进来的文件、`parser_user.h` 用
  `{% if user_header %}` 决定要不要，生成器不再对 Makefile 做「找行尾追加」
  这类字符串手术。
- `source/templates/CMakeLists_custom.txt.tmpl`：
  与 Makefile 等价的一套 CMake 目标（`parser` 静态库 + `parser_demo`），
  由 `generatorCMakeCustom` 生成；`%copy` 的源文件由 `{% for %}` 展开进
  `add_library`。两份 CMake 模板末尾都给 MSVC 加 `/utf-8`
  （`target_compile_options(parser|parser_demo PRIVATE /utf-8)`）：产物源码
  是 UTF-8，MSVC 不加这个开关会按当前代码页（简体中文是 GBK/936）解码，
  产物里的中文字符串字面量会在编译期被读坏（C2001）。
- `source/templates/Makefile_custom_cpp17.tmpl` / `CMakeLists_custom_cpp17.txt.tmpl`：
  目标语言为 C++17 时的构建脚本（`CXXFLAGS = -std=c++17`，`LIB_SRCS` 只有
  `parser.cpp`，头文件 `parser.hpp` / `parser_user.h`），由
  `generatorMakefileCpp17` / `generatorCMakeCpp17` 生成，产物名按 `%namespace`
  前缀替换。
- `source/templates/RESULT.md.tmpl`：生成信息表、产物清单、各可选小节、冲突消解与
  未消解冲突；产物清单用 `{% for file in product_files %}` 展开，末尾的
  「文法分析报告」（`{{ report }}`）由 `{% if full_report %}` 把着，只有
  `--full-report` 时才输出（见 [`08-generated-code.md`](08-generated-code.md#17-resultmd默认简化版)）。

  它的章节有两种来源：

  | 来源 | 章节 | 怎么进 RESULT.md |
  | --- | --- | --- |
  | 独立模板（静态文案） | 「编译运行」`result_run_section_{python,c_cpp17}.md`、「运行时与内存」`result_runtime_section_{custom,cpp17,python}.md`、`%tree` 的「语法树」`result_tree_section_{custom,cpp17,python}.md` | `{% include "run_section" %}` / `{% include "runtime_section" %}` / `{% include "tree_section" %}` |
  | 生成器算出来的值 | 生成信息表、`%sync`、容量常量/宏、`%epilogue` / `%main`、`%lookahead`、IELR 拆分、Mermaid 图示、产物清单、位置信息与 `%commit`、冲突消解、分析报告 | `{{ sync_section }}` 这类变量（空串 = 这一节不出现） |

  `{% include %}` 的片段名在 RESULT.md 里是**固定的**，具体取哪一份模板由
  `generatorResultMd` 按后端决定（片段表只放该给的那一份；不该出现的名字不放，
  include 就输出空串，正好等于"这一节不出现"）。include 进来的片段是**按模板渲染**
  的，用的是同一个上下文——所以 `result_run_section_c_cpp17.md` 里的
  `{{ main_file }}` / `{{ link_command }}` / `{{ lib_note }}` 就直接取到了值，
  不必再对渲染结果做字符串替换。

  运行时/内存一节各档语言分开写（C++17 明确写「不支持自定义分配器」，C# 与 Go
  写「语义值靠 GC 回收、不支持 `%destructor`」）；
  `%mode ielr` 时额外渲染「IELR(1) 状态拆分」小节（状态数 + 拆分记录）。
  C# 的 `RESULT.md` 不走这套（`generatorCsharpResultMd` 在 C++ 里拼）。
- 演示程序 `main.c`：从标准输入读一行（到换行或 EOF）后开始解析；
  纯 `%tree` 文法的 demo 打印语法树，带语义动作的文法打印解析结果。写了
  `%main` 时这一整段被用户正文替换，入口行为由用户自己决定。

## 8. C++17 语言后端（`--lang` / `%lang`）

C++17 产物**不是**对 C 产物做文本改写，而是**另一套模板 + 另一套后端**：

| 层 | C99（`--lang c`） | C++17（缺省，`--lang cpp17`） |
| --- | --- | --- |
| 后端 | `source/src/code_gen_c99.cpp`（`CodeGenC99`） | `source/src/code_gen_cpp17.cpp`（`CodeGenCpp17`） |
| 模板 | `source/templates/c99_*.tmpl`（模板引擎格式） | `source/templates/cpp17_*.tmpl`（68 份，模板引擎格式） |
| 产物 | `parser.h` / `parser.c` / `parser_token.*` / `parser_table.c` / `main.c` / `parser_user.h` | `parser.hpp` / `parser.cpp` / `main.cpp` / `parser_user.h` |
| 公开 API | `Parser_RunText` / `ParserError_T` / `ParserTree_T` | `Parser::Engine::runText` / `Parser::Error` / `Parser::Tree`（+ C 名字的兼容层） |
| 构建脚本 | `Makefile_custom.tmpl` / `CMakeLists_custom.txt.tmpl` | `Makefile_custom_cpp17.tmpl` / `CMakeLists_custom_cpp17.txt.tmpl` |

`Project::generateParserProject` 按 `request.lang` 在 `generateCustomProject`（C99）与
`generateCpp17Project`（C++17）之间分岔；两者共用「分析表 / 词法表 / 语义动作
翻译」（`CodeGen::actionCases` 带一个 `cpp17` 开关，LL/LR 的动作代码形式只在少数
地方不同），但**不共用模板**，也不做任何生成期文本改写。`--lang csharp`
走同一层的另一个岔口（`generateCsharpProject`，词法模式是
`generateLexCsharpProject`），见 §12。

### 8.1 C++17 产物的形态（Qt 风格 + 标准库）

- **一个命名空间一套类**：`namespace Parser` 里是 `class Engine`（Qt 风格
  d-pointer，`std::unique_ptr<Private> d`）、`class Tree`、`class Error`、
  `struct Token`、`enum TokenType`、`enum class Status`；
- **标准库数据结构**：`std::string_view`（输入与词素视图）、`std::string`、
  `std::vector`、`std::unordered_map`、`std::unique_ptr`；
- **`inline constexpr` 取代 `#define`**：`kValueSlots` / `kMaxStackDepth` /
  `kMaxErrors` / `kErrorRecoveryTokens` 都是命名空间里的常量；
- **分析表与词法表**用 `std::int32_t` / `std::uint16_t` 数组表示（`kAction` /
  `kGoto` / `kLexRules` / `kRegexDfas` …），与 C 目标同一份生成期数据；
- **不支持自定义分配器**：没有 `PARSER_USER_*` 释放钩子，语义值按值拷贝，语法树
  的所有权是 `std::unique_ptr`，析构函数就是释放；
- **兼容层**（`source/templates/cpp17_compat.hpp.tmpl`）：命名空间里额外提供
  `PARSER_OK` / `PARSER_ERR_*` / `PARSER_MAX_*`、`ParserError_T`、
  `Parser_RunText` / `Parser_Run` / `Parser_GetLastError` / `Parser_GetError`，
  底层共用一个进程级 `Engine`（**非线程安全**）。这样 `%main` / `%epilogue` 里
  按 C 产物写法写的代码不用改就能编。

### 8.2 词法与行动序列的生成方式

- 词法块（`source/templates/cpp17_lexer*.tmpl`）与 C 目标的块一一对应，但用
  `std::string_view` 切片、`std::isdigit(static_cast<unsigned char>(...))` 这类
  C++ 写法；固定字面量走 `kLexRules`，`%token regex` / `%identifier` / `%commit`
  走标准库 `<regex>`（`kRegexRes` / `kIdentifierRe` / `kCommitRes` +
  `lexRegexMatch()`，见 §13）；
- LR 驱动（`cpp17_lr_stack.cpp` / `cpp17_lr_loop.cpp`）用 `struct LrStack` 存
  **状态 + 语义值 + 变量名 + 树节点**，`$n` 翻成 `stack->values[stack->top - k + n - 1]`；
- LL(1) 驱动（`cpp17_ll.cpp`）用 `LlFrame` / `LlStack` 复刻 C 目标的语义帧；
- 诊断文本与 C 目标逐字一致（模式短名取模板变量 `{{ lr_name }}`、Token 类型名、期望集合）。

### 8.3 命名空间与文件名前缀：`%namespace` / `--namespace`

`CodeGenCpp17::namespaceName` / `filePrefix` 决定命名空间名与文件名前缀
（`GrammarConfig::cppNamespace`，命令行 `--namespace` 优先；空 = `Parser` + 不带前缀）：

- 头文件保护宏按产物文件名生成（`parser.hpp` → `PARSER_HPP`，`myapp_parser.hpp`
  → `MYAPP_PARSER_HPP`）——多实例共存的前提；
- `parser.hpp` 里对 `parser_user.h` 的 `#include` 与 `#line` 恢复行都用带前缀的名字；
- `Makefile` / `CMakeLists.txt` / `RESULT.md` 里的产物名由 `renameProductFiles()`
  替换（`libparser.a` / `parser_demo` / `%copy` 文件不动，脚本自身不加前缀）；
- 前缀为空时产物名就是 `parser.hpp` / `parser.cpp` / `main.cpp` / `parser_user.h`；
- `%namespace` 是 C++17 专属：配 `--lang c` 会直接报错（不静默忽略）。

示例 `example/namespace_demo.gram`。

### 8.4 `%cpp_only`：声明这份文法只能出 C++17

文法依赖 C++17 产物才有的东西（`Engine` / `std::string_view` / 兼容层）时写：

```text
%cpp_only
```

命令行 `--lang c`（或 `%lang c`）会被拦下来报错，避免有人误以为 C 产物也能编过。
指令解析与校验在 `source/src/grammar_loader.cpp`（`GrammarConfig::cppOnly`），命令行
拦截在 `source/src/main.cpp`。

## 9. Python 语言后端（`--lang python`）

Python 产物最直接的用途是**给文法做快速分析**：不用装编译器，`python3 main.py` 就能看到同一份文法在同样输入下的解析结果、错误消息与语法树。它和 C / C++17 一样，是「另一套模板 + 另一个后端」。

它也是**给 AI / 脚本当接口**最合适的一档：编码代理（Codex / Claude Code 这类带终端的助手）用 `printf '...' | python3 main.py` 或 `python3 -c "import parser; ..."` 就能拿到解析结果，输出通常只有几行——不用把长日志或整份输入贴进上下文，也不必把文法、产物读进上下文。「改文法 → 重新生成 → 看结果」这个循环因此能在很小的上下文里转很多圈；而且任何有 `python3` 的沙箱（CI、容器、AI 执行环境）都能跑，不必先配 C 工具链。

| 层 | C99（`--lang c`） | C++17（缺省） | Python（`--lang python`） | C#（`--lang csharp`） | Rust（`--lang rust`） | Go（`--lang go`） |
| --- | --- | --- | --- | --- | --- | --- |
| 后端 | `source/src/code_gen_c99.cpp`（`CodeGenC99`） | `source/src/code_gen_cpp17.cpp` | `source/src/code_gen_python.cpp`（`CodeGenPython`） | `source/src/code_gen_csharp.cpp`（`CodeGenCsharp`） | `source/src/code_gen_rust.cpp`（`CodeGenRust`） | `source/src/code_gen_go.cpp`（`CodeGenGo`） |
| 模板 | `source/templates/c99_*`（`.tmpl`，50 份） | `source/templates/cpp17_*`（`.tmpl`，68 份） | `source/templates/python_*`（`.tmpl`，51 份） | `source/templates/csharp_*`（`.tmpl`，49 份） | `source/templates/rust_*`（`.tmpl`，53 份） | `source/templates/go_*`（`.tmpl`，53 份） |
| 产物 | 10 个文件 + `Makefile` / `CMakeLists.txt` | `parser.hpp` / `parser.cpp` / `main.cpp` / `parser_user.h` | `parser.py` / `main.py`（+ `RESULT.md`，可选 `mermaid/` 与 `%copy` 的文件） | `Parser.cs` / `Token.cs` / `Program.cs` / `parser.csproj` | `Cargo.toml` / `src/parser.rs` / `src/token.rs` / `src/main.rs` | `go.mod` / `parser.go` / `token.go` / `main.go`（三个 `.go` 都是 `package main`） |
| 公开入口 | `Parser_RunText` / `Parser_Run` | `Parser::Engine::runText` / `Engine::run` | 解析器类的 `run_text` / `run` / `parse_tree` | `new Parser().runText` / `run` / `parseTree` | `Parser::new()` 后 `run_text` / `run` / `parse_tree` | `NewParser()` 后 `RunText` / `Run` / `ParseTree` |
| 结果 | `ParserValue_T` | `ParserValue_T`（值语义） | `engine.result`（任意 Python 对象） | `engine.result()`（`object` / `%value_type` / `ParserValue`） | `engine.result()`（`i64` / `%value_type` / `ParserValue`，**按值拷贝**） | `engine.Result()`（`int64` / `%value_type` / `ParserValue`，**按值拷贝**） |
| 构建脚本 | 有 | 有 | **没有**（不需要编译） | `parser.csproj`（`dotnet build`） | `Cargo.toml`（`cargo build`；计算只用标准库，词法正则依赖官方的 `regex`） | `go.mod`（`go build`；**只用标准库**，词法正则用标准库的 `regexp`） |

### 9.1 产物形态

`CodeGenPython::parserPy` 把生成期算好的表重排成 Python 字面量（列表 / 元组 /
字典），再填进 `source/templates/python_parser.py.tmpl` —— 解析与词法**共用同一份
骨架**，靠模板里的 `{% if lex %}`（docstring 的用法示例）与 `{% if tree_class %}`
（`Tree` 类整段）分档，生成器只传两个布尔量：

- 模块级常量：`STATUS_OK` / `STATUS_ERR_*`（数值与 C / C++17 逐字一致，方便三份
  产物对照）、`status_name()`、`TOKEN_*` 与 `TOKEN_NAMES`、`token_name()` /
  `token_text()`、`class Token`、`class Tree`（词法产物里整段不出现）、`class ParserError`；
- 词法表：`_LEX_RULES` / `_LEX_SKIP_CHARS` / `_KEYWORDS` / `_REGEX_RULES` /
  `_IDENTIFIER_RE` / `_COMMIT_RES`（正则走 Python 的 `re`，见 §10.4）；
- 分析表：LR 的 `_ACTION` / `_GOTO` / `_REDUCE` / `_PRODUCTION_LENGTH` /
  `_PRODUCTION_LHS` / `_LOOKAHEAD_*`，或 LL 的 `_LL_TABLE` / `_LL_FOLLOW` /
  `_PRODUCTION_IS_LEFT_EMPTY`；外加两边共用的 `_EXPECTED` / `_EXPECTED_FALLBACK` /
  `_SYNC_TOKENS` / `_HAS_*` / `_ERROR_TOKEN_ID` 与 `%tree` 的 `_TREE_PROD_*` /
  `_TREE_NONTERM_NAMES`；
- 解析器类：`class <类名>`（`%class` / `--class`，缺省 `Parser`），方法里有
  `run_text(text)` / `run(tokens)` / `parse_tree(text)`；容量是**类属性**
  （`max_stack_depth` / `max_errors` / `error_recovery_tokens`，分别对应
  `%max_stack_depth` / `%max_errors` / `%error_recovery_tokens`）；错误读
  `last_error`（一个 `ParserError`，有 `code` / `line` / `column` / `message`）
  与 `errors`（列表），结果读 `result`。
- 用户代码：`%code ... %endcode` 的正文插在 `import sys` 之后、
  状态码常量之前（逐块归一化后拼接），`%epilogue` 追加到模块末尾，
  `%main` 替换 `main.py` 的演示体。

`source/templates/python_*.tmpl` 的清单（**报告 / 版面类的文本全在模板里**：
生成器只算数据 + 填值，`code_gen_python.cpp` 里已没有产物字面量的拼接）：

| 模板 | 作用 |
| --- | --- |
| `python_parser.py` | `parser.py` 骨架：模块 docstring（解析 / 词法两种用法由 `{% if lex %}` 选）、状态码、`Token` / `Tree`（整段由 `{% if tree_class %}` 决定）/ `ParserError`、表与引擎的插入点 |
| `python_engine.py` | 解析器类：容量属性、`run_text` / `run` / `parse_tree`、内部小结构（`_Holder` / `_LlFrame`）、`_lex_regex_match` 与建树辅助 |
| `python_lexer.py` + `python_lexer_*.py` | 词法器与各词法块（固定字面量 / 字符串 / 数字 / 正则 / 标识符），与 C 目标的块一一对应；正则走 Python 的 `re`（见 §10.4） |
| `python_lr_action.py` | `_apply_action(production, values, result)`：语义动作按产生式编号分派 |
| `python_lr.py` | LR 栈 / 移进归约 / `error` 产生式与 `%sync` 恢复 / 主循环 |
| `python_ll.py` | LL(1) 驱动与建树（帧栈复刻 C 目标的语义帧） |
| `python_main.py` + `python_demo_main.py` | `main.py` 外壳与内建演示体（读标准输入一行后解析） |
| `python_lr_tables.py` / `python_ll_tables.py` / `python_tree_tables.py` | 分析表 / 语法树表三节：节标题 + 前导空行 + 静态说明 + `_ACTION = ` 这些行；`{% if %}` 决定 LR(k) / LL(k) / FOLLOW 小节是否出现 |
| `python_int_array.txt` / `python_int_rows.txt` | 整数表的排版（每行几个、续行缩进、`[[…], […]]` 的方括号）：`{% for %}` + `PyIntItem` / `PyIntRow` |
| `python_lex_rules.py` / `python_lex_skip_chars.py` / `python_keywords.py` / `python_lex_regexes.py` / `python_lex_constants.py` | 词法表这一节（`# ---------- 词法表 ----------` 标题在 `python_lex_rules.py` 里）与词法模式的常量段 |
| `python_constants.py` / `python_token_types.py` / `python_misc_tables.py` | 容量常量段（`_EXPECTED` 每行走 `PyExpectedRow`）、Token 类型段、`%sync`/`error` 与非终结符名表 |
| `python_action_cases.py` / `python_expand_cases.py` | `_apply_action` 的 `if/elif` 链与 LL 的 `_ll_expand` 分支：12 空格缩进、`.gram` 行号注释、逆序压栈都在模板里 |
| `python_demo_commit_print.py` / `python_demo_tree_print.py` / `python_run_loop_method.py` / `python_grammar_line_marker.txt` | 演示 main 的两段片段、`_run_loop` 外壳、注释里的 `.gram` 行号 |
| `python_user_code_block.txt` / `python_main_block.txt` | 用户正文块与 `%main` 块的「标题 + 正文」；**故意不以换行收尾** |
| `result_run_section_python.md` / `result_tree_section_python.md` | `RESULT.md` 的「运行」段与 `%tree` 语法树小节 |

### 9.2 语义动作的 Python 方言

`ActionTranslator` 构造时多一个 `python` 开关（`translateAction(...)` 的对应参数），
打开后同一个 Pratt 解析器产出 Python 表达式，而不是 C 表达式：

| 动作写法 | C / C++17 生成 | Python 生成 |
| --- | --- | --- |
| `$$` | `result.value`（或 `$$.<字段>`） | `result.value`（`_apply_action` 的出参 `result`） |
| `$1` `$2` …（LR） | `stack->values[stack->top - n + 1]` | `values[n-1]`（右部切片副本） |
| `$1` `$2` …（LL） | 帧槽位 | `values[n]` |
| `&&` / `\|\|` / `!` | 原样 | `and` / `or` / `not`（也接受直接写 `and` / `or` / `not`；`or` / `and` 的优先级按 Python 语义排序） |
| 语句结尾 | `;` | 不带分号 |
| `yyerrok` / `yyclearin` | `Parser_LrYyErrOk()` / `Parser_LrYyClearIn()` | `self._yy_err_ok()` / `self._yy_clearin()` |

因为语义值就是普通 Python 对象，`values` 传的是**右部语义值的切片副本**：
只读 `$n` + 写 `$$` 的动作（示例文法都是这种写法）完全够用；想直接改栈上的值
（`$$ = $1 + 1` 之外还回写 `$1`）不是 Python 产物支持的用法。

词法器给出的 Token 语义值约定：字符串 Token 是去掉引号并解好转义的 `str`，
数字 Token 是 `int` / `float`，标识符与固定字面量是词素文本，EOF 与 `error`
伪 Token 是 `None`。

### 9.3 用户代码与缩进归一化

Python 没有预处理层、也没有 `parser_user.h` 这个落点，正文直接进 `parser.py`：

```
import sys

# ---------- %code ... %endcode（用户正文） ----------

<归一化后的用户代码>

# ============ 状态码 ... ============
```

`CodeGenPython::normalizeUserCode()` 是这一段的唯一入口，`%code ... %endcode` /
`%code … %endcode`（`parserPy`，逐块做，块之间空一行）、`%epilogue` 与 `%main`
（`generatePythonProject` / `mainPy`）都走它。它做三件事：

1. 吃掉行尾 `\r`、去掉首尾空行；
2. 按所有非空行**最小**的前导空白宽度（tab 记 4 列）去掉公共缩进——文法里常常
   跟着排版缩进写，直接贴进模块顶层就是 `IndentationError`；
3. 剩下的行首 tab 换成 4 个空格（避免同一段代码里 tab / 空格混用）。

行首空白之外的字符一个都不动，所以多行字符串、注释里的内容原样保留。没有用户
代码时不插这一段。

### 9.4 生成期拒绝的写法与 `%class`

`CodeGenPython::checkPythonSupported`（`source/src/code_gen_python.cpp`）在生成前统一拦
这些 C 专属写法，报错带原文件行号、不静默忽略：`%value_type`（`valueType`）、
`%union`（`unionBody`，`%token <成员>` / `%type <成员>` 都要 `%union`，所以一起
拦住）、`%namespace`（`cppNamespace`）。命令行侧 `source/src/main.cpp` 还会更早拦一次，
好让错误消息更直白。

`%class NAME` / `--class NAME` 决定解析器类名，解析与校验在
`source/src/grammar_loader.cpp`（`GrammarConfig::pythonClass`，配 `pythonClassExplicit`
区分「写了空串」与「没写」）：先查 Python 关键字表（`pythonKeywords()`），再按
`[A-Za-z_][A-Za-z0-9_]*` 校验。配 `--lang c` / `--lang cpp17` 会直接报错
（要改 C++ 产物的名字用 `%namespace`）。产物文件名与类名无关，永远是
`parser.py` / `main.py`（`CodeGenPython::parserFileName` / `mainFileName`）。

### 9.5 与 C / C++17 的一致性

三份产物共用同一份生成期数据（ACTION / GOTO / 词法表 / 动作翻译），所以同一份
文法（不含上面那几条 C 专属写法时）在同样输入下的输出逐字一致：错误码、行列号、
错误消息文本、`%tree` 的树形文本都对得上。用户正文都**没有
`#line`**（Python 报错靠自己的 traceback），演示程序在 `result` 不是 `None` 时
会多打一行 `解析结果: <repr>`（C 演示要用户在 `%code ... %endcode` 里定义
`PARSER_USER_DEMO_PRINT_VALUE` 才打）。示例：`example/python_demo.gram`。

### 9.6 LR(k) / LALR(k)：第二层 ACTION 表与 k Token 窗口

`%mode lrk` / `%mode lalrk` 且 k > 1 时，三份后端都按同一套办法生成（k = 1 时
生成的代码与从前一样，第二层表和窗口都不会出现）：

| 后端 | 表（`code_gen*.cpp` 生成） | 运行时（模板） |
| --- | --- | --- |
| C99 | `s_ParserAction`（移进）+ `s_ParserReduce`（归约/接受）+ `s_ParserLookaheadCode` / `s_ParserLookaheadColumn`（列索引） | `source/templates/c99_custom_lr_runtime.c.tmpl`、`c99_custom_lr_item_types.c.tmpl` |
| C++17 | `kAction` + `kReduce` + `kLookaheadCode` / `kLookaheadColumn` | `source/templates/cpp17_lr_loop.cpp.tmpl`、`cpp17_core.cpp.tmpl` |
| Python | `_ACTION` + `_REDUCE` + `_LOOKAHEAD_CODE` / `_LOOKAHEAD_COLUMN` | `source/templates/python_lr.py.tmpl`、`python_engine.py.tmpl` |

要点：

- **列不是「Token 数 ^ k」**：只给文法里真正用到的 k 元组建列，编码成整数后按序
  排列、运行时二分；所以 k = 3 也不会把表撑爆。
- **窗口头永远是「当前 Token」**：`current`（C++17 是 `m_current`、Python 是
  `_current`）加上后面 k-1 个 Token。`yyclearin`、错误恢复里「丢弃 lookahead」、
  主循环前进全都只是挪窗口，`%tree` / Token 数组模式 / 文本流模式共用一套。
- **取词法单元可能提前**：文本流模式下窗口会预先多取 k-1 个 Token，所以词法错误
  可能比 k = 1 时更早报出来（这是 k > 1 的必然结果）。
- **错误恢复只查移进层**：`error` 伪 Token 与 `%sync` 同步 Token 的移进都直接查第一
  层（`Parser_LRCanShiftSyncToken` 判据也是它）；panic reduce 则在 k > 1 时扫**全部**
  归约列——错误发生时窗口常常不在任何展望符集合里（那正是 LR(k) 提前发现错误的
  方式），只查当前窗口那一列会找不到任何可归约的项。
- 改这些模板后**必须**重跑 `python3 tools/embed_templates.py`。

### 9.7 LL(k) / LL(0)：预测分析表按 k 元组建列

LL 系的产物只有一张预测分析表，k 的变化全部落在**列轴**上（`code_gen*.cpp` 的
LL 表生成器 + `custom_ll_*` / `cpp17_ll.cpp` / `python_ll.py` 模板）：

| k | 列 | 运行时列号 |
| --- | --- | --- |
| 0（LL(0)） | 只有一列 | 恒为 0（不看 lookahead） |
| 1（LL(1)） | 每个 Token 一列（密表，与从前逐字节一致） | `current.type` |
| ≥ 2 | 文法里真正用到的 k 元组各一列 | 按窗口的 k 元组编码二分查列 |

与 LR 侧的区别：LR 是「移进层 + 归约层」两张表，LL 只有一张表，但列的含义
一样是 k 元组。两端共用的东西：

- **窗口**：`Parser_FillLookaheadWindow` / `Parser_AdvanceLookahead`（C）、
  `fillLookaheadWindow` / `advanceWindow`（C++17）、`_fill_lookahead_window` /
  `_advance_window`（Python）——LL 与 LR 各有一份，语义相同：窗口头永远是
  `current`，前进时整体左移再补一个新 Token。
- **列编码**：$\sum_i \mathrm{Token号}_i\cdot(\text{Token 个数})^i$（低位是窗口头），
  生成期排好序、运行时二分。

LL(0) / LL(1) 时列号直接算得出来（0 / Token 号），那两张列编码表仍会生成一个
占位元素，纯粹是为了让模板里的查表函数能编译（C 用 `#if` 分档，C++17 / Python
用运行时常量）。另外 LL 系的错误同步也用同一个列号：表里那一列有产生式就表示
「栈顶非终结符能取这个窗口」（FIRST，或可空非终结符的 FOLLOW——ε 产生式的那些
列）。非可空非终结符的 FOLLOW 不在预测表里，生成器另外发一张
`s_ParserFollow` / `kLlFollow` / `_LL_FOLLOW` 给 FOLLOW 集恢复用（见 §10 上面
「错误恢复」那一小节）。

## 10. 词法模式后端（`--mode lex` / `%mode lex`）

词法模式不跑分析层：`main` 直接跳过 `Analysis::*Analyze`（`request.analysis` 传
nullptr），产物由 `source/src/project.cpp` 的三个生成函数分头组装：
`generateLexProject()`（C）、`generateLexPythonProject()`（Python）、
`generateLexCpp17Project()`（C++17），三者共用 `lexTerminals()`（内置
Token + 文法声明的 + END）与 `generatorLexResultMd()`（RESULT.md 按语言给示例）。

| 目标 | 文件 | 由谁生成 |
| --- | --- | --- |
| C | `lexer.h` / `lexer.c` | `CodeGenC99::lexerOnlyH()` / `lexerOnlyC()` + `lexer_h_custom.h` / `lexer_c_custom.c` |
| C | `parser_token.h` / `.c` | 与解析产物同一套 `customParserTokenH/C` + `lexDfa` 的表 |
| C | `main.c` | `lexer_main_custom.c`（内建 CLI 在 `lexer_demo_main_custom.c`；写了 `%main` 就整段替换） |
| C++17 | `lexer.hpp` | `CodeGenCpp17::lexerHpp()` + 模板 `cpp17_parser.hpp`（标记换成词法版本） |
| C++17 | `lexer.cpp` | `CodeGenCpp17::lexerCpp()` + `cpp17_parser.cpp` / `cpp17_lexer.cpp` / `cpp17_core.cpp` / `cpp17_public.cpp` |
| C++17 | `main.cpp` | `cpp17_lex_main.cpp`（内建 CLI 在 `cpp17_lex_demo_main.cpp`） |
| Python | `parser.py` | `CodeGenPython::lexerPy()` + `python_parser.py` / `python_lex_engine.py` / `python_lexer.py` |
| Python | `main.py` | `CodeGenPython::lexerMainPy()` + `python_lex_main.py` / `python_lex_demo_main.py` |
| C# | `Parser.cs` | `CodeGenCsharp::lexerCs()` + `csharp_parser.cs` / `csharp_lex_engine.cs` / `csharp_lexer*.cs` |
| C# | `Program.cs` | `CodeGenCsharp::lexerMainCs()` + `csharp_lex_program.cs` / `csharp_lex_demo_main.cs` |
| Rust | `src/parser.rs` | `CodeGenRust::lexerRs()` + `rust_parser.rs` / `rust_lex_engine.rs` / `rust_lexer*.rs` |
| Rust | `src/main.rs` | `CodeGenRust::lexerMainRs()` + `rust_lex_main.rs` / `rust_lex_demo_main.rs` |
| Go | `parser.go` | `CodeGenGo::lexerGo()` + `go_parser.go` / `go_lex_engine.go` / `go_lexer*.go` |
| Go | `main.go` | `CodeGenGo::lexerMainGo()` + `go_lex_main.go` / `go_lex_demo_main.go` |
| 六档 | `Makefile` / `CMakeLists.txt` / `parser.csproj` / `Cargo.toml` / `go.mod` / `RESULT.md` | C 用 `Makefile_lex` / `CMakeLists_lex.txt`，C++17 用 `Makefile_lex_cpp17` / `CMakeLists_lex_cpp17.txt`（`liblexer.a` + `lexer_demo`），Python 不出构建脚本，C# 不出 Makefile（`dotnet build` 直接编译），Rust 用 `cargo build`（产物带一个 `regex = "1"`，第一次要联网），Go 用 `go build` |

要点：

- 词法器本体与解析产物**同一份生成器**（`generatorLexerC` + `customizeLexerText`），
  所以 `%space` / `%token regex` / `%identifier` / `%token <成员>` 的行为完全一致。
  唯一区别：`lexer.c` 里「Token 数组模式」那一段用模板标记
  `__LEX_ADVANCE_ARRAY_MODE__` 在词法产物里被裁掉——公开的 `ParserLexer_T` 没有
  `pTokens` / `tokenCount` / `tokenPosition` 这几个字段，留着编译不过（解析产物
  那一边这段的展开保持不变，模板改动对解析产物是零影响）。
- Token 表按「内置 Token（STRING / NUMBER / VARIABLE）+ 文法声明的 + END」去重
  排序，与解析产物的 `analysis.terminals` 同一套编号规则（Token 号 = 数组下标）。
- C++17 的词法产物复用解析产物的模板，只把「解析才有」的几段做成模板变量 /
  条件：`cpp17_parser.hpp.tmpl` 的 `{{ engine_parse_api }}`（解析产物填
  `runText` / `run`，词法产物填 `tokenize`）、`{{ tree_api }}` 与
  `{{ compat_api }}`（词法产物给空串）、`cpp17_parser.hpp.tmpl` 里
  `{% if lex %}` 决定的文档措辞；`cpp17_public.cpp.tmpl` 的
  `{{ engine_parse_impl }}` 与 `{% if parse_tree %}` / `{% if tree_class %}`
  （词法产物两个都关掉，所以 `Tree` 的定义与 `Engine::parseTree` 都不出现）、
  `cpp17_core.cpp.tmpl` 的 `{{ core_syntax_error }}`（期望集合那一段）与
  `{{ core_parse_entry }}`（解析入口）。解析产物把这些原样填回去，产物逐字
  不变；词法产物的 `Private` 里只有错误表 + 词法器 + `tokenize`，Tree / 兼容层
  都不生成。
- Python 侧把词法表抽成 `pyLexTables()` 共用，词法产物的 `parser.py` 只带词法表
  与「词法器 + `tokenize`」的引擎（`python_lex_engine.py`）；Tree 类与
  `_append_tree_text` 整段去掉（词法产物不该带语法树 API）。
- C / C++17 产物的 `#line` 占位符照旧由 `resolveLineDirectives()` 落地：
  `%code ... %endcode`、`%union`、`%main` 里的编译器报错同样能指回 `.gram`；`%epilogue`
  贴到 `lexer.c` 末尾；`%copy` 的文件进 `liblexer.a` 的源码列表。Python 词法产物
  与解析产物一样，用 `# 语法文件 … 行 N` 注释标出行号（见 §4.2）。
- 目标语言**六档都支持**：C 走 `generateLexProject()`、C++17 走
  `generateLexCpp17Project()`、Python 走 `generateLexPythonProject()`、C# 走
  `generateLexCsharpProject()`、Rust 走 `generateLexRustProject()`、Go 走
  `generateLexGoProject()`；六者的产物里都没有解析器（C++17 是 `lexer.hpp` /
  `lexer.cpp`，Python 是 `lexer.py`，C# 是 `Parser.cs`，Rust 是 `src/parser.rs` +
  `src/token.rs`，Go 是 `parser.go` + `token.go`），见
  [`08-generated-code.md`](08-generated-code.md) §12 / §15。

## 11. 改模板 / 加模板的流程

1. 改 `source/templates/*.tmpl`（或新增模板；新模板也统一用 `.tmpl`）。
2. 若新增了文件，要在对应的后端里接上（C99 是 `source/src/code_gen_c99.cpp`，其余后端各自
   一个 `code_gen_<语言>.cpp`；组装在 `source/src/project.cpp`）；
   至少重跑 `python3 tools/embed_templates.py` 让内联表包含它。
3. `make`；必要时按 [`09-build-and-test.md`](09-build-and-test.md) §4 的手工小测
   把受影响的示例生成 + 编译一遍。
4. 开发期想跳过内联，可 `SCRIPT_ENGINE_TEMPLATES=templates ./build/bin/ParseEngine ...`，
   但发行产物必须重新内联。

## 12. C# 语言后端（`--lang csharp` / `%lang csharp`）

C# 产物**不是**把 C 产物塞进 `.cs`，而是另一套模板（`source/templates/csharp_*`）与
另一个后端（`source/src/code_gen_csharp.cpp`）生成的：产物是 `Parser.cs` /
`Token.cs` / `Program.cs` / `parser.csproj` / `RESULT.md`。目标语言选择规则与
C / C++17 / Python 完全一致（命令行 `--lang` 覆盖文法里的 `%lang`，缺省
`cpp17`）。

### 12.1 产物形态

| | C# 产物（`--lang csharp`） |
| --- | --- |
| 文件 | `Parser.cs`（解析器类 + 常量 + 表 + 运行时）/ `Token.cs`（Token 类型常量 + `Token` 类）/ `Program.cs`（演示入口）/ `parser.csproj`（`net8.0` 构建脚本）/ `RESULT.md` |
| 类型名 | `Parser` / `Token` / `Parser.Tree` / `Parser.ParserError`（首字母大写） |
| 解析入口 | `Parser engine = new Parser(); engine.runText("...")` |
| 错误 | `engine.errors()` / `engine.lastError()`（`Parser.ParserError`，`ToString()` 直接出「第 N 行第 M 列：…」） |
| 语法树 | `Parser.Tree`（字段 `Name` / `Text` / `Children`，`Print()` 出缩进文本） |
| 语义值 | `%value_type` 原样填进产物；`%union` 生成一个带成员字段的顶层类 `ParserValue`；都没写时是 `object` |
| 内存 | 全部由 GC 托管，没有分配器、没有释放钩子（也就不支持 `%destructor`） |
| 报错定位 | 没有可移植的 `#line`：五处「来自 `.gram` 的代码段」前后各插一行 `// 在 <文法文件> 的第 <N> 行` / `// 继续 <产物文件> 的正文` 注释 |

入口类按 C# 惯例叫 `Program`（C# 不允许成员名与类名相同，所以入口类不能叫
`Main`）。

### 12.2 语义值类型

`__VALUE_TYPE__` 由生成期决定，填进运行时（数组、`_Holder`、`Token.value`）：

| 写法 | C# 里的语义值类型 | 说明 |
| --- | --- | --- |
| `%value_type double` | `double`（原样填） | 动作里 `$$ = $1 + $3` 就是 C# 的加法，编译期就能查类型 |
| `%union { int ival; }` | 生成类 `ParserValue` | 成员声明原样贴进类（请按 C# 的类型写），`$n` / `$$` 展开成 `values[i].ival` / `result.value.ival` |
| 都不写 | `object` | 词法器给出 `int` / `double` / `string`；动作里的算术要自己转型 |

词法器把「Token 词素 → 语义值」集中在两个生成出来的静态方法里：
`_valueFromText(type, text)` 与 `_valueFromNumber(number, isFloat)`。模板侧统一
写 `item.value = _valueFromText(...)`。

### 12.3 用户代码的落点

| 写法 | 落点 |
| --- | --- |
| `%code ... %endcode` | `Parser.cs` 的 `using` / `namespace` 之后、`class Parser` 之前（**文件级**：里面请写 C# 类 / 接口） |
| `%class_body` … `%endbody` | `Parser` 类内部（辅助方法 / 字段 / 内部类） |
| `%epilogue` | `Parser.cs` 末尾（全局作用域） |
| `%main` | `Program.cs` 的 `Program` 类成员（内建 demo 整段替换） |
| `%package` / `%import` | 文件级 `namespace NAME;` / `using NAME;`（可重复；`using` 排在 `namespace` 之前） |

## 13. `%commit` 与位置信息 API 的生成细节（六档）

`%commit` / `%trivia` 与 `ParserLocation` / `current_location` 在两个层上落地：
**生成期**把规则编成表、把「词法声明序」写进表；**运行时**用同一套最长匹配逻辑
决定「这段文本是 Token、收进容器、还是丢掉」，并维护当前 lookahead 的位置。
六档的**算法**一致，但**正则由谁来匹配**不一样：

| 后端 | 正则匹配器 | 产物里的表 |
| --- | --- | --- |
| C99 | 等价普通模式使用 DFA，其余使用共享有序运行时 | `s_ParserRegexDfas` / `kLexClassMap` 那套字符类划分 |
| C++17 `std`（**缺省**） | 标准库 `<regex>`（ECMAScript） | `kRegexRes` / `kIdentifierRe` / `kCommitRes` + `lexRegexMatch()` |
| C++17 `vm` | 全规则使用共享有序运行时，支持资源预算 | `PeRegexMatchEx()` |
| C++17 `dfa` | 仅已证明等价的 ASCII 子集使用 DFA，其余回退标准库 | `kLexClassMap` / `kLexPageIndex` / `kLexPageData` + 每条规则的 `trans` / `accept` 表 + `lexDfaScan()` |
| Python | 标准库 `re` | `_REGEX_RULES` / `_IDENTIFIER_RE` / `_COMMIT_RES` + `_lex_regex_match()` |
| C# | `System.Text.RegularExpressions.Regex` | `_REGEX_RE_LIST` / `_IDENTIFIER_RE` / `_COMMIT_RE_LIST` + `_lexRegexMatch()` |
| Rust | Rust 官方的 `regex` crate（`Cargo.toml` 里的 `regex = "1"`） | `_REGEX_RES` / `_IDENTIFIER_RE` / `_COMMIT_RES` + `lex_regex_match()`（`OnceLock` 里各编译一次） |
| Go | 标准库 `regexp`（RE2） | `sParserRegexRes` / `sParserIdentifierRe` / `sParserCommitRes` + `sParserLexRegexMatch()`（`regexp.MustCompile` 在包级变量初始化时各编译一次） |

**C++17 缺省把正则交给 `std::regex`**（用 `--cpp-lexer dfa` 或 `%cpp_lexer dfa`
启用等价子集的 DFA 快速路径，其余模式自动回退标准库；`--cpp-lexer vm` 对全部规则使用有序运行时）；**C99 对普通模式使用 DFA，上下文语法使用内嵌有序正则运行时**，其余各档把正则原样交给各自
语言的正则库。

**生成期校验与报错口径**：六档的正则都在**文法加载时**（`readRegexPattern`）用
生成器自带的编译器（`source/src/regex_dfa.cpp`）验一遍，但**认不下的处理不一样**：

- **C99 / C++17 `vm`**：需要使用自带编译器生成不可变程序，所以「正则语法有误 / `{m,n}` 上限 1000」
  是**硬错误**（`main.cpp` 按目标语言与词法引擎分流，报错文案与从前逐字相同）；
- **C++17 `std` / `dfa` 回退路径 / Python / C# / Rust / Go**：只打一条**警告**（`GrammarConfig::regexDiagnostics`
  → `main.cpp`），生成照常往下走——自带编译器只是「顺手校验一遍」，它不认识不等于
  目标语言不认（`[0-9]{1,2000}` 这类超出上限的写法就是典型：`std::regex` / `re` /
  .NET 的 `Regex` / `regex` 都收）。
- **「正则能匹配空串」与引擎无关**，六档一律是硬错误（Lexer 在哪个语言里都没法前进）。

### 13.1 词法匹配（六档同一套顺序）

1. 取 Token 前先跳 `%space` 空白；
2. 在「固定字面量 + `%token regex` + `%commit` / `%trivia`」里做最长匹配：
   字面量先按长度降序试，正则与 commit 各自从当前位置起匹配，再在规则之间比较长度
   （C99 与 C++17 `dfa` 查 DFA 表；C++17 的两个后端保留 ECMAScript 的分支顺序与非贪婪语义；其他语言使用各自的正则语义，
   并且都要求「必须从当前位置起」——C++17 用 `match_continuous`、
   Python 的 `pattern.match` 本来就是锚定的、C# 额外校验 `Match.Index == start`、
   Rust 用 `Regex::find_at` 再确认 `m.start() == start`、Go 用
   `FindStringIndex` 再确认 `loc[0] == 0`）；
3. **同长度时按「词法声明序」比较**：`%define` / `%token` / `%token regex` /
   `%commit` / `%trivia` 共用一个自增序号（生成期写进表：C 是
   `ParserLexRule_T::order` / `s_ParserRegexOrders` / `s_ParserCommitOrders`，
   C++17 是 `LexRule::order` / `kRegexOrders` / `kCommitOrders`，Python 是
   `_LEX_RULES_ORDER` / `_COMMIT_RES` 的第 4 项，C# 是 `_LEX_RULES_ORDER` /
   `_COMMIT_ORDERS`，Rust 是 `_LEX_RULES_ORDER` / `_REGEX_ORDERS` /
   `_COMMIT_ORDERS`，Go 是 `sParserLexRulesOrder` / `sParserRegexOrders` /
   `sParserCommitOrders`）；
4. commit 命中就吃掉原文：`skip`（`%trivia` / `%commit skip`）丢掉，其余
   `append{line, type, text}` 收进容器，然后**继续取下一个 Token**；
5. 六档各留一个最小匹配入口：C99 的 `Parser_LexDfaMatch`、C++17 的
   `lexRegexMatch`、Python 的 `_lex_regex_match`、C# 的 `_lexRegexMatch`、
   Rust 的 `lex_regex_match`、Go 的 `sParserLexRegexMatch`。

### 13.2 容器与位置 API 的六档表达

| | C99 | C++17 | Python | C# | Rust | Go |
| --- | --- | --- | --- | --- | --- | --- |
| 容器元素 | `ParserCommit { int line; const char *type; char *text; }` | `ParserCommit`（`std::string` 字段） | `ParserCommit` namedtuple | `parser.Commit`（`final` 字段） | `ParserCommit`（`line` / `kind` / `text`） | `ParserCommit`（`Line` / `Type` / `Text`） |
| 容器 | `ParserCommitList`（`items` / `count` / `capacity`） | `std::vector<ParserCommit>` | `list[ParserCommit]` | `List<Commit>` | `Vec<ParserCommit>` | `[]ParserCommit` |
| 取容器 | `Parser_GetCommits()` | `Engine::commits()` | `engine.commits` | `engine.Commits` | `engine.commits()` | `engine.Commits()` |
| 释放 | `Parser_FreeCommits()`（`text` 是 `malloc`；`type` 静态、不要 free） | 标准库（无） | 引用计数（无） | GC（无） | 所有权（无） | GC（无） |
| 位置类型 | `ParserLocation` | `ParserLocation` | `ParserLocation` namedtuple | `Parser.Location`（`readonly struct`） | `ParserLocation`（三个字段） | `ParserLocation`（三个字段） |
| 取位置 | `Parser_CurrentLocation()` | `Engine::currentLocation()` | `engine.current_location()` | `engine.CurrentLocation` | `engine.current_location()` | `engine.CurrentLocation()` |
| Token 偏移 | `ParserToken_T::offset` | `Token::offset` | `token.offset` | `Token.offset` | `token.offset` | `Token.Offset` |
| 树节点位置 | `Parser_TreeGetLocation(node)` | `Tree::location()` | `Tree.location` | `Tree.Location` | `Tree::location()` | `Tree.Location()` |

细节：

- **offset 一律是 UTF-8 字节偏移**：C99 / C++17 直接在字节缓冲上走；
  Python 每一字符累加 `len(ch.encode("utf-8"))`、Go 累加 `len(string(ch))`；
  C# 的字符串是 UTF-16，所以按「ASCII 1 / 两字节 2 / 三字节 3 / 代理对 4」累加。
  六档都与 C99 的字节偏移对齐。
- **树节点位置**：叶节点是 Token 起点；分支节点取第一个有位置的子节点，
  六档都按需计算（先建父节点再填子节点也对）；都没有时是 0（C# 的
  `tree.location()` 返回 `null`）表示未知。
- `$line` / `$column` 六档都改读位置 API（C 是 `Parser_CurrentLocation()`，
  C++17 是 `currentLocation()`，Python 是 `current_location()`，
  C# 是 `CurrentLocation`，Rust 是 `current_location()`，Go 是 `CurrentLine()` /
  `CurrentColumn()`——后几个内部都读位置 API）——取值与从前一致。
- 词法模式的产物（`%mode lex`）：C99 暴露
  `Parser_LexGetCommits(&lexer)` / `Parser_LexFreeCommits(&lexer)`；C++17 / Python /
  C# / Rust / Go 的词法器实例上也带同一组 `commits` / `location` 接口
  （Rust / Go 是 `Commits()` / `CurrentLocation()`，词法器的 `Parser` 里只有
  错误表 + 词法器 + `tokenize`）。

### 13.3 %commit 的产物表（生成期）

| 目标语言 | 词法规则表字段 | commit 表 |
| --- | --- | --- |
| C99 | `ParserLexRule_T::order` | `s_ParserCommitDfas` / `s_ParserCommitTypes` / `s_ParserCommitSkips` / `s_ParserCommitOrders`（`PARSER_COMMIT_COUNT`） |
| C++17 | `LexRule::order` | `kCommitRes` / `kCommitTypes` / `kCommitSkips` / `kCommitOrders`（`kCommitCount`） |
| Python | `_LEX_RULES` 第 5 项 | `_COMMIT_RES`（`(正则, 类型名, 是否 skip, 声明序)`） |
| C# | `_LEX_RULES_ORDER` | `_COMMIT_RE_LIST` / `_COMMIT_TYPES` / `_COMMIT_SKIPS` / `_COMMIT_ORDERS` |
| Rust | `_LEX_RULES_ORDER` | `_COMMIT_RES`（`OnceLock<Vec<Regex>>`）/ `_COMMIT_TYPES` / `_COMMIT_SKIPS` / `_COMMIT_ORDERS` |
| Go | `sParserLexRulesOrder` | `sParserCommitRes`（`[]*regexp.Regexp`）/ `sParserCommitTypes` / `sParserCommitSkips` / `sParserCommitOrders` |

没有声明 `%commit` / `%trivia` 时，产物行为与从前一样（C# 与 Python 连 `count` 都不用，
直接靠空表 / 空数组；C++17 / C99 用一张 1 元素的占位表 + `count = 0`）。
**Go 档更省**：它把「有没有 `%commit`」做成模板开关——没有的话那段匹配代码整段
不生成，表也不发（`sParserCommitRes` 之类只在真有 `%commit` 时才出现）。

## 14. 文法导出档（`--lang gram`）

最后一个「目标」不是一门编程语言，而是**只要文法与分析报告、不要代码**的导出档：
产物目录里只有三样东西（外加 `%copy` 的文件），没有解析器、没有词法器、也没有
`Makefile` / `parser.csproj` 这类构建脚本。

| | 文法导出档（`--lang gram`） |
| --- | --- |
| 产物 | ① 输入的那份 `.gram`（**逐字节**导出，文件名沿用输入文件；没给文件时是内建的 `default_grammar.gram`，导出名 `grammar.gram`）② 它的分析报告 `<文法名>.analysis.md`（`pl0.gram` → `pl0.analysis.md`）③ `RESULT.md`（生成信息 + 产物清单 + 怎么用），外加 `%copy` 的文件 |
| 组装 | `source/src/project.cpp` 的 `generateGramProject()` + `generatorGramAnalysisMd()` / `generatorGramResultMd()`（`Project::generateParserProject` 在模式分岔之前先走这一条） |
| 模式 | 16 种模式均可导出；报告随模式变化（LL 预测表 / LR 状态机与 ACTION-GOTO / GLR 多路径 / Earley-CYK 图表 / `lex` Token 表），详见 [支持矩阵](15-language-mode-support.md) |
| 报告 | `<文法名>.analysis.md` = 生成信息（源文法 / 导出文件 / 模式）+ 冲突消解 + 未消解冲突 + `--report` 那份分析报告全文（`request.reportText`，与 `--report` 逐字一致）；`RESULT.md` 只讲产出了什么、怎么用，不重复贴报告 |
| 不适用指令 | `%namespace` / `%class` / `%package` / `%import` / `%class_body` / `%destructor` / `%cpp_only` 在这一档全部**提示一句并忽略**（不生成代码，没有落点；`%cpp_only` 只约束代码生成） |

它解决的问题：产物目录里只有生成出来的源码时，「这份代码是从哪份文法来的、
当时的分析结果是什么」只能回去翻命令行。导出档把这两样钉在一起，方便归档 /
提交 / 交接给别的工具；分析报告单独成文件、和 `.gram` 成对（`pl0.gram` ↔
`pl0.analysis.md`），报告内容就是 `--report` 那份，不用再单独存。

```sh
# 只要文法 + 分析报告（默认 ielr）
ParseEngine example/pl0.gram --out-dir /tmp/pl0_gram --lang gram
ls /tmp/pl0_gram      # pl0.gram  pl0.analysis.md  RESULT.md

# 换模式再导一次：报告就是那一套分析（这里看 LALR 的冲突）
ParseEngine example/ielr_demo.gram --out-dir /tmp/ielr_gram --lang gram \
    --mode lalr --teach
```

### C++ DFA 与标准库一致性回归

运行 `python tests/cpp_dfa_std.py`。该回归比较 `--cpp-lexer std` 与 `dfa`
的实际词法输出、退出状态和位置，覆盖分支、非贪婪、范围重复、Unicode、断言、
否定字符类、非法 UTF-8 和空白。C++ DFA 快速路径逐字节扫描，避免把过长 UTF-8 编码误解为 ASCII。可证明等价的 ASCII 模式继续生成 DFA；其余模式整份回退到
`std::regex`，包括 `%identifier`、`%commit`、`%trivia`，避免不同规则使用不同语义。

C99 的普通 DFA 路径继续使用最长接受前缀语义；扩展模式使用有序运行时，详见 [17 扩展正则表达式](17-regex-extensions.md)。C/C++ 共用编译器已修复 `{m,n}`
误变成 `{m,}`、`{m,}` 被误报上下限错误、范围重复丢失非贪婪标记和 `\0` 被误拒绝。
回归同时直接将共用编译器的 DFA 完整匹配结果与 `std::regex_match` 对照，验证重复边界。

C++ 显式 Unicode 规则使用与 C99 共用的内嵌有序运行时，可与标准库字节规则混用，见 [18 Unicode 正则模式](18-regex-unicode.md)。

正则的步数、回溯、工作内存、前瞻深度、取消与结构化失败状态见 [20 正则运行时资源预算](20-regex-runtime-budgets.md)。
