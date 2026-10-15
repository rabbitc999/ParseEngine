# 示例

| 文件 | 用途 |
| --- | --- |
| `sample.gram` | 干净的示例文法：诊断为空，用来演示悬停 / 补全 / 跳转 / 大纲 / 语义高亮 / 格式化；里面有一条 `%token KW_LET include "sample_keywords.csv"` |
| `sample_keywords.csv` | 上面那条 `include` 用的两列别名表：`KW_LET,"let"` 是唯一被导入的行，另外还有表头行、没被 include 的 `KW_IF`、文法里不存在的 `OTHER`（都该被忽略）与第 3 列噪音列 |
| `sample_broken.gram` | 故意写错的文法：演示诊断与快速修复（未知指令、未知 `%mode` 取值、`%sync` 用未声明 Token、未声明的非终结符、隐式 Token 提示、未使用 Token） |
| `client.py` | 最小 LSP 客户端：用真实的 `Content-Length` 帧驱动 `lsp-server`，适合当编辑器接入的参考实现，也当冒烟测试 |

> `%token NAME include "表.csv"` 只给这个 Token **追加字面量别名**、不新建 Token；
> 表的第一列不是被导入的 Token 名（含表头）就整行忽略。悬停到 `KW_LET` 能看到
> 别名来自哪份 CSV，悬停到 `include` 本身有写法与语义说明。

## 跑一遍

```text
make -C library/lsp            # 构建 build/lsp/lsp-server
python library/lsp/examples/client.py build/lsp/lsp-server library/lsp/examples/sample.gram
python library/lsp/examples/client.py build/lsp/lsp-server library/lsp/examples/sample_broken.gram
```

干净示例应当输出 `诊断：0 个 error，0 个 warning` 并以 0 退出；坏例子会列出诊断与快速修复，
退出码是 2（有 error）。

## 接到编辑器

任何支持 LSP 的编辑器只要把 `.gram` 关联到下面这条命令即可（stdio 传输）：

```text
<仓库>/build/lsp/lsp-server --stdio
```

例如 VS Code 的 `settings.json`（配合通用 LSP 客户端扩展）：

```json
{
  "languageServerExample.trace.server": "off",
  "languageServerExample.server": "<仓库>/build/lsp/lsp-server --stdio"
}
```

编辑器需要把语言 id 设成 `gram`；服务器不依赖工作区里的任何文件，全文同步即可。
