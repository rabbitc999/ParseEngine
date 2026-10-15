#include "pl0super_detail.hpp"

namespace pl0s {
namespace {

/**
 * 简化器：树上跑三趟——常量折叠 / 代数化简 / 死分支。
 *
 * 每改一处就给对应的计数 +1（CLI 的 `--stats` 会打印），方便看"简化到底省了什么"。
 * 窥孔在代码生成之后做（`codegen.cpp` 里），它也记在同一个统计里。
 */
class Simplifier {
public:
    Simplifier(SimplifyStats* stats, std::string* error) : stats_(stats), error_(error) {}

    /** 整棵程序（声明里的过程体也要走一遍）。 */
    bool run(Program& program) {
        if (!runDecls(program.decls)) return false;
        return foldStmts(program.body);
    }

private:
    SimplifyStats* stats_ = nullptr;
    std::string* error_ = nullptr;

    void count(int SimplifyStats::*field) {
        if (stats_) ++(stats_->*field);
    }

    bool runDecls(std::vector<Decl>& decls) {
        for (Decl& decl : decls) {
            if (decl.kind != DeclKind::Proc) continue;
            if (!runDecls(decl.decls)) return false;
            if (!foldStmts(decl.body)) return false;
        }
        return true;
    }

    /** 语句序列：先把表达式折了，再按常量条件砍分支。 */
    bool foldStmts(std::vector<StmtPtr>& stmts) {
        std::vector<StmtPtr> kept;
        for (StmtPtr& stmt : stmts) {
            if (!stmt) continue;
            switch (stmt->kind) {
                case StmtKind::Assign:
                    stmt->args[0] = fold(stmt->args[0]);
                    if (!stmt->args[0]) return false;
                    break;
                case StmtKind::Write:
                    stmt->args[0] = fold(stmt->args[0]);
                    if (!stmt->args[0]) return false;
                    break;
                case StmtKind::If: {
                    stmt->args[0] = fold(stmt->args[0]);
                    if (!stmt->args[0]) return false;
                    if (!foldStmts(stmt->body)) return false;
                    if (stmt->otherwise) {
                        // else 分支也当成一条语句序列来折（它可能是块，也可能是单条）
                        std::vector<StmtPtr> one{stmt->otherwise};
                        if (!foldStmts(one)) return false;
                        stmt->otherwise = one.empty() ? nullptr : one.front();
                    }
                    if (stmt->args[0]->kind == ExprKind::Number) {          // 常量条件
                        const bool take = stmt->args[0]->number != 0;
                        count(&SimplifyStats::deadBranch);
                        if (take) {
                            for (const StmtPtr& inner : stmt->body) kept.push_back(inner);
                        } else if (stmt->otherwise) {
                            kept.push_back(stmt->otherwise);
                        }
                        continue;
                    }
                    break;
                }
                case StmtKind::While:
                    stmt->args[0] = fold(stmt->args[0]);
                    if (!stmt->args[0]) return false;
                    if (!foldStmts(stmt->body)) return false;
                    if (stmt->args[0]->kind == ExprKind::Number && stmt->args[0]->number == 0) {
                        count(&SimplifyStats::deadBranch);                  // `while 0` 整块删掉
                        continue;
                    }
                    break;
                case StmtKind::For:
                    stmt->args[0] = fold(stmt->args[0]);
                    stmt->args[1] = fold(stmt->args[1]);
                    if (!stmt->args[0] || !stmt->args[1]) return false;
                    if (!foldStmts(stmt->body)) return false;
                    // 初值、终值都是常量、方向已经走完 → 整块删掉
                    if (stmt->args[0]->kind == ExprKind::Number &&
                        stmt->args[1]->kind == ExprKind::Number) {
                        const std::int64_t from = stmt->args[0]->number;
                        const std::int64_t to = stmt->args[1]->number;
                        if ((!stmt->down && from > to) || (stmt->down && from < to)) {
                            count(&SimplifyStats::deadBranch);
                            continue;
                        }
                    }
                    break;
                case StmtKind::Block:
                    if (!foldStmts(stmt->body)) return false;
                    break;
                default:
                    break;                                                  // call / goto / label 没表达式
            }
            kept.push_back(stmt);
        }
        stmts.swap(kept);
        return true;
    }

