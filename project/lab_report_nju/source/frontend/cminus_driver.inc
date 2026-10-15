#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

int main(int argc, char **argv)
{
    const char *inputPath = NULL;
    std::string outPrefix = "cminus_out";
    int dumpAst = 1;
    int emitIr = 1;
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--out") == 0 && i + 1 < argc) outPrefix = argv[++i];
        else if (std::strcmp(argv[i], "--no-ast") == 0) dumpAst = 0;
        else if (std::strcmp(argv[i], "--no-ir") == 0) emitIr = 0;
        else if (argv[i][0] == '-')
        {
            std::fprintf(stderr, "unknown option: %s\n", argv[i]); return 2;
        }
        else inputPath = argv[i];
    }
    if (inputPath == NULL)
    {
        std::fprintf(stderr, "usage: %s [--out prefix] [--no-ast] [--no-ir] file.cmm\n", argv[0]);
        return 2;
    }
    std::ifstream stream(inputPath, std::ios::binary);
    if (!stream)
    {
        std::fprintf(stderr, "cannot open file: %s\n", inputPath); return 2;
    }
    const std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    ParserValue_T result = 0;
    Cminus_Reset();
    const int32_t status = Parser_RunText(text.c_str(), &result);
    if (status != PARSER_OK)
    {
        const ParserError_T *error = Parser_GetLastError();
        std::fprintf(stderr, "Error type B at Line %u Column %u: %s\n",
                     static_cast<unsigned>(error->line), static_cast<unsigned>(error->column), error->message);
        return 1;
    }
    return Cminus_AnalyzeAndEmit(outPrefix.c_str(), dumpAst, emitIr);
}
