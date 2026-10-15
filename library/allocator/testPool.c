#include "pool.h"
#include <stdio.h>
#include <string.h>
/** @brief 报告验证失败。 @param condition 验证条件。 @param line 行号。 */
static void check(int condition, int line) { if (!condition) { fprintf(stderr,"pool failure at %d\n",line); exit(1); } }
#define CHECK(c) check((c),__LINE__)
/** @brief 验证 C99 分配、扩容、回收、耗尽和清零。 @return 成功返回零。 */
int main(void) {
    unsigned char storage[8192]; MemoryPool pool = {0}; MemoryPoolStats stats;
    unsigned char *a, *b, *grown; size_t count = 0; void *blocks[100]; int foreign = 0;
    CHECK(memoryPoolInit(&pool,storage,sizeof(storage)));
    CHECK(!memoryPoolAllocate(&pool,64,3));
    a = (unsigned char *)memoryPoolAllocate(&pool,128,64);
    b = (unsigned char *)memoryPoolAllocate(&pool,128,64);
    CHECK(a && b && a!=b && (uintptr_t)a%64==0 && (uintptr_t)b%64==0);
    memset(a,0x5a,128);
    CHECK(!memoryPoolFinish(&pool,NULL));
    CHECK(!memoryPoolReallocate(&pool,a,SIZE_MAX,64) && a[127]==0x5a);
    CHECK(!memoryPoolReallocate(&pool,a+1,256,64));
    grown = (unsigned char *)memoryPoolReallocate(&pool,a,256,64);
    CHECK(grown && grown[127]==0x5a && (uintptr_t)grown%64==0);
    CHECK(memoryPoolRelease(&pool,b)); CHECK(!memoryPoolRelease(&pool,b));
    CHECK(!memoryPoolRelease(&pool,&foreign));
    CHECK(memoryPoolRelease(&pool,grown));
#if MEMORY_ALLOCATOR_TYPE == MEMORY_ALLOCATOR_LINKED
    /* 三块释放后应合并，足以再次分配接近整个池的块。 */
    a = (unsigned char *)memoryPoolAllocate(&pool,7500,16); CHECK(a); CHECK(memoryPoolRelease(&pool,a));
#endif
    while(count<100 && (blocks[count]=memoryPoolAllocate(&pool,128,16))) ++count;
    CHECK(count>0 && count<100);
    while(count) CHECK(memoryPoolRelease(&pool,blocks[--count]));
    CHECK(pool.stats.liveBlocks==0);
    CHECK(memoryPoolFinish(&pool,&stats)); CHECK(stats.totalBlocks>2);
    for(count=0;count<sizeof(storage);++count) CHECK(storage[count]==0);
    CHECK(memoryPoolInit(&pool,storage,sizeof(storage)));
    a=(unsigned char *)memoryPoolAllocate(&pool,1,1);CHECK(a);
    CHECK(memoryPoolReallocate(&pool,a,0,16)==NULL && pool.stats.liveBlocks==0);
    CHECK(memoryPoolFinish(&pool,NULL));
    puts("C99 pool: OK");return 0;
}
