#!/bin/sh
# ci/run.sh —— ParseEngine 的门禁：构建文法生成器 + 跑它自己的检查。
#
# 为什么要有它：回归散在 check.sh、tests/*.py 和两份 CMake 单测里，换台机器就可能
# 有人漏跑。这个脚本是**唯一的门禁入口**，本地与 Gitee Go 都调它，保证判据一致。
#
# 用法（在仓库根跑）：
#   sh ci/run.sh                 全部（含 CMake 构建的 stackvm 单测）
#   sh ci/run.sh --skip-vm-tests 跳过 ParseEngine 自己的 CMake / 生成产物检查（快一点）
#   sh ci/run.sh --no-python     只编 ParseEngine 本身，不跑 tests/*.py
#   sh ci/run.sh --help
#
# 环境要求：g++ / gcc / make / python3 / cmake / OpenSSL 开发包。全部为必需项，缺一个直接退出 2。
# **不需要 root**：脚本只往 build/ 与 tmp/ 写（都在 .gitignore 里）。若工作树里已有
# 别人留下的 root 产物（例如旧构建），先清掉再跑，否则 make 会写不进去。
#
# 退出码：0 全绿；1 有检查失败；2 环境不满足。

set -u
# 已有诊断夹具使用中文；三语专项测试会自行隔离环境。
export MAGEL_LANG=zh
export PARSEENGINE_LANG=zh

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd) || exit 2
cd "$root" || exit 2

skip_vm=0
run_python=1
for arg in "$@"; do
    case "$arg" in
    --skip-vm-tests) skip_vm=1 ;;
    --no-python) run_python=0 ;;
    -h | --help)
        awk 'NR > 1 && /^#/ { sub(/^# ?/, ""); print; next } NR > 1 { exit }' "$0"
        exit 0
        ;;
    *)
        echo "未知参数：$arg（用 --help 看用法）" >&2
        exit 2
        ;;
    esac
done

have() { command -v "$1" >/dev/null 2>&1; }

echo "== 环境检测 =="
missing=''
for tool in g++ gcc make cmake; do
    if have "$tool"; then
        printf '  %-8s %s\n' "$tool" "$(command -v "$tool")"
    else
        printf '  %-8s 缺失（必需）\n' "$tool"
        missing="$missing $tool"
    fi
done
PYTHON=''
for candidate in python3 python; do
    if have "$candidate" && [ "$("$candidate" -c 'import sys; print("MAGEL-PYTHON-OK" if sys.version_info >= (3, 11) else "too-old")' 2>/dev/null)" = "MAGEL-PYTHON-OK" ]; then PYTHON="$candidate"; break; fi
done
if [ -n "$PYTHON" ]; then
    printf '  %-8s %s\n' python "$(command -v "$PYTHON")"
else
    printf '  %-8s 缺失（必需）\n' python
    missing="$missing python"
fi
if [ -n "$missing" ]; then
    echo
    echo "缺少必需工具：$missing —— 装好再跑。" >&2
    exit 2
fi

step=0
failed=''
run_step() { # 标题 命令...
    title=$1
    shift
    step=$((step + 1))
    printf '\n[%2d] %s ... ' "$step" "$title"
    started=$(date +%s)
    if "$@" >"tmp/ci/logs/$(printf '%02d' "$step").log" 2>&1; then
        echo "ok（$(($(date +%s) - started))s）"
        return 0
    fi
    echo "失败（$(($(date +%s) - started))s）"
    echo "     完整输出：tmp/ci/logs/$(printf '%02d' "$step").log"
    failed="$failed
  - $title"
    return 1
}

mkdir -p tmp/ci/logs

# ---- 1. ParseEngine 生成器（Magel 的 gen.sh 也用它把 magel.gram 生成出解析器）----
# `source/src/templates_data.cpp` 是入库的，裸 make 就够；生成物 build/bin/ParseEngine*。
[ -x build/bin/ParseEngine ] || [ -x build/bin/ParseEngine.exe ] || \
    run_step "构建 ParseEngine 生成器" make -C source -j4

# ---- 2. ParseEngine 自己那一摊（不依赖 Magel 仓库；本地有 magel/ 时脚本会顺带检查它的二进制）----
if [ "$skip_vm" = 0 ]; then
    run_step "ParseEngine 生成产物清单" "$PYTHON" tests/pe_build_files.py
    run_step "ParseEngine 三语、控制台与 ZIP 发布脚本" "$PYTHON" tests/parseengine_i18n.py
fi

# ---- 3. 汇总 ----
echo
echo "== 汇总 =="
if [ -z "$failed" ]; then
    echo "  步骤：$step，全部通过"
    exit 0
fi
fail_count=$(printf '%s\n' "$failed" | grep -c '^  - ')
echo "  步骤：$step，成功 $((step - fail_count))，失败 $fail_count"
echo "  失败：$failed"
exit 1
