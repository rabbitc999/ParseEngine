/**
 * @file    embed_pool.c
 * @brief   嵌入式静态内存池实现（C99，可被 C++ 直接编译）
 * @details 块表在池对象内部（定长数组），一份内存只被切一次：
 *          申请时首次适配 + 按需切块，释放时与相邻空闲块合并。
 *          没有任何一次 malloc —— 裸机上拿一块数组就能跑。
 */

#include "embed_pool.h"

/* ---------------- 内部小工具 ---------------- */

/**
 * @brief  地址是否按 EMBED_POOL_ALIGN_SIZE 对齐。
 */
static uint8_t EmbedPool_IsAligned(const void *pAddr)
{
    return (uint8_t)((((size_t)pAddr) & (size_t)(EMBED_POOL_ALIGN_SIZE - 1u)) == 0u);
}

/**
 * @brief  向上取整到 EMBED_POOL_ALIGN_SIZE。
 * @note   用减法判溢出：`need` 回绕说明请求大到不可能是真的。
 */
static uint32_t EmbedPool_AlignUp(uint32_t bytes, uint8_t *pOverflow)
{
    uint32_t aligned = (bytes + (EMBED_POOL_ALIGN_SIZE - 1u)) &
                       ~(uint32_t)(EMBED_POOL_ALIGN_SIZE - 1u);
    *pOverflow = (uint8_t)(aligned < bytes);
    return aligned;
}

/**
 * @brief  在 index 位置插入一条记录，后面的整体后移。
 * @retval 1-插入成功，0-块表已满
 */
static uint8_t EmbedPool_InsertAt(EmbedPool_T *pPool, uint32_t index,
                                  uint32_t offset, uint32_t size, uint8_t used)
{
    uint32_t move = 0;

    if (pPool->blockCount >= EMBED_POOL_MAX_BLOCKS) {
        return 0u;
    }
    move = pPool->blockCount;
    while (move > index) {
        pPool->blocks[move] = pPool->blocks[move - 1u];
        move--;
    }
    pPool->blocks[index].offset = offset;
    pPool->blocks[index].size = size;
    pPool->blocks[index].used = used;
    pPool->blockCount++;
    return 1u;
}

/**
 * @brief  删除 index 处的记录，后面的整体前移。
 */
static void EmbedPool_RemoveAt(EmbedPool_T *pPool, uint32_t index)
{
    uint32_t move = index;

    while (move + 1u < pPool->blockCount) {
        pPool->blocks[move] = pPool->blocks[move + 1u];
        move++;
    }
    pPool->blockCount--;
}

/**
 * @brief  把 index 处的空闲块和左右邻居合并（块表保持首尾相接）。
 */
static void EmbedPool_Coalesce(EmbedPool_T *pPool, uint32_t index)
{
    /* 先并右边：并进来一条就少一条，索引不动。 */
    while (index + 1u < pPool->blockCount &&
           pPool->blocks[index].used == 0u &&
           pPool->blocks[index + 1u].used == 0u) {
        pPool->blocks[index].size += pPool->blocks[index + 1u].size;
        EmbedPool_RemoveAt(pPool, index + 1u);
    }

    /* 再并左边：并入前一条后，当前索引消失。 */
    if (index > 0u && pPool->blocks[index].used == 0u &&
        pPool->blocks[index - 1u].used == 0u) {
        pPool->blocks[index - 1u].size += pPool->blocks[index].size;
        EmbedPool_RemoveAt(pPool, index);
    }
}

/* ---------------- 对外接口 ---------------- */

int8_t EmbedPool_Create(EmbedPool_T *pPool, void *pBuffer, uint32_t capacity)
{
    if (pPool == NULL || pBuffer == NULL) {
        return EMBED_POOL_ERR_NULL_POINTER;
    }
    if (!EmbedPool_IsAligned(pBuffer)) {
        return EMBED_POOL_ERR_BAD_ALIGNMENT;
    }
    if (capacity < (uint32_t)EMBED_POOL_MIN_SPLIT) {
        return EMBED_POOL_ERR_BAD_CAPACITY;
    }

    pPool->pBuffer = (uint8_t *)pBuffer;
    pPool->capacity = capacity;

    /* 复用 Reset 的"只留一整块空闲"逻辑，创建与清空因此永远走同一条路。 */
    return EmbedPool_Reset(pPool);
}

