use big_number::BigInt;
use std::collections::{BTreeMap, BTreeSet};
use std::path::Path;

#[derive(Clone, Debug, Default)]
pub struct Quad {
    pub line: usize,
    pub op: String,
    pub args: Vec<String>,
}
impl Quad {
    pub fn arg(&self, i: usize) -> &str {
        self.args
            .get(i)
            .filter(|v| v.as_str() != "_")
            .map(String::as_str)
            .unwrap_or("")
    }
}

#[derive(Clone, Debug, Default)]
pub struct QuadFile {
    pub path: String,
    pub name: String,
    pub expected: String,
    pub expected_text: String,
    pub input: String,
    pub quads: Vec<Quad>,
    pub diagnostics: Vec<String>,
}
impl QuadFile {
    pub fn ok(&self) -> bool {
        self.diagnostics.is_empty()
    }
}

fn split_args(text: &str) -> Vec<String> {
    let mut result = Vec::new();
    let mut start = 0;
    let mut quoted = false;
    let mut escaped = false;
    let mut depth = 0;
    for (i, c) in text.char_indices() {
        if escaped {
            escaped = false;
            continue;
        }
        if c == '\\' && quoted {
            escaped = true;
            continue;
        }
        if c == '"' {
            quoted = !quoted;
            continue;
        }
        if !quoted {
            match c {
                '(' | '[' | '{' => depth += 1,
                ')' | ']' | '}' => depth -= 1,
                ',' if depth == 0 => {
                    result.push(text[start..i].trim().to_string());
                    start = i + 1;
                }
                _ => {}
            }
        }
    }
    result.push(text[start..].trim().to_string());
    result
}

pub fn parse_quads(text: &str, label: impl Into<String>) -> QuadFile {
    let mut file = QuadFile {
        path: label.into(),
        ..Default::default()
    };
    for (i, line) in text.lines().enumerate() {
        let line = line.trim();
        if line.is_empty() {
            continue;
        }
        if let Some(body) = line.strip_prefix('#') {
            if let Some((key, value)) = body.split_once(':') {
                let value = value.trim().to_string();
                match key.trim() {
                    "名称" => file.name = value,
                    "期望" => file.expected = value,
                    "期望文本" => file.expected_text = value,
                    "输入" => file.input = value,
                    _ => {}
                }
            }
            continue;
        }
        match (line.find('('), line.rfind(')')) {
            (Some(open), Some(close)) if close > open => {
                let parts = split_args(&line[open + 1..close]);
                if parts.first().is_some_and(|s| !s.is_empty()) {
                    file.quads.push(Quad {
                        line: i + 1,
                        op: parts[0].clone(),
                        args: parts[1..].to_vec(),
                    });
                } else {
                    file.diagnostics
                        .push(format!("第 {} 行：四元式是空的", i + 1));
                }
            }
            _ => file
                .diagnostics
                .push(format!("第 {} 行：不是四元式（缺少括号）", i + 1)),
        }
    }
    file
}
pub fn load_quad_file(path: impl AsRef<Path>) -> QuadFile {
    let path = path.as_ref();
    let name = path.to_string_lossy().into_owned();
    match std::fs::read_to_string(path) {
        Ok(text) => parse_quads(&text, name),
        Err(_) => QuadFile {
            path: name.clone(),
            diagnostics: vec![format!("打不开文件：{name}")],
            ..Default::default()
        },
    }
}

#[derive(Clone, Debug, PartialEq)]
pub struct TypeInfo {
    pub name: String,
    pub bits: u8,
    pub unsigned: bool,
    pub float: bool,
    pub size: usize,
}
pub fn type_of(name: &str) -> TypeInfo {
    let name = if name.is_empty() { "i16" } else { name };
    let bits = name[1..].parse::<u8>().unwrap_or(16);
    TypeInfo {
        name: name.into(),
        bits,
        unsigned: name.starts_with('u'),
        float: name.starts_with('f'),
        size: (bits as usize / 8).max(1),
    }
}

#[derive(Clone, Debug, Default)]
pub struct Program {
    pub name: String,
    pub code: Vec<Quad>,
    pub labels: BTreeMap<String, usize>,
    pub types: BTreeMap<String, TypeInfo>,
    pub parameters: BTreeMap<String, Vec<String>>,
    /// 可供模块调用者链接的公开标签；裸四元式默认全部公开。
    pub exports: BTreeSet<String>,
    pub diagnostics: Vec<String>,
    pub instructions: Vec<Instruction>,
    pub stack_labels: BTreeMap<String, usize>,
}
impl Program {
    pub fn ok(&self) -> bool {
        self.diagnostics.is_empty()
    }
}

#[derive(Clone, Debug, PartialEq)]
pub struct Instruction {
    pub op: String,
    pub name: String,
    pub literal: Option<Value>,
}
impl Instruction {
    fn plain(op: &str) -> Self {
        Self {
            op: op.into(),
            name: String::new(),
            literal: None,
        }
    }
    fn named(op: &str, name: &str) -> Self {
        Self {
            op: op.into(),
            name: name.into(),
            literal: None,
        }
    }
    pub fn to_text(&self) -> String {
        if let Some(value) = &self.literal {
            return format!("{} {}", self.op, value.display());
        }
        if self.name.is_empty() {
            self.op.clone()
        } else {
            format!("{} {}", self.op, self.name)
        }
    }
}

fn push_operand(code: &mut Vec<Instruction>, token: &str) {
    if let Ok(n) = token.parse::<i64>() {
        code.push(Instruction {
            op: "PUSH".into(),
            name: String::new(),
            literal: Some(Value::Int(n)),
        });
    } else if let Ok(n) = token.parse::<f64>() {
        code.push(Instruction {
            op: "PUSH".into(),
            name: String::new(),
            literal: Some(Value::Float(n)),
        });
    } else if let Some(text) = token.strip_prefix('"').and_then(|s| s.strip_suffix('"')) {
        code.push(Instruction {
            op: "PUSH".into(),
            name: String::new(),
            literal: Some(Value::Text(unescape(text))),
        });
    } else {
        code.push(Instruction::named("LOAD", token));
    }
}

fn lower_to_stack(file: &QuadFile, program: &mut Program) {
    let code = &mut program.instructions;
    for quad in &file.quads {
        let (op, _) = if quad.op.starts_with(":=") {
            (":=", "")
        } else {
            quad.op.split_once(':').unwrap_or((&quad.op, ""))
        };
        let a = quad.arg(0);
        let b = quad.arg(1);
        let dest = quad.arg(2);
        match op {
            "decl" => {
                if b == "local" {
                    code.push(Instruction::named("DECLARE", dest));
                }
            }
            "label" => {
                program.stack_labels.insert(dest.into(), code.len());
            }
            ":=" => {
                push_operand(code, a);
                code.push(Instruction::named("STORE", dest));
            }
            "neg" | "~" | "cvt" => {
                push_operand(code, a);
                code.push(Instruction::plain(match op {
                    "neg" => "NEG",
                    "~" => "NOT",
                    _ => "CVT",
                }));
                code.push(Instruction::named("STORE", dest));
            }
            "+" | "-" | "*" | "/" | "%" | "&" | "|" | "^" | "<<" | ">>" | "==" | "!=" | "<"
            | "<=" | ">" | ">=" | "mul" | "div" => {
                push_operand(code, a);
                push_operand(code, b);
                code.push(Instruction::plain(match op {
                    "+" => "ADD",
                    "-" => "SUB",
                    "*" => "MUL",
                    "/" => "DIV",
                    "%" => "MOD",
                    "&" => "AND",
                    "|" => "OR",
                    "^" => "XOR",
                    "<<" => "SHL",
                    ">>" => "SHR",
                    "==" => "EQ",
                    "!=" => "NE",
                    "<" => "LT",
                    "<=" => "LE",
                    ">" => "GT",
                    ">=" => "GE",
                    "mul" => "MULWIDE",
                    _ => "DIVWIDE",
                }));
                if op == "mul" || op == "div" {
                    code.push(Instruction::named("STORE", quad.arg(3)));
                }
                code.push(Instruction::named("STORE", dest));
            }
            "j" | "jz" | "jnz" => {
                if op != "j" {
                    push_operand(code, a)
                }
                code.push(Instruction::named(
                    match op {
                        "j" => "JMP",
                        "jz" => "JZ",
                        _ => "JNZ",
                    },
                    dest,
                ));
            }
            "write" => {
                push_operand(code, a);
                code.push(Instruction::plain("PRINT"));
            }
            "print" => {
                push_operand(code, a);
                code.push(Instruction::plain("PRINTTEXT"));
            }
            "putc" => {
                push_operand(code, a);
                code.push(Instruction::plain("PUTC"));
            }
            "read" | "getc" => code.push(Instruction::named(
                if op == "read" { "READ" } else { "GETC" },
                dest,
            )),
            "param" => {
                push_operand(code, a);
                code.push(Instruction::named("PARAM", dest));
            }
            "call" => {
                code.push(Instruction::named("CALL", a));
                if !dest.is_empty() {
                    code.push(Instruction::named("STORE", dest));
                }
            }
            "callptr" => {
                push_operand(code, a);
                code.push(Instruction::plain("CALLPTR"));
                if !dest.is_empty() {
                    code.push(Instruction::named("STORE", dest));
                }
            }
            "ret" => {
                push_operand(code, a);
                code.push(Instruction::plain("RET"));
            }
            "addr" => {
                code.push(Instruction::named("ADDR", a));
                code.push(Instruction::named("STORE", dest));
            }
            "alloc" | "load" => {
                push_operand(code, a);
                code.push(Instruction::plain(if op == "alloc" {
                    "ALLOC"
                } else {
                    "LOADAT"
                }));
                code.push(Instruction::named("STORE", dest));
            }
            "store" => {
                push_operand(code, a);
                push_operand(code, b);
                code.push(Instruction::plain("STOREAT"));
            }
            "free" | "drop" => {
                push_operand(code, a);
                code.push(Instruction::plain(if op == "free" {
                    "FREE"
                } else {
                    "DROP"
                }));
            }
            "check" | "checkptr" => {
                push_operand(code, a);
                push_operand(code, b);
                code.push(Instruction::named(
                    if op == "check" { "CHECK" } else { "CHECKPTR" },
                    dest,
                ));
            }
            "halt" => code.push(Instruction::plain("HALT")),
            _ => {}
        }
    }
    if code
        .last()
        .is_none_or(|instruction| instruction.op != "HALT")
    {
        code.push(Instruction::plain("HALT"));
    }
}

