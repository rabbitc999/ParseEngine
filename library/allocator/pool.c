#ifndef PARSEENGINE_MEMORY_POOL_IMPLEMENTATION
#define PARSEENGINE_MEMORY_POOL_IMPLEMENTATION
#include "pool.h"
#include <stdlib.h>
#include <string.h>

typedef struct MemoryPoolBlock {
    size_t size, previousSize, payload, requested, nextFree, previousFree;
} MemoryPoolBlock;
#define MEMORY_POOL_ALIGNMENT ((size_t)sizeof(void *))
#define MEMORY_POOL_NONE SIZE_MAX
static inline int memoryPoolValidAlignment(size_t a) { return a && !(a & (a - 1)); }
static inline MemoryPoolBlock memoryPoolBlock(const MemoryPool *p, size_t at) {
    MemoryPoolBlock b; memcpy(&b, p->storage + at, sizeof(b)); return b;
}
static inline void memoryPoolStore(MemoryPool *p, size_t at, MemoryPoolBlock b) { memcpy(p->storage + at, &b, sizeof(b)); }
#if MEMORY_ALLOCATOR_TYPE == MEMORY_ALLOCATOR_LINKED
static inline void memoryPoolRemoveFree(MemoryPool *p, size_t at, MemoryPoolBlock b) {
    if (b.previousFree == MEMORY_POOL_NONE) p->freeHead = b.nextFree;
    else { MemoryPoolBlock prev = memoryPoolBlock(p, b.previousFree); prev.nextFree = b.nextFree; memoryPoolStore(p, b.previousFree, prev); }
    if (b.nextFree != MEMORY_POOL_NONE) { MemoryPoolBlock next = memoryPoolBlock(p, b.nextFree); next.previousFree = b.previousFree; memoryPoolStore(p, b.nextFree, next); }
    (void)at;
}
static inline void memoryPoolAddFree(MemoryPool *p, size_t at, MemoryPoolBlock b) {
    b.previousFree = MEMORY_POOL_NONE; b.nextFree = p->freeHead;
    if (p->freeHead != MEMORY_POOL_NONE) { MemoryPoolBlock next = memoryPoolBlock(p, p->freeHead); next.previousFree = at; memoryPoolStore(p, p->freeHead, next); }
    p->freeHead = at; memoryPoolStore(p, at, b);
}
#endif
static inline int memoryPoolInit(MemoryPool *p, void *storage, size_t capacity) {
    uintptr_t start, aligned; size_t skipped, rawCapacity = capacity; MemoryPoolBlock b;
    if (!p) return 0;
    memset(p, 0, sizeof(*p)); p->freeHead = MEMORY_POOL_NONE;
    if (!storage) return 0;
    start = (uintptr_t)storage;
    if (start > UINTPTR_MAX - (MEMORY_POOL_ALIGNMENT - 1)) return 0;
    aligned = (start + MEMORY_POOL_ALIGNMENT - 1) & ~(uintptr_t)(MEMORY_POOL_ALIGNMENT - 1);
    skipped = (size_t)(aligned - start);
    if (capacity < skipped) return 0;
    capacity = (capacity - skipped) & ~(MEMORY_POOL_ALIGNMENT - 1);
    if (capacity < sizeof(b) + sizeof(size_t) + MEMORY_POOL_ALIGNMENT) return 0;
    p->storage = (unsigned char *)storage + skipped; p->capacity = capacity;
    p->rawStorage = storage; p->rawCapacity = rawCapacity;
#if MEMORY_ALLOCATOR_TYPE == MEMORY_ALLOCATOR_LINKED
    memset(&b, 0, sizeof(b)); b.size = capacity;
    memoryPoolAddFree(p, 0, b);
#else
    (void)b;
    p->freeHead = 0; /* 线性实现：freeHead 当 bump 游标 */
#endif
    return 1;
}
static inline int memoryPoolOwns(const MemoryPool *p, const void *pointer) {
    uintptr_t address = (uintptr_t)pointer, start;
    if (!p || !p->storage || !pointer) return 0;
    start = (uintptr_t)p->storage;
    return address >= start && address - start < p->capacity;
}
#if MEMORY_ALLOCATOR_TYPE == MEMORY_ALLOCATOR_LINKED
static inline void *memoryPoolAllocate(MemoryPool *p, size_t bytes, size_t alignment) {
    size_t at;
    if (!p || !p->storage || !memoryPoolValidAlignment(alignment)) return NULL;
    if (!bytes) bytes = 1;
    for (at = p->freeHead; at != MEMORY_POOL_NONE;) {
        MemoryPoolBlock b = memoryPoolBlock(p, at);
        uintptr_t base = (uintptr_t)(p->storage + at), address;
        size_t payload, wanted;
        if (base > UINTPTR_MAX - sizeof(b) - sizeof(size_t) ||
            base + sizeof(b) + sizeof(size_t) > UINTPTR_MAX - (alignment - 1)) return NULL;
        address = (base + sizeof(b) + sizeof(size_t) + alignment - 1) & ~(uintptr_t)(alignment - 1);
        payload = (size_t)(address - base);
        if (payload > SIZE_MAX - bytes || payload + bytes > SIZE_MAX - (MEMORY_POOL_ALIGNMENT - 1)) return NULL;
        wanted = (payload + bytes + MEMORY_POOL_ALIGNMENT - 1) & ~(MEMORY_POOL_ALIGNMENT - 1);
        if (wanted <= b.size) {
            memoryPoolRemoveFree(p, at, b);
            if (b.size - wanted >= sizeof(b) + sizeof(size_t) + MEMORY_POOL_ALIGNMENT) {
                MemoryPoolBlock rest; size_t next;
                memset(&rest, 0, sizeof(rest)); rest.size = b.size - wanted; rest.previousSize = wanted;
                b.size = wanted; memoryPoolAddFree(p, at + wanted, rest);
                next = at + wanted + rest.size;
                if (next < p->capacity) { MemoryPoolBlock n = memoryPoolBlock(p, next); n.previousSize = rest.size; memoryPoolStore(p, next, n); }
            }
            b.payload = payload; b.requested = bytes; memoryPoolStore(p, at, b);
            memcpy((void *)(address - sizeof(size_t)), &at, sizeof(at));
            p->stats.liveBytes += b.size; ++p->stats.liveBlocks; ++p->stats.totalBlocks;
            if (p->stats.liveBytes > p->stats.peakBytes) p->stats.peakBytes = p->stats.liveBytes;
            return (void *)address;
        }
        at = b.nextFree;
    }
    return NULL;
}
static inline int memoryPoolRelease(MemoryPool *p, void *pointer) {
    size_t at, next; MemoryPoolBlock b;
    if (!memoryPoolOwns(p, pointer) || (uintptr_t)pointer - (uintptr_t)p->storage < sizeof(b) + sizeof(size_t)) return 0;
    memcpy(&at, (unsigned char *)pointer - sizeof(size_t), sizeof(at));
    if (at % MEMORY_POOL_ALIGNMENT || at > p->capacity - sizeof(b)) return 0;
    b = memoryPoolBlock(p, at);
    if (!b.payload || b.size < sizeof(b) + sizeof(size_t) || b.size > p->capacity - at ||
        b.payload >= b.size || p->storage + at + b.payload != pointer) return 0;
    p->stats.liveBytes -= b.size; --p->stats.liveBlocks;
    b.payload = b.requested = 0;
    memoryPoolStore(p, at, b);
    next = at + b.size;
    if (next < p->capacity) {
        MemoryPoolBlock n = memoryPoolBlock(p, next);
        if (!n.payload) { memoryPoolRemoveFree(p, next, n); b.size += n.size; }
    }
    if (b.previousSize && b.previousSize <= at) {
        size_t prevAt = at - b.previousSize;
        MemoryPoolBlock prev = memoryPoolBlock(p, prevAt);
        if (!prev.payload) { memoryPoolRemoveFree(p, prevAt, prev); prev.size += b.size; b = prev; at = prevAt; }
    }
    next = at + b.size;
    if (next < p->capacity) { MemoryPoolBlock n = memoryPoolBlock(p, next); n.previousSize = b.size; memoryPoolStore(p, next, n); }
    memoryPoolAddFree(p, at, b); return 1;
}
#else /* MEMORY_ALLOCATOR_TYPE == MEMORY_ALLOCATOR_LINEAR */
static inline void *memoryPoolAllocate(MemoryPool *p, size_t bytes, size_t alignment) {
    size_t at, avail, wanted; uintptr_t base, address; size_t payload; MemoryPoolBlock b;
    if (!p || !p->storage || !memoryPoolValidAlignment(alignment)) return NULL;
    if (!bytes) bytes = 1;
    at = p->freeHead; /* 线性实现：freeHead 当 bump 游标（已对齐到 MEMORY_POOL_ALIGNMENT） */
    if (at > p->capacity) return NULL;
    avail = p->capacity - at;
    if (sizeof(b) + sizeof(size_t) > avail) return NULL;
    base = (uintptr_t)(p->storage + at);
    if (base > UINTPTR_MAX - sizeof(b) - sizeof(size_t) - (alignment - 1)) return NULL;
    address = (base + sizeof(b) + sizeof(size_t) + alignment - 1) & ~(uintptr_t)(alignment - 1);
    payload = (size_t)(address - base);
    if (payload > avail || bytes > avail - payload) return NULL;
    if (payload + bytes > SIZE_MAX - (MEMORY_POOL_ALIGNMENT - 1)) return NULL;
    wanted = (payload + bytes + MEMORY_POOL_ALIGNMENT - 1) & ~(MEMORY_POOL_ALIGNMENT - 1);
    if (wanted > avail) return NULL;
    memset(&b, 0, sizeof(b));
    b.size = wanted; b.previousSize = at; b.payload = payload; b.requested = bytes;
    memoryPoolStore(p, at, b);
    memcpy((void *)(address - sizeof(size_t)), &at, sizeof(at));
    p->freeHead = at + wanted;
    p->stats.liveBytes += b.size; ++p->stats.liveBlocks; ++p->stats.totalBlocks;
    if (p->stats.liveBytes > p->stats.peakBytes) p->stats.peakBytes = p->stats.liveBytes;
    return (void *)address;
}
static inline int memoryPoolRelease(MemoryPool *p, void *pointer) {
    size_t at; MemoryPoolBlock b;
    if (!memoryPoolOwns(p, pointer) || (uintptr_t)pointer - (uintptr_t)p->storage < sizeof(b) + sizeof(size_t)) return 0;
    memcpy(&at, (unsigned char *)pointer - sizeof(size_t), sizeof(at));
    if (at % MEMORY_POOL_ALIGNMENT || at > p->capacity - sizeof(b)) return 0;
    b = memoryPoolBlock(p, at);
    if (!b.payload || b.size < sizeof(b) + sizeof(size_t) || b.size > p->capacity - at ||
        b.payload >= b.size || p->storage + at + b.payload != pointer) return 0;
    p->stats.liveBytes -= b.size; --p->stats.liveBlocks;
    /* 墓碑：地址保留记录但不再复用，直到 reset / 整池清零。 */
    b.payload = b.requested = 0;
    memoryPoolStore(p, at, b);
    return 1;
}
#endif
/** @brief 调整分配并在成功复制后释放旧块。
 * @param p 池句柄。 @param pointer 旧地址。 @param bytes 新字节数。 @param alignment 对齐量。 @return 新地址或 NULL。 */
