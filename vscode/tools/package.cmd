@echo off
REM Package the VS Code extension into a .vsix (CMD entry point).
REM
REM This file forwards every argument to tools\package.js, which does the real
REM work. Keep it ASCII-only with CRLF line endings: cmd.exe parses batch files
REM line by line, and non-ASCII text with LF endings can break that parsing.
REM
REM Usage:
REM   vscode\tools\package.cmd
REM   vscode\tools\package.cmd --skip-tests
REM   vscode\tools\package.cmd --list
REM   vscode\tools\package.cmd --out D:\pkg

setlocal

where node >nul 2>nul
if errorlevel 1 goto :no_node

node "%~dp0package.js" %*
exit /b %ERRORLEVEL%

:no_node
echo [GramEditor] node not found. Install Node.js 18+ first.
exit /b 1
