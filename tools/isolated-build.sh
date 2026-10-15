#!/bin/sh
# tools/isolated-build.sh —— 在 `tmp/<树名>/` 里造一棵「只有自己改动」的源码树并编译。
#
# 为什么需要（配合 AGENTS.md §2「并发与编译锁」的规则 5）：多 agent 共用一棵树时，
# 别人的中间状态会让 `make` 直接编不过（你改你的 C99，他改他的 C++17 改到一半），
# 于是你既没法验收、也不该去改他的文件。办法是**把工作区里属于你的那几个文件
# 盖到一棵以 HEAD 为底的干净树上**再编——别人改什么跟你无关。
#
# 用法：
#   tools/isolated-build.sh <树名> [要覆盖的文件或 glob...]
#     <树名>    tmp/<树名>/ 这棵隔离树（会先清掉重建）
#     文件      默认 = source/src/code_gen_c99.cpp + source/templates/c99_*（C99 那一档）
#
# 产物：tmp/<树名>/build/bin/ParseEngine.exe
#   —— 直接拿它跑 tools/gen_all.sh / tools/smoke.sh 验收就行。
#
# 注意：以 HEAD 为底，所以**未跟踪的新文件要用 glob 显式带上**（默认那个
# `source/templates/c99_*` 就把新模板带上了）。临时文件（tmp/）不进 git，
# 但脚本本身在 tools/ 下。

set -u

PATH="/usr/bin:/bin:$PATH"
export PATH

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd) || exit 2
cd "$root" || exit 2

name=${1:-}
if [ -z "$name" ]; then
    echo "用法：$0 <树名> [要覆盖的文件或 glob...]" >&2
    echo "      $0 head        只按 HEAD 编一棵树（不覆盖任何文件）——用来出验收基线" >&2
    exit 2
fi
shift

# head 模式：纯 HEAD，一个文件都不覆盖。它的产物就是「当前提交」的基准，
# 拿它 gen_all 出 tmp/base_<日期>，再和自己那棵树比，diff 应该只剩自己的改动。
pure_head=0
if [ "$name" = "head" ]; then
    pure_head=1
fi

files=${*:-}
if [ -z "$files" ] && [ "$pure_head" -eq 0 ]; then
    files="source/src/code_gen_c99.cpp source/templates/c99_*"
fi

tree="tmp/isolated/$name"

# 默认**增量**：树已经在就只把自己那几个文件盖上去，再 `make`——Makefile 带
# `-MMD` 依赖跟踪，只会重编改过的那个 .o 再链一次（几秒）。整棵重建才 100 秒左右，
# 只在「HEAD 动过」「第一次」「要彻底排除污染」时才需要：传 --clean（或
# FORCE_CLEAN=1）。
force_clean=0
[ "${FORCE_CLEAN:-0}" = "1" ] && force_clean=1
kept=""
for arg in "$@"; do
    if [ "$arg" = "--clean" ]; then
        force_clean=1
    else
        kept="$kept $arg"
    fi
done
set -- $kept

if [ "$force_clean" -eq 0 ] && [ -f "$tree/source/Makefile" ]; then
    # 增量：报一句 HEAD 有没有动过（树是旧 HEAD 建的，diff 里会冒出别人的差异）
    if [ -f "$tree/.head" ]; then
        was=$(cat "$tree/.head" 2>/dev/null)
        now=$(git rev-parse HEAD 2>/dev/null)
        if [ "$was" != "$now" ]; then
            echo "注意：HEAD 动过了（树建在 $was，现在是 $now）——建议加 --clean 重建" >&2
        fi
    fi
    echo "隔离树 $tree：增量（只重编改过的文件；要重建加 --clean）"
else
    if [ -d "$tree" ]; then
        find "$tree" -depth -delete 2>/dev/null || rm -rf "$tree"
    fi
    mkdir -p "$tree" || exit 2
    # 以 HEAD 为底：git archive 是只读操作，不碰 .git
    if ! git archive HEAD | tar -x -C "$tree"; then
        echo "git archive 失败（沙箱里 .git 不可读？）" >&2
        exit 2
    fi
    git rev-parse HEAD >"$tree/.head" 2>/dev/null || true
    echo "隔离树 $tree：以 HEAD 为底整棵重建（冷编译约 100 秒）"
fi

# 把当前工作区里属于我的文件盖上去
copied=0
for pattern in $files; do
    for src in $pattern; do
        [ -f "$src" ] || continue
        dest="$tree/$src"
        mkdir -p "$(dirname "$dest")" || exit 2
        cp "$src" "$dest" || exit 2
        copied=$((copied + 1))
    done
done
if [ "$pure_head" -eq 0 ] && [ "$copied" -eq 0 ]; then
    echo "一个文件都没盖上——检查一下参数" >&2
    exit 2
fi
if [ "$pure_head" -eq 1 ]; then
    echo "隔离树 $tree：纯 HEAD（基准）"
else
    echo "隔离树 $tree：HEAD 为底 + 覆盖 $copied 个文件"
fi

cd "$tree" || exit 2
echo "== make -C source templates =="
if ! make -C source templates; then
    echo "模板内联失败" >&2
    exit 1
fi
echo "== make =="
if ! make -C source; then
    echo "编译失败——**隔离树里还是你的东西**，改完重跑本脚本即可；" >&2
    echo "不用动共享工作区的锁。" >&2
    exit 1
fi
echo "产物：$root/$tree/build/bin/ParseEngine.exe"
