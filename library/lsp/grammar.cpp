#include "grammar.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <map>
#include <set>
#include <sstream>

namespace lsp {
namespace {

bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
bool identStart(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_'; }
bool identPart(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.' || c == '$';
}
std::string trim(const std::string& text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}
std::size_t utf16Units(unsigned char first) {
    if (first < 0x80) return 1;
    if ((first & 0xE0) == 0xC0) return 1;
    if ((first & 0xF0) == 0xE0) return 1;
    return 2; // 非 BMP：UTF-16 里是代理对
}
std::size_t utf8Length(unsigned char first) {
    if (first < 0x80) return 1;
    if ((first & 0xE0) == 0xC0) return 2;
    if ((first & 0xF0) == 0xE0) return 3;
    if ((first & 0xF8) == 0xF0) return 4;
    return 1;
}

enum class Raw { Directive, Ident, String, Number, Comment, Code, Assign, Dollar, Member, Punct };

struct RawToken {
    Raw kind = Raw::Punct;
    std::string text;
    Range range;
    bool firstOnLine = false;
};

class Cursor {
public:
    explicit Cursor(const std::string& text) : text_(text) {}
    bool eof() const { return index_ >= text_.size(); }
    char peek(std::size_t ahead = 0) const {
        return index_ + ahead < text_.size() ? text_[index_ + ahead] : '\0';
    }
    char get() {
        if (eof()) return '\0';
        const unsigned char c = static_cast<unsigned char>(text_[index_]);
        if (c == '\n') {
            ++index_;
            ++line_;
            column_ = 0;
            return '\n';
        }
        index_ += utf8Length(c);
        column_ += utf16Units(c);
        return static_cast<char>(c);
    }
    void advance(std::size_t count) {
        for (std::size_t i = 0; i < count && !eof(); ++i) get();
    }
    /** 前进到指定字节偏移（偏移必须 >= 当前偏移）。 */
    void advanceTo(std::size_t offset) {
        while (index_ < offset && !eof()) get();
    }
    Position position() const { return Position{line_, column_}; }
    std::size_t offset() const { return index_; }

private:
    const std::string& text_;
    std::size_t index_ = 0;
    std::size_t line_ = 0;
    std::size_t column_ = 0;
};

struct ScanResult {
    std::vector<RawToken> tokens;
    std::vector<Diagnostic> diagnostics;
};

/** 从 `from` 开始找下一行行首：该行去掉空白后以 marker 开头时返回它的偏移。 */
std::size_t findMarkerLine(const std::string& text, std::size_t from,
                           const std::string& marker) {
    std::size_t offset = from;
    while (offset < text.size()) {
        std::size_t lineEnd = text.find('\n', offset);
        if (lineEnd == std::string::npos) lineEnd = text.size();
        const std::string line = trim(text.substr(offset, lineEnd - offset));
        if (line.rfind(marker, 0) == 0) return offset;
        offset = lineEnd + 1;
    }
    return std::string::npos;
}

/**
 * @brief 段标记后面的尾巴是不是 `include "路径"`。
 *
 * 与生成器同一套写法：`include` 与路径之间可以空格、也可以写 `=`，
 * 路径用单引号或双引号都行（`%code include "user.h"` / `%epilogue include="epi.cpp"`）。
 * 命中时这一段的正文来自外部文件，**不需要** `%endcode` 之类的结束标记。
 */
bool isIncludeTail(const std::string& tail) {
    static const std::string keyword = "include";
    if (tail.rfind(keyword, 0) != 0) return false;
    std::size_t index = keyword.size();
    while (index < tail.size() && tail[index] == ' ') ++index;
    if (index < tail.size() && tail[index] == '=') {
        ++index;
        while (index < tail.size() && tail[index] == ' ') ++index;
    }
    return index < tail.size() && (tail[index] == '"' || tail[index] == '\'');
}

/** 容错扫描：注释、字符串、代码块与 `%指令` 都产出带位置的 token。 */
ScanResult scanText(const std::string& text) {
    ScanResult result;
    Cursor cursor(text);
    std::size_t lastTokenLine = static_cast<std::size_t>(-1);
    std::string lastDirective;

    auto emit = [&](Raw kind, const Range& range, const std::string& content) {
        RawToken token;
        token.kind = kind;
        token.text = content;
        token.range = range;
        token.firstOnLine = range.start.line != lastTokenLine;
        if (kind != Raw::Comment) lastTokenLine = range.start.line;
        result.tokens.push_back(token);
    };
    auto error = [&](const Range& range, const std::string& code, const std::string& message) {
        Diagnostic diagnostic;
        diagnostic.range = range;
        diagnostic.severity = Severity::Error;
        diagnostic.code = code;
        diagnostic.message = message;
        result.diagnostics.push_back(diagnostic);
    };

    while (!cursor.eof()) {
        const char c = cursor.peek();
        if (isSpace(c)) { cursor.get(); continue; }
        const Position start = cursor.position();

        if (c == '/' && cursor.peek(1) == '*') {
            cursor.advance(2);
            while (!cursor.eof() && !(cursor.peek() == '*' && cursor.peek(1) == '/')) cursor.get();
            const bool closed = !cursor.eof();
            if (closed) cursor.advance(2);
            emit(Raw::Comment, Range{start, cursor.position()}, "");
            if (!closed) error(Range{start, cursor.position()}, "unterminated-comment", "块注释没有闭合（缺少 */）");
            continue;
        }
        if (c == '/' && cursor.peek(1) == '/') {
            while (!cursor.eof() && cursor.peek() != '\n') cursor.get();
            emit(Raw::Comment, Range{start, cursor.position()}, "");
            continue;
        }
        if (c == '"') {
            // 字符串正文按**原始字节**收：`Cursor::get()` 为了算行列只返回多字节
            // 字符的首字节，直接 push_back 会把中文截断（`"数字"` 只剩两个字节，
            // 悬停里的 alias / 字面量就成了乱码）。这里改成按偏移切片，整段收下。
            std::string value;
            cursor.get();
            bool closed = false;
            while (!cursor.eof()) {
                const char ch = cursor.peek();
                if (ch == '\\' && cursor.peek(1) != '\0') {
                    cursor.get();                       // 反斜杠本身不要
                    const std::size_t from = cursor.offset();
                    cursor.get();                       // 被转义的一个字符（可能多字节）
                    value.append(text, from, cursor.offset() - from);
                    continue;
                }
                if (ch == '"') { cursor.get(); closed = true; break; }
                const std::size_t from = cursor.offset();
                cursor.get();
                value.append(text, from, cursor.offset() - from);
            }
            emit(Raw::String, Range{start, cursor.position()}, value);
            if (!closed) error(Range{start, cursor.position()}, "unterminated-string", "字符串没有闭合（缺少 \"）");
            continue;
        }
        if (c == '%' && cursor.peek(1) == '{' && cursor.peek(2) == '{') {
            cursor.advance(3);
            // `%{{ ... }}%`：结束标记是 `}}%`，可以出现在行内。
            const std::size_t marker = text.find("}}%", cursor.offset());
            const bool closed = marker != std::string::npos;
            cursor.advanceTo(closed ? marker : text.size());
            if (closed) cursor.advance(3);
            emit(Raw::Code, Range{start, cursor.position()}, "%{{");
            error(Range{start, cursor.position()}, "disabled-user-code",
                  "%{{ ... }}% 语法已禁用；请改用 %code ... %endcode");
            continue;
        }
        if (c == '%') {
            if (!identStart(cursor.peek(1))) {
                cursor.get();
                emit(Raw::Punct, Range{start, cursor.position()}, "%");
                continue;
            }
            cursor.get();
            std::string name;
            while (!cursor.eof() && identPart(cursor.peek())) name.push_back(cursor.get());
            const std::string directive = "%" + name;
            emit(Raw::Directive, Range{start, cursor.position()}, directive);
            lastDirective = directive;
            /* 段标记可以带 `include "路径"`：正文在外部文件里，本行收成一个 Code
             * token 就结束（不找结束标记，也不把 include 那截当文法文本扫）。 */
            {
                const std::size_t lineEnd = text.find('\n', cursor.offset());
                const std::size_t stop = (lineEnd == std::string::npos) ? text.size() : lineEnd;
                const std::string tail = trim(text.substr(cursor.offset(), stop - cursor.offset()));
                if ((directive == "%code" || directive == "%class_body" ||
                     directive == "%main" || directive == "%epilogue") &&
                    isIncludeTail(tail)) {
                    cursor.advanceTo(stop);
                    emit(Raw::Code, Range{start, cursor.position()}, directive);
                    continue;
                }
            }
            // 段标记：正文原样收集成一个 Code token（不解析里面的内容）。
            if (directive == "%code" || directive == "%class_body") {
                const std::string terminator = directive == "%code" ? "%endcode" : "%endbody";
                const std::size_t bodyStart = cursor.offset();
                std::size_t marker = findMarkerLine(text, bodyStart, terminator);
                if (marker == std::string::npos) {
                    cursor.advanceTo(text.size());
                    emit(Raw::Code, Range{start, cursor.position()}, directive);
                    error(Range{start, cursor.position()}, "unterminated-section",
                          directive + " 段缺少结束标记 " + terminator);
                    continue;
                }
                cursor.advanceTo(marker);
                emit(Raw::Code, Range{start, cursor.position()}, directive);
                continue;
            }
            if (directive == "%main" || directive == "%epilogue") {
                const std::size_t bodyStart = cursor.offset();
                std::size_t marker = std::string::npos;
                for (const char* candidate : {"%main", "%epilogue", "%code", "%class_body"}) {
                    const std::size_t found = findMarkerLine(text, bodyStart, candidate);
                    if (found != std::string::npos && (marker == std::string::npos || found < marker)) {
                        marker = found;
                    }
                }
                if (marker == std::string::npos) marker = text.size();
                cursor.advanceTo(marker);
                emit(Raw::Code, Range{start, cursor.position()}, directive);
                continue;
            }
            continue;
        }
        if (c == '{') {
            int depth = 0;
            do {
                const char ch = cursor.get();
                if (ch == '{') ++depth;
                else if (ch == '}') --depth;
                else if (ch == '"' || ch == '\'') {
                    const char quote = ch;
                    while (!cursor.eof() && cursor.peek() != quote && cursor.peek() != '\n') {
                        if (cursor.peek() == '\\') cursor.get();
                        cursor.get();
                    }
                    if (cursor.peek() == quote) cursor.get();
                }
            } while (!cursor.eof() && depth > 0);
            emit(Raw::Code, Range{start, cursor.position()}, "{");
            if (depth > 0) error(Range{start, cursor.position()}, "unterminated-action", "语义动作的 { 没有配对（缺少 }）");
            continue;
        }
        if (c == '<' && (lastDirective == "%token" || lastDirective == "%type" ||
                         lastDirective == "%destructor")) {
            cursor.get();
            std::string member;
            while (!cursor.eof() && cursor.peek() != '>' && cursor.peek() != '\n') member.push_back(cursor.get());
            if (cursor.peek() == '>') cursor.get();
            emit(Raw::Member, Range{start, cursor.position()}, member);
            continue;
        }
        if (c == ':' && cursor.peek(1) == '=') {
            cursor.advance(2);
            emit(Raw::Assign, Range{start, cursor.position()}, ":=");
            continue;
        }
        if (c == '$') {
            cursor.get();
            std::string name = "$";
            while (!cursor.eof() && identPart(cursor.peek())) name.push_back(cursor.get());
            emit(Raw::Dollar, Range{start, cursor.position()}, name);
            continue;
        }
        if (identStart(c)) {
            std::string name;
            while (!cursor.eof() && identPart(cursor.peek())) name.push_back(cursor.get());
            emit(Raw::Ident, Range{start, cursor.position()}, name);
            continue;
        }
        if (std::isdigit(static_cast<unsigned char>(c))) {
            std::string number;
            while (!cursor.eof() && std::isxdigit(static_cast<unsigned char>(cursor.peek()))) {
                number.push_back(cursor.get());
            }
            emit(Raw::Number, Range{start, cursor.position()}, number);
            continue;
        }
        cursor.get();
        emit(Raw::Punct, Range{start, cursor.position()}, std::string(1, c));
    }
    return result;
}

const std::vector<std::pair<std::string, std::string>>& directiveDocs() {
    static const std::vector<std::pair<std::string, std::string>> table = {
        {"%token", "声明 Token：`%token NAME`、`%token NAME \"字面量\"`、`%token regex NAME \"正则\"`；可写 `%token <成员> NAME`；`%token NAME include \"表.csv\"` 从 CSV 追加字面量别名"},
        {"%define", "固定字符串 Token（等价 `%token NAME \"字面量\"`），多个候选用 | 分隔"},
        {"%regex", "用正则定义 Token：`%regex NAME \"正则\"`"},
        {"%space", "取 Token 前跳过的空白字符集，缺省 \"\\b\"（全部空白）"},
        {"%start", "起始非终结符，缺省取第一条规则的左部"},
        {"%example", "生成文档里的示例输入：`%example \"符合本文法的文本\"`；支持字符串转义，最多一次"},
        {"%mode", "生成模式：ll1 / lr0 / slr / lalr / lr1 / ielr / lrk / lalrk / slrk / llk / ll0 / glr / earley / cyk / dd / lex，缺省 ielr"},
        {"%lookahead", "展望符长度 k（lrk / lalrk / slrk / llk / ll0 有效）"},
        {"%teach", "打开教学模式（等价命令行 --teach）"},
        {"%mermaid", "生成 Mermaid 状态机图（等价命令行 --mermaid）"},
        {"%glr_max_results", "GLR 一次解析最多缓存几个结果（1..4096，缺省 16；只对 %mode glr 有意义）"},
        {"%lang", "产物语言：c / cpp17 / python / csharp / rust / go / gram，缺省 cpp17"},
        {"%class", "Python 产物的解析器类名（只对 --lang python 有效）"},
        {"%package", "C# namespace / Rust crate 名 / Go module path（只对 --lang csharp / rust / go 有效）"},
        {"%import", "C# using / Rust use / Go import（只对 --lang csharp / rust / go 有效，可重复）"},
        {"%class_body", "段标记：正文原样插进解析器类内部（用 %endbody 收尾）"},
        {"%endbody", "结束 %class_body 段（单独成行，正文不解析）"},
        {"%namespace", "C++17 产物的命名空间名与文件名前缀"},
        {"%prefix", "C99 产物的公开符号前缀（等价命令行 --prefix；不能以 Parser / PARSER 开头）"},
        {"%value_type", "语义值类型（生成器补 typedef … ParserValue_T;）"},
        {"%code", "用户代码段，用 %endcode 收尾"},
        {"%endcode", "结束 %code 段（单独成行，正文不解析）"},
        {"%union", "语义值联合体，配合 %type / %token <成员> 使用"},
        {"%type", "给非终结符绑定 %union 成员：`%type <成员> a b`"},
        {"%destructor", "给 %union 成员登记清理代码：`%destructor <成员> { … }`"},
        {"%max_stack_depth", "分析栈深度上限（1..65535，缺省 64）"},
        {"%max_errors", "一次解析最多报几条错（1..4096，缺省 8）"},
        {"%error_recovery_tokens", "错误恢复后静默几个 Token（0..4096，缺省 3）"},
        {"%sync", "错误恢复的同步锚点 Token（必须是已声明的 Token）"},
        {"%error_context", "错误里多一段「出错行原文 + ^」"},
        {"%empty", "空产生式（等价右部什么都不写）"},
        {"%if", "条件编译：%if / %elif / %else / %endif，按 --define 决定"},
        {"%elif", "条件编译分支（配合 %if）"},
        {"%else", "条件编译分支（配合 %if）"},
        {"%endif", "结束条件编译"},
        {"%epilogue", "段标记：正文贴到产物末尾"},
        {"%main", "段标记：正文替换产物的 demo main"},
        {"%tree", "生成通用语法树 API（%mode lex 下忽略）"},
        {"%left", "声明 Token 左结合与优先级（行内自左向右递增）"},
        {"%right", "声明 Token 右结合与优先级"},
        {"%nonassoc", "声明 Token 不可结合"},
        {"%prec", "写在产生式右部末尾，覆盖该产生式的优先级来源"},
        {"%expect", "允许保留 N 个未消解的移入/归约冲突"},
        {"%identifier", "声明标识符正则，生成标识符识别与关键字查表接口"},
        {"%commit", "把「识别但不进语法层」的文本收进容器：%commit [类型] \"模式\" / %commit skip \"模式\""},
        {"%trivia", "匹配即丢（等价 %commit skip）"},
        {"%copy", "把文法目录下的文件/目录拷进产物：%copy [file|dir] 路径"},
        {"%build_files", "是否导出工程构建文件（Makefile / CMakeLists.txt）：%build_files on / off，只能声明一次"},
        {"%cpp_lexer", "C++17 产物的词法引擎：%cpp_lexer dfa / std，只能声明一次（命令行 --cpp-lexer 优先）。dfa（缺省）生成期把正则编译成 DFA 表，产物不依赖 <regex>；std 交给 std::regex"},
        {"%include", "文件级包含：把被引 .gram 的内容内联进来，`%include \"路径\"`（也接受 `%include='路径'` 与单引号，必须单独成行）。相对路径按包含它的文件所在目录解析，支持递归与循环检测；%code / %class_body / %epilogue / %main 段内不展开。内联后报错行号指向拼接后的文本"},
        {"%cpp_only", "声明这份文法只能生成 C++17 产物"},
    };
    return table;
}

std::string directiveDetail(const std::string& name) {
    for (const auto& entry : directiveDocs()) {
        if (entry.first == name) return entry.second;
    }
    return {};
}

/**
 * 列出一个目录下可被 `%include` 引用的 `.gram` 文件名（只取文件名，不带目录）。
 *
 * 目录读不了就返回空列表——补全拿不到候选是常态，不该报错。
 *
 * @param dir UTF-8 本地目录；空串直接返回空列表
 * @return 文件名列表，按字典序排好
 */
std::vector<std::string> grammarCandidates(const std::string& dir) {
    std::vector<std::string> names;
    if (dir.empty()) return names;
    std::error_code ec;
    std::filesystem::directory_iterator it(std::filesystem::u8path(dir), ec);
    if (ec) return names;
    for (const auto& entry : it) {
        std::error_code entryEc;
        if (!entry.is_regular_file(entryEc) || entryEc) continue;
        const std::string name = entry.path().filename().u8string();
        if (name.size() > 5 && name.compare(name.size() - 5, 5, ".gram") == 0) {
            names.push_back(name);
        }
    }
    std::sort(names.begin(), names.end());
    return names;
}

} // namespace

const std::vector<std::string>& GrammarDocument::modeNames() {
    static const std::vector<std::string> names = {
        "ll1", "lr0", "slr", "lalr", "lr1", "ielr", "lrk", "lalrk", "slrk", "llk", "ll0", "glr", "earley", "cyk", "dd", "lex"};
    return names;
}
const std::vector<std::string>& GrammarDocument::languageNames() {
    static const std::vector<std::string> names = {
        "c", "cpp17", "python", "csharp", "rust", "go", "gram"};
    return names;
}
const std::vector<std::pair<std::string, std::string>>& GrammarDocument::directiveTable() {
    return directiveDocs();
}

void GrammarDocument::setText(std::string text) {
    text_ = std::move(text);
    diagnostics_.clear();
    directives_.clear();
    tokens_.clear();
    rules_.clear();
    references_.clear();
    startName_.clear();
    syncs_.clear();
    inlineKeywords_.clear();
    insertPosition_ = Position{0, 0};
    buildModel();
}

void GrammarDocument::addDiagnostic(const Range& range, Severity severity, const std::string& code,
                                    const std::string& message) {
    Diagnostic diagnostic;
    diagnostic.range = range;
    diagnostic.severity = severity;
    diagnostic.code = code;
    diagnostic.message = message;
    diagnostics_.push_back(diagnostic);
}

const TokenDecl* GrammarDocument::findToken(const std::string& name) const {
    for (const auto& token : tokens_) if (token.name == name) return &token;
    return nullptr;
}
const RuleDecl* GrammarDocument::findRule(const std::string& name) const {
    for (const auto& rule : rules_) if (rule.name == name) return &rule;
    return nullptr;
}
bool GrammarDocument::isInlineKeyword(const Range& range) const {
    for (const auto& keyword : inlineKeywords_) {
        if (keyword.start.line == range.start.line && keyword.start.column == range.start.column) {
            return true;
        }
    }
    return false;
}

void GrammarDocument::scan() {}

void GrammarDocument::buildModel() {
    const ScanResult scanned = scanText(text_);
    for (const auto& diagnostic : scanned.diagnostics) diagnostics_.push_back(diagnostic);
    const auto& tokens = scanned.tokens;

    std::set<std::string> declared;
    auto addToken = [&](const std::string& name, const std::string& directive, const Range& range,
                        const std::string& pattern, bool literal, const std::string& member) {
        if (name.empty() || declared.count(name)) return;
        declared.insert(name);
        TokenDecl decl;
        decl.name = name;
        decl.directive = directive;
        decl.range = range;
        decl.pattern = pattern;
        decl.literal = literal;
        decl.member = member;
        tokens_.push_back(decl);
    };

    std::size_t firstRuleLine = static_cast<std::size_t>(-1);
    std::string modeName;
    for (std::size_t i = 0; i < tokens.size(); ++i) {
        const RawToken& token = tokens[i];
        if (token.kind == Raw::Directive && token.firstOnLine) {
            DirectiveDecl decl;
            decl.name = token.text;
            decl.range = token.range;
            std::string argument;
            for (std::size_t j = i + 1; j < tokens.size() &&
                 tokens[j].range.start.line == token.range.start.line; ++j) {
                if (tokens[j].kind == Raw::Comment) break;
                if (!argument.empty()) argument += " ";
                argument += tokens[j].kind == Raw::String ? "\"" + tokens[j].text + "\"" : tokens[j].text;
            }
            decl.argument = argument;
            directives_.push_back(decl);
            if (directiveDetail(token.text).empty()) {
                addDiagnostic(token.range, Severity::Error, "unknown-directive",
                              "不认识的指令 " + token.text + "（完整指令表见 doc/12-grammar-usage.md）");
            }
            const std::string& name = token.text;
            const bool collects = name == "%token" || name == "%define" || name == "%regex" ||
                                  name == "%left" || name == "%right" || name == "%nonassoc";
            if (collects) {
                std::string member;
                // 最近一次 addToken 成功的那一项，供 `alias "显示名"` 回填。
                std::size_t lastToken = static_cast<std::size_t>(-1);
                for (std::size_t k = i + 1; k < tokens.size() &&
                     tokens[k].range.start.line == token.range.start.line; ++k) {
                    if (tokens[k].kind == Raw::Comment) break;
                    if (tokens[k].kind == Raw::Member) { member = tokens[k].text; continue; }
                    if (tokens[k].kind != Raw::Ident) continue;
                    // `regex` 与 `include` 是 `%token` 的行内关键字，后一个词不是 Token 名：
                    //   %token regex   NAME "正则"      —— regex 后面是正则
                    //   %token NAME    include "表.csv" —— include 后面是 CSV 别名表路径
                    // 它们后面紧跟着的那段字符串是**关键字自己的参数**，绝不能当成本行
                    // 声明出来的固定字面量（`%token A include "1.csv"` 里的 `1.csv`
                    // 不是 A 的字面量，是 CSV 文件名）。
                    if (name == "%token" &&
                        (tokens[k].text == "regex" || tokens[k].text == "include")) {
                        inlineKeywords_.push_back(tokens[k].range);
                        if (tokens[k].text == "include" && k + 1 < tokens.size() &&
                            tokens[k + 1].kind == Raw::String &&
                            tokens[k + 1].range.start.line == token.range.start.line) {
                            // CSV 路径记到紧邻的那个 Token 上（`%token A B include "t.csv"`
                            // 绑的是 B，与生成器的解析一致），悬停与大纲要显示它。
                            if (lastToken != static_cast<std::size_t>(-1)) {
                                tokens_[lastToken].csvPath = tokens[k + 1].text;
                            }
                            ++k;
                        }
                        continue;
                    }
                    // `alias "显示名"` 同理：alias 不是 Token 名，它的参数作用在
                    // 前一个声明上（`%token A "x" alias "A"`、`%token A include "t.csv" alias "A"`）。
                    if (tokens[k].text == "alias") {
                        inlineKeywords_.push_back(tokens[k].range);
                        if (k + 1 < tokens.size() && tokens[k + 1].kind == Raw::String &&
                            tokens[k + 1].range.start.line == token.range.start.line) {
                            if (lastToken != static_cast<std::size_t>(-1)) {
                                tokens_[lastToken].alias = tokens[k + 1].text;
                            }
                            ++k;
                        }
                        continue;
                    }
                    std::string pattern;
                    bool literal = false;
                    if (k + 1 < tokens.size() && tokens[k + 1].kind == Raw::String &&
                        tokens[k + 1].range.start.line == token.range.start.line) {
                        pattern = tokens[k + 1].text;
                        literal = true;
                    }
                    const std::size_t before = tokens_.size();
                    addToken(tokens[k].text, name, tokens[k].range, pattern, literal, member);
                    if (tokens_.size() > before) lastToken = tokens_.size() - 1;
                }
            }
            if (name == "%mode" || name == "%lang") {
                for (std::size_t k = i + 1; k < tokens.size() &&
                     tokens[k].range.start.line == token.range.start.line; ++k) {
                    if (tokens[k].kind != Raw::Ident) continue;
                    const auto& valid = name == "%mode" ? modeNames() : languageNames();
                    if (std::find(valid.begin(), valid.end(), tokens[k].text) == valid.end()) {
                        addDiagnostic(tokens[k].range, Severity::Error, "unknown-value",
                                      (name == "%mode" ? "未知的生成模式 " : "未知的产物语言 ") + tokens[k].text);
                    } else if (name == "%mode") {
                        modeName = tokens[k].text;
                    }
                    break;
                }
            }
            if (name == "%start") {
                for (std::size_t k = i + 1; k < tokens.size() &&
                     tokens[k].range.start.line == token.range.start.line; ++k) {
                    if (tokens[k].kind == Raw::Ident) {
                        startName_ = tokens[k].text;
                        startRange_ = tokens[k].range;
                        break;
                    }
                }
            }
            if (name == "%sync") {
                for (std::size_t k = i + 1; k < tokens.size() &&
                     tokens[k].range.start.line == token.range.start.line; ++k) {
                    if (tokens[k].kind == Raw::Ident) syncs_.emplace_back(tokens[k].text, tokens[k].range);
                }
            }
            continue;
        }
        // 规则：`Ident :=`
        if (token.kind == Raw::Ident && i + 1 < tokens.size() && tokens[i + 1].kind == Raw::Assign) {
            RuleDecl rule;
            rule.name = token.text;
            rule.range = token.range;
            rule.lines.push_back(token.range.start.line);
            if (firstRuleLine == static_cast<std::size_t>(-1)) firstRuleLine = token.range.start.line;
            int alternatives = 1;
            std::size_t k = i + 2;
            while (k < tokens.size()) {
                const RawToken& body = tokens[k];
                if (body.kind == Raw::Punct && body.text == ";") break;
                if (body.kind == Raw::Assign) break;
                if (body.kind == Raw::Ident && body.firstOnLine && body.text == rule.name &&
                    k + 1 < tokens.size() && tokens[k + 1].kind == Raw::Assign) {
                    break; // 下一条规则开始
                }
                if (body.kind == Raw::Punct && body.text == "|") { ++alternatives; ++k; continue; }
                if (body.kind == Raw::Comment) { ++k; continue; }
                if (body.kind == Raw::Code) { rule.hasAction = true; ++k; continue; }
                if (body.kind == Raw::Directive) {
                    if (body.text == "%prec" && k + 1 < tokens.size() &&
                        tokens[k + 1].kind == Raw::Ident) {
                        // %prec 的参数是优先级来源 Token：算一次使用，也能跳转定义。
                        rule.references.push_back(tokens[k + 1].range);
                        references_.push_back(
                            {rule.name, tokens[k + 1].text, tokens[k + 1].range, false});
                        k += 2;
                        continue;
                    }
                    ++k;
                    continue;
                }
                if (body.kind == Raw::Ident || body.kind == Raw::String) {
                    rule.references.push_back(body.range);
                    references_.push_back({rule.name, body.text, body.range, body.kind == Raw::String});
                    if (body.range.start.line != rule.lines.back()) rule.lines.push_back(body.range.start.line);
                    ++k;
                    continue;
                }
                ++k;
            }
            rule.alternatives = alternatives;
            // 同名产生式分行写是合法的：合并成一条，候选数与行号累加。
            bool merged = false;
            for (auto& existing : rules_) {
                if (existing.name != rule.name) continue;
                existing.alternatives += rule.alternatives;
                existing.hasAction = existing.hasAction || rule.hasAction;
                existing.lines.insert(existing.lines.end(), rule.lines.begin(), rule.lines.end());
                existing.references.insert(existing.references.end(), rule.references.begin(),
                                           rule.references.end());
                merged = true;
                break;
            }
            if (!merged) {
                rules_.push_back(rule);
            }
            continue;
        }
    }

    // 插入位置：第一条规则之前最后一条指令的下一行行首。
    if (!directives_.empty()) {
        Position insert = Position{directives_.front().range.start.line, 0};
        for (const auto& directive : directives_) {
            if (firstRuleLine != static_cast<std::size_t>(-1) &&
                directive.range.start.line >= firstRuleLine) {
                break;
            }
            insert = Position{directive.range.start.line + 1, 0};
        }
        insertPosition_ = insert;
    }
    rulesStart_ = Position{firstRuleLine == static_cast<std::size_t>(-1) ? 0 : firstRuleLine, 0};

    // 引用检查与占用统计。
    std::set<std::string> ruleNames;
    for (const auto& rule : rules_) ruleNames.insert(rule.name);
    std::set<std::string> usedTokens, usedRules;
    for (const auto& reference : references_) {
        if (reference.literal) continue;
        const std::string& name = reference.name;
        if (name == "error" || name == "%empty") continue;
        if (declared.count(name)) { usedTokens.insert(name); continue; }
        if (ruleNames.count(name)) { usedRules.insert(name); continue; }
        if (std::isupper(static_cast<unsigned char>(name[0]))) {
            // 生成器和 yacc 一样允许「规则里直接用的大写名」当隐式 Token：
            // 不报错，只提示一句，并给"显式声明"的快速修复。
            usedTokens.insert(name);
            // 三个**默认 Token**（生成器内建：STRING / NUMBER / VARIABLE）不需要声明，
            // 也就不提示 —— 文法里直接用它们是正常写法（`NUMBER "[0-9]+"` 那些
            // %token / %identifier 声明可以省掉）。
            if (name == "NUMBER" || name == "VARIABLE" || name == "STRING") continue;
            addDiagnostic(reference.range, Severity::Information, "implicit-token",
                          "Token " + name + " 没有显式声明，按隐式 Token 处理（可用 %token " +
                              name + " 声明）");
            continue;
        } else {
            addDiagnostic(reference.range, Severity::Error, "undefined-symbol",
                          "未声明的非终结符 " + name + "（加一条 " + name + " := …; 规则）");
        }
    }
    if (!startName_.empty() && !ruleNames.count(startName_)) {
        addDiagnostic(startRange_, Severity::Error, "unknown-start",
                      "%start 指向的 " + startName_ + " 不是一条产生式");
    }
    for (const auto& sync : syncs_) {
        if (!declared.count(sync.first)) {
            addDiagnostic(sync.second, Severity::Error, "unknown-sync",
                          "%sync 的 " + sync.first + " 不是已声明的 Token");
        }
    }
    // `%mode lex` 只有词法、没有规则，谈不上「没用到的 Token」。
    if (modeName != "lex") {
        for (const auto& token : tokens_) {
            if (usedTokens.count(token.name) || token.directive == "%sync") continue;
            addDiagnostic(token.range, Severity::Warning, "unused-token",
                          "Token " + token.name + " 声明了但规则里没有用到");
        }
        for (const auto& rule : rules_) {
            if (usedRules.count(rule.name)) continue;
            if (rule.name == startName_) continue;
            if (!rules_.empty() && rule.name == rules_.front().name) continue;
            addDiagnostic(rule.range, Severity::Warning, "unused-rule",
                          "产生式 " + rule.name + " 没有被引用（也不是起始符号）");
        }
    }
}

std::vector<SymbolInfo> GrammarDocument::symbols() const {
    std::vector<SymbolInfo> out;
    for (const auto& directive : directives_) {
        out.push_back({directive.name, "directive", trim(directive.argument), directive.range});
    }
    for (const auto& token : tokens_) {
        std::string detail = token.directive;
        if (!token.pattern.empty()) detail += " \"" + token.pattern + "\"";
        if (!token.csvPath.empty()) detail += " include \"" + token.csvPath + "\"";
        if (!token.alias.empty()) detail += " alias \"" + token.alias + "\"";
        out.push_back({token.name, "token", detail, token.range});
    }
    for (const auto& rule : rules_) {
        out.push_back({rule.name, "rule",
                       std::to_string(rule.alternatives) + " 个候选" +
                           (rule.name == startName_ ? " · 起始符号" : ""),
                       rule.range});
    }
    std::sort(out.begin(), out.end(), [](const SymbolInfo& a, const SymbolInfo& b) {
        return a.range.start < b.range.start;
    });
    return out;
}

std::vector<SemanticToken> GrammarDocument::semanticTokens() const {
    const ScanResult scanned = scanText(text_);
    std::set<std::string> ruleNames;
    for (const auto& rule : rules_) ruleNames.insert(rule.name);
    std::vector<SemanticToken> out;
    for (const auto& token : scanned.tokens) {
        switch (token.kind) {
        case Raw::Directive: out.push_back({token.range, "keyword"}); break;
        case Raw::Comment: out.push_back({token.range, "comment"}); break;
        case Raw::String: out.push_back({token.range, "string"}); break;
        case Raw::Number: out.push_back({token.range, "number"}); break;
        case Raw::Dollar: out.push_back({token.range, "variable"}); break;
        case Raw::Member: out.push_back({token.range, "parameter"}); break;
        case Raw::Assign: out.push_back({token.range, "operator"}); break;
        case Raw::Code:
            // 语义 token 不能跨行：多行区块按行拆开。
            for (std::size_t line = token.range.start.line; line <= token.range.end.line; ++line) {
                const Position start = line == token.range.start.line ? token.range.start : Position{line, 0};
                Position end = line == token.range.end.line ? token.range.end : Position{line + 1, 0};
                if (end.line != line) end = Position{line, 0};
                if (end < start) continue;
                out.push_back({Range{start, end}, "macro"});
            }
            break;
        case Raw::Ident: {
            // `%token` 的行内关键字（include / alias）按关键字着色，别当成 Token 或函数。
            if (isInlineKeyword(token.range)) {
                out.push_back({token.range, "keyword"});
                break;
            }
            const bool isToken = findToken(token.text) != nullptr;
            const bool isRule = ruleNames.count(token.text) != 0;
            out.push_back({token.range, isToken && !isRule ? "type" : "function"});
            break;
        }
        case Raw::Punct: break;
        }
    }
    std::sort(out.begin(), out.end(), [](const SemanticToken& a, const SemanticToken& b) {
        return a.range.start < b.range.start;
    });
    return out;
}

HoverResult GrammarDocument::hover(Position position) const {
    const ScanResult scanned = scanText(text_);
    for (const auto& token : scanned.tokens) {
        if (!contains(token.range, position)) continue;
        std::string markdown;
        if (token.kind == Raw::Directive) {
            markdown = "**" + token.text + "**";
            const std::string detail = directiveDetail(token.text);
            if (!detail.empty()) markdown += "\n\n" + detail;
        } else if (token.kind == Raw::Ident) {
            if (isInlineKeyword(token.range)) {
                if (token.text == "include") {
                    markdown =
                        "**`include`**：从 CSV 追加字面量别名\n\n"
                        "写法 `%token NAME include \"表.csv\"`（可带成员：`%token <ival> NUM include \"表.csv\"`）。"
                        "打开 CSV 当两列别名表，只取**第一列等于 NAME** 的行，把**第二列**"
                        "作为字面量追加到该 Token 的别名列表末尾；等价于逐行写 `%token NAME \"字面量\"`。\n\n"
                        "- **只追加别名**：Token 仍要由普通 `%token` 声明，本指令不新建 Token。\n"
                        "- 第一列不是被导入的 Token 名（含表头行）→ 整行忽略；第三列及以后一律忽略。\n"
                        "- 第二列**按字面量处理，不编译成正则**（要正则请用 `%token regex`）。\n"
                        "- CSV 路径相对**声明它的 `.gram` 文件所在目录**解析。\n"
                        "- 别名必须唯一：不同 Token 用同一个别名、或别名撞上已声明的 Token 名都会报错。";
                } else {
                    markdown =
                        "**`alias`**：给 Token 一个显示名（`%token NAME \"字面量\" alias \"显示名\"`）。"
                        "只改变显示名称，不改变 Token 身份；也能写在 `%token NAME include \"表.csv\"` 后面。";
                }
            } else if (const TokenDecl* decl = findToken(token.text)) {
                markdown = "**Token `" + decl->name + "`**（" + decl->directive + "）";
                if (!decl->pattern.empty()) markdown += "\n\n模式：`" + decl->pattern + "`";
                if (decl->literal) markdown += "\n\n固定字面量";
                if (!decl->csvPath.empty()) {
                    markdown += "\n\n字面量别名来自 CSV `" + decl->csvPath +
                                "`（取第一列等于 `" + decl->name + "` 的那些行的第二列）";
                }
                if (!decl->alias.empty()) markdown += "\n\n显示别名：`" + decl->alias + "`";
                if (!decl->member.empty()) markdown += "\n\n语义值成员：`" + decl->member + "`";
            } else if (const RuleDecl* rule = findRule(token.text)) {
                markdown = "**非终结符 `" + rule->name + "`**：`" + rule->name +
                           " := …`（" + std::to_string(rule->alternatives) + " 个候选）";
                if (rule->name == startName_) markdown += "\n\n起始符号（%start）";
            }
        } else if (token.kind == Raw::Dollar) {
            if (token.text == "$$") markdown = "**`$$`**：当前产生式的语义值（左部）";
            else if (token.text.rfind("$line", 0) == 0) markdown = "**`$line`**：当前行号";
            else if (token.text.rfind("$token", 0) == 0) markdown = "**`$token`**：当前 Token 文本";
            else if (token.text.rfind("$code", 0) == 0) markdown = "**`$code`**：当前 Token 原始文本";
            else markdown = "**`" + token.text + "`**：右部第 " + token.text.substr(1) + " 个符号的语义值";
        } else if (token.kind == Raw::Member) {
            markdown = "**`<" + token.text + ">`**：%union 成员名";
        }
        if (!markdown.empty()) return HoverResult{true, token.range, markdown};
        return HoverResult{false, token.range, {}};
    }
    return HoverResult{};
}

std::vector<CompletionItem> GrammarDocument::completion(Position position) const {
    const ScanResult scanned = scanText(text_);
    std::vector<CompletionItem> items;
    std::string lineDirective;
    bool atLineStart = true;
    bool nameOnLine = false;
    for (const auto& token : scanned.tokens) {
        if (token.range.start.line != position.line) continue;
        if (!(token.range.start < position)) continue;
        atLineStart = false;
        if (token.kind == Raw::Directive) lineDirective = token.text;
        else if (token.kind == Raw::Ident) nameOnLine = true;
        if (token.kind == Raw::Ident && lineDirective == "%mode") lineDirective = "%mode";
    }
    if (lineDirective == "%mode") {
        for (const auto& name : modeNames()) items.push_back({name, "enum", "生成模式", name});
        return items;
    }
    if (lineDirective == "%lang") {
        for (const auto& name : languageNames()) items.push_back({name, "enum", "产物语言", name});
        return items;
    }
    if (lineDirective == "%cpp_lexer") {
        items.push_back({"dfa", "enum", "生成期编译成 DFA 表（缺省，产物不依赖 <regex>）", "dfa"});
        items.push_back({"std", "enum", "交给 std::regex", "std"});
        return items;
    }
    if (lineDirective == "%build_files") {
        items.push_back({"on", "enum", "导出 Makefile / CMakeLists.txt", "on"});
        items.push_back({"off", "enum", "不导出工程构建文件", "off"});
        return items;
    }
    if (lineDirective == "%include") {
        const std::vector<std::string> names = grammarCandidates(baseDir_);
        for (const auto& name : names) {
            items.push_back({name, "file", "同目录下的 .gram 文件", "\"" + name + "\""});
        }
        if (items.empty()) {
            items.push_back({"\"\"", "file", "%include \"路径\"：被引 .gram 的内容内联进来", "\"\""});
        }
        return items;
    }
    if (atLineStart) {
        for (const auto& entry : directiveDocs()) {
            items.push_back({entry.first, "keyword", entry.second, entry.first});
        }
    }
    // `%token NAME ` 之后能接的是两个行内关键字：include（CSV 别名表）/ alias（显示名）。
    // 放在 Token 名候选前面，它们才是这一行的正常续写。
    if (lineDirective == "%token" && nameOnLine) {
        items.push_back({"include", "keyword",
                         "%token NAME include \"表.csv\"：从 CSV 追加字面量别名", "include \"\""});
        items.push_back({"alias", "keyword", "给这个 Token 一个显示名", "alias \"\""});
    }
    for (const auto& token : tokens_) {
        std::string detail = token.directive;
        if (!token.pattern.empty()) detail += " \"" + token.pattern + "\"";
        if (!token.csvPath.empty()) detail += " include \"" + token.csvPath + "\"";
        items.push_back({token.name, "type", detail, token.name});
    }
    for (const auto& rule : rules_) {
        items.push_back({rule.name, "function", "非终结符", rule.name});
    }
    items.push_back({"%empty", "keyword", "空产生式", "%empty"});
    items.push_back({"%prec", "keyword", "覆盖优先级来源", "%prec "});
    return items;
}

std::vector<Range> GrammarDocument::definition(Position position) const {
    const ScanResult scanned = scanText(text_);
    for (const auto& token : scanned.tokens) {
        if (!contains(token.range, position)) continue;
        if (token.kind != Raw::Ident) return {};
        if (const TokenDecl* decl = findToken(token.text)) return {decl->range};
        if (const RuleDecl* rule = findRule(token.text)) return {rule->range};
        return {};
    }
    return {};
}

std::vector<CodeAction> GrammarDocument::codeActions() const {
    std::vector<CodeAction> actions;
    for (const auto& diagnostic : diagnostics_) {
        if (diagnostic.code != "undefined-token" && diagnostic.code != "implicit-token" &&
            diagnostic.code != "undefined-symbol") continue;
        // 从引用的位置反查名字，避免解析诊断文本。
        std::string name;
        for (const auto& reference : references_) {
            if (reference.range.start.line == diagnostic.range.start.line &&
                reference.range.start.column == diagnostic.range.start.column) {
                name = reference.name;
                break;
            }
        }
        if (name.empty()) continue;
        CodeAction action;
        action.kind = "quickfix";
        if (diagnostic.code == "undefined-token" || diagnostic.code == "implicit-token") {
            action.title = "声明 %token " + name;
            action.edit.range = Range{insertPosition_, insertPosition_};
            action.edit.newText = "%token " + name + "\n";
        } else {
            action.title = "添加规则 " + name + " := %empty;";
            Position end = insertPosition_;
            for (const auto& token : scanText(text_).tokens) if (end < token.range.end) end = token.range.end;
            action.edit.range = Range{end, end};
            action.edit.newText = "\n" + name + " := %empty;\n";
        }
        actions.push_back(action);
    }
    return actions;
}

std::string GrammarDocument::format() const {
    std::vector<std::string> lines;
    std::istringstream in(text_);
    for (std::string line; std::getline(in, line);) {
        const auto last = line.find_last_not_of(" \t\r");
        lines.push_back(last == std::string::npos ? std::string() : line.substr(0, last + 1));
    }
    std::vector<std::string> out;
    int blank = 0;
    for (const auto& line : lines) {
        if (line.empty()) { if (++blank > 1) continue; } else { blank = 0; }
        out.push_back(line);
    }
    while (!out.empty() && out.back().empty()) out.pop_back();
    std::string text;
    for (const auto& line : out) text += line + "\n";
    return text;
}

std::string GrammarDocument::checkReport(const std::string& path) const {
    std::ostringstream out;
    for (const auto& diagnostic : diagnostics_) {
        const char* level = diagnostic.severity == Severity::Error     ? "error"
                            : diagnostic.severity == Severity::Warning ? "warning"
                                                                       : "info";
        out << path << ":" << (diagnostic.range.start.line + 1) << ":"
            << (diagnostic.range.start.column + 1) << ": "
            << level << ": "
            << diagnostic.message << " [" << diagnostic.code << "]\n";
    }
    return out.str();
}

} // namespace lsp
