/**
 * @file bind.hpp
 * @brief 把 TOML 投射到 C++ 模型类（以及反过来）：`toml::encode` / `toml::decode`。
 *
 * 与同目录 JSON 库（`library/json/bind.hpp`）同一套设计：**C++ 没有内置反射**，
 * 所以"模型类 ↔ TOML"由成员指针表声明字段，C++17 的语法就够：
 *
 * ```cpp
 * struct Server { std::string host; int port = 0; bool tls = false; };
 *
 * TOML_MODEL(Server,
 *     toml::field("host", &Server::host),
 *     toml::field("port", &Server::port),
 *     toml::field("tls", &Server::tls));
 *
 * const toml::Value toml = toml::encode(server);              // 模型 → TOML
 * const toml::DecodeResult<Server> back = toml::decode<Server>(toml);  // TOML → 模型
 * ```
 *
 * 支持的字段类型：
 *
 *   - 标量：`bool`、各种整数、浮点、`std::string`、`std::string_view`（编码）、
 *     `const char *`、`toml::DateTime`、以及 `toml::Value` 本身（原样透传）；
 *   - 容器：`std::vector<T>`、`std::map<std::string, T>`、`std::unordered_map<std::string, T>`；
 *   - `std::optional<T>`：TOML 没有 null，**编码时空值字段直接省略**，解码时缺字段就是空；
 *   - `enum`：默认按底层整数；`DecodeOptions::enumAsString` 打开后按 `to_string` 写；
 *   - 其它有 `TOML_MODEL` 绑定的模型类（可嵌套、可放进容器）。
 *
 * `TOML_MODEL` 展开的是**显式特化**，只能写在**全局作用域**（类型本身可以放在
 * 命名空间里，写全限定名即可）。
 */

#ifndef PARSE_ENGINE_TOML_BIND_HPP
#define PARSE_ENGINE_TOML_BIND_HPP

#include "toml.hpp"

#include <charconv>
#include <cstdlib>
#include <map>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <unordered_map>

namespace toml
{

/**
 * @brief 一个字段的绑定：TOML 里的名字 + 指向成员的指针。
 */
template <class T, class M>
struct Field
{
    std::string_view name; ///< TOML 里的字段名。
    M T::*member;          ///< 成员指针。
};

/**
 * @brief 造一个字段绑定。
 * @param name TOML 字段名。
 * @param member 成员指针，例如 `&Server::host`。
 * @return 字段绑定。
 */
template <class T, class M>
constexpr Field<T, M> field(std::string_view name, M T::*member) noexcept
{
    return Field<T, M>{name, member};
}

/**
 * @brief 模型绑定：用户特化它，声明有哪些字段（也可以用 `TOML_MODEL` 宏）。
 */
template <class T>
struct Binding;

/**
 * @brief 声明一个模型的字段表。
 *
 * ```cpp
 * TOML_MODEL(Server,
 *     toml::field("host", &Server::host),
 *     toml::field("port", &Server::port));
 * ```
 */
#define TOML_MODEL(Type, ...)                    \
    template <>                                  \
    struct toml::Binding<Type>                   \
    {                                            \
        static constexpr auto fields()           \
        {                                        \
            return std::make_tuple(__VA_ARGS__); \
        }                                        \
    }

/**
 * @brief 判断一个类型有没有 `TOML_MODEL` 绑定。
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

/**
 * @brief 解码选项。
 */
struct DecodeOptions
{
    /** 缺字段是否算错误（可选字段除外）。 */
    bool strictMissing = true;

    /** 多出来的字段是否算错误（默认不算：方便前端多传字段）。 */
    bool strictUnknown = false;

    /** 字符串形式的数字（`"12"`）是否允许喂给整数 / 浮点字段。 */
    bool allowStringNumbers = true;

    /** 枚举是否按 `to_string` 的字符串写（编码用；解码仍接受字符串与整数）。 */
    bool enumAsString = false;
};

/**
 * @brief 解码结果。
 *
 * @tparam T 目标类型。
 */
template <class T>
struct DecodeResult
{
    std::optional<T> value;              ///< 成功时的模型。
    std::vector<Diagnostic> diagnostics; ///< 失败时的诊断（带 TOML 路径）。

    /**
     * @brief 是否成功。
     * @return 成功返回 true。
     */
    [[nodiscard]] bool ok() const noexcept { return value.has_value(); }

