#include "containerAllocator.hpp"
#include <cstdio>
/** @brief 演示为字符串和连续容器提供调用者缓冲区。 @return 成功返回零。 */
int main() {
    alignas(std::max_align_t) unsigned char storage[64 * 1024];
    Memory::BufferRuntimeArena arena(storage, sizeof(storage));
    {
        Memory::RuntimeSession session(arena);
        Memory::String message("custom allocation pool");
        Memory::Vector<int> values{1, 2, 3};
        std::printf("%s: %d elements, %zu live blocks\n", message.c_str(), int(values.size()), arena.stats().liveBlocks);
    }
    std::printf("released: %zu live blocks, peak %zu bytes\n", arena.stats().liveBlocks, arena.lastRunStats().peakBytes);
}
