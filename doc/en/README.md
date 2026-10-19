# Repository Documentation

> This is the English edition. The Chinese edition is the default and is the one maintained first — see [中文文档](../../doc/README.md)。

> **Magel's glorious history**: This language grew up in this repository in its early days — the generator, StackVM, standard library, glue and reflection were all polished together here (the Magel documents below are the record of that era). It has since been split into an independent repository at <https://github.com/rabbitc999/magel>; links to `magel/` in this document now point to the corresponding documents in the new repository.

ParseEngine's [tri-language display, embedded assets and release notes](../16-i18n-and-packaging.md) cover `--ui-lang`, the `man` and `lsp` subcommands, the console and pipes, and the standalone packaging scripts for the two tools.

For the Magel 1.0.0 name, the `.mgl` project layout and the Chinese/English/Japanese language selection, see [Magel naming and tri-language notes](https://github.com/rabbitc999/magel/blob/master/doc/32-naming-i18n.md).
Magel's [documentation index](https://github.com/rabbitc999/magel/blob/master/doc/README.md) walks through things step by step with official example source, expected output and error boundaries. Beginners start from [08 first programs](https://github.com/rabbitc999/magel/blob/master/doc/08-first-programs.md); specialized environments and acceptance scope are in [28 sample verification](https://github.com/rabbitc999/magel/blob/master/doc/28-library-migration.md).

Magel's current language baseline: **1.0.0**, current version: **1.2.0**.

For reference sources and lexical lifetime checking, see [04 references and iteration](https://github.com/rabbitc999/magel/blob/master/doc/04-references.md); the dedicated regression is `tests/magel_reference_escape.py`.

For the `T &` default read-only reference, see [reference syntax sugar](https://github.com/rabbitc999/magel/blob/master/doc/04-references.md#默认只读引用语法糖); for panic on unassigned regions, custom heap range checks and AOT default unchecked / explicit checked, see [23 memory allocator](https://github.com/rabbitc999/magel/blob/master/doc/23-memory-allocator.md); the dedicated tests are `tests/magel_reference_sugar.py` and `tests/magel_memory_guard.py`.

For cross-feature combinations of references with generics, enums, callbacks, iterators, interfaces and Drop, see [reference_combinations.mgl](https://github.com/rabbitc999/magel/blob/master/example/samples/reference_combinations.mgl); the dedicated regression is `tests/magel_reference_combinations.py`, covering normal interpretation / AOT and illegal-escape diagnostics.

Start from [01 repository documentation overview](../01-overview.md). This directory is the **sole entry point for all repository documentation**: repository-wide navigation, cross-project notes, and the manual and implementation documents of the ParseEngine parser generator all live here.

| Document | Content |
| --- | --- |
| [01 Repository Documentation Overview](../01-overview.md) | Entry point to all projects, additional libraries and all documents |
| [02 Max-Level Grammar](../02-gram-legend.md) | A fictional side story spanning the parser, Magel and course experiments, continuing Magel 1.0, iteration chains and multi-database acceptance |
| [03 Architecture](../03-architecture.md) | Modules, data flow and code conventions of the generator |
| [04 One .gram, Eight Languages](../04-parseengine-story.md) | The evolution story of the ParseEngine generator |
| [05 Grammar](../05-grammar.md) | The `.gram` syntax specification and generation-time validation |
| [06 Analysis](../06-analysis.md) | Algorithms such as LL, LR and IELR |
| [07 Code Generation](../07-codegen.md) | Template system and language backends |
| [08 Generated Code](../08-generated-code.md) | Inventory of output artifacts and APIs |
| [09 Build and Test](../09-build-and-test.md) | Generator build, template rebuild and regression |
| [09 Sealed Side Story](../09-extra.md) | Paragraphs removed from 02, sealed as "grammar is the key"; speak the seal phrase and the repository's parser generator unseals it |
| [10 Example Grammars](../10-examples.md) | A guided tour of `example/` |
| [11 Project Examples](../11-project-examples.md) | A guided tour of `project/` |
| [12 Grammar Usage Manual](../12-grammar-usage.md) | Directives, semantic actions and error recovery |
| [13 Generation Modes](../13-modes.md) | Sixteen generation modes and the runtimes |
| [14 Target Languages and Output Artifacts](../14-languages-and-products.md) | Backend selection and artifact forms |
| [15 Support Matrix](../15-language-mode-support.md) | Current scope of languages, modes and incremental APIs |
| [16 Tri-language, Embedded Assets and Release](../16-i18n-and-packaging.md) | Display options, embedded manual and language server, Unicode console, standalone Python packaging scripts |
| [Magel EBNF Grammar Reference](https://github.com/rabbitc999/magel/blob/master/doc/33-ebnf.md) | Current lexical specification, all productions, anonymous function types, generic entry points and semantic-check boundaries |
| [Magel Language Philosophy](https://github.com/rabbitc999/magel/blob/master/doc/00-philosophy.md) | Write simply, run everywhere; value semantics, explicit references, resource management and no move semantics |
| [Documentation Maintenance Guidelines](../../project/document/README.md) | Unified conventions, technical document templates, documentation generator and sync checks |

> The `09` pages (build and test, sealed side story) have no English edition yet; the entries above
> link to the Chinese originals. Pages without an English edition are always linked to their Chinese
> source rather than to a partial translation.

## mc Connecting to External Databases

Magel ships standard library modules for MySQL, SQLite, PostgreSQL and Microsoft SQL Server / ODBC,
[`std::sql`](https://github.com/rabbitc999/magel/blob/master/std/sql.mgl), host glue [`glue/std_sql.hpp`](https://github.com/rabbitc999/magel/blob/master/glue/std_sql.hpp),
and a [student information management example project](https://github.com/rabbitc999/magel/blob/master/sql-magel/README.md) inside the Magel project; the retained table creation / initial data /
query SQL lives in [`magel/sql-magel/sql/`](https://github.com/rabbitc999/magel/blob/master/sql-magel/sql/schema.sql). For the library's unified entry point and backend selection see
[19 SQL databases](https://github.com/rabbitc999/magel/blob/master/doc/19-sql.md); for detailed interfaces see the [sql-magel interface reference](https://github.com/rabbitc999/magel/blob/master/sql-magel/doc/01-sql-library.md).

## mc Connecting to External Networks

[18 TCP and minimal HTTP](https://github.com/rabbitc999/magel/blob/master/doc/18-network.md): standard library modules
[`std::net`](https://github.com/rabbitc999/magel/blob/master/std/net.mgl) and host glue [`glue/std_net.hpp`](https://github.com/rabbitc999/magel/blob/master/glue/std_net.hpp)
provide `TcpSocket` / `TcpListener` (connection timeouts, line reading) and `HttpClient` (GET / POST,
`parseUrl` / `buildRequest` / `parseResponse`); the deterministic sample is
[`magel/example/samples/std_net.mgl`](https://github.com/rabbitc999/magel/blob/master/example/samples/std_net.mgl).

For Magel's postfix generic declarations, function and callback type inference, and enum variant shorthand, see the [generics and collections document](https://github.com/rabbitc999/magel/blob/master/doc/10-abstractions-and-collections.md#类型推导与枚举简写).

For Magel's `ExplicitConvert<T>` / `ImplicitConvert<T>`, explicit selection rules and ownership of conversion results, see the [type conversion interfaces](https://github.com/rabbitc999/magel/blob/master/doc/10-abstractions-and-collections.md#显式与隐式类型转换接口).

## mc Collection Literals and Key Protocols

The [generics and collections document](https://github.com/rabbitc999/magel/blob/master/doc/10-abstractions-and-collections.md#set--map-字面量与键协议) describes Set `{a,b,c}`, Map `{key:value,...}`, empty-collection context inference, custom-key `Equal<bool,T>` and the optional `Hash<T>`.

## mc Value Semantics and Ownership

Magel supports `static T new(...)` as a [construction entry point](https://github.com/rabbitc999/magel/blob/master/doc/09-values-and-resources.md#static-t-new构造入口): self is provided automatically, an owning value is returned, discarding the result is forbidden; the default declaration remains legal. Macro temporary variables support `$0`–`$9`.

[03 Value semantics and chained calls](https://github.com/rabbitc999/magel/blob/master/doc/03-value-semantics.md): strings / containers / big numbers and structs are all value types (`String a = b;` copies rather than aliases); the compiler manages intermediate values of chained calls such as `f().g()` through internal temporaries, automatically destructing them, releasing resources and clearing internal slots once the full expression ends, keeping the final value and the borrowed sources it needs, and documenting the related fixes and regressions; for iteration chains see
[05 standard iterators](https://github.com/rabbitc999/magel/blob/master/doc/05-iterators.md).

For Magel's two-tier incremental cache, shared dependency validation and limitations, see the [deployment document](https://github.com/rabbitc999/magel/blob/master/doc/22-deployment.md).

## Unified Build Directory

<!-- docgen:build-summary:start -->
The build entry point is `build.sh`; the actual steps are maintained by `build-all.sh`. Use Git Bash on Windows; use a POSIX shell on Linux/macOS. The scripts build ParseEngine, libraries and example projects (Magel, MetroLang and RustView have been split into independent repositories) and do not run tests; those independent repositories are not in the unified build manifest.

```sh
sh build.sh                  # 全量构建
sh build.sh --list           # 检测环境并显示清单，不执行构建
sh build.sh -q               # 安静模式，完整输出仍写入日志
sh clear.sh --dry-run        # 预览清理范围
sh clear.sh                 # 清理构建产物
```

Files produced by the build entry point live under the repository-root `build/`. The generator is in `build/bin/` (release in `build/bin/release/`), Rust in `build/rust/<library name>/`, example projects in `build/project/<project name>/`, logs in `build/build-all/logs/`, and full example artifacts in `build/build-all/examples/`. Magel generates and compiles inside a source copy at `build/work/magel/`, dependent libraries in `build/work/build/`, and other libraries use directories under the root `build/`; Magel, MetroLang and RustView have been split into independent repositories, with build artifacts in their own repositories.

The build entry actually runs `python3`, `python`, picking an available Python 3 interpreter and skipping unusable placeholder programs and Python 2; the detection result is passed to the generator, and `PYTHON` can be used to override the choice. A C/C++ compiler, make and Python 3 are required tools; when the Rust, .NET or Go toolchains are missing, the corresponding parts are skipped.

Cleanup is limited to Git-ignored artifacts inside the root `build/`, keeping version-controlled files, untracked-but-not-ignored files and nested Git repositories. Cleanup is refused when `tmp/build.lock` is detected; artifacts left behind by old versions in other directories are out of scope.

The paths above apply to the repository's unified build entry point. When running a subproject's Makefile, generation scripts or test tools directly, follow the paths in that project's documentation.

When maintaining this section, edit the [unified configuration](../../project/document/documentation.json) or the [build summary template](../../project/document/templates/build-summary.md.tmpl), then run the documentation generator. For template usage, see the [Documentation Maintenance Guidelines](../../project/document/README.md).
<!-- docgen:build-summary:end -->

## Custom Allocator Library

[`library/allocator`](../../library/allocator/README.md) provides a linked-list pool, a linear pool and C++17 container adapters, under the namespace `Memory`. Build artifacts live in `build/allocator/`.

## Magel Binary Stream Parsing

[`std::binary::BinaryReader`](https://github.com/rabbitc999/magel/blob/master/doc/12-binary-reader.md) reads little/big-endian primitive numbers, fixed-length strings and arrays through C++ glue.
The core [lang IEEE 754 bidirectional interface](https://github.com/rabbitc999/magel/blob/master/doc/06-runtime.md#ieee-754-字节双向转换) converts between float32/float64 and four/eight-byte lists and arrays.

## Magel JSON and TOML

The [JSON/TOML parsing interfaces](https://github.com/rabbitc999/magel/blob/master/doc/13-json-toml.md) plug copies of `library/json` and `library/toml` into Magel, providing parsing, nested reads, traversal, diagnostics and serialization.

[Magel references and iteration](https://github.com/rabbitc999/magel/blob/master/doc/04-references.md): non-null references, default dereferencing, read-only conversion, string / big-number / collection / dictionary / tuple references, single-byte character reads and writes, read-only method checking, list element write-back and the P1 three-backend regression.

[Magel library migration and regression validation](https://github.com/rabbitc999/magel/blob/master/doc/28-library-migration.md): entry points for the standard library, robot, GUI, car and turtle tests.

For Magel's unnamed variable-length symbol table, the magel/CLI switches, measured space savings and reverse-engineering cost, see the [symbol compaction document](https://github.com/rabbitc999/magel/blob/master/doc/25-compact-symbols.md).

[Magel PC bytecode encryption](https://github.com/rabbitc999/magel/blob/master/doc/26-svmb-crypto.md): standalone OpenSSL tools, key management and integrity acceptance.

[Magel optional panic call stacks](https://github.com/rabbitc999/magel/blob/master/doc/27-panic-trace.md): diagnostics strategy when macros are trimmed and symbols are hidden.

Magel line-level debugging covers macro bodies, include and submodule origins; see [panic and source line numbers](https://github.com/rabbitc999/magel/blob/master/doc/27-panic-trace.md#magel-行级来源).

[Magel tuple read-only reference regression](https://github.com/rabbitc999/magel/blob/master/example/samples/p2_tuple_readonly.mgl): read-only references to tuple elements and nested origins, whole-value replacement and independent copies; for the semantics see [04 references and iteration](https://github.com/rabbitc999/magel/blob/master/doc/04-references.md).

[Magel standard iterators](https://github.com/rabbitc999/magel/blob/master/doc/05-iterators.md): lazy chained adapters, list reference iteration, ownership transfer and consuming operations.

[Magel imports and name resolution](https://github.com/rabbitc999/magel/blob/master/doc/02-name-resolution.md) describes library name shorthand without `using`, user declarations taking priority, and disambiguation of same-named imports.

For Magel annotations and reflection — compile-time annotations, `@Reflect` automatic metadata generation, Magel typed queries and all forms of call bridging, three-backend end-to-end acceptance and performance reports, implementation scope, the default-off switch and artifact compatibility rules — see [30 annotations and reflection](https://github.com/rabbitc999/magel/blob/master/doc/30-reflection.md).

For Magel's type and memory audit fixes, `loop` owned-value exits and borrowed returns, plus the three-backend regression, see [values and resources](https://github.com/rabbitc999/magel/blob/master/doc/09-values-and-resources.md#类型与所有权审计回归).

[Magel creating a new application project (magel new)](https://github.com/rabbitc999/magel/blob/master/doc/21-packages.md#新建应用工程)

[Magel short scene directives and running dropped-in files](https://github.com/rabbitc999/magel/blob/master/doc/31-cli.md).

For Magel operator ownership and early-exit tests, see [09 values and resources](https://github.com/rabbitc999/magel/blob/master/doc/09-values-and-resources.md#运算符与提前退出边界回归322).

[Magel Web student information management system](https://github.com/rabbitc999/magel/blob/master/example/web-students/README.md): real HTTP, SQLite, browser pages and three-backend acceptance.

Magel keeps only AOT and `.svmb` compilation artifacts and removes the embedded VM export. See the [exporter notes](https://github.com/rabbitc999/magel/blob/master/magel-emit/README.md) and [scene directives](https://github.com/rabbitc999/magel/blob/master/doc/31-cli.md).

Magel's [lambda and function-pointer callbacks](https://github.com/rabbitc999/magel/blob/master/doc/01-language.md#lambda-与函数指针回调) support iterator chains and callback signature inference, support independent by-value captured environments, and reject reference captures and captures of resources lacking a safe copy protocol; for the full sample see [lambda_callbacks.mgl](https://github.com/rabbitc999/magel/blob/master/example/samples/lambda_callbacks.mgl).

[Magel recursion and nested ownership](https://github.com/rabbitc999/magel/blob/master/doc/03-value-semantics.md#9-递归局部存储与嵌套拥有值): local aggregates, reference slots, arrays, function values and interface combinations; the [calculator tool-chain example](https://github.com/rabbitc999/magel/blob/master/example/calculator/README.md) includes both flat and nested ASTs.

[Custom error messages for all eight targets](../19-custom-error-messages.md) and [regex runtime budgets](../20-regex-runtime-budgets.md) (Chinese).

[Regex compatibility reports and validation without writing output](../21-regex-compatibility.md) (Chinese).

[Binary incremental generation and C++11 subset tests](../22-incremental-generation.md) (Chinese).
