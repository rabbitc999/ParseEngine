/**
 * @file utils.hpp
 * @brief 通用小工具：模板加载、C 字符串转义、空行压缩。
 */

#ifndef SCRIPT_ENGINE_UTILS_HPP
#define SCRIPT_ENGINE_UTILS_HPP

#include <string>
#include <unordered_map>

namespace ParseEngine
{

/**
 * @brief 生成器通用工具集合（无状态，全部为静态函数）。
 */
class Utils
{
public:
    /**
     * @brief 读取模板（默认取内联模板表，可用环境变量 SCRIPT_ENGINE_TEMPLATES 覆盖）。
     * @param name 模板名（不带后缀）。磁盘上先找 `<name>.tmpl`（模板引擎格式），
     *             再找 `<name>.template`（老的占位符格式）。
     * @return 模板文本；找不到模板（生成器自身不完整）时以内部错误终止。
     */
    static const std::string &loadTemplate(const std::string &name);

    /**
     * @brief 把字符串转成安全的 C 字符串字面量（含两侧双引号）。
     * @param value 原始字符串。
     * @return 形如 "abc" 的 C 字面量文本，内部反斜杠与双引号已转义。
     */
    static std::string varCString(const std::string &value);

    /**
     * @brief 把连续 3 个以上的换行压成一个空行。
     * @param text 输入文本。
     * @return 压缩后的文本。
     */
    static std::string collapseBlankLines(const std::string &text);

private:
    static std::unordered_map<std::string, std::string> s_templateCache; ///< 模板缓存
};

} // namespace ParseEngine

#endif // SCRIPT_ENGINE_UTILS_HPP
