#ifndef PARSE_ENGINE_EMBED_STACKVM_EMBED_POOL_ALLOCATOR_HPP
#define PARSE_ENGINE_EMBED_STACKVM_EMBED_POOL_ALLOCATOR_HPP

/*
 * embed_pool_allocator.hpp —— 把 C 写的静态内存池（`embed_pool.h` / `embed_pool.c`）
 * 接到 `stackvm::HeapAllocator` 上的薄薄一层。
 *
 * 分配策略、块表、停机清理都在 C 那边（`EmbedPool_*`），这里只做三件事：
 *   1. **地址翻译**：VM 用绝对地址（堆跟在数据区后面），池用缓冲区指针，
 *      两者差一个 `base`；
 *   2. **接口适配**：`allocate` / `release` / `contains` / `capacityAt` / 读写字节
 *      翻译成 `EmbedPool_Alloc` / `EmbedPool_Free` / `EmbedPool_FindLive` …；
 *   3. **失败翻译**：分配不到返回 0（VM 会把它变成"堆内存不足"的诊断），
 *      地址不对时写 `reason` 而不是崩。
 *
 * 三条与 VM 相关的约定（照抄参考实现时别丢）：
 *   - `reset(base)` 每次 `run()` 前都会被调；这里顺手让 `base` 至少为 1，
 *     因为**地址 0 是空指针**，不能分出去；
 *   - **释放过的地址可以再分配出去**（VM 明确允许自定义分配器复用地址，
 *     见 `stackvm.hpp` 的 `HeapAllocator` 注释）；但"释放后再读"仍然会被
 *     `capacityAt` / `readBytes` 拦下来 —— 它们只认**活跃**块；
 *   - 池装不下时 `allocate` 返回 0，不要返回越界地址。
 *
 * 没有 STL 容器、没有 `new`：对象本身就一块，适合塞在 `.bss` 里。
 */

#include "embed_pool.h"
#include "stackvm.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

namespace embed_stackvm
{

/**
 * @brief 固定容量的 VM 字节堆，底层是 C 静态内存池。
 *
 * @tparam Capacity 这块堆的字节数（按器件改）。块表大小由 `EMBED_POOL_MAX_BLOCKS` 决定，
 *                  编译时用 `-DEMBED_POOL_MAX_BLOCKS=N` 调整。
 */
template <std::size_t Capacity>
class EmbedPoolAllocator final : public stackvm::HeapAllocator
{
    static_assert(Capacity > 0u, "EmbedPoolAllocator 的容量必须大于 0");

public:
    EmbedPoolAllocator() { EmbedPool_Create(&pool_, storage_, static_cast<std::uint32_t>(Capacity)); }

    /** 每次 run() 前调用：整池清空（块表、计数、峰值全部归零）。 */
    void reset(std::size_t base) override
    {
        base_ = (base == 0u) ? 1u : base;
        EmbedPool_Reset(&pool_);
    }

    [[nodiscard]] std::size_t base() const override { return base_; }

    /** 要 bytes 个字节；池里没有装得下的空闲块时返回 0（**不能**返回越界地址）。 */
    [[nodiscard]] std::size_t allocate(std::size_t bytes) override
    {
        if (bytes == 0u || bytes > kMaxRequest) return 0u;
        void *block = EmbedPool_Alloc(&pool_, static_cast<std::uint32_t>(bytes));
        if (block == nullptr) return 0u;
        return AddressOf(block);
    }

    bool release(std::size_t address, std::string *reason = nullptr) override
    {
        if (address == 0u) return true; /* 空指针本来就不用释放 */
        if (address < base_) return Fail(reason, "地址不在嵌入式堆里");
        if (EmbedPool_Free(&pool_, MutablePointerOf(address)) == EMBED_POOL_ERR_OK) return true;
        return Fail(reason, "释放的不是活跃块（重复释放或野指针）");
    }

