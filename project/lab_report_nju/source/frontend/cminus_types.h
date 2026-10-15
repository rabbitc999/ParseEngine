#pragma once

#include <cstdint>
#include <string>

enum Cminus_NodeKind
{
    C_PROGRAM = 1,
    C_LIST,
    C_TYPE_INT,
    C_TYPE_FLOAT,
    C_STRUCT_DEF,
    C_STRUCT_REF,
    C_EXT_VARS,
    C_FUNCTION,
    C_PARAM,
    C_BLOCK,
    C_DEF,
    C_INIT,
    C_VAR_DECL,
    C_ARRAY_DECL,
    C_EXPR_STMT,
    C_RETURN,
    C_IF,
    C_WHILE,
    C_ASSIGN,
    C_BINARY,
    C_UNARY,
    C_CALL,
    C_INDEX,
    C_MEMBER,
    C_VAR,
    C_INT_LITERAL,
    C_FLOAT_LITERAL
};

enum Cminus_Operator
{
    CM_ASSIGN = 1,
    CM_AND,
    CM_OR,
    CM_LT,
    CM_LE,
    CM_GT,
    CM_GE,
    CM_EQ,
    CM_NE,
    CM_ADD,
    CM_SUB,
    CM_MUL,
    CM_DIV,
    CM_NEG,
    CM_NOT
};

int Cminus_TokenValue(int tokenType, const char *name, const std::string &text);
int Cminus_New(int kind, const char *text, int line);
int Cminus_Type(int kind, int line);
int Cminus_List(int first);
int Cminus_Append(int list, int item);
int Cminus_Node1(int kind, int child, int line);
int Cminus_Node2(int kind, int left, int right, int line);
int Cminus_Node3(int kind, int first, int second, int third, int line);
int Cminus_Named(int kind, int nameToken, int child, int line);
int Cminus_ArrayDecl(int declaration, int sizeToken, int line);
int Cminus_Function(int nameToken, int type, int params, int body, int line);
int Cminus_Binary(int op, int left, int right, int line);
int Cminus_Unary(int op, int operand, int line);
int Cminus_SetRoot(int root);
void Cminus_Reset(void);
int Cminus_AnalyzeAndEmit(const char *outPrefix, int dumpAst, int emitIr);
const char *Cminus_LastError(void);

#define PARSER_USER_TOKEN_VALUE(pToken)                                  \
    Cminus_TokenValue((int)((pToken)->type), (pToken)->pName,             \
                      std::string((pToken)->text))
