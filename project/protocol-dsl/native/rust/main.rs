mod parser;
mod token;

use parser::{Parser, Tree};
use std::collections::{BTreeMap, BTreeSet};
use std::env;
use std::fs;
use std::path::{Path, PathBuf};

#[derive(Clone)]
struct Tok {
    text: String,
    line: i32,
}
#[derive(Clone)]
struct EnumItem {
    name: String,
    value: u64,
}
#[derive(Clone)]
struct Field {
    name: String,
    ty: String,
    length: String,
    bytes: bool,
    fixed: bool,
}
#[derive(Clone)]
struct Decl {
    kind: String,
    name: String,
    base: String,
    items: Vec<EnumItem>,
    fields: Vec<Field>,
    borrowed: bool,
}
struct Schema {
    name: String,
    endian: String,
    decls: Vec<Decl>,
}

fn leaves(node: &Tree, out: &mut Vec<Tok>) {
    if node.children.is_empty() {
        if !node.text.is_empty() {
            out.push(Tok {
                text: node.text.clone(),
                line: node.location().map(|x| x.line).unwrap_or(0),
            });
        }
    } else {
        for child in &node.children {
            leaves(child, out);
        }
    }
}

struct Cursor {
    tokens: Vec<Tok>,
    pos: usize,
}
impl Cursor {
    fn new(tokens: Vec<Tok>) -> Self {
        Self { tokens, pos: 0 }
    }
    fn done(&self) -> bool {
        self.pos == self.tokens.len()
    }
    fn take(&mut self) -> Result<String, String> {
        if self.done() {
            return Err("unexpected end of protocol".into());
        }
        let value = self.tokens[self.pos].text.clone();
        self.pos += 1;
        Ok(value)
    }
    fn expect(&mut self, expected: &str) -> Result<(), String> {
        let line = if self.done() {
            0
        } else {
            self.tokens[self.pos].line
        };
        let actual = self.take()?;
        if actual != expected {
            Err(format!(
                "line {}: expected `{}`, got `{}`",
                line, expected, actual
            ))
        } else {
            Ok(())
        }
    }
}

fn integer_type(s: &str) -> bool {
    matches!(
        s,
        "u8" | "u16" | "u32" | "u64" | "i8" | "i16" | "i32" | "i64"
    )
}
fn unsigned_type(s: &str) -> bool {
    matches!(s, "u8" | "u16" | "u32" | "u64")
}
fn check_name(s: &str, what: &str) -> Result<(), String> {
    const RESERVED: &[&str] = &[
        "as",
        "async",
        "await",
        "break",
        "const",
        "continue",
        "crate",
        "dyn",
        "else",
        "enum",
        "extern",
        "false",
        "fn",
        "for",
        "if",
        "impl",
        "in",
        "let",
        "loop",
        "match",
        "mod",
        "move",
        "mut",
        "pub",
        "ref",
        "return",
        "self",
        "Self",
        "static",
        "struct",
        "super",
        "trait",
        "true",
        "type",
        "unsafe",
        "use",
        "where",
        "while",
        "decode",
        "decode_from",
        "encode",
        "encode_into",
    ];
    if s.starts_with('_') || RESERVED.contains(&s) {
        Err(format!("{} `{}` is reserved", what, s))
    } else {
        Ok(())
    }
}
fn width(s: &str) -> usize {
    match s {
        "u8" | "i8" => 1,
        "u16" | "i16" => 2,
        "u32" | "i32" => 4,
        _ => 8,
    }
}
fn rust_type(s: &str) -> &str {
    s
}
fn snake(s: &str) -> String {
    let mut out = String::new();
    let chars: Vec<char> = s.chars().collect();
    for (i, ch) in chars.iter().enumerate() {
        if i > 0 && ch.is_uppercase() && chars[i - 1].is_lowercase() {
            out.push('_');
        }
        out.extend(ch.to_lowercase());
    }
    out
}

