#ifndef STACKVM_CRYPTO_HPP
#define STACKVM_CRYPTO_HPP

/**
 * @file stackvm_crypto.hpp
 * @brief `.svmb` 载荷的可选加密：ChaCha20（RFC 8439），头文件实现、零依赖。
 *
 * ## 放在哪一层
 *
 * 加密只作用在**载荷区**：文件头 24 字节永远是明文（魔数、版本、标志、长度、校验和），
 * 这样格式自描述、加载器可以先校验长度与校验和再解密。载荷区的两种形态：
 *
 * @code
 * flags = 0x0000（默认）：[明文 payload]
 * flags = 0x0001（加密）：[12 字节 nonce][ChaCha20(key, nonce, counter=0) XOR 明文 payload]
 * @endcode
 *
 * 加了 12 字节 nonce，所以加密后的文件恰好比明文大 12 字节；其余每个字节的**长度都不变**
 * （流密码不填充、不分组）。校验和对**存储形态**（nonce + 密文）计算，因此"损坏"能在解密前
 * 就被发现，不会变成解密失败或随机报错。
 *
 * ## 密钥与 nonce 从哪来
 *
 * - 密钥来自编译期宏 `STACKVM_PAYLOAD_KEY`（字符串，缺省见下）。生产端与消费端**必须用同一个
 *   宏值编译**，否则报"载荷解密失败"。
 * - nonce 由**明文载荷**派生：前 8 字节是载荷的 FNV-1a 64，后 4 字节是载荷长度（小端）。
 *   这样不需要随机数发生器，同一份输入永远编出同一份字节（可复现、可对账），而不同内容的
 *   载荷不会共享同一条密钥流。
 *
 * ## 这层加密挡的是什么
 *
 * 挡的是"随手 `strings` / 反汇编就能读懂产物"这类分析：没有密钥时，名字表、类型表、指令流
 * 都不可读。**它不挡有决心的攻击者**：密钥就编译在消费端的可执行文件里，密钥流的构造方式
 * 也是公开的（本文件就是），拿到二进制的人迟早能把它挖出来。别把它当"加密存储"，当成
 * "提高门槛的胶带"。
 *
 * ## 编译期开关
 *
 * - 缺省：加密支持编进去（`--encrypt` 可用，也能读加密产物）。
 * - `-DSTACKVM_NO_PAYLOAD_CRYPTO=1`：整段去掉，产物小一点（嵌入式友好）。此时写加密产物、
 *   读加密产物都会给出明确错误，明文产物一切照旧。
 * - `-DSTACKVM_PAYLOAD_KEY="你的密钥"`：换密钥。
 */

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace stackvm {

/** 编译期是否编进了载荷加密。 */
#if defined(STACKVM_NO_PAYLOAD_CRYPTO) && STACKVM_NO_PAYLOAD_CRYPTO
inline constexpr bool kPayloadCryptoCompiled = false;
#else
inline constexpr bool kPayloadCryptoCompiled = true;
#endif

/** 缺省密钥：**示例值，正式产物请用 `-DSTACKVM_PAYLOAD_KEY=…` 换掉**（两端必须一致）。 */
#ifndef STACKVM_PAYLOAD_KEY
#define STACKVM_PAYLOAD_KEY "ParseEngine-payload-key-2026-10-01-please-change-me"
#endif

/** 头部 `flags` 的位：0x0001 = 载荷区加密。其余位当前未使用，遇到就报错（前向兼容的护栏）。 */
inline constexpr std::uint16_t kProgramFlagEncrypted = 0x0001u;

/** 加密载荷区开头的明文 nonce 长度。 */
inline constexpr std::size_t kPayloadNonceSize = 12u;

/**
 * 加密信封里"明文头"的固定标记。
 *
 * 加密的是 `标记 + 原始 payload`，读的时候先解出这 8 个字节比对：不一致就说明**密钥不对**
 * （或密文被改过），能立刻报"解密失败"，而不是拿一串垃圾去解析、报出莫名其妙的格式错。
 * 它只是"钥匙对不对"的自检，不是认证标签——别指望它挡篡改。
 */
inline constexpr std::uint8_t kPayloadMarker[8] = {'S', 'V', 'M', 'B', 'E', 'N', 'C', '1'};
inline constexpr std::size_t kPayloadMarkerSize = sizeof(kPayloadMarker);

/** 小端读 32 位。 */
inline std::uint32_t cryptoLoad32(const std::uint8_t *bytes)
{
    return static_cast<std::uint32_t>(bytes[0]) |
           (static_cast<std::uint32_t>(bytes[1]) << 8u) |
           (static_cast<std::uint32_t>(bytes[2]) << 16u) |
           (static_cast<std::uint32_t>(bytes[3]) << 24u);
}

/** 小端写 32 位。 */
inline void cryptoStore32(std::uint8_t *bytes, std::uint32_t value)
{
    bytes[0] = static_cast<std::uint8_t>(value & 0xffu);
    bytes[1] = static_cast<std::uint8_t>((value >> 8u) & 0xffu);
    bytes[2] = static_cast<std::uint8_t>((value >> 16u) & 0xffu);
    bytes[3] = static_cast<std::uint8_t>((value >> 24u) & 0xffu);
}

/** 循环左移。 */
inline std::uint32_t cryptoRotl(std::uint32_t value, unsigned count)
{
    return (value << count) | (value >> (32u - count));
}

/** ChaCha20 的四分之一轮。 */
inline void cryptoQuarterRound(std::uint32_t &a, std::uint32_t &b, std::uint32_t &c, std::uint32_t &d)
{
    a += b; d ^= a; d = cryptoRotl(d, 16u);
    c += d; b ^= c; b = cryptoRotl(b, 12u);
    a += b; d ^= a; d = cryptoRotl(d, 8u);
    c += d; b ^= c; b = cryptoRotl(b, 7u);
}

/**
 * @brief 生成一块 64 字节的 ChaCha20 密钥流（RFC 8439 §2.3.2 的 20 轮版本）。
 * @param key 32 字节密钥。
 * @param nonce 12 字节 nonce（IETF 变体）。
 * @param counter 块计数器（`<svmb>` 从 0 开始）。
 * @param out 64 字节输出。
 */
inline void cryptoChaCha20Block(const std::uint8_t key[32], const std::uint8_t nonce[12],
                                std::uint32_t counter, std::uint8_t out[64])
{
    static const std::uint32_t constants[4] = {0x61707865u, 0x3320646eu, 0x79622d32u, 0x6b206574u};
    std::uint32_t initial[16];
    for (unsigned index = 0u; index < 4u; ++index) initial[index] = constants[index];
    for (unsigned index = 0u; index < 8u; ++index) initial[4u + index] = cryptoLoad32(key + 4u * index);
    initial[12] = counter;
    for (unsigned index = 0u; index < 3u; ++index) initial[13u + index] = cryptoLoad32(nonce + 4u * index);

    std::uint32_t working[16];
    for (unsigned index = 0u; index < 16u; ++index) working[index] = initial[index];
    for (unsigned round = 0u; round < 10u; ++round)
    {
        cryptoQuarterRound(working[0], working[4], working[8], working[12]);
        cryptoQuarterRound(working[1], working[5], working[9], working[13]);
        cryptoQuarterRound(working[2], working[6], working[10], working[14]);
        cryptoQuarterRound(working[3], working[7], working[11], working[15]);
        cryptoQuarterRound(working[0], working[5], working[10], working[15]);
        cryptoQuarterRound(working[1], working[6], working[11], working[12]);
        cryptoQuarterRound(working[2], working[7], working[8], working[13]);
        cryptoQuarterRound(working[3], working[4], working[9], working[14]);
    }
    for (unsigned index = 0u; index < 16u; ++index)
        cryptoStore32(out + 4u * index, working[index] + initial[index]);
}

/** 用同一条密钥流异或一段数据（加密与解密是同一个操作）。 */
inline void cryptoChaCha20Xor(const std::uint8_t key[32], const std::uint8_t nonce[12],
                              std::uint32_t counter, std::uint8_t *data, std::size_t size)
{
    std::uint8_t stream[64];
    std::size_t offset = 0u;
    while (offset < size)
    {
        cryptoChaCha20Block(key, nonce, counter++, stream);
        const std::size_t chunk = (size - offset < 64u) ? (size - offset) : 64u;
        for (std::size_t index = 0u; index < chunk; ++index) data[offset + index] ^= stream[index];
        offset += chunk;
    }
}

/** 把 `STACKVM_PAYLOAD_KEY` 的任意长度文本摊成 32 字节密钥（不是 KDF，只是定长化）。 */
inline void cryptoPayloadKey(std::uint8_t key[32])
{
    const char *text = STACKVM_PAYLOAD_KEY;
    std::size_t length = std::strlen(text);
    if (length == 0u) length = 1u;
    std::uint8_t material[32];
    for (unsigned index = 0u; index < 32u; ++index)
        material[index] = static_cast<std::uint8_t>(text[index % length]);
    /* 用 ChaCha20 自身把 material 摊开：不同长度的密钥值不会只差在末尾。 */
    static const std::uint8_t zeroNonce[12] = {};
    std::uint8_t block[64];
    cryptoChaCha20Block(material, zeroNonce, 0u, block);
    std::memcpy(key, block, 32u);
}

/**
 * @brief 由明文载荷派生 12 字节 nonce。
 *
 * 前 8 字节 = 载荷的 FNV-1a 64（与 `.svmb` 校验和同一个思路），后 4 字节 = 载荷长度。
 * 两者都只依赖明文，所以同一份输入编出的密文永远一样；写进文件的是这 12 字节本身（明文），
 * 读的一方直接取用，不需要重算。
 */
inline void cryptoPayloadNonce(const std::uint8_t *payload, std::size_t size, std::uint8_t nonce[12])
{
    std::uint64_t hash = 1469598103934665603ull;
    for (std::size_t index = 0u; index < size; ++index)
    {
        hash ^= payload[index];
        hash *= 1099511628211ull;
    }
    for (unsigned index = 0u; index < 8u; ++index)
        nonce[index] = static_cast<std::uint8_t>((hash >> (8u * index)) & 0xffu);
    const std::uint32_t length = static_cast<std::uint32_t>(size & 0xffffffffu);
    for (unsigned index = 0u; index < 4u; ++index)
        nonce[8u + index] = static_cast<std::uint8_t>((length >> (8u * index)) & 0xffu);
}

/**
 * @brief 自检：跑 RFC 8439 的两组官方向量。
 * @return 全部吻合返回 true。VM 单元测试用它，改动本文件后必须仍然为 true。
 */
inline bool cryptoSelfTest()
{
    /* §2.3.2：counter=1 的密钥流块。 */
    static const std::uint8_t key[32] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
        0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f};
    static const std::uint8_t nonce[12] = {
        0x00, 0x00, 0x00, 0x09, 0x00, 0x00, 0x00, 0x4a, 0x00, 0x00, 0x00, 0x00};
    static const std::uint8_t expected[64] = {
        0x10, 0xf1, 0xe7, 0xe4, 0xd1, 0x3b, 0x59, 0x15, 0x50, 0x0f, 0xdd, 0x1f, 0xa3, 0x20, 0x71, 0xc4,
        0xc7, 0xd1, 0xf4, 0xc7, 0x33, 0xc0, 0x68, 0x03, 0x04, 0x22, 0xaa, 0x9a, 0xc3, 0xd4, 0x6c, 0x4e,
        0xd2, 0x82, 0x64, 0x46, 0x07, 0x9f, 0xaa, 0x09, 0x14, 0xc2, 0xd7, 0x05, 0xd9, 0x8b, 0x02, 0xa2,
        0xb5, 0x12, 0x9c, 0xd1, 0xde, 0x16, 0x4e, 0xb9, 0xcb, 0xd0, 0x83, 0xe8, 0xa2, 0x50, 0x3c, 0x4e};
    std::uint8_t block[64];
    cryptoChaCha20Block(key, nonce, 1u, block);
    if (std::memcmp(block, expected, 64u) != 0) return false;

    /* §2.4.2：加密 "Ladies and Gentlemen of the class of '99…" 的前 114 字节。 */
    static const std::uint8_t message[114] = {
        0x4c, 0x61, 0x64, 0x69, 0x65, 0x73, 0x20, 0x61, 0x6e, 0x64, 0x20, 0x47, 0x65, 0x6e, 0x74, 0x6c,
        0x65, 0x6d, 0x65, 0x6e, 0x20, 0x6f, 0x66, 0x20, 0x74, 0x68, 0x65, 0x20, 0x63, 0x6c, 0x61, 0x73,
        0x73, 0x20, 0x6f, 0x66, 0x20, 0x27, 0x39, 0x39, 0x3a, 0x20, 0x49, 0x66, 0x20, 0x49, 0x20, 0x63,
        0x6f, 0x75, 0x6c, 0x64, 0x20, 0x6f, 0x66, 0x66, 0x65, 0x72, 0x20, 0x79, 0x6f, 0x75, 0x20, 0x6f,
        0x6e, 0x6c, 0x79, 0x20, 0x6f, 0x6e, 0x65, 0x20, 0x74, 0x69, 0x70, 0x20, 0x66, 0x6f, 0x72, 0x20,
        0x74, 0x68, 0x65, 0x20, 0x66, 0x75, 0x74, 0x75, 0x72, 0x65, 0x2c, 0x20, 0x73, 0x75, 0x6e, 0x73,
        0x63, 0x72, 0x65, 0x65, 0x6e, 0x20, 0x77, 0x6f, 0x75, 0x6c, 0x64, 0x20, 0x62, 0x65, 0x20, 0x69,
        0x74, 0x2e};
    static const std::uint8_t expectedCipher[114] = {
        0x6e, 0x2e, 0x35, 0x9a, 0x25, 0x68, 0xf9, 0x80, 0x41, 0xba, 0x07, 0x28, 0xdd, 0x0d, 0x69, 0x81,
        0xe9, 0x7e, 0x7a, 0xec, 0x1d, 0x43, 0x60, 0xc2, 0x0a, 0x27, 0xaf, 0xcc, 0xfd, 0x9f, 0xae, 0x0b,
        0xf9, 0x1b, 0x65, 0xc5, 0x52, 0x47, 0x33, 0xab, 0x8f, 0x59, 0x3d, 0xab, 0xcd, 0x62, 0xb3, 0x57,
        0x16, 0x39, 0xd6, 0x24, 0xe6, 0x51, 0x52, 0xab, 0x8f, 0x53, 0x0c, 0x35, 0x9f, 0x08, 0x61, 0xd8,
        0x07, 0xca, 0x0d, 0xbf, 0x50, 0x0d, 0x6a, 0x61, 0x56, 0xa3, 0x8e, 0x08, 0x8a, 0x22, 0xb6, 0x5e,
        0x52, 0xbc, 0x51, 0x4d, 0x16, 0xcc, 0xf8, 0x06, 0x81, 0x8c, 0xe9, 0x1a, 0xb7, 0x79, 0x37, 0x36,
        0x5a, 0xf9, 0x0b, 0xbf, 0x74, 0xa3, 0x5b, 0xe6, 0xb4, 0x0b, 0x8e, 0xed, 0xf2, 0x78, 0x5e, 0x42,
        0x87, 0x4d};
    static const std::uint8_t counterNonce[12] = {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x4a, 0x00, 0x00, 0x00, 0x00};
    std::uint8_t buffer[114];
    std::memcpy(buffer, message, 114u);
    cryptoChaCha20Xor(key, counterNonce, 1u, buffer, 114u);
    return std::memcmp(buffer, expectedCipher, 114u) == 0;
}

} // namespace stackvm

#endif
