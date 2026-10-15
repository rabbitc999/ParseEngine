//! library/preprocess-rust 的自检：条件编译、符号、包含、行号映射、诊断。
//!
//! 不用任何测试框架（本 crate 不依赖第三方），就是一批 `#[test]` + `cargo test`。
//! 用例与 C++ 版 `library/preprocess/test_preprocess.cpp` 一一对应，**共 102 处断言、
//! 期望字符串逐字相同**——这是“翻译没走样”的主要证据。
//!
//! ```sh
//! cargo test --manifest-path library/preprocess-rust/Cargo.toml
//! ```
//!
//! 最后一条 `example_fixture` 会把 `examples/main.pp` 的输出与
//! `examples/expected-debug.txt` 逐字节对照一遍。

use std::collections::HashMap;
use std::env;
use std::fs;
use std::path::Path;

use preprocess::{
    Define, IncludeLoader, IncludeRequest, IncludeResult, Options, Preprocessor,
    Result as PreprocessResult, Severity,
};

// ---------------------------------------------------------------------------
//  断言与小工具
// ---------------------------------------------------------------------------

/// 记一条断言（失败就把表达式与行号打出来）。
macro_rules! check {
    ($condition:expr) => {
        assert!(
            $condition,
            "断言失败：{} （第 {} 行）",
            stringify!($condition),
            line!()
        )
    };
}

/// 断言两个字符串相等，失败时把两边打印出来。
fn check_text(title: &str, actual: &str, expected: &str) {
    assert!(
        actual == expected,
        "断言失败：{title}\n    实际：{}\n    期望：{}",
        escape(actual),
        escape(expected)
    );
}

/// 把字符串转义成一行，便于在失败信息里看。
fn escape(text: &str) -> String {
    let mut out = String::new();
    for ch in text.chars() {
        match ch {
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            '\t' => out.push_str("\\t"),
            _ => out.push(ch),
        }
    }
    out
}

/// 取临时目录（Windows 用 TEMP，其余用 TMPDIR / /tmp）。
fn temp_dir() -> String {
    env::var("TEMP")
        .or_else(|_| env::var("TMPDIR"))
        .unwrap_or_else(|_| "/tmp".to_string())
}

/// 写一个临时文件。
///
/// 文件放在 `<临时目录>/preprocess-tests/` 下，名字**原样使用**——因为被包含的
/// 文件里写的是相对路径（`include "cycle-b.inc"`），名字必须对得上。
fn write_temp(name: &str, text: &str) -> String {
    let dir = format!("{}/preprocess-tests", temp_dir());
    let _ = fs::create_dir_all(&dir); // 已存在就忽略
    let path = format!("{dir}/{name}");
    let _ = fs::write(&path, text.as_bytes());
    path
}

/// 删除临时文件。
fn remove_temp(path: &str) {
    let _ = fs::remove_file(path);
}

/// 把路径里的反斜杠换成正斜杠，便于和库输出的路径比较（库会规范化路径）。
fn normalized(path: &str) -> String {
    path.replace('\\', "/")
}

/// 示例目录（跟着 crate 走，和 C++ 版一样）。
fn example_dir() -> String {
    format!("{}/examples", env!("CARGO_MANIFEST_DIR").replace('\\', "/"))
}

// ---------------------------------------------------------------------------
//  用例
// ---------------------------------------------------------------------------

/// 没有指令时输入输出逐字节相同。
#[test]
fn identity() {
    let preprocessor = Preprocessor::default();
    let mut result = PreprocessResult::default();
    check!(preprocessor.run_text("a\nb\n\nc\n", "<t>", "", &mut result));
    check_text("无条件指令时逐字节不变", &result.text, "a\nb\n\nc\n");
    check!(result.diagnostics.is_empty());
    check!(result.lines_in == 4);
    check!(result.lines_out == 4);
}