int8_t EmbedPool_Reset(EmbedPool_T *pPool)
{
    if (pPool == NULL) {
        return EMBED_POOL_ERR_NULL_POINTER;
    }
    if (pPool->pBuffer == NULL || pPool->capacity < (uint32_t)EMBED_POOL_MIN_SPLIT) {
        return EMBED_POOL_ERR_BAD_CAPACITY;
    }

    /*
     * 停机清理：**整张块表**回到"一整块空闲"，计数与峰值一起归零。
     * 上一次运行借出去的地址从此作废（VM 也只在同一次 run 内使用它们），
     * 所以下一次运行能重新从头分配。显式清一遍全表，调试时看内存也干净。
     */
    {
        uint32_t index = 0;
        for (index = 0; index < EMBED_POOL_MAX_BLOCKS; index++) {
            pPool->blocks[index].offset = 0u;
            pPool->blocks[index].size = 0u;
            pPool->blocks[index].used = 0u;
        }
    }
    pPool->blocks[0].offset = 0u;
    pPool->blocks[0].size = pPool->capacity;
    pPool->blocks[0].used = 0u;
    pPool->blockCount = 1u;
    pPool->liveBytes = 0u;
    pPool->liveBlocks = 0u;
    pPool->peakBytes = 0u;
    return EMBED_POOL_ERR_OK;
}

void *EmbedPool_Alloc(EmbedPool_T *pPool, uint32_t bytes)
{
    uint32_t need = 0u;
    uint32_t index = 0u;
    uint8_t overflow = 0u;

    if (pPool == NULL || pPool->pBuffer == NULL || bytes == 0u) {
        return NULL;
    }

    need = EmbedPool_AlignUp(bytes, &overflow);
    if (overflow != 0u) {
        return NULL;
    }

    for (index = 0u; index < pPool->blockCount; index++) {
        uint32_t rest = 0u;

        if (pPool->blocks[index].used != 0u) {
            continue;
        }
        if (pPool->blocks[index].size < need) {
            continue;
        }

        rest = pPool->blocks[index].size - need;
        if (rest >= (uint32_t)EMBED_POOL_MIN_SPLIT) {
            /* 尾巴够开一块新的，就切出去；切不动（块表满）则整块给出去。 */
            if (EmbedPool_InsertAt(pPool, index + 1u, pPool->blocks[index].offset + need, rest,
                                   0u) != 0u) {
                pPool->blocks[index].size = need;
            }
        }

        pPool->blocks[index].used = 1u;
        pPool->liveBytes += pPool->blocks[index].size;
        pPool->liveBlocks++;
        if (pPool->liveBytes > pPool->peakBytes) {
            pPool->peakBytes = pPool->liveBytes;
        }
        return (void *)(pPool->pBuffer + pPool->blocks[index].offset);
    }

    return NULL;
}

int8_t EmbedPool_Free(EmbedPool_T *pPool, void *pBlock)
{
    uint32_t offset = 0u;
    uint32_t index = 0u;

    if (pPool == NULL || pBlock == NULL) {
        return EMBED_POOL_ERR_NULL_POINTER;
    }
    if (EmbedPool_Contains(pPool, pBlock) == 0u) {
        return EMBED_POOL_ERR_INVALID_POINTER;
    }

    offset = (uint32_t)((uint8_t *)pBlock - pPool->pBuffer);
    for (index = 0u; index < pPool->blockCount; index++) {
        if (pPool->blocks[index].offset != offset) {
            continue;
        }
        if (pPool->blocks[index].used == 0u) {
            return EMBED_POOL_ERR_INVALID_POINTER; /* 重复释放 */
        }
        pPool->blocks[index].used = 0u;
        pPool->liveBytes -= pPool->blocks[index].size;
        pPool->liveBlocks--;
        EmbedPool_Coalesce(pPool, index);
        return EMBED_POOL_ERR_OK;
    }

    /* 地址在池里，但不是块首（野指针或块内部地址）。 */
    return EMBED_POOL_ERR_INVALID_POINTER;
}

