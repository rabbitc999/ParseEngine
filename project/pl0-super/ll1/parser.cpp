#include "pl0super_detail.hpp"

#include <map>

namespace pl0s {
namespace {

/** 名字里分隔"过程 → 变量 → 字段"的记号（`$` 不在标识符里出现，所以永远不歧义）。 */
constexpr char kSep = '$';

/** 一个类型的描述：整数 / 浮点 / 字符串 / 结构体 / 引用。 */
struct TypeSpec {
    enum class Kind { Int, Float, Str, Struct };
    Kind kind = Kind::Int;
    std::string structName;      ///< kind == Struct 时的结构体名

    bool isText() const { return kind == Kind::Str; }
    bool isFloat() const { return kind == Kind::Float; }
};

/** 符号表里的一条。 */
struct Symbol {
    enum class Kind { Const, Var, Array, Str, StructVar, StructArray, Proc, Ref };
    Kind kind = Kind::Var;
    TypeSpec type;               ///< 标量类型；数组是元素类型；Str 用不到
    std::int64_t value = 0;      ///< Const 的值
    std::int64_t length = 0;     ///< Array 的长度
    std::string mangled;         ///< 扁平化基名（`main$p`），字段访问时再加 `$字段`
    std::string structName;      ///< StructVar / StructArray 的结构体名
    ExprPtr refTarget;           ///< Ref：绑定一次的目标表达式（用的时候克隆）
};

/**
 * 递归下降语法分析器（顺带做名字、类型、标号检查）。
 *
 * 两处刻意"简化"的表示：
 *   1. **结构体按字段扁平化**：`var p: Point;` 落成 `main$p$x`、`main$p$y` 两个变量，
 *      `p.x` 在解析期就解析成普通变量名（目标机不用知道结构体）；
 *   2. **引用是"绑定一次的名字别名"**：`ref r := p.x;` 记下目标表达式，之后每次用 `r`
 *      都展开成目标（可以读也可以写，就是不能重新绑定——所以"没有改地址的权力"）。
 */
class Parser {
public:
    Parser(const std::vector<Token>& tokens, std::string* error)
        : tokens_(tokens), error_(error) {
        scopes_.emplace_back();
    }

    /** 解析整棵程序。 */
    bool run(Program& out) {
        if (!expectKeyword("program")) return false;
        if (!expectIdent(out.name, "程序名")) return false;
        if (!expectSymbol(";")) return false;
        if (!parseDecls(out.decls, "main")) return false;
        if (!expectKeyword("begin")) return false;
        if (!parseStmts(out.body)) return false;
        if (!expectKeyword("end")) return false;
        if (!expectSymbol(".")) return false;
        if (!checkLabels()) return false;
        for (const auto& entry : structs_) out.structs.push_back(entry.second);
        if (error_) error_->clear();
        return true;
    }

private:
    const std::vector<Token>& tokens_;
    std::string* error_ = nullptr;
    std::size_t at_ = 0;
    std::vector<std::map<std::string, Symbol>> scopes_;
    std::map<std::string, StructDef> structs_;                   ///< 结构体声明
    std::string prefix_;                                        ///< 当前过程的变量名前缀
    std::vector<std::pair<std::string, int>> labels_;            ///< 本过程声明的标号
    std::vector<std::pair<std::string, int>> gotos_;             ///< 本过程用到的 goto

    const Token& peek() const { return tokens_[at_ < tokens_.size() ? at_ : tokens_.size() - 1]; }
    const Token& take() {
        const std::size_t index = at_ < tokens_.size() ? at_ : tokens_.size() - 1;
        if (at_ < tokens_.size()) ++at_;
        return tokens_[index];
    }
    bool atEnd() const { return peek().kind == TokKind::End; }
    bool fail(const std::string& what) {
        if (error_ && error_->empty()) *error_ = detail::lineError(peek().line, what);
        return false;
    }
    bool isKeyword(const std::string& text) const {
        return peek().kind == TokKind::Keyword && peek().text == text;
    }
    bool isSymbol(const std::string& text) const {
        return peek().kind == TokKind::Symbol && peek().text == text;
    }
    bool takeKeyword(const std::string& text) {
        if (!isKeyword(text)) return false;
        ++at_;
        return true;
    }
    bool takeSymbol(const std::string& text) {
        if (!isSymbol(text)) return false;
        ++at_;
        return true;
    }
    bool expectKeyword(const std::string& text) {
        return takeKeyword(text) ? true : fail("这里要关键字 " + text);
    }
    bool expectSymbol(const std::string& text) {
        return takeSymbol(text) ? true : fail("这里要符号 " + text);
    }
    bool expectIdent(std::string& out, const std::string& what) {
        if (peek().kind != TokKind::Ident) return fail("这里要" + what);
        out = take().text;
        return true;
    }
    const Symbol* lookup(const std::string& name) const {
        for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
            const auto found = it->find(name);
            if (found != it->end()) return &found->second;
        }
        return nullptr;
    }
    bool definedHere(const std::string& name) const { return scopes_.back().count(name) != 0; }

