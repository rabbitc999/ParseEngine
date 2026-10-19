/** @file regex_vm.cpp
 * @brief 正则虚拟机编译器：按分支顺序保存回溯点，保留捕获和零宽断言。
 */
#include "ParseEngine/regex_vm.hpp"
#include <algorithm>
namespace ParseEngine
{
namespace
{
/** @brief 将 AST 递归翻译成带显式后继的指令。 */
class VmCompiler
{
public:
    RegexVmProgram program; ///< 编译中的程序
    /** @brief 添加指令。 @param op 操作码。 @param next 后继。 @return 指令地址。 */
    int emit(int op, int next = 0)
    {
        RegexVmInstruction instruction;
        instruction.op = op;
        instruction.next = next;
        program.code.push_back(instruction);
        return static_cast<int>(program.code.size()) - 1;
    }
    /** @brief 编译子树。 @param node 子树。 @param next 后继。 @return 入口地址。 */
    int build(const std::shared_ptr<RegexNode> &node, int next)
    {
        using Kind = RegexNode::Kind;
        switch (node->kind)
        {
        case Kind::Empty: return next;
        case Kind::Class:
        {
            const int pc = emit(1, next);
            if (program.unicode)
            {
                program.code[pc].rangeOffset = static_cast<int>(program.ranges.size());
                program.code[pc].rangeCount = static_cast<int>(node->ranges.size());
                for (const auto &range : node->ranges)
                {
                    program.ranges.push_back(static_cast<uint32_t>(range.first));
                    program.ranges.push_back(static_cast<uint32_t>(range.second));
                }
                return pc;
            }
            for (unsigned byte = 0; byte < 256; ++byte)
                for (const auto &range : node->ranges)
                    if ((byte >= range.first) && (byte <= range.second))
                        program.code[pc].bits[byte >> 3] |= 1u << (byte & 7);
            return pc;
        }
        case Kind::Concat: return build(node->left, build(node->right, next));
        case Kind::Alt:
        {
            const int pc = emit(2);
            const int left = build(node->left, next);
            const int right = build(node->right, next);
            program.code[pc].next = left;
            program.code[pc].other = right;
            return pc;
        }
        case Kind::Group:
        {
            program.groups = std::max(program.groups, node->group);
            const int end = emit(3, next);
            program.code[end].value = 2 * node->group + 1;
            const int body = build(node->left, end);
            const int begin = emit(3, body);
            program.code[begin].value = 2 * node->group;
            return begin;
        }
        case Kind::Assertion:
            return emit(node->assertion == RegexNode::Assertion::LineStart ? 4 : 5, next);
        case Kind::WordBoundary:
        case Kind::NotWordBoundary:
        {
            const int pc = emit(6, next);
            program.code[pc].value = node->kind == Kind::NotWordBoundary;
            return pc;
        }
        case Kind::Backref:
        {
            const int pc = emit(7, next);
            program.code[pc].value = node->group;
            program.code[pc].other = node->ignoreCase;
            return pc;
        }
        case Kind::LookAhead:
        case Kind::NegativeLookAhead:
        {
            const int pc = emit(8, next);
            const int body = build(node->left, 0);
            program.code[pc].other = body;
            program.code[pc].value = node->kind == Kind::NegativeLookAhead;
            return pc;
        }
        default:
        {
            const int minimum = node->kind == Kind::Plus ? 1 :
                (node->kind == Kind::Repeat ? node->min : 0);
            const int maximum = node->kind == Kind::Opt ? 1 :
                (node->kind == Kind::Repeat ? node->max : -1);
            int tail = next;
            if (maximum < 0)
            {
                const int split = emit(2);
                const int guard = emit(9);
                const int body = build(node->left, split);
                program.code[guard].next = body;
                program.code[split].next = node->lazy ? tail : guard;
                program.code[split].other = node->lazy ? guard : tail;
                tail = split;
            }
            else
                for (int index = minimum; index < maximum; ++index)
                {
                    const int body = build(node->left, tail);
                    const int split = emit(2, node->lazy ? tail : body);
                    program.code[split].other = node->lazy ? body : tail;
                    tail = split;
                }
            for (int index = 0; index < minimum; ++index)
                tail = build(node->left, tail);
            return tail;
        }
        }
    }
};
}
/** @brief 识别模式开头的 u 标志。 @param pattern 模式。 @return Unicode 模式返回真。 */
bool RegexVm::unicodeMode(const std::string &pattern)
{
    bool unicode = false;
    std::size_t pos = 0;
    while (pattern.compare(pos, 2, "(?") == 0)
    {
        std::size_t end = pos + 2;
        bool found = false;
        while (end < pattern.size() && (pattern[end] == 'i' || pattern[end] == 'u'))
        {
            if (pattern[end] == 'u') found = true;
            ++end;
        }
        if (end == pos + 2 || end >= pattern.size() || pattern[end] != ')') break;
        unicode = unicode || found;
        pos = end + 1;
    }
    return unicode;
}
/** @brief 检查 DFA 无法表达的上下文节点。 @param node AST 根。 @return 需要上下文返回真。 */
bool RegexVm::contextRequired(const std::shared_ptr<RegexNode> &node)
{
    if (!node) return false;
    using Kind = RegexNode::Kind;
    return node->kind == Kind::Assertion || node->kind == Kind::LookAhead ||
        node->kind == Kind::NegativeLookAhead || node->kind == Kind::WordBoundary ||
        node->kind == Kind::NotWordBoundary || node->kind == Kind::Backref ||
        contextRequired(node->left) || contextRequired(node->right);
}
/** @brief 检查有序运行时需求。 @param node AST 根。 @return 需要运行时返回真。 */
bool RegexVm::required(const std::shared_ptr<RegexNode> &node)
{
    if (!node) return false;
    using Kind = RegexNode::Kind;
    return node->unicode || node->ignoreCase || node->lazy || node->kind == Kind::Assertion ||
        node->kind == Kind::LookAhead || node->kind == Kind::NegativeLookAhead ||
        node->kind == Kind::WordBoundary || node->kind == Kind::NotWordBoundary ||
        node->kind == Kind::Backref || required(node->left) || required(node->right);
}
namespace
{
/** @brief 收集捕获组的表达式。 @param node AST。 @param groups 组编号映射。 */
void collectGroups(const std::shared_ptr<RegexNode> &node,
                   std::map<int, std::shared_ptr<RegexNode>> &groups)
{
    if (!node) return;
    if (node->kind == RegexNode::Kind::Group) groups[node->group] = node->left;
    collectGroups(node->left, groups);
    collectGroups(node->right, groups);
}
/** @brief 考虑捕获内容最小长度的零宽检查。
 * @param node AST。 @param groups 捕获定义。 @param active 防止自引用循环。
 * @return 成功匹配可能不消耗字符时返回 true。
 */
bool nullableNode(const std::shared_ptr<RegexNode> &node,
                  const std::map<int, std::shared_ptr<RegexNode>> &groups,
                  std::set<int> &active)
{
    using Kind = RegexNode::Kind;
    switch (node->kind)
    {
    case Kind::Class: return false;
    case Kind::Concat: return nullableNode(node->left, groups, active) && nullableNode(node->right, groups, active);
    case Kind::Alt: return nullableNode(node->left, groups, active) || nullableNode(node->right, groups, active);
    case Kind::Group: case Kind::Plus: return nullableNode(node->left, groups, active);
    case Kind::Repeat: return node->min == 0 || nullableNode(node->left, groups, active);
    case Kind::Backref:
    {
        const auto found = groups.find(node->group);
        if (found == groups.end() || !active.insert(node->group).second) return true;
        const bool result = nullableNode(found->second, groups, active);
        active.erase(node->group);
        return result;
    }
    default: return true;
    }
}
}
/** @brief 检查规则是否可能不消耗输入。 @param node AST 根。 @return 可能零宽返回真。 */
bool RegexVm::nullable(const std::shared_ptr<RegexNode> &node)
{
    std::map<int, std::shared_ptr<RegexNode>> groups;
    std::set<int> active;
    collectGroups(node, groups);
    return nullableNode(node, groups, active);
}
/** @brief 编译字节或 Unicode 码点字符域的有序正则程序。
 * @param pattern 原正则。 @param out 输出程序。 @param error 解析错误。
 * @return 编译成功返回 true。
 */
bool RegexVm::compile(const std::string &pattern, RegexVmProgram &out, Error &error)
{
    std::shared_ptr<RegexNode> ast;
    const bool unicode = unicodeMode(pattern);
    std::string bytePattern;
    for (const unsigned char byte : pattern)
    {
        if (unicode || byte < 128u) bytePattern.push_back(static_cast<char>(byte));
        else
        {
            bytePattern.push_back(static_cast<char>(0xC0u | (byte >> 6u)));
            bytePattern.push_back(static_cast<char>(0x80u | (byte & 0x3Fu)));
        }
    }
    if (!RegexDfa::parse(bytePattern, ast, error)) return false;
    VmCompiler compiler;
    compiler.program.unicode = unicode;
    compiler.emit(0);
    compiler.program.entry = compiler.build(ast, 0);
    out = std::move(compiler.program);
    return true;
}
}
