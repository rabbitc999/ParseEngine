"""Schema model, validation, and code generation for protocol-dsl."""

from pathlib import Path
from string import Template
import keyword
import re


class ProtocolDefinitionError(ValueError):
    pass


INTEGER_TYPES = {
    "u8": (1, False), "u16": (2, False), "u32": (4, False), "u64": (8, False),
    "i8": (1, True), "i16": (2, True), "i32": (4, True), "i64": (8, True),
}
CPP_TYPES = {
    "u8": "std::uint8_t", "u16": "std::uint16_t", "u32": "std::uint32_t",
    "u64": "std::uint64_t", "i8": "std::int8_t", "i16": "std::int16_t",
    "i32": "std::int32_t", "i64": "std::int64_t",
}
C_TYPES = {
    "u8": "uint8_t", "u16": "uint16_t", "u32": "uint32_t", "u64": "uint64_t",
    "i8": "int8_t", "i16": "int16_t", "i32": "int32_t", "i64": "int64_t",
}
RUST_TYPES = {
    "u8": "u8", "u16": "u16", "u32": "u32", "u64": "u64",
    "i8": "i8", "i16": "i16", "i32": "i32", "i64": "i64",
}
CPP_KEYWORDS = set("alignas alignof and and_eq asm auto bitand bitor bool break case catch char "
                   "class compl concept const consteval constexpr constinit const_cast continue "
                   "co_await co_return co_yield decltype default delete do double dynamic_cast else "
                   "enum explicit export extern false float for friend goto if inline int long mutable "
                   "namespace new noexcept not not_eq nullptr operator or or_eq private protected public "
                   "register reinterpret_cast requires return short signed sizeof static static_assert "
                   "static_cast struct switch template this thread_local throw true try typedef typeid "
                   "typename union unsigned using virtual void volatile wchar_t while xor xor_eq".split())
TYPE_RESERVED = {"DecodeError", "EncodeError", "ProtocolDecodeError", "ProtocolEncodeError",
                 "IntEnum", "dataclass", "detail"}
FIELD_RESERVED = {"decode", "decode_from", "encode", "encode_to"}


def one(value):
    return [value]


def append(values, value):
    return values + [value]


def make_schema(name, endian, declarations):
    return {"name": str(name), "endian": str(endian), "declarations": declarations}


def make_enum(name, base, items, line):
    return {"kind": "enum", "name": str(name), "base": str(base),
            "items": items, "line": int(line)}


def make_enum_item(name, value, line):
    return {"name": str(name), "value": int(str(value)), "line": int(line)}


def make_packet(name, fields, line):
    return {"kind": "packet", "name": str(name), "fields": fields, "line": int(line)}


def make_field(type_name, name, line):
    return {"kind": "value", "type": str(type_name), "name": str(name), "line": int(line)}


def make_bytes_field(name, length, line):
    return {"kind": "bytes", "name": str(name), "length": length, "line": int(line)}


def fixed_length(value):
    return {"kind": "fixed", "value": int(str(value))}


def field_length(name):
    return {"kind": "field", "value": str(name)}


def _check_identifier(name, what, line):
    if name.startswith("_"):
        raise ProtocolDefinitionError("第 %d 行：%s `%s` 不能以下划线开头" %
                                      (line, what, name))
    if keyword.iskeyword(name) or name in CPP_KEYWORDS:
        raise ProtocolDefinitionError("第 %d 行：%s `%s` 是目标语言保留字" % (line, what, name))


