# 12 Modes Supported by Each Language and the Incremental API

> This is the English edition. The Chinese edition is the default and is the one maintained first — see [中文文档](../README.md)。

This chapter records the supported scope of the current version in one place. The eight code-generation targets are C99, C++17, Python, C#, Rust, Go, Java, and JavaScript/TypeScript; `gram` is the grammar-and-analysis-report export profile, not a ninth programming language. The default target is `cpp17` and the default mode is `ielr`. The command line `--lang` / `--mode` overrides `%lang` / `%mode` in the grammar.

The algorithm, applicable scenarios and limitations of each mode are in [10 Generation Modes and Runtimes](13-modes.md), and artifacts and invocation are in [11 Target Languages, Output Artifacts, and API](14-languages-and-products.md).

## Generation Support Matrix

"Supported" means the backend accepts that mode; the grammar must still satisfy the corresponding conflict, left-recursion, semantic-action and directive constraints. "Export" in the `gram` column only means the grammar and the analysis report are generated, not a parser, lexer or incremental API.

| Mode | Algorithm or purpose | C99 `c` | C++17 `cpp17` | Python `python` | C# `csharp` | Rust `rust` | Go `go` | Java `java` | JS/TS `js` | `gram` export |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `ll0` | LL(0) | Supported | Supported | Supported | Supported | Supported | Supported | Not supported | Not supported | Export |
| `ll1` | LL(1) | Supported | Supported | Supported | Supported | Supported | Supported | Not supported | Not supported | Export |
| `llk` | LL(k) | Supported | Supported | Supported | Supported | Supported | Supported | Not supported | Not supported | Export |
| `lr0` | Canonical LR(0) | Supported | Supported | Supported | Supported | Supported | Supported | Not supported | Not supported | Export |
| `slr` | SLR(1) | Supported | Supported | Supported | Supported | Supported | Supported | Not supported | Not supported | Export |
| `slrk` | SLR(k) | Supported | Supported | Supported | Supported | Supported | Supported | Not supported | Not supported | Export |
| `lalr` | LALR(1) | Supported | Supported | Supported | Supported | Supported | Supported | Not supported | Not supported | Export |
| `lalrk` | LALR(k) | Supported | Supported | Supported | Supported | Supported | Supported | Not supported | Not supported | Export |
| `lr1` | Canonical LR(1) | Supported | Supported | Supported | Supported | Supported | Supported | Not supported | Not supported | Export |
| `lrk` | Canonical LR(k) | Supported | Supported | Supported | Supported | Supported | Supported | Not supported | Not supported | Export |
| `ielr` | IELR(1), default mode | Supported | Supported | Supported | Supported | Supported | Supported | Not supported | Not supported | Export |
| `glr` | GSS multi-path parsing | Not supported | Supported | Supported | Supported | Supported | Supported | Not supported | Not supported | Export |
| `earley` | Earley chart parsing | Not supported | Supported | Supported | Supported | Supported | Supported | Supported | Supported | Export |
| `cyk` | CNF conversion, CYK decision and result-tree reconstruction | Not supported | Supported | Supported | Supported | Supported | Supported | Supported | Supported | Export |
| `dd` | Backtracking recursive descent | Supported | Supported | Supported | Supported | Supported | Supported | Supported | Supported | Export |
| `lex` | Lexing only | Supported | Supported | Supported | Supported | Supported | Supported | Not supported | Not supported | Export |
| Total | 16 modes | 13 | 16 | 16 | 16 | 16 | 16 | 3 | 3 | 16 |

C99 does not provide GLR, Earley or CYK runtimes; Go, like the other four full backends, provides GLR, Earley and CYK. Go's Earley/CYK first decides the whole input with a chart algorithm, then rebuilds all result trees against the original grammar via GSS and executes semantic actions.
Java and JS/TS are narrow DD/Earley/CYK backends: Java generates a self-contained `Parser.java`, and JS generates `parser.ts` with erasable TypeScript type annotations. Both profiles support `%tree` but do not execute semantic actions, and do not support `%value_type` / `%union`, `%sync`, `%commit`, `%copy` or user code sections; Earley keeps the first derivation tree, and once the CYK decision passes, the first tree is recovered using the original-grammar Earley table, without showing CNF synthetic nodes.
The `gram` export profile does not generate a runtime, so all 16 modes can be exported; for GLR/Earley/CYK the `analysis.md` keeps the corresponding conflict, multi-path or chart analysis information.

