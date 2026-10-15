#pragma once
// `.gram` 文法文档模型与分析：给编辑器用的容错扫描 + 诊断 / 悬停 / 补全 /
// 跳转 / 大纲 / 语义高亮 / 保守格式化 / 快速修复。
//
// 共用的值类型（位置 / 区间 / 诊断 / 悬停 / 补全 …）在 `types.hpp` 里；这里只放文法
// 自己的声明与文档模型。
#include <cstddef>
#include <string>
#include <vector>

#include "types.hpp"

namespace lsp {

/**
 * 一个声明出来的终结符（%token / %define / %regex / 优先级声明）。
 */
struct TokenDecl {
    std::string name;
    std::string pattern;    // 正则或字面量原文（没有就是外部产生）
    std::string alias;      // alias "..." 里的显示别名
    std::string member;     // %token <成员> 的成员名
    std::string csvPath;    // `%token NAME include "表.csv"` 里的 CSV 路径（没有就是空）
    std::string directive;  // 来自哪条指令
    Range range;            // 名字的位置（跳转目标）
    bool literal = false;   // 是固定字面量
};

/**
 * 一条产生式（同名产生式只留一条，alternatives 记录候选数量）。
 */
struct RuleDecl {
    std::string name;
    Range range;
    std::vector<std::size_t> lines;      // 出现过的行（0 基）
    std::vector<Range> references;       // 右部引用（标识符或字面量）的位置
    int alternatives = 1;
    bool hasAction = false;
    bool used = false;
};

/** 一条 `%指令`（保留原文参数，悬停与大纲用）。 */
struct DirectiveDecl {
    std::string name;       // 含 `%`
    Range range;
    std::string argument;   // 该行剩余文本（去掉注释）
};

/**
 * 规则右部的一次引用（用于定义跳转、未声明符号检查与占用统计）。
 */
struct Reference {
    std::string from;      // 所属产生式
    std::string name;      // 引用的名字或字面量
    Range range;
    bool literal = false;  // 是 "字面量" 而不是标识符
};

/**
 * 一块文法文档：一次 setText 会重扫并重算诊断。
 */
class GrammarDocument {
public:
    /**
     * 换掉整份文档文本并**重新扫描与重算诊断**（等价于一次全文同步）。
     *
     * 这是唯一的写入口：诊断、指令表、终结符、产生式、引用、语义高亮结果
     * 全部由它派生，所以调用一次之后再读其它 getter 才是最新状态。
     *
     * @param text 新的全文（UTF-8；行列按 UTF-16 代码单元换算，见文件头约定）
     */
    void setText(std::string text);

    /** 当前全文（与 `setText` 传入的完全一致，不做换行或空白归一化）。 */
    const std::string& text() const { return text_; }

    /** 诊断列表：错误 / 警告 / 提示三级，`Diagnostic::code` 是稳定短码，便于测试与抑制。 */
    const std::vector<Diagnostic>& diagnostics() const { return diagnostics_; }

    /** 文档里出现的 `%指令`（保留原始参数文本，悬停与大纲用）。 */
    const std::vector<DirectiveDecl>& directives() const { return directives_; }

    /** 声明出来的终结符：`%token` / `%define` / `%regex` / 优先级声明。 */
    const std::vector<TokenDecl>& tokens() const { return tokens_; }

    /**
     * 产生式列表；同名产生式合并成一条，`alternatives` 记录候选数量，
     * `references` 汇总所有候选右部的引用（跳转定义与未声明符号检查用）。
     */
    const std::vector<RuleDecl>& rules() const { return rules_; }
    /**
     * 规则区（第一个 `%%` 之后）的起始位置；没有就是文件末尾。
     */
    Position rulesSectionStart() const { return rulesStart_; }
    /**
     * 指令区结束（第一个 `%%` 之前）的插入位置，快速修复用。
     */
    Position directiveInsertPosition() const { return directiveInsert_; }

    /**
     * 文档大纲：指令 / 终结符 / 产生式三类符号，带显示用详情与可跳转区间。
     *
     * @return 符号列表（顺序稳定，便于测试与 diff）
     */
    std::vector<SymbolInfo> symbols() const;

