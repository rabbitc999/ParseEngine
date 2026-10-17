> This is the English edition. The Chinese edition is the default and is the one maintained first — see [中文文档](../README.md)。

# Display Language, Embedded Assets and Release

The ParseEngine command line supports Chinese and English. The code generation target is still controlled by options such as `--lang cpp17`; the display language uses the separate `--ui-lang`.

```sh
ParseEngine --help --ui-lang zh
ParseEngine --help --ui-lang en
ParseEngine --help --ui-lang ja
ParseEngine example/calc.gram --lang cpp17 --out-dir build/calc --ui-lang ja
```

Display language precedence is an explicit `--ui-lang`, then `PARSEENGINE_LANG`, `LC_ALL`, `LC_MESSAGES`, `LANG`, and finally the Windows user UI language. A Chinese locale selects Chinese and any other locale selects English; `auto` ignores any fixed choice in the tool environment and re-detects the locale. Locale strings such as `zh_CN.UTF-8` are supported; `C` / `POSIX` selects English.

Help, command-line argument checking, generation-complete and cleanup messages, packaging results and registered common diagnostics use the trilingual tables. Analysis-specific diagnostics that have no translation entry keep their full original text; analysis reports, generated source comments and templates keep their original text. Display options do not modify the grammar, identifiers, source snippets or generated code.

The authoritative message table is `source/i18n/messages.tsv`; run `python tools/embed_parseengine_i18n.py` to generate the static C++ table. Messages are embedded in the executable, and no language resource file is read at runtime. Make and CMake regenerate the embedded assets after messages or release scripts change; a build environment without Python can use the checked-in generated header, but after changing the assets you must first regenerate in an environment that has Python.

## Embedded Manual (`ParseEngine man`)

The command line carries an **embedded manual**: the bodies of `doc/` and `doc/en/` are compressed at
build time by `tools/embed_manual.py` into `source/src/manual_data.cpp`, and **no repository file is read
at runtime** — the executable can be copied anywhere on its own and still be queried.

```sh
ParseEngine man                     # index: 18 doc pages + directive / mode / language / example lists
ParseEngine man 12-grammar-usage    # show one page (`man 12`, `man %token` work too)
ParseEngine man directives          # every %directive, each with a one-line summary
ParseEngine man modes               # the 16 generation modes: usage + one-line purpose
ParseEngine man languages           # target languages and how many modes each supports
ParseEngine man examples            # the example grammars in example/
ParseEngine man --search lookahead   # full-text search (page bodies / directives / modes / languages)
ParseEngine man --json              # machine-readable (for editors and scripts)
ParseEngine man --lang en 04        # body language: zh / en / ja / auto (auto-detected by default)
```

