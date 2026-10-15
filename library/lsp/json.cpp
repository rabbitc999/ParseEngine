#include "json.hpp"

#include <cmath>
#include <cstdio>
#include <sstream>

namespace lsp {
namespace {

void appendEscaped(std::string& out, const std::string& text) {
    out.push_back('"');
    for (unsigned char c : text) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char buffer[8];
                std::snprintf(buffer, sizeof(buffer), "\\u%04x", c);
                out += buffer;
            } else {
                out.push_back(static_cast<char>(c));
            }
        }
    }
    out.push_back('"');
}

void appendNumber(std::string& out, double value) {
    if (std::isfinite(value) && std::floor(value) == value &&
        std::fabs(value) < 9.0e15) {
        out += std::to_string(static_cast<long long>(value));
        return;
    }
    std::ostringstream stream;
    stream.precision(17);
    stream << value;
    out += stream.str();
}

void dumpValue(const Json& value, std::string& out, int indent, int depth) {
    const std::string pad = indent >= 0 ? std::string(static_cast<std::size_t>(indent) * (depth + 1), ' ') : std::string();
    const std::string closePad = indent >= 0 ? std::string(static_cast<std::size_t>(indent) * depth, ' ') : std::string();
    switch (value.kind()) {
    case Json::Kind::Null: out += "null"; break;
    case Json::Kind::Bool: out += value.asBool() ? "true" : "false"; break;
    case Json::Kind::Number: appendNumber(out, value.asNumber()); break;
    case Json::Kind::String: appendEscaped(out, value.asString()); break;
    case Json::Kind::Array: {
        if (value.items().empty()) { out += "[]"; break; }
        out += '[';
        for (std::size_t i = 0; i < value.items().size(); ++i) {
            if (i) out += ',';
            if (indent >= 0) { out += '\n'; out += pad; }
            dumpValue(value.items()[i], out, indent, depth + 1);
        }
        if (indent >= 0) { out += '\n'; out += closePad; }
        out += ']';
        break;
    }
    case Json::Kind::Object: {
        if (value.members().empty()) { out += "{}"; break; }
        out += '{';
        for (std::size_t i = 0; i < value.members().size(); ++i) {
            if (i) out += ',';
            if (indent >= 0) { out += '\n'; out += pad; }
            appendEscaped(out, value.members()[i].first);
            out += indent >= 0 ? ": " : ":";
            dumpValue(value.members()[i].second, out, indent, depth + 1);
        }
        if (indent >= 0) { out += '\n'; out += closePad; }
        out += '}';
        break;
    }
    }
}

class Parser {
public:
    Parser(const std::string& text) : text_(text) {}

    bool parse(Json& out) {
        skipSpace();
        if (!value(out)) return false;
        skipSpace();
        if (index_ != text_.size()) return fail("trailing content");
        return true;
    }
    const std::string& error() const { return error_; }

private:
    bool fail(const std::string& what) {
        if (error_.empty()) {
            error_ = what + " at offset " + std::to_string(index_);
        }
        return false;
    }
    void skipSpace() {
        while (index_ < text_.size() &&
               (text_[index_] == ' ' || text_[index_] == '\t' || text_[index_] == '\n' ||
                text_[index_] == '\r')) {
            ++index_;
        }
    }
    bool value(Json& out) {
        if (index_ >= text_.size()) return fail("unexpected end");
        const char c = text_[index_];
        if (c == '{') return object(out);
        if (c == '[') return array(out);
        if (c == '"') {
            std::string text;
            if (!string(text)) return false;
            out = Json::string(text);
            return true;
        }
        if (c == 't' || c == 'f') return literal(out);
        if (c == 'n') {
            if (text_.compare(index_, 4, "null") != 0) return fail("invalid literal");
            index_ += 4;
            out = Json::null();
            return true;
        }
        return number(out);
    }
    bool literal(Json& out) {
        if (text_.compare(index_, 4, "true") == 0) {
            index_ += 4;
            out = Json::boolean(true);
            return true;
        }
        if (text_.compare(index_, 5, "false") == 0) {
            index_ += 5;
            out = Json::boolean(false);
            return true;
        }
        return fail("invalid literal");
    }
    bool number(Json& out) {
        const std::size_t start = index_;
        if (index_ < text_.size() && (text_[index_] == '-' || text_[index_] == '+')) ++index_;
        bool digits = false;
        while (index_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[index_]))) {
            ++index_;
            digits = true;
        }
        if (index_ < text_.size() && text_[index_] == '.') {
            ++index_;
            while (index_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[index_]))) {
                ++index_;
                digits = true;
            }
        }
        if (!digits) return fail("invalid number");
        if (index_ < text_.size() && (text_[index_] == 'e' || text_[index_] == 'E')) {
            ++index_;
            if (index_ < text_.size() && (text_[index_] == '-' || text_[index_] == '+')) ++index_;
            bool expDigits = false;
            while (index_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[index_]))) {
                ++index_;
                expDigits = true;
            }
            if (!expDigits) return fail("invalid exponent");
        }
        try {
            out = Json::number(std::stod(text_.substr(start, index_ - start)));
        } catch (const std::exception&) {
            return fail("invalid number");
        }
        return true;
    }
    bool string(std::string& out) {
        if (text_[index_] != '"') return fail("expected string");
        ++index_;
        while (index_ < text_.size()) {
            const char c = text_[index_++];
            if (c == '"') return true;
            if (c != '\\') { out.push_back(c); continue; }
            if (index_ >= text_.size()) return fail("bad escape");
            const char esc = text_[index_++];
            switch (esc) {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'u': {
                unsigned code = 0;
                if (!hex4(code)) return false;
                // 代理对：\uD800-\uDBFF 后面跟 \uDC00-\uDFFF。
                if (code >= 0xD800 && code <= 0xDBFF && text_.compare(index_, 2, "\\u") == 0) {
                    const std::size_t save = index_;
                    index_ += 2;
                    unsigned low = 0;
                    if (hex4(low) && low >= 0xDC00 && low <= 0xDFFF) {
                        code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                    } else {
                        index_ = save;
                    }
                }
                appendUtf8(out, code);
                break;
            }
            default: return fail("unknown escape");
            }
        }
        return fail("unterminated string");
    }
    bool hex4(unsigned& out) {
        if (index_ + 4 > text_.size()) return fail("bad \\u escape");
        unsigned value = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = text_[index_++];
            value <<= 4;
            if (c >= '0' && c <= '9') value |= static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') value |= static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') value |= static_cast<unsigned>(c - 'A' + 10);
            else return fail("bad \\u escape");
        }
        out = value;
        return true;
    }
    static void appendUtf8(std::string& out, unsigned code) {
        if (code < 0x80) {
            out.push_back(static_cast<char>(code));
        } else if (code < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (code >> 6)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else if (code < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (code >> 12)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (code >> 18)));
            out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        }
    }
    bool object(Json& out) {
        out = Json::object();
        ++index_; // {
        skipSpace();
        if (index_ < text_.size() && text_[index_] == '}') { ++index_; return true; }
        for (;;) {
            skipSpace();
            std::string key;
            if (!string(key)) return false;
            skipSpace();
            if (index_ >= text_.size() || text_[index_] != ':') return fail("expected ':'");
            ++index_;
            skipSpace();
            Json member;
            if (!value(member)) return false;
            out.set(key, std::move(member));
            skipSpace();
            if (index_ < text_.size() && text_[index_] == ',') { ++index_; continue; }
            if (index_ < text_.size() && text_[index_] == '}') { ++index_; return true; }
            return fail("expected ',' or '}'");
        }
    }
    bool array(Json& out) {
        out = Json::array();
        ++index_; // [
        skipSpace();
        if (index_ < text_.size() && text_[index_] == ']') { ++index_; return true; }
        for (;;) {
            skipSpace();
            Json item;
            if (!value(item)) return false;
            out.push(std::move(item));
            skipSpace();
            if (index_ < text_.size() && text_[index_] == ',') { ++index_; continue; }
            if (index_ < text_.size() && text_[index_] == ']') { ++index_; return true; }
            return fail("expected ',' or ']'");
        }
    }

    const std::string& text_;
    std::size_t index_ = 0;
    std::string error_;
};