    /** 类型：`int` / `float` / `str` / 结构体名。 */
    bool parseType(TypeSpec& out, const std::string& what) {
        // `int` / `float` / `str` 是关键字，结构体名是标识符，两种都收
        if (peek().kind != TokKind::Ident && peek().kind != TokKind::Keyword) {
            return fail("这里要" + what + "的类型");
        }
        const std::string name = take().text;
        if (name == "int") { out.kind = TypeSpec::Kind::Int; return true; }
        if (name == "float") { out.kind = TypeSpec::Kind::Float; return true; }
        if (name == "str") { out.kind = TypeSpec::Kind::Str; return true; }
        if (structs_.count(name) != 0) {
            out.kind = TypeSpec::Kind::Struct;
            out.structName = name;
            return true;
        }
        return fail("不认的类型：" + name);
    }

    /** 常量（数组长度、const 初值）。 */
    bool parseConstExpr(std::int64_t& value, const std::string& what) {
        ExprPtr expr;
        if (!parseExpr(expr)) return false;
        if (expr->kind == ExprKind::Number) {
            value = expr->number;
            return true;
        }
        return fail(what + "要是整数常量（不能是变量或浮点）");
    }

    /** 把一个结构体声明的字段抄成可嵌套的树（字段类型也在这里查）。 */
    bool parseStructFields(std::vector<Field>& out) {
        // 字段表用 `( … )`：`{ }` 与 `(* *)` 在本语言里是注释，留给注释用
        if (!expectSymbol("(")) return false;
        while (!takeSymbol(")")) {
            Field field;
            if (!expectIdent(field.name, "字段名")) return false;
            if (!expectSymbol(":")) return false;
            TypeSpec type;
            if (!parseType(type, "字段 " + field.name)) return false;
            if (type.kind == TypeSpec::Kind::Str) {
                return fail("结构体字段暂时不能是 str（要文本请用引用指向外部变量）");
            }
            if (type.kind == TypeSpec::Kind::Struct) {
                // 嵌套结构体：把里层的字段也拍平进来（`p.inner.x`）
                field.kind = TypeKind::Struct;
                field.structName = type.structName;
                field.subs = structs_[type.structName].fields;
            } else {
                field.kind = type.kind == TypeSpec::Kind::Float ? TypeKind::Float : TypeKind::Int;
            }
            out.push_back(std::move(field));
            if (!expectSymbol(";")) return false;
        }
        return true;
    }

