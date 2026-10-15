#include "regalloc.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace regalloc {
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
/** 操作数是内存（写成 `[x]`）时取出符号名；不是内存返回空。 */
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
        if (c == ',') { const std::string item = trim(current); if (!item.empty()) out.push_back(item); current.clear(); }
        else current.push_back(c);
    }
    const std::string item = trim(current);
    if (!item.empty()) out.push_back(item);
    return out;
}
bool isAllocatable(const std::string& name, const std::set<std::string>& labels,
                   const std::set<std::string>& physical) {
    if (name.empty() || name == "_" || !isName(name)) return false;
    if (labels.count(name) != 0) return false;
    if (physical.count(name) != 0) return false;
    return true;
}

/** 一条指令的 def / use（只含可分配的虚拟寄存器）。 */
void defUse(const Instruction& ins, const std::set<std::string>& labels,
            const std::set<std::string>& physical, std::set<std::string>& defs,
            std::set<std::string>& uses) {
    auto addUse = [&](const std::string& operand) {
        const std::string memory = memoryName(operand);
        if (!memory.empty()) return; // 内存符号不参与寄存器分配
        if (isAllocatable(operand, labels, physical)) uses.insert(operand);
    };
    if (ins.op == "label" || ins.op == "jmp" || ins.op == "ret" || ins.op == "halt") return;
    if (ins.op == "call") return; // 被调用者的寄存器由调用约定处理
    if (ins.op == "store") {
        addUse(ins.src1);
        addUse(ins.src2);
        return;
    }
    if (isAllocatable(ins.dst, labels, physical)) defs.insert(ins.dst);
    addUse(ins.src1);
    addUse(ins.src2);
}

std::set<std::string> labelSet(const Program& program) {
    std::set<std::string> labels;
    for (const auto& block : program) if (!block.label.empty()) labels.insert(block.label);
    // 跳转目标也是标号。
    for (const auto& block : program) {
        for (const auto& ins : block.code) {
            if (ins.op == "jmp") labels.insert(ins.src1);
            else if (ins.op == "jz" || ins.op == "jnz") labels.insert(ins.src2);
        }
    }
    return labels;
}

std::set<std::string> physicalSet(const Machine& machine) {
    std::set<std::string> names(machine.registers.begin(), machine.registers.end());
    // 溢出重写用的 scratch 寄存器是**保留名**：它们不参与分配，也不会被再溢出
    // （否则"溢出 → 插入 load/store → 再分配 → 再溢出"会雪球式膨胀）。
    names.insert("SCRATCH");
    names.insert("SCRATCH2");
    return names;
}

void addEdge(Interference& graph, const std::string& a, const std::string& b) {
    if (a == b || a.empty() || b.empty()) return;
    graph.edges[a].insert(b);
    graph.edges[b].insert(a);
}

/** 线性扫描用的活跃区间（按全程序指令序号）。 */
struct Interval {
    std::string name;
    std::size_t start = static_cast<std::size_t>(-1);
    std::size_t end = 0;
    bool spansCall = false;
    double cost = 0.0;
};

} // namespace

Machine Machine::teaching(std::size_t count) {
    Machine machine;
    const char* names[] = {"A", "B", "C", "D", "E", "F", "G", "H"};
    for (std::size_t i = 0; i < count && i < 8; ++i) machine.registers.push_back(names[i]);
    if (machine.registers.size() >= 1) machine.calleeSaved.insert(machine.registers[0]);
    if (machine.registers.size() >= 2) machine.calleeSaved.insert(machine.registers[1]);
    return machine;
}

std::size_t Interference::degree(const std::string& name) const {
    const auto found = edges.find(name);
    return found == edges.end() ? 0 : found->second.size();
}
std::size_t Interference::edgeCount() const {
    std::size_t total = 0;
    for (const auto& entry : edges) total += entry.second.size();
    return total / 2;
}

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
std::vector<std::string> virtualRegisters(const Program& program) {
    const auto labels = labelSet(program);
    std::set<std::string> names;
    for (const auto& block : program) {
        for (const auto& ins : block.code) {
            std::set<std::string> defs, uses;
            defUse(ins, labels, {}, defs, uses);
            names.insert(defs.begin(), defs.end());
            names.insert(uses.begin(), uses.end());
        }
    }
    return std::vector<std::string>(names.begin(), names.end());
}

