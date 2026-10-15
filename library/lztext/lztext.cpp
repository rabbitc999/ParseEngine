/** @file lztext.cpp
 *  @brief LZ77 + 规范哈夫曼的无依赖压缩库实现。
 *
 *  见 lztext.hpp 的格式说明与选型依据。整个文件只用 C++17 标准库。
 */

#include "lztext.hpp"

#include <algorithm>
#include <cstring>
#include <queue>
#include <utility>

namespace lztext
{
namespace
{

/* ---- 常量 ---- */
constexpr int kMaxCodeBits = 15;      ///< 哈夫曼码长上限（与 deflate 同，便于规范解码）
constexpr int kMainAlphabet = 273;    ///< 256 字面量 + 1 结束符 + 16 长度桶
constexpr int kDistAlphabet = 25;     ///< 距离桶 0..24，最远 2^25 - 1
constexpr int kEosSymbol = 256;       ///< 结束符
constexpr int kLenSymbolBase = 257;   ///< 长度桶 0 在主字母表中的符号值
constexpr unsigned kMinMatch = 3;     ///< 最短匹配
constexpr unsigned kMaxMatch = 65537u;        ///< 长度桶 15 的上界
constexpr unsigned kMaxDistance = (1u << 24) - 1u;
constexpr int kHashBits = 18;
constexpr std::size_t kHashSize = std::size_t(1) << kHashBits;
constexpr std::size_t kMaxOutput = std::size_t(64) * 1024 * 1024; ///< 解压上限，防损坏数据把内存撑爆

/** 长度桶 b 的起始长度：桶 b 覆盖 [2^b + 2, 2^(b+1) + 1]。 */
inline unsigned lengthBase(int bucket) { return (1u << bucket) + 2u; }

/** 由匹配长度求长度桶：b = floor(log2(len - 2))。 */
inline int lengthBucket(unsigned len)
{
    unsigned v = len - 2u;
    int b = 0;
    while (v > 1u) { v >>= 1; ++b; }
    return b;
}

/** 由距离求距离桶：b = floor(log2(dist))，桶 b 覆盖 [2^b, 2^(b+1) - 1]。 */
inline int distanceBucket(unsigned dist)
{
    unsigned v = dist;
    int b = 0;
    while (v > 1u) { v >>= 1; ++b; }
    return b;
}

/* ---- 位流（高位在前）---- */
class BitWriter
{
public:
    void putBit(int bit)
    {
        acc_ = (acc_ << 1) | (unsigned)(bit & 1);
        if (++nbits_ == 8)
        {
            out_.push_back((unsigned char)acc_);
            acc_ = 0;
            nbits_ = 0;
        }
    }
    /** @brief 写 n 个比特，先写 value 的高位。 */
    void putBits(unsigned value, int n)
    {
        for (int i = n - 1; i >= 0; --i) putBit((int)((value >> i) & 1u));
    }
    void flush()
    {
        if (nbits_ != 0)
        {
            out_.push_back((unsigned char)(acc_ << (8 - nbits_)));
            acc_ = 0;
            nbits_ = 0;
        }
    }
    std::vector<unsigned char> &buffer() { return out_; }

private:
    std::vector<unsigned char> out_;
    unsigned acc_ = 0;
    int nbits_ = 0;
};

class BitReader
{
public:
    BitReader(const unsigned char *data, std::size_t size) : data_(data), size_(size) {}
    /** @return 读到 0/1；数据用完返回 -1。 */
    int getBit()
    {
        if (nbits_ == 0)
        {
            if (pos_ >= size_) return -1;
            acc_ = data_[pos_++];
            nbits_ = 8;
        }
        int bit = (int)((acc_ >> 7) & 1u);
        acc_ <<= 1;
        --nbits_;
        return bit;
    }
    /** @brief 读 n 个比特，先读到的是高位。 */
    unsigned getBits(int n)
    {
        unsigned value = 0;
        for (int i = 0; i < n; ++i)
        {
            int bit = getBit();
            if (bit < 0) return 0;
            value = (value << 1) | (unsigned)bit;
        }
        return value;
    }

private:
    const unsigned char *data_;
    std::size_t size_;
    std::size_t pos_ = 0;
    unsigned acc_ = 0;
    int nbits_ = 0;
};

/* ---- 哈夫曼 ---- */
/**
 * @brief 由频率构造码长，码长限制在 kMaxCodeBits 以内。
 * @param freq 每个符号的频率（长度 n）。
 * @param length 输出：每个符号的码长，0 表示不出现。
 */
void buildCodeLengths(const std::vector<unsigned> &freq, std::vector<unsigned char> &length)
{
    const int n = (int)freq.size();
    length.assign((std::size_t)n, (unsigned char)0);

    std::vector<int> used;
    for (int i = 0; i < n; ++i)
        if (freq[i] != 0u) used.push_back(i);
    if (used.empty()) return;
    if (used.size() == 1)
    {
        // 只有一个符号时规范哈夫曼退化成 0 位码，解码端无法定位；强制给 1 位。
        length[(std::size_t)used[0]] = 1;
        return;
    }

    std::vector<unsigned> f(freq);
    for (int attempt = 0; attempt < 8; ++attempt)
    {
        // 节点编号：>= 0 是内部节点（下标进 children），< 0 是叶子（-id - 1 为符号）。
        struct Item
        {
            unsigned long long weight;
            int seq;
            int id;
        };
        struct Cmp
        {
            bool operator()(const Item &a, const Item &b) const
            {
                if (a.weight != b.weight) return a.weight > b.weight;
                return a.seq > b.seq;
            }
        };
        std::priority_queue<Item, std::vector<Item>, Cmp> pq;
        std::vector<std::pair<int, int>> children;
        int seq = 0;
        for (int sym : used) pq.push(Item{f[(std::size_t)sym], seq++, -sym - 1});
        while (pq.size() > 1u)
        {
            Item a = pq.top();
            pq.pop();
            Item b = pq.top();
            pq.pop();
            const int id = (int)children.size();
            children.push_back(std::make_pair(a.id, b.id));
            pq.push(Item{a.weight + b.weight, seq++, id});
        }
        const int root = pq.top().id;

        std::vector<unsigned char> len((std::size_t)n, (unsigned char)0);
        std::vector<std::pair<int, int>> stack;
        stack.push_back(std::make_pair(root, 0));
        while (!stack.empty())
        {
            std::pair<int, int> cur = stack.back();
            stack.pop_back();
            if (cur.first < 0)
            {
                len[(std::size_t)(-cur.first - 1)] = (unsigned char)cur.second;
                continue;
            }
            stack.push_back(std::make_pair(children[(std::size_t)cur.first].first, cur.second + 1));
            stack.push_back(std::make_pair(children[(std::size_t)cur.first].second, cur.second + 1));
        }
        int maxLen = 0;
        for (int i = 0; i < n; ++i) maxLen = std::max(maxLen, (int)len[(std::size_t)i]);
        if (maxLen <= kMaxCodeBits)
        {
            length.swap(len);
            return;
        }
        // 码长超标：把频率差异压平再试一次（会略微损失压缩率）。
        for (std::size_t i = 0; i < f.size(); ++i)
            if (f[i] != 0u) f[i] = (f[i] + 1u) >> 1;
    }

    // 兜底：给全部符号等长码，保证一定能编出来（此时压缩率会明显变差）。
    int bits = 1;
    while ((1 << bits) < n) ++bits;
    if (bits > kMaxCodeBits) bits = kMaxCodeBits;
    for (int i = 0; i < n; ++i) length[(std::size_t)i] = (unsigned char)bits;
}

/** @brief 由码长算规范哈夫曼码（RFC1951 的赋值顺序：码长升序、符号升序）。 */
void buildCanonicalCodes(const std::vector<unsigned char> &length, std::vector<unsigned> &code)
{
    const int n = (int)length.size();
    unsigned blCount[kMaxCodeBits + 1] = {0};
    for (int i = 0; i < n; ++i)
        if (length[(std::size_t)i] != 0u) blCount[length[(std::size_t)i]]++;

    unsigned next[kMaxCodeBits + 1] = {0};
    unsigned c = 0;
    for (int bits = 1; bits <= kMaxCodeBits; ++bits)
    {
        c = (c + blCount[bits - 1]) << 1;
        next[bits] = c;
    }
    code.assign((std::size_t)n, 0u);
    for (int i = 0; i < n; ++i)
    {
        const unsigned char l = length[(std::size_t)i];
        if (l != 0u) code[(std::size_t)i] = next[l]++;
    }
}

/** @brief 解码侧的哈夫曼表：按（码长、符号）排序后的符号表 + 每档个数。 */
class HuffmanDecoder
{
public:
    explicit HuffmanDecoder(int alphabetSize) : size_(alphabetSize) {}

