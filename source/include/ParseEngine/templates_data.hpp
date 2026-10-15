/**
 * @file templates_data.hpp
 * @brief 内联在可执行文件里的模板表（由 tools/embed_templates.py 生成）。
 */

#ifndef SCRIPT_ENGINE_TEMPLATES_DATA_HPP
#define SCRIPT_ENGINE_TEMPLATES_DATA_HPP

#include <string>
#include <unordered_map>

namespace ParseEngine
{

/**
 * @brief 取内联模板表（键为不带 .tmpl / .template 后缀的模板名）。
 * @return 模板名到模板文本的只读映射。
 */
const std::unordered_map<std::string, std::string> &embeddedTemplates();

} // namespace ParseEngine

#endif // SCRIPT_ENGINE_TEMPLATES_DATA_HPP
