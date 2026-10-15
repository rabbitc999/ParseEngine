//! 移位 / 按位运算示例：读 core/example/operators.quad，生成并模拟，验证与期望一致。
//!
//! 运行：`cargo run --manifest-path library/transfer-rust/Cargo.toml --bin quad-transfer-operators`

use quad_transfer::{
    default_config, display_value, generate, load_quad_file, optimize_detailed, render_output,
    simulate, Target,
};

fn main() -> Result<(), String> {
    let default_path =
        std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../transfer/core/example/operators.quad");
    let path = std::env::args()
        .nth(1)
        .map(std::path::PathBuf::from)
        .unwrap_or(default_path);
    let source = load_quad_file(&path.to_string_lossy())?;
    let optimized = optimize_detailed(&source.program)?;
    let assembly = generate(&optimized.program, &default_config(Target::Arm))?;
    let execution = simulate(&assembly, &source.input, 1_000_000)?;
    let actual = render_output(&execution);

    println!("【示例】{}（{}）", source.name, path.display());
    println!("{assembly}");
    println!(
        "运行结果：{actual}（指令数 {}）{}",
        execution.steps,
        if actual == source.expect {
            "  [与期望一致]"
        } else {
            "  [与期望不一致]"
        }
    );
    if !execution.text.is_empty() {
        println!("文本输出：{}", execution.text);
    }
    let _ = display_value;
    Ok(())
}
