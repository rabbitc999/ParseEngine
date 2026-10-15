# 显示语言、Windows 编码与 ZIP 发布

ParseEngine 命令行支持中文、英文和日语。代码生成目标仍由 `--lang cpp17` 等选项控制；显示语言使用独立的 `--ui-lang`。

```sh
ParseEngine --help --ui-lang zh
ParseEngine --help --ui-lang en
ParseEngine --help --ui-lang ja
ParseEngine example/calc.gram --lang cpp17 --out-dir build/calc --ui-lang ja
```

显示语言的优先级为显式 `--ui-lang`、`PARSEENGINE_LANG`、`LC_ALL`、`LC_MESSAGES`、`LANG`，最后查询 Windows 用户 UI 语言。中文区域选择中文，日文区域选择日语，其他区域选择英文；`auto` 忽略工具环境中的固定选择并重新检测区域。支持 `zh_CN.UTF-8` / `ja_JP.UTF-8` 等区域串；`C` / `POSIX` 选择英文。

帮助、命令行参数检查、生成完成与清理提示、打包结果及已登记的常见诊断使用三语表。专门分析诊断未收录译文时保留完整原文；分析报告、生成源码注释和模板维持原有文本。显示选项不修改文法、标识符、源码片段及生成代码。

权威消息表为 `source/i18n/messages.tsv`，运行 `python tools/embed_parseengine_i18n.py` 生成静态 C++ 表。消息随可执行文件内嵌，运行时不读语言资源文件。Make 和 CMake 在消息或发布脚本变化后同步生成内嵌资产；没有 Python 的构建环境可以使用已入库的生成头文件，但更改资产后须先在有 Python 的环境重新生成。

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

Windows 专项在 936 / 932 代码页的真实控制台缓冲区读取 UTF-16 字符，核对 C++ 流、C stdio、实际工具提示及代码页恢复；管道单独按 UTF-8 严格解码。另测含空格和中日文的路径、三语选项优先级、生成源码一致、ZIP CRC、字节一致、拒绝覆盖、故障清理和仓库外运行。其他平台跳过 Windows 控制台检查。
