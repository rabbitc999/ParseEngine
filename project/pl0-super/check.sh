#!/bin/sh
# project/pl0-super/check.sh —— 正常样例比输出，错误样例比报错首行。
#
# 用法：make check（先 make 出 build/parser_demo）
#   samples/*.p0s        正常样例，输出与 expected/<名字>.out 逐字比
#   samples-error/*.p0s  出错样例，报错首行与 expected-error/<名字>.txt 比
set -u

DEMO=${DEMO:-build/pl0-super}
STATUS=0

for src in samples/*.p0s; do
    name=$(basename "$src" .p0s)
    want="expected/$name.out"
    if [ ! -f "$want" ]; then
        echo "[跳过] $name（没有 $want）"
        continue
    fi
    got=$("$DEMO" "$src" 2>&1 | tr -d '\r')
    if [ "$got" = "$(cat "$want" | tr -d '\r')" ]; then
        echo "[通过] $name"
    else
        echo "[失败] $name"
        echo "  期望：$(cat "$want" | tr -d '\r' | tr '\n' ' ')"
        echo "  实际：$(printf '%s' "$got" | tr '\n' ' ')"
        STATUS=1
    fi
done

for src in samples-error/*.p0s; do
    name=$(basename "$src" .p0s)
    want="expected-error/$name.txt"
    if [ ! -f "$want" ]; then
        echo "[跳过] $name（没有 $want）"
        continue
    fi
    got=$("$DEMO" "$src" 2>&1 | head -n 1 | tr -d '\r')
    if [ "$got" = "$(head -n 1 "$want" | tr -d '\r')" ]; then
        echo "[通过] $name（报错）"
    else
        echo "[失败] $name（报错）"
        echo "  期望首行：$(head -n 1 "$want")"
        echo "  实际首行：$got"
        STATUS=1
    fi
done

if [ "$STATUS" -eq 0 ]; then
    echo "pl0-super：全部样例通过"
fi
exit $STATUS
