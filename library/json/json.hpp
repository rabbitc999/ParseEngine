/**
 * @file json.hpp
 * @brief 现代 C++17 的 JSON 库：解析、序列化，以及可直接喂给 STL 算法的迭代器。
 *
 * 设计要点：
 *
 *   - **存储**用 `std::variant`：null / bool / int64 / double / string / array / object；
 *   - **对象保序**：`Object` 是"插入顺序的 vector<Member>"，成员是公开字段，
 *     所以 `for (auto& [key, value] : object)` 直接可用（结构化绑定）；
 *   - **迭代器**：`Array` / `Object` 暴露标准迭代器，`std::for_each` /
 *     `std::count_if` / `std::transform` / `std::accumulate` 这些算法直接能用；
 *     `Value` 自己也能迭代——数组给元素、对象给成员、标量是空区间；
 *   - **现代特性**：`std::optional` / `std::string_view` / `std::variant` /
 *     `std::visit` / `if constexpr` / `std::from_chars` / `to_chars` / 结构化绑定 /
 *     `[[nodiscard]]` / `constexpr`。
 *
 * 只依赖 C++17 标准库，不参与 ParseEngine 主程序的构建。
 *
 * 快速看一眼用法：
 *
 * ```cpp
 * const json::ParseResult doc = json::parse(R"({"name":"calc","tokens":["+","-"]})");
 * if (!doc.ok()) { ... }
 *
 * const json::Object& root = doc.value->asObject();       // 取对象
 * for (const auto& [key, value] : root) { ... }           // 结构化绑定遍历
 *
 * const json::Array& tokens = doc.value->find("tokens")->asArray();
 * const auto plusCount = std::count_if(                     // STL 算法直接可用
 *     tokens.begin(), tokens.end(),
 *     [](const json::Value& item) { return item.asString() == "+"; });
 * ```
 */

#ifndef PARSE_ENGINE_JSON_HPP
#define PARSE_ENGINE_JSON_HPP

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

namespace json
{

class Value;

/** JSON 数组：`std::vector<Value>`，标准迭代器，STL 算法直接可用。 */
using Array = std::vector<Value>;

/**
 * @brief JSON 的 `null`。
 */
struct Null
{
    /**
     * @brief 两个 null 相等。
     * @return 恒为 true。
     */
    friend constexpr bool operator==(Null, Null) noexcept { return true; }

    /**
     * @brief 两个 null 不相等。
     * @return 恒为 false。
     */
    friend constexpr bool operator!=(Null, Null) noexcept { return false; }
};

/** 对象成员（定义在 Value 之后：它按值持有 Value）。 */
struct Member;

/**
 * @brief JSON 对象：**保持插入顺序**的成员表。
 *
 * 查找是线性的（配置 / 报文这类场景成员不多，保序比 O(1) 更值）；
 * 迭代顺序就是插入顺序（解析时即文件里的先后顺序）。
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
     * @brief 取成员值，不存在就插入一个 null。
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

/**
 * @brief JSON 值。
 */
class Value
{
public:
    /** 底层存储：variant 覆盖 JSON 的全部类型。 */
    using Storage = std::variant<Null, bool, std::int64_t, double, std::string, Array, Object>;

    /** 构造：默认是 null。 */
    Value() noexcept : storage_(Null{}) {}

    /** 构造：`nullptr` → null。 */
    Value(std::nullptr_t) noexcept : storage_(Null{}) {}

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

    /** 构造：数组。 */
    Value(Array value) : storage_(std::move(value)) {}

    /** 构造：对象。 */
    Value(Object value) : storage_(std::move(value)) {}

    /**
     * @brief 用初始化列表构造数组：`Value{1, 2, 3}`。
     * @param items 元素。
     * @return 数组值。
     */
    static Value array(std::initializer_list<Value> items);

    /**
     * @brief 用初始化列表构造对象：`Value::object({{"a", 1}, {"b", "x"}})`。
     * @param items 成员。
     * @return 对象值。
     */
    static Value object(std::initializer_list<std::pair<std::string, Value>> items);

    // ---- 类型判断 ---------------------------------------------------------

    /** 是不是 null。 */
    [[nodiscard]] bool isNull() const noexcept { return std::holds_alternative<Null>(storage_); }
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
    /** 是不是数组。 */
    [[nodiscard]] bool isArray() const noexcept { return std::holds_alternative<Array>(storage_); }
    /** 是不是对象。 */
    [[nodiscard]] bool isObject() const noexcept { return std::holds_alternative<Object>(storage_); }

    /**
     * @brief 类型名（诊断 / 统计用）。
     * @return `null` / `bool` / `int` / `double` / `string` / `array` / `object`。
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
    /** 取对象；不是对象返回 nullptr。 */
    [[nodiscard]] Object *asObject() noexcept;
    /** 取对象（只读）。 */
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
     * @brief 取对象（可写），不是对象就抛 `std::domain_error`。
     * @return 对象引用。
     * @throws std::domain_error 当前值不是对象。
     */
    [[nodiscard]] Object &requireObject();

    /**
     * @brief 取对象（只读），不是对象就抛 `std::domain_error`。
     * @return 对象引用。
     * @throws std::domain_error 当前值不是对象。
     */
    [[nodiscard]] const Object &requireObject() const;

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
     */
    [[nodiscard]] const Value &at(std::size_t index) const;

