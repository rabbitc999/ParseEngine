/** @file regex_compatibility.cpp
 * @brief 词法正则的实际引擎、共享语义与原生能力分类。
 */
#include "ParseEngine/regex_compatibility.hpp"
#include "ParseEngine/regex_dfa.hpp"
#include "ParseEngine/regex_vm.hpp"
#include <algorithm>
#include <set>
#include <sstream>

namespace ParseEngine
{
namespace
{
/** @brief AST 及原文中可确认的能力，原文扫描不把转义后的反斜线当成操作符。 */
struct Traits
{
    bool unicode = false, ignoreCase = false, lookAhead = false, lookBehind = false;
    bool backref = false, properties = false;
    std::set<std::string> names;
};

/** @brief 收集 AST 能力。@param node 子树。@param traits 收集结果。 */
void visit(const std::shared_ptr<RegexNode> &node, Traits &traits)
{
    if (!node) return;
    using Kind = RegexNode::Kind;
    traits.unicode = traits.unicode || node->unicode;
    traits.ignoreCase = traits.ignoreCase || node->ignoreCase;
    if (node->lazy) traits.names.insert("非贪婪");
    switch (node->kind) {
    case Kind::Group: traits.names.insert("捕获"); break;
    case Kind::LookAhead: traits.lookAhead = true; traits.names.insert("前瞻"); break;
    case Kind::NegativeLookAhead: traits.lookAhead = true; traits.names.insert("负前瞻"); break;
    case Kind::Backref: traits.backref = true; traits.names.insert("反向引用"); break;
    case Kind::WordBoundary: case Kind::NotWordBoundary: traits.names.insert("词边界"); break;
    case Kind::Assertion: traits.names.insert("锚点"); break;
    case Kind::Alt: traits.names.insert("分支"); break;
    case Kind::Star: case Kind::Plus: case Kind::Opt: case Kind::Repeat: traits.names.insert("重复"); break;
    default: break;
    }
    visit(node->left, traits);
    visit(node->right, traits);
}

/** @brief 补充 AST 不支持的后顾、属性转义及预定义类信息。
 * @param pattern 原模式。@param traits 收集结果。@param parsed 是否有完整 AST。
 */
void scan(const std::string &pattern, Traits &traits, bool parsed)
{
    bool inClass = false;
    traits.unicode = RegexVm::unicodeMode(pattern);
    for (std::size_t index = 0; index < pattern.size(); ++index) {
        const char ch = pattern[index];
        if (ch == '\\' && index + 1 < pattern.size()) {
            const char escaped = pattern[++index];
            if (!parsed && !inClass && escaped >= '1' && escaped <= '9') {
                traits.backref = true; traits.names.insert("反向引用");
            }
            if (escaped == 'p' || escaped == 'P') {
                traits.properties = true; traits.names.insert("Unicode 属性");
            }
            if (std::string("dDsSwW").find(escaped) != std::string::npos)
                traits.names.insert("预定义字符类");
            continue;
        }
        if (ch == '[') { inClass = true; traits.names.insert("字符类"); }
        else if (ch == ']') inClass = false;
        else if (!parsed && !inClass && (pattern.compare(index, 4, "(?<=") == 0 ||
                              pattern.compare(index, 4, "(?<!") == 0)) {
            traits.lookBehind = true; traits.names.insert("后顾");
        }
        else if (!parsed && !inClass && (pattern.compare(index, 3, "(?=") == 0 ||
                              pattern.compare(index, 3, "(?!") == 0)) {
            traits.lookAhead = true;
            traits.names.insert(pattern[index + 2] == '=' ? "前瞻" : "负前瞻");
        }
    }
    // 即使其它原生扩展使 AST 校验失败，也保留全局前缀能力。
    for (std::size_t start = 0; pattern.compare(start, 2, "(?") == 0;) {
        const auto end = pattern.find(')', start + 2);
        if (end == std::string::npos) break;
        const auto flags = pattern.substr(start + 2, end - start - 2);
        if (flags.empty() || flags.find_first_not_of("imsuxU-") != std::string::npos) break;
        traits.ignoreCase = traits.ignoreCase || flags.find('i') != std::string::npos;
        start = end + 1;
    }
    if (traits.unicode) traits.names.insert("Unicode 模式");
    if (traits.ignoreCase) traits.names.insert("忽略大小写");
}

/** @brief 限制跨原生库的等价证明范围，未知转义和原生特性不冒充已验证。
 * @param pattern 原模式。@return 仅包含保守 ASCII 结构时为真。
 */
bool portableAscii(const std::string &pattern)
{
    return RegexCompatibility::cppDfaCompatible(pattern) && pattern.find('\\') == std::string::npos;
}

/** @brief 原生引擎名称。@param language 目标。@return 引擎标识。 */
std::string nativeEngine(const std::string &language)
{
    if (language == "cpp17") return "std::regex";
    if (language == "python") return "re";
    if (language == "csharp") return "System.Text.RegularExpressions.Regex";
    if (language == "rust") return "regex";
    if (language == "go") return "regexp";
    if (language == "java") return "java.util.regex.Pattern";
    if (language == "js") return "RegExp";
    return "unknown";
}

/** @brief 转义 Markdown 表格及 HTML/内联标记，保留原始模式的可读显示。
 * @param text 原文本。@return 转义文本。
 */
std::string cell(const std::string &text)
{
    std::string out;
    for (const char ch : text) {
        switch (ch) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '|': out += "&#124;"; break;
        case '`': out += "&#96;"; break;
        case '\\': out += "&#92;"; break;
        case '[': out += "&#91;"; break;
        case ']': out += "&#93;"; break;
        case '*': out += "&#42;"; break;
        case '_': out += "&#95;"; break;
        case '\n': out += "<br>"; break;
        case '\r': out += "&#13;"; break;
        default: out += ch; break;
        }
    }
    return out;
}
}

