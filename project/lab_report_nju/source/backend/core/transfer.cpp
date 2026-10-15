// 四元式读取 / 校验 / 目标代码生成 / 模拟执行。
//
// 机器与类型模型（细节见 types.hpp）：
//   * 变量类型：i8/u8/i16/u16/i32/u32/i64/u64/i128/u128/f32/f64；
//   * 机器原生字长 word_bits ∈ {8,16,32,64}，记类型宽度 W、字长 M：
//       W == M  单条原生指令（i16 省略后缀，保持旧输出）；
//       W <  M  单条指令 + 按类型归一化（模拟器用掩码/算术移位合成，
//               真实后端可展开成 AND / SLL+SRA 序列）；
//       W >  M  多字展开：变量按 M 位分片（x#0 是低字），加减用
//               不进位/进位（ADD/ADDC、SUB/SUBB）链，按位逐字，
//               乘除等交给运行时例程（RCALL / RCALLW）。
//   * 立即数放不进机器字长时按字拆开，逐字写入分片。
//   * 取内存分位数：按 `LD.<type>` / `ST.<type>`（可被配置覆盖）单次访问，
//     宽类型按机器字长拆成多次访问。
//   * 乘法和除法使用**固定的命名寄存器对**：IR 里写 `HI` / `LO`，
//     生成时映射到目标寄存器对（MIPS HI:LO、8086 DX:AX、ARM regHi:regLo）。
#include "transfer.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <stdexcept>

namespace transfer {
namespace {

[[noreturn]] void error(const std::string& message) { throw std::runtime_error(message); }

std::string trim(const std::string& text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}

/** 变量名；寄存器名（$t0、AX、r12、HI…）也按同样的名字语法书写。 */
bool identifier(const std::string& text) {
    if (text.empty() || text == "_") return false;
    const unsigned char first = static_cast<unsigned char>(text[0]);
    if (!(std::isalpha(first) || text[0] == '_' || text[0] == '$')) return false;
    for (unsigned char c : text) {
        if (!(std::isalnum(c) || c == '_' || c == '.' || c == '$')) return false;
    }
    return true;
}

bool valueOperand(const std::string& text) {
    return text == "_" ? false
                       : (isNumericLiteral(text) || isStringLiteral(text) || identifier(text));
}

std::string replaceAll(std::string text, const std::string& key, const std::string& value) {
    for (std::size_t pos = text.find(key); pos != std::string::npos;
         pos = text.find(key, pos + value.size())) {
        text.replace(pos, key.size(), value);
    }
    return text;
}

/** 逗号/空白分词；双引号内的文本保持完整（print 用），引号本身保留。 */
std::vector<std::string> words(const std::string& text) {
    std::vector<std::string> out;
    std::string current;
    bool quoted = false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (quoted) {
            current.push_back(c);
            if (c == '\\' && i + 1 < text.size()) current.push_back(text[++i]);
            else if (c == '"') quoted = false;
        } else if (c == '"') {
            quoted = true;
            current.push_back(c);
        } else if (c == ',' || std::isspace(static_cast<unsigned char>(c))) {
            if (!current.empty()) out.push_back(current);
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    if (!current.empty()) out.push_back(current);
    return out;
}

std::string stripQuotes(const std::string& text) {
    return text.size() >= 2 && text.front() == '"' && text.back() == '"'
               ? text.substr(1, text.size() - 2)
               : text;
}

std::string unescapeText(const std::string& text) {
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\\' && i + 1 < text.size()) {
            const char next = text[++i];
            if (next == 'n') out.push_back('\n');
            else if (next == 't') out.push_back('\t');
            else if (next == 'r') out.push_back('\r');
            else out.push_back(next);
        } else {
            out.push_back(text[i]);
        }
    }
    return out;
}

std::string escapeText(const std::string& text) {
    std::string out;
    for (char c : text) {
        if (c == '\n') out += "\\n";
        else if (c == '\t') out += "\\t";
        else if (c == '\r') out += "\\r";
        else if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else out.push_back(c);
    }
    return out;
}

const std::set<std::string> kBinary{ "+", "-",  "*",  "/", "%",  "==", "!=", "<",
                                     "<=", ">", ">=", "<<", ">>", "&", "|", "^" };
const std::set<std::string> kCarry{ "+c", "-b" };
const std::set<std::string> kCompare{ "==", "!=", "<", "<=", ">", ">=" };
const std::set<std::string> kUnary{ "neg", "odd", "~", "cvt" };
const std::set<std::string> kPair{ "mul", "div" };
/** 字符串指令：复制 / 长度 / 比较 / 拼接 / 取子串（地址+长度）/ 查找。 */
const std::set<std::string> kStringOps{ "scopy", "slen", "scmp", "scat", "ssub", "sfind" };
/** 流指令：取得流句柄 / 读 / 写 / 关闭（句柄就是流名，如 STDIN、STDOUT）。 */
const std::set<std::string> kStreamOps{ "stream", "sread", "swrite", "sclose" };
/** 向量指令：元素位数由类型后缀给出，操作数是"地址 + 下标或长度"。 */
const std::set<std::string> kVectorOps{ "vload", "vstore", "vcopy", "vfill", "vsum", "vmax", "vmin" };
/** 文件指令：真实打开 / 读写 / 关闭外部文件（路径相对当前工作目录）。 */
const std::set<std::string> kFileOps{ "fopen", "fread", "fwrite", "fclose", "feof" };

bool binaryBase(const std::string& base) {
    return kBinary.count(base) != 0 || kCarry.count(base) != 0 || kPair.count(base) != 0;
}
bool expressionBase(const std::string& base) {
    return binaryBase(base) || kUnary.count(base) != 0;
}
bool pairBase(const std::string& base) { return kPair.count(base) != 0; }
bool compareBase(const std::string& base) { return kCompare.count(base) != 0; }
bool carryBase(const std::string& base) { return kCarry.count(base) != 0; }

/** 机器上的固定寄存器名（乘除法结果对与常用工作寄存器）。 */
const std::set<std::string> kRegisterNames{
    "AX", "BX", "CX", "DX", "AL", "AH", "BL", "BH", "CL", "CH", "DL", "DH",
    "SI", "DI", "SP", "BP", "IP", "HI", "LO", "ZR"};

bool registerName(const std::string& name) {
    if (name.empty()) return false;
    if (name[0] == '$') return true;
    if (kRegisterNames.count(name) != 0) return true;
    if ((name[0] == 'r' || name[0] == 'R') && name.size() > 1 &&
        name.find_first_not_of("0123456789", 1) == std::string::npos) {
        return true;
    }
    return false;
}

std::map<std::string, std::string> declaredTypesOf(const Program& program) {
    std::map<std::string, std::string> out;
    for (const auto& quad : program) {
        if (quad.op == "decl") out[quad.result] = typeInfo(quad.a).name;
    }
    return out;
}

/**
 * 解析字面量：无符号类型遇到负号时，先按同宽有符号类型解析再转换，
 * 这样 `-1` 在 u32 上下文里就是 0xFFFFFFFF，而不是直接报错。
 */
Value literalFor(const std::string& text, const std::string& type) {
    try {
        return literalValue(text, type);
    } catch (const std::exception&) {
        if (isUnsignedType(type)) {
            const std::string signedType = "i" + std::to_string(typeBits(type));
            if (isTypeName(signedType)) return convertValue(literalValue(text, signedType), type);
        }
        throw;
    }
}

std::string typeOfName(const std::map<std::string, std::string>& declared,
                       const std::string& name) {
    const auto found = declared.find(name);
    return found == declared.end() ? std::string(defaultTypeName()) : found->second;
}

std::string operationTypeOf(const Quad& quad, const std::map<std::string, std::string>& declared) {
    const std::string suffix = opType(quad.op);
    if (!suffix.empty()) return typeInfo(suffix).name;
    const auto result = declared.find(quad.result);
    if (result != declared.end()) return result->second;
    const auto first = declared.find(quad.a);
    if (first != declared.end()) return first->second;
    const auto second = declared.find(quad.b);
    if (second != declared.end()) return second->second;
    return defaultTypeName();
}

std::string resultTypeOf(const Quad& quad, const std::map<std::string, std::string>& declared) {
    const auto result = declared.find(quad.result);
    if (result != declared.end()) return result->second;
    const std::string base = opBase(quad.op);
    if (compareBase(base) || base == "odd") return "i8";
    return operationTypeOf(quad, declared);
}

} // namespace

Target target(const std::string& text) {
    if (text == "mips") return Target::Mips;
    if (text == "8086") return Target::I8086;
    if (text == "arm") return Target::Arm;
    error("Unknown target: " + text);
}

const char* name(Target value) {
    switch (value) {
    case Target::Mips: return "mips";
    case Target::I8086: return "8086";
    case Target::Arm: return "arm";
    }
    error("Invalid target");
}

std::map<std::string, std::string> variableTypes(const Program& program) {
    return declaredTypesOf(program);
}

std::string quadType(const Quad& quad, const std::map<std::string, std::string>& declared) {
    return operationTypeOf(quad, declared);
}

Program read(std::istream& input) {
    Program out;
    std::size_t line = 0;
    for (std::string text; std::getline(input, text);) {
        ++line;
        // 兼容 Windows 编辑器写出的 UTF-8 BOM。
        if (line == 1 && text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
            static_cast<unsigned char>(text[1]) == 0xBB &&
            static_cast<unsigned char>(text[2]) == 0xBF) {
            text.erase(0, 3);
        }
        text = trim(text.substr(0, text.find('#')));
        if (text.empty()) continue;
        const auto open = text.find('(');
        if (open != std::string::npos) {
            const auto prefix = trim(text.substr(0, open));
            if (!prefix.empty() &&
                (prefix.back() != ':' || !isIntegerLiteral(trim(prefix.substr(0, prefix.size() - 1)))))
                error("Invalid quadruple index at line " + std::to_string(line));
            if (text.back() != ')') error("Missing ')' at line " + std::to_string(line));
            text = text.substr(open + 1, text.size() - open - 2);
        }
        auto fields = words(text);
        if (fields.size() != 4 && fields.size() != 5)
            error("Expected four or five fields at line " + std::to_string(line));
        Quad quad{fields[0], fields[1], fields[2], fields[3],
                  fields.size() == 5 ? fields[4] : "_"};
        if (quad.op == "=") quad.op = "==";
        if (quad.op == "#" || quad.op == "<>") quad.op = "!=";
        if (quad.op == "print") quad.a = stripQuotes(quad.a);
        out.push_back(quad);
    }
    if (input.bad()) error("Cannot read quadruples");
    validate(out);
    return out;
}

std::string write(const Program& program) {
    std::ostringstream out;
    for (std::size_t i = 0; i < program.size(); ++i) {
        const auto& quad = program[i];
        out << i << ": (" << quad.op << ", ";
        out << (quad.op == "print" ? "\"" + escapeText(quad.a) + "\"" : quad.a) << ", ";
        out << quad.b << ", " << quad.result;
        if (!quad.extra.empty() && quad.extra != "_") out << ", " << quad.extra;
        out << ")\n";
    }
    return out.str();
}

