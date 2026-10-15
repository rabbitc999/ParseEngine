use std::collections::BTreeMap;

#[derive(Clone, Debug, PartialEq)]
pub enum Type {
    Void,
    Bool,
    Int(u8),
    Uint(u8),
    Float(u8),
    String,
    Array(Box<Type>, String),
    Ref(Box<Type>),
    Struct {
        name: String,
        fields: Vec<(String, Type)>,
        methods: Vec<String>,
        operators: BTreeMap<String, String>,
    },
    Enum {
        name: String,
        variants: Vec<(String, Option<Type>)>,
    },
    Generic(String),
}

impl Type {
    pub fn i8() -> Self {
        Self::Int(8)
    }
    pub fn i16() -> Self {
        Self::Int(16)
    }
    pub fn i32() -> Self {
        Self::Int(32)
    }
    pub fn i64() -> Self {
        Self::Int(64)
    }
    pub fn u8() -> Self {
        Self::Uint(8)
    }
    pub fn u16() -> Self {
        Self::Uint(16)
    }
    pub fn u32() -> Self {
        Self::Uint(32)
    }
    pub fn u64() -> Self {
        Self::Uint(64)
    }
    pub fn f32() -> Self {
        Self::Float(32)
    }
    pub fn f64() -> Self {
        Self::Float(64)
    }
    pub fn boolean() -> Self {
        Self::Bool
    }
    pub fn string_type() -> Self {
        Self::String
    }
    pub fn array(element: Type, meta: impl Into<String>) -> Self {
        Self::Array(Box::new(element), meta.into())
    }
    pub fn reference(target: Type) -> Self {
        Self::Ref(Box::new(target))
    }
    pub fn struct_of(name: impl Into<String>, fields: Vec<(String, Type)>) -> Self {
        Self::Struct {
            name: name.into(),
            fields,
            methods: Vec::new(),
            operators: BTreeMap::new(),
        }
    }
    pub fn enum_of(name: impl Into<String>, variants: Vec<(String, Option<Type>)>) -> Self {
        Self::Enum {
            name: name.into(),
            variants,
        }
    }
    pub fn generic(name: impl Into<String>) -> Self {
        Self::Generic(name.into())
    }
    pub fn quad_type(&self) -> String {
        match self {
            Self::Int(n) => format!("i{n}"),
            Self::Uint(n) => format!("u{n}"),
            Self::Float(n) => format!("f{n}"),
            Self::Bool => "i8".into(),
            _ => "i64".into(),
        }
    }
    pub fn describe(&self) -> String {
        match self {
            Self::Void => "void".into(),
            Self::Bool => "bool".into(),
            Self::Int(n) => format!("i{n}"),
            Self::Uint(n) => format!("u{n}"),
            Self::Float(n) => format!("f{n}"),
            Self::String => "string".into(),
            Self::Array(inner, meta) => {
                let mut value = format!("array<{}>", inner.describe());
                if !meta.is_empty() {
                    value.push_str(&format!("{{{meta}}}"));
                }
                value
            }
            Self::Ref(inner) => format!("ref<{}>?", inner.describe()),
            Self::Struct {
                name,
                fields,
                methods,
                operators,
            } => {
                let mut body = fields
                    .iter()
                    .map(|(n, t)| format!("{n}: {}", t.describe()))
                    .collect::<Vec<_>>()
                    .join(", ");
                if !methods.is_empty() {
                    body.push_str(&format!(" | methods: {}", methods.join(" ")));
                }
                if !operators.is_empty() {
                    body.push_str(&format!(
                        " | operators: {}",
                        operators
                            .iter()
                            .map(|(op, method)| format!("{op}→{method}"))
                            .collect::<Vec<_>>()
                            .join(" ")
                    ));
                }
                format!("struct {name} {{{body}}}")
            }
            Self::Enum { name, variants } => format!(
                "enum {name} {{{}}}",
                variants
                    .iter()
                    .map(|(n, t)| match t {
                        Some(t) => format!("{n}({})", t.describe()),
                        None => n.clone(),
                    })
                    .collect::<Vec<_>>()
                    .join(", ")
            ),
            Self::Generic(name) => format!("generic<{name}>"),
        }
    }
    fn name(&self) -> &str {
        match self {
            Self::Struct { name, .. } | Self::Enum { name, .. } => name,
            _ => "",
        }
    }
}

