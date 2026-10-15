#ifndef PL0_SUPER_HPP
#define PL0_SUPER_HPP

// pl0-super：PL/0 的超集（数组 / 字符串 / 浮点 / goto / for）→ **直接生成目标机汇编** + 一组简化。
//
// 和 project/pl0-ir 的区别：那一份走"三元式 / 四元式"的中间表示；这一份刻意**不走 IR**——
// 语法树就是"简化代码"，代码生成是栈式目标机（pl0s-vm）的一趟后缀式发射，
// 简化（常量折叠 / 代数化简 / 死分支 / 窥孔）就在树上与指令流上做。
//
// 五块能力（都能单独调）：
//   tokenize()  词法：源码 → 记号流
//   parse()     语法：记号流 → 简化语法树（Program）
//   simplify()  简化：树上三趟（常量折叠 / 代数化简 / 死分支），窥孔在生成之后
//   generate()  代码生成：语法树 → 目标机汇编文本
//   runAsm()    目标机：汇编文本 → 执行结果（自带解释器，样例套件靠它验）

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace pl0s {

/** 词法记号种类。 */
enum class TokKind {
    End,       ///< 结束
    Ident,     ///< 标识符
    Number,    ///< 数字字面量（整数或浮点，看 isFloat）
    Text,      ///< 字符串字面量（text 里是去掉引号后的内容）
    Keyword,   ///< 关键字（begin / end / if / for / goto…）
    Symbol,    ///< 符号（:= / + / ( …）
};

/** 一个词法记号。 */
struct Token {
    TokKind kind = TokKind::End;
    std::string text;        ///< 原文（字符串字面量里是去掉引号后的内容）
    std::int64_t number = 0; ///< 整数字面量的值
    double real = 0.0;       ///< 浮点字面量的值（isFloat 为真时有效）
    bool isFloat = false;    ///< 这个数字字面量是不是浮点（`3.14`）
    int line = 1;            ///< 行号（报错用）
};

/**
 * 词法分析：把源码切成记号流。
 *
 * 认：标识符 / 整数 / 浮点（`3.14`、`.5`、`1e3`）/ "字符串"（支持 \" \\ \n 转义）/
 * 关键字 / 符号；`{ … }`、`(* … *)` 与 `//` 到行尾都是注释。
 *
 * @param source 源码
 * @param error  出错时的中文说明（可为空）
 * @return 记号流（末尾恒有一个 End）；出错返回空
 */
std::vector<Token> tokenize(const std::string& source, std::string* error);

struct Expr;
struct Stmt;
using ExprPtr = std::shared_ptr<Expr>;
using StmtPtr = std::shared_ptr<Stmt>;

/** 表达式种类。 */
enum class ExprKind {
    Number,    ///< 整数字面量（number）
    Float,     ///< 浮点字面量（real）
    Text,      ///< 字符串字面量（text）
    Var,       ///< 变量（text 是名字）
    Index,     ///< 数组下标：args[0] 是数组名（Var），args[1] 是下标
    Binary,    ///< 二元（op + args[0] + args[1]）
    Unary,     ///< 一元（op + args[0]）
    Convert,   ///< 类型转换 `int(x)` / `float(x)`（text 是目标类型）
};

/** 表达式节点。 */
struct Expr {
    ExprKind kind = ExprKind::Number;
    std::string text;
    std::int64_t number = 0;
    double real = 0.0;
    std::string op;
    std::vector<ExprPtr> args;
    bool isText = false;     ///< 结果是不是字符串（类型标注）
    bool isFloat = false;    ///< 结果是不是浮点（和 isText 一起当"类型"）
    int line = 1;
};

/** 语句种类。 */
enum class StmtKind {
    Assign,    ///< 赋值：target 是 Var / Index，args[0] 是右值
    Write,     ///< `write 表达式`
    If,        ///< args[0]=条件，body=then 块，otherwise=else 块（可空）
    While,     ///< args[0]=条件，body=循环体
    For,       ///< `for 变量 := 初值 to/downto 终值 do 体`（含终值）
    Goto,      ///< `goto 标签`
    Label,     ///< `label 标签;`（标号声明）
    Block,     ///< `begin … end`
    Call,      ///< `call 过程名`
};

/** 语句节点。 */
struct Stmt {
    StmtKind kind = StmtKind::Block;
    ExprPtr target;              ///< 赋值目标 / for 的循环变量
    std::vector<ExprPtr> args;   ///< 条件 / 右值 / for 的初值与终值
    std::vector<StmtPtr> body;   ///< 语句序列（块、分支体、循环体）
    StmtPtr otherwise;           ///< if 的 else 分支（可空）
    std::string name;            ///< call 的过程名 / goto、label 的标签名
    bool down = false;           ///< for 是不是 `downto`（递减）
    std::string copyStruct;      ///< 非空 = 结构体整体赋值（按字段复制）的结构体名
    int line = 1;
};

/** 声明的种类。 */
enum class DeclKind {
    Const,     ///< `const N := 10;`
    Var,       ///< `var i, j: int;`（一条多个名字会展开成多条）
    Array,     ///< `array a[N]: int;`（一维定长，长度是常量）
    Str,       ///< `str s;`
    Proc,      ///< `procedure p; 声明… begin … end;`
    StructVar, ///< 结构体变量（按字段扁平化，字段名见 StructDef）
    StructArray, ///< 结构体数组（每个字段一条数组，体里 `p[i].x`）
};

/** 一个字段的类型：整数 / 浮点 / 字符串 / 另一个结构体。 */
enum class TypeKind { Int, Float, Str, Struct };

/** 一条字段声明。 */
struct Field {
    std::string name;
    TypeKind kind = TypeKind::Int;
    std::string structName;      ///< kind == Struct 时的结构体名
    std::vector<Field> subs;     ///< 嵌套结构体的字段（扁平化时按 `名字$子名字` 展开）
};

/** 一个结构体声明：`struct Point { x: int; y: float; };` */
struct StructDef {
    std::string name;
    std::vector<Field> fields;
    int line = 1;
};

/** 一条声明。 */
struct Decl {
    DeclKind kind = DeclKind::Var;
    std::string name;            ///< 名字
    std::int64_t value = 0;      ///< Const 的值
    std::int64_t length = 0;     ///< Array 的长度
    bool isFloat = false;        ///< Var / Array 的元素是不是浮点（默认整数）
    std::string structName;      ///< StructVar / StructArray 的结构体名
    int line = 1;
    std::vector<Decl> decls;     ///< Proc 自己的声明
    std::vector<StmtPtr> body;   ///< Proc 的体
};

/** 一棵程序：`program 名字; 声明… begin 体 end.` */
struct Program {
    std::string name;
    std::vector<StructDef> structs;   ///< 结构体声明（字段扁平化要用）
    std::vector<Decl> decls;
    std::vector<StmtPtr> body;
};

/**
 * 语法分析：记号流 → 简化语法树。
 *
 * 文法（简化成一份，够用就好）：
 *   程序 := "program" 名字 ";" 声明组 "begin" 语句组 "end" "."
 *   声明 := "const" 名字 ":=" 整数 ";" | "var" 名字组 ":" 类型 ";"
 *         | "array" 名字 "[" 常量 "]" ":" 类型 ";"
 *         | "str" 名字组 ";" | "procedure" 名字 ";" 声明组 "begin" 语句组 "end" ";"
 *   语句 := 赋值 | "write" 表达式 | "if" 条件 "then" 语句 ["else" 语句]
 *         | "while" 条件 "do" 语句 | "for" 变量 ":=" 表达式 ("to"|"downto") 表达式 "do" 语句
 *         | "goto" 标签 ";" | "label" 标签 ";" | "begin" 语句组 "end" | "call" 名字
 *   表达式 := or 链 → and 链 → 比较 → 加减 → 乘除模 → 一元 → 基本项（下标 / 括号 / 转换）
 *   类型   := "int"（默认）| "float" | "str"
 *
 * @param tokens 记号流
 * @param out    解析结果
 * @param error  出错时的中文说明（可为空）
 * @return 成功 true
 */
bool parse(const std::vector<Token>& tokens, Program& out, std::string* error);

/**
 * 便利函数：源码 → 语法树（词法 + 语法 + 类型标注 + 名字检查）。
 *
 * @param source 源码
 * @param out    解析结果
 * @param error  出错时的中文说明（可为空）
 * @return 成功 true
 */
bool parseSource(const std::string& source, Program& out, std::string* error);

/** 各趟简化各做了多少次（`--stats` 打印它）。 */
struct SimplifyStats {
    int folded = 0;        ///< 常量折叠（含字符串拼接与类型转换）
    int algebra = 0;       ///< 代数化简（x+0、x*1、x*0…）
    int deadBranch = 0;    ///< 常量条件（if / while / for）
    int peephole = 0;      ///< 窥孔（多余的 push/drop、load+store 同名字…）
};

/**
 * 简化：树上跑三趟（常量折叠 / 代数化简 / 死分支），窥孔在 generate() 之后统计。
 *
 * 1. **常量折叠**：整数的 + - * / % 与比较、浮点的 + - * / 与比较、and/or/not、
 *    字符串拼接、`int(3.7)` / `float(2)` 这类常量转换；
 * 2. **代数化简**：x+0、0+x、x-0、x*1、1*x、x*0、0*x、x/1、--x、not not x；
 * 3. **死分支**：if 的条件是常量就只留一支；while、for 的条件是常量时按能判定的方向收；
 * 4. **窥孔**：在生成的指令行上做（见 CodegenResult::lines）。
 *
 * 除数为常量 0 在编译期就报错；`x / 0` 这种运行期的由目标机报。
 *
 * @param program 语法树（原地改）
 * @param error   出错时的中文说明（可为空）
 * @param stats   各趟计数（可为空）
 * @return 成功 true
 */
bool simplify(Program& program, std::string* error, SimplifyStats* stats = nullptr);

/** 代码生成的产物。 */
struct CodegenResult {
    std::string asmText;                 ///< 目标机汇编文本
    std::vector<std::string> lines;      ///< 拆好的每一行（窥孔直接对它做）
    std::vector<std::string> strings;    ///< 用到的字符串常量（去重）
};

/**
 * 代码生成：语法树 → 目标机汇编（栈式，一条指令一个栈动作）。
 *
 * 约定：每个过程一段 `p_<名字>:`；变量访问是"名字 + 帧"（`load a.i` 从最近的 `i`
 * 那一层的帧里取），所以递归与嵌套作用域不需要显式静态链——见技术报告第 4 节。
 *
 * @param program 已经简化过的语法树
 * @param out     产物（汇编文本 + 字符表）
 * @param error   出错时的中文说明（可为空）
 * @return 成功 true
 */
bool generate(const Program& program, CodegenResult& out, std::string* error);

/** 目标机跑一遍的结果。 */
struct VmResult {
    bool ok = false;             ///< 正常跑到 halt 为 true
    std::string output;          ///< 程序打印出来的东西（每个值一行）
    std::string error;           ///< 运行时错误（中文）
    long steps = 0;              ///< 执行的指令条数（跑飞了能看出来）
    std::vector<std::string> tokens;   ///< 输出按空白切开的结果（样例套件比对用）
};

/**
 * 目标机：执行一段 pl0s 汇编（自带的解释器）。
 *
 * 指令：push / pushf / pushs / dup / swap / drop / add / sub / mul / div / mod / neg /
 * tof / toi / lt / le / gt / ge / eq / ne / and / or / not / load / store / decl /
 * decla / aload / astore / label / jmp / jz / jnz / call / ret / write / halt。
 *
 * @param asmText 汇编文本
 * @param out     结果（输出 / 错误 / 步数）
 * @return 正常跑完 true（运行时错误时 out.error 里有中文说明）
 */
bool runAsm(const std::string& asmText, VmResult& out);

/** 一次完整编译的产物（CLI 与样例套件都走它）。 */
struct CompileResult {
    bool ok = false;
    std::string error;
    Program program;
    CodegenResult code;
    SimplifyStats stats;
};

/**
 * 便利函数：源码 → 语法树 → 简化 → 汇编。
 *
 * @param source 源码
 * @param out    产物（语法树 / 汇编 / 简化计数）
 * @return 成功 true（失败时 out.error 有中文说明）
 */
bool compile(const std::string& source, CompileResult& out);

} // namespace pl0s

#endif // PL0_SUPER_HPP
