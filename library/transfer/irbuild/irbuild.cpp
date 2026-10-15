#include "irbuild.hpp"

#include <sstream>
#include <stdexcept>

namespace t1t {

// ---------------------------------------------------------------- 四元式

QuadBuilder& QuadBuilder::raw(const std::string& op, const std::string& a, const std::string& b,
                              const std::string& result, const std::string& extra) {
    program_.push_back(transfer::Quad{op, a, b, result, extra});
    return *this;
}

QuadBuilder& QuadBuilder::declare(const std::string& name, const std::string& type) {
    return raw("decl", type.empty() ? "i16" : type, "global", name);
}

QuadBuilder& QuadBuilder::assign(const std::string& value, const std::string& target) {
    return raw(":=", value, "_", target);
}

QuadBuilder& QuadBuilder::op(const std::string& op, const std::string& a, const std::string& b,
                             const std::string& target, const std::string& type) {
    return raw(type.empty() ? op : op + ":" + type, a, b, target);
}

QuadBuilder& QuadBuilder::add(const std::string& a, const std::string& b, const std::string& t,
                              const std::string& type) { return op("+", a, b, t, type); }
QuadBuilder& QuadBuilder::sub(const std::string& a, const std::string& b, const std::string& t,
                              const std::string& type) { return op("-", a, b, t, type); }
QuadBuilder& QuadBuilder::mul(const std::string& a, const std::string& b, const std::string& t,
                              const std::string& type) { return op("*", a, b, t, type); }
QuadBuilder& QuadBuilder::div(const std::string& a, const std::string& b, const std::string& t,
                              const std::string& type) { return op("/", a, b, t, type); }

QuadBuilder& QuadBuilder::label(const std::string& name) { return raw("label", "_", "_", name); }
QuadBuilder& QuadBuilder::jump(const std::string& name) { return raw("j", "_", "_", name); }
QuadBuilder& QuadBuilder::jumpIfZero(const std::string& value, const std::string& name) {
    return raw("jz", value, "_", name);
}
QuadBuilder& QuadBuilder::write(const std::string& value) { return raw("write", value, "_", "_"); }
QuadBuilder& QuadBuilder::halt() { return raw("halt", "_", "_", "_"); }

std::string QuadBuilder::text() const { return transfer::write(program_); }

TypedProgram QuadBuilder::lower() const { return lowerQuads(program_).program; }

// ---------------------------------------------------------------- 三元式

std::size_t TripleBuilder::add(const std::string& op, const std::string& a, const std::string& b) {
    irbridge::Triple triple;
    triple.op = op;
    triple.a = a;
    triple.b = b;
    triples_.push_back(triple);
    return triples_.size() - 1;
}

std::size_t TripleBuilder::assign(const std::string& value, const std::string& target) {
    return add(":=", value, target);
}

std::size_t TripleBuilder::arithmetic(const std::string& op, const std::string& a,
                                      const std::string& b) { return add(op, a, b); }

std::size_t TripleBuilder::label(const std::string& name) { return add("label", name, "_"); }
std::size_t TripleBuilder::jump(const std::string& name) { return add("j", "_", name); }
std::size_t TripleBuilder::jumpIfZero(const std::string& value, const std::string& name) {
    return add("jz", value, name);
}
std::size_t TripleBuilder::write(const std::string& value) { return add("write", value, "_"); }
std::size_t TripleBuilder::halt() { return add("halt", "_", "_"); }

std::string TripleBuilder::ref(std::size_t index) { return "(" + std::to_string(index) + ")"; }

std::string TripleBuilder::text() const { return irbridge::writeTriples(triples_); }

irbridge::Quads TripleBuilder::toQuads() const {
    // 经过一次文本往返：`(k)` 这类引用由 irbridge 自己的读取器解析成下标
    irbridge::Triples parsed;
    std::vector<std::string> errors;
    std::istringstream text(irbridge::writeTriples(triples_));
    if (!irbridge::readTriples(text.str(), parsed, errors)) {
        throw std::runtime_error(errors.empty() ? "三元式文本读取失败" : errors.front());
    }
    irbridge::Quads quads;
    if (!irbridge::triplesToQuads(parsed, quads, errors)) {
        throw std::runtime_error(errors.empty() ? "三元式转四元式失败" : errors.front());
    }
    return quads;
}

TypedProgram TripleBuilder::toTT3() const {
    // 走同一套入口：三元式 → 四元式 → TT3（保留位数）
    std::vector<std::string> errors;
    const irbridge::Quads quads = toQuads();
    transfer::Program program;
    std::istringstream text(irbridge::toTransfer(quads));
    program = transfer::read(text);
    return lowerQuads(program).program;
}

} // namespace t1t
