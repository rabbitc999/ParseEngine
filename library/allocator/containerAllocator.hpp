#ifndef MEMORY_CUSTOM_ALLOCATOR
#define MEMORY_CUSTOM_ALLOCATOR 1
#endif
#ifndef PARSEENGINE_MEMORY_ALLOCATOR_HPP
#define PARSEENGINE_MEMORY_ALLOCATOR_HPP
#include "pool.h"
#include <cstddef>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <functional>
#include <limits>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Memory {
/* 当前编译进来的池实现名，由 memoryPool.h 的 MEMORY_ALLOCATOR_TYPE 在编译期决定。 */
#if MEMORY_ALLOCATOR_TYPE == MEMORY_ALLOCATOR_LINKED
inline constexpr const char *allocatorTypeName = "linked";
#else
inline constexpr const char *allocatorTypeName = "linear";
#endif
/** @brief 运行时分配器接口，支持对齐、归属和统计查询。 */
class RuntimeAllocator {
public:
    /** @brief 当前与历史分配统计，字节数包含管理开销。 */
    struct Stats { std::size_t liveBytes = 0, liveBlocks = 0, peakBytes = 0, totalBlocks = 0; };
    /** @brief 销毁分配器接口；调用者应先销毁全部受管对象。 */
    virtual ~RuntimeAllocator() = default;
    /** @brief 分配默认对齐内存。 @param bytes 字节数。 @return 地址或 nullptr。 */
    virtual void *allocate(std::size_t bytes) = 0;
    /** @brief 分配指定对齐内存。 @param bytes 字节数。 @param alignment 对齐量。 @return 地址或 nullptr。 */
    virtual void *allocateAligned(std::size_t bytes, std::size_t alignment) {
        return alignment <= alignof(std::max_align_t) ? allocate(bytes) : nullptr;
    }
    /** @brief 归还分配。 @param pointer 分配地址。 @return 是否成功。 */
    virtual bool release(void *pointer) = 0;
    /** @brief 检查地址归属范围。 @param pointer 待检查地址。 @return 是否在区域内。 */
    virtual bool owns(const void *pointer) const = 0;
    /** @brief 获取当前统计。 @return 字节与块统计。 */
    virtual Stats stats() const = 0;
};
/** @brief 获取当前线程的分配器槽。 @return 槽引用；空表示使用默认区域。 */
inline RuntimeAllocator *&currentRuntimeAllocator() {
    static thread_local RuntimeAllocator *allocator = nullptr;
    return allocator;
}
/** @brief 按需获取进程级默认区域。 @return 分配器地址。 */
inline RuntimeAllocator *defaultRuntimeAllocator();
/** @brief 获取当前线程实际使用的分配器。 @return 当前或默认分配器。 */
inline RuntimeAllocator *runtimeAllocator() { return currentRuntimeAllocator() ? currentRuntimeAllocator() : defaultRuntimeAllocator(); }
/** @brief 切换线程分配器；原分配器必须比其分配存活更久。 @param allocator 新分配器或 nullptr。 */
inline void setRuntimeAllocator(RuntimeAllocator *allocator) {
    // Allocator instances remember their owner; previous owners must outlive their allocations.
    currentRuntimeAllocator() = allocator;
}
inline constexpr bool runtimeMemoryStatsAvailable =
#if defined(MEMORY_USE_MALLOC) && MEMORY_USE_MALLOC
    false;
#else
    true;
