# 01 Repository Documentation Overview

> This is the English edition. The Chinese edition is the default and is the one maintained first — see [中文文档](../../doc/README.md)。

> **Magel's glorious history**: This language grew up in this repository in its early days — the generator, StackVM, standard library, glue and reflection were all polished together here. It has since been split into an independent repository at <https://gitee.com/rabbitc999/magel>; links to `magel/` in this document now point to the corresponding documents in the new repository.
>
> **Hydra86's map**: It too grew up in this repository — the assembler, the multi-core virtual machine, PSF snapshots and the embedding interface all took their final shape here. It has since been split into an independent repository at <https://gitee.com/rabbitc999/hydra-86>; links to `hydra-86/` in this document now point to the corresponding documents in the new repository.
>
> **The RustView line**: It likewise grew up in this repository — the minimal Rust grammar, borrow analysis and zero-dependency SVG / HTML all took their final shape here. It has since been split into an independent repository at <https://gitee.com/rabbitc999/rust-view>; links to `rust-view/` in this document now point to the corresponding documents in the new repository.
>
> **The CAS tree**: It also grew up in this repository — the full expression grammar, the structured integration table and the custom allocation pool adapters all took their final shape here. It has since been split into an independent repository at <https://gitee.com/rabbitc999/cas>; links to `cas/` in this document now point to the corresponding documents in the new repository.

This page describes where documentation across the ParseEngine repository lives. The repository's main entry is the [root README](../../README.md); the generator, Magel, Hydra86, MetroLang, RustView and CAS each have their own project documentation.

## Independent Projects

| Project | Documentation | Content |
| --- | --- | --- |
| ParseEngine parser generator | [Usage entry](../../source/README.md) · [Topic document index](../README.md) | `.gram` grammar, analysis modes, eight target languages, output artifacts and implementation |
| Magel | [Language entry](https://gitee.com/rabbitc999/magel/blob/master/README.md) · [EBNF grammar reference](https://gitee.com/rabbitc999/magel/blob/master/doc/33-ebnf.md) · [Topic documents](https://gitee.com/rabbitc999/magel/blob/master/doc/README.md) · [Magel exporter](https://gitee.com/rabbitc999/magel/blob/master/magel-emit/README.md) | Language syntax baseline, standard library, StackVM, glue and export |
| Hydra86 | [Project entry](https://gitee.com/rabbitc999/hydra-86/blob/master/README.md) · [Detailed documentation](https://gitee.com/rabbitc999/hydra-86/blob/master/docs/README.md) | Assembly, virtual machine, multi-core scheduling, embedding interface (**independent repository**) |
| MetroLang | [Toolchain entry](https://gitee.com/rabbitc999/metro-lang/blob/master/README.md) · [Language reference](https://gitee.com/rabbitc999/metro-lang/blob/master/doc/01-metrolang-reference.md) · [JSON format](https://gitee.com/rabbitc999/metro-lang/blob/master/doc/02-json-format.md) · [Interactive viewer](https://gitee.com/rabbitc999/metro-lang/blob/master/map_view/README.md) | A metro network data language: grammar, compilation / decompilation, SVG maps and route queries (**independent repository**) |
| RustView | [Project entry](https://gitee.com/rabbitc999/rust-view/blob/master/README.md) · [Preview page and demo](https://gitee.com/rabbitc999/rust-view/blob/master/doc/preview.html) · [Grammar](https://gitee.com/rabbitc999/rust-view/blob/master/rust_subset.gram) | Borrow-semantics visualization for minimal Rust pseudocode: ParseEngine-generated frontend, scope stack + borrow state table analysis, hand-written SVG / HTML; **1.0 feature freeze** (**independent repository**) |
| CAS | [Project entry](https://gitee.com/rabbitc999/cas/blob/master/README.md) · [Architecture and validation](https://gitee.com/rabbitc999/cas/blob/master/doc/architecture.md) | Symbolic computation, structured integration table and custom allocation pools (**independent repository**) |

## Parser Generator Documentation

The generator's implementation and manual live in this directory alongside the rest of the repository's documents; the numbering is in the table below.

| No. | Document | Content |
| --- | --- | --- |
| 03 | [Architecture](../03-architecture.md) | Modules and data flow of the generator |
| 05 | [Grammar](../05-grammar.md) | The `.gram` specification and validation |
| 06 | [Analysis](../06-analysis.md) | Algorithms such as LL, LR and IELR |
| 07 | [Code Generation](../07-codegen.md) | Templates and language backends |
| 08 | [Generated Code](../08-generated-code.md) | Artifact structure and APIs |
| 09 | [Build and Test](../09-build-and-test.md) | Generator build, template rebuild and regression |
| 10 | [Example Grammars](../10-examples.md) | A guided tour of the `example/` grammars |
| 11 | [Project Examples](../11-project-examples.md) | A guided tour of the `project/` projects |
| 12 | [Grammar Usage Manual](../12-grammar-usage.md) | Directives and semantic actions |
| 13 | [Generation Modes](../13-modes.md) | Sixteen analysis and generation modes |
| 14 | [Target Languages and Output Artifacts](../14-languages-and-products.md) | Backend selection and artifact forms |
| 15 | [Support Matrix](../15-language-mode-support.md) | Current scope of languages, modes and incremental APIs |
| 16 | [Tri-language, Embedded Assets and Release](../16-i18n-and-packaging.md) | Display options, embedded manual and language server, Unicode console, standalone Python packaging scripts |

## Other Projects and Tools

| Category | Documentation |
| --- | --- |
| Example projects | [project main entry](../../project/README.md) · [course lab experiments](../../project/lab_report_nju/README.md) |
| Standalone libraries | [library directory](../../library/); each library maintains its own `README.md` and technical reports |
| Editor | [VS Code extension](../../vscode/README.md) |
| Documentation maintenance | [unified conventions, templates and documentation generator](../../project/document/README.md) |
| Repository collaboration | [handover notes](../../AGENTS.md) · [tool operation manual](../../SKILL.md) |
| Cross-project narratives | [02 Max-Level Grammar](../02-gram-legend.md) · [04 One .gram, Eight Languages](../04-parseengine-story.md) |

When changing any project, defer to that project's source, tests and local documentation; for the unified scope of the parser's languages and modes, see the [support matrix](../15-language-mode-support.md).

## CAS Symbolic Computation

[CAS project manual](https://gitee.com/rabbitc999/cas/blob/master/README.md) (**independent repository**) · [Architecture and validation](https://gitee.com/rabbitc999/cas/blob/master/doc/architecture.md) · [Built-in symbol list](https://gitee.com/rabbitc999/cas/blob/master/doc/builtins.json).

## Custom Allocator Library

[`library/allocator`](../../library/allocator/README.md) provides a linked-list pool, a linear pool and C++17 container adapters, under the namespace `Memory`; CAS plugs in via `Cas::Memory`. Build artifacts live in `build/allocator/`.

## Magel Binary Stream Parsing

[`std::binary::BinaryReader`](https://gitee.com/rabbitc999/magel/blob/master/doc/12-binary-reader.md) reads little/big-endian primitive numbers, fixed-length strings and arrays through C++ glue.

## Magel JSON and TOML

The [JSON/TOML parsing interfaces](https://gitee.com/rabbitc999/magel/blob/master/doc/13-json-toml.md) plug copies of `library/json` and `library/toml` into Magel, providing parsing, nested reads, traversal, diagnostics and serialization.
