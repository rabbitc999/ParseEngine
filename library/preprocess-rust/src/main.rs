//! preprocess 命令行工具（对应 C++ 版 `library/preprocess/main.cpp`）。
//!
//! 用法见 `--help`；退出码：0 成功，1 有错误诊断，2 用法错误。
//!
//! 与 C++ 版的两处**有意**差别：
//!
//! - `--version` 打的是 `preprocess-rust 1.0（ParseEngine library/preprocess-rust）`；
//! - 写标准输出时 Rust 不做换行翻译，所以 Windows 上的 `stdout` 是 `LF`
//!   （C++ 版的 `std::cout` 是文本模式，会变成 `CRLF`）。**要跟 C++ 版逐字节
//!   对照就用 `-o`**：两边都是二进制写，输出完全一致。

use std::fs;
use std::io::Write;

use preprocess::{
    Define, IncludeRequest, IncludeResult, OpaqueRegion, Options, Preprocessor,
    Result as PreprocessResult, Severity, Syntax, UnknownDirective,
};

/// 打印用法。
fn print_usage(program: &str) {
    println!(
        "用法：{program} <输入文件> [选项]\n\
         \n\
         输出：\n\
         \x20 -o, --out FILE         输出文件（缺省写到标准输出）\n\
         \x20     --keep-directives  指令行原样保留（默认用空行占位）\n\
         \x20     --drop-lines       被丢掉的行直接删除（不补空行，输出更紧凑）\n\
         \x20     --line-markers     换文件时插入 #line 标记\n\
         \x20     --check            只做检查，不输出正文\n\
         \x20     --deps             只列出依赖文件，每行一个\n\
         \n\
         符号与条件：\n\
         \x20 -D, --define NAME[=V]  预定义符号（可重复）\n\
         \x20 -U, --undef NAME       取消一个预定义符号（可重复）\n\
         \x20     --case-insensitive 符号名大小写不敏感\n\
         \x20     --hash             只认 # 前缀（C / C++ 家族）\n\
         \x20     --percent          只认 % 前缀（yacc / .gram / metro 家族）\n\
         \x20     --metro            等价 --percent 且把 %define 当符号定义\n\
         \x20     --grammar          等价 --percent --unknown=keep 并带上 .gram 的不透明区域\n\
         \x20     --syntax MODE      auto / hash / percent / custom，缺省 auto\n\
         \n\
         包含：\n\
         \x20 -I, --include-dir DIR  额外搜索目录（可重复）\n\
         \x20     --no-include-once  同一个文件允许重复展开\n\
         \x20     --max-depth N      最大嵌套深度，缺省 32\n\
         \x20     --csv-to-lines     演示自定义 loader：%import 的 .csv 按表头转成\n\
         \x20                        `行名 列=值 列=值` 的文本行\n\
         \n\
         其它：\n\
         \x20     --prefix CHARS     --syntax custom 时的指令前缀集合\n\
         \x20     --unknown MODE     不认识的指令：error / keep / ignore，缺省 error\n\
         \x20     --stats            把输入行数 / 输出行数 / 依赖数写到标准错误\n\
         \x20 -h, --help             显示这份帮助\n\
         \x20 -v, --version          显示版本"
    );
}

/// 解析 `--unknown=` 的取值。
fn parse_unknown_mode(value: &str) -> Option<UnknownDirective> {
    match value {
        "error" => Some(UnknownDirective::Error),
        "keep" => Some(UnknownDirective::Keep),
        "ignore" => Some(UnknownDirective::Ignore),
        _ => None,
    }
}

/// 读一个文本文件（和库里一样按 UTF-8 解释）。
fn read_text(path: &str) -> Option<String> {
    let bytes = fs::read(path).ok()?;
    Some(String::from_utf8_lossy(&bytes).into_owned())
}

/// 按逗号切一行（够演示用；真正的 CSV 要处理引号与转义）。
///
/// 空行给空列表 —— 与 C++ 版的 `std::getline` 行为对齐（`"".split(',')` 会给一个空串，
/// 而 `std::getline` 直接失败）。
fn split_csv(line: &str) -> Vec<String> {
    if line.is_empty() {
        return Vec::new();
    }
    line.split(',').map(|cell| cell.to_string()).collect()
}

