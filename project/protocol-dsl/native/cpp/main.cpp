#include "parser.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;

struct Tok { std::string text; int line = 0; };
struct EnumItem { std::string name; std::uint64_t value = 0; };
struct Field {
    std::string name, type, length;
    bool bytes = false, fixed = false;
};
struct Decl {
    std::string kind, name, base;
    std::vector<EnumItem> items;
    std::vector<Field> fields;
};
struct Schema { std::string name, endian; std::vector<Decl> decls; };

static void leaves(const Parser::Tree &node, std::vector<Tok> &out)
{
    if (node.childCount() == 0u) {
        if (!node.text().empty()) out.push_back({node.text(), node.location().line});
        return;
    }
    for (std::size_t i = 0; i < node.childCount(); ++i) leaves(*node.child(i), out);
}

class Cursor {
public:
    explicit Cursor(std::vector<Tok> tokens) : m_tokens(std::move(tokens)) {}
    bool done() const { return m_pos == m_tokens.size(); }
    int line() const { return done() ? 0 : m_tokens[m_pos].line; }
    std::string take() {
        if (done()) throw std::runtime_error("unexpected end of protocol");
        return m_tokens[m_pos++].text;
    }
    void expect(const std::string &text) {
        const std::string actual = take();
        if (actual != text) throw std::runtime_error("line " + std::to_string(line()) +
                                                     ": expected `" + text + "`, got `" + actual + "`");
    }
private:
    std::vector<Tok> m_tokens;
    std::size_t m_pos = 0;
};

static bool integerType(const std::string &s)
{
    static const std::set<std::string> names = {"u8","u16","u32","u64","i8","i16","i32","i64"};
    return names.count(s) != 0u;
}
static bool unsignedType(const std::string &s)
{
    return s == "u8" || s == "u16" || s == "u32" || s == "u64";
}
static void checkName(const std::string &s, const char *what)
{
    static const std::set<std::string> reserved = {
        "alignas","alignof","asm","auto","bool","break","case","catch","char","class","const",
        "constexpr","continue","default","delete","do","double","else","enum","explicit","extern",
        "false","float","for","friend","goto","if","inline","int","long","namespace","new","noexcept",
        "nullptr","operator","private","protected","public","register","return","short","signed","sizeof",
        "static","struct","switch","template","this","throw","true","try","typedef","typename","union",
        "unsigned","using","virtual","void","volatile","while","decode","decode_from","encode","encode_to"};
    if(s.empty() || s[0]=='_' || reserved.count(s)) throw std::runtime_error(std::string(what)+" `"+s+"` is reserved");
}
static int widthOf(const std::string &s)
{
    if (s == "u8" || s == "i8") return 1;
    if (s == "u16" || s == "i16") return 2;
    if (s == "u32" || s == "i32") return 4;
    return 8;
}
static std::string cppType(const std::string &s)
{
    static const std::map<std::string,std::string> names = {
        {"u8","std::uint8_t"},{"u16","std::uint16_t"},{"u32","std::uint32_t"},{"u64","std::uint64_t"},
        {"i8","std::int8_t"},{"i16","std::int16_t"},{"i32","std::int32_t"},{"i64","std::int64_t"}};
    const auto it = names.find(s); return it == names.end() ? s : it->second;
}
static std::string snake(const std::string &s)
{
    std::string out;
    for (std::size_t i=0;i<s.size();++i) {
        if (i && std::isupper(static_cast<unsigned char>(s[i])) &&
            std::islower(static_cast<unsigned char>(s[i-1]))) out.push_back('_');
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(s[i]))));
    }
    return out;
}

