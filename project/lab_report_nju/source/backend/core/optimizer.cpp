#include "transfer.hpp"
#include <algorithm>
#include <atomic>
#include <functional>
#include <future>
#include <thread>
#include <iterator>
#include <sstream>
#include <stdexcept>

namespace transfer {
namespace {
using Names = std::set<std::string>;
using Definitions = std::set<std::size_t>;
bool literal(const std::string& s) {
    // 字面量包含整数与浮点写法；寄存器名不是字面量，按普通名字参与数据流。
    return isNumericLiteral(s);
}
bool variable(const std::string& s) { return s != "_" && !literal(s); }
bool binary(const std::string& op) {
    static const Names ops{
        "+",  "-", "*", "/", "%", "==", "!=", "<", "<=", ">", ">=",
        "<<", ">>", "&", "|", "^"};
    return ops.count(opBase(op)) != 0;
}
bool carryOp(const Quad& q) { return opBase(q.op) == "+c" || opBase(q.op) == "-b"; }
bool pairOp(const Quad& q) { return opBase(q.op) == "mul" || opBase(q.op) == "div"; }
bool unaryOp(const Quad& q) {
    const std::string base = opBase(q.op);
    return base == "neg" || base == "odd" || base == "~";
}
bool convertOp(const Quad& q) { return opBase(q.op) == "cvt"; }
/** 可折叠 / 可化简的表达式；进位运算和双寄存器乘除有固定寄存器副作用，保持原样。 */
bool expr(const Quad& q) {
    return (binary(q.op) || unaryOp(q) || convertOp(q)) && !carryOp(q) && !pairOp(q) &&
           q.op != "decl";
}
bool defines(const Quad& q) {
    return expr(q) || q.op == ":=" || q.op == "read" || q.op == "getc" || pairOp(q);
}
/** 一条四元式定义的名字集合；双结果指令（mul/div）有两个目标。 */
Names definedNames(const Quad& q) {
    Names out;
    if (!defines(q)) return out;
    if (q.result != "_" && !q.result.empty()) out.insert(q.result);
    if (pairOp(q) && q.extra != "_" && !q.extra.empty()) out.insert(q.extra);
    return out;
}
bool branch(const Quad& q) {
    return q.op == "j" || q.op == "jz" || q.op == "jnz" || q.op == "ret" || q.op == "iret";
}
Names uses(const Quad& q) {
    Names out;
    const std::string base = opBase(q.op);
    if (expr(q) || base == ":=" || base == "write" || base == "jz" || base == "jnz" ||
        base == "param" || base == "putc" || base == "ret" || carryOp(q) || pairOp(q)) {
        if (variable(q.a)) out.insert(q.a);
        if ((binary(q.op) || pairOp(q)) && variable(q.b)) out.insert(q.b);
    }
    // 数据区三件套：load 用指针，store 用指针和值（addr 的操作数是数据区符号名，
    // 不是变量）。漏算这两个操作数会让"写进数据区的值"被 DCE 当死定义删掉。
    if (base == "load" || base == "store") {
        if (variable(q.a)) out.insert(q.a);
        if (base == "store" && variable(q.b)) out.insert(q.b);
    }
    return out;
}
bool temporary(const std::string& s) {
    if (s.rfind("__tr_", 0) == 0) return true;
    return s.size() > 1 && s[0] == 't' && s.find_first_not_of("0123456789", 1) == std::string::npos;
}
template<class T> std::set<T> intersection(const std::set<T>& a, const std::set<T>& b) {
    std::set<T> out;
    std::set_intersection(a.begin(), a.end(), b.begin(), b.end(), std::inserter(out,out.end()));
    return out;
}
template<class T> void merge(std::set<T>& a, const std::set<T>& b) { a.insert(b.begin(),b.end()); }
std::string key(const Quad& q) {
    std::string a = q.a, b = q.b;
    static const Names commutative{"+", "*", "==", "!=", "&", "|", "^"};
    if (commutative.count(opBase(q.op)) && b < a) std::swap(a,b);
    return q.op + "|" + a + "|" + b;
}
bool mentions(const std::string& k, const std::string& v) {
    auto a = k.find('|'), b = k.find('|',a+1);
    return k.substr(a+1,b-a-1) == v || k.substr(b+1) == v;
}
void transferRD(Definitions& defs, const Program& p, std::size_t i) {
    // call / int 可能改写任意全局变量：按"杀掉所有到达定值"处理。
    const std::string base = opBase(p[i].op);
    if (base == "call" || base == "int") { defs.clear(); return; }
    if (!defines(p[i])) return;
    const Names killed = definedNames(p[i]);
    for (auto it = defs.begin(); it != defs.end();) {
        bool kill = false;
        for (const auto& name : definedNames(p[*it])) {
            if (killed.count(name) != 0) { kill = true; break; }
        }
        if (kill) it = defs.erase(it); else ++it;
    }
    defs.insert(i);
}
void transferAE(Names& ae, const Quad& q) {
    if (defines(q)) for (auto it = ae.begin(); it != ae.end();) {
        bool kill = false;
        for (const auto& name : definedNames(q)) if (mentions(*it, name)) { kill = true; break; }
        if (kill) it = ae.erase(it); else ++it;
    }
    if (expr(q) && !mentions(key(q),q.result)) ae.insert(key(q));
}
std::vector<Names> definite(const Program& p, const Analysis& cfg) {
    Names universe;
    for (const auto& q : p) if (defines(q)) merge(universe, definedNames(q));
    std::vector<Names> in(cfg.blocks.size(),universe), out = in;
    if (in.empty()) return {};
    bool change = true;
    while (change) {
        change = false;
        for (std::size_t b = 0; b < cfg.blocks.size(); ++b) {
            if (!cfg.blocks[b].reachable) continue;
            Names v = b == 0 ? Names{} : universe;
            for (auto pred : cfg.blocks[b].predecessors) if (cfg.blocks[pred].reachable) v = intersection(v,out[pred]);
            auto next = v;
            for (auto i = cfg.blocks[b].begin; i < cfg.blocks[b].end; ++i) {
                const std::string base = opBase(p[i].op);
                if (base == "call" || base == "int") { next = universe; continue; }
                if (defines(p[i])) merge(next, definedNames(p[i]));
            }
            if (v != in[b] || next != out[b]) { in[b] = v; out[b] = next; change = true; }
        }
    }
    std::vector<Names> at(p.size());
    for (std::size_t b = 0; b < cfg.blocks.size(); ++b) {
        auto v = in[b];
        for (auto i = cfg.blocks[b].begin; i < cfg.blocks[b].end; ++i) {
            at[i] = v;
            const std::string base = opBase(p[i].op);
            if (base == "call" || base == "int") { v = universe; continue; }
            if (defines(p[i])) merge(v, definedNames(p[i]));
        }
    }
    return at;
}
bool readable(const Quad& q, const Names& assigned) {
    for (const auto& v : uses(q)) if (!assigned.count(v)) return false;
    return true;
}
/**
 * 可能已经被 `call` / `int` 改写、其后又没有重新定义过的名字（may 分析）。
 *
 * 到达定值在 call 处整表清空（被调用者可能改写任意变量），于是"唯一到达定值"
 * 会被误当成"值就是它"。循环回边还会把一条环内定值再加回来，最危险的形状是
 *
 *     (call, f, _, _) … L: (<, b, 10, t2) … (+:i32, b, 1, t) (:=, t, _, b) (j, _, _, L)
 *
 * —— L 处 b 的唯一到达定值是环内那条 (:=, t, _, b)，于是 b 被换成 t，
 * 第一次进循环时 t 根本还没定义。这个分析把这些名字标出来，让全局常量传播 / CSE
 * 遇到它们就放弃（局部传播本来就在 call 处清空，不受影响）。
 */
std::vector<Names> clobbered(const Program& p, const Analysis& cfg) {
    Names all;
    for (const auto& q : p) if (defines(q)) merge(all, definedNames(q));
    std::vector<Names> in(cfg.blocks.size()), out = in;
    bool change = true;
    while (change) {
        change = false;
        for (std::size_t b = 0; b < cfg.blocks.size(); ++b) {
            if (!cfg.blocks[b].reachable) continue;
            Names v;
            for (auto pred : cfg.blocks[b].predecessors)
                if (cfg.blocks[pred].reachable) merge(v, out[pred]);
            auto next = v;
            for (auto i = cfg.blocks[b].begin; i < cfg.blocks[b].end; ++i) {
                const std::string base = opBase(p[i].op);
                if (base == "call" || base == "int") { merge(next, all); continue; }
                for (const auto& name : definedNames(p[i])) next.erase(name);
            }
            if (v != in[b] || next != out[b]) { in[b] = v; out[b] = next; change = true; }
        }
    }
    std::vector<Names> at(p.size());
    for (std::size_t b = 0; b < cfg.blocks.size(); ++b) {
        auto v = in[b];
        for (auto i = cfg.blocks[b].begin; i < cfg.blocks[b].end; ++i) {
            at[i] = v;
            const std::string base = opBase(p[i].op);
            if (base == "call" || base == "int") { merge(v, all); continue; }
            for (const auto& name : definedNames(p[i])) v.erase(name);
        }
    }
    return at;
}
int wrap(long long n) { n %= 65536; if (n < 0) n += 65536; return static_cast<int>(n >= 32768 ? n - 65536 : n); }
/** 推断一条四元式的运算类型：显式后缀 -> 结果变量 -> 第一个操作数 -> i16。 */
std::string inferType(const Quad& q, const std::map<std::string, std::string>& declared) {
    const std::string suffix = opType(q.op);
    if (!suffix.empty()) return typeInfo(suffix).name;
    const auto result = declared.find(q.result);
    if (result != declared.end()) return result->second;
    const auto first = declared.find(q.a);
    if (first != declared.end()) return first->second;
    const auto second = declared.find(q.b);
    if (second != declared.end()) return second->second;
    return defaultTypeName();
}
/** 字面量是否为 2 的幂；是则返回指数，用于强度削弱。 */
bool powerOfTwo(const std::string& s, int& exponent) {
    if (!isIntegerLiteral(s)) return false;
    const long long n = std::stoll(s);
    if (n < 2 || (n & (n - 1)) != 0) return false;
    exponent = 0;
    for (long long value = n; value > 1; value >>= 1) ++exponent;
    return true;
}
/** 常量折叠：按运算类型求值；除零、溢出等错误一律放弃折叠。 */
bool fold(Quad& q, const std::map<std::string, std::string>& declared) {
    const std::string base = opBase(q.op);
    const std::string type = inferType(q, declared);
    const bool second = binary(q.op) || pairOp(q);
    if (!expr(q) || !literal(q.a) || (second && !literal(q.b))) return false;
    try {
        const Value left = literalValue(q.a, type);
        const Value right = second ? literalValue(q.b, type) : Value{};
        const Value folded =
            base == "cvt" ? convertValue(left, type) : evalTyped(base, type, left, right);
        // 折叠成字面量时保留类型后缀（否则浮点/宽类型的字面量会被当成 i16）。
        q.op = typedOp(":=", type);
        q.a = displayValue(folded);
        q.b = "_";
        return true;
    } catch (const std::exception&) {
        return false;
    }
}
void simplify(Quad& q, const Names& assigned,
              const std::map<std::string, std::string>& declared) {
    if (fold(q, declared)) return;
    const std::string base = opBase(q.op);
    const std::string type = inferType(q, declared);
    const bool integer = !isFloatType(type);
    // 强度削弱：x * 2^k 等价于 x << k，交给目标机一条移位指令。
    if (base == "*" && integer) {
        int k = 0;
        if (powerOfTwo(q.b, k)) { q.op = typedOp("<<", type); q.b = std::to_string(k); }
        else if (powerOfTwo(q.a, k)) {
            q.a = q.b;
            q.b = std::to_string(k);
            q.op = typedOp("<<", type);
        }
        return;
    }
    std::string v;
    if (((base == "+" || base == "-" || base == "<<" || base == ">>") && q.b == "0") ||
        ((base == "*" || base == "/") && q.b == "1")) v = q.a;
    if ((base == "+" && q.a == "0") || (base == "*" && q.a == "1")) v = q.b;
    if (integer) {
        if ((base == "|" || base == "^") && q.b == "0") v = q.a;
        if ((base == "|" || base == "^") && q.a == "0") v = q.b;
        if (base == "&" && q.b == "-1") v = q.a;
        if (base == "&" && q.a == "-1") v = q.b;
        if (base == "|" && (q.a == "-1" || q.b == "-1")) v = "-1";
    }
    if (readable(q,assigned)) {
        if (base == "*" && (q.a == "0" || q.b == "0")) v = "0";
        if (base == "-" && q.a == q.b) v = "0";
        if (integer) {
            if (base == "&" && (q.a == "0" || q.b == "0")) v = "0";
            if ((base == "<<" || base == ">>") && q.a == "0") v = "0";
            if ((base == "&" || base == "|") && q.a == q.b) v = q.a;
            if (base == "^" && q.a == q.b) v = "0";
        }
    }
    if (!v.empty()) { q.op = typedOp(":=", type); q.a = v; q.b = "_"; }
}
void operands(Quad& q, const std::function<std::string(const std::string&)>& f) {
    if (expr(q) || q.op == ":=" || q.op == "write" || q.op == "jz" || q.op == "jnz" ||
        q.op == "param" || q.op == "putc" || q.op == "ret")
        q.a = f(q.a);
    if (binary(q.op)) q.b = f(q.b);
}
} // namespace

Analysis analyze(const Program& p) {
    // Leader 规则：入口、标签、跳转/停机后的下一条指令。每两个 leader
    // 之间形成一个最大基本块；块内只有顺序流，块间边由最后一条指令决定。
    validate(p);
    Analysis a;
    if (p.empty()) return a;
    std::set<std::size_t> leaders{0};
    std::map<std::string,std::size_t> labels;
    for (std::size_t i=0;i<p.size();++i) {
        if (p[i].op == "label") { leaders.insert(i); labels[p[i].result] = i; }
        if ((branch(p[i]) || p[i].op == "halt") && i+1<p.size()) leaders.insert(i+1);
    }
    for (auto i : leaders) { Block b; b.begin=i; a.blocks.push_back(b); }
    a.instructionBlock.resize(p.size());
    for (std::size_t b=0;b<a.blocks.size();++b) {
        a.blocks[b].end = b+1<a.blocks.size() ? a.blocks[b+1].begin : p.size();
        for (auto i=a.blocks[b].begin;i<a.blocks[b].end;++i) a.instructionBlock[i]=b;
    }
    for (std::size_t b=0;b<a.blocks.size();++b) {
        auto& block=a.blocks[b]; const auto& last=p[block.end-1];
        if (last.op=="j" || last.op=="jz" || last.op=="jnz")
            block.successors.push_back(a.instructionBlock[labels.at(last.result)]);
        if (last.op!="j" && last.op!="halt" && last.op!="ret" && last.op!="iret" &&
            b+1<a.blocks.size())
            block.successors.push_back(b+1);
        // 过程间近似：call/int 连到被调用函数的标签，函数体因此可达，
        // 不会被当作死代码删除；返回边不建模（ret/iret 是出口）。
        for (auto i=block.begin;i<block.end;++i) {
            const std::string base=opBase(p[i].op);
            if ((base=="call"||base=="int") && labels.count(p[i].a))
                block.successors.push_back(a.instructionBlock[labels.at(p[i].a)]);
        }
        std::sort(block.successors.begin(),block.successors.end());
        block.successors.erase(std::unique(block.successors.begin(),block.successors.end()),block.successors.end());
        for (auto s:block.successors) a.blocks[s].predecessors.push_back(b);
    }
    std::vector<std::size_t> todo{0};
    while (!todo.empty()) {
        auto b=todo.back(); todo.pop_back();
        if (a.blocks[b].reachable) continue;
        a.blocks[b].reachable=true;
        for (auto s:a.blocks[b].successors) todo.push_back(s);
    }
    Names universe, observable;
    for (const auto& q:p) {
        if (expr(q)) universe.insert(key(q));
        for (const auto& name : definedNames(q)) if (!temporary(name)) observable.insert(name);
        for (const auto& v:uses(q)) if (!temporary(v)) observable.insert(v);
    }
    for (auto& b:a.blocks) if (b.reachable) b.availableOut=universe;
    // 到达定值是并集问题：一个定义从任意前驱到达；同一变量的新定义
    // 会杀死旧定义。可用表达式是交集问题：表达式必须从所有前驱可用。
    bool change=true;
    while (change) {
        change=false;
        for (std::size_t bi=0;bi<a.blocks.size();++bi) {
            auto& b=a.blocks[bi]; if (!b.reachable) continue;
            Definitions rd;
            Names ae=bi==0 ? Names{} : universe;
            for (auto pred:b.predecessors) if (a.blocks[pred].reachable) {
                merge(rd,a.blocks[pred].reachingOut);
                ae=intersection(ae,a.blocks[pred].availableOut);
            }
            auto rdOut=rd; auto aeOut=ae;
            for (auto i=b.begin;i<b.end;++i) { transferRD(rdOut,p,i); transferAE(aeOut,p[i]); }
            if (rd!=b.reachingIn || rdOut!=b.reachingOut || ae!=b.availableIn || aeOut!=b.availableOut) {
                b.reachingIn=rd; b.reachingOut=rdOut; b.availableIn=ae; b.availableOut=aeOut; change=true;
            }
        }
    }
    // 活跃变量采用反向方程 LIVE.in = USE ∪ (LIVE.out - DEF)。
    change=true;
    while(change) {
        change=false;
        for (std::size_t bi=a.blocks.size();bi-->0;) {
            auto& b=a.blocks[bi]; if (!b.reachable) continue;
            Names out=b.successors.empty()?observable:Names{};
            for (auto s:b.successors) merge(out,a.blocks[s].liveIn);
            auto in=out;
            for (auto i=b.end;i-->b.begin;) {
                for (const auto& name : definedNames(p[i])) in.erase(name);
                merge(in,uses(p[i]));
            }
            if(in!=b.liveIn || out!=b.liveOut) { b.liveIn=in; b.liveOut=out; change=true; }
        }
    }
    return a;
}
std::string report(const Program& p,const Analysis& a) {
    std::ostringstream out;
    auto show=[&](const auto& values) { out << '{'; bool first=true; for(const auto& v:values) { if(!first) out << ", "; first=false; out << v; } out << '}'; };
    out << "Definitions: quadruple indexes. Expressions: op|operand1|operand2.\n";
    for(std::size_t i=0;i<a.blocks.size();++i) {
        const auto& b=a.blocks[i];
        out << "B" << i << " [" << b.begin << ", " << b.end << ") " << (b.reachable?"reachable":"unreachable") << "\n  pred="; show(b.predecessors); out << " succ="; show(b.successors);
        out << "\n  RD.in="; show(b.reachingIn); out << " RD.out="; show(b.reachingOut);
        out << "\n  LIVE.in="; show(b.liveIn); out << " LIVE.out="; show(b.liveOut);
        out << "\n  AE.in="; show(b.availableIn); out << " AE.out="; show(b.availableOut); out << '\n';
    }
    out << "\n" << write(p); return out.str();
}

namespace {
Program local(const Program& p,const Analysis& a) {
    auto assigned=definite(p,a);
    const auto declared=variableTypes(p);
    Program out;
    for(const auto& b:a.blocks) {
        // 不可达块不参与优化，但 decl 是类型元信息（不是可执行指令），原样保留：
        // simple_ast 把函数体的 decl 排在函数标签之前，那一小段块没有前驱。
        if(!b.reachable) {
            for(auto i=b.begin;i<b.end;++i) if(p[i].op=="decl") out.push_back(p[i]);
            continue;
        }
        std::map<std::string,std::string> copies, expressions;
        for(auto i=b.begin;i<b.end;++i) {
            auto q=p[i];
            // call / int 可能改写任意全局变量：进来之后不能再沿用调用前的
            // 复制与表达式缓存（否则会把中断处理程序写过的变量当常量传播）。
            const std::string base=opBase(q.op);
            if(base=="call"||base=="int") { copies.clear(); expressions.clear(); }
            operands(q,[&](const std::string& v) { auto it=copies.find(v); return it==copies.end()?v:it->second; });
            simplify(q,assigned[i],declared);
            if(expr(q)) {
                auto it=expressions.find(key(q));
                if(it!=expressions.end()) { q.op=":="; q.a=it->second; q.b="_"; }
            }
            if(defines(q)) {
                for(auto it=copies.begin();it!=copies.end();) {
                    const Names touched = definedNames(q);
                    if(touched.count(it->first)!=0 || touched.count(it->second)!=0) it=copies.erase(it); else ++it;
                }
                for(auto it=expressions.begin();it!=expressions.end();) {
                    bool kill=false;
                    for (const auto& name : definedNames(q))
                        if(it->second==name || mentions(it->first,name)) { kill=true; break; }
                    if(kill) it=expressions.erase(it); else ++it;
                }
                if(q.op==":=" && q.a!=q.result) copies[q.result]=q.a;
                if(expr(q) && !mentions(key(q),q.result)) expressions[key(q)]=q.result;
            }
            if((q.op=="jz" || q.op=="jnz") && literal(q.a)) {
                if((std::stoi(q.a)==0)!=(q.op=="jz")) continue;
                q.op="j"; q.a="_";
            }
            out.push_back(q);
        }
    }
    return out;
}
Program global(const Program& p,const Analysis& cfg) {
    auto assigned=definite(p,cfg);
    auto clob=clobbered(p,cfg);
    std::vector<Definitions> at(p.size());
    for(const auto& b:cfg.blocks) {
        auto rd=b.reachingIn;
        for(auto i=b.begin;i<b.end;++i) { at[i]=rd; transferRD(rd,p,i); }
    }
    auto definitions=[&](const Definitions& rd,const std::string& name) {
        Definitions found; for(auto i:rd) if(p[i].result==name) found.insert(i); return found;
    };
    Program out=p;
    for(const auto& b:cfg.blocks) {
        if(!b.reachable) continue;
        auto ae=b.availableIn;
        for(auto i=b.begin;i<b.end;++i) {
            auto& q=out[i];
            // Use original dataflow facts only; subsequent rounds exploit rewrites.
            operands(q,[&](const std::string& v) {
                if(!variable(v) || !assigned[i].count(v)) return v;
                // 可能被 call / int 改写过的名字：旧定值不再是"这里的值"。
                if(clob[i].count(v)) return v;
                auto ds=definitions(at[i],v);
                if(ds.size()!=1) return v;
                auto d=*ds.begin(); const auto& def=p[d];
                if(def.op!=":=") return v;
                if(literal(def.a)) return def.a;
                if(def.a!=v && !clob[i].count(def.a) && assigned[i].count(def.a) &&
                   definitions(at[d],def.a)==definitions(at[i],def.a)) return def.a;
                return v;
            });
            // CSE consults the unchanged expression and reaching holder definitions.
            if(expr(p[i]) && ae.count(key(p[i]))) {
                for(auto d:at[i]) {
                    if(!expr(p[d]) || key(p[d])!=key(p[i]) || !assigned[i].count(p[d].result)) continue;
                    if(clob[i].count(p[d].result)) continue;  // 持有者可能被 call 改写过
                    auto ds=definitions(at[i],p[d].result);
                    bool same=!ds.empty();
                    for(auto other:ds) same=same && expr(p[other]) && key(p[other])==key(p[i]);
                    if(same) { q.op=":="; q.a=p[d].result; q.b="_"; break; }
                }
            }
            transferAE(ae,p[i]);
        }
    }
    return out;
}
Program dead(const Program& p,const Analysis& cfg) {
    auto assigned=definite(p,cfg);
    // 被调用者可能读取任意非临时变量：把 call/int 当作"使用全部可观察名"，
    // 这样调用前的定义不会被 DCE 删掉。
    Names observable;
    for(const auto& q:p) {
        if(defines(q)) for(const auto& name:definedNames(q)) if(!temporary(name)) observable.insert(name);
        for(const auto& name:uses(q)) if(!temporary(name)) observable.insert(name);
    }
    std::vector<bool> keep(p.size(),true);
    for(const auto& b:cfg.blocks) {
        if(!b.reachable) { for(auto i=b.begin;i<b.end;++i) keep[i]=false; continue; }
        auto live=b.liveOut;
        for(auto i=b.end;i-->b.begin;) {
            const auto& q=p[i];
            const bool pure=(expr(q) || q.op==":=") && q.op!="/" && q.op!="%";
            if(pure && !live.count(q.result) && readable(q,assigned[i])) { keep[i]=false; continue; }
            const std::string base=opBase(q.op);
            if(base=="call"||base=="int") merge(live,observable);
            for (const auto& name : definedNames(q)) live.erase(name);
            merge(live,uses(q));
        }
    }
    // Retain target labels even if their unreachable source branch is also removed.
    Names targets;
    for(std::size_t i=0;i<p.size();++i) {
        if(!keep[i]) continue;
        if(branch(p[i])) targets.insert(p[i].result);
        const std::string base=opBase(p[i].op);
        if(base=="call"||base=="int") targets.insert(p[i].a);
    }
    // decl 是类型元信息而不是可执行指令：即使所在块不可达也不能删。simple_ast 把
    // 函数体的 decl 排在函数标签**之前**，那一小段块没有前驱；一旦按 DCE 删掉，
    // 形参就会退回 i16，宽变量的分片访问也跟着错（见 (param, x, _, self)）。
    Program out; for(std::size_t i=0;i<p.size();++i)
        if(keep[i] || p[i].op=="decl" || (p[i].op=="label" && targets.count(p[i].result))) out.push_back(p[i]);
    return out;
}

// 自然循环由回边（目标支配源）定义。只把操作数在循环外已知、无副作用且
// 不会触发除零的表达式外提；归纳变量只处理 x=x+c，并按 16 位模运算保持语义。
// 每次只改一个循环，调用方随后重建 CFG 和数据流集合。
Program loops(const Program& p,const Analysis& cfg,std::vector<std::string>& trace) {
    const auto n=cfg.blocks.size(); if(!n) return p;
    Definitions all; for(std::size_t b=0;b<n;++b) if(cfg.blocks[b].reachable) all.insert(b);
    std::vector<Definitions> dom(n,all); dom[0]={0};
    bool changed=true;
    while(changed) {
        changed=false;
        for(std::size_t b=1;b<n;++b) if(cfg.blocks[b].reachable) {
            auto d=all;
            for(auto pred:cfg.blocks[b].predecessors) if(cfg.blocks[pred].reachable) d=intersection(d,dom[pred]);
            d.insert(b); if(d!=dom[b]) { dom[b]=d; changed=true; }
        }
    }
    std::map<std::size_t,Definitions> natural;
    for(std::size_t b=0;b<n;++b) if(cfg.blocks[b].reachable) for(auto h:cfg.blocks[b].successors) if(dom[b].count(h)) {
        Definitions members{h,b}; std::vector<std::size_t> pending;
        if(b!=h) pending.push_back(b);
        while(!pending.empty()) {
            auto v=pending.back(); pending.pop_back();
            for(auto pred:cfg.blocks[v].predecessors) if(cfg.blocks[pred].reachable && members.insert(pred).second && pred!=h) pending.push_back(pred);
        }
        merge(natural[h],members);
    }
    auto assigned=definite(p,cfg);
    Names occupied;
    for(const auto& q:p) { occupied.insert(q.a); occupied.insert(q.b); occupied.insert(q.result); }
    std::size_t next=0;
    auto fresh=[&](const std::string& stem) { std::string s; do { s="__tr_"+stem+std::to_string(next++); } while(occupied.count(s)); occupied.insert(s); return s; };
    for(const auto& loop:natural) {
        const auto h=loop.first; const auto& members=loop.second;
        const auto start=cfg.blocks[h].begin;
        if(h==0 || p[start].op!="label") continue;
        bool safe=true, outside=false;
        Names entry;
        bool first=true;
        for(auto pred:cfg.blocks[h].predecessors) {
            if(members.count(pred)) {
                // Inserting a preheader must not intercept an internal fallthrough.
                if(pred+1==h && p[cfg.blocks[pred].end-1].op!="j") safe=false;
            } else {
                outside=true;
                auto last=cfg.blocks[pred].end-1; auto set=assigned[last];
                if(defines(p[last])) merge(set, definedNames(p[last]));
                entry=first?set:intersection(entry,set); first=false;
            }
        }
        for(auto b:members) for(auto pred:cfg.blocks[b].predecessors) if(!members.count(pred) && b!=h && cfg.blocks[pred].reachable) safe=false;
        if(!safe || !outside) continue;
        std::map<std::string,std::vector<std::size_t>> defs;
        for(auto b:members) for(auto i=cfg.blocks[b].begin;i<cfg.blocks[b].end;++i) if(defines(p[i])) defs[p[i].result].push_back(i);
        // LICM: only temporary, unique, nontrapping definitions dominating all uses.
        Definitions move;
        Program pre;
        bool progress=true;
        while(progress) {
            progress=false;
            for(auto b:members) for(auto i=cfg.blocks[b].begin;i<cfg.blocks[b].end;++i) {
                const auto& q=p[i];
                if(move.count(i) || !temporary(q.result) || !expr(q) || q.op=="/" || q.op=="%" || defs[q.result].size()!=1) continue;
                bool invariant=true;
                for(const auto& v:uses(q)) {
                    if(defs.count(v)) invariant=invariant && defs[v].size()==1 && move.count(defs[v][0]);
                    else invariant=invariant && entry.count(v);
                }
                if(!invariant) continue;
                // Every use, including uses outside the loop, must be dominated.
                for(std::size_t j=0;j<p.size() && invariant;++j) if(uses(p[j]).count(q.result)) {
                    auto useBlock=cfg.instructionBlock[j];
                    invariant=dom[useBlock].count(b) && (useBlock!=b || j>i);
                }
                if(invariant) { move.insert(i); pre.push_back(q); progress=true; }
            }
        }
        std::map<std::size_t,Quad> replacements;
        std::map<std::size_t,Program> after;
        // Basic induction x=x+c. Derived x*k becomes a preheader initialization
        // and an increment after x's update, exact modulo 2^16 even on wraparound.
        // 类型化程序（decl 非空）暂时不启用这条 16 位专用变换，避免宽度取模不一致。
        const bool typedProgram = !variableTypes(p).empty();
        if (!typedProgram)
        for(const auto& d:defs) {
            if(d.second.size()!=1 || !entry.count(d.first)) continue;
            auto update=d.second[0]; const auto& q=p[update];
            if(!((q.op=="+" || q.op=="-") && q.a==d.first && literal(q.b))) continue;
            const int step=std::stoi(q.b)*(q.op=="-"?-1:1);
            std::map<std::string,std::vector<std::size_t>> factors;
            for(auto b:members) for(auto i=cfg.blocks[b].begin;i<cfg.blocks[b].end;++i) {
                const auto& mul=p[i]; if(mul.op!="*" || mul.result==d.first) continue;
                if(mul.a==d.first && literal(mul.b)) factors[mul.b].push_back(i);
                else if(mul.b==d.first && literal(mul.a)) factors[mul.a].push_back(i);
            }
            for(const auto& f:factors) {
                const auto temp=fresh("iv");
                pre.push_back({"*",d.first,f.first,temp,"_"});
                after[update].push_back({"+",temp,std::to_string(wrap(static_cast<long long>(step)*std::stoi(f.first))),temp,"_"});
                for(auto i:f.second) replacements[i]={":=",temp,"_",p[i].result,"_"};
            }
        }
        if(pre.empty()) continue;
        const auto preLabel=fresh("pre"); const auto header=p[start].result;
        Program out;
        for(std::size_t i=0;i<p.size();++i) {
            if(i==start) { out.push_back({"label","_","_",preLabel,"_"}); out.insert(out.end(),pre.begin(),pre.end()); }
            if(move.count(i)) continue;
            auto q=replacements.count(i)?replacements.at(i):p[i];
            if(branch(q) && q.result==header && !members.count(cfg.instructionBlock[i])) q.result=preLabel;
            out.push_back(q);
            if(after.count(i)) out.insert(out.end(),after[i].begin(),after[i].end());
        }
        trace.push_back("loop B"+std::to_string(h)+": LICM="+std::to_string(move.size())+", induction/strength-reduced expressions="+std::to_string(replacements.size()));
        return out;
    }
    return p;
}
} // namespace

Optimization optimizeDetailed(const Program& p,std::size_t maxRounds) {
    validate(p);
    Optimization result; result.program=p;
    for(std::size_t round=0;round<maxRounds;++round) {
        const auto before=write(result.program);
        auto pass=[&](const std::string& label,auto f) {
            const auto old=write(result.program);
            result.program=f(result.program,analyze(result.program));
            validate(result.program);
            if(write(result.program)!=old) result.trace.push_back("round "+std::to_string(round+1)+": "+label);
        };
        pass("local folding/propagation/algebra/CSE/copy",local);
        pass("global propagation/CSE",global);
        pass("liveness DCE / unreachable blocks",dead);
        pass("natural-loop LICM / strength reduction / induction variables",[&](const Program& v,const Analysis& a) { return loops(v,a,result.trace); });
        result.rounds=round+1;
        if(write(result.program)==before) return result;
    }
    throw std::runtime_error("Optimizer did not converge within round limit");
}
Program optimize(const Program& p) { return optimizeDetailed(p).program; }

std::vector<Optimization> optimizeParallel(const std::vector<Program>& programs,
                                            std::size_t workerCount) {
    if (workerCount == 0) workerCount = std::max<std::size_t>(1, std::thread::hardware_concurrency());
    workerCount = std::min(workerCount, std::max<std::size_t>(1, programs.size()));
    std::vector<Optimization> result(programs.size());
    std::atomic<std::size_t> next{0};
    std::vector<std::future<void>> workers;
    for (std::size_t w = 0; w < workerCount; ++w) workers.push_back(std::async(std::launch::async, [&] {
        for (;;) { const auto i = next.fetch_add(1); if (i >= programs.size()) return; result[i] = optimizeDetailed(programs[i]); }
    }));
    for (auto& f : workers) f.get();
    return result;
}
} // namespace transfer