    /** 表达式：先折子节点，再折自己。 */
    ExprPtr fold(const ExprPtr& expr) {
        if (!expr) return expr;
        for (ExprPtr& child : expr->args) {
            child = fold(child);
            if (!child) return nullptr;
        }
        if (expr->kind == ExprKind::Binary) return foldBinary(expr);
        if (expr->kind == ExprKind::Unary) return foldUnary(expr);
        if (expr->kind == ExprKind::Convert) return foldConvert(expr);
        return expr;
    }

    /** 类型转换的常量折叠：`float(2)` → `2.0`、`int(3.7)` → `3`。 */
    ExprPtr foldConvert(const ExprPtr& expr) {
        const ExprPtr& inner = expr->args[0];
        if (inner->kind == ExprKind::Number && expr->text == "float") {
            count(&SimplifyStats::folded);
            return detail::makeFloat(static_cast<double>(inner->number), expr->line);
        }
        if (inner->kind == ExprKind::Float && expr->text == "int") {
            count(&SimplifyStats::folded);
            return detail::makeNumber(static_cast<std::int64_t>(inner->real), expr->line);
        }
        return expr;
    }

    /** 一元：取负与取反的常量折叠 + `--x`、`not not x` 化简。 */
    ExprPtr foldUnary(const ExprPtr& expr) {
        const ExprPtr& inner = expr->args[0];
        if (expr->op == "neg") {
            if (inner->kind == ExprKind::Number) {
                count(&SimplifyStats::folded);
                return detail::makeNumber(-inner->number, expr->line);
            }
            if (inner->kind == ExprKind::Float) {
                count(&SimplifyStats::folded);
                return detail::makeFloat(-inner->real, expr->line);
            }
            if (inner->kind == ExprKind::Unary && inner->op == "neg") {
                count(&SimplifyStats::algebra);                            // --x → x
                return inner->args[0];
            }
            return expr;
        }
        if (expr->op == "not") {
            if (inner->kind == ExprKind::Number) {
                count(&SimplifyStats::folded);
                return detail::makeNumber(inner->number == 0 ? 1 : 0, expr->line);
            }
            if (inner->kind == ExprKind::Unary && inner->op == "not") {
                count(&SimplifyStats::algebra);                            // not not x → x
                return inner->args[0];
            }
        }
        return expr;
    }

