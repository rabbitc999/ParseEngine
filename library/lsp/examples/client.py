#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""最小的 LSP 客户端：用真实的 Content-Length 帧驱动 lsp-server。

用法：
    python examples/client.py <lsp-server 可执行文件> <gram 文件>

它会依次发 initialize / initialized / didOpen，打印诊断数量与首条诊断，
再发 hover / completion / definition / documentSymbol / semanticTokens /
formatting / codeAction 并打印摘要，最后 shutdown / exit。

返回码：0 正常；1 参数或协议问题；2 干净示例里出现了 error 级诊断。
"""
import json
import os
import subprocess
import sys


def send(proc, payload):
    body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
    proc.stdin.write(b"Content-Length: %d\r\n\r\n" % len(body) + body)
    proc.stdin.flush()


def receive(proc):
    length = None
    while True:
        line = proc.stdout.readline()
        if not line:
            return None
        line = line.decode("utf-8", "replace").strip()
        if not line:
            break
        if line.lower().startswith("content-length:"):
            length = int(line.split(":", 1)[1].strip())
    if length is None:
        return None
    return json.loads(proc.stdout.read(length).decode("utf-8"))


def request(proc, ident, method, params=None):
    send(proc, {"jsonrpc": "2.0", "id": ident, "method": method, "params": params or {}})
    while True:
        message = receive(proc)
        if message is None:
            raise RuntimeError("服务器提前关闭")
        if message.get("id") == ident:
            return message


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 1
    expect_errors = "--expect-errors" in sys.argv[3:]
    server, path = sys.argv[1], sys.argv[2]
    if not os.path.exists(server) and os.path.exists(server + ".exe"):
        server += ".exe"  # Windows 上 MinGW 会补 .exe
    if not os.path.exists(server):
        print("找不到 lsp-server：%s（先跑 make）" % server)
        return 1
    text = open(path, encoding="utf-8").read()
    uri = "file:///" + os.path.abspath(path).replace("\\", "/")
    proc = subprocess.Popen([server, "--stdio"], stdin=subprocess.PIPE,
                            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    try:
        init = request(proc, 1, "initialize", {"capabilities": {}})
        caps = init["result"]["capabilities"]
        print("initialize：服务器 %s，能力 %d 项"
              % (init["result"]["serverInfo"]["name"], len(caps)))
        send(proc, {"jsonrpc": "2.0", "method": "initialized", "params": {}})
        send(proc, {"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
            "textDocument": {"uri": uri, "languageId": "gram", "version": 1, "text": text}}})

        diagnostics = None
        while diagnostics is None:
            message = receive(proc)
            if message is None:
                raise RuntimeError("没有收到 publishDiagnostics")
            if message.get("method") == "textDocument/publishDiagnostics":
                diagnostics = message["params"]["diagnostics"]
        errors = [d for d in diagnostics if d.get("severity") == 1]
        warnings = [d for d in diagnostics if d.get("severity") == 2]
        print("诊断：%d 个 error，%d 个 warning" % (len(errors), len(warnings)))
        for diagnostic in diagnostics[:5]:
            start = diagnostic["range"]["start"]
            level = "error" if diagnostic.get("severity") == 1 else "warning"
            print("  %s:%d:%d %s %s"
                  % (os.path.basename(path), start["line"] + 1, start["character"] + 1,
                     level, diagnostic["message"]))

        # 找一条带 %token/%define 的行与一条规则行，用来演示悬停与跳转。
        lines = text.splitlines()
        token_line = next((i for i, line in enumerate(lines) if line.startswith("%define")),
                          len(lines) - 1)
        rule_line = next((i for i, line in enumerate(lines)
                          if ":=" in line and not line.lstrip().startswith("%")),
                         len(lines) - 1)
        position = {"line": token_line, "character": 10}

        hover = request(proc, 2, "textDocument/hover",
                        {"textDocument": {"uri": uri}, "position": position})
        markdown = ""
        if hover.get("result"):
            markdown = hover["result"]["contents"]["value"].splitlines()[0]
        print("hover：%s" % (markdown or "（该位置没有说明）"))

        completion = request(proc, 3, "textDocument/completion",
                             {"textDocument": {"uri": uri}, "position": position})
        items = completion["result"]["items"]
        print("completion：%d 个候选（例如 %s）"
              % (len(items), "、".join(item["label"] for item in items[:5])))

        definition = request(proc, 4, "textDocument/definition",
                             {"textDocument": {"uri": uri},
                              "position": {"line": rule_line, "character": 0}})
        print("definition：%d 个目标" % len(definition["result"]))

        symbols = request(proc, 5, "textDocument/documentSymbol",
                          {"textDocument": {"uri": uri}})
        print("documentSymbol：%d 个符号" % len(symbols["result"]))

        tokens = request(proc, 6, "textDocument/semanticTokens/full",
                         {"textDocument": {"uri": uri}})
        print("semanticTokens：%d 个整数（每 5 个一个 token）"
              % len(tokens["result"]["data"]))

        formatting = request(proc, 7, "textDocument/formatting",
                             {"textDocument": {"uri": uri},
                              "options": {"tabSize": 4, "insertSpaces": True}})
        print("formatting：%d 处编辑" % len(formatting["result"]))

        actions = request(proc, 8, "textDocument/codeAction",
                          {"textDocument": {"uri": uri}, "range": {
                              "start": {"line": 0, "character": 0},
                              "end": {"line": 0, "character": 0}},
                           "context": {"diagnostics": diagnostics}})
        print("codeAction：%d 个快速修复%s"
              % (len(actions["result"]),
                 ("（例如 %s）" % actions["result"][0]["title"] if actions["result"] else "")))

        request(proc, 9, "shutdown")
        send(proc, {"jsonrpc": "2.0", "method": "exit"})
        proc.wait(timeout=10)
        if expect_errors:
            # 坏例子：本来就该报 error；一个都没有才算失败。
            return 0 if errors else 3
        return 2 if errors else 0
    finally:
        if proc.poll() is None:
            proc.kill()


if __name__ == "__main__":
    sys.exit(main())
