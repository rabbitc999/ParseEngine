"""End-to-end checks for the generated parser and both codec targets."""

import importlib.util
from pathlib import Path
import shutil
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build"
GENERATED = BUILD / "generated"
CONTROL_GENERATED = BUILD / "control-generated"


def run(*args, expect=0):
    result = subprocess.run(args, cwd=ROOT, text=True, encoding="utf-8",
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if result.returncode != expect:
        raise AssertionError("command failed (%d): %s\nstdout:\n%s\nstderr:\n%s" %
                             (result.returncode, " ".join(map(str, args)),
                              result.stdout, result.stderr))
    return result


def load_generated(directory, module_name):
    path = directory / (module_name + ".py")
    spec = importlib.util.spec_from_file_location(module_name, path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def check_python():
    codec = load_generated(GENERATED, "telemetry_protocol")
    sample = codec.Sample(
        header=codec.Header(sequence=513, kind=codec.MessageKind.sample, flags=0xA5),
        timestamp=0x01020304,
        payload_length=3,
        payload=b"\x10\x20\x30",
        checksum=0xBEEF,
    )
    expected = bytes.fromhex("010202a5040302010300102030efbe")
    assert sample.encode() == expected
    decoded = codec.Sample.decode(expected)
    assert decoded == sample
    try:
        codec.Sample.decode(expected[:-1])
    except codec.ProtocolDecodeError:
        pass
    else:
        raise AssertionError("truncated packet was accepted")
    try:
        codec.Sample(codec.Header(1, codec.MessageKind.sample, 0), 0, 2, b"x", 0).encode()
    except codec.ProtocolEncodeError:
        pass
    else:
        raise AssertionError("length mismatch was accepted")

    control = load_generated(CONTROL_GENERATED, "control_protocol")
    frame = control.ControlFrame(delta=-2, magic=b"\xaa\x55", state=control.State.ready)
    big_endian = bytes.fromhex("fffeaa551234")
    assert frame.encode() == big_endian
    assert control.ControlFrame.decode(big_endian) == frame


def check_cpp():
    compiler = shutil.which("g++")
    if compiler is None:
        print("[skip] C++17 round-trip: g++ not found")
        return
    executable = BUILD / ("cpp-roundtrip.exe" if sys.platform.startswith("win") else "cpp-roundtrip")
    run(compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
        "-I", str(GENERATED), str(ROOT / "tests" / "cpp_roundtrip.cpp"),
        "-o", str(executable))
    result = run(str(executable))
    assert result.stdout.strip() == "010202a5040302010300102030efbe"
    control_executable = BUILD / ("cpp-big-endian.exe" if sys.platform.startswith("win") else "cpp-big-endian")
    run(compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
        "-I", str(CONTROL_GENERATED), str(ROOT / "tests" / "cpp_big_endian.cpp"),
        "-o", str(control_executable))
    run(str(control_executable))


def check_c():
    compiler = shutil.which("gcc")
    if compiler is None:
        print("[skip] C99 round-trip: gcc not found")
        return
    executable = BUILD / ("c-roundtrip.exe" if sys.platform.startswith("win") else "c-roundtrip")
    run(compiler, "-std=c99", "-Wall", "-Wextra", "-Werror",
        "-I", str(GENERATED), str(ROOT / "tests" / "c_roundtrip.c"),
        "-o", str(executable))
    run(str(executable))
    control_executable = BUILD / ("c-big-endian.exe" if sys.platform.startswith("win") else "c-big-endian")
    run(compiler, "-std=c99", "-Wall", "-Wextra", "-Werror",
        "-I", str(CONTROL_GENERATED), str(ROOT / "tests" / "c_big_endian.c"),
        "-o", str(control_executable))
    run(str(control_executable))


def check_rust():
    compiler = shutil.which("rustc")
    if compiler is None:
        print("[skip] Rust round-trip: rustc not found")
        return
    library = BUILD / "protocol_no_std.rlib"
    run(compiler, "--edition=2021", "--crate-type=lib",
        str(ROOT / "tests" / "rust_no_std.rs"), "-o", str(library))
    executable = BUILD / ("rust-roundtrip.exe" if sys.platform.startswith("win") else "rust-roundtrip")
    run(compiler, "--edition=2021", str(ROOT / "tests" / "rust_roundtrip.rs"),
        "-o", str(executable))
    run(str(executable))


def check_diagnostics():
    result = run(sys.executable, str(BUILD / "main.py"),
                 str(ROOT / "examples" / "invalid-forward-length.pdl"),
                 "--out-dir", str(BUILD / "invalid"), expect=1)
    assert "必须在字节数组之前声明" in result.stderr
    reserved = run(sys.executable, str(BUILD / "main.py"),
                   str(ROOT / "examples" / "invalid-reserved.pdl"),
                   "--out-dir", str(BUILD / "invalid-reserved"), expect=1)
    assert "与生成方法重名" in reserved.stderr


def check_native_cpp():
    compiler = shutil.which("g++")
    if compiler is None:
        print("[skip] native C++ generator: g++ not found")
        return
    exe = BUILD / ("protocol-dsl-cpp.exe" if sys.platform.startswith("win") else "protocol-dsl-cpp")
    telemetry = BUILD / "native-cpp-out"
    control = BUILD / "native-cpp-control"
    run(str(exe), "examples/telemetry.pdl", "--out-dir", str(telemetry))
    run(str(exe), "examples/control.pdl", "--out-dir", str(control))
    out = BUILD / ("native-cpp-roundtrip.exe" if sys.platform.startswith("win") else "native-cpp-roundtrip")
    run(compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-I", str(telemetry),
        str(ROOT / "tests" / "cpp_roundtrip.cpp"), "-o", str(out))
    run(str(out))
    big = BUILD / ("native-cpp-big.exe" if sys.platform.startswith("win") else "native-cpp-big")
    run(compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-I", str(control),
        str(ROOT / "tests" / "cpp_big_endian.cpp"), "-o", str(big))
    run(str(big))
    bad = run(str(exe), "examples/invalid-forward-length.pdl", "--out-dir",
              str(BUILD / "native-cpp-invalid"), expect=1)
    assert "earlier unsigned integer" in bad.stderr
    reserved = run(str(exe), "examples/invalid-reserved.pdl", "--out-dir",
                   str(BUILD / "native-cpp-reserved"), expect=1)
    assert "reserved" in reserved.stderr


def check_native_rust():
    compiler = shutil.which("rustc")
    if compiler is None:
        print("[skip] native Rust generator: rustc not found")
        return
    exe = BUILD / ("protocol-dsl-rust.exe" if sys.platform.startswith("win") else "protocol-dsl-rust")
    run(str(exe), "examples/telemetry.pdl", "--out-dir", str(BUILD / "native-rust-out"))
    run(str(exe), "examples/control.pdl", "--out-dir", str(BUILD / "native-rust-control"))
    library = BUILD / "native-rust-no-std.rlib"
    run(compiler, "--edition=2021", "--crate-type=lib", str(ROOT / "tests" / "rust_native_no_std.rs"),
        "-o", str(library))
    out = BUILD / ("native-rust-roundtrip.exe" if sys.platform.startswith("win") else "native-rust-roundtrip")
    run(compiler, "--edition=2021", str(ROOT / "tests" / "rust_native_roundtrip.rs"), "-o", str(out))
    run(str(out))
    bad = run(str(exe), "examples/invalid-forward-length.pdl", "--out-dir",
              str(BUILD / "native-rust-invalid"), expect=1)
    assert "earlier unsigned integer" in bad.stderr
    reserved = run(str(exe), "examples/invalid-reserved.pdl", "--out-dir",
                   str(BUILD / "native-rust-reserved"), expect=1)
    assert "reserved" in reserved.stderr


def main():
    required = ["telemetry_protocol.py", "telemetry_protocol.hpp", "telemetry_protocol.h",
                "telemetry_protocol.rs", "PROTOCOL.md", "protocol.mmd"]
    missing = [name for name in required if not (GENERATED / name).is_file()]
    assert not missing, "missing generated files: %s" % ", ".join(missing)
    run(sys.executable, str(BUILD / "main.py"), str(ROOT / "examples" / "control.pdl"),
        "--out-dir", str(CONTROL_GENERATED), "--target", "all")
    check_python()
    print("[ok] Python codec round-trip and error paths")
    check_cpp()
    print("[ok] C++17 codec round-trip")
    check_c()
    print("[ok] C99 allocation-free codec round-trip")
    check_rust()
    print("[ok] Rust no_std codec compile and round-trip")
    check_diagnostics()
    print("[ok] semantic diagnostic")
    check_native_cpp()
    print("[ok] native C++17 generator and generated codec")
    check_native_rust()
    print("[ok] native Rust generator and no_std generated codec")
    print("all protocol-dsl checks passed")


if __name__ == "__main__":
    main()
