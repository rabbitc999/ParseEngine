/**
 * @file token_dict.hpp
 * @brief `%token NAME include "csv"` 展开：把 CSV 里第一列等于 NAME 的那些
 *        第二列字面量追加到该 Token 的别名表里。
 *
 * 本模块在 `Preprocessor` 之后、`GramParser` 之前被调用：
 * 1. 扫一遍预处理文本，收集 `%token ... include "..."` 指令的位置 / Token 名 /
 *    路径（同时记 CSV 第一列需要匹配的 Token 名集合）；
 * 2. 同一份 CSV 按规范化路径只读一次（同一 `.gram` 内多处引用时省事）；
 * 3. 把每条 CSV 展开成 N 条 `%token NAME "字面量"`，按 CSV 行顺序插入到原指令
 *    所在的位置；
 * 4. 展开前先做一次**全量唯一性扫描**：记录
 *    `字面量 -> (源文件:行:列, token 名)`，冲突即报错，避免"先展开后到"的
 *    顺序问题。
 *
 * 别名按字面量追加（不按 CSV 第一列「所属 Token」分类）。这条规则与手写
 * `%token A "x"` 完全一致——CSV 只是把这条声明集中到外部表里。
 */

#ifndef SCRIPT_ENGINE_TOKEN_DICT_HPP
#define SCRIPT_ENGINE_TOKEN_DICT_HPP

#include <cstddef>
#include <string>
#include <vector>

#include "ParseEngine/error.hpp"

namespace ParseEngine
{

/**
 * @brief 展开结果。
 */
struct TokenDictExpandResult
{
    /**
     * @brief 处理后的文本。
     *
     * 每条 `%token ... include "csv"` 在原位置按 CSV 行顺序展开成 N 条
     * `%token NAME "literal"`（不带 `<成员>` 标签——成员标签留在原指令上），
     * 余下文本原样保留。文本里没有 `include` 时与输入**逐字节相同**。
     */
    std::string text;

    /**
     * @brief 行号映射表（处理后第 i 行 -> 原文件第 lineMap[i - 1] 行）。
     *
     * 没展开任何 `include` 时为空（行号恒等）；展开了若干条时按"处理后行号"
     * 给出，方便错误信息指回原文件。
     */
    std::vector<std::size_t> lineMap;
};

/**
 * @brief `%token NAME include "csv"` 展开器。
 */
class TokenDict
{
public:
    /**
     * @brief 展开预处理后的文本里的 `%token ... include "csv"` 指令。
     *
     * 与 `Preprocessor::run` 同级：行为是纯文本变换——只读 CSV、把每条
     * `include` 合并进对应 Token 的字面量声明，不碰任何其它指令。条件编译
     * 假的 `%if` 块已经被 Preprocessor 丢掉，这里看到的文本里只有"生效"的
     * 指令。
     *
     * 错误格式（与需求一致）：
     * - `.gram` 内部的错误（缺 CSV 文件）：`<gramPath>:<行>:<列>: 找不到
     *   CSV 文件 <相对路径>`；`gramPath` 为空时退化成
     *   `第 <行> 行：找不到 CSV 文件 <相对路径>`。
     * - CSV 内部的错误：`<CSV 相对路径>:<行>:<列>: <说明>`（路径就是
     *   `include "..."` 里写的那个，相对 `.gram` 文件所在目录）。
     * 两种情况都通过 `error.line == 0` 表示「说明文本里已经带了完整位置」，
     * 避免 `main` 再补一次「第 N 行：」。
     *
     * @param text 预处理后的文本（已经过 %if 预处理）。
     * @param baseDir 文法文件所在目录（可为空）。CSV 的相对路径按它解析。
     * @param gramPath 传给 `--grammar-file` 的文法路径原文（只用于报错；
     *                 可为空）。
     * @param out 展开结果（成功时写入）。
     * @param error 出错时写入错误说明。
     * @return 成功返回 true。
     */
    static bool expand(const std::string &text, const std::string &baseDir,
                       const std::string &gramPath, TokenDictExpandResult &out,
                       Error &error);
};

} // namespace ParseEngine

#endif // SCRIPT_ENGINE_TOKEN_DICT_HPP