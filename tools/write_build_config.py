"""记录构建选项；仅内容变化时更新时间戳，触发依赖对象重建。"""
from pathlib import Path
import json
import sys


def main():
    """将命令参数写入指定构建目录的配置文件。"""
    target = Path(sys.argv[1])
    content = json.dumps(sys.argv[2:], ensure_ascii=False) + "\n"
    target.parent.mkdir(parents=True, exist_ok=True)
    if not target.exists() or target.read_text(encoding="utf-8") != content:
        target.write_text(content, encoding="utf-8")


if __name__ == "__main__":
    main()