fn build_schema(c: &mut Cursor) -> Result<Schema, String> {
    c.expect("protocol")?;
    let name = c.take()?;
    check_name(&name, "protocol name")?;
    c.expect(";")?;
    c.expect("endian")?;
    let endian = c.take()?;
    c.expect(";")?;
    let mut decls = Vec::new();
    let mut known: BTreeMap<String, Decl> = BTreeMap::new();
    while !c.done() {
        let kind = c.take()?;
        let dname = c.take()?;
        check_name(&dname, "type name")?;
        if known.contains_key(&dname) || integer_type(&dname) || dname == "bytes" {
            return Err(format!("duplicate type `{}`", dname));
        }
        let mut d = Decl {
            kind: kind.clone(),
            name: dname.clone(),
            base: String::new(),
            items: Vec::new(),
            fields: Vec::new(),
            borrowed: false,
        };
        if kind == "enum" {
            c.expect(":")?;
            d.base = c.take()?;
            c.expect("{")?;
            let mut names = BTreeSet::new();
            let mut values = BTreeSet::new();
            let limit = if width(&d.base) == 8 {
                u64::MAX
            } else {
                (1u64 << (width(&d.base) * 8)) - 1
            };
            loop {
                let n = c.take()?;
                if n == "}" {
                    break;
                }
                check_name(&n, "enum item")?;
                c.expect("=")?;
                let v = c.take()?.parse::<u64>().map_err(|e| e.to_string())?;
                c.expect(";")?;
                if !names.insert(n.clone()) || !values.insert(v) {
                    return Err("duplicate enum item".into());
                }
                if v > limit {
                    return Err(format!("enum value does not fit {}", d.base));
                }
                d.items.push(EnumItem { name: n, value: v });
            }
            c.expect(";")?;
        } else if kind == "packet" {
            c.expect("{")?;
            let mut fields: BTreeMap<String, String> = BTreeMap::new();
            loop {
                let ty = c.take()?;
                if ty == "}" {
                    break;
                }
                let mut f = Field {
                    name: String::new(),
                    ty: String::new(),
                    length: String::new(),
                    bytes: false,
                    fixed: false,
                };
                if ty == "bytes" {
                    f.bytes = true;
                    f.name = c.take()?;
                    c.expect("[")?;
                    f.length = c.take()?;
                    c.expect("]")?;
                    c.expect(";")?;
                    f.fixed = f.length.chars().all(|x| x.is_ascii_digit());
                    if f.fixed {
                        if f.length.parse::<usize>().unwrap_or(0) == 0 {
                            return Err("byte length must be positive".into());
                        }
                    } else if !fields
                        .get(&f.length)
                        .map(|x| unsigned_type(x))
                        .unwrap_or(false)
                    {
                        return Err(format!(
                            "length field `{}` must be an earlier unsigned integer",
                            f.length
                        ));
                    }
                    d.borrowed = true;
                } else {
                    f.ty = ty.clone();
                    f.name = c.take()?;
                    c.expect(";")?;
                    if !integer_type(&ty) && !known.contains_key(&ty) {
                        return Err(format!("unknown or forward type `{}`", ty));
                    }
                    if known.get(&ty).map(|x| x.borrowed).unwrap_or(false) {
                        d.borrowed = true;
                    }
                }
                check_name(&f.name, "field name")?;
                if fields.contains_key(&f.name) {
                    return Err(format!("duplicate field `{}`", f.name));
                }
                fields.insert(f.name.clone(), f.ty.clone());
                d.fields.push(f);
            }
            c.expect(";")?;
        } else {
            return Err("expected enum or packet".into());
        }
        known.insert(dname, d.clone());
        decls.push(d);
    }
    Ok(Schema {
        name,
        endian,
        decls,
    })
}

fn field_type(f: &Field, known: &BTreeMap<String, Decl>) -> String {
    if f.bytes {
        "&'a [u8]".into()
    } else if integer_type(&f.ty) {
        rust_type(&f.ty).into()
    } else {
        format!(
            "{}{}",
            f.ty,
            if known.get(&f.ty).map(|x| x.borrowed).unwrap_or(false) {
                "<'a>"
            } else {
                ""
            }
        )
    }
}