/** @brief 判断 C++ DFA 等价子集。@param pattern 原模式。@return 能安全优化时为真。 */
bool RegexCompatibility::cppDfaCompatible(const std::string &pattern)
{
    bool atom = false;
    for (std::size_t index = 0; index < pattern.size(); ++index) {
        const unsigned char ch = static_cast<unsigned char>(pattern[index]);
        if (ch == 0 || ch >= 128) return false;
        if (ch == '\\') {
            if (++index == pattern.size()) return false;
            if (std::string("nrtfv\\.[](){}*+?^$|-/").find(pattern[index]) == std::string::npos) return false;
            atom = true;
        } else if (ch == '[') {
            const std::size_t start = ++index;
            if (index == pattern.size() || pattern[index] == '^') return false;
            for (; index < pattern.size() && pattern[index] != ']'; ++index) {
                const unsigned char member = static_cast<unsigned char>(pattern[index]);
                if (member == 0 || member >= 128 || member == '[' || member == '\\') return false;
            }
            if (index == pattern.size() || index == start) return false;
            atom = true;
        } else if (ch == '*' || ch == '+' || ch == '?') {
            return atom && index + 1 == pattern.size();
        } else if (std::string(".|(){}^$]").find(static_cast<char>(ch)) != std::string::npos) return false;
        else atom = true;
    }
    return true;
}

/** @brief 按实际生成路径分析逐规则语义。
 * @param lex 词法规则。@param language 目标。@param cppLexer C++ 请求引擎。
 * @return 执行引擎、兼容程度与预算能力。
 */
