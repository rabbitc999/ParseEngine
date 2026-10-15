#include "hydra86.hpp"

#include <cctype>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <vector>

#ifdef T1T_WITH_HYDRA86
#include "pvm/cli/embed.hpp"
#endif

namespace transfer {
namespace {

bool identifier(const std::string& text) {
    if (text.empty() || text == "_") return false;
    const unsigned char first = static_cast<unsigned char>(text.front());
    if (!(std::isalpha(first) || text.front() == '_')) return false;
    for (unsigned char c : text) {
        if (!(std::isalnum(c) || c == '_' || c == '.' || c == '$')) return false;
    }
    return true;
}

std::string labelName(const std::string& name) {
    std::string out = "L_";
    for (unsigned char c : name) out += std::isalnum(c) || c == '_' ? static_cast<char>(c) : '_';
    return out;
}

std::string variableName(const std::string& name, std::map<std::string, std::string>& names) {
    const auto found = names.find(name);
    if (found != names.end()) return found->second;
    const std::string made = "v_" + std::to_string(names.size());
    names.emplace(name, made);
    return made;
}

bool valueField(const Quad& quad, const std::string& field) {
    const std::string base = opBase(quad.op);
    if (field == "a" && (base == "label" || base == "j" || base == "call" || base == "print"))
        return false;
    return true;
}

void requireScalarType(const std::string& type) {
    const TypeInfo& info = typeInfo(type);
    if (info.kind == TypeKind::Float || info.kind == TypeKind::String || info.bits > 64)
        throw std::runtime_error("Hydra86 后端只支持不超过 64 位的整数，遇到类型 " + type);
}

} // namespace

std::string generateHydra86(const Program& program) {
    validate(program);
    const auto types = variableTypes(program);
    for (const auto& item : types) requireScalarType(item.second);

    std::map<std::string, std::string> variables;
    std::map<std::string, std::string> constants;
    std::set<std::string> globals;
    std::vector<std::string> parameterStages(program.size());
    auto addValue = [&](const std::string& value) {
        if (identifier(value)) variableName(value, variables);
        else if (isIntegerLiteral(value) && !constants.count(value))
            constants.emplace(value, "__t1t_const_" + std::to_string(constants.size()));
    };
    for (std::size_t i = 0; i < program.size(); ++i) {
        const auto& quad = program[i];
        const std::string base = opBase(quad.op);
        if (!opType(quad.op).empty()) requireScalarType(opType(quad.op));
        if (base != "decl" && valueField(quad, "a")) addValue(quad.a);
        if (base != "decl") addValue(quad.b);
        if (base == "decl" || base == ":=" || base == "neg" || base == "odd" || base == "~" ||
            base == "cvt" || base == "+" || base == "-" || base == "*" || base == "/" ||
            base == "%" || base == "==" || base == "!=" || base == "<" || base == "<=" ||
            base == ">" || base == ">=" || base == "<<" || base == ">>" || base == "&" ||
            base == "|" || base == "^" || base == "param" || base == "call") {
            if (identifier(quad.result)) variableName(quad.result, variables);
        }
        if (base == "decl" && quad.b == "global") globals.insert(quad.result);
        if (base == "param") parameterStages[i] = "__t1t_arg_" + std::to_string(i);
    }

    std::ostringstream data;
    data << ".data\n"
            "__t1t_print_buf db 32 dup(0)\n"
            "__t1t_newline db 10\n";
    for (const auto& item : constants) data << item.second << " dq " << item.first << "\n";
    for (const auto& item : variables) data << item.second << " dq 0 ; " << item.first << "\n";
    for (const auto& stage : parameterStages) if (!stage.empty()) data << stage << " dq 0\n";

    std::ostringstream code;
    code << "\n.code\n_start:\n";
    std::size_t branchId = 0;
    auto load = [&](const std::string& value, const char* reg) {
        if (identifier(value)) code << "    mov " << reg << ", [" << variables.at(value) << "]\n";
        else if (isIntegerLiteral(value)) code << "    mov " << reg << ", [" << constants.at(value) << "]\n";
        else throw std::runtime_error("Hydra86 后端不支持操作数 " + value);
    };
    auto store = [&](const std::string& name, const char* reg) {
        if (name != "_") code << "    mov [" << variables.at(name) << "], " << reg << "\n";
    };
    auto exit = [&]() {
        code << "    mov r1, 0\n    mov r2, 0\n    syscall\n";
    };

    bool hasHalt = false;
    std::vector<std::pair<std::string, std::string>> pendingParameters;
    for (std::size_t quadIndex = 0; quadIndex < program.size(); ++quadIndex) {
        const auto& quad = program[quadIndex];
        const std::string base = opBase(quad.op);
        if (base == "decl") continue;
        if (base == "label") { code << labelName(quad.result) << ":\n"; continue; }
        if (base == ":=" || base == "cvt") { load(quad.a, "r8"); store(quad.result, "r8"); continue; }
        if (base == "neg") {
            load(quad.a, "r9");
            code << "    mov r8, 0\n    sub r8, r9\n";
            store(quad.result, "r8");
            continue;
        }
        if (base == "~") {
            load(quad.a, "r8");
            code << "    mov r9, -1\n    xor r8, r9\n";
            store(quad.result, "r8");
            continue;
        }
        if (base == "odd") {
            load(quad.a, "r8");
            code << "    mov r9, 1\n    and r8, r9\n";
            store(quad.result, "r8");
            continue;
        }
        if (base == "j") { code << "    jmp " << labelName(quad.result) << "\n"; continue; }
        if (base == "jz" || base == "jnz") {
            load(quad.a, "r8");
            code << "    cmp r8, 0\n    " << (base == "jz" ? "je " : "jne ")
                 << labelName(quad.result) << "\n";
            continue;
        }
        if (base == "param") {
            load(quad.a, "r8");
            code << "    mov [" << parameterStages[quadIndex] << "], r8\n";
            pendingParameters.emplace_back(quad.result, parameterStages[quadIndex]);
            continue;
        }
        if (base == "call") {
            for (const auto& item : variables) {
                if (globals.count(item.first)) continue;
                code << "    mov r8, [" << item.second << "]\n    push r8\n";
            }
            for (const auto& parameter : pendingParameters) {
                code << "    mov r8, [" << parameter.second << "]\n    mov ["
                     << variables.at(parameter.first) << "], r8\n";
            }
            pendingParameters.clear();
            code << "    call " << labelName(quad.a) << "\n";
            code << "    mov r12, r1\n";
            for (auto item = variables.rbegin(); item != variables.rend(); ++item) {
                if (globals.count(item->first)) continue;
                code << "    pop r8\n    mov [" << item->second << "], r8\n";
            }
            store(quad.result, "r12");
            continue;
        }
        if (base == "ret") { load(quad.a, "r1"); code << "    ret\n"; continue; }
        if (base == "write") {
            load(quad.a, "r2");
            code << "    call __t1t_print_i64\n";
            continue;
        }
        if (base == "halt") { exit(); hasHalt = true; continue; }
        if (base == "==" || base == "!=" || base == "<" || base == "<=" ||
            base == ">" || base == ">=") {
            load(quad.a, "r8"); load(quad.b, "r9");
            const std::string yes = "__t1t_cmp_yes_" + std::to_string(branchId);
            const std::string done = "__t1t_cmp_done_" + std::to_string(branchId++);
            const std::map<std::string, std::string> jump{{"==", "je"}, {"!=", "jne"},
                {"<", "jl"}, {"<=", "jle"}, {">", "jg"}, {">=", "jge"}};
            code << "    cmp r8, r9\n    mov r10, 0\n    " << jump.at(base) << " " << yes
                 << "\n    jmp " << done << "\n" << yes << ":\n    mov r10, 1\n"
                 << done << ":\n";
            store(quad.result, "r10");
            continue;
        }
        const std::map<std::string, std::string> binary{{"+", "add"}, {"-", "sub"},
            {"*", "mul"}, {"/", "div"}, {"%", "mod"}, {"<<", "shl"},
            {">>", "shr"}, {"&", "and"}, {"|", "or"}, {"^", "xor"}};
        const auto instruction = binary.find(base);
        if (instruction != binary.end()) {
            load(quad.a, "r8"); load(quad.b, "r9");
            if ((base == "/" || base == "%") && !isUnsignedType(quadType(quad, types))) {
                const std::string id = std::to_string(branchId++);
                code << "    mov r10, 0\n"
                     << "    cmp r8, 0\n    jge __t1t_div_lhs_" << id << "\n"
                     << "    mov r10, 1\n    mov r11, 0\n    sub r11, r8\n    mov r8, r11\n"
                     << "__t1t_div_lhs_" << id << ":\n"
                     << "    cmp r9, 0\n    jge __t1t_div_rhs_" << id << "\n";
                if (base == "/")
                    code << "    mov r11, 1\n    xor r10, r11\n";
                code << "    mov r11, 0\n    sub r11, r9\n    mov r9, r11\n"
                     << "__t1t_div_rhs_" << id << ":\n"
                     << "    " << instruction->second << " r8, r9\n"
                     << "    cmp r10, 0\n    je __t1t_div_done_" << id << "\n"
                     << "    mov r11, 0\n    sub r11, r8\n    mov r8, r11\n"
                     << "__t1t_div_done_" << id << ":\n";
                store(quad.result, "r8");
                continue;
            }
            code << "    " << instruction->second << " r8, r9\n";
            store(quad.result, "r8");
            continue;
        }
        throw std::runtime_error("Hydra86 后端尚未映射四元式指令 " + quad.op);
    }
    if (!hasHalt) exit();

    code << "\n__t1t_print_i64:\n"
            "    push r1\n    push r2\n    push r3\n    push r4\n"
            "    push r5\n    push r6\n    push r7\n"
            "    mov r5, r2\n    mov r6, 0\n    cmp r5, 0\n    jge __t1t_print_abs\n"
            "    mov r6, 1\n    mov r7, 0\n    sub r7, r5\n    mov r5, r7\n"
            "__t1t_print_abs:\n"
            "    mov r4, __t1t_print_buf\n    add r4, 31\n    mov r3, 0\n    mov r7, 10\n"
            "__t1t_print_digit:\n"
            "    mov r2, r5\n    mod r2, r7\n    add r2, 48\n"
            "    mov byte [r4], r2\n    dec r4\n    inc r3\n    div r5, r7\n"
            "    cmp r5, 0\n    jne __t1t_print_digit\n"
            "    cmp r6, 0\n    je __t1t_print_emit\n"
            "    mov r2, 45\n    mov byte [r4], r2\n    dec r4\n    inc r3\n"
            "__t1t_print_emit:\n"
            "    inc r4\n    mov r1, 0x41\n    mov r2, r4\n    syscall\n"
            "    mov r1, 0x41\n    mov r2, __t1t_newline\n    mov r3, 1\n    syscall\n"
            "    pop r7\n    pop r6\n    pop r5\n    pop r4\n"
            "    pop r3\n    pop r2\n    pop r1\n    ret\n";
    return data.str() + code.str();
}

bool hydra86Embedded() {
#ifdef T1T_WITH_HYDRA86
    return true;
#else
    return false;
#endif
}

Hydra86Binary compileHydra86(const std::string& assembly) {
    Hydra86Binary result;
#ifdef T1T_WITH_HYDRA86
    const Hydra86::EmbedImage image = Hydra86::assemble(assembly);
    result.available = true;
    result.ok = image.ok();
    result.bytes = image.m_bytes;
    result.error = image.m_error;
#else
    (void)assembly;
    result.error = "当前构建未嵌入 Hydra86；请配置 T1T_HYDRA86_ROOT 后重新构建";
#endif
    return result;
}

Hydra86Execution runHydra86(const std::string& assembly) {
    Hydra86Execution result;
#ifdef T1T_WITH_HYDRA86
    std::ostringstream output;
    Hydra86::EmbedOptions options;
    options.m_host = true;
    options.m_hostOut = &output;
    const Hydra86::EmbedRun run = Hydra86::runAssembly(assembly, options);
    result.available = true;
    result.ok = run.ok();
    result.exitCode = run.m_exitCode;
    result.cycles = run.m_cycles;
    result.output = output.str();
    result.psf = run.m_psf;
    result.error = run.m_error;
#else
    (void)assembly;
    result.error = "当前构建未嵌入 Hydra86；请配置 T1T_HYDRA86_ROOT 后重新构建";
#endif
    return result;
}

Hydra86Execution runHydra86Binary(const std::vector<std::uint8_t>& image) {
    Hydra86Execution result;
#ifdef T1T_WITH_HYDRA86
    std::ostringstream output;
    Hydra86::EmbedOptions options;
    options.m_host = true;
    options.m_hostOut = &output;
    const Hydra86::EmbedRun run = Hydra86::runImage(image, options);
    result.available = true;
    result.ok = run.ok();
    result.exitCode = run.m_exitCode;
    result.cycles = run.m_cycles;
    result.output = output.str();
    result.psf = run.m_psf;
    result.error = run.m_error;
#else
    (void)image;
    result.error = "当前构建未嵌入 Hydra86；请配置 T1T_HYDRA86_ROOT 后重新构建";
#endif
    return result;
}

} // namespace transfer
