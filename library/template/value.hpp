/**
 * @file value.hpp
 * @brief 模板引擎自己的动态值：`tmpl::Value` / `Array` / `Object`。
 *
 * 模板里的 `{{ path.to.value }}` 需要一个"运行时才知道类型"的值，这就是
 * `Value`。它**完全独立于** `library/json` 里的 `json::Value`：本目录不
 * include 任何 `library/json` 下的文件，两份实现可以各自演化（一份关心
 * JSON 的解析/序列化，一份只关心"模板里能不能取值、真不真、怎么变字符串"）。
 *
 * 设计要点：
 *
 *   - **存储**用 `std::variant`：null / string / int64 / bool / array / object
 *     （只支持整数：代码生成里够用，省掉浮点格式化的一堆坑）；
 *   - **递归**：`Value` 要包含 `Array` / `Object`，直接内联会无限大，所以这两
 *     个类型用 `std::shared_ptr` 持有（拷贝 `Value` 是共享，不是深拷贝）；
 *   - **对象保序**：`Object::members` 是 `std::vector<std::pair<...>>` 而不是
 *     `std::map`，因为模板遍历时**顺序必须稳定**——模型字段的声明顺序就是
 *     生成代码的顺序；
 *   - **现代特性**：`std::variant` / `std::optional` / `std::string_view` /
 *     `std::get_if` / 结构化绑定 / `[[nodiscard]]`。
 *
 * ```cpp
 * tmpl::Value text(std::string("calc"));
 * text.isString();                  // true
 * text.asString().value_or("");     // "calc"
 * text.toString();                  // "calc"
 *
 * auto object = std::make_shared<tmpl::Object>();
 * object->insert("name", tmpl::Value(std::string("calc")));
 * tmpl::Value document(object);
 * document.asObject()->tryGet("name")->toString();   // "calc"
 * ```
 */

