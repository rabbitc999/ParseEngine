# -*- coding: utf-8 -*-
"""把 C++ 版（library/json）与 Rust 版（library/json-rust）的输出逐条比对。

用法（仓库根）：

    python library/json-rust/tools/diff_with_cpp.py                 # 全跑
    python library/json-rust/tools/diff_with_cpp.py 语料            # 只跑 91 条语料
    python library/json-rust/tools/diff_with_cpp.py 命令            # 只跑命令行矩阵

前提：C++ 版已经编过（`make -C library/json`，产物 build/json/json.exe），
Rust 版已经编过（`cargo build --manifest-path library/json-rust/Cargo.toml`）。

比的是什么：

  1. **语料**：91 条 JSON 文本（标量 / 数字 / 转义 / 容器 / 注释 / 各种坏输入），
     逐个喂给两版的 `json fmt --compact --comments --trailing-commas`，比对
     标准输出、标准错误与退出码；
  2. **命令行矩阵**：README 里写到的每条命令（fmt / check / get / keys / stats、
     选项、用法错误、路径不存在），比对同一套东西。

已知且**有意**的差异（脚本会把它们单独列出来，不当失败）：

  * 浮点写法：**根在 C++ 那一侧的实现**——`formatDouble` 从 17 位有效数字起
    逐级缩短，而 17 位对 `f64` 必定能往返，于是循环第一轮就退出、永远印 17 位
    （`0.1` → `0.10000000000000001`）；Rust 用标准库的最短往返写法（`0.1` → `0.1`）。
    两串是同一个 `f64`，互相解析都不变。
  * 落单代理：**Rust 字符串的固有约束**——`"\\udc00"` 这种输入 C++ 会写出非法
    UTF-8 字节，Rust 的 `String` 装不下，直接报 `bad-surrogate`。
  * 用法行里的程序名：`用法：<argv[0]> …` 本来就各不相同。

**不算差异**的：Windows 上 C++ 的 stdout 是文本模式（CRLF）、Rust 一律 LF ——
比对前先把 CRLF 归一（`--out` 写文件时两边本来就都是 LF）。
"""

import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[3]
CPP = ROOT / "build/json/json.exe"
RUST = ROOT / "build/json/rust/debug/json-cli.exe"
RUST_DEBUG = ROOT / "build/json/rust/debug/json-cli"
CONFIG = ROOT / "library/json/examples/config.json"
WORK = ROOT / "tmp/json-rust-diff"

# 一条一行；<NL> / <BOM> 由脚本展开（别交给 shell 的 printf —— 它会把 \uXXXX
# 变成真字节，语料就走样了）。
CORPUS = [
    # 标量
    "null", "true", "false", "0", "-0", "1", "-1", "42",
    "9007199254740993", "-9223372036854775808", "123456789012345678901234567890",
    # 数字
    "1.5", "0.1", "2.0", "-0.0", "1e3", "1E3", "1e+3", "1e-3", "0.0001",
    "1e300", "1e-300", "3.141592653589793", "1.7976931348623157e308", "0.5", "1000000.0",
    # 字符串与转义（反斜杠都是 JSON 转义本身，要原样喂给解析器）
    '""', '"hi"', r'"\u4e2d\u6587"', r'"\ud83d\ude00"', r'"\"\\\/\b\f\n\r\t"',
    '"中"', r'"a\u0000b"', r'"\u0041\u00e9"', r'"\u00ff"', r'"tab\there"',
    # 容器与顺序
    "[]", "{}", "[1]", "[[]]", "[{}]", '{"a":[]}', '{"a":{}}',
    '{"b":1,"a":2,"c":3}', '{"a":[1,2,{"b":[true,null]}]}', "[[[[[1]]]]]",
    '{"nested":{"deep":{"deeper":[1,2,3]}}}', '{  "a" : 1 , "b" : [ 1 , 2 ]  }',
    "[1,2,3]", '{"":1}', '{"dup":1,"dup":2}', '[true,false,null,"x",1,2.5,[],{}]',
    '<BOM>{"bom":1}', '<NL>{"a":1}', '{"a":1}<NL>',
    # 注释与尾逗号（比对时带 --comments --trailing-commas）
    "// c<NL>{}", '{"a":[1,2,],/* block */}', "[1,2,]<NL>// end",
    # 错误样例
    "", "{", "}", "[", "]", "{1:2}", '{"a" 1}', '{"a":1 "b":2}', "[1,2", "[1,]",
    "{,}", "nul", "tru", "fals", "01", "-01", "1.", "1e", "-.5", "+1", '"abc',
    r'"\x"', r'"\u12"', r'"\ud83d"', r'"\ud83dx"', r'"\ude00"', r'"\udc00"',
    "{} {}", "[1,2] junk", '{"a":}', "[1 2]", '"a<NL>"b"', "/* never closed",
]