def validate_schema(schema):
    """Validate ordering and binary-layout constraints; return the enriched schema."""
    if not schema or not schema.get("declarations"):
        raise ProtocolDefinitionError("协议至少要声明一个 enum 或 packet")
    _check_identifier(schema["name"], "协议名", 1)
    known = {}
    for decl in schema["declarations"]:
        name = decl["name"]
        line = decl["line"]
        _check_identifier(name, "类型名", line)
        if name in known or name in INTEGER_TYPES or name == "bytes" or name in TYPE_RESERVED:
            raise ProtocolDefinitionError("第 %d 行：类型 `%s` 重复定义" % (line, name))
        if decl["kind"] == "enum":
            _, signed = INTEGER_TYPES[decl["base"]]
            if signed:
                raise ProtocolDefinitionError("第 %d 行：枚举底层类型必须是无符号整数" % line)
            bits = INTEGER_TYPES[decl["base"]][0] * 8
            limit = (1 << bits) - 1
            item_names = set()
            item_values = set()
            for item in decl["items"]:
                _check_identifier(item["name"], "枚举项", item["line"])
                if item["name"] in item_names:
                    raise ProtocolDefinitionError("第 %d 行：枚举项 `%s` 重复" %
                                                  (item["line"], item["name"]))
                if item["value"] in item_values:
                    raise ProtocolDefinitionError("第 %d 行：枚举值 %d 重复" %
                                                  (item["line"], item["value"]))
                if item["value"] > limit:
                    raise ProtocolDefinitionError("第 %d 行：%s 容不下枚举值 %d" %
                                                  (item["line"], decl["base"], item["value"]))
                item_names.add(item["name"])
                item_values.add(item["value"])
        else:
            field_names = {}
            for field in decl["fields"]:
                fname = field["name"]
                _check_identifier(fname, "字段名", field["line"])
                if fname in FIELD_RESERVED:
                    raise ProtocolDefinitionError("第 %d 行：字段 `%s` 与生成方法重名" %
                                                  (field["line"], fname))
                if fname in field_names:
                    raise ProtocolDefinitionError("第 %d 行：字段 `%s` 重复" %
                                                  (field["line"], fname))
                if field["kind"] == "value":
                    type_name = field["type"]
                    if type_name not in INTEGER_TYPES and type_name not in known:
                        raise ProtocolDefinitionError(
                            "第 %d 行：未知或前向引用的类型 `%s`" % (field["line"], type_name))
                else:
                    length = field["length"]
                    if length["kind"] == "fixed":
                        if length["value"] <= 0:
                            raise ProtocolDefinitionError("第 %d 行：字节数组长度必须大于 0" %
                                                          field["line"])
                    else:
                        ref = length["value"]
                        previous = field_names.get(ref)
                        if previous is None:
                            raise ProtocolDefinitionError(
                                "第 %d 行：长度字段 `%s` 必须在字节数组之前声明" %
                                (field["line"], ref))
                        if previous.get("type") not in ("u8", "u16", "u32", "u64"):
                            raise ProtocolDefinitionError(
                                "第 %d 行：长度字段 `%s` 必须是无符号整数" %
                                (field["line"], ref))
                field_names[fname] = field
        known[name] = decl
        if decl["kind"] == "packet":
            decl["borrowed"] = any(
                field["kind"] == "bytes" or
                (field["kind"] == "value" and field["type"] in known and
                 known[field["type"]].get("borrowed", False))
                for field in decl["fields"]
            )
    schema["types"] = known
    schema["module"] = _snake(schema["name"])
    return schema


def _snake(name):
    text = re.sub(r"([a-z0-9])([A-Z])", r"\1_\2", name).replace("-", "_")
    return text.lower()


def _indent(lines, count=1):
    prefix = "    " * count
    return "\n".join(prefix + line if line else "" for line in lines)


def _python_type(field):
    if field["kind"] == "bytes":
        return "bytes"
    if field["type"] in INTEGER_TYPES:
        return "int"
    return field["type"]