void validate(const Program& program) {
    std::set<std::string> labels;
    std::set<std::string> declared;
    std::map<std::string, std::string> declarations;
    for (std::size_t i = 0; i < program.size(); ++i) {
        const auto& quad = program[i];
        const std::string base = opBase(quad.op);
        const auto fail = [&](const std::string& what) {
            error("Invalid quadruple " + std::to_string(i) + " (" + quad.op + "): " + what);
        };
        if (base == "decl") {
            if (!isTypeName(quad.a)) fail("expected a type name");
            if (quad.b != "_" && quad.b != "global" && quad.b != "local")
                fail("scope must be global / local / _");
            if (!identifier(quad.result)) fail("expected a variable name");
            if (!declared.insert(quad.result).second && declarations[quad.result] != typeInfo(quad.a).name)
                fail("declared twice with different types");
            declarations[quad.result] = typeInfo(quad.a).name;
            continue;
        }
        const std::string suffix = opType(quad.op);
        if (!suffix.empty() && !isTypeName(suffix)) fail("unknown type '" + suffix + "'");
        if (base == "label" || base == "j") {
            if (quad.a != "_" || quad.b != "_" || !identifier(quad.result))
                fail("expected (_, _, label)");
            if (base == "label" && !labels.insert(quad.result).second) fail("duplicate label");
        } else if (base == "jz" || base == "jnz") {
            if (!valueOperand(quad.a) || quad.b != "_" || !identifier(quad.result))
                fail("expected (value, _, label)");
        } else if (base == "read" || base == "getc") {
            if (quad.a != "_" || quad.b != "_" || !identifier(quad.result))
                fail("expected (_, _, variable)");
        } else if (base == "write" || base == "putc") {
            if (!valueOperand(quad.a) || quad.b != "_" || quad.result != "_")
                fail("expected (value, _, _)");
        } else if (base == "print") {
            if (quad.a == "_" || quad.b != "_" || quad.result != "_") fail("expected (text, _, _)");
        } else if (base == "addr" || base == "load" || base == "store") {
            // 数据区三件套（语义与 library/stackvm 的 AddrOf / LoadAt / StoreAt 相同）：
            //   (addr, 符号, _, 目标)  把数据区里该符号的基址写进目标变量
            //   (load, 指针, _, 目标)  按指针读 8 字节（小端）
            //   (store, 指针, 值, _)   按指针写 8 字节（小端）
            // `addr` 的第一个操作数是数据区符号名（不是变量），所以它不参与变量类型推断；
            // 地址与取值都按 8 字节句柄（i64）处理，具体指令由三套后端生成、由模拟器执行。
            if (base == "addr") {
                if (!identifier(quad.a) || quad.b != "_" || !identifier(quad.result))
                    fail("expected (symbol, _, destination)");
            } else if (base == "load") {
                if (!valueOperand(quad.a) || quad.b != "_" || !identifier(quad.result))
                    fail("expected (pointer, _, destination)");
            } else if (!valueOperand(quad.a) || !valueOperand(quad.b) || quad.result != "_") {
                fail("expected (pointer, value, _)");
            }
        } else if (base == "param") {
            if (!valueOperand(quad.a) || quad.b != "_" || !identifier(quad.result))
                fail("expected (value, _, formal)");
        } else if (base == "call" || base == "int") {
            if (!identifier(quad.a) || quad.b != "_") fail("expected (label, _, destination)");
            if (quad.result != "_" && !identifier(quad.result)) fail("expected a destination");
        } else if (base == "ret") {
            if (quad.a != "_" && !valueOperand(quad.a)) fail("expected (value|_, _, _)");
            if (quad.b != "_" || quad.result != "_") fail("expected (value, _, _)");
        } else if (base == "iret" || base == "halt") {
            if (quad.a != "_" || quad.b != "_" || quad.result != "_") fail("expected (_, _, _)");
        } else if (base == ":=") {
            if (!valueOperand(quad.a) || quad.b != "_" || !identifier(quad.result))
                fail("expected (value, _, destination)");
        } else if (pairBase(base)) {
            if (!valueOperand(quad.a) || !valueOperand(quad.b) || !identifier(quad.result))
                fail("expected (left, right, low/quotient)");
            if (!identifier(quad.extra)) fail("expected a second destination (HI / remainder)");
            if (quad.extra != "_" && quad.extra == quad.result) fail("two destinations must differ");
        } else if (kUnary.count(base) != 0) {
            if (!valueOperand(quad.a) || quad.b != "_" || !identifier(quad.result))
                fail("expected (value, _, destination)");
            if (base == "cvt" && suffix.empty()) fail("cvt needs a target type, e.g. cvt:f64");
        } else if (binaryBase(base)) {
            if (!valueOperand(quad.a) || !valueOperand(quad.b) || !identifier(quad.result))
                fail("expected (left, right, destination)");
        } else if (kStringOps.count(base) != 0) {
            // scopy/slen 一元；scmp/scat/sfind 二元；ssub 是 (源, 起点, 目标, 长度)。
            if (base == "scopy" || base == "slen") {
                if (!valueOperand(quad.a) || quad.b != "_" || !identifier(quad.result))
                    fail("expected (value, _, destination)");
            } else if (base == "ssub") {
                if (!valueOperand(quad.a) || !valueOperand(quad.b) || !identifier(quad.result) ||
                    !valueOperand(quad.extra))
                    fail("expected (source, start, destination, length)");
            } else if (!valueOperand(quad.a) || !valueOperand(quad.b) || !identifier(quad.result)) {
                fail("expected (left, right, destination)");
            }
        } else if (kStreamOps.count(base) != 0) {
            if (base == "stream" || base == "sread") {
                if (!valueOperand(quad.a) || quad.b != "_" || !identifier(quad.result))
                    fail("expected (stream, _, destination)");
            } else if (base == "swrite") {
                if (!valueOperand(quad.a) || !valueOperand(quad.b) || quad.result != "_")
                    fail("expected (stream, value, _)");
            } else if (!valueOperand(quad.a) || quad.b != "_" || quad.result != "_") {
                fail("expected (stream, _, _)");
            }
        } else if (kVectorOps.count(base) != 0) {
            if (base == "vload" || base == "vsum" || base == "vmax" || base == "vmin") {
                if (!identifier(quad.a) || !valueOperand(quad.b) || !identifier(quad.result))
                    fail("expected (address, index_or_length, destination)");
            } else if (base == "vcopy") {
                if (!identifier(quad.a) || !valueOperand(quad.b) || !identifier(quad.result))
                    fail("expected (source, length, destination)");
            } else if (!identifier(quad.a) || !valueOperand(quad.b) || !valueOperand(quad.result)) {
                fail("expected (address, index_or_length, value)");
            }
        } else if (kFileOps.count(base) != 0) {
            if (base == "fopen") {
                if (!valueOperand(quad.a) || !valueOperand(quad.b) || !identifier(quad.result))
                    fail("expected (path, mode, handle)");
            } else if (base == "fread" || base == "feof") {
                if (!valueOperand(quad.a) || quad.b != "_" || !identifier(quad.result))
                    fail("expected (handle, _, destination)");
            } else if (base == "fwrite") {
                if (!valueOperand(quad.a) || !valueOperand(quad.b) || quad.result != "_")
                    fail("expected (handle, value, _)");
            } else if (!valueOperand(quad.a) || quad.b != "_" || quad.result != "_") {
                fail("expected (handle, _, _)");
            }
        } else if (base == "system") {
            // system 调用：(system, 调用名, 可选参数, 可选结果变量)。
            if (!identifier(quad.a) || (quad.b != "_" && !valueOperand(quad.b)) ||
                (quad.result != "_" && !identifier(quad.result)))
                fail("expected (call, argument|_, destination|_)");
        } else {
            // 情况 2：四元式本身不存在（与"机器没有该指令"区分开）。
            error("第 " + std::to_string(i) + " 条四元式的操作 '" + quad.op +
                  "' 不存在（未知四元式）");
        }
    }
    for (const auto& quad : program) {
        const std::string base = opBase(quad.op);
        if ((base == "j" || base == "jz" || base == "jnz" || base == "call" || base == "int") &&
            !labels.count(base == "call" || base == "int" ? quad.a : quad.result)) {
            error("Undefined label: " +
                  (base == "call" || base == "int" ? quad.a : quad.result));
        }
    }
}

namespace {

/** 填空模板：{dst} {dst2} {lhs} {rhs} {src} {name} {value}。 */
struct Fill {
    std::string dst, dst2, lhs, rhs, src, name, value;
};

std::string substitute(std::string text, const Fill& fill) {
    text = replaceAll(text, "{dst}", fill.dst);
    text = replaceAll(text, "{dst2}", fill.dst2);
    text = replaceAll(text, "{lhs}", fill.lhs);
    text = replaceAll(text, "{rhs}", fill.rhs);
    text = replaceAll(text, "{src}", fill.src);
    text = replaceAll(text, "{name}", fill.name);
    return replaceAll(text, "{value}", fill.value);
}

/**
 * 一条命令可以由多条指令组成：模板里的换行（配置文件里写 `\n`，或者重复
 * 写同一个 `cmd.*` 键追加）会展开成一串目标指令，每行各自替换占位符并加
 * 类型后缀。取值小于机器字长时的掩码 / 归一化、乘除法搬 HI:LO，都靠它表达。
 */
std::vector<std::string> templateLines(const std::string& text) {
    std::vector<std::string> lines;
    std::istringstream in(text);
    for (std::string line; std::getline(in, line);) {
        const auto trimmed = trim(line);
        if (!trimmed.empty()) lines.push_back(trimmed);
    }
    if (lines.empty()) lines.push_back(trim(text));
    return lines;
}

/** 给指令模板的助记符加类型后缀；i16 不加后缀，保持旧产物不变。 */
std::string withType(std::string text, const std::string& type) {
    if (type == "i16" || type.empty()) return text;
    const auto space = text.find_first_of(" \t");
    if (space == std::string::npos) return text + "." + type;
    return text.substr(0, space) + "." + type + text.substr(space);
}

/** 按宽度选择目标机的取内存 / 存内存助记符（可被配置覆盖）。 */
std::string defaultLoad(Target target, const std::string& type) {
    const unsigned bits = typeBits(type);
    const bool unsignedType = isUnsignedType(type);
    if (isFloatType(type))
        return target == Target::I8086 ? "MOV {dst}, [{name}]"
               : target == Target::Arm ? (bits == 32 ? "VLDR {dst}, [{name}]"
                                                     : "VLDRD {dst}, [{name}]")
                                       : (bits == 32 ? "LWC1 {dst}, {name}" : "LDC1 {dst}, {name}");
    if (target == Target::I8086) return "MOV {dst}, [{name}]";
    if (target == Target::Arm) {
        if (bits == 8) return std::string(unsignedType ? "LDRB " : "LDRSB ") + "{dst}, [{name}]";
        if (bits == 16) return std::string(unsignedType ? "LDRH " : "LDRSH ") + "{dst}, [{name}]";
        if (bits == 32) return "LDR {dst}, [{name}]";
        if (bits == 64) return "LDRD {dst}, [{name}]";
        return "LDRQ {dst}, [{name}]";
    }
    if (bits == 8) return std::string(unsignedType ? "LBU " : "LB ") + "{dst}, {name}";
    if (bits == 16) return std::string(unsignedType ? "LHU " : "LH ") + "{dst}, {name}";
    if (bits == 32) return "LW {dst}, {name}";
    if (bits == 64) return "LD {dst}, {name}";
    return "LQ {dst}, {name}";
}

std::string defaultStore(Target target, const std::string& type) {
    const unsigned bits = typeBits(type);
    if (isFloatType(type))
        return target == Target::I8086 ? "MOV [{name}], {src}"
               : target == Target::Arm ? (bits == 32 ? "VSTR {src}, [{name}]"
                                                     : "VSTRD {src}, [{name}]")
                                       : (bits == 32 ? "SWC1 {src}, {name}" : "SDC1 {src}, {name}");
    if (target == Target::I8086) return "MOV [{name}], {src}";
    if (target == Target::Arm) {
        if (bits == 8) return "STRB {src}, [{name}]";
        if (bits == 16) return "STRH {src}, [{name}]";
        if (bits == 32) return "STR {src}, [{name}]";
        if (bits == 64) return "STRD {src}, [{name}]";
        return "STRQ {src}, [{name}]";
    }
    if (bits == 8) return "SB {src}, {name}";
    if (bits == 16) return "SH {src}, {name}";
    if (bits == 32) return "SW {src}, {name}";
    if (bits == 64) return "SD {src}, {name}";
    return "SQ {src}, {name}";
}

} // namespace