    /** 声明组：struct / const / var / array / str / ref / procedure。 */
    bool parseDecls(std::vector<Decl>& out, const std::string& procPrefix) {
        while (!atEnd()) {
            if (isKeyword("begin")) return true;
            if (takeKeyword("struct")) {                       // `struct Point { x: int; };`
                std::string name;
                if (!expectIdent(name, "结构体名")) return false;
                if (structs_.count(name) != 0) return fail("结构体重复定义：" + name);
                if (definedHere(name)) return fail("名字和已有的东西重了：" + name);
                StructDef def;
                def.name = name;
                def.line = peek().line;
                if (!parseStructFields(def.fields)) return false;
                if (!expectSymbol(";")) return false;
                if (def.fields.empty()) return fail("结构体 " + name + " 至少要有一个字段");
                structs_[name] = def;
                continue;
            }
            if (takeKeyword("const")) {
                std::string name;
                if (!expectIdent(name, "常量名")) return false;
                if (!expectSymbol(":=")) return false;
                std::int64_t value = 0;
                if (!parseConstExpr(value, "常量 " + name + " 的值")) return false;
                if (!expectSymbol(";")) return false;
                if (definedHere(name)) return fail("同名重复定义：" + name);
                Symbol symbol;
                symbol.kind = Symbol::Kind::Const;
                symbol.value = value;
                symbol.mangled = procPrefix + kSep + name;
                scopes_.back()[name] = symbol;
                Decl decl;
                decl.kind = DeclKind::Const;
                decl.name = name;
                decl.value = value;
                decl.line = peek().line;
                out.push_back(decl);
                continue;
            }
            if (isKeyword("str") || isKeyword("var")) {        // `var i, j: int;` / `str s;`
                const bool isStr = takeKeyword("str");
                if (!isStr) takeKeyword("var");
                std::vector<std::string> names;
                std::string name;
                if (!expectIdent(name, "变量名")) return false;
                names.push_back(name);
                while (takeSymbol(",")) {
                    if (!expectIdent(name, "变量名")) return false;
                    names.push_back(name);
                }
                TypeSpec type;
                if (isStr) {
                    type.kind = TypeSpec::Kind::Str;
                    if (takeSymbol(":")) {
                        std::string given;
                        if (!expectIdent(given, "类型名")) return false;
                        if (given != "str") return fail("str 声明的类型只能写 str");
                    }
                } else if (!takeSymbol(":") || !parseType(type, "变量")) {
                    return fail("var 要写类型：`var x: float;` / `var p: Point;`");
                }
                if (!expectSymbol(";")) return false;
                for (const std::string& each : names) {
                    if (definedHere(each)) return fail("同名重复定义：" + each);
                    Symbol symbol;
                    if (type.kind == TypeSpec::Kind::Struct) {
                        symbol.kind = Symbol::Kind::StructVar;      // 结构体变量：按字段扁平化
                        symbol.structName = type.structName;
                    } else {
                        symbol.kind =
                            type.kind == TypeSpec::Kind::Str ? Symbol::Kind::Str : Symbol::Kind::Var;
                    }
                    symbol.type = type;
                    symbol.mangled = procPrefix + kSep + each;
                    scopes_.back()[each] = symbol;
                    Decl decl;
                    decl.kind = symbol.kind == Symbol::Kind::StructVar
                                    ? DeclKind::StructVar
                                    : (symbol.kind == Symbol::Kind::Str ? DeclKind::Str
                                                                        : DeclKind::Var);
                    decl.name = symbol.mangled;
                    decl.isFloat = type.isFloat();
                    decl.structName = type.structName;
                    decl.line = peek().line;
                    out.push_back(decl);
                }
                continue;
            }
            if (takeKeyword("array")) {
                std::string name;
                if (!expectIdent(name, "数组名")) return false;
                if (!expectSymbol("[")) return false;
                std::int64_t length = 0;
                if (!parseConstExpr(length, "数组长度")) return false;
                if (!expectSymbol("]")) return false;
                if (!expectSymbol(":")) return false;
                TypeSpec type;
                if (!parseType(type, "数组元素")) return false;
                if (!expectSymbol(";")) return false;
                if (length <= 0) return fail("数组 " + name + " 的长度要大于 0");
                if (definedHere(name)) return fail("同名重复定义：" + name);
                Symbol symbol;
                symbol.type = type;
                symbol.length = length;
                symbol.mangled = procPrefix + kSep + name;
                if (type.kind == TypeSpec::Kind::Struct) {
                    symbol.kind = Symbol::Kind::StructArray;
                    symbol.structName = type.structName;
                } else {
                    symbol.kind = Symbol::Kind::Array;
                }
                scopes_.back()[name] = symbol;
                Decl decl;
                decl.kind = symbol.kind == Symbol::Kind::StructArray ? DeclKind::StructArray
                                                                     : DeclKind::Array;
                decl.name = symbol.mangled;
                decl.length = length;
                decl.isFloat = type.isFloat();
                decl.structName = type.structName;
                decl.line = peek().line;
                out.push_back(decl);
                continue;
            }
            if (takeKeyword("ref")) {                          // `ref r := p.x;`（绑定一次）
                std::string name;
                if (!expectIdent(name, "引用名")) return false;
                if (definedHere(name)) return fail("同名重复定义：" + name);
                if (!expectSymbol(":=")) return false;
                ExprPtr target;
                TypeSpec type;
                bool writable = false;
                if (!parseDesignator(target, type, &writable)) return false;
                if (!writable) return fail("引用的目标要能读写");
                if (!expectSymbol(";")) return false;
                Symbol symbol;
                symbol.kind = Symbol::Kind::Ref;
                symbol.type = type;
                symbol.refTarget = target;
                symbol.mangled = procPrefix + kSep + name;
                scopes_.back()[name] = symbol;
                continue;
            }
            if (takeKeyword("procedure")) {
                std::string name;
                if (!expectIdent(name, "过程名")) return false;
                if (!expectSymbol(";")) return false;
                if (definedHere(name)) return fail("同名重复定义：" + name);
                Symbol symbol;
                symbol.kind = Symbol::Kind::Proc;
                symbol.mangled = procPrefix + kSep + name;
                scopes_.back()[name] = symbol;
                Decl decl;
                decl.kind = DeclKind::Proc;
                decl.name = symbol.mangled;
                decl.line = peek().line;
                const std::string savedPrefix = prefix_;
                const auto savedLabels = labels_;
                const auto savedGotos = gotos_;
                prefix_ = symbol.mangled;
                labels_.clear();
                gotos_.clear();
                scopes_.emplace_back();
                const bool declOk = parseDecls(decl.decls, prefix_);
                bool bodyOk = declOk;
                if (declOk) {
                    bodyOk = expectKeyword("begin") && parseStmts(decl.body) && expectKeyword("end") &&
                             expectSymbol(";");
                }
                const bool labelsOk = bodyOk ? checkLabels() : false;
                scopes_.pop_back();
                prefix_ = savedPrefix;
                labels_ = savedLabels;
                gotos_ = savedGotos;
                if (!bodyOk || !labelsOk) return false;
                out.push_back(std::move(decl));
                continue;
            }
            return fail("声明部分只认 struct / const / var / array / str / ref / procedure");
        }
        return true;
    }

