// 单元测试：JSON、LSP 帧、文法分析（诊断 / 悬停 / 补全 / 跳转 / 大纲 /
// 语义高亮 / 格式化 / 快速修复）与一次完整的服务器会话。
#include "json.hpp"
#include "lsp.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace {

int failures = 0;

#define CHECK(condition)                                                          \
    do {                                                                          \
        if (!(condition)) {                                                       \
            std::cerr << "FAIL 第 " << __LINE__ << " 行: " #condition "\n";        \
            ++failures;                                                           \
        }                                                                         \
    } while (0)

const char* kCleanGrammar = R"(/* 计算器（测试用） */
%mode lalr
%value_type double

%define NUMBER "[0-9]+"
%define PLUS "+"
%define STAR "*"
%token SEMI
%left PLUS
%left STAR
%start expr

expr := expr PLUS term { $$ = $1 + $3; }
      | term { $$ = $1; };

term := term STAR NUMBER { $$ = $1 * $3; }
      | NUMBER { $$ = $1; }
      | SEMI { $$ = 0; };
)";

const char* kBrokenGrammar = R"(%mode nope
%frobnicate 1
%sync NOSUCH

expr := expr PLUS term;
expr := term;
term := MISSING;
term := term STAR { $$ = $1;
)";

// 段标记与 C99 前缀：%code / %class_body 的结束标记会被单独扫成一个指令 token，
// 所以指令表里必须有它们，否则编辑器会误报 unknown-directive。
const char* kSectionGrammar = R"(%mode lalr
%lang c
%prefix Demo
%value_type int

%code
static int demo_helper(void) { return 1; }
%endcode

%class_body
static int class_helper(void) { return 2; }
%endbody

%token NUMBER "[0-9]+"
%start expr

expr := NUMBER { $$ = $1; };
)";

void testJson() {
    lsp::Json value = lsp::Json::object();
    value.set("jsonrpc", lsp::Json::string("2.0"));
    value.set("id", lsp::Json::integer(7));
    value.set("ok", lsp::Json::boolean(true));
    lsp::Json items = lsp::Json::array();
    items.push(lsp::Json::number(1.5));
    items.push(lsp::Json::string("中文\n\"引号\" \\ 反斜杠"));
    value.set("items", std::move(items));
    const std::string text = value.dump();
    lsp::Json parsed;
    std::string error;
    CHECK(lsp::Json::parse(text, parsed, &error));
    CHECK(parsed.getString("jsonrpc") == "2.0");
    CHECK(parsed.getInteger("id") == 7);
    CHECK(parsed.get("items").items().size() == 2);
    CHECK(parsed.get("items").items()[1].asString() == "中文\n\"引号\" \\ 反斜杠");
    CHECK(parsed.get("items").items()[0].asNumber() == 1.5);
    CHECK(parsed.dump() == text);

    // 转义与代理对
    lsp::Json unicode;
    CHECK(lsp::Json::parse("{\"a\":\"\\u4e2d\\u6587\\ud83d\\ude00\"}", unicode, &error));
    CHECK(unicode.getString("a") == "中文😀");
    CHECK(!lsp::Json::parse("{\"a\":}", unicode, &error));
    CHECK(!error.empty());
}

void testFraming() {
    const std::string payload = "{\"jsonrpc\":\"2.0\",\"method\":\"initialized\"}";
    const std::string framed = lsp::frameMessage(payload);
    CHECK(framed.rfind("Content-Length: ", 0) == 0);
    std::istringstream in(framed);
    std::string read;
    std::string error;
    CHECK(lsp::readMessage(in, read, error));
    CHECK(read == payload);
    std::istringstream empty("");
    CHECK(!lsp::readMessage(empty, read, error));
}

