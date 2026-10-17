# ParseEngine · Parser Generator

<img src="image/gram-editor-256.png" alt="ParseEngine parser generator icon" width="120" align="right" />

ParseEngine generates a lexer, a parser and a buildable project from a single `.gram` grammar, together
with an analysis report. It targets language front ends, configuration formats and teaching examples:
**eight target languages** and **sixteen analysis modes**, with incremental APIs for C++17 and Rust.
One grammar is both the syntax definition and the documentation — declarations such as `%token`,
`%identifier` and `%trivia` *are* the lexical rules, and productions name keywords, blocks and
commands instead of "extracting lexemes" with one large regular expression.

![ParseEngine parser generator overview](tools/overview/ParseEngine-overview.png)

[Open the scalable overview](tools/overview/ParseEngine-overview.svg) · [Full generator manual](source/README.md) · [Documentation index](doc/en/README.md)

> This is the English edition. The Chinese edition is the default and is the one maintained first —
> see [中文 README](README.md). Where the two disagree, the Chinese one wins.

## Quick start

```sh
make -C source
./build/bin/ParseEngine --grammar-file example/calc.gram --mode ielr --lang cpp17 --out-dir build/examples/calc-parser
make -C build/examples/calc-parser
```

On Windows the executable is `build/bin/ParseEngine.exe`. The default target language is C++17 and the
default analysis mode is IELR(1); `--lang` and `--mode` on the command line override `%lang` and
`%mode` in the grammar. The generated directory holds the sources, the build files and a `RESULT.md`
report — state counts, conflicts and parseability are all in there (it is the brief form by default;
add `--full-report` for the full analysis report at the end). For `.gram` syntax and every
command-line option see the [usage manual](doc/12-grammar-usage.md).

## Supported target languages and analysis modes

| Output | Coverage |
| --- | --- |
| C99 | 13 modes |
| C++17, Python, C#, Rust, Go | 16 modes each |
| Java, JavaScript/TypeScript | DD, Earley, CYK |
| `--lang gram` | Export the original grammar and the analysis report; covers all 16 modes |

The sixteen modes include LL, LR, GLR, Earley, CYK, recursive-descent DD and `lex`, which emits the
lexer only. The exact coverage, limits and the C++17 / Rust incremental APIs are governed by the
[support matrix](doc/15-language-mode-support.md).

## Implementation structure

| Location | Contents |
| --- | --- |
| [`source/`](source/README.md) | The generator itself: grammar loading, per-mode front ends, code emission, diagnostics and reports |
| [`source/templates/`](source/templates/) | Output templates per target language (filled in when a project is generated) |
| [`example/`](example/) | Example grammars and expected output |
| [`vscode/`](vscode/README.md) | Editor support for `.gram` (highlighting, completion, diagnostics) |
| `library/` | General libraries alongside the generator (each with its own README); `library/transfer` lowers quadruples to target-machine code |

For implementation details see the [architecture document](doc/03-architecture.md); for the trade-offs
between modes and backends see the [support matrix](doc/15-language-mode-support.md).

## Build

The build entry point is `build.sh`, and the actual steps live in `build-all.sh`. Git Bash is used on
Windows; POSIX shell on Linux/macOS. The script builds ParseEngine, the libraries and the example
projects (Magel, MetroLang and RustView have been split into standalone repositories and
are not part of the unified build list); it does not run tests.

```sh
sh build.sh                  # full build
sh build.sh --list           # detect the environment and print the list, without building
sh build.sh -q               # quiet mode; the full output still goes to the log
sh clear.sh --dry-run        # preview what would be cleaned
sh clear.sh                  # clean build artifacts
```

Everything the build entry point produces lives under the repository root `build/`. The generator is
in `build/bin/` (release in `build/bin/release/`), Rust crates in `build/rust/<library>/`, example
projects in `build/project/<project>/`, logs in `build/build-all/logs/`, and the full set of example
artifacts in `build/build-all/examples/`. Magel is generated and compiled from a source copy in
`build/work/magel/`, with its dependencies in `build/work/build/`, while other libraries reuse the
directories under the root `build/`; Magel, MetroLang and RustView have been split into
standalone repositories, so their build artifacts live in those repositories.

