#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""project/pl0-super/tools/gen_diagrams.py —— 用代码生成技术报告里的 Mermaid 图。

技术报告里的两张图不允许手画（改了代码忘了改图最容易对不上），统一由这个脚本生成：

    python tools/gen_diagrams.py            # 打印两张图的 Mermaid 源码
    python tools/gen_diagrams.py --check     # 与技术报告里的图逐字比对（CI/自验用）

图的内容直接对应实现：
  图 1 「流水线」：main.cpp / check.sh 调用的顺序（文法检查 → 库：词法 → 语法 →
        简化 → 代码生成 → 目标机）
  图 2 「帧与查找」：vm.cpp 的帧模型——变量名是 `过程$变量$字段`，查找从最内层帧往外
        找第一个有这个名字的帧（等价于静态链，指令里不带链指针）
"""

import argparse
import os
import sys


def pipeline_diagram() -> str:
    """图 1：源码到结果的五步，标注每一步落在哪个文件。"""
    steps = [
        ("源码 .p0s", "samples/"),
        ("文法等（pl0-super.gram）", "语法检查 + 行列报错"),
        ("lexer.cpp", "词法：标识符/数字/字符串/注释"),
        ("parser.cpp", "语法 + 名字与类型检查 → 简化代码树"),
        ("simplify.cpp", "常量折叠 / 代数化简 / 死分支"),
        ("codegen.cpp", "后缀式发射 + 窥孔 → 目标机汇编"),
        ("vm.cpp", "栈式目标机执行 → 输出"),
    ]
    lines = ["flowchart TD"]
    for index, (title, note) in enumerate(steps):
        lines.append(f'    S{index}["{title}<br/><small>{note}</small>"]')
    for index in range(len(steps) - 1):
        lines.append(f"    S{index} --> S{index + 1}")
    lines.append("    S6 --> OUT[\"stdout（每个值一行）\"]")
    lines.append("    S6 -.->|--asm| ASM[\"目标机汇编文本\"]")
    lines.append("    S4 -.->|--stats| STAT[\"各趟简化计数\"]")
    return "\n".join(lines)


def frame_diagram() -> str:
    """图 2：帧模型与变量查找（静态链的名字版）。"""
    return "\n".join(
        [
            "flowchart LR",
            '    subgraph F["调用栈（每层一个帧）"]',
            '        M["main 帧<br/>main$i, main$total"]',
            '        O["outer 帧<br/>outer$local"]',
            '        I["inner 帧<br/>（没有自己的变量）"]',
            "        M --> O --> I",
            "    end",
            '    Q["inner 里读 outer$local"] --> L{"从最内层帧往外找<br/>第一个有这个名的帧"}',
            "    L --> I2[\"inner 帧没有 → 跳过\"]",
            "    L --> O2[\"outer 帧有 → 命中（就是静态链那一层）\"]",
            '    R["递归时"] --> R2["每一层调用各有一个 outer 帧<br/>取到的是本次调用的副本"]',
        ]
    )


def report_blocks() -> str:
    """技术报告里直接贴的两段（带围栏）。"""
    return (
        "```mermaid\n" + pipeline_diagram() + "\n```\n\n"
        "```mermaid\n" + frame_diagram() + "\n```\n"
    )


def report_block_list():
    """技术报告里应当出现的两个 Mermaid 代码块（逐块比对，不看中间的正文）。"""
    return [
        "```mermaid\n" + pipeline_diagram() + "\n```",
        "```mermaid\n" + frame_diagram() + "\n```",
    ]


def main() -> int:
    parser = argparse.ArgumentParser(description="生成 pl0-super 技术报告的 Mermaid 图")
    parser.add_argument(
        "--check",
        nargs="?",
        const=str(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "技术报告.md")),
        metavar="报告路径",
        help="与技术报告里的图逐字比对（不给路径就用本工程的技术报告）",
    )
    args = parser.parse_args()
    if args.check:
        with open(args.check, encoding="utf-8") as stream:
            report = stream.read()
        want = report_block_list()
        blocks = []
        inside = False
        for line in report.splitlines():
            if line.strip() == "```mermaid":
                inside = True
                block = [line]
                continue
            if inside and line.strip() == "```":
                inside = False
                block.append(line)
                blocks.append("\n".join(block))
                continue
            if inside:
                block.append(line)
        if blocks != want:
            print("技术报告里的 Mermaid 图与生成结果不一致，请重新生成", file=sys.stderr)
            return 1
        print("Mermaid 图与技术报告一致")
        return 0
    print(report_blocks())
    return 0


if __name__ == "__main__":
    sys.exit(main())