namespace {
/** `reserved` 里的名字（物理寄存器、scratch）不参与分配。 */
Liveness livenessWith(const Program& program, const std::set<std::string>& reserved);
Interference interferenceWith(const Program& program, const Liveness& live,
                              const std::set<std::string>& reserved);
} // namespace

Liveness liveness(const Program& program) { return livenessWith(program, {}); }

namespace {
Liveness livenessWith(const Program& program, const std::set<std::string>& reserved) {
    const auto labels = labelSet(program);
    Liveness result;
    result.liveIn.assign(program.size(), {});
    result.liveOut.assign(program.size(), {});
    std::vector<std::set<std::string>> use(program.size()), def(program.size());
    for (std::size_t b = 0; b < program.size(); ++b) {
        for (const auto& ins : program[b].code) {
            std::set<std::string> d, u;
            defUse(ins, labels, reserved, d, u);
            for (const auto& name : u) if (!def[b].count(name)) use[b].insert(name);
            def[b].insert(d.begin(), d.end());
        }
    }
    bool changed = true;
    while (changed) {
        changed = false;
        for (std::size_t i = program.size(); i-- > 0;) {
            std::set<std::string> out;
            for (std::size_t s : program[i].successors) out.insert(result.liveIn[s].begin(), result.liveIn[s].end());
            std::set<std::string> in = use[i];
            for (const auto& name : out) if (!def[i].count(name)) in.insert(name);
            if (in != result.liveIn[i] || out != result.liveOut[i]) {
                result.liveIn[i] = in;
                result.liveOut[i] = out;
                changed = true;
            }
        }
    }
    // 峰值活跃数：块内逐指令模拟。
    for (std::size_t b = 0; b < program.size(); ++b) {
        std::set<std::string> live = result.liveOut[b];
        result.peakLive = std::max(result.peakLive, live.size());
        for (std::size_t i = program[b].code.size(); i-- > 0;) {
            const auto& ins = program[b].code[i];
            std::set<std::string> d, u;
            defUse(ins, labels, reserved, d, u);
            for (const auto& name : d) live.erase(name);
            live.insert(u.begin(), u.end());
            result.peakLive = std::max(result.peakLive, live.size());
        }
    }
    return result;
}
} // namespace

Interference buildInterference(const Program& program, const Liveness& live) {
    return interferenceWith(program, live, {});
}

namespace {
Interference interferenceWith(const Program& program, const Liveness& live,
                              const std::set<std::string>& reserved) {
    const auto labels = labelSet(program);
    Interference graph;
    for (std::size_t b = 0; b < program.size(); ++b) {
        std::set<std::string> active = live.liveOut[b];
        for (std::size_t i = program[b].code.size(); i-- > 0;) {
            const auto& ins = program[b].code[i];
            std::set<std::string> defs, uses;
            defUse(ins, labels, reserved, defs, uses);
            // 定义与"此刻活跃的其它变量"干涉。
            for (const auto& d : defs) {
                for (const auto& v : active) {
                    if (v == d) continue;
                    if (ins.isMove() && v == ins.src1) continue; // 复制例外：留给合并
                    addEdge(graph, d, v);
                }
            }
            for (const auto& d : defs) active.erase(d);
            active.insert(uses.begin(), uses.end());
        }
    }
    return graph;
}
} // namespace

std::map<std::string, double> spillCosts(const Program& program) {
    const auto labels = labelSet(program);
    std::map<std::string, double> costs;
    for (const auto& block : program) {
        for (const auto& ins : block.code) {
            std::set<std::string> defs, uses;
            defUse(ins, labels, {}, defs, uses);
            for (const auto& name : defs) costs[name] += 1.0;
            for (const auto& name : uses) costs[name] += 2.0; // 使用比定义更贵
            // 循环里的权重更高：后继指向自己的块（自环）或回边粗略加倍。
        }
    }
    // 粗略的循环加权：块的后继中有编号更小的块（回边）时，把该块的代价翻倍。
    for (std::size_t b = 0; b < program.size(); ++b) {
        bool loop = false;
        for (std::size_t s : program[b].successors) if (s <= b) loop = true;
        if (!loop) continue;
        for (const auto& ins : program[b].code) {
            std::set<std::string> defs, uses;
            defUse(ins, labels, {}, defs, uses);
            for (const auto& name : defs) costs[name] += 1.0;
            for (const auto& name : uses) costs[name] += 2.0;
        }
    }
    return costs;
}