/// 默认「空行占位」：指令行与假分支都保留行数。
#[test]
fn preserve_lines() {
    let source = "%symbol DEBUG\n\
                  %if DEBUG\n\
                  keep\n\
                  %else\n\
                  drop\n\
                  %endif\n\
                  tail\n";

    let preprocessor = Preprocessor::default();
    let mut result = PreprocessResult::default();
    check!(preprocessor.run_text(source, "<t>", "", &mut result));
    check_text(
        "指令行与假分支都变成空行（行号不变）",
        &result.text,
        "\n\nkeep\n\n\n\ntail\n",
    );
    check!(result.lines_out == 7);

    // 行号映射：第 i 行来自原文第 i 行。
    let mut one_to_one = result.line_map.len() == 7;
    for (index, origin) in result.line_map.iter().enumerate() {
        if origin.line != index + 1 {
            one_to_one = false;
        }
    }
    check!(one_to_one);

    // 关掉空行占位就是紧凑输出。
    let options = Options {
        preserve_lines: false,
        ..Options::default()
    };
    let compact = Preprocessor::new(options);
    let mut compact_result = PreprocessResult::default();
    check!(compact.run_text(source, "<t>", "", &mut compact_result));
    check_text(
        "--drop-lines 风格：只留下命中分支",
        &compact_result.text,
        "keep\ntail\n",
    );
}

/// C 风格的 `#define` / `#undef` / `#ifdef` / `#ifndef`。
#[test]
fn hash_style() {
    let source = "#define DEBUG 1\n\
                  #ifdef DEBUG\n\
                  debug\n\
                  #endif\n\
                  #ifndef RELEASE\n\
                  not-release\n\
                  #endif\n\
                  #undef DEBUG\n\
                  #ifdef DEBUG\n\
                  never\n\
                  #else\n\
                  gone\n\
                  #endif\n";

    let preprocessor = Preprocessor::default();
    let mut result = PreprocessResult::default();
    check!(preprocessor.run_text(source, "<t>", "", &mut result));
    check_text(
        "#define / #ifdef / #ifndef / #undef",
        &result.text,
        "\n\ndebug\n\n\nnot-release\n\n\n\n\n\ngone\n\n",
    );
    check!(result.count(Severity::Error) == 0);
}

/// 条件表达式：`|| && ! ()`、比较、`defined()`。
#[test]
fn expressions() {
    let preprocessor = Preprocessor::default();

    // 1. 公共求值接口。
    let symbols: HashMap<String, String> = [
        ("A".to_string(), String::new()),
        ("B".to_string(), "0".to_string()),
        ("C".to_string(), "hello".to_string()),
    ]
    .into_iter()
    .collect();
    check!(preprocessor.evaluate("A", &symbols) == Ok(true));
    check!(preprocessor.evaluate("B", &symbols) == Ok(false));
    check!(preprocessor.evaluate("!B", &symbols) == Ok(true));
    check!(preprocessor.evaluate("A && !B", &symbols) == Ok(true));
    check!(preprocessor.evaluate("B || (A && C == hello)", &symbols) == Ok(true));
    check!(preprocessor.evaluate("defined(A) && !defined(NOPE)", &symbols) == Ok(true));
    check!(preprocessor.evaluate("C != hello", &symbols) == Ok(false));
    let outcome = preprocessor.evaluate("A &&", &symbols);
    check!(outcome.is_err());
    let diagnostic = outcome.unwrap_err();
    check!(diagnostic.code == "bad-expression");

    // 2. 条件编译里同样的语法。
    let source = "%symbol LOG\n\
                  %symbol QUIET 0\n\
                  %if LOG && !QUIET\n\
                  verbose\n\
                  %elif LOG\n\
                  quiet\n\
                  %else\n\
                  silent\n\
                  %endif\n";
    let mut result = PreprocessResult::default();
    check!(preprocessor.run_text(source, "<t>", "", &mut result));
    check!(result.text.contains("verbose"));
    check!(!result.text.contains("silent"));
}