#[derive(Clone, Debug)]
pub enum Node {
    Int(i64),
    Float(f64),
    String(String),
    NamedString(String, String),
    Bool(bool),
    Null,
    Var(String),
    Binary(String, Box<Node>, Box<Node>),
    Unary(String, Box<Node>),
    Index(Box<Node>, Box<Node>),
    Offset(Box<Node>, Box<Node>, i64),
    Member(Box<Node>, String),
    Call(String, Vec<(String, Node)>),
    CallPointer(Box<Node>, Vec<(String, Node)>),
    MethodCall(Box<Node>, String, Vec<Node>),
    Lambda(Box<Node>),
    ArrayNew(Type, Vec<Node>),
    StructNew(String, Vec<Node>),
    EnumNew(String, String, Vec<Node>),
    Alloc(Box<Node>),
    Load(Box<Node>, Type),
    Cast(Box<Node>, Type),
    CheckIndex(Box<Node>, Box<Node>, String),
    CheckPointer(Box<Node>, Box<Node>, i64, String),
    Assign(String, Box<Node>),
    AssignAt(Box<Node>, Box<Node>),
    StoreAt(Box<Node>, Box<Node>, Type),
    Declare(String, Type),
    If(Box<Node>, Box<Node>, Box<Node>),
    While(Box<Node>, Box<Node>),
    DoWhile(Box<Node>, Box<Node>),
    Label(String),
    Jump(String),
    Return(Box<Node>),
    ExprStmt(Box<Node>),
    Write(Box<Node>),
    Print(String),
    Putc(Box<Node>),
    Read(String),
    Free(Box<Node>),
    Drop(Box<Node>),
    Block(Vec<Node>),
}

impl Node {
    pub fn i(v: i64) -> Self {
        Self::Int(v)
    }
    pub fn f(v: f64) -> Self {
        Self::Float(v)
    }
    pub fn str(v: impl Into<String>) -> Self {
        Self::String(v.into())
    }
    pub fn var(v: impl Into<String>) -> Self {
        Self::Var(v.into())
    }
    pub fn binary(op: impl Into<String>, a: Node, b: Node) -> Self {
        Self::Binary(op.into(), Box::new(a), Box::new(b))
    }
    pub fn unary(op: impl Into<String>, a: Node) -> Self {
        Self::Unary(op.into(), Box::new(a))
    }
    pub fn assign(name: impl Into<String>, v: Node) -> Self {
        Self::Assign(name.into(), Box::new(v))
    }
    pub fn ret(v: Node) -> Self {
        Self::Return(Box::new(v))
    }
    pub fn write(v: Node) -> Self {
        Self::Write(Box::new(v))
    }
    pub fn block(v: Vec<Node>) -> Self {
        Self::Block(v)
    }
    pub fn declare(name: impl Into<String>, ty: Type) -> Self {
        Self::Declare(name.into(), ty)
    }
    pub fn if_then_else(c: Node, yes: Node, no: Node) -> Self {
        Self::If(Box::new(c), Box::new(yes), Box::new(no))
    }
    pub fn while_loop(c: Node, body: Node) -> Self {
        Self::While(Box::new(c), Box::new(body))
    }
    pub fn call(name: impl Into<String>, args: Vec<(String, Node)>) -> Self {
        Self::Call(name.into(), args)
    }
    pub fn named_str(name: impl Into<String>, text: impl Into<String>) -> Self {
        Self::NamedString(name.into(), text.into())
    }
    pub fn boolean(value: bool) -> Self {
        Self::Bool(value)
    }
    pub fn null() -> Self {
        Self::Null
    }
    pub fn index(base: Node, index: Node) -> Self {
        Self::Index(Box::new(base), Box::new(index))
    }
    pub fn offset(base: Node, index: Node, stride: i64) -> Self {
        Self::Offset(Box::new(base), Box::new(index), stride)
    }
    pub fn member(base: Node, field: impl Into<String>) -> Self {
        Self::Member(Box::new(base), field.into())
    }
    pub fn call_pointer(pointer: Node, args: Vec<(String, Node)>) -> Self {
        Self::CallPointer(Box::new(pointer), args)
    }
    pub fn method_call(base: Node, method: impl Into<String>, args: Vec<Node>) -> Self {
        Self::MethodCall(Box::new(base), method.into(), args)
    }
    pub fn lambda(body: Node) -> Self {
        Self::Lambda(Box::new(body))
    }
    pub fn array_new(ty: Type, values: Vec<Node>) -> Self {
        Self::ArrayNew(ty, values)
    }
    pub fn struct_new(name: impl Into<String>, values: Vec<Node>) -> Self {
        Self::StructNew(name.into(), values)
    }
    pub fn enum_new(
        name: impl Into<String>,
        variant: impl Into<String>,
        payload: Vec<Node>,
    ) -> Self {
        Self::EnumNew(name.into(), variant.into(), payload)
    }
    pub fn alloc(bytes: Node) -> Self {
        Self::Alloc(Box::new(bytes))
    }
    pub fn load_from(address: Node, ty: Type) -> Self {
        Self::Load(Box::new(address), ty)
    }
    pub fn cast(value: Node, ty: Type) -> Self {
        Self::Cast(Box::new(value), ty)
    }
    pub fn check_index(index: Node, limit: Node, label: impl Into<String>) -> Self {
        Self::CheckIndex(Box::new(index), Box::new(limit), label.into())
    }
    pub fn check_pointer(index: Node, base: Node, stride: i64, label: impl Into<String>) -> Self {
        Self::CheckPointer(Box::new(index), Box::new(base), stride, label.into())
    }
    pub fn assign_at(target: Node, value: Node) -> Self {
        Self::AssignAt(Box::new(target), Box::new(value))
    }
    pub fn store_at(address: Node, value: Node, ty: Type) -> Self {
        Self::StoreAt(Box::new(address), Box::new(value), ty)
    }
    pub fn do_while_loop(body: Node, condition: Node) -> Self {
        Self::DoWhile(Box::new(body), Box::new(condition))
    }
    pub fn label(name: impl Into<String>) -> Self {
        Self::Label(name.into())
    }
    pub fn jump(name: impl Into<String>) -> Self {
        Self::Jump(name.into())
    }
    pub fn expr_stmt(value: Node) -> Self {
        Self::ExprStmt(Box::new(value))
    }
    pub fn print(text: impl Into<String>) -> Self {
        Self::Print(text.into())
    }
    pub fn putc(value: Node) -> Self {
        Self::Putc(Box::new(value))
    }
    pub fn read(name: impl Into<String>) -> Self {
        Self::Read(name.into())
    }
    pub fn free_memory(pointer: Node) -> Self {
        Self::Free(Box::new(pointer))
    }
    pub fn release(pointer: Node) -> Self {
        Self::Drop(Box::new(pointer))
    }
}