namespace {

/** 把预着色节点（物理寄存器）加进图里：被调用者保存之外的在 call 处干涉。 */
void addPrecolored(const Program& program, const Machine& machine, Interference& graph) {
    const auto labels = labelSet(program);
    const auto physical = physicalSet(machine);
    for (const auto& block : program) {
        for (const auto& ins : block.code) {
            if (!ins.isCall()) continue;
            // 调用点的活跃集合用块级近似：整个块里出现过的可分配名字都算活跃。
            std::set<std::string> live;
            for (const auto& other : block.code) {
                std::set<std::string> defs, uses;
                defUse(other, labels, physical, defs, uses);
                live.insert(defs.begin(), defs.end());
                live.insert(uses.begin(), uses.end());
            }
            for (const auto& name : machine.registers) {
                if (machine.calleeSaved.count(name)) continue; // 被调用者保存：不用干涉
                for (const auto& value : live) addEdge(graph, "%" + name, value);
            }
        }
    }
}

} // namespace

Allocation colorByGraph(const Program& program, const Machine& machine, const Options& options) {
    Allocation allocation;
    allocation.algorithm = "chaitin";
    const auto labels = labelSet(program);
    const auto physical = physicalSet(machine);
    const Liveness live = livenessWith(program, physical);
    Interference graph = interferenceWith(program, live, physical);
    addPrecolored(program, machine, graph);
    const auto costs = spillCosts(program);
    const std::size_t K = machine.colors();

    // 预着色节点：物理寄存器 + 上面加的 "%物理" 节点。
    std::map<std::string, std::string> precolored;
    for (const auto& name : machine.registers) precolored["%" + name] = name;
    for (const auto& name : machine.registers) precolored[name] = name;

    // 保守合并：把 mov 的定义与源合到一起（合并后高度数邻居少于 K 才行）。
    if (options.coalesce) {
        for (const auto& block : program) {
            for (const auto& ins : block.code) {
                if (!ins.isMove()) continue;
                const std::string& a = ins.dst;
                const std::string& b = ins.src1;
                if (a.empty() || b.empty() || a == b) continue;
                // 只合并"两个可分配虚拟寄存器"：源是常量（`mov t, 1`）或物理寄存器
                // 时谈不上合并，只是普通赋值。
                if (!isAllocatable(a, labels, physical) || !isAllocatable(b, labels, physical)) continue;
                if (graph.edges[a].count(b) != 0) continue; // 已经干涉，不能合并
                const std::set<std::string>& na = graph.edges[a];
                const std::set<std::string>& nb = graph.edges[b];
                std::size_t heavy = 0;
                std::set<std::string> merged = na;
                merged.insert(nb.begin(), nb.end());
                for (const auto& n : merged) {
                    if (n == a || n == b) continue;
                    if (graph.degree(n) >= K) ++heavy;
                }
                if (heavy >= K) continue; // Briggs：高度数邻居太多就放弃
                // 合并 b 到 a：把 b 的邻居接给 a。
                for (const auto& n : nb) {
                    if (n == a) continue;
                    graph.edges[a].insert(n);
                    graph.edges[n].erase(b);
                    graph.edges[n].insert(a);
                }
                graph.edges.erase(b);
                ++allocation.coalescedMoves;
            }
        }
    }

    // 简化：反复压栈度数 < K 的节点；卡住时挑最便宜的溢出候选。
    std::vector<std::string> stack;
    std::set<std::string> potentialSpills;
    std::set<std::string> remaining;
    // 节点集合必须来自"程序里的虚拟寄存器"，不能只取图里的键：
    // 没有任何干涉边的变量（例如最后一个只用一次的临时量）也要分到颜色。
    for (const auto& name : virtualRegisters(program)) {
        if (!physical.count(name)) remaining.insert(name);
    }
    while (!remaining.empty()) {
        bool pushed = false;
        for (auto it = remaining.begin(); it != remaining.end();) {
            const std::size_t degree = [&] {
                std::size_t count = 0;
                for (const auto& n : graph.edges[*it]) if (remaining.count(n)) ++count;
                return count;
            }();
            if (degree < K) {
                stack.push_back(*it);
                it = remaining.erase(it);
                pushed = true;
            } else {
                ++it;
            }
        }
        if (pushed) continue;
        // 挑 degree/成本 最大的压栈（即最便宜溢出的）。
        std::string best;
        double bestScore = -1;
        for (const auto& name : remaining) {
            std::size_t degree = 0;
            for (const auto& n : graph.edges[name]) if (remaining.count(n)) ++degree;
            const double cost = costs.count(name) ? costs.at(name) : 1.0;
            const double score = static_cast<double>(degree) / (cost + 1.0);
            if (score > bestScore) { bestScore = score; best = name; }
        }
        if (best.empty()) break;
        potentialSpills.insert(best);
        stack.push_back(best);
        remaining.erase(best);
    }

    // 选择：逆序弹栈上色；预着色邻居占用的颜色不能用。
    std::map<std::string, std::string> assignment;
    const auto colorOf = [&](const std::string& neighbor) -> std::string {
        const auto pre = precolored.find(neighbor);
        if (pre != precolored.end()) return pre->second;
        const auto found = assignment.find(neighbor);
        return found == assignment.end() ? std::string() : found->second;
    };
    while (!stack.empty()) {
        const std::string name = stack.back();
        stack.pop_back();
        std::set<std::string> used;
        for (const auto& neighbor : graph.edges[name]) {
            const std::string color = colorOf(neighbor);
            if (!color.empty()) used.insert(color);
        }
        std::string chosen;
        for (const auto& color : machine.registers) {
            if (!used.count(color)) { chosen = color; break; }
        }
        if (chosen.empty()) {
            allocation.spilled.insert(name);
        } else {
            assignment[name] = chosen;
        }
    }
    if (!potentialSpills.empty()) {
        // 潜在溢出但实际分到颜色的，按常规继续；真正没颜色的已进 spilled。
    }
    allocation.assignment = assignment;
    std::set<std::string> usedColors;
    for (const auto& entry : assignment) usedColors.insert(entry.second);
    allocation.colorsUsed = usedColors.size();
    return allocation;
}

