#ifndef PARSEENGINE_MEMORY_POOL_H
#define PARSEENGINE_MEMORY_POOL_H

#include <stddef.h>
#include <stdint.h>

/* 分配器类型宏：在**编译期**选择内存池的底层实现（用 -DMEMORY_ALLOCATOR_TYPE=… 覆盖；
 * 缺省链表式）。两种实现共用同一份 MemoryPool 句柄、MemoryPoolStats 与 memoryPool* / memoryHeap* 接口，
 * 所以上层（containerAllocator.hpp 的 arena、容器、导出 exe）不需要任何改动。
 *
 *   MEMORY_ALLOCATOR_LINKED = 1（缺省）
 *       边界标记 + 显式空闲**双向链表**：分配用首次适配，切块后维护相邻块信息，
 *       释放时合并相邻空闲块。**释放过的地址可以再分配出去**，长循环里的
 *       “分配—释放—再分配”不会持续吃内存。
 *   MEMORY_ALLOCATOR_LINEAR = 2
 *       线性（bump）分配：只向后切块，释放只把块记成墓碑，地址到 reset 前**不复用**。
 *       适合“地址一旦给出就不再复用”的护栏场景，代价是长循环占用只增不减。 */
#define MEMORY_ALLOCATOR_LINKED 1
#define MEMORY_ALLOCATOR_LINEAR 2
#ifndef MEMORY_ALLOCATOR_TYPE
#define MEMORY_ALLOCATOR_TYPE MEMORY_ALLOCATOR_LINKED
#endif
#if (MEMORY_ALLOCATOR_TYPE != MEMORY_ALLOCATOR_LINKED) && (MEMORY_ALLOCATOR_TYPE != MEMORY_ALLOCATOR_LINEAR)
#error "MEMORY_ALLOCATOR_TYPE 只能是 MEMORY_ALLOCATOR_LINKED 或 MEMORY_ALLOCATOR_LINEAR"
#endif

/* C99, caller-owned storage; no dynamic metadata or fixed block-count limit.
 * A pool is single-threaded: it carries no lock. The caller must serialize concurrent access
 * (缺省 C++ 侧由 containerAllocator.hpp 的 arena 持 std::mutex 保证，见 BufferRuntimeArena /
 * StaticRuntimeArena)。
 * Functions have internal linkage so exported C++ programs remain one file. */
/** @brief 池统计；字节数包含块头、对齐与块内碎片。 */
typedef struct MemoryPoolStats {
    size_t liveBytes, liveBlocks, peakBytes, totalBlocks;
} MemoryPoolStats;
/** @brief 调用者拥有的单线程分配池句柄。 */
typedef struct MemoryPool {
    void *rawStorage;
    size_t rawCapacity;
    unsigned char *storage;
    size_t capacity;
    size_t freeHead;
    MemoryPoolStats stats;
} MemoryPool;

/** @brief 初始化调用者拥有的分配池。 @param pool 池句柄。 @param storage 缓冲区。 @param capacity 字节数。 @return 成功返回非零。 */
static inline int memoryPoolInit(MemoryPool *pool, void *storage, size_t capacity);
/** @brief 按指定对齐分配内存。 @param pool 池句柄。 @param bytes 字节数。 @param alignment 二的幂次对齐量。 @return 地址或 NULL。 */
static inline void *memoryPoolAllocate(MemoryPool *pool, size_t bytes, size_t alignment);
/** @brief 释放池内块，链表模式合并相邻空闲块。 @param pool 池句柄。 @param pointer 待释放块。 @return 成功返回非零。 */
static inline int memoryPoolRelease(MemoryPool *pool, void *pointer);
/** @brief 判断地址是否在池的存储范围内，不代表地址为有效分配起点。 @param pool 池句柄。 @param pointer 待检查地址。 @return 范围内返回非零。 */
static inline int memoryPoolOwns(const MemoryPool *pool, const void *pointer);
/** @brief 调整块大小，失败保留旧块；零大小释放旧块。 @param pool 池句柄。 @param pointer 旧块或 NULL。 @param bytes 新容量。 @param alignment 新块对齐量。 @return 新地址或 NULL。 */
static inline void *memoryPoolReallocate(MemoryPool *pool, void *pointer, size_t bytes, size_t alignment);
/** @brief 无活动分配时保存统计并清零整个输入缓冲区。 @param pool 池句柄。 @param stats 可选统计输出。 @return 成功返回非零。 */
static inline int memoryPoolFinish(MemoryPool *pool, MemoryPoolStats *stats);
/** @brief 使用宿主堆进行对齐分配。 @param bytes 字节数。 @param alignment 对齐量。 @return 地址或 NULL。 */
static inline void *memoryHeapAllocate(size_t bytes, size_t alignment);
/** @brief 释放宿主堆对齐分配。 @param pointer 已分配地址或 NULL。 */
static inline void memoryHeapRelease(void *pointer);

#include "pool.c"
#endif
