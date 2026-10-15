#include "typed_ir.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace t1t {
namespace {

std::string trim(const std::string& text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}

bool isName(const std::string& text) {
    if (text.empty() || text == "_") return false;
    if (!(std::isalpha(static_cast<unsigned char>(text[0])) || text[0] == '_')) return false;
    for (unsigned char c : text) {
        if (!(std::isalnum(c) || c == '_')) return false;
    }
    return true;
}

bool isIntegerLiteral(const std::string& text) {
    if (text.empty()) return false;
    std::size_t i = (text[0] == '-' || text[0] == '+') ? 1 : 0;
    if (i >= text.size()) return false;
    for (std::size_t k = i; k < text.size(); ++k) {
        if (!std::isdigit(static_cast<unsigned char>(text[k]))) return false;
    }
    return true;
}

/** `mul:u16` → `mul`；`:=` 自己以冒号开头，原样返回。 */
std::string baseOp(const std::string& op) {
    if (op.empty() || op[0] == ':') return op;
    const auto colon = op.find(':');
    return colon == std::string::npos ? op : op.substr(0, colon);
}

/** `mul:u16` → `u16`；没有类型后缀返回空。 */
std::string suffixType(const std::string& op) {
    if (op.empty() || op[0] == ':') return {};
    const auto colon = op.find(':');
    return colon == std::string::npos ? std::string() : op.substr(colon + 1);
}

bool cleanOperand(const std::string& operand) {
    if (operand.empty() || operand == "_") return true;
    if (isName(operand) || isIntegerLiteral(operand)) return true;
    return operand.front() == '[' && operand.back() == ']';   // 内存操作数
}

/** 去掉内存操作数里的位数：`[x:u16]` → `[x]`。 */
std::string plainOperand(const std::string& operand) {
    if (operand.size() < 2 || operand.front() != '[' || operand.back() != ']') return operand;
    const std::string inner = operand.substr(1, operand.size() - 2);
    const auto colon = inner.find(':');
    if (colon == std::string::npos) return operand;
    return "[" + inner.substr(0, colon) + "]";
}

std::vector<std::string> splitOperands(const std::string& text) {
    std::vector<std::string> out;
    std::string current;
    for (char c : text) {
        if (c == ',') {
            const std::string item = trim(current);
            if (!item.empty()) out.push_back(item);
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    const std::string item = trim(current);
    if (!item.empty()) out.push_back(item);
    return out;
}

} // namespace

std::string TypedInstruction::memoryType(const std::string& operand) {
    if (operand.size() < 2 || operand.front() != '[' || operand.back() != ']') return {};
    const std::string inner = operand.substr(1, operand.size() - 2);
    const auto colon = inner.find(':');
    return colon == std::string::npos ? std::string() : inner.substr(colon + 1);
}

std::string effectiveType(const TypedInstruction& ins) {
    return ins.type.empty() ? "i16" : ins.type;
}

Lowering lowerQuads(const transfer::Program& program) {
    Lowering lowering;
    std::map<std::string, std::string> declared;          // 变量 → 声明类型
    for (const auto& quad : program) {
        if (baseOp(quad.op) != "decl") continue;
        if (isName(quad.result)) declared[quad.result] = quad.a;     // (decl, i16, global, x)
    }
    auto typeOf = [&](const std::string& explicitType, const std::string& result,
                      const std::string& first) {
        if (!explicitType.empty()) return explicitType;
        const auto byResult = declared.find(result);
        if (byResult != declared.end()) return byResult->second;
        const auto byFirst = declared.find(first);
        if (byFirst != declared.end()) return byFirst->second;
        return std::string("i16");                        // transfer 的缺省
    };
    auto skip = [&](const std::string& op) {
        ++lowering.skipped;
        ++lowering.skippedOps[op];
    };
    for (const auto& quad : program) {
        const std::string op = baseOp(quad.op);
        if (op == "decl") {
            if (isName(quad.result)) {
                lowering.program.declarations.emplace_back(quad.result, quad.a);
                ++lowering.mapped;
            } else {
                skip(quad.op);
            }
            continue;
        }
        if (!quad.extra.empty() && quad.extra != "_") { skip(quad.op); continue; }   // 双结果
        TypedInstruction ins;
        const std::string explicitType = suffixType(quad.op);
        if (op == "label") {
            if (!isName(quad.result)) { skip(quad.op); continue; }
            ins.op = "label";
            ins.dst = quad.result;
        } else if (op == ":=") {
            ins.op = "mov";
            ins.type = typeOf(explicitType, quad.result, quad.a);
            ins.dst = quad.result;
            ins.src1 = quad.a;
        } else if (op == "+" || op == "-" || op == "*" || op == "/") {
            ins.op = op == "+" ? "add" : op == "-" ? "sub" : op == "*" ? "mul" : "div";
            ins.type = typeOf(explicitType, quad.result, quad.a);
            ins.dst = quad.result;
            ins.src1 = quad.a;
            ins.src2 = quad.b;
        } else if (op == "<" || op == "<=" || op == ">" || op == ">=" || op == "==" || op == "!=") {
            // 比较：结果 0/1，运算宽度看操作数（`<=:i32` 的写法与 transfer 一致）
            ins.op = op == "<" ? "lt" : op == "<=" ? "le" : op == ">" ? "gt" : op == ">=" ? "ge"
                              : op == "==" ? "eq" : "ne";
            ins.type = typeOf(explicitType, quad.a, quad.a);
            ins.dst = quad.result;
            ins.src1 = quad.a;
            ins.src2 = quad.b;
        } else if (op == "j") {
            if (!isName(quad.result)) { skip(quad.op); continue; }
            ins.op = "jmp";
            ins.src1 = quad.result;
        } else if (op == "jz" || op == "jnz") {
            if (!isName(quad.result)) { skip(quad.op); continue; }
            ins.op = op;
            ins.type = typeOf(explicitType, quad.a, quad.a);
            ins.src1 = quad.a;
            ins.src2 = quad.result;
        } else if (op == "write") {
            ins.op = "print";
            ins.type = typeOf(explicitType, quad.a, quad.a);
            ins.src1 = quad.a;
        } else if (op == "halt") {
            ins.op = "halt";
        } else if (op == "ld" || op == "load") {
            ins.op = "ld";
            ins.type = typeOf(explicitType, quad.result, quad.a);
            ins.dst = quad.result;
            ins.src1 = quad.a;
        } else if (op == "st" || op == "store") {
            ins.op = "st";
            ins.type = typeOf(explicitType, "", quad.b);
            ins.src1 = quad.a;
            ins.src2 = quad.b;
        } else {
            skip(quad.op);
            continue;
        }
        if (!cleanOperand(ins.dst) || !cleanOperand(ins.src1) || !cleanOperand(ins.src2)) {
            skip(quad.op);
            continue;
        }
        lowering.program.instructions.push_back(ins);
        ++lowering.mapped;
    }
    lowering.text = print(lowering.program);
    lowering.plain = toPlainText(lowering.program);
    return lowering;
}

std::string print(const TypedProgram& program) {
    std::ostringstream out;
    for (const auto& declaration : program.declarations) {
        out << "decl " << declaration.first << ", " << declaration.second << "\n";
    }
    for (const auto& ins : program.instructions) {
        if (ins.op == "label") { out << "label " << ins.dst << "\n"; continue; }
        const std::string type = effectiveType(ins);
        out << "  " << ins.op << ":" << type;
        std::vector<std::string> operands;
        if (ins.isPhi()) {
            operands.push_back(ins.dst);
            operands.insert(operands.end(), ins.phiArgs.begin(), ins.phiArgs.end());
        } else if (ins.op == "st") {
            operands.push_back(ins.src1);
            operands.push_back(ins.src2);
        } else if (ins.op == "halt") {
            // 不带操作数
        } else if (ins.isBranch()) {
            operands.push_back(ins.src1);      // 条件值在前
            operands.push_back(ins.src2);       // 跳转标号在后
        } else if (ins.op == "jmp") {
            operands.push_back(ins.src1);
        } else {
            if (!ins.dst.empty()) operands.push_back(ins.dst);
            if (!ins.src1.empty()) operands.push_back(ins.src1);
            if (!ins.src2.empty()) operands.push_back(ins.src2);
        }
        for (std::size_t i = 0; i < operands.size(); ++i) out << (i ? ", " : " ") << operands[i];
        out << "\n";
    }
    return out.str();
}

bool parse(const std::string& text, TypedProgram& out, std::vector<std::string>& errors) {
    out = TypedProgram();
    errors.clear();
    std::istringstream in(text);
    std::size_t line = 0;
    for (std::string raw; std::getline(in, raw);) {
        ++line;
        std::string body = raw;
        for (const char* marker : {";", "#", "//"}) {
            const auto pos = body.find(marker);
            if (pos != std::string::npos) body = body.substr(0, pos);
        }
        body = trim(body);
        if (body.empty()) continue;
        const auto space = body.find_first_of(" \t");
        const std::string head = space == std::string::npos ? body : body.substr(0, space);
        const std::string rest = space == std::string::npos ? std::string() : trim(body.substr(space + 1));
        if (head == "decl") {
            const auto parts = splitOperands(rest);
            if (parts.size() != 2) {
                errors.push_back("第 " + std::to_string(line) + " 行：decl 需要 名字, 类型");
                continue;
            }
            out.declarations.emplace_back(parts[0], parts[1]);
            continue;
        }
        if (head == "label") {
            TypedInstruction ins;
            ins.op = "label";
            ins.dst = rest;
            ins.line = line;
            out.instructions.push_back(ins);
            continue;
        }
        TypedInstruction ins;
        const auto colon = head.find(':');
        if (colon == std::string::npos) {
            ins.op = head;
        } else {
            ins.op = head.substr(0, colon);
            ins.type = head.substr(colon + 1);
        }
        ins.line = line;
        const auto operands = splitOperands(rest);
        if (ins.op == "halt") {
            out.instructions.push_back(ins);
            continue;
        }
        if (ins.op == "jmp") {
            if (operands.size() != 1) { errors.push_back("第 " + std::to_string(line) + " 行：jmp 需要一个标号"); continue; }
            ins.src1 = operands[0];
        } else if (ins.op == "call") {          // 调用：一个操作数（被调用者）；返回值由 ret 带走
            if (operands.size() != 1) { errors.push_back("第 " + std::to_string(line) + " 行：call 需要一个操作数"); continue; }
            ins.src1 = operands[0];
        } else if (ins.op == "ret") {           // 返回：0 或 1 个操作数
            if (operands.size() > 1) { errors.push_back("第 " + std::to_string(line) + " 行：ret 最多一个操作数"); continue; }
            if (!operands.empty()) ins.src1 = operands[0];
        } else if (ins.op == "st") {
            if (operands.size() != 2) { errors.push_back("第 " + std::to_string(line) + " 行：st 需要两个操作数"); continue; }
            ins.src1 = operands[0];
            ins.src2 = operands[1];
        } else if (ins.op == "print") {
            if (operands.size() != 1) { errors.push_back("第 " + std::to_string(line) + " 行：print 需要一个操作数"); continue; }
            ins.src1 = operands[0];
        } else if (ins.op == "jz" || ins.op == "jnz") {
            if (operands.size() != 2) { errors.push_back("第 " + std::to_string(line) + " 行：条件跳转需要两个操作数"); continue; }
            ins.src1 = operands[0];
            ins.src2 = operands[1];
        } else if (ins.op == "phi") {
            if (operands.size() < 2) { errors.push_back("第 " + std::to_string(line) + " 行：phi 至少要两个操作数"); continue; }
            ins.dst = operands[0];
            ins.phiArgs.assign(operands.begin() + 1, operands.end());
        } else if (ins.op == "mov" || ins.op == "ld") {
            if (operands.size() != 2) { errors.push_back("第 " + std::to_string(line) + " 行：" + ins.op + " 需要两个操作数"); continue; }
            ins.dst = operands[0];
            ins.src1 = operands[1];
        } else if (ins.op == "load") {          // 与 ld 同义（模块的老写法）
            if (operands.size() != 2) { errors.push_back("第 " + std::to_string(line) + " 行：load 需要两个操作数"); continue; }
            ins.dst = operands[0];
            ins.src1 = operands[1];
        } else if (ins.op == "store") {         // 与 st 同义；内存操作数在 src2，值在 src1
            if (operands.size() != 2) { errors.push_back("第 " + std::to_string(line) + " 行：store 需要两个操作数"); continue; }
            ins.src1 = operands[1];
            ins.src2 = operands[0];
          } else if (ins.op == "add" || ins.op == "sub" || ins.op == "mul" || ins.op == "div") {
              if (operands.size() != 3) { errors.push_back("第 " + std::to_string(line) + " 行：该运算需要三个操作数"); continue; }
              ins.dst = operands[0];
              ins.src1 = operands[1];
              ins.src2 = operands[2];
          } else if (ins.op == "lt" || ins.op == "le" || ins.op == "gt" || ins.op == "ge" ||
                     ins.op == "eq" || ins.op == "ne") {
              // 比较：dst 存 0/1，两个操作数（宽度在 :类型 后缀里）
              if (operands.size() != 3) { errors.push_back("第 " + std::to_string(line) + " 行：比较需要三个操作数"); continue; }
              ins.dst = operands[0];
              ins.src1 = operands[1];
              ins.src2 = operands[2];
          } else {
            errors.push_back("第 " + std::to_string(line) + " 行：不认识的指令 " + ins.op);
            continue;
        }
        out.instructions.push_back(ins);
    }
    if (out.instructions.empty()) errors.push_back("程序是空的");
    return errors.empty();
}

std::string toPlainText(const TypedProgram& program) {
    std::ostringstream out;
    for (const auto& ins : program.instructions) {
        if (ins.op == "label") { out << "label " << ins.dst << "\n"; continue; }
        std::string op = ins.op;
        if (op == "ld") op = "load";
        if (op == "st") op = "store";
        out << "  " << op;
        std::vector<std::string> operands;
        if (ins.isPhi()) {
            operands.push_back(ins.dst);
            for (const auto& arg : ins.phiArgs) operands.push_back(plainOperand(arg));
        } else if (ins.op == "st") {
            operands.push_back(plainOperand(ins.src1));
            operands.push_back(plainOperand(ins.src2));
        } else if (ins.op == "halt") {
            // 不带操作数
        } else if (ins.isBranch()) {
            operands.push_back(plainOperand(ins.src1));
            operands.push_back(ins.src2);
        } else if (ins.op == "jmp") {
            operands.push_back(ins.src1);
        } else {
            if (!ins.dst.empty()) operands.push_back(plainOperand(ins.dst));
            if (!ins.src1.empty()) operands.push_back(plainOperand(ins.src1));
            if (!ins.src2.empty()) operands.push_back(plainOperand(ins.src2));
        }
        for (std::size_t i = 0; i < operands.size(); ++i) out << (i ? ", " : " ") << operands[i];
        out << "\n";
    }
    return out.str();
}

std::map<std::string, std::size_t> widthHistogram(const TypedProgram& program) {
    std::map<std::string, std::size_t> histogram;
    for (const auto& ins : program.instructions) {
        if (ins.op == "label" || ins.op == "halt" || ins.op == "jmp") continue;
        ++histogram[effectiveType(ins)];
    }
    return histogram;
}

std::size_t countInstructions(const std::string& text) {
    std::istringstream in(text);
    std::size_t count = 0;
    for (std::string line; std::getline(in, line);) {
        const auto first = line.find_first_not_of(" \t");
        if (first == std::string::npos) continue;
        if (line.compare(first, 5, "label") == 0) continue;
        ++count;
    }
    return count;
}

std::string stripPrint(const std::string& text) {
    std::istringstream in(text);
    std::ostringstream out;
    for (std::string line; std::getline(in, line);) {
        const auto first = line.find_first_not_of(" \t");
        if (first != std::string::npos && line.compare(first, 5, "print") == 0) continue;
        out << line << "\n";
    }
    return out.str();
}

// ---------------------------------------------------------------- 统一 CFG

std::size_t Program::instructionCount() const {
    std::size_t count = 0;
    for (const auto& block : blocks) count += block.code.size();
    return count;
}

void connectCfg(Program& program) {
    for (auto& block : program.blocks) {
        block.successors.clear();
        block.predecessors.clear();
    }
    std::map<std::string, std::size_t> index;
    for (std::size_t i = 0; i < program.blocks.size(); ++i) {
        if (!program.blocks[i].label.empty()) index[program.blocks[i].label] = i;
    }
    for (std::size_t i = 0; i < program.blocks.size(); ++i) {
        auto add = [&](std::size_t target) {
            auto& successors = program.blocks[i].successors;
            if (std::find(successors.begin(), successors.end(), target) == successors.end()) {
                successors.push_back(target);
            }
        };
        bool falls = true;
        const auto& code = program.blocks[i].code;
        if (!code.empty()) {
            const auto& last = code.back();
            if (last.op == "jmp") {
                const auto found = index.find(last.src1);
                if (found != index.end()) add(found->second);
                falls = false;
            } else if (last.isBranch()) {
                const auto found = index.find(last.src2);
                if (found != index.end()) add(found->second);
            } else if (last.op == "halt" || last.op == "ret") {
                falls = false;
            }
        }
        if (falls && i + 1 < program.blocks.size()) add(i + 1);
    }
    for (std::size_t i = 0; i < program.blocks.size(); ++i) {
        for (std::size_t s : program.blocks[i].successors) {
            program.blocks[s].predecessors.push_back(i);
        }
    }
}

bool parseProgram(const std::string& text, Program& out, std::vector<std::string>& errors) {
    TypedProgram flat;
    if (!parse(text, flat, errors)) {
        out = Program();
        return false;
    }
    out.declarations = flat.declarations;
    out.blocks.clear();
    Block current;
    auto flush = [&]() {
        if (!current.label.empty() || !current.code.empty()) {
            out.blocks.push_back(current);
            current = Block();
        }
    };
    for (const auto& ins : flat.instructions) {
        if (ins.op == "label") {
            flush();
            current.label = ins.dst;
            continue;
        }
        // 终结指令之后自动开新块：与老的三个模块的约定一致。
        if (!current.code.empty() && current.code.back().isTerminator()) flush();
        current.code.push_back(ins);
    }
    flush();
    if (out.blocks.empty()) {
        errors.push_back("程序是空的");
        return false;
    }
    connectCfg(out);
    // φ 的实参按前驱个数对齐。
    for (auto& block : out.blocks) {
        for (auto& ins : block.code) {
            if (!ins.isPhi()) continue;
            ins.phiArgs.resize(block.predecessors.size(), "undef");
        }
    }
    return true;
}

std::string plainText(const Program& program) {
    TypedProgram flat;
    flat.declarations = program.declarations;
    for (const auto& block : program.blocks) {
        TypedInstruction label;
        label.op = "label";
        label.dst = block.label;
        if (!block.label.empty()) flat.instructions.push_back(label);
        for (const auto& ins : block.code) flat.instructions.push_back(ins);
    }
    return toPlainText(flat);
}

} // namespace t1t