    /** 语义高亮的 token 列表（类型名是 LSP 语义类型，如 `keyword`、`class`）。 */
    std::vector<SemanticToken> semanticTokens() const;

    /**
     * 光标处的悬停信息（Markdown）。
     *
     * @param position 光标位置（0 基行列，UTF-16 列）
     * @return 未命中时 `found == false`；命中时带可高亮区间与 Markdown 正文
     */
    HoverResult hover(Position position) const;

    /**
     * 光标处的补全候选：指令名、指令取值、模式 / 语言名、已声明终结符等。
     *
     * @param position 光标位置；按所在行的前缀决定给出哪一类候选
     * @return 候选列表；无处可补时返回空列表（调用方不必特判）
     */
    std::vector<CompletionItem> completion(Position position) const;

    /**
     * 跳转定义：光标在产生式右部引用上时，返回符号声明处的位置。
     *
     * @param position 光标位置
     * @return 声明区间列表（同名多声明时会有多项；未命中返回空列表）
     */
    std::vector<Range> definition(Position position) const;

    /**
     * 快速修复候选（由诊断派生）；目前提供"为隐式终结符补声明"一条。
     *
     * @return 可直接映射成 LSP CodeAction 的补丁列表
     */
    std::vector<CodeAction> codeActions() const;
    /**
     * 保守格式化：去行尾空白、折叠多余空行、保证单个结尾换行。
     */
    std::string format() const;
    /**
     * `--check` 用的文本报告：`FILE:行:列: 级别: 消息`。
     */
    std::string checkReport(const std::string& path) const;

    /** 指令说明表（补全与悬停共用）。 */
    static const std::vector<std::pair<std::string, std::string>>& directiveTable();

    /** 全部 `%mode` 取值名（补全用），顺序固定。 */
    static const std::vector<std::string>& modeNames();

    /** 全部 `--lang` 目标语言名（补全用），顺序固定。 */
    static const std::vector<std::string>& languageNames();

private:
    /**
     * 容错扫描全文：识别指令、终结符、产生式、引用与 %% 分段，并记下诊断用的位置。扫描不假设输入合法，越界 / 未闭合只记诊断。内部使用，由 setText 调用。
     */
    void scan();
    /**
     * 在扫描结果之上建模型：合并同名产生式、汇总引用与占用、算出规则区与指令插入位置，并生成诊断列表。内部使用，由 setText 调用。
     */
    void buildModel();
    void addDiagnostic(const Range& range, Severity severity, const std::string& code,
                       const std::string& message);
    /**
     * 按名字查已声明的终结符（悬停、跳转与未声明检查共用）。
     *
     * @param name 名字或字面量原文
     * @return 命中时指向声明；没有返回 nullptr
     */
    const TokenDecl* findToken(const std::string& name) const;
    /**
     * 按名字查产生式（跳转定义与大纲共用）。
     *
     * @param name 产生式名
     * @return 命中时指向该产生式；没有返回 nullptr
     */
    const RuleDecl* findRule(const std::string& name) const;
    /**
     * 这个区间是不是 `%token` 的行内关键字（`include` / `alias`）。
     *
     * 语义高亮与悬停共用：同名标识符出现在别处（例如一条叫 `include` 的规则）
     * 不该被当成关键字。
     *
     * @param range 待判断区间
     * @return 是行内关键字返回 true
     */
    bool isInlineKeyword(const Range& range) const;

    std::string text_;
    std::vector<Diagnostic> diagnostics_;
    std::vector<DirectiveDecl> directives_;
    std::vector<TokenDecl> tokens_;
    std::vector<RuleDecl> rules_;
    std::vector<Reference> references_;
    std::string startName_;
    Range startRange_;
    std::vector<std::pair<std::string, Range>> syncs_;
    /**
     * `%token` 行内关键字的出现位置（`include` / `alias`）。
     *
     * 这两个词**不是 Token 名**，也不该被当成 `%token` 的参数：`include` 后面跟的是
     * CSV 别名表路径（`%token NAME include "表.csv"`），`alias` 后面跟的是显示名。
     * 记下来给语义高亮（着成 keyword 而不是 function）与悬停用。
     */
    std::vector<Range> inlineKeywords_;
    Position insertPosition_;
    Position rulesStart_;
    Position directiveInsert_;
};

} // namespace lsp
