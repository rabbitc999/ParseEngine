# embed_stackvm

`embed_stackvm` 展示如何给 `library/stackvm` 注入固定容量的 VM 字节堆。它面向内存预算可
预先确定的宿主：分配器的数据区和块表都使用静态数组，运行时不会为 VM 堆扩容。

两种实现，按"要不要复用已释放的内存"挑：

| 实现 | 策略 | 适合 |
| --- | --- | --- |
| `embed_pool.h` / `embed_pool.c` + `embed_pool_allocator.hpp` | **C 写的静态内存池**：首次适配、按需切块、相邻空闲块合并，**释放过的内存可以再分配** | 内存紧张、程序会反复分配释放（容器 / 字符串多）；也是 `magel-emit --target embedded-cpp` 的内置堆 |
| `static_heap_allocator.hpp` | 只向后分配，已释放块留到下次 `reset` 才作废 | 想用"地址不复用"当护栏，宁可多占内存 |

## 接入

```cpp
#include "embed_pool_allocator.hpp"     // C 池；或用 static_heap_allocator.hpp

embed_stackvm::EmbedPoolAllocator<4096> heap;    // 4096 字节的 VM 堆
stackvm::Machine machine;
machine.setHeapAllocator(&heap);
stackvm::RunResult result = machine.run(program);
```

块表大小由 `EMBED_POOL_MAX_BLOCKS`（缺省 32）在编译期定：`-DEMBED_POOL_MAX_BLOCKS=64`；
`magel-emit --target embedded-cpp` 用的是 `--heap-blocks`。

**每次 `run()` 开始时 Machine 都会调用 `reset(base)`**：C 池的 `reset` 把**整张块表**清空
（块表收回"一整块空闲"、占用计数与峰值一起归零），所以同一个分配器对象、同一个 `Machine`
可以连着跑很多轮，第二轮照样从头分配 —— 这就是"停机自动清理内存表"。
`static_heap_allocator.hpp` 走的是同一约定，只是它顺带把已释放地址作废到下一轮。

```cpp
// 同一个池连着跑两轮，第二轮不会"没内存"
for (int round = 0; round < 2; ++round) machine.run(program);
```

分配器负责以下内存：

- 四元式 `alloc` / `free`；
- VM 地址上的 `load` / `store` 与 `checkptr`；
- StackVM 内置运行时和 C++ 胶水通过 `NativeCallContext::makeString()` 创建的字符串。

它暂不接管 C++ 自身的求值栈、调用帧、STL 容器、大整数和运行期对象表。这一版先把最适合
固定 arena 的 VM 字节堆开放出来，保持默认 StackVM 和 Magel 行为不变。

## 策略

C 池（`embed_pool.c`，与 STM32 那章的"静态内存池"同源，只是块大小不再固定）：

- 块表在池对象里（定长数组），**没有一次 `malloc`**；一份内存只切一次，申请时首次适配，
  尾巴够大就切一块新的出去（`EMBED_POOL_MIN_SPLIT`）；
- 释放时与左右相邻空闲块**合并**，所以反复申请 / 释放不会越跑越碎；
- 释放过的地址**可以**再分配出去（VM 明确允许自定义分配器复用地址）；但"释放后再读"
  仍会被 `capacityAt` / `readBytes` 挡下 —— 它们只认活跃块；
- `EmbedPool_Reset` 清空整张块表与统计，这是每次 `run()` 的起点；
- 容量或块表耗尽时 `allocate` 返回 0，由 VM 报"堆内存不足"（不会返回越界地址）；
- `EmbedPool_Validate` 是自检：块表有序、首尾相接、不越界、统计与块表一致。

`static_heap_allocator.hpp` 的策略不同：只向后分配，已释放块保留到下一次 `reset`，
用"地址不复用"当护栏，代价是内存占用只增不减。

两者都保留越界、释放后访问、双重释放与 live/peak 统计。

## 构建

```sh
make -C library/embed_stackvm check
```

示例应输出：

```text
embed_stackvm: 81 checks, 0 failures
42
heap peak=16 live=0
```

CMake 也可独立构建：

```sh
cmake -S library/embed_stackvm -B build/embed_stackvm-cmake
cmake --build build/embed_stackvm-cmake
ctest --test-dir build/embed_stackvm-cmake --output-on-failure
```

## 自定义字节堆：照抄 `custom_heap_allocator.hpp`

嵌入式宿主不想让 VM 碰宿主堆时，**不要再分叉一份 stackvm**，而是自己实现
`stackvm::HeapAllocator` 并用 `Machine::setHeapAllocator()` 接进去。
`custom_heap_allocator.hpp` 是可直接照抄的模板（每个成员都标了 TODO，附三条硬规矩：
地址 0 不能分出去、`reset()` 每次 run 前会调、装不下要返回 0）；
`static_heap_allocator.hpp` 是固定容量 + 静态块表的参考实现。

它替换的范围只是四元式 `alloc` / `free`、load / store、`checkptr` 和字符串胶水用的
**字节堆**；调用帧、STL 容器、大整数与对象表仍走宿主 C++ 内存。