    /** 语句组。 */
    bool parseStmts(std::vector<StmtPtr>& out) {
        while (true) {
            while (takeSymbol(";")) { /* 空语句 */ }
            if (isKeyword("end") || atEnd()) return true;
            StmtPtr stmt;
            if (!parseStmt(stmt)) return false;
            out.push_back(stmt);
            if (!takeSymbol(";")) return true;
        }
    }

    /** 单条语句。 */
    /**
     * **强制语句块**：`if` / `while` / `for` 的分支只能写成 `begin … end`。
     *
     * 这样悬挂 `else`（`if a then if b then s1 else s2`）的移进/归约冲突在文法结构上
     * 就不存在，不需要靠优先级去硬掰；读代码时也不用数 else 归哪个 if。
     *
     * @param out  解析出的块语句
     * @param what 出错信息里怎么称呼它（`if 的 then 分支`…）
     * @return 成功 true
     */
    bool parseBlockStmt(StmtPtr& out, const std::string& what) {
        if (!isKeyword("begin")) {
            return fail(what + "要用 `begin … end` 包起来（本语言强制语句块）");
        }
        return parseStmt(out);
    }

    bool parseStmt(StmtPtr& out) {
        out = std::make_shared<Stmt>();
        out->line = peek().line;
        if (takeKeyword("begin")) {
            out->kind = StmtKind::Block;
            if (!parseStmts(out->body)) return false;
            return expectKeyword("end");
        }
        if (takeKeyword("write")) {
            out->kind = StmtKind::Write;
            ExprPtr value;
            bool writable = false;
            TypeSpec type;
            if (!parseDesignatorOrExpr(value, type, &writable)) return false;
            if (type.kind == TypeSpec::Kind::Struct) return fail("write 不能打印整个结构体（请写字段）");
            out->args.push_back(value);
            return true;
        }
        if (takeKeyword("if")) {
            out->kind = StmtKind::If;
            ExprPtr cond;
            if (!parseExpr(cond)) return false;
            if (cond->isText) return fail("if 的条件不能是字符串");
            out->args.push_back(cond);
            if (!expectKeyword("then")) return false;
            StmtPtr thenStmt;
            if (!parseBlockStmt(thenStmt, "if 的 then 分支")) return false;
            out->body.push_back(thenStmt);
            if (takeKeyword("else")) {
                StmtPtr elseStmt;
                if (!parseBlockStmt(elseStmt, "if 的 else 分支")) return false;
                out->otherwise = elseStmt;
            }
            return true;
        }
        if (takeKeyword("while")) {
            out->kind = StmtKind::While;
            ExprPtr cond;
            if (!parseExpr(cond)) return false;
            if (cond->isText) return fail("while 的条件不能是字符串");
            out->args.push_back(cond);
            if (!expectKeyword("do")) return false;
            StmtPtr body;
            if (!parseBlockStmt(body, "while 的循环体")) return false;
            out->body.push_back(body);
            return true;
        }
        if (takeKeyword("for")) {
            out->kind = StmtKind::For;
            std::string name;
            if (!expectIdent(name, "循环变量")) return false;
            const Symbol* symbol = lookup(name);
            if (symbol == nullptr) return fail("循环变量 " + name + " 没有定义");
            if (symbol->kind != Symbol::Kind::Var) return fail("for 的循环变量要是普通变量：" + name);
            auto target = std::make_shared<Expr>();
            target->kind = ExprKind::Var;
            target->text = symbol->mangled;
            target->isFloat = symbol->type.isFloat();
            target->line = out->line;
            out->target = target;
            if (!expectSymbol(":=")) return false;
            ExprPtr from;
            if (!parseExpr(from)) return false;
            if (from->isText) return fail("for 的初值不能是字符串");
            // 先看 downto，再看 to：写成 `takeKeyword("to") || takeKeyword("downto")` 会把
            // downto 吃成 false（短路求值），循环方向就反了
            const bool down = takeKeyword("downto");
            if (!down && !takeKeyword("to")) return fail("for 里要 to 或 downto");
            out->down = down;
            ExprPtr to;
            if (!parseExpr(to)) return false;
            if (to->isText) return fail("for 的终值不能是字符串");
            if (!expectKeyword("do")) return false;
            out->args.push_back(from);
            out->args.push_back(to);
            StmtPtr body;
            if (!parseBlockStmt(body, "for 的循环体")) return false;
            out->body.push_back(body);
            return true;
        }
        if (takeKeyword("goto")) {
            out->kind = StmtKind::Goto;
            if (!expectIdent(out->name, "标号名")) return false;
            gotos_.push_back({out->name, out->line});
            return true;
        }
        if (takeKeyword("label")) {
            out->kind = StmtKind::Label;
            std::string name;
            if (!expectIdent(name, "标号名")) return false;
            for (const auto& declared : labels_) {
                if (declared.first == name) return fail("标号重复定义：" + name);
            }
            labels_.push_back({name, out->line});
            out->name = name;
            return true;
        }
        if (takeKeyword("call")) {
            out->kind = StmtKind::Call;
            std::string name;
            if (!expectIdent(name, "过程名")) return false;
            const Symbol* symbol = lookup(name);
            if (symbol == nullptr) return fail("过程 " + name + " 没有定义");
            if (symbol->kind != Symbol::Kind::Proc) return fail(name + " 不是过程");
            out->name = symbol->mangled;
            return true;
        }
        return parseAssign(out);
    }

