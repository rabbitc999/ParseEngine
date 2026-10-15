#pragma once
// 本库的内部统一语言：**带位数与符号信息的三地址指令**（TT3）。
//
//   op:类型 dst, src1, src2      例： add:i32 t1, t2, t3 / mov:u16 x, 5
//   内存操作数自己带位数：         ld:u16 v, [slot:u16] / st:u16 [slot:u16], v
//   类型 = 位宽 + 有无符号：i8/u8/i16/u16/i32/u32/i64/u64/i128/u128/f32/f64
//   没写类型时按 transfer 的规则推断：结果变量的声明类型 → 第一个操作数 → i16
//
// 于是"四元式"与"三地址"合并成一套：三地址的 dst/src1/src2 形状 + 四元式的类型
// 与位数。四元式 → TT3 的降级**保留类型**；下游 dataflow / ssa / regalloc 只关心
// 名字与数据流，用 toPlainText() 取"名字视图"，把 :类型 去掉。
#include "transfer.hpp"

#include <cstddef>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace t1t {

struct TypedInstruction {
    std::string op;      // mov / add / sub / mul / div / ld / st / jmp / jz / jnz / print / halt / phi
    std::string type;    // 位宽 + 符号；空 = 未声明（按 i16 处理）
    std::string dst;
    std::string src1;
    std::string src2;
    std::vector<std::string> phiArgs;   // 仅 phi：按前驱顺序
    std::size_t line = 0;

    bool isPhi() const { return op == "phi"; }
    bool isMove() const { return op == "mov"; }
    bool isCopy() const { return op == "mov"; }
    /**
     * 无条件跳转也算一种跳转（regalloc 的旧谓词如此）。
     */
    bool isAnyJump() const { return isBranch() || op == "jmp"; }
    bool isBranch() const { return op == "jz" || op == "jnz"; }
    bool isTerminator() const { return op == "jmp" || isBranch() || op == "halt"; }
    bool isCall() const { return op == "call"; }
    /**
     * 内存操作数里的位数：`[slot:u16]` → `u16`；没有则返回空。
     */
    static std::string memoryType(const std::string& operand);
};

struct TypedProgram {
    std::vector<std::pair<std::string, std::string>> declarations;   // decl 名字, 类型
    std::vector<TypedInstruction> instructions;
};

// ---------------------------------------------------------------- 统一 CFG
//
// 这三件（基本块 / 程序 / CFG 连接）**全库共用一份**：dataflow、ssa、regalloc
// 都不再各自定义 Instruction / Block / Program，而是直接用这里的类型。
struct Block {
    std::string label;
    std::vector<TypedInstruction> code;
    std::vector<std::size_t> successors;
    std::vector<std::size_t> predecessors;
};

struct Program {
    std::vector<std::pair<std::string, std::string>> declarations;   // decl 名字, 类型
    std::vector<Block> blocks;

    /**
     * 统计程序里的指令条数（不含块标号）。。
     *
     * @return 指令总数
     */
    std::size_t instructionCount() const;
};

/**
 * 按块尾指令连 CFG（分支 / halt 之后自动开新块）。
 */
void connectCfg(Program& program);
/**
 * 统一入口：同一份文本既是 TT3 也是普通三地址（类型可省）。
 */
bool parseProgram(const std::string& text, Program& out, std::vector<std::string>& errors);
/**
 * 名字视图（去掉 :类型），供只看数据流的分析和输出使用。
 */
std::string plainText(const Program& program);

/**
 * 四元式 → TT3；翻译不了的条数与操作名记在 skipped / skippedOps 里。
 */
struct Lowering {
    TypedProgram program;
    std::string text;                                // TT3 文本（带 :类型）
    std::string plain;                               // 名字视图（去掉 :类型）
    std::size_t mapped = 0;
    std::size_t skipped = 0;
    std::map<std::string, std::size_t> skippedOps;
};
/**
 * 四元式降级成统一 IR（TT3）：保留类型与位数，翻译不了的操作计数并列出。。
 *
 * @param program 带类型四元式
 * @return 降级结果：TT3 程序 + 文本 + 名字视图 + 计数
 */
Lowering lowerQuads(const transfer::Program& program);

/**
 * 打印 TT3；声明写 `decl 名字, 类型`，内存操作数写 `[x:类型]`。
 */
std::string print(const TypedProgram& program);
/**
 * 解析文本：同一份文本既是 TT3（`op:类型`）也是普通三地址（类型可省），"load/store" 与 "ld/st" 同义。。
 *
 * @param text 待解析文本
 * @param out 解析结果（扁平指令序列）
 * @param errors 错误信息（含行号）
 * @return 全部解析成功返回 true
 */
bool parse(const std::string& text, TypedProgram& out, std::vector<std::string>& errors);

/**
 * 名字视图：丢掉 `:类型`，得到 dataflow / ssa / regalloc 用的三地址文本。
 */
std::string toPlainText(const TypedProgram& program);

/** 类型 → 指令条数（空类型按 i16 计）。 */
std::map<std::string, std::size_t> widthHistogram(const TypedProgram& program);
/**
 * 一条指令的有效类型：显式写着就用它，否则 i16。
 */
std::string effectiveType(const TypedInstruction& ins);

/**
 * 统计三地址文本里的指令条数（跳过 `label` 行与空行）。。
 *
 * @param text 三地址文本
 * @return 指令条数
 */
std::size_t countInstructions(const std::string& text);
/**
 * 去掉 `print` 行：regalloc 的输入语言里没有输出指令。。
 *
 * @param text 三地址文本
 * @return 去掉 print 之后的文本
 */
std::string stripPrint(const std::string& text);

} // namespace t1t