/// 名字与定义解析的边界。
#[test]
fn names() {
    check!(Preprocessor::is_valid_name("DEBUG"));
    check!(Preprocessor::is_valid_name("_x1"));
    check!(!Preprocessor::is_valid_name(""));
    check!(!Preprocessor::is_valid_name("1x"));
    check!(!Preprocessor::is_valid_name("a-b"));

    let mut define = Define::default();
    check!(Preprocessor::parse_define("NAME=value", &mut define).is_ok());
    check!(define.name == "NAME");
    check!(define.value == "value");
    check!(Preprocessor::parse_define("ONLY", &mut define).is_ok());
    check!(define.value.is_empty());
    check!(Preprocessor::parse_define("=x", &mut define).is_err());

    check!(!Preprocessor::parse_truth_value("0"));
    check!(!Preprocessor::parse_truth_value("off"));
    check!(!Preprocessor::parse_truth_value("False"));
    check!(Preprocessor::parse_truth_value("1"));
    check!(Preprocessor::parse_truth_value("yes"));
}

/// 命令行 `-D` 传进来的预定义符号。
#[test]
fn predefined_symbols() {
    let options = Options {
        defines: vec![Define::new("DEBUG", ""), Define::new("LEVEL", "2")],
        ..Options::default()
    };

    let preprocessor = Preprocessor::new(options);
    let mut result = PreprocessResult::default();
    let source = "%if DEBUG\n\
                  debug\n\
                  %endif\n\
                  %if LEVEL == 2\n\
                  level2\n\
                  %endif\n";
    check!(preprocessor.run_text(source, "<t>", "", &mut result));
    check!(result.text.contains("debug"));
    check!(result.text.contains("level2"));
}

/// `include` / `import` / `include_once` 与依赖表。
#[test]
fn include() {
    let included = write_temp("inc-a.inc", "A1\nA2\n");
    let main = write_temp(
        "main-a.pp",
        "%include \"inc-a.inc\"\n\
         %include \"inc-a.inc\"\n\
         %import \"inc-a.inc\"\n\
         after\n",
    );

    let preprocessor = Preprocessor::default();
    let mut result = PreprocessResult::default();
    check!(preprocessor.run_file(&main, &mut result));
    // 第一次展开，后两次被 include-once 跳过（跳过时补空行占位）。
    check_text("include 展开一次，之后去重", &result.text, "A1\nA2\n\n\nafter\n");
    check!(result.dependencies.len() == 2);
    check!(result.include_count == 3);

    // 关掉 include-once 就会重复展开。
    let options = Options {
        include_once_by_default: false,
        ..Options::default()
    };
    let repeated = Preprocessor::new(options);
    let mut repeated_result = PreprocessResult::default();
    check!(repeated.run_file(&main, &mut repeated_result));
    check!(repeated_result.text.contains("A1\nA2\nA1\nA2\n"));

    remove_temp(&included);
    remove_temp(&main);
}

/// 循环包含与深度限制。
#[test]
fn include_cycle() {
    let first = write_temp("cycle-a.inc", "%include \"cycle-b.inc\"\n");
    let second = write_temp("cycle-b.inc", "%include \"cycle-a.inc\"\n");

    let options = Options {
        include_once_by_default: false, // 关掉去重才能走到循环检测
        ..Options::default()
    };
    let preprocessor = Preprocessor::new(options);
    let mut result = PreprocessResult::default();
    check!(!preprocessor.run_file(&first, &mut result));
    check!(result.count(Severity::Error) >= 1);
    let mut cycle = false;
    for diagnostic in &result.diagnostics {
        if diagnostic.code == "include-cycle" {
            cycle = true;
        }
    }
    check!(cycle);

    remove_temp(&first);
    remove_temp(&second);
}