uint8_t EmbedPool_Contains(const EmbedPool_T *pPool, const void *pBlock)
{
    const uint8_t *pStart = NULL;
    const uint8_t *pEnd = NULL;

    if (pPool == NULL || pPool->pBuffer == NULL || pBlock == NULL) {
        return 0u;
    }
    pStart = pPool->pBuffer;
    pEnd = pStart + pPool->capacity;
    if ((const uint8_t *)pBlock < pStart || (const uint8_t *)pBlock >= pEnd) {
        return 0u;
    }
    return 1u;
}

uint8_t EmbedPool_FindLive(const EmbedPool_T *pPool, const void *pBlock, uint32_t *pSize)
{
    uint32_t offset = 0u;
    uint32_t index = 0u;

    if (pPool == NULL || pBlock == NULL || EmbedPool_Contains(pPool, pBlock) == 0u) {
        return 0u;
    }

    offset = (uint32_t)((const uint8_t *)pBlock - pPool->pBuffer);
    for (index = 0u; index < pPool->blockCount; index++) {
        const EmbedPool_Block_T *pEntry = &pPool->blocks[index];
        uint32_t end = pEntry->offset + pEntry->size;

        if (pEntry->used == 0u) {
            continue;
        }
        if (offset < pEntry->offset || offset >= end) {
            continue;
        }
        if (pSize != NULL) {
            *pSize = end - offset;
        }
        return 1u;
    }
    return 0u;
}

uint32_t EmbedPool_LiveBytes(const EmbedPool_T *pPool)
{
    return (pPool == NULL) ? 0u : pPool->liveBytes;
}

uint32_t EmbedPool_LiveBlocks(const EmbedPool_T *pPool)
{
    return (pPool == NULL) ? 0u : pPool->liveBlocks;
}

uint32_t EmbedPool_PeakBytes(const EmbedPool_T *pPool)
{
    return (pPool == NULL) ? 0u : pPool->peakBytes;
}

uint32_t EmbedPool_BlockCount(const EmbedPool_T *pPool)
{
    return (pPool == NULL) ? 0u : pPool->blockCount;
}

int8_t EmbedPool_Validate(const EmbedPool_T *pPool, const char **pMessage)
{
    uint32_t index = 0u;
    uint32_t liveBytes = 0u;
    uint32_t liveBlocks = 0u;

#define EMBED_POOL_FAIL(text)                 \
    do {                                      \
        if (pMessage != NULL) {               \
            *pMessage = (text);               \
        }                                     \
        return EMBED_POOL_ERR_INVALID_POINTER; \
    } while (0)

    if (pPool == NULL || pPool->pBuffer == NULL) {
        EMBED_POOL_FAIL("池对象或缓冲区为空");
    }
    if (pPool->blockCount == 0u || pPool->blockCount > (uint32_t)EMBED_POOL_MAX_BLOCKS) {
        EMBED_POOL_FAIL("块表条目数不合法");
    }
    if (pPool->blocks[0].offset != 0u) {
        EMBED_POOL_FAIL("块表没有从缓冲区起点开始");
    }

    for (index = 0u; index < pPool->blockCount; index++) {
        const EmbedPool_Block_T *pEntry = &pPool->blocks[index];

        if (pEntry->size == 0u) {
            EMBED_POOL_FAIL("存在大小为 0 的块");
        }
        if (pEntry->offset + pEntry->size > pPool->capacity) {
            EMBED_POOL_FAIL("块越过了缓冲区末尾");
        }
        if (index + 1u < pPool->blockCount &&
            pEntry->offset + pEntry->size != pPool->blocks[index + 1u].offset) {
            EMBED_POOL_FAIL("块表不连续（有空洞或重叠）");
        }
        if (pEntry->used != 0u) {
            liveBytes += pEntry->size;
            liveBlocks++;
        }
    }

    if (pPool->blocks[pPool->blockCount - 1u].offset +
            pPool->blocks[pPool->blockCount - 1u].size !=
        pPool->capacity) {
        EMBED_POOL_FAIL("块表没有覆盖到缓冲区末尾");
    }
    if (liveBytes != pPool->liveBytes || liveBlocks != pPool->liveBlocks) {
        EMBED_POOL_FAIL("统计值与块表不一致");
    }
    if (pPool->peakBytes < pPool->liveBytes) {
        EMBED_POOL_FAIL("峰值小于当前占用量");
    }

#undef EMBED_POOL_FAIL
    if (pMessage != NULL) {
        *pMessage = "";
    }
    return EMBED_POOL_ERR_OK;
}
