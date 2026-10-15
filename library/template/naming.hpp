/**
 * @file naming.hpp
 * @brief 命名转换：驼峰 ↔ 下划线（模板模块自带，生成器拿它派生文件名 / 标识符）。
 *
 * 代码生成经常要做这种转换：模型里的 `MyParser` 要变成文件名 `my_parser.hpp`、
 * 环境变量 `MY_PARSER`，或者回头变回 `MyParser`。放在模板模块里而不是各个后端
 * 各自写一份，规则才是统一的。
 *
 * ```cpp
 * tmpl::toSnakeCase("MyParser");     // "my_parser"
 * tmpl::toSnakeCase("HTTPParser");   // "http_parser"
 * tmpl::toSnakeCase("Parser2Value"); // "parser2_value"
 * tmpl::toCamelCase("my_parser");    // "MyParser"
 * ```
 */

#ifndef PARSE_ENGINE_TEMPLATE_NAMING_HPP
#define PARSE_ENGINE_TEMPLATE_NAMING_HPP

#include <cstddef>
#include <string>
#include <string_view>

namespace tmpl
{

namespace naming_detail
{

/** 是不是小写字母或数字。 */
inline bool isLowerOrDigit(char ch) noexcept
{
    return ((ch >= 'a') && (ch <= 'z')) || ((ch >= '0') && (ch <= '9'));
}

/** 是不是大写字母。 */
inline bool isUpper(char ch) noexcept { return (ch >= 'A') && (ch <= 'Z'); }

/** 是不是小写字母。 */
inline bool isLower(char ch) noexcept { return (ch >= 'a') && (ch <= 'z'); }

} // namespace naming_detail

/**
 * @brief 驼峰 / 帕斯卡转下划线：`MyParser` → `my_parser`。
 *
 * 规则（生成文件名、宏名前缀时都按这一套）：
 *   - 遇到大写字母，前面是小写字母或数字，或者它后面跟着小写字母且自己前面
 *     也是大写字母（缩写的最后一个字母），就插一个下划线；
 *   - `_`、`-`、空格、`.` 一律当分隔符，连续分隔符合并成一个下划线；
 *   - 结果全小写，首尾不留分隔符。
 *
 * @param name 原名。
 * @return 下划线形式。
 */
inline std::string toSnakeCase(std::string_view name)
{
    using naming_detail::isLower;
    using naming_detail::isLowerOrDigit;
    using naming_detail::isUpper;

    std::string out;
    out.reserve(name.size() + 4u);
    for (std::size_t index = 0u; index < name.size(); ++index)
    {
        const char ch = name[index];
        if (isUpper(ch))
        {
            const bool afterWord =
                (index > 0u) && isLowerOrDigit(name[index - 1u]);
            const bool acronymEnd =
                (index > 0u) && isUpper(name[index - 1u]) &&
                ((index + 1u) < name.size()) && isLower(name[index + 1u]);
            if ((afterWord || acronymEnd) && !out.empty() && (out.back() != '_'))
            {
                out += '_';
            }
            out += static_cast<char>(ch - 'A' + 'a');
        }
        else if ((ch == '_') || (ch == '-') || (ch == ' ') || (ch == '.'))
        {
            if (!out.empty() && (out.back() != '_'))
            {
                out += '_';
            }
        }
        else
        {
            out += ch;
        }
    }
    while (!out.empty() && (out.back() == '_'))
    {
        out.pop_back();
    }
    return out;
}

/**
 * @brief 下划线转帕斯卡（大驼峰）：`my_parser` → `MyParser`。
 * @param name 原名。
 * @return 大驼峰形式。
 */
inline std::string toCamelCase(std::string_view name)
{
    std::string out;
    out.reserve(name.size());
    bool upperNext = true;
    for (const char ch : name)
    {
        if ((ch == '_') || (ch == '-') || (ch == ' ') || (ch == '.'))
        {
            upperNext = true;
            continue;
        }
        if (upperNext && (ch >= 'a') && (ch <= 'z'))
        {
            out += static_cast<char>(ch - 'a' + 'A');
        }
        else
        {
            out += ch;
        }
        upperNext = false;
    }
    return out;
}

} // namespace tmpl

#endif // PARSE_ENGINE_TEMPLATE_NAMING_HPP