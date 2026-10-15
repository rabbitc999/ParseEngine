// 目标代码生成（由 lib.rs 用 include! 拼进同一模块）。

fn register_like(name: &str) -> bool {
    if name.is_empty() {
        return false;
    }
    if name.starts_with('$') {
        return true;
    }
    if matches!(
        name,
        "AX" | "BX" | "CX" | "DX" | "AL" | "AH" | "BL" | "BH" | "CL" | "CH" | "DL" | "DH"
            | "SI" | "DI" | "SP" | "BP" | "IP" | "HI" | "LO" | "ZR"
    ) {
        return true;
    }
    name.len() > 1
        && matches!(name.chars().next(), Some('r') | Some('R'))
        && name[1..].chars().all(|c| c.is_ascii_digit())
}

fn default_load(target: Target, ty: &str) -> String {
    let bits = type_bits(ty);
    let unsigned = is_unsigned_type(ty);
    if is_float_type(ty) {
        return match target {
            Target::I8086 => "MOV {dst}, [{name}]".into(),
            Target::Arm => {
                if bits == 32 { "VLDR {dst}, [{name}]".into() } else { "VLDRD {dst}, [{name}]".into() }
            }
            Target::Mips => {
                if bits == 32 { "LWC1 {dst}, {name}".into() } else { "LDC1 {dst}, {name}".into() }
            }
        };
    }
    match target {
        Target::I8086 => "MOV {dst}, [{name}]".into(),
        Target::Arm => match bits {
            8 => format!("{} {{dst}}, [{{name}}]", if unsigned { "LDRB" } else { "LDRSB" }),
            16 => format!("{} {{dst}}, [{{name}}]", if unsigned { "LDRH" } else { "LDRSH" }),
            32 => "LDR {dst}, [{name}]".into(),
            64 => "LDRD {dst}, [{name}]".into(),
            _ => "LDRQ {dst}, [{name}]".into(),
        },
        Target::Mips => match bits {
            8 => format!("{} {{dst}}, {{name}}", if unsigned { "LBU" } else { "LB" }),
            16 => format!("{} {{dst}}, {{name}}", if unsigned { "LHU" } else { "LH" }),
            32 => "LW {dst}, {name}".into(),
            64 => "LD {dst}, {name}".into(),
            _ => "LQ {dst}, {name}".into(),
        },
    }
}

fn default_store(target: Target, ty: &str) -> String {
    let bits = type_bits(ty);
    if is_float_type(ty) {
        return match target {
            Target::I8086 => "MOV [{name}], {src}".into(),
            Target::Arm => {
                if bits == 32 { "VSTR {src}, [{name}]".into() } else { "VSTRD {src}, [{name}]".into() }
            }
            Target::Mips => {
                if bits == 32 { "SWC1 {src}, {name}".into() } else { "SDC1 {src}, {name}".into() }
            }
        };
    }
    match target {
        Target::I8086 => "MOV [{name}], {src}".into(),
        Target::Arm => match bits {
            8 => "STRB {src}, [{name}]".into(),
            16 => "STRH {src}, [{name}]".into(),
            32 => "STR {src}, [{name}]".into(),
            64 => "STRD {src}, [{name}]".into(),
            _ => "STRQ {src}, [{name}]".into(),
        },
        Target::Mips => match bits {
            8 => "SB {src}, {name}".into(),
            16 => "SH {src}, {name}".into(),
            32 => "SW {src}, {name}".into(),
            64 => "SD {src}, {name}".into(),
            _ => "SQ {src}, {name}".into(),
        },
    }
}

fn branch_mnemonic(target: Target, base: &str) -> &'static str {
    let condition = match base {
        "==" => "EQ",
        "!=" => "NE",
        "<" => "LT",
        "<=" => "LE",
        ">" => "GT",
        _ => "GE",
    };
    match (target, condition) {
        (Target::I8086, "EQ") => "JE",
        (Target::I8086, "NE") => "JNE",
        (Target::I8086, "LT") => "JL",
        (Target::I8086, "LE") => "JLE",
        (Target::I8086, "GT") => "JG",
        (Target::I8086, _) => "JGE",
        (_, "EQ") => "BEQ",
        (_, "NE") => "BNE",
        (_, "LT") => "BLT",
        (_, "LE") => "BLE",
        (_, "GT") => "BGT",
        (_, _) => "BGE",
    }
}

struct Generator<'a> {
    out: String,
    config: &'a MachineConfig,
    declared: HashMap<String, String>,
    types: HashMap<String, String>,
    registers: HashMap<String, String>,
    word_type: String,
}

impl<'a> Generator<'a> {
    fn word(&self) -> u32 {
        self.config.word_bits
    }

    fn emit(&mut self, text: String) {
        self.out.push_str(&format!("    {text}\n"));
    }

    /// 展开一条命令模板：可能是多条目标指令（模板里的换行）。
    /// `fill` 顺序是 [dst, dst2, lhs, rhs, src, name, value]。
    fn emit_lines(&mut self, template: &str, fill: [&str; 7], ty: &str) {
        for line in template.lines() {
            let line = line.trim();
            if line.is_empty() {
                continue;
            }
            let mut text = line.to_string();
            for (key, value) in [
                ("{dst}", fill[0]),
                ("{dst2}", fill[1]),
                ("{lhs}", fill[2]),
                ("{rhs}", fill[3]),
                ("{src}", fill[4]),
                ("{name}", fill[5]),
                ("{value}", fill[6]),
            ] {
                text = text.replace(key, value);
            }
            self.emit(with_type(&text, ty));
        }
    }

    /// 机器能力检查：四元式合法但机器没有对应指令族时报错。
    fn require_feature(&self, feature: &str, index: usize, quad: &Quad) -> Result<(), String> {
        if self.config.supports(feature) {
            return Ok(());
        }
        Err(format!(
            "四元式 {} ({}) 存在，但目标机 {} 没有实现指令族 '{}'（配置里可用 feature.{}=on 打开）",
            index, quad.op, self.config.target, feature, feature
        ))
    }

    /// 值操作数的类型：字符串字面量是 str，数字字面量按默认数值类型。
    fn value_operand_type(&self, operand: &str) -> String {
        if is_string_literal(operand) {
            return "str".to_string();
        }
        if is_numeric_literal(operand) {
            return default_type().to_string();
        }
        self.type_of(operand)
    }

    fn wide(&self, ty: &str) -> bool {
        !is_float_type(ty) && type_bits(ty) > self.word()
    }

    fn words_of(&self, ty: &str) -> u32 {
        (type_bits(ty) + self.word() - 1) / self.word()
    }

    fn type_of(&self, name: &str) -> String {
        self.types.get(name).cloned().unwrap_or_else(|| default_type().into())
    }