    /**
     * @brief 成功时取值（失败时抛异常，省得判空）。
     * @return 模型引用。
     * @throws std::runtime_error 解码失败。
     */
    const T &require() const
    {
        if (!value.has_value()) throw std::runtime_error("TOML 解码失败");
        return *value;
    }
};

// ---------------------------------------------------------------------------
//  类型特征
// ---------------------------------------------------------------------------

namespace detail
{

/** 是不是 `std::optional`。 */
template <class T>
struct isOptional : std::false_type
{
};

/** `std::optional<U>` 特化。 */
template <class U>
struct isOptional<std::optional<U>> : std::true_type
{
    using inner = U; ///< 内部的类型。
};

/** 是不是 `std::vector`。 */
template <class T>
struct isVector : std::false_type
{
};

/** `std::vector<U>` 特化。 */
template <class U, class A>
struct isVector<std::vector<U, A>> : std::true_type
{
    using inner = U; ///< 元素类型。
};

/** 是不是以字符串为键的 map（`std::map` / `std::unordered_map`）。 */
template <class T>
struct isStringMap : std::false_type
{
};

/** `std::map<std::string, V, …>` 特化。 */
template <class V, class C, class A>
struct isStringMap<std::map<std::string, V, C, A>> : std::true_type
{
    using mapped_type = V; ///< 值类型。
};

/** `std::unordered_map<std::string, V, …>` 特化。 */
template <class V, class H, class E, class A>
struct isStringMap<std::unordered_map<std::string, V, H, E, A>> : std::true_type
{
    using mapped_type = V; ///< 值类型。
};

/** 是不是字符串视图 / C 字符串 / `std::string`。 */
template <class T>
struct isStringLike
    : std::bool_constant<std::is_same_v<std::decay_t<T>, std::string> ||
                         std::is_same_v<std::decay_t<T>, std::string_view> ||
                         std::is_same_v<std::decay_t<T>, const char *> ||
                         std::is_same_v<std::decay_t<T>, char *>>
{
};

/** 编译期 false（`static_assert` 里用来打印类型名）。 */
template <class T>
struct alwaysFalse : std::false_type
{
};

/**
 * @brief 解码上下文：路径、选项与诊断列表。
 */
struct Context
{
    DecodeOptions options;                ///< 选项。
    std::string path;                     ///< 当前 TOML 路径。
    std::vector<Diagnostic> *diagnostics; ///< 诊断输出。

    /**
     * @brief 记一条诊断。
     * @param code 短码。
     * @param message 说明。
     */
    void error(std::string code, std::string message)
    {
        Diagnostic diagnostic;
        diagnostic.path = path.empty() ? std::string("$") : path;
        diagnostic.code = std::move(code);
        diagnostic.message = std::move(message);
        diagnostics->push_back(std::move(diagnostic));
    }

    /**
     * @brief 进入一个表字段。
     * @param key 字段名。
     * @return 原来的路径（退出时还回去）。
     */
    std::string pushKey(std::string_view key)
    {
        const std::string saved = path;
        path += path.empty() ? std::string(key) : ("." + std::string(key));
        return saved;
    }

    /**
     * @brief 进入一个数组元素。
     * @param index 下标。
     * @return 原来的路径。
     */
    std::string pushIndex(std::size_t index)
    {
        const std::string saved = path;
        path += "[" + std::to_string(index) + "]";
        return saved;
    }

