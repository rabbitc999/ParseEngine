# 01 仓库文档总览

> **Magel 的那段光辉历史**：这门语言早期就在本仓库里长大——生成器、StackVM、标准库、
> 胶水与反射都是在这里一起打磨出来的（下面这些 Magel 文档就是那段记录）。它现已拆分为
> 独立仓库 <https://gitee.com/rabbitc999/magel>；本文里指向 `magel/` 的链接都改指新仓库的
> 同名文档。
>
> **Hydra86 的那张地图**：它也在本仓库里长大——汇编器、多核虚拟机、PSF 快照与嵌入
> 接口都是在这里定型的。它现已拆分为独立仓库 <https://gitee.com/rabbitc999/hydra-86>；
> 本文里指向 `hydra-86/` 的链接都改指新仓库的同名文档。
>
> **RustView 的那条线**：它同样在本仓库里长大——极简 Rust 文法、借用分析与零依赖
> SVG / HTML 都是在这里定型的。它现已拆分为独立仓库 <https://gitee.com/rabbitc999/rust-view>；
> 本文里指向 `rust-view/` 的链接都改指新仓库的同名文档。
>
> **CAS 的那棵树**：它也在本仓库里长大——完整表达式文法、结构化积分表与自定义分配池
> 适配都是在这里定型的。它现已拆分为独立仓库 <https://gitee.com/rabbitc999/cas>；
> 本文里指向 `cas/` 的链接都改指新仓库的同名文档。

本页说明 ParseEngine 仓库各处文档的归属。仓库总入口是[根目录 README](../README.md)；生成器、Magel、Hydra86、MetroLang、RustView、CAS 各有独立工程文档。

## 独立工程

| 工程 | 文档 | 内容 |
| --- | --- | --- |
| ParseEngine 解析器生成器 | [使用入口](../source/README.md) · [专题文档索引](README.md) | `.gram` 文法、分析模式、八种目标语言、生成产物和实现 |
| Magel | [语言入口](https://gitee.com/rabbitc999/magel/blob/master/README.md) · [EBNF 语法参考](https://gitee.com/rabbitc999/magel/blob/master/doc/33-ebnf.md) · [专题文档](https://gitee.com/rabbitc999/magel/blob/master/doc/README.md) · [Magel 导出器](https://gitee.com/rabbitc999/magel/blob/master/magel-emit/README.md) | 语言语法基线、标准库、StackVM、胶水与导出 |
| Hydra86 | [项目入口](https://gitee.com/rabbitc999/hydra-86/blob/master/README.md) · [详细文档](https://gitee.com/rabbitc999/hydra-86/blob/master/docs/README.md) | 汇编、虚拟机、多核调度、嵌入接口（**独立仓库**） |
| MetroLang | [工具链入口](https://gitee.com/rabbitc999/metro-lang/blob/master/README.md) · [语言参考](https://gitee.com/rabbitc999/metro-lang/blob/master/doc/01-metrolang-reference.md) · [JSON 格式](https://gitee.com/rabbitc999/metro-lang/blob/master/doc/02-json-format.md) · [交互式查看器](https://gitee.com/rabbitc999/metro-lang/blob/master/map_view/README.md) | 地铁线网数据语言：文法、编译 / 反编译、SVG 地图与路线查询（**独立仓库**） |
| RustView | [项目入口](https://gitee.com/rabbitc999/rust-view/blob/master/README.md) · [效果图与演示页](https://gitee.com/rabbitc999/rust-view/blob/master/doc/preview.html) · [文法](https://gitee.com/rabbitc999/rust-view/blob/master/rust_subset.gram) | 极简 Rust 伪代码的借用语义可视化：ParseEngine 生成前端、作用域栈 + 借用状态表分析、手写 SVG / HTML；**1.0 功能冻结**（**独立仓库**） |
| CAS | [项目入口](https://gitee.com/rabbitc999/cas/blob/master/README.md) · [架构与验证](https://gitee.com/rabbitc999/cas/blob/master/doc/architecture.md) | 符号计算、结构化积分表与自定义分配池（**独立仓库**） |

## 解析器生成器文档

生成器的实现与手册与仓库其他文档同放本目录，编号见下表。

| 编号 | 文档 | 内容 |
| --- | --- | --- |
| 03 | [架构](03-architecture.md) | 生成器的模块与数据流 |
| 05 | [文法](05-grammar.md) | `.gram` 规范与校验 |
| 06 | [分析](06-analysis.md) | LL、LR、IELR 等算法 |
| 07 | [代码生成](07-codegen.md) | 模板与语言后端 |
| 08 | [生成代码](08-generated-code.md) | 产物结构与 API |
| 09 | [构建和测试](09-build-and-test.md) | 生成器构建、模板重建与回归 |
| 10 | [示例文法](10-examples.md) | `example/` 文法导读 |
| 11 | [工程示例](11-project-examples.md) | `project/` 项目导读 |
| 12 | [文法使用手册](12-grammar-usage.md) | 指令与语义动作 |
| 13 | [生成模式](13-modes.md) | 十六种分析和生成模式 |
| 14 | [目标语言与产物](14-languages-and-products.md) | 后端选择与产物形态 |
| 15 | [支持矩阵](15-language-mode-support.md) | 语言、模式和增量 API 的当前范围 |
| 16 | [三语、Windows 编码与 ZIP 发布](16-i18n-and-packaging.md) | 显示选项、Unicode 控制台、独立 Python 打包脚本 |

## 其他项目与工具

| 类别 | 文档 |
| --- | --- |
| 示例工程 | [project 总入口](../project/README.md) · [课程实验](../project/lab_report_nju/README.md) |
| 独立库 | [library 目录](../library/)；各库的 `README.md` 和技术报告随库维护 |
| 编辑器 | [VS Code 插件](../vscode/README.md) |
| 文档维护 | [统一口径、模板与文档生成器](../project/document/README.md) |
| 仓库协作 | [交接单](../AGENTS.md) · [工具操作手册](../SKILL.md) |
| 跨项目叙事 | [02 满级文法](02-gram-legend.md) · [05 一份 .gram，八种语言](05-parseengine-story.md) |

改动某个工程时，以该工程的源码、测试和本地文档为准；解析器的语言与模式范围统一见[支持矩阵](15-language-mode-support.md)。

## CAS 符号计算

[CAS 项目手册](https://gitee.com/rabbitc999/cas/blob/master/README.md)（**独立仓库**） · [架构与验证](https://gitee.com/rabbitc999/cas/blob/master/doc/architecture.md) · [内置符号清单](https://gitee.com/rabbitc999/cas/blob/master/doc/builtins.json)。

## 自定义分配器库

[`library/allocator`](../library/allocator/README.md) 提供链表池、线性池和 C++17 容器适配，命名空间为 `Memory`；CAS 使用 `Cas::Memory` 接入。构建产物位于 `build/allocator/`。

## Magel 二进制流解析

[`std::binary::BinaryReader`](https://gitee.com/rabbitc999/magel/blob/master/doc/12-binary-reader.md) 通过 C++ 胶水读取大小端基础数值、定长字符串与数组。

## Magel JSON 与 TOML

[JSON/TOML 解析接口](https://gitee.com/rabbitc999/magel/blob/master/doc/13-json-toml.md) 将 `library/json`、`library/toml` 的副本接入 Magel，提供解析、嵌套读取、遍历、诊断和序列化。
