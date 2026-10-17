# ParseEngine · 解析器生成器

<img src="image/gram-editor-256.png" alt="ParseEngine 解析器生成器图标" width="120" align="right" />

ParseEngine 从一份 `.gram` 文法生成词法器、解析器和可构建的工程，并附上分析报告。它面向语言前端、
配置格式与教学示例：**八种目标语言**、**十六种分析模式**，C++17 与 Rust 另提供增量 API。
一份文法同时是语法定义与文档——`%token`、`%identifier`、`%trivia` 这些声明就是词法规则，
产生式里点得出关键字、块与命令，而不是靠一个大正则去"提取词素"。

![ParseEngine 解析器生成器总览](tools/overview/ParseEngine-overview.png)

[查看可缩放的总览图](tools/overview/ParseEngine-overview.svg) · [生成器完整手册](source/README.md) · [文档导航](doc/README.md) · [English](README.en.md) / [English docs](doc/en/README.md)

## 快速开始

```sh
make -C source
./build/bin/ParseEngine --grammar-file example/calc.gram --mode ielr --lang cpp17 --out-dir build/examples/calc-parser
make -C build/examples/calc-parser
```

Windows 上可执行文件为 `build/bin/ParseEngine.exe`。默认目标语言是 C++17，默认分析模式是 IELR(1)；
命令行的 `--lang`、`--mode` 可覆盖文法里的 `%lang`、`%mode`。生成目录中有源码、构建文件和
`RESULT.md` 分析报告——状态数、冲突、可解析性都在里面（默认是简化版，要末尾那份完整
报告加 `--full-report`）。`.gram` 写法与全部命令行选项见
[使用手册](doc/12-grammar-usage.md)。

## 支持的目标语言与分析模式

| 输出 | 支持范围 |
| --- | --- |
| C99 | 13 种模式 |
| C++17、Python、C#、Rust、Go | 各 16 种模式 |
| Java、JavaScript/TypeScript | DD、Earley、CYK |
| `--lang gram` | 导出原文法与分析报告，覆盖全部 16 种模式 |

十六种模式包括 LL、LR、GLR、Earley、CYK、递归下降 DD 和仅生成词法器的 `lex`。各模式的准确支持
范围、限制和 C++17 / Rust 增量 API 以[支持矩阵](doc/15-language-mode-support.md)为准。

## 实现结构

| 位置 | 内容 |
| --- | --- |
| [`source/`](source/README.md) | 生成器本体：文法读取、各模式前端、代码发射、诊断与报告 |
| [`source/templates/`](source/templates/) | 各目标语言的产物模板（生成工程时填进去） |
| [`example/`](example/) | 示例文法与预期产物 |
| [`vscode/`](vscode/README.md) | `.gram` 的编辑器支持（高亮、补全、诊断） |
| `library/` | 与生成器并列的通用库（各库各有 README）；`library/transfer` 可把四元式降为目标机代码 |

实现细节见[架构文档](doc/03-architecture.md)，模式与后端的取舍见[支持矩阵](doc/15-language-mode-support.md)。

## 构建

<!-- docgen:build-summary:start -->
构建入口是 `build.sh`，实际步骤由 `build-all.sh` 维护。Windows 下使用 Git Bash；Linux/macOS 使用 POSIX Shell。脚本构建 ParseEngine、库和示例工程（Magel、MetroLang、Hydra86、RustView、CAS 已拆分为独立仓库），不运行测试；上述独立仓库不在统一构建清单中。

```sh
sh build.sh                  # 全量构建
sh build.sh --list           # 检测环境并显示清单，不执行构建
sh build.sh -q               # 安静模式，完整输出仍写入日志
sh clear.sh --dry-run        # 预览清理范围
sh clear.sh                 # 清理构建产物
```

构建入口产生的文件统一位于仓库根目录 `build/`。生成器在 `build/bin/`（release 在 `build/bin/release/`），Rust 在 `build/rust/<库名>/`，示例工程在 `build/project/<工程名>/`，日志在 `build/build-all/logs/`，全量示例产物在 `build/build-all/examples/`。Magel 在 `build/work/magel/` 的源码副本中生成和编译，依赖库在 `build/work/build/`，其他库沿用根目录 `build/` 下的目录；Magel、MetroLang、Hydra86、RustView 与 CAS 已拆分为独立仓库，构建产物在各自仓库里。