RegexCompatibilityReport RegexCompatibility::analyze(const LexSpec &lex,
    const std::string &language, const std::string &cppLexer)
{
    RegexCompatibilityReport report;
    report.language = language;
    report.cppLexer = language == "cpp17" ? cppLexer : "";
    for (const auto &rule : lex.regex) {
        RegexRuleCompatibility info;
        info.name = rule.first; info.category = "token"; info.pattern = rule.second;
        report.rules.push_back(std::move(info));
    }
    if (lex.identifier) {
        RegexRuleCompatibility info;
        info.name = "VARIABLE"; info.category = "identifier"; info.pattern = *lex.identifier;
        report.rules.push_back(std::move(info));
    }
    for (const auto &rule : lex.commits) {
        RegexRuleCompatibility info;
        info.name = rule.type; info.category = rule.isSkip ? "trivia" : "commit"; info.pattern = rule.pattern;
        report.rules.push_back(std::move(info));
    }
    std::vector<std::shared_ptr<RegexNode>> asts(report.rules.size());
    std::vector<Error> errors(report.rules.size());
    std::vector<Traits> traits(report.rules.size());
    std::string cVmTrigger, cppFallback;
    bool allUnicode = !report.rules.empty();
    for (std::size_t index = 0; index < report.rules.size(); ++index) {
        const auto &rule = report.rules[index];
        RegexDfa::parse(rule.pattern, asts[index], errors[index]);
        visit(asts[index], traits[index]); scan(rule.pattern, traits[index], errors[index].empty());
        allUnicode = allUnicode && traits[index].unicode;
        const bool nonAscii = std::any_of(rule.pattern.begin(), rule.pattern.end(),
            [](unsigned char ch) { return ch >= 128; });
        if (cVmTrigger.empty() && asts[index] && (RegexVm::required(asts[index]) || nonAscii)) cVmTrigger = rule.name;
        if (cppFallback.empty() && !cppDfaCompatible(rule.pattern)) cppFallback = rule.name;
    }
    report.configurableBudgets = (language == "c" && !report.rules.empty()) ||
        (language == "cpp17" && (cppLexer == "vm" || allUnicode));
    for (std::size_t index = 0; index < report.rules.size(); ++index) {
        auto &rule = report.rules[index];
        const auto &ability = traits[index];
        rule.features.assign(ability.names.begin(), ability.names.end());
        if (!errors[index].empty()) rule.features.push_back("生成器未识别全部语法");
        if (language == "gram") {
            rule.engine = "none"; rule.characterMode = "原文保留";
            rule.compatibility = RegexCompatibilityKind::NotApplicable;
            rule.reason = "文法导出不执行正则";
            continue;
        }
        const bool sharedVm = language == "c" ? !cVmTrigger.empty() :
            (language == "cpp17" && (cppLexer == "vm" || ability.unicode));
        const bool sharedDfa = language == "c" ? !sharedVm :
            (language == "cpp17" && cppLexer == "dfa" && cppFallback.empty());
        rule.engine = sharedVm ? "ordered-vm" : sharedDfa ? (language == "c" ? "c-dfa" : "cpp-dfa") : nativeEngine(language);
        rule.defaultBudget = sharedVm || (language == "c" && sharedDfa);
        rule.characterMode = ability.unicode && sharedVm ? "Unicode 17.0 / UTF-8" :
            ((language == "c" || language == "cpp17") ? "字节 / ASCII 字符类" : "原生字符域 / 目标库版本");
        if (sharedVm || sharedDfa) {
            rule.compatibility = errors[index].empty() ? RegexCompatibilityKind::Shared : RegexCompatibilityKind::Unsupported;
            if (!errors[index].empty()) rule.reason = errors[index].message;
            else if (sharedVm && ability.unicode) rule.reason = "显式 Unicode 模式使用固定的 Unicode 17.0 共享实现";
            else if (sharedVm && language == "c") rule.reason = "规则集合中的 " + cVmTrigger + " 需要有序/字节程序，整份 C 词法器使用共享运行时";
            else if (sharedVm) rule.reason = "显式选择 vm，全部规则使用可配置预算的有序运行时";
            else rule.reason = "使用生成期 DFA；该路径保留共享规则的前缀匹配语义";
            continue;
        }
        rule.compatibility = errors[index].empty() && portableAscii(rule.pattern)
            ? RegexCompatibilityKind::Shared : RegexCompatibilityKind::Native;
        if (language == "cpp17" && cppLexer == "dfa") rule.reason = "整个规则集回退到标准库：" + cppFallback + " 不属于等价 ASCII 快速子集";
        else rule.reason = "由目标原生正则库执行";
        if (!errors[index].empty()) rule.reason += "；生成器未能完整校验：" + errors[index].message;
        else if (rule.compatibility == RegexCompatibilityKind::Native) rule.reason += "；未证明与共享字节/Unicode 17.0 规范完全等价";
        else rule.reason += "；处于已确认的保守 ASCII 等价子集";
        std::string unsupported;
        if ((language == "rust" || language == "go") && (ability.lookAhead || ability.lookBehind || ability.backref))
            unsupported = "当前 " + nativeEngine(language) + " 后端不支持前瞻、后顾或反向引用";
        else if (language == "cpp17" && ability.lookBehind) unsupported = "std::regex 的 ECMAScript 路径不支持后顾断言";
        else if ((language == "js" && (ability.ignoreCase || ability.unicode)) ||
                 ((language == "csharp" || language == "go") && ability.unicode))
            unsupported = "当前后端原样传递正则，未转换该全局 (?i)/(?u) 前缀";
        else if ((language == "python" || language == "js") && ability.properties)
            unsupported = "当前后端的原生正则配置不支持共享 Unicode 属性转义";
        if (!unsupported.empty()) {
            rule.compatibility = RegexCompatibilityKind::Unsupported;
            rule.reason = unsupported;
        }
    }
    return report;
}