#[derive(Clone, Debug)]
pub struct Function {
    pub name: String,
    pub owner: String,
    pub parameters: Vec<(String, Type)>,
    pub result: Type,
    pub body: Node,
    pub generic_params: Vec<Type>,
}

impl Function {
    pub fn new(name: impl Into<String>, body: Node) -> Self {
        Self {
            name: name.into(),
            owner: String::new(),
            parameters: Vec::new(),
            result: Type::i32(),
            body,
            generic_params: Vec::new(),
        }
    }
    pub fn quad_label(&self) -> String {
        if self.owner.is_empty() {
            self.name.clone()
        } else {
            format!("{}.{}", self.owner, self.name)
        }
    }
}

#[derive(Clone, Debug, Default)]
pub struct Program {
    pub types: Vec<Type>,
    pub constants: Vec<(String, Type, String)>,
    pub variables: Vec<(String, Type)>,
    pub functions: Vec<Function>,
}

impl Program {
    pub fn add_constant(&mut self, name: impl Into<String>, ty: Type, literal: impl Into<String>) {
        self.constants.push((name.into(), ty, literal.into()));
    }
    pub fn add_variable(&mut self, name: impl Into<String>, ty: Type) {
        self.variables.push((name.into(), ty));
    }
    pub fn add_method(&mut self, owner: &str, mut function: Function) {
        for ty in &mut self.types {
            if let Type::Struct { name, methods, .. } = ty {
                if name == owner {
                    methods.push(function.name.clone());
                }
            }
        }
        function.owner = owner.into();
        self.functions.push(function);
    }
    pub fn add_operator(&mut self, owner: &str, op: &str, method: &str) {
        for ty in &mut self.types {
            if let Type::Struct {
                name, operators, ..
            } = ty
            {
                if name == owner {
                    operators.insert(op.into(), method.into());
                }
            }
        }
    }
}

#[derive(Clone, Debug)]
pub struct Entry {
    pub name: String,
    pub ty: Type,
    pub body: String,
}

#[derive(Clone, Debug, Default)]
pub struct CodegenResult {
    pub quads: String,
    pub data: String,
    pub diagnostics: Vec<String>,
    pub entries: Vec<Entry>,
}