pub fn assemble(file: &QuadFile) -> Program {
    let mut program = Program {
        name: file.name.clone(),
        ..Default::default()
    };
    let mut pending = Vec::new();
    for (index, quad) in file.quads.iter().enumerate() {
        let op = if quad.op.starts_with(":=") {
            ":="
        } else {
            quad.op.split(':').next().unwrap_or("")
        };
        if op == "decl" {
            program
                .types
                .insert(quad.arg(2).into(), type_of(quad.arg(0)));
            if quad.arg(1) == "local" {
                pending.push(quad.arg(2).to_string());
            }
            continue;
        }
        if op == "label" {
            program.labels.insert(quad.arg(2).into(), index);
            if !pending.is_empty() {
                program
                    .parameters
                    .insert(quad.arg(2).into(), pending.clone());
            }
            pending.clear();
            continue;
        }
        pending.clear();
        if ![
            ":=", "neg", "~", "cvt", "+", "-", "*", "/", "%", "&", "|", "^", "<<", ">>", "==",
            "!=", "<", "<=", ">", ">=", "mul", "div", "j", "jz", "jnz", "write", "print", "putc",
            "read", "getc", "param", "call", "callptr", "ret", "halt", "addr", "alloc", "free",
            "drop", "check", "checkptr", "load", "store",
        ]
        .contains(&op)
        {
            program.diagnostics.push(format!(
                "第 {} 条四元式：本虚拟机不支持操作 '{}'",
                index + 1,
                quad.op
            ));
        }
    }
    program.exports.extend(program.labels.keys().cloned());
    program.code = file.quads.clone();
    lower_to_stack(file, &mut program);
    program
}

#[derive(Clone, Debug, Default)]
pub struct OptimizationReport {
    pub rounds: usize,
    pub constants_folded: usize,
    pub algebraic_simplified: usize,
    pub dead_branches: usize,
    pub dead_code: usize,
    pub trace: Vec<String>,
}
impl OptimizationReport {
    pub fn total(&self) -> usize {
        self.constants_folded + self.algebraic_simplified + self.dead_branches + self.dead_code
    }
}

pub fn optimize(file: &mut QuadFile, max_rounds: usize) -> OptimizationReport {
    let mut report = OptimizationReport::default();
    for _ in 0..max_rounds {
        report.rounds += 1;
        let before = file.quads.clone();
        let mut constants: BTreeMap<String, String> = BTreeMap::new();
        for quad in &mut file.quads {
            let op = if quad.op.starts_with(":=") {
                ":="
            } else {
                quad.op.split(':').next().unwrap_or("")
            }
            .to_string();
            if [
                "label", "j", "call", "callptr", "ret", "halt", "read", "getc", "store",
            ]
            .contains(&op.as_str())
            {
                constants.clear();
            }
            if [
                ":=", "+", "-", "*", "/", "%", "&", "|", "^", "<<", ">>", "neg", "~", "==", "!=",
                "<", "<=", ">", ">=", "cvt", "jz", "jnz", "write", "param",
            ]
            .contains(&op.as_str())
            {
                let count = if [
                    "+", "-", "*", "/", "%", "&", "|", "^", "<<", ">>", "==", "!=", "<", "<=", ">",
                    ">=", "jz", "jnz",
                ]
                .contains(&op.as_str())
                {
                    2
                } else {
                    1
                };
                for i in 0..count.min(quad.args.len()) {
                    if let Some(value) = constants.get(&quad.args[i]) {
                        quad.args[i] = value.clone();
                    }
                }
            }
            let dest = quad.arg(2).to_string();
            if op == ":=" {
                if quad.arg(0).parse::<f64>().is_ok() {
                    constants.insert(dest, quad.arg(0).into());
                } else {
                    constants.remove(&dest);
                }
                continue;
            }
            if [
                "+", "-", "*", "/", "%", "&", "|", "^", "<<", ">>", "==", "!=", "<", "<=", ">",
                ">=",
            ]
            .contains(&op.as_str())
            {
                let a = quad.arg(0).parse::<i64>();
                let b = quad.arg(1).parse::<i64>();
                if let (Ok(a), Ok(b)) = (a, b) {
                    let folded = match op.as_str() {
                        "+" => Some(a.wrapping_add(b)),
                        "-" => Some(a.wrapping_sub(b)),
                        "*" => Some(a.wrapping_mul(b)),
                        "/" if b != 0 => Some(a / b),
                        "%" if b != 0 => Some(a % b),
                        "&" => Some(a & b),
                        "|" => Some(a | b),
                        "^" => Some(a ^ b),
                        "<<" => Some(a.wrapping_shl(b as u32)),
                        ">>" => Some(a.wrapping_shr(b as u32)),
                        "==" => Some((a == b) as i64),
                        "!=" => Some((a != b) as i64),
                        "<" => Some((a < b) as i64),
                        "<=" => Some((a <= b) as i64),
                        ">" => Some((a > b) as i64),
                        ">=" => Some((a >= b) as i64),
                        _ => None,
                    };
                    if let Some(value) = folded {
                        quad.op = ":=".into();
                        quad.args = vec![value.to_string(), "_".into(), dest.clone()];
                        constants.insert(dest, value.to_string());
                        report.constants_folded += 1;
                        continue;
                    }
                }
                let replacement = match op.as_str() {
                    "+" | "-" | "|" | "^" | "<<" | ">>" if quad.arg(1) == "0" => {
                        Some(quad.arg(0).to_string())
                    }
                    "*" | "/" if quad.arg(1) == "1" => Some(quad.arg(0).to_string()),
                    "*" if quad.arg(0) == "1" => Some(quad.arg(1).to_string()),
                    _ => None,
                };
                if let Some(value) = replacement {
                    quad.op = ":=".into();
                    quad.args = vec![value, "_".into(), dest.clone()];
                    report.algebraic_simplified += 1;
                }
            }
            if op == "jz" || op == "jnz" {
                if let Ok(value) = quad.arg(0).parse::<i64>() {
                    let take = if op == "jz" { value == 0 } else { value != 0 };
                    if take {
                        quad.op = "j".into();
                        quad.args[0] = "_".into();
                    } else {
                        quad.op = "nop".into();
                    }
                    report.dead_branches += 1;
                }
            }
            if !dest.is_empty() && op != "j" && op != "jz" && op != "jnz" && op != "label" {
                constants.remove(&dest);
            }
        }
        let mut unreachable = false;
        file.quads.retain(|quad| {
            let op = quad.op.split(':').next().unwrap_or("");
            if op == "label" {
                unreachable = false;
            }
            if op == "nop" {
                report.dead_code += 1;
                return false;
            }
            if unreachable && op != "decl" {
                report.dead_code += 1;
                return false;
            }
            if ["j", "halt", "ret"].contains(&op) {
                unreachable = true;
            }
            true
        });
        if file.quads.len() == before.len()
            && file
                .quads
                .iter()
                .zip(&before)
                .all(|(a, b)| a.op == b.op && a.args == b.args)
        {
            break;
        }
    }
    report
}

