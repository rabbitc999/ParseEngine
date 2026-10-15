#!/bin/sh
# project/log-ir/check.sh —— 跑全部样例：
#   1) samples/*.log        必须成功退出，且 stdout 与 expected/*.json 一致；
#   2) samples-error/*.log  必须报错退出（非 0），且 stderr 首行与
#                           expected-error/*.txt 一致（报错文案 + 行列号）。
#
# 用法：sh check.sh          （Makefile 的 check 目标就是这么调的）
#
# 产物是 Python，不需要编译；PYTHON 可以用环境变量覆盖（默认 python）。
# 期望文件是 UTF-8（%main 把 stdout 固定成 UTF-8，不受控制台代码页影响），
# 比对前两边都去掉行尾的 \r，这样 Windows / Linux 共用同一份期望文件。

set -u

PYTHON=${PYTHON:-python}
MAIN=${MAIN:-build/main.py}
SAMPLES=${SAMPLES:-samples}
EXPECTED=${EXPECTED:-expected}
ERROR_SAMPLES=${ERROR_SAMPLES:-samples-error}
EXPECTED_ERROR=${EXPECTED_ERROR:-expected-error}

if [ ! -f "$MAIN" ]; then
    echo "找不到 $MAIN；先跑 make（或 make generate）" >&2
    exit 2
fi

fail=0
count=0

normalize() {
    sed 's/\r$//' "$1" > "$2"
}

# ---- 正常样例：stdout 与期望逐行一致 ----
for src in "$SAMPLES"/*.log; do
    name=$(basename "$src" .log)
    expected="$EXPECTED/$name.json"
    actual=$(mktemp) || exit 2
    count=$((count + 1))
    if [ ! -f "$expected" ]; then
        echo "[FAIL] $name：缺少期望文件 $expected"
        fail=1
        rm -f "$actual"
        continue
    fi
    if ! "$PYTHON" "$MAIN" "$src" 2>"$actual.err" | sed 's/\r$//' >"$actual"; then
        echo "[FAIL] $name：解析失败"
        sed 's/^/       /' "$actual.err"
        fail=1
        rm -f "$actual" "$actual.err"
        continue
    fi
    normalize "$expected" "$actual.exp"
    if diff -u "$actual.exp" "$actual" >"$actual.diff" 2>&1; then
        echo "[ok]   $name"
    else
        echo "[FAIL] $name：JSON 与期望不一致"
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
for src in "$ERROR_SAMPLES"/*.log; do
    if [ ! -f "$src" ]; then
        break
    fi
    name=$(basename "$src" .log)
    expected="$EXPECTED_ERROR/$name.txt"
    actual=$(mktemp) || exit 2
    errorCount=$((errorCount + 1))
    if [ ! -f "$expected" ]; then
        echo "[FAIL] $name：缺少期望文件 $expected"
        fail=1
        rm -f "$actual"
        continue
    fi
    if "$PYTHON" "$MAIN" "$src" >/dev/null 2>"$actual"; then
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
