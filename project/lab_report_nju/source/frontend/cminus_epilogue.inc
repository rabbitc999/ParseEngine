#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <vector>

namespace
{
struct Cminus_Node
{
    int kind = 0;
    std::string text;
    int line = 0;
    std::vector<int> children;
};

struct Cminus_FunctionInfo
{
    std::string result;
    std::vector<std::string> params;
    std::vector<std::string> paramNames;
    int node = -1;
};

std::vector<Cminus_Node> g_nodes;
std::vector<std::string> g_tokens;
int g_root = -1;
std::vector<std::string> g_errors;
std::vector<std::map<std::string, std::string> > g_scopes;
std::map<std::string, Cminus_FunctionInfo> g_functions;
std::map<std::string, std::map<std::string, std::string> > g_structs;
std::set<std::string> g_structNames;
std::string g_returnType;
std::vector<std::string> g_quads;
int g_temp = 0;
int g_label = 0;
int g_anonymousStruct = 0;

const Cminus_Node &Cminus_At(int id)
{
    static Cminus_Node empty;
    return (id >= 0 && static_cast<std::size_t>(id) < g_nodes.size())
               ? g_nodes[static_cast<std::size_t>(id)]
               : empty;
}

Cminus_Node &Cminus_AtMut(int id)
{
    return g_nodes[static_cast<std::size_t>(id)];
}

std::string Cminus_Token(int id)
{
    return (id >= 0 && static_cast<std::size_t>(id) < g_tokens.size())
               ? g_tokens[static_cast<std::size_t>(id)]
               : std::string();
}

void Cminus_Error(int type, int line, const std::string &message)
{
    std::ostringstream out;
    out << "Error type " << type << " at Line " << line << ": " << message;
    g_errors.push_back(out.str());
}

bool Cminus_IsList(int id)
{
    return id >= 0 && Cminus_At(id).kind == C_LIST;
}

void Cminus_RegisterStruct(int id);
void Cminus_FillStruct(int id);

std::string Cminus_TypeOfSpec(int id)
{
    const Cminus_Node &initial = Cminus_At(id);
    if (initial.kind == C_STRUCT_DEF &&
        (initial.text.empty() || g_structNames.count(initial.text) == 0))
    {
        Cminus_RegisterStruct(id);
        Cminus_FillStruct(id);
    }
    const Cminus_Node &node = Cminus_At(id);
    if (node.kind == C_TYPE_INT) return "int";
    if (node.kind == C_TYPE_FLOAT) return "float";
    if (node.kind == C_STRUCT_REF || node.kind == C_STRUCT_DEF)
    {
        if (node.text.empty()) return "struct <anonymous>";
        if (node.kind == C_STRUCT_REF && g_structNames.count(node.text) == 0)
            Cminus_Error(17, node.line, "Undefined structure \"" + node.text + "\".");
        return "struct " + node.text;
    }
    return "<error>";
}

std::string Cminus_DeclName(int id)
{
    const Cminus_Node &node = Cminus_At(id);
    if (node.kind == C_INIT && !node.children.empty()) return Cminus_DeclName(node.children[0]);
    if (node.kind == C_VAR_DECL) return node.text;
    if (node.kind == C_ARRAY_DECL && !node.children.empty()) return Cminus_DeclName(node.children[0]);
    return std::string();
}

std::string Cminus_DeclType(int id, const std::string &base)
{
    const Cminus_Node &node = Cminus_At(id);
    if (node.kind == C_INIT && !node.children.empty()) return Cminus_DeclType(node.children[0], base);
    if (node.kind == C_ARRAY_DECL && node.children.size() == 2)
        return "array[" + Cminus_At(node.children[1]).text + "] of " +
               Cminus_DeclType(node.children[0], base);
    return base;
}

bool Cminus_IsArray(const std::string &type)
{
    return type.compare(0, 6, "array[") == 0;
}

std::string Cminus_ArrayElement(const std::string &type)
{
    const std::size_t pos = type.find("] of ");
    return pos == std::string::npos ? "<error>" : type.substr(pos + 5);
}

bool Cminus_IsNumeric(const std::string &type)
{
    return type == "int" || type == "float";
}

bool Cminus_SameType(const std::string &left, const std::string &right)
{
    return left == right || left == "<error>" || right == "<error>";
}

void Cminus_PushScope()
{
    g_scopes.push_back(std::map<std::string, std::string>());
}

void Cminus_PopScope()
{
    if (!g_scopes.empty()) g_scopes.pop_back();
}

bool Cminus_Declare(const std::string &name, const std::string &type, int line)
{
    if (g_scopes.empty()) Cminus_PushScope();
    std::map<std::string, std::string> &scope = g_scopes.back();
    if (scope.count(name) != 0)
    {
        Cminus_Error(3, line, "Redefined variable \"" + name + "\".");
        return false;
    }
    scope[name] = type;
    return true;
}

std::string Cminus_Lookup(const std::string &name, int line)
{
    for (std::vector<std::map<std::string, std::string> >::reverse_iterator it =
             g_scopes.rbegin(); it != g_scopes.rend(); ++it)
    {
        std::map<std::string, std::string>::const_iterator found = it->find(name);
        if (found != it->end()) return found->second;
    }
    Cminus_Error(1, line, "Undefined variable \"" + name + "\".");
    return "<error>";
}

bool Cminus_FindVariable(const std::string &name)
{
    for (std::vector<std::map<std::string, std::string> >::reverse_iterator it =
             g_scopes.rbegin(); it != g_scopes.rend(); ++it)
        if (it->count(name) != 0) return true;
    return false;
}

std::string Cminus_CheckExpr(int id);

void Cminus_CheckDecl(int id, const std::string &base)
{
    const Cminus_Node &node = Cminus_At(id);
    const int decl = node.kind == C_INIT && !node.children.empty() ? node.children[0] : id;
    const std::string name = Cminus_DeclName(decl);
    const std::string type = Cminus_DeclType(decl, base);
    Cminus_Declare(name, type, node.line);
    if (node.kind == C_INIT && node.children.size() == 2)
    {
        const std::string valueType = Cminus_CheckExpr(node.children[1]);
        if (!Cminus_SameType(type, valueType))
            Cminus_Error(5, node.line, "Type mismatched for assignment.");
    }
}

std::string Cminus_CheckExpr(int id)
{
    const Cminus_Node &node = Cminus_At(id);
    if (node.kind == C_VAR) return Cminus_Lookup(node.text, node.line);
    if (node.kind == C_INT_LITERAL) return "int";
    if (node.kind == C_FLOAT_LITERAL) return "float";
    if (node.kind == C_ASSIGN || (node.kind == C_BINARY && node.text == "="))
    {
        if (node.children.size() != 2) return "<error>";
        const Cminus_Node &leftNode = Cminus_At(node.children[0]);
        if (leftNode.kind != C_VAR && leftNode.kind != C_INDEX && leftNode.kind != C_MEMBER)
            Cminus_Error(6, node.line, "The left-hand side of an assignment must be a variable.");
        const std::string left = Cminus_CheckExpr(node.children[0]);
        const std::string right = Cminus_CheckExpr(node.children[1]);
        if (!Cminus_SameType(left, right)) Cminus_Error(5, node.line, "Type mismatched for assignment.");
        return left;
    }
    if (node.kind == C_BINARY && node.children.size() == 2)
    {
        const std::string left = Cminus_CheckExpr(node.children[0]);
        const std::string right = Cminus_CheckExpr(node.children[1]);
        if (!Cminus_SameType(left, right) || !Cminus_IsNumeric(left))
            Cminus_Error(7, node.line, "Type mismatched for operands.");
        if (node.text == "&&" || node.text == "||" || node.text == "<" ||
            node.text == "<=" || node.text == ">" || node.text == ">=" ||
            node.text == "==" || node.text == "!=") return "int";
        return left;
    }
    if (node.kind == C_UNARY && !node.children.empty())
    {
        const std::string operand = Cminus_CheckExpr(node.children[0]);
        if (!Cminus_IsNumeric(operand)) Cminus_Error(7, node.line, "Type mismatched for operands.");
        return node.text == "!" ? "int" : operand;
    }
    if (node.kind == C_CALL)
    {
        std::map<std::string, Cminus_FunctionInfo>::const_iterator found = g_functions.find(node.text);
        if (found == g_functions.end())
        {
            if (Cminus_FindVariable(node.text))
                Cminus_Error(11, node.line, "\"" + node.text + "\" is not a function.");
            else
                Cminus_Error(2, node.line, "Undefined function \"" + node.text + "\".");
            return "<error>";
        }
        const Cminus_Node &args = node.children.empty() ? Cminus_At(-1) : Cminus_At(node.children[0]);
        std::vector<std::string> actual;
        for (std::size_t i = 0; i < args.children.size(); ++i)
            actual.push_back(Cminus_CheckExpr(args.children[i]));
        if (actual.size() != found->second.params.size())
            Cminus_Error(9, node.line, "Function \"" + node.text + "\" is not applicable for arguments.");
        else
            for (std::size_t i = 0; i < actual.size(); ++i)
                if (!Cminus_SameType(actual[i], found->second.params[i]))
                {
                    Cminus_Error(9, node.line, "Function \"" + node.text + "\" is not applicable for arguments.");
                    break;
                }
        return found->second.result;
    }
    if (node.kind == C_INDEX && node.children.size() == 2)
    {
        const std::string base = Cminus_CheckExpr(node.children[0]);
        const std::string index = Cminus_CheckExpr(node.children[1]);
        if (!Cminus_IsArray(base)) Cminus_Error(10, node.line, "Indexing a non-array value.");
        if (index != "int" && index != "<error>") Cminus_Error(12, node.line, "Array index is not an integer.");
        return Cminus_ArrayElement(base);
    }
    if (node.kind == C_MEMBER && !node.children.empty())
    {
        const std::string base = Cminus_CheckExpr(node.children[0]);
        if (base.compare(0, 7, "struct ") != 0)
        {
            Cminus_Error(13, node.line, "Illegal use of '.'.");
            return "<error>";
        }
        const std::string structName = base.substr(7);
        std::map<std::string, std::map<std::string, std::string> >::const_iterator record = g_structs.find(structName);
        if (record == g_structs.end() || record->second.count(node.text) == 0)
        {
            Cminus_Error(14, node.line, "Non-existent field \"" + node.text + "\".");
            return "<error>";
        }
        return record->second.find(node.text)->second;
    }
    return "<error>";
}

void Cminus_CheckStmt(int id);

void Cminus_CheckBlock(int id, bool newScope)
{
    const Cminus_Node &block = Cminus_At(id);
    if (newScope) Cminus_PushScope();
    if (block.children.size() > 0)
    {
        const Cminus_Node &defs = Cminus_At(block.children[0]);
        for (std::size_t i = 0; i < defs.children.size(); ++i)
        {
            const Cminus_Node &def = Cminus_At(defs.children[i]);
            if (def.children.size() != 2) continue;
            const std::string base = Cminus_TypeOfSpec(def.children[0]);
            const Cminus_Node &decls = Cminus_At(def.children[1]);
            for (std::size_t j = 0; j < decls.children.size(); ++j)
                Cminus_CheckDecl(decls.children[j], base);
        }
    }
    if (block.children.size() > 1)
    {
        const Cminus_Node &stmts = Cminus_At(block.children[1]);
        for (std::size_t i = 0; i < stmts.children.size(); ++i) Cminus_CheckStmt(stmts.children[i]);
    }
    if (newScope) Cminus_PopScope();
}

void Cminus_CheckStmt(int id)
{
    const Cminus_Node &node = Cminus_At(id);
    if (node.kind == C_BLOCK) Cminus_CheckBlock(id, true);
    else if (node.kind == C_EXPR_STMT && !node.children.empty()) Cminus_CheckExpr(node.children[0]);
    else if (node.kind == C_RETURN && !node.children.empty())
    {
        const std::string actual = Cminus_CheckExpr(node.children[0]);
        if (!Cminus_SameType(actual, g_returnType)) Cminus_Error(8, node.line, "Type mismatched for return.");
    }
    else if (node.kind == C_IF || node.kind == C_WHILE)
    {
        if (!node.children.empty())
        {
            const std::string cond = Cminus_CheckExpr(node.children[0]);
            if (cond != "int" && cond != "<error>") Cminus_Error(7, node.line, "Condition must have type int.");
        }
        if (node.children.size() > 1) Cminus_CheckStmt(node.children[1]);
        if (node.children.size() > 2 && node.children[2] >= 0) Cminus_CheckStmt(node.children[2]);
    }
}

void Cminus_RegisterStruct(int id)
{
    Cminus_Node &node = Cminus_AtMut(id);
    if (node.kind != C_STRUCT_DEF) return;
    if (node.text.empty())
    {
        std::ostringstream name;
        name << "$anonymous" << ++g_anonymousStruct;
        node.text = name.str();
    }
    if (g_structNames.count(node.text) != 0)
    {
        Cminus_Error(16, node.line, "Duplicated name \"" + node.text + "\".");
        return;
    }
    g_structNames.insert(node.text);
    g_structs[node.text] = std::map<std::string, std::string>();
}

void Cminus_FillStruct(int id)
{
    const Cminus_Node &node = Cminus_At(id);
    if (node.kind != C_STRUCT_DEF || node.text.empty() || node.children.empty()) return;
    std::map<std::string, std::string> &fields = g_structs[node.text];
    const Cminus_Node &defs = Cminus_At(node.children[0]);
    for (std::size_t i = 0; i < defs.children.size(); ++i)
    {
        const Cminus_Node &def = Cminus_At(defs.children[i]);
        if (def.children.size() != 2) continue;
        const std::string base = Cminus_TypeOfSpec(def.children[0]);
        const Cminus_Node &decls = Cminus_At(def.children[1]);
        for (std::size_t j = 0; j < decls.children.size(); ++j)
        {
            const std::string name = Cminus_DeclName(decls.children[j]);
            if (fields.count(name) != 0) Cminus_Error(15, Cminus_At(decls.children[j]).line, "Redefined field \"" + name + "\".");
            else fields[name] = Cminus_DeclType(decls.children[j], base);
        }
    }
}

void Cminus_RegisterFunction(int id)
{
    const Cminus_Node &node = Cminus_At(id);
    if (node.kind != C_FUNCTION) return;
    if (g_functions.count(node.text) != 0)
    {
        Cminus_Error(4, node.line, "Redefined function \"" + node.text + "\".");
        return;
    }
    Cminus_FunctionInfo info;
    info.result = Cminus_TypeOfSpec(node.children[0]);
    info.node = id;
    const Cminus_Node &params = Cminus_At(node.children[1]);
    for (std::size_t i = 0; i < params.children.size(); ++i)
    {
        const Cminus_Node &param = Cminus_At(params.children[i]);
        info.params.push_back(Cminus_DeclType(param.children[1], Cminus_TypeOfSpec(param.children[0])));
        info.paramNames.push_back(Cminus_DeclName(param.children[1]));
    }
    g_functions[node.text] = info;
}

void Cminus_Analyze(void)
{
    g_errors.clear();
    g_scopes.clear();
    g_functions.clear();
    g_structs.clear();
    g_structNames.clear();
    g_anonymousStruct = 0;
    Cminus_PushScope();
    Cminus_FunctionInfo readInfo; readInfo.result = "int"; g_functions["read"] = readInfo;
    Cminus_FunctionInfo writeInfo; writeInfo.result = "int"; writeInfo.params.push_back("int");
    writeInfo.paramNames.push_back("value"); g_functions["write"] = writeInfo;
    const Cminus_Node &program = Cminus_At(g_root);
    const Cminus_Node &items = program.children.empty() ? Cminus_At(-1) : Cminus_At(program.children[0]);
    for (std::size_t i = 0; i < items.children.size(); ++i)
    {
        const Cminus_Node &item = Cminus_At(items.children[i]);
        if (item.kind == C_STRUCT_DEF) Cminus_RegisterStruct(items.children[i]);
        if (item.kind == C_EXT_VARS && !item.children.empty() && Cminus_At(item.children[0]).kind == C_STRUCT_DEF)
            Cminus_RegisterStruct(item.children[0]);
    }
    for (std::size_t i = 0; i < items.children.size(); ++i)
    {
        const Cminus_Node &item = Cminus_At(items.children[i]);
        if (item.kind == C_STRUCT_DEF) Cminus_FillStruct(items.children[i]);
        if (item.kind == C_EXT_VARS)
        {
            const std::string base = Cminus_TypeOfSpec(item.children[0]);
            if (Cminus_At(item.children[0]).kind == C_STRUCT_DEF) Cminus_FillStruct(item.children[0]);
            const Cminus_Node &decls = Cminus_At(item.children[1]);
            for (std::size_t j = 0; j < decls.children.size(); ++j) Cminus_CheckDecl(decls.children[j], base);
        }
        if (item.kind == C_FUNCTION) Cminus_RegisterFunction(items.children[i]);
    }
    for (std::size_t i = 0; i < items.children.size(); ++i)
    {
        const Cminus_Node &item = Cminus_At(items.children[i]);
        if (item.kind != C_FUNCTION) continue;
        g_returnType = Cminus_TypeOfSpec(item.children[0]);
        Cminus_PushScope();
        const Cminus_Node &params = Cminus_At(item.children[1]);
        for (std::size_t j = 0; j < params.children.size(); ++j)
        {
            const Cminus_Node &param = Cminus_At(params.children[j]);
            Cminus_Declare(Cminus_DeclName(param.children[1]),
                           Cminus_DeclType(param.children[1], Cminus_TypeOfSpec(param.children[0])), param.line);
        }
        Cminus_CheckBlock(item.children[2], false);
        Cminus_PopScope();
    }
}

std::string Cminus_NewTemp() { std::ostringstream s; s << "t" << ++g_temp; return s.str(); }
std::string Cminus_NewLabel(const char *prefix) { std::ostringstream s; s << prefix << ++g_label; return s.str(); }
void Cminus_Emit(const std::string &op, const std::string &a, const std::string &b, const std::string &r)
{ g_quads.push_back("(" + op + ", " + a + ", " + b + ", " + r + ")"); }

std::string Cminus_EmitExpr(int id)
{
    const Cminus_Node &node = Cminus_At(id);
    if (node.kind == C_VAR || node.kind == C_INT_LITERAL || node.kind == C_FLOAT_LITERAL) return node.text;
    if (node.kind == C_ASSIGN && node.children.size() == 2)
    {
        const std::string value = Cminus_EmitExpr(node.children[1]);
        const std::string target = Cminus_EmitExpr(node.children[0]);
        Cminus_Emit(":=", value, "_", target); return target;
    }
    if (node.kind == C_BINARY && node.children.size() == 2)
    {
        const std::string left = Cminus_EmitExpr(node.children[0]);
        const std::string right = Cminus_EmitExpr(node.children[1]);
        const std::string temp = Cminus_NewTemp();
        Cminus_Emit(node.text + ":i32", left, right, temp); return temp;
    }
    if (node.kind == C_UNARY && !node.children.empty())
    {
        const std::string value = Cminus_EmitExpr(node.children[0]);
        const std::string temp = Cminus_NewTemp();
        Cminus_Emit(node.text == "!" ? "!" : "neg", value, "_", temp); return temp;
    }
    if (node.kind == C_CALL)
    {
        const Cminus_Node &args = node.children.empty() ? Cminus_At(-1) : Cminus_At(node.children[0]);
        if (node.text == "write")
        {
            const std::string value = args.children.empty() ? "0" : Cminus_EmitExpr(args.children[0]);
            Cminus_Emit("write:i32", value, "_", "_"); return value;
        }
        const std::string temp = Cminus_NewTemp();
        if (node.text == "read") { Cminus_Emit("read", "_", "_", temp); return temp; }
        const Cminus_FunctionInfo &function = g_functions[node.text];
        for (std::size_t i = 0; i < args.children.size(); ++i)
            Cminus_Emit("param", Cminus_EmitExpr(args.children[i]), "_",
                        i < function.paramNames.size() ? function.paramNames[i] : "_");
        Cminus_Emit("call", node.text, "_", temp); return temp;
    }
    if (node.kind == C_INDEX && node.children.size() == 2)
        return Cminus_EmitExpr(node.children[0]) + "[" + Cminus_EmitExpr(node.children[1]) + "]";
    if (node.kind == C_MEMBER && !node.children.empty()) return Cminus_EmitExpr(node.children[0]) + "." + node.text;
    return "0";
}

void Cminus_EmitStmt(int id);
void Cminus_EmitBlock(int id)
{
    const Cminus_Node &block = Cminus_At(id);
    const Cminus_Node &defs = Cminus_At(block.children[0]);
    for (std::size_t i = 0; i < defs.children.size(); ++i)
    {
        const Cminus_Node &def = Cminus_At(defs.children[i]);
        const Cminus_Node &decls = Cminus_At(def.children[1]);
        for (std::size_t j = 0; j < decls.children.size(); ++j)
        {
            const Cminus_Node &decl = Cminus_At(decls.children[j]);
            Cminus_Emit("decl", "i32", "local", Cminus_DeclName(decls.children[j]));
            if (decl.kind == C_INIT) Cminus_Emit(":=", Cminus_EmitExpr(decl.children[1]), "_", Cminus_DeclName(decl.children[0]));
        }
    }
    const Cminus_Node &stmts = Cminus_At(block.children[1]);
    for (std::size_t i = 0; i < stmts.children.size(); ++i) Cminus_EmitStmt(stmts.children[i]);
}

void Cminus_EmitStmt(int id)
{
    const Cminus_Node &node = Cminus_At(id);
    if (node.kind == C_BLOCK) Cminus_EmitBlock(id);
    else if (node.kind == C_EXPR_STMT && !node.children.empty()) Cminus_EmitExpr(node.children[0]);
    else if (node.kind == C_RETURN && !node.children.empty()) Cminus_Emit("ret:i32", Cminus_EmitExpr(node.children[0]), "_", "_");
    else if (node.kind == C_IF)
    {
        const std::string otherwise = Cminus_NewLabel("else");
        const std::string done = Cminus_NewLabel("endif");
        Cminus_Emit("jz", Cminus_EmitExpr(node.children[0]), "_", otherwise);
        Cminus_EmitStmt(node.children[1]);
        Cminus_Emit("j", "_", "_", done);
        Cminus_Emit("label", "_", "_", otherwise);
        if (node.children.size() > 2 && node.children[2] >= 0) Cminus_EmitStmt(node.children[2]);
        Cminus_Emit("label", "_", "_", done);
    }
    else if (node.kind == C_WHILE)
    {
        const std::string begin = Cminus_NewLabel("while");
        const std::string done = Cminus_NewLabel("endwhile");
        Cminus_Emit("label", "_", "_", begin);
        Cminus_Emit("jz", Cminus_EmitExpr(node.children[0]), "_", done);
        Cminus_EmitStmt(node.children[1]);
        Cminus_Emit("j", "_", "_", begin);
        Cminus_Emit("label", "_", "_", done);
    }
}

void Cminus_GenerateIr(void)
{
    g_quads.clear(); g_temp = 0; g_label = 0;
    const Cminus_Node &program = Cminus_At(g_root);
    const Cminus_Node &items = Cminus_At(program.children[0]);
    for (std::size_t i = 0; i < items.children.size(); ++i)
    {
        const Cminus_Node &item = Cminus_At(items.children[i]);
        if (item.kind != C_EXT_VARS) continue;
        const Cminus_Node &decls = Cminus_At(item.children[1]);
        for (std::size_t j = 0; j < decls.children.size(); ++j)
            Cminus_Emit("decl", "i32", "global", Cminus_DeclName(decls.children[j]));
    }
    Cminus_Emit("call", "main", "_", "_"); Cminus_Emit("halt", "_", "_", "_");
    for (std::size_t i = 0; i < items.children.size(); ++i)
    {
        const Cminus_Node &item = Cminus_At(items.children[i]);
        if (item.kind != C_FUNCTION) continue;
        Cminus_Emit("label", "_", "_", item.text);
        const Cminus_Node &params = Cminus_At(item.children[1]);
        for (std::size_t j = 0; j < params.children.size(); ++j)
            Cminus_Emit("decl", "i32", "local", Cminus_DeclName(Cminus_At(params.children[j]).children[1]));
        Cminus_EmitBlock(item.children[2]);
    }
}

const char *Cminus_KindName(int kind)
{
    static const char *names[] = {"Invalid", "Program", "List", "TypeInt", "TypeFloat",
        "StructDef", "StructRef", "ExtVars", "Function", "Param", "Block", "Def", "Init",
        "VarDecl", "ArrayDecl", "ExprStmt", "Return", "If", "While", "Assign", "Binary",
        "Unary", "Call", "Index", "Member", "Var", "Int", "Float"};
    return kind >= 0 && kind < static_cast<int>(sizeof(names) / sizeof(names[0])) ? names[kind] : "Unknown";
}

void Cminus_DumpNode(std::ostream &out, int id, int depth)
{
    const Cminus_Node &node = Cminus_At(id);
    out << std::string(static_cast<std::size_t>(depth * 2), ' ') << Cminus_KindName(node.kind);
    if (!node.text.empty()) out << ": " << node.text;
    out << " (line " << node.line << ")\n";
    for (std::size_t i = 0; i < node.children.size(); ++i)
        if (node.children[i] >= 0) Cminus_DumpNode(out, node.children[i], depth + 1);
}
} // namespace

