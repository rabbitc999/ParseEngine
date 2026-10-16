#include "lsp.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace lsp {
namespace {

const char* kServerName = "parseengine-lsp";
const char* kServerVersion = "1.0.0";

int completionKind(const std::string& kind) {
    if (kind == "keyword") return 14;
    if (kind == "function") return 3;
    if (kind == "enum") return 13;
    return 7; // type / class
}

int symbolKind(const std::string& kind) {
    if (kind == "directive") return 20; // Key
    if (kind == "token") return 22;     // EnumMember
    return 12;                          // Function
}

} // namespace

std::string frameMessage(const std::string& payload) {
    return "Content-Length: " + std::to_string(payload.size()) + "\r\n\r\n" + payload;
}

bool readMessage(std::istream& in, std::string& payload, std::string& error) {
    payload.clear();
    error.clear();
    std::string line;
    std::size_t length = 0;
    bool hasLength = false;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) break;
        const auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string key = line.substr(0, colon);
        std::string value = line.substr(colon + 1);
        std::transform(key.begin(), key.end(), key.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        const auto first = value.find_first_not_of(" \t");
        value = first == std::string::npos ? std::string() : value.substr(first);
        if (key == "content-length") {
            try {
                length = static_cast<std::size_t>(std::stoull(value));
                hasLength = true;
            } catch (const std::exception&) {
                error = "invalid Content-Length";
                return false;
            }
        }
    }
    if (!hasLength) return false;
    payload.resize(length);
    if (length > 0) in.read(&payload[0], static_cast<std::streamsize>(length));
    if (static_cast<std::size_t>(in.gcount()) != length) {
        error = "truncated message";
        return false;
    }
    return true;
}

Json Server::response(const Json& id, Json result) const {
    Json message = Json::object();
    message.set("jsonrpc", Json::string("2.0"));
    message.set("id", id);
    message.set("result", std::move(result));
    return message;
}

Json Server::error(const Json& id, int code, const std::string& text) const {
    Json message = Json::object();
    message.set("jsonrpc", Json::string("2.0"));
    message.set("id", id);
    Json errorObject = Json::object();
    errorObject.set("code", Json::integer(code));
    errorObject.set("message", Json::string(text));
    message.set("error", std::move(errorObject));
    return message;
}

Json Server::rangeJson(const Range& range) const {
    Json start = Json::object();
    start.set("line", Json::integer(static_cast<long long>(range.start.line)));
    start.set("character", Json::integer(static_cast<long long>(range.start.column)));
    Json end = Json::object();
    end.set("line", Json::integer(static_cast<long long>(range.end.line)));
    end.set("character", Json::integer(static_cast<long long>(range.end.column)));
    Json result = Json::object();
    result.set("start", std::move(start));
    result.set("end", std::move(end));
    return result;
}

Json Server::positionOf(const Json& params) const {
    const Json position = params.get("position");
    Json result = Json::object();
    result.set("line", Json::integer(position.getInteger("line", 0)));
    result.set("character", Json::integer(position.getInteger("character", 0)));
    return result;
}

Json Server::publishDiagnostics(const std::string& uri, const std::vector<Diagnostic>& diagnostics,
                                const std::string& source) const {
    Json items = Json::array();
    for (const auto& diagnostic : diagnostics) {
        Json item = Json::object();
        item.set("range", rangeJson(diagnostic.range));
        item.set("severity", Json::integer(static_cast<long long>(diagnostic.severity)));
        item.set("code", Json::string(diagnostic.code));
        item.set("source", Json::string(source));
        item.set("message", Json::string(diagnostic.message));
        items.push(std::move(item));
    }
    Json params = Json::object();
    params.set("uri", Json::string(uri));
    params.set("diagnostics", std::move(items));
    Json notification = Json::object();
    notification.set("jsonrpc", Json::string("2.0"));
    notification.set("method", Json::string("textDocument/publishDiagnostics"));
    notification.set("params", std::move(params));
    return notification;
}

Server::Document* Server::document(const Json& params) {
    const Json textDocument = params.get("textDocument");
    const std::string uri = textDocument.getString("uri");
    if (uri.empty()) return nullptr;
    const auto found = documents_.find(uri);
    return found == documents_.end() ? nullptr : &found->second;
}