void testCleanGrammar() {
    lsp::GrammarDocument doc;
    doc.setText(kCleanGrammar);
    int errors = 0;
    for (const auto& diagnostic : doc.diagnostics()) {
        if (diagnostic.severity == lsp::Severity::Error) {
            std::cerr << "  意外诊断: " << diagnostic.message << "\n";
            ++errors;
        }
    }
    CHECK(errors == 0);
    CHECK(doc.tokens().size() == 4);   // NUMBER / PLUS / STAR / SEMI
    CHECK(doc.rules().size() == 2);    // expr / term
    CHECK(doc.rules()[0].alternatives == 2);
    CHECK(doc.rules()[1].alternatives == 3);
    CHECK(doc.symbols().size() > 6);
    CHECK(!doc.semanticTokens().empty());
    CHECK(!doc.format().empty());
    CHECK(doc.format() == doc.format());

    // 段标记带 `include "路径"`：正文在外部文件里，不需要结束标记、也不报错
    lsp::GrammarDocument included;
    included.setText(
        "%mode lalr\n"
        "%code include \"user.h\"\n"
        "%start expr\n"
        "expr := NUMBER { $$ = $1; };\n"
        "%epilogue include=\"epi.cpp\"\n"
        "%main include 'main.cpp'\n");
    int includeErrors = 0;
    for (const auto& diagnostic : included.diagnostics()) {
        if (diagnostic.severity == lsp::Severity::Error) {
            std::cerr << "  include 意外诊断: " << diagnostic.message << "\n";
            ++includeErrors;
        }
    }
    CHECK(includeErrors == 0);
    // NUMBER / VARIABLE / STRING 是生成器内建的默认 Token：直接用它们**不该有提示**
    CHECK(included.diagnostics().empty());
    CHECK(included.rules().size() == 1);   // expr 一条规则照常解析

    // 悬停：%token 指令与 Token 名
    const lsp::HoverResult directiveHover = doc.hover(lsp::Position{1, 2});
    CHECK(directiveHover.found);
    CHECK(directiveHover.markdown.find("%mode") != std::string::npos);
    const lsp::HoverResult tokenHover = doc.hover(lsp::Position{4, 10});
    CHECK(tokenHover.found);
    CHECK(tokenHover.markdown.find("NUMBER") != std::string::npos);

    // 补全：行首给指令，规则区给 Token / 非终结符
    const auto lineStart = doc.completion(lsp::Position{2, 0});
    bool hasDirective = false;
    for (const auto& item : lineStart) if (item.label == "%token") hasDirective = true;
    CHECK(hasDirective);
    const auto inRule = doc.completion(lsp::Position{13, 9});
    bool hasRule = false, hasEmpty = false;
    for (const auto& item : inRule) {
        if (item.label == "term") hasRule = true;
        if (item.label == "%empty") hasEmpty = true;
    }
    CHECK(hasRule);
    CHECK(hasEmpty);

    // 跳转：规则右部的 NUMBER → %define 行
    const auto definition = doc.definition(lsp::Position{15, 18});
    CHECK(definition.size() == 1);
    CHECK(definition[0].start.line == 4);
}

void testDisabledLegacyCodeBlock() {
    lsp::GrammarDocument doc;
    doc.setText("%{{\nint legacy;\n}}%\n");
    bool rejected = false;
    for (const auto& diagnostic : doc.diagnostics()) {
        if (diagnostic.code == "disabled-user-code") rejected = true;
    }
    CHECK(rejected);

    lsp::GrammarDocument clean;
    clean.setText("");
    bool offersLegacy = false;
    bool offersCode = false;
    for (const auto& item : clean.completion({0, 0})) {
        if (item.label == "%{{") offersLegacy = true;
        if (item.label == "%code") offersCode = true;
    }
    CHECK(!offersLegacy);
    CHECK(offersCode);
}

