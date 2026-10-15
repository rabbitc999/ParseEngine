/* include_demo_main.cpp —— `%main include` 进来的演示入口
 *
 * %main 段替换产物 main.c / main.cpp 里的内建 demo main（外壳不变：
 * stdio / string / parser.h 都还在）。为了 c 与 cpp17 两个语言档都用同一份，
 * 这里只写 C99 那套名字（C++17 产物靠命名空间里的兼容层，如 Parser_RunText）。
 */
int main(void)
{
    const char *text = "1+2*3";
    ParserValue_T result;
    memset(&result, 0, sizeof(result));
    printf("解析输入: %s\n", text);
    if (Parser_RunText(text, &result) != PARSER_OK)
    {
        printf("解析失败\n");
        return 1;
    }
    printf("解析成功\n");
    printf("解析结果: ");
    PARSER_USER_DEMO_PRINT_VALUE(&result);
    printf("\n");
    return 0;
}
