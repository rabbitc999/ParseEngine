/**
 * @file    embed_pool.h
 * @brief   嵌入式静态内存池（C99）：给 StackVM 字节堆用，也可单独在裸机上用。
 * @details 把一整块用户提供的连续内存切成"块"，块表放在池对象里（定长数组，零动态分配），
 *          首次适配 + 切块 + 相邻空闲块合并，所以**释放过的内存可以再分配出去**。
 *          思路与 STM32 那章的"静态内存池"一脉相承，只是块大小不再固定：
 *          VM 的 `alloc` 请求长度千变万化，固定块池要么浪费很多，要么装不下。
 *
 *          用法（裸机上只要这四步）：
 *          @code
 *          static EmbedPool_T pool;                     // 池对象本身也是静态的
 *          static uint8_t memory[8 * 1024];             // 或链接脚本里的段
 *          EmbedPool_Create(&pool, memory, sizeof(memory));
 *          void *p = EmbedPool_Alloc(&pool, 64);        // 失败给 NULL
 *          EmbedPool_Free(&pool, p);
 *          EmbedPool_Reset(&pool);                      // 整池清空，重新可用
 *          @endcode
 *
 *          两条硬规矩（从 VM 的角度）：
 *            1. 池里**第一个可分配地址不会落在缓冲区首字节之外**，而 VM 侧还要再加 `base`；
 *               地址 0 是空指针，由调用方（`embed_pool_allocator.hpp`）用 `base >= 1` 保证。
 *            2. `EmbedPool_Reset` 必须把**所有内部表**清干净（块表、计数、峰值），
 *               否则第二次运行会带着上一次的占用继续分配，越跑越少。
 *
 *          本文件刻意写成"能被 C++ 直接编译"的 C：不用 C++ 关键字、不做 void* 隐式转换、
 *          不用变长数组，也没有设计初始化器 —— 因为它会被整段内嵌进导出的 C++ 程序里。
 */

#ifndef EMBED_POOL_H
#define EMBED_POOL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------- 可调参数 ---------------- */

/**
 * 块表容量（条目数）。每个条目 12 字节左右，32 条约 384 字节，裸机上可以直接吃。
 * 碎片化严重的负载（大块小块交替申请）需要更大的表；编译时用
 * `-DEMBED_POOL_MAX_BLOCKS=64` 或（导出的程序里）`--heap-blocks N` 覆盖。
 */
#ifndef EMBED_POOL_MAX_BLOCKS
#define EMBED_POOL_MAX_BLOCKS 32u
#endif

/** 对齐字节数：VM 的槽位、浮点直读都是 8 字节，这里跟着来。 */
#define EMBED_POOL_ALIGN_SIZE 8u

/**
 * 切开之后剩下的尾巴小于这个值就不再切了，整块给出去。
 * 太小会让块表被"碎渣"塞满，太大则浪费尾部空间。
 */
#define EMBED_POOL_MIN_SPLIT 16u

/* ---------------- 错误码 ---------------- */

#define EMBED_POOL_ERR_OK               0    /**< 成功 */
#define EMBED_POOL_ERR_NULL_POINTER    -1    /**< 空指针参数 */
#define EMBED_POOL_ERR_BAD_ALIGNMENT   -2    /**< 缓冲区未按 EMBED_POOL_ALIGN_SIZE 对齐 */
#define EMBED_POOL_ERR_BAD_CAPACITY    -3    /**< 容量为 0 或小于最小块 */
#define EMBED_POOL_ERR_TOO_MANY_BLOCKS -4    /**< 块表满了（申请时碎片太多） */
#define EMBED_POOL_ERR_NO_FREE_BLOCK   -5    /**< 池里没有装得下的空闲块 */
#define EMBED_POOL_ERR_INVALID_POINTER -6    /**< 指针不在池里 / 不是块首 / 已经释放过 */

/* ---------------- 数据结构 ---------------- */

/**
 * @brief  块表里的一条记录。
 * @note   用偏移而不是指针：同一次运行里缓冲区不会搬，但偏移量更适合打印与调试，
 *         也不会在 32 位目标上多占一倍空间。
 */
typedef struct __EmbedPool_Block_T
{
    uint32_t offset;    /**< 相对缓冲区起点的字节偏移 */
    uint32_t size;      /**< 块大小（字节） */
    uint8_t  used;      /**< 1-已分配，0-空闲 */
} EmbedPool_Block_T;

