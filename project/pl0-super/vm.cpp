#include "pl0super_detail.hpp"

#include <map>
#include <sstream>

namespace pl0s {
namespace {

/** 目标机里的一个值：整数 / 浮点 / 文本。 */
struct Value {
    enum class Kind { Int, Float, Text };
    Kind kind = Kind::Int;
    std::int64_t i = 0;
    double f = 0.0;
    std::string s;

    static Value ofInt(std::int64_t value) {
        Value v;
        v.kind = Kind::Int;
        v.i = value;
        return v;
    }
    static Value ofFloat(double value) {
        Value v;
        v.kind = Kind::Float;
        v.f = value;
        return v;
    }
    static Value ofText(std::string value) {
        Value v;
        v.kind = Kind::Text;
        v.s = std::move(value);
        return v;
    }
    /** 打印形式（整数原样、浮点按 formatFloat、文本就是文本）。 */
    std::string text() const {
        if (kind == Kind::Text) return s;
        if (kind == Kind::Float) return detail::formatFloat(f);
        return std::to_string(i);
    }
    /** 当数用（文本在编译期已经被挡掉，这里给 0 兜底）。 */
    double number() const { return kind == Kind::Float ? f : static_cast<double>(i); }
    bool isFloat() const { return kind == Kind::Float; }
};

/** 目标机的运行时错误（内部用异常传上来）。 */
struct VmError {
    std::string what;
};

/**
 * 栈式目标机。
 *
 * 帧模型：每个过程调用压一层帧，帧里放这个过程的变量与数组；变量名是 `过程.变量`，
 * 找变量时**从最内层帧往外找同名过程的那一层**——等价于 PL/0 的静态链，
 * 但不用在指令里传链指针（见技术报告第 4 节）。
 */
class Vm {
public:
    Vm(const std::vector<std::string>& code, VmResult& out) : code_(code), out_(out) {}

    bool run() {
        try {
            scanLabels();
            frames_.push_back(Frame{"main", {}, {}});
            pc_ = labelOf("p_main");
            while (pc_ < code_.size()) {
                if (++out_.steps > 5000000) throw VmError{"执行步数超过上限（死循环？）"};
                const std::string line = strip(code_[pc_]);
                if (!line.empty() && step(line)) continue;
                ++pc_;
            }
        } catch (const VmError& err) {
            out_.error = err.what;
            return false;
        }
        out_.ok = true;
        return true;
    }

private:
    const std::vector<std::string>& code_;
    VmResult& out_;
    std::size_t pc_ = 0;
    std::vector<Value> stack_;
    struct Frame {
        std::string name;
        std::map<std::string, Value> vars;
        std::map<std::string, std::vector<Value>> arrays;
    };
    std::vector<Frame> frames_;
    std::map<std::string, std::size_t> labels_;       ///< 标号 → 行号
    std::map<std::string, std::string> strings_;      ///< sN → 文本

    /** 去掉首尾空白。 */
    static std::string strip(const std::string& line) {
        const auto first = line.find_first_not_of(" \t\r");
        if (first == std::string::npos) return {};
        const auto last = line.find_last_not_of(" \t\r");
        return line.substr(first, last - first + 1);
    }

    /** 去掉字符串字面量的引号并解转义。 */
    static std::string unquote(const std::string& text) {
        std::string out;
        for (std::size_t i = 0; i < text.size(); ++i) {
            if (text[i] == '"') continue;
            if (text[i] == '\\' && i + 1 < text.size()) {
                const char next = text[++i];
                out += next == 'n' ? '\n' : next;
                continue;
            }
            out += text[i];
        }
        return out;
    }

    /** 先把标号与字符串常量扫出来（跳转都是向前向后随便跳，所以不能边跑边建）。 */
    void scanLabels() {
        for (std::size_t i = 0; i < code_.size(); ++i) {
            const std::string line = strip(code_[i]);
            if (line.empty()) continue;
            const auto stringAt = line.find(": .str ");
            if (stringAt != std::string::npos) {                    // `sN: .str "文本"`
                strings_[line.substr(0, stringAt)] = unquote(line.substr(stringAt + 7));
                continue;
            }
            if (line.compare(0, 6, "label ") == 0) {                // `label 名字:` —— 名字在中间
                std::string name = line.substr(6);
                if (!name.empty() && name.back() == ':') name.pop_back();
                labels_[name] = i;
                continue;
            }
            if (line.back() == ':') {
                labels_[line.substr(0, line.size() - 1)] = i;
            }
        }
    }

    std::size_t labelOf(const std::string& name) const {
        const auto found = labels_.find(name);
        if (found == labels_.end()) throw VmError{"找不到标号：" + name};
        return found->second;
    }

