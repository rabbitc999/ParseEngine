#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把 source/templates/ 下的模板内联成 C++ 源码（仅构建期使用，产物无 Python 依赖）。

模板后缀有两种：

  - `.tmpl`     模板引擎格式（`{{ 变量 }}` / `{% if %}` / `{% for %}` / `{# 注释 #}`），
                 新迁移过来的模板都用它；
  - `.template` 老格式（`__占位符__` + 生成器里的 replaceAll），还没迁移的先用着。

两者按"去掉后缀的名字"共享同一个键（生成器里 `Utils::loadTemplate("csharp_token.cs")`
两种后缀都能命中）；同名时 `.tmpl` 优先，并打印一句提醒——正常迁移完就该把
老的 `.template` 删掉。

压缩：模板文本不再以原始字符串字面量直接摊在 `.rodata` 里，而是**整体拼接后用
library/lztext 压缩**，只把压缩后的字节内联进来，运行时由 `embeddedTemplates()`
解压重建。实测 1.09 MB 模板 -> 196 KB（比率 0.172），这是缩小 ParseEngine.exe
体积的主要手段。压缩算法**只有一份 C++ 实现**（library/lztext），本脚本通过调用
`lztext_pack` 命令行工具来压缩，不在 Python 里重复实现一遍。
"""

import os
import shutil
import subprocess
import sys
import tempfile

DELIM = "PGX"


def pack_tool_path(root):
    """定位 lztext_pack 命令行工具；找不到就现场编译一个。"""
    env = os.environ.get("LZTEXT_PACK")
    if env and os.path.isfile(env):
        return env
    for candidate in ("build/lztext/lztext-pack.exe", "build/lztext/lztext-pack"):
        path = os.path.join(root, candidate)
        if os.path.isfile(path):
            return path
    compiler = shutil.which("g++") or shutil.which("c++") or shutil.which("clang++")
    if compiler is None:
        raise RuntimeError("找不到 lztext_pack，也没有可用的 C++ 编译器；"
                           "先跑 `make -C library/lztext pack`，或设置 LZTEXT_PACK 指向该工具")
    src_dir = os.path.join(root, "library", "lztext")
    out_dir = os.path.join(root, "build", "lztext")
    os.makedirs(out_dir, exist_ok=True)
    out_path = os.path.join(out_dir, "lztext-pack.exe" if os.name == "nt" else "lztext-pack")
    subprocess.check_call([compiler, "-std=c++17", "-O2", "-I", src_dir,
                           os.path.join(src_dir, "lztext_pack.cpp"),
                           os.path.join(src_dir, "lztext.cpp"),
                           "-o", out_path])
    return out_path


def compress_bundle(root, blob):
    """用 lztext_pack 压缩一段字节；工具自带解压回读比对，产物一定可解。"""
    tool = pack_tool_path(root)
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


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    template_dir = os.path.join(root, "source", "templates")
    out_path = os.path.join(root, "source", "src", "templates_data.cpp")

    # 键 = 去掉后缀的模板名；同名时 .tmpl 覆盖 .template
    picked = {}
    for name in sorted(os.listdir(template_dir)):
        for suffix in (".template", ".tmpl"):
            if not name.endswith(suffix):
                continue
            key = name[: -len(suffix)]
            old = picked.get(key)
            if old is None or suffix == ".tmpl":
                if old is not None and old[1] != suffix:
                    print("注意：%s 同时有 %s 和 %s，用 %s（老模板该删了）"
                          % (key, old[0], name, name), file=sys.stderr)
                picked[key] = (name, suffix)

    names = [picked[key][0] for key in sorted(picked)]

    # 读进来的文本统一按 LF：仓库开了 core.autocrlf 时工作区的模板是 CRLF，
    # 而 C++ 编译器读源码里的原始字符串时也会把 CRLF 归一成 LF。这里先归一，
    # 内联表和 .tmpl 直读（SCRIPT_ENGINE_TEMPLATES）的结果就一致了。
    entries = []          # (键, 原始字节)
    for name in names:
        key = name[: -len(".tmpl" if name.endswith(".tmpl") else ".template")]
        with open(os.path.join(template_dir, name), "r", encoding="utf-8") as src:
            content = src.read()
        entries.append((key, content.encode("utf-8")))

    # 整体拼接后再压缩：跨模板的重复也能被利用，比率明显好于逐个压缩
    #（实测整体 0.172 vs 逐个约 0.36）。
    blob = b"".join(data for _, data in entries)
    packed = compress_bundle(root, blob)

    offsets = []
    cursor = 0
    for _, data in entries:
        offsets.append((cursor, len(data)))
        cursor += len(data)

    with open(out_path, "w", encoding="utf-8", newline="") as handle:
        handle.write("// 本文件由 tools/embed_templates.py 生成，请勿手工修改。\n")
        handle.write("// 生成命令：python3 tools/embed_templates.py\n")
        handle.write("// 模板文本以 lztext 压缩形式内联（运行时解压），"
                     "算法见 library/lztext/README.md。\n\n")
        handle.write('#include "ParseEngine/templates_data.hpp"\n\n')
        handle.write('#include "lztext/lztext.hpp"\n\n')
        handle.write("namespace ParseEngine\n{\n\n")
        handle.write("namespace\n{\n\n")
        handle.write("// 全部模板按顺序拼接后压缩的结果（%d 段，原始 %d 字节，压缩后 %d 字节）。\n"
                     % (len(entries), len(blob), len(packed)))
        handle.write(byte_array("kPacked", packed))
        handle.write("\n\n")
        handle.write("""struct TemplateSlice
{
    const char *name;      ///< 去掉后缀的模板名
    std::size_t offset;    ///< 在解压后缓冲区里的起始下标
    std::size_t length;    ///< 字节数
};

""")
        handle.write("const TemplateSlice kSlices[] = {\n")
        for (key, _), (offset, length) in zip(entries, offsets):
            handle.write('    {"%s", %d, %d},\n' % (key, offset, length))
        handle.write("};\n\n")
        handle.write("} // namespace\n\n")
        handle.write("""const std::unordered_map<std::string, std::string> &embeddedTemplates()
{
    // 解压一次，之后所有模板都从这个缓冲区里切片。解压失败时 k_text 为空，
    // 下面的下标检查会跳过全部条目（模板取不到会走 fatalInternal，不会越界读）。
    static const std::string k_text =
        lztext::decompressString(kPacked, sizeof(kPacked));
    static const std::unordered_map<std::string, std::string> k_table = [] {
        std::unordered_map<std::string, std::string> table;
        table.reserve(sizeof(kSlices) / sizeof(kSlices[0]));
        for (const TemplateSlice &slice : kSlices)
        {
            if (slice.offset + slice.length > k_text.size()) continue;
            table.emplace(slice.name,
                          std::string(k_text.data() + slice.offset, slice.length));
        }
        return table;
    }();
    return k_table;
}

""")
        handle.write("} // namespace ParseEngine\n")
    print("wrote %s (%d templates, %d -> %d bytes, ratio %.3f)"
          % (out_path, len(names), len(blob), len(packed),
             (len(packed) / len(blob)) if blob else 0.0))


if __name__ == "__main__":
    sys.exit(main())
