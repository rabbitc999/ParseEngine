#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把 `doc/` 文档、`doc/en/` 英文版与示例清单内联成 C++ 源码（仅构建期使用）。

产物是 `source/src/manual_data.cpp`，供 `ParseEngine man` 子命令在终端离线查询：

  * **文档页**：`doc/*.md` 的正文（UTF-8），有 `doc/en/<同名>` 时一并内联英文正文；
    另外记录短编号、文件名、标题与一句摘要。
  * **文法指令**：从 `doc/12-grammar-usage.md` 的指令表里抽 `%指令` 与说明
    （中英各抽一遍，按指令名对齐）。
  * **生成模式**：从 `doc/15-language-mode-support.md` 的矩阵表抽模式名与算法，
    再用 `doc/13-modes.md` 的行补一句用途。
  * **目标语言**：同上矩阵表的表头与合计行：`--lang` 取值、显示名与支持的模式数。
  * **示例**：`example/manifest.tsv` 里登记过的示例文法（语言档与固定输入）。

正文体积大（约 1.3 MB），与 `tools/embed_templates.py` 一样**整体拼接后用
library/lztext 压缩**，只把压缩字节内联进来，运行时解压一次再按偏移切片——
压缩/解压算法只有 `library/lztext` 一份 C++ 实现，本脚本通过 `lztext_pack`
命令行工具调用它，不在 Python 里重复实现。

改 `doc/` 之后重新生成（`make -C source manual` 也会跑）：

    python tools/embed_manual.py