/// 找不到文件、未闭合条件、多余 `else` / `endif`。
#[test]
fn diagnostics() {
    {
        let preprocessor = Preprocessor::default();
        let mut result = PreprocessResult::default();
        check!(!preprocessor.run_text("%include \"nope.inc\"\n", "<t>", "", &mut result));
        check!(result.diagnostics.len() == 1);
        check!(result.diagnostics[0].code == "include-not-found");
        check!(result.diagnostics[0].line == 1);
    }
    {
        let preprocessor = Preprocessor::default();
        let mut result = PreprocessResult::default();
        check!(!preprocessor.run_text("%if A\nx\n", "<t>", "", &mut result));
        check!(result.diagnostics[0].code == "unterminated-if");
        check!(result.diagnostics[0].line == 1);
    }
    {
        let preprocessor = Preprocessor::default();
        let mut result = PreprocessResult::default();
        check!(!preprocessor.run_text("x\n%endif\n", "<t>", "", &mut result));
        check!(result.diagnostics[0].code == "unmatched-endif");
    }
    {
        let preprocessor = Preprocessor::default();
        let mut result = PreprocessResult::default();
        check!(!preprocessor.run_text("%else\n", "<t>", "", &mut result));
        check!(result.diagnostics[0].code == "unmatched-else");
    }
    {
        let preprocessor = Preprocessor::default();
        let mut result = PreprocessResult::default();
        check!(!preprocessor.run_text("%if A\n%else\n%else\n%endif\n", "<t>", "", &mut result));
        let mut duplicate = false;
        for diagnostic in &result.diagnostics {
            if diagnostic.code == "duplicate-else" {
                duplicate = true;
            }
        }
        check!(duplicate);
    }
    {
        let preprocessor = Preprocessor::default();
        let mut result = PreprocessResult::default();
        check!(!preprocessor.run_text("%frobnicate 1\n", "<t>", "", &mut result));
        check!(result.diagnostics[0].code == "unknown-directive");
    }
    {
        let preprocessor = Preprocessor::default();
        let mut result = PreprocessResult::default();
        check!(!preprocessor.run_text("%error \"炸了\"\n", "<t>", "", &mut result));
        check!(result.diagnostics[0].code == "error-directive");
        check!(result.diagnostics[0].message == "炸了");
    }
    {
        let preprocessor = Preprocessor::default();
        let mut result = PreprocessResult::default();
        check!(preprocessor.run_text("%warning \"小心\"\nx\n", "<t>", "", &mut result));
        check!(result.count(Severity::Warning) == 1);
        check!(result.ok()); // 警告不算失败
    }
}

/// 未知指令的三种策略：`error` / `keep` / `ignore`。
#[test]
fn unknown_policy() {
    let source = "%token NUMBER \"[0-9]+\"\n%define X \"y\"\n";

    let keep_processor = Preprocessor::new(Preprocessor::for_grammar());
    let mut keep_result = PreprocessResult::default();
    check!(keep_processor.run_text(source, "<t>", "", &mut keep_result));
    check_text("keep：文法指令原样保留", &keep_result.text, source);

    let ignore = Options {
        unknown_directive: preprocess::UnknownDirective::Ignore,
        ..Options::default()
    };
    let ignore_processor = Preprocessor::new(ignore);
    let mut ignore_result = PreprocessResult::default();
    check!(ignore_processor.run_text(source, "<t>", "", &mut ignore_result));
    check_text("ignore：不认识的指令静默丢掉", &ignore_result.text, "\n\n");
}

/// 注释、字符串、不透明区域里的指令不当指令。
#[test]
fn not_directives() {
    let source = "// %if NOPE\n\
                  /*\n\
                  %if NOPE\n\
                  */\n\
                  const char *s = \"%if NOPE\";\n\
                  %code\n\
                  %if NOPE\n\
                  %endcode\n\
                  %symbol OK\n\
                  %if OK\n\
                  yes\n\
                  %endif\n";

    // 用 .gram 预设：`%code … %endcode` 里的正文不解析。
    let preprocessor = Preprocessor::new(Preprocessor::for_grammar());
    let mut result = PreprocessResult::default();
    check!(preprocessor.run_text(source, "<t>", "", &mut result));
    check!(result.text.contains("yes"));
    check!(result.count(Severity::Error) == 0);
}

