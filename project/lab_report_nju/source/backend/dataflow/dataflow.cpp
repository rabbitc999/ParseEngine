#include "dataflow.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <deque>
#include <sstream>

namespace dataflow {
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

bool parseConstant(const std::string& text, long long& value) {
    if (text.empty()) return false;
    std::size_t i = (text[0] == '-' || text[0] == '+') ? 1 : 0;
    if (i >= text.size()) return false;
    for (std::size_t k = i; k < text.size(); ++k) {
        if (!std::isdigit(static_cast<unsigned char>(text[k]))) return false;
    }
    value = std::strtoll(text.c_str(), nullptr, 10);
    return true;
}
bool isConstant(const std::string& text) {
    long long ignored = 0;
    return parseConstant(text, ignored);
}

std::string memoryName(const std::string& operand) {
    if (operand.size() >= 3 && operand.front() == '[' && operand.back() == ']') {
        return trim(operand.substr(1, operand.size() - 2));
    }
    return {};
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

/** 连接 CFG（后继去重，保持出现顺序）。 */
void connectCfg(Program& program) {
    for (auto& block : program) {
        block.successors.clear();
        block.predecessors.clear();
    }
    std::map<std::string, std::size_t> index;
    for (std::size_t i = 0; i < program.size(); ++i) {
        if (!program[i].label.empty()) index[program[i].label] = i;
    }
    for (std::size_t i = 0; i < program.size(); ++i) {
        auto add = [&](std::size_t target) {
            if (std::find(program[i].successors.begin(), program[i].successors.end(), target) ==
                program[i].successors.end()) {
                program[i].successors.push_back(target);
            }
        };
        bool falls = true;
        if (!program[i].code.empty()) {
            const auto& last = program[i].code.back();
            if (last.op == "jmp") {
                if (index.count(last.src1)) add(index[last.src1]);
                falls = false;
            } else if (last.isBranch()) {
                if (index.count(last.src2)) add(index[last.src2]);
            } else if (last.op == "halt") {
                falls = false;
            }
        }
        if (falls && i + 1 < program.size()) add(i + 1);
    }
    for (std::size_t i = 0; i < program.size(); ++i) {
        for (std::size_t s : program[i].successors) program[s].predecessors.push_back(i);
    }
}

/** 操作数是不是可改名变量（标号 / 常量 / `[内存]` 都不算）。 */
bool isVariable(const std::string& operand) {
    if (operand.empty() || isConstant(operand)) return false;
    if (!memoryName(operand).empty()) return false;
    return isName(operand);
}

/** 表达式里是否用到了某个变量（按标识符切词，避免 `ab` 命中 `a`）。 */
bool expressionUses(const std::string& expression, const std::string& name) {
    std::string token;
    for (std::size_t i = 0; i <= expression.size(); ++i) {
        const char c = i < expression.size() ? expression[i] : ' ';
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') {
            token.push_back(c);
            continue;
        }
        if (token == name) return true;
        token.clear();
    }
    return false;
}

/** 指令在某块里的全局序号（到达定值用它给每条定值编号）。 */
std::size_t globalIndex(const Program& program, std::size_t block, std::size_t index) {
    std::size_t total = index;
    for (std::size_t b = 0; b < block; ++b) total += program[b].code.size();
    return total;
}

std::string definitionKey(const std::string& variable, std::size_t index) {
    return variable + "@" + std::to_string(index);
}
std::string definitionVariable(const std::string& key) {
    const auto at = key.find('@');
    return at == std::string::npos ? key : key.substr(0, at);
}

/** 一条指令产生的表达式（可用表达式分析用）；不产生表达式返回空串。 */
std::string expressionOf(const Instruction& ins) {
    if (ins.op == "add" || ins.op == "sub" || ins.op == "mul" || ins.op == "div") {
        std::string left = ins.src1;
        std::string right = ins.src2;
        if ((ins.op == "add" || ins.op == "mul") && right < left) std::swap(left, right);
        return left + " " + ins.op + " " + right;
    }
    if (ins.op == "load") {
        const std::string slot = memoryName(ins.src1);
        if (!slot.empty()) return "[" + slot + "]";
    }
    return {};
}

std::string canonicalConstant(long long value) {
    return std::to_string(value);
}

} // namespace