#[derive(Clone, Debug, Default)]
pub struct DataArea {
    pub binary: Vec<u8>,
    pub symbols: BTreeMap<String, usize>,
    pub strides: BTreeMap<String, usize>,
    pub sizes: BTreeMap<String, usize>,
}
impl DataArea {
    pub fn load(binary: impl AsRef<Path>, layout: impl AsRef<Path>) -> Result<Self, String> {
        let binary = std::fs::read(binary).map_err(|e| e.to_string())?;
        let layout = std::fs::read_to_string(layout).map_err(|e| e.to_string())?;
        Self::from_parts(binary, &layout)
    }
    pub fn from_parts(binary: Vec<u8>, layout: &str) -> Result<Self, String> {
        let mut data = Self {
            binary,
            ..Default::default()
        };
        for line in layout.lines() {
            let mut words = line.split_whitespace();
            let Some(name) = words.next() else { continue };
            for word in words {
                if let Some((key, value)) = word.split_once('=') {
                    if let Ok(n) = value.parse::<usize>() {
                        match key {
                            "offset" => {
                                data.symbols.insert(name.into(), n);
                            }
                            "size" => {
                                data.sizes.insert(name.into(), n);
                            }
                            "stride" => {
                                data.strides.insert(name.into(), n);
                            }
                            _ => {}
                        }
                    }
                }
            }
        }
        Ok(data)
    }
    pub fn read_c_string(&self, address: usize, limit: usize) -> Option<String> {
        if address >= self.binary.len() {
            return None;
        }
        let end = (address + limit).min(self.binary.len());
        let slice = &self.binary[address..end];
        let nul = slice.iter().position(|b| *b == 0).unwrap_or(slice.len());
        Some(String::from_utf8_lossy(&slice[..nul]).into_owned())
    }
    pub fn read_int(&self, address: usize) -> Option<i64> {
        Some(i64::from_le_bytes(
            self.binary.get(address..address + 8)?.try_into().ok()?,
        ))
    }
    pub fn write_int(&mut self, address: usize, value: i64) -> bool {
        if let Some(bytes) = self.binary.get_mut(address..address + 8) {
            bytes.copy_from_slice(&value.to_le_bytes());
            true
        } else {
            false
        }
    }
    pub fn read_double(&self, address: usize) -> Option<f64> {
        self.read_int(address).map(|n| f64::from_bits(n as u64))
    }
    pub fn write_double(&mut self, address: usize, value: f64) -> bool {
        self.write_int(address, value.to_bits() as i64)
    }
}

#[derive(Clone, Debug, PartialEq)]
pub enum Value {
    Int(i64),
    Float(f64),
    Text(String),
}

pub struct NativeContext<'a> {
    heap: &'a mut Heap,
    data: Option<&'a mut DataArea>,
    heap_base: usize,
    text: &'a mut String,
}
impl NativeContext<'_> {
    pub fn read_string(&self, address: i64) -> Result<String, String> {
        read_string(
            self.data.as_deref(),
            self.heap,
            address.max(0) as usize,
            self.heap_base,
        )
    }
    pub fn make_string(&mut self, value: &str) -> Value {
        alloc_string(self.heap, value, self.heap_base)
    }
    pub fn write_text(&mut self, value: &str) {
        self.text.push_str(value);
    }
}

type NativeCallback =
    Box<dyn for<'a> Fn(&mut NativeContext<'a>, &[Value]) -> Result<Value, String>>;

#[derive(Default)]
pub struct NativeRegistry {
    entries: BTreeMap<String, (usize, NativeCallback)>,
}
impl NativeRegistry {
    pub fn new() -> Self {
        Self::default()
    }
    pub fn add(
        &mut self,
        name: impl Into<String>,
        arity: usize,
        callback: impl for<'a> Fn(&mut NativeContext<'a>, &[Value]) -> Result<Value, String>
            + 'static,
    ) -> Result<(), String> {
        let name = name.into();
        if name.starts_with("__vm_") {
            return Err(format!("{name} 是虚拟机保留的内建函数名"));
        }
        if self.entries.contains_key(&name) {
            return Err(format!("宿主函数重复注册：{name}"));
        }
        self.entries.insert(name, (arity, Box::new(callback)));
        Ok(())
    }
}
impl Value {
    pub fn int(&self) -> i64 {
        match self {
            Self::Int(n) => *n,
            Self::Float(n) => *n as i64,
            Self::Text(s) => s.parse().unwrap_or(0),
        }
    }
    pub fn float(&self) -> f64 {
        match self {
            Self::Int(n) => *n as f64,
            Self::Float(n) => *n,
            Self::Text(s) => s.parse().unwrap_or(0.0),
        }
    }
    fn truthy(&self) -> bool {
        match self {
            Self::Text(s) => !s.is_empty(),
            _ => self.float() != 0.0,
        }
    }
    fn display(&self) -> String {
        match self {
            Self::Int(n) => n.to_string(),
            Self::Float(n) => format_float(*n),
            Self::Text(s) => s.clone(),
        }
    }
}
fn format_float(n: f64) -> String {
    let mut s = n.to_string();
    if s.ends_with(".0") {
        s.truncate(s.len() - 2)
    }
    s
}

#[derive(Clone, Debug, Default)]
pub struct CoroutineInfo {
    pub id: usize,
    pub state: String,
    pub instruction: usize,
    pub frame_depth: usize,
    pub waiting_for: usize,
}

#[derive(Clone, Debug, Default)]
pub struct InstructionHookInfo {
    pub steps: usize,
    pub instruction: usize,
    pub coroutine: usize,
    pub opcode: String,
}

#[derive(Clone, Debug, Default)]
pub struct RunResult {
    pub ok: bool,
    pub output: String,
    pub text: String,
    pub error: String,
    pub steps: usize,
    pub timing_available: bool,
    pub started_nanoseconds: u64,
    pub finished_nanoseconds: u64,
    pub elapsed_nanoseconds: u64,
    pub max_stack_depth: usize,
    pub heap_peak_bytes: usize,
    pub heap_live_bytes: usize,
    pub heap_live_blocks: usize,
    pub live_objects: usize,
    pub coroutines: Vec<CoroutineInfo>,
    pub trace: Vec<String>,
}

