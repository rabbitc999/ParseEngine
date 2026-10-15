#include "transfer.hpp"
#include <fstream>
#include <iterator>
#include <sstream>
#include <stdexcept>

namespace transfer {
namespace {
std::string trimConfig(std::string s) {
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    s = s.substr(first, s.find_last_not_of(" \t\r\n") - first + 1);
    return s;
}
void setCommand(MachineConfig& config, const std::string& key,
                const std::string& value) {
    config.commands[key] = value;
}

/**
 * 一条命令可以由多条目标指令组成：把 `\n` 转义还原成真正的换行，
 * 生成阶段会逐行展开（每行各自替换占位符、各自加类型后缀）。
 */
std::string decodeTemplate(const std::string& value) {
    std::string out;
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '\\' && i + 1 < value.size() && value[i + 1] == 'n') {
            out.push_back('\n');
            ++i;
        } else if (value[i] == '\\' && i + 1 < value.size() && value[i + 1] == '\\') {
            out.push_back('\\');
            ++i;
        } else {
            out.push_back(value[i]);
        }
    }
    return out;
}
} // namespace

std::vector<std::uint8_t> loadDataBin(const std::string& path);

/**
 * 建立三种目标机的教学 ABI。这里的寄存器只承担四元式翻译中的临时值，
 * 并不模拟真实操作系统调用约定；因此用户可以通过配置文件替换它们。
 */
