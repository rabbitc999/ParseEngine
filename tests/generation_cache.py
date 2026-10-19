"""二进制增量生成、失效条件、产物时间戳及真实 C/C++ 子集回归。"""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
EXE = ".exe" if os.name == "nt" else ""
ENGINE = ROOT / "build/bin" / ("ParseEngine" + EXE)


def run(args, *, ok=True, input_text=None):
    start = time.perf_counter()
    result = subprocess.run([str(x) for x in args], cwd=ROOT, capture_output=True,
                            encoding="utf-8", errors="replace", input=input_text)
    if (result.returncode == 0) != ok:
        raise AssertionError(f"{args}\n{result.stdout}\n{result.stderr}")
    return result.stdout + result.stderr, time.perf_counter() - start


def generate(grammar, out, *flags, ok=True):
    return run([ENGINE, grammar, "--ui-lang", "zh", "--out-dir", out, *flags], ok=ok)


def snapshot(out):
    return {p.relative_to(out).as_posix(): p.read_bytes() for p in out.rglob("*")
            if p.is_file() and ".parseengine-cache" not in p.parts}


def compile_driver(out, text, expected=0, value=None):
    driver = out / "cache_check.cpp"
    source = '#include "parser.hpp"\nint main() { Parser::Engine engine; Parser::ParserValue_T value{};'
    source += 'auto status=engine.runText(' + json.dumps(text, ensure_ascii=False) + ', &value);'
    source += ('if(status==Parser::Status::Ok) return 1;' if expected is None
               else f'if(static_cast<int>(status)!={expected}) return 1;')
    if value is not None:
        source += f'if(value!={value}) return 2;'
    source += 'return 0;}\n'
    driver.write_text(source, encoding="utf-8")
    binary = out / ("cache_check" + EXE)
    run(["g++", "-std=c++17", "-O0", out / "parser.cpp", driver, "-o", binary])
    run([binary])