    /** 赋值：`设计式 := 表达式`；结构体变量整体赋值按字段复制。 */
    bool parseAssign(StmtPtr& out) {
        if (peek().kind != TokKind::Ident) return fail("认不出的语句");
        ExprPtr target;
        TypeSpec type;
        bool writable = false;
        const std::size_t back = at_;
        if (!parseDesignator(target, type, &writable)) return false;
        if (!expectSymbol(":=")) {                             // 不是赋值：可能写错了
            at_ = back;
            return fail("认不出的语句（赋值要写 `:=`）");
        }
        out->kind = StmtKind::Assign;
        out->target = target;
        ExprPtr value;
        if (type.kind == TypeSpec::Kind::Struct) {             // 结构体整体赋值
            const std::size_t valueBack = at_;
            TypeSpec valueType;
            bool valueWritable = false;
            ExprPtr source;
            if (parseDesignator(source, valueType, &valueWritable) &&
                valueType.kind == TypeSpec::Kind::Struct &&
                valueType.structName == type.structName) {
                out->copyStruct = type.structName;
                out->args.push_back(source);
                return true;
            }
            at_ = valueBack;
            return fail("结构体只能整体复制另一个同类型结构体（或按字段赋值）");
        }
        if (!parseExpr(value)) return false;
        if (!assignable(type, value, target->text)) return false;
        out->args.push_back(value);
        return true;
    }

    /** 赋值相容性：整数 → 浮点自动提升；浮点 → 整数要写 int(...)。 */
    bool assignable(const TypeSpec& target, const ExprPtr& value, const std::string& what) {
        if (target.isText() != value->isText) {
            return fail(what + " 的类型不符：两边要都是字符串或都是数字");
        }
        if (!target.isText() && !target.isFloat() && value->isFloat) {
            return fail(what + " 是整数，右值是浮点（要转换请写 int(...)）");
        }
        return true;
    }