## Incremental Parsing Support

"Incremental" here means the generated parser handles modifications to the input text, not incremental builds by Make/CMake/Cargo. Currently only C++17 and Rust provide incremental parsing APIs; supporting ordinary code generation does not mean incremental parsing is supported.

| Mode | C++17 | Rust | C99 / Python / C# / Go / Java / JS |
| --- | --- | --- | --- |
| `ll0` / `ll1` / `llk` / `dd` | Conservative caching | Conservative caching | No incremental API |
| `lr0` / `slr` / `slrk` / `lalr` / `lalrk` / `lr1` / `lrk` / `ielr` | token cache + prefix checkpoint | token cache + prefix checkpoint | No incremental API |
| `glr` / `earley` / `cyk` | Experimental GLR cache | Experimental GLR cache | No incremental API; ordinary mode support still follows the table above |
| `lex` | No incremental API | No incremental API | No incremental API |

- **LL / DD**: reuse the previous state, semantic results, errors and `%commit` segments for identical text; a text change triggers a full recomputation.
- **Deterministic LR**: saves a token cache and periodic LR stack snapshots; after a text change it resumes from the nearest checkpoint within the common token prefix and reruns the suffix, falling back to a full parse if recovery fails. By default a snapshot is stored every 32 tokens; the interval is configurable, and 0 disables snapshots. Local repair of the state stack, continuation of error recovery, the semantic side-effect policy and CST patching are not yet fully implemented.
- **Experimental GLR cache**: saves token spans, diffs, reduce indices and a real packed forest. Edited leaves, empty spans and insertion boundaries are invalidation starting points, and invalidation propagates to all ancestors through reverse dependencies along `parents`. Nodes that remain valid keep stable IDs, and suffix spans are remapped by the token-count difference; a new parse re-confirms candidates, adding only invalidated or newly appearing symbol nodes and clearing candidates and unreachable nodes that no longer occur. The GSS control flow still goes fully through the table, semantic actions follow the existing policy, and the public `Tree` result is rematerialized; this is not GSS checkpoint recovery or in-place CST patching. Earley/CYK share this forest patching but still fully rebuild the decision chart.

| Purpose | C++17 | Rust |
| --- | --- | --- |
| LL / DD / deterministic LR cache | `IncrementalCache` | `IncrementalCache::new()` |
| LL / DD / deterministic LR entry | `Engine::runTextIncremental(...)` | `Parser::run_text_incremental(...)` |
| LR snapshot interval | `checkpointInterval` | `checkpoint_interval` |
| GLR family cache | `GlrIncrementalCache` | `GlrIncrementalCache::new()` |
| GLR family semantic entry | `runGlrIncremental(...)` | `run_glr_incremental(...)` |
| GLR family tree-building entry | `runGlrTreeIncremental(...)` | `run_glr_tree_incremental(...)` |
| Semantic reuse policy | `semanticReuse` | `semantic_reuse` |
| Semantic environment revision | `semanticRevision` | `semantic_revision` |
| Whether reused this time | `reusedSemanticResults` | `reused_semantic_results` |

The GLR family defaults to `GlrSemanticReusePolicy::Disabled`, executing semantic actions every time. Only when the caller explicitly declares `Pure` and neither the text nor the semantic environment revision has changed is reuse of successful results in non-tree modes allowed. Purity is guaranteed by the caller and covers actions, lexer hooks and result copying; file IO, process spawning, logging or writes to mutable symbol tables should stay disabled. No reuse happens with an ambiguity callback, a failed result or in tree mode. After the environment changes, increment the revision or call `clear()`; `clear()` keeps the policy and the current revision. After switching from disabled to a pure-action policy, recompute once first.

The `Pure` / `Disabled` policies above currently belong only to the GLR family cache and cannot be applied to the `IncrementalCache` of LL/DD/LR. Those modes still require the caller to manage external semantic state and the cache lifetime itself.

## Forest Patching Interface

