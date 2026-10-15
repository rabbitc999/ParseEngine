#pragma once
// 极简 JSON（对象保持插入顺序，输出稳定，便于测试与差分）。
// 只依赖标准库：用于 LSP 的 JSON-RPC 报文。
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace lsp {

class Json {
public:
    enum class Kind { Null, Bool, Number, String, Array, Object };

    Json() = default;
    /**
     * 造一个 JSON null 值（LSP 报文里的显式 null，与"成员不存在"是两回事）。
     *
     * @return 值为 null 的 JSON 对象
     */
    static Json null();
    /**
     * 造一个布尔值。
     *
     * @param value 真 / 假
     * @return 值为该布尔的 JSON 对象
     */
    static Json boolean(bool value);
    /**
     * 造一个整数值，`dump()` 输出不带小数点。
     *
     * LSP 的 `line` / `character` / `id` 都用它，避免浮点格式带来的差异。
     *
     * @param value 整数值
     * @return 值为该整数的 JSON 对象
     */
    static Json integer(long long value);
    /**
     * 造一个浮点值。
     *
     * @param value 浮点值
     * @return 值为该浮点的 JSON 对象
     */
    static Json number(double value);
    /**
     * 造一个字符串值（UTF-8；`dump()` 按 JSON 规则转义，含 `\uXXXX`）。
     *
     * @param value 字符串内容
     * @return 值为该字符串的 JSON 对象
     */
    static Json string(std::string value);
    /**
     * 造一个空数组，随后用 `push()` 追加元素。
     *
     * @return 空数组
     */
    static Json array();
    /**
     * 造一个空对象，随后用 `set()` 写成员（成员保持插入顺序，输出稳定便于差分）。
     *
     * @return 空对象
     */
    static Json object();

    /**
     * 对象：按键写入（重复键覆盖），保留首次插入的位置。
     */
    Json& set(const std::string& key, Json value);
    /** 数组：追加。 */
    Json& push(Json value);

    Kind kind() const { return kind_; }
    bool isNull() const { return kind_ == Kind::Null; }
    bool isBool() const { return kind_ == Kind::Bool; }
    bool isNumber() const { return kind_ == Kind::Number; }
    bool isString() const { return kind_ == Kind::String; }
    bool isArray() const { return kind_ == Kind::Array; }
    bool isObject() const { return kind_ == Kind::Object; }

    /**
     * 取字符串内容。
     *
     * @return 字符串常引用；当前值不是字符串时返回**空串**的常引用
     *         （要区分"空串"与"类型不对"，先用 isString() 判断）
     */
    const std::string& asString() const;
    /**
     * 取浮点值，整数值自动提升为浮点。
     *
     * @param fallback 当前值不是数字时返回它
     * @return 数值
     */
    double asNumber(double fallback = 0.0) const;
    /**
     * 取整数值；浮点会截断（编辑器偶尔发 `3.0` 这种行列写法）。
     *
     * @param fallback 当前值不是数字时返回它
     * @return 整数值
     */
    long long asInteger(long long fallback = 0) const;
    /**
     * 取布尔值。
     *
     * @param fallback 当前值不是布尔时返回它
     * @return 布尔值
     */
    bool asBool(bool fallback = false) const;

    /** 对象成员（插入顺序）；非对象返回空。 */
    const std::vector<std::pair<std::string, Json>>& members() const;
    /** 数组元素；非数组返回空。 */
    const std::vector<Json>& items() const;
    /**
     * 元素个数：数组是元素数、对象是成员数，其他类型返回 0。
     *
     * @return 元素 / 成员个数
     */
    std::size_t size() const;

    /**
     * 取对象成员；不存在或不是对象时返回 null 单例。
     */
    Json get(const std::string& key) const;
    /**
     * 查找对象成员，返回**指针**，以便区分"成员存在但值为 null"与"成员不存在"。
     *
     * @param key 成员名
     * @return 命中时指向该成员值（生命周期同本文档）；未命中或不是对象时返回 nullptr
     */
    const Json* find(const std::string& key) const;
    std::string getString(const std::string& key, const std::string& fallback = {}) const;
    /**
     * 取对象成员的整数值（等价于 get(key).asInteger(fallback)）。
     *
     * @param key      成员名
     * @param fallback 成员不存在或不是数字时返回它
     * @return 整数值
     */
    long long getInteger(const std::string& key, long long fallback = 0) const;

    /** 序列化；indent >= 0 时美化输出。 */
    std::string dump(int indent = -1) const;
    /**
     * 解析；失败时返回 false 并（可选）给出带位置的原因。
     */
    static bool parse(const std::string& text, Json& out, std::string* error = nullptr);

private:
    Kind kind_ = Kind::Null;
    bool bool_ = false;
    double number_ = 0.0;
    std::string string_;
    std::vector<Json> items_;
    std::vector<std::pair<std::string, Json>> members_;
};

} // namespace lsp