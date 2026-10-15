/** @file test_lztext.cpp
 *  @brief lztext 自检：往返一致性 + 边界用例 + 随机语料模糊测试。
 *
 *  用法：./test_lztext [可选：额外喂一个真实文件做往返验证]
 *  退出码 0 表示全部通过。
 */

#include "lztext.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace
{

int g_failed = 0;
int g_passed = 0;

void check(bool ok, const std::string &what)
{
    if (ok)
    {
        ++g_passed;
        return;
    }
    ++g_failed;
    std::cout << "  失败：" << what << "\n";
}

/** @brief 压缩→解压，比对原文，并打印压缩率。 */
void roundTrip(const std::string &name, const std::string &text)
{
    std::vector<unsigned char> packed = lztext::compress(text, 9);
    std::string back;
    const bool ok = lztext::decompress(packed.data(), packed.size(), back);
    check(ok, name + "：解压返回失败");
    check(back == text, name + "：往返后内容不一致");

    const double ratio = text.empty() ? 0.0 : (double)packed.size() / (double)text.size();
    std::printf("  %-28s %8zu -> %8zu  比率 %.3f\n", name.c_str(), text.size(), packed.size(), ratio);
}

/** @brief 简易线性同余随机源，保证多次运行结果可复现。 */
struct Rng
{
    unsigned state;
    explicit Rng(unsigned seed) : state(seed) {}
    unsigned next()
    {
        state = state * 1664525u + 1013904223u;
        return state >> 8;
    }
};

} // namespace

int main(int argc, char **argv)
{
    std::cout << "lztext 自检\n";

    roundTrip("空输入", std::string());
    roundTrip("单字节", std::string("a"));
    roundTrip("两字节", std::string("ab"));
    roundTrip("全同字节 x1000", std::string(1000, 'x'));
    roundTrip("递增字节 0..255", [] {
        std::string s;
        for (int i = 0; i < 256; ++i) s.push_back((char)(unsigned char)i);
        return s;
    }());
    roundTrip("重复短串", [] {
        std::string s;
        for (int i = 0; i < 500; ++i) s += "abcabcabd";
        return s;
    }());
    roundTrip("含 NUL 与高位字节", std::string("a\0\xff"
                                              "b\0",
                                              5));
    roundTrip("长距离重复", [] {
        std::string s(300000, 'z');
        s += "中间标记";
        s += std::string(300000, 'z');
        return s;
    }());
    roundTrip("伪代码文本", [] {
        std::string s;
        for (int i = 0; i < 2000; ++i)
        {
            s += "    int value" + std::to_string(i % 97) + " = compute(" + std::to_string(i) + ");\n";
            s += "    // 第 " + std::to_string(i) + " 行注释，用来制造长距离重复\n";
        }
        return s;
    }());

    /* 随机语料模糊测试：不同规模 + 不同重复程度 */
    std::cout << "\n  随机语料模糊测试：\n";
    for (int seed = 1; seed <= 12; ++seed)
    {
        Rng rng((unsigned)seed * 7919u);
        const std::size_t n = 1000u * (std::size_t)(1 + seed * 13);
        const int alphabet = 2 + (seed % 250);   // 越小越重复
        std::string text;
        text.reserve(n);
        for (std::size_t i = 0; i < n; ++i) text.push_back((char)(unsigned char)(rng.next() % (unsigned)alphabet));
        std::vector<unsigned char> packed = lztext::compress(text, 9);
        std::string back;
        const bool ok = lztext::decompress(packed.data(), packed.size(), back);
        check(ok && back == text, "随机语料 seed=" + std::to_string(seed) + "（" + std::to_string(n) + " 字节）");
    }
    std::cout << "  模糊测试完成（12 组，最大 160 KB）\n";

    /* 压缩级别一致性：每个级别都要能正确往返 */
    std::cout << "\n  压缩级别 1..9 往返一致性：\n";
    {
        Rng rng(20261011u);
        std::string text;
        for (int i = 0; i < 60000; ++i) text.push_back((char)(unsigned char)(rng.next() % 64u));
        for (int level = 1; level <= 9; ++level)
        {
            std::vector<unsigned char> packed = lztext::compress(text, level);
            std::string back;
            const bool ok = lztext::decompress(packed.data(), packed.size(), back);
            check(ok && back == text, "level=" + std::to_string(level));
            std::printf("    level %d  %8zu 字节\n", level, packed.size());
        }
    }

    /* 损坏数据要被拒，不能崩 */
    std::cout << "\n  损坏数据拒绝：\n";
    {
        const std::string text = std::string(5000, 'q') + "尾巴";
        std::vector<unsigned char> packed = lztext::compress(text, 9);
        std::string back;
        check(!lztext::decompress(packed.data(), 3, back), "截断到 3 字节应失败");
        std::vector<unsigned char> broken = packed;
        broken[0] = 'X';
        check(!lztext::decompress(broken.data(), broken.size(), back), "magic 改错应失败");
        broken = packed;
        broken[4] = 99;
        check(!lztext::decompress(broken.data(), broken.size(), back), "method 非法应失败");
    }

    /* 可选：拿一个真实文件做往返 */
    if (argc > 1)
    {
        std::cout << "\n  真实文件：\n";
        std::ifstream in(argv[1], std::ios::binary);
        if (in)
        {
            std::ostringstream ss;
            ss << in.rdbuf();
            roundTrip(argv[1], ss.str());
        }
        else
        {
            std::cout << "  打不开：" << argv[1] << "\n";
        }
    }

    std::cout << "\n通过 " << g_passed << " 项，失败 " << g_failed << " 项\n";
    return g_failed == 0 ? 0 : 1;
}