    /**
     * 找变量：**从最内层帧往外找第一个有这个名字的帧**。
     *
     * 变量名是"过程$变量$字段"这种全局唯一名（`$` 不在标识符里出现），所以
     * "最内层有它的那一帧"正好就是静态链指向的那一层：递归时拿到的是本次调用的副本，
     * 嵌套过程读外层变量时找的是外层的帧 —— 不用在指令里传链指针。
     */
    Value* findVar(const std::string& name) {
        for (auto it = frames_.rbegin(); it != frames_.rend(); ++it) {
            const auto found = it->vars.find(name);
            if (found != it->vars.end()) return &found->second;
        }
        return nullptr;
    }

    std::vector<Value>* findArray(const std::string& name) {
        for (auto it = frames_.rbegin(); it != frames_.rend(); ++it) {
            const auto found = it->arrays.find(name);
            if (found != it->arrays.end()) return &found->second;
        }
        return nullptr;
    }

    void pushValue(Value value) { stack_.push_back(std::move(value)); }

    Value popValue() {
        if (stack_.empty()) throw VmError{"运行时栈空了（指令不配对）"};
        Value value = stack_.back();
        stack_.pop_back();
        return value;
    }

    void callFrame(const std::string& name) {
        if (frames_.size() > 4096) throw VmError{"调用层数超过上限（递归太深？）"};
        frames_.push_back(Frame{name, {}, {}});
    }

    /** 算术：整数对整数出整数，沾了浮点就提升；字符串只允许 `+`（拼接）。 */
    static Value arithmetic(const std::string& op, const Value& a, const Value& b) {
        if (op == "add" && (a.kind == Value::Kind::Text || b.kind == Value::Kind::Text)) {
            if (a.kind != Value::Kind::Text || b.kind != Value::Kind::Text) {
                throw VmError{"字符串拼接的两边都要是字符串"};
            }
            return Value::ofText(a.s + b.s);
        }
        if (a.kind == Value::Kind::Text || b.kind == Value::Kind::Text) {
            throw VmError{"字符串不能做算术"};
        }
        const bool isFloat = a.isFloat() || b.isFloat();
        if (op == "mod") {
            if (isFloat) throw VmError{"% 只支持整数"};
            if (b.i == 0) throw VmError{"% 的除数是 0"};
            return Value::ofInt(a.i % b.i);
        }
        const double left = a.number();
        const double right = b.number();
        if (op == "div") {
            if (isFloat) {
                if (right == 0.0) throw VmError{"除数是 0"};
                return Value::ofFloat(left / right);
            }
            if (b.i == 0) throw VmError{"除数是 0"};
            return Value::ofInt(a.i / b.i);                 // 整数除法向零截断
        }
        const double result = op == "add" ? left + right : (op == "sub" ? left - right : left * right);
        return isFloat ? Value::ofFloat(result) : Value::ofInt(static_cast<std::int64_t>(result));
    }

    /** 比较：文本按字典序，数字按大小（整数浮点混比会提升）。 */
    static bool compare(const std::string& op, const Value& a, const Value& b) {
        if (a.kind == Value::Kind::Text || b.kind == Value::Kind::Text) {
            if (a.kind != Value::Kind::Text || b.kind != Value::Kind::Text) {
                throw VmError{"字符串和数字不能比"};
            }
            if (op == "eq") return a.s == b.s;
            if (op == "ne") return a.s != b.s;
            if (op == "lt") return a.s < b.s;
            if (op == "le") return a.s <= b.s;
            if (op == "gt") return a.s > b.s;
            return a.s >= b.s;
        }
        const double left = a.number();
        const double right = b.number();
        if (op == "eq") return left == right;
        if (op == "ne") return left != right;
        if (op == "lt") return left < right;
        if (op == "le") return left <= right;
        if (op == "gt") return left > right;
        return left >= right;
    }