static inline void *memoryPoolReallocate(MemoryPool *p, void *pointer, size_t bytes, size_t alignment) {
    size_t at; MemoryPoolBlock block; void *result;
    if (!pointer) return memoryPoolAllocate(p, bytes, alignment);
    if (!memoryPoolOwns(p, pointer) || (uintptr_t)pointer - (uintptr_t)p->storage < sizeof(block) + sizeof(size_t)) return NULL;
    memcpy(&at, (unsigned char *)pointer - sizeof(size_t), sizeof(at));
    if (at % MEMORY_POOL_ALIGNMENT || at > p->capacity - sizeof(block)) return NULL;
    block = memoryPoolBlock(p, at);
    if (!block.payload || block.size < sizeof(block) + sizeof(size_t) || block.size > p->capacity - at ||
        block.payload >= block.size || block.requested > block.size - block.payload || p->storage + at + block.payload != pointer) return NULL;
    if (!bytes) { memoryPoolRelease(p, pointer); return NULL; }
    result = memoryPoolAllocate(p, bytes, alignment);
    if (!result) return NULL;
    memcpy(result, pointer, bytes < block.requested ? bytes : block.requested);
    memoryPoolRelease(p, pointer);
    return result;
}
static inline int memoryPoolFinish(MemoryPool *p, MemoryPoolStats *stats) {
    size_t i;
    volatile unsigned char *buffer;
    if (!p || !p->storage || p->stats.liveBlocks) return 0;
    if (stats) *stats = p->stats;
    buffer = (volatile unsigned char *)p->rawStorage;
    for (i = 0; i < p->rawCapacity; ++i) buffer[i] = 0;
    memset(p, 0, sizeof(*p)); p->freeHead = MEMORY_POOL_NONE;
    return 1;
}
static inline void *memoryHeapAllocate(size_t bytes, size_t alignment) {
    void *raw; uintptr_t address;
    if (!memoryPoolValidAlignment(alignment)) return NULL;
    if (!bytes) bytes = 1;
    if (bytes > SIZE_MAX - sizeof(void *) || bytes + sizeof(void *) > SIZE_MAX - (alignment - 1)) return NULL;
    raw = malloc(bytes + sizeof(void *) + alignment - 1);
    if (!raw) return NULL;
    address = ((uintptr_t)raw + sizeof(void *) + alignment - 1) & ~(uintptr_t)(alignment - 1);
    memcpy((void *)(address - sizeof(void *)), &raw, sizeof(raw)); return (void *)address;
}
static inline void memoryHeapRelease(void *pointer) {
    void *raw;
    if (!pointer) return;
    memcpy(&raw, (unsigned char *)pointer - sizeof(void *), sizeof(raw)); free(raw);
}
#endif
