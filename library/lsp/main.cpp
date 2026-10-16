// 一个最小但完整的 `.gram` 语言服务器（独立可执行文件）。
//
//   lsp-server --stdio            在标准输入输出上跑 LSP（编辑器默认用法）
//   lsp-server --check FILE       直接校验一份文法（CI / 命令行）
//   lsp-server --version | --help
//
// 真正的命令行实现是 `lsp::runCommandLine`（在 lsp.cpp 里）：ParseEngine 生成器
// 把同一份源码编进去，用 `ParseEngine lsp` 提供完全一样的服务，见
// doc/04-parseengine-story.md 与 library/lsp/README.md。
#include "lsp.hpp"

/**
 * @brief 独立语言服务器的入口；逻辑全部委托给 `lsp::runCommandLine`。
 * @param argc 参数个数。
 * @param argv 参数数组。
 * @return 进程退出码（0 正常 / 1 校验有错 / 2 参数错误）。
 */
int main(int argc, char** argv) {
    return lsp::runCommandLine(argc, argv);
}
