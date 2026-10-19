/** @file regex_extended_c.c
 * @brief 输出 C99 扩展正则词法器的 Token 与注释，供跨后端对照。
 */
#include "lexer.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
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
    char *owned = NULL;
    const char *input;
    if (argc != 1 && argc != 2) return 2;
    if (argc == 1)
    {
        size_t length = 0, capacity = 4096;
        int ch;
        owned = (char *)malloc(capacity);
        if (owned == NULL) return 2;
        while ((ch = getchar()) != EOF)
        {
            if (length + 1 >= capacity)
            {
                char *grown = (char *)realloc(owned, capacity * 2);
                if (grown == NULL) { free(owned); return 2; }
                owned = grown; capacity *= 2;
            }
            owned[length++] = (char)ch;
        }
        owned[length] = '\0';
    }
    input = argc == 2 ? argv[1] : owned;
    Parser_LexInit(&lexer, input);
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
    free(owned);
    return status == PARSER_OK ? 0 : 1;
}
