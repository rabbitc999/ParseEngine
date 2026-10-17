"""总览图生成回归：各项目的 `make_*_overview.py` 能重跑，产物落在各自目录。

判据：
  1. 每个生成器都能跑通（README 的总览图都还在）；
  2. 产物成对存在（`.svg` + `.png`）、非空，PNG 带正确签名，SVG 是矢量文本；
  3. 生成是**确定**的：同一份源码跑两次，SVG 逐字节一致（改文案后重跑只应该改内容）。

Magel 的总览图随它自己的仓库走了（那边 `tests/make_overview.py` 管），MetroLang 亦然
（那边的 `tools/make_overview.py` 管），RustView 同样（它自己仓库的
`tools/make_overview.py` 管），这里只剩本仓库的一张。

用法（在仓库任意目录执行都可以）：
  python tests/make_overview_image.py            # 重新生成并断言
  python tests/make_overview_image.py --repo DIR # 指定仓库根
"""

from pathlib import Path
import argparse
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]

# 生成器（相对仓库根）→ (产物名, 产物目录)（与各自脚本的 print 对齐）。
# Magel / MetroLang / RustView 的总览图随各自独立仓库走（那边 tools/make_overview.py 管）。
GENERATORS = {
    "tools/make_overview_image.py": ("ParseEngine", "tools/overview"),
}

PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"


def run(args, cwd):
    """执行命令并保留诊断。"""
    return subprocess.run([str(a) for a in args], cwd=cwd, capture_output=True,
                          encoding="utf-8", errors="replace", timeout=300)


def main():
    """跑一遍总览图生成回归。"""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=ROOT)
    args = parser.parse_args()
    root = args.repo.resolve()
    failures = []

    for script, (name, out_rel) in GENERATORS.items():
        out_dir = root / out_rel
        result = run([sys.executable, root / script], cwd=root)
        if result.returncode != 0:
            failures.append(f"{script} 退出码 {result.returncode}\n{result.stdout}\n{result.stderr}")
            continue

        svg = out_dir / f"{name}-overview.svg"
        png = out_dir / f"{name}-overview.png"
        for produced in (svg, png):
            if not produced.exists() or produced.stat().st_size == 0:
                failures.append(f"{script} 没有产出 {produced.relative_to(root)}")
                continue
            if produced.suffix == ".png" and produced.read_bytes()[:8] != PNG_SIGNATURE:
                failures.append(f"{produced.relative_to(root)} 不是 PNG")
            if produced.suffix == ".svg" and "<svg" not in produced.read_text(
                    encoding="utf-8", errors="replace")[:2000]:
                failures.append(f"{produced.relative_to(root)} 不像 SVG")
        print(f"[通过] {script} → {out_rel}/{name}-overview.svg + .png")

    # 确定性：重跑一次 ParseEngine 的总览图，SVG 必须逐字节一致。
    target = root / "tools" / "overview" / "ParseEngine-overview.svg"
    if target.exists():
        before = target.read_bytes()
        run([sys.executable, root / "tools" / "make_overview_image.py"], cwd=root)
        if target.read_bytes() != before:
            failures.append("make_overview_image.py 两次生成的 SVG 不一致（不确定）")
        else:
            print("[通过] 重复生成逐字节一致（确定）")

    if failures:
        raise SystemExit("\n\n".join(failures))
    print(f"总览图回归通过（{len(GENERATORS)} 个生成器，产物在 tools/overview/）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
