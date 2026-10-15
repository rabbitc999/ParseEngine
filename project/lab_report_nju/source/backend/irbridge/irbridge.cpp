#include "irbridge.hpp"

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>

namespace irbridge {
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
        if (!(std::isalnum(c) || c == '_' || c == '.')) return false;
    }
    return true;
}
bool isInteger(const std::string& text) {
    if (text.empty()) return false;
    std::size_t i = (text[0] == '-' || text[0] == '+') ? 1 : 0;
    if (i == text.size()) return false;
    for (; i < text.size(); ++i) if (!std::isdigit(static_cast<unsigned char>(text[i]))) return false;
    return true;
}
/** 拆 `i: (op, a, b[, r])`；返回字段，行首下标可选。 */
bool splitRecord(const std::string& line, std::vector<std::string>& fields, std::string& reason) {
    std::string text = trim(line);
    if (text.empty()) { reason = "空行"; return false; }
    const auto open = text.find('(');
    if (open == std::string::npos || text.back() != ')') {
        reason = "缺少括号（应形如 i: (op, a, b)）";
        return false;
    }
    const std::string prefix = trim(text.substr(0, open));
    if (!prefix.empty()) {
        if (prefix.back() != ':') { reason = "行首下标格式应为 i:"; return false; }
        const std::string digits = trim(prefix.substr(0, prefix.size() - 1));
        if (!isInteger(digits) && !digits.empty()) { reason = "行首不是整数下标"; return false; }
    }
    const std::string body = text.substr(open + 1, text.size() - open - 2);
    std::string current;
    for (char c : body) {
        if (c == ',') { fields.push_back(trim(current)); current.clear(); }
        else current.push_back(c);
    }
    fields.push_back(trim(current));
    if (fields.size() < 2) { reason = "字段太少"; return false; }
    return true;
}

bool isCompareOp(const std::string& op) {
    return op == "==" || op == "!=" || op == "<" || op == "<=" || op == ">" || op == ">=";
}

} // namespace

bool parseRef(const std::string& text, std::size_t& index) {
    const std::string value = trim(text);
    if (value.size() < 3 || value.front() != '(' || value.back() != ')') return false;
    const std::string inner = value.substr(1, value.size() - 2);
    if (!isInteger(inner)) return false;
    const long long parsed = std::stoll(inner);
    if (parsed < 0) return false;
    index = static_cast<std::size_t>(parsed);
    return true;
}

bool isTemporary(const std::string& name) {
    if (name.rfind("__tr_", 0) == 0) return true;
    return name.size() > 1 && name[0] == 't' &&
           name.find_first_not_of("0123456789", 1) == std::string::npos;
}

bool readTriples(const std::string& text, Triples& out, std::vector<std::string>& errors) {
    out.clear();
    errors.clear();
    std::istringstream in(text);
    std::size_t line = 0;
    for (std::string raw; std::getline(in, raw);) {
        ++line;
        const std::string trimmed = trim(raw);
        if (trimmed.empty()) continue;
        // 跳过表头（`三元式：共 N 条` 之类）
        if (trimmed.find('(') == std::string::npos) {
            // 表头（`三元式：共 N 条`）、注释行直接跳过。
            if (trimmed.find("：") != std::string::npos || trimmed.find(':') != std::string::npos ||
                trimmed[0] == '#' || trimmed.rfind("//", 0) == 0) continue;
            errors.push_back(std::to_string(line) + ": 无法识别的内容");
            continue;
        }
        std::vector<std::string> fields;
        std::string reason;
        if (!splitRecord(trimmed, fields, reason)) {
            errors.push_back(std::to_string(line) + ": " + reason);
            continue;
        }
        if (fields.size() != 3) {
            errors.push_back(std::to_string(line) + ": 三元式应有三字段（op, arg1, arg2）");
            continue;
        }
        out.push_back({fields[0], fields[1], fields[2]});
    }
    return errors.empty();
}

bool readQuads(const std::string& text, Quads& out, std::vector<std::string>& errors) {
    out.clear();
    errors.clear();
    std::istringstream in(text);
    std::size_t line = 0;
    for (std::string raw; std::getline(in, raw);) {
        ++line;
        const std::string trimmed = trim(raw);
        if (trimmed.empty()) continue;
        if (trimmed.find('(') == std::string::npos) {
            if (trimmed.find("：") != std::string::npos || trimmed.find(':') != std::string::npos ||
                trimmed[0] == '#' || trimmed.rfind("//", 0) == 0) continue;
            errors.push_back(std::to_string(line) + ": 无法识别的内容");
            continue;
        }
        std::vector<std::string> fields;
        std::string reason;
        if (!splitRecord(trimmed, fields, reason)) {
            errors.push_back(std::to_string(line) + ": " + reason);
            continue;
        }
        if (fields.size() != 4) {
            errors.push_back(std::to_string(line) + ": 四元式应有四字段（op, arg1, arg2, result）");
            continue;
        }
        out.push_back({fields[0], fields[1], fields[2], fields[3]});
    }
    return errors.empty();
}