#[derive(Default)]
struct Frame {
    return_pc: usize,
    result: String,
    locals: BTreeMap<String, Value>,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
enum CoroutineState {
    #[default]
    Ready,
    Running,
    Suspended,
    Waiting,
    Done,
}

#[derive(Default)]
struct CoroutineContext {
    frames: Vec<Frame>,
    pending: Vec<(String, Value)>,
    pc: usize,
    state: CoroutineState,
    waiting_for: usize,
}
#[derive(Default)]
struct Heap {
    bytes: Vec<u8>,
    blocks: BTreeMap<usize, usize>,
    peak: usize,
}
impl Heap {
    fn alloc(&mut self, size: usize, base: usize) -> usize {
        let size = size.max(1).div_ceil(8) * 8;
        // A stale pointer must never become valid again within the same run.
        // Freed addresses stay retired until the whole heap is reset.
        let address = base + self.bytes.len().max(8);
        let pad = address - base - self.bytes.len();
        self.bytes.resize(self.bytes.len() + pad + size, 0);
        self.blocks.insert(address, size);
        self.peak = self.peak.max(self.blocks.values().sum());
        address
    }
    fn range(&self, address: usize, count: usize) -> Option<usize> {
        self.blocks
            .iter()
            .find(|(start, size)| {
                address >= **start
                    && address
                        .checked_add(count)
                        .is_some_and(|end| end <= **start + **size)
            })
            .map(|(start, _)| *start)
    }
    fn free(&mut self, address: usize) -> bool {
        self.blocks.remove(&address).is_some()
    }
    fn live_bytes(&self) -> usize {
        self.blocks.values().sum()
    }
}

#[derive(Clone, Debug)]
enum Key {
    Int(i64),
    Float(f64),
    Text(String),
}
impl PartialEq for Key {
    fn eq(&self, other: &Self) -> bool {
        self.cmp(other).is_eq()
    }
}
impl Eq for Key {}
impl PartialOrd for Key {
    fn partial_cmp(&self, other: &Self) -> Option<std::cmp::Ordering> {
        Some(self.cmp(other))
    }
}
impl Ord for Key {
    fn cmp(&self, other: &Self) -> std::cmp::Ordering {
        use std::cmp::Ordering;
        match (self, other) {
            (Self::Int(a), Self::Int(b)) => a.cmp(b),
            (Self::Float(a), Self::Float(b)) => a.total_cmp(b),
            (Self::Text(a), Self::Text(b)) => a.cmp(b),
            (Self::Int(_), _) => Ordering::Less,
            (Self::Float(_), Self::Text(_)) => Ordering::Less,
            _ => Ordering::Greater,
        }
    }
}
#[derive(Clone, Debug)]
enum Object {
    Big(BigInt),
    Vec(Vec<Key>),
    Set(BTreeSet<Key>),
    Map(BTreeMap<Key, Key>),
}
#[derive(Default)]
struct ObjectStore {
    next: i64,
    items: BTreeMap<i64, Object>,
}
impl ObjectStore {
    fn add(&mut self, value: Object) -> i64 {
        let id = 1_000_000_000_000 + self.next;
        self.next += 1;
        self.items.insert(id, value);
        id
    }
    fn get(&self, id: i64) -> Result<&Object, String> {
        self.items
            .get(&id)
            .ok_or_else(|| format!("对象句柄无效或已释放：{id}"))
    }
    fn get_mut(&mut self, id: i64) -> Result<&mut Object, String> {
        self.items
            .get_mut(&id)
            .ok_or_else(|| format!("对象句柄无效或已释放：{id}"))
    }
}

pub struct Machine {
    pub trace: bool,
    pub data: Option<DataArea>,
    time_callback: Option<Box<dyn FnMut() -> u64>>,
    hook_interval: usize,
    instruction_hook: Option<Box<dyn FnMut(&InstructionHookInfo)>>,
    native_registry: NativeRegistry,
}
impl Machine {
    pub fn new(trace: bool) -> Self {
        Self {
            trace,
            data: None,
            time_callback: None,
            hook_interval: 0,
            instruction_hook: None,
            native_registry: NativeRegistry::new(),
        }
    }
    pub fn set_data(&mut self, data: DataArea) {
        self.data = Some(data)
    }
    pub fn set_time_callback(&mut self, callback: impl FnMut() -> u64 + 'static) {
        self.time_callback = Some(Box::new(callback));
    }
    pub fn clear_time_callback(&mut self) {
        self.time_callback = None;
    }
    pub fn set_instruction_hook(
        &mut self,
        interval: usize,
        hook: impl FnMut(&InstructionHookInfo) + 'static,
    ) {
        self.hook_interval = interval;
        self.instruction_hook = Some(Box::new(hook));
    }
    pub fn clear_instruction_hook(&mut self) {
        self.hook_interval = 0;
        self.instruction_hook = None;
    }
    pub fn set_native_registry(&mut self, registry: NativeRegistry) {
        self.native_registry = registry;
    }
    pub fn run(&mut self, program: &Program, input: &[String], step_limit: usize) -> RunResult {
        let mut result = RunResult::default();
        if let Some(clock) = self.time_callback.as_mut() {
            result.timing_available = true;
            result.started_nanoseconds = clock();
        }
        if !program.ok() {
            result.error = program.diagnostics.join("\n");
            if let Some(clock) = self.time_callback.as_mut() {
                result.finished_nanoseconds = clock();
                result.elapsed_nanoseconds = result
                    .finished_nanoseconds
                    .saturating_sub(result.started_nanoseconds);
            }
            return result;
        }
        let mut globals: BTreeMap<String, Value> = BTreeMap::new();
        let mut frames: Vec<Frame> = Vec::new();
        let mut pending: Vec<(String, Value)> = Vec::new();
        let mut heap = Heap::default();
        let mut objects = ObjectStore::default();
        let mut pc = 0usize;
        let mut coroutines = vec![CoroutineContext {
            state: CoroutineState::Running,
            ..Default::default()
        }];
        let mut current_coroutine = 0usize;
        let mut input_pos = 0usize;
        let data_len = self.data.as_ref().map(|d| d.binary.len()).unwrap_or(0);
        let heap_base = (data_len + 7).div_ceil(8) * 8;
        let mut failure = None;
        let next_runnable = |contexts: &[CoroutineContext], after: usize| -> Option<usize> {
            (1..=contexts.len())
                .map(|offset| (after + offset) % contexts.len())
                .find(|&index| {
                    matches!(
                        contexts[index].state,
                        CoroutineState::Ready | CoroutineState::Suspended
                    )
                })
        };
        while pc < program.code.len() {
            if result.steps >= step_limit {
                failure = Some("超过执行步数上限".to_string());
                break;
            }
            let quad = &program.code[pc];
            let executed_pc = pc;
            let executed_coroutine = current_coroutine;
            let (op, suffix) = if quad.op.starts_with(":=") {
                (":=", "")
            } else {
                quad.op.split_once(':').unwrap_or((&quad.op, ""))
            };
            let next = pc + 1;
            result.steps += 1;
            if self.trace {
                result
                    .trace
                    .push(format!("{pc}: {} ({})", quad.op, quad.args.join(", ")));
            }
            let value = |name: &str,
                         frames: &Vec<Frame>,
                         globals: &BTreeMap<String, Value>|
             -> Result<Value, String> {
                if name.is_empty() {
                    return Ok(Value::Int(0));
                }
                if let Some(text) = name.strip_prefix('"').and_then(|v| v.strip_suffix('"')) {
                    return Ok(Value::Text(unescape(text)));
                }
                if let Ok(n) = name.parse::<i64>() {
                    return Ok(Value::Int(n));
                }
                if let Ok(n) = name.parse::<f64>() {
                    return Ok(Value::Float(n));
                }
                if let Some(v) = frames.last().and_then(|f| f.locals.get(name)) {
                    return Ok(v.clone());
                }
                globals
                    .get(name)
                    .cloned()
                    .ok_or_else(|| format!("未定义变量：{name}"))
            };
            let mut jump = None;
            let execution: Result<(), String> = (|| {
                let a = quad.arg(0);
                let b = quad.arg(1);
                let dest = quad.arg(2);
                let set = |name: &str,
                           mut v: Value,
                           frames: &mut Vec<Frame>,
                           globals: &mut BTreeMap<String, Value>| {
                    if name.is_empty() {
                        return;
                    }
                    if let Some(ty) = program.types.get(name) {
                        v = normalize(v, ty);
                    }
                    if let Some(frame) = frames.last_mut() {
                        if frame.locals.contains_key(name) {
                            frame.locals.insert(name.into(), v);
                            return;
                        }
                    }
                    globals.insert(name.into(), v);
                };
                match op {
                    "decl" => {
                        if b == "local" {
                            let name = dest.to_string();
                            if let Some(frame) = frames.last_mut() {
                                frame.locals.entry(name).or_insert(Value::Int(0));
                            } else {
                                globals.entry(name).or_insert(Value::Int(0));
                            }
                        } else {
                            globals.entry(dest.to_string()).or_insert(Value::Int(0));
                        }
                    }
                    "label" => {}
                    "halt" => {
                        jump = Some(program.code.len());
                    }
                    ":=" => {
                        let v = value(a, &frames, &globals)?;
                        set(dest, v, &mut frames, &mut globals);
                    }
                    "neg" | "~" => {
                        let v = value(a, &frames, &globals)?;
                        let r = if op == "~" {
                            Value::Int(!v.int())
                        } else if matches!(v, Value::Float(_)) {
                            Value::Float(-v.float())
                        } else {
                            Value::Int(v.int().wrapping_neg())
                        };
                        set(dest, r, &mut frames, &mut globals);
                    }
                    "cvt" => {
                        let v = value(a, &frames, &globals)?;
                        let ty = type_of(suffix);
                        set(dest, normalize(v, &ty), &mut frames, &mut globals);
                    }
                    "+" | "-" | "*" | "/" | "%" | "&" | "|" | "^" | "<<" | ">>" | "==" | "!="
                    | "<" | "<=" | ">" | ">=" => {
                        let x = value(a, &frames, &globals)?;
                        let y = value(b, &frames, &globals)?;
                        let ty = type_of(suffix);
                        let float = ty.float
                            || matches!(x, Value::Float(_))
                            || matches!(y, Value::Float(_));
                        let r = if ["==", "!=", "<", "<=", ">", ">="].contains(&op) {
                            let cmp = if float {
                                let (a, b) = (x.float(), y.float());
                                match op {
                                    "==" => a == b,
                                    "!=" => a != b,
                                    "<" => a < b,
                                    "<=" => a <= b,
                                    ">" => a > b,
                                    _ => a >= b,
                                }
                            } else if ty.unsigned {
                                let (a, b) = (x.int() as u64, y.int() as u64);
                                match op {
                                    "==" => a == b,
                                    "!=" => a != b,
                                    "<" => a < b,
                                    "<=" => a <= b,
                                    ">" => a > b,
                                    _ => a >= b,
                                }
                            } else {
                                let (a, b) = (x.int(), y.int());
                                match op {
                                    "==" => a == b,
                                    "!=" => a != b,
                                    "<" => a < b,
                                    "<=" => a <= b,
                                    ">" => a > b,
                                    _ => a >= b,
                                }
                            };
                            Value::Int(cmp as i64)
                        } else if float && ["+", "-", "*", "/", "%"].contains(&op) {
                            let (a, b) = (x.float(), y.float());
                            Value::Float(match op {
                                "+" => a + b,
                                "-" => a - b,
                                "*" => a * b,
                                "/" => a / b,
                                _ => a % b,
                            })
                        } else {
                            let (a, b) = (x.int(), y.int());
                            Value::Int(match op {
                                "+" => a.wrapping_add(b),
                                "-" => a.wrapping_sub(b),
                                "*" => a.wrapping_mul(b),
                                "/" => a.checked_div(b).ok_or("除零或溢出")?,
                                "%" => a.checked_rem(b).ok_or("除零或溢出")?,
                                "&" => a & b,
                                "|" => a | b,
                                "^" => a ^ b,
                                "<<" => a.wrapping_shl(b as u32),
                                ">>" => {
                                    if ty.unsigned {
                                        ((a as u64).wrapping_shr(b as u32)) as i64
                                    } else {
                                        a.wrapping_shr(b as u32)
                                    }
                                }
                                _ => 0,
                            })
                        };
                        set(dest, normalize(r, &ty), &mut frames, &mut globals);
                    }
                    "mul" | "div" => {
                        let x = value(a, &frames, &globals)?.int() as i128;
                        let y = value(b, &frames, &globals)?.int() as i128;
                        let ty = type_of(suffix);
                        let bits = ty.bits as u32;
                        let mask = (1i128 << bits) - 1;
                        let (lo, hi) = if op == "mul" {
                            let z = x * y;
                            ((z & mask) as i64, ((z >> bits) & mask) as i64)
                        } else {
                            if y == 0 {
                                return Err("除零".into());
                            }
                            ((x / y) as i64, (x % y) as i64)
                        };
                        set(dest, Value::Int(lo), &mut frames, &mut globals);
                        set(quad.arg(3), Value::Int(hi), &mut frames, &mut globals);
                    }
                    "j" | "jz" | "jnz" => {
                        let take = if op == "j" {
                            true
                        } else {
                            let truth = value(a, &frames, &globals)?.truthy();
                            if op == "jz" {
                                !truth
                            } else {
                                truth
                            }
                        };
                        if take {
                            jump = Some(
                                *program
                                    .labels
                                    .get(dest)
                                    .ok_or_else(|| format!("不存在标签：{dest}"))?,
                            )
                        }
                    }
                    "write" => {
                        let v = value(a, &frames, &globals)?;
                        let ty = if suffix.is_empty() {
                            program.types.get(a).cloned()
                        } else {
                            Some(type_of(suffix))
                        };
                        let text = match ty {
                            Some(ty) if ty.float => format_float(v.float()),
                            Some(ty) if ty.unsigned => (normalize(v, &ty).int() as u64).to_string(),
                            Some(_) => v.int().to_string(),
                            None => v.display(),
                        };
                        if !result.output.is_empty() {
                            result.output.push(' ')
                        }
                        result.output.push_str(&text);
                    }
                    "print" => {
                        result
                            .text
                            .push_str(&value(a, &frames, &globals)?.display());
                    }
                    "putc" => {
                        let n = value(a, &frames, &globals)?.int() as u8;
                        result.text.push(n as char);
                    }
                    "read" | "getc" => {
                        let item = input.get(input_pos).ok_or("输入已用完")?;
                        input_pos += 1;
                        let v = if op == "getc" {
                            Value::Int(item.as_bytes().first().copied().unwrap_or(0) as i64)
                        } else if item.contains(['.', 'e', 'E']) {
                            Value::Float(item.parse().map_err(|_| "输入不是数字")?)
                        } else {
                            Value::Int(item.parse().map_err(|_| "输入不是数字")?)
                        };
                        set(dest, v, &mut frames, &mut globals);
                    }
                    "param" => {
                        pending.push((dest.into(), value(a, &frames, &globals)?));
                    }
                    "call" | "callptr" => {
                        let target = if op == "call" {
                            a.to_string()
                        } else {
                            let address = value(a, &frames, &globals)?.int() as usize;
                            if address == 0 {
                                return Err("空函数指针".into());
                            }
                            self.data
                                .as_ref()
                                .and_then(|d| d.read_c_string(address, 256))
                                .ok_or("函数指针地址无效")?
                        };
                        let arguments = std::mem::take(&mut pending);
                        let coroutine_call = matches!(
                            target.as_str(),
                            "std::Coroutine::create"
                                | "std::Coroutine::wait"
                                | "std::Coroutine::yield"
                                | "std::Coroutine::yelid"
                        );
                        if coroutine_call {
                            if target == "std::Coroutine::create" {
                                if arguments.len() != 1 {
                                    return Err("std::Coroutine::create 需要一个函数指针".into());
                                }
                                let address = arguments[0].1.int() as usize;
                                let label_name = self
                                    .data
                                    .as_ref()
                                    .and_then(|data| data.read_c_string(address, 256))
                                    .ok_or("std::Coroutine::create 收到了无效函数指针")?;
                                let label = *program
                                    .labels
                                    .get(&label_name)
                                    .ok_or_else(|| format!("找不到协程入口：{label_name}"))?;
                                let mut context = CoroutineContext {
                                    pc: label,
                                    state: CoroutineState::Ready,
                                    ..Default::default()
                                };
                                context.frames.push(Frame::default());
                                coroutines.push(context);
                                set(
                                    dest,
                                    Value::Int((coroutines.len() - 1) as i64),
                                    &mut frames,
                                    &mut globals,
                                );
                            } else if target == "std::Coroutine::wait" {
                                if arguments.len() != 1 {
                                    return Err("std::Coroutine::wait 需要一个句柄".into());
                                }
                                let handle = arguments[0].1.int();
                                if handle <= 0 || handle as usize >= coroutines.len() {
                                    return Err(format!(
                                        "std::Coroutine::wait 收到无效句柄：{handle}"
                                    ));
                                }
                                let target_id = handle as usize;
                                if target_id == current_coroutine {
                                    return Err("协程不能等待自己".into());
                                }
                                set(dest, Value::Int(0), &mut frames, &mut globals);
                                if coroutines[target_id].state != CoroutineState::Done {
                                    let current = current_coroutine;
                                    coroutines[current].frames = std::mem::take(&mut frames);
                                    coroutines[current].pending = std::mem::take(&mut pending);
                                    coroutines[current].pc = next;
                                    coroutines[current].state = CoroutineState::Waiting;
                                    coroutines[current].waiting_for = target_id;
                                    let next_id = if matches!(
                                        coroutines[target_id].state,
                                        CoroutineState::Ready | CoroutineState::Suspended
                                    ) {
                                        Some(target_id)
                                    } else {
                                        next_runnable(&coroutines, current)
                                    }
                                    .ok_or("协程等待形成死锁")?;
                                    current_coroutine = next_id;
                                    frames = std::mem::take(&mut coroutines[next_id].frames);
                                    pending = std::mem::take(&mut coroutines[next_id].pending);
                                    coroutines[next_id].state = CoroutineState::Running;
                                    jump = Some(coroutines[next_id].pc);
                                }
                            } else {
                                if !arguments.is_empty() {
                                    return Err(format!("{target} 不接受参数"));
                                }
                                set(dest, Value::Int(0), &mut frames, &mut globals);
                                if let Some(next_id) = next_runnable(&coroutines, current_coroutine)
                                {
                                    let current = current_coroutine;
                                    coroutines[current].frames = std::mem::take(&mut frames);
                                    coroutines[current].pending = std::mem::take(&mut pending);
                                    coroutines[current].pc = next;
                                    coroutines[current].state = CoroutineState::Suspended;
                                    current_coroutine = next_id;
                                    frames = std::mem::take(&mut coroutines[next_id].frames);
                                    pending = std::mem::take(&mut coroutines[next_id].pending);
                                    coroutines[next_id].state = CoroutineState::Running;
                                    jump = Some(coroutines[next_id].pc);
                                }
                            }
                        } else if let Some(&label) = program.labels.get(&target) {
                            let mut frame = Frame {
                                return_pc: next,
                                result: dest.into(),
                                ..Default::default()
                            };
                            if op == "callptr" {
                                let params = program
                                    .parameters
                                    .get(&target)
                                    .map(Vec::as_slice)
                                    .unwrap_or(&[]);
                                if arguments.len() != params.len() {
                                    return Err(format!("{target} 需要 {} 个参数", params.len()));
                                }
                                for (i, (_, value)) in arguments.into_iter().enumerate() {
                                    frame.locals.insert(params[i].clone(), value);
                                }
                            } else {
                                for (name, value) in arguments {
                                    frame.locals.insert(name, value);
                                }
                            }
                            frames.push(frame);
                            jump = Some(label);
                        } else {
                            let native = if !target.starts_with("__vm_") {
                                if let Some((arity, callback)) =
                                self.native_registry.entries.get(&target)
                                {
                                    if arguments.len() != *arity {
                                        return Err(format!(
                                            "宿主函数 {target} 需要 {arity} 个参数，实际得到 {} 个",
                                            arguments.len()
                                        ));
                                    }
                                    let values: Vec<Value> = arguments
                                        .iter()
                                        .map(|(_, value)| value.clone())
                                        .collect();
                                    let mut context = NativeContext {
                                        heap: &mut heap,
                                        data: self.data.as_mut(),
                                        heap_base,
                                        text: &mut result.text,
                                    };
                                    callback(&mut context, &values)?
                                } else {
                                    native_call(
                                        &target,
                                        &arguments,
                                        &mut heap,
                                        self.data.as_mut(),
                                        &mut objects,
                                        input,
                                        &mut input_pos,
                                        heap_base,
                                        &mut result.text,
                                    )?
                                }
                            } else {
                                native_call(
                                    &target,
                                    &arguments,
                                    &mut heap,
                                    self.data.as_mut(),
                                    &mut objects,
                                    input,
                                    &mut input_pos,
                                    heap_base,
                                    &mut result.text,
                                )?
                            };
                            set(dest, native, &mut frames, &mut globals);
                        }
                    }
                    "ret" => {
                        let v = value(a, &frames, &globals)?;
                        if current_coroutine != 0 && frames.len() == 1 {
                            frames.clear();
                            pending.clear();
                            coroutines[current_coroutine].state = CoroutineState::Done;
                            let completed = current_coroutine;
                            let mut next_id = None;
                            for (index, context) in coroutines.iter_mut().enumerate() {
                                if context.state == CoroutineState::Waiting
                                    && context.waiting_for == completed
                                {
                                    context.state = CoroutineState::Ready;
                                    context.waiting_for = 0;
                                    next_id.get_or_insert(index);
                                }
                            }
                            if next_id.is_none() {
                                next_id = next_runnable(&coroutines, completed);
                            }
                            if let Some(index) = next_id {
                                current_coroutine = index;
                                frames = std::mem::take(&mut coroutines[index].frames);
                                pending = std::mem::take(&mut coroutines[index].pending);
                                coroutines[index].state = CoroutineState::Running;
                                jump = Some(coroutines[index].pc);
                            } else {
                                jump = Some(program.code.len());
                            }
                        } else {
                            let frame = frames.pop().ok_or("函数外 ret")?;
                            set(&frame.result, v, &mut frames, &mut globals);
                            jump = Some(frame.return_pc);
                        }
                    }
                    "addr" => {
                        let address = self
                            .data
                            .as_ref()
                            .and_then(|d| d.symbols.get(a))
                            .copied()
                            .ok_or_else(|| format!("数据区里没有符号：{a}"))?;
                        set(dest, Value::Int(address as i64), &mut frames, &mut globals);
                    }
                    "alloc" => {
                        let size = value(a, &frames, &globals)?.int();
                        if size <= 0 {
                            return Err("alloc 大小必须是正数".into());
                        }
                        let address = heap.alloc(size as usize, heap_base);
                        set(dest, Value::Int(address as i64), &mut frames, &mut globals);
                    }
                    "free" | "drop" => {
                        let address = value(a, &frames, &globals)?.int();
                        if address >= 1_000_000_000_000 {
                            if objects.items.remove(&address).is_none() && op == "free" {
                                return Err(format!("对象已经释放过了：{address}"));
                            }
                        } else if address != 0 && !heap.free(address as usize) && op == "free" {
                            return Err(format!("无效堆地址或已经 free 过了：{address}"));
                        }
                    }
                    "check" => {
                        let index = value(a, &frames, &globals)?.int();
                        let bound = value(b, &frames, &globals)?.int();
                        if index < 0 || index >= bound {
                            return Err(format!(
                                "数组下标越界：{}（下标 {index}，上限 {bound}）",
                                dest
                            ));
                        }
                    }
                    "checkptr" => {
                        let index = value(a, &frames, &globals)?.int();
                        let address = value(b, &frames, &globals)?.int() as usize;
                        let stride = dest.parse::<usize>().unwrap_or(8);
                        let width = stride.max(1);
                        let size = if let Some(data) = &self.data {
                            data.symbols.iter().find_map(|(name, start)| {
                                let size = *data.sizes.get(name).unwrap_or(&0);
                                if address >= *start && address < start + size {
                                    Some(start + size - address)
                                } else {
                                    None
                                }
                            })
                        } else {
                            None
                        }
                        .or_else(|| {
                            heap.range(address, 1).and_then(|start| {
                                heap.blocks.get(&start).map(|size| start + size - address)
                            })
                        })
                        .ok_or("指针不在有效内存块内")?;
                        if index < 0
                            || (index as usize)
                                .checked_mul(width)
                                .and_then(|n| n.checked_add(width))
                                .is_none_or(|n| n > size)
                        {
                            return Err(format!("指针下标越界：{index}"));
                        }
                    }
                    "load" | "store" => {
                        let address = value(a, &frames, &globals)?.int();
                        if address < 0 {
                            return Err("负地址".into());
                        }
                        let address = address as usize;
                        let float = type_of(suffix).float;
                        if op == "load" {
                            let raw = read_memory(self.data.as_ref(), &heap, address, heap_base)?;
                            let v = if float {
                                Value::Float(f64::from_bits(raw as u64))
                            } else {
                                Value::Int(raw)
                            };
                            set(dest, v, &mut frames, &mut globals);
                        } else {
                            let v = value(b, &frames, &globals)?;
                            let raw = if float {
                                v.float().to_bits() as i64
                            } else {
                                v.int()
                            };
                            write_memory(self.data.as_mut(), &mut heap, address, heap_base, raw)?;
                        }
                    }
                    _ => return Err(format!("本虚拟机不支持操作：{}", quad.op)),
                }
                Ok(())
            })();
            if let Err(error) = execution {
                failure = Some(error);
            }
            if self.hook_interval != 0 && result.steps % self.hook_interval == 0 {
                if let Some(hook) = self.instruction_hook.as_mut() {
                    hook(&InstructionHookInfo {
                        steps: result.steps,
                        instruction: executed_pc,
                        coroutine: executed_coroutine,
                        opcode: quad.op.clone(),
                    });
                }
            }
            if failure.is_some() {
                break;
            }
            pc = jump.unwrap_or(next);
        }
        result.ok = failure.is_none();
        result.error = failure.unwrap_or_default();
        result.heap_peak_bytes = heap.peak;
        result.heap_live_bytes = heap.live_bytes();
        result.heap_live_blocks = heap.blocks.len();
        result.live_objects = objects.items.len();
        if let Some(clock) = self.time_callback.as_mut() {
            result.finished_nanoseconds = clock();
            result.elapsed_nanoseconds = result
                .finished_nanoseconds
                .saturating_sub(result.started_nanoseconds);
        }
        coroutines[current_coroutine].pc = pc;
        if result.ok {
            coroutines[current_coroutine].state = CoroutineState::Done;
        }
        coroutines[current_coroutine].frames = frames;
        coroutines[current_coroutine].pending = pending;
        result.coroutines = coroutines
            .iter()
            .enumerate()
            .map(|(id, context)| CoroutineInfo {
                id,
                state: match context.state {
                    CoroutineState::Ready => "ready",
                    CoroutineState::Running => "running",
                    CoroutineState::Suspended => "suspended",
                    CoroutineState::Waiting => "waiting",
                    CoroutineState::Done => "done",
                }
                .into(),
                instruction: context.pc,
                frame_depth: context.frames.len(),
                waiting_for: context.waiting_for,
            })
            .collect();
        result
    }
}

fn normalize(value: Value, ty: &TypeInfo) -> Value {
    if ty.float {
        return Value::Float(if ty.bits == 32 {
            (value.float() as f32) as f64
        } else {
            value.float()
        });
    }
    let mut n = value.int();
    if ty.bits < 64 {
        let mask = (1u64 << ty.bits) - 1;
        n = ((n as u64) & mask) as i64;
        if !ty.unsigned {
            let shift = 64 - ty.bits;
            n = (n << shift) >> shift;
        }
    }
    Value::Int(n)
}
fn unescape(text: &str) -> String {
    let mut out = String::new();
    let mut chars = text.chars();
    while let Some(c) = chars.next() {
        if c == '\\' {
            out.push(match chars.next() {
                Some('n') => '\n',
                Some('t') => '\t',
                Some('r') => '\r',
                Some('\\') => '\\',
                Some('"') => '"',
                Some(c) => c,
                None => '\\',
            });
        } else {
            out.push(c)
        }
    }
    out
}
fn read_memory(
    data: Option<&DataArea>,
    heap: &Heap,
    address: usize,
    base: usize,
) -> Result<i64, String> {
    if let Some(data) = data {
        if let Some(n) = data.read_int(address) {
            return Ok(n);
        }
    }
    if address >= base && heap.range(address, 8).is_some() {
        let offset = address - base;
        return Ok(i64::from_le_bytes(
            heap.bytes[offset..offset + 8].try_into().unwrap(),
        ));
    }
    Err(format!("堆越界读或数据区越界读：地址 {address}"))
}
fn write_memory(
    data: Option<&mut DataArea>,
    heap: &mut Heap,
    address: usize,
    base: usize,
    value: i64,
) -> Result<(), String> {
    if let Some(data) = data {
        if data.write_int(address, value) {
            return Ok(());
        }
    }
    if address >= base && heap.range(address, 8).is_some() {
        let offset = address - base;
        heap.bytes[offset..offset + 8].copy_from_slice(&value.to_le_bytes());
        return Ok(());
    }
    Err(format!("堆越界写或数据区越界写：地址 {address}"))
}
#[expect(
    clippy::too_many_arguments,
    reason = "native functions share the VM's run state"
)]
fn native_call(
    name: &str,
    args: &[(String, Value)],
    heap: &mut Heap,
    data: Option<&mut DataArea>,
    objects: &mut ObjectStore,
    input: &[String],
    input_pos: &mut usize,
    heap_base: usize,
    text: &mut String,
) -> Result<Value, String> {
    if name.starts_with("__vm_bint_") || name == "__vm_read_bint" {
        return native_bigint(
            name,
            args,
            heap,
            data.as_deref(),
            objects,
            input,
            input_pos,
            heap_base,
        );
    }
    if name.starts_with("__vm_vec_")
        || name.starts_with("__vm_set_")
        || name.starts_with("__vm_map_")
        || name.starts_with("__vm_object_")
    {
        return native_object(name, args, heap, data.as_deref(), objects, heap_base);
    }
    if name.starts_with("__vm_str_")
        || ["__vm_read_text", "__vm_memcpy", "__vm_memset", "__vm_print"].contains(&name)
    {
        return native_string(name, args, heap, data, input, input_pos, heap_base, text);
    }
    Err(format!("函数入口不存在：{name}"))
}