static Schema buildSchema(Cursor &c)
{
    Schema s; c.expect("protocol"); s.name=c.take(); checkName(s.name,"protocol name"); c.expect(";");
    c.expect("endian"); s.endian=c.take(); c.expect(";");
    std::map<std::string,Decl> known;
    while (!c.done()) {
        Decl d; d.kind=c.take(); d.name=c.take(); checkName(d.name,"type name");
        if (known.count(d.name) || integerType(d.name) || d.name=="bytes")
            throw std::runtime_error("duplicate type `"+d.name+"`");
        if (d.kind=="enum") {
            c.expect(":"); d.base=c.take(); c.expect("{");
            std::set<std::string> names; std::set<std::uint64_t> values;
            const int bits=widthOf(d.base)*8;
            const std::uint64_t limit=bits==64?std::numeric_limits<std::uint64_t>::max():((std::uint64_t(1)<<bits)-1u);
            while (true) {
                const std::string n=c.take(); if(n=="}") break;
                checkName(n,"enum item"); c.expect("="); const std::uint64_t v=std::stoull(c.take()); c.expect(";");
                if(!names.insert(n).second || !values.insert(v).second) throw std::runtime_error("duplicate enum item");
                if(v>limit) throw std::runtime_error("enum value does not fit "+d.base);
                d.items.push_back({n,v});
            }
            c.expect(";");
        } else if (d.kind=="packet") {
            c.expect("{"); std::map<std::string,std::string> fields;
            while (true) {
                std::string type=c.take(); if(type=="}") break;
                Field f;
                if(type=="bytes") {
                    f.bytes=true; f.name=c.take(); c.expect("["); f.length=c.take(); c.expect("]"); c.expect(";");
                    f.fixed=std::all_of(f.length.begin(),f.length.end(),[](unsigned char x){return std::isdigit(x)!=0;});
                    if(f.fixed) { if(std::stoull(f.length)==0) throw std::runtime_error("byte length must be positive"); }
                    else if(!fields.count(f.length) || !unsignedType(fields[f.length]))
                        throw std::runtime_error("length field `"+f.length+"` must be an earlier unsigned integer");
                } else {
                    f.type=type; f.name=c.take(); c.expect(";");
                    if(!integerType(type) && !known.count(type)) throw std::runtime_error("unknown or forward type `"+type+"`");
                }
                checkName(f.name,"field name"); if(fields.count(f.name)) throw std::runtime_error("duplicate field `"+f.name+"`");
                fields[f.name]=f.type; d.fields.push_back(std::move(f));
            }
            c.expect(";");
        } else throw std::runtime_error("expected enum or packet");
        known[d.name]=d; s.decls.push_back(std::move(d));
    }
    return s;
}