def _python_declarations(schema):
    blocks = []
    known = schema["types"]
    little = schema["endian"] == "little"
    for decl in schema["declarations"]:
        if decl["kind"] == "enum":
            lines = ["class %s(IntEnum):" % decl["name"]]
            lines += ["    %s = %d" % (item["name"], item["value"]) for item in decl["items"]]
            blocks.append("\n".join(lines))
            continue
        lines = ["@dataclass", "class %s:" % decl["name"]]
        for field in decl["fields"]:
            lines.append("    %s: %s" % (field["name"], _python_type(field)))
        lines += ["", "    @classmethod", "    def decode_from(cls, data, offset=0):"]
        decode = []
        ctor = []
        for field in decl["fields"]:
            name = field["name"]
            ctor.append("%s=%s" % (name, name))
            if field["kind"] == "bytes":
                length = str(field["length"]["value"])
                decode.append("%s, offset = _read_bytes(data, offset, int(%s))" % (name, length))
            elif field["type"] in INTEGER_TYPES:
                size, signed = INTEGER_TYPES[field["type"]]
                decode.append("%s, offset = _read_integer(data, offset, %d, %s, %s)" %
                              (name, size, repr(signed), repr(little)))
            elif known[field["type"]]["kind"] == "enum":
                enum_decl = known[field["type"]]
                size, signed = INTEGER_TYPES[enum_decl["base"]]
                decode.append("_%s, offset = _read_integer(data, offset, %d, %s, %s)" %
                              (name, size, repr(signed), repr(little)))
                decode.append("%s = _as_enum(%s, _%s)" % (name, field["type"], name))
            else:
                decode.append("%s, offset = %s.decode_from(data, offset)" % (name, field["type"]))
        decode.append("return cls(%s), offset" % ", ".join(ctor))
        lines += ["        " + line for line in decode]
        lines += ["", "    @classmethod", "    def decode(cls, data):",
                  "        value, offset = cls.decode_from(data, 0)",
                  "        if offset != len(data):",
                  "            raise ProtocolDecodeError(\"trailing bytes: %d\" % (len(data) - offset))",
                  "        return value", "", "    def encode(self):", "        out = bytearray()"]
        encode = []
        for field in decl["fields"]:
            name = field["name"]
            expr = "self.%s" % name
            if field["kind"] == "bytes":
                length = field["length"]
                expected = str(length["value"]) if length["kind"] == "fixed" else "int(self.%s)" % length["value"]
                encode += ["_%s = bytes(%s)" % (name, expr),
                           "if len(_%s) != %s:" % (name, expected),
                           "    raise ProtocolEncodeError(\"%s must contain %%d bytes\" %% %s)" %
                           (name, expected),
                           "out += _%s" % name]
            elif field["type"] in INTEGER_TYPES:
                size, signed = INTEGER_TYPES[field["type"]]
                encode.append("out += _write_integer(%s, %d, %s, %s, %r)" %
                              (expr, size, repr(signed), repr(little), name))
            elif known[field["type"]]["kind"] == "enum":
                enum_decl = known[field["type"]]
                size, signed = INTEGER_TYPES[enum_decl["base"]]
                encode.append("out += _write_integer(_enum_value(%s, %s, %r), %d, %s, %s, %r)" %
                              (field["type"], expr, name, size, repr(signed), repr(little), name))
            else:
                encode.append("out += %s.encode()" % expr)
        encode.append("return bytes(out)")
        lines += ["        " + line for line in encode]
        blocks.append("\n".join(lines))
    return "\n\n\n".join(blocks)


def _cpp_type(field):
    if field["kind"] == "bytes":
        return "std::vector<std::uint8_t>"
    return CPP_TYPES.get(field["type"], field["type"])


