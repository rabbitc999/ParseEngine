# 自定义分配器

`library/allocator` 是独立的 C99 / C++17 自定义分配池与容器适配库。C++ 命名空间为 `Memory`，C 接口采用 `MemoryPool` 类型与 `memoryPool*` 函数名，接口和配置不依赖具体语言工程。

CAS 直接引用本库，通过 `Cas::Memory` 命名空间别名接入；`Cas::Allocator` 保留其进程级池适配接口。CAS 不维护分配器实现副本。

## 使用

头文件搜索路径设为仓库的 `library/`：

```cpp
#include <allocator/containerAllocator.hpp>

alignas(std::max_align_t) unsigned char storage[64 * 1024];
Memory::BufferRuntimeArena arena(storage, sizeof(storage));
{
    Memory::RuntimeSession session(arena);
    Memory::Vector<int> values{1, 2, 3};
    Memory::String text("custom allocation pool");
} // 容器先析构；会话恢复之前的分配器并清零池。
```

C99 调用者只需包含 `allocator/pool.h`，它会内嵌 `pool.c` 的实现；函数具有内部链接，无须单独链接静态库。所有调用者缓冲区必须比其分配对象存活更久。

- `memoryPoolInit`：连接外部存储并初始化池。
- `memoryPoolAllocate`：按二的幂次对齐量分配；耗尽返回空指针。
- `memoryPoolReallocate`：调整容量并复制数据，失败保留旧块；零大小释放旧块。
- `memoryPoolRelease`：归还块；无效或重复释放返回失败。
- `memoryPoolOwns`：检查地址是否位于池范围内，不保证地址为有效分配起点。
- `memoryPoolFinish`：要求无活动分配，保存统计并清零输入缓冲区。

## 分配策略与 C++ 接口

默认 `MEMORY_ALLOCATOR_TYPE=MEMORY_ALLOCATOR_LINKED`，使用首次适配、块拆分和相邻空闲块合并。编译时设置 `MEMORY_ALLOCATOR_TYPE=MEMORY_ALLOCATOR_LINEAR` 可选择线性池：释放会更新活动统计，地址在整个池重置前不会复用。

`Memory::BufferRuntimeArena` 使用调用者缓冲区；`DynamicRuntimeArena` 一次向宿主堆保留连续存储；`StaticRuntimeArena<N>` 内置固定容量缓冲区。三者的分配与统计操作带锁，底层 C 池不带锁。`AllocationScope` 只切换当前线程的分配器，`RuntimeSession` 还管理会话开始与清零；会话结束时仍有活动块会终止进程。

`Memory::Allocator<T>` 为容器记录分配归属，切换当前分配器后，既有容器仍向自己的池申请和释放内存。提供 `String`、`Vector`、`Map`、`Set` 等容器、字符串流、`makeUnique`、`makeShared` 与 `Function`。标准容器接口按标准库约定命名，其余公开类型和函数采用驼峰命名。

未显式指定分配区域时，库按需创建默认 512 MiB 区域；可通过显式区域避免该后备分配。CAS 连接自己的默认 64 MiB 池，不使用库后备区域。字节统计包含块头、对齐和块内碎片；`MEMORY_USE_MALLOC=1` 可切换宿主堆分配，此时池统计不可用。内存申请与缓冲区创建失败会抛出 `std::bad_alloc`，C 接口则返回空指针或失败值。

## 构建与验证

Windows 使用 Git Bash；产物统一在根 `build/allocator/` 下。根 `sh build.sh` 已纳入该组件，构建入口只构建、不执行测试。

```sh
make -C library/allocator
make -C library/allocator check
```

`all` 构建示例和测试程序；`check` 验证 C99/C++17 的链表与线性两种模式，覆盖对齐、耗尽、扩容失败保留旧块、非法地址拒绝、合并回收、会话复用、容器归属和异常清理。

也可使用 CMake；引用目标为 `ParseEngine::Allocator`：

```sh
cmake -S library/allocator -B build/allocator-cmake
cmake --build build/allocator-cmake
ctest --test-dir build/allocator-cmake --output-on-failure
```
