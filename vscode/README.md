# GramEditor for VS Code —— `.gram` 编辑插件

<img src="https://raw.githubusercontent.com/rabbitc999/ParseEngine/master/image/gram-editor-256.png" alt="GramEditor 图标" width="96" align="right" />

给 ParseEngine 的 `.gram` 文件提供完整的编辑体验：语法高亮、诊断、悬停、补全、
跳转定义、文档大纲、格式化、快速修复、一键生成解析器，以及 Token / 语法树预览。

**语言服务器内嵌在编译器里**：插件不再单独去找一个 `lsp-server`，而是用
`ParseEngine lsp --stdio` 起服务、`ParseEngine lsp --check FILE` 做校验；发布包
自带 `bin/ParseEngine[.exe]`（打包时由 `tools/embed-compiler.js` 拷进来），
装上就能用，不需要仓库、也不需要另装编译器。**Magel（`.mgl`）与 MetroLang
（`.metro` / `.m`）的支持都已经搬走**，见
[Magel 插件](https://github.com/rabbitc999/magel/tree/master/vscode)（那边的语言服务器
同样是编译器本体 `magel lsp`）；本插件只声明 `.gram` 一个后缀。

生成菜单与 `gram.defaultMode` 支持全部 16 种模式，包括 `earley` / `cyk`；
两种图表模式支持 C++17、Python、C#、Rust、Go、Java、JavaScript 目标产物。
菜单列出模式不代表每个语言都支持；完整组合与增量 API 见 [15 支持矩阵](https://github.com/rabbitc999/ParseEngine/blob/master/doc/15-language-mode-support.md)。

**零依赖**：不需要 `npm install`（`package.json` 没有 `dependencies` / `devDependencies`），
也不用打包构建。`vscode` 模块由宿主提供，其余全部用 Node 内置模块实现。
`npm run package` 只需要 Node 18+；只有 `npm run package:vsce` 会在缺少 `vsce` 时
经 `npx` 现拉一份（要联网）。

插件图标 `icon.png` 由 `python tools/make_icon.py` 生成（同一张图也导出成各尺寸 PNG
与 Windows `.ico` 供仓库文档使用），改图标改那个脚本即可。

## 大文法与增量生成

1.2 版本加入 UTF-16 范围同步、版本化诊断、请求取消、同版本扫描复用，以及生成配置、
按上次配置重跑、强制重建和可取消生成任务。生成默认启用二进制分析缓存，状态栏显示
缓存命中和耗时，同一输出目录的任务互斥。

配置示例、取消的边界与后续范围见 [23 VS Code 工作流](../doc/23-vscode-workflow.md)。

## 功能

| 功能 | 来源 | 说明 |
| --- | --- | --- |
| 语法高亮 | `syntaxes/gram.tmLanguage.json` | 指令、Token、非终结符、语义动作、字符串、注释、`$n`、`<成员>`；`%token` 的两个行内关键字 `include`（CSV 字面量别名表）与 `alias`（显示名）按关键字着色 |
| 诊断 | `ParseEngine lsp` | 打开与编辑时实时推送；波浪线 + 问题面板 |
| 悬停 | `ParseEngine lsp` | `%指令` / Token（含 CSV 别名来源与 `alias` 显示名）/ 非终结符 / `$n` 的说明 |
| 补全 | `ParseEngine lsp` | `%`、`<`、空格触发；行首补指令，取值处补候选，`%token NAME ` 后补 `include` / `alias`，规则区补符号 |
| 跳转定义 | `ParseEngine lsp` | Token / 非终结符引用 → 声明处 |
| 文档大纲 | `ParseEngine lsp` | 大纲视图（`Ctrl+Shift+O`） |
| 格式化 | `ParseEngine lsp` | 保守整理：去行尾空白、折叠空行、保证结尾换行 |
| 快速修复 | `ParseEngine lsp` | 灯泡菜单：补 `%token`、补产生式 |
| 校验文法 | `ParseEngine lsp --check` | 命令 `Gram: 校验当前文法` |
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

`.gram` 没有独立的 `FoldingRangeProvider`：折叠标记全部写在
`language-configuration.json` 里（`// #region` 一类），插件不再注册折叠能力。
`.mgl` 的宏块折叠在
[Magel 插件](https://github.com/rabbitc999/magel/tree/master/vscode) 里。

想确认注册是否完整，跑一句自检（打包前也会自动跑）：

```text
node vscode/tools/check-manifest.js
```

它会检查语言 id / 扩展名 / 语言配置文件 / scopeName / 激活事件 / 入口文件 /
`[gram]` 默认值是否都对得上，任何一项缺失都会以非 0 退出码结束。

## 使用前提

**装了发布的 `.vsix` 就什么都不用准备**：编译器（连同内嵌语言服务器）跟着包走，
放在 `bin/ParseEngine[.exe]`。

从源码跑（开发态）时才需要自己编一份：

```text
make                     # 生成 build/bin/ParseEngine
```

编译器路径按下面的顺序找，先命中先用：

1. 设置里的 `gram.parseEnginePath`；
2. 仓库里的 `build/bin/ParseEngine[.exe]`（从工作区目录向上依次找仓库根，判据是
   同时存在 `AGENTS.md`、`source/templates` 与 `library/lsp`；只在自己身处源码
   仓库里时才查这一步，免得刚 `make` 出来的新编译器被旧的打包副本盖住）；
3. **插件自带的 `bin/ParseEngine[.exe]`**（发布包里就有，装到别人机器上走的就是这条）；
4. `PATH` 里的 `ParseEngine`。

语言服务器跟着编译器走（`ParseEngine lsp …`），所以上面这一条链就够；只有想
自定义构建、或临时对照独立编译出的 `lsp-server` 时，才用得上：

| 设置项 | 说明 |
| --- | --- |
| `gram.lspServerPath` | 【一般不用填】独立构建的 `lsp-server` 路径；填了就改用它（参数回到 `--stdio` / `--check`） |
| `gram.parseEnginePath` | ParseEngine 可执行文件路径；留空时按上面的顺序自动找 |
| `gram.pythonPath` | 解析预览使用的 Python 3 命令或路径；留空时尝试 `python` / `python3` / `py -3` |
| `gram.defaultMode` | 生成对话框默认模式（如 `ielr`） |
| `gram.defaultLang` | 生成对话框默认语言（如 `cpp17`） |
| `gram.traceServer` | 设为 `messages` 会把语言服务器 stderr 写进输出面板 |

## 命令

命令面板（`Ctrl+Shift+P`）里输入 `Gram`：

| 命令 | 作用 |
| --- | --- |
| `Gram: 校验当前文法` | 跑 `ParseEngine lsp --check`，输出写进 "GramEditor" 面板，诊断刷进问题面板 |
| `Gram: 生成解析器…` | 依次选模式、目标语言、输出目录，调 `ParseEngine` 生成产物 |
| `Gram: 预览 Token 与语法树` | `F7` 打开预览；`Ctrl+Enter` 解析输入，可保存 CSV / PNG / JPEG / SVG |
| `Gram: 重启语言服务器` | 改完路径或重新构建后重启（状态栏右侧的 `Gram:` 也可点） |
| `Gram: 打开文法指令手册` | 优先打开仓库里的 `../doc/12-grammar-usage.md`；仓库不在旁边时改用内嵌手册 `ParseEngine man 12-grammar-usage`（按界面语言取正文） |

## 开发与调试

插件就是普通 JS，直接改文件即可。要在 VS Code 里调试：

1. 用 VS Code 打开仓库根目录（或直接打开 `vscode/` 目录）；
2. 按 `F5`，会启动一个"扩展开发宿主"窗口；
3. 在新窗口里打开任意 `.gram` 文件验证效果。

命令行冒烟测试（不需要 VS Code，直接驱动真实的内嵌语言服务器与生成器；想测独立
编译的 `lsp-server` 就把路径塞进环境变量 `LSP_SERVER`）：

```text
node vscode/test/smoke.js
node vscode/test/extension-smoke.js
node vscode/test/p0.js
```

三个脚本分工不同：

| 脚本 | 测什么 |
| --- | --- |
| `test/smoke.js` | LSP 全流程：初始化 → 打开文档 → 诊断 → 悬停 → 补全 → 跳转 → 大纲 → 格式化 → 快速修复；生成 Python / C++17 产物；LALR / GLR Token 与语法树预览 |
| `test/extension-smoke.js` | 用"桩 vscode"直接加载 `extension.js`：验证 6 个语言能力与 9 个命令、预览 Webview 与内联脚本、服务器启动和 `deactivate` 收尾 |

## 自动打包（.vsix）

入口统一走 npm（与 Magel 仓库一致），在 `vscode/` 里跑：

```text
npm run package                    # 全套：内嵌编译器 → 清单自检 → 冒烟测试 → 打包
npm run package -- --skip-tests    # 跳过冒烟测试（清单检查仍会跑）
npm run package -- --list          # 只列出会被打包的文件
npm run package -- --dry-run       # 只打印要跑的命令
npm run package -- --universal     # 不打平台标记，产出通用包
npm run package -- --out D:\pkg    # 指定输出目录，默认 vscode/dist

npm run package:vsce               # 改走官方 vsce（校验更严，能直接 vsce publish）
npm run embed                      # 只做内嵌编译器这一步
npm run check                      # 只做清单自检
npm test                           # 只跑两个冒烟测试
```

`npm run package` **不依赖 vsce**：自己用 Node 内置 zlib 生成 `.vsix`
（ZIP + `extension.vsixmanifest`），离线可用、零依赖。`npm run package:vsce`
是同一套前置检查之后改调 `vsce`，PATH 上没有 `vsce` 时会退回
`npx --yes @vscode/vsce`（要联网）。两条路的产物名一致：
`dist/gram-editor-<版本>-<平台>.vsix`。

不习惯 npm 时，`tools/package.cmd`、`tools/package.ps1`、`tools/package.sh`
三个入口等价于 `npm run package`，都转发到同一份 `tools/package.js`。

`npm run package` 按顺序做的事：

1. 跑 `tools/embed-compiler.js` 把编译器拷进 `bin/`（内容一样就跳过）；
2. 读 `package.json`，检查必需文件是否齐全（含 `bin/ParseEngine[.exe]`）；
3. 按 `.vscodeignore` 收集文件（测试、打包脚本、`dist/` 等都不进包；
   `bin/` **要**进包，那是自带编译器）；
4. 跑 `tools/check-manifest.js`（`gram` 语言注册自检）；
5. 跑 `test/smoke.js` 与 `test/extension-smoke.js`；
6. 生成 `[Content_Types].xml` 与 `extension.vsixmanifest`，压缩成
   `dist/gram-editor-<版本>-<平台>.vsix`，并打印安装命令。

**平台标记**：包里带的是当前机器编出来的 `ParseEngine`，所以清单的 `Identity`
会写上 `TargetPlatform`（如 `win32-x64`）、文件名也缀上同样的后缀，VS Code 只会
把它装到匹配的机器上——不然 macOS / Linux 用户装上会得到一个跑不起来的 Windows
可执行文件。要在别的平台发布，就在那个平台上构建并打包一次；确实需要通用包
（例如编译器交给 `PATH` 或 `gram.parseEnginePath`），加 `--universal`。

想手动只做内嵌这一步（例如先编好编译器再打两次包）：

```text
node vscode/tools/embed-compiler.js            # 自动找 build/bin 或 PATH
node vscode/tools/embed-compiler.js --from D:/tmp/ParseEngine.exe
node vscode/tools/embed-compiler.js --check    # 只检查 bin/ 里有没有，缺就退出 1
```

安装（Windows x64 上打出来的包）：

```text
code --install-extension vscode/dist/gram-editor-1.0.0-win32-x64.vsix
```

## 目录结构

```text
vscode/
├── package.json                 插件清单：语言、语法、命令、设置
├── language-configuration.json  注释、括号、自动闭合、折叠标记
├── syntaxes/
│   └── gram.tmLanguage.json     `.gram` TextMate 语法高亮
├── bin/                         自带的 ParseEngine[.exe]（打包时由 embed-compiler.js 拷入，不入库）
├── extension.js                 激活、服务器生命周期、文档同步、命令
├── src/
│   ├── providers.js             VS Code 语言能力（悬停 / 补全 / 跳转 / 大纲 / 格式化 / 修复）
│   ├── lspClient.js             最小 LSP 客户端（Content-Length 帧 + JSON-RPC）
│   ├── parseEngine.js           ParseEngine 生成与 `lsp --check` 的命令行封装
│   ├── preview.js               Python 预览器生成与结构化结果读取
│   ├── previewPanel.js          Token 表、完整树布局与 CSV / 图片导出 Webview
│   └── serverLocator.js         ParseEngine（自带 → 仓库 → PATH）与内嵌服务器参数
├── tools/
│   ├── package.js               `.vsix` 打包实现（自写 ZIP + 清单），`npm run package`
│   ├── package-vsce.js          改走官方 vsce 的打包，`npm run package:vsce`
│   ├── package.cmd / .ps1 / .sh 三种命令行入口（都转发到 package.js）
│   ├── embed-compiler.js        把编译好的 ParseEngine 拷进 bin/
│   └── check-manifest.js        语言注册自检（只认 `gram` 一个语言与 `.gram` 一个后缀）
├── test/smoke.js                冒烟测试（纯 Node）
├── test/extension-smoke.js      插件集成测试（桩 vscode + 真实服务器）
└── .vscode/launch.json          F5 调试配置
```

## 与语言服务的关系

插件的诊断、补全、悬停、跳转、大纲等能力都来自 `library/lsp` 那份语言服务——
它现在已经**编进生成器内部**，由 `ParseEngine lsp --stdio` 暴露；生成与预览则调用
`ParseEngine` 的普通命令行。两边同一份代码、同一个二进制，所以编辑器里的诊断与
生成时的报错天然一致：改 `library/lsp` 或 `source/`，重新 `make` 出来的编译器
同时更新了插件的行为（发布包记得再跑一次 `npm run package`）。

## 作者与发布者（清单字段）

| 字段 | 值 | 说明 |
| --- | --- | --- |
| `publisher` | `rabbitc999` | 发布者**标识**：必须是 ASCII，且与 marketplace 上的 publisher 账号一致。插件全名是 `<发布者>.<名字>`（本插件即 `rabbitc999.gram-editor`）——换标识等于换身份，装过旧标识的要卸掉重装。 |
| `author` | 小兔糖糖OvO `<rabbitc999@outlook.com>` | 详情页里显示的**作者**（人类可读名）。`publisher` 不收中文（vsce 会报 *Expected the identifier of a publisher, not its human-friendly name*），所以名字写在这里。 |
| `license` | `MIT` | 许可见 插件自身的 `license` 字段。 |
| `icon` | `icon.png` | 插件图标（魔术棒 + `C` + `Value`，由项目根的 `tools/make_icon.py` 生成）。 |

发布：先在 marketplace 建好 `rabbitc999` 这个 publisher 并配好 PAT，然后 `npx @vscode/vsce publish`。

