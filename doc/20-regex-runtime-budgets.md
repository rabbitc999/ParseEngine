# 正则运行时资源预算与结构化结果

共享有序正则运行时和 C99 DFA 路径现在明确区分匹配、不匹配、超限、取消和系统分配失败。生成的词法器遇到资源错误会立即停止规则比较；不会把它当成不匹配，再选择较短 Token。解析入口保留资源细分码，递归下降不会为此回溯尝试其他产生式。

## 支持范围

| 路径 | 可配置预算 | 说明 |
| --- | --- | --- |
| C99 DFA | 步数、取消 | 没有动态工作内存和回溯栈；对应计数为零 |
| C99 有序运行时 | 全部 | 前瞻、捕获、反向引用和 Unicode 共用预算 |
| C++17 `--cpp-lexer vm` / `%cpp_lexer vm` | 全部 | 全部显式正则规则在生成期编译到共享运行时 |
| C++17 全部显式正则都带 `(?u)` | 全部 | 无论选择 `std` 还是 `dfa`，Unicode 规则都使用共享运行时 |
| C++17 含标准库字节正则或原生 DFA 路径 | 不接受统一配置 | `kRegexBudgetSupported` 为 false，`setRegexLimits` 返回 false，不改变原配置 |
| Python、C#、Rust、Go、Java、JavaScript 原生正则库 | 尚无此统一 API | 不能把目标库内部限制表述为共享运行时预算 |

`std` 仍为 C++17 默认配置。需要对所有显式正则提供预算保证时，选择 `vm`；该选项要求规则能被生成器的正则编译器识别，无法识别会在生成期报错。混用标准库字节规则与 Unicode 规则时，Unicode 部分仍有默认预算，但不接受声称覆盖全部规则的配置。

预算针对**单条规则的一次前缀匹配**，不是整份文件或整个解析器的资源上限。固定字面量、内建字符串/数字、输入副本、语法栈、语义动作和 CST 不计入正则预算。需要跨规则的总体取消策略时，可在取消函数的调用方上下文中维护计数或截止条件。

## 配置和默认值

公开结构体 `PeRegexLimits`（C++ 别名 `RegexLimits`）包含：

| 字段 | 默认值 | 含义 |
| --- | --- | --- |
| `maxSteps` | 1,000,000 | 指令执行和反向引用字符比较的总数 |
| `maxMemory` | 64 MiB | 请求的工作内存，包含捕获、空循环标记、回溯快照、栈容量及分配头 |
| `maxBacktracks` | 100,000 | 实际弹出备选状态、回退执行的次数 |
| `maxStates` | 100,000 | 同时保留的待处理回溯状态数量 |
| `maxLookaheadDepth` | 64 | 前瞻嵌套层数，不把最外层匹配算作前瞻 |
| `cancel` / `userData` | 空指针 | 取消函数和调用方上下文，返回非零即取消 |

零数值字段表示使用默认值；需要提高限制时显式给出较大值。嵌套前瞻共享外层的计数和工作内存，不能通过负前瞻吞掉资源错误。Unicode 反向引用按比较的码点消耗步数。工作内存计数不包含 C 分配器内部元数据和碎片，因此不等同于进程 RSS。

取消函数在执行检查点调用，不得重入当前匹配、抛出异常或通过 `longjmp` 跳出；跨线程取消时，调用方应使用原子状态或相应同步机制。它不改变目标正则原有的大小写、捕获、优先级和匹配顺序。

## 状态与结果

`PeRegexResult`（C++ 别名 `RegexMatchResult`，状态类型别名为 `RegexStatus`）包含 `status`、`byteLength`、`steps`、`peakMemory`、`backtracks` 和 `peakStates`：

| 匹配状态 | 值 | 解析入口对应状态 |
| --- | --- | --- |
| `PE_REGEX_MATCH` | 1 | 规则匹配成功；整次词法/解析成功仍返回 `PARSER_OK` / `Status::Ok`（0） |
| `PE_REGEX_NO_MATCH` | 0 | 规则未匹配，继续尝试其他规则 |
| `PE_REGEX_LIMIT` | -30 | `PARSER_ERR_REGEX_LIMIT` / `Status::ErrRegexLimit` |
| `PE_REGEX_CANCELLED` | -31 | `PARSER_ERR_REGEX_CANCELLED` / `Status::ErrRegexCancelled` |
| `PE_REGEX_NO_MEMORY` | -32 | `PARSER_ERR_REGEX_MEMORY` / `Status::ErrRegexMemory` |

