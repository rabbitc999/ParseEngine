/**
 * @file toml.hpp
 * @brief 现代 C++17 的 TOML v1.0 库：解析、序列化，以及可直接喂给 STL 算法的迭代器。
 *
 * 设计仿照同目录的 JSON 库（`library/json`），把"配置数据"当成一等公民：
 *
 *   - **存储**用 `std::variant`：bool / int64 / double / string / datetime / array / table；
 *   - **表保序**：`Object` 是"插入顺序的 vector<Member>"，成员是公开字段，所以
 *     `for (auto& [key, value] : table)` 直接可用（结构化绑定）；
 *   - **迭代器**：`Array` / `Object` 暴露标准迭代器，`Value` 自己也能迭代；
 *   - **现代特性**：`std::optional` / `std::string_view` / `std::variant` /
 *     `std::visit` / `if constexpr` / `std::from_chars` / 结构化绑定 / `[[nodiscard]]`。
 *
 * 覆盖 TOML v1.0 的主要语法：基本 / 字面量 / 多行字符串（含行尾反斜杠折行）、
 * 整数（十 / 十六 / 八 / 二进制，允许 `_` 分隔）、浮点（含 `inf` / `nan`）、布尔、
 * 四种日期时间（带偏移 / 本地 / 纯日期 / 纯时间）、数组、内联表、标准表、表数组、
 * 点分键、`#` 注释与 `\uXXXX` / `\UXXXXXXXX` 转义。语义动作之外还会做定义冲突
 * 检查（重复键、表被重定义、内联表被追加、点分键表被具名表头重开等）。
 *
 * 只依赖 C++17 标准库，不参与 ParseEngine 主程序的构建。
 *
 * 快速看一眼用法：
 *
 * ```cpp
 * const toml::ParseResult doc = toml::parse(R"(
 *     [server]
 *     host = "localhost"
 *     port = 8080
 *     tags = ["prod", "edge"]
 * )");
 * if (!doc.ok()) { ... }
 *
 * const toml::Object& root = doc.value->asTable();          // 顶层表
 * for (const auto& [key, value] : root) { ... }             // 结构化绑定遍历
 *
 * const toml::Object& server = root.at("server").asTable();
 * const auto port = server.at("port").asInt().value_or(0);
 * ```
 */

