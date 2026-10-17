# 显示语言、内嵌资产与发布

ParseEngine 命令行支持中文和英文。代码生成目标仍由 `--lang cpp17` 等选项控制；显示语言使用独立的 `--ui-lang`。

```sh
ParseEngine --help --ui-lang zh
ParseEngine --help --ui-lang en
ParseEngine --help --ui-lang ja
ParseEngine example/calc.gram --lang cpp17 --out-dir build/calc --ui-lang ja
```

显示语言的优先级为显式 `--ui-lang`、`PARSEENGINE_LANG`、`LC_ALL`、`LC_MESSAGES`、`LANG`，最后查询 Windows 用户 UI 语言。中文区域选择中文，其他区域选择英文；`auto` 忽略工具环境中的固定选择并重新检测区域。支持 `zh_CN.UTF-8` 等区域串；`C` / `POSIX` 选择英文。

帮助、命令行参数检查、生成完成与清理提示、打包结果及已登记的常见诊断使用双语表。专门分析诊断未收录译文时保留完整原文；分析报告、生成源码注释和模板维持原有文本。显示选项不修改文法、标识符、源码片段及生成代码。

权威消息表为 `source/i18n/messages.tsv`，运行 `python tools/embed_parseengine_i18n.py` 生成静态 C++ 表。消息随可执行文件内嵌，运行时不读语言资源文件。Make 和 CMake 在消息或发布脚本变化后同步生成内嵌资产；没有 Python 的构建环境可以使用已入库的生成头文件，但更改资产后须先在有 Python 的环境重新生成。

## 内嵌手册（`ParseEngine man`）

命令行带一份**内嵌手册**：`doc/` 与 `doc/en/` 的正文在构建期被 `tools/embed_manual.py` 压缩内联进 `source/src/manual_data.cpp`，运行时**不读仓库文件**，把可执行文件单独拷到别处也照样能查。

```sh
ParseEngine man                     # 目录：18 页文档 + 指令 / 模式 / 语言 / 示例清单
ParseEngine man 12-grammar-usage    # 看某一页（`man 12`、`man %token` 也行）
ParseEngine man directives          # 全部 %指令（含每条的一句话说明）
ParseEngine man modes               # 16 种生成模式：用法 + 一句话说明
ParseEngine man languages           # 目标语言与各自支持的模式数
ParseEngine man examples            # example/ 里的示例文法一览
ParseEngine man --search 展望符      # 全文搜关键词（页正文 / 指令 / 模式 / 语言）
ParseEngine man --json              # 机器可读（编辑器 / 脚本用）
ParseEngine man --lang en 04        # 正文语言：zh / en / ja / auto（缺省自动检测）
```

| 要点 | 说明 |
| --- | --- |
| 数据来源 | `doc/*.md` + `doc/en/*.md` + `example/manifest.tsv`；**不含** `09-build-and-test.md` 与 `09-extra.md`（那两页是仓库自己的构建与实验记录，不该随安装在别处的编译器一起发出去） |
| 重新内联 | 改完 `doc/` 后跑 `make -C source manual`（等价 `python tools/embed_manual.py`），再编译 |
| 体积 | 约 1.3 MB Markdown 用 `library/lztext` 压到约 0.4 MB（比率 ≈ 0.29），生成 2 MB 量级的 C++ 字节数组 |
| 中英对照 | 有英文版的页在目录里标 `[en]`，页脚写 `来源：doc/<名字>` / `英文版：doc/en/<名字>`；`--lang en` 或有英文版且区域是英文时取英文正文 |
| 分页与颜色 | 正文超过一屏且找得到 `less` 时用 `less -R` 分页；重定向或设了 `NO_COLOR` 时不出 ANSI 颜色 |
| 退出码 | 0 = 命中；1 = 没找到目标 / 搜索无结果；2 = 参数非法（与 `lsp` 的 `--check` 同口径） |

## 内嵌语言服务器与插件自带的编译器

`library/lsp` 的 `.gram` 语言服务器（`json` / `grammar` / `lsp` 三份源码）**一起编进生成器**，所以同一个可执行文件既是解析器生成器、又是语言服务器：

```sh
ParseEngine lsp --stdio              # 起 LSP（Content-Length 帧 + JSON-RPC）
ParseEngine lsp --check FILE...      # 只做一次校验，诊断打到标准输出
ParseEngine lsp --version            # parseengine-lsp 1.0.0
```

参数、退出码与诊断格式与独立构建的 `build/lsp/lsp-server[.exe]` 完全一致——`library/lsp` 仍然可以单独构建（`make -C library/lsp`），`lsp --check` 干净文法退出 0、有错退出 1。**VS Code 插件默认就用内嵌的这份**，并把编译器打进 `.vsix`：

