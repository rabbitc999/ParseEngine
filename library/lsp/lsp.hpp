#pragma once
// LSP 传输与服务器：`Content-Length` 帧 + JSON-RPC 分派。
#include "grammar.hpp"
#include "json.hpp"

#include <istream>
#include <map>
#include <ostream>
#include <string>

namespace lsp {

/**
 * 把一段 JSON 打包成 LSP 消息（`Content-Length: N\r\n\r\n{…}`）。
 */
std::string frameMessage(const std::string& payload);
/**
 * 从流里读一条消息；返回 false 表示流结束或格式错误。
 */
bool readMessage(std::istream& in, std::string& payload, std::string& error);

/**
 * 一个最小但完整的 `.gram` 语言服务器：
 * initialize / initialized / shutdown / exit、didOpen / didChange / didClose、
 * hover、completion、definition、documentSymbol、semanticTokens/full、
 * formatting、codeAction，以及 didOpen / didChange 后的 publishDiagnostics。
 */
class Server {
public:
    /**
     * 处理一条 JSON-RPC 报文，返回要写回的报文（没有响应时返回空串）。
     */
    std::string handleMessage(const std::string& payload);
    bool exitRequested() const { return exitRequested_; }
    int exitCode() const { return exitCode_; }
    /**
     * 直接校验一份文法文本（`--check` 与测试用）。
     */
    static std::string checkText(const std::string& text, const std::string& path);

private:
    struct Document {
        std::string text;
        GrammarDocument model;
    };
    /**
     * 造一条 JSON-RPC 成功响应：`{"jsonrpc":"2.0","id":…,"result":…}`。
     *
     * @param id     请求里的 id（原样回填，可能是数字或字符串）
     * @param result 结果对象，可以是 null
     * @return 可直接交给 frameMessage 的响应对象
     */
    Json response(const Json& id, Json result) const;
    /**
     * 造一条 JSON-RPC 错误响应：`{"jsonrpc":"2.0","id":…,"error":{code,message}}`。
     *
     * 用 JSON-RPC 标准码（-32700 解析失败、-32601 方法不存在、-32602 参数不合法）。
     *
     * @param id      请求里的 id；连 id 都解析不出来时传 null
     * @param code    JSON-RPC 错误码（负数）
     * @param message 给用户看的说明（中文）
     * @return 可直接交给 frameMessage 的响应对象
     */
    Json error(const Json& id, int code, const std::string& message) const;
    /**
     * 造一条 `textDocument/publishDiagnostics` 通知（didOpen / didChange 之后推给编辑器）。
     *
     * @param uri   文档 URI，原样回填，编辑器据此把诊断贴到对应标签页
     * @param model 该文档当前的分析结果，诊断从这里取
     * @return 通知报文对象（通知不带 id）
     */
    Json publishDiagnostics(const std::string& uri, const std::vector<Diagnostic>& diagnostics,
                            const std::string& source = "grammar") const;
    /**
     * 从请求参数里取光标位置：读 `params.position.{line,character}`。
     *
     * @param params 请求的 params
     * @return `{"line":…,"character":…}`；参数缺失时两项都是 0（安全退化为文件开头）
     */
    Json positionOf(const Json& params) const;
    /**
     * 把内部 Range 转成 LSP 的 `{"start":{…},"end":{…}}`。
     *
     * @param range 内部区间（0 基行列，列按 UTF-16 代码单元）
     * @return LSP 区间对象
     */
    Json rangeJson(const Range& range) const;
    /**
     * 按请求参数里的 `textDocument.uri` 找已打开的文档。
     *
     * @param params 请求的 params
     * @return 文档句柄；URI 未打开或参数缺失时返回 nullptr，调用方需自行回错误
     */
    Document* document(const Json& params);
    /**
     * didOpen：登记新文档并立刻扫一遍（诊断随后由 publishDiagnostics 推回）。
     *
     * @param uri  文档 URI
     * @param text 全文内容
     */
    void openDocument(const std::string& uri, const std::string& text,
                      const std::string& languageId);
    /**
     * didChange：整文本替换（本实现只支持全文同步），替换后重扫并重推诊断。
     *
     * @param uri  文档 URI
     * @param text 新的全文
     */
    void changeDocument(const std::string& uri, const std::string& text);

    std::map<std::string, Document> documents_;
    bool shutdown_ = false;
    bool exitRequested_ = false;
    int exitCode_ = 0;
};

/** 标准输入输出上的服务器主循环。 */
int runStdioServer(Server& server, std::istream& in, std::ostream& out, std::ostream& log);

/**
 * 命令行入口：`--stdio` / `--check FILE...` / `--version` / `--help`。
 *
 * 独立可执行文件 `lsp-server` 与内嵌到 ParseEngine 生成器时的 `ParseEngine lsp`
 * 共用同一个入口，所以两条路的参数、退出码与输出完全一致。Windows 上会把标准
 * 输入输出切成二进制模式（LSP 的 `Content-Length` 帧不能做换行转换）。
 *
 * @param argc 参数个数（与 `main` 一致，含程序名）。
 * @param argv 参数数组（UTF-8）。
 * @return 正常为零；`--check` 有 error 为一；参数错误为二。
 */
int runCommandLine(int argc, char** argv);

} // namespace lsp
