#include "embed_pool_allocator.hpp"
#include "static_heap_allocator.hpp"

#include <iostream>
#include <string>

namespace
{
int checks = 0;
int failures = 0;

#define CHECK(condition)                                                        \
    do                                                                          \
    {                                                                           \
        ++checks;                                                               \
        if (!(condition))                                                       \
        {                                                                       \
            ++failures;                                                         \
            std::cerr << "FAIL: " #condition " at line " << __LINE__ << '\n'; \
        }                                                                       \
    } while (false)

stackvm::Program assemble(const std::string &text)
{
    const stackvm::QuadFile quads = stackvm::parseQuads(text, "<embed-test>");
    stackvm::Program program;
    CHECK(stackvm::assemble(quads, program));
    return program;
}
/**
 * C 写的静态内存池（`embed_pool.c`）与它的 VM 适配器。
 *
 * 重点验收两件事：
 *   1. **停机清理**：`EmbedPool_Reset` 把块表 / 计数 / 峰值一起清空，所以同一个池
 *      （同一个 Machine）连着跑两轮，第二轮照样从头分配 —— 这是嵌入式反复 `run()` 的前提；
 *   2. 释放过的内存可以再分配出去（首次适配 + 切块 + 合并），但"释放后再读"仍被挡下。
 */
void testEmbedPool()
{
    /* ---- C 层直测：切块 / 合并 / 重复释放 / 自检 ---- */
    static std::uint8_t memory[1024];
    EmbedPool_T pool;
    const char *message = nullptr;
    CHECK(EmbedPool_Create(&pool, memory, sizeof(memory)) == EMBED_POOL_ERR_OK);
    CHECK(EmbedPool_Validate(&pool, &message) == EMBED_POOL_ERR_OK);

    void *small = EmbedPool_Alloc(&pool, 64u);
    void *middle = EmbedPool_Alloc(&pool, 128u);
    void *tiny = EmbedPool_Alloc(&pool, 32u);
    CHECK(small != nullptr && middle != nullptr && tiny != nullptr);
    CHECK(EmbedPool_LiveBytes(&pool) == 224u);
    CHECK(EmbedPool_LiveBlocks(&pool) == 3u);
    CHECK(EmbedPool_BlockCount(&pool) == 4u); /* 3 个占用 + 1 个空闲尾巴 */

    CHECK(EmbedPool_Free(&pool, middle) == EMBED_POOL_ERR_OK);
    CHECK(EmbedPool_Free(&pool, middle) == EMBED_POOL_ERR_INVALID_POINTER); /* 重复释放 */
    CHECK(EmbedPool_Free(&pool, small) == EMBED_POOL_ERR_OK);
    CHECK(EmbedPool_Free(&pool, tiny) == EMBED_POOL_ERR_OK);
    CHECK(EmbedPool_LiveBytes(&pool) == 0u);
    CHECK(EmbedPool_BlockCount(&pool) == 1u); /* 全部并回一整块 */
    CHECK(EmbedPool_Validate(&pool, &message) == EMBED_POOL_ERR_OK);

    void *whole = EmbedPool_Alloc(&pool, sizeof(memory)); /* 合并生效才拿得到整块 */
    CHECK(whole == memory);
    CHECK(EmbedPool_BlockCount(&pool) == 1u);
    CHECK(EmbedPool_Alloc(&pool, 8u) == nullptr); /* 池空了就不能再给 */
    CHECK(EmbedPool_Free(&pool, whole) == EMBED_POOL_ERR_OK);

    CHECK(EmbedPool_Reset(&pool) == EMBED_POOL_ERR_OK);
    CHECK(EmbedPool_LiveBytes(&pool) == 0u);
    CHECK(EmbedPool_PeakBytes(&pool) == 0u); /* 峰值也归零 */
    CHECK(EmbedPool_BlockCount(&pool) == 1u);

    /* ---- 适配器 + Machine：同一个池连着跑两轮 ---- */
    embed_stackvm::EmbedPoolAllocator<256u> heap;
    stackvm::Machine machine;
    machine.setHeapAllocator(&heap);
    const stackvm::Program program = assemble(
        "0: (decl, i64, global, a)\n"
        "1: (alloc, 24, _, a)\n"
        "2: (store:i32, a, 5, _)\n"
        "3: (load:i32, a, _, value)\n"
        "4: (write, value, _, _)\n"
        "5: (free, a, _, _)\n"
        "6: (halt, _, _, _)\n");
    for (int round = 0; round < 2; ++round)
    {
        const stackvm::RunResult result = machine.run(program);
        CHECK(result.ok);
        CHECK(result.output == "5");
        CHECK(result.heapLiveBytes == 0u);
        CHECK(result.heapLiveBlocks == 0u);
        CHECK(result.heapPeakBytes == 24u);              /* 每轮都从头来，峰值相同 */
        CHECK(heap.blockCount() == 1u);                  /* 停机后块表已收回一整块 */
        CHECK(heap.validate(&message) == EMBED_POOL_ERR_OK);
    }

    /* 释放之后再读：适配器只认活跃块，所以会被挡下（use-after-free 保护）。 */
    heap.reset(8u);
    const std::size_t address = heap.allocate(16u);
    CHECK(address == 8u);
    CHECK(heap.release(address));
    char scratch = 0;
    CHECK(!heap.readBytes(address, &scratch, 1u));
    std::size_t capacity = 0u;
    CHECK(!heap.capacityAt(address, capacity));
    CHECK(heap.contains(address)); /* 但仍算"堆里的地址"，错误分类才说得通 */

    /* 池装不下：VM 把它变成正常的"堆内存不足"诊断，而不是崩掉。 */
    const stackvm::Program exhausted = assemble(
        "0: (decl, i64, global, a)\n"
        "1: (alloc, 512, _, a)\n"
        "2: (halt, _, _, _)\n");
    const stackvm::RunResult failed = machine.run(exhausted);
    CHECK(!failed.ok);
    CHECK(failed.error.find("堆内存不足") != std::string::npos);
}
} // namespace