MachineConfig defaultConfig(Target t) {
    MachineConfig config;
    config.target = t;
    config.wordBits = 16;
    // 内建教学机默认实现全部指令族；配置里可以关掉其中任意一族。
    config.features = {"carry",  "shift",  "bitwise", "mul",    "div",   "float",
                       "string", "vector", "stream",  "file",   "system", "data"};
    if (t == Target::Mips) {
        config.regA = "$t0";
        config.regB = "$t1";
        config.regC = "$t2";
        config.regHi = "HI";
        config.regLo = "LO";
        config.memoryLoad = "LH {dst}, {name}";
        config.memoryStore = "SH {src}, {name}";
        config.immediateLoad = "LI {dst}, {value}";
        setCommand(config, "add", "ADDU {dst}, {lhs}, {rhs}");
        setCommand(config, "sub", "SUBU {dst}, {lhs}, {rhs}");
        setCommand(config, "mul", "MUL {dst}, {lhs}, {rhs}");
        setCommand(config, "div", "DIV {lhs}, {rhs}");
        setCommand(config, "shl", "SLL {dst}, {lhs}, {rhs}");
        setCommand(config, "shr", "SRA {dst}, {lhs}, {rhs}");
        setCommand(config, "shru", "SRL {dst}, {lhs}, {rhs}");
        setCommand(config, "and", "AND {dst}, {lhs}, {rhs}");
        setCommand(config, "or", "OR {dst}, {lhs}, {rhs}");
        setCommand(config, "xor", "XOR {dst}, {lhs}, {rhs}");
        // MIPS 没有单独的按位取反指令，NOR r,r,r 是最常见的等价写法。
        setCommand(config, "not", "NOR {dst}, {lhs}, {lhs}");
        setCommand(config, "fadd", "ADD {dst}, {lhs}, {rhs}");
        setCommand(config, "fsub", "SUB {dst}, {lhs}, {rhs}");
        setCommand(config, "fmul", "MUL {dst}, {lhs}, {rhs}");
        setCommand(config, "fdiv", "DIV {dst}, {lhs}, {rhs}");
        setCommand(config, "fneg", "NEG {dst}, {lhs}");
        // 乘除法：一条四元式展开成多条目标指令（结果进固定寄存器对 HI:LO）。
        setCommand(config, "mul", "MUL {dst}, {lhs}, {rhs}");
        setCommand(config, "mulu", "MULU {dst}, {lhs}, {rhs}");
        setCommand(config, "div", "DIV {lhs}, {rhs}\nMFLO {dst}");
        setCommand(config, "divu", "DIVU {lhs}, {rhs}\nMFLO {dst}");
        setCommand(config, "rem", "DIV {lhs}, {rhs}\nMFHI {dst}");
        setCommand(config, "remu", "DIVU {lhs}, {rhs}\nMFHI {dst}");
        setCommand(config, "mul2", "MULT {lhs}, {rhs}\nMFLO {dst}\nMFHI {dst2}");
        setCommand(config, "mul2u", "MULTU {lhs}, {rhs}\nMFLO {dst}\nMFHI {dst2}");
        setCommand(config, "div2", "DIV {lhs}, {rhs}\nMFLO {dst}\nMFHI {dst2}");
        setCommand(config, "div2u", "DIVU {lhs}, {rhs}\nMFLO {dst}\nMFHI {dst2}");
    } else if (t == Target::I8086) {
        config.regA = "AX";
        config.regB = "BX";
        config.regC = "CX";
        config.regHi = "DX";
        config.regLo = "AX";
        config.memoryLoad = "MOV {dst}, [{name}]";
        config.memoryStore = "MOV [{name}], {src}";
        config.immediateLoad = "MOV {dst}, {value}";
        setCommand(config, "add", "ADD {dst}, {rhs}");
        setCommand(config, "sub", "SUB {dst}, {rhs}");
        setCommand(config, "mul", "IMUL {rhs}");
        setCommand(config, "div", "IDIV {rhs}");
        setCommand(config, "shl", "SHL {dst}, {rhs}");
        setCommand(config, "shr", "SAR {dst}, {rhs}");
        setCommand(config, "shru", "SHR {dst}, {rhs}");
        setCommand(config, "and", "AND {dst}, {rhs}");
        setCommand(config, "or", "OR {dst}, {rhs}");
        setCommand(config, "xor", "XOR {dst}, {rhs}");
        setCommand(config, "not", "NOT {dst}");
        setCommand(config, "fadd", "FADD {dst}, {rhs}");
        setCommand(config, "fsub", "FSUB {dst}, {rhs}");
        setCommand(config, "fmul", "FMUL {dst}, {rhs}");
        setCommand(config, "fdiv", "FDIV {dst}, {rhs}");
        setCommand(config, "fneg", "FCHS {dst}");
        // 8086：乘除法用固定的 AX/DX，先把结果搬到目标变量。
        setCommand(config, "mul", "IMUL {rhs}\nMOV {dst}, AX");
        setCommand(config, "mulu", "MUL {rhs}\nMOV {dst}, AX");
        setCommand(config, "div", "CWD\nIDIV {rhs}\nMOV {dst}, AX");
        setCommand(config, "divu", "XOR DX, DX\nDIV {rhs}\nMOV {dst}, AX");
        setCommand(config, "rem", "CWD\nIDIV {rhs}\nMOV {dst}, DX");
        setCommand(config, "remu", "XOR DX, DX\nDIV {rhs}\nMOV {dst}, DX");
        setCommand(config, "mul2", "IMUL {rhs}\nMOV {dst}, AX\nMOV {dst2}, DX");
        setCommand(config, "mul2u", "MUL {rhs}\nMOV {dst}, AX\nMOV {dst2}, DX");
        setCommand(config, "div2", "CWD\nIDIV {rhs}\nMOV {dst}, AX\nMOV {dst2}, DX");
        setCommand(config, "div2u", "XOR DX, DX\nDIV {rhs}\nMOV {dst}, AX\nMOV {dst2}, DX");
    } else {
        config.regA = "r0";
        config.regB = "r1";
        config.regC = "r2";
        config.regHi = "r12";
        config.regLo = "r14";
        config.memoryLoad = "LDRSH {dst}, [{name}]";
        config.memoryStore = "STRH {src}, [{name}]";
        config.immediateLoad = "MOV {dst}, #{value}";
        setCommand(config, "add", "ADD {dst}, {lhs}, {rhs}");
        setCommand(config, "sub", "SUB {dst}, {lhs}, {rhs}");
        setCommand(config, "mul", "MUL {dst}, {lhs}, {rhs}");
        setCommand(config, "div", "SDIV {dst}, {lhs}, {rhs}");
        setCommand(config, "shl", "LSL {dst}, {lhs}, {rhs}");
        setCommand(config, "shr", "ASR {dst}, {lhs}, {rhs}");
        setCommand(config, "shru", "LSR {dst}, {lhs}, {rhs}");
        setCommand(config, "and", "AND {dst}, {lhs}, {rhs}");
        setCommand(config, "or", "ORR {dst}, {lhs}, {rhs}");
        setCommand(config, "xor", "EOR {dst}, {lhs}, {rhs}");
        setCommand(config, "not", "MVN {dst}, {lhs}");
        setCommand(config, "fadd", "VADD {dst}, {lhs}, {rhs}");
        setCommand(config, "fsub", "VSUB {dst}, {lhs}, {rhs}");
        setCommand(config, "fmul", "VMUL {dst}, {lhs}, {rhs}");
        setCommand(config, "fdiv", "VDIV {dst}, {lhs}, {rhs}");
        setCommand(config, "fneg", "VNEG {dst}, {lhs}");
        // ARM：乘除法用显式的寄存器对（低位 dst、高位 dst2）。
        setCommand(config, "mul", "MUL {dst}, {lhs}, {rhs}");
        setCommand(config, "mulu", "MUL {dst}, {lhs}, {rhs}");
        setCommand(config, "div", "SDIV {dst}, {lhs}, {rhs}");
        setCommand(config, "divu", "UDIV {dst}, {lhs}, {rhs}");
        setCommand(config, "rem", "SDIV {dst}, {lhs}, {rhs}\nMLS {dst}, {lhs}, {dst}, {rhs}");
        setCommand(config, "remu", "UDIV {dst}, {lhs}, {rhs}\nMLS {dst}, {lhs}, {dst}, {rhs}");
        setCommand(config, "mul2", "SMULL {dst}, {dst2}, {lhs}, {rhs}");
        setCommand(config, "mul2u", "UMULL {dst}, {dst2}, {lhs}, {rhs}");
        setCommand(config, "div2", "SDIV {dst}, {lhs}, {rhs}\nMLS {dst2}, {lhs}, {dst}, {rhs}");
        setCommand(config, "div2u", "UDIV {dst}, {lhs}, {rhs}\nMLS {dst2}, {lhs}, {dst}, {rhs}");
    }
    return config;
}

