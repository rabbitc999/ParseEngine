#!/bin/sh
# project/bigint-calc/check.sh —— 跑全部样例：
#   1) samples/*.expr        必须成功退出，且 stdout 与 expected/*.txt 逐行一致；
#   2) samples-error/*.expr  必须报错退出（非 0），且 stderr 首行与
#                            expected-error/*.txt 一致。
#
# 用法：sh check.sh          （Makefile / local.mk 的 check 目标就是这么调的）
#
# 大数结果就是真值：期望文件是把驱动的输出固化下来的（已人工核对 + 与 Python
# 的大整数结果对过），改动语义动作 / 大数库 / 优先级声明都会立刻在这里报出来。
#
# 期望文件在 Windows 上检出时可能是 CRLF（仓库 core.autocrlf），比对前两边都
# 去掉行尾的 \r，这样同一份期望文件在 Windows / Linux 都可用。

set -u

# 让脚本在任何工作目录下都能跑：路径都以脚本自己所在目录为准。
cd "$(dirname "$0")" || exit 2

DEMO=${DEMO:-build/parser_demo}
SAMPLES=${SAMPLES:-samples}
EXPECTED=${EXPECTED:-expected}
ERROR_SAMPLES=${ERROR_SAMPLES:-samples-error}
EXPECTED_ERROR=${EXPECTED_ERROR:-expected-error}

# Git Bash / MSYS 下 `build/parser_demo` 会解析到 `build/parser_demo.exe`；
# 这里再兜一次，免得平台差异把整个自检挡掉。
if [ ! -x "$DEMO" ] && [ -x "$DEMO.exe" ]; then
    DEMO="$DEMO.exe"
fi

if [ ! -x "$DEMO" ]; then
    echo "找不到 $DEMO；先跑 make（生成 + 编译）" >&2
    exit 2
fi

fail=0
count=0

normalize() {
    sed 's/\r$//' "$1" > "$2"
}

# ---- 正常样例：stdout 与期望逐行一致 ----
for src in "$SAMPLES"/*.expr; do
    name=$(basename "$src" .expr)
    expected="$EXPECTED/$name.txt"
    actual=$(mktemp) || exit 2
    count=$((count + 1))
    if [ ! -f "$expected" ]; then
        echo "[FAIL] $name：缺少期望文件 $expected"
        fail=1
        rm -f "$actual"
        continue
    fi
    if ! "$DEMO" "$src" 2>"$actual.err" | sed 's/\r$//' >"$actual"; then
        echo "[FAIL] $name：求值失败"
        sed 's/^/       /' "$actual.err"
        fail=1
        rm -f "$actual" "$actual.err"
        continue
    fi
    normalize "$expected" "$actual.exp"
    if diff -u "$actual.exp" "$actual" >"$actual.diff" 2>&1; then
        echo "[ok]   $name（$(wc -c <"$actual.exp" | tr -d ' ') 字节）"
    else
        echo "[FAIL] $name：结果与期望不一致"
        sed 's/^/       /' "$actual.diff"
        fail=1
    fi
    rm -f "$actual" "$actual.exp" "$actual.err" "$actual.diff"
done

if [ "$fail" -eq 0 ]; then
    echo "[ok]   $count 个正常样例"
fi

# ---- 错误样例：必须报错退出，且首行文案与期望一致 ----
errorCount=0
for src in "$ERROR_SAMPLES"/*.expr; do
    if [ ! -f "$src" ]; then
        break
    fi
    name=$(basename "$src" .expr)
    expected="$EXPECTED_ERROR/$name.txt"
    actual=$(mktemp) || exit 2
    errorCount=$((errorCount + 1))
    if [ ! -f "$expected" ]; then
        echo "[FAIL] $name：缺少期望文件 $expected"
        fail=1
        rm -f "$actual"
        continue
    fi
    if "$DEMO" "$src" >/dev/null 2>"$actual"; then
        echo "[FAIL] $name：本应报错退出，却成功返回"
        fail=1
        rm -f "$actual"
        continue
    fi
    sed -e 's/\r$//' -e '1!d' "$actual" >"$actual.first"
    normalize "$expected" "$actual.exp"
    if diff -u "$actual.exp" "$actual.first" >"$actual.diff" 2>&1; then
        echo "[ok]   $name（报错：$(head -n 1 "$actual.exp")）"
    else
        echo "[FAIL] $name：报错文案与期望不一致"
        sed 's/^/       /' "$actual.diff"
        fail=1
    fi
    rm -f "$actual" "$actual.exp" "$actual.first" "$actual.diff"
done

if [ "$fail" -eq 0 ]; then
    echo "全部通过（$count 个正常样例 + $errorCount 个错误样例）"
fi
exit "$fail"
