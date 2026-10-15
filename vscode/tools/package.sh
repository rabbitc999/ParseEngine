#!/bin/sh
# 打包 VS Code 插件（.vsix）：转发到 tools/package.js（POSIX 入口）。
#
# 用法：
#   sh vscode/tools/package.sh
#   sh vscode/tools/package.sh --skip-tests
#   sh vscode/tools/package.sh --list
set -e

exec node "$(dirname "$0")/package.js" "$@"
