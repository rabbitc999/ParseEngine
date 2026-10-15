#!/bin/sh
# project/check.sh —— 跑全部样例：
#   1) samples/*.pl0        必须解析成功，三元式与 expected/*.triples 逐行一致；
#   2) samples-error/*.pl0  必须报错退出（非 0），且 stderr 首行与
#                           expected-error/*.txt 一致（错误文案 + 行列号）。
#
# 用法：sh check.sh          （Makefile 的 check 目标就是这么调的）
#
# 期望文件是用「已验证正确」的输出固化下来的（见 README「样例与期望输出」），
# 一旦三元式生成改动，这里会立刻报出是哪个样例、哪一行不同。
#
# 期望文件在 Windows 上检出时可能是 CRLF（仓库 core.autocrlf=true），比对前
# 两边都去掉行尾的 \r，这样同一份期望文件在 Windows / Linux 都可用。

set -u

DEMO=${DEMO:-build/parser_demo}
SAMPLES=${SAMPLES:-samples}
EXPECTED=${EXPECTED:-expected}
ERROR_SAMPLES=${ERROR_SAMPLES:-samples-error}
EXPECTED_ERROR=${EXPECTED_ERROR:-expected-error}

if [ ! -x "$DEMO" ]; then
    echo "找不到 $DEMO；先跑 make（或 make build）" >&2
    exit 2
fi

fail=0
count=0

# 去掉行尾 \r 后写到第二个参数，供 diff 使用
normalize() {
    sed 's/\r$//' "$1" > "$2"
}

for src in "$SAMPLES"/*.pl0; do
    name=$(basename "$src" .pl0)
    expected="$EXPECTED/$name.triples"
    actual=$(mktemp) || exit 2
    count=$((count + 1))
    if [ ! -f "$expected" ]; then
        echo "[FAIL] $name：缺少期望文件 $expected"
        fail=1
        rm -f "$actual"
        continue
    fi
    # 产物在 Windows 上按文本模式写 stdout（CRLF），比对前统一去掉行尾的 \r
    if ! "$DEMO" "$src" 2>"$actual.err" | sed 's/\r$//' >"$actual"; then
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
        echo "[FAIL] $name：三元式与期望不一致"
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
for src in "$ERROR_SAMPLES"/*.pl0; do
    if [ ! -f "$src" ]; then
        break
    fi
    name=$(basename "$src" .pl0)
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