    /** 二元：常量折叠 + 代数化简。 */
    ExprPtr foldBinary(const ExprPtr& expr) {
        const ExprPtr& left = expr->args[0];
        const ExprPtr& right = expr->args[1];
        const std::string& op = expr->op;
        if (expr->isText) {                                                // 字符串拼接
            if (left->kind == ExprKind::Text && right->kind == ExprKind::Text) {
                auto node = std::make_shared<Expr>();
                node->kind = ExprKind::Text;
                node->text = left->text + right->text;
                node->isText = true;
                node->line = expr->line;
                count(&SimplifyStats::folded);
                return node;
            }
            return expr;
        }
        const bool leftConst = left->kind == ExprKind::Number || left->kind == ExprKind::Float;
        const bool rightConst = right->kind == ExprKind::Number || right->kind == ExprKind::Float;
        const bool isFloat = left->isFloat || right->isFloat;
        const double lhs = left->kind == ExprKind::Float ? left->real
                                                         : static_cast<double>(left->number);
        const double rhs = right->kind == ExprKind::Float ? right->real
                                                          : static_cast<double>(right->number);
        const auto number = [&](double value) {
            return isFloat ? detail::makeFloat(value, expr->line)
                           : detail::makeNumber(static_cast<std::int64_t>(value), expr->line);
        };
        const auto boolean = [&](bool value) {
            return detail::makeNumber(value ? 1 : 0, expr->line);
        };
        if (leftConst && rightConst) {                                     // 常量折叠
            if (op == "+") { count(&SimplifyStats::folded); return number(lhs + rhs); }
            if (op == "-") { count(&SimplifyStats::folded); return number(lhs - rhs); }
            if (op == "*") { count(&SimplifyStats::folded); return number(lhs * rhs); }
            if (op == "/") {
                if (!isFloat && static_cast<std::int64_t>(rhs) == 0) {
                    if (error_) *error_ = detail::lineError(expr->line, "除数是常量 0");
                    return nullptr;
                }
                if (isFloat && rhs == 0.0) {
                    if (error_) *error_ = detail::lineError(expr->line, "除数是常量 0");
                    return nullptr;
                }
                count(&SimplifyStats::folded);
                return isFloat ? detail::makeFloat(lhs / rhs, expr->line)
                               : detail::makeNumber(static_cast<std::int64_t>(lhs) /
                                                        static_cast<std::int64_t>(rhs),
                                                    expr->line);
            }
            if (op == "%") {
                if (static_cast<std::int64_t>(rhs) == 0) {
                    if (error_) *error_ = detail::lineError(expr->line, "% 的除数是常量 0");
                    return nullptr;
                }
                count(&SimplifyStats::folded);
                return detail::makeNumber(static_cast<std::int64_t>(lhs) %
                                              static_cast<std::int64_t>(rhs),
                                          expr->line);
            }
            if (op == "<") { count(&SimplifyStats::folded); return boolean(lhs < rhs); }
            if (op == "<=") { count(&SimplifyStats::folded); return boolean(lhs <= rhs); }
            if (op == ">") { count(&SimplifyStats::folded); return boolean(lhs > rhs); }
            if (op == ">=") { count(&SimplifyStats::folded); return boolean(lhs >= rhs); }
            if (op == "=") { count(&SimplifyStats::folded); return boolean(lhs == rhs); }
            if (op == "<>") { count(&SimplifyStats::folded); return boolean(lhs != rhs); }
            if (op == "and") { count(&SimplifyStats::folded); return boolean(lhs != 0 && rhs != 0); }
            if (op == "or") { count(&SimplifyStats::folded); return boolean(lhs != 0 || rhs != 0); }
        }
        // 代数化简（只动整数，浮点保持原样，免得改变求值顺序）
        const bool leftZero = left->kind == ExprKind::Number && left->number == 0;
        const bool rightZero = right->kind == ExprKind::Number && right->number == 0;
        const bool leftOne = left->kind == ExprKind::Number && left->number == 1;
        const bool rightOne = right->kind == ExprKind::Number && right->number == 1;
        if (op == "+") {
            if (rightZero && !isFloat) { count(&SimplifyStats::algebra); return left; }
            if (leftZero && !isFloat) { count(&SimplifyStats::algebra); return right; }
        }
        if (op == "-" && rightZero && !isFloat) {
            count(&SimplifyStats::algebra);
            return left;
        }
        if (op == "*") {
            if (rightZero || leftZero) {
                count(&SimplifyStats::algebra);
                return detail::makeNumber(0, expr->line);
            }
            if (rightOne && !isFloat) { count(&SimplifyStats::algebra); return left; }
            if (leftOne && !isFloat) { count(&SimplifyStats::algebra); return right; }
        }
        if (op == "/" && rightOne && !isFloat) {
            count(&SimplifyStats::algebra);
            return left;
        }
        return expr;
    }
};

} // namespace

bool simplify(Program& program, std::string* error, SimplifyStats* stats) {
    if (error) error->clear();
    Simplifier simplifier(stats, error);
    if (!simplifier.run(program)) return false;
    if (error) error->clear();
    return true;
}

} // namespace pl0s