// ---------------------------------------------------------------- 解析 / 打印

bool parse(const std::string& text, Program& out, std::vector<std::string>& errors) {
    // 全库只有一套解析器：委托给共享 IR 的 parseProgram（TT3 / 普通三地址都吃）
    t1t::Program shared;
    if (!t1t::parseProgram(text, shared, errors)) {
        out.clear();
        return false;
    }
    out = shared.blocks;
    return true;
}
std::string print(const Program& program) {
    // 全库只有一套打印器：把块序列包成共享 Program 再走 plainText
    t1t::Program shared;
    shared.blocks = program;
    return t1t::plainText(shared);
}
std::vector<std::string> definedVariables(const Instruction& ins) {
    if (ins.op == "mov" || ins.op == "add" || ins.op == "sub" || ins.op == "mul" ||
        ins.op == "div" || ins.op == "load") {
        if (isVariable(ins.dst)) return {ins.dst};
    }
    return {};
}

std::vector<std::string> usedVariables(const Instruction& ins) {
    std::vector<std::string> uses;
    auto add = [&](const std::string& operand) {
        if (isVariable(operand)) uses.push_back(operand);
    };
    if (ins.op == "mov") {
        add(ins.src1);
    } else if (ins.op == "add" || ins.op == "sub" || ins.op == "mul" || ins.op == "div") {
        add(ins.src1);
        add(ins.src2);
    } else if (ins.op == "store" || ins.op == "print" || ins.isBranch()) {
        add(ins.src1);
    }
    return uses;
}

// ---------------------------------------------------------------- 格元素

bool Element::operator==(const Element& other) const {
    return shape == other.shape && items == other.items && values == other.values;
}

Element Element::ofSet(std::set<std::string> items) {
    Element element;
    element.shape = Shape::Set;
    element.items = std::move(items);
    return element;
}

Element Element::ofMap(std::map<std::string, std::string> values) {
    Element element;
    element.shape = Shape::Map;
    element.values = std::move(values);
    return element;
}

std::string formatElement(const Element& element) {
    std::ostringstream out;
    out << "{";
    if (element.shape == Element::Shape::Set) {
        bool first = true;
        for (const auto& item : element.items) {
            out << (first ? "" : ", ") << item;
            first = false;
        }
    } else {
        bool first = true;
        for (const auto& entry : element.values) {
            out << (first ? "" : ", ") << entry.first << "=" << entry.second;
            first = false;
        }
    }
    out << "}";
    return out.str();
}

bool Analysis::equal(const Element& a, const Element& b) const { return a == b; }

bool Analysis::lessEqual(const Element& a, const Element& b) const {
    return equal(merge(a, b), b);
}

std::vector<std::string> expressions(const Program& program) {
    std::set<std::string> found;
    for (const auto& block : program) {
        for (const auto& ins : block.code) {
            const std::string expression = expressionOf(ins);
            if (!expression.empty()) found.insert(expression);
        }
    }
    return std::vector<std::string>(found.begin(), found.end());
}

// ---------------------------------------------------------------- 实例一：活跃变量

