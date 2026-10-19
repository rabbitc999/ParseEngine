/** @file generation_cache.cpp
 * @brief 带版本、SHA-256 校验和原子发布的二进制分析缓存。
 */
#include "ParseEngine/generation_cache.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <type_traits>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif
namespace ParseEngine {
namespace {
constexpr std::size_t maxBytes = 512u * 1024u * 1024u;
/** @brief 固定小端二进制档案；所有长度先校验剩余字节。 */
struct Archive {
    std::string bytes;
    std::size_t position = 0;
    bool reading = false, good = true;
    /** @brief 剩余字节。@return 可读取长度。 */
    std::size_t remaining() const { return bytes.size() - position; }
};
/** @brief 编解码整数。@param a 档案。@param v 整数。 */
template<class T, std::enable_if_t<std::is_integral<T>::value, int> = 0>
void io(Archive &a, T &v) {
    if (!a.good) return;
    if (!a.reading) {
        const auto value = static_cast<std::uint64_t>(v);
        for (int index = 0; index < 8; ++index) a.bytes.push_back(static_cast<char>(value >> (8 * index)));
    } else {
        if (a.remaining() < 8) { a.good = false; return; }
        std::uint64_t value = 0;
        for (int index = 0; index < 8; ++index)
            value |= static_cast<std::uint64_t>(static_cast<unsigned char>(a.bytes[a.position++])) << (8 * index);
        if constexpr (std::is_signed<T>::value) {
            std::int64_t signedValue;
            std::memcpy(&signedValue, &value, sizeof(value));
            if (signedValue < std::numeric_limits<T>::min() || signedValue > std::numeric_limits<T>::max()) a.good = false;
            else v = static_cast<T>(signedValue);
        } else {
            if (value > static_cast<std::uint64_t>(std::numeric_limits<T>::max())) a.good = false;
            else v = static_cast<T>(value);
        }
    }
}
/** @brief 编解码字符串。@param a 档案。@param v 文本。 */
void io(Archive &a, std::string &v) {
    std::size_t count = v.size(); io(a, count);
    if (!a.good) return;
    if (!a.reading) a.bytes += v;
    else if (count > a.remaining()) a.good = false;
    else { v.assign(a.bytes, a.position, count); a.position += count; }
}
// 先声明全部复合类型，保证模板两阶段名称查找能找到嵌套编解码器。
template<class T> void io(Archive &, std::vector<T> &);
template<class T> void io(Archive &, std::set<T> &);
template<class K, class V> void io(Archive &, std::map<K,V> &);
template<class K, class V> void io(Archive &, std::pair<K,V> &);
template<class T> void io(Archive &, std::optional<T> &);
void io(Archive &, Production &); void io(Archive &, Grammar &); void io(Archive &, PriorityConfig &);
void io(Archive &, LrItem &); void io(Archive &, SlrState &); void io(Archive &, LalrState &);
void io(Archive &, LrAction &); void io(Archive &, LrConflict &); void io(Archive &, LrResolution &);
void io(Archive &, IelrSplit &); void io(Archive &, AnalysisResult &);
/** @brief 顺序编解码字段。@param a 档案。@param fields 字段引用。 */
template<class... T> void fields(Archive &a, T &...values) { (io(a, values), ...); }
/** @brief 校验容器元素数量。@param a 档案。@param n 数量。@return 长度可信时为真。 */
bool count(Archive &a, std::size_t &n) {
    io(a,n);
    if (a.reading && n > a.remaining() / 8) a.good = false;
    return a.good;
}
/** @brief 编解码序列。@param a 档案。@param v 序列。 */
template<class T> void io(Archive &a, std::vector<T> &v) {
    std::size_t n = v.size(); if (!count(a,n)) return;
    if (!a.reading) { for (auto &item:v) io(a,item); }
    else { v.clear(); for (std::size_t i=0;i<n && a.good;++i) { T item{}; io(a,item); if(a.good) v.push_back(std::move(item)); } }
}
/** @brief 编解码有序集合并拒绝重复元素。@param a 档案。@param v 集合。 */
template<class T> void io(Archive &a, std::set<T> &v) {
    std::size_t n=v.size(); if(!count(a,n)) return;
    if(!a.reading) { for(const auto &item:v) { T copy=item; io(a,copy); } }
    else { v.clear(); for(std::size_t i=0;i<n && a.good;++i) { T item{}; io(a,item); if(a.good && !v.insert(std::move(item)).second) a.good=false; } }
}
/** @brief 编解码映射并拒绝重复键。@param a 档案。@param v 映射。 */
template<class K,class V> void io(Archive &a, std::map<K,V> &v) {
    std::size_t n=v.size(); if(!count(a,n)) return;
    if(!a.reading) { for(auto &item:v) { K key=item.first; fields(a,key,item.second); } }
    else { v.clear(); for(std::size_t i=0;i<n && a.good;++i) { K key{}; V value{}; fields(a,key,value); if(a.good && !v.emplace(std::move(key),std::move(value)).second) a.good=false; } }
}
/** @brief 编解码键对。@param a 档案。@param v 键对。 */
template<class K,class V> void io(Archive &a,std::pair<K,V> &v) { fields(a,v.first,v.second); }
/** @brief 编解码可选值。@param a 档案。@param v 可选值。 */
template<class T> void io(Archive &a,std::optional<T> &v) {
    bool present=v.has_value(); io(a,present);
    if(a.reading) { if(present) v=T{}; else v.reset(); }
    if(present && a.good) io(a,*v);
}
/** @brief 编解码产生式。@param a 档案。@param v 产生式。 */
void io(Archive&a,Production&v) { fields(a,v.rhs,v.action,v.transform,v.baseRhsLength,v.owner,v.id,v.lhs,v.sourceOffset,v.actionLine); }
/** @brief 编解码文法。@param a 档案。@param v 文法。 */
void io(Archive&a,Grammar&v) { fields(a,v.start,v.rules); }
/** @brief 编解码优先级。@param a 档案。@param v 配置。 */
void io(Archive&a,PriorityConfig&v) { fields(a,v.precedence,v.associativity,v.productionPrec,v.expectConflicts); }
/** @brief 编解码 LR 项。@param a 档案。@param v 项。 */
void io(Archive&a,LrItem&v) { fields(a,v.lhs,v.prod,v.dot); }
/** @brief 编解码 LR(0) 状态。@param a 档案。@param v 状态。 */
void io(Archive&a,SlrState&v) { io(a,v.items); }
/** @brief 编解码带展望符状态。@param a 档案。@param v 状态。 */
void io(Archive&a,LalrState&v) { io(a,v.items); }
/** @brief 编解码并校验动作种类。@param a 档案。@param v 动作。 */
void io(Archive&a,LrAction&v) { int kind=static_cast<int>(v.kind); fields(a,kind,v.value); if(kind<0||kind>2) a.good=false; else v.kind=static_cast<LrAction::Kind>(kind); }
/** @brief 编解码冲突。@param a 档案。@param v 冲突。 */
void io(Archive&a,LrConflict&v) { fields(a,v.kind,v.state,v.token,v.text); }
/** @brief 编解码冲突消解。@param a 档案。@param v 记录。 */
void io(Archive&a,LrResolution&v) { fields(a,v.state,v.token,v.kind,v.reason,v.chosen,v.discarded); }
/** @brief 编解码 IELR 拆分。@param a 档案。@param v 记录。 */
void io(Archive&a,IelrSplit&v) { fields(a,v.lalrState,v.token,v.kind,v.reason,v.parts); }
/** @brief 编解码完整分析对象；新增字段时必须提升缓存版本。@param a 档案。@param v 结果。 */
void io(Archive&a,AnalysisResult&v) {
    fields(a,v.mode,v.grammar,v.terminals,v.nonterms,v.first,v.follow,v.hasFollow,v.lr1StateCount,
        v.productions,v.slrStates,v.lalrStates,v.transitions,v.action,v.lookaheadK,v.lookaheadColumns,
        v.lookaheadColumnIndex,v.firstK,v.followK,v.gotoTable,v.table,v.unresolved,v.resolutions,
        v.warnings,v.priority,v.ielrLalrStateCount,v.ielrSplits,v.glrAlternatives,v.ddLeftRecursionCycle,
        v.lr1MergedStateCount,v.teach);
}
/** @brief 读取有界二进制文件。@param path 文件。@param bytes 输出。@return 完整读取时为真。 */
bool read(const std::filesystem::path &path,std::string &bytes) {
    std::error_code ec; const auto size=std::filesystem::file_size(path,ec);
    if(ec || size>maxBytes) return false;
    std::ifstream in(path,std::ios::binary); if(!in) return false;
    bytes.resize(static_cast<std::size_t>(size));
    in.read(bytes.data(),static_cast<std::streamsize>(size)); return static_cast<bool>(in);
}
/** @brief 确认非 LL 模式的文法形状并刷新动作、行号与偏移。
 * @param grammar 当前输入。@param result 缓存结果。@return 可安全刷新时为真。
 */
bool refresh(const Grammar &grammar,AnalysisResult &result) {
    if(result.mode=="ll1"||result.mode=="llk"||result.mode=="ll0") return true; // LL 键包含完整动作和位置。
    for(const auto &rule:grammar.rules) {
        auto found=result.grammar.rules.find(rule.first);
        if(found==result.grammar.rules.end()||found->second.size()!=rule.second.size()) return false;
        for(std::size_t i=0;i<rule.second.size();++i) {
            auto &old=found->second[i]; const auto &current=rule.second[i];
            if(old.rhs!=current.rhs||old.transform!=current.transform||old.owner!=current.owner||old.baseRhsLength!=current.baseRhsLength) return false;
            old.action=current.action; old.sourceOffset=current.sourceOffset; old.actionLine=current.actionLine;
        }
    }
    std::map<std::string,std::size_t> indices;
    for(auto &production:result.productions) {
        const auto rule=result.grammar.rules.find(production.lhs);
        if(rule==result.grammar.rules.end()) return false;
        auto &index=indices[production.lhs]; if(index>=rule->second.size()) return false;
        const auto &current=rule->second[index++];
        if(production.rhs!=current.rhs||production.transform!=current.transform) return false;
        production.action=current.action; production.sourceOffset=current.sourceOffset; production.actionLine=current.actionLine;
    }
    return true;
}
}

/** @brief 计算标准 SHA-256。@param bytes 原始字节。@return 64 个十六进制字符。 */
std::string GenerationCache::digest(const std::string &bytes) {
    static constexpr std::uint32_t constants[]={
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
    std::array<std::uint32_t,8> hash={0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    const auto rotate=[](std::uint32_t v,int n){return (v>>n)|(v<<(32-n));};
    const std::size_t blocks=(bytes.size()+9+63)/64;
    const auto bitLength=static_cast<std::uint64_t>(bytes.size())*8;
    for(std::size_t block=0;block<blocks;++block) {
        std::uint32_t words[64]={};
        for(std::size_t j=0;j<64;++j) {
            const auto offset=block*64+j; unsigned char byte=0;
            if(offset<bytes.size()) byte=static_cast<unsigned char>(bytes[offset]);
            else if(offset==bytes.size()) byte=0x80;
            else if(offset>=blocks*64-8) byte=static_cast<unsigned char>(bitLength>>(8*(blocks*64-1-offset)));
            words[j/4]|=static_cast<std::uint32_t>(byte)<<(24-8*(j%4));
        }
        for(int j=16;j<64;++j) {
            const auto a=words[j-15],b=words[j-2];
            words[j]=words[j-16]+(rotate(a,7)^rotate(a,18)^(a>>3))+words[j-7]+(rotate(b,17)^rotate(b,19)^(b>>10));
        }
        auto a=hash[0],b=hash[1],c=hash[2],d=hash[3],e=hash[4],f=hash[5],g=hash[6],h=hash[7];
        for(int j=0;j<64;++j) {
            const auto t1=h+(rotate(e,6)^rotate(e,11)^rotate(e,25))+((e&f)^(~e&g))+constants[j]+words[j];
            const auto t2=(rotate(a,2)^rotate(a,13)^rotate(a,22))+((a&b)^(a&c)^(b&c));
            h=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;
        }
        hash[0]+=a;hash[1]+=b;hash[2]+=c;hash[3]+=d;hash[4]+=e;hash[5]+=f;hash[6]+=g;hash[7]+=h;
    }
    std::string out; const char *hex="0123456789abcdef";
    for(const auto word:hash) for(int shift=28;shift>=0;shift-=4) out+=hex[(word>>shift)&15];
    return out;
}
/** @brief 标识正在运行的生成器。@param executable 启动路径。@return 文件摘要或空。 */
std::string GenerationCache::buildId(const std::string &executable) {
    std::filesystem::path path=std::filesystem::u8path(executable);
#if defined(_WIN32)
    std::wstring buffer(32768,L'\0'); const auto length=GetModuleFileNameW(nullptr,buffer.data(),static_cast<DWORD>(buffer.size()));
    if(length>0 && length<buffer.size()) { buffer.resize(length); path=buffer; }
#else
    std::error_code ec; const auto actual=std::filesystem::read_symlink("/proc/self/exe",ec); if(!ec) path=actual;
#endif
    std::string bytes; return read(path,bytes)?digest(bytes):std::string();
}
/** @brief 计算建表依赖键。@param grammar 文法。@param tokens 终结符。@param priority 优先级。
 * @param mode 模式。@param k 展望长度。@param teach 教学开关。@param reportProfile 报告分类。
 * @param build 生成器摘要。@return 二进制输入的 SHA-256。
 */
std::string GenerationCache::key(const Grammar &grammar,const std::vector<std::string>&tokens,
    const PriorityConfig &priority,const std::string &mode,int k,bool teach,const std::string &reportProfile,const std::string &build) {
    Grammar normalized=grammar;
    const bool ll=mode=="ll1"||mode=="llk"||mode=="ll0";
    if(!ll) for(auto &rule:normalized.rules) for(auto &production:rule.second) {
        production.action.clear();production.sourceOffset=0;production.actionLine=0;
    }
    Archive a; auto tokenCopy=tokens; auto prec=priority; auto modeCopy=mode; auto profile=reportProfile; auto buildCopy=build;
    fields(a,normalized,tokenCopy,prec,modeCopy,k,teach,profile,buildCopy);
    return digest(a.bytes);
}
/** @brief 读取并校验二进制缓存。@param directory 目录。@param key 键。@param grammar 当前文法。
 * @param analysis 恢复结果。@param report 分析报告。@param reason 未命中原因。@return 命中时为真。
 */
bool GenerationCache::load(const std::string &directory,const std::string &key,const Grammar &grammar,
    AnalysisResult &analysis,std::string &report,std::string &reason) {
    const auto path=std::filesystem::u8path(directory)/("analysis-"+key+".bin");
    Archive a; a.reading=true;
    if(!read(path,a.bytes)) {reason="不存在或无法读取";return false;}
    if(a.bytes.size()<8||a.bytes.compare(0,8,std::string("PECACHE\0",8))!=0) {reason="文件头无效";return false;}
    a.position=8; std::uint64_t version=0; std::string storedKey,checksum,payload;
    fields(a,version,storedKey,checksum,payload);
    if(!a.good||a.remaining()!=0||version!=1||storedKey!=key||digest(payload)!=checksum) {reason="版本、键或校验和不匹配";return false;}
    Archive body;body.reading=true;body.bytes=std::move(payload);
    AnalysisResult restored; std::string restoredReport; fields(body,restored,restoredReport);
    if(!body.good||body.remaining()!=0||!refresh(grammar,restored)) {reason="内容不完整或文法结构不匹配";return false;}
    analysis=std::move(restored);report=std::move(restoredReport);reason.clear();return true;
}
/** @brief 原子发布缓存。@param directory 目录。@param key 键。@param analysis 分析结果。
 * @param report 报告。@param reason 失败原因。@return 发布成功时为真。
 */
bool GenerationCache::save(const std::string &directory,const std::string &key,const AnalysisResult &analysis,
    const std::string &report,std::string &reason) {
    Archive body; auto copy=analysis;auto reportCopy=report;fields(body,copy,reportCopy);
    Archive a;a.bytes.assign("PECACHE\0",8);std::uint64_t version=1;auto keyCopy=key;auto checksum=digest(body.bytes);
    fields(a,version,keyCopy,checksum,body.bytes);
    if(a.bytes.size()>maxBytes) {reason="缓存超过 512 MiB";return false;}
    const auto root=std::filesystem::u8path(directory);std::error_code ec;
    std::filesystem::create_directories(root,ec);if(ec) {reason=ec.message();return false;}
    const auto path=root/("analysis-"+key+".bin");
    static std::atomic<unsigned> serial{0};
#if defined(_WIN32)
    const auto pid=GetCurrentProcessId();
#else
    const auto pid=getpid();
#endif
    const auto temp=root/(".analysis-"+key+"-"+std::to_string(pid)+"-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+"-"+std::to_string(serial++)+".tmp");
    { std::ofstream out(temp,std::ios::binary|std::ios::trunc);if(out) {out.write(a.bytes.data(),static_cast<std::streamsize>(a.bytes.size()));out.close();}
      if(!out) {std::filesystem::remove(temp,ec);reason="无法完整写入缓存";return false;} }
#if defined(_WIN32)
    const bool published=MoveFileExW(temp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=0;
#else
    std::filesystem::rename(temp,path,ec);const bool published=!ec;
#endif
    if(!published) {std::filesystem::remove(temp,ec);reason="无法发布缓存文件";return false;}
    return true;
}
}