"""

import os
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DOC_DIR = os.path.join(ROOT, "doc")
EN_DIR = os.path.join(DOC_DIR, "en")
MANIFEST = os.path.join(ROOT, "example", "manifest.tsv")
OUT_CPP = os.path.join(ROOT, "source", "src", "manual_data.cpp")

#: 十六种生成模式名，顺序与文档矩阵表一致（`source/src/analysis.cpp` 也认这一组）。
MODE_NAMES = ["ll0", "ll1", "llk", "lr0", "slr", "slrk", "lalr", "lalrk",
              "lr1", "lrk", "ielr", "glr", "earley", "cyk", "dd", "lex"]


def pack_tool_path():
    """定位 lztext_pack 命令行工具；找不到就现场编译一个。"""
    env = os.environ.get("LZTEXT_PACK")
    if env and os.path.isfile(env):
        return env
    for candidate in ("build/lztext/lztext-pack.exe", "build/lztext/lztext-pack"):
        path = os.path.join(ROOT, candidate)
        if os.path.isfile(path):
            return path
    compiler = shutil.which("g++") or shutil.which("c++") or shutil.which("clang++")
    if compiler is None:
        raise RuntimeError("找不到 lztext_pack，也没有可用的 C++ 编译器；"
                           "先跑 `make -C library/lztext pack`，或设置 LZTEXT_PACK 指向该工具")
    src_dir = os.path.join(ROOT, "library", "lztext")
    out_dir = os.path.join(ROOT, "build", "lztext")
    os.makedirs(out_dir, exist_ok=True)
    out_path = os.path.join(out_dir, "lztext-pack.exe" if os.name == "nt" else "lztext-pack")
    subprocess.check_call([compiler, "-std=c++17", "-O2", "-I", src_dir,
                           os.path.join(src_dir, "lztext_pack.cpp"),
                           os.path.join(src_dir, "lztext.cpp"),
                           "-o", out_path])
    return out_path


def compress_bundle(blob):
    """用 lztext_pack 压缩一段字节；工具自带解压回读比对，产物一定可解。"""
    tool = pack_tool_path()
    with tempfile.TemporaryDirectory() as workdir:
        in_path = os.path.join(workdir, "in.bin")
        out_path = os.path.join(workdir, "out.bin")
        with open(in_path, "wb") as handle:
            handle.write(blob)
        subprocess.check_call([tool, in_path, out_path])
        with open(out_path, "rb") as handle:
            return handle.read()


def byte_array(name, data):
    """把字节数组写成 C++ 初始化列表，每行 24 个。"""
    lines = ["const unsigned char %s[] = {" % name]
    for i in range(0, len(data), 24):
        chunk = data[i:i + 24]
        lines.append("    " + ", ".join("0x%02x" % b for b in chunk) + ",")
    lines.append("};")
    return "\n".join(lines)


def read_text(path):
    """读一份 UTF-8 文本并把换行归一成 LF（仓库开了 core.autocrlf）。"""
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        return handle.read().replace("\r\n", "\n").replace("\r", "\n")


def plain(text):
    """去掉 Markdown 行内标记，留出能放进一行的纯文本摘要。

    只处理反引号、链接与星号强调：下划线在标识符里太常见（`INT_MAX`、
    `FIRST_k`），当成强调处理会把名字吃坏。
    """
    text = re.sub(r"`([^`]*)`", r"\1", text)
    text = re.sub(r"\[([^\]]*)\]\([^)]*\)", r"\1", text)
    text = re.sub(r"\*\*\*([^*]+)\*\*\*", r"\1", text)
    text = re.sub(r"\*\*([^*]+)\*\*", r"\1", text)
    text = re.sub(r"\*([^*\n]+)\*", r"\1", text)
    text = text.replace("\\|", "|").replace("&nbsp;", " ")
    text = re.sub(r"<br\s*/?>", " ", text)
    return re.sub(r"\s+", " ", text).strip()


def page_summary(body, title):
    """取正文里第一段能当摘要的普通文字（跳过标题、引用、表格、代码块、列表）。"""
    in_fence = False
    for line in body.split("\n"):
        stripped = line.strip()
        if stripped.startswith("```"):
            in_fence = not in_fence
            continue
        if in_fence or not stripped:
            continue
        if stripped.startswith(("#", ">", "|", "-", "*", "+", "<", "!", "[")):
            continue
        if stripped == title:
            continue
        text = plain(stripped)
        if len(text) >= 8:
            return text[:200] + ("…" if len(text) > 200 else "")
    return ""


def page_title(body, fallback):
    """取正文里第一个一级标题。"""
    for line in body.split("\n"):
        if line.startswith("# "):
            return plain(line[2:])
    return fallback


def pages():
    """按「索引优先、再按编号」排序收集文档页。"""
    result = []
    for name in sorted(os.listdir(DOC_DIR)):
        if not name.endswith(".md"):
            continue
        body = read_text(os.path.join(DOC_DIR, name))
        stem = name[:-3]
        title = page_title(body, name)
        english = ""
        english_path = os.path.join(EN_DIR, name)
        if os.path.isfile(english_path):
            english = read_text(english_path)
        # 有英文版时也取英文标题：`man --lang en` 的目录与页头得是英文，
        # 不然正文切了语言、标题还留在中文。
        result.append(dict(stem=stem, file=name, title=title,
                           titleEn=page_title(english, "") if english else "",
                           summary=page_summary(body, title), body=body, en=english))
    # 短编号只发给第一份同号文件（本仓库里 09 有两份），免得 `man 09` 指向不定。
    used = set()
    for page in result:
        match = re.match(r"^(\d+)-", page["stem"])
        number = match.group(1) if match else ""
        if number and number not in used:
            used.add(number)
            page["id"] = number
        else:
            page["id"] = ""

    def order(page):
        """索引页排最前，其余按短编号、再按文件名。"""
        if page["stem"].lower() == "readme":
            return (0, 0, page["stem"])
        return (1, int(page["id"]) if page["id"] else 999, page["stem"])

    result.sort(key=order)
    return result


def tables(path):
    """把一份 Markdown 里的表格拆成「表头 -> 行」的列表。

    单元格里的 `\\|` 是转义过的竖线（`doc/12-grammar-usage.md` 的字面量表格
    里到处都是），只按没转义的 `|` 切。
    """
    if not os.path.isfile(path):
        return []
    result = []
    header = None
    for line in read_text(path).split("\n"):
        stripped = line.strip()
        if stripped.startswith("|") and stripped.endswith("|"):
            cells = [cell.strip() for cell in re.split(r"(?<!\\)\|", stripped)[1:-1]]
            if header is None:
                header = cells
            elif all(re.fullmatch(r":?-{2,}:?", cell) for cell in cells):
                continue
            else:
                result.append((header, cells))
        else:
            header = None
    return result


def directives():
    """从 `doc/12-grammar-usage.md` 与英文版抽 `%指令`，按指令名对齐中英说明。"""
    found = {}

    def collect(path, key):
        for _, cells in tables(path):
            if len(cells) < 2 or "%" not in cells[0]:
                continue
            match = re.match(r"`(%[A-Za-z_][A-Za-z0-9_]*)", cells[0])
            if not match:
                continue
            name = match.group(1)
            entry = found.setdefault(name, dict(name=name, usage="", summary="", summaryEn=""))
            if not entry["usage"]:
                entry["usage"] = plain(cells[0])
            if not entry[key]:
                entry[key] = plain(cells[-1])

    collect(os.path.join(DOC_DIR, "12-grammar-usage.md"), "summary")
    collect(os.path.join(EN_DIR, "12-grammar-usage.md"), "summaryEn")
    return [found[name] for name in sorted(found)]


def mode_matrix(path):
    r"""读支持矩阵：表头给目标语言，行给模式。

    表头形如 `C99 \`c\``，末行是「合计 / Total」；返回（语言表, 模式名 -> 算法或用途）。
    表头那一行的数据行（本仓库里是 `ll0`）也要收进模式表，所以见到表头只是记下语言，
    不跳过这一行。
    """
    languages = []
    totals = []
    descriptions = {}
    for header, cells in tables(path):
        if len(cells) < 3 or len(header) != len(cells):
            continue
        first = plain(header[0]) if header else ""
        isMatrix = "模式" in first or first.lower().startswith("mode")
        if not languages:
            # 矩阵的第一条数据行是 `ll0`；靠它确认这确实是要找的那张表。
            if not isMatrix or plain(cells[0]).lower() != MODE_NAMES[0]:
                continue
            for cell in header[2:]:
                match = re.search(r"`([A-Za-z][A-Za-z0-9]*)`", cell)
                label = plain(re.sub(r"`[^`]*`", "", cell)).strip()
                languages.append(dict(name=match.group(1) if match else "",
                                      usage=label if label.isascii() else "",
                                      summary="", summaryEn=""))
            if len(languages) < 3:
                languages = []
                continue
        total = plain(cells[0])
        if "合计" in total or total.lower().startswith("total"):
            totals = [plain(cell) for cell in cells[2:]]
            continue
        name = plain(cells[0]).lower()
        if name in MODE_NAMES:
            descriptions.setdefault(name, plain(cells[1]))
    for index, language in enumerate(languages):
        count = totals[index] if index < len(totals) else ""
        if language["name"] == "gram":
            language["summary"] = "文法导出档：只写文法原文与 RESULT.md，不生成解析器"
            language["summaryEn"] = "Grammar export: writes the grammar text and RESULT.md only; no parser"
        elif count:
            language["summary"] = "支持 %s 种模式中的 %s 种" % (len(MODE_NAMES), count)
            language["summaryEn"] = "Supports %s of the %s modes" % (count, len(MODE_NAMES))
    return languages, descriptions


def modes():
    """模式名与算法取自支持矩阵；用途一句取自 `doc/13-modes.md`（中英各一遍）。"""
    _, descriptions = mode_matrix(os.path.join(DOC_DIR, "15-language-mode-support.md"))
    _, descriptions_en = mode_matrix(os.path.join(EN_DIR, "15-language-mode-support.md"))
    purposes, purposes_en = {}, {}
    for path, target in ((os.path.join(DOC_DIR, "13-modes.md"), purposes),
                         (os.path.join(EN_DIR, "13-modes.md"), purposes_en)):
        for _, cells in tables(path):
            if len(cells) >= 3:
                name = plain(cells[0]).lower()
                if name in MODE_NAMES:
                    target.setdefault(name, plain(cells[-1]))
    result = []
    for name in MODE_NAMES:
        summary = purposes.get(name, "") or descriptions.get(name, "")
        summary_en = purposes_en.get(name, "") or descriptions_en.get(name, "")
        result.append(dict(name=name, usage=descriptions.get(name, ""),
                           summary=summary, summaryEn=summary_en))
    return result


def examples():
    """读 `example/manifest.tsv`：示例名、验证过的语言档与固定输入。"""
    result = []
    if not os.path.isfile(MANIFEST):
        return result
    with open(MANIFEST, "r", encoding="utf-8", errors="replace") as handle:
        for line in handle:
            if line.startswith("#") or not line.strip():
                continue
            cells = line.rstrip("\n").split("\t")
            if len(cells) < 2 or not cells[0].strip():
                continue
            result.append(dict(name=cells[0].strip(), usage=cells[1].strip(),
                               summary=cells[2].strip() if len(cells) > 2 else "",
                               summaryEn=""))
    return result


def quote(text):
    """C++ 字符串字面量（转义反斜线与引号，中文按 UTF-8 原样写入）。"""
    return '"' + text.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n") + '"'


def pages_array(entries):
    """把文档页元信息写成 C++ 记录表。"""
    lines = ["const ManualPage kPages[] = {"]
    for entry in entries:
        lines.append("    {%s, %s, %s, %s, %s, %s, %s}," % (
            quote(entry["id"]), quote(entry["stem"]), quote(entry["file"]),
            quote(entry["title"]), quote(entry["titleEn"]),
            quote(entry["summary"]),
            "true" if entry["en"] else "false"))
    lines.append("};")
    return "\n".join(lines)


def entries_array(name, entries):
    """把一批「名字 / 用法 / 说明（中英）」条目写成 C++ 记录表。"""
    lines = ["const ManualEntry %s[] = {" % name]
    for entry in entries:
        lines.append("    {%s, %s, %s, %s}," % (
            quote(entry["name"]), quote(entry["usage"]),
            quote(entry["summary"]), quote(entry.get("summaryEn", ""))))
    lines.append("};")
    return "\n".join(lines)


def main():
    """生成内联手册数据源文件。"""
    page_list = pages()
    directive_list = directives()
    mode_list = modes()
    language_list, _ = mode_matrix(os.path.join(DOC_DIR, "15-language-mode-support.md"))
    example_list = examples()

    # 正文（中英各一遍）拼接后整体压缩：跨页的重复也能被利用。
    slices = []
    blob_parts = []
    cursor = 0
    for page in page_list:
        data = page["body"].encode("utf-8")
        slices.append(("page/" + page["file"], cursor, len(data)))
        blob_parts.append(data)
        cursor += len(data)
        if page["en"]:
            data = page["en"].encode("utf-8")
            slices.append(("page-en/" + page["file"], cursor, len(data)))
            blob_parts.append(data)
            cursor += len(data)
    blob = b"".join(blob_parts)
    packed = compress_bundle(blob)

    with open(OUT_CPP, "w", encoding="utf-8", newline="") as handle:
        handle.write("// 本文件由 tools/embed_manual.py 生成，请勿手工修改。\n")
        handle.write("// 生成命令：python3 tools/embed_manual.py\n")
        handle.write("// 文档正文以 lztext 压缩形式内联（运行时解压一次再按偏移切片），"
                     "算法见 library/lztext/README.md。\n\n")
        handle.write('#include "ParseEngine/manual_data.hpp"\n\n')
        handle.write('#include "lztext/lztext.hpp"\n\n')
        handle.write("#include <cstddef>\n")
        handle.write("#include <string>\n\n")
        handle.write("namespace ParseEngine\n{\nnamespace Manual\n{\n\nnamespace\n{\n\n")
        handle.write("// 全部文档正文按顺序拼接后压缩的结果（%d 段，原始 %d 字节，压缩后 %d 字节）。\n"
                     % (len(slices), len(blob), len(packed)))
        handle.write(byte_array("kPacked", packed))
        handle.write("\n\nstruct Slice\n{\n    const char *key;   ///< `page/<文件>` 或 `page-en/<文件>`\n"
                     "    std::size_t offset;  ///< 解压后缓冲区里的起始下标\n"
                     "    std::size_t length;  ///< 字节数\n};\n\n")
        handle.write("const Slice kSlices[] = {\n")
        for key, offset, length in slices:
            handle.write('    {%s, %d, %d},\n' % (quote(key), offset, length))
        handle.write("};\n\n")
        handle.write("} // namespace\n\n")

        handle.write(pages_array(page_list) + "\n\n")
        handle.write(entries_array("kEntries", directive_list) + "\n\n")
        handle.write(entries_array("kModes", mode_list) + "\n\n")
        handle.write(entries_array("kLanguages", language_list) + "\n\n")
        handle.write(entries_array("kExamples", example_list) + "\n")

        handle.write("\nconst std::string &text()\n{\n"
                     "    // 解压一次；失败时是空串，下面的下标检查会跳过全部条目。\n"
                     "    static const std::string kText =\n"
                     "        lztext::decompressString(kPacked, sizeof(kPacked));\n"
                     "    return kText;\n}\n\n")
        handle.write("std::size_t pageCount() { return sizeof(kPages) / sizeof(kPages[0]); }\n")
        handle.write("const ManualPage &pageAt(std::size_t index) { return kPages[index]; }\n")
        handle.write("std::size_t sliceCount() { return sizeof(kSlices) / sizeof(kSlices[0]); }\n")
        handle.write("std::string sliceAt(std::size_t index)\n{\n"
                     "    const Slice &slice = kSlices[index];\n"
                     "    const std::string &body = text();\n"
                     "    if (slice.offset + slice.length > body.size())\n"
                     "    {\n        return {};\n    }\n"
                     "    return body.substr(slice.offset, slice.length);\n}\n")
        handle.write("const char *sliceKeyAt(std::size_t index) { return kSlices[index].key; }\n\n")
        handle.write("std::size_t directiveCount() { return sizeof(kEntries) / sizeof(kEntries[0]); }\n")
        handle.write("const ManualEntry &directiveAt(std::size_t index) { return kEntries[index]; }\n")
        handle.write("std::size_t modeCount() { return sizeof(kModes) / sizeof(kModes[0]); }\n")
        handle.write("const ManualEntry &modeAt(std::size_t index) { return kModes[index]; }\n")
        handle.write("std::size_t languageCount() { return sizeof(kLanguages) / sizeof(kLanguages[0]); }\n")
        handle.write("const ManualEntry &languageAt(std::size_t index) { return kLanguages[index]; }\n")
        handle.write("std::size_t exampleCount() { return sizeof(kExamples) / sizeof(kExamples[0]); }\n")
        handle.write("const ManualEntry &exampleAt(std::size_t index) { return kExamples[index]; }\n\n")
        handle.write("} // namespace Manual\n} // namespace ParseEngine\n")

    print("wrote %s (%d pages, %d directives, %d modes, %d languages, %d examples; "
          "%d -> %d bytes, ratio %.3f)"
          % (os.path.relpath(OUT_CPP, ROOT), len(page_list), len(directive_list),
             len(mode_list), len(language_list), len(example_list),
             len(blob), len(packed), (len(packed) / len(blob)) if blob else 0.0))


if __name__ == "__main__":
    sys.exit(main())