namespace {

/** 后向 / 并集（may）：OUT 里去掉本指令的定义，加上本指令的使用。 */
class LiveAnalysis : public Analysis {
public:
    std::string name() const override { return "live"; }
    std::string title() const override { return "活跃变量"; }
    Direction direction() const override { return Direction::Backward; }
    Element identity() const override { return Element::ofSet({}); }
    Element boundary() const override { return Element::ofSet({}); }
    Element merge(const Element& a, const Element& b) const override {
        Element result = a;
        result.items.insert(b.items.begin(), b.items.end());
        return result;
    }
    Element transfer(const Program&, std::size_t, std::size_t, const Instruction& ins,
                     const Element& state) const override {
        Element result = state;
        for (const auto& def : definedVariables(ins)) result.items.erase(def);
        for (const auto& use : usedVariables(ins)) result.items.insert(use);
        return result;
    }
};

// ---------------------------------------------------------------- 实例二：到达定值

/** 前向 / 并集（may）：定值编号写成 `变量@指令序号`。 */
class ReachAnalysis : public Analysis {
public:
    std::string name() const override { return "reach"; }
    std::string title() const override { return "到达定值"; }
    Direction direction() const override { return Direction::Forward; }
    Element identity() const override { return Element::ofSet({}); }
    Element boundary() const override { return Element::ofSet({}); }
    Element merge(const Element& a, const Element& b) const override {
        Element result = a;
        result.items.insert(b.items.begin(), b.items.end());
        return result;
    }
    Element transfer(const Program& program, std::size_t block, std::size_t index,
                     const Instruction& ins, const Element& state) const override {
        Element result = state;
        for (const auto& def : definedVariables(ins)) {
            for (auto it = result.items.begin(); it != result.items.end();) {
                if (definitionVariable(*it) == def) it = result.items.erase(it);
                else ++it;
            }
            result.items.insert(definitionKey(def, globalIndex(program, block, index)));
        }
        return result;
    }
};

// ---------------------------------------------------------------- 实例三：可用表达式

/** 前向 / 交集（must）：单位元是全集，入口处是空集。 */
class AvailAnalysis : public Analysis {
public:
    explicit AvailAnalysis(const Program& program) {
        for (const auto& expression : expressions(program)) universe_.insert(expression);
    }
    std::string name() const override { return "avail"; }
    std::string title() const override { return "可用表达式"; }
    Direction direction() const override { return Direction::Forward; }
    Element identity() const override { return Element::ofSet(universe_); }
    Element boundary() const override { return Element::ofSet({}); }
    Element merge(const Element& a, const Element& b) const override {
        Element result;
        result.shape = Element::Shape::Set;
        for (const auto& item : a.items) {
            if (b.items.count(item)) result.items.insert(item);
        }
        return result;
    }
    Element transfer(const Program&, std::size_t, std::size_t, const Instruction& ins,
                     const Element& state) const override {
        Element result = state;
        std::set<std::string> killed;
        for (const auto& def : definedVariables(ins)) {
            for (const auto& expression : result.items) {
                if (expressionUses(expression, def)) killed.insert(expression);
            }
        }
        if (ins.op == "store") {
            const std::string slot = memoryName(ins.src2);
            if (!slot.empty()) {
                const std::string target = "[" + slot + "]";
                for (const auto& expression : result.items) {
                    if (expression.find(target) != std::string::npos) killed.insert(expression);
                }
            }
        }
        for (const auto& item : killed) result.items.erase(item);
        const std::string expression = expressionOf(ins);
        if (!expression.empty()) result.items.insert(expression);
        return result;
    }

private:
    std::set<std::string> universe_;
};

// ---------------------------------------------------------------- 实例四：常量传播

/** 前向 / 映射格：⊤（还不知道）< 十进制常量 < ⊥（非常量）。 */
class ConstAnalysis : public Analysis {
public:
    std::string name() const override { return "const"; }
    std::string title() const override { return "常量传播"; }
    Direction direction() const override { return Direction::Forward; }
    Element identity() const override { return Element::ofMap({}); }
    Element boundary() const override { return Element::ofMap({}); }
    Element merge(const Element& a, const Element& b) const override {
        Element result;
        result.shape = Element::Shape::Map;
        std::set<std::string> keys;
        for (const auto& entry : a.values) keys.insert(entry.first);
        for (const auto& entry : b.values) keys.insert(entry.first);
        for (const auto& key : keys) {
            const std::string merged = mergeValue(valueOf(a, key), valueOf(b, key));
            if (merged != "⊤") result.values[key] = merged;
        }
        return result;
    }
    Element transfer(const Program&, std::size_t, std::size_t, const Instruction& ins,
                     const Element& state) const override {
        Element result = state;
        if (ins.op == "mov") {
            setValue(result, ins.dst, valueOfOperand(state, ins.src1));
        } else if (ins.op == "add" || ins.op == "sub" || ins.op == "mul" || ins.op == "div") {
            setValue(result, ins.dst,
                     fold(ins.op, valueOfOperand(state, ins.src1), valueOfOperand(state, ins.src2)));
        } else if (ins.op == "load") {
            setValue(result, ins.dst, "⊥");     // 内存内容不追踪
        }
        return result;
    }

private:
    static std::string valueOf(const Element& element, const std::string& key) {
        const auto found = element.values.find(key);
        return found == element.values.end() ? "⊤" : found->second;
    }
    static void setValue(Element& element, const std::string& key, const std::string& value) {
        if (!isVariable(key)) return;
        if (value == "⊤") element.values.erase(key);
        else element.values[key] = value;
    }
    static std::string valueOfOperand(const Element& element, const std::string& operand) {
        long long constant = 0;
        if (parseConstant(operand, constant)) return canonicalConstant(constant);
        if (!memoryName(operand).empty()) return "⊥";
        if (!isVariable(operand)) return "⊤";
        return valueOf(element, operand);
    }
    static std::string mergeValue(const std::string& a, const std::string& b) {
        if (a == "⊤") return b;
        if (b == "⊤") return a;
        return a == b ? a : "⊥";
    }
    static std::string fold(const std::string& op, const std::string& a, const std::string& b) {
        if (a == "⊥" || b == "⊥") return "⊥";
        if (a == "⊤" || b == "⊤") return "⊤";
        long long left = 0;
        long long right = 0;
        if (!parseConstant(a, left) || !parseConstant(b, right)) return "⊥";
        if (op == "add") return canonicalConstant(left + right);
        if (op == "sub") return canonicalConstant(left - right);
        if (op == "mul") return canonicalConstant(left * right);
        if (right == 0) return "⊥";
        return canonicalConstant(left / right);
    }
};

} // namespace

