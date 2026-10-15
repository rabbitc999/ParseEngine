/** @file lztext.hpp
 *  @brief LZ77 + 规范哈夫曼的无依赖压缩库（C++17）。
 *
 *  用途：把程序内嵌的大段文本（代码模板、内嵌标准库等）以压缩形式存进二进制，
 *  运行时再解压，用来缩小可执行文件体积。
 *
 *  算法选型依据（在本仓库的真实语料上实测，整体拼接打包）：
 *  @code
 *    原始 2.57 MB（ParseEngine 模板 1.09 MB + magel 内嵌资产 1.48 MB）
 *    brotli -11        0.156   静态字典约 120 KB，自实现不划算
 *    bzip2  -9         0.156   BWT 解压要 5~6 倍块内存
 *    lzma   -9         0.158   范围编码 + 上下文建模，解码器复杂度高一个量级
 *    zstd   -22        0.161   FSE/tANS 解码器上千行
 *    deflate -9        0.226   LZ77 + 哈夫曼，但窗口上限只有 32 KB
 *  @endcode
 *  另外实测「字典大小 → 压缩率」：32 KB 0.210、256 KB 0.163、1 MB 0.151（1 MB 后饱和）。
 *  也就是说对代码类文本，**窗口开大比换更强的熵编码更划算**。本库因此采用：
 *    - LZ77 匹配窗口取**整个输入**（不设 32 KB 上限）；
 *    - 熵编码用**规范哈夫曼**（解码器小、实现简单），不引入算术/范围编码。
 *
 *  不依赖任何第三方库（不用 zlib / liblzma / OpenSSL），只用 C++17 标准库。
 *
 *  容器格式（小端无关，按字节流）：
 *  @code
 *    magic  4B   "MLZ1"
 *    method 1B   0 = 原样存储，1 = LZ77 + 哈夫曼
 *    usize  varint（LEB128）解压后的字节数
 *    [method == 1] 主字母表码长（273 个符号 × 4 bit）
 *                 距离字母表码长（25 个符号 × 4 bit）
 *    载荷：按位打包（高位在前）的符号流，以结束符收尾
 *  @endcode
 *  压缩时会自动比较：压缩后不比原文小就直接按 method 0 原样存储。
 */

#ifndef LZTEXT_HPP
#define LZTEXT_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace lztext
{

/**
 * @brief 压缩一块字节数据。
 * @param data 输入数据首地址；为空时必须搭配 size == 0。
 * @param size 输入字节数。
 * @param level 压缩级别 1~9（越大越慢、压缩率越好）；越界按 9 处理。
 * @return 压缩后的字节流（含本库的容器头）。压缩后不划算时内部自动退回原样存储。
 */
std::vector<unsigned char> compress(const unsigned char *data, std::size_t size, int level = 9);

/** @brief 压缩字符串（按字节处理，不关心编码）。 */
std::vector<unsigned char> compress(const std::string &text, int level = 9);

/**
 * @brief 解压。
 * @param src 压缩数据首地址。
 * @param size 压缩数据字节数。
 * @param out 输出：解压后的原文；失败时内容未定义（请先 clear 由本函数重填）。
 * @return 成功返回 true；数据损坏、magic 不对、长度不符都返回 false。
 */
bool decompress(const unsigned char *src, std::size_t size, std::string &out);

/**
 * @brief 解压的便捷封装。
 * @param ok 可选，用来接收成功标志；失败时返回空串。
 */
std::string decompressString(const unsigned char *src, std::size_t size, bool *ok = nullptr);

} // namespace lztext

#endif // LZTEXT_HPP
