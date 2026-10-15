/**
 * @file model.hpp
 * @brief 模板引擎的"取数"抽象：`Model` / `ModelAdapter` / `ValueModel` / `resolve`。
 *
 * 模板引擎不关心数据是 C++ 模型类、是动态拼出来的 `Value`、还是别的什么，
 * 它只认一个抽象：**给一个名字，返回一个 `Value`**。这就是 `Model`：
 *
 * ```cpp
 * class Model
 * {
 * public:
 *     virtual ~Model() = default;
 *     virtual Value get(const std::string &key) const = 0;
 * };
 * ```
 *
 * 三种现成的实现：
 *
 *   - `ModelAdapter<T>`：把 `TEMPLATE_MODEL` 绑定过的模型类包成 `Model`
 *     （字段表 → 成员指针 → `encode`，全程编译期展开，没有字符串查找表）；
 *   - `ValueModel`：把已经造好的 `Value` 对象包成 `Model`——`a.b.c` 这种
 *     多级路径中间那几级就靠它逐级下降；
 *   - `ChildModel`（在 `engine.hpp`）：循环体的子上下文，带作用域链。
 *
 * `resolve()` 是路径求值：`"a.b.c"` 按点号切段，逐级向下，任何一级不是对象
 * 就直接返回空值——模板里 `{{ 不存在的字段 }}` 只会输出空串，不会抛异常。
 */

#ifndef PARSE_ENGINE_TEMPLATE_MODEL_HPP
#define PARSE_ENGINE_TEMPLATE_MODEL_HPP

#include "bind.hpp"

#include <cstddef>
#include <string>
#include <utility>

namespace tmpl
{

/**
 * @brief 模板引擎的取数接口：一个名字 → 一个值。
 *
 * 引擎只依赖这个虚接口，所以模型类（`ModelAdapter`）、动态值（`ValueModel`）、
 * 循环子上下文（`ChildModel`）可以用同一套模板语法。
 */
class Model
{
public:
    /** 虚析构：子类通过基类指针使用。 */
    virtual ~Model() = default;

    /**
     * @brief 按名字取值。
     * @param key 字段名（路径里的一段）。
     * @return 字段值；没有这个字段返回空 `Value{}`。
     */
    virtual Value get(const std::string &key) const = 0;
};

/**
 * @brief 把绑定了 `TEMPLATE_MODEL` 的模型类包成 `Model`。
 *
 * 每次 `get()` 都按字段表线性比对名字，命中就把成员 `encode` 成 `Value`。
 * 字段表是 `constexpr` 的 tuple，`std::apply` + 折叠表达式展开后编译器能
 * 直接把整张表内联掉。
 *
 * @tparam T 模型类（需要有 `TEMPLATE_MODEL` 绑定）。
 */
template <class T>
class ModelAdapter : public Model
{
public:
    const T &obj; ///< 被包装的模型（引用：不拷贝，也不管理生命周期）。

    /**
     * @brief 绑定一个模型对象。
     * @param o 模型对象，生命周期要活过本适配器。
     */
    explicit ModelAdapter(const T &o) : obj(o) {}

    /**
     * @brief 按名字取字段。
     * @param key 字段名。
     * @return 字段值；没有这个字段（或类型没有绑定）返回空 `Value{}`。
     */
    Value get(const std::string &key) const override
    {
        if constexpr (hasBinding<T>::value)
        {
            Value result;
            if constexpr (std::tuple_size<decltype(Binding<T>::fields())>::value == 0u)
            {
                // 零字段模型（纯静态片段的占位模型）：任何名字都取不到，
                // 这里显式收一下参数，免得空折叠展开成一条没用处的语句。
                (void)key;
            }
            else
            {
                // 折叠表达式 + 短路：找到第一个同名字段就停；找不到就是空值。
                std::apply(
                    [&](const auto &...fields) {
                        ((fields.name == key ? (result = encode(obj.*(fields.member)), true) : false) || ...);
                    },
                    Binding<T>::fields());
            }
            return result;
        }
        else
        {
            (void)key;
            return Value{};
        }
    }
};

/**
 * @brief 把一个已经造好的 `Value`（对象）包成 `Model`。
 *
 * 用途：`resolve()` 处理 `"a.b.c"` 这种多级路径时，中间几级拿到的是
 * `Value` 而不是模型类，需要再往下一层一层地取。
 */
class ValueModel : public Model
{
public:
    /** 空适配器（还没绑任何值）。 */
    ValueModel() noexcept = default;