| Point | Detail |
| --- | --- |
| Data source | `doc/*.md` + `doc/en/*.md` + `example/manifest.tsv`; `09-build-and-test.md` and `09-extra.md` are **excluded** (that pair is this repository's own build and experiment log and should not ship with a compiler installed elsewhere) |
| Re-embedding | after editing `doc/`, run `make -C source manual` (equivalent to `python tools/embed_manual.py`) and rebuild |
| Size | about 1.3 MB of Markdown compressed by `library/lztext` to about 0.4 MB (ratio ≈ 0.29), producing a C++ byte array of roughly 2 MB |
| Bilingual pages | pages that have an English edition are marked `[en]` in the index, and the footer shows `来源：doc/<name>` / `英文版：doc/en/<name>`; the English body is used with `--lang en` or when the display language resolves to English |
| Paging and colour | bodies longer than one screen page through `less -R` when `less` is available; no ANSI colour when output is redirected or `NO_COLOR` is set |
| Exit codes | 0 = hit; 1 = target not found / search found nothing; 2 = bad arguments (same convention as `lsp --check`) |

## Embedded Language Server and the Bundled Compiler

The `.gram` language server from `library/lsp` (the `json` / `grammar` / `lsp` sources) is **compiled into
the generator**, so one executable is both the parser generator and the language server:

```sh
ParseEngine lsp --stdio              # start the LSP (Content-Length framing + JSON-RPC)
ParseEngine lsp --check FILE...      # validate once, print diagnostics to stdout
ParseEngine lsp --version            # parseengine-lsp 1.0.0
```

Arguments, exit codes and diagnostic format are identical to the separately built
`build/lsp/lsp-server[.exe]` — `library/lsp` can still be built on its own
(`make -C library/lsp`), and `lsp --check` exits 0 for a clean grammar and 1 when there are errors.
**The VS Code extension uses this embedded copy by default** and ships the compiler inside the `.vsix`:

| Step | How |
| --- | --- |
| Locating the compiler | `vscode/src/serverLocator.js` tries the `gram.parseEnginePath` setting → the repository's `build/bin` → the bundled `bin/` → `PATH`, in that order |
| Starting the server | `ParseEngine lsp --stdio` (setting `gram.lspServerPath` to a standalone `lsp-server` switches back to plain `--stdio`) |
| Validating a grammar | `ParseEngine lsp --check FILE`; `error` / `warning` / `info` map to the three severities of the problems panel |
| Bundling the compiler | while packaging, `vscode/tools/embed-compiler.js` copies `ParseEngine[.exe]` into `vscode/bin/` (skipped when the content already matches; `vscode/bin/` is not checked in) |
| Platform tag | `vscode/tools/package.js` writes `TargetPlatform` (e.g. `win32-x64`) into the `.vsix` `Identity` and appends the same suffix to the file name; `--universal` produces a tag-free package |
| Opening the manual | `Gram: 打开文法指令手册` prefers `doc/12-grammar-usage.md` in the repository; when the repository is not next to the extension it falls back to `ParseEngine man 12-grammar-usage` and shows the same text inside the editor |

The extension's own layout, commands and packaging details are in [`vscode/README.md`](../vscode/README.md).

## Windows Encoding

Both tools use the same `library/console/utf8.hpp`: on Windows the UTF-8 argv is obtained from the CRT wide-character arguments, and paths are converted via `filesystem::u8path`. C++ console output uses `WriteConsoleW`; C stdio and child processes that inherit the console use the UTF-8 code page while running. Output is flushed before exit and the original input and output code pages are restored, so the user's window is not permanently switched to 65001. Files and pipes always emit UTF-8, no BOM is added, and the JSON protocol is unchanged.

Windows PowerShell 5.1 may interpret the pipeline of an external program using its own legacy encoding; the tool cannot choose a decoder on behalf of the parent process. When using its pipeline you can set the following first:

```powershell
[Console]::OutputEncoding = New-Object System.Text.UTF8Encoding($false)
$OutputEncoding = [Console]::OutputEncoding
```

Reading redirected UTF-8 text should explicitly use `Get-Content -Encoding UTF8 文件`. The terminal font must still contain CJK glyphs.

## Python ZIP Release Script

ZIP is a release operation, implemented by `tools/package_compiler.py` using the Python 3.11+ standard library; the C++ tools do not include ZIP algorithms. Package the two tools directly:

```sh
python tools/package_compiler.py ParseEngine out dist/ParseEngine-windows-x64.zip --ui-lang zh
python tools/package_compiler.py magel out dist/magel-windows-x64.zip --ui-lang ja
```

The script reads `build/bin/ParseEngine(.exe)` or `magel/magel(.exe)` by default, and `--compiler FILE` can point to a different build. The output parent directory must exist; the default name includes the OS and architecture, and inside the repository Magel also reads VERSION. The ZIP contains only the compiler executable for the current platform, not the host Python, C++ compiler or dynamic libraries. The embedded templates, standard library and messages remain stored in the executable.

ParseEngine additionally provides a thin entry point that extracts the same embedded Python script and passes argv through directly, without going through a shell:

```sh
ParseEngine zip
ParseEngine zip out dist/ParseEngine.zip --ui-lang ja
```

`PARSEENGINE_PYTHON` can specify the Python path. The script itself does not depend on the repository to package an explicitly given executable; the ParseEngine thin entry point therefore also works after being extracted outside the repository. Magel's release uses only the standalone Python script and does not provide a `magel zip` command.

Packaging uses DEFLATE and creates files exclusively; existing files and links are refused rather than overwritten, and a failed write deletes the incomplete archive. Inside the archive the name is uniformly `ParseEngine.exe` / `ParseEngine` or `magel.exe` / `magel`. It does not modify or re-link the compiler, write the registry or install system components.

## Verification

```sh
python tests/parseengine_i18n.py
python tests/magel_i18n.py --magel magel/magel.exe
python tests/magel_zip.py --magel magel/magel.exe
```

The Windows-specific tests read UTF-16 characters from a real console buffer in code pages 936 / 932, checking C++ streams, C stdio, the actual tool messages and code page restoration; pipes are strictly decoded as UTF-8 separately. They also test paths containing spaces and CJK, the trilingual option precedence, identical generated source, ZIP CRC, byte-for-byte identity, refusal to overwrite, failure cleanup and running outside the repository. Other platforms skip the Windows console checks.