std::string writeTriples(const Triples& triples) {
    std::ostringstream out;
    out << "三元式：共 " << triples.size() << " 条\n";
    for (std::size_t i = 0; i < triples.size(); ++i) {
        // 与仓库 fixture（printf "%3d"）逐字对齐。
        out << std::setw(3) << i << ": (" << triples[i].op << ", " << triples[i].a << ", "
            << triples[i].b << ")\n";
    }
    return out.str();
}

std::string writeQuads(const Quads& quads) {
    std::ostringstream out;
    out << "四元式：共 " << quads.size() << " 条\n";
    for (std::size_t i = 0; i < quads.size(); ++i) {
        out << std::setw(3) << i << ": (" << quads[i].op << ", " << quads[i].a << ", "
            << quads[i].b << ", " << quads[i].result << ")\n";
    }
    return out.str();
}

bool triplesToQuads(const Triples& triples, Quads& out, std::vector<std::string>& errors) {
    out.clear();
    errors.clear();
    // 谁被引用过：被引用的条目需要分配一个显式名字。
    std::vector<bool> referenced(triples.size(), false);
    for (const auto& triple : triples) {
        for (const std::string* operand : {&triple.a, &triple.b}) {
            std::size_t index = 0;
            if (!parseRef(*operand, index)) continue;
            if (index >= triples.size()) {
                errors.push_back("引用 (" + std::to_string(index) + ") 超出三元式范围");
                continue;
            }
            referenced[index] = true;
        }
    }
    auto nameOf = [&](std::size_t index) { return "t" + std::to_string(index); };
    auto resolve = [&](const std::string& operand, std::string& outOperand) {
        std::size_t index = 0;
        if (!parseRef(operand, index)) { outOperand = operand; return true; }
        if (index >= triples.size()) {
            errors.push_back("引用 (" + std::to_string(index) + ") 超出三元式范围");
            return false;
        }
        const std::string op = triples[index].op;
        if (op == "label" || op == "j" || op == "write") {
            errors.push_back("引用 (" + std::to_string(index) + ") 指向不产生值的 " + op);
            return false;
        }
        outOperand = nameOf(index);
        return true;
    };
    for (std::size_t i = 0; i < triples.size(); ++i) {
        const Triple& triple = triples[i];
        std::string a, b;
        if (!resolve(triple.a, a)) continue;
        if (!resolve(triple.b, b)) continue;
        if (triple.op == "label") { out.push_back({"label", "_", "_", triple.a}); continue; }
        if (triple.op == "j") { out.push_back({"j", "_", "_", triple.b}); continue; }
        if (triple.op == "jz" || triple.op == "jnz") {
            out.push_back({triple.op, a, "_", triple.b});
            continue;
        }
        if (triple.op == "write" || triple.op == "read" || triple.op == "halt") {
            if (triple.op == "read") out.push_back({"read", "_", "_", triple.a});
            else if (triple.op == "halt") out.push_back({"halt", "_", "_", "_"});
            else out.push_back({"write", a, "_", "_"});
            continue;
        }
        if (triple.op == ":=") { out.push_back({":=", a, "_", triple.b}); continue; }
        if (triple.op == "neg" || triple.op == "odd" || triple.op == "~") {
            out.push_back({triple.op, a, "_", referenced[i] ? nameOf(i) : "_"});
            continue;
        }
        // 二元运算 / 关系
        if (!isCompareOp(triple.op) && !(triple.op == "+" || triple.op == "-" || triple.op == "*" ||
                                         triple.op == "/" || triple.op == "%" || triple.op == "<<" ||
                                         triple.op == ">>" || triple.op == "&" || triple.op == "|" ||
                                         triple.op == "^")) {
            errors.push_back("第 " + std::to_string(i) + " 条的操作 " + triple.op + " 不认识");
            continue;
        }
        out.push_back({triple.op, a, b, referenced[i] ? nameOf(i) : "_"});
    }
    return errors.empty();
}