fn need<'a>(args: &'a [(String, Value)], index: usize, name: &str) -> Result<&'a Value, String> {
    args.get(index)
        .map(|(_, v)| v)
        .ok_or_else(|| format!("{name} 需要 {} 个参数", index + 1))
}
fn read_bytes(
    data: Option<&DataArea>,
    heap: &Heap,
    address: usize,
    count: usize,
    base: usize,
) -> Result<Vec<u8>, String> {
    if let Some(data) = data {
        if let Some(bytes) = data.binary.get(address..address + count) {
            return Ok(bytes.to_vec());
        }
    }
    if address >= base && heap.range(address, count).is_some() {
        return Ok(heap.bytes[address - base..address - base + count].to_vec());
    }
    Err(format!("内存地址 {address} 不在有效块内"))
}
fn write_bytes(
    data: Option<&mut DataArea>,
    heap: &mut Heap,
    address: usize,
    bytes: &[u8],
    base: usize,
) -> Result<(), String> {
    if let Some(data) = data {
        if let Some(target) = data.binary.get_mut(address..address + bytes.len()) {
            target.copy_from_slice(bytes);
            return Ok(());
        }
    }
    if address >= base && heap.range(address, bytes.len()).is_some() {
        heap.bytes[address - base..address - base + bytes.len()].copy_from_slice(bytes);
        return Ok(());
    }
    Err(format!("内存地址 {address} 不在有效块内"))
}
fn read_string(
    data: Option<&DataArea>,
    heap: &Heap,
    address: usize,
    base: usize,
) -> Result<String, String> {
    if address == 0 {
        return Ok(String::new());
    }
    let mut bytes = Vec::new();
    for i in 0..1_000_000usize {
        let byte = read_bytes(data, heap, address + i, 1, base)?[0];
        if byte == 0 {
            return Ok(String::from_utf8_lossy(&bytes).into_owned());
        }
        bytes.push(byte);
    }
    Err("字符串没有结尾的 NUL".into())
}
fn alloc_string(heap: &mut Heap, text: &str, base: usize) -> Value {
    let address = heap.alloc(text.len() + 1, base);
    let offset = address - base;
    heap.bytes[offset..offset + text.len()].copy_from_slice(text.as_bytes());
    heap.bytes[offset + text.len()] = 0;
    Value::Int(address as i64)
}
#[expect(
    clippy::too_many_arguments,
    reason = "native functions share the VM's run state"
)]
fn native_bigint(
    name: &str,
    args: &[(String, Value)],
    heap: &mut Heap,
    data: Option<&DataArea>,
    objects: &mut ObjectStore,
    input: &[String],
    input_pos: &mut usize,
    base: usize,
) -> Result<Value, String> {
    let n = |i| need(args, i, name).map(Value::int);
    let get = |i: usize, objects: &ObjectStore| -> Result<BigInt, String> {
        match objects.get(n(i)?)? {
            Object::Big(v) => Ok(v.clone()),
            _ => Err("需要 bint 句柄".into()),
        }
    };
    let result = match name {
        "__vm_bint_from_int" => BigInt::from_i64(n(0)?),
        "__vm_bint_from_flt" => BigInt::from_i64(need(args, 0, name)?.float() as i64),
        "__vm_bint_from_str" => {
            BigInt::from_string(&read_string(data, heap, n(0)? as usize, base)?)
                .ok_or("不是合法的大整数文本")?
        }
        "__vm_read_bint" => {
            let value = input.get(*input_pos).ok_or("输入不够，取不到大整数")?;
            *input_pos += 1;
            BigInt::from_string(value).ok_or("输入项不是合法的大整数")?
        }
        "__vm_bint_add" => get(0, objects)? + get(1, objects)?,
        "__vm_bint_sub" => get(0, objects)? - get(1, objects)?,
        "__vm_bint_mul" => get(0, objects)? * get(1, objects)?,
        "__vm_bint_div" => {
            let divisor = get(1, objects)?;
            if divisor.is_zero() {
                return Err("大整数 / 0：除数是零".into());
            }
            get(0, objects)? / divisor
        }
        "__vm_bint_mod" => {
            let divisor = get(1, objects)?;
            if divisor.is_zero() {
                return Err("大整数 % 0：除数是零".into());
            }
            get(0, objects)? % divisor
        }
        "__vm_bint_neg" => -get(0, objects)?,
        "__vm_bint_cmp" => {
            return Ok(Value::Int(match get(0, objects)?.cmp(&get(1, objects)?) {
                std::cmp::Ordering::Less => -1,
                std::cmp::Ordering::Equal => 0,
                std::cmp::Ordering::Greater => 1,
            }))
        }
        "__vm_bint_to_int" => {
            let value = get(0, objects)?;
            let low = value.to_u64() as i64;
            return Ok(Value::Int(if value.sign() < 0 {
                low.wrapping_neg()
            } else {
                low
            }));
        }
        "__vm_bint_to_flt" => return Ok(Value::Float(get(0, objects)?.to_f64())),
        "__vm_bint_str" => return Ok(alloc_string(heap, &get(0, objects)?.to_string(), base)),
        _ => return Err(format!("函数入口不存在：{name}")),
    };
    Ok(Value::Int(objects.add(Object::Big(result))))
}

