/**
 * @file utils.cpp
 * @brief 通用小工具实现。
 */

#include "ParseEngine/utils.hpp"

#include "ParseEngine/error.hpp"
#include "ParseEngine/templates_data.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <vector>

namespace ParseEngine
{

std::unordered_map<std::string, std::string> Utils::s_templateCache;

const std::string &Utils::loadTemplate(const std::string &name)
{
    const auto found = s_templateCache.find(name);
    if (found != s_templateCache.end())
    {
        return found->second;
    }
    std::string content;
    // 开发期可以用 SCRIPT_ENGINE_TEMPLATES 指向模板目录，直接读磁盘上的模板；
    // 正常运行走内联表，可执行文件不依赖外部文件。
    // 后缀优先 .tmpl（模板引擎格式），其次 .template（老的占位符格式）。
    const char *envDir = std::getenv("SCRIPT_ENGINE_TEMPLATES");
    bool loaded = false;
    if ((envDir != nullptr) && (envDir[0] != '\0'))
    {
        for (const char *suffix : {".tmpl", ".template"})
        {
            const std::filesystem::path path =
                std::filesystem::path(envDir) / (name + suffix);
            std::ifstream stream(path, std::ios::binary);
            if (!stream)
            {
                continue;
            }
            content.assign(std::istreambuf_iterator<char>(stream),
                           std::istreambuf_iterator<char>());
            loaded = true;
            break;
        }
    }
    if (!loaded)
    {
        const auto &table = embeddedTemplates();
        const auto embedded = table.find(name);
        if (embedded == table.end())
        {
            // 模板是构建时内联进来的（tools/embed_templates.py）：名字都
            // 是代码里写死的，取不到只可能是生成器自身组装有问题，按内部
            // 错误立刻终止，不走用户错误路径
            fatalInternal("找不到模板 " + name);
        }
        content = embedded->second;
    }
    const auto inserted = s_templateCache.emplace(name, std::move(content));
    return inserted.first->second;
}

std::string Utils::varCString(const std::string &value)
{
    std::string result = "\"";
    for (const char ch : value)
    {
        switch (ch)
        {
        case '\\':
            result += "\\\\";
            break;
        case '"':
            result += "\\\"";
            break;
        case '\n':
            result += "\\n";
            break;
        case '\r':
            result += "\\r";
            break;
        case '\t':
            result += "\\t";
            break;
        case '\v':
            result += "\\v";
            break;
        case '\f':
            result += "\\f";
            break;
        default:
            // 控制字节（含字面量里的 \b 通配、\0）一律写成三位八进制：
            // 八进制转义固定吃 3 位，不像 \x 会贪心吃掉后面的十六进制字符
            if ((static_cast<unsigned char>(ch) < 0x20u) || (ch == 0x7f))
            {
                char buffer[8];
                std::snprintf(buffer, sizeof(buffer), "\\%03o",
                              static_cast<unsigned>(static_cast<unsigned char>(
                                  ch)));
                result += buffer;
            }
            else
            {
                result += ch;
            }
            break;
        }
    }
    result += "\"";
    return result;
}

std::string Utils::collapseBlankLines(const std::string &text)
{
    // 把 3 个及以上连续换行压成 2 个（即最多保留一个空行）。
    std::string result;
    result.reserve(text.size());
    int newlineRun = 0;
    for (const char ch : text)
    {
        if (ch == '\n')
        {
            ++newlineRun;
            if (newlineRun <= 2)
            {
                result += ch;
            }
        }
        else
        {
            newlineRun = 0;
            result += ch;
        }
    }
    return result;
}

} // namespace ParseEngine
