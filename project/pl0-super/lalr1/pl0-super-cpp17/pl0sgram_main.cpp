/**
 * main.cpp —— 演示入口：读标准输入一行后解析（C++17，LALR(1)）
 * 由 ParseEngine 自动生成，请勿手工修改。
 *
 * 文法里写了 %main 时，用户正文整段替换内建 demo main（外壳不变，stdio /
 * string / parser.hpp 都还在；正文前面自动补一行 `using namespace pl0sgram;`，
 * 所以生成器自己的名字可以直接写）。
 */

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

#include "pl0sgram_parser.hpp"

using namespace pl0sgram;

#line 1244 "D:\\Desktop\\Workspace\\Script-Engine\\project\\pl0-super\\lalr1\\pl0-super.gram"
/* 入口：pl0-super [--asm|--stats] 源文件 —— 不给文件就读标准输入。
 * ① 生成出来的前端归约时就把"简化代码"建进池子；② 库做简化 / 出汇编 / 跑目标机。 */
int main(int argc, char **argv)
{
    ParserValue_T result;
    int32_t status = 0;
    const ParserError_T *pError = NULL;
    std::ifstream stream;
    std::string text;
    std::string path;
    bool showAsm = false;
    bool showStats = false;

    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        if (arg == "--asm") showAsm = true;
        else if (arg == "--stats") showStats = true;
        else path = arg;
    }
    if (!path.empty())
    {
        stream.open(path.c_str(), std::ios::binary);
        if (!stream)
        {
            fprintf(stderr, "打不开文件: %s\n", path.c_str());
            return 2;
        }
        text.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
        stream.close();
    }
    else
    {
        text.assign(std::istreambuf_iterator<char>(std::cin), std::istreambuf_iterator<char>());
    }

    Pl0s_Reset();
    memset(&result, 0, sizeof(result));
    status = Parser_RunText(text.c_str(), &result);
    if (status != PARSER_OK)
    {
        pError = Parser_GetLastError();
        fprintf(stderr, "第 %u 行第 %u 列：%s\n", (unsigned)pError->line,
                (unsigned)pError->column, pError->message);
        return 1;
    }
    if (Pl0s_Error()[0] != '\0')
    {
        fprintf(stderr, "%s\n", Pl0s_Error());
        return 1;
    }
    pl0s::Program program;
    if (!Pl0s_TakeProgram(&program))
    {
        fprintf(stderr, "%s\n", Pl0s_Error());
        return 1;
    }

    std::string error;
    pl0s::SimplifyStats stats;
    if (!pl0s::simplify(program, &error, &stats))
    {
        fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    pl0s::CodegenResult code;
    if (!pl0s::generate(program, code, &error))
    {
        fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    if (showStats)
    {
        printf("简化：折叠 %d，代数 %d，死分支 %d，窥孔 %d\n", stats.folded, stats.algebra,
               stats.deadBranch, stats.peephole);
    }
    if (showAsm)
    {
        fputs(code.asmText.c_str(), stdout);
        return 0;
    }
    pl0s::VmResult run;
    if (!pl0s::runAsm(code.asmText, run))
    {
        fprintf(stderr, "运行时错误：%s\n", run.error.c_str());
        return 1;
    }
    fputs(run.output.c_str(), stdout);
    return 0;
}
#line 111 "pl0sgram_main.cpp"