Allocation linearScan(const Program& program, const Machine& machine, const Options& options) {
    Allocation allocation;
    allocation.algorithm = "linear";
    const auto labels = labelSet(program);
    const auto physical = physicalSet(machine);
    const auto costs = spillCosts(program);
    // 打平程序，记录每条指令的序号。
    struct Point { std::size_t block; std::size_t index; std::size_t order; };
    std::vector<Point> points;
    std::map<std::string, Interval> intervals;
    std::size_t order = 0;
    std::vector<std::size_t> callOrders;   // 调用点在打平后的指令序号
    for (std::size_t b = 0; b < program.size(); ++b) {
        for (std::size_t i = 0; i < program[b].code.size(); ++i) {
            const auto& ins = program[b].code[i];
            if (ins.isCall()) callOrders.push_back(order);
            std::set<std::string> defs, uses;
            defUse(ins, labels, physical, defs, uses);
            for (const auto& name : defs) {
                Interval& interval = intervals[name];
                interval.name = name;
                interval.start = std::min(interval.start, order);
                interval.end = std::max(interval.end, order);
            }
            for (const auto& name : uses) {
                Interval& interval = intervals[name];
                interval.name = name;
                interval.start = std::min(interval.start, order);
                interval.end = std::max(interval.end, order);
            }
            ++order;
        }
    }
    for (auto& entry : intervals) {
        if (entry.second.start == static_cast<std::size_t>(-1)) entry.second.start = 0;
        entry.second.cost = costs.count(entry.first) ? costs.at(entry.first) : 1.0;
        // 活跃区间里夹着 call：这个值跨调用存活，只能放进被调用者保存的寄存器。
        for (std::size_t callOrder : callOrders) {
            if (callOrder >= entry.second.start && callOrder <= entry.second.end) {
                entry.second.spansCall = true;
                break;
            }
        }
    }
    std::vector<Interval> all;
    for (const auto& entry : intervals) all.push_back(entry.second);
    std::sort(all.begin(), all.end(), [](const Interval& a, const Interval& b) {
        return a.start != b.start ? a.start < b.start : a.name < b.name;
    });
    std::vector<Interval> active;
    auto freeRegister = [&](const Interval& current) -> std::string {
        for (const auto& reg : machine.registers) {
            // 跨调用存活的值不能占用调用者保存的寄存器。
            if (current.spansCall && !machine.calleeSaved.count(reg)) continue;
            bool taken = false;
            for (const auto& other : active) {
                const auto found = allocation.assignment.find(other.name);
                if (found == allocation.assignment.end()) continue;
                if (found->second != reg) continue;
                taken = true;
                break;
            }
            if (!taken) return reg;
        }
        return {};
    };
    for (const auto& interval : all) {
        active.erase(std::remove_if(active.begin(), active.end(),
                                    [&](const Interval& other) { return other.end < interval.start; }),
                     active.end());
        const std::string reg = freeRegister(interval);
        if (!reg.empty()) {
            allocation.assignment[interval.name] = reg;
            active.push_back(interval);
            continue;
        }
        // 没有空闲寄存器：溢出一个"区间结束最晚、且代价最低"的。
        const Interval* victim = &interval;
        double worst = interval.cost / (interval.end - interval.start + 1.0);
        for (const auto& other : active) {
            const double score = other.cost / (other.end - other.start + 1.0);
            if (other.end > interval.end || score < worst) {
                victim = &other;
                worst = std::min(worst, score);
            }
        }
        if (victim != &interval) {
            allocation.spilled.insert(victim->name);
            allocation.assignment.erase(victim->name);
            active.erase(std::remove_if(active.begin(), active.end(), [&](const Interval& other) {
                             return other.name == victim->name;
                         }),
                         active.end());
            const std::string freed = freeRegister(interval);
            if (!freed.empty()) {
                allocation.assignment[interval.name] = freed;
                active.push_back(interval);
                continue;
            }
        }
        allocation.spilled.insert(interval.name);
    }
    std::set<std::string> usedColors;
    for (const auto& entry : allocation.assignment) usedColors.insert(entry.second);
    allocation.colorsUsed = usedColors.size();
    (void)options;
    return allocation;
}