/**
 * 解析机器配置。每行只能包含一个 key=value，# 和 ; 开头的行是注释。
 * cmd.* 的值保留为模板，生成器稍后替换 {dst}/{lhs}/{rhs} 等占位符。
 */
MachineConfig loadConfig(std::istream& in, Target fallback) {
    MachineConfig config = defaultConfig(fallback);
    // 记录用户已经写过的模板键：第一次覆盖默认值，之后再写同一个键就追加，
    // 于是"一条命令由多条指令完成"可以直接写多行同名键。
    std::set<std::string> userTemplates;
    const auto assignTemplate = [&](const std::string& key, const std::string& value) {
        const std::string decoded = decodeTemplate(value);
        auto& slot = config.commands[key];
        if (userTemplates.insert(key).second) slot = decoded;
        else if (!slot.empty()) slot += "\n" + decoded;
        else slot = decoded;
    };
    std::string line;
    std::size_t lineNo = 0;
    while (std::getline(in, line)) {
        ++lineNo;
        // 兼容 Windows 记事本写出的 UTF-8 BOM（四元式读取也做了同样的处理）。
        if (lineNo == 1 && line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF &&
            static_cast<unsigned char>(line[1]) == 0xBB &&
            static_cast<unsigned char>(line[2]) == 0xBF) {
            line.erase(0, 3);
        }
        line = trimConfig(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') {
            continue;
        }
        const auto equal = line.find('=');
        if (equal == std::string::npos) {
            throw std::runtime_error("Invalid machine config line " +
                                     std::to_string(lineNo));
        }
        const auto key = trimConfig(line.substr(0, equal));
        const auto value = trimConfig(line.substr(equal + 1));
        if (key == "target") {
            config = defaultConfig(target(value));
        } else if (key == "word_bits") {
            config.wordBits = static_cast<unsigned>(std::stoul(value));
        } else if (key == "stack_words") {
            config.stackWords = static_cast<std::size_t>(std::stoull(value));
        } else if (key == "reg_a") {
            config.regA = value;
        } else if (key == "reg_b") {
            config.regB = value;
        } else if (key == "reg_c") {
            config.regC = value;
        } else if (key == "reg_hi") {
            config.regHi = value;
        } else if (key == "reg_lo") {
            config.regLo = value;
        } else if (key == "memory_load") {
            config.memoryLoad = decodeTemplate(value);
        } else if (key == "memory_store") {
            config.memoryStore = decodeTemplate(value);
        } else if (key == "immediate_load") {
            config.immediateLoad = decodeTemplate(value);
        } else if (key.rfind("cmd.", 0) == 0) {
            assignTemplate(key.substr(4), value);
        } else if (key.rfind("load.", 0) == 0 || key.rfind("store.", 0) == 0) {
            assignTemplate(key, value);
        } else if (key.rfind("data.", 0) == 0) {
            // 初值按字面量文本保存，具体类型由变量的声明决定。
            config.data[key.substr(5)] = value;
        } else if (key.rfind("rodata.", 0) == 0) {
            config.readOnlyData[key.substr(7)] = value;
        } else if (key == "data_bin") {
            config.dataBinPath = value;
        } else if (key == "features") {
            // features=carry,shift,bitwise,... ：整份替换这台机器的指令族。
            config.features.clear();
            std::string item;
            std::istringstream list(value);
            while (std::getline(list, item, ',')) {
                const auto trimmed = trimConfig(item);
                if (!trimmed.empty()) config.features.insert(trimmed);
            }
        } else if (key.rfind("feature.", 0) == 0) {
            // feature.string=off / on：单独开关某一族。
            const std::string name = key.substr(8);
            const std::string flag = trimConfig(value);
            if (flag == "off" || flag == "0" || flag == "false") config.features.erase(name);
            else config.features.insert(name);
        } else {
            throw std::runtime_error("Unknown machine config key '" + key +
                                     "' at line " + std::to_string(lineNo));
        }
    }
    // 机器原生字长必须是 8/16/32/64 之一：后端据此决定运算展开方式。
    if (config.wordBits != 8 && config.wordBits != 16 && config.wordBits != 32 &&
        config.wordBits != 64) {
        throw std::runtime_error("word_bits must be 8, 16, 32 or 64");
    }
    return config;
}
MachineConfig loadConfigFile(const std::string& path, Target fallback) {
    std::ifstream in(path);
    if (!in) {
        throw std::runtime_error("Cannot open machine config: " + path);
    }
    auto config = loadConfig(in, fallback);
    if (!config.dataBinPath.empty()) {
        // `data_bin=` 写相对路径时：先按当前目录找，找不到就相对配置文件所在目录找。
        // 合并进 transfer-one-thread 之后示例放在 core/ 下，靠这一步保持路径写法不变。
        std::string binPath = config.dataBinPath;
        {
            std::ifstream direct(binPath, std::ios::binary);
            if (!direct) {
                const auto slash = path.find_last_of("/\\");
                if (slash != std::string::npos) {
                    const std::string dir = path.substr(0, slash + 1);
                    const std::string bare = binPath.substr(binPath.find_last_of("/\\") + 1);
                    for (const std::string& candidate : {dir + binPath, dir + bare}) {
                        std::ifstream alternative(candidate, std::ios::binary);
                        if (alternative) { binPath = candidate; break; }
                    }
                }
            }
        }
        config.dataImage = loadDataBin(binPath);
    }
    return config;
}
/** 以二进制模式读取完整文件，故不会发生文本换行转换或编码转换。 */
std::vector<std::uint8_t> loadDataBin(const std::string& path) {
    std::ifstream in(path,std::ios::binary);
    if(!in) throw std::runtime_error("Cannot open data Bin: "+path);
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in),{});
}