#ifndef PARSE_ENGINE_TOML_HPP
#define PARSE_ENGINE_TOML_HPP

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace toml
{

class Value;

/** 数组：`std::vector<Value>`，标准迭代器，STL 算法直接可用。 */
using Array = std::vector<Value>;

/**
 * @brief 日期 / 时间的种类。
 */
enum class DateTimeKind
{
    OffsetDateTime, ///< 带时区偏移的日期时间，例如 `1979-05-27T07:32:00Z`。
    LocalDateTime,  ///< 本地日期时间，例如 `1979-05-27T07:32:00`。
    LocalDate,      ///< 本地日期，例如 `1979-05-27`。
    LocalTime,      ///< 本地时间，例如 `07:32:00`。
};

/**
 * @brief TOML 的日期 / 时间值。
 *
 * 一个结构体覆盖四种形态，用 `kind` 区分：日期时间（带 / 不带偏移）、纯日期、
 * 纯时间。`offsetMinutes` 只在 `OffsetDateTime` 时有效，东八区是 `+480`。
 */
struct DateTime
{
    DateTimeKind kind = DateTimeKind::OffsetDateTime; ///< 形态。
    int year = 0;                                     ///< 年。
    int month = 0;                                    ///< 月（1..12）。
    int day = 0;                                      ///< 日（1..31）。
    int hour = 0;                                     ///< 时（0..23）。
    int minute = 0;                                   ///< 分（0..59）。
    int second = 0;                                   ///< 秒（0..60，允许闰秒 60）。
    std::uint32_t nanosecond = 0;                     ///< 秒的小数部分，单位纳秒。
    int offsetMinutes = 0;                            ///< 时区偏移（分钟），东为正。

    /**
     * @brief 两个日期时间相等。
     * @param left 左。
     * @param right 右。
     * @return 相等返回 true。
     */
    friend bool operator==(const DateTime &left, const DateTime &right) noexcept;

    /**
     * @brief 两个日期时间不相等。
     * @param left 左。
     * @param right 右。
     * @return 不相等返回 true。
     */
    friend bool operator!=(const DateTime &left, const DateTime &right) noexcept
    {
        return !(left == right);
    }

    /**
     * @brief 转成 ISO 8601 / RFC 3339 文本。
     * @return 文本，例如 `1979-05-27T07:32:00.5Z`。
     */
    [[nodiscard]] std::string toString() const;
};

/** 表（TOML 把对象叫 table）：与 JSON 的 Object 同义。 */
struct Member;

/**
 * @brief TOML 表：**保持插入顺序**的成员表。
 *
 * 查找是线性的（配置这类场景成员不多，保序比 O(1) 更值）；迭代顺序就是插入顺序
 * （解析时即文件里的先后顺序）。`Table` 是它的同义词。
 */
class Object
{
public:
    using value_type = Member;
    using container_type = std::vector<Member>;
    using iterator = container_type::iterator;
    using const_iterator = container_type::const_iterator;
    using size_type = std::size_t;

    /** 开始迭代。 */
    iterator begin() noexcept;
    /** 结束迭代。 */
    iterator end() noexcept;
    /** 开始迭代（只读）。 */
    const_iterator begin() const noexcept;
    /** 结束迭代（只读）。 */
    const_iterator end() const noexcept;
    /** 开始迭代（只读）。 */
    const_iterator cbegin() const noexcept;
    /** 结束迭代（只读）。 */
    const_iterator cend() const noexcept;

    /**
     * @brief 是否没有成员。
     * @return 空返回 true。
     */
    [[nodiscard]] bool empty() const noexcept;

    /**
     * @brief 成员个数。
     * @return 个数。
     */
    [[nodiscard]] size_type size() const noexcept;

    /** 清空。 */
    void clear() noexcept;

    /**
     * @brief 查找成员。
     * @param key 成员名。
     * @return 迭代器；没找到返回 `end()`。
     */
    [[nodiscard]] iterator find(std::string_view key) noexcept;

    /**
     * @brief 查找成员（只读）。
     * @param key 成员名。
     * @return 迭代器；没找到返回 `end()`。
     */
    [[nodiscard]] const_iterator find(std::string_view key) const noexcept;

    /**
     * @brief 是否包含某个成员。
     * @param key 成员名。
     * @return 包含返回 true。
     */
    [[nodiscard]] bool contains(std::string_view key) const noexcept;

    /**
     * @brief 成员个数（STL 风格，0 或 1）。
     * @param key 成员名。
     * @return 0 或 1。
     */
    [[nodiscard]] size_type count(std::string_view key) const noexcept;

    /**
     * @brief 取成员值，不存在就插入一个空表。
     * @param key 成员名。
     * @return 成员值的引用。
     */
    Value &operator[](std::string_view key);

    /**
     * @brief 取成员值（只读）。
     * @param key 成员名。
     * @return 值的指针；不存在返回 nullptr。
     */
    [[nodiscard]] const Value *tryGet(std::string_view key) const noexcept;

    /**
     * @brief 取成员值（可写）。
     * @param key 成员名。
     * @return 值的指针；不存在返回 nullptr。
     */
    [[nodiscard]] Value *tryGet(std::string_view key) noexcept;

    /**
     * @brief 取成员值（只读），不存在就抛 `std::out_of_range`。
     * @param key 成员名。
     * @return 值的引用。
     * @throws std::out_of_range 没有这个成员。
     */
    [[nodiscard]] const Value &at(std::string_view key) const;

    /**
     * @brief 插入或覆盖一个成员。
     * @param key 成员名。
     * @param value 值。
     * @return 成员引用。
     */
    Member &insert(std::string key, Value value);

    /**
     * @brief 就地构造一个成员（存在同名成员时覆盖它的值）。
     * @param key 成员名。
     * @param args 值的构造参数。
     * @return 成员引用。
     */
    template <typename... Args>
    Member &emplace(std::string key, Args &&...args)
    {
        return insert(std::move(key), Value(std::forward<Args>(args)...));
    }

    /**
     * @brief 删除一个成员。
     * @param key 成员名。
     * @return 删掉了返回 true。
     */
    bool erase(std::string_view key) noexcept;

    /**
     * @brief 成员表（只读），需要原样传给别的接口时用。
     * @return 成员表引用。
     */
    [[nodiscard]] const container_type &members() const noexcept;

private:
    /** 值需要直接访问成员表（迭代器要用）。 */
    friend class Value;

    container_type members_; ///< 按插入顺序保存的成员。
};

/** 表：`Object` 的同义词（TOML 术语）。 */
using Table = Object;

/**
 * @brief TOML 值。
 *
 * 顶层通常是一张表；数组元素可以是任意值（TOML 1.0 允许数组混合类型）。
 */
class Value
{
public:
    /** 底层存储：variant 覆盖 TOML 的全部类型。 */
    using Storage = std::variant<bool, std::int64_t, double, std::string, DateTime, Array, Object>;

    /** 构造：默认是一张空表（TOML 没有 null，空表是最自然的占位）。 */
    Value() noexcept : storage_(Object{}) {}

    /** 构造：布尔。 */
    Value(bool value) : storage_(value) {}

    /** 构造：整数（`int` / `long` 等都会走到这里）。 */
    Value(int value) : storage_(static_cast<std::int64_t>(value)) {}

    /** 构造：64 位整数。 */
    Value(std::int64_t value) : storage_(value) {}

    /** 构造：浮点。 */
    Value(double value) : storage_(value) {}

    /** 构造：字符串。 */
    Value(const char *value) : storage_(std::string(value == nullptr ? "" : value)) {}

    /** 构造：字符串。 */
    Value(std::string value) : storage_(std::move(value)) {}

    /** 构造：字符串视图。 */
    Value(std::string_view value) : storage_(std::string(value)) {}

    /** 构造：日期时间。 */
    Value(DateTime value) : storage_(value) {}

    /** 构造：数组。 */
    Value(Array value) : storage_(std::move(value)) {}

    /** 构造：表。 */
    Value(Object value) : storage_(std::move(value)) {}

    /**
     * @brief 用初始化列表构造数组：`Value::array({1, 2, 3})`。
     * @param items 元素。
     * @return 数组值。
     */
    static Value array(std::initializer_list<Value> items);

    /**
     * @brief 用初始化列表构造表：`Value::table({{"a", 1}, {"b", "x"}})`。
     * @param items 成员。
     * @return 表值。
     */
    static Value table(std::initializer_list<std::pair<std::string, Value>> items);

    // ---- 类型判断 ---------------------------------------------------------

    /** 是不是布尔。 */
    [[nodiscard]] bool isBool() const noexcept { return std::holds_alternative<bool>(storage_); }
    /** 是不是整数。 */
    [[nodiscard]] bool isInt() const noexcept { return std::holds_alternative<std::int64_t>(storage_); }
    /** 是不是浮点。 */
    [[nodiscard]] bool isDouble() const noexcept { return std::holds_alternative<double>(storage_); }
    /** 是不是数字（整数或浮点）。 */
    [[nodiscard]] bool isNumber() const noexcept { return isInt() || isDouble(); }
    /** 是不是字符串。 */
    [[nodiscard]] bool isString() const noexcept { return std::holds_alternative<std::string>(storage_); }
    /** 是不是日期时间。 */
    [[nodiscard]] bool isDateTime() const noexcept { return std::holds_alternative<DateTime>(storage_); }
    /** 是不是数组。 */
    [[nodiscard]] bool isArray() const noexcept { return std::holds_alternative<Array>(storage_); }
    /** 是不是表。 */
    [[nodiscard]] bool isTable() const noexcept { return std::holds_alternative<Object>(storage_); }
    /** 是不是表（`isTable` 的同义词，沿用 JSON 库的叫法）。 */
    [[nodiscard]] bool isObject() const noexcept { return isTable(); }

    /**
     * @brief 类型名（诊断 / 统计用）。
     * @return `bool` / `int` / `double` / `string` / `datetime` / `array` / `table`。
     */
    [[nodiscard]] const char *typeName() const noexcept;

    // ---- 取值（std::optional，取不到就是空） -------------------------------

    /** 取布尔。 */
    [[nodiscard]] std::optional<bool> asBool() const noexcept;
    /** 取整数（浮点且是整数时也能取到）。 */
    [[nodiscard]] std::optional<std::int64_t> asInt() const noexcept;
    /** 取浮点（整数会转成浮点）。 */
    [[nodiscard]] std::optional<double> asDouble() const noexcept;
    /** 取字符串视图（指向内部存储，注意生命周期）。 */
    [[nodiscard]] std::optional<std::string_view> asString() const noexcept;
    /** 取日期时间；不是日期时间返回空。 */
    [[nodiscard]] std::optional<DateTime> asDateTime() const noexcept;

    /**
     * @brief 取字符串，不是字符串就用兜底值。
     * @param fallback 兜底。
     * @return 字符串视图。
     */
    [[nodiscard]] std::string_view stringOr(std::string_view fallback) const noexcept;

    /**
     * @brief 取整数，取不到就用兜底值。
     * @param fallback 兜底。
     * @return 整数。
     */
    [[nodiscard]] std::int64_t intOr(std::int64_t fallback) const noexcept;

    /**
     * @brief 取浮点，取不到就用兜底值。
     * @param fallback 兜底。
     * @return 浮点。
     */
    [[nodiscard]] double doubleOr(double fallback) const noexcept;

    /** 取数组；不是数组返回 nullptr。 */
    [[nodiscard]] Array *asArray() noexcept;
    /** 取数组（只读）。 */
    [[nodiscard]] const Array *asArray() const noexcept;
    /** 取表；不是表返回 nullptr。 */
    [[nodiscard]] Object *asTable() noexcept;
    /** 取表（只读）。 */
    [[nodiscard]] const Object *asTable() const noexcept;
    /** 取表；`asTable` 的同义词。 */
    [[nodiscard]] Object *asObject() noexcept;
    /** 取表（只读）；`asTable` 的同义词。 */
    [[nodiscard]] const Object *asObject() const noexcept;

    /**
     * @brief 取数组（可写），不是数组就抛 `std::domain_error`。
     * @return 数组引用。
     * @throws std::domain_error 当前值不是数组。
     */
    [[nodiscard]] Array &requireArray();

    /**
     * @brief 取数组（只读），不是数组就抛 `std::domain_error`。
     * @return 数组引用。
     * @throws std::domain_error 当前值不是数组。
     */
    [[nodiscard]] const Array &requireArray() const;

    /**
     * @brief 取表（可写），不是表就抛 `std::domain_error`。
     * @return 表引用。
     * @throws std::domain_error 当前值不是表。
     */
    [[nodiscard]] Object &requireTable();

    /**
     * @brief 取表（只读），不是表就抛 `std::domain_error`。
     * @return 表引用。
     * @throws std::domain_error 当前值不是表。
     */
    [[nodiscard]] const Object &requireTable() const;

    // ---- 访问与修改 -------------------------------------------------------

    /**
     * @brief 取数组元素。
     * @param index 下标。
     * @return 元素引用。
     * @throws std::domain_error 不是数组。
     * @throws std::out_of_range 下标越界。
     */
    Value &at(std::size_t index);

    /**
     * @brief 取数组元素（只读）。
     * @param index 下标。
     * @return 元素引用。
     * @throws std::domain_error 不是数组。
     * @throws std::out_of_range 下标越界。
     */
    [[nodiscard]] const Value &at(std::size_t index) const;

    /**
     * @brief 取表成员；不存在就插入一个空表。
     * @param key 成员名。
     * @return 成员值引用。
     * @throws std::domain_error 当前值不是表。
     */
    Value &operator[](std::string_view key);

    /**
     * @brief 查找表成员。
     * @param key 成员名。
     * @return 指针；不是表或没找到返回 nullptr。
     */
    [[nodiscard]] const Value *find(std::string_view key) const noexcept;

    /**
     * @brief 查找表成员（可写）。
     * @param key 成员名。
     * @return 指针；不是表或没找到返回 nullptr。
     */
    [[nodiscard]] Value *find(std::string_view key) noexcept;

    /**
     * @brief 追加一个数组元素。
     * @param value 元素。
     * @return 新元素的引用。
     */
    Value &push_back(Value value);

    /**
     * @brief 就地追加一个数组元素。
     * @param args 元素构造参数。
     * @return 新元素的引用。
     */
    template <typename... Args>
    Value &emplace_back(Args &&...args)
    {
        Array &items = requireArray();
        items.emplace_back(std::forward<Args>(args)...);
        return items.back();
    }

    /**
     * @brief 元素个数（数组 / 表）。
     * @return 个数；标量返回 0。
     */
    [[nodiscard]] std::size_t size() const noexcept;

    /**
     * @brief 是否为空（数组 / 表 / 空字符串）。
     * @return 空返回 true。
     */
    [[nodiscard]] bool empty() const noexcept;

    /** 清空数组 / 表（标量重置成空表）。 */
    void clear() noexcept;

    /**
     * @brief 按路径取值：`a.b[0].c`。
     *
     * 路径语法：名字用 `.` 连接，数组下标写 `[n]`；`[n]` 也可以直接跟在根上
     * （`[0].name`）。找不到任何一段就返回 nullptr。
     *
     * @param path 路径。
     * @return 值的指针；找不到返回 nullptr。
     */
    [[nodiscard]] const Value *atPath(std::string_view path) const noexcept;

    // ---- 迭代 -------------------------------------------------------------

    /**
     * @brief 迭代时的引用代理。
     *
     * 数组元素：`index` 有值、`key` 为空；表成员：`key` 有值。
     * 可以隐式转成 `Value&`，所以 `for (auto& item : value)` 里可以直接当值用。
     */
    class Reference
    {
    public:
        Value *value = nullptr;                             ///< 指向元素 / 成员值。
        std::string_view key;                               ///< 表成员名；数组元素为空。
        std::size_t index = static_cast<std::size_t>(-1);   ///< 数组下标；表成员为 npos。

        /** 隐式转成 Value 引用，方便直接当值用。 */
        operator Value &() const noexcept { return *value; }

        /** 取值的引用。 */
        [[nodiscard]] Value &get() const noexcept { return *value; }
    };

    /**
     * @brief 通用迭代器：数组走元素、表走成员。
     *
     * `operator*` 返回内部缓存的 `Reference&`，所以 `for (auto& item : value)`
     * 这种写法也能工作（range-for 里绑的是引用）。
     */
    class iterator
    {
    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = Reference;
        using difference_type = std::ptrdiff_t;
        using pointer = Reference *;
        using reference = Reference &;

        /** 构造一个空的（end）迭代器。 */
        iterator() = default;

        /** 解引用：返回缓存的引用代理。 */
        reference operator*() const;

        /** 成员访问。 */
        pointer operator->() const { return &operator*(); }

        /** 前置自增。 */
        iterator &operator++();

        /** 后置自增。 */
        iterator operator++(int)
        {
            iterator copy = *this;
            ++(*this);
            return copy;
        }

        /** 相等比较。 */
        friend bool operator==(const iterator &left, const iterator &right) noexcept
        {
            return (left.owner_ == right.owner_) && (left.position_ == right.position_);
        }

        /** 不等比较。 */
        friend bool operator!=(const iterator &left, const iterator &right) noexcept
        {
            return !(left == right);
        }

    private:
        /** 被迭代的值（数组或表）。 */
        Value *owner_ = nullptr;

        /** 在容器里的位置（数组下标 / 表成员序号）。 */
        std::size_t position_ = 0u;

        /** `operator*` 的返回值缓存（range-for 要绑引用，所以必须缓存）。 */
        mutable Reference cache_{};

        friend class Value;
    };

    /**
     * 开始迭代：数组 → 元素，表 → 成员，标量 → 空区间。
     *
     * 只提供非 const 版本：迭代给出的引用是**可写**的。只读遍历用
     * `asArray()` / `asTable()`（它们有 const 重载，给的是 `const` 引用）。
     */
    [[nodiscard]] iterator begin() noexcept;

    /** 结束迭代。 */
    [[nodiscard]] iterator end() noexcept;

    // ---- 序列化与比较 -----------------------------------------------------

    /**
     * @brief 序列化成 TOML 文本。
     *
     * 顶层是表时输出完整文档：先写标量 / 数组成员，再写 `[子表]` 与 `[[表数组]]`；
     * 顶层不是表时输出该值的内联写法（例如数组 / 内联表）。
     *
     * @return TOML 文本。
     */
    [[nodiscard]] std::string dump() const;

    /**
     * @brief 内联写法（用于数组元素 / 内联表成员 / 调试）。
     * @return 文本，例如 `{a = 1, b = [2, 3]}`。
     */
    [[nodiscard]] std::string inlineDump() const;

    /**
     * @brief 底层存储（需要 `std::visit` 自己分派时用）。
     * @return variant 引用。
     */
    [[nodiscard]] const Storage &storage() const noexcept { return storage_; }

    /** 相等比较（按 TOML 语义：数字 1 与 1.0 视为相等；表按成员集合比）。 */
    friend bool operator==(const Value &left, const Value &right) noexcept;

    /** 不等比较。 */
    friend bool operator!=(const Value &left, const Value &right) noexcept
    {
        return !(left == right);
    }

private:
    Storage storage_; ///< 值本体。
};

/**
 * @brief 表的一个成员：名字 + 值。
 *
 * 两个字段都是公开的，所以可以 `for (auto& [key, value] : table)` 直接解构。
 * 定义放在 `Value` 之后：它按值持有 `Value`，而 `Value` 又要把 `Object` 放进
 * `std::variant`，两者只能这样排序（`std::vector` 允许元素类型暂时不完整）。
 */
struct Member
{
    std::string key; ///< 成员名。
    Value value;     ///< 成员值。
};

/**
 * @brief 把值写进输出流（用内联写法）。
 * @param stream 输出流。
 * @param value 值。
 * @return 输出流。
 */
std::ostream &operator<<(std::ostream &stream, const Value &value);

/**
 * @brief 解析选项。
 */
struct ParseOptions
{
    /** 允许重复键（后写的覆盖先写的）；默认按 TOML 规范报错。 */
    bool allowDuplicateKeys = false;
};

/**
 * @brief 一条解析诊断。
 */
struct Diagnostic
{
    std::size_t line = 1;   ///< 行号（1 基）。
    std::size_t column = 1; ///< 列号（1 基，按 UTF-8 字节算）。
    std::size_t offset = 0; ///< 字节偏移。
    std::string path;       ///< 投射 / 解码诊断里的 TOML 路径，例如 `server.tags[2]`。
    std::string code;       ///< 短码，例如 `duplicate-key`。
    std::string message;    ///< 说明。

    /**
     * @brief 拼成一行：有路径时是 `路径: 消息 [短码]`，否则是 `行:列: 消息 [短码]`。
     * @return 一行文本。
     */
    [[nodiscard]] std::string toString() const;
};

/**
 * @brief 解析结果。
 */
struct ParseResult
{
    std::optional<Value> value;          ///< 解析成功时的值（顶层总是表）。
    std::vector<Diagnostic> diagnostics; ///< 诊断（失败时至少一条）。

    /**
     * @brief 是否解析成功。
     * @return 成功返回 true。
     */
    [[nodiscard]] bool ok() const noexcept { return value.has_value(); }
};

/**
 * @brief 解析 TOML 文本。
 *
 * 输入按 UTF-8 处理；允许带 BOM 与 CRLF。解析失败时 `diagnostics` 里带行列信息，
 * `value` 为空。
 *
 * @param text TOML 文本。
 * @param options 解析选项。
 * @return 解析结果。
 */
[[nodiscard]] ParseResult parse(std::string_view text, ParseOptions options = {});

} // namespace toml

#endif // PARSE_ENGINE_TOML_HPP
