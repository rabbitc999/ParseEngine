// 目标代码模拟执行（由 lib.rs 用 include! 拼进同一模块）。

#[derive(Clone, Debug)]
struct Frame {
    return_pc: usize,
    destination: String,
    has_destination: bool,
    interrupt: bool,
    locals: HashMap<String, Value>,
    saved_regs: HashMap<String, Value>,
    saved_carry: bool,
    pending: Vec<(String, Value)>,
    has_return: bool,
    return_value: Value,
}

impl Default for Frame {
    fn default() -> Self {
        Frame {
            return_pc: 0,
            destination: String::new(),
            has_destination: false,
            interrupt: false,
            locals: HashMap::new(),
            saved_regs: HashMap::new(),
            saved_carry: false,
            pending: Vec::new(),
            has_return: false,
            return_value: Value::zero(default_type()),
        }
    }
}

struct Machine {
    target: Target,
    word: u32,
    pair_hi: String,
    pair_lo: String,
    var_types: HashMap<String, String>,
    local_names: HashSet<String>,
    read_only: HashSet<String>,
    registers: HashMap<String, Value>,
    memory: HashMap<String, Value>,
    frames: Vec<Frame>,
    carry: bool,
    compared: Option<(Value, Value, String)>,
    output: Vec<Value>,
    text: String,
    steps: usize,
    labels: HashMap<String, usize>,
    code: Vec<Vec<String>>,
    /// 程序计数器。
    cursor: usize,
    /// 命名流的输入 / 游标 / 输出（STDIN 是默认流）。
    stream_inputs: HashMap<String, Vec<String>>,
    stream_cursors: HashMap<String, usize>,
    stream_text: HashMap<String, String>,
    streams_out: HashMap<String, Vec<Value>>,
    /// 打开的文件：读文件一次性读入，写文件直接写。
    open_files: HashMap<String, OpenFile>,
    /// 数据区字节镜像 + 符号基址（`addr` / `load` / `store` 用，见 `.dataarea` 段）。
    data_area: Vec<u8>,
    data_area_symbols: HashMap<String, u64>,
    /// 有没有装载过数据区（区分「装了个空的」与「压根没装」）。
    data_area_loaded: bool,
    ticks: i64,
    random_state: u64,
}

enum OpenFile {
    Reader { text: String, position: usize },
    Writer(std::fs::File),
}

fn string_result_token(token: &str) -> Value {
    let mut value = Value::zero("str");
    value.text = string_literal_text(token);
    value
}

impl Machine {
    fn is_register(&self, name: &str) -> bool {
        self.registers.contains_key(name) || register_like(name) || name == "HI" || name == "LO"
    }

    fn is_local_slot(&self, name: &str) -> bool {
        if self.local_names.contains(name) {
            return true;
        }
        // 数组元素 v.3 与宽类型分片 v.3#1 都跟着基名走。
        if let Some(dot) = name.find('.') {
            if self.local_names.contains(&name[..dot]) {
                return true;
            }
        }
        match name.find('#') {
            Some(index) => {
                let base = &name[..index];
                if self.local_names.contains(base) {
                    return true;
                }
                match base.find('.') {
                    Some(dot) => self.local_names.contains(&base[..dot]),
                    None => false,
                }
            }
            None => false,
        }
    }

    fn slot_type(&self, name: &str) -> String {
        self.var_types.get(name).cloned().unwrap_or_else(|| default_type().into())
    }

    fn resolved_type(&self, name: &str) -> String {
        if let Some(ty) = self.var_types.get(name) {
            return ty.clone();
        }
        // 数组元素：元素类型就是数组声明的类型（v.3 看 v）。
        if let Some(dot) = name.find('.') {
            if let Some(ty) = self.var_types.get(&name[..dot]) {
                return ty.clone();
            }
        }
        if let Some(hash) = name.find('#') {
            let base = &name[..hash];
            if let Some(ty) = self.var_types.get(base) {
                return ty.clone();
            }
            if let Some(dot) = base.find('.') {
                if let Some(ty) = self.var_types.get(&base[..dot]) {
                    return ty.clone();
                }
            }
        }
        default_type().to_string()
    }

    fn read_slot(&self, name: &str, ty: &str) -> Result<Value, String> {
        let table = if self.is_local_slot(name) {
            &self.frames.last().expect("frame").locals
        } else {
            &self.memory
        };
        match table.get(name) {
            Some(value) => convert_value(value, ty),
            None => Err(format!("Read of uninitialized variable: {name}")),
        }
    }

    fn write_slot(&mut self, name: &str, value: &Value, ty: &str) -> Result<(), String> {
        let base = match name.find('#') {
            Some(index) => &name[..index],
            None => name,
        };
        if self.read_only.contains(name) || self.read_only.contains(base) {
            return Err(format!("Write to read-only variable: {name}"));
        }
        let converted = convert_value(value, ty)?;
        if self.is_local_slot(name) {
            self.frames.last_mut().expect("frame").locals.insert(name.to_string(), converted);
        } else {
            self.memory.insert(name.to_string(), converted);
        }
        Ok(())
    }

    fn whole_read(&self, name: &str, ty: &str) -> Result<Value, String> {
        let declared = self.slot_type(name);
        if is_float_type(&declared) || type_bits(&declared) <= self.word {
            return convert_value(&self.read_slot(name, &declared)?, ty);
        }
        let mut bits = 0u128;
        for index in 0..(type_bits(&declared) + self.word - 1) / self.word {
            let part = self.read_slot(&format!("{name}#{index}"), &format!("u{}", self.word))?;
            bits |= (part.bits & mask_of(self.word)) << (index * self.word);
        }
        convert_value(&Value { ty: declared, bits, real: 0.0, text: String::new() }, ty)
    }

    fn whole_write(&mut self, name: &str, value: &Value) -> Result<(), String> {
        let declared = self.slot_type(name);
        if is_float_type(&declared) || type_bits(&declared) <= self.word {
            return self.write_slot(name, value, &declared);
        }
        let converted = convert_value(value, &declared)?;
        for index in 0..(type_bits(&declared) + self.word - 1) / self.word {
            let part = (converted.bits >> (index * self.word)) & mask_of(self.word);
            let word_type = format!("u{}", self.word);
            self.write_slot(
                &format!("{name}#{index}"),
                &Value { ty: word_type.clone(), bits: part, real: 0.0, text: String::new() },
                &word_type,
            )?;
        }
        Ok(())
    }

    fn assign(&mut self, name: &str, value: &Value) -> Result<(), String> {
        if self.is_register(name) {
            self.registers.insert(name.to_string(), value.clone());
            Ok(())
        } else {
            self.whole_write(name, value)
        }
    }