def main():
    (ROOT / "tmp").mkdir(exist_ok=True)
    benchmarks = []
    with tempfile.TemporaryDirectory(prefix="generation-cache-", dir=ROOT / "tmp") as directory:
        base = Path(directory)
        unit = base / ("unit" + EXE)
        run(["g++", "-std=c++17", "-O1", "-Isource/include", "tests/generation_cache.cpp",
             "source/src/generation_cache.cpp", "-o", unit])
        run([unit, base / "unit-cache"])
        run(["g++", "-std=c++11", "-fsyntax-only", ROOT / "example/cpp11_subset.cpp"])

        # 全部十六种模式必须恢复出与从头生成逐字节相同的工程。
        simple = base / "simple.gram"
        simple.write_text('%tree\n%token A "a"\nstart := A;\n', encoding="utf-8")
        for mode in ("lex", "ll0", "ll1", "llk", "lr0", "slr", "slrk", "lalr",
                     "lalrk", "lr1", "lrk", "ielr", "glr", "earley", "cyk", "dd"):
            out = base / mode
            flags = ["--mode", mode]
            grammar = simple
            if mode == "lex":
                grammar = base / "lex.gram"
                grammar.write_text('%mode lex\n%token A "a"\n', encoding="utf-8")
            generate(grammar, out, *flags)
            expected = snapshot(out)
            generate(grammar, out, *flags, "--incremental")
            times = {p: (out / p).stat().st_mtime_ns for p in expected}
            text, _ = generate(grammar, out, *flags, "--incremental")
            assert snapshot(out) == expected, mode
            assert times == {p: (out / p).stat().st_mtime_ns for p in expected}, mode
            assert ("分析缓存命中" in text) == (mode != "lex"), mode

        # 八种代码目标共用分析层，普通/窄后端的报告分类分别缓存。
        shared = base / "language-cache"
        for lang in ("c", "cpp17", "python", "csharp", "rust", "go", "java", "js"):
            out = base / ("language-" + lang)
            generate(simple, out, "--mode", "dd", "--lang", lang)
            expected = snapshot(out)
            generate(simple, out, "--mode", "dd", "--lang", lang, "--cache-dir", shared)
            text, _ = generate(simple, out, "--mode", "dd", "--lang", lang, "--cache-dir", shared)
            assert "分析缓存命中" in text and snapshot(out) == expected, lang

        # 真实大文法：C89 子集与新增 C++11 子集，记录总耗时而不设置易波动的性能断言。
        for name in ("c89_subset", "cpp11_subset"):
            grammar = ROOT / "example" / (name + ".gram")
            out = base / (name + "-out")
            _, uncached = generate(grammar, out)
            expected = snapshot(out)
            _, cold = generate(grammar, out, "--incremental")
            text, warm = generate(grammar, out, "--incremental")
            assert "分析缓存命中" in text and snapshot(out) == expected
            cache_bytes = sum(p.stat().st_size for p in (out / ".parseengine-cache").glob("*.bin"))
            benchmarks.append(dict(grammar=name, uncached_seconds=uncached,
                                   cold_seconds=cold, warm_seconds=warm, cache_bytes=cache_bytes))
            print(json.dumps(benchmarks[-1], ensure_ascii=False), flush=True)
            if name == "cpp11_subset":
                compile_driver(out, (ROOT / "example/cpp11_subset.cpp").read_text(encoding="utf-8"))
                # 不能靠吞任意 Token 流通过：破坏类成员语法必须失败。
                compile_driver(out, "class Broken { public: int ; + };", expected=None)
            else:
                compile_driver(out, "int f(void) { return 1; }")

        # LR 动作与源码位置变化复用建表结果，但生成的动作必须是本轮输入。
        grammar = base / "actions.gram"
        source = '%value_type int\n%token regex A "a"\nstart := A { $$ = 11; };\n'
        grammar.write_text(source, encoding="utf-8")
        out, cache = base / "actions", base / "shared-cache"
        flags = ["--mode", "ielr", "--cache-dir", cache]
        generate(grammar, out, *flags)
        source = "/* new source position */\n" + source.replace("11", "222")
        grammar.write_text(source, encoding="utf-8")
        assert "分析缓存命中" in generate(grammar, out, *flags)[0]
        baseline = base / "action-baseline"
        generate(grammar, baseline, "--mode", "ielr")
        assert snapshot(out) == snapshot(baseline)
        compile_driver(out, "a", value=222)

        # 词法修改仍复用语法分析，输出必须反映新词法。
        source = source.replace('A "a"', 'A "b"')
        grammar.write_text(source, encoding="utf-8")
        assert "分析缓存命中" in generate(grammar, out, *flags)[0]
        compile_driver(out, "b", value=222)
        assert "分析缓存未命中" in generate(grammar, out, *flags, "--rebuild")[0]
        assert "分析缓存" not in generate(grammar, out, *flags, "--no-incremental")[0]

        # 截断、错误版本、错误摘要、带有效摘要的恶意长度都只能触发重算。
        entry = next(cache.glob("analysis-*.bin"))
        valid = entry.read_bytes()
        bad_version = bytearray(valid)
        bad_version[8:16] = (999).to_bytes(8, "little")
        bad_digest = valid[:-1] + bytes([valid[-1] ^ 1])
        # 文件字段依次为版本、键、摘要、负载；负载首字段是 mode 字符串长度。
        key_end = 24 + int.from_bytes(valid[16:24], "little")
        digest_start = key_end + 8
        payload_length_pos = digest_start + 64
        payload_start = payload_length_pos + 8
        malformed = bytearray(valid)
        malformed[payload_start:payload_start + 8] = b"\xff" * 8
        malformed[digest_start:digest_start + 64] = hashlib.sha256(malformed[payload_start:]).hexdigest().encode()
        for damaged in (b"bad", valid[:30], bad_version, bad_digest, malformed, valid + b"extra"):
            entry.write_bytes(damaged)
            assert "分析缓存未命中" in generate(grammar, out, *flags)[0]
            assert entry.read_bytes() == valid

        # 改产生式、优先级、模式、k 或教学配置必须失效。
        grammar.write_text(source.replace("start := A", "start := A A"), encoding="utf-8")
        assert "分析缓存未命中" in generate(grammar, out, *flags)[0]
        grammar.write_text(source.replace("start :=", "%left A\nstart :="), encoding="utf-8")
        assert "分析缓存未命中" in generate(grammar, out, *flags)[0]
        grammar.write_text(source, encoding="utf-8")
        for extra in (("--mode", "lr1"), ("--mode", "lrk", "--lookahead", "2"), ("--teach",)):
            assert "分析缓存未命中" in generate(grammar, out, *flags, *extra)[0]
        # C/C++ 共用分析；语义动作仍在各自的生成器中重新展开。
        assert "分析缓存命中" in generate(grammar, base / "c-target", *flags, "--lang", "c")[0]

        # 纯校验不能创建缓存或输出目录；不可写的缓存路径不影响正常生成。
        untouched = base / "check-only"
        generate(grammar, untouched, "--incremental", "--check")
        assert not untouched.exists()
        blocker = base / "not-a-directory"
        blocker.write_text("user file", encoding="utf-8")
        text, _ = generate(grammar, base / "fallback", "--cache-dir", blocker)
        assert "缓存写入失败" in text and blocker.read_text(encoding="utf-8") == "user file"

        # 并行进程原子发布同一个键，不允许读到半份缓存。
        concurrent_cache = base / "concurrent-cache"
        commands = [[str(ENGINE), str(simple), "--mode", "ielr", "--cache-dir", str(concurrent_cache),
                     "--out-dir", str(base / f"concurrent-{i}")] for i in range(2)]
        processes = [subprocess.Popen(command, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.PIPE) for command in commands]
        for process in processes:
            stdout, stderr = process.communicate(timeout=60)
            assert process.returncode == 0, (stdout, stderr)
        assert not list(concurrent_cache.glob("*.tmp"))
        assert "分析缓存命中" in generate(simple, base / "concurrent-0", "--mode", "ielr", "--cache-dir", concurrent_cache)[0]
    (ROOT / "tmp/generation-cache-benchmark.json").write_text(json.dumps(benchmarks, ensure_ascii=False, indent=2), encoding="utf-8")
    print("二进制增量生成：十六模式、动作刷新、失效与损坏恢复、并行发布及 C/C++ 子集全部通过")


if __name__ == "__main__":
    main()
