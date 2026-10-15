#!/bin/sh
# project/todo-scan/check.sh —— 跑全部用例：
#   1) 正常用例：命令跑成功，stdout 与 expected/<用例>.md 逐行一致；
#   2) 错误用例：命令非 0 退出，stderr 首行与 expected-error/<用例>.txt 一致。
#
# 用法：sh check.sh          （Makefile 的 check 目标就是这么调的）
#
# 报告的「文件」一列用的是命令行里给的文件名，所以用例固定用 samples/… 这种
# 相对路径（make -C 会在本目录下执行）。期望文件是 UTF-8：%main 把 stdout 固定
# 成 UTF-8，不受 Windows 控制台代码页影响；比对前两边都去掉行尾 \r。

set -u

PYTHON=${PYTHON:-python}
MAIN=${MAIN:-build/main.py}

if [ ! -f "$MAIN" ]; then
    echo "找不到 $MAIN；先跑 make（或 make generate）" >&2
    exit 2
fi

fail=0
ok=0

normalize() {
    sed 's/\r$//' "$1" > "$2"
}

# 正常用例：check_ok <用例名> <命令行参数…>
check_ok() {
    name=$1
    shift
    expected="expected/$name.md"
    actual=$(mktemp) || exit 2
    if [ ! -f "$expected" ]; then
        echo "[FAIL] $name：缺少期望文件 $expected"
        fail=1
        rm -f "$actual"
        return
    fi
    if ! "$PYTHON" "$MAIN" "$@" 2>"$actual.err" | sed 's/\r$//' >"$actual"; then
        echo "[FAIL] $name：运行失败"
        sed 's/^/       /' "$actual.err"
        fail=1
        rm -f "$actual" "$actual.err"
        return
    fi
    normalize "$expected" "$actual.exp"
    if diff -u "$actual.exp" "$actual" >"$actual.diff" 2>&1; then
        echo "[ok]   $name（$*）"
        ok=$((ok + 1))
    else
        echo "[FAIL] $name：报告与期望不一致"
        sed 's/^/       /' "$actual.diff"
        fail=1
    fi
    rm -f "$actual" "$actual.exp" "$actual.err" "$actual.diff"
}

# 错误用例：check_err <用例名> <命令行参数…>
check_err() {
    name=$1
    shift
    expected="expected-error/$name.txt"
    actual=$(mktemp) || exit 2
    if [ ! -f "$expected" ]; then
        echo "[FAIL] $name：缺少期望文件 $expected"
        fail=1
        rm -f "$actual"
        return
    fi
    if "$PYTHON" "$MAIN" "$@" >/dev/null 2>"$actual"; then
        echo "[FAIL] $name：本应报错退出，却成功返回"
        fail=1
        rm -f "$actual"
        return
    fi
    sed -e 's/\r$//' -e '1!d' "$actual" >"$actual.first"
    normalize "$expected" "$actual.exp"
    if diff -u "$actual.exp" "$actual.first" >"$actual.diff" 2>&1; then
        echo "[ok]   $name（报错：$(head -n 1 "$actual.exp")）"
        ok=$((ok + 1))
    else
        echo "[FAIL] $name：报错文案与期望不一致"
        sed 's/^/       /' "$actual.diff"
        fail=1
    fi
    rm -f "$actual" "$actual.exp" "$actual.first" "$actual.diff"
}

# ---- 用例表（要加用例就加一行 + 存一份期望文件）----
check_ok  all    samples/app.js samples/util.py
check_ok  single samples/app.js
check_ok  clean  samples/clean.txt
check_err missing samples/nope.js
check_err usage

if [ "$fail" -eq 0 ]; then
    echo "全部通过（$ok 个用例）"
fi
exit "$fail"