| 环节 | 做法 |
| --- | --- |
| 找编译器 | `vscode/src/serverLocator.js` 按「设置 `gram.parseEnginePath` → 仓库 `build/bin` → 插件自带 `bin/` → `PATH`」依次找 `ParseEngine[.exe]` |
| 起服务器 | `ParseEngine lsp --stdio`（`gram.lspServerPath` 填了独立 `lsp-server` 就改用它，参数回到 `--stdio`） |
| 校验文法 | `ParseEngine lsp --check FILE`；诊断按 `error` / `warning` / `info` 映射到问题面板的三档 |
| 自带编译器 | 打包时 `vscode/tools/embed-compiler.js` 把 `ParseEngine[.exe]` 拷进 `vscode/bin/`（内容一致就跳过；`vscode/bin/` 不入库） |
| 平台标记 | `vscode/tools/package.js` 给 `.vsix` 的 `Identity` 写 `TargetPlatform`（如 `win32-x64`）并把同样的后缀缀进文件名；`--universal` 可以产出不带标记的通用包 |
| 打开手册 | `Gram: 打开文法指令手册` 优先打开仓库里的 `doc/12-grammar-usage.md`；仓库不在旁边时改用 `ParseEngine man 12-grammar-usage`，把同一份正文显示在编辑器里 |

插件自身的目录、命令与打包细节见 [`vscode/README.md`](../vscode/README.md)。

## Windows 编码

两个工具使用同一份 `library/console/utf8.hpp`：Windows 从 CRT 宽字符参数取得 UTF-8 argv，路径通过 `filesystem::u8path` 转换。C++ 控制台输出使用 `WriteConsoleW`；C stdio 和继承控制台的子进程在运行期间使用 UTF-8 代码页。退出前刷新输出并恢复原输入、输出代码页，不把用户窗口永久改成 65001。文件和管道始终输出 UTF-8，不添加 BOM，不改变 JSON 协议。

Windows PowerShell 5.1 可能按自己的旧编码解读外部程序管道；工具无法替父进程选择解码器。使用它的管道时可先设置：

```powershell
[Console]::OutputEncoding = New-Object System.Text.UTF8Encoding($false)
$OutputEncoding = [Console]::OutputEncoding
```

读取已重定向的 UTF-8 文本应显式使用 `Get-Content -Encoding UTF8 文件`。终端字体仍需包含中日文字形。

## Python ZIP 发布脚本

ZIP 是发布操作，由 `tools/package_compiler.py` 使用 Python 3.11+ 标准库实现，C++ 工具不包含 ZIP 算法。直接打包两个工具：

```sh
python tools/package_compiler.py ParseEngine out dist/ParseEngine-windows-x64.zip --ui-lang zh
python tools/package_compiler.py magel out dist/magel-windows-x64.zip --ui-lang ja
```

脚本默认读取 `build/bin/ParseEngine(.exe)` 或 `magel/magel(.exe)`，可用 `--compiler FILE` 指定其他构建。输出父目录须存在；默认名称含系统和架构，Magel 在仓库内还读取 VERSION。ZIP 只包含当前平台的编译器可执行文件，不包含宿主 Python、C++ 编译器和动态库。内嵌模板、标准库与消息仍保存在可执行文件中。

ParseEngine 另外提供薄入口，释放内嵌的同一份 Python 脚本后直接传递 argv，不经 shell：

```sh
ParseEngine zip
ParseEngine zip out dist/ParseEngine.zip --ui-lang ja
```

`PARSEENGINE_PYTHON` 可指定 Python 路径。脚本本身不依赖仓库即可打包明确指定的可执行文件；ParseEngine 薄入口因此也能在解压到仓库外后使用。Magel 的发布仅使用独立 Python 脚本，不提供 `magel zip` 指令。

打包使用 DEFLATE，排他创建文件；已有文件和链接拒绝覆盖，写入失败删除本次不完整归档。归档内统一为 `ParseEngine.exe` / `ParseEngine` 或 `magel.exe` / `magel`。不修改、重新链接编译器，不写注册表或安装系统组件。

## 验证

```sh
python tests/parseengine_i18n.py
python tests/magel_i18n.py --magel magel/magel.exe
python tests/magel_zip.py --magel magel/magel.exe
```

Windows 专项在 936 / 932 代码页的真实控制台缓冲区读取 UTF-16 字符，核对 C++ 流、C stdio、实际工具提示及代码页恢复；管道单独按 UTF-8 严格解码。另测含空格和中文的路径、显示选项优先级、生成源码一致、ZIP CRC、字节一致、拒绝覆盖、故障清理和仓库外运行。其他平台跳过 Windows 控制台检查。