std::unique_ptr<Analysis> makeAnalysis(const std::string& name, const Program& program) {
    if (name == "live") return std::unique_ptr<Analysis>(new LiveAnalysis());
    if (name == "reach") return std::unique_ptr<Analysis>(new ReachAnalysis());
    if (name == "avail") return std::unique_ptr<Analysis>(new AvailAnalysis(program));
    if (name == "const") return std::unique_ptr<Analysis>(new ConstAnalysis());
    return nullptr;
}

std::vector<std::string> analysisNames() { return {"live", "reach", "avail", "const"}; }

// ---------------------------------------------------------------- 求解器

namespace {

/** 块内传递函数的应用：前向从头到尾，后向反过来。 */
Element applyBlock(const Program& program, const Analysis& analysis, std::size_t block,
                   const Element& start, Direction direction, Solution& solution) {
    Element state = start;
    const auto& code = program[block].code;
    if (direction == Direction::Forward) {
        for (std::size_t i = 0; i < code.size(); ++i) {
            state = analysis.transfer(program, block, i, code[i], state);
            ++solution.transferApplications;
        }
    } else {
        for (std::size_t i = code.size(); i-- > 0;) {
            state = analysis.transfer(program, block, i, code[i], state);
            ++solution.transferApplications;
        }
    }
    return state;
}

/**
 * 处理一个块：算出它的 IN / OUT，返回"有没有变化"。
 *
 *   前向：IN = 入口边界 或 ⋈ 前驱的 OUT；OUT = 块内传递
 *   后向：OUT = 出口边界 或 ⋈ 后继的 IN；IN = 块内传递（反向）
 *
 * `⋈` 从单位元开始合并，这样单位元写错会立刻暴露出来。
 */
bool processBlock(const Program& program, const Analysis& analysis, const Options& options,
                  Direction direction, std::size_t block, Solution& solution) {
    const bool forward = direction == Direction::Forward;
    auto update = [&](const Element& next, Element& slot) {
        if (analysis.equal(next, slot)) return false;
        if (options.checkMonotone && !analysis.lessEqual(slot, next)) solution.monotone = false;
        slot = next;
        return true;
    };
    ++solution.blockVisits;
    ++solution.visitsPerBlock[block];
    bool changed = false;
    if (forward) {
        Element newIn;
        if (block == 0) {
            newIn = analysis.boundary();
        } else {
            newIn = analysis.identity();
            for (std::size_t p : program[block].predecessors) {
                newIn = analysis.merge(newIn, solution.out[p]);
                ++solution.merges;
            }
        }
        changed |= update(newIn, solution.in[block]);
        changed |= update(applyBlock(program, analysis, block, solution.in[block], direction, solution),
                          solution.out[block]);
    } else {
        Element newOut;
        if (program[block].successors.empty()) {
            newOut = analysis.boundary();
        } else {
            newOut = analysis.identity();
            for (std::size_t s : program[block].successors) {
                newOut = analysis.merge(newOut, solution.in[s]);
                ++solution.merges;
            }
        }
        changed |= update(newOut, solution.out[block]);
        changed |= update(applyBlock(program, analysis, block, solution.out[block], direction, solution),
                          solution.in[block]);
    }
    return changed;
}

} // namespace

