/** @file generation_cache.hpp
 * @brief 跨进程的二进制文法分析缓存。
 */
#ifndef PARSEENGINE_GENERATION_CACHE_HPP
#define PARSEENGINE_GENERATION_CACHE_HPP
#include "ParseEngine/analysis.hpp"

namespace ParseEngine {
/** @brief 可选的增量生成缓存；失败只影响性能，不改变生成正确性。 */
class GenerationCache {
public:
    /** @brief 获取运行中的生成器内容标识。
     * @param executable 启动路径。@return SHA-256；不能读取时为空。
     */
    static std::string buildId(const std::string &executable);
    /** @brief 建立分析键，排除不会改变建表结果的目标代码。
     * @param grammar 当前文法。@param tokens 终结符。@param priority 优先级。
     * @param mode 模式。@param k 展望长度。@param teach 教学开关。
     * @param reportProfile 报告目标分类。@param build 生成器内容标识。
     * @return 二进制输入摘要。
     */
    static std::string key(const Grammar &grammar, const std::vector<std::string> &tokens,
        const PriorityConfig &priority, const std::string &mode, int k, bool teach,
        const std::string &reportProfile, const std::string &build);
    /** @brief 加载并按当前输入刷新语义动作与源码位置。
     * @param directory 缓存目录。@param key 分析键。@param grammar 当前文法。
     * @param analysis 恢复结果。@param report 分析报告。@param reason 未命中原因。
     * @return 完整校验和刷新均成功时为真。
     */
    static bool load(const std::string &directory, const std::string &key,
        const Grammar &grammar, AnalysisResult &analysis, std::string &report, std::string &reason);
    /** @brief 完整写入临时文件后发布缓存，失败不影响解析器生成。
     * @param directory 缓存目录。@param key 分析键。@param analysis 成功的分析结果。
     * @param report 分析报告。@param reason 写入失败原因。@return 发布成功时为真。
     */
    static bool save(const std::string &directory, const std::string &key,
        const AnalysisResult &analysis, const std::string &report, std::string &reason);
    /** @brief 计算内容 SHA-256。@param bytes 原始字节。@return 十六进制摘要。 */
    static std::string digest(const std::string &bytes);
};
}
#endif