// 段标记与 C99 前缀：%code / %class_body 的结束标记会被单独扫成一个指令 token，
// 所以指令表里必须有它们，否则编辑器会误报 unknown-directive。
void testSectionDirectives() {
    lsp::GrammarDocument doc;
    doc.setText(kSectionGrammar);

    // 1. 不能有任何错误诊断：这就是 %endcode / %endbody 误报的回归测试。
    int errors = 0;
    for (const auto& diagnostic : doc.diagnostics()) {
        if (diagnostic.severity == lsp::Severity::Error) {
            std::cerr << "  意外诊断: " << diagnostic.message << "\n";
            ++errors;
        }
    }
    CHECK(errors == 0);

    // 2. 三个指令都要能悬停到说明（%prefix 之前也漏在指令表外）。
    const lsp::HoverResult prefixHover = doc.hover(lsp::Position{2, 2});
    CHECK(prefixHover.found);
    CHECK(prefixHover.markdown.find("%prefix") != std::string::npos);

    const lsp::HoverResult endCodeHover = doc.hover(lsp::Position{7, 2});
    CHECK(endCodeHover.found);
    CHECK(endCodeHover.markdown.find("%endcode") != std::string::npos);

    const lsp::HoverResult endBodyHover = doc.hover(lsp::Position{11, 2});
    CHECK(endBodyHover.found);
    CHECK(endBodyHover.markdown.find("%endbody") != std::string::npos);

    // 3. 行首补全里应当能补出这三个指令。
    const auto lineStart = doc.completion(lsp::Position{4, 0});
    bool hasEndCode = false, hasEndBody = false, hasPrefix = false;
    for (const auto& item : lineStart) {
        if (item.label == "%endcode") hasEndCode = true;
        if (item.label == "%endbody") hasEndBody = true;
        if (item.label == "%prefix") hasPrefix = true;
    }
    CHECK(hasEndCode);
    CHECK(hasEndBody);
    CHECK(hasPrefix);
}

// `%token NAME include "表.csv"`：CSV 别的名字表只追加字面量别名，**不新建 Token**。
// 回归点：`include` / `alias` 曾被当成 Token 名（`%token A include "1.csv"` 会多出一个
// 叫 include、字面量是 1.csv 的伪 Token），于是报出假的 unused-token。
const char* kTokenIncludeGrammar = R"(%mode lalr
%value_type int
%token A include "kw.csv"
%token <ival> NUM include "numbers.csv" alias "数字"
%token X "x" alias "艾克斯"
%start expr
expr := A NUM X { $$ = $1; };
)";

// 一条恰好叫 include 的规则：同理不能因为名字撞上关键字就被当成关键字。
const char* kIncludeAsRule = R"(%mode lalr
%token A "a"
%start include
include := A { $$ = $1; };
)";

// 在文格里找第 occurrence 次出现的子串，返回它的 0 基位置（找不到返回 {0,0}）。
// 断言里不写死列号，文法里挪个空格测试也不该红。
lsp::Position findText(const std::string& text, const std::string& needle,
                       std::size_t occurrence = 1) {
    std::size_t offset = std::string::npos;
    for (std::size_t i = 0; i < occurrence; ++i) {
        offset = text.find(needle, offset == std::string::npos ? 0 : offset + 1);
        if (offset == std::string::npos) return {0, 0};
    }
    std::size_t line = 0;
    std::size_t lineStart = 0;
    for (std::size_t i = 0; i < offset; ++i) {
        if (text[i] == '\n') { ++line; lineStart = i + 1; }
    }
    return {line, offset - lineStart};
}

// 某个位置上的语义高亮类型（没有命中返回空串）。
std::string semanticTypeAt(const lsp::GrammarDocument& doc, lsp::Position position) {
    for (const auto& token : doc.semanticTokens()) {
        if (lsp::contains(token.range, position)) return token.type;
    }
    return {};
}