impl CodegenResult {
    pub fn ok(&self) -> bool {
        self.diagnostics.is_empty()
    }
    pub fn export_data_binary(&self) -> Vec<u8> {
        self.build_binary().0
    }
    pub fn export_data_layout(&self) -> Vec<String> {
        self.build_binary().1
    }
    pub fn export_data_header(&self, module: &str) -> String {
        let (bytes, layout) = self.build_binary();
        let mut out = format!("/* {module} 的 .data 二进制（小端）。由 simple_ast 生成。 */\n#ifndef SIMPLE_AST_DATA_{module}_H\n#define SIMPLE_AST_DATA_{module}_H\n\n#include <stdint.h>\n\n");
        for line in layout {
            let (name, rest) = line.split_once(' ').unwrap();
            let offset = rest
                .strip_prefix("offset=")
                .unwrap()
                .split_whitespace()
                .next()
                .unwrap();
            out.push_str(&format!("#define DATA_{name}_OFFSET {offset}u\n"));
        }
        out.push_str(&format!(
            "\nstatic const unsigned char {module}_data[] = {{"
        ));
        for (index, byte) in bytes.iter().enumerate() {
            if index % 12 == 0 {
                out.push_str("\n    ");
            }
            out.push_str(&format!("{byte},"));
        }
        out.push_str("\n};\n\n#endif\n");
        out
    }
    fn build_binary(&self) -> (Vec<u8>, Vec<String>) {
        let handles: BTreeMap<_, _> = self
            .entries
            .iter()
            .enumerate()
            .map(|(i, e)| (e.name.as_str(), i as u32))
            .collect();
        let mut bytes = vec![0; 8];
        let mut layout = Vec::new();
        for entry in &self.entries {
            let align = entry
                .ty_meta()
                .split(';')
                .find_map(|piece| {
                    piece
                        .strip_prefix("align=")
                        .and_then(|n| n.parse::<usize>().ok())
                })
                .unwrap_or(8)
                .max(1);
            while !bytes.len().is_multiple_of(align) {
                bytes.push(0);
            }
            let offset = bytes.len();
            match &entry.ty {
                Type::Array(..) | Type::Struct { .. } => {
                    for value in split_top(&entry.body) {
                        write_value(&mut bytes, value, &handles);
                    }
                }
                Type::Enum { variants, .. } => {
                    let (name, payload) = entry.body.split_once('(').unwrap_or((&entry.body, ""));
                    let tag = variants.iter().position(|(n, _)| n == name).unwrap_or(0) as u32;
                    bytes.extend_from_slice(&tag.to_le_bytes());
                    if !payload.is_empty() {
                        for value in split_top(payload.trim_end_matches(')')) {
                            write_value(&mut bytes, value, &handles);
                        }
                    }
                }
                _ => write_value(&mut bytes, &entry.body, &handles),
            }
            layout.push(format!(
                "{} offset={} size={} stride=8 type={}",
                entry.name,
                offset,
                bytes.len() - offset,
                entry.ty.describe()
            ));
        }
        while !bytes.len().is_multiple_of(8) {
            bytes.push(0);
        }
        bytes.extend_from_slice(&[0; 8]);
        (bytes, layout)
    }
}

fn write_value(bytes: &mut Vec<u8>, text: &str, handles: &BTreeMap<&str, u32>) {
    let text = text.trim();
    if text.starts_with('"') && text.ends_with('"') {
        bytes.extend_from_slice(&text.as_bytes()[1..text.len() - 1]);
        bytes.push(0);
    } else if text.contains(['.', 'e', 'E']) && text.parse::<f64>().is_ok() {
        bytes.extend_from_slice(&text.parse::<f64>().unwrap().to_bits().to_le_bytes());
    } else if let Ok(n) = text.parse::<i64>() {
        bytes.extend_from_slice(&n.to_le_bytes());
    } else {
        bytes.extend_from_slice(&handles.get(text).copied().unwrap_or(0).to_le_bytes());
    }
}

impl Entry {
    fn ty_meta(&self) -> &str {
        if let Type::Array(_, meta) = &self.ty {
            meta
        } else {
            ""
        }
    }
}

fn split_top(text: &str) -> Vec<&str> {
    let text = text
        .trim()
        .trim_start_matches(['[', '{'])
        .trim_end_matches([']', '}']);
    let mut depth = 0i32;
    let mut quoted = false;
    let mut start = 0;
    let mut out = Vec::new();
    for (i, ch) in text.char_indices() {
        if ch == '"' {
            quoted = !quoted;
        }
        if !quoted {
            match ch {
                '[' | '{' | '(' => depth += 1,
                ']' | '}' | ')' => depth -= 1,
                ',' if depth == 0 => {
                    out.push(text[start..i].trim());
                    start = i + 1;
                }
                _ => {}
            }
        }
    }
    if start < text.len() {
        out.push(text[start..].trim());
    }
    out
}

#[derive(Default)]
struct Emitter {
    lines: Vec<String>,
    entries: Vec<Entry>,
    data_lines: Vec<String>,
    diagnostics: Vec<String>,
    env: BTreeMap<String, Type>,
    temp_index: usize,
    label_index: usize,
    lambda_index: usize,
}

