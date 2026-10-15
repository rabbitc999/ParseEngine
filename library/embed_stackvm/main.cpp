#include "static_heap_allocator.hpp"

#include <iostream>

int main()
{
    const stackvm::QuadFile quads = stackvm::parseQuads(
        "0: (decl, i64, global, p)\n"
        "1: (alloc, 16, _, p)\n"
        "2: (store:i32, p, 42, _)\n"
        "3: (load:i32, p, _, value)\n"
        "4: (write, value, _, _)\n"
        "5: (free, p, _, _)\n"
        "6: (halt, _, _, _)\n",
        "<embed-stackvm>");
    stackvm::Program program;
    if (!stackvm::assemble(quads, program)) return 1;

    embed_stackvm::StaticHeapAllocator<256u, 16u> heap;
    stackvm::Machine machine;
    machine.setHeapAllocator(&heap);
    const stackvm::RunResult result = machine.run(program);
    if (!result.ok)
    {
        std::cerr << result.error << '\n';
        return 2;
    }

    std::cout << result.output << '\n';
    std::cout << "heap peak=" << result.heapPeakBytes
              << " live=" << result.heapLiveBytes << '\n';
    return result.output == "42" && result.heapLiveBytes == 0u ? 0 : 3;
}