// `%token NAME include "表.csv"` 的解析与语言特性。
void testTokenInclude() {
    lsp::GrammarDocument doc;
    doc.setText(kTokenIncludeGrammar);

    // 1. 干净：include 的参数（CSV 路径）不该被当成本行的固定字面量，也不该多出
    //    叫 include / alias 的伪 Token —— 这些以前都会变成假 unused-token 警告。
    for (const auto& diagnostic : doc.diagnostics()) {
        std::cerr << "  意外诊断: " << diagnostic.message << "\n";
    }
    CHECK(doc.diagnostics().empty());

    // 2. 声明出来的只有 A / NUM / X 三个。
    CHECK(doc.tokens().size() == 3);
    for (const auto& token : doc.tokens()) {
        CHECK(token.name != "include");
        CHECK(token.name != "alias");
    }

    // 3. CSV 路径记在紧邻的那个 Token 上（%token <ival> NUM include "…" → NUM）。
    CHECK(doc.tokens()[0].csvPath == "kw.csv");
    CHECK(doc.tokens()[1].csvPath == "numbers.csv");
    CHECK(doc.tokens()[1].member == "ival");    // 成员标签照常生效
    CHECK(doc.tokens()[2].csvPath.empty());     // 手写那条没有 include
    CHECK(doc.tokens()[1].alias == "数字");      // include 行上的 alias（中文显示名不该被截断）
    CHECK(doc.tokens()[2].alias == "艾克斯");    // 手写行上的 alias（顺带修的老漏）

    // 4. 语义高亮：include / alias 是关键字，不是函数也不是 Token 名。
    CHECK(semanticTypeAt(doc, findText(kTokenIncludeGrammar, "include")) == "keyword");
    CHECK(semanticTypeAt(doc, findText(kTokenIncludeGrammar, "alias")) == "keyword");
    CHECK(semanticTypeAt(doc, findText(kTokenIncludeGrammar, "NUM")) == "type");
    // CSV 路径仍然是字符串
    CHECK(semanticTypeAt(doc, findText(kTokenIncludeGrammar, "\"kw.csv\"")) == "string");

    // 5. 悬停：include 给写法与语义说明；Token 给出别名来源与成员。
    const lsp::HoverResult includeHover = doc.hover(findText(kTokenIncludeGrammar, "include"));
    CHECK(includeHover.found);
    CHECK(includeHover.markdown.find("CSV") != std::string::npos);
    CHECK(includeHover.markdown.find("只追加别名") != std::string::npos);

    const lsp::HoverResult aliasHover = doc.hover(findText(kTokenIncludeGrammar, "alias"));
    CHECK(aliasHover.found);
    CHECK(aliasHover.markdown.find("显示名") != std::string::npos);

    const lsp::HoverResult csvTokenHover = doc.hover(findText(kTokenIncludeGrammar, "A include"));
    CHECK(csvTokenHover.found);
    CHECK(csvTokenHover.markdown.find("kw.csv") != std::string::npos);

    const lsp::HoverResult memberHover = doc.hover(findText(kTokenIncludeGrammar, "NUM"));
    CHECK(memberHover.found);
    CHECK(memberHover.markdown.find("numbers.csv") != std::string::npos);
    CHECK(memberHover.markdown.find("ival") != std::string::npos);

    // 6. 补全：`%token NAME ` 之后补 include / alias（CSV 别名表 / 显示名）。
    //    位置取 `include` 左边一格（= 名字与 include 之间的空白），模拟刚敲完名字。
    const lsp::Position includePos = findText(kTokenIncludeGrammar, "include");
    const lsp::Position afterName{includePos.line, includePos.column - 1};
    const auto afterTokenName = doc.completion(afterName);
    bool hasInclude = false;
    bool hasAlias = false;
    for (const auto& item : afterTokenName) {
        if (item.label == "include") hasInclude = true;
        if (item.label == "alias") hasAlias = true;
    }
    CHECK(hasInclude);
    CHECK(hasAlias);
    // 行首仍然补 %指令（include 不该挤掉正常的指令补全）。
    const auto atDirectiveStart = doc.completion({afterName.line, 0});
    bool hasTokenDirective = false;
    for (const auto& item : atDirectiveStart) {
        if (item.label == "%token") hasTokenDirective = true;
    }
    CHECK(hasTokenDirective);

    // 7. 大纲与跳转：CSV 来源体现在详情里；`A` 仍能跳到声明处。
    bool outlineHasCsv = false;
    for (const auto& symbol : doc.symbols()) {
        if (symbol.name == "A" && symbol.detail.find("kw.csv") != std::string::npos) {
            outlineHasCsv = true;
            CHECK(symbol.detail.find("include") != std::string::npos);
        }
    }
    CHECK(outlineHasCsv);
    const auto definition = doc.definition(findText(kTokenIncludeGrammar, "A NUM"));
    CHECK(definition.size() == 1);
    CHECK(definition[0].start.line == 2);   // 跳到第 3 行的 `%token A include "kw.csv"`

    // 8. 反例：一条叫 include 的规则不该被当成关键字。
    lsp::GrammarDocument clash;
    clash.setText(kIncludeAsRule);
    CHECK(clash.diagnostics().empty());
    CHECK(clash.tokens().size() == 1);
    const lsp::Position ruleName = findText(kIncludeAsRule, "include", 2);
    CHECK(semanticTypeAt(clash, ruleName) == "function");
    const lsp::HoverResult ruleHover = clash.hover(ruleName);
    CHECK(ruleHover.found);
    CHECK(ruleHover.markdown.find("非终结符") != std::string::npos);
}

