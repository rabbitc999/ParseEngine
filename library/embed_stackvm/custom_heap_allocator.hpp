#ifndef EMBED_STACKVM_CUSTOM_HEAP_ALLOCATOR_HPP
#define EMBED_STACKVM_CUSTOM_HEAP_ALLOCATOR_HPP

/*
 * custom_heap_allocator.hpp —— **自定义 VM 字节堆的模板（照抄这个文件改）**
 *
 * 什么时候用它：宿主没有 malloc / 不想让 VM 碰堆时，自己给 VM 划一块固定内存。
 * 接法是 `Machine::setHeapAllocator(&yourAllocator)`（见 README 的"嵌入式接入"一节），
 * 它只替换四元式 `alloc` / `free`、load / store、`checkptr` 和字符串胶水用的**字节堆**；
 * 调用帧、STL 容器、大整数与对象表仍走宿主 C++ 内存 —— 别对外宣称"整台 VM 零动态分配"。
 *
 * 三条硬规矩（踩过）：
 *   1. **不能把地址 0 分出去**：0 是"空指针"，`checkptr` / 判空全靠它；
 *      所以 `reset(base)` 之后的第一个可分配地址必须 > 0（参考实现从 `base` 之后开始）。
 *   2. `reset()` 每次 `run()` 之前都会被调一次，必须把整块内存"归零重置"，
 *      不能泄漏上一轮的状态。
 *   3. `alloc` 的字节数来自 VM，可能很大 —— 装不下时要**返回 0**（表示分配失败），
 *      不要返回一个越界的地址。
 *
 * 需要实现的接口见 `stackvm.hpp` 的 `class HeapAllocator`；下面每个成员都标了 TODO。
 */