Solution solveWorklist(const Program& program, const Analysis& analysis, const Options& options) {
    Solution solution;
    solution.solver = "worklist";
    solution.analysis = analysis.name();
    const std::size_t n = program.size();
    solution.in.assign(n, analysis.identity());
    solution.out.assign(n, analysis.identity());
    solution.visitsPerBlock.assign(n, 0);
    if (n == 0) return solution;
    std::vector<char> queued(n, 0);
    std::vector<char> visited(n, 0);
    std::deque<std::size_t> queue;
    auto push = [&](std::size_t block) {
        if (queued[block]) return;
        queued[block] = 1;
        queue.push_back(block);
    };
    if (options.seedAll) {
        for (std::size_t b = 0; b < n; ++b) push(b);
    } else if (analysis.direction() == Direction::Forward) {
        // 前向分析的边界是"没有前驱的块"，不止 0 号：函数体（`call` 的目标标签）
        // 与死代码都可能是根块。只种 0 号，工作队列就永远不碰它们（朴素轮询
        // 每轮都过一遍所有块），`reach` 这类前向分析会因此报"两种求解器不一致"。
        for (std::size_t b = 0; b < n; ++b) {
            if (program[b].predecessors.empty()) push(b);
        }
    } else {
        for (std::size_t b = 0; b < n; ++b) {
            if (program[b].successors.empty()) push(b);
        }
    }
    while (!queue.empty()) {
        const std::size_t block = options.lifo ? queue.back() : queue.front();
        if (options.lifo) queue.pop_back();
        else queue.pop_front();
        queued[block] = 0;
        if (solution.blockVisits >= options.maxRounds) {
            solution.converged = false;      // 格写错时兜底，别转不出来
            break;
        }
        const bool changed = processBlock(program, analysis, options, analysis.direction(), block, solution);
        // 只播边界种子的模式下，"第一次处理"必须传播一次：块的初值是单位元，
        // 第一次算出来的结果可能正好等于它（例如出口块的 OUT = ∅），
        // 少这一次传播，前驱 / 后继就永远等不到消息。
        const bool firstVisit = !visited[block];
        visited[block] = 1;
        if (!changed && (options.seedAll || !firstVisit)) continue;
        if (analysis.direction() == Direction::Forward) {
            for (std::size_t s : program[block].successors) push(s);
        } else {
            for (std::size_t p : program[block].predecessors) push(p);
        }
    }
    return solution;
}

