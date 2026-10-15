<#
    打包 VS Code 插件（.vsix）。

    实际逻辑在 tools/package.js：这个脚本只负责找到 node 并转发参数，
    这样 PowerShell / CMD / bash 三种入口共用同一份实现。

    用法：
        powershell -File vscode/tools/package.ps1
        powershell -File vscode/tools/package.ps1 -SkipTests
        powershell -File vscode/tools/package.ps1 -List
        powershell -File vscode/tools/package.ps1 -Out D:\pkg
#>

[CmdletBinding()]
param(
    # 跳过打包前的冒烟测试。
    [switch]$SkipTests,
    # 只列出会被打包的文件，不生成 .vsix。
    [switch]$List,
    # 输出目录，默认 vscode/dist。
    [string]$Out
)

$ErrorActionPreference = 'Stop'

# 1. 找 node（插件与打包脚本都要求 Node.js 18+）。
$node = Get-Command node -ErrorAction SilentlyContinue
if (-not $node) {
    Write-Error '找不到 node，请先安装 Node.js 18+。'
    exit 1
}

# 2. 拼参数转发给 package.js。
$arguments = @()
if ($SkipTests) { $arguments += '--skip-tests' }
if ($List) { $arguments += '--list' }
if ($Out) { $arguments += @('--out', $Out) }

$script = Join-Path $PSScriptRoot 'package.js'
& $node.Source $script @arguments
exit $LASTEXITCODE
