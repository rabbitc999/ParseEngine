//! 最小示例：读 core/example/minimal.quad -> 优化 -> 生成 ARM 风格汇编 -> 模拟执行。
//!
//! 运行：`cargo run --manifest-path library/transfer-rust/Cargo.toml`

use quad_transfer::{
    analyze, default_config, display_value, generate, load_quad_file, optimize_detailed, simulate,
    write_quads, Target,
};

fn main() -> Result<(), String> {
    let default_path =
        std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../transfer/core/example/minimal.quad");
    let path = std::env::args()
        .nth(1)
        .map(std::path::PathBuf::from)
        .unwrap_or(default_path);
    let source = load_quad_file(&path.to_string_lossy())?;
    println!("【读入的示例】{}（{}）", source.name, path.display());
    println!("{}", write_quads(&source.program));

    println!("基本块数：{}", analyze(&source.program)?.blocks.len());
    let optimized = optimize_detailed(&source.program)?;
    println!("【优化后四元式】\n{}", write_quads(&optimized.program));

    let assembly = generate(&optimized.program, &default_config(Target::Arm))?;
    println!("【生成的 ARM 风格汇编】\n{assembly}");

    let execution = simulate(&assembly, &source.input, 1_000_000)?;
    let output = execution
        .output
        .iter()
        .map(display_value)
        .collect::<Vec<_>>()
        .join(" ");
    println!("【运行结果】{output}");
    if !execution.text.is_empty() {
        println!("【文本输出】{}", execution.text);
    }
    Ok(())
}
