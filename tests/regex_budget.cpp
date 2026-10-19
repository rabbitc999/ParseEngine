/** @file regex_budget.cpp
 * @brief 验证正则预算、取消、分配失败及所有退出路径的工作内存释放。
 */
#include "ParseEngine/regex_vm.hpp"
#include <cassert>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <vector>

static size_t allocationCalls, allocationsLive;
static size_t failAt = std::numeric_limits<size_t>::max();
/** @brief 注入可定位的分配失败。@param bytes 字节数。@return 内存或空指针。 */
static void *testAllocate(size_t bytes)
{
    if (allocationCalls++ == failAt) return nullptr;
    void *pointer = std::malloc(bytes);
    if (pointer) ++allocationsLive;
    return pointer;
}
/** @brief 注入扩容失败。@param pointer 原内存。@param bytes 字节数。@return 新内存。 */
static void *testReallocate(void *pointer, size_t bytes)
{
    if (allocationCalls++ == failAt) return nullptr;
    return std::realloc(pointer, bytes);
}
/** @brief 统计释放。@param pointer 分配的内存。 */
static void testFree(void *pointer)
{
    assert(pointer && allocationsLive);
    --allocationsLive;
    std::free(pointer);
}
#define PE_REGEX_MALLOC testAllocate
#define PE_REGEX_REALLOC testReallocate
#define PE_REGEX_FREE testFree
#include "../source/templates/regex_unicode_data.h.tmpl"
#include "../source/templates/regex_vm_runtime.h.tmpl"

/** @brief 持有编译后的指令，确保描述符指针有效。 */
struct Program
{
    ParseEngine::RegexVmProgram compiled;
    std::vector<PeRegexInst> code;
    PeRegexProgram program{};

    /** @brief 编译测试规则。@param pattern 模式。 */
    explicit Program(const std::string &pattern)
    {
        ParseEngine::Error error;
        assert(ParseEngine::RegexVm::compile(pattern, compiled, error));
        for (const auto &instruction : compiled.code) {
            PeRegexInst item{};
            item.op = instruction.op; item.next = instruction.next;
            item.other = instruction.other; item.value = instruction.value;
            item.rangeOffset = instruction.rangeOffset; item.rangeCount = instruction.rangeCount;
            for (unsigned index = 0; index < 32; ++index)
                item.bits[index] = static_cast<unsigned char>(instruction.bits[index]);
            code.push_back(item);
        }
        program = {code.data(), code.size(), static_cast<size_t>(compiled.entry),
            static_cast<size_t>(compiled.groups), compiled.ranges.data(), compiled.unicode};
    }

    /** @brief 匹配并检查资源释放。@param input 输入。@param limits 预算。@return 结果。 */
    PeRegexResult match(const std::string &input, PeRegexLimits limits = {})
    {
        allocationCalls = 0;
        const auto result = PeRegexMatchEx(&program, input.data(), input.size(), &limits);
        assert(allocationsLive == 0);
        assert(result.peakMemory <= (limits.maxMemory ? limits.maxMemory : 64u * 1024u * 1024u));
        return result;
    }
};

/** @brief 根据调用方计数决定取消。@param data 剩余检查次数。@return 是否取消。 */
static int cancelAfter(void *data)
{
    auto &remaining = *static_cast<size_t *>(data);
    if (!remaining) return 1;
    --remaining;
    return 0;
}

/** @brief 覆盖所有预算维度、前瞻共享计数和内存故障。@return 成功返回零。 */
int main()
{
    Program simple("a"), empty("a*"), branching("(a|aa)+b");
    assert(simple.match("a").status == PE_REGEX_MATCH);
    assert(simple.match("z").status == PE_REGEX_NO_MATCH);
    const auto zero = empty.match("");
    assert(zero.status == PE_REGEX_MATCH && zero.byteLength == 0);
    PeRegexLimits limits{};
    limits.maxSteps = 200;
    const auto budget = branching.match(std::string(25, 'a') + "c", limits);
    assert(budget.status == PE_REGEX_LIMIT && budget.steps == 200);
    limits = {}; limits.maxMemory = 1;
    assert(simple.match("a", limits).status == PE_REGEX_LIMIT);
    limits = {}; limits.maxStates = 1;
    assert(branching.match("aaaab", limits).status == PE_REGEX_LIMIT);
    limits = {}; limits.maxBacktracks = 1;
    Program optional("a?a?a?b");
    const auto backtracks = optional.match("ac", limits);
    assert(backtracks.status == PE_REGEX_LIMIT && backtracks.backtracks == 1);
    limits = {}; limits.maxLookaheadDepth = 1;
    Program nested("(?=(?=a))a"), negative("(?!(?=b))a");
    assert(nested.match("a", limits).status == PE_REGEX_LIMIT);
    assert(negative.match("a", limits).status == PE_REGEX_LIMIT);
    limits = {}; limits.maxSteps = 5;
    assert(nested.match("a", limits).status == PE_REGEX_LIMIT);
    assert(nested.match("a", limits).steps == 5);
    size_t remaining = 0;
    limits = {}; limits.cancel = cancelAfter; limits.userData = &remaining;
    const auto immediate = branching.match("aaaab", limits);
    assert(immediate.status == PE_REGEX_CANCELLED && immediate.steps == 0);
    remaining = 10;
    assert(branching.match("aaaab", limits).status == PE_REGEX_CANCELLED);
    Program reference("(?u)(a+)b\\1");
    const std::string repeated = std::string(100, 'a') + "b" + std::string(100, 'a');
    const auto complete = reference.match(repeated);
    assert(complete.status == PE_REGEX_MATCH && complete.byteLength == repeated.size());
    Program prefix("(?u)(a+)b");
    const auto prefixResult = prefix.match(repeated);
    assert(complete.steps >= prefixResult.steps + 100);
    limits = {}; limits.maxSteps = complete.steps - 50;
    assert(reference.match(repeated, limits).status == PE_REGEX_LIMIT);
    remaining = prefixResult.steps + 20;
    limits = {}; limits.cancel = cancelAfter; limits.userData = &remaining;
    assert(reference.match(repeated, limits).status == PE_REGEX_CANCELLED);
    Program allocations("(?=(a+))((a|aa)+)b\\1");
    const std::string input = std::string(32, 'a') + "b" + std::string(32, 'a');
    const auto normal = allocations.match(input);
    assert(normal.status == PE_REGEX_MATCH && normal.byteLength == input.size());
    const size_t calls = allocationCalls;
    assert(calls > 32);
    for (size_t index = 0; index < calls; ++index) {
        failAt = index;
        assert(allocations.match(input).status == PE_REGEX_NO_MEMORY);
    }
    failAt = std::numeric_limits<size_t>::max();
    limits = {}; limits.maxMemory = normal.peakMemory - 1;
    assert(allocations.match(input, limits).status == PE_REGEX_LIMIT);
    const size_t saved = simple.program.groups;
    simple.program.groups = std::numeric_limits<size_t>::max();
    assert(simple.match("a").status == PE_REGEX_LIMIT);
    simple.program.groups = saved;
    assert(simple.match("a").status == PE_REGEX_MATCH);
    std::cout << "正则预算、取消、嵌套前瞻及 " << calls << " 处分配失败回归通过\n";
}