const std::string& emptyString() {
    static const std::string value;
    return value;
}
const std::vector<Json>& emptyItems() {
    static const std::vector<Json> value;
    return value;
}
const std::vector<std::pair<std::string, Json>>& emptyMembers() {
    static const std::vector<std::pair<std::string, Json>> value;
    return value;
}

} // namespace

Json Json::null() { return Json(); }
Json Json::boolean(bool value) { Json json; json.kind_ = Kind::Bool; json.bool_ = value; return json; }
Json Json::integer(long long value) {
    Json json;
    json.kind_ = Kind::Number;
    json.number_ = static_cast<double>(value);
    return json;
}
Json Json::number(double value) { Json json; json.kind_ = Kind::Number; json.number_ = value; return json; }
Json Json::string(std::string value) {
    Json json;
    json.kind_ = Kind::String;
    json.string_ = std::move(value);
    return json;
}
Json Json::array() { Json json; json.kind_ = Kind::Array; return json; }
Json Json::object() { Json json; json.kind_ = Kind::Object; return json; }

Json& Json::set(const std::string& key, Json value) {
    kind_ = Kind::Object;
    for (auto& member : members_) {
        if (member.first == key) { member.second = std::move(value); return *this; }
    }
    members_.emplace_back(key, std::move(value));
    return *this;
}
Json& Json::push(Json value) {
    kind_ = Kind::Array;
    items_.push_back(std::move(value));
    return *this;
}
const std::string& Json::asString() const { return kind_ == Kind::String ? string_ : emptyString(); }
double Json::asNumber(double fallback) const { return kind_ == Kind::Number ? number_ : fallback; }
long long Json::asInteger(long long fallback) const {
    return kind_ == Kind::Number ? static_cast<long long>(number_) : fallback;
}
bool Json::asBool(bool fallback) const { return kind_ == Kind::Bool ? bool_ : fallback; }
const std::vector<std::pair<std::string, Json>>& Json::members() const {
    return kind_ == Kind::Object ? members_ : emptyMembers();
}
const std::vector<Json>& Json::items() const { return kind_ == Kind::Array ? items_ : emptyItems(); }
std::size_t Json::size() const {
    if (kind_ == Kind::Array) return items_.size();
    if (kind_ == Kind::Object) return members_.size();
    return 0;
}
Json Json::get(const std::string& key) const {
    const Json* found = find(key);
    return found ? *found : Json();
}
const Json* Json::find(const std::string& key) const {
    if (kind_ != Kind::Object) return nullptr;
    for (const auto& member : members_) if (member.first == key) return &member.second;
    return nullptr;
}
std::string Json::getString(const std::string& key, const std::string& fallback) const {
    const Json* found = find(key);
    return found && found->isString() ? found->asString() : fallback;
}
long long Json::getInteger(const std::string& key, long long fallback) const {
    const Json* found = find(key);
    return found && found->isNumber() ? found->asInteger() : fallback;
}
std::string Json::dump(int indent) const {
    std::string out;
    dumpValue(*this, out, indent, 0);
    return out;
}
bool Json::parse(const std::string& text, Json& out, std::string* error) {
    Parser parser(text);
    if (!parser.parse(out)) {
        if (error) *error = parser.error();
        return false;
    }
    return true;
}

} // namespace lsp