    /**
     * **设计式**：变量 / 字段 / 数组元素（可作左值）。
     *
     * `p.x`、`pts[i].x`、`p.inner.x` 都在这里解析成"扁平名 + 下标"，引用也在这一步展开。
     *
     * @param out      解析出的表达式
     * @param type     它的类型（结构体整体时是 Struct）
     * @param writable 能不能写（引用可以读写，只是不能再绑定）
     */
    bool parseDesignator(ExprPtr& out, TypeSpec& type, bool* writable) {
        std::string name;
        if (!expectIdent(name, "名字")) return false;
        const Symbol* symbol = lookup(name);
        if (symbol == nullptr) return fail("名字 " + name + " 没有定义");
        if (symbol->kind == Symbol::Kind::Const) {
            out = detail::makeNumber(symbol->value, peek().line);
            type.kind = TypeSpec::Kind::Int;
            if (writable) *writable = false;
            return true;
        }
        if (symbol->kind == Symbol::Kind::Proc) return fail("过程名不能当值用：" + name);
        if (symbol->kind == Symbol::Kind::Ref) {               // 引用：展开成目标
            out = cloneExpr(symbol->refTarget);
            type = symbol->type;
            if (writable) *writable = true;
            return true;
        }
        // 普通变量 / 数组 / 结构体：先落到"扁平基名"
        std::string base = symbol->mangled;
        TypeSpec current = symbol->type;
        if (symbol->kind == Symbol::Kind::StructVar) {
            current.kind = TypeSpec::Kind::Struct;
            current.structName = symbol->structName;
        } else if (symbol->kind == Symbol::Kind::StructArray) {
            current.kind = TypeSpec::Kind::Struct;
            current.structName = symbol->structName;
        }
        // 数组下标：`a[i]` / `pts[i]`
        if (symbol->kind == Symbol::Kind::Array || symbol->kind == Symbol::Kind::StructArray) {
            if (!expectSymbol("[")) return fail("数组 " + name + " 要写下标");
            ExprPtr index;
            if (!parseExpr(index)) return false;
            if (index->isText || index->isFloat) return fail("数组下标要是整数");
            if (!expectSymbol("]")) return false;
            auto node = std::make_shared<Expr>();
            node->kind = ExprKind::Index;
            node->isFloat = symbol->type.isFloat();
            node->line = peek().line;
            auto baseNode = std::make_shared<Expr>();
            baseNode->kind = ExprKind::Var;
            baseNode->text = base;
            baseNode->isFloat = symbol->type.isFloat();
            baseNode->line = node->line;
            node->args.push_back(baseNode);
            node->args.push_back(index);
            out = node;
            type = symbol->type;
            if (writable) *writable = true;
            base = std::string();                              // 下标已经吃掉，后面只能接字段
        } else {
            out = std::make_shared<Expr>();
            out->kind = ExprKind::Var;
            out->text = base;
            out->isFloat = current.isFloat();
            out->isText = current.isText();
            out->line = peek().line;
            type = current;
            if (writable) *writable = symbol->kind != Symbol::Kind::StructVar &&
                                      symbol->kind != Symbol::Kind::StructArray;
        }
        // 字段链：`p.x`、`pts[i].x`、`p.inner.x`
        while (takeSymbol(".")) {
            std::string field;
            if (!expectIdent(field, "字段名")) return false;
            if (type.kind != TypeSpec::Kind::Struct) return fail("只有结构体才有字段：" + field);
            const StructDef& def = structs_[type.structName];
            const Field* found = findField(def.fields, field);
            if (found == nullptr) return fail("结构体 " + type.structName + " 没有字段 " + field);
            const std::string leaf = flattenFieldName(field, *found);
            if (out->kind == ExprKind::Var) {
                out->text += kSep + leaf;
                out->isFloat = found->kind == TypeKind::Float;
                out->isText = false;
            } else if (out->kind == ExprKind::Index) {
                out->args[0]->text += kSep + leaf;
                out->isFloat = found->kind == TypeKind::Float;
            } else {
                return fail("这个设计式不能取字段：" + field);
            }
            if (found->kind == TypeKind::Struct) {
                type.kind = TypeSpec::Kind::Struct;
                type.structName = found->structName;
            } else {
                type.kind = found->kind == TypeKind::Float ? TypeSpec::Kind::Float
                                                           : TypeSpec::Kind::Int;
                type.structName.clear();
            }
            if (writable) *writable = type.kind != TypeSpec::Kind::Struct;
        }
        return true;
    }

    /** 字段名 → 扁平名（嵌套结构体用 `$` 连起来）。 */
    static std::string flattenFieldName(const std::string& name, const Field& field) {
        if (field.subs.empty()) return name;
        std::string out;
        for (const Field& sub : field.subs) {
            if (!out.empty()) out += kSep;
            out += flattenFieldName(name + kSep + sub.name, sub);
        }
        return out;
    }

    /** 在字段树里找名字（一层）。 */
    static const Field* findField(const std::vector<Field>& fields, const std::string& name) {
        for (const Field& field : fields) {
            if (field.name == name) return &field;
        }
        return nullptr;
    }

