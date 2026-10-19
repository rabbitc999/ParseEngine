/** @file regex_vm.hpp
 * @brief 带捕获、断言和反向引用的有序正则虚拟机编译接口。
 */
#ifndef PARSE_ENGINE_REGEX_VM_HPP
#define PARSE_ENGINE_REGEX_VM_HPP
#include "ParseEngine/regex_dfa.hpp"
#include <array>
namespace ParseEngine
{
/** @brief 生成给 C99 运行时的单条正则指令。 */
struct RegexVmInstruction
{
    int op = 0; ///< 操作码，与内嵌运行时保持一致
    int next = 0; ///< 首选后继
    int other = 0; ///< 备选后继或断言入口
    int value = 0; ///< 捕获编号或断言标志
    std::array<unsigned, 32> bits{}; ///< 字节字符类位图
};
/** @brief 一条有序回溯程序。 */
struct RegexVmProgram
{
    std::vector<RegexVmInstruction> code; ///< 指令数组
    int entry = 0; ///< 程序入口
    int groups = 0; ///< 捕获组总数
};
/** @brief 正则 AST 到有序回溯字节码的编译器。 */
class RegexVm
{
public:
    /** @brief 检查必须读取上下文的节点。
     * @param node AST 根。 @return 存在断言或引用时返回 true。
     */
    static bool contextRequired(const std::shared_ptr<RegexNode> &node);
    /** @brief 判断表达式是否需要有序正则运行时。
     * @param node AST 根。 @return 需要运行时时返回 true。
     */
    static bool required(const std::shared_ptr<RegexNode> &node);
    /** @brief 保守检查表达式是否可能不消耗输入。
     * @param node AST 根。 @return 可能零宽匹配时返回 true。
     */
    static bool nullable(const std::shared_ptr<RegexNode> &node);
    /** @brief 将正则编译为有序回溯程序。
     * @param pattern 正则文本。 @param out 输出程序。 @param error 错误。
     * @return 成功返回 true。
     */
    static bool compile(const std::string &pattern, RegexVmProgram &out, Error &error);
};
}
#endif
