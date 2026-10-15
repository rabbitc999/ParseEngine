# GramEditor for VS Code —— `.gram` 编辑插件

<img src="../image/gram-editor-256.png" alt="GramEditor 图标" width="96" align="right" />

给 ParseEngine 的 `.gram` 文件提供完整的编辑体验：语法高亮、诊断、悬停、补全、
跳转定义、文档大纲、格式化、快速修复、一键生成解析器，以及 Token / 语法树预览。
**Magel（`.mgl`）与 MetroLang（`.metro` / `.m`）的支持都已经搬走**：见
[`magel/vscode/`](../magel/vscode/README.md)（那边的语言服务器就是编译器本体
`magel lsp`）与 metro-lang 独立仓库的 [`vscode/`](https://gitee.com/rabbitc999/metro-lang/blob/master/vscode/README.md)；
本插件不再声明这两个后缀。
生成菜单与 `gram.defaultMode` 支持全部 16 种模式，包括 `earley` / `cyk`；
两种图表模式支持 C++17、Python、C#、Rust 目标产物。
菜单列出模式不代表每个语言都支持；完整组合与增量 API 见 [15 支持矩阵](../doc/15-language-mode-support.md)。

**零依赖**：不需要 `npm install`，也不用打包构建。`vscode` 模块由宿主提供，
其余全部用 Node 内置模块实现。

插件图标 `icon.png` 由 `python tools/make_icon.py` 生成（同一张图也导出成各尺寸 PNG
与 Windows `.ico` 供仓库文档使用），改图标改那个脚本即可。

## 功能

| 功能 | 来源 | 说明 |
| --- | --- | --- |
| 语法高亮 | `syntaxes/gram.tmLanguage.json` | 指令、Token、非终结符、语义动作、字符串、注释、`$n`、`<成员>`；`%token` 的两个行内关键字 `include`（CSV 字面量别名表）与 `alias`（显示名）按关键字着色 |
| 诊断 | `lsp-server` | 打开与编辑时实时推送；波浪线 + 问题面板 |
| 悬停 | `lsp-server` | `%指令` / Token（含 CSV 别名来源与 `alias` 显示名）/ 非终结符 / `$n` 的说明 |
| 补全 | `lsp-server` | `%`、`<`、空格触发；行首补指令，取值处补候选，`%token NAME ` 后补 `include` / `alias`，规则区补符号 |
| 跳转定义 | `lsp-server` | Token / 非终结符引用 → 声明处 |
| 文档大纲 | `lsp-server` | 大纲视图（`Ctrl+Shift+O`） |
| 格式化 | `lsp-server` | 保守整理：去行尾空白、折叠空行、保证结尾换行 |
| 快速修复 | `lsp-server` | 灯泡菜单：补 `%token`、补产生式 |
| 校验文法 | `lsp-server --check` | 命令 `Gram: 校验当前文法` |
| 生成解析器 | `ParseEngine` | 命令 `Gram: 生成解析器…`，选模式 / 语言 / 输出目录 |
| 解析预览 | `ParseEngine --syntax-only` + Python | `F7` 打开预览，支持 LALR(1) / LR(1) / IELR(1) / GLR；输入文本后显示 Token 表与完整层级语法树 |
| 预览导出 | Webview | Token 保存为 UTF-8 CSV；语法树保存为 PNG、JPEG 或 SVG，GLR 多棵树统一排版 |

## `gram` 语言注册

插件在 `package.json` 里把 `gram` 语言注册齐了，装好之后 `.gram` 文件会自动
被识别、上色并激活插件：

| 注册项 | 位置 | 内容 |
| --- | --- | --- |
| 语言 id | `contributes.languages` | `id: "gram"`、`extensions: [".gram"]`、别名 `ParseEngine Grammar` / `gram` |
| 语言配置 | `contributes.languages[].configuration` → `language-configuration.json` | 行注释 `//`、块注释 `/* */`、括号、自动闭合；`.gram` 的折叠标记也在这里 |
| 语法高亮 | `contributes.grammars` | `language: "gram"`、`scopeName: "source.gram"` → `syntaxes/gram.tmLanguage.json` |
| 激活时机 | `activationEvents` | `onLanguage:gram`（打开 `.gram` 时才激活，不拖慢启动） |
| 编辑器默认值 | `contributes.configurationDefaults` | `[gram]`：Tab 4 空格、关掉基于词的补全、保存时去行尾空白并保证结尾换行 |
| 语言服务 | `extension.js` | 打开文档时用 `languageId: "gram"` 发 `textDocument/didOpen`，服务器据此给出诊断等能力 |

`.mgl` 的宏块折叠在 [`magel/vscode/`](../magel/vscode/README.md) 里，
`.metro` / `.m` 的块折叠在 metro-lang 独立仓库的 `vscode/` 里（两边都是
`FoldingRangeProvider`，本插件不再注册折叠）。

想确认注册是否完整，跑一句自检（打包前也会自动跑）：

```text
node vscode/tools/check-manifest.js
```

它会检查语言 id / 扩展名 / 语言配置文件 / scopeName / 激活事件 / 入口文件 /
`[gram]` 默认值是否都对得上，任何一项缺失都会以非 0 退出码结束。

## 使用前提

插件要能找到仓库里的两个可执行文件（默认自动探测）：

```text
make                     # 生成 build/bin/ParseEngine
make -C library/lsp      # 生成 build/lsp/lsp-server
```

探测规则：从当前工作区目录向上找仓库根（判据是同时存在 `AGENTS.md`、
`source/templates` 与 `library/lsp`），再按约定路径取
`build/lsp/lsp-server[.exe]` 与 `build/bin/ParseEngine[.exe]`。

工具不在默认位置时，在设置里指定：

| 设置项 | 说明 |
| --- | --- |
| `gram.lspServerPath` | lsp-server 可执行文件路径 |
| `gram.parseEnginePath` | ParseEngine 可执行文件路径 |
| `gram.pythonPath` | 解析预览使用的 Python 3 命令或路径；留空时尝试 `python` / `python3` / `py -3` |
| `gram.defaultMode` | 生成对话框默认模式（如 `ielr`） |
| `gram.defaultLang` | 生成对话框默认语言（如 `cpp17`） |
| `gram.traceServer` | 设为 `messages` 会把服务器 stderr 写进输出面板 |

## 命令

命令面板（`Ctrl+Shift+P`）里输入 `Gram`：

| 命令 | 作用 |
| --- | --- |
| `Gram: 校验当前文法` | 跑 `lsp-server --check`，输出写进 "GramEditor" 面板，诊断刷进问题面板 |
| `Gram: 生成解析器…` | 依次选模式、目标语言、输出目录，调 `ParseEngine` 生成产物 |
| `Gram: 预览 Token 与语法树` | `F7` 打开预览；`Ctrl+Enter` 解析输入，可保存 CSV / PNG / JPEG / SVG |
| `Gram: 重启语言服务器` | 改完路径或重新构建后重启（状态栏右侧的 `Gram:` 也可点） |
| `Gram: 打开文法指令手册` | 打开仓库里的 `../doc/12-grammar-usage.md` |

## 开发与调试

插件就是普通 JS，直接改文件即可。要在 VS Code 里调试：

1. 用 VS Code 打开仓库根目录（或直接打开 `vscode/` 目录）；
2. 按 `F5`，会启动一个"扩展开发宿主"窗口；
3. 在新窗口里打开任意 `.gram` 文件验证效果。

命令行冒烟测试（不需要 VS Code，直接驱动真实的 `lsp-server` 与 `ParseEngine`）：

```text
node vscode/test/smoke.js
node vscode/test/extension-smoke.js
```

两个脚本分工不同：

| 脚本 | 测什么 |
| --- | --- |
| `test/smoke.js` | LSP 全流程：初始化 → 打开文档 → 诊断 → 悬停 → 补全 → 跳转 → 大纲 → 格式化 → 快速修复；生成 Python / C++17 产物；LALR / GLR Token 与语法树预览 |
| `test/extension-smoke.js` | 用"桩 vscode"直接加载 `extension.js`：验证 6 个语言能力与 5 个命令、预览 Webview 与内联脚本、服务器启动和 `deactivate` 收尾 |

## 自动打包（.vsix）

打包脚本自己生成 `.vsix`（ZIP + `extension.vsixmanifest`），**不依赖 vsce**，
所以不需要联网装 npm 包；三种入口都转发到同一份实现：

```text
# 任意平台（推荐）
node vscode/tools/package.js

# Windows CMD
vscode\tools\package.cmd

# Windows PowerShell（执行策略受限时加 -ExecutionPolicy Bypass）
powershell -ExecutionPolicy Bypass -File vscode/tools/package.ps1

# POSIX shell
sh vscode/tools/package.sh
```

常用参数（三种入口通用）：

| 参数 | 作用 |
| --- | --- |
| `--list` | 只列出会被打包的文件，不生成 .vsix |
| `--skip-tests` | 跳过两个冒烟测试（清单检查仍会跑） |
| `--out <目录>` | 指定输出目录，默认 `vscode/dist` |

脚本做的事，按顺序：

1. 读 `package.json`，检查必需文件是否齐全；
2. 跑 `tools/check-manifest.js`（`gram` 语言注册自检）；
3. 跑 `test/smoke.js` 与 `test/extension-smoke.js`；
4. 按 `.vscodeignore` 收集文件（测试、打包脚本、`dist/` 等都不进包）；
5. 生成 `[Content_Types].xml` 与 `extension.vsixmanifest`，压缩成
   `dist/gram-editor-<版本>.vsix`，并打印安装命令。

安装：

```text
code --install-extension vscode/dist/gram-editor-1.0.0.vsix
```

## 目录结构

```text
vscode/
├── package.json                 插件清单：语言、语法、命令、设置
├── language-configuration.json  注释、括号、自动闭合、折叠标记
├── syntaxes/
│   └── gram.tmLanguage.json     `.gram` TextMate 语法高亮
├── extension.js                 激活、服务器生命周期、文档同步、命令
├── src/
│   ├── providers.js             VS Code 语言能力（悬停 / 补全 / 跳转 / 大纲 / 格式化 / 修复）
│   ├── lspClient.js             最小 LSP 客户端（Content-Length 帧 + JSON-RPC）
│   ├── parseEngine.js           ParseEngine 与 `--check` 的命令行封装
│   ├── preview.js               Python 预览器生成与结构化结果读取
│   ├── previewPanel.js          Token 表、完整树布局与 CSV / 图片导出 Webview
│   └── serverLocator.js         仓库根与两个可执行文件的定位
├── tools/
│   ├── package.js               `.vsix` 打包实现（自写 ZIP + 清单）
│   ├── package.cmd / .ps1 / .sh 三种命令行入口
│   └── check-manifest.js        语言注册自检（`gram`；并确认没重复声明 `.mgl` / `.metro`）
├── test/smoke.js                冒烟测试（纯 Node）
├── test/extension-smoke.js      插件集成测试（桩 vscode + 真实服务器）
└── .vscode/launch.json          F5 调试配置
```

## 与语言服务的关系

插件的诊断、补全、悬停、跳转、大纲等能力都来自仓库里的 `library/lsp` 语言服务，
生成与预览则调用 `ParseEngine` 命令行，因此行为与生成器保持一致：
改语言服务只需要改 `library/lsp`，插件会跟着变。

## 作者与发布者（清单字段）

| 字段 | 值 | 说明 |
| --- | --- | --- |
| `publisher` | `rabbitc999` | 发布者**标识**：必须是 ASCII，且与 marketplace 上的 publisher 账号一致。插件全名是 `<发布者>.<名字>`（本插件即 `rabbitc999.gram-editor`）——换标识等于换身份，装过旧标识的要卸掉重装。 |
| `author` | 小兔糖糖OvO `<rabbitc999@outlook.com>` | 详情页里显示的**作者**（人类可读名）。`publisher` 不收中文（vsce 会报 *Expected the identifier of a publisher, not its human-friendly name*），所以名字写在这里。 |
| `license` | `MIT` | 许可见 插件自身的 `license` 字段。 |
| `icon` | `icon.png` | 插件图标（魔术棒 + `C` + `Value`，由项目根的 `tools/make_icon.py` 生成）。 |

发布：先在 marketplace 建好 `rabbitc999` 这个 publisher 并配好 PAT，然后 `npx @vscode/vsce publish`。