void Server::openDocument(const std::string& uri, const std::string& text,
                          const std::string& languageId) {
    Document& doc = documents_[uri];
    doc.text = text;
    // 这一份只服务 `.gram`：语言模型按 `.gram` 文法建，`languageId` 仅作记录
    // （参数照 LSP 的 didOpen 形状保留）。
    (void)languageId;
    doc.model.setText(text);
}

void Server::changeDocument(const std::string& uri, const std::string& text) {
    Document& doc = documents_[uri];
    doc.text = text;
    doc.model.setText(text);
}

std::string Server::checkText(const std::string& text, const std::string& path) {
    GrammarDocument model;
    model.setText(text);
    return model.checkReport(path);
}

std::string Server::handleMessage(const std::string& payload) {
    Json message;
    std::string parseError;
    if (!Json::parse(payload, message, &parseError)) {
        return frameMessage(error(Json(), -32700, "JSON 解析失败：" + parseError).dump());
    }
    const Json id = message.get("id");
    const std::string method = message.getString("method");
    const Json params = message.get("params");
    const bool isRequest = !id.isNull();
    if (method.empty()) return {};

    if (method == "initialize") {
        Json legend = Json::object();
        Json types = Json::array();
        for (const char* type : {"keyword", "type", "function", "string", "number", "comment",
                                 "operator", "variable", "parameter", "macro"}) {
            types.push(Json::string(type));
        }
        legend.set("tokenTypes", std::move(types));
        legend.set("tokenModifiers", Json::array());
        Json semantic = Json::object();
        semantic.set("legend", std::move(legend));
        semantic.set("full", Json::boolean(true));
        Json sync = Json::object();
        sync.set("openClose", Json::boolean(true));
        sync.set("change", Json::integer(1)); // 全量同步
        Json triggers = Json::array();
        for (const char* trigger : {"%", "<", " ", "$"}) triggers.push(Json::string(trigger));
        Json completion = Json::object();
        completion.set("triggerCharacters", std::move(triggers));
        Json capabilities = Json::object();
        capabilities.set("textDocumentSync", std::move(sync));
        capabilities.set("hoverProvider", Json::boolean(true));
        capabilities.set("completionProvider", std::move(completion));
        capabilities.set("definitionProvider", Json::boolean(true));
        capabilities.set("documentSymbolProvider", Json::boolean(true));
        capabilities.set("semanticTokensProvider", std::move(semantic));
        capabilities.set("documentFormattingProvider", Json::boolean(true));
        capabilities.set("codeActionProvider", Json::boolean(true));
        Json info = Json::object();
        info.set("name", Json::string(kServerName));
        info.set("version", Json::string(kServerVersion));
        Json result = Json::object();
        result.set("capabilities", std::move(capabilities));
        result.set("serverInfo", std::move(info));
        return frameMessage(response(id, std::move(result)).dump());
    }
    if (method == "initialized") return {};
    if (method == "shutdown") {
        shutdown_ = true;
        return frameMessage(response(id, Json::null()).dump());
    }
    if (method == "exit") {
        exitRequested_ = true;
        exitCode_ = shutdown_ ? 0 : 1;
        return {};
    }
    if (method == "$/cancelRequest") return {};
    if (method == "textDocument/didOpen") {
        const Json textDocument = params.get("textDocument");
        openDocument(textDocument.getString("uri"), textDocument.getString("text"),
                     textDocument.getString("languageId"));
        const auto found = documents_.find(textDocument.getString("uri"));
        return found == documents_.end() ? std::string()
                                         : frameMessage(publishDiagnostics(
                                               found->first, found->second.model.diagnostics(),
                                               "grammar").dump());
    }
    if (method == "textDocument/didChange") {
        const Json textDocument = params.get("textDocument");
        const std::string uri = textDocument.getString("uri");
        const Json changes = params.get("contentChanges");
        if (!changes.items().empty()) {
            changeDocument(uri, changes.items().back().getString("text"));
        }
        const auto found = documents_.find(uri);
        return found == documents_.end() ? std::string()
                                         : frameMessage(publishDiagnostics(
                                               uri, found->second.model.diagnostics(),
                                               "grammar").dump());
    }
    if (method == "textDocument/didClose") {
        const Json textDocument = params.get("textDocument");
        const std::string uri = textDocument.getString("uri");
        documents_.erase(uri);
        return frameMessage(publishDiagnostics(uri, {}).dump());
    }

    Document* doc = document(params);
    if (doc == nullptr) {
        return isRequest ? frameMessage(error(id, -32602, "未打开的文本文档").dump()) : std::string();
    }
    const Json positionJson = params.get("position");
    const Position position{static_cast<std::size_t>(positionJson.getInteger("line", 0)),
                            static_cast<std::size_t>(positionJson.getInteger("character", 0))};

    if (method == "textDocument/hover") {
        const HoverResult hover = doc->model.hover(position);
        if (!hover.found) return frameMessage(response(id, Json::null()).dump());
        Json contents = Json::object();
        contents.set("kind", Json::string("markdown"));
        contents.set("value", Json::string(hover.markdown));
        Json result = Json::object();
        result.set("contents", std::move(contents));
        result.set("range", rangeJson(hover.range));
        return frameMessage(response(id, std::move(result)).dump());
    }
    if (method == "textDocument/completion") {
        Json items = Json::array();
        const auto completions = doc->model.completion(position);
        for (const auto& item : completions) {
            Json json = Json::object();
            json.set("label", Json::string(item.label));
            json.set("kind", Json::integer(completionKind(item.kind)));
            json.set("detail", Json::string(item.detail));
            json.set("insertText", Json::string(item.insertText));
            items.push(std::move(json));
        }
        Json result = Json::object();
        result.set("isIncomplete", Json::boolean(false));
        result.set("items", std::move(items));
        return frameMessage(response(id, std::move(result)).dump());
    }
    if (method == "textDocument/definition") {
        const std::string uri = params.get("textDocument").getString("uri");
        Json locations = Json::array();
        const auto definitions = doc->model.definition(position);
        for (const auto& range : definitions) {
            Json location = Json::object();
            location.set("uri", Json::string(uri));
            location.set("range", rangeJson(range));
            locations.push(std::move(location));
        }
        return frameMessage(response(id, std::move(locations)).dump());
    }
    if (method == "textDocument/documentSymbol") {
        Json symbols = Json::array();
        const auto documentSymbols = doc->model.symbols();
        for (const auto& symbol : documentSymbols) {
            Json json = Json::object();
            json.set("name", Json::string(symbol.name));
            json.set("kind", Json::integer(symbolKind(symbol.kind)));
            json.set("detail", Json::string(symbol.detail));
            json.set("range", rangeJson(symbol.range));
            json.set("selectionRange", rangeJson(symbol.range));
            symbols.push(std::move(json));
        }
        return frameMessage(response(id, std::move(symbols)).dump());
    }
    if (method == "textDocument/semanticTokens/full") {
        // 编码：deltaLine、deltaStart、length、tokenType、tokenModifiers。
        const std::vector<std::string> legend = {"keyword", "type", "function", "string",
                                                 "number", "comment", "operator", "variable",
                                                 "parameter", "macro"};
        Json data = Json::array();
        std::size_t lastLine = 0;
        std::size_t lastColumn = 0;
        const auto tokens = doc->model.semanticTokens();
        for (const auto& token : tokens) {
            const auto index = std::find(legend.begin(), legend.end(), token.type);
            const std::size_t typeIndex =
                index == legend.end() ? 0 : static_cast<std::size_t>(index - legend.begin());
            const std::size_t deltaLine = token.range.start.line - lastLine;
            const std::size_t deltaStart = deltaLine == 0 ? token.range.start.column - lastColumn
                                                          : token.range.start.column;
            const std::size_t length = token.range.start.line == token.range.end.line
                                           ? token.range.end.column - token.range.start.column
                                           : 0;
            if (length == 0) continue;
            data.push(Json::integer(static_cast<long long>(deltaLine)));
            data.push(Json::integer(static_cast<long long>(deltaStart)));
            data.push(Json::integer(static_cast<long long>(length)));
            data.push(Json::integer(static_cast<long long>(typeIndex)));
            data.push(Json::integer(0));
            lastLine = token.range.start.line;
            lastColumn = token.range.start.column;
        }
        Json result = Json::object();
        result.set("data", std::move(data));
        return frameMessage(response(id, std::move(result)).dump());
    }
    if (method == "textDocument/formatting") {
        const std::string formatted = doc->model.format();
        Json edits = Json::array();
        if (formatted != doc->text) {
            Json edit = Json::object();
            Range whole;
            whole.start = Position{0, 0};
            whole.end = Position{static_cast<std::size_t>(std::count(doc->text.begin(), doc->text.end(), '\n') + 1), 0};
            edit.set("range", rangeJson(whole));
            edit.set("newText", Json::string(formatted));
            edits.push(std::move(edit));
        }
        return frameMessage(response(id, std::move(edits)).dump());
    }
    if (method == "textDocument/codeAction") {
        const std::string uri = params.get("textDocument").getString("uri");
        Json actions = Json::array();
        const auto actionsForDocument = doc->model.codeActions();
        for (const auto& action : actionsForDocument) {
            Json edit = Json::object();
            edit.set("range", rangeJson(action.edit.range));
            edit.set("newText", Json::string(action.edit.newText));
            Json edits = Json::array();
            edits.push(std::move(edit));
            Json changes = Json::object();
            changes.set(uri, std::move(edits));
            Json workspaceEdit = Json::object();
            workspaceEdit.set("changes", std::move(changes));
            Json json = Json::object();
            json.set("title", Json::string(action.title));
            json.set("kind", Json::string(action.kind));
            json.set("edit", std::move(workspaceEdit));
            actions.push(std::move(json));
        }
        return frameMessage(response(id, std::move(actions)).dump());
    }
    if (isRequest) {
        return frameMessage(error(id, -32601, "未实现的方法：" + method).dump());
    }
    return {};
}