def _cpp_declarations(schema):
    blocks = []
    known = schema["types"]
    little = "true" if schema["endian"] == "little" else "false"
    for decl in schema["declarations"]:
        if decl["kind"] == "enum":
            items = ",\n".join("    %s = %d" % (item["name"], item["value"])
                                for item in decl["items"])
            valid = "\n".join("    case %d: return true;" % item["value"] for item in decl["items"])
            blocks.append("enum class %s : %s {\n%s\n};\n\ninline bool is_valid_%s(%s value)\n{\n    switch (static_cast<std::uint64_t>(value)) {\n%s\n    default: return false;\n    }\n}" %
                          (decl["name"], CPP_TYPES[decl["base"]], items, decl["name"],
                           decl["name"], valid))
            continue
        name = decl["name"]
        lines = ["struct %s" % name, "{"]
        for field in decl["fields"]:
            lines.append("    %s %s{};" % (_cpp_type(field), field["name"]))
        lines += ["", "    static %s decode_from(const std::vector<std::uint8_t>& data, std::size_t& offset)" % name,
                  "    {", "        %s value;" % name]
        decode = []
        for field in decl["fields"]:
            fname = field["name"]
            if field["kind"] == "bytes":
                length = str(field["length"]["value"])
                if field["length"]["kind"] == "field":
                    length = "static_cast<std::size_t>(value.%s)" % length
                decode.append("value.%s = detail::read_bytes(data, offset, %s);" % (fname, length))
            elif field["type"] in INTEGER_TYPES:
                decode.append("value.%s = detail::read_integer<%s>(data, offset, %s);" %
                              (fname, CPP_TYPES[field["type"]], little))
            elif known[field["type"]]["kind"] == "enum":
                enum_decl = known[field["type"]]
                base = CPP_TYPES[enum_decl["base"]]
                decode += ["const auto raw_%s = detail::read_integer<%s>(data, offset, %s);" %
                           (fname, base, little),
                           "value.%s = static_cast<%s>(raw_%s);" % (fname, field["type"], fname),
                           "if (!is_valid_%s(value.%s))" % (field["type"], fname),
                           "    throw DecodeError(\"invalid %s value\");" % field["type"]]
            else:
                decode.append("value.%s = %s::decode_from(data, offset);" % (fname, field["type"]))
        lines += ["        " + line for line in decode]
        lines += ["        return value;", "    }", "",
                  "    static %s decode(const std::vector<std::uint8_t>& data)" % name,
                  "    {", "        std::size_t offset = 0;",
                  "        auto value = decode_from(data, offset);",
                  "        if (offset != data.size()) throw DecodeError(\"trailing bytes\");",
                  "        return value;", "    }", "", "    void encode_to(std::vector<std::uint8_t>& out) const", "    {"]
        encode = []
        for field in decl["fields"]:
            fname = field["name"]
            if field["kind"] == "bytes":
                length = field["length"]
                expected = str(length["value"]) if length["kind"] == "fixed" else "static_cast<std::size_t>(%s)" % length["value"]
                encode += ["if (%s.size() != %s)" % (fname, expected),
                           "    throw EncodeError(\"%s has an invalid length\");" % fname,
                           "out.insert(out.end(), %s.begin(), %s.end());" % (fname, fname)]
            elif field["type"] in INTEGER_TYPES:
                encode.append("detail::write_integer<%s>(out, %s, %s);" %
                              (CPP_TYPES[field["type"]], fname, little))
            elif known[field["type"]]["kind"] == "enum":
                enum_decl = known[field["type"]]
                encode += ["if (!is_valid_%s(%s))" % (field["type"], fname),
                           "    throw EncodeError(\"invalid %s value\");" % field["type"],
                           "detail::write_integer<%s>(out, static_cast<%s>(%s), %s);" %
                           (CPP_TYPES[enum_decl["base"]], CPP_TYPES[enum_decl["base"]], fname, little)]
            else:
                encode.append("%s.encode_to(out);" % fname)
        lines += ["        " + line for line in encode]
        lines += ["    }", "", "    std::vector<std::uint8_t> encode() const",
                  "    {", "        std::vector<std::uint8_t> out;", "        encode_to(out);",
                  "        return out;", "    }", "};"]
        blocks.append("\n".join(lines))
    return "\n\n".join(blocks)


def _upper(name):
    return _snake(name).upper()


def _c_type(schema, field):
    if field["kind"] == "bytes":
        return "%s_bytes" % schema["module"]
    return C_TYPES.get(field["type"], "%s_%s" % (schema["module"], field["type"]))


