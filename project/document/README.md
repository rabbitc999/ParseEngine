# 文档模板与生成器

本项目统一仓库公共说明，并提供可复用的 Markdown 技术文档模板。生成器使用 Python，维护信息只需修改 JSON 配置和模板。可按实际需求使用第三方库，新增依赖时同步记录安装方法。它是仓库维护工具，不属于 ParseEngine 文法示例，统一构建脚本无需构建本项目。

## 文档口径

- 中文说明先写用途和最终行为，再写操作步骤、产物路径、验证结果和限制。命令、文件名与 API 标识符保留原文。
- 构建与测试分别说明；区分根目录统一构建入口与子工程单独构建，避免把两者的产物路径混在一起。
- 公共事实在 `documentation.json` 的 `values` 中维护。根 README 和 `doc/README.md` 的构建说明由同一模板生成，禁止单独修改标记区间里的文本。
- 专题文档保留自己的语法、模型、API 和示例，不重复公共支持范围；需要引用时链接权威定义，例如 `.gram`、支持矩阵或项目手册。
- 技术文档使用统一章节：适用范围、环境要求、使用方法、输入与产物、验证方法、已知限制、参考资料。不适用的章节说明原因，验证结果只写实际执行过的检查。
- 修改公共事实后重新生成并执行 `--check`；提交信息使用中文。

## 环境要求与运行

需要 Python 3.10 或更新版本。当前实现无需额外安装依赖，这不是后续扩展的限制。以下命令均在仓库根目录执行；本机如果只有 `python3`，将命令中的 `python` 替换为 `python3`。生成器也可以从其他工作目录运行，配置路径始终相对仓库根目录。

```sh
python project/document/generate.py
python project/document/generate.py --check
python project/document/test_generate.py
```

第一条命令同步公共说明；第二条只检查、不写文件，适合放入 CI；第三条验证生成器的同步、片段保留和错误处理。

退出码：`0` 表示文档已同步或生成成功，`1` 表示检查发现过期文档，`2` 表示配置、模板或文件访问错误。生成前先校验全部任务；配置错误不会留下部分更新。相同内容不重复写入，输出使用 UTF-8 和 LF。

## 文件与职责

| 文件 | 用途 |
| --- | --- |
| [generate.py](generate.py) | 生成全文或替换受管理片段；支持 `--config` 和 `--check` |
| [documentation.json](documentation.json) | 公共事实和现有文档的生成任务 |
| [templates/build-summary.md.tmpl](templates/build-summary.md.tmpl) | 两处 README 共用的构建说明 |
| [templates/document.md.tmpl](templates/document.md.tmpl) | 新技术文档的标准章节模板 |
| [example.json](example.json) | 生成一篇技术文档的完整配置示例 |
| [test_generate.py](test_generate.py) | 标准库 unittest 验证，临时文件放在根目录 `build/` |

## 修改现有公共说明

1. 在 `documentation.json` 修改公共字段，例如 `log_dir`、`python_policy`、`clean_policy`。涉及构建行为的内容先按脚本验证。
2. 调整结构时修改 `templates/build-summary.md.tmpl`，所有引用该模板的任务同时更新。
3. 运行生成器和 `--check`，检查 Git diff 后一起提交配置、模板与生成文档。

当前任务只维护根 README 和 `doc/README.md` 的构建片段。其他章节和各工程的专题手册继续手写；文档生成器不会自行推断代码行为。

## 创建新文档

先运行完整示例，产物放在统一的构建目录内：

```sh
python project/document/generate.py --config project/document/example.json
python project/document/generate.py --config project/document/example.json --check
```

生成文件是 `build/document/example.md`。复制 `example.json`，修改标题、各章节内容和输出路径，即可新增一篇技术文档。需要持续维护的文档，把任务加入 `documentation.json` 的 `documents` 数组，并在根 `doc/README.md` 添加索引；临时预览继续写到 `build/document/`。

模板使用 `{{字段名}}`。例如 `{{title}}` 对应 JSON 中的 `"title"`，值可以包含 Markdown、表格和代码块。字段必须是字符串，缺少字段会报错。字段内容按原样插入，不执行代码，也不二次展开；因此字段中的 `{{...}}` 示例不会被误替换。

## 配置格式

```json
{
  "values": { "title": "公共标题" },
  "documents": [
    {
      "template": "project/document/templates/document.md.tmpl",
      "output": "build/document/example.md",
      "values": {
        "summary": "用途说明",
        "scope": "适用对象",
        "prerequisites": "环境与依赖",
        "usage": "操作命令",
        "outputs": "输入和输出位置",
        "validation": "验证方式",
        "limitations": "已知边界",
        "references": "权威资料链接"
      }
    }
  ]
}
```

顶层 `values` 是公共字段，任务内 `values` 是该文档的字段，可以覆盖同名公共字段。`template` 和 `output` 都相对仓库根目录；输出必须是 `.md` 文件，路径不能指向仓库外。`{{repo}}` 由生成器按输出位置计算，为链接提供回到仓库根目录的前缀，不能由配置覆盖。

省略 `block` 时生成整篇文档，并添加生成来源注释；整篇生成的文件不要直接手工修改。指定 `"block": "build-summary"` 时只更新以下标记之间的内容：

```markdown
<!-- docgen:build-summary:start -->
这里是自动生成的内容。
<!-- docgen:build-summary:end -->
```

标记须各出现一次且顺序正确。生成器保留标记外的内容；标记缺失、重复或顺序错误时直接报错。每个文件可以维护多个不同片段，全文任务和片段任务不能指向同一个文件。