static std::string declarations(const Schema &s)
{
    std::map<std::string,Decl> known; std::ostringstream o; const char *little=s.endian=="little"?"true":"false";
    for(const Decl &d:s.decls) {
        if(d.kind=="enum") {
            o<<"enum class "<<d.name<<" : "<<cppType(d.base)<<" {\n";
            for(std::size_t i=0;i<d.items.size();++i) o<<"    "<<d.items[i].name<<" = "<<d.items[i].value<<(i+1==d.items.size()?"\n":",\n");
            o<<"};\n\ninline bool is_valid_"<<d.name<<"("<<d.name<<" value)\n{\n    switch (static_cast<std::uint64_t>(value)) {\n";
            for(const auto &x:d.items) o<<"    case "<<x.value<<": return true;\n";
            o<<"    default: return false;\n    }\n}\n\n"; known[d.name]=d; continue;
        }
        o<<"struct "<<d.name<<"\n{\n";
        for(const Field &f:d.fields) o<<"    "<<(f.bytes?"std::vector<std::uint8_t>":cppType(f.type))<<" "<<f.name<<"{};\n";
        o<<"\n    static "<<d.name<<" decode_from(const std::vector<std::uint8_t>& data, std::size_t& offset)\n    {\n        "<<d.name<<" value;\n";
        for(const Field &f:d.fields) {
            if(f.bytes) o<<"        value."<<f.name<<" = detail::read_bytes(data, offset, "<<(f.fixed?f.length:"static_cast<std::size_t>(value."+f.length+")")<<");\n";
            else if(integerType(f.type)) o<<"        value."<<f.name<<" = detail::read_integer<"<<cppType(f.type)<<">(data, offset, "<<little<<");\n";
            else if(known[f.type].kind=="enum") {
                const std::string base=cppType(known[f.type].base);
                o<<"        value."<<f.name<<" = static_cast<"<<f.type<<">(detail::read_integer<"<<base<<">(data, offset, "<<little<<"));\n"
                 <<"        if (!is_valid_"<<f.type<<"(value."<<f.name<<")) throw DecodeError(\"invalid enum\");\n";
            } else o<<"        value."<<f.name<<" = "<<f.type<<"::decode_from(data, offset);\n";
        }
        o<<"        return value;\n    }\n\n    static "<<d.name<<" decode(const std::vector<std::uint8_t>& data)\n    {\n        std::size_t offset=0; auto value=decode_from(data,offset);\n        if(offset!=data.size()) throw DecodeError(\"trailing bytes\");\n        return value;\n    }\n\n    void encode_to(std::vector<std::uint8_t>& out) const\n    {\n";
        for(const Field &f:d.fields) {
            if(f.bytes) o<<"        if("<<f.name<<".size() != "<<(f.fixed?f.length:"static_cast<std::size_t>("+f.length+")")<<") throw EncodeError(\"invalid length\");\n        out.insert(out.end(),"<<f.name<<".begin(),"<<f.name<<".end());\n";
            else if(integerType(f.type)) o<<"        detail::write_integer<"<<cppType(f.type)<<">(out,"<<f.name<<","<<little<<");\n";
            else if(known[f.type].kind=="enum") o<<"        if(!is_valid_"<<f.type<<"("<<f.name<<")) throw EncodeError(\"invalid enum\");\n        detail::write_integer<"<<cppType(known[f.type].base)<<">(out,static_cast<"<<cppType(known[f.type].base)<<">("<<f.name<<"),"<<little<<");\n";
            else o<<"        "<<f.name<<".encode_to(out);\n";
        }
        o<<"    }\n\n    std::vector<std::uint8_t> encode() const { std::vector<std::uint8_t> out; encode_to(out); return out; }\n};\n\n";
        known[d.name]=d;
    }
    return o.str();
}

static std::string readFile(const fs::path &p) { std::ifstream f(p,std::ios::binary); if(!f) throw std::runtime_error("cannot read "+p.string()); return {std::istreambuf_iterator<char>(f),{}}; }
static void replaceAll(std::string &s,const std::string &a,const std::string &b) { for(std::size_t p=0;(p=s.find(a,p))!=std::string::npos;p+=b.size()) s.replace(p,a.size(),b); }

int main(int argc,char **argv)
{
    if(argc!=4 || std::string(argv[2])!="--out-dir") { std::cerr<<"usage: protocol-dsl-cpp INPUT --out-dir DIR\n"; return 2; }
    try {
        const std::string text=readFile(argv[1]); Parser::Engine engine; auto tree=engine.parseTree(text);
        if(!tree) { const auto *e=engine.lastError(); std::cerr<<argv[1]<<":"<<(e?std::to_string(e->line()):"0")<<": "<<(e?e->message():"syntax error")<<"\n"; return 1; }
        std::vector<Tok> ts; leaves(*tree,ts); Cursor cursor(std::move(ts)); Schema s=buildSchema(cursor);
        const std::string module=snake(s.name);
        fs::path templatePath="templates/cpp17_header.hpp.tmpl";
        if(!fs::exists(templatePath)) templatePath=fs::path(argv[0]).parent_path().parent_path()/"templates/cpp17_header.hpp.tmpl";
        std::string out=readFile(templatePath);
        replaceAll(out,"${module_name}",module); replaceAll(out,"$module_name",module);
        replaceAll(out,"$protocol_name",s.name); replaceAll(out,"$namespace",module);
        replaceAll(out,"$cpp_declarations",declarations(s));
        fs::create_directories(argv[3]); const fs::path path=fs::path(argv[3])/(module+"_protocol.hpp");
        std::ofstream file(path,std::ios::binary); file<<out; std::cout<<path.string()<<"\n";
    } catch(const std::exception &e) { std::cerr<<argv[1]<<": "<<e.what()<<"\n"; return 1; }
    return 0;
}