# 命令行矩阵：(参数…)，配置文件的路径会在运行前替换进去。
COMMANDS = [
    ["fmt", "<config>"],
    ["fmt", "<config>", "--compact"],
    ["fmt", "<config>", "--indent", "0"],
    ["fmt", "<config>", "--indent", "4"],
    ["fmt", "<config>", "--indent", "x"],
    ["fmt", "<config>", "--bogus"],
    ["check", "<config>"],
    ["get", "limits.maxDepth", "<config>"],
    ["get", "name", "<config>"],
    ["get", "name", "<config>", "--raw"],
    ["get", "tokens[1]", "<config>"],
    ["get", "tokens[1].name", "<config>"],
    ["get", "labels.cn", "<config>", "--raw"],
    ["get", "nope.path", "<config>"],
    ["keys", "<config>"],
    ["keys", "limits", "<config>"],
    ["keys", "tokens", "<config>"],
    ["stats", "<config>"],
    ["get", "limits.maxDepth"],
    ["nope", "<config>"],
    ["fmt", "<config>", "--indent"],
    ["fmt", "<config>", "--out"],
    ["get", "a", "<config>", "extra"],
]


def binary() -> pathlib.Path:
    if RUST.exists():
        return RUST
    if RUST_DEBUG.exists():
        return RUST_DEBUG
    raise SystemExit(f"找不到 Rust 版可执行文件（先 cargo build）：{RUST}")


def normalize(text: str) -> str:
    # C++ 在 Windows 上 stdout 走文本模式（CRLF），Rust 一律 LF。
    return text.replace("\r\n", "\n")


def run(executable: pathlib.Path, arguments: list) -> tuple:
    done = subprocess.run(
        [str(executable), *arguments],
        cwd=str(ROOT),
        stdin=subprocess.DEVNULL,
        capture_output=True,
    )
    return (
        normalize(done.stdout.decode("utf-8", "replace")),
        normalize(done.stderr.decode("utf-8", "replace")),
        done.returncode,
    )


def classify(text: str, left: tuple, right: tuple) -> str:
    """把差异归类；返回 'same' 或差异类的名字。"""
    if left == right:
        return "same"
    # 0.1 / 3.14… / 1e300 这类：两版浮点位数不同，值相同。
    if left[2] == right[2] == 0 and left[0].strip() != right[0].strip():
        try:
            if float(left[0]) == float(right[0]):
                return "浮点写法（值相同）"
        except ValueError:
            pass
    # 落单代理：C++ 写出非法 UTF-8 字节，Rust 报 bad-surrogate。
    if "bad-surrogate" in right[1] or "\ufffd" in left[0]:
        return "落单代理（Rust 拒绝非法 UTF-8）"
    # 用法行里的程序名不同。
    if left[0].startswith("用法：") and right[0].startswith("用法："):
        return "用法行的程序名"
    return "**真差异**"


def report(title: str, cases, args_for, prepare) -> tuple:
    """跑一批用例，返回（一致数、有意差异数、真差异数）。"""
    same = 0
    intentional = []
    real = []
    for case in cases:
        prepare(case)
        arguments = args_for(case)
        left = run(CPP, arguments)
        right = run(binary(), arguments)
        kind = classify(str(case), left, right)
        if kind == "same":
            same += 1
        elif kind == "**真差异**":
            real.append((case, kind, left, right))
        else:
            intentional.append((case, kind, left, right))

    print(f"\n【{title}】共 {len(cases)} 条：一致 {same}，有意差异 {len(intentional)}，真差异 {len(real)}")
    for case, kind, left, right in intentional:
        print(f"  (有意) {kind}  <<{short(case)}>>")
        print(f"      C++  : {short(left)}")
        print(f"      Rust : {short(right)}")
    for case, kind, left, right in real:
        print(f"  **真差异**  <<{short(case)}>>")
        print(f"      C++  : {short(left)}")
        print(f"      Rust : {short(right)}")
    return same, len(intentional), len(real)


def short(value, limit: int = 96) -> str:
    """把长输出截断成一行，方便阅读（300 位小数那种）。"""
    text = repr(value)
    return text if len(text) <= limit else text[:limit] + "…"


def main() -> int:
    what = sys.argv[1] if len(sys.argv) > 1 else ""
    if not CPP.exists():
        raise SystemExit(f"C++ 版没编：{CPP}（先 make -C library/json）")
    WORK.mkdir(parents=True, exist_ok=True)
    case_file = WORK / "case.json"
    case_arg = str(case_file.relative_to(ROOT))
    fmt_args = ["fmt", case_arg, "--compact", "--comments", "--trailing-commas"]

    def prepare_corpus(text: str) -> None:
        expanded = text.replace("<NL>", "\n").replace("<BOM>", "\ufeff")
        case_file.write_bytes(expanded.encode("utf-8"))

    real_total = 0
    if what in ("", "语料", "corpus"):
        _, _, real = report("语料", CORPUS, lambda case: fmt_args, prepare_corpus)
        real_total += real

    if what in ("", "命令", "commands"):
        config = str(CONFIG.relative_to(ROOT))
        broken = WORK / "broken.json"
        broken.write_text("{\n  \"a\": ,\n}", "utf-8")
        cases = [[arg.replace("<config>", config) for arg in item] for item in COMMANDS]
        cases.append(["check", str(broken.relative_to(ROOT))])
        # 用 ASCII 文件名：非 ASCII 路径在 Windows 控制台上两版的回显编码不同
        # （C++ 走窄字符输出，Rust 原样 UTF-8），那是控制台的事，不是 JSON 的事。
        cases.append(["check", str((WORK / "missing-file.json").relative_to(ROOT))])
        _, _, real = report("命令行", cases, lambda arguments: arguments, lambda case: None)
        real_total += real

    print()
    if real_total:
        print(f"结论：有 {real_total} 条**真差异**，需要看。")
        return 1
    print("结论：逐条比对无实质差异（只剩上面标出的有意差异）。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