    /** 地址落在池的缓冲区里就算（含从未分配与已释放的区域）—— VM 用它区分"堆/数据区"。 */
    [[nodiscard]] bool contains(std::size_t address) const override
    {
        if (address < base_) return false;
        return EmbedPool_Contains(&pool_, ConstPointerOf(address)) != 0u;
    }

    /** 地址所在**活跃**块到块尾还剩多少字节（越界检查用）。 */
    [[nodiscard]] bool capacityAt(std::size_t address, std::size_t &capacity) const override
    {
        if (address < base_) return false;
        std::uint32_t remaining = 0u;
        if (EmbedPool_FindLive(&pool_, ConstPointerOf(address), &remaining) == 0u) return false;
        capacity = remaining;
        return true;
    }

    [[nodiscard]] std::size_t liveBytes() const override { return EmbedPool_LiveBytes(&pool_); }
    [[nodiscard]] std::size_t liveBlocks() const override { return EmbedPool_LiveBlocks(&pool_); }
    [[nodiscard]] std::size_t peakBytes() const override { return EmbedPool_PeakBytes(&pool_); }

    [[nodiscard]] bool readBytes(std::size_t address, char *out, std::size_t count) const override
    {
        if (count == 0u) return true;
        if (out == nullptr || address < base_) return false;
        const std::uint8_t *source = ConstPointerOf(address);
        if (!RangeIsLive(source, count)) return false;
        for (std::size_t index = 0u; index < count; ++index)
            out[index] = static_cast<char>(source[index]);
        return true;
    }

    bool writeBytes(std::size_t address, const char *data, std::size_t count) override
    {
        if (count == 0u) return true;
        if (data == nullptr || address < base_) return false;
        std::uint8_t *target = MutablePointerOf(address);
        if (!RangeIsLive(target, count)) return false;
        for (std::size_t index = 0u; index < count; ++index)
            target[index] = static_cast<std::uint8_t>(data[index]);
        return true;
    }

    /* ---- 给宿主和调试用的只读信息（VM 接口之外） ---- */

    [[nodiscard]] constexpr std::size_t capacity() const { return Capacity; }
    /** 块表有效条目数：看碎片化程度。 */
    [[nodiscard]] std::uint32_t blockCount() const { return EmbedPool_BlockCount(&pool_); }
    /** 池内部一致性自检；通过时 `*message` 为空串。 */
    [[nodiscard]] int8_t validate(const char **message = nullptr) const
    {
        return EmbedPool_Validate(&pool_, message);
    }

private:
    static constexpr std::size_t kMaxRequest = 0xffffffffu; /* C 侧偏移是 uint32_t */

    [[nodiscard]] std::uint8_t *MutablePointerOf(std::size_t address)
    {
        return storage_ + (address - base_);
    }

    [[nodiscard]] const std::uint8_t *ConstPointerOf(std::size_t address) const
    {
        return storage_ + (address - base_);
    }

    [[nodiscard]] std::size_t AddressOf(const void *block) const
    {
        return base_ + static_cast<std::size_t>(static_cast<const std::uint8_t *>(block) - storage_);
    }

    /** `[pointer, pointer + count)` 是否整段落在同一个活跃块里。 */
    [[nodiscard]] bool RangeIsLive(const std::uint8_t *pointer, std::size_t count) const
    {
        std::uint32_t available = 0u;
        if (EmbedPool_FindLive(&pool_, pointer, &available) == 0u) return false;
        return count <= available;
    }

    static bool Fail(std::string *reason, const char *message)
    {
        if (reason != nullptr) *reason = message;
        return false;
    }

    EmbedPool_T pool_{};
    std::uint8_t storage_[Capacity]{};
    std::size_t base_ = 1u;
};

} // namespace embed_stackvm

#endif // PARSE_ENGINE_EMBED_STACKVM_EMBED_POOL_ALLOCATOR_HPP
