#!/bin/sh
# tools/diff.sh —— 产物**逐字节**比对（严格口径）。判「这次改动到底有没有动到产物」用它。
#
# 用法：
#   tools/diff.sh <基准目录> <新目录> [语言档...]
#
#     语言档：c / cpp17 / python / csharp / rust / gram，可给多个（空格分隔）；
#             **不给就在全部目录上比**（多 agent 共用一棵树时不要这样用，见下）。
#
# 例（最常用：只比 C++17 那一档）：
#   tools/diff.sh tmp/base33_cpp17 tmp/cpp17_after cpp17
#
# 退出码：0 = 逐字节一致；1 = 有差异（差异直接打在 stdout）；2 = 用法错。
#
# ── 为什么要有它（后续接手的人先读这段）────────────────────────────────────
#
# 这个仓的产物来自「模板 + 生成器」，验收分两档：
#
#   * **逐字节一致（本脚本）**：动**排版**——把字符串字面量搬进模板、把
#     `replaceAll`/拼接换成 `{% for %}` / `{% if %}`——时用它。目标是
#     **产物一个字节都不变**，这是「只搬了位置、没改行为」最硬的证据。
#   * **语义一致**（`PE_DIFF_SEMANTIC=1`，或 `tools/quick-verify.sh`）：产物
#     允许差空白 / 空行（提交时行尾本来会被 git 归一）。只有确实要改产物排版
#     （比如顺手去掉一行多余空行）时才用它，并在提交信息里说明。
#
# 基准怎么来：用**改动之前**的二进制跑 `tools/gen_all.sh`
# （或 `tools/gen_lang.sh` 只生成一档），产物落在 `<输出目录>/<文法>.<语言>/`。
# 例：
#   tools/gen_lang.sh build/bin/ParseEngine.exe tmp/base_cpp17 cpp17
#   …改…
#   tools/gen_lang.sh tmp/isolated/cpp17/build/bin/ParseEngine.exe tmp/after_cpp17 cpp17
#   tools/diff.sh tmp/base_cpp17 tmp/after_cpp17 cpp17
#
# ⚠️ 共享工作区（多 agent / 多人同时改同一棵树）：别人的档也在变，**一定要把
#    语言档传上**，只看自己那一档，否则满屏都是别人的假差异
#    （见 AGENTS.md §2「并发与编译锁」）。
set -u

PATH="/usr/bin:/bin:$PATH"
export PATH

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd) || exit 2
cd "$root" || exit 2

base=${1:-}
after=${2:-}
if [ -z "$base" ] || [ -z "$after" ]; then
    sed -n '2,10p' "$0" | sed 's/^# \{0,1\}//' >&2
    exit 2
fi
shift 2
langs=$*

if [ ! -d "$base" ]; then
    echo "基准目录不存在：$base" >&2
    exit 2
fi
if [ ! -d "$after" ]; then
    echo "新目录不存在：$after" >&2
    exit 2
fi

# 比对选项：默认逐字节；PE_DIFF_SEMANTIC=1 时忽略空白与空行
opts="-r"
mode="逐字节"
if [ "${PE_DIFF_SEMANTIC:-0}" = "1" ]; then
    opts="-r -w -B"
    mode="语义（忽略空白与空行）"
fi

# 收集要比的产物目录名
dirs=""
if [ -n "$langs" ]; then
    for lang in $langs; do
        for dir in "$base"/*."$lang"; do
            [ -d "$dir" ] || continue
            dirs="$dirs $(basename "$dir")"
        done
    done
else
    for dir in "$base"/*; do
        [ -d "$dir" ] || continue
        dirs="$dirs $(basename "$dir")"
    done
fi

if [ -z "$dirs" ]; then
    echo "在 $base 里没找到要比的产物目录（语言档：${langs:-全部}）" >&2
    exit 2
fi

count=0
bad=0
missing=0
for name in $dirs; do
    count=$((count + 1))
    if [ ! -d "$after/$name" ]; then
        echo "[缺] $name：新目录里没有这个产物目录"
        missing=$((missing + 1))
        continue
    fi
    # shellcheck disable=SC2086
    if out=$(diff $opts "$base/$name" "$after/$name"); then
        continue
    fi
    bad=$((bad + 1))
    echo "[差] $name"
    echo "$out" | head -40
    total=$(echo "$out" | grep -c '^[<>]')
    if [ "$total" -gt 40 ]; then
        echo "     …（这个目录共 $total 行差异，上面只打前 40 行）"
    fi
done

echo "───────────────────────────────────────────────"
if [ "$bad" -eq 0 ] && [ "$missing" -eq 0 ]; then
    echo "比了 $count 个产物目录：**$mode口径下完全一致**"
    exit 0
fi
echo "比了 $count 个产物目录：$bad 个有差异、$missing 个缺失（$mode口径）" >&2
exit 1