Rewrite rewrite(const Program& program, const Allocation& allocation, const Machine& machine,
                const std::string& scratch) {
    Rewrite result;
    const auto labels = labelSet(program);
    const auto physical = physicalSet(machine);
    const std::string scratch2 = scratch + "2";
    const auto isSpilled = [&](const std::string& name) {
        return isAllocatable(name, labels, physical) && allocation.spilled.count(name) != 0;
    };
    auto mapName = [&](const std::string& name) {
        const auto memory = memoryName(name);
        if (!memory.empty()) return name;
        if (!isName(name) || labels.count(name)) return name;
        const auto found = allocation.assignment.find(name);
        if (found != allocation.assignment.end()) return found->second;
        return name; // 溢出或未分配：保持原名，下面插入 load/store
    };
    std::set<std::string> slotNames;
    for (const auto& block : program) {
        Block out;
        out.label = block.label;
        for (const auto& ins : block.code) {
            // 冗余复制：两端映射到同一个物理寄存器（合并成功、或本来就同色）时，
            // `mov X, X` 什么也不做，整条删掉——合并的收益要在输出里看得见。
            // 任一端溢出时不能删：那条 `mov` 还承担着把值搬进 / 搬出内存的作用。
            if (ins.isMove() && !ins.dst.empty() && !ins.src1.empty() &&
                !isSpilled(ins.dst) && !isSpilled(ins.src1) &&
                mapName(ins.dst) == mapName(ins.src1)) {
                ++result.eliminatedMoves;
                continue;
            }
            // 使用前读回被溢出的操作数：两个操作数分别用 scratch / scratch2，
            // 避免同一条指令的两次读回互相覆盖。
            auto reload = [&](const std::string& operand, const std::string& target) -> std::string {
                if (!isSpilled(operand)) return mapName(operand);
                Instruction load;
                load.op = "load";
                load.dst = target;
                load.src1 = "[" + machine.spillPrefix + "." + operand + "]";
                out.code.push_back(load);
                ++result.insertedLoads;
                slotNames.insert(operand);
                return target;
            };
            const std::string src1 = reload(ins.src1, scratch);
            const std::string src2 = reload(ins.src2, scratch2);
            Instruction rewritten = ins;
            rewritten.dst = isSpilled(ins.dst) ? scratch : mapName(ins.dst);
            if (ins.op == "store") {
                rewritten.src1 = src1;
                rewritten.src2 = ins.src2;
            } else {
                rewritten.src1 = src1;
                rewritten.src2 = src2;
            }
            out.code.push_back(rewritten);
            // 定义后写回。
            if (isSpilled(ins.dst)) {
                Instruction store;
                store.op = "store";
                store.src1 = scratch;
                store.src2 = "[" + machine.spillPrefix + "." + ins.dst + "]";
                out.code.push_back(store);
                ++result.insertedStores;
                slotNames.insert(ins.dst);
            }
        }
        result.program.push_back(std::move(out));
    }
    result.spillSlots = slotNames.size();
    return result;
}

