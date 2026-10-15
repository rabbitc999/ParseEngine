/* parser_user.h —— %{{...}}% 的用户代码（C++17 产物）
 *
 * C++17 产物里 %value_type / %union 定义在 parser.hpp 的命名空间里，
 * 所以这个文件只有你的用户代码（声明、typedef、宏、static inline）。
 * parser.hpp 在打开命名空间之前包含它。
 */

#line 65 "pl0.gram"
/* ---------------------------------------------------------------
 * 用户代码块：类型、算子码、Token→语义值的钩子、帮助函数声明。
 * 这里的内容进 pl0_parser_user.h，被 pl0_parser.cpp / pl0_main.cpp 共享，
 * 所以只放声明与宏（函数定义放 %epilogue）。
 * --------------------------------------------------------------- */
#include <stdio.h>
#include <string.h>

/* 位置种类 */
#define PL0_PLACE_NONE   0
#define PL0_PLACE_CONST  1
#define PL0_PLACE_VAR    2
#define PL0_PLACE_TEMP   3
#define PL0_PLACE_LABEL  4
#define PL0_PLACE_CODE   5

/* 语义值：一个「位置」。用户自己 typedef ParserValue_T（没写 %value_type） */
typedef struct Pl0_Place_T
{
    int  kind;      /* PL0_PLACE_* */
    int  value;     /* CONST：常量值；LABEL：标签号；CODE：算子码 */
    int  index;     /* TEMP：三元式下标 */
    char name[32];  /* VAR：变量名；其它为空串 */
} ParserValue_T;

/* 算子码：动作里只能传数字/宏，字符串在帮助函数里映射 */
#define PL0_OP_ADD  1
#define PL0_OP_SUB  2
#define PL0_OP_MUL  3
#define PL0_OP_DIV  4
#define PL0_OP_NEG  5
#define PL0_OP_ODD  6
#define PL0_CMP_EQ  7
#define PL0_CMP_NE  8
#define PL0_CMP_LT  9
#define PL0_CMP_LE  10
#define PL0_CMP_GT  11
#define PL0_CMP_GE  12

/* Token → 语义值：数字 Token 取词法器解析好的 value，标识符先记下名字
 * （**不查表** —— 声明处的 `var x` 也要经过这里，那时 x 还没进符号表），
 * 关键字与运算符没有语义值（给空位置）。查表在 Pl0_Resolve 里、真正用到
 * 这个标识符的地方做（赋值左部 / 表达式里的变量 / read 的变量）。
 * 宏本身写在 parser_user.h 里（不参与 C++17 的标识符改写），但它展开在
 * pl0_parser.cpp 的 namespace 内，所以这里能直接用 TOKEN_*。 */
#define PARSER_USER_TOKEN_VALUE(pToken)                                  \
    Pl0_PlaceFromToken((((pToken)->type) == TOKEN_NUMBER) ? 1 : 0,        \
                       (((pToken)->type) == TOKEN_VARIABLE) ? 1 : 0,      \
                       (int)((pToken)->value), (pToken)->pName)

/* ---- 帮助函数（实现见 %epilogue）---- */
ParserValue_T Pl0_PlaceNone(void);
ParserValue_T Pl0_PlaceFromToken(int isNumber, int isIdentifier, int value,
                                 const char *pName);
ParserValue_T Pl0_Resolve(ParserValue_T place);
ParserValue_T Pl0_Code(int code);
ParserValue_T Pl0_Binary(int op, ParserValue_T left, ParserValue_T right);
ParserValue_T Pl0_Negate(ParserValue_T operand);
ParserValue_T Pl0_OddJump(ParserValue_T operand);
ParserValue_T Pl0_CondJump(int cmpOp, ParserValue_T left,
                           ParserValue_T right);
void          Pl0_EndIf(ParserValue_T falseLabel);
ParserValue_T Pl0_BeginWhile(void);
void          Pl0_EndWhile(ParserValue_T falseLabel);
void          Pl0_DefineConst(ParserValue_T name, ParserValue_T value);
void          Pl0_DefineVar(ParserValue_T name);
void          Pl0_Assign(ParserValue_T target, ParserValue_T value);
void          Pl0_Read(ParserValue_T target);
void          Pl0_Write(ParserValue_T value);
int           Pl0_HasError(void);
void          Pl0_PrintTriples(void);
#line 81 "pl0_parser_user.h"