def _c_declarations(schema):
    prefix = schema["module"]
    macro = _upper(schema["name"])
    little = "true" if schema["endian"] == "little" else "false"
    known = schema["types"]
    blocks = []
    for decl in schema["declarations"]:
        cname = "%s_%s" % (prefix, decl["name"])
        if decl["kind"] == "enum":
            constants = "\n".join("#define %s_%s_%s ((%s)%d)" %
                                    (macro, _upper(decl["name"]), _upper(item["name"]),
                                     cname, item["value"]) for item in decl["items"])
            valid = "\n".join("    case %s_%s_%s: return true;" %
                                (macro, _upper(decl["name"]), _upper(item["name"]))
                                for item in decl["items"])
            blocks.append("typedef %s %s;\n%s\n\nstatic inline bool %s_is_valid(%s value)\n{\n    switch (value) {\n%s\n    default: return false;\n    }\n}" %
                          (C_TYPES[decl["base"]], cname, constants, cname, cname, valid))
            continue
        lines = ["typedef struct %s" % cname, "{"]
        for field in decl["fields"]:
            lines.append("    %s %s;" % (_c_type(schema, field), field["name"]))
        lines += ["} %s;" % cname, "",
                  "static inline bool %s_decode_from(const uint8_t *data, size_t size, size_t *offset," % cname,
                  "                                      %s *out, %s_error *error)" % (cname, prefix), "{"]
        decode = []
        for field in decl["fields"]:
            fname = field["name"]
            if field["kind"] == "bytes":
                length = str(field["length"]["value"])
                if field["length"]["kind"] == "field":
                    length = "(size_t)out->%s" % length
                decode += ["if (!%s_read_bytes(data, size, offset, %s, &out->%s, error)) return false;" %
                           (prefix, length, fname)]
            elif field["type"] in INTEGER_TYPES:
                width, signed = INTEGER_TYPES[field["type"]]
                decode += ["uint64_t raw_%s;" % fname,
                           "if (!%s_read_u(data, size, offset, %d, %s, &raw_%s, error)) return false;" %
                           (prefix, width, little, fname),
                           "out->%s = (%s)%s;" %
                           (fname, C_TYPES[field["type"]],
                            "%s_sign_extend(raw_%s, %d)" % (prefix, fname, width)
                            if signed else "raw_%s" % fname)]
            elif known[field["type"]]["kind"] == "enum":
                enum_decl = known[field["type"]]
                width = INTEGER_TYPES[enum_decl["base"]][0]
                etype = "%s_%s" % (prefix, field["type"])
                decode += ["uint64_t raw_%s;" % fname,
                           "if (!%s_read_u(data, size, offset, %d, %s, &raw_%s, error)) return false;" %
                           (prefix, width, little, fname),
                           "out->%s = (%s)raw_%s;" % (fname, etype, fname),
                           "if (!%s_is_valid(out->%s)) { *error = %s_ERROR_ENUM; return false; }" %
                           (etype, fname, macro)]
            else:
                nested = "%s_%s" % (prefix, field["type"])
                decode.append("if (!%s_decode_from(data, size, offset, &out->%s, error)) return false;" %
                              (nested, fname))
        lines += ["    " + line for line in decode]
        lines += ["    return true;", "}", "",
                  "static inline bool %s_decode(const uint8_t *data, size_t size, %s *out, %s_error *error)" %
                  (cname, cname, prefix), "{", "    size_t offset = 0;",
                  "    if (!%s_decode_from(data, size, &offset, out, error)) return false;" % cname,
                  "    if (offset != size) { *error = %s_ERROR_TRAILING; return false; }" % macro,
                  "    return true;", "}", "",
                  "static inline bool %s_encode_to(const %s *value, uint8_t *out, size_t capacity," %
                  (cname, cname), "                                    size_t *offset, %s_error *error)" % prefix, "{"]
        encode = []
        for field in decl["fields"]:
            fname = field["name"]
            if field["kind"] == "bytes":
                length = field["length"]
                expected = str(length["value"]) if length["kind"] == "fixed" else "(size_t)value->%s" % length["value"]
                encode += ["if (value->%s.size != %s) { *error = %s_ERROR_LENGTH; return false; }" %
                           (fname, expected, macro),
                           "if (!%s_write_bytes(out, capacity, offset, value->%s.data, value->%s.size, error)) return false;" %
                           (prefix, fname, fname)]
            elif field["type"] in INTEGER_TYPES:
                width = INTEGER_TYPES[field["type"]][0]
                encode.append("if (!%s_write_u(out, capacity, offset, (uint64_t)value->%s, %d, %s, error)) return false;" %
                              (prefix, fname, width, little))
            elif known[field["type"]]["kind"] == "enum":
                enum_decl = known[field["type"]]
                width = INTEGER_TYPES[enum_decl["base"]][0]
                etype = "%s_%s" % (prefix, field["type"])
                encode += ["if (!%s_is_valid(value->%s)) { *error = %s_ERROR_ENUM; return false; }" %
                           (etype, fname, macro),
                           "if (!%s_write_u(out, capacity, offset, (uint64_t)value->%s, %d, %s, error)) return false;" %
                           (prefix, fname, width, little)]
            else:
                nested = "%s_%s" % (prefix, field["type"])
                encode.append("if (!%s_encode_to(&value->%s, out, capacity, offset, error)) return false;" %
                              (nested, fname))
        lines += ["    " + line for line in encode]
        lines += ["    return true;", "}", "",
                  "static inline bool %s_encode(const %s *value, uint8_t *out, size_t capacity," %
                  (cname, cname), "                               size_t *written, %s_error *error)" % prefix,
                  "{", "    *written = 0;",
                  "    return %s_encode_to(value, out, capacity, written, error);" % cname, "}"]
        blocks.append("\n".join(lines))
    return "\n\n".join(blocks)


def _rust_type(schema, field):
    if field["kind"] == "bytes":
        return "&'a [u8]"
    if field["type"] in RUST_TYPES:
        return RUST_TYPES[field["type"]]
    decl = schema["types"][field["type"]]
    return field["type"] + ("<'a>" if decl.get("borrowed", False) else "")