std::string generate(const Program& program, const MachineConfig& config) {
    validate(program);
    const unsigned word = config.wordBits;
    if (word != 8 && word != 16 && word != 32 && word != 64)
        error("word_bits must be 8, 16, 32 or 64; got " + std::to_string(word));
    const std::string wordType = "u" + std::to_string(word);
    const Target target = config.target;
    const auto declared = declaredTypesOf(program);
    std::map<std::string, std::string> scopes;
    for (const auto& quad : program)
        if (quad.op == "decl") scopes[quad.result] = quad.b == "local" ? "local" : "global";
    std::map<std::string, std::string> types = declared;
    for (const auto& quad : program) {
        const std::string base = opBase(quad.op);
        if ((expressionBase(base) || base == ":=" || base == "read" || base == "getc" ||
             base == "call" || base == "int") &&
            identifier(quad.result) && !types.count(quad.result)) {
            types[quad.result] = resultTypeOf(quad, declared);
        }
        if (pairBase(base) && identifier(quad.extra) && !types.count(quad.extra))
            types[quad.extra] = operationTypeOf(quad, declared);
        if ((base == "addr" || base == "load") && identifier(quad.result) &&
            !types.count(quad.result)) {
            // 数据区里的基址和取出来的槽位都是 8 字节句柄（i64），
            // 窄机器上按机器字分片存放 —— 和 stackvm 的 8 字节槽位一致。
            types[quad.result] = "i64";
        }
    }
    const auto typeOf = [&](const std::string& name) { return typeOfName(types, name); };
    /** 操作数类型：字面量按运算类型解释，变量看声明。 */
    const auto operandTypeOf = [&](const std::string& operand, const std::string& fallback) {
        return isNumericLiteral(operand) ? fallback : typeOf(operand);
    };
    /** 值操作数的类型：字符串字面量是 str，数字字面量按默认数值类型，其余看声明。 */
    const auto valueOperandType = [&](const std::string& operand) {
        if (isStringLiteral(operand)) return std::string("str");
        if (isNumericLiteral(operand)) return std::string(defaultTypeName());
        return typeOf(operand);
    };
    const auto wideType = [&](const std::string& type) {
        return !isFloatType(type) && typeBits(type) > word;
    };
    const auto wordCount = [&](const std::string& type) {
        return (typeBits(type) + word - 1) / word;
    };

    // IR 里的固定寄存器名 -> 目标机实际寄存器名。
    std::map<std::string, std::string> registers{{"HI", config.regHi}, {"LO", config.regLo}};
    registers[config.regA] = config.regA;
    registers[config.regB] = config.regB;
    registers[config.regC] = config.regC;
    registers[config.regHi] = config.regHi;
    registers[config.regLo] = config.regLo;
    const auto mapRegister = [&](const std::string& operand) {
        const auto found = registers.find(operand);
        return found == registers.end() ? operand : found->second;
    };
    const auto isRegister = [&](const std::string& operand) {
        return registers.count(operand) != 0 || registerName(operand);
    };

    std::ostringstream out;
    out << ".target " << name(target) << "\n.bits " << word << "\n";
    // 乘法/除法的固定结果寄存器对（高在前、低在后），模拟器据此解释
    // MULT/DIV、MUL/IMUL 等"结果进固定寄存器"的指令。
    out << ".mulpair " << (config.regHi.empty() ? "HI" : config.regHi) << " "
        << (config.regLo.empty() ? "LO" : config.regLo) << "\n";
    for (const auto& reg : {config.regA, config.regB, config.regC, config.regHi, config.regLo}) {
        if (!reg.empty()) out << ".reg " << reg << "\n";
    }

    // ---- .data：字节镜像、初值、声明（宽类型按机器字分片） ----
    out << ".data\n";
    std::set<std::string> emitted;
    // 字节镜像：程序**声明过**的字节按老写法逐字节给初值（类型跟着声明走），
    // 其余字节用紧凑的 `.hex 起始偏移 十六进制串` 写出（16 字节一行）。
    {
        const auto flush = [&](std::string& chunk, std::size_t start) {
            if (chunk.empty()) return;
            out << "    .hex " << start << " " << chunk << "\n";
            chunk.clear();
        };
        std::string chunk;
        std::size_t chunkStart = 0;
        for (std::size_t i = 0; i < config.dataImage.size(); ++i) {
            const std::string name = "byte_" + std::to_string(i);
            if (types.count(name) != 0) {                       // 声明过的：下面单独写
                flush(chunk, chunkStart);
                continue;
            }
            if (chunk.empty()) chunkStart = i;
            const unsigned byte = config.dataImage[i];
            const char* digits = "0123456789abcdef";
            chunk.push_back(digits[(byte >> 4) & 0xF]);
            chunk.push_back(digits[byte & 0xF]);
            if (chunk.size() >= 32) flush(chunk, chunkStart);
        }
        flush(chunk, chunkStart);
        for (std::size_t i = 0; i < config.dataImage.size(); ++i) {
            const std::string name = "byte_" + std::to_string(i);
            const auto found = types.find(name);
            if (found == types.end()) continue;
            out << "    " << name << " = " << static_cast<unsigned>(config.dataImage[i]);
            if (found->second != "i16") out << " : " << found->second;
            out << "\n";
            emitted.insert(name);
        }
    }
    const auto declaration = [&](const std::string& symbol, const std::string& type,
                                 const std::string& initial, bool hasInitial,
                                 const std::string& scope) {
        out << "    " << (scope == "local" ? "local " : "") << symbol;
        if (hasInitial) out << " = " << initial;
        if (type != "i16") out << " : " << type;
        out << "\n";
    };
    for (const auto& item : config.data) {
        const std::string type = typeOf(item.first);
        if (wideType(type)) {
            const Value value = literalFor(item.second, type);
            for (unsigned i = 0; i < wordCount(type); ++i) {
                declaration(item.first + "#" + std::to_string(i), wordType,
                            u128ToString(u128Mask(u128Shr(value.bits, i * word), word)), true,
                            "global");
            }
        } else {
            declaration(item.first, type, item.second, true,
                        scopes.count(item.first) ? scopes[item.first] : "global");
            emitted.insert(item.first);
        }
    }
    for (const auto& item : types) {
        if (emitted.count(item.first)) continue;
        // 宽类型也要声明：分片按声明的作用域与宽度拆装。
        declaration(item.first, item.second, "", false,
                    scopes.count(item.first) ? scopes[item.first] : "global");
    }
    out << ".enddata\n.rodata\n";
    for (const auto& item : config.readOnlyData) {
        const std::string type = typeOf(item.first);
        out << "    " << item.first << " = " << item.second
            << (type == "i16" ? "" : " : " + type) << "\n";
    }
    out << ".endrodata\n";

    // ---- .dataarea：数据区字节镜像 + 符号基址（addr / load / store 用） ----
    // 与 stackvm 的 DataArea 同一份约定：符号表给基址，读写都是 8 字节小端，
    // 地址就是这里的字节偏移。字节用 `.hex 起始偏移 十六进制串` 紧凑写出（16 字节一行）。
    {
        const char* digits = "0123456789abcdef";
        const auto hexRange = [&](std::size_t from, std::size_t to) {
            std::string text;
            for (std::size_t i = from; i < to; ++i) {
                text.push_back(digits[(config.dataAreaBytes[i] >> 4) & 0xF]);
                text.push_back(digits[config.dataAreaBytes[i] & 0xF]);
            }
            return text;
        };
        out << ".dataarea " << config.dataAreaBytes.size() << "\n";
        for (const auto& item : config.dataAreaSymbols)
            out << "    .sym " << item.first << " " << item.second << "\n";
        for (std::size_t offset = 0; offset < config.dataAreaBytes.size(); offset += 16) {
            out << "    .hex " << offset << " "
                << hexRange(offset, std::min(offset + 16, config.dataAreaBytes.size())) << "\n";
        }
        out << ".enddataarea\n";
    }

    out << ".stack " << config.stackWords << "\n.code\n";

    const bool x86 = target == Target::I8086;
    const std::string& regA = config.regA;
    const std::string& regB = config.regB;
    const std::string& regC = config.regC;
    const auto emit = [&](const std::string& text) { out << "    " << text << '\n'; };
    const auto command = [&](const std::string& key, const std::string& fallback) {
        const auto found = config.commands.find(key);
        return found == config.commands.end() ? fallback : found->second;
    };
    /** 展开一条命令模板：可能是多条目标指令（见模板里的换行）。 */
    const auto emitTemplate = [&](const std::string& tmpl, const Fill& fill,
                                  const std::string& type) {
        for (const auto& line : templateLines(tmpl)) emit(withType(substitute(line, fill), type));
    };
    /**
     * 机器能力检查：四元式本身合法、但这台机器没有实现对应指令族时报错。
     * 与"未知四元式"（validate 报的操作不存在）区分开。
     */
    const auto requireFeature = [&](const std::string& feature, std::size_t index,
                                    const Quad& quad) {
        if (config.supports(feature)) return;
        error("四元式 " + std::to_string(index) + " (" + quad.op + ") 存在，但目标机 " +
              name(config.target) + " 没有实现指令族 '" + feature +
              "'（配置里可用 feature." + feature + "=on 打开）");
    };
    const auto loadTemplate = [&](const std::string& type) {
        return type == "i16" ? config.memoryLoad : command("load." + type, defaultLoad(target, type));
    };
    const auto storeTemplate = [&](const std::string& type) {
        return type == "i16" ? config.memoryStore
                             : command("store." + type, defaultStore(target, type));
    };
    /** 装载操作数到寄存器；寄存器名直接搬运，字面量用立即数模板。 */
    const auto loadTo = [&](const std::string& reg, const std::string& operand,
                            const std::string& type) {
        const std::string resolved = mapRegister(operand);
        if (isRegister(operand)) {
            if (resolved != reg) emit(withType("MOV " + reg + ", " + resolved, "i" + std::to_string(word)));
        } else if (isNumericLiteral(operand)) {
            emitTemplate(config.immediateLoad, Fill{reg, "", "", "", "", "", operand}, type);
        } else {
            // 宽变量按机器字分片存放（实际槽位是 x#0、x#1…）：窄类型读一个宽变量时
            // 取最低那片，就是"截断到 type"的结果；直接读名字在模拟器里没有定义。
            const std::string name =
                (!wideType(type) && wideType(typeOf(operand))) ? operand + "#0" : operand;
            emitTemplate(loadTemplate(type), Fill{reg, "", "", "", "", name, ""}, type);
        }
    };
    /** 把寄存器写回 IR 目的地（变量写内存，寄存器名直接搬运）。 */
    const auto storeTo = [&](const std::string& destination, const std::string& reg,
                             const std::string& type) {
        if (isRegister(destination)) {
            const std::string resolved = mapRegister(destination);
            if (resolved != reg) emit(withType("MOV " + resolved + ", " + reg, "i" + std::to_string(word)));
            return;
        }
        emitTemplate(storeTemplate(type), Fill{"", "", "", "", reg, destination, ""}, type);
    };
    /** 宽类型分片访问：每次搬一个机器字。 */
    const auto loadWord = [&](const std::string& reg, const std::string& variable, unsigned index) {
        emit(withType(substitute(command("load." + wordType, defaultLoad(target, wordType)),
                                 Fill{reg, "", "", "", "", variable + "#" + std::to_string(index), ""}),
                      wordType));
    };
    /** 宽运算的操作数取一个机器字：变量取分片，字面量按字拆开现装。 */
    const auto loadWordOperand = [&](const std::string& reg, const std::string& operand,
                                     const std::string& type, unsigned index) {
        if (!isNumericLiteral(operand)) { loadWord(reg, operand, index); return; }
        const Value value = literalFor(operand, type);
        const std::string part =
            u128ToString(u128Mask(u128Shr(value.bits, index * word), word));
        emitTemplate(config.immediateLoad, Fill{reg, "", "", "", "", "", part}, wordType);
    };
    const auto storeWord = [&](const std::string& variable, unsigned index,
                               const std::string& reg) {
        emit(withType(substitute(command("store." + wordType, defaultStore(target, wordType)),
                            Fill{"", "", "", "", reg, variable + "#" + std::to_string(index), ""}),
                      wordType));
    };
    /** 立即数：放得进机器字长直接装，放不下逐字分片写。 */
    const auto storeImmediate = [&](const std::string& destination, const std::string& literal,
                                    const std::string& valueType,
                                    const std::string& targetType) {
        if (!wideType(valueType) && !wideType(targetType) && !isRegister(destination)) {
            emitTemplate(config.immediateLoad, Fill{regA, "", "", "", "", "", literal}, valueType);
            emitTemplate(storeTemplate(targetType), Fill{"", "", "", "", regA, destination, ""}, targetType);
            return;
        }
        if (!wideType(valueType) && !wideType(targetType)) { // 目标是寄存器
            emitTemplate(config.immediateLoad,
                         Fill{mapRegister(destination), "", "", "", "", "", literal}, valueType);
            return;
        }
        // 宽类型：立即数凑不够机器字长，按目标类型逐字拆开写入分片。
        const Value value = convertValue(literalFor(literal, valueType), targetType);
        for (unsigned i = 0; i < wordCount(targetType); ++i) {
            const std::string part = u128ToString(u128Mask(u128Shr(value.bits, i * word), word));
            emitTemplate(config.immediateLoad, Fill{regA, "", "", "", "", "", part}, wordType);
            storeWord(destination, i, regA);
        }
    };

    std::size_t serial = 0;
    const std::map<std::string, std::string> conditionName{
        {"==", "EQ"}, {"!=", "NE"}, {"<", "LT"}, {"<=", "LE"}, {">", "GT"}, {">=", "GE"}};
    const std::map<std::string, std::string> x86Condition{
        {"EQ", "E"}, {"NE", "NE"}, {"LT", "L"}, {"LE", "LE"}, {"GT", "G"}, {"GE", "GE"}};
    const auto branchTo = [&](const std::string& condition, const std::string& label) {
        emit(x86 ? "J" + x86Condition.at(condition) + " " + label
                 : "B" + condition + " " + label);
    };

    for (std::size_t quadIndex = 0; quadIndex < program.size(); ++quadIndex) {
        const auto& quad = program[quadIndex];
        const std::string base = opBase(quad.op);
        if (base == "decl") continue;
        if (base == "label") { out << "L_" << quad.result << ":\n"; continue; }
        if (base == "j") {
            emit(std::string(x86 ? "JMP " : target == Target::Mips ? "J " : "B ") + "L_" + quad.result);
            continue;
        }
        if (base == "halt") { emit("HALT"); continue; }
        if (base == "param") {
            // 形参类型由被调用者声明的名字决定，值按形参类型传给被调用帧。
            const std::string formalType = typeOf(quad.result);
            if (wideType(formalType)) {
                emit("PUSHW." + formalType + " " + quad.a + ", " + quad.result);
            } else {
                loadTo(regA, quad.a, formalType);
                emit("PUSH." + formalType + " " + regA + ", " + quad.result);
            }
            continue;
        }
        if (base == "call" || base == "int") {
            emit(std::string(base == "call" ? "CALL " : "INT ") + quad.a + ", " +
                 (quad.result == "_" ? std::string("_") : quad.result));
            continue;
        }
        if (base == "ret") {
            if (quad.a == "_") { emit("RET"); continue; }
            const std::string type = typeOf(quad.a);
            if (wideType(type)) emit("RETW." + type + " " + quad.a);
            else { loadTo(regA, quad.a, type); emit("RET " + regA); }
            continue;
        }
        if (base == "iret") { emit("IRET"); continue; }
        if (base == "print") { emit("PRINT \"" + escapeText(quad.a) + "\""); continue; }
        if (base == "putc") {
            loadTo(regA, quad.a, operandTypeOf(quad.a, operationTypeOf(quad, declared)));
            emit("PUTC " + regA);
            continue;
        }
        if (base == "getc") {
            const std::string type = typeOf(quad.result);
            if (wideType(type)) emit("GETCW." + type + " " + quad.result);
            else { emit(withType("GETC " + regA, type)); storeTo(quad.result, regA, type); }
            continue;
        }
        if (base == "read") {
            const std::string type = typeOf(quad.result);
            if (wideType(type)) emit("READW." + type + " " + quad.result);
            else { emit(withType("IN " + regA, type)); storeTo(quad.result, regA, type); }
            continue;
        }
        if (base == "write") {
            const std::string type = operandTypeOf(quad.a, operationTypeOf(quad, declared));
            if (isStringType(type)) emit("SWRITE.str STDOUT, " + quad.a);
            else if (wideType(type)) emit("OUTW." + type + " " + quad.a);
            else { loadTo(regA, quad.a, type); emit(withType("OUT " + regA, type)); }
            continue;
        }
        if (kStringOps.count(base) != 0) {
            // 字符串指令是运行时伪指令（机器没有字符串部件）。
            if (!config.supports("string")) {
                // 没有字符串指令时，常量字符串仍然可以在生成期算好，
                // 退化成"一条常量装载 + 一条存储"（数值结果退化成常量赋值）。
                const auto constant = [&](const std::string& token, Value& out) {
                    if (!isStringLiteral(token)) return false;
                    out.type = "str";
                    out.text = stringLiteralText(token);
                    return true;
                };
                Value left, right;
                const bool leftOk = constant(quad.a, left);
                const bool rightOk = (base == "scopy" || base == "slen") ? true
                                    : constant(quad.b, right);
                if (!leftOk || !rightOk) {
                    error("四元式 " + std::to_string(quadIndex) + " (" + quad.op +
                          ") 需要字符串指令，但目标机 " + name(config.target) +
                          " 没有实现；只有常量字符串能退化成低级指令");
                }
                if (base == "slen") {
                    storeImmediate(quad.result, std::to_string(left.text.size()),
                                   typeOf(quad.result), typeOf(quad.result));
                } else if (base == "scmp") {
                    const int order = left.text < right.text ? -1 : left.text > right.text ? 1 : 0;
                    storeImmediate(quad.result, std::to_string(order), typeOf(quad.result),
                                   typeOf(quad.result));
                } else if (base == "sfind") {
                    const auto position = left.text.find(right.text);
                    const long long index =
                        position == std::string::npos ? -1 : static_cast<long long>(position);
                    storeImmediate(quad.result, std::to_string(index), typeOf(quad.result),
                                   typeOf(quad.result));
                } else {
                    // scopy / scat / ssub：结果仍是字符串，装卸一个字符串常量。
                    std::string text = left.text;
                    if (base == "scat") text += right.text;
                    else if (base == "ssub") {
                        long long start = 0, length = 0;
                        if (!isIntegerLiteral(quad.b) || !isIntegerLiteral(quad.extra)) {
                            error("四元式 " + std::to_string(quadIndex) +
                                  " (ssub) 的起点与长度必须是常量才能退化成低级指令");
                        }
                        start = std::stoll(quad.b);
                        length = std::stoll(quad.extra);
                        text = substring(left, static_cast<int>(start), static_cast<int>(length)).text;
                    }
                    Value literal;
                    literal.type = "str";
                    literal.text = text;
                    // 用"字符串常量装载"这一条低级指令写进目标变量。
                    emit("SCOPY " + quad.result + ", \"" +
                         replaceAll(replaceAll(text, "\\", "\\\\"), "\"", "\\\"") + "\"");
                }
                continue;
            }
            if (base == "scopy") emit("SCOPY " + quad.result + ", " + quad.a);
            else if (base == "slen") emit("SLEN " + quad.result + ", " + quad.a);
            else if (base == "scmp") emit("SCMP " + quad.result + ", " + quad.a + ", " + quad.b);
            else if (base == "scat") emit("SCAT " + quad.result + ", " + quad.a + ", " + quad.b);
            else if (base == "ssub")
                emit("SSUB " + quad.result + ", " + quad.a + ", " + quad.b + ", " + quad.extra);
            else emit("SFIND " + quad.result + ", " + quad.a + ", " + quad.b);
            continue;
        }
        if (kStreamOps.count(base) != 0) {
            requireFeature("stream", quadIndex, quad);
            if (base == "stream") {
                emit("STREAM " + quad.result + ", " + quad.a);
            } else if (base == "sread") {
                emit("SREAD." + typeOf(quad.result) + " " + quad.result + ", " + quad.a);
            } else if (base == "swrite") {
                emit("SWRITE." + valueOperandType(quad.b) + " " +
                     quad.a + ", " + quad.b);
            } else {
                emit("SCLOSE " + quad.a);
            }
            continue;
        }
        if (kVectorOps.count(base) != 0) {
            // 向量指令带元素位数（类型后缀优先，其次看数组声明的元素类型）。
            const std::string suffix = opType(quad.op);
            const std::string element =
                !suffix.empty() ? typeInfo(suffix).name
                                : (declared.count(quad.a) ? declared.at(quad.a) : typeOf(quad.result));
            if (!config.supports("vector")) {
                // 机器没有向量指令：只要下标 / 长度是常量，就展开成低级指令
                // （逐元素的类型化取内存、存内存与标量运算）。
                const auto constantIndex = [&](const std::string& token, long long& out) {
                    if (!isIntegerLiteral(token)) return false;
                    out = std::stoll(token);
                    return true;
                };
                const auto elementName = [&](const std::string& base_, long long index) {
                    return base_ + "." + std::to_string(index);
                };
                long long first = 0;
                if (!constantIndex(quad.b, first) || wideType(element)) {
                    error("四元式 " + std::to_string(quadIndex) + " (" + quad.op +
                          ") 需要向量指令，但目标机 " + name(config.target) +
                          " 没有实现；下标/长度必须是常量并且元素宽度不超过机器字长，"
                          "才能展开成低级指令");
                }
                if (base == "vload") {
                    loadTo(regA, elementName(quad.a, first), element);
                    storeTo(quad.result, regA, element);
                } else if (base == "vstore") {
                    loadTo(regA, quad.result, element);
                    storeTo(elementName(quad.a, first), regA, element);
                } else if (base == "vfill" || base == "vcopy") {
                    const long long count = first; // 第二个操作数是长度
                    for (long long index = 0; index < count; ++index) {
                        if (base == "vfill") {
                            loadTo(regA, quad.result, element);
                            storeTo(elementName(quad.a, index), regA, element);
                        } else {
                            loadTo(regA, elementName(quad.a, index), element);
                            storeTo(elementName(quad.result, index), regA, element);
                        }
                    }
                } else if (base == "vsum") {
                    const long long count = first;
                    if (count <= 0) {
                        storeImmediate(quad.result, "0", element, typeOf(quad.result));
                        continue;
                    }
                    loadTo(regA, elementName(quad.a, 0), element);
                    for (long long index = 1; index < count; ++index) {
                        loadTo(regB, elementName(quad.a, index), element);
                        emitTemplate(command("add", "ADD {dst}, {lhs}, {rhs}"),
                                     Fill{regA, "", regA, regB, "", "", ""}, element);
                    }
                    storeTo(quad.result, regA, typeOf(quad.result));
                } else {
                    error("四元式 " + std::to_string(quadIndex) + " (" + quad.op +
                          ") 需要向量指令；" + base + " 的展开会引入循环，暂不支持低级替换");
                }
                continue;
            }
            if (base == "vload")
                emit("VLOAD." + element + " " + quad.result + ", " + quad.a + ", " + quad.b);
            else if (base == "vstore")
                emit("VSTORE." + element + " " + quad.a + ", " + quad.b + ", " + quad.result);
            else if (base == "vcopy")
                emit("VCOPY." + element + " " + quad.result + ", " + quad.a + ", " + quad.b);
            else if (base == "vfill")
                emit("VFILL." + element + " " + quad.a + ", " + quad.b + ", " + quad.result);
            else {
                const std::string op = base == "vsum" ? "VSUM" : base == "vmax" ? "VMAX" : "VMIN";
                emit(op + "." + element + " " + quad.result + ", " + quad.a + ", " + quad.b);
            }
            continue;
        }
        if (kFileOps.count(base) != 0) {
            // 文件指令也是运行时伪指令：路径按当前工作目录解析。
            requireFeature("file", quadIndex, quad);
            if (base == "fopen") {
                emit("FOPEN " + quad.result + ", " + quad.a + ", " + quad.b);
            } else if (base == "fread") {
                emit("FREAD." + typeOf(quad.result) + " " + quad.result + ", " + quad.a);
            } else if (base == "fwrite") {
                emit("FWRITE." + valueOperandType(quad.b) + " " +
                     quad.a + ", " + quad.b);
            } else if (base == "feof") {
                emit("FEOF." + typeOf(quad.result) + " " + quad.result + ", " + quad.a);
            } else {
                emit("FCLOSE " + quad.a);
            }
            continue;
        }
        if (base == "system") {
            // 系统调用：一条四元式对应一条 SYSCALL 伪指令。
            requireFeature("system", quadIndex, quad);
            emit("SYSCALL " + quad.a + ", " +
                 (quad.result == "_" ? std::string("_") : quad.result) + ", " + quad.b);
            continue;
        }
        if (base == "addr" || base == "load" || base == "store") {
            // 数据区三件套（与 stackvm 的 ADDR / LOADAT / STOREAT 同名同义）：
            //   addr  符号 -> 目标        取数据区符号基址
            //   load  指针 -> 目标        按地址读 8 字节小端
            //   store 指针, 值            按地址写 8 字节小端
            // 三条都是运行时伪指令：地址可能是窄机器上的多字变量（如 i64 分片），
            // 由模拟器按整值凑出地址再去读写 .dataarea —— 与 VLOAD / FOPEN 同理，
            // 所以三种目标机生成同一条伪指令，机器能力由指令族 'data' 表示。
            requireFeature("data", quadIndex, quad);
            if (base == "addr") emit("ADDR " + quad.result + ", " + quad.a);
            else if (base == "load") emit("LOADAT " + quad.result + ", " + quad.a);
            else emit("STOREAT." + valueOperandType(quad.b) + " " + quad.a + ", " + quad.b);
            continue;
        }
        if (base == "jz" || base == "jnz") {
            const std::string type = operandTypeOf(quad.a, operationTypeOf(quad, declared));
            if (wideType(type)) {
                emit("CMPW." + type + " " + quad.a + ", 0");
            } else {
                loadTo(regA, quad.a, type);
                emit(withType("CMP " + regA + (x86 ? ", 0" : ", #0"), type));
            }
            branchTo(base == "jz" ? "EQ" : "NE", "L_" + quad.result);
            continue;
        }
        if (base == ":=") {
            // 值的类型看后缀（折叠后保留了 f64/u32 之类的类型），目标看变量声明。
            const std::string type = operationTypeOf(quad, declared);
            const std::string target = typeOf(quad.result);
            if (isNumericLiteral(quad.a)) {
                storeImmediate(quad.result, quad.a, type, target);
                continue;
            }
            if (wideType(type)) {
                for (unsigned i = 0; i < wordCount(type); ++i) {
                    loadWord(regA, quad.a, i);
                    storeWord(quad.result, i, regA);
                }
            } else {
                loadTo(regA, quad.a, typeOf(quad.a));
                storeTo(quad.result, regA, target);
            }
            continue;
        }
        if (!expressionBase(base)) continue;

        const std::string optype = operationTypeOf(quad, declared);
        const std::string rtype = resultTypeOf(quad, declared);
        const unsigned width = typeBits(optype);
        const bool floatOp = isFloatType(optype);
        const bool unsignedOp = isUnsignedType(optype);
        const bool wide = wideType(optype) || wideType(typeOf(quad.a)) || wideType(typeOf(quad.b));
        // ---- 机器缺乘法 / 除法 / 浮点时的低级语义替换 ----
        if (!config.supports("float") &&
            (floatOp || (base == "cvt" &&
                         (isFloatType(typeOf(quad.a)) || isFloatType(rtype))))) {
            // 常量浮点运算可以在生成期算好（优化器通常已经折叠过）；
            // 运行时浮点运算需要软件浮点例程，这里明确报错。
            const bool constantExpression =
                isNumericLiteral(quad.a) && (base == "neg" || base == "cvt" ||
                                             isNumericLiteral(quad.b));
            if (constantExpression) {
                try {
                    const Value lhs = literalFor(quad.a, optype);
                    const Value rhs = (base == "neg" || base == "cvt") ? Value{}
                                                                       : literalFor(quad.b, optype);
                    const Value folded = base == "cvt" ? convertValue(lhs, optype)
                                                      : evalTyped(base, optype, lhs, rhs);
                    storeImmediate(quad.result, displayValue(folded), optype, rtype);
                    continue;
                } catch (const std::exception&) {
                    // 落到下面的报错。
                }
            }
            error("四元式 " + std::to_string(quadIndex) + " (" + quad.op +
                  ") 需要浮点指令，但目标机 " + name(config.target) +
                  " 没有实现指令族 'float'；运行时的浮点运算需要软件浮点例程，"
                  "常量运算会在生成期折叠");
        }
        if (base == "*" && !config.supports("mul") && !wide) {
            // 常量乘数展开成移位加：x * c = Σ_{k ∈ bits(|c|)} (x << k)。
            long long constant = 0;
            std::string variable;
            if (isIntegerLiteral(quad.b)) { constant = std::stoll(quad.b); variable = quad.a; }
            else if (isIntegerLiteral(quad.a)) { constant = std::stoll(quad.a); variable = quad.b; }
            else {
                // 变乘数：用"移位加"循环代替乘法（只需要加减 / 移位 / 分支）。
                if (!config.supports("shift") || !config.supports("bitwise")) {
                    error("四元式 " + std::to_string(quadIndex) +
                          " 的乘法循环展开需要移位与按位指令族");
                }
                const std::string acc = "__m_acc" + std::to_string(quadIndex);
                const std::string lhsCopy = "__m_a" + std::to_string(quadIndex);
                const std::string rhsCopy = "__m_b" + std::to_string(quadIndex);
                const std::string counter = "__m_n" + std::to_string(quadIndex);
                const std::string loop = "L_mul" + std::to_string(quadIndex);
                const std::string skip = "L_mulskip" + std::to_string(quadIndex);
                const std::string inner = "u" + std::to_string(width);
                // 初始化：acc = 0，缓存 a、b，循环 W 次。
                emitTemplate(config.immediateLoad, Fill{regA, "", "", "", "", "", "0"}, inner);
                storeTo(acc, regA, inner);
                loadTo(regA, quad.a, operandTypeOf(quad.a, optype));
                storeTo(lhsCopy, regA, inner);
                loadTo(regA, quad.b, operandTypeOf(quad.b, optype));
                storeTo(rhsCopy, regA, inner);
                emitTemplate(config.immediateLoad,
                             Fill{regA, "", "", "", "", "", std::to_string(width)}, inner);
                storeTo(counter, regA, inner);
                out << loop << ":\n";
                // if (b & 1) acc += a;
                loadTo(regA, rhsCopy, inner);
                emitTemplate(config.immediateLoad, Fill{regB, "", "", "", "", "", "1"}, inner);
                emitTemplate(command("and", "AND {dst}, {lhs}, {rhs}"),
                             Fill{regA, "", regA, regB, "", "", ""}, inner);
                emit(withType("CMP " + regA + (x86 ? ", 0" : ", #0"), inner));
                branchTo("EQ", skip);
                loadTo(regA, acc, inner);
                loadTo(regB, lhsCopy, inner);
                emitTemplate(command("add", "ADD {dst}, {lhs}, {rhs}"),
                             Fill{regA, "", regA, regB, "", "", ""}, inner);
                storeTo(acc, regA, inner);
                out << skip << ":\n";
                // a <<= 1; b >>= 1（逻辑右移）；--n; if (n != 0) 继续。
                loadTo(regA, lhsCopy, inner);
                emitTemplate(config.immediateLoad, Fill{regB, "", "", "", "", "", "1"}, inner);
                emitTemplate(command("shl", "SLL {dst}, {lhs}, {rhs}"),
                             Fill{regA, "", regA, regB, "", "", ""}, inner);
                storeTo(lhsCopy, regA, inner);
                loadTo(regA, rhsCopy, inner);
                emitTemplate(command("shru", "SRL {dst}, {lhs}, {rhs}"),
                             Fill{regA, "", regA, regB, "", "", ""}, inner);
                storeTo(rhsCopy, regA, inner);
                loadTo(regA, counter, inner);
                emitTemplate(command("sub", "SUB {dst}, {lhs}, {rhs}"),
                             Fill{regA, "", regA, regB, "", "", ""}, inner);
                storeTo(counter, regA, inner);
                emit(withType("CMP " + regA + (x86 ? ", 0" : ", #0"), inner));
                branchTo("NE", loop);
                loadTo(regA, acc, inner);
                storeTo(quad.result, regA, rtype);
                continue;
            }
            if (!config.supports("shift")) {
                error("四元式 " + std::to_string(quadIndex) +
                      " 的乘法展开需要移位指令族 'shift'");
            }
            const unsigned long long magnitude =
                constant < 0 ? static_cast<unsigned long long>(-constant)
                             : static_cast<unsigned long long>(constant);
            loadTo(regA, variable, operandTypeOf(variable, optype));
            emitTemplate(config.immediateLoad, Fill{regB, "", "", "", "", "", "0"}, optype);
            for (int bit = 0; bit < 64; ++bit) {
                if (((magnitude >> bit) & 1ull) == 0) continue;
                emitTemplate(command("shl", "SLL {dst}, {lhs}, {rhs}"),
                             Fill{regC, "", regA, std::to_string(bit), "", "", ""}, optype);
                emitTemplate(command("add", "ADD {dst}, {lhs}, {rhs}"),
                             Fill{regB, "", regB, regC, "", "", ""}, optype);
            }
            if (constant < 0) {
                emit(withType("NEG " + regB + (x86 ? "" : ", " + regB), optype));
            }
            storeTo(quad.result, regB, rtype);
            continue;
        }
        if ((base == "/" || base == "%") && !config.supports("div") && !wide) {
            // 除数是 2 的幂时：无符号除法是逻辑右移，余数是掩码。
            const bool constantDivisor = isIntegerLiteral(quad.b);
            long long divisor = constantDivisor ? std::stoll(quad.b) : 0;
            const unsigned long long magnitude =
                divisor < 0 ? static_cast<unsigned long long>(-divisor)
                            : static_cast<unsigned long long>(divisor);
            if (constantDivisor && isUnsignedType(optype) && divisor > 0 &&
                (magnitude & (magnitude - 1)) == 0) {
                // 2 的幂：移位 / 掩码
                if (!config.supports("shift") || !config.supports("bitwise")) {
                    error("四元式 " + std::to_string(quadIndex) +
                          " 的除法展开需要移位与按位指令族");
                }
                int shift = 0;
                for (auto value = magnitude; value > 1; value >>= 1) ++shift;
                loadTo(regA, quad.a, operandTypeOf(quad.a, optype));
                if (base == "%") {
                    emitTemplate(config.immediateLoad,
                                 Fill{regB, "", "", "", "", "", std::to_string(magnitude - 1)},
                                 optype);
                    emitTemplate(command("and", "AND {dst}, {lhs}, {rhs}"),
                                 Fill{regA, "", regA, regB, "", "", ""}, optype);
                } else {
                    emitTemplate(command("shru", "SRL {dst}, {lhs}, {rhs}"),
                                 Fill{regA, "", regA, std::to_string(shift), "", "", ""}, optype);
                }
                storeTo(quad.result, regA, rtype);
                continue;
            }
            // 一般除数：移位减循环（无符号；只需要加减 / 移位 / 按位 / 分支）。
            if (!constantDivisor && !config.supports("shift")) {
                error("四元式 " + std::to_string(quadIndex) +
                      " 的除法循环展开需要移位与按位指令族");
            }
            if (!isUnsignedType(optype)) {
                error("四元式 " + std::to_string(quadIndex) + " (" + quad.op +
                      ") 的有符号除法需要除法指令；低级替换目前只覆盖无符号类型");
            }
            if (constantDivisor && divisor <= 0) {
                error("四元式 " + std::to_string(quadIndex) + " 的除数为 0 或负数");
            }
            if (!config.supports("bitwise")) {
                error("四元式 " + std::to_string(quadIndex) +
                      " 的除法循环展开需要移位与按位指令族");
            }
            const std::string quotient = "__d_q" + std::to_string(quadIndex);
            const std::string remainder = "__d_r" + std::to_string(quadIndex);
            const std::string bitIndex = "__d_i" + std::to_string(quadIndex);
            const std::string divisorCopy = "__d_b" + std::to_string(quadIndex);
            const std::string loop = "L_div" + std::to_string(quadIndex);
            const std::string skip = "L_divskip" + std::to_string(quadIndex);
            const std::string shortName = "u" + std::to_string(width);
            emitTemplate(config.immediateLoad, Fill{regA, "", "", "", "", "", "0"}, shortName);
            storeTo(quotient, regA, shortName);
            storeTo(remainder, regA, shortName);
            emitTemplate(config.immediateLoad,
                         Fill{regA, "", "", "", "", "", std::to_string(width - 1)}, shortName);
            storeTo(bitIndex, regA, shortName);
            loadTo(regA, quad.b, operandTypeOf(quad.b, optype));
            storeTo(divisorCopy, regA, shortName);
            out << loop << ":\n";
            // t = (dividend >> i) & 1
            loadTo(regA, quad.a, operandTypeOf(quad.a, optype));
            loadTo(regB, bitIndex, shortName);
            emitTemplate(command("shru", "SRL {dst}, {lhs}, {rhs}"),
                         Fill{regA, "", regA, regB, "", "", ""}, shortName);
            emitTemplate(config.immediateLoad, Fill{regB, "", "", "", "", "", "1"}, shortName);
            emitTemplate(command("and", "AND {dst}, {lhs}, {rhs}"),
                         Fill{regA, "", regA, regB, "", "", ""}, shortName);
            // r = (r << 1) | t
            loadTo(regB, remainder, shortName);
            emitTemplate(config.immediateLoad, Fill{regC, "", "", "", "", "", "1"}, shortName);
            emitTemplate(command("shl", "SLL {dst}, {lhs}, {rhs}"),
                         Fill{regB, "", regB, regC, "", "", ""}, shortName);
            emitTemplate(command("or", "OR {dst}, {lhs}, {rhs}"),
                         Fill{regB, "", regB, regA, "", "", ""}, shortName);
            storeTo(remainder, regB, shortName);
            // if (r >= b) { r -= b; q |= 1 << i }
            loadTo(regC, divisorCopy, shortName);
            emit(withType("CMP " + regB + ", " + regC, shortName));
            branchTo("LT", skip);
            emitTemplate(command("sub", "SUB {dst}, {lhs}, {rhs}"),
                         Fill{regB, "", regB, regC, "", "", ""}, shortName);
            storeTo(remainder, regB, shortName);
            emitTemplate(config.immediateLoad, Fill{regB, "", "", "", "", "", "1"}, shortName);
            loadTo(regC, bitIndex, shortName);
            emitTemplate(command("shl", "SLL {dst}, {lhs}, {rhs}"),
                         Fill{regB, "", regB, regC, "", "", ""}, shortName);
            loadTo(regC, quotient, shortName);
            emitTemplate(command("or", "OR {dst}, {lhs}, {rhs}"),
                         Fill{regC, "", regC, regB, "", "", ""}, shortName);
            storeTo(quotient, regC, shortName);
            out << skip << ":\n";
            // --i; if (i >= 0) 继续
            loadTo(regA, bitIndex, shortName);
            emitTemplate(config.immediateLoad, Fill{regB, "", "", "", "", "", "1"}, shortName);
            emitTemplate(command("sub", "SUB {dst}, {lhs}, {rhs}"),
                         Fill{regA, "", regA, regB, "", "", ""}, shortName);
            storeTo(bitIndex, regA, shortName);
            // 计数器会降到 -1，比较要按有符号解释才停得下来。
            emit(withType("CMP " + regA + (x86 ? ", 0" : ", #0"),
                          "i" + std::to_string(width)));
            branchTo("GE", loop);
            loadTo(regA, base == "%" ? remainder : quotient, shortName);
            storeTo(quad.result, regA, rtype);
            continue;
        }
        // 机器能力检查：浮点、乘除、移位、按位、进位各自可以缺失。
        if (floatOp || (base == "cvt" && (isFloatType(typeOf(quad.a)) || isFloatType(rtype))))
            requireFeature("float", quadIndex, quad);
        if (pairBase(base)) requireFeature(base == "mul" ? "mul" : "div", quadIndex, quad);
        else if (base == "*") requireFeature("mul", quadIndex, quad);
        else if (base == "/" || base == "%") requireFeature("div", quadIndex, quad);
        else if (base == "<<" || base == ">>") requireFeature("shift", quadIndex, quad);
        else if (base == "&" || base == "|" || base == "^" || base == "~")
            requireFeature("bitwise", quadIndex, quad);
        else if (carryBase(base)) requireFeature("carry", quadIndex, quad);

        if (base == "cvt") {
            const std::string sourceType = operandTypeOf(quad.a, optype);
            // 源或目标比机器字还宽时转换交给运行时例程。
            if (wideType(sourceType) || wideType(rtype)) {
                emit("RCALL." + optype + " cvt, " + quad.result + ", " + quad.a + ", _");
                continue;
            }
            loadTo(regA, quad.a, sourceType);
            emit("CVT." + optype + " " + regA + ", " + regA);
            storeTo(quad.result, regA, rtype);
            continue;
        }
        if (floatOp) {
            loadTo(regA, quad.a, operandTypeOf(quad.a, optype));
            if (base != "neg") loadTo(regB, quad.b, operandTypeOf(quad.b, optype));
            if (compareBase(base)) {
                emit("FCMP." + optype + " " + regA + ", " + regB);
                const std::string yes = "T_true_" + std::to_string(serial);
                const std::string done = "T_done_" + std::to_string(serial++);
                emitTemplate(config.immediateLoad, Fill{regC, "", "", "", "", "", "0"}, rtype);
                branchTo(conditionName.at(base), yes);
                emit(std::string(x86 ? "JMP " : target == Target::Mips ? "J " : "B ") + done);
                out << yes << ":\n";
                emitTemplate(config.immediateLoad, Fill{regC, "", "", "", "", "", "1"}, rtype);
                out << done << ":\n";
                storeTo(quad.result, regC, rtype);
                continue;
            }
            const std::string key = base == "+" ? "fadd" : base == "-" ? "fsub"
                                    : base == "*" ? "fmul" : base == "/" ? "fdiv" : "fneg";
            const std::map<std::string, std::string> fallback{
                {"fadd", "ADD {dst}, {lhs}, {rhs}"}, {"fsub", "SUB {dst}, {lhs}, {rhs}"},
                {"fmul", "MUL {dst}, {lhs}, {rhs}"}, {"fdiv", "DIV {dst}, {lhs}, {rhs}"},
                {"fneg", "NEG {dst}, {lhs}"}};
            emitTemplate(command(key, fallback.at(key)), Fill{regA, "", regA, regB, "", "", ""}, optype);
            storeTo(quad.result, regA, rtype);
            continue;
        }
        if (wide) {
            const unsigned count = wordCount(optype);
            if (base == "+" || base == "-" || carryBase(base)) {
                const bool subtract = base == "-" || base == "-b";
                const bool chainFromCarry = carryBase(base);
                for (unsigned i = 0; i < count; ++i) {
                    loadWordOperand(regA, quad.a, optype, i);
                    loadWordOperand(regB, quad.b, optype, i);
                    const bool carryStep = chainFromCarry || i > 0;
                    const std::string mnemonic = subtract ? (carryStep ? "SUBB" : "SUB")
                                                          : (carryStep ? "ADDC" : "ADD");
                    emit(withType(mnemonic + " " + regA + ", " + regA + ", " + regB, wordType));
                    storeWord(quad.result, i, regA);
                }
                continue;
            }
            if (base == "&" || base == "|" || base == "^") {
                const std::string mnemonic = base == "&" ? "AND" : base == "|" ? "OR" : "XOR";
                for (unsigned i = 0; i < count; ++i) {
                    loadWordOperand(regA, quad.a, optype, i);
                    loadWordOperand(regB, quad.b, optype, i);
                    emit(withType(mnemonic + " " + regA + ", " + regA + ", " + regB, wordType));
                    storeWord(quad.result, i, regA);
                }
                continue;
            }
            if (base == "~") {
                for (unsigned i = 0; i < count; ++i) {
                    loadWord(regA, quad.a, i);
                    emit(withType("NOT " + regA + ", " + regA, wordType));
                    storeWord(quad.result, i, regA);
                }
                continue;
            }
            // 机器没有这么宽的运算：交给运行时例程（真实后端这里是库例程）。
            if (pairBase(base))
                emit("RCALLW." + optype + " " + base + ", " + quad.result + ", " + quad.extra +
                     ", " + quad.a + ", " + quad.b);
            else
                emit("RCALL." + optype + " " + base + ", " + quad.result + ", " + quad.a + ", " +
                     quad.b);
            continue;
        }

        // ---- 原生宽度（W <= 机器字长） ----
        loadTo(regA, quad.a, operandTypeOf(quad.a, optype));
        const bool second = binaryBase(base) && base != "neg" && base != "odd" && base != "~";
        if (second) loadTo(regB, quad.b, operandTypeOf(quad.b, optype));
        if (base == "neg") {
            emit(withType("NEG " + regA + (x86 ? "" : ", " + regA), optype));
            storeTo(quad.result, regA, rtype);
            continue;
        }
        if (base == "odd") {
            emitTemplate(config.immediateLoad, Fill{regB, "", "", "", "", "", "1"}, optype);
            emit(withType("AND " + regA + ", " + (x86 ? regB : regA + ", " + regB), optype));
            storeTo(quad.result, regA, rtype);
            continue;
        }
        if (base == "~") {
            emit(withType(substitute(command("not", x86 ? "NOT {dst}" : "NOR {dst}, {lhs}, {lhs}"),
                                Fill{regA, "", regA, regA, "", "", ""}),
                          optype));
            storeTo(quad.result, regA, rtype);
            continue;
        }
        if (carryBase(base)) {
            emit(withType(std::string(base == "+c" ? "ADDC " : "SUBB ") + regA + ", " + regA +
                              ", " + regB,
                          optype));
            storeTo(quad.result, regA, rtype);
            continue;
        }
        if (compareBase(base)) {
            const std::string yes = "T_true_" + std::to_string(serial);
            const std::string done = "T_done_" + std::to_string(serial++);
            emit(withType("CMP " + regA + ", " + regB, optype));
            emitTemplate(config.immediateLoad, Fill{regC, "", "", "", "", "", "0"}, rtype);
            branchTo(conditionName.at(base), yes);
            emit(std::string(x86 ? "JMP " : target == Target::Mips ? "J " : "B ") + done);
            out << yes << ":\n";
            emitTemplate(config.immediateLoad, Fill{regC, "", "", "", "", "", "1"}, rtype);
            out << done << ":\n";
            storeTo(quad.result, regC, rtype);
            continue;
        }
        if (base == "*" || base == "/" || base == "%" || pairBase(base)) {
            // 乘法/除法：结果进固定的命名寄存器对（HI/LO、DX:AX、regHi/regLo），
            // 一条四元式展开成一串目标指令，具体序列由配置里的 mul/div/rem/
            // mul2/div2（无符号加 u 后缀）模板决定。
            const std::string stem = pairBase(base) ? (base == "mul" ? "mul2" : "div2")
                                    : base == "*" ? "mul"
                                    : base == "/" ? "div"
                                                  : "rem";
            const std::map<std::string, std::string> fallback{
                {"mul", "MUL {dst}, {lhs}, {rhs}"},
                {"mulu", "MULU {dst}, {lhs}, {rhs}"},
                {"div", "DIV {lhs}, {rhs}\nMFLO {dst}"},
                {"divu", "DIVU {lhs}, {rhs}\nMFLO {dst}"},
                {"rem", "DIV {lhs}, {rhs}\nMFHI {dst}"},
                {"remu", "DIVU {lhs}, {rhs}\nMFHI {dst}"},
                {"mul2", "MULT {lhs}, {rhs}\nMFLO {dst}\nMFHI {dst2}"},
                {"mul2u", "MULTU {lhs}, {rhs}\nMFLO {dst}\nMFHI {dst2}"},
                {"div2", "DIV {lhs}, {rhs}\nMFLO {dst}\nMFHI {dst2}"},
                {"div2u", "DIVU {lhs}, {rhs}\nMFLO {dst}\nMFHI {dst2}"}};
            const std::string key = stem + (unsignedOp ? "u" : "");
            emitTemplate(command(key, fallback.at(key)),
                         Fill{quad.result, quad.extra, regA, regB, "", "", ""}, optype);
            continue;
        }
        const std::string key = base == "+" ? "add" : base == "-" ? "sub"
                                : base == "<<" ? "shl" : base == ">>" ? (unsignedOp ? "shru" : "shr")
                                : base == "&" ? "and" : base == "|" ? "or" : "xor";
        const std::map<std::string, std::string> fallback{
            {"add", "ADD {dst}, {lhs}, {rhs}"},   {"sub", "SUB {dst}, {lhs}, {rhs}"},
            {"shl", "SLL {dst}, {lhs}, {rhs}"},   {"shr", "SRA {dst}, {lhs}, {rhs}"},
            {"shru", "SRL {dst}, {lhs}, {rhs}"},  {"and", "AND {dst}, {lhs}, {rhs}"},
            {"or", "OR {dst}, {lhs}, {rhs}"},     {"xor", "XOR {dst}, {lhs}, {rhs}"}};
        // 移位位数按类型宽度取模：机器没有该宽度时先掩码再移位。
        if ((base == "<<" || base == ">>") && width < 64 && (width & (width - 1)) == 0) {
            const std::string mask = std::to_string(width - 1);
            emitTemplate(config.immediateLoad, Fill{regC, "", "", "", "", "", mask},
                         "i" + std::to_string(word));
            emit(withType("AND " + regB + ", " + regB + ", " + regC, "i" + std::to_string(word)));
        }
        emitTemplate(command(key, fallback.at(key)), Fill{regA, "", regA, regB, "", "", ""}, optype);
        storeTo(quad.result, regA, rtype);
    }
    emit("HALT");
    return out.str();
}

