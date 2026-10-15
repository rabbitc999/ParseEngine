#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""生成 GramEditor（VS Code 插件）的应用图标。

只依赖 Pillow，画的是确定性的几何图形，随时可以重新生成：

    python tools/make_icon.py                 # 生成默认风格的图标
    python tools/make_icon.py --preview       # 把几种字形都导出到 image/ 预览
    python tools/make_icon.py --variant assign

产物：
    image/gram-editor-1024.png … -16.png   各尺寸 PNG（README / 商店用）
    image/gram-editor.ico                  多尺寸 ICO（16…256）
    vscode/icon.png                         VS Code 插件图标（256×256）

设计约束（都是为了"不土"）：
    * 不用投影、不用内高光、不用花哨的多色渐变——只有一层很淡的纵向底色；
    * 字形只用一种强调色，笔画粗细统一、端点全圆角，留足负空间；
    * 所有坐标按画布比例写，超采样后降采样，任何尺寸都干净。
"""

from __future__ import annotations

import argparse
import os
import sys

from PIL import Image, ImageDraw

# ---------------------------------------------------------------------------
#  基本参数
# ---------------------------------------------------------------------------

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

#: 逻辑画布边长（所有坐标都按 1.0 = 画布边长的比例写）。
LOGICAL = 1024

#: 超采样倍数：先按 4 倍画再降采样，边缘才干净。
SUPERSAMPLE = 4

#: 需要导出的 PNG 尺寸。
PNG_SIZES = (1024, 512, 256, 128, 64, 48, 32, 24, 16)

#: ICO 里包含的尺寸（Windows 按场景自己挑）。
ICO_SIZES = (16, 24, 32, 48, 64, 128, 256)

#: 圆角半径（占画布边长的比例）。
CORNER_RADIUS = 0.225

#: 底色：深蓝灰，上下略微渐变（差值很小，只为避免"死板的一片纯色"）。
BG_TOP = (30, 41, 59)       # #1E293B
BG_BOTTOM = (12, 17, 28)    # #0C111C

#: 强调色（字形颜色）：青绿，在深底上对比足够，又不刺眼。
ACCENT = (52, 211, 153)     # #34D399

#: 可选字形 → 说明。
VARIANTS = {
    "percent": "百分号 %：.gram 的所有指令都以它开头",
    "assign": "产生式运算符 := ",
    "arrow": "推导箭头：方框（非终结符）→ 圆点（终结符）",
}

DEFAULT_VARIANT = "percent"

# ---------------------------------------------------------------------------
#  底色
# ---------------------------------------------------------------------------


def _background(size: int) -> Image.Image:
    """生成圆角方形底色（很淡的纵向渐变，没有投影与内高光）。"""
    # 1. 渐变：先算 1×256 的一列，再横向拉伸，比逐像素快得多。
    column = Image.new("RGB", (1, 256))
    pixels = column.load()
    for y in range(256):
        t = y / 255
        pixels[0, y] = (
            round(BG_TOP[0] + (BG_BOTTOM[0] - BG_TOP[0]) * t),
            round(BG_TOP[1] + (BG_BOTTOM[1] - BG_TOP[1]) * t),
            round(BG_TOP[2] + (BG_BOTTOM[2] - BG_TOP[2]) * t),
        )
    gradient = column.resize((size, size), Image.Resampling.BICUBIC).convert("RGBA")

    # 2. 圆角遮罩：圆形以外的部分抠成全透明。
    mask = Image.new("L", (size, size), 0)
    ImageDraw.Draw(mask).rounded_rectangle(
        (0, 0, size - 1, size - 1),
        radius=round(size * CORNER_RADIUS),
        fill=255,
    )
    gradient.putalpha(mask)
    return gradient


# ---------------------------------------------------------------------------
#  字形零件与三种字形
# ---------------------------------------------------------------------------


def _dot(draw: ImageDraw.ImageDraw, center: tuple, radius: float, color: tuple) -> None:
    """画一个实心圆（字形里的"点"）。"""
    draw.ellipse(
        (center[0] - radius, center[1] - radius, center[0] + radius, center[1] + radius),
        fill=color,
    )


def _stroke(draw: ImageDraw.ImageDraw, points: list, width: int, color: tuple) -> None:
    """画一条折线：圆角接头 + 两端补圆帽，保证端点也是圆的。"""
    draw.line(points, fill=color, width=width, joint="curve")
    half = width / 2
    for point in (points[0], points[-1]):
        draw.ellipse(
            (point[0] - half, point[1] - half, point[0] + half, point[1] + half),
            fill=color,
        )


def _glyph_percent(draw: ImageDraw.ImageDraw, size: int, color: tuple) -> None:
    """`%`：两个圆点 + 一条斜杠。"""
    s = size
    _stroke(draw, [(0.728 * s, 0.272 * s), (0.272 * s, 0.728 * s)], round(0.079 * s), color)
    _dot(draw, (0.306 * s, 0.306 * s), 0.106 * s, color)
    _dot(draw, (0.694 * s, 0.694 * s), 0.106 * s, color)


def _glyph_assign(draw: ImageDraw.ImageDraw, size: int, color: tuple) -> None:
    """`:=`：左冒号两个方点、右等号两条横杠。"""
    s = size
    thickness = round(0.082 * s)
    half = thickness / 2

    # 1. 冒号：两个圆角方点。
    for center_y in (0.360 * s, 0.640 * s):
        draw.rounded_rectangle(
            (0.275 * s - half, center_y - half, 0.275 * s + half, center_y + half),
            radius=half,
            fill=color,
        )

    # 2. 等号：两条圆头横杠。
    for center_y in (0.400 * s, 0.600 * s):
        _stroke(draw, [(0.475 * s, center_y), (0.725 * s, center_y)], thickness, color)


def _glyph_arrow(draw: ImageDraw.ImageDraw, size: int, color: tuple) -> None:
    """`方框 → 圆点`：非终结符推导出终结符。"""
    s = size
    width = round(0.074 * s)

    # 1. 左：圆角方框（非终结符）。
    draw.rounded_rectangle(
        (0.200 * s, 0.365 * s, 0.360 * s, 0.635 * s),
        radius=0.048 * s,
        fill=color,
    )

    # 2. 中：箭杆 + 箭头。
    _stroke(draw, [(0.430 * s, 0.500 * s), (0.620 * s, 0.500 * s)], width, color)
    _stroke(
        draw,
        [(0.560 * s, 0.395 * s), (0.700 * s, 0.500 * s), (0.560 * s, 0.605 * s)],
        width,
        color,
    )

    # 3. 右：圆点（终结符）。
    _dot(draw, (0.800 * s, 0.500 * s), 0.086 * s, color)


GLYPHS = {
    "percent": _glyph_percent,
    "assign": _glyph_assign,
    "arrow": _glyph_arrow,
}

# ---------------------------------------------------------------------------
#  渲染与导出
# ---------------------------------------------------------------------------


def render(variant: str) -> Image.Image:
    """按给定字形渲染超采样母图。"""
    size = LOGICAL * SUPERSAMPLE
    canvas = _background(size)
    GLYPHS[variant](ImageDraw.Draw(canvas), size, ACCENT + (255,))
    return canvas


def _resize(master: Image.Image, size: int) -> Image.Image:
    """把母图降采样到目标尺寸。"""
    return master.resize((size, size), Image.Resampling.LANCZOS)


def _ensure_dir(path: str) -> None:
    """确保目录存在。"""
    os.makedirs(path, exist_ok=True)


def write_outputs(variant: str) -> list:
    """导出全部产物，返回写出的文件列表。"""
    master = render(variant)
    image_dir = os.path.join(REPO_ROOT, "image")
    _ensure_dir(image_dir)
    written = []

    # 1. 各尺寸 PNG。
    for size in PNG_SIZES:
        path = os.path.join(image_dir, f"gram-editor-{size}.png")
        _resize(master, size).save(path)
        written.append(path)

    # 2. 多尺寸 ICO。
    ico_path = os.path.join(image_dir, "gram-editor.ico")
    _resize(master, 256).save(ico_path, format="ICO", sizes=[(s, s) for s in ICO_SIZES])
    written.append(ico_path)

    # 3. VS Code 插件图标：市场要求 PNG、正方形、至少 128×128，这里给 256。
    vscode_icon = os.path.join(REPO_ROOT, "vscode", "icon.png")
    _ensure_dir(os.path.dirname(vscode_icon))
    _resize(master, 256).save(vscode_icon)
    written.append(vscode_icon)

    return written


def write_previews() -> list:
    """把每种字形各导出一张 256×256 预览图，方便挑风格。"""
    image_dir = os.path.join(REPO_ROOT, "image")
    _ensure_dir(image_dir)
    written = []
    for variant in VARIANTS:
        path = os.path.join(image_dir, f"icon-preview-{variant}.png")
        _resize(render(variant), 256).save(path)
        written.append(path)
    return written


def main(argv: list) -> int:
    """命令行入口。"""
    parser = argparse.ArgumentParser(description="生成 GramEditor 图标")
    parser.add_argument("--variant", choices=sorted(VARIANTS), default=DEFAULT_VARIANT,
                        help="字形（默认 %(default)s）")
    parser.add_argument("--preview", action="store_true",
                        help="导出每种字形的预览图，不覆盖正式产物")
    args = parser.parse_args(argv)

    if args.preview:
        for path in write_previews():
            print("  " + os.path.relpath(path, REPO_ROOT).replace("\\", "/"))
        print("\n预览已生成；挑好后用 --variant <名字> 出正式图标：")
        for name, description in VARIANTS.items():
            print(f"  {name:8s} {description}")
        return 0

    written = write_outputs(args.variant)
    for path in written:
        relative = os.path.relpath(path, REPO_ROOT).replace("\\", "/")
        with Image.open(path) as image:
            if path.endswith(".ico"):
                sizes = ", ".join(f"{w}×{h}" for w, h in sorted(image.info.get("sizes", [])))
                print(f"  {relative}  ICO（{sizes}）")
            else:
                print(f"  {relative}  {image.width}×{image.height}")

    print(f"\n共 {len(written)} 个文件已生成（字形：{args.variant}）。")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
