#include "pl0super_detail.hpp"

namespace pl0s {

std::vector<Token> tokenize(const std::string& source, std::string* error) {
    using detail::identPart;
    using detail::identStart;
    using detail::keywords;
    using detail::lineError;
    std::vector<Token> tokens;
    std::size_t i = 0;
    int line = 1;
    const auto fail = [&](const std::string& what) {
        if (error) *error = lineError(line, what);
        return std::vector<Token>{};
    };
    while (i < source.size()) {
        const char c = source[i];
        if (c == '\n') {
            ++line;
            ++i;
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\r') {
            ++i;
            continue;
        }
        if (c == '{') {                                   // { 注释 }
            while (i < source.size() && source[i] != '}') {
                if (source[i] == '\n') ++line;
                ++i;
            }
            if (i >= source.size()) return fail("注释 { 没有闭合");
            ++i;
            continue;
        }
        if (c == '(' && i + 1 < source.size() && source[i + 1] == '*') {
            i += 2;
            while (i + 1 < source.size() && !(source[i] == '*' && source[i + 1] == ')')) {
                if (source[i] == '\n') ++line;
                ++i;
            }
            if (i + 1 >= source.size()) return fail("注释 (* 没有闭合");
            i += 2;
            continue;
        }
        if (c == '/' && i + 1 < source.size() && source[i + 1] == '/') {
            while (i < source.size() && source[i] != '\n') ++i;
            continue;
        }
        if (identStart(c)) {
            const std::size_t start = i;
            while (i < source.size() && identPart(source[i])) ++i;
            Token token;
            token.text = source.substr(start, i - start);
            token.kind = keywords().count(token.text) != 0 ? TokKind::Keyword : TokKind::Ident;
            token.line = line;
            tokens.push_back(std::move(token));
            continue;
        }
        if ((c >= '0' && c <= '9') ||
            (c == '.' && i + 1 < source.size() && source[i + 1] >= '0' && source[i + 1] <= '9')) {
            const std::size_t start = i;
            bool isFloat = false;
            while (i < source.size() && source[i] >= '0' && source[i] <= '9') ++i;
            if (i < source.size() && source[i] == '.') {
                isFloat = true;
                ++i;
                while (i < source.size() && source[i] >= '0' && source[i] <= '9') ++i;
            }
            if (i < source.size() && (source[i] == 'e' || source[i] == 'E')) {
                std::size_t j = i + 1;
                if (j < source.size() && (source[j] == '+' || source[j] == '-')) ++j;
                if (j < source.size() && source[j] >= '0' && source[j] <= '9') {
                    isFloat = true;
                    i = j;
                    while (i < source.size() && source[i] >= '0' && source[i] <= '9') ++i;
                }
            }
            Token token;
            token.text = source.substr(start, i - start);
            token.line = line;
            token.kind = TokKind::Number;
            token.isFloat = isFloat;
            if (isFloat) {
                token.real = std::atof(token.text.c_str());
                token.number = static_cast<std::int64_t>(token.real);
            } else {
                token.number = std::atoll(token.text.c_str());
            }
            tokens.push_back(std::move(token));
            continue;
        }
        if (c == '"') {
            ++i;
            std::string text;
            bool closed = false;
            while (i < source.size()) {
                const char ch = source[i];
                if (ch == '"') {
                    closed = true;
                    ++i;
                    break;
                }
                if (ch == '\n') break;
                if (ch == '\\' && i + 1 < source.size()) {
                    const char next = source[i + 1];
                    if (next == 'n') text += '\n';
                    else if (next == 't') text += '\t';
                    else if (next == '"') text += '"';
                    else if (next == '\\') text += '\\';
                    else return fail(std::string("字符串里不认的转义：\\") + next);
                    i += 2;
                    continue;
                }
                text += ch;
                ++i;
            }
            if (!closed) return fail("字符串字面量没有闭合");
            Token token;
            token.kind = TokKind::Text;
            token.text = text;
            token.line = line;
            tokens.push_back(std::move(token));
            continue;
        }
        const std::string two = source.substr(i, 2);
        if (two == ":=" || two == "<=" || two == ">=" || two == "<>") {
            Token token;
            token.kind = TokKind::Symbol;
            token.text = two;
            token.line = line;
            tokens.push_back(std::move(token));
            i += 2;
            continue;
        }
        if (std::string("+-*/%=<>()[];:.,").find(c) != std::string::npos) {
            Token token;
            token.kind = TokKind::Symbol;
            token.text = std::string(1, c);
            token.line = line;
            tokens.push_back(std::move(token));
            ++i;
            continue;
        }
        return fail(std::string("认不出的字符：") + c);
    }
    Token end;
    end.kind = TokKind::End;
    end.line = line;
    tokens.push_back(end);
    if (error) error->clear();
    return tokens;
}

} // namespace pl0s