构建入口实际运行 `python3`、`python`，选择可用的 Python 3 解释器，跳过不可用的占位程序和 Python 2；检测结果传给生成器和 CAS。CAS 的 Makefile 单独运行时也会自动检测，可用 `PYTHON` 覆盖。C/C++ 编译器、make 和 Python 3 为必需工具；Rust、.NET 和 Go 工具链缺失时跳过对应部分。

清理仅限根目录 `build/` 内被 Git 忽略的产物，保留受版本控制文件、未被忽略的未跟踪文件和嵌套 Git 仓库。检测到 `tmp/build.lock` 时拒绝清理；旧版本留在其他目录的产物不在清理范围内。

上述路径适用于仓库统一构建入口。直接运行子工程的 Makefile、生成脚本或测试工具时，以该工程文档中的路径为准。

维护这段说明时修改 [统一配置](project/document/documentation.json) 或 [构建说明模板](project/document/templates/build-summary.md.tmpl)，再运行文档生成器。模板使用方法见 [文档维护规范](project/document/README.md)。
<!-- docgen:build-summary:end -->

## 门禁与打包

本地与 CI 用同一个入口，不需要 root：

```sh
sh ci/run.sh                                   # 构建生成器 + 跑 ParseEngine 自己的检查与回归
sh ci/run.sh --skip-vm-tests                   # 跳过较慢的那部分，快一点
python tools/package_compiler.py ParseEngine   # 打成 ParseEngine-<平台>.zip
```

## 文档与许可

[仓库文档总览](doc/01-overview.md)汇总所有文档；项目故事有[满级文法与一次大扫除](doc/02-gram-legend.md)、
[一份 .gram，八种语言](doc/04-parseengine-story.md)。协作与生成物维护规则见 [`AGENTS.md`](AGENTS.md)；
许可证见 [`LICENSE`](LICENSE)（MIT）。

## 相邻工程（分项目）

上面讲的是本仓库的主体 ParseEngine；下面这些工程与它并列，各自独立演进——其中几个的前端**就是
ParseEngine 生成的**（它们既是用户，也是最好的用例）。各工程的总览图在 [`tools/overview/`](tools/overview/)，
生成脚本是 `tools/make_*_overview.py`。

| 工程 | 是什么 | 入口 |
| --- | --- | --- |
| **Hydra86** | 8086 风格汇编器 + 多核虚拟机；可汇编、执行、反汇编、调试——**已拆分为独立仓库** | — |
| **MetroLang** | 地铁线网数据语言工具链：编译器、模板化反编译器、自包含 HTML/SVG 地图、Dijkstra 路线查询与交互式查看器；前端由 `metrolang.gram` 生成（169 个关键字各占一条产生式，842 状态 · 0 冲突）——**已拆分为独立仓库** | <https://gitee.com/rabbitc999/metro-lang> |
| **RustView** | 极简 Rust 伪代码的**借用语义可视化**（教学用）：Move / 借用 / 冲突 / 悬垂画成零依赖 SVG + HTML；前端由 `rust_subset.gram` 生成，1.0 功能已冻结——**已拆分为独立仓库** | <https://gitee.com/rabbitc999/rust-view> |
| **CAS** | C++17 符号计算系统：完整文法生成前端、自定义分配池、259 条积分规则构建期编译成节点——**已拆分为独立仓库** | — |
| **Magel** | 由 ParseEngine 生成前端、编译到 StackVM 的语言与工具链（标准库、宿主胶水、C ABI、板端 VM）——**已拆分为独立仓库** | <https://github.com/rabbitc999/magel> |

与生成器并列的通用库——[`library/allocator`](library/allocator/README.md)（C99 池与 `Memory`
命名空间的容器适配）、[`library/transfer`](library/transfer/README.md)（四元式 → 基本块 / CFG /
目标机）、[`library/preprocess`](library/preprocess/README.md)、[`library/template`](library/template/README.md)、
[`library/json`](library/json/README.md)、[`library/toml`](library/toml/README.md) 等——各有自己的 README。
