#!/bin/sh
# build-all.sh —— 一键构建：ParseEngine + 库和示例（不跑测试）
#
# Magel、MetroLang、Hydra86、RustView 与 CAS 已拆分为独立仓库（https://github.com/rabbitc999/magel、
# https://gitee.com/rabbitc999/metro-lang、https://gitee.com/rabbitc999/rust-view；
# Hydra86 与 CAS 的仓库地址不在本仓库提供），
# 它们的构建与门禁都在各自仓库里。
#
# 为什么要有它：独立工程位于 source/；
# `library/` 下 10 个 C/C++ 库 + 7 个 Rust crate，`project/` 下另有 7 个带 Makefile 的示例，
# 挨个敲容易漏。这个脚本按固定顺序全过一遍，**跳过测试**（`make check` / `check.sh` /
# 冒烟脚本一律不跑），缺工具链的那部分自动跳过并在末尾汇总。
#
# 用法（在仓库根跑）：
#   sh build-all.sh                 # 全量（生成器 + 库 + 示例）
#   sh build-all.sh --list          # 只做环境检测 + 打印将要构建的清单
#   sh build-all.sh --release       # 生成器走 release 构建
#   sh build-all.sh --no-rust       # 跳过 Rust crate
#   sh build-all.sh --no-examples   # 跳过 example/ 全量产物生成
#   sh build-all.sh --no-projects   # 跳过 project/
#   sh build-all.sh -q              # 安静模式：只打标题与结果，完整输出收进 build/build-all/logs/
#
# 输出：缺省把每条构建命令的完整输出**直接打在终端**，同时留档到 build/build-all/logs/；
#       -q/--quiet 才回到"只打标题与结果"的紧凑模式（-v/--verbose 是缺省行为，兼容保留）。
#       本脚本只构建、不跑测试（`make check` / `check.sh` / 冒烟脚本一律不跑）。
#
# 环境检测（缺谁跳过谁，必需项缺了直接退出）：
#   g++ / gcc / GNU make / Python 3 —— 必需（生成器与 C / C++ 产物）
#                                 Windows 上 GNU make 一般叫 mingw32-make.exe，两个名字都认
#   cargo / rustc               —— 可选（library/*-rust 与 Rust 档产物）
#   dotnet                      —— 可选（C# 档产物）
#   go                          —— 可选（Go 档产物）
#
# 退出码：0 全部成功（跳过的部分不算失败）；1 有一步失败（失败清单在末尾）；2 用法错。

set -u

# ---- 0. 定位仓库根 + 补 PATH（这个环境的坑，见 AGENTS.md §2）----
PATH="/c/MINGW64_YACC/bin:/c/Program Files/Git/usr/bin:/usr/bin:$PATH"
export PATH
root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd) || exit 2
cd "$root" || exit 2

list_only=0
quiet=0
build_rust=1
build_projects=1
build_examples=1
generator_target=''
unknown=''

for arg in "$@"; do
    case "$arg" in
    --list) list_only=1 ;;
    --release) generator_target='release' ;;
    --no-rust) build_rust=0 ;;
    --no-projects) build_projects=0 ;;
    --no-examples) build_examples=0 ;;
    -q | --quiet) quiet=1 ;;
    -v | --verbose) quiet=0 ;;
    -h | --help)
        awk 'NR > 1 && /^#/ { sub(/^# ?/, ""); print; next } NR > 1 { exit }' "$0"
        exit 0
        ;;
    *) unknown="$unknown $arg" ;;
    esac
done
if [ -n "$unknown" ]; then
    echo "未知参数：$unknown（用 --help 看用法）" >&2
    exit 2
fi

have() { command -v "$1" >/dev/null 2>&1; }

# ---- 1. 环境检测 ----
missing_required=''
echo "== 环境检测 =="
check_tool() { # 名字 命令 是否必需
    if have "$2"; then
        printf '  %-8s %s\n' "$1" "$(command -v "$2")"
        return 0
    fi
    if [ "$3" = "required" ]; then
        printf '  %-8s 缺失（**必需**）\n' "$1"
        missing_required="$missing_required $1"
    else
        printf '  %-8s 缺失（相关部分跳过）\n' "$1"
    fi
    return 1
}

check_tool g++ g++ required
check_tool gcc gcc required
# GNU make 在 Windows 上通常只以 mingw32-make 的名字提供（MSYS 自带的才叫 make），
# 两个名字都认。真名不是 make 时，往 build/.tools/ 放一个同名垫片挂进 PATH，
# 这样 tools/lock.sh、各工程 Makefile 里写死的 `make` 也能落到同一个程序上。
make_command=''
for candidate in make mingw32-make gmake; do
    if have "$candidate"; then
        make_command=$candidate
        break
    fi
done
check_tool 'GNU make' "$make_command" required
if [ -n "$make_command" ] && [ "$make_command" != make ]; then
    make_shim_dir="$root/build/.tools"
    mkdir -p "$make_shim_dir" || exit 1
    printf '#!/bin/sh\nexec %s "$@"\n' "$make_command" >"$make_shim_dir/make" || exit 1
    chmod +x "$make_shim_dir/make" 2>/dev/null || true
    PATH="$make_shim_dir:$PATH"
    export PATH
