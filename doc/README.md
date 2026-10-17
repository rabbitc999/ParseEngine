# 仓库文档

> **中文（默认）** · [English](en/README.md) —— 英文版与中文版内容对应；中文版优先维护，
> 两处说法有出入时以中文版为准。尚无独立英文译文的页面由英文索引链到中文原文。

> **Magel 的那段光辉历史**：这门语言早期就在本仓库里长大——生成器、StackVM、标准库、
> 胶水与反射都是在这里一起打磨出来的（下面这些 Magel 文档就是那段记录）。它现已拆分为
> 独立仓库 <https://github.com/rabbitc999/magel>；本文里指向 `magel/` 的链接都改指新仓库的
> 同名文档。

ParseEngine 的[中英日显示、内嵌手册与语言服务器、发布说明](16-i18n-and-packaging.md)覆盖 `--ui-lang`、`man` 与 `lsp` 子命令、控制台与管道、两个工具的独立打包脚本。

Magel 1.0.0 的名称、`.mgl` 工程布局与中文／英文／日语语言选择见[Magel 名称与三语说明](https://github.com/rabbitc999/magel/blob/master/doc/32-naming-i18n.md)。
Magel 的[文档索引](https://github.com/rabbitc999/magel/blob/master/doc/README.md)结合正式 example 源码、expected 输出与错误边界逐步讲解。初学者从 [08 基础程序](https://github.com/rabbitc999/magel/blob/master/doc/08-first-programs.md) 开始，专项环境和验收范围见 [28 样例验证](https://github.com/rabbitc999/magel/blob/master/doc/28-library-migration.md)。

Magel 当前语言基线：**1.0.0**，当前版本：**1.2.0**。

引用来源与词法生命周期检查见 [04 引用与迭代](https://github.com/rabbitc999/magel/blob/master/doc/04-references.md)，专项回归为 `tests/magel_reference_escape.py`。

`T &` 默认只读引用见 [引用语法糖](https://github.com/rabbitc999/magel/blob/master/doc/04-references.md#默认只读引用语法糖)；未分配区域的 panic、自定义堆范围检查及 AOT 默认 unchecked / 显式 checked 见 [23 内存分配器](https://github.com/rabbitc999/magel/blob/master/doc/23-memory-allocator.md)，专项为 `tests/magel_reference_sugar.py` 和 `tests/magel_memory_guard.py`。

引用与泛型、枚举、回调、迭代器、接口及 Drop 的跨功能组合见 [reference_combinations.mgl](https://github.com/rabbitc999/magel/blob/master/example/samples/reference_combinations.mgl)，专项回归为 `tests/magel_reference_combinations.py`，覆盖正常解释 / AOT 与非法逃逸诊断。

从 [01 仓库文档总览](01-overview.md) 进入。此目录是**仓库全部文档的唯一入口**：整个仓库的导航、跨项目说明，以及 ParseEngine 解析器生成器的手册与实现文档都放在这里。

| 文档 | 内容 |
| --- | --- |
| [01 仓库文档总览](01-overview.md) | 各工程、附加库及全部文档的入口 |
| [02 满级文法](02-gram-legend.md) | 跨解析器、Magel 和课程实验的虚构外传，续写 Magel 1.0、迭代链与多数据库验收 |
| [03 架构](03-architecture.md) | 生成器的模块、数据流与代码约定 |
| [04 一份 .gram，八种语言](04-parseengine-story.md) | ParseEngine 生成器的演进故事 |
| [05 文法](05-grammar.md) | `.gram` 语法规范与生成期校验 |
| [06 分析](06-analysis.md) | LL、LR、IELR 等算法 |
| [07 代码生成](07-codegen.md) | 模板系统与语言后端 |
| [08 生成代码](08-generated-code.md) | 产物清单与 API |
| [09 构建和测试](09-build-and-test.md) | 生成器构建、模板重建与回归 |
| [09 封存外传](09-extra.md) | 从 02 撤下的段落，以「文法即钥匙」封存；输入那句封印语、由仓库的解析器生成器拆封 |
| [10 示例文法](10-examples.md) | `example/` 导读 |
| [11 工程示例](11-project-examples.md) | `project/` 导读 |
| [12 文法使用手册](12-grammar-usage.md) | 指令、语义动作与错误恢复 |
| [13 生成模式](13-modes.md) | 十六种生成模式与运行时 |
| [14 目标语言与产物](14-languages-and-products.md) | 后端选择与产物形态 |
| [15 支持矩阵](15-language-mode-support.md) | 语言、模式与增量 API 的当前范围 |
| [16 双语、内嵌资产与发布](16-i18n-and-packaging.md) | 显示选项、内嵌手册与语言服务器、Unicode 控制台、独立 Python 打包脚本 |
| [Magel EBNF 语法参考](https://github.com/rabbitc999/magel/blob/master/doc/33-ebnf.md) | 当前词法、全部产生式、匿名函数类型、泛型入口与语义检查边界 |
| [Magel 语言哲学](https://github.com/rabbitc999/magel/blob/master/doc/00-philosophy.md) | 简单编写，到处运行；值语义、显式引用、资源管理与没有移动语义 |
| [文档维护规范](../project/document/README.md) | 统一口径、技术文档模板、文档生成器与同步检查 |

## mc 连外部数据库

Magel 支持 MySQL、SQLite、PostgreSQL 与 Microsoft SQL Server / ODBC 的标准库模块
[`std::sql`](https://github.com/rabbitc999/magel/blob/master/std/sql.mgl)、宿主胶水 [`glue/std_sql.hpp`](https://github.com/rabbitc999/magel/blob/master/glue/std_sql.hpp)，
以及 Magel 工程内的[学生信息管理示例工程](https://github.com/rabbitc999/magel/blob/master/sql-magel/README.md)；保留的建表 / 初始数据 /
查询 SQL 在 [`magel/sql-magel/sql/`](https://github.com/rabbitc999/magel/blob/master/sql-magel/sql/schema.sql)，库的统一入口与后端选择见
[19 SQL 数据库](https://github.com/rabbitc999/magel/blob/master/doc/19-sql.md)，详细接口见 [sql-magel 接口参考](https://github.com/rabbitc999/magel/blob/master/sql-magel/doc/01-sql-library.md)。

## mc 连外部网络

[18 TCP 与极简 HTTP](https://github.com/rabbitc999/magel/blob/master/doc/18-network.md)：标准库模块
[`std::net`](https://github.com/rabbitc999/magel/blob/master/std/net.mgl) 与宿主胶水 [`glue/std_net.hpp`](https://github.com/rabbitc999/magel/blob/master/glue/std_net.hpp)，
提供 `TcpSocket` / `TcpListener`（连接超时、行读取）与 `HttpClient`（GET / POST、
`parseUrl` / `buildRequest` / `parseResponse`）；确定性样例是
[`magel/example/samples/std_net.mgl`](https://github.com/rabbitc999/magel/blob/master/example/samples/std_net.mgl)。

Magel 的后置泛型声明、函数与回调类型推导、枚举变体简写见[泛型与集合文档](https://github.com/rabbitc999/magel/blob/master/doc/10-abstractions-and-collections.md#类型推导与枚举简写)。

Magel 的 `ExplicitConvert<T>` / `ImplicitConvert<T>`、显式选择规则与转换结果所有权见[类型转换接口](https://github.com/rabbitc999/magel/blob/master/doc/10-abstractions-and-collections.md#显式与隐式类型转换接口)。

## mc 集合字面量与键协议

[泛型与集合文档](https://github.com/rabbitc999/magel/blob/master/doc/10-abstractions-and-collections.md#set--map-字面量与键协议)说明 Set `{a,b,c}`、Map `{key:value,...}`、空集合上下文推导、自定义键的 `Equal<bool,T>` 和可选 `Hash<T>`。

## mc 值语义与所有权

Magel 支持 `static T new(...)` [构造入口](https://github.com/rabbitc999/magel/blob/master/doc/09-values-and-resources.md#static-t-new构造入口)：自动提供 self 并返回拥有值，禁止丢弃结果；默认声明仍合法。宏临时变量支持 `$0`～`$9`。

[03 值语义与链式调用](https://github.com/rabbitc999/magel/blob/master/doc/03-value-semantics.md)：字符串 / 容器 / 大数与结构体
一样是值类型（`String a = b;` 是复制不是别名），编译器通过内部临时变量管理 `f().g()`
等链式调用的中间值，完整表达式结束后自动析构、释放资源并清空内部槽，保留最终值和所需的借用来源，并说明相关修复及回归；迭代链见
[05 标准迭代器](https://github.com/rabbitc999/magel/blob/master/doc/05-iterators.md)。

Magel 的两层增量缓存、共享依赖校验和限制见[部署文档](https://github.com/rabbitc999/magel/blob/master/doc/22-deployment.md)。

## 统一构建目录

<!-- docgen:build-summary:start -->
构建入口是 `build.sh`，实际步骤由 `build-all.sh` 维护。Windows 下使用 Git Bash；Linux/macOS 使用 POSIX Shell。脚本构建 ParseEngine、库和示例工程（Magel、MetroLang、RustView 已拆分为独立仓库），不运行测试；上述独立仓库不在统一构建清单中。

```sh
sh build.sh                  # 全量构建
sh build.sh --list           # 检测环境并显示清单，不执行构建
sh build.sh -q               # 安静模式，完整输出仍写入日志
sh clear.sh --dry-run        # 预览清理范围
sh clear.sh                 # 清理构建产物
```

构建入口产生的文件统一位于仓库根目录 `build/`。生成器在 `build/bin/`（release 在 `build/bin/release/`），Rust 在 `build/rust/<库名>/`，示例工程在 `build/project/<工程名>/`，日志在 `build/build-all/logs/`，全量示例产物在 `build/build-all/examples/`。Magel 在 `build/work/magel/` 的源码副本中生成和编译，依赖库在 `build/work/build/`，其他库沿用根目录 `build/` 下的目录；Magel、MetroLang 与 RustView 已拆分为独立仓库，构建产物在各自仓库里。

构建入口实际运行 `python3`、`python`，选择可用的 Python 3 解释器，跳过不可用的占位程序和 Python 2；检测结果传给生成器，也可用 `PYTHON` 覆盖选择结果。C/C++ 编译器、make 和 Python 3 为必需工具；Rust、.NET 和 Go 工具链缺失时跳过对应部分。

清理仅限根目录 `build/` 内被 Git 忽略的产物，保留受版本控制文件、未被忽略的未跟踪文件和嵌套 Git 仓库。检测到 `tmp/build.lock` 时拒绝清理；旧版本留在其他目录的产物不在清理范围内。

上述路径适用于仓库统一构建入口。直接运行子工程的 Makefile、生成脚本或测试工具时，以该工程文档中的路径为准。

维护这段说明时修改 [统一配置](../project/document/documentation.json) 或 [构建说明模板](../project/document/templates/build-summary.md.tmpl)，再运行文档生成器。模板使用方法见 [文档维护规范](../project/document/README.md)。
<!-- docgen:build-summary:end -->

## 自定义分配器库

[`library/allocator`](../library/allocator/README.md) 提供链表池、线性池和 C++17 容器适配，命名空间为 `Memory`。构建产物位于 `build/allocator/`。

## Magel 二进制流解析

[`std::binary::BinaryReader`](https://github.com/rabbitc999/magel/blob/master/doc/12-binary-reader.md) 通过 C++ 胶水读取大小端基础数值、定长字符串与数组。
核心 [lang IEEE 754 双向接口](https://github.com/rabbitc999/magel/blob/master/doc/06-runtime.md#ieee-754-字节双向转换) 提供 float32/float64 与四/八字节列表及数组互转。

## Magel JSON 与 TOML

[JSON/TOML 解析接口](https://github.com/rabbitc999/magel/blob/master/doc/13-json-toml.md) 将 `library/json`、`library/toml` 的副本接入 Magel，提供解析、嵌套读取、遍历、诊断和序列化。

[Magel 引用与迭代](https://github.com/rabbitc999/magel/blob/master/doc/04-references.md)：非空引用、默认解引用、只读转换、字符串 / 大数 / 集合 / 字典 / 元组引用、字符单字节读写、只读方法检查、列表元素写回与 P1 三后端回归。

[Magel 库迁移与回归验证](https://github.com/rabbitc999/magel/blob/master/doc/28-library-migration.md)：标准库、机器人、GUI、小车与海龟测试入口。

Magel 的无名变长符号表、magel/CLI 开关、空间实测与逆向成本见[符号压缩文档](https://github.com/rabbitc999/magel/blob/master/doc/25-compact-symbols.md)。

[Magel PC 字节码加密](https://github.com/rabbitc999/magel/blob/master/doc/26-svmb-crypto.md)：独立 OpenSSL 工具、密钥管理与完整性验收。

[Magel 可选 panic 调用栈](https://github.com/rabbitc999/magel/blob/master/doc/27-panic-trace.md)：宏裁剪与隐藏符号时的诊断策略。

Magel 行级调试支持宏正文、include 与子模块来源，见 [panic 与源码行号](https://github.com/rabbitc999/magel/blob/master/doc/27-panic-trace.md#magel-行级来源)。

[Magel 元组只读引用回归](https://github.com/rabbitc999/magel/blob/master/example/samples/p2_tuple_readonly.mgl)：元组元素与嵌套来源的只读引用、整体替换及独立复制；语义说明见 [04 引用与迭代](https://github.com/rabbitc999/magel/blob/master/doc/04-references.md)。

[Magel 标准迭代器](https://github.com/rabbitc999/magel/blob/master/doc/05-iterators.md)：惰性链式适配、列表引用迭代、所有权转移与消费操作。

[Magel 导入与名称解析](https://github.com/rabbitc999/magel/blob/master/doc/02-name-resolution.md)说明无需 using 的库名称简写、用户声明优先和同名导入消歧。

Magel 注解与反射的编译期注解、`@Reflect` 自动元数据生成、Magel 类型化查询与全部形式调用桥接、三后端端到端验收与性能报告、实现范围、默认关闭开关与产物兼容规则见 [30 注解与反射](https://github.com/rabbitc999/magel/blob/master/doc/30-reflection.md)。

Magel 类型与内存审计修复、loop 拥有值退出及借用返回的三后端回归见 [值与资源](https://github.com/rabbitc999/magel/blob/master/doc/09-values-and-resources.md#类型与所有权审计回归)。

[Magel 新建应用工程（magel new）](https://github.com/rabbitc999/magel/blob/master/doc/21-packages.md#新建应用工程)

[Magel 简短场景指令与拖入文件运行](https://github.com/rabbitc999/magel/blob/master/doc/31-cli.md)。

Magel 运算符所有权与提前退出测试见[09 值与资源](https://github.com/rabbitc999/magel/blob/master/doc/09-values-and-resources.md#运算符与提前退出边界回归322)。

[Magel Web 学生信息管理系统](https://github.com/rabbitc999/magel/blob/master/example/web-students/README.md)：真实 HTTP、SQLite、浏览器页面和三后端验收。

Magel 仅保留 AOT 与 `.svmb` 编译产物，删除内嵌 VM 导出。参见[导出器说明](https://github.com/rabbitc999/magel/blob/master/magel-emit/README.md)与[场景指令](https://github.com/rabbitc999/magel/blob/master/doc/31-cli.md)。

Magel 的 [lambda 与函数指针回调](https://github.com/rabbitc999/magel/blob/master/doc/01-language.md#lambda-与函数指针回调)支持迭代器链与回调签名推导，支持独立的按值捕获环境，并拒绝引用和缺少安全复制协议的资源捕获；完整样例见[lambda_callbacks.mgl](https://github.com/rabbitc999/magel/blob/master/example/samples/lambda_callbacks.mgl)。

[Magel 递归与嵌套所有权](https://github.com/rabbitc999/magel/blob/master/doc/03-value-semantics.md#9-递归局部存储与嵌套拥有值)：局部聚合、引用槽、数组、函数值与接口组合；[计算器工具链示例](https://github.com/rabbitc999/magel/blob/master/example/calculator/README.md)包含扁平与嵌套两种 AST。
