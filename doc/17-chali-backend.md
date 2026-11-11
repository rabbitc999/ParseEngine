# ChaLi 目标语言

`--lang chali` 或文法中的 `%lang chali` 生成原生 ChaLi 源码 `parser.chl` 与
`RESULT.md`。支持全部 16 种生成模式，默认使用 IELR。

| 类别 | 模式 |
| --- | --- |
| 预测分析 | `ll0`、`ll1`、`llk` |
| LR | `lr0`、`slr`、`slrk`、`lalr`、`lalrk`、`lr1`、`ielr`、`lrk` |
| 多路径与图表 | `glr`、`earley`、`cyk` |
| 回溯 | `dd` |
| 纯词法 | `lex` |

- LL(k)、LR(k)、SLR(k) 和 LALR(k) 使用 `%lookahead K` 或 `--lookahead K` 配置窗口，默认是 2；上限取决于该文法的终结符数量和 64 位展望符编码。也可写 `--mode "LL(2)"` 或 `--mode "LR(2)"`。输入末尾用 END 补齐。
- GLR 遍历分析表保留的冲突动作，路径共享栈前缀。只对成功派生执行语义动作，每个保留结果各执行一次。
- GLR 的 `ParserResult.alternatives` 包含成功派生，每项有 `value` 和 `tree`。`ParserResult.value` 和 `tree` 对应首项。有路径超出 `%max_stack_depth` 时，`truncated` 为真。
- Earley 在原产生式上做预测、扫描和完成，CYK 用 CNF 规则判定区间；两者通过后都复用 GLR 重建原文法派生。三种多路径模式均拒绝不消费 Token 的归约环。
- DD 按产生式声明顺序回溯，选择首个完整匹配，只执行该派生的语义动作。DD 在生成时拒绝左递归。

```sh
ParseEngine example/chali_lr1.gram --lang chali --mode ielr --out-dir out/chali
chali run out/chali/parser.chl
# 输入 1+(2*3)，得到 7
chali build out/chali/parser.chl --out parser
```

LL(1) 示例见 [`chali_ll1.gram`](../example/chali_ll1.gram)，LR(1) 系列示例见
[`chali_lr1.gram`](../example/chali_lr1.gram)。语义动作中的代码须为 ChaLi；
原有 C/C++ 文法中的 `#include`、宏和函数调用不会自动翻译。

## API 与语义值

- `Parser::tokenize(String)` 返回 `ParserLexResult`，其中 `tokens` 在成功时以 END 结尾，`comments` 保存保留的注释，失败时可读 `message` 和字节 `offset`。纯词法产物提供这个入口。
- `Parser::runText(String)` 完成词法分析并解析完整输入。
- `Parser::runTokens(List<ParserToken>)` 接受显式 Token 序列，必须恰有一个位于末尾的 END。
- `Parser::tokenKind(String)` 查询终结符编号，未知名称返回 -1。 `Parser::tokenName(int64)` 查询名称，编号无效时返回空串。
- `ParserToken` 包含 `kind`、`text`、`offset`、`value`。
- `ParserResult` 包含 `ok`、`value`、`tree`、`message`、`offset`。
  失败偏移以 UTF-8 字节计；解析在首个错误处停止。

默认语义值为 `float64`，NUMBER 自动转换为数值；其他 Token 取类型缺省值。
`%value_type` 可声明 ChaLi 类型。自定义类型的 Token 值由 `runTokens` 调用方填写，
`runText` 不为自定义类型隐式转换数值。动作支持 `$$` 与从 1 起的 `$n`，
分别展开为结果值与右部值；字符串、字符字面量和注释中的 `$` 保持原样。
无动作时取 `$1`，空产生式取类型缺省值。

`%code`、`%epilogue` 和 `%main` 原样嵌入同一个 `.chl` 文件；未指定 `%main` 时，
默认入口从标准输入读取一行并输出结果。`%tree` 使默认入口输出括号形式的语法树；
`ParserResult.tree` 保留非终结符名称及按输入顺序排列的子树。
`%max_stack_depth` 限制 LL 递归深度或 LR 状态栈深度。

## 词法与边界

支持逐项声明的字面量 `%token`、`%token regex`、`%identifier`、`%space`、
`%trivia` 和 `%commit skip`。规则按最长匹配选择，同长度按声明顺序；
显式规则优先于内置 NUMBER、VARIABLE、STRING 规则。字面量中的 `\b` 为空白通配。
匹配规则须至少消费一个字符。用 `%trivia keep "模式"` 收集注释，在 `ParserResult.comments` 中读取原文和起始 UTF-8 字节偏移。
默认内置标识符会将 `abc` 识别成一个 VARIABLE，
单字母关键字组成的测试句子须用空格分隔。

正则使用 ChaLi `std::regex::Regex` 的原生 ECMAScript 规则，生成期检查表达式能否编译，
报告标识实际执行引擎；不支持共享 Unicode 属性和 `(?u)` / `(?i)` 前缀。
`--strict-regex` 继续执行现有严格兼容性检查。

当前明确拒绝 `%union`、成员标注、`%destructor`、`error` 产生式、`%sync`、
带值 `%commit`、`%class_body`、`%package`、`%import` 和 `%copy`。
LL 模式要求文法直接满足所选预测条件，暂不执行自动左递归改写后的语义动作。
没有增量解析 API，也不生成 Make/CMake 工程文件。
所有运行时模板随生成器内嵌，生成与运行均无需读取模板目录。

## 回归验证

```sh
make -C source -j4
python tests/chali_backend.py --chali /path/to/chali
```

测试编译并执行十六种模式的产物，覆盖 k 元组决策、GLR 多条歧义派生、语义动作、优先级、嵌套、空产生式、
IELR 拆分、最长匹配、声明优先级、trivia、语法树、Token API、自定义类型与用户代码，
并检查错误输入、越界值引用与不支持模式的拒绝行为。
