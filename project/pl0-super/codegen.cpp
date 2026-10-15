#include "pl0super_detail.hpp"

#include <sstream>

namespace pl0s {
namespace {

/**
 * 代码生成器：语法树 → 栈式目标机汇编（一条指令一个栈动作）。
 *
 * 没有寄存器分配、没有活跃分析——表达式一律走后缀式压栈，指令流上的窥孔负责把
 * "算了等于没算"的相邻两条删掉。这就是这一版的取舍：**简化代替优化**。
 */
class Generator {
public:
    Generator(CodegenResult& out, std::string* error) : out_(out), error_(error) {}

    /** 生成整棵程序：先数据段（字符串常量），再各过程，最后主程序。 */
    bool run(const Program& program) {
        program_ = &program;
        collectStrings(program);
        emit(".data");
        for (std::size_t i = 0; i < out_.strings.size(); ++i) {
            emit("s" + std::to_string(i) + ": .str " + quote(out_.strings[i]));
        }
        emit(".text");
        if (!emitProcs(program.decls)) return false;
        emit("p_main:");
        emitScope(program.decls);
        if (!emitStmts(program.body)) return false;
        emit("halt");
        return true;
    }

private:
    CodegenResult& out_;
    const Program* program_ = nullptr;      ///< 结构体字段表要从这里查
    std::string* error_ = nullptr;
    std::size_t labelSerial_ = 0;
    std::size_t tempSerial_ = 0;

    void emit(const std::string& line) { out_.lines.push_back(line); }

    bool fail(const std::string& what) {
        if (error_ && error_->empty()) *error_ = what;
        return false;
    }

    std::string newLabel(const std::string& tag) {
        return "L_" + tag + std::to_string(labelSerial_++);
    }

    /** 字符串常量去重：同样的文本只留一份，代码里引 `sN`。 */
    std::string stringLabel(const std::string& text) {
        for (std::size_t i = 0; i < out_.strings.size(); ++i) {
            if (out_.strings[i] == text) return "s" + std::to_string(i);
        }
        out_.strings.push_back(text);
        return "s" + std::to_string(out_.strings.size() - 1);
    }

    /** 汇编里的字符串字面量（带引号与转义）。 */
    static std::string quote(const std::string& text) {
        std::string out = "\"";
        for (const char c : text) {
            if (c == '"' || c == '\\') out += '\\';
            if (c == '\n') {
                out += "\\n";
                continue;
            }
            out += c;
        }
        out += '"';
        return out;
    }

    void collectStrings(const Program& program) {
        walkStmts(program.body);
        for (const Decl& decl : program.decls) collectDecl(decl);
    }
    void collectDecl(const Decl& decl) {
        walkStmts(decl.body);
        for (const Decl& inner : decl.decls) collectDecl(inner);
    }
    void walkStmts(const std::vector<StmtPtr>& stmts) {
        for (const StmtPtr& stmt : stmts) {
            if (!stmt) continue;
            for (const ExprPtr& expr : stmt->args) walkExpr(expr);
            if (stmt->target) walkExpr(stmt->target);
            walkStmts(stmt->body);
            if (stmt->otherwise) walkStmts(std::vector<StmtPtr>{stmt->otherwise});
        }
    }
    void walkExpr(const ExprPtr& expr) {
        if (!expr) return;
        if (expr->kind == ExprKind::Text) stringLabel(expr->text);
        for (const ExprPtr& child : expr->args) walkExpr(child);
    }

    /** 过程：先发里层的过程（声明顺序），再发自己那一段。 */
    bool emitProcs(const std::vector<Decl>& decls) {
        for (const Decl& decl : decls) {
            if (decl.kind != DeclKind::Proc) continue;
            if (!emitProcs(decl.decls)) return false;
            emit("p_" + decl.name + ":");
            emitScope(decl.decls);
            if (!emitStmts(decl.body)) return false;
            emit("ret");
        }
        return true;
    }

    /** 进过程时的声明：变量给初值、字符串给空串、数组开格子。 */
    /**
     * 结构体字段表 → 扁平叶子表（嵌套结构体展开成 `名字$子名字`）。
     *
     * 结构体变量在目标机里**不存在**：`var p: Point;` 就是 `p$x`、`p$y` 两个变量，
     * 所以这里把字段表摊平，声明与整体复制都按叶子来。
     *
     * @param prefix 字段名前缀（顶层为空）
     * @param fields 字段表
     * @param out    叶子表（名字 + 是不是浮点）
     */
    static void flattenFields(const std::string& prefix, const std::vector<pl0s::Field>& fields,
                              std::vector<std::pair<std::string, bool>>& out) {
        for (const pl0s::Field& field : fields) {
            const std::string name = prefix.empty() ? field.name : prefix + "$" + field.name;
            if (!field.subs.empty()) {
                flattenFields(name, field.subs, out);
                continue;
            }
            out.push_back({name, field.kind == pl0s::TypeKind::Float});
        }
    }

