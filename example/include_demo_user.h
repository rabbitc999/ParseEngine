/* include_demo_user.h —— `%code include` 进来的用户代码（进 parser_user.h）
 *
 * 放什么：声明、typedef、宏、static inline 函数。**不要**写非 inline 的函数定义
 * （parser_user.h 会被 parser.c / parser_token.c 一起 include，链接期会炸重复定义），
 * 函数定义放到 %epilogue 里（本例是 include_demo_epilogue.cpp）。
 */

#include <stdio.h>

/* 声明放这里：parser.c 与 parser_token.c 都能看到它（定义在 %epilogue 里） */
double IncludeDemoScale(double value);

/* 数字 Token 的语义值取自词法器解析好的 pToken->value */
#define PARSER_USER_TOKEN_VALUE(pToken) ((ParserValue_T)((pToken)->value))

/* 演示程序（main.c / main.cpp）打印解析结果 */
#define PARSER_USER_DEMO_PRINT_VALUE(pValue) printf("%g", *(pValue))