    /**
     * @brief 取对象成员；不存在就插入一个 null。
     * @param key 成员名。
     * @return 成员值引用。
     */
    Value &operator[](std::string_view key);

    /**
     * @brief 查找对象成员。
     * @param key 成员名。
     * @return 指针；不是对象或没找到返回 nullptr。
     */
    [[nodiscard]] const Value *find(std::string_view key) const noexcept;

    /**
     * @brief 查找对象成员（可写）。
     * @param key 成员名。
     * @return 指针；不是对象或没找到返回 nullptr。
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
     * @brief 元素个数（数组 / 对象）。
     * @return 个数；标量返回 0。
     */
    [[nodiscard]] std::size_t size() const noexcept;

    /**
     * @brief 是否为空（数组 / 对象 / 空字符串）。
     * @return 空返回 true。
     */
    [[nodiscard]] bool empty() const noexcept;

    /** 清空数组 / 对象（标量变成 null）。 */
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
     * 数组元素：`index` 有值、`key` 为空；对象成员：`key` 有值。
     * 可以隐式转成 `Value&`，所以 `for (auto& item : value) { item.value … }`
     * 与 `item.value.asString()` 都行。
     */
    class Reference
    {
    public:
        Value *value = nullptr;      ///< 指向元素 / 成员值。
        std::string_view key;        ///< 对象成员名；数组元素为空。
        std::size_t index = static_cast<std::size_t>(-1); ///< 数组下标；对象成员为 npos。

        /** 隐式转成 Value 引用，方便直接当值用。 */
        operator Value &() const noexcept { return *value; }

        /** 取值的引用。 */
        [[nodiscard]] Value &get() const noexcept { return *value; }
    };

    /**
     * @brief 通用迭代器：数组走元素、对象走成员。
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
        /** 被迭代的值（数组或对象）；end 迭代器为 nullptr 之外的同一个 owner。 */
        Value *owner_ = nullptr;

        /** 在容器里的位置（数组下标 / 对象成员序号）。 */
        std::size_t position_ = 0u;

        /** `operator*` 的返回值缓存（range-for 要绑引用，所以必须缓存）。 */
        mutable Reference cache_{};

        friend class Value;
    };

    /**
     * 开始迭代：数组 → 元素，对象 → 成员，标量 → 空区间。
     *
     * 只提供非 const 版本：迭代给出的引用是**可写**的。只读遍历用
     * `asArray()` / `asObject()`（它们有 const 重载，给的是 `const` 引用）。
     */
    [[nodiscard]] iterator begin() noexcept;

    /** 结束迭代。 */
    [[nodiscard]] iterator end() noexcept;

    // ---- 序列化与比较 -----------------------------------------------------

    /**
     * @brief 序列化成 JSON 文本。
     * @param indent 缩进：< 0 单行紧凑；0 换行但不缩进；> 0 每层缩进这么多空格。
     * @return JSON 文本。
     */
    [[nodiscard]] std::string dump(int indent = -1) const;

    /**
     * @brief 底层存储（需要 `std::visit` 自己分派时用）。
     * @return variant 引用。
     */
    [[nodiscard]] const Storage &storage() const noexcept { return storage_; }

    /** 相等比较（按 JSON 语义：数字 1 与 1.0 视为相等）。 */
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
 * @brief 对象的一个成员：名字 + 值。
 *
 * 两个字段都是公开的，所以可以 `for (auto& [key, value] : object)` 直接解构。
 * 定义放在 `Value` 之后：它按值持有 `Value`，而 `Value` 又要把 `Object` 放进
 * `std::variant`，两者只能这样排序（`std::vector` 允许元素类型暂时不完整）。
 */
struct Member
{
    std::string key; ///< 成员名。
    Value value;     ///< 成员值。
};

/**
 * @brief 把值写进输出流。
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
    /** 允许 `//` 行注释与 `/\* … *​/` 块注释（配置文件的常见扩展）。 */
    bool allowComments = false;

    /** 允许数组 / 对象末尾多一个逗号。 */
    bool allowTrailingCommas = false;
};

/**
 * @brief 一条解析诊断。
 */
struct Diagnostic
{
    std::size_t line = 1;   ///< 行号（1 基）。
    std::size_t column = 1; ///< 列号（1 基，按 UTF-8 字节算）。
    std::size_t offset = 0; ///< 字节偏移。
    std::string path;       ///< 投射 / 解码诊断里的 JSON 路径，例如 `user.tags[2].name`。
    std::string code;       ///< 短码，例如 `unexpected-token`。
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
    std::optional<Value> value;            ///< 解析成功时的值。
    std::vector<Diagnostic> diagnostics;   ///< 诊断（失败时至少一条）。

    /**
     * @brief 是否解析成功。
     * @return 成功返回 true。
     */
    [[nodiscard]] bool ok() const noexcept { return value.has_value(); }
};

/**
 * @brief 解析 JSON 文本。
 *
 * 输入按 UTF-8 处理；允许带 BOM。解析失败时 `diagnostics` 里带行列信息，
 * `value` 为空。
 *
 * @param text JSON 文本。
 * @param options 解析选项。
 * @return 解析结果。
 */
[[nodiscard]] ParseResult parse(std::string_view text, ParseOptions options = {});

} // namespace json

#endif // PARSE_ENGINE_JSON_HPP