fn declarations(s: &Schema) -> String {
    let little = if s.endian == "little" {
        "true"
    } else {
        "false"
    };
    let mut known: BTreeMap<String, Decl> = BTreeMap::new();
    let mut o = String::new();
    for d in &s.decls {
        if d.kind == "enum" {
            o+=&format!("#[repr({})]\n#[allow(non_camel_case_types)]\n#[derive(Debug, Clone, Copy, PartialEq, Eq)]\npub enum {} {{\n",d.base,d.name);
            for x in &d.items {
                o += &format!("    {} = {},\n", x.name, x.value);
            }
            o += "}\n\n";
            o+=&format!("impl core::convert::TryFrom<{}> for {} {{\n    type Error = DecodeError;\n    fn try_from(value: {}) -> Result<Self, Self::Error> {{\n        match value {{\n",d.base,d.name,d.base);
            for x in &d.items {
                o += &format!("            {} => Ok(Self::{}),\n", x.value, x.name);
            }
            o += "            _ => Err(DecodeError::InvalidEnum),\n        }\n    }\n}\n\n";
            known.insert(d.name.clone(), d.clone());
            continue;
        }
        let lt = if d.borrowed { "<'a>" } else { "" };
        o += &format!(
            "#[derive(Debug, Clone, PartialEq, Eq)]\npub struct {}{} {{\n",
            d.name, lt
        );
        for f in &d.fields {
            o += &format!("    pub {}: {},\n", f.name, field_type(f, &known));
        }
        o += "}\n\n";
        o+=&format!("impl{} {}{} {{\n    pub fn decode_from(data: {}[u8], offset: &mut usize) -> Result<Self, DecodeError> {{\n",lt,d.name,lt,if d.borrowed{"&'a "}else{"&"});
        for f in &d.fields {
            if f.bytes {
                o += &format!(
                    "        let {} = read_bytes(data, offset, {})?;\n",
                    f.name,
                    if f.fixed {
                        f.length.clone()
                    } else {
                        format!("{} as usize", f.length)
                    }
                );
            } else if integer_type(&f.ty) {
                o += &format!(
                    "        let {} = read_u(data, offset, {}, {})? as {};\n",
                    f.name,
                    width(&f.ty),
                    little,
                    f.ty
                );
            } else if known.get(&f.ty).map(|x| x.kind.as_str()) == Some("enum") {
                let base = &known[&f.ty].base;
                o += &format!(
                    "        let {} = {}::try_from(read_u(data, offset, {}, {})? as {})?;\n",
                    f.name,
                    f.ty,
                    width(base),
                    little,
                    base
                );
            } else {
                o += &format!(
                    "        let {} = {}::decode_from(data, offset)?;\n",
                    f.name, f.ty
                );
            }
        }
        o += "        Ok(Self { ";
        for (i, f) in d.fields.iter().enumerate() {
            if i > 0 {
                o += ", ";
            }
            o += &f.name;
        }
        o += " })\n    }\n\n";
        o+=&format!("    pub fn decode(data: {}[u8]) -> Result<Self, DecodeError> {{ let mut offset=0; let value=Self::decode_from(data,&mut offset)?; if offset!=data.len() {{return Err(DecodeError::TrailingBytes);}} Ok(value) }}\n\n",if d.borrowed{"&'a "}else{"&"});
        o+="    pub fn encode_into(&self, out: &mut [u8], offset: &mut usize) -> Result<(), EncodeError> {\n";
        for f in &d.fields {
            if f.bytes {
                o+=&format!("        if self.{}.len() != {} {{return Err(EncodeError::LengthMismatch);}}\n        write_bytes(out,offset,self.{})?;\n",f.name,if f.fixed{f.length.clone()}else{format!("self.{} as usize",f.length)},f.name);
            } else if integer_type(&f.ty) {
                o += &format!(
                    "        write_u(out,offset,self.{} as u64,{}, {})?;\n",
                    f.name,
                    width(&f.ty),
                    little
                );
            } else if known.get(&f.ty).map(|x| x.kind.as_str()) == Some("enum") {
                let base = &known[&f.ty].base;
                o += &format!(
                    "        write_u(out,offset,self.{} as {} as u64,{}, {})?;\n",
                    f.name,
                    base,
                    width(base),
                    little
                );
            } else {
                o += &format!("        self.{}.encode_into(out,offset)?;\n", f.name);
            }
        }
        o+="        Ok(())\n    }\n\n    pub fn encode(&self, out: &mut [u8]) -> Result<usize, EncodeError> { let mut offset=0; self.encode_into(out,&mut offset)?; Ok(offset) }\n}\n\n";
        known.insert(d.name.clone(), d.clone());
    }
    o
}

fn replace_all(mut text: String, key: &str, value: &str) -> String {
    text = text.replace(key, value);
    text
}
fn run() -> Result<PathBuf, String> {
    let args: Vec<String> = env::args().collect();
    if args.len() != 4 || args[2] != "--out-dir" {
        return Err("usage: protocol-dsl-rust INPUT --out-dir DIR".into());
    }
    let input = fs::read_to_string(&args[1]).map_err(|e| e.to_string())?;
    let mut parser = Parser::new();
    let tree = parser.parse_tree(&input).ok_or_else(|| {
        parser
            .last_error()
            .map(|e| format!("{}:{}:{}: {}", args[1], e.line, e.column, e.message))
            .unwrap_or_else(|| "syntax error".into())
    })?;
    let mut tokens = Vec::new();
    leaves(&tree, &mut tokens);
    let schema = build_schema(&mut Cursor::new(tokens))?;
    let module = snake(&schema.name);
    let mut template = PathBuf::from("templates/rust_module.rs.tmpl");
    if !template.exists() {
        template = env::current_exe()
            .map_err(|e| e.to_string())?
            .parent()
            .unwrap()
            .parent()
            .unwrap()
            .join("templates/rust_module.rs.tmpl");
    }
    let mut output = fs::read_to_string(template).map_err(|e| e.to_string())?;
    output = replace_all(output, "$protocol_name", &schema.name);
    output = replace_all(output, "$rust_declarations", &declarations(&schema));
    let dir = Path::new(&args[3]);
    fs::create_dir_all(dir).map_err(|e| e.to_string())?;
    let path = dir.join(format!("{}_protocol.rs", module));
    fs::write(&path, output).map_err(|e| e.to_string())?;
    Ok(path)
}

fn main() {
    match run() {
        Ok(path) => println!("{}", path.display()),
        Err(e) => {
            eprintln!("{}", e);
            std::process::exit(1);
        }
    }
}