void testBrokenGrammar() {
    lsp::GrammarDocument doc;
    doc.setText(kBrokenGrammar);
    bool unknownDirective = false, unknownMode = false, implicitToken = false;
    bool unknownSync = false, unterminatedAction = false;
    for (const auto& diagnostic : doc.diagnostics()) {
        if (diagnostic.code == "unknown-directive") unknownDirective = true;
        if (diagnostic.code == "unknown-value") unknownMode = true;
        if (diagnostic.code == "implicit-token") implicitToken = true;
        if (diagnostic.code == "unknown-sync") unknownSync = true;
        if (diagnostic.code == "unterminated-action") unterminatedAction = true;
    }
    CHECK(unknownDirective);
    CHECK(unknownMode);
    CHECK(implicitToken);
    CHECK(unknownSync);
    CHECK(unterminatedAction);

    // 未声明 Token 的快速修复：插到指令区末尾。
    bool quickFix = false;
    for (const auto& action : doc.codeActions()) {
        if (action.title.find("MISSING") != std::string::npos) {
            quickFix = true;
            CHECK(action.edit.newText.find("%token") != std::string::npos);
        }
    }
    CHECK(quickFix);
    // --check 文本报告
    const std::string report = lsp::Server::checkText(kBrokenGrammar, "broken.gram");
    CHECK(report.find("broken.gram:") != std::string::npos);
    CHECK(report.find("[implicit-token]") != std::string::npos);
}

void testSession() {
    lsp::Server server;
    const std::string initialize =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{}}";
    const std::string initResponse = server.handleMessage(initialize);
    CHECK(initResponse.find("Content-Length:") == 0);
    CHECK(initResponse.find("\"capabilities\"") != std::string::npos);
    CHECK(initResponse.find("semanticTokensProvider") != std::string::npos);

    CHECK(server.handleMessage("{\"jsonrpc\":\"2.0\",\"method\":\"initialized\",\"params\":{}}").empty());

    lsp::Json open = lsp::Json::object();
    open.set("jsonrpc", lsp::Json::string("2.0"));
    open.set("method", lsp::Json::string("textDocument/didOpen"));
    lsp::Json params = lsp::Json::object();
    lsp::Json textDocument = lsp::Json::object();
    textDocument.set("uri", lsp::Json::string("file:///demo.gram"));
    textDocument.set("text", lsp::Json::string(kBrokenGrammar));
    params.set("textDocument", std::move(textDocument));
    open.set("params", std::move(params));
    const std::string openResponse = server.handleMessage(open.dump());
    CHECK(openResponse.find("publishDiagnostics") != std::string::npos);
    CHECK(openResponse.find("implicit-token") != std::string::npos);

    const std::string hover =
        "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"textDocument/hover\",\"params\":"
        "{\"textDocument\":{\"uri\":\"file:///demo.gram\"},\"position\":{\"line\":1,\"character\":2}}}";
    const std::string hoverResponse = server.handleMessage(hover);
    CHECK(hoverResponse.find("contents") != std::string::npos);

    const std::string unknownMethod =
        "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"textDocument/unknown\",\"params\":"
        "{\"textDocument\":{\"uri\":\"file:///demo.gram\"}}}";
    CHECK(server.handleMessage(unknownMethod).find("-32601") != std::string::npos);

    CHECK(server.handleMessage("{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"shutdown\"}").find("\"result\"") != std::string::npos);
    CHECK(server.handleMessage("{\"jsonrpc\":\"2.0\",\"method\":\"exit\"}").empty());
    CHECK(server.exitRequested());
    CHECK(server.exitCode() == 0);
    CHECK(!server.handleMessage("not json").empty());
}