/** @brief 校验已知能力与可选严格语义要求。
 * @param report 分析结果。@param strict 严格模式。@param error 第一条失败原因。
 * @return 所有规则通过时为真。
 */
bool RegexCompatibility::validate(const RegexCompatibilityReport &report, bool strict, Error &error)
{
    for (const auto &rule : report.rules) {
        if (rule.compatibility == RegexCompatibilityKind::Unsupported)
            return fail(error, "正则规则 " + rule.category + "/" + rule.name + " 不受目标 " + report.language + " 支持：" + rule.reason);
        if (strict && rule.compatibility == RegexCompatibilityKind::Native)
            return fail(error, "严格正则校验未通过：" + rule.category + "/" + rule.name + " 的原生语义尚未证明与共享规范等价；" + rule.reason);
    }
    return true;
}

/** @brief 渲染报告。@param report 分析结果。@return 已转义的 Markdown 表格。 */
std::string RegexCompatibility::markdown(const RegexCompatibilityReport &report)
{
    std::ostringstream out;
    out << "\n## 正则兼容性与执行引擎\n\n目标：<code>" << cell(report.language) << "</code>";
    if (!report.cppLexer.empty()) out << "；请求引擎：<code>" << cell(report.cppLexer) << "</code>";
    out << "；全部显式规则可配置共享预算：" << (report.configurableBudgets ? "是" : "否") << "。\n\n";
    out << "仅分析显式 token/identifier/commit/trivia 正则，不替代目标编译器或原生库的语法检查。\n\n";
    if (report.rules.empty()) { out << "未声明显式正则规则。\n"; return out.str(); }
    out << "| 规则 | 正则 | 字符模式 | 实际引擎 | 使用能力 | 兼容性 | 默认预算 | 原因 |\n"
           "| --- | --- | --- | --- | --- | --- | --- | --- |\n";
    for (const auto &rule : report.rules) {
        const char *compatibility = rule.compatibility == RegexCompatibilityKind::Shared ? "已确认/共享" :
            rule.compatibility == RegexCompatibilityKind::Unsupported ? "不支持" :
            rule.compatibility == RegexCompatibilityKind::NotApplicable ? "不执行" : "原生，未完全确认";
        std::string features;
        for (const auto &feature : rule.features) { if (!features.empty()) features += "、"; features += feature; }
        out << "| " << cell(rule.category + "/" + rule.name) << " | <code>" << cell(rule.pattern)
            << "</code> | " << cell(rule.characterMode) << " | <code>" << cell(rule.engine) << "</code> | "
            << cell(features.empty() ? "普通匹配" : features) << " | " << compatibility << " | "
            << (rule.defaultBudget ? "有" : "无") << " | " << cell(rule.reason) << " |\n";
    }
    out << "\n“原生，未完全确认”不表示语法错误；--strict-regex（或 --strict）会保守拒绝这类规则。\n";
    return out.str();
}
}