/**
 * @brief  静态内存池。整块内存由调用方提供，池对象只保存块表与统计。
 */
typedef struct __EmbedPool_T
{
    uint8_t          *pBuffer;      /**< 用户提供的连续内存 */
    uint32_t          capacity;     /**< 缓冲区字节数 */
    uint32_t          blockCount;   /**< 块表**有效**条目数（不是容量） */
    uint32_t          liveBytes;    /**< 已分配字节数合计 */
    uint32_t          liveBlocks;   /**< 已分配块数 */
    uint32_t          peakBytes;    /**< 已分配字节数的历史峰值 */
    EmbedPool_Block_T blocks[EMBED_POOL_MAX_BLOCKS]; /**< 块表：按 offset 升序，首尾相接 */
} EmbedPool_T;

/* ---------------- 接口 ---------------- */

/**
 * @brief  用一整块内存创建池：复位成"整块空闲"。
 * @param  pPool     池对象（调用方提供，通常是静态的）
 * @param  pBuffer   连续内存，需按 EMBED_POOL_ALIGN_SIZE 对齐
 * @param  capacity  字节数，必须 >= EMBED_POOL_MIN_SPLIT
 * @retval 错误码（0 表示成功）
 */
int8_t EmbedPool_Create(EmbedPool_T *pPool, void *pBuffer, uint32_t capacity);

/**
 * @brief  清空所有内部表，回到"刚创建"的状态。
 * @details 块表收缩成"一整块空闲"，计数与峰值归零 —— 这就是**停机清理**：
 *          上一次运行占用的块全部作废，下一次运行又能从头分配。
 * @param  pPool 池对象
 * @retval 错误码
 */
int8_t EmbedPool_Reset(EmbedPool_T *pPool);

/**
 * @brief  申请一块内存（首次适配 + 切块）。
 * @param  pPool 池对象
 * @param  bytes 需要的字节数，内部按 EMBED_POOL_ALIGN_SIZE 向上取整
 * @retval 块地址；池满 / 无合适空闲块 / 块表满时返回 NULL
 * @note   返回的地址一定落在缓冲区里，且**不会**是缓冲区起点之前的位置。
 */
void *EmbedPool_Alloc(EmbedPool_T *pPool, uint32_t bytes);

/**
 * @brief  释放 EmbedPool_Alloc 返回的块，并与相邻空闲块合并。
 * @param  pPool  池对象
 * @param  pBlock 块地址（必须是块首）
 * @retval 错误码（重复释放、野指针都返回 EMBED_POOL_ERR_INVALID_POINTER）
 */
int8_t EmbedPool_Free(EmbedPool_T *pPool, void *pBlock);

/**
 * @brief  这个地址是否落在池的缓冲区里（含从未分配过的区域）。
 * @details VM 用它区分"堆地址"和"数据区地址"，好把错误说成人话；
 *          判断"这块内存还有效吗"请用 EmbedPool_FindLive。
 */
uint8_t EmbedPool_Contains(const EmbedPool_T *pPool, const void *pBlock);

/**
 * @brief  查一个地址属于哪个**活跃**块。
 * @param  pPool 池对象
 * @param  pBlock 地址（不必是块首）
 * @param  pSize 输出：从该地址到块尾还剩多少字节（可为 NULL）
 * @retval 1-在活跃块里，0-不在
 */
uint8_t EmbedPool_FindLive(const EmbedPool_T *pPool, const void *pBlock, uint32_t *pSize);

/** 当前已分配字节数。 */
uint32_t EmbedPool_LiveBytes(const EmbedPool_T *pPool);

/** 当前已分配块数。 */
uint32_t EmbedPool_LiveBlocks(const EmbedPool_T *pPool);

/** 已分配字节数的历史峰值（EmbedPool_Reset 会清零）。 */
uint32_t EmbedPool_PeakBytes(const EmbedPool_T *pPool);

/** 块表有效条目数 —— 调试时看碎片化程度。 */
uint32_t EmbedPool_BlockCount(const EmbedPool_T *pPool);

/**
 * @brief  自检：块表是否按序、首尾相接、不越界，统计是否与块表一致。
 * @param  pPool     池对象
 * @param  pMessage  失败原因（可为 NULL）
 * @retval 错误码
 * @note   只在调试构建里调它：会遍历整张块表。
 */
int8_t EmbedPool_Validate(const EmbedPool_T *pPool, const char **pMessage);

#ifdef __cplusplus
}
#endif

#endif /* EMBED_POOL_H */