Solution solveNaive(const Program& program, const Analysis& analysis, const Options& options) {
    Solution solution;
    solution.solver = "naive";
    solution.analysis = analysis.name();
    const std::size_t n = program.size();
    solution.in.assign(n, analysis.identity());
    solution.out.assign(n, analysis.identity());
    solution.visitsPerBlock.assign(n, 0);
    if (n == 0) return solution;
    for (std::size_t round = 0; round < options.maxRounds; ++round) {
        bool changed = false;
        for (std::size_t b = 0; b < n; ++b) {
            if (processBlock(program, analysis, options, analysis.direction(), b, solution)) {
                changed = true;
            }
        }
        if (!changed) return solution;       // 一整轮没人变 → 到不动点
    }
    solution.converged = false;
    return solution;
}

bool sameSolution(const Solution& a, const Solution& b, const Program& program) {
    if (a.in.size() != b.in.size() || a.out.size() != b.out.size()) return false;
    for (std::size_t i = 0; i < program.size(); ++i) {
        if (a.in[i] != b.in[i] || a.out[i] != b.out[i]) return false;
    }
    return true;
}

std::string report(const Program& program, const Analysis& analysis,
                   const Solution& worklist, const Solution& naive) {
    std::ostringstream out;
    auto line = [&](const std::string& who, const Solution& solution) {
        out << who << "：块访问 " << solution.blockVisits << "，传递函数 "
            << solution.transferApplications << " 次，合并 " << solution.merges << " 次，单调 "
            << (solution.monotone ? "✓" : "✗（格写错了？）") << "，收敛 "
            << (solution.converged ? "✓" : "✗（超过轮数上限）") << "\n";
    };
    out << "分析：" << analysis.title() << "（" << analysis.name() << "）\n"
        << "方向：" << (analysis.direction() == Direction::Forward ? "前向" : "后向") << "\n"
        << "基本块：" << program.size() << "\n";
    line("工作队列", worklist);
    line("朴素轮询", naive);
    out << "两种求解器结果一致：" << (sameSolution(worklist, naive, program) ? "是" : "否")
        << "\n";
    if (worklist.blockVisits != 0 && naive.blockVisits != 0) {
        out << "块访问次数之比（工作队列 / 朴素）：" << worklist.blockVisits << " / "
            << naive.blockVisits << "\n";
    }
    return out.str();
}

std::string dumpSets(const Program& program, const Analysis& analysis, const Solution& solution) {
    std::ostringstream out;
    out << "分析：" << analysis.title() << "（" << analysis.name() << "）\n";
    for (std::size_t b = 0; b < program.size(); ++b) {
        const std::string label = program[b].label.empty() ? ("B" + std::to_string(b)) : program[b].label;
        out << "块 " << b << " " << label << "（" << program[b].code.size() << " 条指令）\n";
        out << "  IN  = " << formatElement(solution.in[b]) << "\n";
        out << "  OUT = " << formatElement(solution.out[b]) << "\n";
    }
    return out.str();
}

std::string mermaid(const Program& program, const Analysis& analysis, const Solution& solution) {
    auto body = [](const Element& element) {
        std::string text = formatElement(element);
        if (text.size() >= 2) text = text.substr(1, text.size() - 2);
        return text.empty() ? std::string("∅") : text;
    };
    std::ostringstream out;
    out << "flowchart TD\n";
    for (std::size_t b = 0; b < program.size(); ++b) {
        const std::string label = program[b].label.empty() ? ("B" + std::to_string(b)) : program[b].label;
        out << "    b" << b << "[\"" << label << "<br/>IN: " << body(solution.in[b])
            << "<br/>OUT: " << body(solution.out[b]) << "\"]\n";
    }
    for (std::size_t b = 0; b < program.size(); ++b) {
        for (std::size_t s : program[b].successors) {
            out << "    b" << b << " --> b" << s << "\n";
        }
    }
    out << "    %% " << analysis.title() << "（" << analysis.name() << "）"
        << (analysis.direction() == Direction::Forward ? "：前向" : "：后向") << "\n";
    return out.str();
}

} // namespace dataflow