    /** 表达式里也能用设计式（`write(p.x)` / `a[i] + 1`）。 */
    bool parseDesignatorOrExpr(ExprPtr& out, TypeSpec& type, bool* writable) {
        if (peek().kind == TokKind::Ident) {
            const std::size_t back = at_;
            TypeSpec localType;
            bool localWritable = false;
            if (parseDesignator(out, localType, &localWritable)) {
                // `p . x` 才当设计式；`a + 1` 也是设计式开头，但后面接的是运算符，
                // 由调用方继续处理——这里只回填类型，表达式留给 parseExpr 里的调用点
                if (!isSymbol(".") && !isSymbol("[")) {
                    type = localType;
                    if (writable) *writable = localWritable;
                    return true;
                }
            }
            at_ = back;
        }
        if (!parseExpr(out)) return false;
        type.kind = out->isText ? TypeSpec::Kind::Str
                                : (out->isFloat ? TypeSpec::Kind::Float : TypeSpec::Kind::Int);
        if (writable) *writable = false;
        return true;
    }

    /** 深拷贝一个表达式（引用展开时用，免得同一个节点被多处共享）。 */
    static ExprPtr cloneExpr(const ExprPtr& expr) {
        if (!expr) return expr;
        auto copy = std::make_shared<Expr>();
        copy->kind = expr->kind;
        copy->text = expr->text;
        copy->number = expr->number;
        copy->real = expr->real;
        copy->op = expr->op;
        copy->isText = expr->isText;
        copy->isFloat = expr->isFloat;
        copy->line = expr->line;
        for (const ExprPtr& child : expr->args) copy->args.push_back(cloneExpr(child));
        return copy;
    }