fi
# 实际运行解释器，跳过 Windows 商店占位程序和 Python 2。
python_command=''
for candidate in python3 python; do
    if have "$candidate" && "$candidate" -c 'import sys; sys.exit(0 if sys.version_info.major == 3 else 1)' >/dev/null 2>&1; then
        python_command=$candidate
        break
    fi
done
check_tool 'Python 3' "$python_command" required
PYTHON=$python_command
export PYTHON
check_tool cargo cargo optional
check_tool rustc rustc optional
check_tool dotnet dotnet optional
check_tool go go optional

if [ -n "$missing_required" ]; then
    echo
    echo "缺少必需工具：$missing_required —— 装好再跑。" >&2
    exit 1
fi

if ! have cargo || ! have rustc; then
    build_rust=0
fi

# 生成器能出哪些语言档由工具链决定（C / C++17 / Python 一定有）
langs='c,cpp17,python'
[ "$build_rust" = 1 ] && langs="$langs,rust"
have dotnet && langs="$langs,csharp"
have go && langs="$langs,go"

# ---- 2. 构建清单 ----
cpp_libs='allocator big_number json toml lsp preprocess simple_ast stackvm template transfer lztext'
rust_libs='big_number-rust json-rust preprocess-rust simple_ast-rust stackvm-rust template-rust transfer-rust'
projects='bigint-calc log-ir mini-basic pl0-ir pl0-super protocol-dsl todo-scan'

echo
echo "== 将要构建（只编译，不跑测试）=="
echo "  生成器     build/bin/ParseEngine${generator_target:+（release）}"
echo "  C++ 库     $cpp_libs"
echo "  独立工程   （Magel、MetroLang、Hydra86、RustView、CAS 已独立：Magel 见 https://github.com/rabbitc999/magel，MetroLang 见 https://gitee.com/rabbitc999/metro-lang，RustView 见 https://gitee.com/rabbitc999/rust-view）"
if [ "$build_rust" = 1 ]; then
    echo "  Rust crate $rust_libs"
else
    echo "  Rust crate （跳过：缺 cargo / rustc）"
fi
if [ "$build_examples" = 1 ]; then
    echo "  example/   全量产物（语言档：$langs）"
else
    echo "  example/   （跳过）"
fi
if [ "$build_projects" = 1 ]; then
    echo "  project/   $projects"
else
    echo "  project/   （跳过）"
fi

if [ "$list_only" = 1 ]; then
    echo
    echo "（--list：只看清单，没有真的构建）"
    exit 0
fi

# ---- 3. 逐步构建 ----
log_dir='build/build-all'
mkdir -p "$log_dir/logs" 2>/dev/null
failed=''
step=0

case "$(uname -s 2>/dev/null || echo unknown)" in
MINGW* | MSYS* | CYGWIN*) host_platform='windows-mingw' ;;
Linux*)
    if [ -n "${WSL_DISTRO_NAME:-}" ]; then host_platform='linux-wsl'; else host_platform='linux'; fi
    ;;
Darwin*) host_platform='macos' ;;
*) host_platform='unknown' ;;
esac

generator_stamp='build/make/.build-all-platform'
library_stamp='build/.build-all-library-platform'
generator_platform_changed=0
library_platform_changed=0
[ -f "$generator_stamp" ] && [ "$(cat "$generator_stamp" 2>/dev/null)" = "$host_platform" ] || generator_platform_changed=1
[ -f "$library_stamp" ] && [ "$(cat "$library_stamp" 2>/dev/null)" = "$host_platform" ] || library_platform_changed=1

run_step() { # 标题 命令...
    title=$1
    shift
    step=$((step + 1))
    log="$log_dir/logs/$(printf '%02d' "$step").log"
    started=$(date +%s)
    if [ "$quiet" = 1 ]; then
        # 紧凑模式：命令输出收进日志，终端只留一行标题 + 结果。
        printf '\n[%2d] %s ... ' "$step" "$title"
        if "$@" >"$log" 2>&1; then ok=0; else ok=1; fi
    else
        # 缺省：完整输出直接打到终端（经 tee 同时留档）。
        # POSIX sh 没有 PIPESTATUS，命令的退出码先写进临时文件再读出来。
        printf '\n[%2d] %s\n' "$step" "$title"
        status_file="$log_dir/logs/.status"
        { "$@" 2>&1; printf '%s' "$?" >"$status_file"; } | tee "$log"
        ok=$(cat "$status_file" 2>/dev/null || echo 1)
    fi
    elapsed=$(($(date +%s) - started))
    if [ "$ok" = 0 ]; then
        if [ "$quiet" = 1 ]; then
            echo "ok（${elapsed}s）"
        else
            printf '     -> ok（%ss）\n' "$elapsed"
        fi
        return 0
    fi
    if [ "$quiet" = 1 ]; then
        echo "失败（${elapsed}s）"
    else
        printf '     -> 失败（%ss）\n' "$elapsed"
    fi
    echo "     完整输出：$log"
    failed="$failed
  - $title"
    return 1
}