    /** 执行一条指令：返回 true 表示 `pc_` 已经自己改好了。 */
    bool step(const std::string& line) {
        if (line[0] == ';') return false;
        if (line == ".data" || line == ".text") return false;
        if (line.back() == ':') return false;                        // 标号
        if (line.find(": .str ") != std::string::npos) return false; // 字符串常量
        std::istringstream in(line);
        std::string op;
        in >> op;
        if (op == "push" || op == "pushf" || op == "pushs") {
            std::string arg;
            in >> arg;
            if (op == "push") pushValue(Value::ofInt(std::atoll(arg.c_str())));
            else if (op == "pushf") pushValue(Value::ofFloat(std::atof(arg.c_str())));
            else {
                const auto found = strings_.find(arg);
                pushValue(Value::ofText(found == strings_.end() ? arg : found->second));
            }
            return false;
        }
        if (op == "decl") {
            std::string arg;
            in >> arg;
            Value value = popValue();
            frames_.back().vars[arg] = value;
            return false;
        }
        if (op == "decla") {
            std::string arg;
            std::size_t length = 0;
            in >> arg >> length;
            frames_.back().arrays[arg] = std::vector<Value>(length, Value::ofInt(0));
            return false;
        }
        if (op == "load") {
            std::string arg;
            in >> arg;
            Value* found = findVar(arg);
            if (found == nullptr) throw VmError{"读未声明的变量：" + arg};
            pushValue(*found);
            return false;
        }
        if (op == "store") {
            std::string arg;
            in >> arg;
            Value value = popValue();
            Value* found = findVar(arg);
            if (found == nullptr) throw VmError{"写未声明的变量：" + arg};
            *found = value;
            return false;
        }
        if (op == "aload" || op == "astore") {
            std::string arg;
            in >> arg;
            const Value index = popValue();
            if (index.kind != Value::Kind::Int) throw VmError{"数组下标要是整数"};
            Value written;
            if (op == "astore") written = popValue();
            std::vector<Value>* array = findArray(arg);
            if (array == nullptr) throw VmError{"数组 " + arg + " 没有声明"};
            if (index.i < 0 || static_cast<std::size_t>(index.i) >= array->size()) {
                throw VmError{"数组 " + arg + " 下标越界：" + std::to_string(index.i)};
            }
            if (op == "aload") pushValue((*array)[static_cast<std::size_t>(index.i)]);
            else (*array)[static_cast<std::size_t>(index.i)] = written;
            return false;
        }
        if (op == "jmp" || op == "jz" || op == "jnz" || op == "call") {
            std::string arg;
            in >> arg;
            if (op == "jmp") {
                pc_ = labelOf(arg);
                return true;
            }
            if (op == "jz" || op == "jnz") {
                const Value cond = popValue();
                const bool zero = cond.number() == 0.0;
                if ((op == "jz") == zero) {
                    pc_ = labelOf(arg);
                    return true;
                }
                return false;
            }
            const std::size_t back = pc_ + 1;                  // call：压帧 + 记返回地址
            callFrame(arg);
            pushValue(Value::ofInt(static_cast<std::int64_t>(back)));
            pc_ = labelOf("p_" + arg);
            return true;
        }
        if (op == "ret") {
            const Value back = popValue();
            if (frames_.size() <= 1) throw VmError{"ret 时没有可返回的帧"};
            frames_.pop_back();
            pc_ = static_cast<std::size_t>(back.i);
            return true;
        }
        if (op == "dup") {
            const Value value = popValue();
            pushValue(value);
            pushValue(value);
            return false;
        }
        if (op == "swap") {
            const Value first = popValue();
            const Value second = popValue();
            pushValue(first);
            pushValue(second);
            return false;
        }
        if (op == "drop") {
            popValue();
            return false;
        }
        if (op == "add" || op == "sub" || op == "mul" || op == "div" || op == "mod") {
            const Value right = popValue();
            const Value left = popValue();
            pushValue(arithmetic(op, left, right));
            return false;
        }
        if (op == "neg" || op == "not" || op == "tof" || op == "toi") {
            const Value value = popValue();
            if (op == "neg") {
                if (value.kind == Value::Kind::Text) throw VmError{"负号不能用在字符串上"};
                pushValue(value.isFloat() ? Value::ofFloat(-value.f) : Value::ofInt(-value.i));
            } else if (op == "not") {
                pushValue(Value::ofInt(value.number() == 0.0 ? 1 : 0));
            } else if (op == "tof") {
                if (value.kind == Value::Kind::Text) throw VmError{"float(...) 不能用在字符串上"};
                pushValue(Value::ofFloat(value.number()));
            } else {
                if (value.kind == Value::Kind::Text) throw VmError{"int(...) 不能用在字符串上"};
                pushValue(Value::ofInt(static_cast<std::int64_t>(value.number())));
            }
            return false;
        }
        if (op == "lt" || op == "le" || op == "gt" || op == "ge" || op == "eq" || op == "ne") {
            const Value right = popValue();
            const Value left = popValue();
            pushValue(Value::ofInt(compare(op, left, right) ? 1 : 0));
            return false;
        }
        if (op == "and" || op == "or") {
            const Value right = popValue();
            const Value left = popValue();
            const bool lhs = left.number() != 0.0;
            const bool rhs = right.number() != 0.0;
            pushValue(Value::ofInt((op == "and" ? (lhs && rhs) : (lhs || rhs)) ? 1 : 0));
            return false;
        }
        if (op == "write") {
            out_.output += popValue().text();
            out_.output += '\n';
            return false;
        }
        if (op == "halt") {
            pc_ = code_.size();
            return true;
        }
        if (op == "label") {
            std::string arg;
            in >> arg;
            return false;
        }
        throw VmError{"不认的指令：" + op};
    }
};

} // namespace

bool runAsm(const std::string& asmText, VmResult& out) {
    out = VmResult{};
    std::vector<std::string> code;
    std::istringstream in(asmText);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        code.push_back(line);
    }
    Vm vm(code, out);
    const bool ok = vm.run();
    std::istringstream tokens(out.output);
    std::string token;
    while (tokens >> token) out.tokens.push_back(token);
    return ok;
}

} // namespace pl0s