int runStdioServer(Server& server, std::istream& in, std::ostream& out, std::ostream& log) {
    while (!server.exitRequested()) {
        std::string payload;
        std::string error;
        if (!readMessage(in, payload, error)) {
            if (!error.empty()) log << "读取 LSP 消息失败：" << error << "\n";
            break;
        }
        const std::string response = server.handleMessage(payload);
        if (!response.empty()) {
            out << response;
            out.flush();
        }
    }
    return server.exitCode();
}

namespace {

/**
 * Windows 上把标准输入输出切成二进制模式：LSP 的 `Content-Length` 帧不能做换行转换。
 */
void useBinaryStdio() {
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
}

/**
 * 读整个文件（去掉 UTF-8 BOM）。
 *
 * @param path 文件路径。
 * @param text 读出时写入文件内容。
 * @return 读成功为 true；打不开为 false（`text` 保持为空）。
 */
bool readWholeFile(const std::string& path, std::string& text) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        text.clear();
        return false;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    text = buffer.str();
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF) {
        text.erase(0, 3);
    }
    return true;
}

/**
 * 打印命令行帮助（`lsp-server --help` 与 `ParseEngine lsp --help` 共用）。
 */
void printCommandLineHelp() {
    std::cout << "parseengine-lsp 1.0.0 —— `.gram` 文法语言服务器\n"
                 "\n"
                 "用法：\n"
                 "  lsp-server --stdio            在标准输入输出上跑 LSP（编辑器默认用法）\n"
                 "  lsp-server --check FILE...    直接校验文法，输出 FILE:行:列: 级别: 消息\n"
                 "  lsp-server --version          版本\n"
                 "  lsp-server --help             本帮助\n"
                 "\n"
                 "内嵌在生成器里时用 `ParseEngine lsp <同样的选项>`。\n"
                 "\n"
                 "能力：诊断、悬停、补全、跳转定义、文档大纲、语义高亮、保守格式化、快速修复。\n";
}

} // namespace

int runCommandLine(int argc, char** argv) {
    useBinaryStdio();
    std::string mode = "--stdio";
    std::vector<std::string> files;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            printCommandLineHelp();
            return 0;
        }
        if (arg == "--version" || arg == "-v") {
            std::cout << "parseengine-lsp 1.0.0\n";
            return 0;
        }
        if (arg == "--check") {
            mode = "--check";
            continue;
        }
        if (arg == "--stdio") {
            mode = "--stdio";
            continue;
        }
        files.push_back(arg);
    }
    if (mode == "--check") {
        if (files.empty()) {
            std::cerr << "用法：lsp-server --check FILE...\n";
            return 2;
        }
        int errors = 0;
        for (const std::string& file : files) {
            std::string text;
            if (!readWholeFile(file, text)) {
                std::cout << file << ": error: 无法读取文件\n";
                ++errors;
                continue;
            }
            const std::string report = Server::checkText(text, file);
            std::cout << report;
            if (report.find(": error:") != std::string::npos) ++errors;
        }
        return errors == 0 ? 0 : 1;
    }
    Server server;
    return runStdioServer(server, std::cin, std::cout, std::cerr);
}

} // namespace lsp
