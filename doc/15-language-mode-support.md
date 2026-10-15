# 12 各语言支持的模式与增量 API

本章统一记录当前版本的支持范围。八种代码生成目标为 C99、C++17、Python、C#、Rust、Go、Java、JavaScript/TypeScript；
`gram` 是文法与分析报告导出档，不是第九种编程语言。默认目标为 `cpp17`，默认模式为 `ielr`。
命令行 `--lang` / `--mode` 覆盖文法中的 `%lang` / `%mode`。

模式的算法、适用场景和限制见 [10 生成模式与运行时](13-modes.md)，
产物与调用方式见 [11 目标语言、产物与 API](14-languages-and-products.md)。

## 生成支持矩阵

“支持”表示后端接受该模式；文法仍须满足相应的冲突、左递归、语义动作与指令约束。
`gram` 列的“导出”仅表示生成文法与分析报告，不生成解析器、词法器或增量 API。

| 模式 | 算法或用途 | C99 `c` | C++17 `cpp17` | Python `python` | C# `csharp` | Rust `rust` | Go `go` | Java `java` | JS/TS `js` | `gram` 导出 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `ll0` | LL(0) | 支持 | 支持 | 支持 | 支持 | 支持 | 支持 | 不支持 | 不支持 | 导出 |
| `ll1` | LL(1) | 支持 | 支持 | 支持 | 支持 | 支持 | 支持 | 不支持 | 不支持 | 导出 |
| `llk` | LL(k) | 支持 | 支持 | 支持 | 支持 | 支持 | 支持 | 不支持 | 不支持 | 导出 |
| `lr0` | 规范 LR(0) | 支持 | 支持 | 支持 | 支持 | 支持 | 支持 | 不支持 | 不支持 | 导出 |
| `slr` | SLR(1) | 支持 | 支持 | 支持 | 支持 | 支持 | 支持 | 不支持 | 不支持 | 导出 |
| `slrk` | SLR(k) | 支持 | 支持 | 支持 | 支持 | 支持 | 支持 | 不支持 | 不支持 | 导出 |
| `lalr` | LALR(1) | 支持 | 支持 | 支持 | 支持 | 支持 | 支持 | 不支持 | 不支持 | 导出 |
| `lalrk` | LALR(k) | 支持 | 支持 | 支持 | 支持 | 支持 | 支持 | 不支持 | 不支持 | 导出 |
| `lr1` | 规范 LR(1) | 支持 | 支持 | 支持 | 支持 | 支持 | 支持 | 不支持 | 不支持 | 导出 |
| `lrk` | 规范 LR(k) | 支持 | 支持 | 支持 | 支持 | 支持 | 支持 | 不支持 | 不支持 | 导出 |
| `ielr` | IELR(1)，默认模式 | 支持 | 支持 | 支持 | 支持 | 支持 | 支持 | 不支持 | 不支持 | 导出 |
| `glr` | GSS 多路径解析 | 不支持 | 支持 | 支持 | 支持 | 支持 | 支持 | 不支持 | 不支持 | 导出 |
| `earley` | Earley 图表解析 | 不支持 | 支持 | 支持 | 支持 | 支持 | 支持 | 支持 | 支持 | 导出 |
| `cyk` | CNF 转换、CYK 判定与结果树重建 | 不支持 | 支持 | 支持 | 支持 | 支持 | 支持 | 支持 | 支持 | 导出 |
| `dd` | 回溯递归下降 | 支持 | 支持 | 支持 | 支持 | 支持 | 支持 | 支持 | 支持 | 导出 |
| `lex` | 仅词法分析 | 支持 | 支持 | 支持 | 支持 | 支持 | 支持 | 不支持 | 不支持 | 导出 |
| 合计 | 16 种模式 | 13 | 16 | 16 | 16 | 16 | 16 | 3 | 3 | 16 |