`cache.forest` holds shared symbol nodes reachable from the accepting root; each node is unique by `(symbol, start, end)`, and `alternatives` holds production numbers and ordered child IDs, preserving ambiguous candidates — it is not a single AST. `forestRoots` (`forest_roots` in Rust) is associated with accepted results, and multiple results can point to the same packed root. A node's `id` is not reused before `clear()`, and a node's index in the array is unstable; `parents` is the deduplicated reverse dependency. Before modifying cache fields, switching grammars, or sharing a cache between different semantic environments, `clear()` should be called.

| Metric | C++17 | Rust | Meaning |
| --- | --- | --- | --- |
| Invalidated nodes | `invalidatedForestNodes` | `invalidated_forest_nodes` | stable node IDs of the previous revision, including transitively invalidated ancestors |
| Reused nodes | `reusedForestNodeCount` | `reused_forest_node_count` | number of nodes in the currently reachable forest that keep an old ID |
| Rebuilt nodes | `rebuiltForestNodeCount` | `rebuilt_forest_node_count` | number of nodes in the currently reachable forest that have a new ID |
| Invalidated edges | `invalidatedForestEdgeCount` | `invalidated_forest_edge_count` | number of child references among all candidates of invalidated old nodes; no longer equal to the number of invalidated reduce entries |

`forestNodeCount` / `forestEdgeCount` (snake_case in Rust) indicate the GSS scale; the actual packed-forest node count is read from `forest.size()` / `forest.len()`. `invalidatedReduceEntries` / `invalidated_reduce_entries` still reference old reduce-array indices, and `GlrReduceEntry::forestNode` / `forest_node` connects a reduction to a shared node. On a previous failure, when error recovery occurs, when switching tree mode, when the semantic revision changes or when an ambiguity callback exists, the forest is conservatively rebuilt; a failure this time publishes no accepting root, and the next time starts from an empty forest. Structural reuse does not skip any semantic action.

## Grammar and Language Boundaries

- `dd` forbids direct and indirect left recursion, returns the first successful path in declaration order, and does not enumerate all ambiguous interpretations.
- GLR/Earley/CYK do not support `error` productions and `%destructor`; Earley/CYK also do not support `%sync`. GLR's multi-path exploration and word-deletion recovery are two different things and do not mean that the full error-recovery state can be reused incrementally.
- Rust semantic values require `Copy + Default + Debug`; code blocks and custom types must use the target language's syntax. Choosing a different backend for the same grammar does not mean that arbitrary user code can compile across languages.
- `lex` generates only the lexer; `gram --mode lex` exports only the lexical grammar and report.

## Verification Basis

On 2026-09-28, Java/JS CYK artifacts were verified with `english-clauses` and `chart_nullable`: Java 22 compiled and ran successfully, and Node.js 26 completed a syntax check of `parser.ts` and ran it directly, covering ambiguous grammars, nullability/left recursion, empty input and illegal input; the dedicated entry point is `python tests/chart_modes.py --langs java,js`.

On 2026-09-28, Go's Earley/CYK artifacts were verified with `english-syntax`, `chart_nullable` and `chart_value_csharp`: Go 1.27 compiled and ran successfully, covering ambiguous trees, nullability/left recursion, illegal input and semantic actions (`2 + 3 * 4` keeps the two results `14` and `20`); GLR/Earley/CYK of `gram` all export the original text, the analysis report and `RESULT.md`. The dedicated entry point is `python tests/chart_modes.py --langs go`.

On 2026-09-27, Java/JS DD and Earley artifacts were also verified with `dd_expr` and `chart_nullable`: Java 22 compiled and ran successfully, and Node.js 26 syntax-checked and directly ran `parser.ts` successfully, with output matching the existing `.expected`; the remaining modes are rejected at generation time. This is a generation-support check, not a compile test of all artifacts. Language limitations are in `Project::generateParserProject` of [`source/src/project.cpp`](../../source/src/project.cpp); mode classification is in [`source/src/analysis.cpp`](../../source/src/analysis.cpp).
The incremental implementation lives in the C++17 and Rust runtime templates of [`source/templates/`](../../source/templates/).
The dedicated regression entry point is in [06 Build, Templates and Small Tests](09-build-and-test.md).