    fn whole_operand(&self, token: &str, ty: &str) -> Result<Value, String> {
        if token.is_empty() || token == "_" {
            return Ok(Value::zero(ty));
        }
        let plain = token.strip_prefix('#').unwrap_or(token);
        if is_numeric_literal(plain) {
            return literal_for(plain, ty);
        }
        if self.is_register(token) {
            let value = self.registers.get(token).cloned().unwrap_or_else(|| Value::zero(ty));
            return convert_value(&value, ty);
        }
        self.whole_read(token, ty)
    }

    fn operand(&self, token: &str, ty: &str) -> Result<Value, String> {
        if token.len() >= 2 && token.starts_with('[') && token.ends_with(']') {
            return self.read_slot(&token[1..token.len() - 1], ty);
        }
        if self.is_register(token) {
            let value = self.registers.get(token).cloned().unwrap_or_else(|| Value::zero(ty));
            return convert_value(&value, ty);
        }
        let plain = token.strip_prefix('#').unwrap_or(token);
        if is_numeric_literal(plain) {
            return literal_for(plain, ty);
        }
        self.read_slot(token, ty)
    }

    fn store_operand(&mut self, token: &str, value: &Value, ty: &str) -> Result<(), String> {
        if token.len() >= 2 && token.starts_with('[') && token.ends_with(']') {
            return self.write_slot(&token[1..token.len() - 1], value, ty);
        }
        if self.is_register(token) {
            self.registers.insert(token.to_string(), convert_value(value, ty)?);
            return Ok(());
        }
        self.write_slot(token, value, ty)
    }

    fn jump(&mut self, label: &str) -> Result<(), String> {
        match self.labels.get(label) {
            Some(pc) => {
                self.cursor = *pc;
                Ok(())
            }
            None => Err(format!("Unknown assembly label: {label}")),
        }
    }

    fn next_input(&mut self, ty: &str) -> Result<Value, String> {
        self.next_stream_input("STDIN", ty)
    }

    /// 从命名流取一个输入项（STDIN 是默认流）。
    fn next_stream_input(&mut self, stream: &str, ty: &str) -> Result<Value, String> {
        let text = self.next_raw(stream)?;
        literal_for(&text, ty)
    }

    /// 取一个原始输入项（不做类型解析，getc / 字符串读取用）。
    fn next_raw(&mut self, stream: &str) -> Result<String, String> {
        let items = self.stream_inputs.entry(stream.to_string()).or_default();
        let cursor = self.stream_cursors.entry(stream.to_string()).or_insert(0);
        if *cursor >= items.len() {
            return Err(format!("Input exhausted on stream {stream}"));
        }
        let text = items[*cursor].clone();
        *cursor += 1;
        Ok(text)
    }

    /// 字符串操作数：字面量直接取文本，变量按声明读成 str。
    fn string_operand(&self, token: &str) -> Result<Value, String> {
        if is_string_literal(token) {
            return Ok(string_result_token(token));
        }
        if self.is_register(token) {
            let value = self.registers.get(token).cloned().unwrap_or_else(|| Value::zero("str"));
            return convert_value(&value, "str");
        }
        self.read_slot(token, "str")
    }

    /// 流句柄：写 STDIN / "STDIN" 都行，变量则取其字符串值。
    fn stream_handle(&self, token: &str) -> Result<String, String> {
        if is_string_literal(token) {
            return Ok(string_literal_text(token));
        }
        if !self.is_register(token) && identifier(token) && !self.var_types.contains_key(token) {
            return Ok(token.to_string());
        }
        Ok(string_value(&self.string_operand(token)?)?.to_string())
    }

    fn as_index(&self, value: &Value) -> Result<i64, String> {
        if value.is_string() {
            return Err("Expected a numeric index or length".into());
        }
        if value.is_float() {
            return Ok(value.real as i64);
        }
        display_value(value).parse().map_err(|_| "invalid index".to_string())
    }

    /// 写流：STDOUT 走 output/text，其他流按流名收集。
    fn stream_write(&mut self, stream: &str, value: &Value) {
        if stream.is_empty() || stream == "STDOUT" {
            if value.is_string() {
                self.text.push_str(&value.text);
            } else {
                self.output.push(value.clone());
            }
        } else {
            let rendered = if value.is_string() {
                value.text.clone()
            } else {
                display_value(value)
            };
            self.stream_text.entry(stream.to_string()).or_default().push_str(&rendered);
            if !value.is_string() {
                self.streams_out.entry(stream.to_string()).or_default().push(value.clone());
            }
        }
    }

    fn call(&mut self, function: &str, destination: &str, interrupt: bool) -> Result<(), String> {
        let pending = std::mem::take(&mut self.frames.last_mut().expect("frame").pending);
        let frame = Frame {
            return_pc: self.cursor,
            destination: destination.to_string(),
            has_destination: destination != "_" && !destination.is_empty(),
            interrupt,
            saved_regs: self.registers.clone(),
            saved_carry: self.carry,
            ..Default::default()
        };
        self.frames.push(frame);
        self.registers.clear();
        self.carry = false;
        for (name, value) in pending {
            self.whole_write(&name, &value)?;
        }
        self.jump(function)
    }

    fn do_return(&mut self, interrupt: bool) -> Result<(), String> {
        if self.frames.len() <= 1 {
            return Err(if interrupt { "IRET without INT".into() } else { "RET without CALL".into() });
        }
        let frame = self.frames.pop().expect("frame");
        if interrupt && !frame.interrupt {
            return Err("IRET without INT".into());
        }
        self.registers = frame.saved_regs;
        self.carry = frame.saved_carry;
        self.cursor = frame.return_pc;
        if frame.has_return && frame.has_destination {
            self.assign(&frame.destination, &frame.return_value)?;
        }
        Ok(())
    }
}

/// 执行生成的汇编文本；`input` 是 STDIN 上 `read` 依次取用的字面量。
pub fn simulate(assembly: &str, input: &[String], step_limit: usize) -> Result<Execution, String> {
    let mut streams: HashMap<String, Vec<String>> = HashMap::new();
    streams.insert("STDIN".to_string(), input.to_vec());
    simulate_streams(assembly, &streams, step_limit)
}

