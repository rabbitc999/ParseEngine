#!/bin/sh
# ci/run.sh —— ParseEngine 的门禁：构建文法生成器 + 跑它自己的检查。
#
# 为什么要有它：回归散在 check.sh、tests/*.py 和两份 CMake 单测里，换台机器就可能
# 有人漏跑。这个脚本是**唯一的门禁入口**，本地与远端 CI 都调它，保证判据一致。
# 判据与平台无关：Gitee 只当作草稿纸跑这一份，GitHub 上是同一份，不许出现平台相关的分支。
#
# 用法（在仓库根跑）：
#   sh ci/run.sh                 C/C++/Python 核心回归与生成产物检查
#   sh ci/run.sh --all-languages 再检查 C#/Rust/Go/Java/JavaScript（工具链必须齐全）
#   sh ci/run.sh --skip-vm-tests 跳过生成产物构建清单与打包检查，保留核心回归
#   sh ci/run.sh --no-python     只编 ParseEngine 本身，不跑 tests/*.py
#   sh ci/run.sh --help
#
# 环境要求：g++ / gcc / make / Python >= 3.11；产物检查还需要 cmake。
# --all-languages 额外要求 dotnet / cargo / rustc / go / javac / java / node。
# Rust 回归离线执行，需要预先缓存 regex crate；JS 产物需要 Node.js >= 22.18。
# **不需要 root**：脚本只往 build/ 与 tmp/ 写（都在 .gitignore 里）。若工作树里已有
# 别人留下的 root 产物（例如旧构建），先清掉再跑，否则 make 会写不进去。
#
# 退出码：0 全绿；1 有检查失败；2 环境不满足。

set -u
# 已有诊断夹具使用中文；双语专项测试会自行隔离环境。
export MAGEL_LANG=zh
export PARSEENGINE_LANG=zh
export PYTHONIOENCODING=utf-8

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd) || exit 2
cd "$root" || exit 2

skip_vm=0
run_python=1
all_languages=0
for arg in "$@"; do
    case "$arg" in
    --skip-vm-tests) skip_vm=1 ;;
    --no-python) run_python=0 ;;
    --all-languages) all_languages=1 ;;
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
if [ "$all_languages" = 1 ] && [ "$run_python" = 0 ]; then
    echo "--all-languages 需要运行 Python 回归，不能与 --no-python 同用。" >&2
    exit 2
fi

have() { command -v "$1" >/dev/null 2>&1; }

echo "== 环境检测 =="
missing=''
required_tools='g++ gcc make'
if [ "$run_python" = 1 ] && [ "$skip_vm" = 0 ]; then required_tools="$required_tools cmake"; fi
if [ "$all_languages" = 1 ]; then required_tools="$required_tools dotnet cargo rustc go javac java node"; fi
for tool in $required_tools; do
    if have "$tool"; then
        printf '  %-8s %s\n' "$tool" "$(command -v "$tool")"
    else
        printf '  %-8s 缺失（必需）\n' "$tool"
        missing="$missing $tool"
    fi
done
PYTHON=''
for candidate in python3 python; do
    if have "$candidate" && [ "$("$candidate" -c 'import sys; print("PARSEENGINE-PYTHON-OK" if sys.version_info >= (3, 11) else "too-old")' 2>/dev/null)" = "PARSEENGINE-PYTHON-OK" ]; then PYTHON="$candidate"; break; fi
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

# ---- 1. 总是更新内嵌资产并增量构建，禁止测试旧生成器 ----
run_step "重建内嵌模板" "$PYTHON" tools/embed_templates.py || exit 1
run_step "重建离线手册" "$PYTHON" tools/embed_manual.py || exit 1
run_step "构建 ParseEngine 生成器" make -C source -j4 || exit 1

# ---- 2. 核心回归，缺工具或失败均明确报错，不静默跳过 ----
if [ "$run_python" = 1 ]; then
    run_step "C++ DFA 与标准库一致性" "$PYTHON" tests/cpp_dfa_std.py
    run_step "扩展正则跨 C/C++ 回归" "$PYTHON" tests/regex_extended.py
    run_step "Unicode 正则跨 C/C++ 回归" "$PYTHON" tests/regex_unicode.py
    run_step "正则预算、取消、分配失败与增量缓存" "$PYTHON" tests/regex_budget.py
    run_step "正则兼容性报告、严格模式与纯校验" "$PYTHON" tests/regex_compatibility.py
    run_step "二进制增量生成与 C/C++ 子集" "$PYTHON" tests/generation_cache.py
    run_step "C/C++ 自定义错误提示" "$PYTHON" tests/error_formatter.py
    run_step "Python 全模式自定义错误提示" "$PYTHON" tests/error_formatter_languages.py --langs python
    run_step "生成文件清理与用户文件保护" "$PYTHON" tests/clear_mode.py
    run_step "C++/Python Earley 与 CYK" "$PYTHON" tests/chart_modes.py --langs cpp17,python
    for mode in glr earley cyk; do
        run_step "C++ $mode 增量语义复用" "$PYTHON" tests/glr_semantic_reuse.py --mode "$mode"
    done
    if [ "$all_languages" = 1 ]; then
        run_step "其余五语言全模式自定义错误提示" "$PYTHON" tests/error_formatter_languages.py --langs csharp,rust,go,java,js
        run_step "其余五语言 Earley 与 CYK" "$PYTHON" tests/chart_modes.py --langs csharp,rust,go,java,js
        for mode in glr earley cyk; do
            run_step "Rust $mode 增量语义复用" "$PYTHON" tests/glr_incremental_rust.py --mode "$mode"
        done
    fi
    if [ "$skip_vm" = 0 ]; then
        run_step "ParseEngine 生成产物清单" "$PYTHON" tests/pe_build_files.py
        run_step "ParseEngine 双语、控制台与 ZIP 发布脚本" "$PYTHON" tests/parseengine_i18n.py
    else
        echo "已按 --skip-vm-tests 跳过产物清单与打包检查。"
    fi
else
    echo "已按 --no-python 跳过全部 Python 回归，仅验证构建。"
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