std::string generate(const Program& program, Target target) {
    return generate(program, defaultConfig(target));
}

namespace {

/** 真正的执行器：流输入按流名分开（STDIN 是默认流）。 */
Execution runSimulation(const std::string& assembly,
                        const std::map<std::string, std::vector<std::string>>& streams,
                        std::size_t stepLimit) {
    Execution result;
    Target machine = Target::Mips;
    unsigned word = 16;
    std::string pairHigh = "HI", pairLow = "LO";
    std::map<std::string, std::string> varTypes;
    std::set<std::string> localNames, readOnly, regNames;
    std::map<std::string, Value> memory;
    std::map<std::string, std::vector<std::string>> streamsIn = streams;
    std::map<std::string, std::size_t> streamCursor;
    std::map<std::string, std::vector<Value>> streamsOut;
    std::map<std::string, std::string> streamsText;
    std::map<std::string, std::fstream> openFiles;
    long long simulatedTicks = 0;
    std::uint64_t randomState = 20260918ull;
    std::vector<std::vector<std::string>> code;
    std::map<std::string, std::size_t> labels;
    /** 数据区：字节镜像 + 符号基址（`addr` / `load` / `store` 用，见 .dataarea 段）。 */
    std::vector<std::uint8_t> dataArea;
    std::map<std::string, std::size_t> dataAreaSymbols;
    std::size_t dataAreaSize = 0;
    /** 解析 `.hex 偏移 十六进制串` / `.bytes 偏移 十进制,十进制…`：起始偏移 + 字节。 */
    const auto parseByteDirective = [](const std::string& line, bool hex, std::size_t& offset,
                                       std::vector<unsigned>& bytes) {
        std::string body = trim(line.substr(hex ? 4 : 6));
        std::string offsetText = body;
        std::string dataText;
        const auto space = body.find_first_of(" \t");
        if (space != std::string::npos) {
            offsetText = trim(body.substr(0, space));
            dataText = trim(body.substr(space + 1));
        }
        bytes.clear();
        if (hex) {
            if (dataText.size() % 2 != 0) {
                error("Invalid .hex (need pairs of hex digits): " + line);
            }
            for (std::size_t i = 0; i + 1 < dataText.size(); i += 2) {
                bytes.push_back(static_cast<unsigned>(
                    std::stoul(dataText.substr(i, 2), nullptr, 16)));
            }
        } else {
            std::string list = dataText;
            for (char& c : list) {
                if (c == ',') c = ' ';
            }
            for (const auto& piece : words(list)) {
                bytes.push_back(static_cast<unsigned>(std::stoul(piece)));
            }
        }
        offset = 0;
        try {
            offset = static_cast<std::size_t>(std::stoul(offsetText));
        } catch (const std::exception&) {
            error("Invalid byte offset: " + line);
        }
    };

    // ---- 解析汇编：头部、声明段与代码段 ----
    {
        enum class Section { None, Data, Rodata, DataArea };
        Section section = Section::None;
        bool inCode = false;
        std::istringstream source(assembly);
        for (std::string raw; std::getline(source, raw);) {
            std::string text = trim(raw);
            if (text.empty() || text[0] == ';') continue;
            if (!inCode) {
                if (text.rfind(".target", 0) == 0) { machine = target(trim(text.substr(7))); continue; }
                if (text.rfind(".bits", 0) == 0) {
                    word = static_cast<unsigned>(std::stoul(trim(text.substr(5))));
                    continue;
                }
                if (text.rfind(".mulpair", 0) == 0) {
                    const auto parts = words(trim(text.substr(8)));
                    if (parts.size() == 2) { pairHigh = parts[0]; pairLow = parts[1]; }
                    continue;
                }
                if (text.rfind(".reg", 0) == 0) { regNames.insert(trim(text.substr(4))); continue; }
                if (text == ".data") { section = Section::Data; continue; }
                if (text == ".enddata") { section = Section::None; continue; }
                if (text == ".rodata") { section = Section::Rodata; continue; }
                if (text == ".endrodata") { section = Section::None; continue; }
                if (text == ".dataarea" || text.rfind(".dataarea ", 0) == 0) {
                    // 段头带的是字节总数（`.dataarea 124`）：字节仍按 .hex 行装载，
                    // 没写满的部分在解析结束时补零，地址空间与生成器声明的一致。
                    section = Section::DataArea;
                    const auto parts = words(trim(text.substr(9)));
                    if (!parts.empty()) {
                        try {
                            dataAreaSize = static_cast<std::size_t>(std::stoul(parts[0]));
                        } catch (const std::exception&) {
                            error("Invalid .dataarea size: " + raw);
                        }
                    }
                    continue;
                }
                if (text == ".enddataarea") { section = Section::None; continue; }
                if (text.rfind(".stack", 0) == 0) continue;
                if (text == ".code") { inCode = true; continue; }
                if (section == Section::None) continue;
                if (section == Section::DataArea) {
                    // 符号基址：`.sym 符号 偏移`（simple_ast 的 .layout 表里的 offset）。
                    if (text.rfind(".sym", 0) == 0) {
                        const auto parts = words(trim(text.substr(4)));
                        if (parts.size() != 2) error("Invalid .sym (need symbol and offset): " + raw);
                        try {
                            dataAreaSymbols[parts[0]] =
                                static_cast<std::size_t>(std::stoul(parts[1]));
                        } catch (const std::exception&) {
                            error("Invalid .sym offset: " + raw);
                        }
                        continue;
                    }
                    // 字节镜像：与 .data 段同一套紧凑写法。
                    if (text.rfind(".hex", 0) == 0 || text.rfind(".bytes", 0) == 0) {
                        std::size_t offset = 0;
                        std::vector<unsigned> bytes;
                        parseByteDirective(text, text.rfind(".hex", 0) == 0, offset, bytes);
                        for (const unsigned byte : bytes) {
                            if (byte > 255) error("Byte out of range (0..255): " + raw);
                            if (dataArea.size() < offset + 1) dataArea.resize(offset + 1, 0);
                            dataArea[offset++] = static_cast<std::uint8_t>(byte);
                        }
                        continue;
                    }
                    continue;
                }
                // 紧凑字节：`.hex 偏移 十六进制串` / `.bytes 偏移 十进制,十进制…`
                // 每个字节照旧得到一个 `byte_<偏移>` 符号（类型按默认类型）。
                if (text.rfind(".hex", 0) == 0 || text.rfind(".bytes", 0) == 0) {
                    const bool hex = text.rfind(".hex", 0) == 0;
                    std::size_t offset = 0;
                    std::vector<unsigned> bytes;
                    parseByteDirective(text, hex, offset, bytes);
                    for (const unsigned byte : bytes) {
                        if (byte > 255) error("Byte out of range (0..255): " + raw);
                        const std::string name = "byte_" + std::to_string(offset++);
                        const std::string type = defaultTypeName();
                        varTypes[name] = typeInfo(type).name;
                        memory[name] = literalValue(std::to_string(byte), typeInfo(type).name);
                        if (section == Section::Rodata) readOnly.insert(name);
                    }
                    continue;
                }
                // 声明行：[local ]name[ = value][ : type]
                bool local = false;
                if (text.rfind("local ", 0) == 0) { local = true; text = trim(text.substr(6)); }
                std::string type = defaultTypeName();
                const auto colon = text.rfind(':');
                if (colon != std::string::npos) {
                    type = trim(text.substr(colon + 1));
                    text = trim(text.substr(0, colon));
                }
                if (!isTypeName(type)) error("Invalid declaration: " + raw);
                std::string name = text, value;
                const auto equal = text.find('=');
                if (equal != std::string::npos) {
                    name = trim(text.substr(0, equal));
                    value = trim(text.substr(equal + 1));
                }
                if (name.empty()) error("Invalid declaration: " + raw);
                varTypes[name] = typeInfo(type).name;
                if (local) localNames.insert(name);
                if (section == Section::Rodata) readOnly.insert(name);
                if (!value.empty()) memory[name] = literalValue(value, typeInfo(type).name);
                continue;
            }
            if (text.back() == ':') {
                const std::string label = trim(text.substr(0, text.size() - 1));
                if (!labels.emplace(label, code.size()).second)
                    error("Duplicate assembly label: " + label);
                continue;
            }
            code.push_back(words(text));
        }
        // 段头声明了字节总数、.hex 行没写满时补零：地址仍然有效，读到的是 0。
        if (dataArea.size() < dataAreaSize) dataArea.resize(dataAreaSize, 0);
    }

    // ---- 运行状态：寄存器、进位标志、调用帧 ----
    std::map<std::string, Value> regs;
    bool carry = false;
    bool compared = false;
    Value compareLhs, compareRhs;
    std::string compareType = defaultTypeName();
    std::size_t pc = 0;
    struct Frame {
        std::size_t returnPc = 0;
        std::string destination;
        bool hasDestination = false;
        bool interrupt = false;
        std::map<std::string, Value> locals;
        std::map<std::string, Value> savedRegs;
        bool savedCarry = false;
        std::vector<std::pair<std::string, Value>> pending;
        bool hasReturn = false;
        Value returnValue;
    };
    std::vector<Frame> frames(1);
    const auto frame = [&]() -> Frame& { return frames.back(); };

    const auto isRegister = [&](const std::string& text) {
        return regNames.count(text) != 0 || registerName(text);
    };
    const auto isLocalSlot = [&](const std::string& name) {
        if (localNames.count(name) != 0) return true;
        // 数组元素 v.3 与宽类型分片 v.3#1 都跟着基名走。
        const auto dot = name.find('.');
        if (dot != std::string::npos && localNames.count(name.substr(0, dot)) != 0) return true;
        const auto hash = name.find('#');
        if (hash == std::string::npos) return false;
        const std::string base = name.substr(0, hash);
        const auto inner = base.find('.');
        return localNames.count(base) != 0 ||
               (inner != std::string::npos && localNames.count(base.substr(0, inner)) != 0);
    };
    const auto slotType = [&](const std::string& name) {
        const auto found = varTypes.find(name);
        if (found != varTypes.end()) return found->second;
        // 数组元素：元素类型就是数组声明的类型（v.3 看 v）。
        auto dot = name.find('.');
        if (dot != std::string::npos) {
            const auto base = varTypes.find(name.substr(0, dot));
            if (base != varTypes.end()) return base->second;
        }
        const auto hash = name.find('#');
        if (hash != std::string::npos) {
            dot = name.find('.');
            const auto base = varTypes.find(name.substr(0, hash));
            if (base != varTypes.end()) return base->second;
            if (dot != std::string::npos) {
                const auto arrayBase = varTypes.find(name.substr(0, dot));
                if (arrayBase != varTypes.end()) return arrayBase->second;
            }
        }
        return std::string(defaultTypeName());
    };
    const auto slotTable = [&](const std::string& name) -> std::map<std::string, Value>& {
        return isLocalSlot(name) ? frame().locals : memory;
    };
    const auto readSlot = [&](const std::string& name, const std::string& type) -> Value {
        auto& table = slotTable(name);
        const auto found = table.find(name);
        if (found == table.end()) error("Read of uninitialized variable: " + name);
        return convertValue(found->second, type);
    };
    const auto writeSlot = [&](const std::string& name, const Value& value,
                               const std::string& type) {
        const auto hash = name.find('#');
        const std::string base = hash == std::string::npos ? name : name.substr(0, hash);
        if (readOnly.count(name) != 0 || readOnly.count(base) != 0)
            error("Write to read-only variable: " + name);
        slotTable(name)[name] = convertValue(value, type);
    };
    /** 宽类型变量与机器字分片之间的拆装。 */
    const auto wholeRead = [&](const std::string& name, const std::string& type) -> Value {
        // 变量实际宽度看声明；窄变量读单个槽再转换，宽变量按分片重组。
        const std::string declared = slotType(name);
        if (isFloatType(declared) || typeBits(declared) <= word)
            return convertValue(readSlot(name, declared), type);
        Value out = valueFromBits(U128{}, declared);
        for (unsigned i = 0; i < (typeBits(declared) + word - 1) / word; ++i) {
            const Value part = readSlot(name + "#" + std::to_string(i), "u" + std::to_string(word));
            out.bits = u128Or(out.bits, u128Shl(valueBits(part), i * word));
        }
        return convertValue(out, type);
    };
    const auto wholeWrite = [&](const std::string& name, const Value& value) {
        const std::string type = slotType(name);
        if (isFloatType(type) || typeBits(type) <= word) { writeSlot(name, value, type); return; }
        const Value converted = convertValue(value, type);
        for (unsigned i = 0; i < (typeBits(type) + word - 1) / word; ++i) {
            writeSlot(name + "#" + std::to_string(i),
                      valueFromBits(u128Shr(valueBits(converted), i * word),
                                    "u" + std::to_string(word)),
                      "u" + std::to_string(word));
        }
    };
    const auto assign = [&](const std::string& name, const Value& value) {
        if (isRegister(name)) regs[name] = value;
        else wholeWrite(name, value);
    };
    /** 整值操作数：字面量按类型解析，寄存器取其值，其余按变量分片重组。 */
    const auto wholeOperand = [&](const std::string& token, const std::string& type) -> Value {
        if (token.empty() || token == "_") return Value{}; // 一元运算的占位操作数
        const std::string plain = (!token.empty() && token[0] == '#') ? token.substr(1) : token;
        if (isNumericLiteral(plain)) return literalFor(plain, type);
        if (isRegister(token)) return convertValue(regs[token], type);
        return wholeRead(token, type);
    };
    const auto operand = [&](const std::string& token, const std::string& type) -> Value {
        if (token.size() >= 2 && token.front() == '[' && token.back() == ']')
            return readSlot(token.substr(1, token.size() - 2), type);
        if (isRegister(token)) return convertValue(regs[token], type);
        // 立即数可能写成 #1（ARM 风格）或 -1。
        const std::string plain = (!token.empty() && token[0] == '#') ? token.substr(1) : token;
        if (isNumericLiteral(plain)) return literalFor(plain, type);
        return readSlot(token, type);
    };
    const auto storeOperand = [&](const std::string& token, const Value& value,
                                  const std::string& type) {
        if (token.size() >= 2 && token.front() == '[' && token.back() == ']')
            writeSlot(token.substr(1, token.size() - 2), value, type);
        else if (isRegister(token)) regs[token] = convertValue(value, type);
        else writeSlot(token, value, type);
    };
    const auto jump = [&](const std::string& label) {
        const auto found = labels.find(label);
        if (found == labels.end()) error("Unknown assembly label: " + label);
        pc = found->second;
    };
    /** 从命名流取一个输入项：STDIN 就是普通输入列表，其他流按流名分开。 */
    const auto nextInput = [&](const std::string& stream, const std::string& type) -> Value {
        auto& items = streamsIn[stream];
        auto& cursor = streamCursor[stream];
        if (cursor >= items.size()) error("Input exhausted on stream " + stream);
        return literalValue(items[cursor++], type);
    };
    /** 字符串操作数：字面量直接取文本，变量按声明读成 str。 */
    const auto stringOperand = [&](const std::string& token) -> Value {
        if (isStringLiteral(token)) {
            Value value;
            value.type = "str";
            value.text = stringLiteralText(token);
            return value;
        }
        if (isRegister(token)) return convertValue(regs[token], "str");
        return readSlot(token, "str");
    };
    /** 流句柄：写 STDIN / "STDIN" 都行，变量则取其字符串值。 */
    const auto streamHandle = [&](const std::string& token) -> std::string {
        if (isStringLiteral(token)) return stringLiteralText(token);
        if (!isRegister(token) && identifier(token) && varTypes.count(token) == 0) return token;
        return stringValue(stringOperand(token));
    };
    const auto asIndex = [&](const Value& value) -> long long {
        if (isStringType(value.type)) error("Expected a numeric index or length");
        if (isFloatType(value.type)) return static_cast<long long>(value.real);
        return std::stoll(displayValue(value));
    };
    /** 写流：STDOUT 走 output/text，其他流按流名收集。 */
    const auto streamWrite = [&](const std::string& stream, const Value& value) {
        if (stream.empty() || stream == "STDOUT") {
            if (isStringType(value.type)) result.text += value.text;
            else result.output.push_back(value);
        } else {
            // 其他流（STDERR 等）：文本按写入顺序累积，数值同时记在 streams 里。
            streamsText[stream] += isStringType(value.type) ? value.text : displayValue(value);
            if (!isStringType(value.type)) streamsOut[stream].push_back(value);
        }
    };
    const auto doCall = [&](const std::string& function, const std::string& destination,
                            bool interrupt) {
        // 先把调用方挂起的实参取出来，再建立新帧并把实参按形参类型写进去
        // （宽形参要拆成分片，和函数体里的分片访问保持一致）。
        auto pending = frame().pending;
        frame().pending.clear();
        Frame next;
        next.returnPc = pc;
        next.destination = destination;
        next.hasDestination = destination != "_" && !destination.empty();
        next.interrupt = interrupt;
        next.savedRegs = regs;
        next.savedCarry = carry;
        frames.push_back(std::move(next));
        regs.clear();
        carry = false;
        for (auto& item : pending) wholeWrite(item.first, item.second);
        jump(function);
    };
    const auto doReturn = [&](bool interruptReturn) {
        if (frames.size() <= 1) error(interruptReturn ? "IRET without INT" : "RET without CALL");
        Frame finished = std::move(frames.back());
        if (interruptReturn && !finished.interrupt) error("IRET without INT");
        frames.pop_back();
        regs = finished.savedRegs;
        carry = finished.savedCarry;
        pc = finished.returnPc;
        if (finished.hasReturn && finished.hasDestination)
            assign(finished.destination, finished.returnValue);
    };
    const std::map<std::string, std::string> conditions{
        {"BEQ", "=="}, {"BNE", "!="}, {"BLT", "<"}, {"BLE", "<="}, {"BGT", ">"}, {"BGE", ">="},
        {"JE", "=="},  {"JNE", "!="}, {"JL", "<"},  {"JLE", "<="}, {"JG", ">"},  {"JGE", ">="}};
    const std::set<std::string> loadMnemonics{
        "LD",   "LB",   "LBU", "LH",   "LHU",  "LW",   "LQ",   "LDRSB", "LDRB",
        "LDRSH", "LDRH", "LDR", "LDRD", "LDRQ", "LWC1", "LDC1", "VLDR",  "VLDRD"};
    const std::set<std::string> storeMnemonics{
        "ST",   "SB",   "SH",   "SW",   "SQ",   "STRB", "STRH", "STR",
        "STRD", "STRQ", "SWC1", "SDC1", "VSTR", "VSTRD"};
    const std::map<std::string, std::string> aluMnemonics{
        {"ADD", "+"}, {"ADDU", "+"}, {"SUB", "-"}, {"SUBU", "-"},
        {"AND", "&"}, {"OR", "|"},   {"ORR", "|"}, {"XOR", "^"}, {"EOR", "^"}};
    // 目标机当前只影响 .target 头与固定寄存器对；指令语义全部按类型求值。
    (void)machine;

    while (pc < code.size()) {
        if (result.steps++ >= stepLimit) error("Instruction limit exceeded");
        const auto w = code[pc++];
        if (w.empty()) continue;
        if (std::getenv("TRANSFER_TRACE") != nullptr) {
            std::cerr << "  pc=" << (pc - 1) << " :";
            for (const auto& token : w) std::cerr << ' ' << token;
            std::cerr << '\n';
        }
        std::string mnemonic = w[0];
        std::string type = defaultTypeName();
        const auto dot = mnemonic.find('.');
        if (dot != std::string::npos) {
            type = mnemonic.substr(dot + 1);
            mnemonic = mnemonic.substr(0, dot);
        }
        if (!isTypeName(type)) error("Unknown type in instruction: " + w[0]);
        const auto need = [&](std::size_t count) {
            if (w.size() != count + 1) error("Invalid operands for " + w[0]);
        };
        /** 机器字长检查：超过机器字长的运算必须由后端展开成多字序列或
         *  运行时例程（RCALL / RCALLW / READW / OUTW 等）。 */
        const auto checkWidth = [&]() {
            if (!isFloatType(type) && typeBits(type) > word) {
                error("Instruction " + w[0] + " needs " + std::to_string(typeBits(type)) +
                      "-bit ALU, but the machine word is " + std::to_string(word) + " bits");
            }
        };

        if (mnemonic == "HALT") {
            need(0);
            result.streams = streamsOut;
            result.streamText = streamsText;
            return result;
        }
        if (mnemonic == "RET" || mnemonic == "IRET") {
            if (mnemonic == "RET" && w.size() == 2) {
                frame().hasReturn = true;
                frame().returnValue = operand(w[1], type);
            } else {
                need(0);
            }
            doReturn(mnemonic == "IRET");
            continue;
        }
        if (mnemonic == "RETW") {
            need(1);
            frame().hasReturn = true;
            frame().returnValue = wholeOperand(w[1], type);
            doReturn(false);
            continue;
        }
        if (mnemonic == "CALL" || mnemonic == "INT") {
            if (w.size() < 2 || w.size() > 3) error("Invalid operands for " + mnemonic);
            // 生成的函数标签带 L_ 前缀（与跳转一致）。
            doCall("L_" + w[1], w.size() == 3 ? w[2] : "_", mnemonic == "INT");
            continue;
        }
        if (mnemonic == "PUSH" || mnemonic == "PUSHW") {
            need(2);
            frame().pending.emplace_back(
                w[2], mnemonic == "PUSH" ? operand(w[1], type) : wholeOperand(w[1], type));
            continue;
        }
        if (mnemonic == "J" || mnemonic == "B" || mnemonic == "JMP") {
            need(1);
            jump(w[1]);
            continue;
        }
        if (conditions.count(mnemonic) != 0) {
            if (w.size() == 2) {
                if (!compared) error("Branch without CMP");
                if (!valueIsZero(
                        evalTyped(conditions.at(mnemonic), compareType, compareLhs, compareRhs)))
                    jump(w[1]);
            } else if (w.size() == 4) {
                checkWidth();
                if (!valueIsZero(evalTyped(conditions.at(mnemonic), type, operand(w[1], type),
                                           operand(w[2], type))))
                    jump(w[3]);
            } else {
                error("Invalid operands for " + mnemonic);
            }
            continue;
        }
        if (mnemonic == "BEQZ" || mnemonic == "BNEZ") {
            need(2);
            checkWidth();
            const bool isZero = valueIsZero(operand(w[1], type));
            if (isZero == (mnemonic == "BEQZ")) jump(w[2]);
            continue;
        }
        if (mnemonic == "CMP" || mnemonic == "FCMP") {
            need(2);
            compareLhs = operand(w[1], type);
            compareRhs = operand(w[2], type);
            compareType = type;
            compared = true;
            continue;
        }
        if (mnemonic == "CMPW") { // 宽类型比较：整值参与，调用方随后按标志跳转
            need(2);
            compareLhs = wholeOperand(w[1], type);
            compareRhs = wholeOperand(w[2], type);
            compareType = type;
            compared = true;
            continue;
        }
        if (mnemonic == "MOV" || mnemonic == "LI") {
            need(2);
            storeOperand(w[1], operand(w[2], type), type);
            continue;
        }
        if (mnemonic == "CVT") {
            need(2);
            const Value source = isRegister(w[2]) ? regs[w[2]] : readSlot(w[2], slotType(w[2]));
            storeOperand(w[1], convertValue(source, type), type);
            continue;
        }
        if (mnemonic == "IN" || mnemonic == "GETC") {
            need(1);
            auto& items = streamsIn["STDIN"];
            auto& cursor = streamCursor["STDIN"];
            if (cursor >= items.size()) error("Input exhausted on stream STDIN");
            // read 取一个按类型解析的字面量；getc 取输入项的第一个字符编码。
            Value value;
            if (mnemonic == "GETC") {
                const std::string text = items[cursor];
                value = literalValue(std::to_string(text.empty() ? 0u
                                                                 : static_cast<unsigned char>(text[0])),
                                     type);
            } else {
                value = literalValue(items[cursor], type);
            }
            ++cursor;
            storeOperand(w[1], value, type);
            continue;
        }
        if (mnemonic == "OUT") {
            need(1);
            result.output.push_back(convertValue(operand(w[1], type), type));
            continue;
        }
        if (mnemonic == "OUTW") {
            need(1);
            result.output.push_back(convertValue(wholeRead(w[1], type), type));
            continue;
        }
        if (mnemonic == "PUTC") {
            need(1);
            result.text.push_back(static_cast<char>(valueLowByte(operand(w[1], type))));
            continue;
        }
        if (mnemonic == "PRINT") {
            need(1);
            result.text += unescapeText(stripQuotes(w[1]));
            continue;
        }
        if (mnemonic == "READW") {
            need(1);
            wholeWrite(w[1], nextInput("STDIN", type));
            continue;
        }
        if (mnemonic == "GETCW") { // 宽类型变量接收一个字符编码
            need(1);
            auto& items = streamsIn["STDIN"];
            auto& cursor = streamCursor["STDIN"];
            if (cursor >= items.size()) error("Input exhausted on stream STDIN");
            const std::string text = items[cursor++];
            wholeWrite(w[1], literalValue(std::to_string(text.empty() ? 0u
                                                                     : static_cast<unsigned char>(text[0])),
                                          type));
            continue;
        }
        if (mnemonic == "RCALL" || mnemonic == "RCALLW") {
            // 运行时例程：宽类型的乘法 / 除法 / 移位 / 比较等按类型求值。
            if (mnemonic == "RCALL") {
                if (w.size() != 5) error("Invalid operands for RCALL");
                assign(w[2], evalTyped(w[1], type, wholeOperand(w[3], type),
                                       wholeOperand(w[4], type)));
            } else {
                if (w.size() != 6) error("Invalid operands for RCALLW");
                Value first, second;
                if (w[1] == "mul")
                    multiplyWide(type, wholeOperand(w[4], type), wholeOperand(w[5], type), second,
                                 first);
                else
                    divideWide(type, wholeOperand(w[4], type), wholeOperand(w[5], type), first,
                               second);
                assign(w[2], first);
                assign(w[3], second);
            }
            continue;
        }
        // ---- 字符串指令（按字节计数与比较） ----
        if (mnemonic == "SCOPY" || mnemonic == "SLEN" || mnemonic == "SCMP" ||
            mnemonic == "SCAT" || mnemonic == "SSUB" || mnemonic == "SFIND") {
            if (mnemonic == "SCOPY") {
                need(2);
                assign(w[1], stringOperand(w[2]));
            } else if (mnemonic == "SLEN") {
                need(2);
                assign(w[1], stringLength(stringOperand(w[2]), slotType(w[1])));
            } else if (mnemonic == "SCMP") {
                need(3);
                assign(w[1], stringCompare(stringOperand(w[2]), stringOperand(w[3]), slotType(w[1])));
            } else if (mnemonic == "SCAT") {
                need(3);
                assign(w[1], stringConcat(stringOperand(w[2]), stringOperand(w[3])));
            } else if (mnemonic == "SSUB") {
                need(4);
                assign(w[1], substring(stringOperand(w[2]),
                                       static_cast<int>(asIndex(operand(w[3], "i32"))),
                                       static_cast<int>(asIndex(operand(w[4], "i32")))));
            } else {
                need(3);
                assign(w[1], stringFind(stringOperand(w[2]), stringOperand(w[3]), slotType(w[1])));
            }
            continue;
        }
        // ---- 流指令：句柄就是流名（STDIN / STDOUT / STDERR / 自定义） ----
        if (mnemonic == "STREAM") {
            need(2);
            Value handle;
            handle.type = "str";
            handle.text = streamHandle(w[2]);
            assign(w[1], handle);
            continue;
        }
        if (mnemonic == "SREAD") {
            need(2);
            const std::string stream = streamHandle(w[2]);
            const std::string valueType = slotType(w[1]);
            Value value;
            if (isStringType(valueType)) {
                auto& items = streamsIn[stream];
                auto& cursor = streamCursor[stream];
                if (cursor >= items.size()) error("Input exhausted on stream " + stream);
                value.type = "str";
                value.text = items[cursor++];
            } else {
                value = nextInput(stream, valueType);
            }
            assign(w[1], value);
            continue;
        }
        if (mnemonic == "SWRITE") {
            need(2);
            const std::string stream = streamHandle(w[1]);
            // 数值操作数可能来自宽变量（分片存储），所以要按整值读取。
            streamWrite(stream, isStringType(type) ? stringOperand(w[2])
                                                   : wholeOperand(w[2], type));
            continue;
        }
        if (mnemonic == "SCLOSE") { need(1); continue; }
        // ---- 向量指令：元素地址 = 基名.下标，元素位数由类型后缀给出 ----
        if (mnemonic == "VLOAD") {
            need(3);
            const std::string element = w[2] + "." + std::to_string(asIndex(operand(w[3], "i32")));
            assign(w[1], wholeRead(element, type));
            continue;
        }
        if (mnemonic == "VSTORE") {
            need(3);
            const std::string element = w[1] + "." + std::to_string(asIndex(operand(w[2], "i32")));
            wholeWrite(element, operand(w[3], type));
            continue;
        }
        if (mnemonic == "VFILL") {
            // VFILL 基名, 长度, 值
            need(3);
            const long long length = asIndex(operand(w[2], "i32"));
            const Value value = operand(w[3], type);
            for (long long index = 0; index < length; ++index)
                wholeWrite(w[1] + "." + std::to_string(index), value);
            continue;
        }
        if (mnemonic == "VCOPY") {
            // VCOPY 目标, 源, 长度
            need(3);
            const long long length = asIndex(operand(w[3], "i32"));
            for (long long index = 0; index < length; ++index)
                wholeWrite(w[1] + "." + std::to_string(index),
                           wholeRead(w[2] + "." + std::to_string(index), type));
            continue;
        }
        if (mnemonic == "VSUM" || mnemonic == "VMAX" || mnemonic == "VMIN") {
            need(3);
            const long long length = asIndex(operand(w[3], "i32"));
            Value accumulator;
            bool first = true;
            for (long long index = 0; index < length; ++index) {
                const Value element = wholeRead(w[2] + "." + std::to_string(index), type);
                if (first) { accumulator = element; first = false; continue; }
                if (mnemonic == "VSUM") {
                    accumulator = evalTyped("+", type, accumulator, element);
                } else {
                    const Value less = evalTyped("<", type, element, accumulator);
                    const Value greater = evalTyped(">", type, element, accumulator);
                    if (!valueIsZero(mnemonic == "VMAX" ? greater : less)) accumulator = element;
                }
            }
            if (first) accumulator = literalValue("0", type);
            assign(w[1], accumulator);
            continue;
        }
        // ---- 系统调用：exit / time / ticks / getpid / rand / brk / putchar / getchar ----
        if (mnemonic == "SYSCALL") {
            need(3);
            const std::string id = w[1];
            const std::string destination = w[2];
            const bool hasDestination = destination != "_" && !destination.empty();
            const std::string resultType = slotType(hasDestination ? destination : "i32");
            const auto setResult = [&](const Value& value) {
                if (hasDestination) assign(destination, value);
            };
            if (id == "exit") {
                result.exitCode = static_cast<int>(asIndex(operand(w[3], "i32")));
                result.streams = streamsOut;
                result.streamText = streamsText;
                return result;
            }
            if (id == "time" || id == "ticks") {
                simulatedTicks += static_cast<long long>(result.steps);
                setResult(literalValue(std::to_string(simulatedTicks), resultType));
            } else if (id == "getpid") {
                setResult(literalValue("1", resultType));
            } else if (id == "rand") {
                randomState = randomState * 6364136223846793005ull + 1442695040888963407ull;
                const auto next = (randomState >> 33) & 0x7FFFFFFFull;
                setResult(literalValue(std::to_string(next), resultType));
            } else if (id == "brk") {
                simulatedTicks += 64;
                setResult(literalValue(std::to_string(1024 + simulatedTicks), resultType));
            } else if (id == "putchar") {
                result.text.push_back(static_cast<char>(valueLowByte(operand(w[3], "u8"))));
            } else if (id == "getchar") {
                auto& items = streamsIn["STDIN"];
                auto& cursor = streamCursor["STDIN"];
                if (cursor >= items.size()) error("Input exhausted on stream STDIN");
                const std::string text = items[cursor++];
                setResult(literalValue(std::to_string(text.empty() ? 0u
                                                                   : static_cast<unsigned char>(text[0])),
                                       resultType));
            } else {
                error("Unknown system call: " + id);
            }
            continue;
        }
        // ---- 外部文件 IO：真实打开 / 读 / 写 / 关闭 ----
        if (mnemonic == "FOPEN") {
            need(3);
            const std::string path = isStringLiteral(w[2]) ? stringLiteralText(w[2])
                                     : isRegister(w[2]) ? stringValue(regs[w[2]])
                                                        : stringValue(readSlot(w[2], "str"));
            const std::string mode = isStringLiteral(w[3]) ? stringLiteralText(w[3])
                                                           : stringValue(readSlot(w[3], "str"));
            std::ios::openmode flags = std::ios::in;
            if (mode == "w") flags = std::ios::out | std::ios::trunc;
            else if (mode == "a") flags = std::ios::out | std::ios::app;
            else if (mode == "r+") flags = std::ios::in | std::ios::out;
            std::fstream file(path, flags);
            if (!file.is_open()) error("Cannot open file: " + path);
            openFiles[path] = std::move(file);
            Value handle;
            handle.type = "str";
            handle.text = path;
            assign(w[1], handle);
            continue;
        }
        if (mnemonic == "FREAD") {
            need(2);
            const std::string path = streamHandle(w[2]);
            const auto found = openFiles.find(path);
            if (found == openFiles.end()) error("File is not open: " + path);
            const std::string valueType = slotType(w[1]);
            Value value;
            if (isStringType(valueType)) {
                std::string line;
                std::getline(found->second, line);
                value.type = "str";
                value.text = line;
            } else {
                std::string token;
                if (!(found->second >> token)) error("Read past end of file: " + path);
                value = literalValue(token, valueType);
            }
            assign(w[1], value);
            continue;
        }
        if (mnemonic == "FWRITE") {
            need(2);
            const std::string path = streamHandle(w[1]);
            const auto found = openFiles.find(path);
            if (found == openFiles.end()) error("File is not open: " + path);
            const Value value = isStringType(type) ? stringOperand(w[2])
                                                   : wholeOperand(w[2], type);
            found->second << displayValue(value);
            if (isStringType(value.type)) found->second << '\n';
            continue;
        }
        if (mnemonic == "FEOF") {
            need(2);
            const std::string path = streamHandle(w[2]);
            const auto found = openFiles.find(path);
            if (found == openFiles.end()) error("File is not open: " + path);
            assign(w[1], boolValue(found->second.eof(), slotType(w[1])));
            continue;
        }
        if (mnemonic == "FCLOSE") {
            need(1);
            const std::string path = streamHandle(w[1]);
            const auto found = openFiles.find(path);
            if (found != openFiles.end()) { found->second.close(); openFiles.erase(found); }
            continue;
        }
        // ---- 数据区三件套：地址是数据区里的字节偏移，读写都是 8 字节小端 ----
        // 与 library/stackvm 的 AddrOf / LoadAt / StoreAt 同一套语义与报错文案。
        if (mnemonic == "ADDR") {
            need(2);
            if (dataAreaSymbols.empty() && dataArea.empty())
                error("没有加载数据区，无法取符号地址：" + w[2]);
            const auto found = dataAreaSymbols.find(w[2]);
            if (found == dataAreaSymbols.end()) error("数据区里没有符号：" + w[2]);
            // 基址按目标变量的类型存放（i64 时窄机器上自动分片）。
            assign(w[1], literalValue(std::to_string(found->second), slotType(w[1])));
            continue;
        }
        if (mnemonic == "LOADAT" || mnemonic == "STOREAT") {
            need(2);
            if (dataArea.empty())
                error(std::string("没有加载数据区，无法按地址") +
                      (mnemonic == "LOADAT" ? "取值" : "写值"));
            // load 是 `LOADAT 目标, 指针`，store 是 `STOREAT 指针, 值`。
            const std::string pointer = mnemonic == "LOADAT" ? w[2] : w[1];
            const long long address = asIndex(wholeOperand(pointer, "u64"));
            if (address < 0 || static_cast<std::size_t>(address) + 8u > dataArea.size())
                error(std::string("数据区越界") + (mnemonic == "LOADAT" ? "读" : "写") +
                      "：地址 " + std::to_string(address));
            if (mnemonic == "LOADAT") {
                // 小端 8 字节 -> 位模式，再按目标变量的类型收进去。
                std::uint64_t bits = 0;
                for (int index = 7; index >= 0; --index) {
                    bits = (bits << 8u) | dataArea[static_cast<std::size_t>(address) +
                                                   static_cast<std::size_t>(index)];
                }
                // 目标变量是浮点时这 8 个字节是 IEEE-754 双精度**位模式**（数据区的每个
                // 槽位就是 8 字节，`(load:f64 …)` / `(store:f64 …)` 带上类型来区分），
                // 所以按 f64 重新解释；整型才按 u64 收。
                assign(w[1], valueFromBits(U128{bits, 0}, isFloatType(slotType(w[1])) ? "f64" : "u64"));
            } else {
                // 值是浮点：8 个字节写它的 IEEE-754 位模式；整型：按自己的类型补齐到 8 字节
                // （有符号类型符号扩展）。
                const U128 bits = isFloatType(type)
                                      ? u128Mask(valueBits(wholeOperand(w[2], "f64")), 64)
                                      : u128Mask(valueBits(convertValue(wholeOperand(w[2], type), "u64")), 64);
                for (unsigned index = 0; index < 8; ++index) {
                    dataArea[static_cast<std::size_t>(address) + index] =
                        static_cast<std::uint8_t>((bits.lo >> (index * 8)) & 0xFFu);
                }
            }
            continue;
        }
        if (loadMnemonics.count(mnemonic) != 0) {
            need(2);
            checkWidth();
            storeOperand(w[1], operand(w[2], type), type);
            continue;
        }
        if (storeMnemonics.count(mnemonic) != 0) {
            need(2);
            checkWidth();
            // 存内存的两个操作数顺序随目标机不同：MIPS/ARM 是 "源, 名字"，
            // 8086 是 "[名字], 源"。按哪边是内存操作数来判断。
            const auto memoryOperand = [&](const std::string& token) {
                if (token.size() >= 2 && token.front() == '[' && token.back() == ']') return true;
                return !isRegister(token) && !isNumericLiteral(token);
            };
            if (memoryOperand(w[1]))
                storeOperand(w[1], operand(w[2], type), type);
            else if (memoryOperand(w[2]))
                storeOperand(w[2], operand(w[1], type), type);
            else
                error("Store without a memory operand: " + w[0]);
            continue;
        }
        if (mnemonic == "ADDC" || mnemonic == "SUBB") {
            checkWidth();
            std::string destination;
            Value left, right;
            if (w.size() == 4) {
                destination = w[1];
                left = operand(w[2], type);
                right = operand(w[3], type);
            } else if (w.size() == 3) {
                destination = w[1];
                left = operand(w[1], type);
                right = operand(w[2], type);
            } else {
                error("Invalid operands for " + mnemonic);
            }
            bool flag = false;
            const Value value = mnemonic == "ADDC" ? addWithCarry(type, left, right, carry, flag)
                                                   : subWithBorrow(type, left, right, carry, flag);
            carry = flag;
            storeOperand(destination, value, type);
            continue;
        }
        if (mnemonic == "NEG") {
            checkWidth();
            if (w.size() == 3 || w.size() == 2) {
                const std::string source = w.size() == 3 ? w[2] : w[1];
                storeOperand(w[1], evalTyped("neg", type, operand(source, type), Value{}), type);
            } else {
                error("Invalid operands for NEG");
            }
            continue;
        }
        if (mnemonic == "NOT" || mnemonic == "MVN" || mnemonic == "NOR") {
            checkWidth();
            if (w.size() == 2) { // 8086 风格：单操作数原地取反（NOT AX）
                storeOperand(w[1], evalTyped("~", type, operand(w[1], type), Value{}), type);
            } else if (w.size() == 3) {
                storeOperand(w[1], evalTyped("~", type, operand(w[2], type), Value{}), type);
            } else if (w.size() == 4) {
                const Value combined =
                    evalTyped("|", type, operand(w[2], type), operand(w[3], type));
                storeOperand(w[1], evalTyped("~", type, combined, Value{}), type);
            } else {
                error("Invalid operands for " + mnemonic);
            }
            continue;
        }
        if (aluMnemonics.count(mnemonic) != 0) {
            checkWidth();
            if (w.size() == 4)
                storeOperand(w[1], evalTyped(aluMnemonics.at(mnemonic), type, operand(w[2], type),
                                             operand(w[3], type)),
                             type);
            else if (w.size() == 3)
                storeOperand(w[1], evalTyped(aluMnemonics.at(mnemonic), type, operand(w[1], type),
                                             operand(w[2], type)),
                             type);
            else
                error("Invalid operands for " + mnemonic);
            continue;
        }
        if (mnemonic == "SLL" || mnemonic == "SHL" || mnemonic == "LSL" || mnemonic == "SRA" ||
            mnemonic == "SAR" || mnemonic == "ASR" || mnemonic == "SRL" || mnemonic == "SHR" ||
            mnemonic == "LSR") {
            checkWidth();
            const bool left = mnemonic == "SLL" || mnemonic == "SHL" || mnemonic == "LSL";
            const bool logical = mnemonic == "SRL" || mnemonic == "SHR" || mnemonic == "LSR";
            const std::string shiftType =
                left || isUnsignedType(type)
                    ? type
                    : (logical ? "u" + std::to_string(typeBits(type))
                               : "i" + std::to_string(typeBits(type)));
            const std::string operation = left ? "<<" : ">>";
            if (w.size() == 4)
                storeOperand(w[1], evalTyped(operation, shiftType, operand(w[2], shiftType),
                                             operand(w[3], shiftType)),
                             type);
            else if (w.size() == 3)
                storeOperand(w[1], evalTyped(operation, shiftType, operand(w[1], shiftType),
                                             operand(w[2], shiftType)),
                             type);
            else
                error("Invalid operands for " + mnemonic);
            continue;
        }
        if (mnemonic == "UMULL" || mnemonic == "SMULL") {
            checkWidth();
            Value low, high;
            multiplyWide(type, operand(w[3], type), operand(w[4], type), high, low);
            storeOperand(w[1], low, type);
            storeOperand(w[2], high, type);
            continue;
        }
        if (mnemonic == "MULT" || mnemonic == "MULTU") {
            checkWidth();
            Value low, high;
            multiplyWide(type, operand(w[1], type), operand(w[2], type), high, low);
            regs[pairLow] = convertValue(low, type);
            regs[pairHigh] = convertValue(high, type);
            continue;
        }
        if (mnemonic == "MUL" || mnemonic == "IMUL" || mnemonic == "MULU") {
            checkWidth();
            if (w.size() == 4) {
                storeOperand(w[1], evalTyped("*", type, operand(w[2], type), operand(w[3], type)),
                             type);
            } else if (w.size() == 3) {
                storeOperand(w[1], evalTyped("*", type, operand(w[1], type), operand(w[2], type)),
                             type);
            } else if (w.size() == 2) {
                // 8086 风格：AX 与操作数相乘，结果进固定寄存器对 DX:AX。
                Value low, high;
                multiplyWide(type, regs[pairLow], operand(w[1], type), high, low);
                regs[pairLow] = convertValue(low, type);
                regs[pairHigh] = convertValue(high, type);
            } else {
                error("Invalid operands for " + mnemonic);
            }
            continue;
        }
        if (mnemonic == "MLS") {
            checkWidth();
            const Value product = evalTyped("*", type, operand(w[3], type), operand(w[4], type));
            storeOperand(w[1], evalTyped("-", type, operand(w[2], type), product), type);
            continue;
        }
        if (mnemonic == "MFHI") { need(1); storeOperand(w[1], regs[pairHigh], type); continue; }
        if (mnemonic == "MFLO") { need(1); storeOperand(w[1], regs[pairLow], type); continue; }
        if (mnemonic == "DIV" || mnemonic == "DIVU" || mnemonic == "IDIV" || mnemonic == "SDIV" ||
            mnemonic == "UDIV") {
            checkWidth();
            if (w.size() == 4) { // ARM：UDIV q, a, b
                storeOperand(w[1], evalTyped("/", type, operand(w[2], type), operand(w[3], type)),
                             type);
            } else if (w.size() == 3) { // MIPS：DIV a, b -> LO=商、HI=余数
                Value quotient, remainder;
                divideWide(type, operand(w[1], type), operand(w[2], type), quotient, remainder);
                regs[pairLow] = convertValue(quotient, type);
                regs[pairHigh] = convertValue(remainder, type);
            } else if (w.size() == 2) { // 8086：AX / 操作数 -> AX=商、DX=余数
                Value quotient, remainder;
                divideWide(type, regs[pairLow], operand(w[1], type), quotient, remainder);
                regs[pairLow] = convertValue(quotient, type);
                regs[pairHigh] = convertValue(remainder, type);
            } else {
                error("Invalid operands for " + mnemonic);
            }
            continue;
        }
        error("Unknown instruction: " + w[0]);
    }
    error("Assembly ended without HALT");
}

} // namespace

Execution simulate(const std::string& assembly, const std::vector<std::string>& input,
                   std::size_t stepLimit) {
    std::map<std::string, std::vector<std::string>> streams;
    streams["STDIN"] = input;
    return runSimulation(assembly, streams, stepLimit);
}

Execution simulate(const std::string& assembly,
                   const std::map<std::string, std::vector<std::string>>& streams,
                   std::size_t stepLimit) {
    return runSimulation(assembly, streams, stepLimit);
}

Execution simulate(const std::string& assembly, std::initializer_list<std::int32_t> input,
                   std::size_t stepLimit) {
    std::vector<std::string> text;
    for (const auto value : input) text.push_back(std::to_string(value));
    return simulate(assembly, text, stepLimit);
}

} // namespace transfer