    /**
     * @brief 绑定一个值。
     * @param value 值，生命周期要活过本适配器。
     */
    explicit ValueModel(const Value &value) noexcept : value_(&value) {}

    /**
     * @brief 改绑另一个值。
     * @param value 值，生命周期要活过本适配器。
     */
    void bind(const Value &value) noexcept { value_ = &value; }

    /**
     * @brief 按名字取成员。
     * @param key 成员名。
     * @return 成员值；不是对象 / 没有这个成员返回空 `Value{}`。
     */
    Value get(const std::string &key) const override
    {
        if (value_ == nullptr) return Value{};
        const Object *members = value_->asObject();
        if (members == nullptr) return Value{};
        const Value *member = members->tryGet(key);
        return member != nullptr ? *member : Value{};
    }

private:
    const Value *value_ = nullptr; ///< 被包装的值。
};

/**
 * @brief 用"名字 → 值"的表当模型：给还没做专门模型类的模板用。
 *
 * 生成器里最常见的形态是"模板写好了，但字段还是临时拼的"——`renderValues`
 * 就是给这种场合准备的：不用先声明 struct + `TEMPLATE_MODEL` 绑定，直接
 * 把名字和值摆出来渲染，模板里的 `{{ 名字 }}` 照样能用。
 *
 * ```cpp
 * const std::string text = tmpl::renderValues(source, {
 *     {"mode_name", modeLabelText(mode)},
 *     {"ns", nameSpace},
 * });
 * ```
 */
class MapModel : public Model
{
public:
    /** 表里的一项：名字 → 值。 */
    using Entry = std::pair<std::string, Value>;

    /** 空表。 */
    MapModel() = default;

    /**
     * @brief 用初始列表建模型。
     * @param entries 名字 / 值对。
     */
    MapModel(std::initializer_list<Entry> entries) : entries_(entries) {}

    /**
     * @brief 用现成的表建模型。
     * @param entries 名字 / 值对。
     */
    explicit MapModel(std::vector<Entry> entries) : entries_(std::move(entries)) {}

    /**
     * @brief 加一项（同名后加的覆盖前面的）。
     * @param key 名字。
     * @param value 值。
     */
    void set(std::string key, Value value)
    {
        for (Entry &entry : entries_)
        {
            if (entry.first == key)
            {
                entry.second = std::move(value);
                return;
            }
        }
        entries_.emplace_back(std::move(key), std::move(value));
    }

    /**
     * @brief 按名字取值。
     * @param key 名字。
     * @return 值；表里没有返回空 `Value{}`。
     */
    Value get(const std::string &key) const override
    {
        for (const Entry &entry : entries_)
        {
            if (entry.first == key) return entry.second;
        }
        return Value{};
    }

private:
    std::vector<Entry> entries_; ///< 名字 → 值。
};

/**
 * @brief 求值一条点号路径：`"a.b.c"`。
 *
 * 逐段调用 `current->get(segment)`；除了最后一段之外，每一段的结果都必须是
 * 对象，否则说明路径走不通，直接返回空值。空路径、空段、缺失字段一律返回
 * 空 `Value{}`——模板里只会输出空串，不抛异常。
 *
 * @param root 根上下文（模型适配器 / 循环子上下文…）。
 * @param path 点号分隔的路径。
 * @return 路径末端的值；走不通返回空 `Value{}`。
 */
inline Value resolve(const Model &root, const std::string &path)
{
    if (path.empty()) return Value{};

    const Model *current = &root;
    ValueModel nested;  // 中间级的"Value → Model"适配器（复用同一个对象）
    Value holder;       // 保存上一级的值，保证 nested 指向的内存一直活着
    std::size_t begin = 0;

    while (true)
    {
        const std::size_t dot = path.find('.', begin);
        const std::size_t length = (dot == std::string::npos) ? std::string::npos : dot - begin;
        const std::string segment = path.substr(begin, length);

        const Value value = current->get(segment);
        if (dot == std::string::npos)
        {
            return value; // 最后一段：找到什么就是什么（空值也算结果）
        }

        // 后面还有段：这一级必须是对象，否则路径断开。
        if (!value.isObject()) return Value{};
        holder = value;
        nested.bind(holder);
        current = &nested;
        begin = dot + 1;
    }
}

} // namespace tmpl

#endif // PARSE_ENGINE_TEMPLATE_MODEL_HPP
