#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""生成 README 用的项目总览图（tools/overview/ParseEngine-overview.png）。

只用标准库 + Pillow；画的是确定性的示意图，随时可以重新生成：

    python tools/make_overview_image.py

排版：四栏 —— ① 生成器 ② 应用 ③ 附加库 ④ 工具。
信息都对应仓库现有内容（src/ · project/ · library/ · vscode/；
    magel / metro-lang / rust-view 已全部拆独立仓库），
改这些目录时记得一起改本脚本的文案。

为什么不用 SVG/HTML：README 在 GitHub 上渲染 PNG 最稳，且这张图文字多，
位图预览（手机 / 聊天窗口）也不会因为缺字体而串行。
"""

from __future__ import annotations

import os
import sys

from PIL import Image, ImageDraw, ImageFont

# 逻辑画布（所有坐标都按这个尺寸写），实际按 SCALE 倍画、最后缩到 OUT_SCALE 倍导出。
# 默认 4 倍画 + 3 倍导出：PNG 是 4680x2700，字形按 4 倍渲染再降采样，
# 放大看也清楚；想要更大就把 SE_IMAGE_SCALE / SE_IMAGE_OUT_SCALE 调高。
WIDTH, HEIGHT = 1560, 900
SCALE = int(os.environ.get("SE_IMAGE_SCALE", "4"))
OUT_SCALE = int(os.environ.get("SE_IMAGE_OUT_SCALE", "3"))
OUT_WIDTH, OUT_HEIGHT = WIDTH * OUT_SCALE, HEIGHT * OUT_SCALE

FONT_DIR = os.path.join(os.environ.get("WINDIR", r"C:\Windows"), "Fonts")


def _pick_font(candidates):
    """按顺序取第一个存在的字体，返回 (路径, 字体集合内的序号)。

    Windows 优先用微软雅黑（原图口径）；Linux / macOS 回退到 Noto Sans CJK SC /
    苹方，换平台也能重新生成同一张图（字形略有出入属正常）。
    """
    for path, index in candidates:
        if os.path.exists(path):
            return path, index
    return candidates[0]


# 正文无衬线字体：微软雅黑 → Noto Sans CJK SC（.ttc 里 SC 是第 2 个）→ 苹方。
FONT_SANS, FONT_SANS_INDEX = _pick_font((
    (os.path.join(FONT_DIR, "msyh.ttc"), 0),
    ("/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc", 2),
    ("/usr/share/fonts/SourceHanSansSC-Regular-2.otf", 0),
    ("/System/Library/Fonts/PingFang.ttc", 0),
))
FONT_SANS_BOLD, FONT_SANS_BOLD_INDEX = _pick_font((
    (os.path.join(FONT_DIR, "msyhbd.ttc"), 0),
    ("/usr/share/fonts/opentype/noto/NotoSansCJK-Bold.ttc", 2),
    ("/System/Library/Fonts/PingFang.ttc", 0),
))

# 字体路径 → .ttc 集合序号，供 font() 拼装时查。
_FONT_INDEX = {FONT_SANS: FONT_SANS_INDEX, FONT_SANS_BOLD: FONT_SANS_BOLD_INDEX}

# 颜色（与 README 的浅色主题一致）
BG = "#f4f7fb"
PANEL = "#ffffff"
PANEL_EDGE = "#d8e1ee"
CARD = "#f7f9fc"
CARD_EDGE = "#e2e9f3"
TITLE = "#10233f"
SUBTITLE = "#4a5b73"
PTITLE = "#14507e"
CTITLE = "#33475f"
CODE = "#22405f"
NOTE = "#55677f"
STEP_FILL = "#f4f8fd"
STEP_EDGE = "#cfe0f3"
STEP_TEXT = "#16405f"
CHIP_FILL = "#eef4fb"
CHIP_EDGE = "#c9dcf0"
CHIP_TEXT = "#1d4f7a"
LANG_FILL = "#eaf6ef"
LANG_EDGE = "#bfe0cc"
LANG_TEXT = "#1e6b46"
BADGE = "#14507e"
ARROW = "#8ea2bb"
FOOT = "#64748b"

# 四栏统一版式
COL_X = [40, 410, 780, 1150]   # 每栏左边界
COL_W = 360                    # 每栏宽度
COL_TOP = 118                  # 每栏顶部
COL_H = 700                    # 每栏高度


def font(path: str, size: int) -> ImageFont.FreeTypeFont:
    return ImageFont.truetype(path, int(size * SCALE), index=_FONT_INDEX.get(path, 0))


class Canvas:
    """按 1 倍坐标写、实际按 SCALE 倍画；同时记录 SVG 元素。"""

    def __init__(self) -> None:
        self.image = Image.new("RGB", (WIDTH * SCALE, HEIGHT * SCALE), BG)
        self.draw = ImageDraw.Draw(self.image)
        self.svg = []
        self.texts = []          # (内容, 字体) —— 生成后拿来自查字形

    def rect(self, x, y, w, h, fill, edge=None, radius=0, width=1):
        box = [x * SCALE, y * SCALE, (x + w) * SCALE, (y + h) * SCALE]
        if radius:
            self.draw.rounded_rectangle(box, radius=radius * SCALE, fill=fill,
                                        outline=edge, width=int(width * SCALE))
        else:
            self.draw.rectangle(box, fill=fill, outline=edge,
                                width=int(width * SCALE))
        self.svg.append(
            '<rect x="%g" y="%g" width="%g" height="%g"%s fill="%s"%s/>' % (
                x, y, w, h,
                ' rx="%g"' % radius if radius else "",
                fill,
                ' stroke="%s" stroke-width="%g"' % (edge, width) if edge else ""))

    def text(self, x, baseline, content, font_, fill):
        """按「基线」定位画文字。"""
        ascent, _descent = font_.getmetrics()
        self.draw.text((x * SCALE, baseline * SCALE - ascent), content,
                       font=font_, fill=fill)
        self.texts.append((content, font_))
        self.svg.append(self._svg_text(x, baseline, content, font_, fill))

    def text_center(self, cx, cy, content, font_, fill):
        self.draw.text((cx * SCALE, cy * SCALE), content, font=font_, fill=fill,
                       anchor="mm")
        self.texts.append((content, font_))
        self.svg.append(self._svg_text(cx, cy, content, font_, fill,
                                       anchor="middle", central=True))

    def arrow(self, x1, y1, x2, y2, color=ARROW, width=2, head=7):
        self.draw.line([x1 * SCALE, y1 * SCALE, x2 * SCALE, y2 * SCALE],
                       fill=color, width=int(width * SCALE))
        self.draw.polygon([(x2 * SCALE, y2 * SCALE),
                           ((x2 - head) * SCALE, (y2 - head * 0.5) * SCALE),
                           ((x2 - head) * SCALE, (y2 + head * 0.5) * SCALE)],
                          fill=color)
        self.svg.append('<line x1="%g" y1="%g" x2="%g" y2="%g" stroke="%s" '
                        'stroke-width="%g"/>' % (x1, y1, x2, y2, color, width))
        self.svg.append('<polygon points="%g,%g %g,%g %g,%g" fill="%s"/>' % (
            x2, y2, x2 - head, y2 - head * 0.5, x2 - head, y2 + head * 0.5, color))

    def _svg_text(self, x, y, content, font_, fill, anchor=None, central=False):
        family = ("Consolas, monospace"
                  if font_.path.endswith(("consola.ttf", "consolab.ttf"))
                  else "Microsoft YaHei, Segoe UI, Noto Sans SC, sans-serif")
        weight = (' font-weight="700"'
                  if "bd" in font_.path or "Bold" in font_.path else "")
        extra = ""
        if anchor:
            extra += ' text-anchor="%s"' % anchor
        if central:
            extra += ' dominant-baseline="central"'
        return ('<text x="%g" y="%g" font-family="%s" font-size="%g"%s fill="%s"%s>'
                '%s</text>' % (x, y, family, font_.size / SCALE, weight, fill, extra,
                               _escape(content)))

    # ---------- 字形自查：缺字 / 全是豆腐块时直接报错 ----------

    def check_glyphs(self) -> None:
        seen = {}
        for content, font_ in self.texts:
            for ch in content:
                if ch.isspace():
                    continue
                mask = font_.getmask(ch)
                if mask.getbbox() is None:
                    sys.exit("字形缺失：字体 %s 画不出 %r" % (font_.path, ch))
                key = (font_.path, ch)
                if key in seen:
                    continue
                seen[key] = (hash(mask.tobytes()) if hasattr(mask, "tobytes")
                             else hash(bytes(mask)))
        # 豆腐块的特征：不同汉字渲染出完全相同的位图
        by_font = {}
        for (path, ch), digest in seen.items():
            by_font.setdefault(path, {}).setdefault(digest, []).append(ch)
        for path, groups in by_font.items():
            for chars in groups.values():
                if len(chars) > 1:
                    sys.exit("字形可疑（像是豆腐块）：字体 %s 下 %s 渲染结果相同"
                             % (path, "".join(sorted(chars))))

    def save(self, png_path: str, svg_path: str) -> None:
        self.check_glyphs()
        final = self.image.resize((OUT_WIDTH, OUT_HEIGHT), Image.LANCZOS)
        final.save(png_path)
        svg = ('<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" '
               'viewBox="0 0 %d %d">\n  <rect width="%d" height="%d" fill="%s"/>\n'
               '  %s\n</svg>\n' % (WIDTH, HEIGHT, WIDTH, HEIGHT, WIDTH, HEIGHT, BG,
                                   "\n  ".join(self.svg)))
        with open(svg_path, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(svg)


def _escape(text: str) -> str:
    return (text.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;"))


def build() -> Canvas:
    for path in (FONT_SANS, FONT_SANS_BOLD, FONT_SANS):
        if not os.path.exists(path):
            sys.exit("找不到字体：%s" % path)

    f_h1 = font(FONT_SANS_BOLD, 31)
    f_sub = font(FONT_SANS, 15)
    f_ptitle = font(FONT_SANS_BOLD, 17)
    f_ctitle = font(FONT_SANS_BOLD, 13)
    f_code = font(FONT_SANS, 12)
    f_label = font(FONT_SANS, 12)
    f_note = font(FONT_SANS, 12)
    f_grid = font(FONT_SANS, 11)
    f_step = font(FONT_SANS_BOLD, 13)
    f_chip = font(FONT_SANS_BOLD, 12)
    f_lang = font(FONT_SANS_BOLD, 13)
    f_badge = font(FONT_SANS_BOLD, 11)
    f_newt = font(FONT_SANS_BOLD, 13)
    f_newd = font(FONT_SANS, 12)
    f_foot = font(FONT_SANS, 12)

    c = Canvas()

    # ---------- 标题 ----------
    c.text(40, 58, "ParseEngine 解析器生成器 · 仓库全景", f_h1, TITLE)
    c.text(40, 88,
           "一份 .gram → 16 种生成模式 → 8 种目标语言 + 文法导出档 · "
           "生成器 / 应用 / 附加库 / 工具四层，核心生成器无第三方依赖",
           f_sub, SUBTITLE)

    # ============================================================
    # ① 生成器
    # ============================================================
    x = COL_X[0]
    c.rect(x, COL_TOP, COL_W, COL_H, PANEL, PANEL_EDGE, radius=14, width=1.5)
    c.text(x + 18, COL_TOP + 30, "① 生成器", f_ptitle, PTITLE)
    c.text(x + 18, COL_TOP + 52, "source/",
           f_label, NOTE)

    # 五步流水线
    steps = ("%if / %elif 预处理（--define 裁剪）",
             "文法解析（指令 / 规则 / 动作）",
             "词法：正则 → NFA → DFA",
             "分析：16 种模式，出表 + 消解",
             "代码生成：模板 → 完整工程")
    for i, label in enumerate(steps):
        top = COL_TOP + 72 + i * 40
        c.rect(x + 18, top, COL_W - 36, 32, STEP_FILL, STEP_EDGE, radius=8)
        c.rect(x + 26, top + 7, 18, 18, BADGE, radius=5)
        c.text_center(x + 35, top + 16, str(i + 1), f_badge, "#ffffff")
        c.text(x + 52, top + 20, label, f_step, STEP_TEXT)

    # 16 种模式（LL / LR / GLR / Earley / CYK / 递归下降 / lex）
    c.text(x + 18, COL_TOP + 292, "16 种生成模式", f_ctitle, CTITLE)
    chips = (("LL(0)", 0, 0), ("LL(1)", 1, 0), ("LL(k)", 2, 0),
             ("LR(0)", 3, 0), ("SLR(1)", 0, 1), ("SLR(k)", 1, 1),
             ("LALR(1)", 2, 1), ("LALR(k)", 3, 1), ("LR(1)", 0, 2),
             ("LR(k)", 1, 2), ("IELR(1)", 2, 2),
             ("LEX", 3, 2), ("GLR", 0, 3),
             ("DD", 1, 3), ("Earley", 2, 3), ("CYK", 3, 3))
    chip_w, chip_h, gap = 70, 26, 8
    for label, col, row in chips:
        w = chip_w
        cx = x + 18 + col * (chip_w + gap)
        cy = COL_TOP + 304 + row * (chip_h + gap)
        c.rect(cx, cy, w, chip_h, CHIP_FILL, CHIP_EDGE, radius=7)
        c.text_center(cx + w / 2, cy + 14, label, f_chip, CHIP_TEXT)

    # 六个完整后端 + 两个 DD/Earley/CYK 窄后端，排成两行、每行四个。
    c.text(x + 18, COL_TOP + 450, "8 种目标语言（6 完整 + 2 窄）+ 导出档", f_ctitle, CTITLE)
    langs = (("C99", 0, 0), ("C++17", 1, 0), ("Python", 2, 0), ("C#", 3, 0),
             ("Rust", 0, 1), ("Go", 1, 1), ("Java*", 2, 1), ("JS/TS*", 3, 1))
    lang_w = (COL_W - 36 - 3 * gap) // 4
    for label, col, row in langs:
        cx = x + 18 + col * (lang_w + gap)
        cy = COL_TOP + 464 + row * (chip_h + gap)
        c.rect(cx, cy, lang_w, chip_h, LANG_FILL, LANG_EDGE, radius=7)
        c.text_center(cx + lang_w / 2, cy + 14, label, f_lang, LANG_TEXT)

    # 生成器要点
    c.text(x + 18, COL_TOP + 546, "要点", f_ctitle, CTITLE)
    for i, line in enumerate((
            "C++17 只用标准库，无第三方依赖",
            "source/templates/ 构建期内联，运行时无依赖",
            "单个可执行文件，运行不读目录",
            "47 条指令全部带生成期校验与行号",
            "* Java / JS-TS 当前仅支持 DD / Earley / CYK")):
        c.text(x + 18, COL_TOP + 568 + i * 20, line, f_note, NOTE)

    # ============================================================
    # ② 应用
    # ============================================================
    x = COL_X[1]
    c.rect(x, COL_TOP, COL_W, COL_H, PANEL, PANEL_EDGE, radius=14, width=1.5)
    c.text(x + 18, COL_TOP + 30, "② 应用", f_ptitle, PTITLE)
    c.text(x + 18, COL_TOP + 52, "根目录工程 · project/ · example/", f_label, NOTE)

    # 两套课程实验：摘要独立成卡，具体工程仍列入下方完整工程清单。
    c.text(x + 18, COL_TOP + 76, "课程实验（2 套 · 8 份报告）", f_ctitle, CTITLE)
    labs = (
        ("lab_report_axhu", "安徽新华：3 份 · 文法 / 词法 / 语法"),
        ("lab_report_nju", "南京大学：5 份 · 严格 C-- 编译全流程"),
    )
    for i, (name, desc) in enumerate(labs):
        top = COL_TOP + 88 + i * 52
        c.rect(x + 18, top, COL_W - 36, 46, CARD, CARD_EDGE, radius=8)
        c.text(x + 28, top + 19, name, f_code, CODE)
        c.text(x + 28, top + 37, desc, f_note, NOTE)

    # 根目录应用工程：magel / metro-lang / rust-view 全部拆成独立仓库。
    c.text(x + 18, COL_TOP + 208, "根目录应用工程（0 个，均已拆分独立仓库）", f_ctitle, CTITLE)
    root_projects = ()
    root_w = (COL_W - 36 - 2 * 6) // 3
    for i, (name, desc) in enumerate(root_projects):
        left = x + 18 + (i % 3) * (root_w + 6)
        top = COL_TOP + 222 + (i // 3) * 50
        c.rect(left, top, root_w, 44, CARD, CARD_EDGE, radius=8)
        c.text(left + 7, top + 17, name, f_code, CODE)
        c.text(left + 7, top + 35, desc, f_grid, NOTE)

    # project/ 完整工程：双列网格与右侧附加库使用同一尺寸。
    c.text(x + 18, COL_TOP + 336, "project/ 完整工程（9 个）", f_ctitle, CTITLE)
    projects = (
        ("pl0-ir", "PL/0 → 三元式 / 四元式"),
        ("mini-basic", "字节码 + 128 行 VM"),
        ("log-ir", "日志 → JSON（Python）"),
        ("todo-scan", "扫 TODO 出 Markdown"),
        ("pl0-super", "两套前端：ll1 / lalr1"),
        ("bigint-calc", "大整数计算器（BigInt）"),
        ("protocol-dsl", "三套编译器 → 四档代码"),
        ("lab_report_axhu", "安徽新华编译实验（3 份）"),
        ("lab_report_nju", "南京大学 C-- 实验（5 份）"),
    )
    grid_gap = 8
    grid_w = (COL_W - 36 - grid_gap) / 2
    for i, (name, desc) in enumerate(projects):
        col, row = i % 2, i // 2
        left = x + 18 + col * (grid_w + grid_gap)
        top = COL_TOP + 350 + row * 52
        card_w = COL_W - 36 if i == len(projects) - 1 and len(projects) % 2 == 1 else grid_w
        c.rect(left, top, card_w, 44, CARD, CARD_EDGE, radius=8)
        c.text(left + 10, top + 17, name, f_code, CODE)
        c.text(left + 10, top + 35, desc, f_grid, NOTE)

    # example/
    c.text(x + 18, COL_TOP + 626, "example/ · 46 份文法", f_ctitle, CTITLE)
    for i, line in enumerate((
            "calc / json / ini / mini_lang / llk_demo",
            "pl0 / c89_subset / lexer_only / commit_demo",
            "manifest.tsv 记实测语言档")):
        c.text(x + 18, COL_TOP + 648 + i * 20, line, f_note, NOTE)

    # ============================================================
    # ③ 附加库
    # ============================================================
    x = COL_X[2]
    c.rect(x, COL_TOP, COL_W, COL_H, PANEL, PANEL_EDGE, radius=14, width=1.5)
    c.text(x + 18, COL_TOP + 30, "③ 附加库", f_ptitle, PTITLE)
    c.text(x + 18, COL_TOP + 52, "library/ · 与主流程独立", f_label, NOTE)

    # 名字后面带 [rust] 的库同时有 C++ 与 Rust 版本；双列网格与应用栏对齐。
    c.text(x + 18, COL_TOP + 76, "独立组件（10 个）", f_ctitle, CTITLE)
    libs = (
        ("template[rust]", "模板引擎", "C++ / Rust"),
        ("transfer[rust]", "IR 后端工具链", "C++ / Rust"),
        ("simple_ast[rust]", "AST → IR + .data", "类型 / 泛型 / lambda"),
        ("stackvm[rust]", "栈式虚拟机", "对象表 / bint / 容器"),
        ("json[rust]", "JSON 数据模型", "C++17 / Rust"),
        ("toml", "TOML 数据模型", "表 / 表数组 / 日期时间"),
        ("big_number[rust]", "大整数 + BigFloat", "C++ / Rust"),
        ("preprocess[rust]", "通用预处理器", "# / % 条件编译"),
        ("lsp", ".gram 语言服务", "诊断 / 补全 / 格式化"),
        ("allocator", "自定义分配池", "C99 / C++17 · Memory"),
    )
    for i, (name, desc, note) in enumerate(libs):
        col, row = i % 2, i // 2
        left = x + 18 + col * (grid_w + grid_gap)
        top = COL_TOP + 88 + row * 54
        card_w = COL_W - 36 if i == len(libs) - 1 and len(libs) % 2 == 1 else grid_w
        c.rect(left, top, card_w, 50, CARD, CARD_EDGE, radius=8)
        c.text(left + 10, top + 15, name, f_code, CODE)
        c.text(left + 10, top + 30, desc, f_grid, NOTE)
        c.text(left + 10, top + 44, note, f_grid, NOTE)

    # 后端链路用和生成器流水线一致的全宽步骤卡，形成上下两级层次。
    c.text(x + 18, COL_TOP + 372, "transfer 后端链路", f_ctitle, CTITLE)
    transfer_steps = (
        ("IR 分析", "四元式 → 基本块 / CFG → 数据流"),
        ("优化", "常量 · CSE · DCE · 循环优化"),
        ("SSA 与分配", "Chaitin / 线性扫描"),
        ("教学后端", "MIPS / 8086 / ARM → 模拟执行"),
    )
    for i, (name, desc) in enumerate(transfer_steps):
        top = COL_TOP + 386 + i * 54
        c.rect(x + 18, top, COL_W - 36, 48, STEP_FILL, STEP_EDGE, radius=8)
        c.text(x + 28, top + 21, name, f_code, CODE)
        c.text(x + 112, top + 21, desc, f_grid, NOTE)

    # ============================================================
    # ④ 工具
    # ============================================================
    x = COL_X[3]
    c.rect(x, COL_TOP, COL_W, COL_H, PANEL, PANEL_EDGE, radius=14, width=1.5)
    c.text(x + 18, COL_TOP + 30, "④ 工具", f_ptitle, PTITLE)
    c.text(x + 18, COL_TOP + 52, "vscode/ · tools/ · build/", f_label, NOTE)

    # 编辑器
    c.text(x + 18, COL_TOP + 76, "编辑器（VS Code 插件）", f_ctitle, CTITLE)
    editors = (
        ("GramEditor for VS Code", "零依赖，自带 .vsix 打包", "vscode/ · 接 library/lsp · 不需要 npm"),
    )
    for i, (name, desc, note) in enumerate(editors):
        top = COL_TOP + 88 + i * 56
        c.rect(x + 18, top, COL_W - 36, 50, CARD, CARD_EDGE, radius=9)
        c.text(x + 28, top + 18, name, f_code, CODE)
        c.text(x + 28, top + 34, desc, f_note, NOTE)
        c.text(x + 28, top + 48, note, f_note, NOTE)

    # 编辑能力
    c.text(x + 18, COL_TOP + 176, "编辑 .gram 时的能力", f_ctitle, CTITLE)
    for i, line in enumerate((
            "高亮 · 折叠 · 实时诊断",
            "悬停说明 · 补全 · 跳转定义",
            "大纲 · 格式化 · 快速修复",
            "一键生成解析器 · 改 library/lsp 即生效",
            "Token / 完整语法树预览 · CSV / PNG / JPEG / SVG")):
        c.text(x + 18, COL_TOP + 194 + i * 18, line, f_note, NOTE)

    # tools/：维护脚本按用途分四张卡片（和 ③ 一样的三行卡片）
    c.text(x + 18, COL_TOP + 288, "tools/ · 构建 / 验收 / 图示", f_ctitle, CTITLE)
    tools = (
        ("构建与模板", "embed_templates.py 内联模板", "gen_all.sh 按 manifest 全量重生成"),
        ("验收三档", "quick-verify.sh 秒级（约 24 秒）", "isolated-build.sh 分钟级 · gen_all.sh 提交前"),
        ("比对与协作", "diff.sh 产物逐字节比对", "lock.sh 共享工作区的编译锁"),
        ("图示与图标", "make_overview_image.py 本图", "make_icon.py 图标（PNG + .ico）"),
    )
    for i, (name, desc, note) in enumerate(tools):
        top = COL_TOP + 310 + i * 52
        c.rect(x + 18, top, COL_W - 36, 48, CARD, CARD_EDGE, radius=9)
        c.text(x + 28, top + 15, name, f_code, CODE)
        c.text(x + 28, top + 30, desc, f_note, NOTE)
        c.text(x + 28, top + 42, note, f_note, NOTE)

    # 测试与产物
    c.text(x + 18, COL_TOP + 534, "构建 / 测试 / 产物", f_ctitle, CTITLE)
    for i, line in enumerate((
            "sh build.sh   统一构建，产物位于 build/",
            "make -C source test-repro   两次生成逐字一致",
            "project/document/   模板与文档同步生成器")):
        c.text(x + 18, COL_TOP + 556 + i * 20, line, f_note, NOTE)

    # ============================================================
    # 底栏：四层之间的连线说明 + 人机协同
    # ============================================================
    c.text(40, 846,
           "四层关系：生成器（①）产出解析器工程；应用（②）含 project/ 实验"
           "（magel / metro-lang / rust-view 已拆独立仓库）；"
           "附加库（③）提供数据模型与后端；工具（④）辅助开发和验证",
           f_foot, FOOT)
    c.text(40, 872,
           "人机协同：需求、文法与算法选型由人，实现由 Codex + DeepSeek；"
           "真实生成、目标工具链与回归样例负责验收，当前口径见 doc/15 与专题文档",
           f_foot, FOOT)

    return c


def main() -> int:
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    out_dir = os.path.join(root, "tools", "overview")
    os.makedirs(out_dir, exist_ok=True)
    png_path = os.path.join(out_dir, "ParseEngine-overview.png")
    svg_path = os.path.join(out_dir, "ParseEngine-overview.svg")
    build().save(png_path, svg_path)
    print("已生成 %s" % png_path)
    print("已生成 %s" % svg_path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