    /**
     * @brief 还原路径。
     * @param saved 之前保存的路径。
     */
    void restore(std::string saved) { path = std::move(saved); }
};

/**
 * @brief 把字符串解析成整数（用 `std::from_chars`）。
 * @param text 文本。
 * @param out 结果。
 * @return 成功返回 true。
 */
inline bool parseIntegerText(std::string_view text, std::int64_t &out)
{
    const std::from_chars_result result =
        std::from_chars(text.data(), text.data() + text.size(), out);
    return (result.ec == std::errc()) && (result.ptr == text.data() + text.size());
}

} // namespace detail

// ---------------------------------------------------------------------------
//  编码：模型 → TOML
// ---------------------------------------------------------------------------

/**
 * @brief 把一个 C++ 值编码成 TOML 值。
 *
 * @tparam T 类型。
 * @param value 值。
 * @return TOML 值。
 */
template <class T>
Value encode(const T &value)
{
    using U = std::decay_t<T>;

    if constexpr (std::is_same_v<U, Value>)
    {
        return value;
    }
    else if constexpr (std::is_same_v<U, DateTime>)
    {
        return Value(value);
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
    else if constexpr (std::is_floating_point_v<U>)
    {
        return Value(static_cast<double>(value));
    }
    else if constexpr (detail::isStringLike<U>::value)
    {
        return Value(std::string(std::string_view(value)));
    }
    else if constexpr (detail::isOptional<U>::value)
    {
        // TOML 没有 null：空的 optional 编码成一张空表（字段级编码会直接省略）。
        if (!value.has_value()) return Value(Object{});
        return encode(*value);
    }
    else if constexpr (detail::isVector<U>::value)
    {
        Array items;
        items.reserve(value.size());
        for (const auto &item : value) items.push_back(encode(item));
        return Value(std::move(items));
    }
    else if constexpr (detail::isStringMap<U>::value)
    {
        Object table;
        for (const auto &entry : value) table.insert(entry.first, encode(entry.second));
        return Value(std::move(table));
    }
    else if constexpr (hasBinding<U>::value)
    {
        Object table;
        std::apply(
            [&](const auto &...fields) {
                (encodeField(table, fields, value), ...);
            },
            Binding<U>::fields());
        return Value(std::move(table));
    }
    else
    {
        static_assert(detail::alwaysFalse<U>::value,
                      "这个类型没有 TOML 绑定：给它写 TOML_MODEL(...)，"
                      "或者换成 bool / 整数 / 浮点 / string / DateTime / vector / map / optional");
        return Value(Object{});
    }
}

/**
 * @brief 编码一个字段（空 optional 直接省略，不写进表）。
 *
 * @tparam Model 模型类型。
 * @tparam F 字段类型。
 * @param table 目标表。
 * @param one 字段绑定。
 * @param value 模型。
 */
template <class Model, class F>
void encodeField(Object &table, const F &one, const Model &value)
{
    using MemberType = std::decay_t<decltype(value.*(one.member))>;
    if constexpr (detail::isOptional<MemberType>::value)
    {
        if (!(value.*(one.member)).has_value()) return;
    }
    table.insert(std::string(one.name), encode(value.*(one.member)));
}

// ---------------------------------------------------------------------------
//  解码：TOML → 模型
// ---------------------------------------------------------------------------

/**
 * @brief 把一个 TOML 值解码进已有的对象。
 *
 * @tparam T 目标类型。
 * @param value TOML 值。
 * @param out 目标对象（成功时被覆盖）。
 * @param context 解码上下文（路径与诊断）。
 * @return 成功返回 true。
 */
template <class T>
bool decodeInto(const Value &value, T &out, detail::Context &context)
{
    using U = std::decay_t<T>;

    if constexpr (std::is_same_v<U, Value>)
    {
        out = value;
        return true;
    }
    else if constexpr (std::is_same_v<U, DateTime>)
    {
        const std::optional<DateTime> parsed = value.asDateTime();
        if (!parsed.has_value())
        {
            context.error("type-mismatch", std::string("期望日期时间，实际是 ") + value.typeName());
            return false;
        }
        out = *parsed;
        return true;
    }
    else if constexpr (std::is_same_v<U, bool>)
    {
        const std::optional<bool> parsed = value.asBool();
        if (!parsed.has_value())
        {
            context.error("type-mismatch", std::string("期望布尔，实际是 ") + value.typeName());
            return false;
        }
        out = *parsed;
        return true;
    }
    else if constexpr (std::is_enum_v<U>)
    {
        std::int64_t number = 0;
        if (const std::optional<std::int64_t> parsed = value.asInt())
        {
            number = *parsed;
        }
        else if (context.options.allowStringNumbers && value.isString())
        {
            if (!detail::parseIntegerText(value.asString().value(), number))
            {
                context.error("type-mismatch", std::string("期望枚举，实际是字符串 \"") +
                                                   std::string(value.asString().value()) + "\"");
                return false;
            }
        }
        else
        {
            context.error("type-mismatch", std::string("期望枚举，实际是 ") + value.typeName());
            return false;
        }
        out = static_cast<U>(number);
        return true;
    }
    else if constexpr (std::is_integral_v<U>)
    {
        std::optional<std::int64_t> parsed = value.asInt();
        if (!parsed.has_value() && context.options.allowStringNumbers && value.isString())
        {
            std::int64_t number = 0;
            if (detail::parseIntegerText(value.asString().value(), number)) parsed = number;
        }
        if (!parsed.has_value())
        {
            context.error("type-mismatch", std::string("期望整数，实际是 ") + value.typeName());
            return false;
        }
        out = static_cast<U>(*parsed);
        return true;
    }
    else if constexpr (std::is_floating_point_v<U>)
    {
        std::optional<double> parsed = value.asDouble();
        if (!parsed.has_value() && context.options.allowStringNumbers && value.isString())
        {
            const std::string text(value.asString().value());
            char *end = nullptr;
            const double number = std::strtod(text.c_str(), &end);
            if ((end != nullptr) && (*end == '\0')) parsed = number;
        }
        if (!parsed.has_value())
        {
            context.error("type-mismatch", std::string("期望浮点，实际是 ") + value.typeName());
            return false;
        }
        out = static_cast<U>(*parsed);
        return true;
    }
    else if constexpr (detail::isStringLike<U>::value)
    {
        const std::optional<std::string_view> parsed = value.asString();
        if (!parsed.has_value())
        {
            context.error("type-mismatch", std::string("期望字符串，实际是 ") + value.typeName());
            return false;
        }
        if constexpr (std::is_same_v<U, std::string>)
        {
            out = std::string(*parsed);
        }
        else if constexpr (std::is_same_v<U, std::string_view>)
        {
            context.error("unsupported-target", "string_view 不能作为解码目标（只用于编码）");
            return false;
        }
        else
        {
            context.error("unsupported-target", "不能把 TOML 字符串解码进 C 字符串指针");
            return false;
        }
        return true;
    }
    else if constexpr (detail::isOptional<U>::value)
    {
        // TOML 没有 null：字段存在就解码，缺字段由模型层把它留空。
        typename detail::isOptional<U>::inner inner{};
        if (!decodeInto(value, inner, context)) return false;
        out = std::move(inner);
        return true;
    }
    else if constexpr (detail::isVector<U>::value)
    {
        const Array *items = value.asArray();
        if (items == nullptr)
        {
            context.error("type-mismatch", std::string("期望数组，实际是 ") + value.typeName());
            return false;
        }
        U result;
        result.reserve(items->size());
        bool ok = true;
        for (std::size_t index = 0; index < items->size(); ++index)
        {
            const std::string saved = context.pushIndex(index);
            typename detail::isVector<U>::inner item{};
            const bool itemOk = decodeInto((*items)[index], item, context);
            context.restore(saved);
            if (!itemOk) ok = false;
            else result.push_back(std::move(item));
        }
        if (ok) out = std::move(result);
        return ok;
    }
    else if constexpr (detail::isStringMap<U>::value)
    {
        const Object *table = value.asTable();
        if (table == nullptr)
        {
            context.error("type-mismatch", std::string("期望表，实际是 ") + value.typeName());
            return false;
        }
        U result;
        bool ok = true;
        for (const Member &member : *table)
        {
            const std::string saved = context.pushKey(member.key);
            typename detail::isStringMap<U>::mapped_type item{};
            const bool itemOk = decodeInto(member.value, item, context);
            context.restore(saved);
            if (!itemOk) ok = false;
            else result.emplace(member.key, std::move(item));
        }
        if (ok) out = std::move(result);
        return ok;
    }
    else if constexpr (hasBinding<U>::value)
    {
        const Object *table = value.asTable();
        if (table == nullptr)
        {
            context.error("type-mismatch", std::string("期望表，实际是 ") + value.typeName());
            return false;
        }

        bool ok = true;
        std::apply(
            [&](const auto &...fields) {
                auto handle = [&](const auto &one) {
                    const Value *member = table->tryGet(one.name);
                    if (member == nullptr)
                    {
                        using MemberType = std::decay_t<decltype(out.*(one.member))>;
                        if constexpr (!detail::isOptional<MemberType>::value)
                        {
                            if (context.options.strictMissing)
                            {
                                const std::string saved = context.pushKey(one.name);
                                context.error("missing-field",
                                              "缺少字段 \"" + std::string(one.name) + "\"");
                                context.restore(saved);
                                ok = false;
                            }
                        }
                        return;
                    }
                    const std::string saved = context.pushKey(one.name);
                    const bool fieldOk = decodeInto(*member, out.*(one.member), context);
                    context.restore(saved);
                    if (!fieldOk) ok = false;
                };
                (handle(fields), ...);
            },
            Binding<U>::fields());

        if (context.options.strictUnknown)
        {
            for (const Member &member : *table)
            {
                const bool known = std::apply(
                    [&](const auto &...fields) { return ((fields.name == member.key) || ...); },
                    Binding<U>::fields());
                if (known) continue;
                const std::string saved = context.pushKey(member.key);
                context.error("unknown-field", "模型里没有字段 \"" + member.key + "\"");
                context.restore(saved);
                ok = false;
            }
        }
        return ok;
    }
    else
    {
        static_assert(detail::alwaysFalse<U>::value,
                      "这个类型没有 TOML 绑定：给它写 TOML_MODEL(...)，"
                      "或者换成 bool / 整数 / 浮点 / string / DateTime / vector / map / optional");
        return false;
    }
}

/**
 * @brief 把 TOML 解码成模型。
 *
 * @tparam T 目标类型。
 * @param value TOML 值。
 * @param options 解码选项。
 * @return 解码结果（失败时带 TOML 路径的诊断）。
 */
template <class T>
DecodeResult<std::decay_t<T>> decode(const Value &value, DecodeOptions options = DecodeOptions{})
{
    using U = std::decay_t<T>;
    DecodeResult<U> result;
    detail::Context context{options, {}, &result.diagnostics};
    U out{};
    if (decodeInto(value, out, context))
    {
        result.value = std::move(out);
    }
    return result;
}

} // namespace toml

#endif // PARSE_ENGINE_TOML_BIND_HPP