    /** 找一个结构体声明的字段叶子表（找不到就空表）。 */
    std::vector<std::pair<std::string, bool>> structLeaves(const std::string& structName) const {
        std::vector<std::pair<std::string, bool>> out;
        if (program_ == nullptr) return out;
        for (const pl0s::StructDef& def : program_->structs) {
            if (def.name == structName) {
                flattenFields("", def.fields, out);
                break;
            }
        }
        return out;
    }

    void emitScope(const std::vector<Decl>& decls) {
        for (const Decl& decl : decls) {
            if (decl.kind == DeclKind::Var) {
                emit(decl.isFloat ? "pushf 0" : "push 0");
                emit("decl " + decl.name);
            } else if (decl.kind == DeclKind::Str) {
                emit("pushs \"\"");
                emit("decl " + decl.name);
            } else if (decl.kind == DeclKind::Array) {
                emit("decla " + decl.name + " " + std::to_string(decl.length));
            } else if (decl.kind == DeclKind::StructVar) {
                // 结构体变量：每个字段一个变量（`main$p$x`）
                for (const auto& leaf : structLeaves(decl.structName)) {
                    emit(leaf.second ? "pushf 0" : "push 0");
                    emit("decl " + decl.name + "$" + leaf.first);
                }
            } else if (decl.kind == DeclKind::StructArray) {
                // 结构体数组：每个字段一条数组（`main$pts$x`）
                for (const auto& leaf : structLeaves(decl.structName)) {
                    emit("decla " + decl.name + "$" + leaf.first + " " +
                         std::to_string(decl.length));
                }
            }
        }
    }

    bool emitStmts(const std::vector<StmtPtr>& stmts) {
        for (const StmtPtr& stmt : stmts) {
            if (!emitStmt(stmt)) return false;
        }
        return true;
    }

    bool emitStmt(const StmtPtr& stmt) {
        if (!stmt) return true;
        switch (stmt->kind) {
            case StmtKind::Block:
                return emitStmts(stmt->body);
            case StmtKind::Assign: {
                if (!stmt->copyStruct.empty()) {          // 结构体整体赋值 = 逐字段复制
                    const std::string source = structNameOf(*stmt->args[0]);
                    const std::string target = structNameOf(*stmt->target);
                    if (source.empty() || target.empty()) {
                        return fail("结构体整体复制要写成名字路径");
                    }
                    for (const auto& leaf : structLeaves(stmt->copyStruct)) {
                        emit("load " + source + "$" + leaf.first);
                        emit("store " + target + "$" + leaf.first);
                    }
                    return true;
                }
                if (!expr(stmt->args[0])) return false;
                if (stmt->target->kind == ExprKind::Var) {
                    emit("store " + stmt->target->text);
                    return true;
                }
                // 数组元素：值先压、下标后压（目标机的 astore 先弹下标、再弹值）
                if (!expr(stmt->target->args[1])) return false;
                emit("astore " + stmt->target->args[0]->text);
                return true;
            }
            case StmtKind::Write:
                if (!expr(stmt->args[0])) return false;
                emit("write");
                return true;
            case StmtKind::If: {
                if (!expr(stmt->args[0])) return false;
                const std::string elseLabel = newLabel("else");
                const std::string endLabel = newLabel("endif");
                emit("jz " + elseLabel);
                if (!emitStmts(stmt->body)) return false;
                emit("jmp " + endLabel);
                emit("label " + elseLabel + ":");
                if (stmt->otherwise && !emitStmt(stmt->otherwise)) return false;
                emit("label " + endLabel + ":");
                return true;
            }
            case StmtKind::While: {
                const std::string top = newLabel("while");
                const std::string end = newLabel("wend");
                emit("label " + top + ":");
                if (!expr(stmt->args[0])) return false;
                emit("jz " + end);
                if (!emitStmts(stmt->body)) return false;
                emit("jmp " + top);
                emit("label " + end + ":");
                return true;
            }
            case StmtKind::For: {
                // 终值只求一次，存进隐藏变量；循环体末尾把循环变量 ±1
                const std::string limit = "__limit" + std::to_string(tempSerial_++);
                if (!expr(stmt->args[1])) return false;
                emit("decl " + limit);        // decl 会弹掉刚压上去的值并存进这一帧
                if (!expr(stmt->args[0])) return false;
                emit("store " + stmt->target->text);
                const std::string top = newLabel("for");
                const std::string end = newLabel("fend");
                emit("label " + top + ":");
                emit("load " + stmt->target->text);
                emit("load " + limit);
                emit(stmt->down ? "ge" : "le");
                emit("jz " + end);
                if (!emitStmts(stmt->body)) return false;
                emit("load " + stmt->target->text);
                emit("push 1");
                emit(stmt->down ? "sub" : "add");
                emit("store " + stmt->target->text);
                emit("jmp " + top);
                emit("label " + end + ":");
                return true;
            }
            case StmtKind::Goto:
                emit("jmp " + stmt->name);
                return true;
            case StmtKind::Label:
                emit("label " + stmt->name + ":");
                return true;
            case StmtKind::Call:
                emit("call " + stmt->name);
                return true;
        }
        return fail("生成代码时不认的语句");
    }

