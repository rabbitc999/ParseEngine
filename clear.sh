#!/bin/sh
# clear.sh - 清理仓库根目录 build/ 中的可再生产物。
#
# 只删除 .gitignore 已明确忽略的路径；受版本控制文件和普通未跟踪文件都不会删除。
# 嵌套 Git 仓库也由 git clean 自动跳过。

set -u

root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P) || exit 2
dry_run=0

for arg in "$@"; do
    case "$arg" in
    -n | --dry-run) dry_run=1 ;;
    -h | --help)
        cat <<'EOF'
用法：
  sh clear.sh             删除根目录 build/ 中被 .gitignore 忽略的构建产物与缓存
  sh clear.sh --dry-run   只显示将删除的路径

脚本不会删除受版本控制文件、普通未跟踪文件或嵌套 Git 仓库。
EOF
        exit 0
        ;;
    *)
        echo "未知参数：$arg（用 --help 查看用法）" >&2
        exit 2
        ;;
    esac
done

if ! command -v git >/dev/null 2>&1; then
    echo "找不到 git，无法按 .gitignore 安全清理。" >&2
    exit 1
fi

top_raw=$(git -C "$root" rev-parse --show-toplevel 2>/dev/null) || {
    echo "clear.sh 必须位于 Git 仓库根目录。" >&2
    exit 1
}
top=$(CDPATH= cd -- "$top_raw" 2>/dev/null && pwd -P) || exit 1
if [ "$top" != "$root" ]; then
    echo "安全检查失败：脚本目录不是仓库根目录。" >&2
    echo "脚本目录：$root" >&2
    echo "仓库根目录：$top" >&2
    exit 1
fi

if [ -d "$root/tmp/build.lock" ]; then
    echo "检测到 tmp/build.lock；可能有构建正在运行，拒绝清理。" >&2
    echo "请先用 sh tools/lock.sh status 确认锁状态。" >&2
    exit 1
fi

cd "$root" || exit 1
if [ "$dry_run" = 1 ]; then
    echo "== 预览：以下被忽略文件将被删除 =="
    git clean -ndX -- build/
    exit $?
fi

echo "== 清理根目录 build/ 中被 .gitignore 忽略的构建产物与缓存 =="
git clean -fdX -- build/
status=$?
if [ "$status" -eq 0 ]; then
    echo "清理完成。"
fi
exit "$status"
