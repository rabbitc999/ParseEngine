//! 自检之三：命令行工具与投射演示（对应 C++ 版 `make -C library/json check` 的后半段）。
//!
//! 期望输出取自 **C++ 版跑同一份示例数据的实测结果**（逐字节比对过；只差 Windows
//! 文本模式的 CRLF——C++ 的 stdout 是 `\r\n`，Rust 一律 LF，`--out` 两边都是 LF）。
//!
//! ```sh
//! cargo test --manifest-path library/json-rust/Cargo.toml
//! ```
//!
//! 示例数据是 C++ 版目录里的那一份（`library/json/examples/config.json`），
//! **两版共用，不复制**——和 `library/transfer-rust` 读 `library/transfer/core/example/`
//! 的做法一致。

use std::io::Write;
use std::path::PathBuf;
use std::process::{Command, Stdio};

/// C++ 版放在 `library/json/examples/config.json`，这里直接用它的路径。
fn config() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../json/examples/config.json")
}

/// 临时文件放 cargo 给的临时目录（集成测试里就有这个环境变量）。
fn temp(name: &str) -> PathBuf {
    PathBuf::from(env!("CARGO_TARGET_TMPDIR")).join(name)
}

/// 跑一次命令行工具，返回（标准输出、标准错误、退出码）。
///
/// 标准输入挂到空设备上：不给文件时它会去读标准输入（与 C++ 版一致），
/// 挂空设备让"读到空输入"这件事是确定的，而不是看测试进程继承了谁的 stdin。
fn cli(arguments: &[&str]) -> (String, String, i32) {
    let done = Command::new(env!("CARGO_BIN_EXE_json-cli"))
        .args(arguments)
        .stdin(Stdio::null())
        .output()
        .expect("跑不动 json-cli");
    (
        String::from_utf8_lossy(&done.stdout).into_owned(),
        String::from_utf8_lossy(&done.stderr).into_owned(),
        done.status.code().unwrap_or(-1),
    )
}

/// 示例数据的紧凑形态（`fmt --compact` 的期望输出）。
fn compact() -> String {
    format!(
        "{}\n",
        concat!(
            r#"{"name":"pl0-super","version":2,"debug":false,"targets":["mips","arm"],"#,
            r#""limits":{"maxDepth":64,"maxErrors":8,"ratio":0.75},"#,
            r#""tokens":[{"name":"NUMBER","priority":10,"pattern":"[0-9]+"},"#,
            r#"{"name":"IDENT","priority":5,"pattern":"[A-Za-z_][A-Za-z0-9_]*"}],"#,
            r#""labels":{"cn":"PL/0 超集","en":"PL/0 superset"}}"#,
        )
    )
}

/// `fmt`：紧凑输出。
#[test]
fn cli_fmt_compact() {
    let path = config();
    let (out, err, code) = cli(&["fmt", path.to_str().unwrap(), "--compact"]);
    assert_eq!(code, 0, "{err}");
    assert_eq!(out, compact());
    assert_eq!(err, "");
}

/// `fmt`：缩进输出（缺省 2 空格）与 `--indent 4`。
#[test]
fn cli_fmt_indent() {
    let path = config();
    let body = r#"{
  "name": "pl0-super",
  "version": 2,
  "debug": false,
  "targets": [
    "mips",
    "arm"
  ],
  "limits": {
    "maxDepth": 64,
    "maxErrors": 8,
    "ratio": 0.75
  },
  "tokens": [
    {
      "name": "NUMBER",
      "priority": 10,
      "pattern": "[0-9]+"
    },
    {
      "name": "IDENT",
      "priority": 5,
      "pattern": "[A-Za-z_][A-Za-z0-9_]*"
    }
  ],
  "labels": {
    "cn": "PL/0 超集",
    "en": "PL/0 superset"
  }
}"#;
    let (out, _, code) = cli(&["fmt", path.to_str().unwrap()]);
    assert_eq!(code, 0);
    assert_eq!(out, format!("{body}\n"));

    let (out, _, code) = cli(&["fmt", path.to_str().unwrap(), "--indent", "4"]);
    assert_eq!(code, 0);
    assert!(out.contains("\n    \"name\": \"pl0-super\","), "{out}");
    assert!(out.contains("\n        \"mips\","), "{out}");

    // `--indent 0`：换行但不缩进。
    let (out, _, _) = cli(&["fmt", path.to_str().unwrap(), "--indent", "0"]);
    assert!(out.starts_with("{\n\"name\": \"pl0-super\",\n"));
}