def _rust_declarations(schema):
    little = "true" if schema["endian"] == "little" else "false"
    known = schema["types"]
    blocks = []
    for decl in schema["declarations"]:
        if decl["kind"] == "enum":
            variants = "\n".join("    %s = %d," % (item["name"], item["value"])
                                  for item in decl["items"])
            matches = "\n".join("            %d => Ok(Self::%s)," % (item["value"], item["name"])
                                 for item in decl["items"])
            base = RUST_TYPES[decl["base"]]
            blocks.append("#[repr(%s)]\n#[allow(non_camel_case_types)]\n#[derive(Debug, Clone, Copy, PartialEq, Eq)]\npub enum %s {\n%s\n}\n\nimpl core::convert::TryFrom<%s> for %s {\n    type Error = DecodeError;\n    fn try_from(value: %s) -> Result<Self, Self::Error> {\n        match value {\n%s\n            _ => Err(DecodeError::InvalidEnum),\n        }\n    }\n}" %
                          (base, decl["name"], variants, base, decl["name"], base, matches))
            continue
        name = decl["name"]
        lifetime = "<'a>" if decl.get("borrowed", False) else ""
        impl_lifetime = "<'a>" if decl.get("borrowed", False) else ""
        lines = ["#[derive(Debug, Clone, PartialEq, Eq)]", "pub struct %s%s {" % (name, lifetime)]
        for field in decl["fields"]:
            lines.append("    pub %s: %s," % (field["name"], _rust_type(schema, field)))
        lines += ["}", "", "impl%s %s%s {" % (impl_lifetime, name, lifetime),
                  "    pub fn decode_from(data: %s[u8], offset: &mut usize) -> Result<Self, DecodeError> {" %
                  ("&'a " if decl.get("borrowed", False) else "&")]
        decode = []
        fields = []
        for field in decl["fields"]:
            fname = field["name"]
            fields.append(fname)
            if field["kind"] == "bytes":
                length = str(field["length"]["value"])
                if field["length"]["kind"] == "field":
                    length = "%s as usize" % length
                decode.append("let %s = read_bytes(data, offset, %s)?;" % (fname, length))
            elif field["type"] in INTEGER_TYPES:
                width = INTEGER_TYPES[field["type"]][0]
                decode.append("let %s = read_u(data, offset, %d, %s)? as %s;" %
                              (fname, width, little, RUST_TYPES[field["type"]]))
            elif known[field["type"]]["kind"] == "enum":
                enum_decl = known[field["type"]]
                width = INTEGER_TYPES[enum_decl["base"]][0]
                decode.append("let %s = %s::try_from(read_u(data, offset, %d, %s)? as %s)?;" %
                              (fname, field["type"], width, little, RUST_TYPES[enum_decl["base"]]))
            else:
                decode.append("let %s = %s::decode_from(data, offset)?;" % (fname, field["type"]))
        lines += ["        " + line for line in decode]
        lines += ["        Ok(Self { %s })" % ", ".join(fields), "    }", "",
                  "    pub fn decode(data: %s[u8]) -> Result<Self, DecodeError> {" %
                  ("&'a " if decl.get("borrowed", False) else "&"), "        let mut offset = 0;",
                  "        let value = Self::decode_from(data, &mut offset)?;",
                  "        if offset != data.len() { return Err(DecodeError::TrailingBytes); }",
                  "        Ok(value)", "    }", "",
                  "    pub fn encode_into(&self, out: &mut [u8], offset: &mut usize) -> Result<(), EncodeError> {"]
        encode = []
        for field in decl["fields"]:
            fname = field["name"]
            if field["kind"] == "bytes":
                length = field["length"]
                expected = str(length["value"]) if length["kind"] == "fixed" else "self.%s as usize" % length["value"]
                encode += ["if self.%s.len() != %s { return Err(EncodeError::LengthMismatch); }" %
                           (fname, expected), "write_bytes(out, offset, self.%s)?;" % fname]
            elif field["type"] in INTEGER_TYPES:
                width = INTEGER_TYPES[field["type"]][0]
                encode.append("write_u(out, offset, self.%s as u64, %d, %s)?;" %
                              (fname, width, little))
            elif known[field["type"]]["kind"] == "enum":
                enum_decl = known[field["type"]]
                width = INTEGER_TYPES[enum_decl["base"]][0]
                encode.append("write_u(out, offset, self.%s as %s as u64, %d, %s)?;" %
                              (fname, RUST_TYPES[enum_decl["base"]], width, little))
            else:
                encode.append("self.%s.encode_into(out, offset)?;" % fname)
        lines += ["        " + line for line in encode]
        lines += ["        Ok(())", "    }", "",
                  "    pub fn encode(&self, out: &mut [u8]) -> Result<usize, EncodeError> {",
                  "        let mut offset = 0;", "        self.encode_into(out, &mut offset)?;",
                  "        Ok(offset)", "    }", "}"]
        blocks.append("\n".join(lines))
    return "\n\n".join(blocks)