#endif
inline constexpr std::size_t defaultRuntimeCapacity = 512u * 1024u * 1024u;
/** @brief 获取默认分配器槽，供宿主在首次使用前接入专用池。 @return 原子指针槽。 */
inline std::atomic<RuntimeAllocator *> &defaultRuntimeAllocatorSlot() {
    static std::atomic<RuntimeAllocator *> pointer{nullptr};
    return pointer;
}
/** @brief 查询已创建的默认分配器，不触发创建。 @return 地址或 nullptr。 */
inline RuntimeAllocator *defaultRuntimeAllocatorIfCreated() { return defaultRuntimeAllocatorSlot().load(); }
/** @brief 对调用者缓冲区加锁的分配区域。 */
class BufferRuntimeArena : public RuntimeAllocator {
    mutable std::mutex m_mutex;
    void *m_buffer;
    std::size_t m_bytes;
    MemoryPool m_pool{};
    Stats m_last{};
public:
    /** @brief 连接外部存储。 @param buffer 存储地址。 @param bytes 容量。 */
    BufferRuntimeArena(void *buffer, std::size_t bytes) : m_buffer(buffer), m_bytes(bytes) {
        if (runtimeMemoryStatsAvailable && !memoryPoolInit(&m_pool, buffer, bytes)) throw std::bad_alloc();
    }
    BufferRuntimeArena(const BufferRuntimeArena &) = delete;
    BufferRuntimeArena &operator=(const BufferRuntimeArena &) = delete;
    std::size_t capacity() const { return m_bytes; }
    void *allocate(std::size_t bytes) override { return allocateAligned(bytes, alignof(std::max_align_t)); }
    void *allocateAligned(std::size_t bytes, std::size_t alignment) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        return runtimeMemoryStatsAvailable ? memoryPoolAllocate(&m_pool, bytes, alignment) : memoryHeapAllocate(bytes, alignment);
    }
    bool release(void *p) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!runtimeMemoryStatsAvailable) { memoryHeapRelease(p); return true; }
        return memoryPoolRelease(&m_pool, p) != 0;
    }
    bool owns(const void *p) const override { return memoryPoolOwns(&m_pool, p) != 0; }
    Stats stats() const override {
        std::lock_guard<std::mutex> lock(m_mutex);
        return {m_pool.stats.liveBytes, m_pool.stats.liveBlocks, m_pool.stats.peakBytes, m_pool.stats.totalBlocks};
    }
    /** @brief 获取上次结束会话的统计。 @return 保存的统计。 */
    Stats lastRunStats() const { return m_last; }
    /** @brief 查询上次会话峰值。 @return 占用字节数。 */
    std::size_t peakBytes() const { return m_last.peakBytes; }
    /** @brief 要求无活动块后清零会话存储，并保存统计。 */
    void finish() {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_pool.storage) return;
        if (m_pool.stats.liveBlocks) {
            std::fputs("运行时分配器：仍有活动块，不能清零\n", stderr);
            std::abort();
        }
        MemoryPoolStats saved{};
        memoryPoolFinish(&m_pool, &saved);
        m_last = {saved.liveBytes, saved.liveBlocks, saved.peakBytes, saved.totalBlocks};
    }
    /** @brief 开始或恢复无活动分配的会话。 */
    void begin() {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_pool.stats.liveBlocks) throw std::bad_alloc();
        if (runtimeMemoryStatsAvailable && !m_pool.storage && !memoryPoolInit(&m_pool, m_buffer, m_bytes)) throw std::bad_alloc();
    }
};
/** @brief 从宿主堆一次保留连续缓冲区的分配区域。 */
class DynamicRuntimeArena final : public BufferRuntimeArena {
    void *m_storage;
    static void *reserve(std::size_t bytes) {
        if (!runtimeMemoryStatsAvailable) return nullptr;
        if (bytes < 64) throw std::bad_alloc();
        void *p = std::malloc(bytes);
        if (!p) throw std::bad_alloc();
        return p;
    }
    DynamicRuntimeArena(void *storage, std::size_t bytes) : BufferRuntimeArena(storage, bytes), m_storage(storage) {}
public:
    /** @brief 从宿主堆保留存储。 @param bytes 容量，默认 512 MiB。 */
    explicit DynamicRuntimeArena(std::size_t bytes = defaultRuntimeCapacity) : DynamicRuntimeArena(reserve(bytes), bytes) {}
    ~DynamicRuntimeArena() override { finish(); std::free(m_storage); }
};
inline RuntimeAllocator *defaultRuntimeAllocator() {
    // Deliberately survives static container destruction; no global new/delete interception.
    alignas(DynamicRuntimeArena) static unsigned char storage[sizeof(DynamicRuntimeArena)];
    static auto *arena = [] {
        auto *result = new (storage) DynamicRuntimeArena();
        defaultRuntimeAllocatorSlot().store(result);
        return result;
    }();
    return arena;
}
/** @brief 会话守卫；退出时恢复分配器，并要求所有会话分配均已释放。 */
class RuntimeSession {
    BufferRuntimeArena &m_arena;
    RuntimeAllocator *m_previous;
public:
    /** @brief 建立会话并保存原分配器。 @param arena 存活时间覆盖本会话的区域。 */
    explicit RuntimeSession(BufferRuntimeArena &arena) : m_arena(arena), m_previous(currentRuntimeAllocator()) {
        m_arena.begin(); setRuntimeAllocator(&m_arena);
    }
    ~RuntimeSession() { setRuntimeAllocator(m_previous); m_arena.finish(); }
    RuntimeSession(const RuntimeSession &) = delete;
    RuntimeSession &operator=(const RuntimeSession &) = delete;
};
/** @brief 仅切换当前线程分配器的作用域守卫。 */
class AllocationScope {
    RuntimeAllocator *m_previous;
public:
    /** @brief 暂时切换分配器。 @param owner 新分配器。 */
    explicit AllocationScope(RuntimeAllocator *owner) : m_previous(currentRuntimeAllocator()) { setRuntimeAllocator(owner); }
    ~AllocationScope() { setRuntimeAllocator(m_previous); }
    AllocationScope(const AllocationScope &) = delete;
    AllocationScope &operator=(const AllocationScope &) = delete;
};
/** @brief 内置固定容量缓冲区的分配区域。 */
template<std::size_t Capacity> class StaticRuntimeArena final : public RuntimeAllocator {
    static_assert(Capacity >= 64, "runtime arena capacity must be at least 64 bytes");
    alignas(std::max_align_t) unsigned char m_storage[Capacity];
    MemoryPool m_pool{};
    mutable std::mutex m_mutex;  /* 静态 arena 可能被并发的 VM 线程共用，和 BufferRuntimeArena 一样加锁 */
public:
    StaticRuntimeArena() { reset(); }
    /** @brief 在无活动分配时重置区域。 */
    void reset() {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_pool.stats.liveBlocks) {
            std::fputs("运行时分配器：仍有活动块，不能重置\n", stderr);
            std::abort();
        }
        memoryPoolInit(&m_pool, m_storage, Capacity);
    }
    std::size_t capacity() const { return m_pool.capacity; }
    void *allocate(std::size_t bytes) override { return allocateAligned(bytes, alignof(std::max_align_t)); }
    void *allocateAligned(std::size_t bytes, std::size_t alignment) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        return runtimeMemoryStatsAvailable ? memoryPoolAllocate(&m_pool, bytes, alignment) : memoryHeapAllocate(bytes, alignment);
    }
    bool release(void *p) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!runtimeMemoryStatsAvailable) { memoryHeapRelease(p); return true; }
        return memoryPoolRelease(&m_pool, p) != 0;
    }
    bool owns(const void *p) const override {
        std::lock_guard<std::mutex> lock(m_mutex);
        return memoryPoolOwns(&m_pool, p) != 0;
    }
    Stats stats() const override {
        std::lock_guard<std::mutex> lock(m_mutex);
        return {m_pool.stats.liveBytes, m_pool.stats.liveBlocks, m_pool.stats.peakBytes, m_pool.stats.totalBlocks};
    }
};
}
namespace Memory {
/** @brief 向指定区域分配，失败抛出 bad_alloc。 @param owner 区域或 nullptr。 @param bytes 容量。 @param alignment 对齐量。 @return 有效地址。 */
inline void *allocate(Memory::RuntimeAllocator *owner, std::size_t bytes, std::size_t alignment) {
    if (!owner) owner = Memory::defaultRuntimeAllocator();
    void *p = owner->allocateAligned(bytes, alignment);
    if (!p) {
        if (owner) std::fprintf(stderr, "运行时分配器：arena 用尽（要 %zu 字节）\n", bytes);
        throw std::bad_alloc();
    }
    return p;
}
/** @brief 向原区域归还分配，无效释放会终止进程。 @param owner 所属区域。 @param p 待归还地址。 */
inline void release(Memory::RuntimeAllocator *owner, void *p) noexcept {
    if (!p) return;
    if (!owner) owner = Memory::defaultRuntimeAllocator();
    if (!owner->release(p)) {
        std::fputs("运行时分配器：无效或重复释放 arena 内存\n", stderr);
        std::abort();
    }
}
/** @brief 为标准容器记录分配归属。 @tparam T 元素类型。 */
template<class T> class Allocator {
public:
    using value_type = T;
    using propagate_on_container_move_assignment = std::true_type;
    using propagate_on_container_swap = std::true_type;
    using propagate_on_container_copy_assignment = std::false_type;
    Memory::RuntimeAllocator *owner = Memory::currentRuntimeAllocator();
    Allocator() noexcept = default;
    explicit Allocator(Memory::RuntimeAllocator *p) noexcept : owner(p) {}
    template<class U> Allocator(const Allocator<U> &other) noexcept : owner(other.owner) {}
    Allocator select_on_container_copy_construction() const noexcept { return Allocator(); }
    T *allocate(std::size_t count) {
        if (count > std::numeric_limits<std::size_t>::max() / sizeof(T)) throw std::bad_array_new_length();
        return static_cast<T *>(Memory::allocate(owner, count * sizeof(T), alignof(T)));
    }
    void deallocate(T *p, std::size_t) noexcept { Memory::release(owner, p); }
    template<class U> bool operator==(const Allocator<U> &other) const noexcept { return owner == other.owner; }
    template<class U> bool operator!=(const Allocator<U> &other) const noexcept { return !(*this == other); }
};
/** @brief 同时保存对象析构操作与存储归属的删除器。 @tparam T 对象类型。 */
template<class T> struct Deleter {
    Memory::RuntimeAllocator *owner = nullptr;
    void *allocation = nullptr;
    void (*destroy)(void *) = nullptr;
    Deleter() = default;
    Deleter(Memory::RuntimeAllocator *o, void *p, void (*d)(void *)) : owner(o), allocation(p), destroy(d) {}
    template<class U> Deleter(const Deleter<U> &d) noexcept : owner(d.owner), allocation(d.allocation), destroy(d.destroy) {}
    void operator()(T *p) const noexcept {
        if (!p) return;
        if (!destroy) std::abort();
        destroy(allocation); Memory::release(owner, allocation);
    }
};
#ifdef MEMORY_CUSTOM_ALLOCATOR
template<class T> using ContainerAllocator = Allocator<T>;
template<class T> using UniquePtr = std::unique_ptr<T, Deleter<T>>;
/** @brief 在当前区域构造独占对象。 @tparam T 对象类型。 @param args 构造参数。 @return 独占指针。 */
template<class T, class... Args> UniquePtr<T> makeUnique(Args&&... args) {
    auto *owner = Memory::runtimeAllocator();
    void *storage = Memory::allocate(owner, sizeof(T), alignof(T));
    try {
        T *object = new (storage) T(std::forward<Args>(args)...);
        return UniquePtr<T>(object, Deleter<T>(owner, storage, [](void *p) { static_cast<T *>(p)->~T(); }));
    } catch (...) { Memory::release(owner, storage); throw; }
}
/** @brief 在当前区域构造共享对象及控制块。 @tparam T 对象类型。 @param args 构造参数。 @return 共享指针。 */
template<class T, class... Args> std::shared_ptr<T> makeShared(Args&&... args) { return std::allocate_shared<T>(Allocator<T>(), std::forward<Args>(args)...); }
#else
template<class T> using ContainerAllocator = std::allocator<T>;
template<class T> using UniquePtr = std::unique_ptr<T>;
/** @brief 在当前区域构造独占对象。 @tparam T 对象类型。 @param args 构造参数。 @return 独占指针。 */
template<class T, class... Args> UniquePtr<T> makeUnique(Args&&... args) { return std::make_unique<T>(std::forward<Args>(args)...); }
/** @brief 在当前区域构造共享对象及控制块。 @tparam T 对象类型。 @param args 构造参数。 @return 共享指针。 */
template<class T, class... Args> std::shared_ptr<T> makeShared(Args&&... args) { return std::make_shared<T>(std::forward<Args>(args)...); }
#endif
#ifdef MEMORY_CUSTOM_ALLOCATOR
template<class Signature> class Function;
/** @brief 使用自定义分配器保存可复制调用目标。 @tparam R 返回类型。 @tparam Args 参数类型。 */
template<class R, class... Args> class Function<R(Args...)> {
    struct Callable {
        virtual ~Callable() = default;
        virtual R call(Args... args) = 0;
        virtual std::shared_ptr<Callable> clone() const = 0;
    };
    template<class F> struct Holder final : Callable {
        F function;
        explicit Holder(F f) : function(std::move(f)) {}
        R call(Args... args) override { return std::invoke(function, std::forward<Args>(args)...); }
        std::shared_ptr<Callable> clone() const override { return makeShared<Holder<F>>(function); }
    };
    std::shared_ptr<Callable> m_callable;
public:
    Function() = default;
    Function(std::nullptr_t) {}
    Function(const Function &f) : m_callable(f.m_callable ? f.m_callable->clone() : nullptr) {}
    Function(Function &&) noexcept = default;
    Function &operator=(const Function &f) { Function copy(f); m_callable.swap(copy.m_callable); return *this; }
    Function &operator=(Function &&) noexcept = default;
    Function &operator=(std::nullptr_t) { m_callable.reset(); return *this; }
    template<class F, class = std::enable_if_t<!std::is_same_v<std::decay_t<F>, Function> && std::is_invocable_r_v<R, F&, Args...>>>
    Function(F f) : m_callable(makeShared<Holder<std::decay_t<F>>>(std::move(f))) {}
    explicit operator bool() const noexcept { return bool(m_callable); }
    friend bool operator==(const Function &f, std::nullptr_t) noexcept { return !f.m_callable; }
    friend bool operator!=(const Function &f, std::nullptr_t) noexcept { return bool(f.m_callable); }
    friend bool operator==(std::nullptr_t, const Function &f) noexcept { return !f.m_callable; }
    friend bool operator!=(std::nullptr_t, const Function &f) noexcept { return bool(f.m_callable); }
    R operator()(Args... args) const {
        if (!m_callable) throw std::bad_function_call();
        return m_callable->call(std::forward<Args>(args)...);
    }
};
#else
template<class T> using Function = std::function<T>;
#endif
template<class T> using Vector = std::vector<T, ContainerAllocator<T>>;
template<class T> using Deque = std::deque<T, ContainerAllocator<T>>;
template<class T> using List = std::list<T, ContainerAllocator<T>>;
template<class K, class V, class C = std::less<K>> using Map = std::map<K, V, C, ContainerAllocator<std::pair<const K, V>>>;
template<class K, class C = std::less<K>> using Set = std::set<K, C, ContainerAllocator<K>>;
template<class K, class V, class H = std::hash<K>, class E = std::equal_to<K>> using UnorderedMap = std::unordered_map<K, V, H, E, ContainerAllocator<std::pair<const K, V>>>;
template<class K, class H = std::hash<K>, class E = std::equal_to<K>> using UnorderedSet = std::unordered_set<K, H, E, ContainerAllocator<K>>;
#ifdef MEMORY_CUSTOM_ALLOCATOR
/** @brief 使用自定义分配器的字符串，兼容生成解析器所需操作。 */
class String : public std::basic_string<char, std::char_traits<char>, Allocator<char>> {
    using Base = std::basic_string<char, std::char_traits<char>, Allocator<char>>;
public:
    using Base::Base;
    using Base::operator=;
    String() = default;
    String(const String &) = default;
    String(String &&) noexcept = default;
    String &operator=(const String &) = default;
    String &operator=(String &&) noexcept = default;
    String(const Base &s) : Base(s) {}
    String(Base &&s) : Base(std::move(s)) {}
    String(const std::string &s) : Base(s.data(), s.size()) {}
    String(std::string_view s) : Base(s.data(), s.size()) {}
    operator std::string() const { return std::string(data(), size()); }
    operator std::filesystem::path() const { return std::filesystem::u8path(data(), data() + size()); }
    String substr(std::size_t pos = 0, std::size_t count = Base::npos) const { return String(Base::substr(pos, count)); }
    friend String operator+(String a, const String &b) { a.append(b); return a; }
    friend String operator+(String a, const char *b) { a.append(b); return a; }
    friend String operator+(const char *a, const String &b) { String r(a); r.append(b); return r; }
    friend String operator+(String a, const std::string &b) { a.append(b.data(), b.size()); return a; }
    friend String operator+(const std::string &a, const String &b) { String r(a); r.append(b); return r; }
    friend String operator+(String a, char b) { a.push_back(b); return a; }
    friend String operator+(char a, const String &b) { String r(1, a); r.append(b); return r; }
};
#else
using String = std::string;
#endif
template<class T> String toString(T value) { return String(std::to_string(value)); }
inline std::filesystem::path u8path(const String &text) { return std::filesystem::u8path(text.data(), text.data() + text.size()); }
#ifdef MEMORY_CUSTOM_ALLOCATOR
template<class Stream> class TextStream : public Stream {
public:
    using Stream::Stream;
    using Stream::str;
    String str() const { return String(Stream::str()); }
};
using OutputStringStream = TextStream<std::basic_ostringstream<char, std::char_traits<char>, Allocator<char>>>;
using InputStringStream = TextStream<std::basic_istringstream<char, std::char_traits<char>, Allocator<char>>>;
using StringStream = TextStream<std::basic_stringstream<char, std::char_traits<char>, Allocator<char>>>;
#else
using OutputStringStream = std::ostringstream;
using InputStringStream = std::istringstream;
using StringStream = std::stringstream;
#endif
/** @brief 获取使用默认长生命周期区域的容器分配器。 @tparam T 元素类型。 @return 分配器。 */
template<class T> ContainerAllocator<T> persistentAllocator() {
#ifdef MEMORY_CUSTOM_ALLOCATOR
    return ContainerAllocator<T>(Memory::defaultRuntimeAllocator());
#else
    return ContainerAllocator<T>();
#endif
}
}
#ifdef MEMORY_CUSTOM_ALLOCATOR
namespace std {
template<> struct hash<Memory::String> {
    size_t operator()(const Memory::String &s) const noexcept { return hash<string_view>()(string_view(s.data(), s.size())); }
};
}
#endif
#endif