C99 不提供 GLR、Earley、CYK 运行时；Go 与另外四个完整后端一样提供 GLR、
Earley、CYK。Go 的 Earley/CYK 先用图表算法判定整段输入，再由 GSS 按原文法
重建全部结果树并执行语义动作。
Java 与 JS/TS 是 DD/Earley/CYK 窄后端：Java 生成自包含 `Parser.java`，JS 生成带可擦除
TypeScript 类型标注的 `parser.ts`。两档支持 `%tree`，但不执行语义动作，也不支持
`%value_type` / `%union`、`%sync`、`%commit`、`%copy` 或用户代码段；Earley 保留第一棵
派生树，CYK 判定通过后用原文法 Earley 表恢复第一棵树，不显示 CNF 合成节点。
`gram` 导出档不生成运行时，因此 16 种模式均可导出；GLR/Earley/CYK 的
`analysis.md` 会保留对应的冲突、多路径或图表分析信息。

## 增量解析支持

这里的“增量”指生成的解析器处理输入文本修改，不是 Make/CMake/Cargo 的增量构建。
目前只有 C++17 与 Rust 提供增量解析 API；支持普通代码生成不等于支持增量解析。

| 模式 | C++17 | Rust | C99 / Python / C# / Go / Java / JS |
| --- | --- | --- | --- |
| `ll0` / `ll1` / `llk` / `dd` | 保守缓存 | 保守缓存 | 无增量 API |
| `lr0` / `slr` / `slrk` / `lalr` / `lalrk` / `lr1` / `lrk` / `ielr` | token cache + 前缀 checkpoint | token cache + 前缀 checkpoint | 无增量 API |
| `glr` / `earley` / `cyk` | GLR 实验缓存 | GLR 实验缓存 | 无增量 API；普通模式支持仍以上表为准 |
| `lex` | 无增量 API | 无增量 API | 无增量 API |

- **LL / DD**：同文本复用上次状态、语义结果、错误与 `%commit` 片段；文本变化完整重算。
- **确定性 LR**：保存 token cache 和定期 LR 栈快照，文本变化后从公共 token 前缀内最近的
  checkpoint 恢复并重跑后缀，恢复失败回退完整解析。默认每 32 个 token 存一次快照，间隔
  可配置，0 禁用快照。状态栈局部修补、错误恢复延续、语义副作用策略和 CST 修补尚未完整实现。
- **GLR 实验缓存**：保存 token span、diff、reduce 索引和真正的 packed forest。
  编辑命中的叶子、空跨度及插入边界是失效起点，沿 `parents` 反向依赖传播至全部祖先。
  未失效节点保留稳定 ID，后缀跨度按 token 数量差重映射；新解析重新确认候选，只新增失效
  或新出现的符号节点，清掉不再出现的候选与不可达节点。GSS 控制流程仍完整走表，语义动作
  照既有策略执行，公开 `Tree` 结果重新物化；这不是 GSS checkpoint 恢复或 CST 原位修补。
  Earley/CYK 共用这套 forest 修补，但仍完整重建判定图表。

| 用途 | C++17 | Rust |
| --- | --- | --- |
| LL / DD / 确定性 LR 缓存 | `IncrementalCache` | `IncrementalCache::new()` |
| LL / DD / 确定性 LR 入口 | `Engine::runTextIncremental(...)` | `Parser::run_text_incremental(...)` |
| LR 快照间隔 | `checkpointInterval` | `checkpoint_interval` |
| GLR 家族缓存 | `GlrIncrementalCache` | `GlrIncrementalCache::new()` |
| GLR 家族语义入口 | `runGlrIncremental(...)` | `run_glr_incremental(...)` |
| GLR 家族建树入口 | `runGlrTreeIncremental(...)` | `run_glr_tree_incremental(...)` |
| 语义复用策略 | `semanticReuse` | `semantic_reuse` |
| 语义环境版本 | `semanticRevision` | `semantic_revision` |
| 本次是否复用 | `reusedSemanticResults` | `reused_semantic_results` |

GLR 家族默认 `GlrSemanticReusePolicy::Disabled`，每次执行语义动作。只有调用方显式声明
`Pure`，且文本和语义环境版本都不变时，才允许复用非 tree 模式的成功结果。纯度由调用方
保证，涵盖动作、词法钩子与结果复制；文件 IO、进程启动、日志或可变符号表写入应保持禁用。
有歧义回调、失败结果或 tree 模式时不复用。环境改变后递增版本或调用 `clear()`；
`clear()` 保留策略与当前版本。从禁用切到纯动作策略后先重算一次。

上述 `Pure` / `Disabled` 策略目前只属于 GLR 家族缓存，不能套用到 LL/DD/LR 的
`IncrementalCache`。这些模式仍需调用方自行管理外部语义状态和缓存生命周期。