#ifndef PARSE_ENGINE_TEMPLATE_VALUE_HPP
#define PARSE_ENGINE_TEMPLATE_VALUE_HPP

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace tmpl
{

/**
 * @brief 模板引擎的值：null / string / int64 / bool / array / object。
 *
 * 六种类型互斥，`isXxx()` 判定、`asXxx()` 取值（取不到返回空 / nullptr），
 * `truthy()` 给 `{% if %}` 用，`toString()` 给 `{{ }}` 用。
 */
class Value
{
public:
    /** 数组（定义在 `Value` 之后：它按值持有 `std::vector<Value>`）。 */
    struct Array;

    /** 对象（定义在 `Value` 之后：它按值持有 `Value`）。 */
    struct Object;

    /** 底层存储：null / string / int64 / bool / array / object。 */
    using Storage = std::variant<std::monostate,
                                 std::string,
                                 std::int64_t,
                                 bool,
                                 std::shared_ptr<Array>,
                                 std::shared_ptr<Object>>;

    /** null。 */
    Value() noexcept = default;

    /** `nullptr` 也是 null（显式重载，免得在 `const char *` / `bool` 之间挑不定）。 */
    Value(std::nullptr_t) noexcept {}

    /** 字符串（拿所有权）。 */
    Value(std::string text) : data_(std::move(text)) {}

    /** 字符串视图：拷贝成自己的 `std::string`（不持有别人的内存）。 */
    Value(std::string_view text) : data_(std::string(text)) {}

    /** C 字符串；空指针按空串处理。 */
    Value(const char *text) : data_(text != nullptr ? std::string(text) : std::string()) {}

    /** 64 位整数。 */
    Value(std::int64_t number) noexcept : data_(number) {}

    /** `int`（免得 `Value(3)` 在 int64 / bool 之间挑不定）。 */
    Value(int number) noexcept : data_(static_cast<std::int64_t>(number)) {}

    /** 布尔。 */
    Value(bool flag) noexcept : data_(flag) {}

    /** 其它整数类型（`size_t` / `unsigned` / `char` 之类）统一收成 int64。 */
    template <class Integer,
              std::enable_if_t<std::is_integral_v<Integer> && !std::is_same_v<Integer, bool> &&
                                   !std::is_same_v<Integer, int> && !std::is_same_v<Integer, std::int64_t>,
                               int> = 0>
    Value(Integer number) noexcept : data_(static_cast<std::int64_t>(number))
    {
    }

    /** 数组（与其它 `Value` 共享同一份数据）。 */
    Value(std::shared_ptr<Array> items) noexcept : data_(std::move(items)) {}

    /** 对象（与其它 `Value` 共享同一份数据）。 */
    Value(std::shared_ptr<Object> members) noexcept : data_(std::move(members)) {}

    // ---- 类型判断 ---------------------------------------------------------

    /** 是不是 null。 */
    [[nodiscard]] bool isNull() const noexcept { return std::holds_alternative<std::monostate>(data_); }

    /** 是不是字符串。 */
    [[nodiscard]] bool isString() const noexcept { return std::holds_alternative<std::string>(data_); }

    /** 是不是整数。 */
    [[nodiscard]] bool isInt() const noexcept { return std::holds_alternative<std::int64_t>(data_); }

    /** 是不是布尔。 */
    [[nodiscard]] bool isBool() const noexcept { return std::holds_alternative<bool>(data_); }

    /** 是不是数组（空指针算"不是"）。 */
    [[nodiscard]] bool isArray() const noexcept { return asArray() != nullptr; }

    /** 是不是对象（空指针算"不是"）。 */
    [[nodiscard]] bool isObject() const noexcept { return asObject() != nullptr; }

    // ---- 取值（取不到就是空） ---------------------------------------------

    /**
     * @brief 取字符串。
     * @return 字符串视图；不是字符串返回 `std::nullopt`。
     * @warning 视图指向 `Value` 内部的 `std::string`，`Value` 改掉或销毁就失效。
     */
    [[nodiscard]] std::optional<std::string_view> asString() const noexcept
    {
        if (const std::string *text = std::get_if<std::string>(&data_)) return std::string_view(*text);
        return std::nullopt;
    }

    /** @brief 取整数。 @return 整数；不是整数返回 `std::nullopt`。 */
    [[nodiscard]] std::optional<std::int64_t> asInt() const noexcept
    {
        if (const std::int64_t *number = std::get_if<std::int64_t>(&data_)) return *number;
        return std::nullopt;
    }

    /** @brief 取布尔。 @return 布尔；不是布尔返回 `std::nullopt`。 */
    [[nodiscard]] std::optional<bool> asBool() const noexcept
    {
        if (const bool *flag = std::get_if<bool>(&data_)) return *flag;
        return std::nullopt;
    }

    /**
     * @brief 取数组。
     * @return 数组指针；不是数组返回 nullptr。
     * @warning 指针指向 `Value` 内部的数组，**值得先活着**：`encode(x).asArray()`
     *          这种写法在语句结束就悬空了，随时可能读到垃圾。
     */
    [[nodiscard]] Array *asArray() noexcept
    {
        std::shared_ptr<Array> *holder = std::get_if<std::shared_ptr<Array>>(&data_);
        return (holder != nullptr && holder->get() != nullptr) ? holder->get() : nullptr;
    }

    /** @brief 取数组（只读）。 @return 数组指针；不是数组返回 nullptr（生命周期同上）。 */
    [[nodiscard]] const Array *asArray() const noexcept
    {
        const std::shared_ptr<Array> *holder = std::get_if<std::shared_ptr<Array>>(&data_);
        return (holder != nullptr && holder->get() != nullptr) ? holder->get() : nullptr;
    }

    /**
     * @brief 取对象。
     * @return 对象指针；不是对象返回 nullptr。
     * @warning 同 `asArray()`：指针的生命周期跟着 `Value` 走。
     */
    [[nodiscard]] Object *asObject() noexcept
    {
        std::shared_ptr<Object> *holder = std::get_if<std::shared_ptr<Object>>(&data_);
        return (holder != nullptr && holder->get() != nullptr) ? holder->get() : nullptr;
    }

    /** @brief 取对象（只读）。 @return 对象指针；不是对象返回 nullptr。 */
    [[nodiscard]] const Object *asObject() const noexcept
    {
        const std::shared_ptr<Object> *holder = std::get_if<std::shared_ptr<Object>>(&data_);
        return (holder != nullptr && holder->get() != nullptr) ? holder->get() : nullptr;
    }

    // ---- 模板语义 ---------------------------------------------------------

    /**
     * @brief `{% if cond %}` 的真值判断。
     *
     * 规则：bool 按值；string 非空为真；array 非空为真；object 非空为真；
     * 整数 0 为假；null 为假。
     *
     * （实现放在 `Array` / `Object` 定义之后：要知道它们是不是空的，
     * 这两个类型得是完整的。）
     *
     * @return 真值。
     */
    [[nodiscard]] bool truthy() const noexcept;

    /**
     * @brief `{{ value }}` 的文本化。
     *
     * 规则：string 原样；bool → `"true"` / `"false"`；int → 十进制；
     * 其它（null / array / object）→ 空串。
     *
     * @return 文本。
     */
    [[nodiscard]] std::string toString() const
    {
        if (const std::optional<std::string_view> text = asString()) return std::string(*text);
        if (const std::optional<bool> flag = asBool()) return *flag ? std::string("true") : std::string("false");
        if (const std::optional<std::int64_t> number = asInt()) return std::to_string(*number);
        return std::string();
    }

private:
    Storage data_; ///< 值本身。
};

/**
 * @brief 数组：一串 `Value`。
 *
 * `{% for x in list %}` 只认它（`Value::asArray()` 拿到的就是本类型）。
 */
struct Value::Array
{
    std::vector<Value> items; ///< 元素（顺序就是要遍历的顺序）。
};

/**
 * @brief 对象：**保持插入顺序**的成员表。
 *
 * 查找是线性的——模型字段就那几个，保序（= 字段声明顺序）比 O(1) 更值。
 * 用 `std::vector<std::pair<...>>` 而不是 `std::map`，模板遍历顺序才是稳定的。
 */
struct Value::Object
{
    std::vector<std::pair<std::string, Value>> members; ///< 成员，按插入顺序。

    /**
     * @brief 追加一个成员。
     * @param key 成员名。
     * @param value 成员值。
     */
    void insert(std::string key, Value value) { members.emplace_back(std::move(key), std::move(value)); }

    /**
     * @brief 按名字找成员。
     * @param key 成员名。
     * @return 成员指针；没有返回 nullptr。
     */
    [[nodiscard]] const Value *tryGet(std::string_view key) const noexcept
    {
        for (const auto &entry : members)
            if (std::string_view(entry.first) == key) return &entry.second;
        return nullptr;
    }

    /** @brief 是不是没有成员。 @return 空返回 true。 */
    [[nodiscard]] bool empty() const noexcept { return members.empty(); }

    /** @brief 成员个数。 @return 个数。 */
    [[nodiscard]] std::size_t size() const noexcept { return members.size(); }
};

/** `Value::Array` 的短名字，方便 `tmpl::Array` 这样写。 */
using Array = Value::Array;

/** `Value::Object` 的短名字，方便 `tmpl::Object` 这样写。 */
using Object = Value::Object;

/**
 * @brief `{% if cond %}` 的真值判断。
 *
 * 规则：bool 按值；string 非空为真；array 非空为真；object 非空为真；
 * 整数 0 为假；null 为假。
 *
 * @return 真值。
 */
inline bool Value::truthy() const noexcept
{
    if (const std::optional<bool> flag = asBool()) return *flag;
    if (const std::optional<std::string_view> text = asString()) return !text->empty();
    if (const std::optional<std::int64_t> number = asInt()) return *number != 0;
    if (const Array *items = asArray()) return !items->items.empty();
    if (const Object *members = asObject()) return !members->members.empty();
    return false; // null
}

} // namespace tmpl

#endif // PARSE_ENGINE_TEMPLATE_VALUE_HPP
