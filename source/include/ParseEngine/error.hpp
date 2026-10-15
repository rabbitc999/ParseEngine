/**
 * @file error.hpp
 * @brief 生成器内部的错误值。
 *
 * 生成器**不开异常**：会失败的函数一律 `bool` 返回 + 末尾 `Error &` 出参，
 * 出错时把说明写进 `error` 并返回 false，调用方立刻原样向上返回。
 */

#ifndef SCRIPT_ENGINE_ERROR_HPP
#define SCRIPT_ENGINE_ERROR_HPP

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>

namespace ParseEngine
{

/**
 * @brief 一条错误：说明文本 + 原文件行号。
 */
struct Error
{
    std::string message;   ///< 错误说明
    std::size_t line = 0u; ///< 原文件行号（1 起）；0 = 位置未知

    /**
     * @brief 是否还没有错误。
     * @return 没有错误返回 true。
     */
    bool empty() const
    {
        return message.empty();
    }
};

/**
 * @brief 记录错误：只在还没有错误时写入，保证「最先出错的地方」胜出。
 * @param error 错误槽。
 * @param message 错误说明。
 * @param line 原文件行号（1 起）；0 表示位置未知。
 * @return 恒为 false，便于 `return fail(...)` 写法。
 */
inline bool fail(Error &error, const std::string &message,
                 std::size_t line = 0u)
{
    if (error.empty())
    {
        error.message = message;
        error.line = line;
    }
    return false;
}

/**
 * @brief 记录错误（原样传递下层错误）。
 * @param error 错误槽。
 * @param cause 下层错误。
 * @return 恒为 false。
 */
inline bool fail(Error &error, const Error &cause)
{
    return fail(error, cause.message, cause.line);
}

/**
 * @brief 记录错误并加语境前缀。
 *
 * 用于「本层知道下层错误说的是哪一段」的场合（例如正则、某条产生式），
 * 前缀直接拼在下层说明前面。
 * @param error 错误槽。
 * @param cause 下层错误。
 * @param prefix 语境前缀。
 * @return 恒为 false。
 */
inline bool fail(Error &error, const Error &cause, const std::string &prefix)
{
    return fail(error, prefix + cause.message, cause.line);
}

/**
 * @brief 生成器内部一致性错误的兜底：打印后立即终止。
 *
 * 只用于「按设计不可能发生」的场合：内联模板缺失、分段标记对不上这类
 * 生成器自身的问题。用户输入错误一律走 Error 出参，绝不在这里终止。
 * 因为不走异常，`-fno-exceptions` 也能构建。
 * @param message 说明文本。
 */
[[noreturn]] inline void fatalInternal(const std::string &message)
{
    std::fprintf(stderr,
                 "生成器内部错误：%s\n"
                 "（这属于生成器自身的问题，不是文法错误；请确认生成器版本与"
                 "source/templates/ 一致，可用 tools/embed_templates.py 重新生成内联表）\n",
                 message.c_str());
    std::abort();
}

} // namespace ParseEngine

#endif // SCRIPT_ENGINE_ERROR_HPP