impl Emitter {
    fn emit(&mut self, op: &str, a: &str, b: &str, c: &str) {
        self.lines.push(format!("({op}, {a}, {b}, {c})"));
    }
    fn temp(&mut self) -> String {
        self.temp_index += 1;
        format!("t{}", self.temp_index)
    }
    fn label(&mut self, hint: &str) -> String {
        self.label_index += 1;
        format!("{hint}_{}", self.label_index)
    }
    fn data(&mut self, name: String, ty: Type, body: String) {
        if self.entries.iter().any(|entry| entry.name == name) {
            return;
        }
        self.data_lines
            .push(format!("# {name} : {}", ty.describe()));
        if let Type::Array(_, meta) = &ty {
            if !meta.is_empty() {
                self.data_lines.push(format!("#   meta: {meta}"));
            }
        }
        self.data_lines.push(format!("{name}: {body}"));
        self.entries.push(Entry { name, ty, body });
    }
    fn infer(&self, node: &Node) -> Type {
        match node {
            Node::Float(_) => Type::f64(),
            Node::Bool(_) => Type::Bool,
            Node::String(_) | Node::NamedString(..) => Type::String,
            Node::Var(name) => self.env.get(name).cloned().unwrap_or(Type::i32()),
            Node::ArrayNew(ty, _)
            | Node::Cast(_, ty)
            | Node::Load(_, ty)
            | Node::Declare(_, ty) => ty.clone(),
            Node::StructNew(name, _) | Node::EnumNew(name, _, _) => {
                self.env.get(name).cloned().unwrap_or(Type::i32())
            }
            Node::Index(base, _) | Node::Offset(base, _, _) => match self.infer(base) {
                Type::Array(inner, _) | Type::Ref(inner) => *inner,
                _ => Type::i32(),
            },
            Node::Member(base, field) => match self.infer(base) {
                Type::Struct { fields, .. } => fields
                    .into_iter()
                    .find(|(n, _)| n == field)
                    .map(|(_, t)| t)
                    .unwrap_or(Type::i32()),
                _ => Type::i32(),
            },
            Node::Binary(op, left, right) => {
                if ["==", "!=", "<", "<=", ">", ">=", "&&", "||"].contains(&op.as_str()) {
                    return Type::Bool;
                }
                let left_ty = self.infer(left);
                if let Type::Struct {
                    name, operators, ..
                } = &left_ty
                {
                    if let Some(method) = operators.get(op) {
                        return self
                            .env
                            .get(&format!("{name}.{method}"))
                            .cloned()
                            .unwrap_or(Type::i32());
                    }
                }
                if matches!(self.infer(right), Type::Float(_)) {
                    Type::f64()
                } else {
                    left_ty
                }
            }
            Node::Unary(op, _) if op == "!" => Type::Bool,
            Node::Unary(_, value)
            | Node::CheckIndex(value, _, _)
            | Node::CheckPointer(value, _, _, _) => self.infer(value),
            Node::Call(name, _) => self.env.get(name).cloned().unwrap_or(Type::i32()),
            Node::MethodCall(base, method, _) => self
                .env
                .get(&format!("{}.{}", self.infer(base).name(), method))
                .cloned()
                .unwrap_or(Type::i32()),
            Node::Alloc(_) | Node::Lambda(_) => Type::i64(),
            _ => Type::i32(),
        }
    }
    fn expr(&mut self, node: &Node) -> String {
        match node {
            Node::Int(n) => n.to_string(),
            Node::Float(n) => float_text(*n),
            Node::Bool(v) => if *v { "1" } else { "0" }.into(),
            Node::Null => "0".into(),
            Node::Var(name) => {
                if !self.env.contains_key(name) {
                    self.diagnostics.push(format!("引用了未声明的变量：{name}"));
                }
                name.clone()
            }
            Node::String(value) | Node::NamedString(_, value) => {
                let name = if let Node::NamedString(name, _) = node {
                    name.clone()
                } else {
                    self.label("str")
                };
                self.data(name.clone(), Type::String, format!("\"{value}\""));
                let dst = self.temp();
                self.emit("addr", &name, "_", &dst);
                dst
            }
            Node::Binary(op, left, right) => {
                let lt = self.infer(left);
                let rt = self.infer(right);
                if (matches!(lt, Type::Ref(_)) || matches!(rt, Type::Ref(_)))
                    && op != "=="
                    && op != "!="
                {
                    self.diagnostics.push(format!(
                        "引用只支持 == / !=（可空判断），不做指针运算：运算符 {op}"
                    ));
                }
                if let Type::Struct {
                    name, operators, ..
                } = &lt
                {
                    if let Some(method) = operators.get(op) {
                        let a = self.expr(left);
                        let b = self.expr(right);
                        let dst = self.temp();
                        self.emit("param", &a, "_", "self");
                        self.emit("param", &b, "_", "other");
                        self.emit("call", &format!("{name}.{method}"), "_", &dst);
                        return dst;
                    }
                }
                let a = self.expr(left);
                let b = self.expr(right);
                let dst = self.temp();
                let ty = if matches!(lt, Type::Float(_)) || matches!(rt, Type::Float(_)) {
                    Type::f64()
                } else if ["==", "!=", "<", "<=", ">", ">="].contains(&op.as_str()) {
                    Type::i32()
                } else {
                    lt
                };
                self.emit(
                    &format!(
                        "{}:{}",
                        if op == "&&" {
                            "&"
                        } else if op == "||" {
                            "|"
                        } else {
                            op
                        },
                        ty.quad_type()
                    ),
                    &a,
                    &b,
                    &dst,
                );
                dst
            }
            Node::Unary(op, value) => {
                let ty = self.infer(value).quad_type();
                let a = self.expr(value);
                let dst = self.temp();
                if op == "!" {
                    self.emit("==:i32", &a, "0", &dst);
                } else {
                    self.emit(
                        &format!("{}:{ty}", if op == "-" { "neg" } else { op }),
                        &a,
                        "_",
                        &dst,
                    );
                }
                dst
            }
            Node::Cast(value, ty) => {
                let a = self.expr(value);
                let dst = self.temp();
                self.emit(&format!("cvt:{}", ty.quad_type()), &a, "_", &dst);
                dst
            }
            Node::Index(_, _) | Node::Member(_, _) => {
                let ty = self.infer(node).quad_type();
                let addr = self.address(node);
                let dst = self.temp();
                self.emit(&format!("load:{ty}"), &addr, "_", &dst);
                dst
            }
            Node::Offset(_, _, _) => self.address(node),
            Node::Alloc(bytes) => {
                let n = self.expr(bytes);
                let dst = self.temp();
                self.emit("alloc", &n, "_", &dst);
                dst
            }
            Node::Load(address, ty) => {
                let a = self.expr(address);
                let dst = self.temp();
                self.emit(&format!("load:{}", ty.quad_type()), &a, "_", &dst);
                dst
            }
            Node::ArrayNew(ty, items) => {
                let name = self.label("arr");
                let inner = if let Type::Array(inner, _) = ty {
                    &**inner
                } else {
                    ty
                };
                let values: Vec<_> = items
                    .iter()
                    .map(|item| {
                        self.literal_for(item, inner)
                            .unwrap_or_else(|| self.expr(item))
                    })
                    .collect();
                self.data(name.clone(), ty.clone(), format!("[{}]", values.join(", ")));
                let dst = self.temp();
                self.emit("addr", &name, "_", &dst);
                dst
            }
            Node::StructNew(name, fields) => {
                let ty = self.env.get(name).cloned().unwrap_or(Type::i64());
                let field_types = if let Type::Struct { fields, .. } = &ty {
                    fields.iter().map(|(_, t)| t.clone()).collect::<Vec<_>>()
                } else {
                    Vec::new()
                };
                let values: Vec<_> = fields
                    .iter()
                    .enumerate()
                    .map(|(i, v)| {
                        self.literal_for(v, field_types.get(i).unwrap_or(&Type::i64()))
                            .unwrap_or_else(|| self.expr(v))
                    })
                    .collect();
                let label = self.label("obj");
                self.data(label.clone(), ty, format!("{{{}}}", values.join(", ")));
                let dst = self.temp();
                self.emit("addr", &label, "_", &dst);
                dst
            }
            Node::EnumNew(name, variant, payload) => {
                let ty = self.env.get(name).cloned().unwrap_or(Type::i64());
                let values: Vec<_> = payload
                    .iter()
                    .map(|v| {
                        self.literal_for(v, &Type::i64())
                            .unwrap_or_else(|| self.expr(v))
                    })
                    .collect();
                let label = self.label("enum");
                self.data(
                    label.clone(),
                    ty,
                    format!("{variant}({})", values.join(", ")),
                );
                let dst = self.temp();
                self.emit("addr", &label, "_", &dst);
                dst
            }
            Node::Call(name, args) => {
                let values: Vec<_> = args
                    .iter()
                    .map(|(param, value)| (param.clone(), self.expr(value)))
                    .collect();
                for (param, value) in values {
                    self.emit(
                        "param",
                        &value,
                        "_",
                        if param.is_empty() { "arg" } else { &param },
                    );
                }
                let dst = self.temp();
                self.emit("call", name, "_", &dst);
                dst
            }
            Node::CallPointer(pointer, args) => {
                let ptr = self.expr(pointer);
                let values: Vec<_> = args
                    .iter()
                    .map(|(param, value)| (param.clone(), self.expr(value)))
                    .collect();
                for (param, value) in values {
                    self.emit(
                        "param",
                        &value,
                        "_",
                        if param.is_empty() { "arg" } else { &param },
                    );
                }
                let dst = self.temp();
                self.emit("callptr", &ptr, "_", &dst);
                dst
            }
            Node::MethodCall(base, method, args) => {
                let owner = self.infer(base).name().to_string();
                let receiver = self.expr(base);
                let values: Vec<_> = args.iter().map(|v| self.expr(v)).collect();
                self.emit("param", &receiver, "_", "self");
                for value in values {
                    self.emit("param", &value, "_", "arg");
                }
                let dst = self.temp();
                self.emit("call", &format!("{owner}.{method}"), "_", &dst);
                dst
            }
            Node::Lambda(body) => {
                self.lambda_index += 1;
                let name = format!("lambda_{}", self.lambda_index);
                let saved = std::mem::take(&mut self.lines);
                self.stmt(body);
                self.emit("ret", "0", "_", "_");
                let lines = std::mem::replace(&mut self.lines, saved);
                self.data(
                    name.clone(),
                    Type::i32(),
                    format!("{{ {name} | {} }}", lines.join(" | ")),
                );
                name
            }
            Node::CheckIndex(index, limit, label) => {
                let i = self.expr(index);
                let n = self.expr(limit);
                self.emit("check", &i, &n, label);
                i
            }
            Node::CheckPointer(index, base, stride, label) => {
                let i = self.expr(index);
                let b = self.expr(base);
                self.emit("checkptr", &i, &b, &format!("{stride}, {label}"));
                i
            }
            other => {
                self.stmt(other);
                "0".into()
            }
        }
    }
    fn literal_for(&self, node: &Node, ty: &Type) -> Option<String> {
        let value = match node {
            Node::Int(n) => n.to_string(),
            Node::Float(n) => float_text(*n),
            Node::Bool(v) => (*v as i32).to_string(),
            Node::Null => "0".into(),
            _ => return None,
        };
        if matches!(ty, Type::Float(_)) && !value.contains(['.', 'e', 'E']) {
            Some(format!("{value}.0"))
        } else {
            Some(value)
        }
    }
    fn address(&mut self, node: &Node) -> String {
        let (base, offset) = match node {
            Node::Index(base, pos) => {
                let a = self.expr(base);
                let p = self.expr(pos);
                let scaled = self.temp();
                self.emit("*:i64", &p, "8", &scaled);
                (a, scaled)
            }
            Node::Offset(base, pos, stride) => {
                let a = self.expr(base);
                let p = self.expr(pos);
                let scaled = self.temp();
                self.emit("*:i64", &p, &stride.to_string(), &scaled);
                (a, scaled)
            }
            Node::Member(base, field) => {
                let ty = self.infer(base);
                let idx = if let Type::Struct { fields, .. } = ty {
                    fields.iter().position(|(n, _)| n == field).unwrap_or(0)
                } else {
                    0
                };
                (self.expr(base), (idx * 8).to_string())
            }
            _ => {
                self.diagnostics
                    .push("赋值目标只能是变量、下标或成员".into());
                return "0".into();
            }
        };
        let dst = self.temp();
        self.emit("+:i64", &base, &offset, &dst);
        dst
    }
    fn stmt(&mut self, node: &Node) {
        match node {
            Node::Block(items) => {
                for item in items {
                    self.stmt(item);
                }
            }
            Node::ExprStmt(value) => {
                self.expr(value);
            }
            Node::Assign(name, value) => {
                let v = self.expr(value);
                self.emit(":=", &v, "_", name);
            }
            Node::AssignAt(target, value) => {
                let v = self.expr(value);
                if let Node::Var(name) = &**target {
                    self.emit(":=", &v, "_", name);
                } else {
                    let ty = self.infer(target).quad_type();
                    let a = self.address(target);
                    self.emit(&format!("store:{ty}"), &a, &v, "_");
                }
            }
            Node::StoreAt(address, value, ty) => {
                let v = self.expr(value);
                let a = self.expr(address);
                self.emit(&format!("store:{}", ty.quad_type()), &a, &v, "_");
            }
            Node::Declare(name, ty) => {
                self.env.insert(name.clone(), ty.clone());
                self.emit("decl", &ty.quad_type(), "local", name);
            }
            Node::If(cond, yes, no) => {
                let otherwise = self.label("else");
                let done = self.label("endif");
                let c = self.expr(cond);
                self.emit("jz", &c, "_", &otherwise);
                self.stmt(yes);
                self.emit("j", "_", "_", &done);
                self.emit("label", "_", "_", &otherwise);
                self.stmt(no);
                self.emit("label", "_", "_", &done);
            }
            Node::While(cond, body) => {
                let start = self.label("while");
                let done = self.label("endwhile");
                self.emit("label", "_", "_", &start);
                let c = self.expr(cond);
                self.emit("jz", &c, "_", &done);
                self.stmt(body);
                self.emit("j", "_", "_", &start);
                self.emit("label", "_", "_", &done);
            }
            Node::DoWhile(body, cond) => {
                let start = self.label("do");
                self.emit("label", "_", "_", &start);
                self.stmt(body);
                let c = self.expr(cond);
                self.emit("jnz", &c, "_", &start);
            }
            Node::Label(name) => self.emit("label", "_", "_", name),
            Node::Jump(name) => self.emit("j", "_", "_", name),
            Node::Return(value) => {
                let v = self.expr(value);
                self.emit("ret", &v, "_", "_");
            }
            Node::Write(value) => {
                let v = self.expr(value);
                self.emit("write", &v, "_", "_");
            }
            Node::Print(text) => {
                let escaped = text
                    .replace('\\', "\\\\")
                    .replace('"', "\\\"")
                    .replace('\n', "\\n")
                    .replace('\t', "\\t");
                self.emit("print", &format!("\"{escaped}\""), "_", "_");
            }
            Node::Putc(value) => {
                let v = self.expr(value);
                self.emit("putc", &v, "_", "_");
            }
            Node::Read(name) => self.emit("read", "_", "_", name),
            Node::Free(value) | Node::Drop(value) => {
                let v = self.expr(value);
                self.emit(
                    if matches!(node, Node::Free(_)) {
                        "free"
                    } else {
                        "drop"
                    },
                    &v,
                    "_",
                    "_",
                );
            }
            other => {
                self.expr(other);
            }
        }
    }
}