/// 带命名流的执行：`streams` 是流名（STDIN / STDOUT / STDERR / 自定义）到输入项的映射。
pub fn simulate_streams(
    assembly: &str,
    streams: &HashMap<String, Vec<String>>,
    step_limit: usize,
) -> Result<Execution, String> {
    let mut machine = Machine {
        target: Target::Mips,
        word: 16,
        pair_hi: "HI".into(),
        pair_lo: "LO".into(),
        var_types: HashMap::new(),
        local_names: HashSet::new(),
        read_only: HashSet::new(),
        registers: HashMap::new(),
        memory: HashMap::new(),
        frames: vec![Frame::default()],
        carry: false,
        compared: None,
        output: Vec::new(),
        text: String::new(),
        steps: 0,
        labels: HashMap::new(),
        code: Vec::new(),
        cursor: 0,
        stream_inputs: streams.clone(),
        stream_cursors: HashMap::new(),
        stream_text: HashMap::new(),
        streams_out: HashMap::new(),
        open_files: HashMap::new(),
        data_area: Vec::new(),
        data_area_symbols: HashMap::new(),
        data_area_loaded: false,
        ticks: 0,
        random_state: 20260918,
    };

    #[derive(PartialEq)]
    enum Section {
        None,
        Data,
        Rodata,
        DataArea,
    }
    let mut section = Section::None;
    let mut data_area_size = 0usize;
    let mut in_code = false;
    for raw in assembly.lines() {
        let text = raw.trim();
        if text.is_empty() || text.starts_with(';') {
            continue;
        }
        if !in_code {
            if let Some(rest) = text.strip_prefix(".target") {
                machine.target = match rest.trim() {
                    "mips" => Target::Mips,
                    "8086" => Target::I8086,
                    "arm" => Target::Arm,
                    other => return Err(format!("Unknown target: {other}")),
                };
                continue;
            }
            if let Some(rest) = text.strip_prefix(".bits") {
                machine.word = rest.trim().parse().map_err(|_| "invalid .bits")?;
                continue;
            }
            if let Some(rest) = text.strip_prefix(".mulpair") {
                let parts = split_fields(rest);
                if parts.len() == 2 {
                    machine.pair_hi = parts[0].clone();
                    machine.pair_lo = parts[1].clone();
                }
                continue;
            }
            if let Some(rest) = text.strip_prefix(".reg") {
                machine.registers.insert(rest.trim().to_string(), Value::zero(default_type()));
                continue;
            }
            if text == ".data" {
                section = Section::Data;
                continue;
            }
            if text == ".dataarea" || text.starts_with(".dataarea ") {
                // 段头带的是字节总数（`.dataarea 124`）：字节仍按 .hex 行装载，
                // 没写满的部分在解析结束时补零，地址空间与生成器声明的一致。
                section = Section::DataArea;
                machine.data_area_loaded = true;
                let rest = text[".dataarea".len()..].trim();
                if !rest.is_empty() {
                    data_area_size = rest
                        .parse()
                        .map_err(|_| format!("Invalid .dataarea size: {raw}"))?;
                }
                continue;
            }
            if text == ".enddataarea" {
                section = Section::None;
                continue;
            }
            if text == ".enddata" || text == ".endrodata" {
                section = Section::None;
                continue;
            }
            if text == ".rodata" {
                section = Section::Rodata;
                continue;
            }
            if text.starts_with(".stack") {
                continue;
            }
            if text == ".code" {
                in_code = true;
                continue;
            }
            if section == Section::None {
                continue;
            }
            if section == Section::DataArea {
                // 符号基址：`.sym 符号 偏移`（simple_ast 的 .layout 表里的 offset）。
                if let Some(rest) = text.strip_prefix(".sym") {
                    let parts = split_fields(rest);
                    if parts.len() != 2 {
                        return Err(format!("Invalid .sym (need symbol and offset): {raw}"));
                    }
                    let offset = parts[1]
                        .parse::<u64>()
                        .map_err(|_| format!("Invalid .sym offset: {raw}"))?;
                    machine.data_area_symbols.insert(parts[0].clone(), offset);
                    continue;
                }
                // 字节镜像：与 `.data` 段同一套紧凑写法（16 字节一行的 .hex，或 .bytes）。
                if text.starts_with(".hex") || text.starts_with(".bytes") {
                    let hex = text.starts_with(".hex");
                    let rest = text[if hex { 4 } else { 6 }..].trim();
                    let (head, body) = match rest.split_once(char::is_whitespace) {
                        Some((head, body)) => (head, body.trim()),
                        None => (rest, ""),
                    };
                    let offset = head
                        .parse::<usize>()
                        .map_err(|_| format!("Invalid byte directive: {raw}"))?;
                    let bytes = if hex {
                        let digits: Vec<char> =
                            body.chars().filter(|ch| !ch.is_whitespace()).collect();
                        let mut out = Vec::new();
                        for pair in digits.chunks(2) {
                            let text: String = pair.iter().collect();
                            out.push(
                                u8::from_str_radix(&text, 16)
                                    .map_err(|_| format!("Invalid hex byte: {raw}"))?,
                            );
                        }
                        out
                    } else {
                        let mut out = Vec::new();
                        for item in body.split(',') {
                            let item = item.trim();
                            if item.is_empty() {
                                continue;
                            }
                            out.push(
                                item.parse::<u8>()
                                    .map_err(|_| format!("Byte out of range (0..255): {raw}"))?,
                            );
                        }
                        out
                    };
                    for (index, byte) in bytes.into_iter().enumerate() {
                        let at = offset + index;
                        if machine.data_area.len() < at + 1 {
                            machine.data_area.resize(at + 1, 0);
                        }
                        machine.data_area[at] = byte;
                    }
                    continue;
                }
                continue;
            }
            // 声明行：[local ]name[ = value][ : type]
            let mut line = text.to_string();
            let local = line.starts_with("local ");
            if local {
                line = line[6..].trim().to_string();
            }
            let mut ty = default_type().to_string();
            if let Some(index) = line.rfind(':') {
                ty = line[index + 1..].trim().to_string();
                line = line[..index].trim().to_string();
            }
            if !is_type_name(&ty) {
                return Err(format!("Invalid declaration: {raw}"));
            }
            let (name, value) = match line.split_once('=') {
                Some((name, value)) => (name.trim().to_string(), Some(value.trim().to_string())),
                None => (line.trim().to_string(), None),
            };
            if name.is_empty() {
                return Err(format!("Invalid declaration: {raw}"));
            }
            machine.var_types.insert(name.clone(), ty.clone());
            if local {
                machine.local_names.insert(name.clone());
            }
            if section == Section::Rodata {
                machine.read_only.insert(name.clone());
            }
            if let Some(text) = value {
                let parsed = literal_value(&text, &ty)?;
                machine.memory.insert(name, parsed);
            }
            continue;
        }
        if text.ends_with(':') {
            let label = text[..text.len() - 1].trim().to_string();
            machine.labels.insert(label, machine.code.len());
            continue;
        }
        machine.code.push(split_fields(text));
    }
    // `.dataarea` 段头声明的字节总数：`.hex` 没写满的部分补零，
    // 这样地址空间与生成器声明的一致（越界判定也跟着对）。
    if machine.data_area.len() < data_area_size {
        machine.data_area.resize(data_area_size, 0);
    }

    let steps_limit = step_limit;
    while machine.cursor < machine.code.len() {
        if machine.steps >= steps_limit {
            return Err("Instruction limit exceeded".into());
        }
        machine.steps += 1;
        let words = machine.code[machine.cursor].clone();
        machine.cursor += 1;
        if words.is_empty() {
            continue;
        }
        let full = words[0].clone();
        // 汇编里的类型后缀用点分隔（MOV.u16），与四元式的冒号写法不同。
        let (base, ty) = match full.find('.') {
            Some(index) => (full[..index].to_string(), full[index + 1..].to_string()),
            None => (full.clone(), default_type().to_string()),
        };
        let base = base.as_str();
        if !is_type_name(&ty) {
            return Err(format!("Unknown type in instruction: {full}"));
        }
        let operands = &words[1..];
        let need = |count: usize| -> Result<(), String> {
            if operands.len() != count {
                Err(format!("Invalid operands for {full}"))
            } else {
                Ok(())
            }
        };
        let check_width = || -> Result<(), String> {
            if !is_float_type(&ty) && type_bits(&ty) > machine.word {
                Err(format!(
                    "Instruction {full} needs {}-bit ALU, but the machine word is {} bits",
                    type_bits(&ty),
                    machine.word
                ))
            } else {
                Ok(())
            }
        };

        match base {
            "HALT" => {
                need(0)?;
                return Ok(Execution {
                    memory: machine.memory,
                    output: machine.output,
                    text: machine.text,
                    stream_text: machine.stream_text,
                    streams: machine.streams_out,
                    exit_code: 0,
                    steps: machine.steps,
                });
            }
            "RET" | "IRET" => {
                if base == "RET" && operands.len() == 1 {
                    let value = machine.operand(&operands[0], &ty)?;
                    let frame = machine.frames.last_mut().expect("frame");
                    frame.has_return = true;
                    frame.return_value = value;
                } else {
                    need(0)?;
                }
                machine.do_return(base == "IRET")?;
            }
            "RETW" => {
                need(1)?;
                let value = machine.whole_operand(&operands[0], &ty)?;
                let frame = machine.frames.last_mut().expect("frame");
                frame.has_return = true;
                frame.return_value = value;
                machine.do_return(false)?;
            }
            "CALL" | "INT" => {
                if operands.is_empty() || operands.len() > 2 {
                    return Err(format!("Invalid operands for {base}"));
                }
                let destination = operands.get(1).cloned().unwrap_or_else(|| "_".to_string());
                let function = format!("L_{}", operands[0]);
                machine.call(&function, &destination, base == "INT")?;
            }
            "PUSH" | "PUSHW" => {
                need(2)?;
                let value = if base == "PUSH" {
                    machine.operand(&operands[0], &ty)?
                } else {
                    machine.whole_operand(&operands[0], &ty)?
                };
                machine.frames.last_mut().expect("frame").pending.push((operands[1].clone(), value));
            }
            "J" | "B" | "JMP" => {
                need(1)?;
                machine.jump(&operands[0])?;
            }
            "BEQ" | "BNE" | "BLT" | "BLE" | "BGT" | "BGE" | "JE" | "JNE" | "JL" | "JLE"
            | "JG" | "JGE" => {
                let condition = match base {
                    "BEQ" | "JE" => "==",
                    "BNE" | "JNE" => "!=",
                    "BLT" | "JL" => "<",
                    "BLE" | "JLE" => "<=",
                    "BGT" | "JG" => ">",
                    _ => ">=",
                };
                if operands.len() == 1 {
                    let (lhs, rhs, compare_type) = machine
                        .compared
                        .clone()
                        .ok_or("Branch without CMP")?;
                    if !value_is_zero(&eval_typed(condition, &compare_type, &lhs, &rhs)?) {
                        machine.jump(&operands[0])?;
                    }
                } else if operands.len() == 3 {
                    check_width()?;
                    let lhs = machine.operand(&operands[0], &ty)?;
                    let rhs = machine.operand(&operands[1], &ty)?;
                    if !value_is_zero(&eval_typed(condition, &ty, &lhs, &rhs)?) {
                        machine.jump(&operands[2])?;
                    }
                } else {
                    return Err(format!("Invalid operands for {base}"));
                }
            }
            "BEQZ" | "BNEZ" => {
                need(2)?;
                check_width()?;
                let value = machine.operand(&operands[0], &ty)?;
                if value_is_zero(&value) == (base == "BEQZ") {
                    machine.jump(&operands[1])?;
                }
            }
            "CMP" | "FCMP" => {
                need(2)?;
                let lhs = machine.operand(&operands[0], &ty)?;
                let rhs = machine.operand(&operands[1], &ty)?;
                machine.compared = Some((lhs, rhs, ty.clone()));
            }
            "CMPW" => {
                need(2)?;
                let lhs = machine.whole_operand(&operands[0], &ty)?;
                let rhs = machine.whole_operand(&operands[1], &ty)?;
                machine.compared = Some((lhs, rhs, ty.clone()));
            }
            "MOV" | "LI" => {
                need(2)?;
                let value = machine.operand(&operands[1], &ty)?;
                machine.store_operand(&operands[0], &value, &ty)?;
            }
            "CVT" => {
                need(2)?;
                let source = if machine.is_register(&operands[1]) {
                    machine.registers.get(&operands[1]).cloned().unwrap_or_else(|| Value::zero(&ty))
                } else {
                    machine.read_slot(&operands[1], &machine.slot_type(&operands[1]))?
                };
                let converted = convert_value(&source, &ty)?;
                machine.store_operand(&operands[0], &converted, &ty)?;
            }
            "IN" | "GETC" => {
                need(1)?;
                let text = machine.next_raw("STDIN")?;
                let value = if base == "GETC" {
                    let code = text.chars().next().map(|c| c as u32).unwrap_or(0);
                    literal_value(&code.to_string(), &ty)?
                } else {
                    literal_value(&text, &ty)?
                };
                machine.store_operand(&operands[0], &value, &ty)?;
            }
            "OUT" => {
                need(1)?;
                let value = machine.operand(&operands[0], &ty)?;
                machine.output.push(convert_value(&value, &ty)?);
            }
            "OUTW" => {
                need(1)?;
                let value = machine.whole_operand(&operands[0], &ty)?;
                machine.output.push(convert_value(&value, &ty)?);
            }
            "PUTC" => {
                need(1)?;
                let value = machine.operand(&operands[0], &ty)?;
                machine.text.push(value_low_byte(&value) as char);
            }
            "PRINT" => {
                need(1)?;
                machine.text.push_str(&unescape(&strip_quotes(&operands[0])));
            }
            "READW" => {
                need(1)?;
                let value = machine.next_input(&ty)?;
                machine.whole_write(&operands[0], &value)?;
            }
            "GETCW" => {
                need(1)?;
                let text = machine.next_raw("STDIN")?;
                let code = text.chars().next().map(|c| c as u32).unwrap_or(0);
                let value = literal_value(&code.to_string(), &ty)?;
                machine.whole_write(&operands[0], &value)?;
            }
            "RCALL" | "RCALLW" => {
                if base == "RCALL" {
                    need(4)?;
                    let lhs = machine.whole_operand(&operands[2], &ty)?;
                    let rhs = machine.whole_operand(&operands[3], &ty)?;
                    let value = eval_typed(&operands[0], &ty, &lhs, &rhs)?;
                    machine.assign(&operands[1], &value)?;
                } else {
                    need(5)?;
                    let lhs = machine.whole_operand(&operands[3], &ty)?;
                    let rhs = machine.whole_operand(&operands[4], &ty)?;
                    let (first, second) = if operands[0] == "mul" {
                        let (hi, lo) = multiply_wide(&ty, &lhs, &rhs)?;
                        (lo, hi)
                    } else {
                        divide_wide(&ty, &lhs, &rhs)?
                    };
                    machine.assign(&operands[1], &first)?;
                    machine.assign(&operands[2], &second)?;
                }
            }
            // ---- 字符串指令 ----
            "SCOPY" | "SLEN" | "SCMP" | "SCAT" | "SSUB" | "SFIND" => {
                if base == "SCOPY" {
                    need(2)?;
                    let value = machine.string_operand(&operands[1])?;
                    let name = operands[0].clone();
                    machine.assign(&name, &value)?;
                } else if base == "SLEN" {
                    need(2)?;
                    let value = machine.string_operand(&operands[1])?;
                    let result_type = machine.resolved_type(&operands[0]);
                    let length = string_length(&value, &result_type)?;
                    let name = operands[0].clone();
                    machine.assign(&name, &length)?;
                } else if base == "SCMP" {
                    need(3)?;
                    let lhs = machine.string_operand(&operands[1])?;
                    let rhs = machine.string_operand(&operands[2])?;
                    let result_type = machine.resolved_type(&operands[0]);
                    let order = string_compare(&lhs, &rhs, &result_type)?;
                    let name = operands[0].clone();
                    machine.assign(&name, &order)?;
                } else if base == "SCAT" {
                    need(3)?;
                    let lhs = machine.string_operand(&operands[1])?;
                    let rhs = machine.string_operand(&operands[2])?;
                    let text = string_concat(&lhs, &rhs)?;
                    let name = operands[0].clone();
                    machine.assign(&name, &text)?;
                } else if base == "SSUB" {
                    need(4)?;
                    let source = machine.string_operand(&operands[1])?;
                    let start = machine.as_index(&machine.operand(&operands[2], "i32")?)?;
                    let length = machine.as_index(&machine.operand(&operands[3], "i32")?)?;
                    let text = substring(&source, start, length)?;
                    let name = operands[0].clone();
                    machine.assign(&name, &text)?;
                } else {
                    need(3)?;
                    let haystack = machine.string_operand(&operands[1])?;
                    let needle = machine.string_operand(&operands[2])?;
                    let result_type = machine.resolved_type(&operands[0]);
                    let position = string_find(&haystack, &needle, &result_type)?;
                    let name = operands[0].clone();
                    machine.assign(&name, &position)?;
                }
            }
            // ---- 流指令 ----
            "STREAM" => {
                need(2)?;
                let handle = machine.stream_handle(&operands[1])?;
                let mut value = Value::zero("str");
                value.text = handle;
                let name = operands[0].clone();
                machine.assign(&name, &value)?;
            }
            "SREAD" => {
                need(2)?;
                let stream = machine.stream_handle(&operands[1])?;
                let value_type = machine.resolved_type(&operands[0]);
                let value = if is_string_type(&value_type) {
                    let text = machine.next_raw(&stream)?;
                    let mut value = Value::zero("str");
                    value.text = text;
                    value
                } else {
                    machine.next_stream_input(&stream, &value_type)?
                };
                let name = operands[0].clone();
                machine.assign(&name, &value)?;
            }
            "SWRITE" => {
                need(2)?;
                let stream = machine.stream_handle(&operands[0])?;
                let value = if is_string_type(&ty) {
                    machine.string_operand(&operands[1])?
                } else {
                    machine.whole_operand(&operands[1], &ty)?
                };
                machine.stream_write(&stream, &value);
            }
            "SCLOSE" => {
                need(1)?;
            }
            // ---- 数据区三件套：地址是数据区里的字节偏移，读写都是 8 字节小端 ----
            // 与 library/stackvm 的 ADDR / LOADAT / STOREAT 同一套语义与报错文案。
            "ADDR" => {
                need(2)?;
                if !machine.data_area_loaded {
                    return Err(format!("没有加载数据区，无法取符号地址：{}", operands[1]));
                }
                let Some(offset) = machine.data_area_symbols.get(&operands[1]).copied() else {
                    return Err(format!("数据区里没有符号：{}", operands[1]));
                };
                // 基址按目标变量的类型存放（i64 时窄机器上自动分片）。
                let name = operands[0].clone();
                let ty = machine.slot_type(&name);
                let value = literal_for(&offset.to_string(), &ty)?;
                machine.assign(&name, &value)?;
            }
            "LOADAT" | "STOREAT" => {
                need(2)?;
                let reading = base == "LOADAT";
                if !machine.data_area_loaded {
                    return Err(format!(
                        "没有加载数据区，无法按地址{}",
                        if reading { "取值" } else { "写值" }
                    ));
                }
                // load 是 `LOADAT 目标, 指针`，store 是 `STOREAT 指针, 值`。
                let pointer = if reading { operands[1].clone() } else { operands[0].clone() };
                let address = machine.as_index(&machine.whole_operand(&pointer, "u64")?)?;
                if address < 0 || address as usize + 8 > machine.data_area.len() {
                    return Err(format!(
                        "数据区越界{}：地址 {address}",
                        if reading { "读" } else { "写" }
                    ));
                }
                let at = address as usize;
                if reading {
                    // 小端 8 字节 -> 位模式，再按目标变量的类型收进去。
                    let mut bits: u64 = 0;
                    for index in (0..8).rev() {
                        bits = (bits << 8) | machine.data_area[at + index] as u64;
                    }
                    let name = operands[0].clone();
                    let slot = machine.slot_type(&name);
                    // 目标变量是浮点时这 8 个字节是 IEEE-754 双精度**位模式**
                    // （数据区每个槽位就是 8 字节，`(load:f64 …)` 带类型来区分）。
                    let value = if is_float_type(&slot) {
                        let mut value = Value::zero(&slot);
                        value.bits = bits as u128;
                        value.real = f64::from_bits(bits);
                        value
                    } else {
                        convert_value(&literal_for(&bits.to_string(), "u64")?, &slot)?
                    };
                    machine.assign(&name, &value)?;
                } else {
                    // 值是浮点：8 个字节写它的 IEEE-754 位模式；整型：按自己的类型
                    // 补齐到 8 字节（有符号类型符号扩展）。
                    let value = machine.whole_operand(&operands[1], &ty)?;
                    let bits: u64 = if value.is_float() {
                        value.real.to_bits()
                    } else {
                        convert_value(&value, "u64")?.bits as u64
                    };
                    for index in 0..8 {
                        machine.data_area[at + index] = ((bits >> (index * 8)) & 0xFF) as u8;
                    }
                }
            }
            // ---- 向量指令 ----
            "VLOAD" => {
                need(3)?;
                let index = machine.as_index(&machine.operand(&operands[2], "i32")?)?;
                let element = format!("{}.{}", operands[1], index);
                let value = machine.whole_read(&element, &ty)?;
                let name = operands[0].clone();
                machine.assign(&name, &value)?;
            }
            "VSTORE" => {
                need(3)?;
                let index = machine.as_index(&machine.operand(&operands[1], "i32")?)?;
                let element = format!("{}.{}", operands[0], index);
                let value = machine.operand(&operands[2], &ty)?;
                machine.whole_write(&element, &value)?;
            }
            "VFILL" => {
                need(3)?;
                let length = machine.as_index(&machine.operand(&operands[1], "i32")?)?;
                let value = machine.operand(&operands[2], &ty)?;
                for index in 0..length {
                    machine.whole_write(&format!("{}.{}", operands[0], index), &value)?;
                }
            }
            "VCOPY" => {
                need(3)?;
                let length = machine.as_index(&machine.operand(&operands[2], "i32")?)?;
                for index in 0..length {
                    let value = machine.whole_read(&format!("{}.{}", operands[1], index), &ty)?;
                    machine.whole_write(&format!("{}.{}", operands[0], index), &value)?;
                }
            }
            "VSUM" | "VMAX" | "VMIN" => {
                need(3)?;
                let length = machine.as_index(&machine.operand(&operands[2], "i32")?)?;
                let mut accumulator: Option<Value> = None;
                for index in 0..length {
                    let element = machine.whole_read(&format!("{}.{}", operands[1], index), &ty)?;
                    accumulator = Some(match accumulator {
                        None => element,
                        Some(current) => {
                            if base == "VSUM" {
                                eval_typed("+", &ty, &current, &element)?
                            } else {
                                let less = eval_typed("<", &ty, &element, &current)?;
                                let greater = eval_typed(">", &ty, &element, &current)?;
                                let take = if base == "VMAX" { greater } else { less };
                                if value_is_zero(&take) { current } else { element }
                            }
                        }
                    });
                }
                let value = accumulator.unwrap_or_else(|| Value::zero(&ty));
                let name = operands[0].clone();
                machine.assign(&name, &value)?;
            }
            // ---- 外部文件 IO ----
            "FOPEN" => {
                need(3)?;
                let path = if is_string_literal(&operands[1]) {
                    string_literal_text(&operands[1])
                } else {
                    string_value(&machine.string_operand(&operands[1])?)?.to_string()
                };
                let mode = if is_string_literal(&operands[2]) {
                    string_literal_text(&operands[2])
                } else {
                    string_value(&machine.string_operand(&operands[2])?)?.to_string()
                };
                let file = match mode.as_str() {
                    "w" => OpenFile::Writer(
                        std::fs::File::create(&path)
                            .map_err(|error| format!("Cannot open file {path}: {error}"))?,
                    ),
                    "a" => OpenFile::Writer(
                        std::fs::OpenOptions::new()
                            .create(true)
                            .append(true)
                            .open(&path)
                            .map_err(|error| format!("Cannot open file {path}: {error}"))?,
                    ),
                    _ => {
                        let text = std::fs::read_to_string(&path)
                            .map_err(|error| format!("Cannot open file {path}: {error}"))?;
                        OpenFile::Reader { text, position: 0 }
                    }
                };
                machine.open_files.insert(path.clone(), file);
                let mut handle = Value::zero("str");
                handle.text = path;
                let name = operands[0].clone();
                machine.assign(&name, &handle)?;
            }
            "FREAD" => {
                need(2)?;
                let path = machine.stream_handle(&operands[1])?;
                let value_type = machine.resolved_type(&operands[0]);
                let value = match machine.open_files.get_mut(&path) {
                    Some(OpenFile::Reader { text, position }) => {
                        if is_string_type(&value_type) {
                            let rest = &text[*position..];
                            let (line, consumed) = match rest.find('\n') {
                                Some(index) => (rest[..index].to_string(), index + 1),
                                None => (rest.to_string(), rest.len()),
                            };
                            *position += consumed;
                            let mut value = Value::zero("str");
                            value.text = line.trim_end_matches('\r').to_string();
                            value
                        } else {
                            let bytes = text.as_bytes();
                            while *position < bytes.len()
                                && (bytes[*position] as char).is_whitespace()
                            {
                                *position += 1;
                            }
                            let start = *position;
                            while *position < bytes.len()
                                && !(bytes[*position] as char).is_whitespace()
                            {
                                *position += 1;
                            }
                            if start == *position {
                                return Err(format!("Read past end of file: {path}"));
                            }
                            literal_value(&text[start..*position], &value_type)?
                        }
                    }
                    _ => return Err(format!("File is not open: {path}")),
                };
                let name = operands[0].clone();
                machine.assign(&name, &value)?;
            }
            "FWRITE" => {
                need(2)?;
                let path = machine.stream_handle(&operands[0])?;
                let value = if is_string_type(&ty) {
                    machine.string_operand(&operands[1])?
                } else {
                    machine.whole_operand(&operands[1], &ty)?
                };
                match machine.open_files.get_mut(&path) {
                    Some(OpenFile::Writer(file)) => {
                        use std::io::Write;
                        let text = if value.is_string() {
                            format!("{}\n", value.text)
                        } else {
                            display_value(&value)
                        };
                        write!(file, "{text}")
                            .map_err(|error| format!("Cannot write file {path}: {error}"))?;
                    }
                    _ => return Err(format!("File is not open: {path}")),
                }
            }
            "FEOF" => {
                need(2)?;
                let path = machine.stream_handle(&operands[1])?;
                let eof = match machine.open_files.get(&path) {
                    Some(OpenFile::Reader { text, position }) => *position >= text.len(),
                    _ => return Err(format!("File is not open: {path}")),
                };
                let result_type = machine.resolved_type(&operands[0]);
                let value = if is_float_type(&result_type) {
                    make_float(if eof { 1.0 } else { 0.0 }, &result_type)
                } else {
                    make_integer(if eof { 1 } else { 0 }, &result_type)
                };
                let name = operands[0].clone();
                machine.assign(&name, &value)?;
            }
            "FCLOSE" => {
                need(1)?;
                let path = machine.stream_handle(&operands[0])?;
                machine.open_files.remove(&path);
            }
            // ---- 系统调用 ----
            "SYSCALL" => {
                need(3)?;
                let id = operands[0].clone();
                let destination = operands[1].clone();
                let has_destination = destination != "_" && !destination.is_empty();
                let result_type =
                    machine.resolved_type(if has_destination { &destination } else { "i32" });
                if id == "exit" {
                    let code = machine.as_index(&machine.operand(&operands[2], "i32")?)?;
                    return Ok(Execution {
                        memory: machine.memory,
                        output: machine.output,
                        text: machine.text,
                        stream_text: machine.stream_text,
                        streams: machine.streams_out,
                        exit_code: code as i32,
                        steps: machine.steps,
                    });
                }
                let value = if id == "time" || id == "ticks" {
                    machine.ticks += machine.steps as i64;
                    literal_value(&machine.ticks.to_string(), &result_type)?
                } else if id == "getpid" {
                    literal_value("1", &result_type)?
                } else if id == "rand" {
                    machine.random_state =
                        machine.random_state.wrapping_mul(6364136223846793005).wrapping_add(1442695040888963407);
                    let next = (machine.random_state >> 33) & 0x7FFF_FFFF;
                    literal_value(&next.to_string(), &result_type)?
                } else if id == "brk" {
                    machine.ticks += 64;
                    literal_value(&(1024 + machine.ticks).to_string(), &result_type)?
                } else if id == "putchar" {
                    let byte = value_low_byte(&machine.operand(&operands[2], "u8")?);
                    machine.text.push(byte as char);
                    continue;
                } else if id == "getchar" {
                    let text = machine.next_raw("STDIN")?;
                    let code = text.chars().next().map(|c| c as u32).unwrap_or(0);
                    literal_value(&code.to_string(), &result_type)?
                } else {
                    return Err(format!("Unknown system call: {id}"));
                };
                if has_destination {
                    machine.assign(&destination, &value)?;
                }
            }
            _ => {
                if matches!(
                    base,
                    "LD" | "LB" | "LBU" | "LH" | "LHU" | "LW" | "LQ" | "LDRSB" | "LDRB"
                        | "LDRSH" | "LDRH" | "LDR" | "LDRD" | "LDRQ" | "LWC1" | "LDC1" | "VLDR"
                        | "VLDRD"
                ) {
                    need(2)?;
                    check_width()?;
                    let value = machine.operand(&operands[1], &ty)?;
                    machine.store_operand(&operands[0], &value, &ty)?;
                } else if matches!(
                    base,
                    "ST" | "SB" | "SH" | "SW" | "SQ" | "STRB" | "STRH" | "STR" | "STRD"
                        | "STRQ" | "SWC1" | "SDC1" | "VSTR" | "VSTRD"
                ) {
                    need(2)?;
                    check_width()?;
                    let memory_operand = |token: &str| {
                        (token.len() >= 2 && token.starts_with('[') && token.ends_with(']'))
                            || (!machine.is_register(token) && !is_numeric_literal(token))
                    };
                    if memory_operand(&operands[0]) {
                        let value = machine.operand(&operands[1], &ty)?;
                        machine.store_operand(&operands[0], &value, &ty)?;
                    } else if memory_operand(&operands[1]) {
                        let value = machine.operand(&operands[0], &ty)?;
                        machine.store_operand(&operands[1], &value, &ty)?;
                    } else {
                        return Err(format!("Store without a memory operand: {full}"));
                    }
                } else if base == "ADDC" || base == "SUBB" {
                    check_width()?;
                    let (destination, lhs, rhs) = if operands.len() == 3 {
                        (
                            operands[0].clone(),
                            machine.operand(&operands[1], &ty)?,
                            machine.operand(&operands[2], &ty)?,
                        )
                    } else if operands.len() == 2 {
                        let lhs = machine.operand(&operands[0], &ty)?;
                        let rhs = machine.operand(&operands[1], &ty)?;
                        (operands[0].clone(), lhs, rhs)
                    } else {
                        return Err(format!("Invalid operands for {base}"));
                    };
                    let (value, flag) = if base == "ADDC" {
                        add_with_carry(&ty, &lhs, &rhs, machine.carry)?
                    } else {
                        sub_with_borrow(&ty, &lhs, &rhs, machine.carry)?
                    };
                    machine.carry = flag;
                    machine.store_operand(&destination, &value, &ty)?;
                } else if matches!(base, "NEG" | "FNEG" | "VNEG" | "FCHS") {
                    check_width()?;
                    let source = if operands.len() == 2 { &operands[1] } else { &operands[0] };
                    let value = machine.operand(source, &ty)?;
                    let negated = eval_typed("neg", &ty, &value, &Value::zero(&ty))?;
                    machine.store_operand(&operands[0], &negated, &ty)?;
                } else if base == "NOT" || base == "MVN" || base == "NOR" {
                    check_width()?;
                    if operands.len() == 1 {
                        let value = machine.operand(&operands[0], &ty)?;
                        let inverted = eval_typed("~", &ty, &value, &Value::zero(&ty))?;
                        machine.store_operand(&operands[0], &inverted, &ty)?;
                    } else if operands.len() == 2 {
                        let value = machine.operand(&operands[1], &ty)?;
                        let inverted = eval_typed("~", &ty, &value, &Value::zero(&ty))?;
                        machine.store_operand(&operands[0], &inverted, &ty)?;
                    } else if operands.len() == 3 {
                        let lhs = machine.operand(&operands[1], &ty)?;
                        let rhs = machine.operand(&operands[2], &ty)?;
                        let combined = eval_typed("|", &ty, &lhs, &rhs)?;
                        let inverted = eval_typed("~", &ty, &combined, &Value::zero(&ty))?;
                        machine.store_operand(&operands[0], &inverted, &ty)?;
                    } else {
                        return Err(format!("Invalid operands for {base}"));
                    }
                } else if matches!(
                    base,
                    "ADD" | "ADDU" | "FADD" | "VADD" | "SUB" | "SUBU" | "FSUB" | "VSUB" | "AND"
                        | "OR" | "ORR" | "XOR" | "EOR" | "FMUL" | "VMUL" | "FDIV" | "VDIV"
                ) {
                    check_width()?;
                    let operation = match base {
                        "ADD" | "ADDU" | "FADD" | "VADD" => "+",
                        "SUB" | "SUBU" | "FSUB" | "VSUB" => "-",
                        "FMUL" | "VMUL" => "*",
                        "FDIV" | "VDIV" => "/",
                        "AND" => "&",
                        "OR" | "ORR" => "|",
                        _ => "^",
                    };
                    let (destination, lhs, rhs) = if operands.len() == 3 {
                        (
                            operands[0].clone(),
                            machine.operand(&operands[1], &ty)?,
                            machine.operand(&operands[2], &ty)?,
                        )
                    } else if operands.len() == 2 {
                        let lhs = machine.operand(&operands[0], &ty)?;
                        let rhs = machine.operand(&operands[1], &ty)?;
                        (operands[0].clone(), lhs, rhs)
                    } else {
                        return Err(format!("Invalid operands for {base}"));
                    };
                    let value = eval_typed(operation, &ty, &lhs, &rhs)?;
                    machine.store_operand(&destination, &value, &ty)?;
                } else if matches!(
                    base,
                    "SLL" | "SHL" | "LSL" | "SRA" | "SAR" | "ASR" | "SRL" | "SHR" | "LSR"
                ) {
                    check_width()?;
                    let left = matches!(base, "SLL" | "SHL" | "LSL");
                    let logical = matches!(base, "SRL" | "SHR" | "LSR");
                    let shift_type = if left || is_unsigned_type(&ty) {
                        ty.clone()
                    } else if logical {
                        format!("u{}", type_bits(&ty))
                    } else {
                        format!("i{}", type_bits(&ty))
                    };
                    let (destination, lhs, rhs) = if operands.len() == 3 {
                        (
                            operands[0].clone(),
                            machine.operand(&operands[1], &shift_type)?,
                            machine.operand(&operands[2], &shift_type)?,
                        )
                    } else if operands.len() == 2 {
                        let lhs = machine.operand(&operands[0], &shift_type)?;
                        let rhs = machine.operand(&operands[1], &shift_type)?;
                        (operands[0].clone(), lhs, rhs)
                    } else {
                        return Err(format!("Invalid operands for {base}"));
                    };
                    let value = eval_typed(if left { "<<" } else { ">>" }, &shift_type, &lhs, &rhs)?;
                    machine.store_operand(&destination, &value, &ty)?;
                } else if base == "UMULL" || base == "SMULL" {
                    need(4)?;
                    check_width()?;
                    let lhs = machine.operand(&operands[2], &ty)?;
                    let rhs = machine.operand(&operands[3], &ty)?;
                    let (hi, lo) = multiply_wide(&ty, &lhs, &rhs)?;
                    machine.store_operand(&operands[0], &lo, &ty)?;
                    machine.store_operand(&operands[1], &hi, &ty)?;
                } else if base == "MULT" || base == "MULTU" {
                    need(2)?;
                    check_width()?;
                    let lhs = machine.operand(&operands[0], &ty)?;
                    let rhs = machine.operand(&operands[1], &ty)?;
                    let (hi, lo) = multiply_wide(&ty, &lhs, &rhs)?;
                    machine.registers.insert(machine.pair_lo.clone(), lo);
                    machine.registers.insert(machine.pair_hi.clone(), hi);
                } else if base == "MUL" || base == "IMUL" || base == "MULU" {
                    check_width()?;
                    if is_float_type(&ty) && operands.len() == 3 {
                        let lhs = machine.operand(&operands[1], &ty)?;
                        let rhs = machine.operand(&operands[2], &ty)?;
                        let value = eval_typed("*", &ty, &lhs, &rhs)?;
                        machine.store_operand(&operands[0], &value, &ty)?;
                    } else if operands.len() == 3 {
                        let lhs = machine.operand(&operands[1], &ty)?;
                        let rhs = machine.operand(&operands[2], &ty)?;
                        let value = eval_typed("*", &ty, &lhs, &rhs)?;
                        machine.store_operand(&operands[0], &value, &ty)?;
                    } else if operands.len() == 2 {
                        let lhs = machine.operand(&operands[0], &ty)?;
                        let rhs = machine.operand(&operands[1], &ty)?;
                        let value = eval_typed("*", &ty, &lhs, &rhs)?;
                        machine.store_operand(&operands[0], &value, &ty)?;
                    } else if operands.len() == 1 {
                        let lhs = machine
                            .registers
                            .get(&machine.pair_lo.clone())
                            .cloned()
                            .unwrap_or_else(|| Value::zero(&ty));
                        let rhs = machine.operand(&operands[0], &ty)?;
                        let (hi, lo) = multiply_wide(&ty, &lhs, &rhs)?;
                        machine.registers.insert(machine.pair_lo.clone(), lo);
                        machine.registers.insert(machine.pair_hi.clone(), hi);
                    } else {
                        return Err(format!("Invalid operands for {base}"));
                    }
                } else if base == "MLS" {
                    need(4)?;
                    check_width()?;
                    let first = machine.operand(&operands[1], &ty)?;
                    let second = machine.operand(&operands[2], &ty)?;
                    let third = machine.operand(&operands[3], &ty)?;
                    let product = eval_typed("*", &ty, &second, &third)?;
                    let value = eval_typed("-", &ty, &first, &product)?;
                    machine.store_operand(&operands[0], &value, &ty)?;
                } else if base == "MFHI" {
                    need(1)?;
                    let value = machine
                        .registers
                        .get(&machine.pair_hi.clone())
                        .cloned()
                        .unwrap_or_else(|| Value::zero(&ty));
                    machine.store_operand(&operands[0], &value, &ty)?;
                } else if base == "MFLO" {
                    need(1)?;
                    let value = machine
                        .registers
                        .get(&machine.pair_lo.clone())
                        .cloned()
                        .unwrap_or_else(|| Value::zero(&ty));
                    machine.store_operand(&operands[0], &value, &ty)?;
                } else if matches!(base, "DIV" | "DIVU" | "IDIV" | "SDIV" | "UDIV") {
                    check_width()?;
                    let pair_low = machine.pair_lo.clone();
                    let pair_high = machine.pair_hi.clone();
                    if operands.len() == 3 {
                        let lhs = machine.operand(&operands[1], &ty)?;
                        let rhs = machine.operand(&operands[2], &ty)?;
                        let value = eval_typed("/", &ty, &lhs, &rhs)?;
                        machine.store_operand(&operands[0], &value, &ty)?;
                    } else if operands.len() == 2 {
                        let lhs = machine.operand(&operands[0], &ty)?;
                        let rhs = machine.operand(&operands[1], &ty)?;
                        let (quotient, remainder) = divide_wide(&ty, &lhs, &rhs)?;
                        machine.registers.insert(pair_low, quotient);
                        machine.registers.insert(pair_high, remainder);
                    } else if operands.len() == 1 {
                        let lhs = machine
                            .registers
                            .get(&pair_low)
                            .cloned()
                            .unwrap_or_else(|| Value::zero(&ty));
                        let rhs = machine.operand(&operands[0], &ty)?;
                        let (quotient, remainder) = divide_wide(&ty, &lhs, &rhs)?;
                        machine.registers.insert(pair_low, quotient);
                        machine.registers.insert(pair_high, remainder);
                    } else {
                        return Err(format!("Invalid operands for {base}"));
                    }
                } else {
                    return Err(format!("Unknown instruction: {full}"));
                }
            }
        }
    }
    Err("Assembly ended without HALT".into())
}
