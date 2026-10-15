// 基本块 / CFG 分析与保守优化（由 lib.rs 用 include! 拼进同一模块）。

/// 划分基本块并构造 CFG；`call`/`int` 连到被调用函数，`ret`/`iret` 是出口。
pub fn analyze(program: &Program) -> Result<Analysis, String> {
    validate(program)?;
    let mut analysis = Analysis::default();
    if program.is_empty() {
        return Ok(analysis);
    }
    let mut leaders: HashSet<usize> = HashSet::from([0]);
    let mut labels: HashMap<String, usize> = HashMap::new();
    for (index, quad) in program.iter().enumerate() {
        let base = op_base(&quad.op);
        if base == "label" {
            leaders.insert(index);
            labels.insert(quad.result.clone(), index);
        }
        if matches!(base, "j" | "jz" | "jnz" | "ret" | "iret" | "halt") && index + 1 < program.len() {
            leaders.insert(index + 1);
        }
    }
    let mut starts: Vec<usize> = leaders.into_iter().collect();
    starts.sort_unstable();
    for (order, begin) in starts.iter().enumerate() {
        let end = starts.get(order + 1).copied().unwrap_or(program.len());
        analysis.blocks.push(Block { begin: *begin, end, ..Default::default() });
        for _ in *begin..end {
            analysis.instruction_block.push(order);
        }
    }
    for order in 0..analysis.blocks.len() {
        let block = analysis.blocks[order].clone();
        let last = &program[block.end - 1];
        let base = op_base(&last.op);
        if matches!(base, "j" | "jz" | "jnz") {
            if let Some(target) = labels.get(&last.result) {
                analysis.blocks[order].successors.push(analysis.instruction_block[*target]);
            }
        }
        if !matches!(base, "j" | "halt" | "ret" | "iret") && order + 1 < analysis.blocks.len() {
            analysis.blocks[order].successors.push(order + 1);
        }
        for index in block.begin..block.end {
            let inner = op_base(&program[index].op);
            if (inner == "call" || inner == "int") && labels.contains_key(&program[index].a) {
                let target = labels[&program[index].a];
                analysis.blocks[order].successors.push(analysis.instruction_block[target]);
            }
        }
        analysis.blocks[order].successors.sort_unstable();
        analysis.blocks[order].successors.dedup();
    }
    for order in 0..analysis.blocks.len() {
        let successors = analysis.blocks[order].successors.clone();
        for successor in successors {
            analysis.blocks[successor].predecessors.push(order);
        }
    }
    let mut stack = vec![0usize];
    while let Some(order) = stack.pop() {
        if analysis.blocks[order].reachable {
            continue;
        }
        analysis.blocks[order].reachable = true;
        stack.extend(analysis.blocks[order].successors.iter().copied());
    }
    Ok(analysis)
}

/// 把 CFG 与可达性输出成可读报告。
pub fn report(program: &Program, analysis: &Analysis) -> String {
    let mut out = String::new();
    for (index, block) in analysis.blocks.iter().enumerate() {
        let successors = block
            .successors
            .iter()
            .map(|value| value.to_string())
            .collect::<Vec<_>>()
            .join(", ");
        let predecessors = block
            .predecessors
            .iter()
            .map(|value| value.to_string())
            .collect::<Vec<_>>()
            .join(", ");
        out.push_str(&format!(
            "B{index} [{}, {}) {}\n  pred={{{predecessors}}} succ={{{successors}}}\n",
            block.begin,
            block.end,
            if block.reachable { "reachable" } else { "unreachable" }
        ));
    }
    out.push('\n');
    out.push_str(&write_quads(program));
    out
}

fn power_of_two(text: &str) -> Option<u32> {
    let value: i128 = text.parse().ok()?;
    if value < 2 || value & (value - 1) != 0 {
        return None;
    }
    Some(value.trailing_zeros())
}

