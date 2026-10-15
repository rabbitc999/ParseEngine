use stackvm::{assemble, load_quad_file, optimize, DataArea, Machine};

fn main() {
    if let Err(message) = execute() {
        eprintln!("{message}");
        std::process::exit(1);
    }
}

fn execute() -> Result<(), String> {
    let mut args = std::env::args().skip(1).peekable();
    let mut command = "run".to_string();
    if args
        .peek()
        .is_some_and(|s| ["run", "check", "disasm"].contains(&s.as_str()))
    {
        command = args.next().unwrap();
    }
    let path=args.next().ok_or("用法：stackvm [run|check|disasm] file.quad [--input TEXT] [--data FILE] [--layout FILE] [--no-opt] [--trace] [--limit N] [--expected]")?;
    let mut input = None;
    let mut data = None;
    let mut layout = None;
    let mut no_opt = false;
    let mut trace = false;
    let mut expected = false;
    let mut limit = 1_000_000;
    while let Some(option) = args.next() {
        match option.as_str() {
            "--input" => input = Some(args.next().ok_or("--input 缺少取值")?),
            "--data" => data = Some(args.next().ok_or("--data 缺少取值")?),
            "--layout" => layout = Some(args.next().ok_or("--layout 缺少取值")?),
            "--no-opt" => no_opt = true,
            "--trace" => trace = true,
            "--expected" => expected = true,
            "--limit" => {
                limit = args
                    .next()
                    .ok_or("--limit 缺少取值")?
                    .parse()
                    .map_err(|_| "--limit 不是整数")?
            }
            "--stats" => {}
            other => return Err(format!("不认识的选项：{other}")),
        }
    }
    let mut source = load_quad_file(&path);
    if !source.ok() {
        return Err(source.diagnostics.join("\n"));
    }
    if !no_opt {
        optimize(&mut source, 4);
    }
    let program = assemble(&source);
    if !program.ok() {
        return Err(program.diagnostics.join("\n"));
    }
    if command == "check" {
        println!(
            "可组装：{} 条栈指令，{} 个标签，{} 个变量声明",
            program.instructions.len(),
            program.stack_labels.len(),
            program.types.len()
        );
        return Ok(());
    }
    if command == "disasm" {
        println!(
            "; {}",
            if source.name.is_empty() {
                &path
            } else {
                &source.name
            }
        );
        for (index, instruction) in program.instructions.iter().enumerate() {
            println!("{index}: {}", instruction.to_text());
        }
        return Ok(());
    }
    let mut vm = Machine::new(trace);
    if let Some(binary) = data {
        let layout = layout.unwrap_or_else(|| format!("{binary}.layout"));
        vm.set_data(DataArea::load(&binary, &layout).map_err(|e| format!("数据区加载失败：{e}"))?);
    }
    let text = input.unwrap_or_else(|| source.input.clone());
    let values: Vec<String> = text
        .split(|c: char| c.is_whitespace() || c == ',')
        .filter(|s| !s.is_empty())
        .map(str::to_string)
        .collect();
    let result = vm.run(&program, &values, limit);
    if trace {
        for line in &result.trace {
            println!("{line}");
        }
    }
    if !result.output.is_empty() {
        println!("{}", result.output);
    }
    if !result.text.is_empty() {
        println!("{}", result.text);
    }
    if !result.ok {
        return Err(format!("运行失败：{}", result.error));
    }
    if expected && !source.expected.is_empty() {
        println!(
            "{}",
            if result.output == source.expected {
                "成功"
            } else {
                "失败"
            }
        );
    }
    Ok(())
}
