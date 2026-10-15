#include "containerAllocator.hpp"
#include <cstdio>
#include <cstdlib>
/** @brief 验证条件。 @param condition 条件值。 @param line 行号。 */
void check(bool condition,int line) { if(!condition) { std::fprintf(stderr,"container failure at %d\n",line);std::exit(1); } }
#define CHECK(c) check(bool(c),__LINE__)
/** @brief 测试容器归属、异常回收和会话复用。 @return 成功返回零。 */
int main() {
    alignas(std::max_align_t) unsigned char first[65536],second[65536];
    Memory::BufferRuntimeArena firstArena(first,sizeof(first)),secondArena(second,sizeof(second));
    CHECK(Memory::currentRuntimeAllocator()==nullptr);
    for(int pass=0;pass<2;++pass) {
        Memory::RuntimeSession session(firstArena);
        {
            Memory::Vector<int> values(128,7);
            Memory::String text(100,'x');
            Memory::Map<int,Memory::String> map;map.emplace(3,text);
            auto object=Memory::makeUnique<int>(42);
            Memory::Function<int(int)> function=[](int x){return x*2;};
            auto copiedFunction=function; CHECK(copiedFunction(9)==18 && *object==42);
            CHECK(values.get_allocator().owner==&firstArena && firstArena.owns(values.data()));
            {
                Memory::AllocationScope scope(&secondArena);
                values.reserve(512);
                CHECK(firstArena.owns(values.data()));
                Memory::Vector<int> other(128,9);
                CHECK(secondArena.owns(other.data()));
            }
            CHECK(secondArena.stats().liveBlocks==0);
            try { struct Throws { Throws(){throw 17;} }; auto bad=Memory::makeUnique<Throws>(); CHECK(false); }
            catch(int value) { CHECK(value==17); }
        }
        CHECK(firstArena.stats().liveBlocks==0);
    }
    CHECK(Memory::currentRuntimeAllocator()==nullptr && firstArena.lastRunStats().peakBytes>0);
    { Memory::DynamicRuntimeArena dynamic(4096); Memory::AllocationScope scope(&dynamic); Memory::Vector<int> v(8,1); CHECK(dynamic.owns(v.data())); }
    { Memory::StaticRuntimeArena<4096> fixed; Memory::AllocationScope scope(&fixed); Memory::String s(100,'q'); CHECK(fixed.owns(s.data())); }
    std::puts("C++ containers: OK");
}
