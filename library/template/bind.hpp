/**
 * @file bind.hpp
 * @brief 把 C++ 模型类投射成模板引擎的 `tmpl::Value`：`field` / `TEMPLATE_MODEL` / `encode`。
 *
 * **C++ 没有内置反射**（静态反射要到 C++26 才进标准），所以"模型类 → 模板值"
 * 必须由某一方声明字段。本库和 `library/json/bind.hpp` 选的是同一套办法：
 * **成员指针表**，既不用代码生成，也不用额外的反射库，C++17 的语法就够：
 *
 * ```cpp
 * struct Token { std::string name; int priority = 0; };
 * struct Grammar { std::string name; std::vector<Token> tokens; bool debug = false; };
 *
 * TEMPLATE_MODEL(Token,
 *     tmpl::field("name", &Token::name),
 *     tmpl::field("priority", &Token::priority));
 *
 * TEMPLATE_MODEL(Grammar,
 *     tmpl::field("name", &Grammar::name),
 *     tmpl::field("tokens", &Grammar::tokens),
 *     tmpl::field("debug", &Grammar::debug));
 *
 * const tmpl::Value value = tmpl::encode(grammar);   // 模型 → 模板值
 * tmpl::render("class {{ name }} { ... }", grammar);      // 直接渲染
 * ```
 *
 * 用到的现代特性：成员指针、`std::apply` + 折叠表达式、`if constexpr`、
 * `std::void_t` 检测、`std::optional`、结构化绑定。风格与 `json/bind.hpp`
 * 对齐，但**不依赖**它——本目录不 include 任何 `library/json` 下的文件。
 *
 * 注意一条 C++ 的硬性规则：`TEMPLATE_MODEL` 展开的是**显式特化**，只能写在
 * **全局作用域**（或者 `tmpl` 命名空间里）。类型本身可以放在命名空间里，
 * 写全限定名即可：
 *
 * ```cpp
 * namespace app { struct Point { int x = 0; }; }
 * TEMPLATE_MODEL(app::Point, tmpl::field("x", &app::Point::x));   // 全局作用域
 * ```
 *
 * 支持的字段类型：
 *
 *   - 标量：`bool`、各种整数、`enum`、`std::string`、`std::string_view`、
 *     `const char *` / `char *`，以及 `tmpl::Value` 本身（原样透传）；
 *   - 容器：`std::vector<T>`（元素可以是标量、其它模型、甚至嵌套 vector）；
 *   - 其它有 `TEMPLATE_MODEL` 绑定的模型类（可以嵌套、可以放进 vector）。
 *
 * 同一个模型类可以同时有 JSON 和 TEMPLATE 两份绑定——它们是两个不同命名
 * 空间里的两个特化，互不干扰：
 *
 * ```cpp
 * JSON_MODEL(Token, json::field("name", &Token::name));
 * TEMPLATE_MODEL(Token, tmpl::field("name", &Token::name));
 * ```
 */

#ifndef PARSE_ENGINE_TEMPLATE_BIND_HPP
#define PARSE_ENGINE_TEMPLATE_BIND_HPP

#include "value.hpp"