def _doc_tables(schema):
    sections = []
    for decl in schema["declarations"]:
        if decl["kind"] == "enum":
            rows = ["| 名称 | 值 |", "| --- | ---: |"]
            rows += ["| `%s` | `%d` |" % (item["name"], item["value"]) for item in decl["items"]]
            sections.append("## enum `%s : %s`\n\n%s" % (decl["name"], decl["base"], "\n".join(rows)))
        else:
            rows = ["| 顺序 | 字段 | 类型 | 长度 |", "| ---: | --- | --- | --- |"]
            for index, field in enumerate(decl["fields"], 1):
                if field["kind"] == "bytes":
                    length = field["length"]
                    size = str(length["value"]) if length["kind"] == "fixed" else "字段 `%s`" % length["value"]
                    type_name = "bytes"
                else:
                    type_name = field["type"]
                    size = "%d 字节" % INTEGER_TYPES[type_name][0] if type_name in INTEGER_TYPES else "嵌套"
                rows.append("| %d | `%s` | `%s` | %s |" % (index, field["name"], type_name, size))
            sections.append("## packet `%s`\n\n%s" % (decl["name"], "\n".join(rows)))
    return "\n\n".join(sections)


def _mermaid(schema):
    lines = ["classDiagram"]
    for decl in schema["declarations"]:
        lines.append("    class %s {" % decl["name"])
        if decl["kind"] == "enum":
            lines.append("        <<enumeration>>")
            lines += ["        %s = %d" % (item["name"], item["value"]) for item in decl["items"]]
        else:
            for field in decl["fields"]:
                type_name = "bytes" if field["kind"] == "bytes" else field["type"]
                lines.append("        %s %s" % (type_name, field["name"]))
        lines.append("    }")
    for decl in schema["declarations"]:
        if decl["kind"] != "packet":
            continue
        for field in decl["fields"]:
            if field["kind"] == "value" and field["type"] in schema["types"]:
                lines.append("    %s --> %s : %s" % (decl["name"], field["type"], field["name"]))
    return "\n".join(lines)


def _render(template_dir, name, values):
    text = (template_dir / name).read_text(encoding="utf-8")
    return Template(text).substitute(values)


def generate(schema, output_dir, target, template_dir):
    output_dir.mkdir(parents=True, exist_ok=True)
    values = {
        "protocol_name": schema["name"],
        "module_name": schema["module"],
        "namespace": schema["module"],
        "macro_prefix": _upper(schema["name"]),
        "endian": schema["endian"],
        "python_declarations": _python_declarations(schema),
        "cpp_declarations": _cpp_declarations(schema),
        "c_declarations": _c_declarations(schema),
        "rust_declarations": _rust_declarations(schema),
        "doc_tables": _doc_tables(schema),
        "mermaid": _mermaid(schema),
    }
    files = []
    specs = []
    if target in ("all", "python"):
        specs.append((schema["module"] + "_protocol.py", "python_module.py.tmpl"))
    if target in ("all", "cpp17"):
        specs.append((schema["module"] + "_protocol.hpp", "cpp17_header.hpp.tmpl"))
    if target in ("all", "c99"):
        specs.append((schema["module"] + "_protocol.h", "c99_header.h.tmpl"))
    if target in ("all", "rust"):
        specs.append((schema["module"] + "_protocol.rs", "rust_module.rs.tmpl"))
    if target in ("all", "docs"):
        specs += [("PROTOCOL.md", "protocol.md.tmpl"), ("protocol.mmd", "protocol.mmd.tmpl")]
    for output_name, template_name in specs:
        path = output_dir / output_name
        path.write_text(_render(template_dir, template_name, values), encoding="utf-8", newline="\n")
        files.append(path)
    return files
