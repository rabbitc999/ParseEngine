/**
 * main.cpp —— 演示入口：读标准输入一行后解析（C++17，LALR(1)）
 * 由 Script-Engine 自动生成，请勿手工修改。
 *
 * 文法里写了 %main 时，用户正文整段替换内建 demo main（外壳不变，stdio /
 * string / parser.hpp 都还在；正文前面自动补一行 `using namespace pl0;`，
 * 所以生成器自己的名字可以直接写）。
 */

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

#include "pl0_parser.hpp"

using namespace pl0;

#line 735 "pl0.gram"
/* ============================================================
 * 入口：pl0 <源文件> —— 读文件、解析、打印三元式。
 *
 * %main 会替换内建 demo main，外壳（stdio / string / pl0_parser.hpp）
 * 仍是模板提供的；C++17 下它是独立编译单元，模板已经插好
 * `using namespace pl0;`，所以下面直接用 RunText / ParserValue_T。
 * ============================================================ */
int main(int argc, char **argv)
{
    char text[16384];
    size_t length = 0;
    FILE *pFile = NULL;
    ParserValue_T result;
    int32_t status = 0;
    const ParserError_T *pError = NULL;

    if (argc != 2)
    {
        fprintf(stderr, "用法: %s <PL/0 源文件>\n", argv[0]);
        return 2;
    }
    pFile = fopen(argv[1], "rb");
    if (pFile == NULL)
    {
        fprintf(stderr, "打不开文件: %s\n", argv[1]);
        return 2;
    }
    length = fread(text, 1u, sizeof(text) - 1u, pFile);
    fclose(pFile);
    text[length] = '\0';

    memset(&result, 0, sizeof(result));
    status = Parser_RunText(text, &result);
    if (status != PARSER_OK)
    {
        pError = Parser_GetLastError();
        fprintf(stderr, "语法错误: code=%d line=%u column=%u message=%s\n",
                (int)pError->code, (unsigned)pError->line,
                (unsigned)pError->column, pError->message);
        return 1;
    }
    if (Pl0_HasError() != 0)
    {
        return 1;
    }
    Pl0_PrintTriples();
    return 0;
}
#line 69 "pl0_main.cpp"