    /** 表达式 → 一段后缀式栈指令。 */
    bool expr(const ExprPtr& node) {
        if (!node) return fail("空表达式");
        switch (node->kind) {
            case ExprKind::Number:
                emit("push " + std::to_string(node->number));
                return true;
            case ExprKind::Float:
                emit("pushf " + detail::formatFloat(node->real));
                return true;
            case ExprKind::Text:
                emit("pushs " + stringLabel(node->text));
                return true;
            case ExprKind::Var:
                emit("load " + node->text);
                return true;
            case ExprKind::Index:
                if (!expr(node->args[1])) return false;
                emit("aload " + node->args[0]->text);
                return true;
            case ExprKind::Convert:
                if (!expr(node->args[0])) return false;
                emit(node->text == "float" ? "tof" : "toi");
                return true;
            case ExprKind::Unary:
                if (!expr(node->args[0])) return false;
                emit(node->op == "neg" ? "neg" : "not");
                return true;
            case ExprKind::Binary:
                if (!expr(node->args[0])) return false;
                if (!expr(node->args[1])) return false;
                emit(binaryOp(node->op));
                return true;
        }
        return fail("生成代码时不认的表达式");
    }

    /** 源码运算符 → 目标机助记符（`+` → `add`、`<>` → `ne`…）。 */
    static std::string binaryOp(const std::string& op) {
        if (op == "+") return "add";
        if (op == "-") return "sub";
        if (op == "*") return "mul";
        if (op == "/") return "div";
        if (op == "%") return "mod";
        if (op == "=") return "eq";
        if (op == "<>") return "ne";
        if (op == "<") return "lt";
        if (op == "<=") return "le";
        if (op == ">") return "gt";
        if (op == ">=") return "ge";
        return op;                                   // and / or 同名
    }

    /** 取一个设计式的"基名"（结构体变量是 `前缀$名字`，数组元素不支持整体复制）。 */
    static std::string structNameOf(const Expr& expr) {
        if (expr.kind == pl0s::ExprKind::Var) return expr.text;
        return {};
    }
};

/**
 * 指令流窥孔：把"算了等于没算"的相邻两条删掉（都保持语义）。
 *
 * - `push 0` + `add` / `sub`：加零减零等于没算；
 * - `push 1` + `mul` / `div`：乘一除一等亍没算；
 * - `push N` + `drop`：压了马上丢；
 * - `load X` + `store X`：同名的自我赋值。
 *
 * @param lines 指令行（原地删）
 * @return 删掉的对数
 */
int peepholeLines(std::vector<std::string>& lines) {
    int removed = 0;
    const auto startsWith = [](const std::string& line, const std::string& prefix) {
        return line.compare(0, prefix.size(), prefix) == 0;
    };
    std::vector<std::string> out;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (i + 1 < lines.size()) {
            const std::string& first = lines[i];
            const std::string& second = lines[i + 1];
            const bool identity = (first == "push 0" && (second == "add" || second == "sub")) ||
                                  (first == "push 1" && (second == "mul" || second == "div"));
            const bool wasted = startsWith(first, "push ") && second == "drop";
            const bool selfStore = startsWith(first, "load ") &&
                                   second == "store " + first.substr(5);
            if (identity || wasted || selfStore) {
                ++removed;
                ++i;
                continue;
            }
        }
        out.push_back(lines[i]);
    }
    lines.swap(out);
    return removed;
}

/** 把指令行拼回文本（末尾统一换行）。 */
std::string joinLines(const std::vector<std::string>& lines) {
    std::string text;
    for (const std::string& line : lines) {
        text += line;
        text += '\n';
    }
    return text;
}

} // namespace

bool generate(const Program& program, CodegenResult& out, std::string* error) {
    out = CodegenResult{};
    Generator generator(out, error);
    if (!generator.run(program)) return false;
    out.asmText = joinLines(out.lines);
    if (error) error->clear();
    return true;
}

bool compile(const std::string& source, CompileResult& out) {
    out = CompileResult{};
    std::string error;
    if (!parseSource(source, out.program, &error)) {
        out.error = error;
        return false;
    }
    if (!simplify(out.program, &error, &out.stats)) {
        out.error = error;
        return false;
    }
    if (!generate(out.program, out.code, &error)) {
        out.error = error;
        return false;
    }
    out.stats.peephole = peepholeLines(out.code.lines);      // 窥孔在生成之后
    out.code.asmText = joinLines(out.code.lines);
    out.ok = true;
    return true;
}

} // namespace pl0s