int main()
{
    embed_stackvm::StaticHeapAllocator<32u, 8u> heap;

    heap.reset(8u);
    const std::size_t first = heap.allocate(8u);
    std::int64_t stored = 0;
    CHECK(first == 8u);
    CHECK(heap.writeInt(first, 123));
    CHECK(heap.readInt(first, stored));
    CHECK(stored == 123);
    std::string releaseError;
    CHECK(heap.release(first, &releaseError));
    CHECK(!heap.readInt(first, stored));
    CHECK(!heap.release(first, &releaseError));
    CHECK(releaseError.find("free 过了") != std::string::npos);

    heap.reset(8u);
    const std::size_t left = heap.allocate(8u);
    const std::size_t right = heap.allocate(8u);
    CHECK(left == 8u);
    CHECK(right == 16u);
    CHECK(heap.release(left));
    CHECK(heap.release(right));
    CHECK(heap.allocate(16u) == 24u);
    CHECK(!heap.readInt(left, stored));

    stackvm::Machine machine;
    machine.setHeapAllocator(&heap);

    const stackvm::Program reuse = assemble(
        "0: (decl, i64, global, a)\n"
        "1: (decl, i64, global, b)\n"
        "2: (alloc, 16, _, a)\n"
        "3: (store:i32, a, 77, _)\n"
        "4: (load:i32, a, _, value)\n"
        "5: (write, value, _, _)\n"
        "6: (free, a, _, _)\n"
        "7: (alloc, 16, _, b)\n"
        "8: (==:i64, a, b, same)\n"
        "9: (write, same, _, _)\n"
        "10: (free, b, _, _)\n"
        "11: (halt, _, _, _)\n");
    const stackvm::RunResult reused = machine.run(reuse);
    CHECK(reused.ok);
    CHECK(reused.output == "77 0");
    CHECK(reused.heapPeakBytes == 16u);
    CHECK(reused.heapLiveBytes == 0u);
    CHECK(reused.heapLiveBlocks == 0u);
    CHECK(heap.committedBytes() == 32u);

    stackvm::NativeRegistry registry;
    CHECK(registry.add(
        "cpp_text", 0u,
        [](stackvm::NativeCallContext &context, const std::vector<stackvm::NativeValue> &) {
            return stackvm::NativeValue::fromInt(context.makeString("hello"));
        }));
    machine.setNativeRegistry(&registry);
    const stackvm::Program nativeString = assemble(
        "0: (call, cpp_text, _, text)\n"
        "1: (param, text, _, value)\n"
        "2: (call, __vm_str_len, _, length)\n"
        "3: (write, length, _, _)\n"
        "4: (free, text, _, _)\n"
        "5: (halt, _, _, _)\n");
    const stackvm::RunResult nativeResult = machine.run(nativeString);
    CHECK(nativeResult.ok);
    CHECK(nativeResult.output == "5");
    CHECK(nativeResult.heapPeakBytes >= 6u);
    CHECK(nativeResult.heapLiveBytes == 0u);

    const stackvm::Program exhausted = assemble(
        "0: (decl, i64, global, a)\n"
        "1: (decl, i64, global, b)\n"
        "2: (alloc, 24, _, a)\n"
        "3: (alloc, 16, _, b)\n"
        "4: (halt, _, _, _)\n");
    const stackvm::RunResult failed = machine.run(exhausted);
    CHECK(!failed.ok);
    CHECK(failed.error.find("堆内存不足") != std::string::npos);
    CHECK(failed.heapPeakBytes == 24u);
    CHECK(failed.heapLiveBytes == 24u);

    /* 第二次 run 必须 reset，不能继承上一次的活块和峰值。 */
    const stackvm::Program empty = assemble("0: (halt, _, _, _)\n");
    const stackvm::RunResult reset = machine.run(empty);
    CHECK(reset.ok);
    CHECK(reset.heapPeakBytes == 0u);
    CHECK(reset.heapLiveBytes == 0u);
    CHECK(heap.committedBytes() == 0u);

    testEmbedPool();

    std::cout << "embed_stackvm: " << checks << " checks, " << failures
              << " failures\n";
    return failures == 0 ? 0 : 1;
}