/// `check`：只校验，打印类型与顶层项数。
#[test]
fn cli_check() {
    let path = config();
    let (out, err, code) = cli(&["check", path.to_str().unwrap()]);
    assert_eq!(code, 0);
    assert_eq!(out, "JSON 有效：object，顶层 7 项\n");
    assert_eq!(err, "");

    // 坏输入：诊断带文件名与行列，退出码 1。
    let broken = temp("broken.json");
    std::fs::write(&broken, "{\n  \"a\": ,\n}").expect("写不了临时文件");
    let (out, err, code) = cli(&["check", broken.to_str().unwrap()]);
    assert_eq!(code, 1);
    assert_eq!(out, "");
    assert_eq!(
        err,
        format!(
            "{}:2:8: 不认识的字符 ',' [unexpected-token]\n",
            broken.display()
        )
    );
}

/// `get`：按路径取值，`--raw` 时字符串不加引号。
#[test]
fn cli_get() {
    let path = config();
    let path_text = path.to_str().unwrap();

    assert_eq!(cli(&["get", "limits.maxDepth", path_text]).0, "64\n");
    assert_eq!(cli(&["get", "name", path_text, "--raw"]).0, "pl0-super\n");
    assert_eq!(cli(&["get", "name", path_text]).0, "\"pl0-super\"\n");
    assert_eq!(cli(&["get", "targets[1]", path_text, "--raw"]).0, "arm\n");
    assert_eq!(cli(&["get", "labels.cn", path_text, "--raw"]).0, "PL/0 超集\n");
    assert_eq!(cli(&["get", "debug", path_text]).0, "false\n");
    assert_eq!(cli(&["get", "limits.ratio", path_text]).0, "0.75\n");

    // 路径不存在：报错 + 退出码 1。
    let (out, err, code) = cli(&["get", "nope.path", path_text]);
    assert_eq!(code, 1);
    assert_eq!(out, "");
    assert_eq!(err, "路径不存在：nope.path\n");
}

/// `keys`：列出成员名（对象给名字，数组给 `[i]`）。
#[test]
fn cli_keys() {
    let path = config();
    let path_text = path.to_str().unwrap();
    assert_eq!(
        cli(&["keys", path_text]).0,
        "name\nversion\ndebug\ntargets\nlimits\ntokens\nlabels\n"
    );
    assert_eq!(cli(&["keys", "limits", path_text]).0, "maxDepth\nmaxErrors\nratio\n");
    assert_eq!(cli(&["keys", "tokens", path_text]).0, "[0]\n[1]\n");
    assert_eq!(cli(&["keys", "targets", path_text]).0, "[0]\n[1]\n");

    // 不是对象也不是数组：退出码 1。
    let scalar = temp("scalar.json");
    std::fs::write(&scalar, "42").expect("写不了临时文件");
    let (out, err, code) = cli(&["keys", scalar.to_str().unwrap()]);
    assert_eq!(code, 1);
    assert_eq!(out, "");
    assert_eq!(err, "这个值既不是对象也不是数组：int\n");
}

/// `stats`：类型分布与最大深度（键按名字排序，与 C++ 的 `std::map` 一致）。
#[test]
fn cli_stats() {
    let path = config();
    let (out, _, code) = cli(&["stats", path.to_str().unwrap()]);
    assert_eq!(code, 0);
    assert_eq!(
        out,
        concat!(
            "array: 2\n",
            "array.items: 4\n",
            "array.nested: 2\n",
            "bool: 1\n",
            "double: 1\n",
            "int: 5\n",
            "object: 5\n",
            "object.members: 18\n",
            "string: 9\n",
            "maxdepth: 4\n",
        )
    );
}

/// 注释 / 尾逗号开关，以及标准输入。
#[test]
fn cli_loose_options_and_stdin() {
    let loose = temp("loose.json");
    std::fs::write(&loose, "// 注释\n{\"a\": [1, 2,], /* 块 */}").expect("写不了临时文件");

    // 不放宽：注释直接报错。
    let (_, err, code) = cli(&["check", loose.to_str().unwrap()]);
    assert_eq!(code, 1);
    assert!(err.contains("unexpected-token"), "{err}");

    // 放宽之后能过。
    let (out, _, code) = cli(&["fmt", loose.to_str().unwrap(), "--compact", "--comments", "--trailing-commas"]);
    assert_eq!(code, 0);
    assert_eq!(out, "{\"a\":[1,2]}\n");

    // 不给文件就从标准输入读。
    let mut child = Command::new(env!("CARGO_BIN_EXE_json-cli"))
        .args(["fmt", "--compact"])
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .spawn()
        .expect("跑不动 json-cli");
    child
        .stdin
        .as_mut()
        .expect("没有 stdio")
        .write_all("{\"b\":1,\"a\":2}".as_bytes())
        .expect("写不进标准输入");
    let done = child.wait_with_output().expect("收不到输出");
    assert_eq!(String::from_utf8_lossy(&done.stdout), "{\"b\":1,\"a\":2}\n");
    assert_eq!(done.status.code(), Some(0));
}