fn object_key(
    value: &Value,
    is_text: bool,
    heap: &Heap,
    data: Option<&DataArea>,
    base: usize,
) -> Result<Key, String> {
    if is_text {
        return Ok(Key::Text(read_string(
            data,
            heap,
            value.int() as usize,
            base,
        )?));
    }
    match value {
        Value::Float(n) => Ok(Key::Float(*n)),
        _ => Ok(Key::Int(value.int())),
    }
}
fn key_value(key: Key, heap: &mut Heap, base: usize) -> Value {
    match key {
        Key::Int(n) => Value::Int(n),
        Key::Float(n) => Value::Float(n),
        Key::Text(text) => alloc_string(heap, &text, base),
    }
}
fn native_object(
    name: &str,
    args: &[(String, Value)],
    heap: &mut Heap,
    data: Option<&DataArea>,
    objects: &mut ObjectStore,
    base: usize,
) -> Result<Value, String> {
    let n = |i| need(args, i, name).map(Value::int);
    let zero = Value::Int(0);
    let result = match name {
        "__vm_vec_new" => Value::Int(objects.add(Object::Vec(Vec::new()))),
        "__vm_set_new" => Value::Int(objects.add(Object::Set(BTreeSet::new()))),
        "__vm_map_new" => Value::Int(objects.add(Object::Map(BTreeMap::new()))),
        "__vm_object_copy" => {
            let copy = objects.get(n(0)?)?.clone();
            Value::Int(objects.add(copy))
        }
        "__vm_object_free" => {
            if objects.items.remove(&n(0)?).is_none() {
                return Err("对象已经释放过了".into());
            }
            zero
        }
        "__vm_vec_push" | "__vm_vec_insert" | "__vm_vec_set" | "__vm_vec_find" => {
            let (value_index, flag_index) = if name == "__vm_vec_push" {
                (1, 2)
            } else {
                (2, 3)
            };
            let (value_index, flag_index) = if name == "__vm_vec_find" {
                (1, 2)
            } else {
                (value_index, flag_index)
            };
            let key = object_key(
                need(args, value_index, name)?,
                n(flag_index)? != 0,
                heap,
                data,
                base,
            )?;
            let vec = match objects.get_mut(n(0)?)? {
                Object::Vec(items) => items,
                _ => return Err("需要 vec 句柄".into()),
            };
            match name {
                "__vm_vec_push" => {
                    vec.push(key);
                    zero
                }
                "__vm_vec_insert" => {
                    let index = n(1)?;
                    if index < 0 || index as usize > vec.len() {
                        return Err("vec 下标越界".into());
                    }
                    vec.insert(index as usize, key);
                    zero
                }
                "__vm_vec_set" => {
                    let index = n(1)?;
                    let slot = vec.get_mut(index as usize).ok_or("vec 下标越界")?;
                    *slot = key;
                    zero
                }
                _ => Value::Int(
                    vec.iter()
                        .position(|item| item == &key)
                        .map(|i| i as i64)
                        .unwrap_or(-1),
                ),
            }
        }
        "__vm_vec_at" | "__vm_vec_pop" => {
            let key = {
                let vec = match objects.get_mut(n(0)?)? {
                    Object::Vec(items) => items,
                    _ => return Err("需要 vec 句柄".into()),
                };
                if name == "__vm_vec_pop" {
                    vec.pop().ok_or("vec.pop：表是空的")?
                } else {
                    vec.get(n(1)? as usize).cloned().ok_or("vec 下标越界")?
                }
            };
            key_value(key, heap, base)
        }
        "__vm_vec_remove" | "__vm_vec_len" | "__vm_vec_clear" => {
            let vec = match objects.get_mut(n(0)?)? {
                Object::Vec(items) => items,
                _ => return Err("需要 vec 句柄".into()),
            };
            match name {
                "__vm_vec_remove" => {
                    let index = n(1)?;
                    if index < 0 || index as usize >= vec.len() {
                        return Err("vec 下标越界".into());
                    }
                    vec.remove(index as usize);
                    zero
                }
                "__vm_vec_len" => Value::Int(vec.len() as i64),
                _ => {
                    vec.clear();
                    zero
                }
            }
        }
        "__vm_set_add" | "__vm_set_has" | "__vm_set_remove" => {
            let key = object_key(need(args, 1, name)?, n(2)? != 0, heap, data, base)?;
            let set = match objects.get_mut(n(0)?)? {
                Object::Set(items) => items,
                _ => return Err("需要 set 句柄".into()),
            };
            match name {
                "__vm_set_add" => {
                    set.insert(key);
                    zero
                }
                "__vm_set_has" => Value::Int(set.contains(&key) as i64),
                _ => Value::Int(set.remove(&key) as i64),
            }
        }
        "__vm_set_at" => {
            let key = {
                let set = match objects.get(n(0)?)? {
                    Object::Set(items) => items,
                    _ => return Err("需要 set 句柄".into()),
                };
                set.iter()
                    .nth(n(1)? as usize)
                    .cloned()
                    .ok_or("set 下标越界")?
            };
            key_value(key, heap, base)
        }
        "__vm_set_len" | "__vm_set_clear" => {
            let set = match objects.get_mut(n(0)?)? {
                Object::Set(items) => items,
                _ => return Err("需要 set 句柄".into()),
            };
            if name == "__vm_set_len" {
                Value::Int(set.len() as i64)
            } else {
                set.clear();
                zero
            }
        }
        "__vm_map_put" => {
            let key = object_key(need(args, 1, name)?, n(2)? != 0, heap, data, base)?;
            let val = object_key(need(args, 3, name)?, n(4)? != 0, heap, data, base)?;
            let map = match objects.get_mut(n(0)?)? {
                Object::Map(items) => items,
                _ => return Err("需要 map 句柄".into()),
            };
            map.insert(key, val);
            zero
        }
        "__vm_map_get" | "__vm_map_has" | "__vm_map_remove" => {
            let key = object_key(need(args, 1, name)?, n(2)? != 0, heap, data, base)?;
            let map = match objects.get_mut(n(0)?)? {
                Object::Map(items) => items,
                _ => return Err("需要 map 句柄".into()),
            };
            match name {
                "__vm_map_get" => {
                    let found = map.get(&key).cloned();
                    if let Some(value) = found {
                        key_value(value, heap, base)
                    } else if n(3)? != 0 {
                        alloc_string(heap, "", base)
                    } else {
                        zero
                    }
                }
                "__vm_map_has" => Value::Int(map.contains_key(&key) as i64),
                _ => Value::Int(map.remove(&key).is_some() as i64),
            }
        }
        "__vm_map_len" | "__vm_map_clear" => {
            let map = match objects.get_mut(n(0)?)? {
                Object::Map(items) => items,
                _ => return Err("需要 map 句柄".into()),
            };
            if name == "__vm_map_len" {
                Value::Int(map.len() as i64)
            } else {
                map.clear();
                zero
            }
        }
        "__vm_map_key_at" | "__vm_map_val_at" => {
            let key = {
                let map = match objects.get(n(0)?)? {
                    Object::Map(items) => items,
                    _ => return Err("需要 map 句柄".into()),
                };
                let (key, value) = map.iter().nth(n(1)? as usize).ok_or("map 下标越界")?;
                if name == "__vm_map_key_at" {
                    key.clone()
                } else {
                    value.clone()
                }
            };
            key_value(key, heap, base)
        }
        _ => return Err(format!("函数入口不存在：{name}")),
    };
    Ok(result)
}