    bool parseExpr(ExprPtr& out) { return parseOr(out); }
    bool parseOr(ExprPtr& out) {
        if (!parseAnd(out)) return false;
        while (takeKeyword("or")) {
            ExprPtr right;
            if (!parseAnd(right)) return false;
            if (out->isText || right->isText) return fail("or 的两侧要是数字（0/1）");
            out = makeBinary("or", out, right);
        }
        return true;
    }
    bool parseAnd(ExprPtr& out) {
        if (!parseCompare(out)) return false;
        while (takeKeyword("and")) {
            ExprPtr right;
            if (!parseCompare(right)) return false;
            if (out->isText || right->isText) return fail("and 的两侧要是数字（0/1）");
            out = makeBinary("and", out, right);
        }
        return true;
    }
    bool parseCompare(ExprPtr& out) {
        if (!parseAdd(out)) return false;
        while (isSymbol("<") || isSymbol("<=") || isSymbol(">") || isSymbol(">=") ||
               isSymbol("=") || isSymbol("<>")) {
            const std::string op = take().text;
            ExprPtr right;
            if (!parseAdd(right)) return false;
            if (out->isText != right->isText) return fail("比较的两边类型不同（字符串与数字不能比）");
            out = makeBinary(op, out, right);
        }
        return true;
    }
    bool parseAdd(ExprPtr& out) {
        if (!parseMul(out)) return false;
        while (isSymbol("+") || isSymbol("-")) {
            const std::string op = take().text;
            ExprPtr right;
            if (!parseMul(right)) return false;
            if (out->isText || right->isText) {
                if (op != "+") return fail("字符串只支持 +（拼接）");
                if (!out->isText || !right->isText) return fail("字符串拼接的两边都要是字符串");
            }
            out = makeBinary(op, out, right);
        }
        return true;
    }
    bool parseMul(ExprPtr& out) {
        if (!parseUnary(out)) return false;
        while (isSymbol("*") || isSymbol("/") || isSymbol("%")) {
            const std::string op = take().text;
            ExprPtr right;
            if (!parseUnary(right)) return false;
            if (out->isText || right->isText) return fail(op + " 不能用在字符串上");
            if (op == "%" && (out->isFloat || right->isFloat)) return fail("% 只支持整数");
            out = makeBinary(op, out, right);
        }
        return true;
    }
    bool parseUnary(ExprPtr& out) {
        if (isSymbol("-")) {
            ++at_;
            if (!parseUnary(out)) return false;
            if (out->isText) return fail("负号不能用在字符串上");
            if (out->kind == ExprKind::Number) {
                out = detail::makeNumber(-out->number, out->line);
                return true;
            }
            if (out->kind == ExprKind::Float) {
                out = detail::makeFloat(-out->real, out->line);
                return true;
            }
            auto node = std::make_shared<Expr>();
            node->kind = ExprKind::Unary;
            node->op = "neg";
            node->isFloat = out->isFloat;
            node->line = out->line;
            node->args.push_back(out);
            out = node;
            return true;
        }
        if (takeKeyword("not")) {
            if (!parseUnary(out)) return false;
            if (out->isText) return fail("not 不能用在字符串上");
            auto node = std::make_shared<Expr>();
            node->kind = ExprKind::Unary;
            node->op = "not";
            node->line = out->line;
            node->args.push_back(out);
            out = node;
            return true;
        }
        return parsePrimary(out);
    }
    bool parsePrimary(ExprPtr& out) {
        if (peek().kind == TokKind::Number) {
            const Token token = take();
            out = token.isFloat ? detail::makeFloat(token.real, token.line)
                                : detail::makeNumber(token.number, token.line);
            return true;
        }
        if (peek().kind == TokKind::Text) {
            const Token token = take();
            out = std::make_shared<Expr>();
            out->kind = ExprKind::Text;
            out->text = token.text;
            out->isText = true;
            out->line = token.line;
            return true;
        }
        if (takeSymbol("(")) {
            if (!parseExpr(out)) return false;
            return expectSymbol(")");
        }
        // 类型转换：`int(x)` / `float(x)`（`int`、`float` 是关键字，单独在这里认）
        if ((isKeyword("int") || isKeyword("float")) && at_ + 1 < tokens_.size() &&
            tokens_[at_ + 1].kind == TokKind::Symbol && tokens_[at_ + 1].text == "(") {
            const Token keyword = take();
            expectSymbol("(");
            ExprPtr inner;
            if (!parseExpr(inner)) return false;
            if (!expectSymbol(")")) return false;
            if (inner->isText) return fail(keyword.text + "(...) 不能用在字符串上");
            out = makeConvert(keyword.text, inner, keyword.line);
            return true;
        }
        if (peek().kind == TokKind::Ident) {
            const Token token = peek();
            if ((token.text == "int" || token.text == "float") && at_ + 1 < tokens_.size() &&
                tokens_[at_ + 1].kind == TokKind::Symbol && tokens_[at_ + 1].text == "(") {
                at_ += 2;
                ExprPtr inner;
                if (!parseExpr(inner)) return false;
                if (!expectSymbol(")")) return false;
                if (inner->isText) return fail(token.text + "(...) 不能用在字符串上");
                out = makeConvert(token.text, inner, token.line);
                return true;
            }
            TypeSpec type;
            bool writable = false;
            if (parseDesignator(out, type, &writable)) {
                if (type.kind == TypeSpec::Kind::Struct) {
                    return fail("结构体整体不能当值用（请写字段，或者整体赋值给另一个同类型变量）");
                }
                return true;
            }
            return false;
        }
        return fail("这里要一个表达式");
    }
    /** 造一个类型转换节点；操作数是常量时直接折成常量（`float(2)` → `2.0`）。 */
    static ExprPtr makeConvert(const std::string& target, const ExprPtr& inner, int line) {
        if (inner->kind == ExprKind::Number && target == "float") {
            return detail::makeFloat(static_cast<double>(inner->number), line);
        }
        if (inner->kind == ExprKind::Float && target == "int") {
            return detail::makeNumber(static_cast<std::int64_t>(inner->real), line);
        }
        auto node = std::make_shared<Expr>();
        node->kind = ExprKind::Convert;
        node->text = target;
        node->isFloat = target == "float";
        node->line = line;
        node->args.push_back(inner);
        return node;
    }

    ExprPtr makeBinary(const std::string& op, const ExprPtr& left, const ExprPtr& right) {
        auto node = std::make_shared<Expr>();
        node->kind = ExprKind::Binary;
        node->op = op;
        node->line = left->line;
        node->isText = left->isText || right->isText;
        node->isFloat = !node->isText && (left->isFloat || right->isFloat);
        node->args.push_back(left);
        node->args.push_back(right);
        return node;
    }
    bool checkLabels() {
        for (const auto& jump : gotos_) {
            bool found = false;
            for (const auto& declared : labels_) {
                if (declared.first == jump.first) found = true;
            }
            if (!found) return fail("goto " + jump.first + " 没有对应的 label");
        }
        return true;
    }
};

} // namespace

bool parse(const std::vector<Token>& tokens, Program& out, std::string* error) {
    Parser parser(tokens, error);
    return parser.run(out);
}

bool parseSource(const std::string& source, Program& out, std::string* error) {
    std::string local;
    const std::vector<Token> tokens = tokenize(source, &local);
    if (!local.empty()) {
        if (error) *error = local;
        return false;
    }
    return parse(tokens, out, error);
}

} // namespace pl0s