/// `--out`：写文件（与标准输出同一份内容，LF 结尾）。
#[test]
fn cli_out_file() {
    let path = config();
    let path_text = path.to_str().unwrap();
    let target = temp("config.compact.json");
    let (out, _, code) = cli(&[
        "fmt",
        path_text,
        "--compact",
        "--out",
        target.to_str().unwrap(),
    ]);
    assert_eq!(code, 0);
    assert_eq!(out, "", "--out 时标准输出不该有东西");
    let written = std::fs::read_to_string(&target).expect("读不了写出来的文件");
    assert_eq!(written, compact());
    assert!(!written.contains('\r'), "产物不该带 CR");

    // 再用这个文件跑一遍 check（对应 C++ 版 check 目标里的那一步）。
    assert_eq!(
        cli(&["check", target.to_str().unwrap()]).0,
        "JSON 有效：object，顶层 7 项\n"
    );
}

/// 用法错误：使用帮助、不认识的选项 / 命令、位置参数。
#[test]
fn cli_usage_errors() {
    let (out, _, code) = cli(&[]);
    assert_eq!(code, 2);
    assert!(out.starts_with("用法："), "{out}");
    assert!(out.contains("--trailing-commas"), "{out}");

    // `--help` 要放在命令后面（`json-cli fmt --help`）才走"帮助"分支；
    // 放在命令位就是"不认识的命令"——与 C++ 版一致（它也是按命令名分派的）。
    let path = config();
    let (out, err, code) = cli(&["--help", path.to_str().unwrap()]);
    assert_eq!(code, 2);
    assert_eq!(err, "不认识的命令：--help\n");
    assert!(out.contains("文件缺省从标准输入读。"), "{out}");

    let (out, _, code) = cli(&["fmt", path.to_str().unwrap(), "--help"]);
    assert_eq!(code, 0);
    assert!(out.contains("文件缺省从标准输入读。"), "{out}");
    let (_, err, code) = cli(&["fmt", path.to_str().unwrap(), "--bogus"]);
    assert_eq!(code, 2);
    assert_eq!(err, "不认识的选项：--bogus\n");

    // 命令名不认识：给个能解析的文件，否则它会先去读标准输入（先读输入、再分派命令，
    // 与 C++ 版顺序一致）。
    let (_, err, code) = cli(&["nope", path.to_str().unwrap()]);
    assert_eq!(code, 2);
    assert_eq!(err, "不认识的命令：nope\n");

    let (_, err, code) = cli(&["get"]);
    assert_eq!(code, 2);
    assert_eq!(err, "get 需要路径，例如 get user.tags[0] config.json\n");

    let (_, err, code) = cli(&["fmt", path.to_str().unwrap(), "--indent"]);
    assert_eq!(code, 2);
    assert_eq!(err, "--indent 缺少取值\n");

    let (_, err, code) = cli(&["fmt", path.to_str().unwrap(), "--out"]);
    assert_eq!(code, 2);
    assert_eq!(err, "--out 缺少取值\n");

    let (_, err, code) = cli(&["get", "a", path.to_str().unwrap(), "extra"]);
    assert_eq!(code, 2);
    assert_eq!(err, "位置参数太多了：extra\n");

    let missing = temp("没有这个文件.json");
    let (_, err, code) = cli(&["check", missing.to_str().unwrap()]);
    assert_eq!(code, 2);
    assert!(err.starts_with("打不开文件："), "{err}");
}

/// 投射演示：JSON → 模型 → JSON（对应 C++ 版的 `model-demo`）。
#[test]
fn model_demo() {
    let path = config();
    let done = Command::new(env!("CARGO_BIN_EXE_json-model-demo"))
        .arg(path.to_str().unwrap())
        .output()
        .expect("跑不动 json-model-demo");
    assert_eq!(done.status.code(), Some(0));
    let out = String::from_utf8_lossy(&done.stdout);
    let expected = format!(
        "配置：pl0-super v2\n\
         目标：2 个，限制 maxDepth=64\n\
         词法：2 条，最高优先级=10\n\
         标签 cn = PL/0 超集\n\
         标签 en = PL/0 superset\n\
         \n\
         重新编码：{}",
        compact()
    );
    assert_eq!(out, expected);

    // 不带参数时用缺省路径（就是上面那份示例数据）。
    let done = Command::new(env!("CARGO_BIN_EXE_json-model-demo"))
        .output()
        .expect("跑不动 json-model-demo");
    assert_eq!(done.status.code(), Some(0));
    assert_eq!(String::from_utf8_lossy(&done.stdout), expected);

    // 坏输入：报错 + 退出码 1。
    let broken = temp("demo_broken.json");
    std::fs::write(&broken, "{\"name\": 1}").expect("写不了临时文件");
    let done = Command::new(env!("CARGO_BIN_EXE_json-model-demo"))
        .arg(broken.to_str().unwrap())
        .output()
        .expect("跑不动 json-model-demo");
    assert_eq!(done.status.code(), Some(1));
    let err = String::from_utf8_lossy(&done.stderr);
    assert!(err.starts_with("投射失败："), "{err}");
    assert!(err.contains("期望字符串，实际是 int"), "{err}");
}