/// 造一个演示用的 CSV loader。
///
/// 规则：只接管 `%import` 的 `.csv`；第一行当表头，之后每行生成
/// `row 列=值 列=值 …` 一行文本。想接别的数据源（JSON / 数据库 / 内存表）时，
/// 照这个形状改 `options.include_loader` 即可。
fn make_csv_loader() -> Box<dyn Fn(&IncludeRequest) -> IncludeResult> {
    Box::new(|request: &IncludeRequest| {
        // 1. 只接管 `%import` 的 .csv，其余交回默认逻辑。
        let is_csv = request.path.len() >= 4 && request.path.ends_with(".csv");
        if !is_csv || !request.imported {
            return IncludeResult::use_default();
        }
        if request.resolved_path.is_empty() {
            return IncludeResult::error(format!("找不到 CSV：{}", request.path));
        }

        // 2. 读文件：第一行当表头。
        let content = match read_text(&request.resolved_path) {
            Some(text) => text,
            None => {
                return IncludeResult::error(format!("读不了 CSV：{}", request.resolved_path));
            }
        };

        let mut lines = content.split('\n');
        let mut headers: Vec<String> = split_csv(lines.next().unwrap_or(""));
        // 表头可能带 UTF-8 BOM（Windows 记事本另存为 UTF-8 时常见），去掉它。
        if let Some(first) = headers.first_mut() {
            if first.as_bytes().starts_with(&[0xEF, 0xBB, 0xBF]) {
                *first = first[3..].to_string();
            }
        }

        let mut generated = String::new();
        let mut row_number = 0usize;
        for raw in lines {
            let mut line = raw;
            if line.ends_with('\r') {
                line = &line[..line.len() - 1];
            }
            let cells = split_csv(line);
            if cells.is_empty() {
                continue;
            }
            row_number += 1;
            generated.push_str(&format!("row{row_number}"));
            for (index, cell) in cells.iter().enumerate() {
                generated.push(' ');
                if index < headers.len() && !headers[index].is_empty() {
                    generated.push_str(&headers[index]);
                } else {
                    generated.push_str(&format!("col{}", index + 1));
                }
                generated.push('=');
                generated.push_str(cell);
            }
            generated.push('\n');
        }

        // 3. 用文件路径当标识：依赖表、去重、循环检测都认它。
        IncludeResult::inline_text_as(generated, request.resolved_path.clone())
    })
}

/// 把 `-U` 指定的符号从预定义里去掉。
fn remove_defines(options: &mut Options, names: &[String]) {
    for name in names {
        options.defines.retain(|define| &define.name != name);
    }
}