## Forest 修补接口

`cache.forest` 保存接受根可达的共享符号节点；每个节点按 `(symbol, start, end)` 唯一，
`alternatives` 保存产生式编号和有序 child ID，保留歧义候选，不是单棵 AST。
`forestRoots`（Rust 为 `forest_roots`）与接受结果关联，多个结果可以指向同一个 packed 根。
节点 `id` 在 `clear()` 前不复用，节点在数组中的下标不稳定；`parents` 是去重后的反向依赖。
修改缓存字段、换文法或不同语义环境之间共享缓存前，应调用 `clear()`。

| 指标 | C++17 | Rust | 含义 |
| --- | --- | --- | --- |
| 失效节点 | `invalidatedForestNodes` | `invalidated_forest_nodes` | 上一版的稳定节点 ID，含传递失效祖先 |
| 复用节点 | `reusedForestNodeCount` | `reused_forest_node_count` | 当前可达 forest 中保留旧 ID 的节点数 |
| 重建节点 | `rebuiltForestNodeCount` | `rebuilt_forest_node_count` | 当前可达 forest 中新增 ID 的节点数 |
| 失效边 | `invalidatedForestEdgeCount` | `invalidated_forest_edge_count` | 失效旧节点的全部候选中 child 引用数量，不再等于失效 reduce 条目数 |

`forestNodeCount` / `forestEdgeCount`（Rust 为 snake_case）表示 GSS 规模，
实际 packed forest 节点数读取 `forest.size()` / `forest.len()`。
`invalidatedReduceEntries` / `invalidated_reduce_entries` 仍引用旧 reduce 数组下标，
`GlrReduceEntry::forestNode` / `forest_node` 把归约连接到共享节点。
前一次失败、发生错误恢复、切换 tree 模式、语义版本变化或存在歧义回调时，保守重建 forest；
本次失败不发布接受根，下一次从空 forest 开始。结构复用不会额外跳过任何语义动作。

## 文法与语言边界

- `dd` 禁止直接和间接左递归，按声明顺序返回第一条成功路径，不枚举全部歧义解释。
- GLR/Earley/CYK 不支持 `error` 产生式与 `%destructor`；Earley/CYK 也不支持 `%sync`。
  GLR 的多路径探索与删词恢复是两件事，不代表完整错误恢复状态可增量复用。
- Rust 语义值需要 `Copy + Default + Debug`；代码块与自定义类型须使用目标语言语法。
  选择同一文法的不同后端并不意味着任意用户代码都能跨语言编译。
- `lex` 只生成词法器；`gram --mode lex` 只导出词法文法与报告。

## 核验依据

2026-09-28 用 `english-clauses` 与 `chart_nullable` 核验 Java/JS 的 CYK 产物：
Java 22 编译运行通过，Node.js 26 完成 `parser.ts` 语法检查并直接运行，覆盖歧义文法、
可空/左递归、空输入和非法输入；专项入口是
`python tests/chart_modes.py --langs java,js`。

2026-09-28 用 `english-syntax`、`chart_nullable` 与 `chart_value_csharp` 核验 Go 的
Earley/CYK 产物：Go 1.27 编译运行通过，覆盖歧义树、可空/左递归、非法输入与
语义动作（`2 + 3 * 4` 保留 `14`、`20` 两个结果）；`gram` 的 GLR/Earley/CYK
均导出原文、分析报告与 `RESULT.md`。专项入口是 `python tests/chart_modes.py --langs go`。

2026-09-27 另用 `dd_expr` 与 `chart_nullable` 核验 Java/JS 的 DD、Earley 产物：
Java 22 编译运行通过，Node.js 26 对 `parser.ts` 做语法检查并直接运行通过，输出与既有
`.expected` 一致；其余模式由生成阶段拒绝。这是生成支持检查，不是全部产物的编译测试。
语言限制见 [`source/src/project.cpp`](../source/src/project.cpp) 的 `Project::generateParserProject`；
模式分类见 [`source/src/analysis.cpp`](../source/src/analysis.cpp)。
增量实现位于 [`source/templates/`](../source/templates/) 的 C++17 与 Rust 运行时模板。
专项回归入口见 [06 构建、模板与小测](09-build-and-test.md)。