#include <memory>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace tmpl
{

/**
 * @brief 一个字段的绑定：模板里的名字 + 指向成员的指针。
 */
template <class T, class M>
struct Field
{
    std::string_view name; ///< 模板里的字段名（`{{ name }}` 里的那个 name）。
    M T::*member;          ///< 成员指针。
};

/**
 * @brief 造一个字段绑定。
 * @param name 模板字段名。
 * @param member 成员指针，例如 `&User::name`。
 * @return 字段绑定。
 */
template <class T, class M>
constexpr Field<T, M> field(std::string_view name, M T::*member) noexcept
{
    return Field<T, M>{name, member};
}

/**
 * @brief 模型绑定：用户特化它，声明有哪些字段（也可以用 `TEMPLATE_MODEL` 宏）。
 *
 * 没有特化时下面的 `hasBinding` 是 false，`encode` 会给出编译期提示。
 */
template <class T>
struct Binding;

/**
 * @brief 声明一个模型的字段表。
 *
 * ```cpp
 * TEMPLATE_MODEL(Token,
 *     tmpl::field("name", &Token::name),
 *     tmpl::field("priority", &Token::priority));
 * ```
 *
 * 展开后就是一个 `tmpl::Binding<T>` 特化：字段表是 `constexpr` 的 tuple，
 * 编译期就能拿到，运行时零查找开销（只有模板里那点线性查找）。
 */
#define TEMPLATE_MODEL(Type, ...)                \
    template <>                                  \
    struct tmpl::Binding<Type>              \
    {                                            \
        static constexpr auto fields()           \
        {                                        \
            return std::make_tuple(__VA_ARGS__); \
        }                                        \
    }

/**
 * @brief 判断一个类型有没有 `TEMPLATE_MODEL` 绑定。
 */
template <class T, class = void>
struct hasBinding : std::false_type
{
};

/** 有 `Binding<T>::fields()` 时特化成 true。 */
template <class T>
struct hasBinding<T, std::void_t<decltype(Binding<T>::fields())>> : std::true_type
{
};

// ---------------------------------------------------------------------------
//  类型特征
// ---------------------------------------------------------------------------

namespace detail
{

/** 是不是字符串视图 / C 字符串 / `std::string`。 */
template <class T>
struct isStringLike
    : std::bool_constant<std::is_same_v<std::decay_t<T>, std::string> ||
                         std::is_same_v<std::decay_t<T>, std::string_view> ||
                         std::is_same_v<std::decay_t<T>, const char *> ||
                         std::is_same_v<std::decay_t<T>, char *>>
{
};

/** 是不是 `std::vector`。 */
template <class T>
struct isVector : std::false_type
{
};

/** `std::vector<U, A>` 特化。 */
template <class U, class A>
struct isVector<std::vector<U, A>> : std::true_type
{
    using inner = U; ///< 元素类型。
};

/** 编译期 false（`static_assert` 里用来打印类型名）。 */
template <class T>
struct alwaysFalse : std::false_type
{
};

} // namespace detail

// ---------------------------------------------------------------------------
//  编码：模型 → 模板值
// ---------------------------------------------------------------------------

/**
 * @brief 把一个 C++ 值编码成模板引擎的 `Value`。
 *
 * 分发顺序（`if constexpr`）：`Value` → `bool` → `enum` → 整数 → 字符串 →
 * `std::vector` → 有 `TEMPLATE_MODEL` 绑定的模型类 → 编译期报错。
 *
 * @tparam T 类型。
 * @param value 值。
 * @return 模板值。
 */
template <class T>
Value encode(const T &value)
{
    using U = std::decay_t<T>;

    if constexpr (std::is_same_v<U, Value>)
    {
        return value;
    }
    else if constexpr (std::is_same_v<U, bool>)
    {
        return Value(value);
    }
    else if constexpr (std::is_enum_v<U>)
    {
        return Value(static_cast<std::int64_t>(value));
    }
    else if constexpr (std::is_integral_v<U>)
    {
        return Value(static_cast<std::int64_t>(value));
    }
    else if constexpr (detail::isStringLike<U>::value)
    {
        // C 字符串可能是空指针：按空串处理，免得构造出非法的 string_view。
        if constexpr (std::is_pointer_v<U>)
        {
            if (value == nullptr) return Value(std::string());
        }
        return Value(std::string(std::string_view(value)));
    }
    else if constexpr (detail::isVector<U>::value)
    {
        auto items = std::make_shared<Array>();
        items->items.reserve(value.size());
        for (const auto &item : value) items->items.push_back(encode(item));
        return Value(std::move(items));
    }
    else if constexpr (hasBinding<U>::value)
    {
        auto members = std::make_shared<Object>();
        // 折叠表达式：把每个字段按 "名字 → 成员" 塞进对象，顺序就是声明顺序。
        std::apply(
            [&](const auto &...fields) {
                ((members->insert(std::string(fields.name), encode(value.*(fields.member)))), ...);
            },
            Binding<U>::fields());
        return Value(std::move(members));
    }
    else
    {
        static_assert(detail::alwaysFalse<U>::value,
                      "这个类型没有模板绑定：给它写 TEMPLATE_MODEL(...)，"
                      "或者换成 bool / 整数 / enum / string / vector / tmpl::Value");
        return Value(nullptr);
    }
}

} // namespace tmpl

#endif // PARSE_ENGINE_TEMPLATE_BIND_HPP