    /** @brief 从位流里读码长表（每符号 4 bit）并建表。 */
    bool read(BitReader &br)
    {
        std::vector<unsigned char> length((std::size_t)size_, (unsigned char)0);
        for (int i = 0; i < size_; ++i) length[(std::size_t)i] = (unsigned char)br.getBits(4);
        return build(length);
    }

    bool build(const std::vector<unsigned char> &length)
    {
        std::fill(count_, count_ + kMaxCodeBits + 1, (unsigned short)0);
        int total = 0;
        for (int len = 1; len <= kMaxCodeBits; ++len)
        {
            for (int sym = 0; sym < size_; ++sym)
            {
                if (length[(std::size_t)sym] == (unsigned char)len)
                {
                    if (total >= size_) return false;
                    symbols_[(std::size_t)total++] = sym;
                }
            }
            count_[len] = (unsigned short)total;
        }
        return total > 0;
    }

    /** @return 解出的符号；位流损坏返回 -1。 */
    int decode(BitReader &br) const
    {
        int code = 0;
        int first = 0;   // 本档第一个码
        int index = 0;   // 本档第一个符号在 symbols_ 里的下标
        for (int len = 1; len <= kMaxCodeBits; ++len)
        {
            const int bit = br.getBit();
            if (bit < 0) return -1;
            code |= bit;
            const int c = count_[len] - index;
            if (code - first < c) return symbols_[(std::size_t)(index + (code - first))];
            index += c;
            first = (first + c) << 1;
            code <<= 1;
        }
        return -1;
    }

private:
    int size_;
    unsigned short count_[kMaxCodeBits + 1];
    int symbols_[kMainAlphabet];
};

/* ---- LZ77 匹配器 ---- */
struct Matcher
{
    const unsigned char *data = nullptr;
    std::size_t size = 0;
    std::vector<int> head;   ///< 哈希 → 最近一次出现的位置
    std::vector<int> prev;   ///< 位置 → 同哈希的上一次出现位置（链）
    int depth = 256;         ///< 链的最大回溯长度
    unsigned niceLength = 4096; ///< 匹配到这个长度就提前收工

