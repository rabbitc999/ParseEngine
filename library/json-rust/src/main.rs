//! `json-cli`：`library/json` 命令行工具的 Rust 版（对应 C++ 版 `main.cpp`）。
//!
//! ```sh
//! json-cli fmt   examples/config.json --indent 2
//! json-cli check examples/config.json
//! json-cli get   limits.maxDepth examples/config.json
//! json-cli keys  examples/config.json
//! json-cli stats examples/config.json
//! ```
//!
//! 命令、选项、退出码与输出文案都与 C++ 版一致（自检里就是拿后者当基准比的）：
//! 退出码 **0** 成功、**1** 解析或取值失败、**2** 用法错误。
//!
//! 唯一一处差异：C++ 版按**字节**读输入，Rust 版要求输入是合法 UTF-8（`Value::String`
//! 装的就是 `String`）——非 UTF-8 输入会报一行说明并以 1 退出，而不是硬解析下去。

use std::collections::BTreeMap;
use std::io::{Read, Write};
use std::process::ExitCode;

use json::{parse_with, ParseOptions, Value};

/// 程序入口。
fn main() -> ExitCode {
    ExitCode::from(run())
}

/// 跑一遍命令行，返回退出码。
fn run() -> u8 {
    let arguments: Vec<String> = std::env::args().collect();
    let program = arguments.first().cloned().unwrap_or_else(|| "json-cli".to_string());
    if arguments.len() < 2 {
        print_usage(&program);
        return 2;
    }

    // 1. 命令与参数。
    let command = arguments[1].clone();
    let mut path = String::new();
    let mut file = String::new();
    let mut output_file = String::new();
    let mut indent: i32 = 2;
    let mut compact = false;
    let mut raw = false;
    let mut positionals: Vec<String> = Vec::new();
    let mut options = ParseOptions::default();

    let mut index = 2;
    while index < arguments.len() {
        let argument = arguments[index].clone();
        if argument == "-h" || argument == "--help" {
            print_usage(&program);
            return 0;
        }
        if argument == "--indent" {
            if index + 1 >= arguments.len() {
                eprintln!("--indent 缺少取值");
                return 2;
            }
            index += 1;
            // C++ 版用 atoi：认不出来的就当 0，负数一律抬到 0。
            indent = arguments[index].parse::<i32>().unwrap_or(0).max(0);
            index += 1;
            continue;
        }
        if argument == "--compact" {
            compact = true;
            index += 1;
            continue;
        }
        if argument == "--raw" {
            raw = true;
            index += 1;
            continue;
        }
        if argument == "--out" {
            if index + 1 >= arguments.len() {
                eprintln!("--out 缺少取值");
                return 2;
            }
            index += 1;
            output_file = arguments[index].clone();
            index += 1;
            continue;
        }
        if argument == "--comments" {
            options.allow_comments = true;
            index += 1;
            continue;
        }
        if argument == "--trailing-commas" {
            options.allow_trailing_commas = true;
            index += 1;
            continue;
        }
        if argument.starts_with('-') && argument.len() > 1 {
            eprintln!("不认识的选项：{argument}");
            return 2;
        }
        positionals.push(argument);
        index += 1;
    }

    // 2. 位置参数按命令解释：
    //      fmt / check / stats   [文件]
    //      get <路径> [文件]
    //      keys [路径] [文件]    （只给一个时按文件算，要按路径查就写两个）
    if command == "get" {
        if positionals.is_empty() {
            eprintln!("get 需要路径，例如 get user.tags[0] config.json");
            return 2;
        }
        path = positionals[0].clone();
        if positionals.len() > 1 {
            file = positionals[1].clone();
        }
    } else if command == "keys" {
        if positionals.len() == 1 {
            file = positionals[0].clone();
        } else if positionals.len() >= 2 {
            path = positionals[0].clone();
            file = positionals[1].clone();
        }
    } else if !positionals.is_empty() {
        file = positionals[0].clone();
    }
    if positionals.len() > 2 {
        eprintln!("位置参数太多了：{}", positionals[2]);
        return 2;
    }

    // 3. 读输入并解析。
    let Some(text) = read_input(&file) else {
        return 2;
    };

    let doc = parse_with(&text, options);
    let root = match doc.into_value() {
        Ok(value) => value,
        Err(diagnostics) => {
            let source = if file.is_empty() { "<stdin>" } else { file.as_str() };
            for diagnostic in &diagnostics {
                eprintln!("{source}:{diagnostic}");
            }
            return 1;
        }
    };

    // 4. 分派命令（结果先攒成文本，最后统一写标准输出或 `--out` 的文件）。
    let mut output = String::new();
    match command.as_str() {
        "fmt" => {
            output.push_str(&root.dump_indent(if compact { -1 } else { indent }));
            output.push('\n');
        }
        "check" => {
            let count = if root.is_array() || root.is_object() { root.len() } else { 0 };
            output.push_str(&format!("JSON 有效：{}，顶层 {} 项\n", root.type_name(), count));
        }
        "get" => match root.at_path(&path) {
            Some(found) => {
                if raw && found.is_string() {
                    output.push_str(found.string_or(""));
                } else {
                    output.push_str(&found.dump_indent(indent));
                }
                output.push('\n');
            }
            None => {
                eprintln!("路径不存在：{path}");
                return 1;
            }
        },
        "keys" => {
            let target = if path.is_empty() { Some(&root) } else { root.at_path(&path) };
            let Some(target) = target else {
                eprintln!("路径不存在：{path}");
                return 1;
            };
            if let Some(object) = target.as_object() {
                for member in object.iter() {
                    output.push_str(&member.key);
                    output.push('\n');
                }
            } else if target.is_array() {
                for index in 0..target.len() {
                    output.push_str(&format!("[{index}]\n"));
                }
            } else {
                eprintln!("这个值既不是对象也不是数组：{}", target.type_name());
                return 1;
            }
        }
        "stats" => {
            let mut counters: BTreeMap<String, usize> = BTreeMap::new();
            let mut max_depth = 0usize;
            collect_stats(&root, &mut counters, 1, &mut max_depth);
            for (name, count) in &counters {
                output.push_str(&format!("{name}: {count}\n"));
            }
            output.push_str(&format!("maxdepth: {max_depth}\n"));
        }
        other => {
            eprintln!("不认识的命令：{other}");
            print_usage(&program);
            return 2;
        }
    }

    // 5. 输出。
    if output_file.is_empty() {
        let _ = std::io::stdout().write_all(output.as_bytes());
    } else if std::fs::write(&output_file, output.as_bytes()).is_err() {
        eprintln!("写不了输出文件：{output_file}");
        return 2;
    }
    0
}

