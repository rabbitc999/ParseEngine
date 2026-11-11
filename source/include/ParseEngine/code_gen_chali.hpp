/** @file code_gen_chali.hpp @brief ChaLi LL / LR / GLR 后端。 */
#ifndef PARSE_ENGINE_CODE_GEN_CHALI_HPP
#define PARSE_ENGINE_CODE_GEN_CHALI_HPP
#include "ParseEngine/project.hpp"
namespace ParseEngine {
/** @brief 判断 ChaLi 是否支持模式。@param mode 规范模式名。@return 支持时为真。 */
bool supportsChaLiMode(const std::string &mode);
/** @brief 生成自包含 ChaLi 解析器源码与使用说明。
 * @param request 工程配置。@param analysis 已构造的分析表。
 * @param out 产物映射。@param error 失败原因。@return 生成成功时为真。
 */
bool generateChaLiProject(const ProjectRequest &request,
                          const AnalysisResult &analysis,
                          std::map<std::string, std::string> &out, Error &error);
}
#endif