bool quadsToTriples(const Quads& quads, Triples& out, std::vector<std::string>& errors) {
    out.clear();
    errors.clear();
    if (quads.empty()) return true;
    // 变量集合：被 := 写在 result 上的名字，以及出现在操作数位置的非临时名。
    std::set<std::string> variables;
    for (const auto& quad : quads) {
        if (quad.op == ":=" && isName(quad.result)) variables.insert(quad.result);
        for (const std::string* operand : {&quad.a, &quad.b}) {
            if (isName(*operand) && !isTemporary(*operand)) variables.insert(*operand);
        }
        if (quad.op == "read" && isName(quad.result)) variables.insert(quad.result);
    }
    // 值名（表达式结果）→ 三元式下标。
    std::map<std::string, std::size_t> valueIndex;
    auto isValue = [&](const std::string& name) {
        return isName(name) && (isTemporary(name) || !variables.count(name));
    };
    auto operand = [&](const std::string& text) {
        const auto found = valueIndex.find(text);
        return found == valueIndex.end() ? text : "(" + std::to_string(found->second) + ")";
    };
    for (const auto& quad : quads) {
        const std::string& op = quad.op;
        if (op == "label") { out.push_back({"label", quad.result, "_"}); continue; }
        if (op == "j") { out.push_back({"j", "_", quad.result}); continue; }
        if (op == "jz" || op == "jnz") {
            out.push_back({op, operand(quad.a), quad.result});
            continue;
        }
        if (op == "halt") { out.push_back({"halt", "_", "_"}); continue; }
        if (op == "write") { out.push_back({"write", operand(quad.a), "_"}); continue; }
        if (op == "read") {
            out.push_back({"read", quad.result, "_"});
            continue;
        }
        if (op == ":=") {
            out.push_back({":=", operand(quad.a), quad.result});
            continue;
        }
        // 表达式：先写一条三元式，结果落在它的下标上。
        const std::size_t index = out.size();
        const bool unary = op == "neg" || op == "odd" || op == "~";
        out.push_back({op, operand(quad.a), unary ? "_" : operand(quad.b)});
        if (isName(quad.result)) {
            if (isValue(quad.result)) {
                valueIndex[quad.result] = index;
            } else {
                // 结果落在变量上：补一条赋值，保持语义。
                out.push_back({":=", "(" + std::to_string(index) + ")", quad.result});
            }
        }
    }
    return errors.empty();
}

std::string toTransfer(const Quads& quads, bool withDecls) {
    std::ostringstream out;
    out << "# irbridge 导出：library/transfer 可直接读的四元式\n";
    if (withDecls) {
        std::set<std::string> variables;
        for (const auto& quad : quads) {
            if (quad.op == "read" && isName(quad.result)) variables.insert(quad.result);
            if (quad.op == ":=" && isName(quad.result) && !isTemporary(quad.result)) {
                variables.insert(quad.result);
            }
        }
        for (const auto& name : variables) out << "(decl, i16, global, " << name << ")\n";
    }
    for (std::size_t i = 0; i < quads.size(); ++i) {
        const auto& quad = quads[i];
        out << i << ": (" << quad.op << ", " << quad.a << ", " << quad.b << ", " << quad.result
            << ")\n";
    }
    return out.str();
}

std::string toTransferFromTriples(const Triples& triples, bool withDecls) {
    Quads quads;
    std::vector<std::string> errors;
    if (!triplesToQuads(triples, quads, errors)) {
        std::string text = "# irbridge 导出失败：\n";
        for (const auto& error : errors) text += "# " + error + "\n";
        return text;
    }
    return toTransfer(quads, withDecls);
}

namespace {

std::string statsCommon(const std::vector<std::string>& ops, std::size_t count,
                        std::size_t temps) {
    std::size_t arithmetic = 0, compare = 0, jump = 0, io = 0, assign = 0, label = 0, other = 0;
    for (const auto& op : ops) {
        if (op == "+" || op == "-" || op == "*" || op == "/" || op == "%" || op == "neg" ||
            op == "odd" || op == "~" || op == "<<" || op == ">>" || op == "&" || op == "|" ||
            op == "^") ++arithmetic;
        else if (isCompareOp(op)) ++compare;
        else if (op == "j" || op == "jz" || op == "jnz") ++jump;
        else if (op == "read" || op == "write") ++io;
        else if (op == ":=") ++assign;
        else if (op == "label") ++label;
        else ++other;
    }
    std::ostringstream out;
    out << "总条数：" << count << "\n"
        << "算术 / 移位 / 按位：" << arithmetic << "\n"
        << "比较：" << compare << "\n"
        << "跳转：" << jump << "\n"
        << "标签：" << label << "\n"
        << "赋值：" << assign << "\n"
        << "输入输出：" << io << "\n"
        << "临时量：" << temps << "\n"
        << "其它：" << other << "\n";
    return out.str();
}

} // namespace

std::string statistics(const Quads& quads) {
    std::vector<std::string> ops;
    std::set<std::string> temps;
    for (const auto& quad : quads) {
        ops.push_back(quad.op);
        if (isName(quad.result) && (isTemporary(quad.result) || quad.op != ":=")) {
            temps.insert(quad.result);
        }
    }
    return statsCommon(ops, quads.size(), temps.size());
}

std::string statistics(const Triples& triples) {
    std::vector<std::string> ops;
    for (const auto& triple : triples) ops.push_back(triple.op);
    std::size_t refs = 0;
    for (const auto& triple : triples) {
        std::size_t index = 0;
        if (parseRef(triple.a, index)) ++refs;
        if (parseRef(triple.b, index)) ++refs;
    }
    std::string text = statsCommon(ops, triples.size(), refs);
    text += "被 (k) 引用的次数：" + std::to_string(refs) + "\n";
    return text;
}

} // namespace irbridge