    fn source_type(&self, operand: &str, quad: &Quad) -> String {
        if is_numeric_literal(operand) {
            operation_type(quad, &self.declared)
        } else {
            self.type_of(operand)
        }
    }

    fn is_register(&self, name: &str) -> bool {
        self.registers.contains_key(name) || register_like(name)
    }

    fn map_register(&self, name: &str) -> String {
        self.registers.get(name).cloned().unwrap_or_else(|| name.to_string())
    }

    fn load_template(&self, ty: &str) -> String {
        if ty == default_type() {
            command(self.config, "memory_load", "LH {dst}, {name}")
        } else {
            command(self.config, &format!("load.{ty}"), &default_load(self.config.target, ty))
        }
    }

    fn store_template(&self, ty: &str) -> String {
        if ty == default_type() {
            command(self.config, "memory_store", "SH {src}, {name}")
        } else {
            command(self.config, &format!("store.{ty}"), &default_store(self.config.target, ty))
        }
    }

    fn load_to(&mut self, reg: &str, operand: &str, ty: &str) {
        if self.is_register(operand) {
            let resolved = self.map_register(operand);
            if resolved != reg {
                self.emit(with_type(
                    &format!("MOV {reg}, {resolved}"),
                    &format!("i{}", self.word()),
                ));
            }
        } else if is_numeric_literal(operand) {
            let template = command(self.config, "immediate_load", "LI {dst}, {value}");
            self.emit_lines(&template, [reg, "", "", "", "", "", operand], ty);
        } else {
            let template = self.load_template(ty);
            self.emit_lines(&template, [reg, "", "", "", "", operand, ""], ty);
        }
    }

    fn store_to(&mut self, destination: &str, reg: &str, ty: &str) {
        if self.is_register(destination) {
            let resolved = self.map_register(destination);
            if resolved != reg {
                self.emit(with_type(
                    &format!("MOV {resolved}, {reg}"),
                    &format!("i{}", self.word()),
                ));
            }
            return;
        }
        let template = self.store_template(ty);
        self.emit_lines(&template, ["", "", "", "", reg, destination, ""], ty);
    }

    /// 立即数 -> 目标（窄类型：一条立即数装载 + 一条存储）。
    fn store_immediate(&mut self, destination: &str, literal: &str, value_type: &str, target_type: &str) {
        let template = command(self.config, "immediate_load", "LI {dst}, {value}");
        let destination = destination.to_string();
        let reg = self.config.reg_a.clone();
        self.emit_lines(&template, [&reg, "", "", "", "", "", literal], value_type);
        let store = self.store_template(target_type);
        self.emit_lines(&store, ["", "", "", "", &reg, &destination, ""], target_type);
    }

    fn load_word(&mut self, reg: &str, variable: &str, index: u32) {
        let template = self.load_template(&self.word_type.clone());
        let word_type = self.word_type.clone();
        let name = format!("{variable}#{index}");
        self.emit_lines(&template, [reg, "", "", "", "", &name, ""], &word_type);
    }

    fn store_word(&mut self, variable: &str, index: u32, reg: &str) {
        let template = self.store_template(&self.word_type.clone());
        let word_type = self.word_type.clone();
        let name = format!("{variable}#{index}");
        self.emit_lines(&template, ["", "", "", "", reg, &name, ""], &word_type);
    }

    fn load_word_operand(&mut self, reg: &str, operand: &str, ty: &str, index: u32) {
        if !is_numeric_literal(operand) {
            self.load_word(reg, operand, index);
            return;
        }
        let value = literal_for(operand, ty).unwrap_or_else(|_| Value::zero(ty));
        let part = (value.bits >> (index * self.word())) & mask_of(self.word());
        let template = command(self.config, "immediate_load", "LI {dst}, {value}");
        let word_type = self.word_type.clone();
        let part_text = part.to_string();
        self.emit_lines(&template, [reg, "", "", "", "", "", &part_text], &word_type);
    }

}

