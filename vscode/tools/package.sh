#!/bin/sh
# 打包 VS Code 插件（.vsix）：转发到 tools/package.js（POSIX 入口）。
#
# 逻辑只有一份，在 tools/package.js；这里只是给不习惯 npm 的人一个快捷方式，
# 与 `npm run package -- <参数>` 完全等价。
#
# 用法：
#   sh vscode/tools/package.sh
#   sh vscode/tools/package.sh --skip-tests
#   sh vscode/tools/package.sh --list
set -e

exec node "$(dirname "$0")/package.js" "$@"