#[expect(
    clippy::too_many_arguments,
    reason = "native functions share the VM's run state"
)]
fn native_string(
    name: &str,
    args: &[(String, Value)],
    heap: &mut Heap,
    mut data: Option<&mut DataArea>,
    input: &[String],
    input_pos: &mut usize,
    base: usize,
    text_out: &mut String,
) -> Result<Value, String> {
    let n = |i| need(args, i, name).map(Value::int);
    let read = |i: usize, heap: &Heap, data: Option<&DataArea>| {
        read_string(data, heap, n(i)? as usize, base)
    };
    let zero = Value::Int(0);
    let result = match name {
        "__vm_str_lit" => alloc_string(heap, &read(0, heap, data.as_deref())?, base),
        "__vm_str_len" => Value::Int(read(0, heap, data.as_deref())?.len() as i64),
        "__vm_str_join" => {
            let mut joined = String::new();
            for i in 0..args.len() {
                joined.push_str(&read(i, heap, data.as_deref())?)
            }
            alloc_string(heap, &joined, base)
        }
        "__vm_str_sub" => {
            let value = read(0, heap, data.as_deref())?;
            let start = n(1)?.max(0).min(value.len() as i64) as usize;
            let end = if n(2)? < 0 {
                value.len()
            } else {
                (n(2)? as usize).min(value.len()).max(start)
            };
            alloc_string(
                heap,
                &String::from_utf8_lossy(&value.as_bytes()[start..end]),
                base,
            )
        }
        "__vm_str_of_int" => alloc_string(heap, &n(0)?.to_string(), base),
        "__vm_str_of_flt" => {
            let precision = n(1)?.clamp(0, 100) as usize;
            alloc_string(
                heap,
                &format!("{:.*}", precision, need(args, 0, name)?.float()),
                base,
            )
        }
        "__vm_str_cmp" => Value::Int(
            match read(0, heap, data.as_deref())?.cmp(&read(1, heap, data.as_deref())?) {
                std::cmp::Ordering::Less => -1,
                std::cmp::Ordering::Equal => 0,
                std::cmp::Ordering::Greater => 1,
            },
        ),
        "__vm_str_pos" => Value::Int(
            read(0, heap, data.as_deref())?
                .find(&read(1, heap, data.as_deref())?)
                .map(|i| i as i64)
                .unwrap_or(-1),
        ),
        "__vm_str_at" => {
            let value = read(0, heap, data.as_deref())?;
            let index = n(1)?;
            if index < 0 || index as usize > value.len() {
                return Err(format!("字符串下标越界：{index}"));
            }
            Value::Int(value.as_bytes().get(index as usize).copied().unwrap_or(0) as i64)
        }
        "__vm_str_set" => {
            let value = read(0, heap, data.as_deref())?;
            let index = n(1)?;
            if index < 0 || index as usize >= value.len() {
                return Err(format!("字符串下标越界：{index}"));
            }
            write_bytes(
                data.as_deref_mut(),
                heap,
                n(0)? as usize + index as usize,
                &[n(2)? as u8],
                base,
            )?;
            zero
        }
        "__vm_read_text" => {
            let value = input.get(*input_pos).ok_or("输入不够，取不到字符串")?;
            *input_pos += 1;
            alloc_string(heap, value, base)
        }
        "__vm_memcpy" => {
            let count = n(2)?;
            if count < 0 {
                return Err("memcpy 的字节数不能是负数".into());
            }
            let bytes = read_bytes(data.as_deref(), heap, n(1)? as usize, count as usize, base)?;
            write_bytes(data.as_deref_mut(), heap, n(0)? as usize, &bytes, base)?;
            zero
        }
        "__vm_memset" => {
            let count = n(2)?;
            if count < 0 {
                return Err("memset 的字节数不能是负数".into());
            }
            write_bytes(
                data.as_deref_mut(),
                heap,
                n(0)? as usize,
                &vec![n(1)? as u8; count as usize],
                base,
            )?;
            zero
        }
        "__vm_str_fmt" | "__vm_print" => {
            let format = read(0, heap, data.as_deref())?;
            let rendered = format_native(&format, &args[1..], heap, data.as_deref(), base)?;
            if name == "__vm_print" {
                text_out.push_str(&rendered);
                zero
            } else {
                alloc_string(heap, &rendered, base)
            }
        }
        _ => return Err(format!("函数入口不存在：{name}")),
    };
    Ok(result)
}

