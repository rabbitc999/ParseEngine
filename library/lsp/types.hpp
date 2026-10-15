#pragma once
// 语言服务器共用的值类型：位置 / 区间 / 诊断 / 悬停 / 补全 / 大纲 / 语义高亮 / 编辑 / 代码动作。
//
// 位置约定与 LSP 一致：行、列都从 0 开始，列是 **UTF-16 代码单元**偏移
// （中文一个字算 1 列，非 BMP 字符算 2 列）。
//
// `.gram` 的模型（`grammar.hpp` 的 GrammarDocument）与 Magel 的宏模型
// （`magel/lsp/mc.hpp` 的 McDocument）都只用这里的类型，不互相依赖。
#include <cstddef>
#include <string>
#include <vector>

namespace lsp {

struct Position {
    std::size_t line = 0;
    std::size_t column = 0;
};
struct Range {
    Position start;
    Position end;
};
/** @brief 位置排序：先比行，再比列。 @param a 左。 @param b 右。 @return a 在 b 前返回 true。 */
inline bool operator<(Position a, Position b) {
    return a.line != b.line ? a.line < b.line : a.column < b.column;
}
/**
 * 判断位置是否落在区间内（左闭右开：start <= p < end）。
 *
 * @param range 待判断区间（0 基行列，列按 UTF-16 代码单元）
 * @param position 待判断位置
 * @return 落在区间内返回 true
 */
inline bool contains(const Range& range, Position position) {
    if (position < range.start) return false;
    return position < range.end;
}

enum class Severity { Error = 1, Warning = 2, Information = 3, Hint = 4 };

struct Diagnostic {
    Range range;
    Severity severity = Severity::Error;
    std::string code;    // 稳定短码，便于测试与抑制
    std::string message;
};

struct SymbolInfo {
    std::string name;
    std::string kind;    // 语义种类（"directive" / "token" / "macro" …）
    std::string detail;  // 悬停/大纲里显示的补充说明
    Range range;
};

struct SemanticToken {
    Range range;
    std::string type;    // 语义 token 类型名（见 initialize 的 legend）
};

struct HoverResult {
    bool found = false;
    Range range;
    std::string markdown;
};

struct CompletionItem {
    std::string label;
    std::string kind;
    std::string detail;
    std::string insertText;
};

struct TextEdit {
    Range range;
    std::string newText;
};

struct CodeAction {
    std::string title;
    std::string kind;
    TextEdit edit;
};

} // namespace lsp