namespace {

std::size_t memoryOps(const Program& program) {
    std::size_t count = 0;
    for (const auto& block : program) {
        for (const auto& ins : block.code) if (ins.op == "load" || ins.op == "store") ++count;
    }
    return count;
}
std::size_t instructionCount(const Program& program) {
    std::size_t count = 0;
    for (const auto& block : program) count += block.code.size();
    return count;
}

} // namespace

Result allocate(const Program& program, const Machine& machine, const std::string& algorithm,
                const Options& options) {
    Result result;
    result.stats.colors = machine.colors();
    result.stats.algorithm = algorithm;
    result.stats.instructionsBefore = instructionCount(program);
    result.stats.memoryOpsBefore = memoryOps(program);
    const auto reserved = physicalSet(machine);
    const auto live = livenessWith(program, reserved);
    result.stats.peakLive = live.peakLive;
    result.stats.interferenceEdges = interferenceWith(program, live, reserved).edgeCount();

    Program current = program;
    for (std::size_t round = 0; round < std::max<std::size_t>(1, options.maxSpillRounds); ++round) {
        result.stats.rounds = round + 1;
        Allocation allocation = algorithm == "linear" ? linearScan(current, machine, options)
                                                       : colorByGraph(current, machine, options);
        // 报告里保留**第一轮**的分配：溢出决定与重写就是按它做的；
        // 之后几轮只是为了把插入的 load/store 也分配好（通常已经没有虚拟寄存器）。
        if (round == 0) result.allocation = allocation;
        if (round == 0) {
            result.stats.colorsUsed = allocation.colorsUsed;
            result.stats.coalescedMoves = allocation.coalescedMoves;
        }
        if (allocation.spilled.empty()) {
            result.rewritten = rewrite(current, allocation, machine);
            result.stats.eliminatedMoves += result.rewritten.eliminatedMoves;
            break;
        }
        Rewrite rewritten = rewrite(current, allocation, machine);
        result.rewritten = rewritten;
        result.stats.spilledRegisters = allocation.spilled.size();
        result.stats.spillLoads += rewritten.insertedLoads;
        result.stats.spillStores += rewritten.insertedStores;
        result.stats.spillSlots = rewritten.spillSlots;
        result.stats.eliminatedMoves += rewritten.eliminatedMoves;
        current = rewritten.program;
    }
    result.stats.instructionsAfter = instructionCount(result.rewritten.program);
    result.stats.memoryOpsAfter = memoryOps(result.rewritten.program);
    result.stats.virtualRegisters = virtualRegisters(program).size();
    if (result.stats.spilledRegisters != 0) {
        result.notes.push_back("有 " + std::to_string(result.stats.spilledRegisters) +
                              " 个虚拟寄存器溢出，已插入 load/store 重写");
    }
    if (result.stats.colorsUsed > 0 && result.stats.colorsUsed == result.stats.colors) {
        result.notes.push_back("所有物理寄存器都用上了，说明寄存器压力接近上限");
    }
    return result;
}

std::string report(const Result& result, const Machine& machine) {
    std::ostringstream out;
    out << "算法：" << result.stats.algorithm << "\n"
        << "物理寄存器：" << machine.colors() << " 个（";
    for (std::size_t i = 0; i < machine.registers.size(); ++i)
        out << (i ? ", " : "") << machine.registers[i];
    out << "）\n"
        << "虚拟寄存器：" << result.stats.virtualRegisters << "\n"
        << "峰值活跃：" << result.stats.peakLive << "\n"
        << "干涉图边数：" << result.stats.interferenceEdges << "\n"
        << "用到的颜色：" << result.stats.colorsUsed << " / " << result.stats.colors << "\n"
        << "溢出寄存器：" << result.stats.spilledRegisters << "\n"
        << "溢出槽：" << result.stats.spillSlots << "\n"
        << "插入 load/store：" << result.stats.spillLoads << " / " << result.stats.spillStores << "\n"
        << "合并掉的 mov：" << result.stats.coalescedMoves << "\n"
        << "删除的冗余 mov：" << result.stats.eliminatedMoves << "\n"
        << "指令数：" << result.stats.instructionsBefore << " → " << result.stats.instructionsAfter
        << "\n"
        << "访存次数：" << result.stats.memoryOpsBefore << " → " << result.stats.memoryOpsAfter << "\n"
        << "迭代轮数：" << result.stats.rounds << "\n";
    for (const auto& note : result.notes) out << "提示：" << note << "\n";
    return out.str();
}

} // namespace regalloc