/// 打印用法（文案与 C++ 版逐行一致）。
fn print_usage(program: &str) {
    let lines = [
        format!("用法：{program} <命令> [文件] [选项]"),
        String::new(),
        "命令：".to_string(),
        "  fmt              格式化输出（--indent N 缩进，--compact 单行）".to_string(),
        "  check            只校验，打印诊断".to_string(),
        "  get <路径>       按路径取值，例如 user.tags[1].name".to_string(),
        "  keys [路径]      列出对象成员名（一行一个）".to_string(),
        "  stats            统计各类型节点数与最大深度".to_string(),
        String::new(),
        "选项：".to_string(),
        "  --indent N       缩进空格数（缺省 2）".to_string(),
        "  --compact        单行紧凑输出".to_string(),
        "  --raw            get 时字符串不加引号".to_string(),
        "  --out FILE       把命令输出写到文件（缺省标准输出）".to_string(),
        "  --comments       允许 // 与块注释".to_string(),
        "  --trailing-commas 允许末尾逗号".to_string(),
        "  -h, --help       显示这份帮助".to_string(),
        String::new(),
        "文件缺省从标准输入读。".to_string(),
    ];
    println!("{}", lines.join("\n"));
}

/// 读一个文件或标准输入（`path` 为空表示标准输入）。
///
/// 失败时已经打过错误信息，返回 `None`。
fn read_input(path: &str) -> Option<String> {
    let bytes = if path.is_empty() {
        let mut buffer = Vec::new();
        if std::io::stdin().read_to_end(&mut buffer).is_err() {
            eprintln!("读不了标准输入");
            return None;
        }
        buffer
    } else {
        match std::fs::read(path) {
            Ok(bytes) => bytes,
            Err(_) => {
                eprintln!("打不开文件：{path}");
                return None;
            }
        }
    };
    match String::from_utf8(bytes) {
        Ok(text) => Some(text),
        Err(_) => {
            eprintln!("输入不是合法的 UTF-8（Rust 版的字符串是 UTF-8，C++ 版按字节读）");
            None
        }
    }
}

/// 统计一个值的类型分布与最大深度（顺带演示迭代器的用法；对应 C++ 版 `collectStats`）。
fn collect_stats(
    value: &Value,
    counters: &mut BTreeMap<String, usize>,
    depth: usize,
    max_depth: &mut usize,
) {
    *max_depth = (*max_depth).max(depth);
    *counters.entry(value.type_name().to_string()).or_insert(0) += 1;

    // 1. 数组：数一下"有多少个是容器"，再逐个往里走。
    if let Some(items) = value.as_array() {
        *counters.entry("array.items".to_string()).or_insert(0) += items.len();
        let nested = items.iter().filter(|item| item.is_array() || item.is_object()).count();
        *counters.entry("array.nested".to_string()).or_insert(0) += nested;
        for item in items {
            collect_stats(item, counters, depth + 1, max_depth);
        }
        return;
    }

    // 2. 对象：按插入顺序遍历成员。
    if let Some(object) = value.as_object() {
        *counters.entry("object.members".to_string()).or_insert(0) += object.len();
        for member in object.iter() {
            collect_stats(&member.value, counters, depth + 1, max_depth);
        }
    }
}