# Git Bash 中的 GNU Make 仍会继承 Windows_NT，于是部分 Makefile 选择 `cmd /c`
# 分支，再被 sh 二次解释，`mkdir` 实际没有执行。命令行覆盖 OS 让这些构建统一
# 使用已经可用的 POSIX shell；Linux/macOS 下同样安全。
#
# 覆盖 SHELL 时名字要分平台：原生 mingw32-make 把 SHELL 当程序名解析，传不带
# `.exe` 的 `sh` 会以 0xc0000005 直接崩溃（MSYS 自带的 make 没有这个问题），
# 所以 Windows 上给 `sh.exe`。
case "$host_platform" in
windows-mingw) make_shell='sh.exe' ;;
*) make_shell='sh' ;;
esac

run_make() {
    make SHELL="$make_shell" OS= "$@"
}

build_generator() {
    target=$1
    if [ "$generator_platform_changed" = 1 ]; then
        echo
        echo "     检测到构建平台切换，清理旧生成器对象（当前：$host_platform）"
        sh tools/lock.sh acquire "build-all 平台切换：$host_platform" || return 1
        if ! PE_LOCK_HELD=1 make -C source clean; then
            sh tools/lock.sh release
            return 1
        fi
        if [ -n "$target" ]; then
            LOCK_REUSE=1 sh tools/lock.sh build "$target" || return 1
        else
            LOCK_REUSE=1 sh tools/lock.sh build || return 1
        fi
        sh tools/lock.sh release || return 1
        mkdir -p "$(dirname "$generator_stamp")"
        printf '%s\n' "$host_platform" >"$generator_stamp"
        return 0
    fi
    if [ -n "$target" ]; then
        sh tools/lock.sh build "$target"
    else
        sh tools/lock.sh build
    fi
}

# 3.1 生成器（走共享锁：编译时持锁、绿了自动放锁）
if [ -n "$generator_target" ]; then
    if run_step "生成器（release）" build_generator release; then generator_ok=1; else generator_ok=0; fi
else
    if run_step "生成器" build_generator ''; then generator_ok=1; else generator_ok=0; fi
fi

generator_dir="$root/build/bin"
[ -n "$generator_target" ] && generator_dir="$generator_dir/release"
generator_executable="$generator_dir/ParseEngine"
[ -x "$generator_executable.exe" ] && generator_executable="$generator_executable.exe"

# 3.2 C++ 库（每个库编自己的 lib*.a）
for name in $cpp_libs; do
    if [ "$library_platform_changed" = 1 ]; then
        run_make -C "library/$name" clean >/dev/null 2>&1 || true
    fi
    run_step "库 library/$name" run_make -C "library/$name"
done
mkdir -p "$(dirname "$library_stamp")"
printf '%s\n' "$host_platform" >"$library_stamp"

# 3.2a MetroLang、Magel、Hydra86、RustView 与 CAS 一样拆成了独立仓库（分别见
# https://gitee.com/rabbitc999/metro-lang、https://github.com/rabbitc999/magel、
# https://gitee.com/rabbitc999/rust-view；Hydra86 与 CAS 的仓库地址不在本仓库提供），
# 它们的构建与门禁都在各自仓库里；
# 父仓库不再构建任何独立工程。


# 3.3 Rust crate
if [ "$build_rust" = 1 ]; then
    for name in $rust_libs; do
        run_step "crate library/$name" cargo build --manifest-path "library/$name/Cargo.toml" --target-dir "build/rust/$name"
    done
fi

# 3.4 example/ 全量产物（生成器已经在 3.1 里出好了，这一步只生成、不编译）
if [ "$build_examples" = 1 ] && [ "$generator_ok" = 1 ]; then
    GEN_ALL_LANGS="$langs" run_step "示例产物 example/（$langs）" \
        sh tools/gen_all.sh "$generator_executable" "$log_dir/examples"
fi

# 3.5 project/ 下的示例工程。
if [ "$build_projects" = 1 ] && [ "$generator_ok" = 1 ]; then
    for name in $projects; do
        run_step "工程 project/$name" run_make -C "project/$name" \
            "ENGINE=$generator_executable" "GEN_DIR=../../build/project/$name" \
            "BUILD_DIR=../../build/project/$name" "QUAD_DIR=../../build/project/$name/quads"
    done
fi

# ---- 4. 汇总 ----
echo
echo "== 汇总 =="
if [ -z "$failed" ]; then
    echo "  步骤：$step，全部成功"
    echo "  本脚本只构建、不跑测试（要跑测试用 make -C source test-smoke / 各工程的 make check）"
    exit 0
fi
fail_count=$(printf '%s\n' "$failed" | grep -c '^  - ')
echo "  步骤：$step，成功 $((step - fail_count))，失败 $fail_count"
echo "  失败：$failed"
echo "  提示：完整输出在 $log_dir/logs/ 下；也可以单独重跑那一步"
exit 1