/// 生成教学汇编：类型后缀、分片访问、进位链、固定寄存器对与运行时伪指令。
pub fn generate(program: &Program, config: &MachineConfig) -> Result<String, String> {
    validate(program)?;
    let word = config.word_bits;
    if ![8, 16, 32, 64].contains(&word) {
        return Err(format!("word_bits must be 8, 16, 32 or 64; got {word}"));
    }
    let declared = variable_types(program);
    let mut scopes: HashMap<String, String> = HashMap::new();
    for quad in program {
        if op_base(&quad.op) == "decl" {
            scopes.insert(
                quad.result.clone(),
                if quad.b == "local" { "local".to_string() } else { "global".to_string() },
            );
        }
    }
    let mut types = declared.clone();
    for quad in program {
        let base = op_base(&quad.op);
        if (is_binary_base(base)
            || is_unary_base(base)
            || matches!(base, ":=" | "read" | "getc" | "call" | "int"))
            && identifier(&quad.result)
            && !types.contains_key(&quad.result)
        {
            types.insert(quad.result.clone(), result_type(quad, &declared));
        }
        if is_pair_base(base) && identifier(&quad.extra) && !types.contains_key(&quad.extra) {
            types.insert(quad.extra.clone(), operation_type(quad, &declared));
        }
    }

    let mut registers: HashMap<String, String> = HashMap::new();
    registers.insert("HI".into(), config.reg_hi.clone());
    registers.insert("LO".into(), config.reg_lo.clone());
    for reg in [&config.reg_a, &config.reg_b, &config.reg_c, &config.reg_hi, &config.reg_lo] {
        registers.insert(reg.clone(), reg.clone());
    }

    let mut generator = Generator {
        out: String::new(),
        config,
        declared,
        types,
        registers,
        word_type: format!("u{word}"),
    };
    let reg_a = config.reg_a.clone();
    let reg_b = config.reg_b.clone();
    let reg_c = config.reg_c.clone();
    let reg_hi = config.reg_hi.clone();
    let reg_lo = config.reg_lo.clone();
    let x86 = config.target == Target::I8086;

    {
        let g = &mut generator;
        g.out.push_str(&format!(".target {}\n.bits {}\n", config.target, word));
        g.out.push_str(&format!(".mulpair {} {}\n", reg_hi, reg_lo));
        for reg in [&reg_a, &reg_b, &reg_c, &reg_hi, &reg_lo] {
            g.out.push_str(&format!(".reg {reg}\n"));
        }
        g.out.push_str(".data\n");
        for (index, byte) in config.data_image.iter().enumerate() {
            g.out.push_str(&format!("    byte_{index} = {byte}\n"));
        }
        let mut emitted: HashSet<String> = (0..config.data_image.len())
            .map(|index| format!("byte_{index}"))
            .collect();
        let mut data_names: Vec<_> = config.data.keys().cloned().collect();
        data_names.sort();
        for name in data_names {
            let text = config.data.get(&name).cloned().unwrap_or_default();
            let ty = g.type_of(&name);
            let scope = scopes.get(&name).cloned().unwrap_or_else(|| "global".to_string());
            if g.wide(&ty) {
                let value = literal_for(&text, &ty)?;
                for index in 0..g.words_of(&ty) {
                    let part = (value.bits >> (index * word)) & mask_of(word);
                    g.out.push_str(&format!(
                        "    {}#{} = {} : {}\n",
                        name, index, part, g.word_type
                    ));
                }
            } else {
                let prefix = if scope == "local" { "local " } else { "" };
                let suffix = if ty == default_type() { String::new() } else { format!(" : {ty}") };
                g.out.push_str(&format!("    {prefix}{name} = {text}{suffix}\n"));
            }
            emitted.insert(name);
        }
        let mut type_names: Vec<_> = g.types.keys().cloned().collect();
        type_names.sort();
        for name in type_names {
            if emitted.contains(&name) {
                continue;
            }
            let ty = g.type_of(&name);
            let scope = scopes.get(&name).cloned().unwrap_or_else(|| "global".to_string());
            let prefix = if scope == "local" { "local " } else { "" };
            let suffix = if ty == default_type() { String::new() } else { format!(" : {ty}") };
            g.out.push_str(&format!("    {prefix}{name}{suffix}\n"));
        }
        g.out.push_str(".enddata\n.rodata\n");
        let mut ro_names: Vec<_> = config.rodata.keys().cloned().collect();
        ro_names.sort();
        for name in ro_names {
            let text = config.rodata.get(&name).cloned().unwrap_or_default();
            let ty = g.type_of(&name);
            let suffix = if ty == default_type() { String::new() } else { format!(" : {ty}") };
            g.out.push_str(&format!("    {name} = {text}{suffix}\n"));
        }
        g.out.push_str(".endrodata\n");
        // ---- .dataarea：数据区字节镜像 + 符号基址（addr / load / store 用） ----
        // 与 stackvm 的 DataArea 同一份约定：符号表给基址，读写都是 8 字节小端，
        // 地址就是这里的字节偏移。字节用 `.hex 起始偏移 十六进制串` 紧凑写出（16 字节一行）。
        g.out
            .push_str(&format!(".dataarea {}\n", config.data_area_bytes.len()));
        let mut area_symbols: Vec<_> = config.data_area_symbols.iter().collect();
        area_symbols.sort();
        for (name, offset) in area_symbols {
            g.out.push_str(&format!("    .sym {name} {offset}\n"));
        }
        let digits = b"0123456789abcdef";
        let mut area_offset = 0usize;
        while area_offset < config.data_area_bytes.len() {
            let end = (area_offset + 16).min(config.data_area_bytes.len());
            let mut hex = String::new();
            for byte in &config.data_area_bytes[area_offset..end] {
                hex.push(digits[(byte >> 4) as usize] as char);
                hex.push(digits[(byte & 0x0F) as usize] as char);
            }
            g.out.push_str(&format!("    .hex {area_offset} {hex}\n"));
            area_offset = end;
        }
        g.out.push_str(".enddataarea\n");
        g.out
            .push_str(&format!(".stack {}\n.code\n", config.stack_words));
    }

    let mut serial = 0usize;
    for (quad_index, quad) in program.iter().enumerate() {
        let base = op_base(&quad.op);
        match base {
            "decl" => continue,
            "label" => {
                let label = quad.result.clone();
                generator.out.push_str(&format!("L_{label}:\n"));
                continue;
            }
            "j" => {
                let label = quad.result.clone();
                generator.emit(format!("J L_{label}"));
                continue;
            }
            "halt" => {
                generator.emit("HALT".into());
                continue;
            }
            "param" => {
                let formal_type = generator.type_of(&quad.result);
                if generator.wide(&formal_type) {
                    let (a, result) = (quad.a.clone(), quad.result.clone());
                    generator.emit(format!("PUSHW.{formal_type} {a}, {result}"));
                } else {
                    let operand = quad.a.clone();
                    let source = generator.source_type(&operand, quad);
                    generator.load_to(&reg_a, &operand, &source);
                    let result = quad.result.clone();
                    generator.emit(format!("PUSH.{formal_type} {reg_a}, {result}"));
                }
                continue;
            }
            "call" | "int" => {
                let mnemonic = if base == "call" { "CALL" } else { "INT" };
                let target = quad.a.clone();
                let destination =
                    if quad.result == "_" { "_".to_string() } else { quad.result.clone() };
                generator.emit(format!("{mnemonic} {target}, {destination}"));
                continue;
            }
            "ret" => {
                if quad.a == "_" {
                    generator.emit("RET".into());
                } else {
                    let operand = quad.a.clone();
                    let ty = generator.type_of(&operand);
                    if generator.wide(&ty) {
                        generator.emit(format!("RETW.{ty} {operand}"));
                    } else {
                        generator.load_to(&reg_a, &operand, &ty);
                        generator.emit(format!("RET {reg_a}"));
                    }
                }
                continue;
            }
            "iret" => {
                generator.emit("IRET".into());
                continue;
            }
            "print" => {
                let text = escape(&quad.a);
                generator.emit(format!("PRINT \"{text}\""));
                continue;
            }
            "putc" => {
                let operand = quad.a.clone();
                let ty = generator.source_type(&operand, quad);
                generator.load_to(&reg_a, &operand, &ty);
                generator.emit(format!("PUTC {reg_a}"));
                continue;
            }
            "getc" => {
                let ty = generator.type_of(&quad.result);
                let result = quad.result.clone();
                if generator.wide(&ty) {
                    generator.emit(format!("GETCW.{ty} {result}"));
                } else {
                    generator.emit(with_type(&format!("GETC {reg_a}"), &ty));
                    generator.store_to(&result, &reg_a, &ty);
                }
                continue;
            }
            "read" => {
                let ty = generator.type_of(&quad.result);
                let result = quad.result.clone();
                if generator.wide(&ty) {
                    generator.emit(format!("READW.{ty} {result}"));
                } else {
                    generator.emit(with_type(&format!("IN {reg_a}"), &ty));
                    generator.store_to(&result, &reg_a, &ty);
                }
                continue;
            }
            "write" => {
                let operand = quad.a.clone();
                let ty = generator.source_type(&operand, quad);
                if is_string_type(&ty) {
                    let text = format!("SWRITE.str STDOUT, {operand}");
                    generator.emit(text);
                    continue;
                }
                if generator.wide(&ty) {
                    generator.emit(format!("OUTW.{ty} {operand}"));
                } else {
                    generator.load_to(&reg_a, &operand, &ty);
                    generator.emit(with_type(&format!("OUT {reg_a}"), &ty));
                }
                continue;
            }
            // ---- 字符串指令（运行时伪指令） ----
            _ if is_string_op(base) => {
                generator.require_feature("string", quad_index, quad)?;
                let (a, b, result, extra) =
                    (quad.a.clone(), quad.b.clone(), quad.result.clone(), quad.extra.clone());
                let text = match base {
                    "scopy" => format!("SCOPY {result}, {a}"),
                    "slen" => format!("SLEN {result}, {a}"),
                    "scmp" => format!("SCMP {result}, {a}, {b}"),
                    "scat" => format!("SCAT {result}, {a}, {b}"),
                    "ssub" => format!("SSUB {result}, {a}, {b}, {extra}"),
                    _ => format!("SFIND {result}, {a}, {b}"),
                };
                generator.emit(text);
                continue;
            }
            // ---- 流指令 ----
            _ if is_stream_op(base) => {
                generator.require_feature("stream", quad_index, quad)?;
                let text = match base {
                    "stream" => format!("STREAM {}, {}", quad.result, quad.a),
                    "sread" => format!("SREAD.{} {}, {}", generator.type_of(&quad.result),
                                       quad.result, quad.a),
                    "swrite" => format!("SWRITE.{} {}, {}", generator.value_operand_type(&quad.b),
                                        quad.a, quad.b),
                    _ => format!("SCLOSE {}", quad.a),
                };
                generator.emit(text);
                continue;
            }
            // ---- 向量指令（元素位数写在后缀里） ----
            _ if is_vector_op(base) => {
                generator.require_feature("vector", quad_index, quad)?;
                let suffix = op_type(&quad.op);
                let element = if !suffix.is_empty() {
                    normalized_type(suffix)?
                } else if let Some(ty) = generator.declared.get(&quad.a) {
                    ty.clone()
                } else {
                    generator.type_of(&quad.result)
                };
                let text = match base {
                    "vload" => format!("VLOAD.{element} {}, {}, {}", quad.result, quad.a, quad.b),
                    "vstore" => format!("VSTORE.{element} {}, {}, {}", quad.a, quad.b, quad.result),
                    "vcopy" => format!("VCOPY.{element} {}, {}, {}", quad.result, quad.a, quad.b),
                    "vfill" => format!("VFILL.{element} {}, {}, {}", quad.a, quad.b, quad.result),
                    "vsum" => format!("VSUM.{element} {}, {}, {}", quad.result, quad.a, quad.b),
                    "vmax" => format!("VMAX.{element} {}, {}, {}", quad.result, quad.a, quad.b),
                    _ => format!("VMIN.{element} {}, {}, {}", quad.result, quad.a, quad.b),
                };
                generator.emit(text);
                continue;
            }
            // ---- 外部文件 IO ----
            _ if is_file_op(base) => {
                generator.require_feature("file", quad_index, quad)?;
                let text = match base {
                    "fopen" => format!("FOPEN {}, {}, {}", quad.result, quad.a, quad.b),
                    "fread" => format!("FREAD.{} {}, {}", generator.type_of(&quad.result),
                                       quad.result, quad.a),
                    "fwrite" => format!("FWRITE.{} {}, {}", generator.value_operand_type(&quad.b),
                                        quad.a, quad.b),
                    "feof" => format!("FEOF.{} {}, {}", generator.type_of(&quad.result),
                                      quad.result, quad.a),
                    _ => format!("FCLOSE {}", quad.a),
                };
                generator.emit(text);
                continue;
            }
            "system" => {
                generator.require_feature("system", quad_index, quad)?;
                let destination = if quad.result == "_" { "_" } else { &quad.result };
                let text = format!("SYSCALL {}, {}, {}", quad.a, destination, quad.b);
                generator.emit(text);
                continue;
            }
            "addr" | "load" | "store" => {
                // 数据区三件套（与 stackvm 的 ADDR / LOADAT / STOREAT 同名同义）：
                //   addr  符号 -> 目标   取数据区符号基址
                //   load  指针 -> 目标   按地址读 8 字节小端
                //   store 指针, 值       按地址写 8 字节小端
                // 三条都是运行时伪指令：地址可能是窄机器上的多字变量，由模拟器按整值
                // 凑出地址再去读写 `.dataarea` —— 所以三种目标机生成同一条伪指令，
                // 机器能力由指令族 'data' 表示（配置里 `feature.data=off` 就报错）。
                generator.require_feature("data", quad_index, quad)?;
                let text = match base {
                    "addr" => format!("ADDR {}, {}", quad.result, quad.a),
                    "load" => format!("LOADAT {}, {}", quad.result, quad.a),
                    _ => format!(
                        "STOREAT.{} {}, {}",
                        generator.value_operand_type(&quad.b),
                        quad.a,
                        quad.b
                    ),
                };
                generator.emit(text);
                continue;
            }
            "jz" | "jnz" => {
                let operand = quad.a.clone();
                let ty = generator.source_type(&operand, quad);
                if generator.wide(&ty) {
                    generator.emit(format!("CMPW.{ty} {operand}, 0"));
                } else {
                    generator.load_to(&reg_a, &operand, &ty);
                    let zero = if x86 { "0" } else { "#0" };
                    generator.emit(with_type(&format!("CMP {reg_a}, {zero}"), &ty));
                }
                let label = quad.result.clone();
                let mnemonic = if x86 {
                    if base == "jz" { "JE" } else { "JNE" }
                } else if base == "jz" {
                    "BEQ"
                } else {
                    "BNE"
                };
                generator.emit(format!("{mnemonic} L_{label}"));
                continue;
            }
            ":=" => {
                let value_type = operation_type(quad, &generator.declared);
                let target = generator.type_of(&quad.result);
                let operand = quad.a.clone();
                let destination = quad.result.clone();
                if is_numeric_literal(&operand) {
                    if !generator.wide(&value_type) && !generator.wide(&target) {
                        let template = command(config, "immediate_load", "LI {dst}, {value}");
                        generator.emit_lines(
                            &template,
                            [&reg_a, "", "", "", "", "", &operand],
                            &value_type,
                        );
                        let store = generator.store_template(&target);
                        generator.emit_lines(
                            &store,
                            ["", "", "", "", &reg_a, &destination, ""],
                            &target,
                        );
                    } else {
                        let value = literal_for(&operand, &value_type)
                            .and_then(|parsed| convert_value(&parsed, &target))
                            .unwrap_or_else(|_| Value::zero(&target));
                        let template = command(config, "immediate_load", "LI {dst}, {value}");
                        for index in 0..generator.words_of(&target) {
                            let part = (value.bits >> (index * word)) & mask_of(word);
                            let word_type = generator.word_type.clone();
                            let part_text = part.to_string();
                            generator.emit_lines(
                                &template,
                                [&reg_a, "", "", "", "", "", &part_text],
                                &word_type,
                            );
                            generator.store_word(&destination, index, &reg_a);
                        }
                    }
                } else if generator.wide(&target) {
                    for index in 0..generator.words_of(&target) {
                        generator.load_word(&reg_a, &operand, index);
                        generator.store_word(&destination, index, &reg_a);
                    }
                } else {
                    let source = generator.source_type(&operand, quad);
                    generator.load_to(&reg_a, &operand, &source);
                    generator.store_to(&destination, &reg_a, &target);
                }
                continue;
            }
            _ => {}
        }
        if !is_binary_base(base) && !is_unary_base(base) {
            continue;
        }
        let optype = operation_type(quad, &generator.declared);
        let rtype = result_type(quad, &generator.declared);
        let width = type_bits(&optype);
        let float_op = is_float_type(&optype);
        let unsigned_op = is_unsigned_type(&optype);
        let is_wide = generator.wide(&optype)
            || generator.wide(&generator.type_of(&quad.a))
            || generator.wide(&generator.type_of(&quad.b));

        // ---- 机器缺乘法 / 除法 / 浮点时的低级语义替换 ----
        if !config.supports("float")
            && (float_op
                || (base == "cvt"
                    && (is_float_type(&generator.type_of(&quad.a)) || is_float_type(&rtype))))
        {
            let constant_expression = is_numeric_literal(&quad.a)
                && (base == "neg" || base == "cvt" || is_numeric_literal(&quad.b));
            if constant_expression {
                let lhs = literal_for(&quad.a, &optype);
                let rhs = if base == "neg" || base == "cvt" {
                    Ok(Value::zero(&optype))
                } else {
                    literal_for(&quad.b, &optype)
                };
                if let (Ok(lhs), Ok(rhs)) = (lhs, rhs) {
                    let folded = if base == "cvt" {
                        convert_value(&lhs, &optype)
                    } else {
                        eval_typed(base, &optype, &lhs, &rhs)
                    };
                    if let Ok(value) = folded {
                        let text = display_value(&value);
                        let result = quad.result.clone();
                        generator.store_immediate(&result, &text, &optype, &rtype);
                        continue;
                    }
                }
            }
            return Err(format!(
                "四元式 {} ({}) 需要浮点指令，但目标机 {} 没有实现指令族 'float'；运行时浮点运算需要软件浮点例程，常量运算会在生成期折叠",
                quad_index, quad.op, config.target
            ));
        }
        if base == "*" && !config.supports("mul") && !is_wide {
            // 常量乘数展开成移位加：x * c = Σ (x << k)。
            let (constant, variable) = if is_integer_literal(&quad.b) {
                (quad.b.parse::<i64>().unwrap_or(0), quad.a.clone())
            } else if is_integer_literal(&quad.a) {
                (quad.a.parse::<i64>().unwrap_or(0), quad.b.clone())
            } else {
                // 变乘数：用"移位加"循环代替乘法（只需要加减 / 移位 / 分支）。
                if !config.supports("shift") || !config.supports("bitwise") {
                    return Err(format!(
                        "四元式 {quad_index} 的乘法循环展开需要移位与按位指令族"
                    ));
                }
                let acc = format!("__m_acc{quad_index}");
                let lhs_copy = format!("__m_a{quad_index}");
                let rhs_copy = format!("__m_b{quad_index}");
                let counter = format!("__m_n{quad_index}");
                let loop_label = format!("L_mul{quad_index}");
                let skip_label = format!("L_mulskip{quad_index}");
                let inner = format!("u{width}");
                let immediate = command(config, "immediate_load", "LI {dst}, {value}");
                let and = command(config, "and", "AND {dst}, {lhs}, {rhs}");
                let add = command(config, "add", "ADD {dst}, {lhs}, {rhs}");
                let shl = command(config, "shl", "SLL {dst}, {lhs}, {rhs}");
                let shru = command(config, "shru", "SRL {dst}, {lhs}, {rhs}");
                let sub = command(config, "sub", "SUB {dst}, {lhs}, {rhs}");
                let one = "1".to_string();
                generator.emit_lines(&immediate, [&reg_a, "", "", "", "", "", "0"], &inner);
                generator.store_to(&acc, &reg_a, &inner);
                let a_type = generator.source_type(&quad.a, quad);
                generator.load_to(&reg_a, &quad.a, &a_type);
                generator.store_to(&lhs_copy, &reg_a, &inner);
                let b_type = generator.source_type(&quad.b, quad);
                generator.load_to(&reg_a, &quad.b, &b_type);
                generator.store_to(&rhs_copy, &reg_a, &inner);
                let width_text = width.to_string();
                generator.emit_lines(&immediate, [&reg_a, "", "", "", "", "", &width_text], &inner);
                generator.store_to(&counter, &reg_a, &inner);
                generator.out.push_str(&format!("{loop_label}:\n"));
                generator.load_to(&reg_a, &rhs_copy, &inner);
                generator.emit_lines(&immediate, [&reg_b, "", "", "", "", "", &one], &inner);
                generator.emit_lines(&and, [&reg_a, "", &reg_a, &reg_b, "", "", ""], &inner);
                let zero = if x86 { "0" } else { "#0" };
                generator.emit(with_type(&format!("CMP {reg_a}, {zero}"), &inner));
                generator.emit(format!("{} {skip_label}", branch_mnemonic(config.target, "==")));
                generator.load_to(&reg_a, &acc, &inner);
                generator.load_to(&reg_b, &lhs_copy, &inner);
                generator.emit_lines(&add, [&reg_a, "", &reg_a, &reg_b, "", "", ""], &inner);
                generator.store_to(&acc, &reg_a, &inner);
                generator.out.push_str(&format!("{skip_label}:\n"));
                generator.load_to(&reg_a, &lhs_copy, &inner);
                generator.emit_lines(&immediate, [&reg_b, "", "", "", "", "", &one], &inner);
                generator.emit_lines(&shl, [&reg_a, "", &reg_a, &reg_b, "", "", ""], &inner);
                generator.store_to(&lhs_copy, &reg_a, &inner);
                generator.load_to(&reg_a, &rhs_copy, &inner);
                generator.emit_lines(&shru, [&reg_a, "", &reg_a, &reg_b, "", "", ""], &inner);
                generator.store_to(&rhs_copy, &reg_a, &inner);
                generator.load_to(&reg_a, &counter, &inner);
                generator.emit_lines(&sub, [&reg_a, "", &reg_a, &reg_b, "", "", ""], &inner);
                generator.store_to(&counter, &reg_a, &inner);
                generator.emit(with_type(&format!("CMP {reg_a}, {zero}"), &inner));
                generator.emit(format!("{} {loop_label}", branch_mnemonic(config.target, "!=")));
                generator.load_to(&reg_a, &acc, &inner);
                let result = quad.result.clone();
                generator.store_to(&result, &reg_a, &rtype);
                continue;
            };
            if !config.supports("shift") {
                return Err(format!("四元式 {quad_index} 的乘法展开需要移位指令族 'shift'"));
            }
            let magnitude = constant.unsigned_abs();
            let source_type = generator.source_type(&variable, quad);
            generator.load_to(&reg_a, &variable, &source_type);
            let template = command(config, "immediate_load", "LI {dst}, {value}");
            generator.emit_lines(&template, [&reg_b, "", "", "", "", "", "0"], &optype);
            for bit in 0..64 {
                if (magnitude >> bit) & 1 == 0 {
                    continue;
                }
                let shift = bit.to_string();
                let shl = command(config, "shl", "SLL {dst}, {lhs}, {rhs}");
                generator.emit_lines(&shl, [&reg_c, "", &reg_a, &shift, "", "", ""], &optype);
                let add = command(config, "add", "ADD {dst}, {lhs}, {rhs}");
                generator.emit_lines(&add, [&reg_b, "", &reg_b, &reg_c, "", "", ""], &optype);
            }
            if constant < 0 {
                let text = if x86 {
                    format!("NEG {reg_b}")
                } else {
                    format!("NEG {reg_b}, {reg_b}")
                };
                generator.emit(with_type(&text, &optype));
            }
            let result = quad.result.clone();
            generator.store_to(&result, &reg_b, &rtype);
            continue;
        }
        if (base == "/" || base == "%") && !config.supports("div") && !is_wide {
            // 常量 2 的幂走移位 / 掩码；其余（含变除数）走移位减循环。
            let constant_divisor = is_integer_literal(&quad.b);
            let divisor = if constant_divisor { quad.b.parse::<i64>().unwrap_or(0) } else { 0 };
            if constant_divisor && divisor == 0 {
                return Err("Division by zero".into());
            }
            let magnitude = divisor.unsigned_abs();
            if !is_unsigned_type(&optype) || divisor < 0 {
                // 有符号除法：低级替换目前只覆盖无符号类型。
                return Err(format!(
                    "四元式 {quad_index} 的有符号除法需要除法指令；低级替换目前只覆盖无符号类型"
                ));
            }
            if !config.supports("shift") || !config.supports("bitwise") {
                return Err(format!("四元式 {quad_index} 的除法展开需要移位与按位指令族"));
            }
            let source_type = generator.source_type(&quad.a, quad);
            if divisor > 0 && magnitude & (magnitude - 1) == 0 {
                // 2 的幂：移位 / 掩码。
                let shift = magnitude.trailing_zeros().to_string();
                generator.load_to(&reg_a, &quad.a, &source_type);
                if base == "%" {
                    let mask = (magnitude - 1).to_string();
                    let template = command(config, "immediate_load", "LI {dst}, {value}");
                    generator.emit_lines(&template, [&reg_b, "", "", "", "", "", &mask], &optype);
                    let and = command(config, "and", "AND {dst}, {lhs}, {rhs}");
                    generator.emit_lines(&and, [&reg_a, "", &reg_a, &reg_b, "", "", ""], &optype);
                } else {
                    let shru = command(config, "shru", "SRL {dst}, {lhs}, {rhs}");
                    generator.emit_lines(&shru, [&reg_a, "", &reg_a, &shift, "", "", ""], &optype);
                }
                let result = quad.result.clone();
                generator.store_to(&result, &reg_a, &rtype);
                continue;
            }
            // 一般除数：移位减循环。
            let quotient = format!("__d_q{quad_index}");
            let remainder = format!("__d_r{quad_index}");
            let bit_index = format!("__d_i{quad_index}");
            let divisor_copy = format!("__d_b{quad_index}");
            let loop_label = format!("L_div{quad_index}");
            let skip_label = format!("L_divskip{quad_index}");
            let inner = format!("u{width}");
            let signed_inner = format!("i{width}");
            let immediate = command(config, "immediate_load", "LI {dst}, {value}");
            let and = command(config, "and", "AND {dst}, {lhs}, {rhs}");
            let or = command(config, "or", "OR {dst}, {lhs}, {rhs}");
            let shl = command(config, "shl", "SLL {dst}, {lhs}, {rhs}");
            let shru = command(config, "shru", "SRL {dst}, {lhs}, {rhs}");
            let sub = command(config, "sub", "SUB {dst}, {lhs}, {rhs}");
            let one = "1".to_string();
            let zero = if x86 { "0" } else { "#0" };
            generator.emit_lines(&immediate, [&reg_a, "", "", "", "", "", "0"], &inner);
            generator.store_to(&quotient, &reg_a, &inner);
            generator.store_to(&remainder, &reg_a, &inner);
            let top = (width - 1).to_string();
            generator.emit_lines(&immediate, [&reg_a, "", "", "", "", "", &top], &inner);
            generator.store_to(&bit_index, &reg_a, &inner);
            let b_type = generator.source_type(&quad.b, quad);
            generator.load_to(&reg_a, &quad.b, &b_type);
            generator.store_to(&divisor_copy, &reg_a, &inner);
            generator.out.push_str(&format!("{loop_label}:\n"));
            generator.load_to(&reg_a, &quad.a, &source_type);
            generator.load_to(&reg_b, &bit_index, &inner);
            generator.emit_lines(&shru, [&reg_a, "", &reg_a, &reg_b, "", "", ""], &inner);
            generator.emit_lines(&immediate, [&reg_b, "", "", "", "", "", &one], &inner);
            generator.emit_lines(&and, [&reg_a, "", &reg_a, &reg_b, "", "", ""], &inner);
            generator.load_to(&reg_b, &remainder, &inner);
            generator.emit_lines(&immediate, [&reg_c, "", "", "", "", "", &one], &inner);
            generator.emit_lines(&shl, [&reg_b, "", &reg_b, &reg_c, "", "", ""], &inner);
            generator.emit_lines(&or, [&reg_b, "", &reg_b, &reg_a, "", "", ""], &inner);
            generator.store_to(&remainder, &reg_b, &inner);
            generator.load_to(&reg_c, &divisor_copy, &inner);
            generator.emit(with_type(&format!("CMP {reg_b}, {reg_c}"), &inner));
            generator.emit(format!("{} {skip_label}", branch_mnemonic(config.target, "<")));
            generator.emit_lines(&sub, [&reg_b, "", &reg_b, &reg_c, "", "", ""], &inner);
            generator.store_to(&remainder, &reg_b, &inner);
            generator.emit_lines(&immediate, [&reg_b, "", "", "", "", "", &one], &inner);
            generator.load_to(&reg_c, &bit_index, &inner);
            generator.emit_lines(&shl, [&reg_b, "", &reg_b, &reg_c, "", "", ""], &inner);
            generator.load_to(&reg_c, &quotient, &inner);
            generator.emit_lines(&or, [&reg_c, "", &reg_c, &reg_b, "", "", ""], &inner);
            generator.store_to(&quotient, &reg_c, &inner);
            generator.out.push_str(&format!("{skip_label}:\n"));
            generator.load_to(&reg_a, &bit_index, &inner);
            generator.emit_lines(&immediate, [&reg_b, "", "", "", "", "", &one], &inner);
            generator.emit_lines(&sub, [&reg_a, "", &reg_a, &reg_b, "", "", ""], &inner);
            generator.store_to(&bit_index, &reg_a, &inner);
            // 计数器会降到 -1，比较按有符号解释才停得下来。
            generator.emit(with_type(&format!("CMP {reg_a}, {zero}"), &signed_inner));
            generator.emit(format!("{} {loop_label}", branch_mnemonic(config.target, ">=")));
            let source = if base == "%" { remainder } else { quotient };
            generator.load_to(&reg_a, &source, &inner);
            let result = quad.result.clone();
            generator.store_to(&result, &reg_a, &rtype);
            continue;
        }
        // 机器能力检查：浮点、乘除、移位、按位、进位各自可以缺失。
        if float_op {
            generator.require_feature("float", quad_index, quad)?;
        }
        if is_pair_base(base) {
            generator.require_feature(if base == "mul" { "mul" } else { "div" }, quad_index, quad)?;
        } else if base == "*" {
            generator.require_feature("mul", quad_index, quad)?;
        } else if base == "/" || base == "%" {
            generator.require_feature("div", quad_index, quad)?;
        } else if base == "<<" || base == ">>" {
            generator.require_feature("shift", quad_index, quad)?;
        } else if matches!(base, "&" | "|" | "^" | "~") {
            generator.require_feature("bitwise", quad_index, quad)?;
        } else if base == "+c" || base == "-b" {
            generator.require_feature("carry", quad_index, quad)?;
        }

        if base == "cvt" {
            let source = generator.source_type(&quad.a, quad);
            let (result, operand) = (quad.result.clone(), quad.a.clone());
            if generator.wide(&source) || generator.wide(&rtype) {
                generator.emit(format!("RCALL.{optype} cvt, {result}, {operand}, _"));
            } else {
                generator.load_to(&reg_a, &operand, &source);
                generator.emit(format!("CVT.{optype} {reg_a}, {reg_a}"));
                generator.store_to(&result, &reg_a, &rtype);
            }
            continue;
        }
        if float_op {
            let a = quad.a.clone();
            let a_type = generator.source_type(&a, quad);
            generator.load_to(&reg_a, &a, &a_type);
            if base != "neg" {
                let b = quad.b.clone();
                let b_type = generator.source_type(&b, quad);
                generator.load_to(&reg_b, &b, &b_type);
            }
            if is_compare_base(base) {
                generator.emit(format!("FCMP.{optype} {reg_a}, {reg_b}"));
                let yes = format!("T_true_{serial}");
                let done = format!("T_done_{serial}");
                serial += 1;
                let template = command(config, "immediate_load", "LI {dst}, {value}");
                generator.emit_lines(&template, [&reg_c, "", "", "", "", "", "0"], &rtype);
                generator.emit(format!("{} {yes}", branch_mnemonic(config.target, base)));
                generator.emit(format!("B {done}"));
                generator.out.push_str(&format!("{yes}:\n"));
                generator.emit_lines(&template, [&reg_c, "", "", "", "", "", "1"], &rtype);
                generator.out.push_str(&format!("{done}:\n"));
                let result = quad.result.clone();
                generator.store_to(&result, &reg_c, &rtype);
                continue;
            }
            let key = match base {
                "+" => "fadd",
                "-" => "fsub",
                "*" => "fmul",
                "/" => "fdiv",
                _ => "fneg",
            };
            let fallback = match base {
                "neg" => "NEG {dst}, {lhs}",
                "+" => "ADD {dst}, {lhs}, {rhs}",
                "-" => "SUB {dst}, {lhs}, {rhs}",
                "*" => "MUL {dst}, {lhs}, {rhs}",
                _ => "DIV {dst}, {lhs}, {rhs}",
            };
            let template = command(config, key, fallback);
            generator.emit_lines(&template, [&reg_a, "", &reg_a, &reg_b, "", "", ""], &optype);
            let result = quad.result.clone();
            generator.store_to(&result, &reg_a, &rtype);
            continue;
        }
        if is_wide {
            let count = generator.words_of(&optype);
            if matches!(base, "+" | "-" | "+c" | "-b") {
                let subtract = base == "-" || base == "-b";
                let from_carry = base == "+c" || base == "-b";
                let (a, b, result) = (quad.a.clone(), quad.b.clone(), quad.result.clone());
                for index in 0..count {
                    generator.load_word_operand(&reg_a, &a, &optype, index);
                    generator.load_word_operand(&reg_b, &b, &optype, index);
                    let carry_step = from_carry || index > 0;
                    let mnemonic = if subtract {
                        if carry_step { "SUBB" } else { "SUB" }
                    } else if carry_step {
                        "ADDC"
                    } else {
                        "ADD"
                    };
                    let word_type = generator.word_type.clone();
                    generator.emit(with_type(
                        &format!("{mnemonic} {reg_a}, {reg_a}, {reg_b}"),
                        &word_type,
                    ));
                    generator.store_word(&result, index, &reg_a);
                }
                continue;
            }
            if matches!(base, "&" | "|" | "^") {
                let mnemonic = match base {
                    "&" => "AND",
                    "|" => "OR",
                    _ => "XOR",
                };
                let (a, b, result) = (quad.a.clone(), quad.b.clone(), quad.result.clone());
                for index in 0..count {
                    generator.load_word(&reg_a, &a, index);
                    generator.load_word(&reg_b, &b, index);
                    let word_type = generator.word_type.clone();
                    generator.emit(with_type(
                        &format!("{mnemonic} {reg_a}, {reg_a}, {reg_b}"),
                        &word_type,
                    ));
                    generator.store_word(&result, index, &reg_a);
                }
                continue;
            }
            if base == "~" {
                let (a, result) = (quad.a.clone(), quad.result.clone());
                for index in 0..count {
                    generator.load_word(&reg_a, &a, index);
                    let word_type = generator.word_type.clone();
                    generator.emit(with_type(&format!("NOT {reg_a}, {reg_a}"), &word_type));
                    generator.store_word(&result, index, &reg_a);
                }
                continue;
            }
            if is_pair_base(base) {
                let (result, extra, a, b) =
                    (quad.result.clone(), quad.extra.clone(), quad.a.clone(), quad.b.clone());
                generator.emit(format!("RCALLW.{optype} {base}, {result}, {extra}, {a}, {b}"));
            } else {
                let (result, a, b) = (quad.result.clone(), quad.a.clone(), quad.b.clone());
                generator.emit(format!("RCALL.{optype} {base}, {result}, {a}, {b}"));
            }
            continue;
        }

        // ---- 原生宽度 ----
        let a = quad.a.clone();
        let a_type = generator.source_type(&a, quad);
        generator.load_to(&reg_a, &a, &a_type);
        let needs_second = is_binary_base(base) && !matches!(base, "neg" | "odd" | "~");
        if needs_second {
            let b = quad.b.clone();
            let b_type = generator.source_type(&b, quad);
            generator.load_to(&reg_b, &b, &b_type);
        }
        if base == "neg" {
            let text = if x86 {
                format!("NEG {reg_a}")
            } else {
                format!("NEG {reg_a}, {reg_a}")
            };
            generator.emit(with_type(&text, &optype));
            let result = quad.result.clone();
            generator.store_to(&result, &reg_a, &rtype);
            continue;
        }
        if base == "odd" {
            let template = command(config, "immediate_load", "LI {dst}, {value}");
            generator.emit_lines(&template, [&reg_b, "", "", "", "", "", "1"], &optype);
            let text = if x86 {
                format!("AND {reg_a}, {reg_b}")
            } else {
                format!("AND {reg_a}, {reg_a}, {reg_b}")
            };
            generator.emit(with_type(&text, &optype));
            let result = quad.result.clone();
            generator.store_to(&result, &reg_a, &rtype);
            continue;
        }
        if base == "~" {
            let template = command(
                config,
                "not",
                if x86 { "NOT {dst}" } else { "NOR {dst}, {lhs}, {lhs}" },
            );
            generator.emit_lines(&template, [&reg_a, "", &reg_a, &reg_a, "", "", ""], &optype);
            let result = quad.result.clone();
            generator.store_to(&result, &reg_a, &rtype);
            continue;
        }
        if base == "+c" || base == "-b" {
            let mnemonic = if base == "+c" { "ADDC" } else { "SUBB" };
            generator.emit(with_type(
                &format!("{mnemonic} {reg_a}, {reg_a}, {reg_b}"),
                &optype,
            ));
            let result = quad.result.clone();
            generator.store_to(&result, &reg_a, &rtype);
            continue;
        }
        if is_compare_base(base) {
            let yes = format!("T_true_{serial}");
            let done = format!("T_done_{serial}");
            serial += 1;
            generator.emit(with_type(&format!("CMP {reg_a}, {reg_b}"), &optype));
            let template = command(config, "immediate_load", "LI {dst}, {value}");
            generator.emit_lines(&template, [&reg_c, "", "", "", "", "", "0"], &rtype);
            generator.emit(format!("{} {yes}", branch_mnemonic(config.target, base)));
            generator.emit(format!("B {done}"));
            generator.out.push_str(&format!("{yes}:\n"));
            generator.emit_lines(&template, [&reg_c, "", "", "", "", "", "1"], &rtype);
            generator.out.push_str(&format!("{done}:\n"));
            let result = quad.result.clone();
            generator.store_to(&result, &reg_c, &rtype);
            continue;
        }
        if base == "*" || base == "/" || base == "%" || is_pair_base(base) {
            // 乘除法：结果进固定命名寄存器对，一条四元式展开成一串目标指令，
            // 具体序列由配置里的 mul/div/rem/mul2/div2（无符号加 u 后缀）决定。
            let stem = if is_pair_base(base) {
                if base == "mul" { "mul2" } else { "div2" }
            } else if base == "*" {
                "mul"
            } else if base == "/" {
                "div"
            } else {
                "rem"
            };
            let key = format!("{stem}{}", if unsigned_op { "u" } else { "" });
            let fallback = match key.as_str() {
                "mul" => "MUL {dst}, {lhs}, {rhs}",
                "mulu" => "MULU {dst}, {lhs}, {rhs}",
                "div" => "DIV {lhs}, {rhs}\nMFLO {dst}",
                "divu" => "DIVU {lhs}, {rhs}\nMFLO {dst}",
                "rem" => "DIV {lhs}, {rhs}\nMFHI {dst}",
                "remu" => "DIVU {lhs}, {rhs}\nMFHI {dst}",
                "mul2" => "MULT {lhs}, {rhs}\nMFLO {dst}\nMFHI {dst2}",
                "mul2u" => "MULTU {lhs}, {rhs}\nMFLO {dst}\nMFHI {dst2}",
                "div2" => "DIV {lhs}, {rhs}\nMFLO {dst}\nMFHI {dst2}",
                _ => "DIVU {lhs}, {rhs}\nMFLO {dst}\nMFHI {dst2}",
            };
            let template = command(config, &key, fallback);
            let result = quad.result.clone();
            let extra = quad.extra.clone();
            generator.emit_lines(
                &template,
                [&result, &extra, &reg_a, &reg_b, "", "", ""],
                &optype,
            );
            continue;
        }
        let key = match base {
            "+" => "add",
            "-" => "sub",
            "<<" => "shl",
            ">>" => if unsigned_op { "shru" } else { "shr" },
            "&" => "and",
            "|" => "or",
            _ => "xor",
        };
        let fallback = match base {
            "+" => "ADD {dst}, {lhs}, {rhs}",
            "-" => "SUB {dst}, {lhs}, {rhs}",
            "<<" => "SLL {dst}, {lhs}, {rhs}",
            ">>" => {
                if unsigned_op { "SRL {dst}, {lhs}, {rhs}" } else { "SRA {dst}, {lhs}, {rhs}" }
            }
            "&" => "AND {dst}, {lhs}, {rhs}",
            "|" => "OR {dst}, {lhs}, {rhs}",
            _ => "XOR {dst}, {lhs}, {rhs}",
        };
        if matches!(base, "<<" | ">>") && width < 64 && width & (width - 1) == 0 {
            let mask = (width - 1).to_string();
            let template = command(config, "immediate_load", "LI {dst}, {value}");
            let native = format!("i{word}");
            generator.emit_lines(&template, [&reg_c, "", "", "", "", "", &mask], &native);
            generator.emit(with_type(
                &format!("AND {reg_b}, {reg_b}, {reg_c}"),
                &native,
            ));
        }
        let template = command(config, key, fallback);
        generator.emit_lines(&template, [&reg_a, "", &reg_a, &reg_b, "", "", ""], &optype);
        let result = quad.result.clone();
        generator.store_to(&result, &reg_a, &rtype);
    }
    generator.emit("HALT".into());
    Ok(generator.out)
}
