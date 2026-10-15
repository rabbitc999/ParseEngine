use simple_ast::{generate, Function, Node, Program, Type};

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let prefix = std::env::args()
        .nth(1)
        .unwrap_or_else(|| "out/simple_ast_demo".into());
    let mut program = Program::default();
    program.add_variable("numbers", Type::array(Type::i32(), "count=3;align=8"));
    program.add_variable("answer", Type::i32());
    program.functions.push(Function::new(
        "main",
        Node::block(vec![
            Node::assign(
                "numbers",
                Node::ArrayNew(
                    Type::array(Type::i32(), "count=3;align=8"),
                    vec![Node::i(10), Node::i(20), Node::i(30)],
                ),
            ),
            Node::assign(
                "answer",
                Node::Index(Box::new(Node::var("numbers")), Box::new(Node::i(2))),
            ),
            Node::write(Node::var("answer")),
            Node::ret(Node::i(0)),
        ]),
    ));
    let result = generate(&program, "simple_ast_demo");
    if !result.ok() {
        return Err(format!("生成失败：{:?}", result.diagnostics).into());
    }
    if let Some(parent) = std::path::Path::new(&prefix).parent() {
        std::fs::create_dir_all(parent)?;
    }
    std::fs::write(format!("{prefix}.quad"), &result.quads)?;
    std::fs::write(format!("{prefix}.data"), &result.data)?;
    std::fs::write(format!("{prefix}.data.bin"), result.export_data_binary())?;
    std::fs::write(
        format!("{prefix}.data.h"),
        result.export_data_header("simple_ast_demo"),
    )?;
    std::fs::write(
        format!("{prefix}.layout"),
        format!("{}\n", result.export_data_layout().join("\n")),
    )?;
    println!("已生成 {prefix}.quad 与数据区文件");
    Ok(())
}