int Cminus_TokenValue(int, const char *name, const std::string &text)
{
    const std::string value = !text.empty() ? text : (name == NULL ? std::string() : std::string(name));
    g_tokens.push_back(value);
    return static_cast<int>(g_tokens.size()) - 1;
}

int Cminus_New(int kind, const char *text, int line)
{
    Cminus_Node node; node.kind = kind; node.text = text == NULL ? std::string() : std::string(text); node.line = line;
    g_nodes.push_back(node); return static_cast<int>(g_nodes.size()) - 1;
}

int Cminus_Type(int kind, int line)
{
    return Cminus_New(kind, kind == C_TYPE_INT ? "int" : "float", line);
}

int Cminus_List(int first)
{
    const int id = Cminus_New(C_LIST, "", first >= 0 ? Cminus_At(first).line : 0);
    if (first >= 0) Cminus_AtMut(id).children.push_back(first); return id;
}

int Cminus_Append(int list, int item)
{
    if (!Cminus_IsList(list)) list = Cminus_List(list);
    if (item >= 0) Cminus_AtMut(list).children.push_back(item); return list;
}

int Cminus_Node1(int kind, int child, int line)
{ int id = Cminus_New(kind, "", line); if (child >= 0) Cminus_AtMut(id).children.push_back(child); return id; }
int Cminus_Node2(int kind, int left, int right, int line)
{ int id = Cminus_Node1(kind, left, line); if (right >= 0) Cminus_AtMut(id).children.push_back(right); return id; }
int Cminus_Node3(int kind, int first, int second, int third, int line)
{ int id = Cminus_Node2(kind, first, second, line); Cminus_AtMut(id).children.push_back(third); return id; }
int Cminus_Named(int kind, int nameToken, int child, int line)
{ int id = Cminus_New(kind, Cminus_Token(nameToken).c_str(), line); if (child >= 0) Cminus_AtMut(id).children.push_back(child); return id; }
int Cminus_ArrayDecl(int declaration, int sizeToken, int line)
{
    const int size = Cminus_Named(C_INT_LITERAL, sizeToken, -1, line);
    return Cminus_Node2(C_ARRAY_DECL, declaration, size, line);
}
int Cminus_Function(int functionHead, int type, int, int body, int line)
{
    const Cminus_Node &head = Cminus_At(functionHead);
    int id = Cminus_New(C_FUNCTION, head.text.c_str(), line);
    Cminus_AtMut(id).children.push_back(type);
    Cminus_AtMut(id).children.push_back(head.children.empty() ? Cminus_List(-1) : head.children[0]);
    Cminus_AtMut(id).children.push_back(body); return id;
}
int Cminus_Binary(int op, int left, int right, int line)
{
    static const char *names[] = {"", "=", "&&", "||", "<", "<=", ">", ">=", "==", "!=", "+", "-", "*", "/"};
    int id = Cminus_Node2(op == CM_ASSIGN ? C_ASSIGN : C_BINARY, left, right, line);
    Cminus_AtMut(id).text = (op >= CM_ASSIGN && op <= CM_DIV) ? names[op] : "?";
    return id;
}
int Cminus_Unary(int op, int operand, int line)
{ int id = Cminus_Node1(C_UNARY, operand, line); Cminus_AtMut(id).text = op == CM_NOT ? "!" : "-"; return id; }
int Cminus_SetRoot(int root) { g_root = root; return root; }
void Cminus_Reset(void) { g_nodes.clear(); g_tokens.clear(); g_root = -1; g_errors.clear(); }
const char *Cminus_LastError(void) { static std::string joined; joined.clear(); for (std::size_t i = 0; i < g_errors.size(); ++i) joined += g_errors[i] + "\n"; return joined.c_str(); }

int Cminus_AnalyzeAndEmit(const char *outPrefix, int dumpAst, int emitIr)
{
    Cminus_Analyze();
    if (!g_errors.empty())
    {
        for (std::size_t i = 0; i < g_errors.size(); ++i) std::cerr << g_errors[i] << '\n';
        return 1;
    }
    const std::string prefix = outPrefix == NULL ? "cminus_out" : outPrefix;
    if (dumpAst)
    {
        std::ofstream ast((prefix + ".ast").c_str()); Cminus_DumpNode(ast, g_root, 0);
    }
    if (emitIr)
    {
        Cminus_GenerateIr();
        std::ofstream quad((prefix + ".quad").c_str());
        quad << "# C-- quadruples generated by ParseEngine frontend\n";
        for (std::size_t i = 0; i < g_quads.size(); ++i) quad << i << ": " << g_quads[i] << '\n';
    }
    return 0;
}