// GLR 与递归下降（dd）是后加的模式，%glr_max_results 也是后加的指令：
// 三者都必须进「已知值 / 指令表」，否则编辑器会把 %mode glr 报成未知取值、
// 把 %glr_max_results 报成 unknown-directive。
const char* kGlrGrammar = R"(%mode glr
%glr_max_results 8
%start expr

expr := expr PLUS expr;
expr := NUMBER;
)";

const char* kDdGrammar = R"(%mode dd
%start expr

expr := NUMBER PLUS expr;
expr := NUMBER;
)";

void testNewModesAndGlrDirective() {
    // 1. %mode glr + %glr_max_results：一条错误诊断都不该有。
    lsp::GrammarDocument glr;
    glr.setText(kGlrGrammar);
    int glrErrors = 0;
    for (const auto& diagnostic : glr.diagnostics()) {
        if (diagnostic.severity == lsp::Severity::Error) {
            std::cerr << "  意外诊断(glr): " << diagnostic.message << "\n";
            ++glrErrors;
        }
    }
    CHECK(glrErrors == 0);

    // 2. %glr_max_results 要能悬停到说明（漏在指令表外时这里也会失败）。
    const lsp::HoverResult glrHover = glr.hover(lsp::Position{1, 2});
    CHECK(glrHover.found);
    CHECK(glrHover.markdown.find("%glr_max_results") != std::string::npos);

    // 3. %mode dd：同样一条错误都不该有。
    lsp::GrammarDocument dd;
    dd.setText(kDdGrammar);
    int ddErrors = 0;
    for (const auto& diagnostic : dd.diagnostics()) {
        if (diagnostic.severity == lsp::Severity::Error) {
            std::cerr << "  意外诊断(dd): " << diagnostic.message << "\n";
            ++ddErrors;
        }
    }
    CHECK(ddErrors == 0);

    // 4. 图表模式也要被接受，并出现在补全清单里。
    for (const char* mode : {"earley", "cyk"}) {
        lsp::GrammarDocument chart;
        chart.setText(std::string("%mode ") + mode + "\n%start expr\nexpr := NUMBER;\n");
        for (const auto& diagnostic : chart.diagnostics())
            CHECK(diagnostic.code != "unknown-value");
    }
    bool hasGlr = false, hasEarley = false, hasCyk = false, hasDd = false;
    for (const auto& item : dd.completion(lsp::Position{0, 6})) {
        if (item.label == "glr") hasGlr = true;
        if (item.label == "earley") hasEarley = true;
        if (item.label == "cyk") hasCyk = true;
        if (item.label == "dd") hasDd = true;
    }
    CHECK(hasGlr);
    CHECK(hasEarley);
    CHECK(hasCyk);
    CHECK(hasDd);
}

