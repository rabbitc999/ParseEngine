//! 示例集运行器：读 core/example/*.quad，逐个优化 + 生成 + 模拟并比对期望。
//!
//! 运行：`cargo run --manifest-path library/transfer-rust/Cargo.toml --bin quad-transfer-suite`

use quad_transfer::{
    default_config, generate, load_config_file, load_data_area, load_quad_file,
    optimize_detailed, render_output, resolve_config_path, simulate, Target,
};

fn main() -> Result<(), String> {
    // crate 在 library/transfer-rust/，四元式库本体（core/example 等）在
    // library/transfer/，所以要从 crate 目录往上一层的 transfer/ 走。
    let transfer_dir = std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
        .parent()
        .ok_or("无法定位 library/transfer-rust 的上级目录")?
        .join("transfer");
    let default_directory = transfer_dir.join("core/example");
    let directory = std::env::args()
        .nth(1)
        .map(std::path::PathBuf::from)
        .unwrap_or(default_directory);
    let directory = if directory.is_absolute() {
        directory
    } else {
        std::env::current_dir()
            .map_err(|error| format!("无法读取当前目录：{error}"))?
            .join(directory)
    };
    // 与 `make -C library/transfer check` 保持一致，让示例里的相对文件路径
    // （例如 `../../build/transfer-one-thread/example_io.txt`）按库根目录解析。
    std::env::set_current_dir(&transfer_dir)
        .map_err(|error| format!("无法切换到 {}：{error}", transfer_dir.display()))?;
    let mut files: Vec<std::path::PathBuf> = std::fs::read_dir(&directory)
        .map_err(|error| format!("无法读取目录 {}：{error}", directory.display()))?
        .filter_map(|entry| entry.ok().map(|item| item.path()))
        .filter(|path| path.extension().map(|ext| ext == "quad").unwrap_or(false))
        .collect();
    files.sort();
    if files.is_empty() {
        return Err(format!("目录 {} 里没有 .quad 示例", directory.display()));
    }
    let mut passed = 0;
    let mut failed = 0;
    println!("示例目录：{}，目标机：arm", directory.display());
    for file in &files {
        let name = file
            .file_name()
            .unwrap_or_default()
            .to_string_lossy()
            .to_string();
        let outcome = (|| -> Result<Option<bool>, String> {
            // 负例（`# 期望错误:`）在 Rust 版里一律跳过：有的负例在**校验期**就报错
            // （比如"未知四元式"），连 `load_quad_file` 都过不去，所以先看原文有没有
            // 这个标记，再看解析出来的字段。
            let text = std::fs::read_to_string(file).map_err(|error| error.to_string())?;
            let negative = text
                .lines()
                .any(|line| line.trim_start().starts_with("# 期望错误"));
            let source = match load_quad_file(&file.to_string_lossy()) {
                Ok(source) => source,
                Err(error) if negative => {
                    println!("[跳过] {name} —— 期望报错的负例（由 C++ 版校验）：{error}");
                    return Ok(None);
                }
                Err(error) => return Err(error),
            };
            if !source.expect_error.is_empty() {
                println!("[跳过] {name} —— 期望报错的负例（由 C++ 版检查）");
                return Ok(None);
            }
            let optimized = optimize_detailed(&source.program)?;
            let mut config = if source.config_path.is_empty() {
                default_config(Target::Arm)
            } else {
                let config_path = resolve_config_path(&source.path, &source.config_path);
                load_config_file(&config_path, Target::Arm)?
            };
            if !source.data_path.is_empty() {
                // `# 数据:` / `# 布局:`：装数据区字节镜像与符号表（addr / load / store 用）。
                let bin = resolve_config_path(&source.path, &source.data_path);
                let layout = resolve_config_path(&source.path, &source.layout_path);
                load_data_area(&mut config, &bin, &layout)?;
            }
            let assembly = generate(&optimized.program, &config)?;
            let execution = simulate(&assembly, &source.input, 1_000_000)?;
            let actual = render_output(&execution);
            let ok = actual == source.expect
                && (source.expect_text.is_empty() || execution.text == source.expect_text);
            println!(
                "{} {name} —— {}",
                if ok { "[通过]" } else { "[失败]" },
                source.name
            );
            if !ok {
                println!("        期望输出：{}，实际输出：{actual}", source.expect);
                if !source.expect_text.is_empty() && execution.text != source.expect_text {
                    println!(
                        "        期望文本：{}，实际文本：{}",
                        source.expect_text, execution.text
                    );
                }
            }
            Ok(Some(ok))
        })();
        match outcome {
            Ok(Some(true)) => passed += 1,
            Ok(Some(false)) => failed += 1,
            Ok(None) => {}
            Err(error) => {
                // 这里只该滤掉两类：**范围外**与**负例**（`# 期望错误:`，由 C++ 版校验）。
                // 数据区三件套 `addr` / `load` / `store` **已经实现**，所以「未知四元式」
                // 「指令族」这两类不再当跳过 —— 它们现在只可能是真失败。
                // 实测（core/example 26 份）：23 通过 / 0 失败 / 3 跳过。
                let unsupported = error.contains("unsupported operation")
                    || error.contains("需要字符串指令")
                    || error.contains("需要向量指令")
                    || error.contains("Unknown instruction");
                if unsupported {
                    println!("[跳过] {name} —— Rust 版暂不支持：{error}");
                } else {
                    println!("[失败] {name} —— {error}");
                    failed += 1;
                }
            }
        }
    }
    println!("共 {} 个示例：{passed} 通过，{failed} 失败。", files.len());
    if failed == 0 {
        Ok(())
    } else {
        Err("存在失败示例".into())
    }
}