    void init(const unsigned char *d, std::size_t n, int level)
    {
        data = d;
        size = n;
        head.assign(kHashSize, -1);
        prev.assign(n, -1);
        switch (level)
        {
        case 1: depth = 16; niceLength = 64; break;
        case 2: depth = 32; niceLength = 128; break;
        case 3: depth = 64; niceLength = 256; break;
        case 4: depth = 96; niceLength = 512; break;
        case 5: depth = 128; niceLength = 1024; break;
        case 6: depth = 192; niceLength = 2048; break;
        case 7: depth = 256; niceLength = 4096; break;
        case 8: depth = 384; niceLength = 8192; break;
        default: depth = 512; niceLength = 16384; break;
        }
    }

    unsigned hashAt(std::size_t p) const
    {
        const unsigned h = (unsigned)data[p] | ((unsigned)data[p + 1] << 8) | ((unsigned)data[p + 2] << 16);
        return (h * 2654435761u) >> (32 - kHashBits);
    }

    void insert(std::size_t p)
    {
        if (p + 3 > size) return;
        const unsigned h = hashAt(p);
        prev[p] = head[h];
        head[h] = (int)p;
    }

    /** @brief 在 [0, pos) 里找最长匹配。 */
    void find(std::size_t pos, unsigned &bestLen, unsigned &bestDist) const
    {
        bestLen = 0;
        bestDist = 0;
        if (pos + kMinMatch > size) return;
        unsigned maxPossible = (unsigned)(size - pos);
        if (maxPossible > kMaxMatch) maxPossible = kMaxMatch;

        int cand = head[hashAt(pos)];
        int limit = depth;
        while (cand >= 0 && limit-- > 0)
        {
            const unsigned dist = (unsigned)(pos - (std::size_t)cand);
            if (dist > kMaxDistance) break;   // 链按由近到远排列，后面只会更远
            if (dist > 0u && bestLen < maxPossible &&
                data[(std::size_t)cand + bestLen] == data[pos + bestLen])
            {
                unsigned l = 0;
                while (l < maxPossible && data[(std::size_t)cand + l] == data[pos + l]) ++l;
                if (l > bestLen)
                {
                    bestLen = l;
                    bestDist = dist;
                    if (l >= niceLength) break;
                }
            }
            cand = prev[(std::size_t)cand];
        }
    }
};

struct Token
{
    unsigned sym;        ///< 主字母表符号（字面量或 257 + 长度桶）
    unsigned extraLen;   ///< 长度附加位的值
    unsigned extraDist;  ///< 距离附加位的值
    unsigned char lenBits;
    unsigned char distBucket;
    bool isMatch;
};

void writeHeader(std::vector<unsigned char> &out, unsigned char method, std::uint64_t rawSize)
{
    out.push_back((unsigned char)'M');
    out.push_back((unsigned char)'L');
    out.push_back((unsigned char)'Z');
    out.push_back((unsigned char)'1');
    out.push_back(method);
    std::uint64_t v = rawSize;
    do
    {
        unsigned char b = (unsigned char)(v & 0x7fu);
        v >>= 7;
        if (v != 0u) b = (unsigned char)(b | 0x80u);
        out.push_back(b);
    } while (v != 0u);
}

} // namespace

std::vector<unsigned char> compress(const unsigned char *data, std::size_t size, int level)
{
    std::vector<unsigned char> stored;
    writeHeader(stored, 0, (std::uint64_t)size);
    stored.insert(stored.end(), data, data + size);

    if (size == 0u || data == nullptr) return stored;
    if (level < 1 || level > 9) level = 9;

    /* ---- 第一遍：切分出 token 并统计频率 ---- */
    Matcher matcher;
    matcher.init(data, size, level);

    std::vector<Token> tokens;
    std::vector<unsigned> freqMain((std::size_t)kMainAlphabet, 0u);
    std::vector<unsigned> freqDist((std::size_t)kDistAlphabet, 0u);

    std::size_t pos = 0;
    while (pos < size)
    {
        unsigned len = 0, dist = 0;
        matcher.find(pos, len, dist);
        if (len >= kMinMatch && pos + 1 < size)
        {
            // 惰性匹配：下一个位置能匹配到更长，就把当前字节当字面量发出去。
            unsigned len2 = 0, dist2 = 0;
            matcher.find(pos + 1, len2, dist2);
            if (len2 > len) len = 0;
        }
        if (len >= kMinMatch)
        {
            const int lb = lengthBucket(len);
            const int db = distanceBucket(dist);
            Token t;
            t.sym = (unsigned)(kLenSymbolBase + lb);
            t.extraLen = len - lengthBase(lb);
            t.lenBits = (unsigned char)lb;
            t.distBucket = (unsigned char)db;
            t.extraDist = dist - (1u << db);
            t.isMatch = true;
            tokens.push_back(t);
            freqMain[t.sym]++;
            freqDist[(std::size_t)db]++;
            for (unsigned k = 0; k < len; ++k) matcher.insert(pos + k);
            pos += len;
        }
        else
        {
            Token t;
            t.sym = (unsigned)data[pos];
            t.extraLen = 0;
            t.extraDist = 0;
            t.lenBits = 0;
            t.distBucket = 0;
            t.isMatch = false;
            tokens.push_back(t);
            freqMain[t.sym]++;
            matcher.insert(pos);
            ++pos;
        }
    }
    freqMain[(std::size_t)kEosSymbol]++;

    /* ---- 第二遍：建哈夫曼表并输出 ---- */
    std::vector<unsigned char> lenMain, lenDist;
    buildCodeLengths(freqMain, lenMain);
    buildCodeLengths(freqDist, lenDist);
    std::vector<unsigned> codeMain, codeDist;
    buildCanonicalCodes(lenMain, codeMain);
    buildCanonicalCodes(lenDist, codeDist);

    BitWriter bw;
    for (int i = 0; i < kMainAlphabet; ++i) bw.putBits(lenMain[(std::size_t)i], 4);
    for (int i = 0; i < kDistAlphabet; ++i) bw.putBits(lenDist[(std::size_t)i], 4);
    for (const Token &t : tokens)
    {
        bw.putBits(codeMain[t.sym], (int)lenMain[t.sym]);
        if (t.isMatch)
        {
            if (t.lenBits != 0u) bw.putBits(t.extraLen, (int)t.lenBits);
            bw.putBits(codeDist[(std::size_t)t.distBucket], (int)lenDist[(std::size_t)t.distBucket]);
            if (t.distBucket != 0u) bw.putBits(t.extraDist, (int)t.distBucket);
        }
    }
    bw.putBits(codeMain[(std::size_t)kEosSymbol], (int)lenMain[(std::size_t)kEosSymbol]);
    bw.flush();

    std::vector<unsigned char> packed;
    writeHeader(packed, 1, (std::uint64_t)size);
    const std::vector<unsigned char> &body = bw.buffer();
    packed.insert(packed.end(), body.begin(), body.end());
    return (packed.size() < stored.size()) ? packed : stored;
}

std::vector<unsigned char> compress(const std::string &text, int level)
{
    return compress(reinterpret_cast<const unsigned char *>(text.data()), text.size(), level);
}

bool decompress(const unsigned char *src, std::size_t size, std::string &out)
{
    out.clear();
    if (src == nullptr || size < 5u) return false;
    if (std::memcmp(src, "MLZ1", 4) != 0) return false;

    std::size_t p = 4;
    const unsigned char method = src[p++];
    std::uint64_t rawSize = 0;
    int shift = 0;
    while (p < size)
    {
        const unsigned char b = src[p++];
        rawSize |= (std::uint64_t)(b & 0x7fu) << shift;
        shift += 7;
        if ((b & 0x80u) == 0u) break;
        if (shift > 63) return false;
    }
    if (rawSize > (std::uint64_t)kMaxOutput) return false;

    if (method == 0u)
    {
        if (p + (std::size_t)rawSize > size) return false;
        out.assign(reinterpret_cast<const char *>(src + p), (std::size_t)rawSize);
        return true;
    }
    if (method != 1u) return false;

    BitReader br(src + p, size - p);
    HuffmanDecoder decMain(kMainAlphabet);
    HuffmanDecoder decDist(kDistAlphabet);
    if (!decMain.read(br) || !decDist.read(br)) return false;

    out.reserve((std::size_t)rawSize);
    for (;;)
    {
        const int sym = decMain.decode(br);
        if (sym < 0) return false;
        if (sym == kEosSymbol) break;
        if (sym < kEosSymbol)
        {
            out.push_back((char)(unsigned char)sym);
            continue;
        }
        const int lb = sym - kLenSymbolBase;
        if (lb < 0 || lb > 15) return false;
        const unsigned length = lengthBase(lb) + br.getBits(lb);
        const int db = decDist.decode(br);
        if (db < 0 || db >= kDistAlphabet) return false;
        const unsigned distance = (1u << db) + br.getBits(db);
        const std::size_t start = out.size();
        if (distance == 0u || distance > start) return false;
        const std::size_t from = start - distance;
        for (unsigned i = 0; i < length; ++i) out.push_back(out[from + i]);
        if (out.size() > (std::size_t)rawSize) return false;
    }
    return out.size() == (std::size_t)rawSize;
}

std::string decompressString(const unsigned char *src, std::size_t size, bool *ok)
{
    std::string out;
    const bool good = decompress(src, size, out);
    if (ok != nullptr) *ok = good;
    return good ? out : std::string();
}

} // namespace lztext