/// 指令行末尾挂的注释要当注释：`%include "a.inc"   // 说明`。
///
/// 引号里的 `//` / `/*` 不算注释起点（路径里可能出现）。
#[test]
fn trailing_comment() {
    let include_path = write_temp("trailing-comment.inc", "INCLUDED\n");
    let slash = include_path.rfind(['/', '\\']);

    let mut options = Preprocessor::for_metro();
    if let Some(slash) = slash {
        options.include_dirs.push(include_path[..slash].to_string());
    }

    let source = "%include \"trailing-comment.inc\"   // 行注释\n\
                  %include \"trailing-comment.inc\"   /* 块注释 */\n\
                  %symbol NAME value // 注释\n\
                  %if NAME == \"value\"\n\
                  yes\n\
                  %endif\n";

    let preprocessor = Preprocessor::new(options);
    let mut result = PreprocessResult::default();
    check!(preprocessor.run_text(source, "<t>", ".", &mut result));
    check!(result.count(Severity::Error) == 0);
    check!(result.text.contains("INCLUDED"));
    check!(result.text.contains("yes"));
}

/// `#pragma once` 与 metro 别名。
#[test]
fn pragma_and_metro_aliases() {
    let included = write_temp("once.inc", "#pragma once\nB1\n");
    let main = write_temp(
        "main-once.pp",
        "#include \"once.inc\"\n\
         #include \"once.inc\"\n\
         tail\n",
    );

    // 关掉默认去重，只有 #pragma once 生效时才只展开一次。
    let options = Options {
        include_once_by_default: false,
        ..Options::default()
    };
    let preprocessor = Preprocessor::new(options);
    let mut result = PreprocessResult::default();
    check!(preprocessor.run_file(&main, &mut result));
    // `#pragma once` 那一行本身变成空行，所以开头有一个空行。
    check_text("#pragma once 生效", &result.text, "\nB1\n\ntail\n");

    // metro 预设：`%define` / `%undefine` 也算符号定义。
    let metro_processor = Preprocessor::new(Preprocessor::for_metro());
    let mut metro_result = PreprocessResult::default();
    check!(metro_processor.run_text("%define M\n%ifdef M\nm\n%endif\n", "<t>", "", &mut metro_result));
    check!(metro_result.text.contains("m"));

    remove_temp(&included);
    remove_temp(&main);
}

/// `#line` 标记与行号映射里的来源文件。
#[test]
fn line_markers() {
    let included = write_temp("marker.inc", "IN\n");
    let main = write_temp(
        "main-marker.pp",
        "top\n\
         %include \"marker.inc\"\n\
         bottom\n",
    );

    let options = Options {
        emit_line_markers: true,
        ..Options::default()
    };
    let preprocessor = Preprocessor::new(options);
    let mut result = PreprocessResult::default();
    check!(preprocessor.run_file(&main, &mut result));
    check_text(
        "换文件时插 #line",
        &result.text,
        &format!(
            "#line 1 \"{}\"\n\
             top\n\
             #line 1 \"{}\"\n\
             IN\n\
             #line 3 \"{}\"\n\
             bottom\n",
            normalized(&main),
            normalized(&included),
            normalized(&main)
        ),
    );

    remove_temp(&included);
    remove_temp(&main);
}

