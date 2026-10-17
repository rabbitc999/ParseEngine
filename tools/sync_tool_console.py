"""同步 ParseEngine 侧的 UTF-8 控制台头。

`library/console/utf8.hpp` 是源文件，下面这份是**只读副本**：

  * `source/include/ParseEngine/tool_console.hpp`   —— ParseEngine 本体（按它的 include 根命名）

改动流程：只改 `library/console/utf8.hpp`，再跑本脚本（`source/Makefile` 与
`source/CMakeLists.txt` 也会在构建时自动跑）。副本只读的原因：工程自包含，构建时不依赖 `library/`。

Magel 已经剥离成独立项目（https://github.com/rabbitc999/magel.git），它自带一份同名头，
由它自己的 `tools/sync_tool_console.py` 同步到板端副本；本脚本**不再写进 magel/**。
"""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

SOURCE = "library/console/utf8.hpp"

TARGETS = (
    "source/include/ParseEngine/tool_console.hpp",
)


def main():
    """把源文件逐字节复制到各副本；仅在内容变化时写入，避免无意义的全部重编译。"""
    source = (ROOT / SOURCE).read_bytes()
    for name in TARGETS:
        target = ROOT / name
        if not target.exists() or target.read_bytes() != source:
            target.write_bytes(source)


if __name__ == "__main__":
    main()