/**
 * 布局表就是 `simple_ast` 的 `exportDataLayout()` 输出，一行一个符号：
 *
 *     arr_1 offset=0 size=32 stride=8 type=array<i32>{count=4;align=2;label=numbers}
 *
 * 只认 `符号名 offset=N` 这两个字段（`size` / `stride` / `type` 由别处使用），
 * 和 `library/stackvm` 的 `DataArea::load` 取同一套字段，两个后端因此看到同一份布局。
 */
std::map<std::string, std::size_t> loadDataLayout(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("Cannot open data layout: " + path);
    std::map<std::string, std::size_t> symbols;
    for (std::string line; std::getline(in, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::size_t space = line.find(' ');
        const std::size_t offsetAt = line.find("offset=");
        if (space == std::string::npos || offsetAt == std::string::npos) continue;
        const std::string name = line.substr(0, space);
        const std::size_t end = line.find(' ', offsetAt);
        const std::string text = line.substr(offsetAt + 7, end - offsetAt - 7);
        try {
            symbols[name] = static_cast<std::size_t>(std::stoull(text));
        } catch (const std::exception&) {
            throw std::runtime_error("Invalid data layout offset for '" + name + "': " + text);
        }
    }
    return symbols;
}

/**
 * `--layout` 留空时按 `<binPath>.layout` 找，和 `library/stackvm` 的默认一致，
 * 这样 `simple_ast` 生成的五件套里 `.data.bin` 与 `.data.bin.layout` 能直接对接。
 */
void loadDataArea(MachineConfig& config, const std::string& binPath,
                  const std::string& layoutPath) {
    config.dataAreaBytes = loadDataBin(binPath);
    config.dataAreaSymbols = loadDataLayout(layoutPath.empty() ? binPath + ".layout"
                                                              : layoutPath);
}
}