零宽成功通过 `status == PE_REGEX_MATCH && byteLength == 0` 表达，与不匹配区分。文法中的零宽词法规则仍由加载器拒绝，防止词法器不前进。资源失败时 `byteLength` 为零，不把未完成的搜索结果作为可信前缀返回。

最近结果是**最近尝试的一条规则**的统计，不一定是最后获胜的 Token 规则，也不是整次解析的总计。一旦发生资源失败，该结果保持到下一次解析初始化；下一次完整解析清除结果并保留预算配置。增量缓存直接命中时可以不执行匹配，此时没有新的匹配统计。

## C++17

```cpp
Parser::Engine engine;
Parser::RegexLimits limits{};
limits.maxSteps = 500000;
limits.maxMemory = 8 * 1024 * 1024;
limits.maxLookaheadDepth = 16;
if (!engine.setRegexLimits(limits)) {
    // 生成时改用 --cpp-lexer vm，或确认全部正则都是 Unicode 规则。
    return 1;
}

Parser::ParserMemory::Vector<Parser::Token> tokens;
const auto status = engine.tokenize(text, &tokens); // %mode lex
if (status == Parser::Status::ErrRegexLimit) {
    const auto &result = engine.lastRegexResult();
    // result.steps / peakMemory / backtracks / peakStates 可用于诊断。
}
engine.setRegexLimits({}); // 恢复默认预算
```

解析模式使用 `runText`，GLR 家族也支持 `runGlr` 和增量入口。资源失败不会作为可复用的增量解析结果缓存；调整预算或解除取消后，同一输入会重试。不同 Engine 的配置与结果互不影响。更换错误提示函数仍可通过上一章的回调自定义 -30/-31/-32 的文案。

## C99

```c
PeRegexLimits limits = {0};
limits.maxSteps = 500000;
Parser_SetRegexLimits(&limits);

ParserLexer_T lexer;
ParserToken_T token;
Parser_LexInit(&lexer, text);
int32_t status = Parser_NextToken(&lexer, &token);
if (status == PARSER_ERR_REGEX_CANCELLED) {
    const PeRegexResult *result = Parser_GetLastRegexResult();
    /* result 是内部只读记录。 */
}
Parser_SetRegexLimits(NULL); /* 恢复默认；下一次初始化清除失败结果 */
```

C 沿用现有全局运行时配置方式。包含正则规则的产物定义 `PARSER_HAS_REGEX_RUNTIME`，并提供预算 API；只有固定 Token 的产物无需这些入口。

需要独立匹配一条生成规则时，使用：

```c
PeRegexResult result = Parser_LexDfaMatchEx(
    &s_ParserRegexDfas[0], text, length, &limits);
```

该函数显式接收剩余字节数，不修改全局配置或最近结果，可在外部持有的不可变程序上独立使用。`Parser_LexDfaMatchLength` 使用当前全局预算并保存结果；旧的 `Parser_LexDfaMatch` 保留长度返回形式，通过 `Parser_GetLastRegexResult` 区分失败原因。生成词法器在一个位置比较多条 Token/commit 规则时，共用一次剩余长度计算，减少重复 `strlen`。

## 验证与示例

```sh
python tests/regex_budget.py
sh ci/run.sh
```

回归覆盖所有预算维度、取消检查、零宽结果区分、负前瞻传播、长反向引用、107 处分配/扩容失败注入及工作内存释放。生成产物验证 C 的 DFA、C/C++ 的有序与 Unicode 路径、短 Token 不遮蔽资源失败、首个 Token 后出错的位置、运行时分配失败、递归下降的致命失败传播，以及增量失败失效与重试。专项已接入统一 CI。

`example/regex_budget.gram` 展示 `%cpp_lexer vm` 与 Unicode、前瞻、反向引用的组合。其他性能优化、统一兼容性报告和词法状态是后续独立工作，不由此预算 API 隐式提供。