/// 程序入口。
fn main() {
    let arguments: Vec<String> = std::env::args().collect();
    let program = arguments
        .first()
        .cloned()
        .unwrap_or_else(|| "preprocess".to_string());

    let mut options = Options::default();
    let mut input = String::new();
    let mut output = String::new();
    let mut undefines: Vec<String> = Vec::new();
    let mut check_only = false;
    let mut list_deps = false;
    let mut show_stats = false;

    // 1. 解析参数：`--opt=value` 与 `--opt value` 都接受。
    let mut index = 1usize;
    while index < arguments.len() {
        let argument = arguments[index].clone();
        let (key, inline_value) = match argument.find('=') {
            Some(equal) => (
                argument[..equal].to_string(),
                argument[equal + 1..].to_string(),
            ),
            None => (argument.clone(), String::new()),
        };

        // 取选项的值：`--opt=v` 直接用，`--opt v` 取下一个参数。
        macro_rules! take_value {
            () => {{
                if !inline_value.is_empty() {
                    Some(inline_value.clone())
                } else if index + 1 >= arguments.len() {
                    eprintln!("选项 {} 缺少取值", key);
                    std::process::exit(2);
                } else {
                    index += 1;
                    Some(arguments[index].clone())
                }
            }};
        }

        match key.as_str() {
            "-h" | "--help" => {
                print_usage(&program);
                return;
            }
            "-v" | "--version" => {
                println!("preprocess-rust 1.0（ParseEngine library/preprocess-rust）");
                return;
            }
            "-o" | "--out" => {
                output = take_value!().unwrap();
                index += 1;
                continue;
            }
            "-D" | "--define" => {
                let text = take_value!().unwrap();
                let mut define = Define::default();
                if let Err(diagnostic) = Preprocessor::parse_define(&text, &mut define) {
                    eprintln!("选项 -D： {}", diagnostic.message);
                    std::process::exit(2);
                }
                options.defines.push(define);
                index += 1;
                continue;
            }
            "-U" | "--undef" => {
                undefines.push(take_value!().unwrap());
                index += 1;
                continue;
            }
            "-I" | "--include-dir" => {
                options.include_dirs.push(take_value!().unwrap());
                index += 1;
                continue;
            }
            "--prefix" => {
                let text = take_value!().unwrap();
                if text.is_empty() {
                    eprintln!("--prefix 不能为空");
                    std::process::exit(2);
                }
                options.directive_prefixes = text;
                index += 1;
                continue;
            }
            "--unknown" => {
                let text = take_value!().unwrap();
                match parse_unknown_mode(&text) {
                    Some(mode) => options.unknown_directive = mode,
                    None => {
                        eprintln!("--unknown 只接受 error / keep / ignore");
                        std::process::exit(2);
                    }
                }
                index += 1;
                continue;
            }
            "--max-depth" => {
                let text = take_value!().unwrap();
                match text.parse::<usize>() {
                    Ok(depth) => options.max_include_depth = depth,
                    Err(_) => {
                        eprintln!("--max-depth 只接受非负整数");
                        std::process::exit(2);
                    }
                }
                index += 1;
                continue;
            }
            "--metro" => {
                options.syntax = Syntax::Percent;
                options.percent_define_is_symbol = true;
            }
            "--hash" => options.syntax = Syntax::Hash,
            "--percent" => options.syntax = Syntax::Percent,
            "--grammar" => {
                options.syntax = Syntax::Percent;
                options.unknown_directive = UnknownDirective::Keep;
                options.opaque_regions = vec![
                    OpaqueRegion::new("%code", "%endcode", true),
                    OpaqueRegion::new("%class_body", "%endbody", true),
                ];
            }
            "--syntax" => {
                let text = take_value!().unwrap();
                match text.as_str() {
                    "auto" => options.syntax = Syntax::Auto,
                    "hash" => options.syntax = Syntax::Hash,
                    "percent" => options.syntax = Syntax::Percent,
                    "custom" => options.syntax = Syntax::Custom,
                    _ => {
                        eprintln!("--syntax 只接受 auto / hash / percent / custom");
                        std::process::exit(2);
                    }
                }
                index += 1;
                continue;
            }
            "--case-insensitive" => options.case_insensitive = true,
            "--no-include-once" => options.include_once_by_default = false,
            "--keep-directives" => options.keep_directive_lines = true,
            "--drop-lines" => options.preserve_lines = false,
            "--line-markers" => options.emit_line_markers = true,
            "--csv-to-lines" => options.include_loader = Some(make_csv_loader()),
            "--check" => check_only = true,
            "--deps" => list_deps = true,
            "--stats" => show_stats = true,
            _ => {
                if !argument.is_empty() && argument.starts_with('-') && input.is_empty() {
                    eprintln!("不认识的选项：{argument}");
                    std::process::exit(2);
                }
                if input.is_empty() {
                    input = argument;
                } else {
                    eprintln!("只能给一个输入文件（多余的是 {argument}）");
                    std::process::exit(2);
                }
            }
        }
        index += 1;
    }

    if input.is_empty() {
        print_usage(&program);
        std::process::exit(2);
    }

    remove_defines(&mut options, &undefines);

    // 2. 跑预处理。
    let preprocessor = Preprocessor::new(options);
    let mut result = PreprocessResult::default();
    let ok = preprocessor.run_file(&input, &mut result);

    // 3. 诊断一律写到 stderr，方便管道里只拿正文。
    for diagnostic in &result.diagnostics {
        eprintln!("{}", diagnostic.to_text());
    }

    if list_deps {
        for dependency in &result.dependencies {
            println!("{dependency}");
        }
    } else if !check_only {
        if output.is_empty() {
            let mut stdout = std::io::stdout();
            let _ = stdout.write_all(result.text.as_bytes());
            let _ = stdout.flush();
        } else if let Err(error) = fs::write(&output, result.text.as_bytes()) {
            eprintln!("写不了输出文件：{output}（{error}）");
            std::process::exit(2);
        }
    }

    if show_stats {
        eprintln!(
            "输入 {} 行，输出 {} 行，include {} 次，依赖 {} 个，诊断 {} 错 / {} 警",
            result.lines_in,
            result.lines_out,
            result.include_count,
            result.dependencies.len(),
            result.count(Severity::Error),
            result.count(Severity::Warning)
        );
    }

    if !ok {
        std::process::exit(1);
    }
}