fn float_text(n: f64) -> String {
    let mut s = n.to_string();
    if !s.contains(['.', 'e', 'E']) {
        s.push_str(".0");
    }
    s
}

pub fn generate(program: &Program, module: &str) -> CodegenResult {
    let mut em = Emitter::default();
    let mut lines = vec![format!("# module: {module}")];
    for ty in &program.types {
        lines.push(format!("# type {}", ty.describe()));
        em.env.insert(ty.name().into(), ty.clone());
    }
    for function in &program.functions {
        em.env
            .insert(function.quad_label(), function.result.clone());
        em.env
            .insert(function.name.clone(), function.result.clone());
    }
    for (name, ty, _) in &program.constants {
        em.env.insert(name.clone(), ty.clone());
        lines.push(format!("# const {name} : {}", ty.describe()));
    }
    for (name, ty) in &program.variables {
        em.env.insert(name.clone(), ty.clone());
        em.emit("decl", &ty.quad_type(), "global", name);
    }
    for (name, _, value) in &program.constants {
        em.emit(":=", value, "_", name);
    }
    let mut index = 0;
    flush(&mut lines, &mut em.lines, &mut index);
    let mut funcs: Vec<_> = program.functions.iter().collect();
    funcs.sort_by_key(|f| if f.name == "main" { 0 } else { 1 });
    if funcs.first().is_some_and(|f| f.name == "main") {
        em.emit("call", "main", "_", "_");
        em.emit("halt", "_", "_", "_");
        flush(&mut lines, &mut em.lines, &mut index);
    }
    for f in funcs {
        for (name, ty) in &f.parameters {
            em.env.insert(name.clone(), ty.clone());
            em.emit("decl", &ty.quad_type(), "local", name);
        }
        if !f.owner.is_empty() {
            em.env.insert(
                "self".into(),
                program
                    .types
                    .iter()
                    .find(|ty| ty.name() == f.owner)
                    .cloned()
                    .unwrap_or(Type::i64()),
            );
            em.emit("decl", "i64", "local", "self");
        }
        em.emit("label", "_", "_", &f.quad_label());
        em.stmt(&f.body);
        em.emit("ret", "0", "_", "_");
        flush(&mut lines, &mut em.lines, &mut index);
    }
    lines.push("(halt, _, _, _)".into());
    let mut data = format!("# simple_ast data: {module}\n");
    for line in &em.data_lines {
        data.push_str(line);
        data.push('\n');
    }
    CodegenResult {
        quads: format!("{}\n", lines.join("\n")),
        data,
        diagnostics: em.diagnostics,
        entries: em.entries,
    }
}

fn flush(out: &mut Vec<String>, pending: &mut Vec<String>, index: &mut usize) {
    for line in pending.drain(..) {
        out.push(format!("{}: {line}", *index));
        *index += 1;
    }
}
