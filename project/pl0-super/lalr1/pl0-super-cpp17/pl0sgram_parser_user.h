/* parser_user.h —— %code ... %endcode 的用户代码（C++17 产物）
 *
 * C++17 产物里 %value_type / %union 定义在 parser.hpp 的命名空间里，
 * 所以这个文件只有你的用户代码（声明、typedef、宏、static inline）。
 * parser.hpp 在打开命名空间之前包含它。
 */

#line 188 "D:\\Desktop\\Workspace\\Script-Engine\\project\\pl0-super\\lalr1\\pl0-super.gram"

/* ============================================================
 * 共享声明（进 pl0sgram_parser_user.h，被 parser 与 main 各包含一次）
 * ============================================================ */

#include "pl0super.hpp"

#include <stdio.h>
#include <string.h>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

/* 词法池里的一条：取值的 Token 都登记进来，语义值是它的下标 */
typedef struct Pl0s_Lex_T
{
    int         kind;      /* 1=NUMBER 2=DECIMAL 3=STRING 4=VARIABLE */
    std::string text;
    double      number;
    int         line;
} Pl0s_Lex_T;

/* Token → 语义值（生成出来的 parser 对每个 Token 调这个宏） */
#define PARSER_USER_TOKEN_VALUE(pToken)                                              \
    Pl0s_TokenValue((((pToken)->type) == TOKEN_NUMBER)     ? 1                        \
                    : (((pToken)->type) == TOKEN_DECIMAL)  ? 2                        \
                    : (((pToken)->type) == TOKEN_STRING)   ? 3                        \
                    : (((pToken)->type) == TOKEN_VARIABLE) ? 4                        \
                                                           : 0,                       \
                    (double)((pToken)->value),                                       \
                    ((pToken)->pName != nullptr ? (pToken)->pName : (pToken)->name),  \
                    (int)((pToken)->text.size()))

/* 源码运算符 → 助记符：动作里只能写标识符（迷你语法不认字符串字面量），
 * 所以统一用枚举常量（pl0-ir 的 Basic_Op_T 也是这个做法）。 */
typedef enum Pl0s_Op_T
{
    PL0S_OP_ADD = 0, PL0S_OP_SUB, PL0S_OP_MUL, PL0S_OP_DIV, PL0S_OP_MOD,
    PL0S_OP_EQ,      PL0S_OP_NE,  PL0S_OP_LT,  PL0S_OP_LE,  PL0S_OP_GT,
    PL0S_OP_GE,      PL0S_OP_AND, PL0S_OP_OR,  PL0S_OP_NEG, PL0S_OP_NOT
} Pl0s_Op_T;

typedef enum Pl0s_TypeTag_T
{
    PL0S_TYPE_INT = 0, PL0S_TYPE_FLOAT, PL0S_TYPE_STR, PL0S_TYPE_STRUCT
} Pl0s_TypeTag_T;

/* ---- 动作函数（定义在 %epilogue）---- */
double Pl0s_TokenValue(int kind, double value, const char* pName, int nameLen);
void   Pl0s_Reset(void);
void   Pl0s_FinishProgram(double name);
void   Pl0s_EndBlock(double listIndex);

double Pl0s_BeginStruct(double name);
void   Pl0s_AddField(double name, double typeIndex, int line);
void   Pl0s_EndStruct(double tag, int line);
void   Pl0s_AddConst(double name, double exprIndex, int line);
void   Pl0s_IdList(double name);
void   Pl0s_IdListAdd(double name);
void   Pl0s_AddVars(double typeIndex, int line);
void   Pl0s_AddStr(int line);
void   Pl0s_AddArray(double name, double lengthExpr, double typeIndex, int line);
void   Pl0s_AddRef(double name, double placeIndex, int line);
void   Pl0s_BeginProc(double name, int line);
void   Pl0s_EndProc(void);
double Pl0s_Type(int kind, double structName);

double Pl0s_Name(double name, int line);
double Pl0s_Field(double placeIndex, double name, int line);
double Pl0s_Index(double placeIndex, double exprIndex, int line);

double Pl0s_IntLiteral(double lexIndex);
double Pl0s_RealLiteral(double lexIndex);
double Pl0s_TextLiteral(double lexIndex);
double Pl0s_PlaceAsExpr(double placeIndex);
double Pl0s_Binary(int op, double left, double right, int line);
double Pl0s_Unary(int op, double operand, int line);
double Pl0s_Convert(int target, double operand, int line);

double Pl0s_NewList(void);
double Pl0s_ListAdd(double listIndex, double stmtIndex);
double Pl0s_BlockStmt(double listIndex);
double Pl0s_Assign(double placeIndex, double exprIndex, int line);
double Pl0s_Write(double exprIndex, int line);
double Pl0s_If(double cond, double thenStmt, double elseStmt);
double Pl0s_While(double cond, double body);
double Pl0s_For(double name, double from, double to, int down, double body, int line);
double Pl0s_Goto(double name, int line);
double Pl0s_Label(double name, int line);
double Pl0s_Call(double name, int line);

/* ---- %main 用：拿结果 / 说错误 ---- */
const char* Pl0s_Error(void);
int         Pl0s_TakeProgram(pl0s::Program* out);
#line 108 "pl0sgram_parser_user.h"
