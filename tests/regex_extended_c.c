/** @file regex_extended_c.c
 * @brief 输出 C99 扩展正则词法器的 Token 与注释，供跨后端对照。
 */
#include "lexer.h"
#include <stdio.h>
#include <string.h>
/** @brief 对命令行输入执行词法分析。 @param argc 参数数。 @param argv 输入文本。
 * @return 词法成功返回零。
 */
int main(int argc, char **argv)
{
    ParserLexer_T lexer;
    ParserToken_T token;
    const ParserCommitList *commits;
    int index;
    int32_t status;
    if (argc != 2) return 2;
    Parser_LexInit(&lexer, argv[1]);
    for (;;)
    {
        char text[PARSER_MAX_STRING_LENGTH];
        memset(&token, 0, sizeof(token));
        status = Parser_NextToken(&lexer, &token);
        if (status != PARSER_OK) break;
        Parser_TokenText(&token, text, (uint32_t)sizeof(text));
        printf("%s:%s:%u:%u\n", Parser_TokenName(token.type), text, token.line, token.column);
        if (token.type == TOKEN_END) break;
    }
    commits = Parser_LexGetCommits(&lexer);
    for (index = 0; index < commits->count; ++index)
        printf("commit:%s:%s:%d\n", commits->items[index].type,
               commits->items[index].text, commits->items[index].line);
    Parser_LexFreeCommits(&lexer);
    return status == PARSER_OK ? 0 : 1;
}