#include "stackvm.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace embed_stackvm
{

/**
 * @brief 你自己的固定内存块分配器：把一整块 `std::uint8_t*` 当成 VM 的堆。
 *
 * @tparam Capacity 这块内存的字节数（按你的器件改）。
 */
template <std::size_t Capacity>
class CustomHeapAllocator final : public stackvm::HeapAllocator
{
public:
    /**
     * @param memory 宿主提供的一整块内存（静态数组、链接脚本里的段、外部 SDRAM 都行）。
     * @param size 这块内存的字节数；要 >= Capacity。
     */
    CustomHeapAllocator(std::uint8_t *memory, std::size_t size)
        : memory_(memory), size_(size) {}

    /** 每次 run() 前调用：把整块内存重置成"空闲"。 */
    void reset(std::size_t base) override
    {
        base_ = base < size_ ? base : size_;
        liveBytes_ = 0;
        liveBlocks_ = 0;
        peakBytes_ = 0;
        blocks_.clear();          /* TODO：换成你自己的块表 / 位图 */
    }

    [[nodiscard]] std::size_t base() const override { return base_; }

    /**
     * @brief 要 bytes 个字节；装不下返回 0（**不能**返回越界地址）。
     */
    [[nodiscard]] std::size_t allocate(std::size_t bytes) override
    {
        const std::size_t aligned = Align(bytes);
        /* TODO：在这里挑空位。下面是最朴素的"顺序往后放"，你自己的策略（首次适配 /
         * 最佳适配 / 伙伴系统）只影响碎片，不影响接口语义。 */
        const std::size_t offset = used_;
        if (aligned == 0 || offset + aligned > size_ - base_) return 0;
        used_ += aligned;
        blocks_.push_back(Block{base_ + offset, bytes});
        liveBytes_ += bytes;
        liveBlocks_ += 1;
        if (liveBytes_ > peakBytes_) peakBytes_ = liveBytes_;
        return base_ + offset;    /* 注意：address 已经在 base 之后，天然不为 0 */
    }

    /** 释放（地址不对就写 reason 并返回 false，别崩）。 */
    bool release(std::size_t address, std::string *reason = nullptr) override
    {
        for (std::size_t index = 0; index < blocks_.size(); ++index)
        {
            if (blocks_[index].address != address) continue;
            liveBytes_ -= blocks_[index].bytes;
            liveBlocks_ -= 1;
            blocks_.erase(blocks_.begin() + static_cast<std::ptrdiff_t>(index));
            return true;
        }
        /* TODO：按你的策略决定"重复 free / 野地址"是报错还是忽略。 */
        if (reason != nullptr) *reason = "释放了一个不是活跃块的地址";
        return false;
    }

    /** 这个地址在不在堆里（`checkptr` 用）。 */
    [[nodiscard]] bool contains(std::size_t address) const override
    {
        return address >= base_ && address < base_ + used_;
    }

    /** 这个地址所在那一块还能用多少字节（越界检查要用它）。 */
    [[nodiscard]] bool capacityAt(std::size_t address, std::size_t &capacity) const override
    {
        for (const Block &block : blocks_)
        {
            if (address < block.address || address >= block.address + block.bytes) continue;
            capacity = block.bytes - (address - block.address);
            return true;
        }
        return false;
    }

    [[nodiscard]] std::size_t liveBytes() const override { return liveBytes_; }
    [[nodiscard]] std::size_t liveBlocks() const override { return liveBlocks_; }
    [[nodiscard]] std::size_t peakBytes() const override { return peakBytes_; }

    /* 下面两个是"按地址直读直写"，字符串拼接与 load/store 都走它们。 */
    [[nodiscard]] bool readBytes(std::size_t address, char *out, std::size_t count) const override
    {
        if (out == nullptr || !RangeInside(address, count)) return false;
        for (std::size_t index = 0; index < count; ++index)
            out[index] = static_cast<char>(memory_[address + index]);
        return true;
    }

    bool writeBytes(std::size_t address, const char *data, std::size_t count) override
    {
        if (data == nullptr || !RangeInside(address, count)) return false;
        for (std::size_t index = 0; index < count; ++index)
            memory_[address + index] = static_cast<std::uint8_t>(data[index]);
        return true;
    }

private:
    struct Block
    {
        std::size_t address = 0;
        std::size_t bytes = 0;
    };

    /** 8 字节对齐（VM 的槽位是 8 字节，浮点/整数直读要它）。 */
    static std::size_t Align(std::size_t bytes) { return (bytes + 7u) & ~std::size_t(7u); }

    [[nodiscard]] bool RangeInside(std::size_t address, std::size_t count) const
    {
        if (address < base_) return false;
        return count <= size_ - address;      /* 先减再比，免得 address + count 溢出 */
    }

    std::uint8_t *memory_ = nullptr;   /* 整块内存 */
    std::size_t size_ = 0;             /* 整块内存的字节数 */
    std::size_t base_ = 0;             /* 本次 run 的起始地址（由 reset 给） */
    std::size_t used_ = 0;             /* 顺序放置时的水位（换成你的策略后可删） */
    std::vector<Block> blocks_;        /* TODO：容量受限的宿主可以换成定长数组 */
    std::size_t liveBytes_ = 0;
    std::size_t liveBlocks_ = 0;
    std::size_t peakBytes_ = 0;
};

/**
 * 用法（把 `kStaticMemory` 换成你器件上的那块内存）：
 *
 * ```cpp
 * static std::uint8_t kStaticMemory[64 * 1024];      // 或链接脚本里的段
 * embed_stackvm::CustomHeapAllocator<sizeof(kStaticMemory)> heap(kStaticMemory,
 *                                                               sizeof(kStaticMemory));
 * stackvm::Machine machine;
 * machine.setHeapAllocator(&heap);                    // 换掉默认的宿主堆
 * machine.load(program);
 * machine.run();
 * ```
 *
 * 想直接要一个能跑的参考实现看 `static_heap_allocator.hpp`（固定容量 + 静态块表）。
 * 注意 `Machine` 是**按值实例化**的公开类型：改了 `stackvm.hpp` 的字段布局之后，
 * 下游的 Makefile 必须把 `stackvm.hpp` 列为对象依赖、把 `libstackvm.a` 列为最终目标依赖，
 * 否则增量构建会把旧对象和新库链在一起，表现为**一进 VM 就崩**。
 */

} // namespace embed_stackvm

#endif // EMBED_STACKVM_CUSTOM_HEAP_ALLOCATOR_HPP