fn format_native(
    format: &str,
    args: &[(String, Value)],
    heap: &Heap,
    data: Option<&DataArea>,
    base: usize,
) -> Result<String, String> {
    let mut out = String::new();
    let mut chars = format.chars().peekable();
    let mut index = 0;
    while let Some(ch) = chars.next() {
        if ch != '%' {
            out.push(ch);
            continue;
        }
        if chars.peek() == Some(&'%') {
            chars.next();
            out.push('%');
            continue;
        }
        let mut flags = String::new();
        while chars.peek().is_some_and(|c| "-+ #0".contains(*c)) {
            flags.push(chars.next().unwrap());
        }
        let mut width = String::new();
        while chars.peek().is_some_and(|c| c.is_ascii_digit()) {
            width.push(chars.next().unwrap());
        }
        let mut precision = String::new();
        if chars.peek() == Some(&'.') {
            chars.next();
            while chars.peek().is_some_and(|c| c.is_ascii_digit()) {
                precision.push(chars.next().unwrap());
            }
        }
        let spec = chars.next().ok_or("格式串以 % 结尾")?;
        let arg = args
            .get(index)
            .map(|(_, v)| v)
            .ok_or_else(|| format!("格式串里的占位符比实参多（%{spec}）"))?;
        index += 1;
        let value = match spec {
            's' => {
                let value = read_string(data, heap, arg.int() as usize, base)?;
                if precision.is_empty() {
                    value
                } else {
                    value
                        .chars()
                        .take(precision.parse().unwrap_or(usize::MAX))
                        .collect()
                }
            }
            'c' => String::from(arg.int() as u8 as char),
            'd' | 'i' => arg.int().to_string(),
            'u' => format!("{}", arg.int() as u64),
            'x' => format!("{:x}", arg.int() as u64),
            'X' => format!("{:X}", arg.int() as u64),
            'b' => format!("{:b}", arg.int() as u64),
            'f' | 'F' => format!("{:.*}", precision.parse().unwrap_or(3), arg.float()),
            'e' => format!("{:.*e}", precision.parse().unwrap_or(3), arg.float()),
            'E' => format!("{:.*E}", precision.parse().unwrap_or(3), arg.float()),
            'g' | 'G' => format_float(arg.float()),
            _ => return Err(format!("不支持格式转换：%{spec}")),
        };
        let pad = width
            .parse::<usize>()
            .unwrap_or(0)
            .saturating_sub(value.len())
            .min(10_000);
        if flags.contains('-') {
            out.push_str(&value);
            out.extend(std::iter::repeat_n(' ', pad));
        } else {
            out.extend(std::iter::repeat_n(
                if flags.contains('0') { '0' } else { ' ' },
                pad,
            ));
            out.push_str(&value);
        }
    }
    if index != args.len() {
        return Err("格式化时实参比格式串里的占位符多".into());
    }
    Ok(out)
}