/// 自定义 loader：让 `%import` 吃 CSV、`%include` 走默认、还能跳过与报错。
///
/// 这里用一个最小的「CSV → 文本」回调，模拟 metro_editor 里
/// `%import "stations.csv" AS STATIONS` 那种「把数据源变成文法文本」的用法。
#[test]
fn include_loader() {
    let csv = write_temp("stations.csv", "name,lon,lat\nA,1,2\nB,3,4\n");
    let main = write_temp(
        "main-loader.pp",
        "%import \"stations.csv\"\n\
         %include \"plain.inc\"\n\
         %import \"skip.csv\"\n\
         %import \"broken.csv\"\n",
    );
    write_temp("plain.inc", "plain\n");
    write_temp("skip.csv", "x\n");
    write_temp("broken.csv", "y\n");

    let mut options = Options::default();
    options
        .include_dirs
        .push(format!("{}/preprocess-tests", temp_dir()));
    let loader: IncludeLoader = Box::new(|request: &IncludeRequest| {
        // 1. 只接管 CSV：其它文件交回默认逻辑（读文件）。
        let is_csv = request.path.len() >= 4 && request.path.ends_with(".csv");
        if !is_csv {
            return IncludeResult::use_default();
        }

        // 2. 按名字造出不同结果，覆盖 Skip / Error / 正常三种分支。
        if request.path == "skip.csv" {
            return IncludeResult::skip();
        }
        if request.path == "broken.csv" {
            return IncludeResult::error("这份 CSV 坏了");
        }
        if !request.imported {
            // `%include` 来的 CSV 不处理，交回默认（读原文）。
            return IncludeResult::use_default();
        }

        // 3. 真的把 CSV 转成文本：第一行是表头，后面每行变成 `列=值; 列=值`。
        let content = match fs::read(&request.resolved_path) {
            Ok(bytes) => String::from_utf8_lossy(&bytes).into_owned(),
            Err(_) => return IncludeResult::error("读不了 CSV"),
        };
        let mut lines = content.split('\n');
        let mut headers: Vec<String> = Vec::new();
        let mut rows: Vec<String> = Vec::new();
        if let Some(header) = lines.next() {
            for cell in header.split(',') {
                headers.push(cell.replace('\r', ""));
            }
        }
        for line in lines {
            let mut cells: Vec<String> = Vec::new();
            for cell in line.split(',') {
                cells.push(cell.replace('\r', ""));
            }
            let mut text = "row".to_string();
            for (index, cell) in cells.iter().enumerate() {
                let column = headers
                    .get(index)
                    .cloned()
                    .unwrap_or_else(|| "col".to_string());
                text += &format!(" {column}={cell}");
            }
            rows.push(text);
        }

        let mut generated = String::new();
        for row in &rows {
            generated.push_str(row);
            generated.push('\n');
        }
        IncludeResult::inline_text_as(generated, format!("<csv:{}>", request.path))
    });
    options.include_loader = Some(loader);

    let preprocessor = Preprocessor::new(options);
    let mut result = PreprocessResult::default();
    // broken.csv 故意报错
    check!(!preprocessor.run_file(&main, &mut result));

    // 1. CSV 被转成了文本，并且用回调给的 label 进依赖表。
    check!(result.text.contains("row name=A lon=1 lat=2"));
    check!(result.text.contains("row name=B lon=3 lat=4"));
    check!(result.text.contains("plain"));
    let csv_dependency = result
        .dependencies
        .iter()
        .any(|dependency| dependency == "<csv:stations.csv>");
    check!(csv_dependency);

    // 2. Skip 分支什么都不做（那一行只留空行占位）。
    check!(!result.text.contains("skip"));

    // 3. Error 分支给出 include-loader-error。
    let mut loader_error = false;
    for diagnostic in &result.diagnostics {
        if diagnostic.code == "include-loader-error" && diagnostic.message == "这份 CSV 坏了" {
            loader_error = true;
        }
    }
    check!(loader_error);

    remove_temp(&csv);
    remove_temp(&main);
    remove_temp(&format!("{}/preprocess-tests/plain.inc", temp_dir()));
    remove_temp(&format!("{}/preprocess-tests/skip.csv", temp_dir()));
    remove_temp(&format!("{}/preprocess-tests/broken.csv", temp_dir()));
}

/// 端到端：跑示例目录里的示例文件并与期望输出对照。
#[test]
fn example_fixture() {
    let dir = example_dir();
    let main = format!("{dir}/main.pp");
    if !Path::new(&main).exists() {
        panic!("读不到示例 {main}");
    }

    let options = Options {
        include_dirs: vec![dir.clone()],
        defines: vec![Define::new("DEBUG", "")],
        ..Options::default()
    };
    let preprocessor = Preprocessor::new(options);
    let mut result = PreprocessResult::default();
    check!(preprocessor.run_file(&main, &mut result));

    let expected = fs::read(format!("{dir}/expected-debug.txt")).expect("读不到 expected-debug.txt");
    let expected = String::from_utf8_lossy(&expected).into_owned();
    check_text("示例：与 expected-debug.txt 一致", &result.text, &expected);

    // main.pp + common.inc + debug.inc
    check!(result.dependencies.len() == 3);
}