The build entry point actually runs `python3` / `python`, picks a usable Python 3 interpreter, and
skips unusable placeholders and Python 2; the detection result is passed to the generator, and `PYTHON` can override the choice.
A C/C++ compiler, make and Python 3 are required; when the Rust, .NET or Go toolchains are missing the
corresponding parts are skipped.

Cleaning is limited to ignored artifacts under the root `build/`; version-controlled files, untracked
files that are not ignored, and nested Git repositories are preserved. Cleaning is refused when
`tmp/build.lock` is detected; artifacts that older versions left in other directories are out of scope.

The paths above apply to the repository's unified build entry point. When running a sub-project's
Makefile, generation script or test tools directly, follow the paths in that project's documentation.

> This section mirrors the Chinese block in [`README.md`](README.md) (maintained with
> [documentation.json](project/document/documentation.json) and the
> [build-summary template](project/document/templates/build-summary.md.tmpl)); see the
> [documentation maintenance guide](project/document/README.md).

## Gates and packaging

Local runs and CI share one entry point and need no root:

```sh
sh ci/run.sh                                   # build the generator + run ParseEngine's own checks and regressions
sh ci/run.sh --skip-vm-tests                   # skip the slower part for a faster run
python tools/package_compiler.py ParseEngine   # package into ParseEngine-<platform>.zip
```

## Documentation and license

The [repository documentation index](doc/01-overview.md) gathers all documents; the project stories are
[Max-Level Grammar and a Big Cleanup](doc/02-gram-legend.md) and
[One .gram, Eight Languages](doc/04-parseengine-story.md). Collaboration and artifact-maintenance rules
are in [`AGENTS.md`](AGENTS.md); the license is in [`LICENSE`](LICENSE) (MIT).

English pages live in [`doc/en/`](doc/en/README.md); the low-level specifications without an English
edition are linked to the Chinese originals from that index.

## Neighbouring projects (split out)

The sections above cover this repository's main body, ParseEngine; the projects below sit alongside it
and evolve independently — the front ends of several of them **are generated by ParseEngine** (they are
both users and the best test cases). Overview diagrams for each project are in
[`tools/overview/`](tools/overview/), produced by the `tools/make_*_overview.py` scripts.

| Project | What it is | Entry point |
| --- | --- | --- |
| **MetroLang** | Toolchain for a metro-network data language: compiler, templated decompiler, self-contained HTML/SVG maps, Dijkstra route queries and an interactive viewer; the front end is generated from `metrolang.gram` (169 keywords, one production each, 842 states · 0 conflicts) — **split into a standalone repository** | <https://gitee.com/rabbitc999/metro-lang> |
| **RustView** | **Borrow-semantics visualisation** for a minimalist Rust pseudo-language (teaching): moves, borrows, conflicts and dangling references drawn as dependency-free SVG + HTML; the front end is generated from `rust_subset.gram`, with the 1.0 feature set frozen — **split into a standalone repository** | <https://gitee.com/rabbitc999/rust-view> |
| **Magel** | A language and toolchain whose front end is generated by ParseEngine and which compiles to StackVM (standard library, host glue, C ABI, board-side VM) — **split into a standalone repository** | <https://github.com/rabbitc999/magel> |

General libraries alongside the generator — [`library/allocator`](library/allocator/README.md) (C99
pools and container adapters for the `Memory` namespace), [`library/transfer`](library/transfer/README.md)
(quadruples → basic blocks / CFG / target machine), [`library/preprocess`](library/preprocess/README.md),
[`library/template`](library/template/README.md), [`library/json`](library/json/README.md),
[`library/toml`](library/toml/README.md) and others — each have their own README.
