#ifndef PARSE_ENGINE_EMBED_STACKVM_STATIC_HEAP_ALLOCATOR_HPP
#define PARSE_ENGINE_EMBED_STACKVM_STATIC_HEAP_ALLOCATOR_HPP

#include "../stackvm/stackvm.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace embed_stackvm
{

/**
 * @brief 固定容量、零动态元数据分配的 StackVM 字节堆。
 *
 * 数据和块表都在对象内部的 `std::array` 中，适合把 VM 堆容量在编译期钉死。分配策略与
 * StackVM 默认堆一致：首次适配、切块、相邻空闲块合并。容量或块表耗尽时 allocate()
 * 返回 0，Machine 会把它转换成正常的“堆内存不足”运行期诊断。
 */
template<std::size_t Capacity, std::size_t MaxBlocks = 32u>
class StaticHeapAllocator final : public stackvm::HeapAllocator
{
    static_assert(Capacity > 0u, "StaticHeapAllocator capacity must be positive");
    static_assert(MaxBlocks > 0u, "StaticHeapAllocator block table must not be empty");

public:
    struct Block
    {
        std::size_t address = 0u;
        std::size_t size = 0u;
        bool free = false;
    };

    void reset(std::size_t base) override
    {
        base_ = base == 0u ? 1u : base;
        used_ = 0u;
        blockCount_ = 0u;
        peak_ = 0u;
        storage_.fill(0u);
    }

    [[nodiscard]] std::size_t base() const override { return base_; }

    [[nodiscard]] std::size_t allocate(std::size_t bytes) override
    {
        if (bytes == 0u) return 0u;

        // 固定堆也保留已释放地址，直到 reset，避免悬空指针命中新对象。
        if (blockCount_ == MaxBlocks || bytes > Capacity - used_) return 0u;
        const std::size_t address = base_ + used_;
        blocks_[blockCount_++] = Block{address, bytes, false};
        used_ += bytes;
        refreshPeak();
        return address;
    }

    bool release(std::size_t address, std::string *reason = nullptr) override
    {
        if (address == 0u) return true;
        for (std::size_t index = 0u; index < blockCount_; ++index)
        {
            if (blocks_[index].address != address) continue;
            if (blocks_[index].free)
            {
                if (reason != nullptr) *reason = "这块内存已经 free 过了";
                return false;
            }
            blocks_[index].free = true;
            return true;
        }
        if (reason != nullptr)
        {
            *reason = "地址 " + std::to_string(address) + " 不在嵌入式堆里";
        }
        return false;
    }

    [[nodiscard]] bool contains(std::size_t address) const override
    {
        return address >= base_ && address - base_ < used_;
    }

    [[nodiscard]] bool capacityAt(std::size_t address,
                                  std::size_t &capacity) const override
    {
        const Block *block = liveBlockAt(address);
        if (block == nullptr) return false;
        capacity = block->address + block->size - address;
        return true;
    }

    [[nodiscard]] std::size_t liveBytes() const override
    {
        std::size_t total = 0u;
        for (std::size_t i = 0u; i < blockCount_; ++i)
        {
            if (!blocks_[i].free) total += blocks_[i].size;
        }
        return total;
    }

    [[nodiscard]] std::size_t liveBlocks() const override
    {
        std::size_t total = 0u;
        for (std::size_t i = 0u; i < blockCount_; ++i)
        {
            if (!blocks_[i].free) ++total;
        }
        return total;
    }

    [[nodiscard]] std::size_t peakBytes() const override { return peak_; }

    [[nodiscard]] bool readBytes(std::size_t address, char *out,
                                 std::size_t count) const override
    {
        if (count == 0u) return true;
        if (!usableRange(address, count)) return false;
        std::memcpy(out, storage_.data() + (address - base_), count);
        return true;
    }

    bool writeBytes(std::size_t address, const char *data, std::size_t count) override
    {
        if (count == 0u) return true;
        if (!usableRange(address, count)) return false;
        std::memcpy(storage_.data() + (address - base_), data, count);
        return true;
    }

    [[nodiscard]] constexpr std::size_t capacity() const { return Capacity; }
    [[nodiscard]] std::size_t committedBytes() const { return used_; }
    [[nodiscard]] std::size_t blockCount() const { return blockCount_; }

private:
    [[nodiscard]] const Block *liveBlockAt(std::size_t address) const
    {
        for (std::size_t i = 0u; i < blockCount_; ++i)
        {
            const Block &block = blocks_[i];
            if (!block.free && address >= block.address && address - block.address < block.size)
            {
                return &block;
            }
        }
        return nullptr;
    }

    [[nodiscard]] bool usableRange(std::size_t address, std::size_t count) const
    {
        const Block *block = liveBlockAt(address);
        if (block == nullptr) return false;
        const std::size_t offset = address - block->address;
        return count <= block->size - offset;
    }

    void refreshPeak()
    {
        peak_ = std::max(peak_, liveBytes());
    }

    std::size_t base_ = 1u;
    std::size_t used_ = 0u;
    std::size_t blockCount_ = 0u;
    std::size_t peak_ = 0u;
    std::array<std::uint8_t, Capacity> storage_{};
    std::array<Block, MaxBlocks> blocks_{};
};

} // namespace embed_stackvm

#endif // PARSE_ENGINE_EMBED_STACKVM_STATIC_HEAP_ALLOCATOR_HPP
