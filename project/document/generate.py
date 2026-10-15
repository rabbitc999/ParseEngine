#!/usr/bin/env python3
"""从 JSON 内容和 Markdown 模板生成文档或更新受管理片段。"""

import argparse
import json
import os
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[2]
PLACEHOLDER = re.compile(r"\{\{\s*([A-Za-z_][A-Za-z0-9_]*)\s*\}\}")
BLOCK_ID = re.compile(r"[a-z][a-z0-9-]*\Z")


def repository_path(value):
    """解析仓库内的路径，拒绝仓库外路径及符号链接逃逸。"""
    if not isinstance(value, str) or not value:
        raise ValueError("路径必须是非空字符串")
    path = (ROOT / value).resolve()
    if not path.is_relative_to(ROOT):
        raise ValueError(f"路径超出仓库范围：{value}")
    return path


def read_text(path):
    """读取 UTF-8 文件并统一换行，兼容带 BOM 的配置和文档。"""
    return path.read_text(encoding="utf-8-sig")


def render(template, values):
    """替换命名占位符；缺失字段会报错，字段内容不进行二次展开。"""
    missing = sorted(set(PLACEHOLDER.findall(template)) - values.keys())
    if missing:
        raise ValueError("模板缺少字段：" + ", ".join(missing))
    return PLACEHOLDER.sub(lambda match: values[match.group(1)], template).rstrip() + "\n"


def replace_block(document, block, content):
    """仅替换唯一的标记区间，保留文档中手写的其他章节。"""
    if not isinstance(block, str) or not BLOCK_ID.fullmatch(block):
        raise ValueError(f"无效片段名称：{block}")
    start = f"<!-- docgen:{block}:start -->"
    end = f"<!-- docgen:{block}:end -->"
    if document.count(start) != 1 or document.count(end) != 1:
        raise ValueError(f"片段 {block} 必须有且仅有一对 start/end 标记")
    first = document.index(start)
    last = document.index(end)
    if first >= last:
        raise ValueError(f"片段 {block} 的标记顺序错误")
    return document[:first] + start + "\n" + content + end + document[last + len(end):]


def generate(config_path, check=False):
    """先校验全部任务，再生成；检查模式不写文件，过期时返回 1。"""
    config = json.loads(read_text(config_path))
    if not isinstance(config, dict):
        raise ValueError("配置顶层必须是对象")
    common = config.get("values", {})
    documents = config.get("documents")
    if not isinstance(common, dict) or not isinstance(documents, list) or not documents:
        raise ValueError("配置需要 values 对象和非空 documents 数组")
    pending = {}
    claimed = set()
    for entry in documents:
        if not isinstance(entry, dict):
            raise ValueError("documents 中的任务必须是对象")
        template = repository_path(entry["template"])
        output = repository_path(entry["output"])
        if output.suffix != ".md":
            raise ValueError(f"输出必须是 Markdown 文件：{output}")
        local = entry.get("values", {})
        if not isinstance(local, dict):
            raise ValueError("任务的 values 必须是对象")
        values = dict(common, **local)
        # 链接前缀自动按输出位置计算，模板无需为根目录和 doc/ 分别维护。
        relative_root = os.path.relpath(ROOT, output.parent).replace("\\", "/")
        values["repo"] = "" if relative_root == "." else relative_root + "/"
        if any(not isinstance(key, str) or not isinstance(value, str)
               for key, value in values.items()):
            raise ValueError("模板字段名和字段值必须都是字符串")
        block = entry.get("block")
        claim = (output, block)
        if claim in claimed or (output, None) in claimed or (
                block is None and output in pending):
            raise ValueError(f"重复或冲突的输出任务：{entry['output']}")
        claimed.add(claim)
        content = render(read_text(template), values)
        if block is not None:
            document = pending.get(output)
            if document is None:
                document = read_text(output)
            content = replace_block(document, block, content)
        else:
            content = "<!-- 由 project/document/generate.py 生成；请修改配置或模板。 -->\n\n" + content
        pending[output] = content

    outdated = []
    for output, content in pending.items():
        if output.exists() and read_text(output) == content:
            continue
        outdated.append(output)
        if not check:
            output.parent.mkdir(parents=True, exist_ok=True)
            output.write_text(content, encoding="utf-8", newline="\n")
        print(f"{'过期' if check else '已生成'}：{output.relative_to(ROOT).as_posix()}")
    if not outdated:
        print("文档已同步。")
    return 1 if check and outdated else 0


def main():
    """处理命令行参数，报告配置错误并返回可供 CI 使用的退出码。"""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", default="project/document/documentation.json", help="仓库相对路径的 JSON 配置")
    parser.add_argument("--check", action="store_true", help="只检查过期文档，不写文件")
    args = parser.parse_args()
    try:
        return generate(repository_path(args.config), args.check)
    except (OSError, ValueError, KeyError, TypeError) as error:
        print(f"文档生成失败：{error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
