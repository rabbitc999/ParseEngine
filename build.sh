#!/bin/sh
# 根目录构建入口；所有产物统一放在根目录 build/；实际步骤由 build-all.sh 维护。
root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd) || exit 2
exec sh "$root/build-all.sh" "$@"
