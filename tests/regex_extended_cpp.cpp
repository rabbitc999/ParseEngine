/** @file regex_extended_cpp.cpp
 * @brief 输出 C++17 扩展正则词法器的 Token 与注释，供跨后端对照。
 */
#include "lexer.hpp"
#include <cstdio>
/** @brief 对命令行输入执行词法分析。 @param argc 参数数。 @param argv 输入文本。
 * @return 词法成功返回零。
 */
int main(int argc, char **argv)
{
    using namespace Parser;
    if (argc != 1 && argc != 2) return 2;
    ParserMemory::String input;
    if (argc == 2) input = argv[1];
    else
    {
        char buffer[4096];
        std::size_t count;
        while ((count = std::fread(buffer, 1, sizeof(buffer), stdin)) != 0) input.append(buffer, count);
    }
    Engine engine;
    ParserMemory::Vector<Token> tokens;
    const Status status = engine.tokenize(input, &tokens);
    for (const auto &token : tokens)
    {
        const auto text = tokenText(token);
        std::printf("%s:%s:%u:%u\n", tokenName(token.type), text.c_str(), token.line, token.column);
    }
    for (const auto &commit : engine.commits())
        std::printf("commit:%s:%s:%d\n", commit.type.c_str(), commit.text.c_str(), commit.line);
    return status == Status::Ok ? 0 : 1;
}