void testExampleDirective() {
    lsp::GrammarDocument doc;
    doc.setText("%example \"say \\\"hi\\\"\\nnext\"\n"
                "%tree\n%start text\ntext := STRING;\n");
    for (const auto& diagnostic : doc.diagnostics())
        CHECK(diagnostic.code != "unknown-directive");
    const lsp::HoverResult hover = doc.hover({0, 2});
    CHECK(hover.found);
    CHECK(hover.markdown.find("%example") != std::string::npos);
    bool completed = false;
    for (const auto& item : doc.completion({4, 0}))
        if (item.label == "%example") completed = true;
    CHECK(completed);
}

/**
 * `%include` / `%build_files` / `%cpp_lexer` 三条例外指令的补全、悬停与诊断。
 *
 * 这三个指令生成器都支持，但一度没登记进 LSP 的指令表，于是补全里看不到、悬停
 * 没说明、还会被误报成 unknown-directive。
 */
void testIncludeAndBuildDirectives() {
    lsp::GrammarDocument doc;
    doc.setText("%include \"base.gram\"\n"
                "%build_files on\n"
                "%cpp_lexer dfa\n"
                "%start text\ntext := A;\n");
    for (const auto& diagnostic : doc.diagnostics())
        CHECK(diagnostic.code != "unknown-directive");
    for (const std::string name : {"%include", "%build_files", "%cpp_lexer"}) {
        bool completed = false;
        for (const auto& item : doc.completion({4, 0}))
            if (item.label == name) completed = true;
        CHECK(completed);
    }
    const lsp::HoverResult includeHover = doc.hover({0, 2});
    CHECK(includeHover.found);
    CHECK(includeHover.markdown.find("%include") != std::string::npos);
    // 行内取值补全：`%cpp_lexer` 给 dfa / std，`%build_files` 给 on / off。
    bool dfa = false;
    for (const auto& item : doc.completion({2, 11}))
        if (item.label == "dfa") dfa = true;
    CHECK(dfa);
    bool off = false;
    for (const auto& item : doc.completion({1, 13}))
        if (item.label == "off") off = true;
    CHECK(off);
}

/**
 * `%include` 链上的声明要被主文档认账：被引文件里声明的 Token 不报
 * 「隐式 Token」，`%sync` 认，补全也把它们列出来（detail 标「来自 %include」）。
 *
 * 用临时目录真实读写：include 是文件级的，绕开磁盘测不了。
 */
void testIncludeDeclarations() {
    const char* dir = std::getenv("TEMP");
    CHECK(dir != nullptr);
    const std::string base = std::string(dir) + "/lsp-test-include";
    std::filesystem::create_directories(base);
    { std::ofstream f(base + "/base.gram", std::ios::binary);
      f << "%token MODULE \"module\"\n%token IMPORT \"import\"\n"; }
    lsp::GrammarDocument doc;
    doc.setBaseDir(base);
    doc.setText("%include \"base.gram\"\n"
                "%sync MODULE\n"
                "%start top\ntop := MODULE module ;\nmodule := IMPORT ;\n");
    for (const auto& diagnostic : doc.diagnostics()) {
        CHECK(diagnostic.code != "implicit-token");
        CHECK(diagnostic.code != "unknown-sync");
    }
    CHECK(doc.includedTokens().count("MODULE") > 0);
    CHECK(doc.includedTokens().count("IMPORT") > 0);
    bool listed = false;
    for (const auto& item : doc.completion({3, 0}))
        if (item.label == "MODULE" && item.detail.find("%include") != std::string::npos)
            listed = true;
    CHECK(listed);
    std::filesystem::remove_all(base);
}

} // namespace

int main() {
    testJson();
    testFraming();
    testCleanGrammar();
    testDisabledLegacyCodeBlock();
    testSectionDirectives();
    testTokenInclude();
    testBrokenGrammar();
    testNewModesAndGlrDirective();
    testExampleDirective();
    testIncludeAndBuildDirectives();
    testIncludeDeclarations();
    testSession();
    if (failures == 0) {
        std::cout << "lsp-tests：全部通过\n";
        return 0;
    }
    std::cout << "lsp-tests：" << failures << " 个断言失败\n";
    return 1;
}