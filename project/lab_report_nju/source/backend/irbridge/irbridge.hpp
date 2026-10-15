#pragma once
// 前端胶水：三元式 ↔ 四元式互转，并可导出 `library/transfer` 能直接读的四元式。
//
// 文本格式与仓库现有产物一致（`project/pl0-ir/expected/*.triples` 与
// `pl0_quads.gram` 的打印）：
//   三元式  i: (op, arg1, arg2)              结果=本条下标，引用写 (k)
//   四元式  i: (op, arg1, arg2, result)      结果显式写名字
// 两边的表头行（`三元式：共 N 条` / `四元式：共 N 条`）都可选，解析时忽略。
#include <cstddef>
#include <string>
#include <vector>

namespace irbridge {

/**
 * 三元式：`a` / `b` 里可以出现 `(k)`（引用第 k 条的结果）、`_`、常量、变量名。
 */
struct Triple {
    std::string op;
    std::string a;
    std::string b;
};
/**
 * 四元式：`result` 是显式结果（临时量名或变量名；不产生值时写 `_`）。
 */
struct Quad {
    std::string op;
    std::string a;
    std::string b;
    std::string result;
};

using Triples = std::vector<Triple>;
using Quads = std::vector<Quad>;

/** 解析三元式文本；非法行给出 `行号: 原因`。 */
bool readTriples(const std::string& text, Triples& out, std::vector<std::string>& errors);
/**
 * 解析四元式文本（`(op, a, b, r)`，字段可用逗号或空白分隔）。
 */
bool readQuads(const std::string& text, Quads& out, std::vector<std::string>& errors);
/**
 * 按仓库现有打印格式写回（带表头与 `%3d` 下标）。
 */
std::string writeTriples(const Triples& triples);
/**
 * 按仓库固定格式写出四元式文本（下标对齐、字段顺序固定），便于与 fixture 逐字比对。。
 *
 * @param quads 四元式序列
 * @return 文本
 */
std::string writeQuads(const Quads& quads);

/** `(k)` 引用：返回 true 并给出下标。 */
bool parseRef(const std::string& text, std::size_t& index);
/**
 * 临时量名（`t1` / `t2` / `__tr_*`）。
 */
bool isTemporary(const std::string& name);

/**
 * 三元式 → 四元式：`(k)` 换成第 k 条结果的显式名字。
 * 被引用过、且本身产生值的条目会分到 `t<下标>`；没被引用的结果写 `_`。
 * `(k)` 指向不产生值的条目（label / j / write）时报错。
 */
bool triplesToQuads(const Triples& triples, Quads& out, std::vector<std::string>& errors);

/**
 * 四元式 → 三元式：把"值名"（表达式的结果）折回 `(k)` 引用；变量仍然按名字写。
 * 非 `:=` 的四元式如果结果落在变量上，会拆成"表达式 + 赋值"两条三元式，
 * 保证语义不变（这也是 `pl0-ir` 两份文法的差别所在）。
 */
bool quadsToTriples(const Quads& quads, Triples& out, std::vector<std::string>& errors);

/**
 * 导出 `library/transfer` 可直接读的四元式文本；`withDecls` 时补 `decl`。
 */
std::string toTransfer(const Quads& quads, bool withDecls = true);
/** 同样的导出，但输入是三元式。 */
std::string toTransferFromTriples(const Triples& triples, bool withDecls = true);

/**
 * 统计信息（表格输出）：各类操作数、临时量、标签、最大引用下标。
 */
std::string statistics(const Quads& quads);
/**
 * 统计四元式的分类计数（算术 / 比较 / 跳转 / 标签 / 赋值 / 输入输出 / 临时量）。。
 *
 * @param quads 四元式序列
 * @return 中文统计文本
 */
std::string statistics(const Triples& triples);

} // namespace irbridge