/// 保守优化：常量传播 / 折叠、代数化简、强度削弱，直到不再变化。
pub fn optimize_detailed(input: &Program) -> Result<Optimization, String> {
    validate(input)?;
    let mut current = input.clone();
    let mut trace = Vec::new();
    for round in 0..100 {
        let before = write_quads(&current);
        let declared = variable_types(&current);
        // 常量表带上类型：只在类型一致时传播（cvt 两边类型不同的情况不传播）。
        let mut constants: HashMap<String, (String, Value)> = HashMap::new();
        let mut out: Program = Vec::new();
        for quad in &current {
            let mut q = quad.clone();
            let base = op_base(&q.op).to_string();
            if base == "call" || base == "int" {
                // 被调用者可能改写全局变量：清掉常量表。
                constants.clear();
            }
            if base == "decl" {
                out.push(q);
                continue;
            }
            // 常量传播：只把字面量赋值传播到后面的同名操作数。
            if base == ":=" && is_numeric_literal(&q.a) {
                let ty = operation_type(&q, &declared);
                if let Ok(value) = literal_for(&q.a, &ty) {
                    constants.insert(q.result.clone(), (ty.clone(), value));
                }
            } else if !q.result.is_empty() && q.result != "_" {
                constants.remove(&q.result);
            }
            if !q.extra.is_empty() && q.extra != "_" {
                constants.remove(&q.extra);
            }
            if (is_binary_base(&base) || is_unary_base(&base)) && !matches!(base.as_str(), "+c" | "-b") {
                let ty = operation_type(&q, &declared);
                let rtype = result_type(&q, &declared);
                // 传常量
                if let Some((constant_type, value)) = constants.get(&q.a) {
                    let text = display_value(value);
                    if *constant_type != ty {
                        let text = text;
                        let _ = text;
                    } else {
                    q.a = if is_numeric_literal(&text) { text } else { q.a.clone() };
                    }
                }
                if is_binary_base(&base) {
                    if let Some((constant_type, value)) = constants.get(&q.b) {
                        let text = display_value(value);
                        if *constant_type == ty {
                            q.b = if is_numeric_literal(&text) { text } else { q.b.clone() };
                        }
                    }
                }
                // 折叠
                let wants_second = is_binary_base(&base) && !matches!(base.as_str(), "neg" | "odd" | "~" | "cvt");
                if is_numeric_literal(&q.a) && (!wants_second || is_numeric_literal(&q.b)) {
                    let lhs = literal_for(&q.a, &ty);
                    let rhs = if wants_second { literal_for(&q.b, &ty) } else { Ok(Value::zero(&ty)) };
                    if let (Ok(lhs), Ok(rhs)) = (lhs, rhs) {
                        let folded = if base == "cvt" {
                            convert_value(&lhs, &ty)
                        } else {
                            eval_typed(&base, &ty, &lhs, &rhs)
                        };
                        if let Ok(value) = folded {
                            let text = display_value(&value);
                            q.op = typed_op(":=", &ty);
                            q.a = text;
                            q.b = "_".into();
                            q.extra = "_".into();
                        }
                    }
                }
                // 代数化简与强度削弱
                let integer = !is_float_type(&ty);
                let fresh_base = op_base(&q.op).to_string();
                if fresh_base == "*" && integer {
                    if let Some(exponent) = power_of_two(&q.b) {
                        q.op = typed_op("<<", &ty);
                        q.b = exponent.to_string();
                    } else if let Some(exponent) = power_of_two(&q.a) {
                        q.a = q.b.clone();
                        q.b = exponent.to_string();
                        q.op = typed_op("<<", &ty);
                    }
                }
                let fresh_base = op_base(&q.op).to_string();
                let mut value: Option<String> = None;
                if ((fresh_base == "+" || fresh_base == "-" || fresh_base == "<<" || fresh_base == ">>")
                    && q.b == "0")
                    || ((fresh_base == "*" || fresh_base == "/") && q.b == "1")
                {
                    value = Some(q.a.clone());
                }
                if (fresh_base == "+" && q.a == "0") || (fresh_base == "*" && q.a == "1") {
                    value = Some(q.b.clone());
                }
                if integer {
                    if (matches!(fresh_base.as_str(), "|" | "^") && q.b == "0")
                        || (fresh_base == "&" && q.b == "-1")
                    {
                        value = Some(q.a.clone());
                    }
                    if (matches!(fresh_base.as_str(), "|" | "^") && q.a == "0")
                        || (fresh_base == "&" && q.a == "-1")
                    {
                        value = Some(q.b.clone());
                    }
                    if fresh_base == "|" && (q.a == "-1" || q.b == "-1") {
                        value = Some("-1".into());
                    }
                    if fresh_base == "&" && (q.a == "0" || q.b == "0") {
                        value = Some("0".into());
                    }
                    if matches!(fresh_base.as_str(), "<<" | ">>") && q.a == "0" {
                        value = Some("0".into());
                    }
                    if matches!(fresh_base.as_str(), "&" | "|") && q.a == q.b {
                        value = Some(q.a.clone());
                    }
                    if fresh_base == "^" && q.a == q.b {
                        value = Some("0".into());
                    }
                }
                if let Some(text) = value {
                    q.op = typed_op(":=", &rtype);
                    q.a = text;
                    q.b = "_".into();
                }
            }
            out.push(q);
        }
        let after = write_quads(&out);
        if after == before {
            return Ok(Optimization { program: current, trace, rounds: round + 1 });
        }
        trace.push(format!("round {}: constant/algebra/strength optimization", round + 1));
        current = out;
    }
    Err("optimizer did not converge".into())
}

pub fn optimize(program: &Program) -> Result<Program, String> {
    Ok(optimize_detailed(program)?.program)
}
