#ifndef PL0_SUPER_DETAIL_HPP
#define PL0_SUPER_DETAIL_HPP

// pl0-super 的内部小工具（只有 .cpp 用；不进公共接口）。

#include "pl0super.hpp"

#include <cmath>
#include <iomanip>
#include <set>
#include <sstream>
#include <string>

namespace pl0s {
namespace detail {

/** 关键字表（词法与语法共用）。 */
inline const std::set<std::string>& keywords() {
    static const std::set<std::string> table = {
        "program", "const", "var",  "array", "str",  "struct", "ref",     "procedure",
        "begin",   "end",   "if",   "then",  "else", "while",  "do",      "for",
        "to",      "downto", "call", "write", "goto", "label",  "and",     "or",
        "not",     "int",   "float",
    };
    return table;
}

/** 字符能不能当标识符的开头。 */
inline bool identStart(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

/** 字符能不能出现在标识符里。 */
inline bool identPart(char c) { return identStart(c) || (c >= '0' && c <= '9'); }

/** 报错统一带行号。 */
inline std::string lineError(int line, const std::string& what) {
    return "第 " + std::to_string(line) + " 行：" + what;
}

/**
 * 浮点的打印形式：最多 6 位有效数字，去掉多余的 0 与末尾小数点。
 *
 * `2.500000` → `2.5`，`3.000000` → `3`，`0.333333` → `0.333333`。
 *
 * @param value 浮点值
 * @return 打印文本
 */
inline std::string formatFloat(double value) {
    std::ostringstream out;
    out << std::setprecision(6) << value;
    std::string text = out.str();
    const bool plain = text.find('e') == std::string::npos && text.find('E') == std::string::npos;
    if (plain && text.find('.') != std::string::npos) {
        while (!text.empty() && text.back() == '0') text.pop_back();
        if (!text.empty() && text.back() == '.') text.pop_back();
    }
    return text.empty() ? std::string("0") : text;
}

/** 造一个整数常量表达式（解析器与简化器共用）。 */
inline ExprPtr makeNumber(std::int64_t value, int line) {
    auto node = std::make_shared<Expr>();
    node->kind = ExprKind::Number;
    node->number = value;
    node->line = line;
    return node;
}

/** 造一个浮点常量表达式。 */
inline ExprPtr makeFloat(double value, int line) {
    auto node = std::make_shared<Expr>();
    node->kind = ExprKind::Float;
    node->real = value;
    node->isFloat = true;
    node->line = line;
    return node;
}

} // namespace detail
} // namespace pl0s

#endif // PL0_SUPER_DETAIL_HPP
