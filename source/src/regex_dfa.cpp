/**
 * @file regex_dfa.cpp
 * @brief 正则到 DFA 的编译器实现。
 *
 * 分层：正则文本 →（UTF-8 解码）码点序列 → AST → Thompson NFA（转移符号是
 * 字符类编号）→ 子集构造 → 划分法最小化。字符域是 Unicode 码点，字符类划分
 * 用「区间端点」双向装桶，规模只与区间数成正比，不会随码点总数增长。
 */

#include "ParseEngine/regex_dfa.hpp"
#include "ParseEngine/regex_vm.hpp"
#include "../templates/regex_unicode_data.h.tmpl"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <deque>
#include <map>

namespace ParseEngine
{

namespace
{

constexpr char32_t k_maxCodePoint = 0x10FFFFu;

/**
 * @brief 区间表规范化：排序、合并重叠与相邻区间。
 * @param ranges 输入区间表。
 * @return 规范化后的区间表。
 */std::vector<std::pair<char32_t, char32_t>> normalizeRanges(
    std::vector<std::pair<char32_t, char32_t>> ranges)
{
    std::sort(ranges.begin(), ranges.end());
    std::vector<std::pair<char32_t, char32_t>> merged;
    for (const auto &range : ranges)
    {
        if ((!merged.empty()) && (range.first <= merged.back().second + 1u))
        {
            merged.back().second = std::max(merged.back().second, range.second);
            continue;
        }
        merged.push_back(range);
    }
    return merged;
}

/**
 * @brief 区间表求补（相对 0..U+10FFFF，不含 U+0000 以外的处理由调用方决定）。
 * @param ranges 输入区间表。
 * @return 补集区间表。
 */
std::vector<std::pair<char32_t, char32_t>> complementRanges(
    const std::vector<std::pair<char32_t, char32_t>> &ranges)
{
    const auto normalized = normalizeRanges(ranges);
    std::vector<std::pair<char32_t, char32_t>> result;
    char32_t next = 0;
    for (const auto &range : normalized)
    {
        if (range.first > next)
        {
            result.emplace_back(next, range.first - 1u);
        }
        next = range.second + 1u;
    }
    if (next <= k_maxCodePoint)
    {
        result.emplace_back(next, k_maxCodePoint);
    }
    return result;
}

/**
 * @brief 区间表并集：原地并入另一张区间表。
 * @param target 目标区间表。
 * @param add 追加的区间。
 */
void unionRanges(std::vector<std::pair<char32_t, char32_t>> &target,
                 const std::vector<std::pair<char32_t, char32_t>> &add)
{
    target.insert(target.end(), add.begin(), add.end());
    target = normalizeRanges(target);
}

/**
 * @brief 区间表是否包含某个码点。
 * @param ranges 区间表（已规范化）。
 * @param point 码点。
 * @return 包含返回 true。
 */
bool rangesContain(const std::vector<std::pair<char32_t, char32_t>> &ranges,
                   char32_t point)
{
    std::size_t low = 0;
    std::size_t high = ranges.size();
    while (low < high)
    {
        const std::size_t middle = low + (high - low) / 2u;
        if (ranges[middle].second < point)
        {
            low = middle + 1u;
        }
        else if (ranges[middle].first > point)
        {
            high = middle;
        }
        else
        {
            return true;
        }
    }
    return false;
}

/* ---------------- 码点属性表 ---------------- */

/**
 * @brief 解析 `\p{...}` 的属性名，返回码点区间表。
 *
 * 只实现语言词法真正用得上的粗粒度（单 / 双字母）类；`Script=*` 之类的
 * 写法一律报「不支持的属性」。表值是 [起, 止] 的扁平静态数组。
 * @param name 属性名（如 `L`、`Lu`、`Nd`、`Zs`）。
 * @param out 输出区间表（可空）。
 * @return 识别成功返回 true。
 */
bool propertyRanges(const std::string &name,
                    std::vector<std::pair<char32_t, char32_t>> &out)
{
    if ((name == "L") || (name == "Letter"))
    {
        out = {{0x41u, 0x5Au}, {0x61u, 0x7Au}, {0xAAu, 0xAAu}, {0xB5u, 0xB5u},
               {0xBAu, 0xBAu}, {0xC0u, 0xD6u}, {0xD8u, 0xF6u}, {0xF8u, 0x2C1u},
               {0x370u, 0x374u}, {0x376u, 0x377u}, {0x37Au, 0x37Du},
               {0x37Fu, 0x37Fu}, {0x386u, 0x386u}, {0x388u, 0x38Au},
               {0x38Cu, 0x38Cu}, {0x38Eu, 0x3A1u}, {0x3A3u, 0x3F5u},
               {0x3F7u, 0x481u}, {0x48Au, 0x52Fu}, {0x531u, 0x556u},
               {0x559u, 0x559u}, {0x560u, 0x588u}, {0x5D0u, 0x5EAu},
               {0x5F0u, 0x5F2u}, {0x620u, 0x64Au}, {0x66Eu, 0x66Fu},
               {0x671u, 0x6D3u}, {0x6D5u, 0x6D5u}, {0x6E5u, 0x6E6u},
               {0x6EEu, 0x6EFu}, {0x6FAu, 0x6FCu}, {0x6FFu, 0x6FFu},
               {0x710u, 0x710u}, {0x712u, 0x72Fu}, {0x74Du, 0x7A5u},
               {0x7B1u, 0x7B1u}, {0x7CAu, 0x7EAu}, {0x7F4u, 0x7F5u},
               {0x7FAu, 0x7FAu}, {0x800u, 0x815u}, {0x81Au, 0x81Au},
               {0x824u, 0x824u}, {0x828u, 0x828u}, {0x840u, 0x858u},
               {0x904u, 0x939u}, {0x93Du, 0x93Du}, {0x950u, 0x950u},
               {0x958u, 0x961u}, {0x971u, 0x980u}, {0x985u, 0x98Cu},
               {0x98Fu, 0x990u}, {0x993u, 0x9A8u}, {0x9AAu, 0x9B0u},
               {0x9B2u, 0x9B2u}, {0x9B6u, 0x9B9u}, {0x9DFu, 0x9E1u},
               {0xA05u, 0xA0Au}, {0xA0Fu, 0xA10u}, {0xA13u, 0xA28u},
               {0xA2Au, 0xA30u}, {0xA32u, 0xA33u}, {0xA35u, 0xA36u},
               {0xA38u, 0xA39u}, {0xA59u, 0xA5Cu}, {0xA72u, 0xA74u},
               {0xB05u, 0xB0Cu}, {0xB0Fu, 0xB10u}, {0xB13u, 0xB28u},
               {0xB2Au, 0xB30u}, {0xB32u, 0xB33u}, {0xB35u, 0xB39u},
               {0xB85u, 0xB8Au}, {0xB8Eu, 0xB90u}, {0xB92u, 0xB95u},
               {0xB99u, 0xB9Au}, {0xB9Cu, 0xB9Cu}, {0xB9Eu, 0xB9Fu},
               {0xBA3u, 0xBA4u}, {0xBA8u, 0xBAAu}, {0xBAEu, 0xBB9u},
               {0xC05u, 0xC0Cu}, {0xC0Eu, 0xC10u}, {0xC12u, 0xC28u},
               {0xC2Au, 0xC39u}, {0xC58u, 0xC5Au}, {0xC85u, 0xC8Cu},
               {0xC8Eu, 0xC90u}, {0xC92u, 0xCA8u}, {0xCAAu, 0xCB3u},
               {0xCB5u, 0xCB9u}, {0xD05u, 0xD0Cu}, {0xD0Eu, 0xD10u},
               {0xD12u, 0xD3Au}, {0xD3Du, 0xD3Du}, {0xD85u, 0xD96u},
               {0xD9Au, 0xDB1u}, {0xDB3u, 0xDBBu}, {0xE01u, 0xE30u},
               {0xE32u, 0xE33u}, {0xE40u, 0xE46u}, {0xE81u, 0xE82u},
               {0xE84u, 0xE84u}, {0xE87u, 0xE88u}, {0xE8Au, 0xE8Au},
               {0xE94u, 0xE97u}, {0xE99u, 0xE9Fu}, {0xEA1u, 0xEA3u},
               {0xEA5u, 0xEA5u}, {0xEA7u, 0xEA7u}, {0xEAAu, 0xEABu},
               {0xEADu, 0xEB0u}, {0xEB2u, 0xEB3u}, {0xEBDu, 0xEBDu},
               {0xEC0u, 0xEC4u}, {0xEC6u, 0xEC6u}, {0xF00u, 0xF00u},
               {0xF40u, 0xF47u}, {0xF49u, 0xF6Cu}, {0x1000u, 0x102Au},
               {0x103Fu, 0x103Fu}, {0x1050u, 0x1055u}, {0x10A0u, 0x10C5u},
               {0x10D0u, 0x10FAu}, {0x1100u, 0x1159u}, {0x115Fu, 0x11A2u},
               {0x11A8u, 0x11F9u}, {0x1200u, 0x1248u}, {0x124Au, 0x124Du},
               {0x1250u, 0x1256u}, {0x1258u, 0x1258u}, {0x125Au, 0x125Du},
               {0x1260u, 0x1288u}, {0x128Au, 0x128Du}, {0x1290u, 0x12B0u},
               {0x1E00u, 0x1F15u}, {0x1F18u, 0x1F1Du}, {0x1F20u, 0x1F45u},
               {0x1F48u, 0x1F4Du}, {0x1F50u, 0x1F57u}, {0x1F5Fu, 0x1F7Du},
               {0x1F80u, 0x1FB4u}, {0x1FB6u, 0x1FBCu}, {0x1FBEu, 0x1FBEu},
               {0x1FC2u, 0x1FC4u}, {0x1FC6u, 0x1FCCu}, {0x1FD0u, 0x1FD3u},
               {0x1FD6u, 0x1FDBu}, {0x1FE0u, 0x1FECu}, {0x1FF2u, 0x1FF4u},
               {0x1FF6u, 0x1FFCu}, {0x2C00u, 0x2C2Eu}, {0x2C30u, 0x2C5Eu},
               {0x2C60u, 0x2C7Bu}, {0x2C80u, 0x2CE4u}, {0x2D00u, 0x2D25u},
               {0x2D30u, 0x2D65u}, {0x2D80u, 0x2D96u}, {0x2DA0u, 0x2DA6u},
               {0x3005u, 0x3006u}, {0x3007u, 0x3007u}, {0x3021u, 0x3029u},
               {0x3031u, 0x3035u}, {0x3038u, 0x303Cu}, {0x3041u, 0x3096u},
               {0x309Du, 0x309Fu}, {0x30A1u, 0x30FAu}, {0x30FCu, 0x30FFu},
               {0x3105u, 0x312Du}, {0x3131u, 0x318Eu}, {0x31A0u, 0x31BAu},
               {0x3400u, 0x4DB5u}, {0x4E00u, 0x9FCCu}, {0xA000u, 0xA48Cu},
               {0xA4D0u, 0xA4FDu}, {0xA500u, 0xA60Cu}, {0xAC00u, 0xD7A3u},
               {0xF900u, 0xFA6Du}, {0xFB00u, 0xFB06u}, {0xFB13u, 0xFB17u},
               {0xFB1Du, 0xFB28u}, {0xFB2Au, 0xFB36u}, {0xFB38u, 0xFB3Cu},
               {0xFB40u, 0xFB41u}, {0xFB43u, 0xFB44u}, {0xFB46u, 0xFBB1u},
               {0xFBD3u, 0xFD3Du}, {0xFD50u, 0xFD8Fu}, {0xFDF0u, 0xFDFBu},
               {0xFE70u, 0xFE74u}, {0xFE76u, 0xFEFCu}, {0xFF21u, 0xFF3Au},
               {0xFF41u, 0xFF5Au}, {0xFF66u, 0xFFBEu}, {0xFFC2u, 0xFFC7u},
               {0x10000u, 0x1000Bu}, {0x10280u, 0x1029Cu}, {0x10800u, 0x10805u},
               {0x10A00u, 0x10A00u}, {0x12000u, 0x12398u}, {0x13000u, 0x1342Eu},
               {0x16800u, 0x16A38u}, {0x1B000u, 0x1B001u}, {0x1D400u, 0x1D454u},
               {0x1D456u, 0x1D49Cu}, {0x1E900u, 0x1E943u}, {0x20000u, 0x2A6D6u},
               {0x2A700u, 0x2B734u}, {0x2F800u, 0x2FA1Du}};
        return true;
    }
    if ((name == "Lu") || (name == "Uppercase_Letter"))
    {
        out = {{0x41u, 0x5Au}, {0xC0u, 0xD6u}, {0xD8u, 0xDEu}, {0x100u, 0x100u},
               {0x102u, 0x102u}, {0x104u, 0x104u}, {0x106u, 0x106u},
               {0x108u, 0x108u}, {0x10Au, 0x10Au}, {0x10Cu, 0x10Cu},
               {0x10Eu, 0x10Eu}, {0x110u, 0x110u}, {0x112u, 0x112u},
               {0x114u, 0x114u}, {0x116u, 0x116u}, {0x118u, 0x118u},
               {0x11Au, 0x11Au}, {0x11Cu, 0x11Cu}, {0x11Eu, 0x11Eu},
               {0x120u, 0x120u}, {0x122u, 0x122u}, {0x124u, 0x124u},
               {0x126u, 0x126u}, {0x128u, 0x128u}, {0x12Au, 0x12Au},
               {0x12Cu, 0x12Cu}, {0x12Eu, 0x12Eu}, {0x130u, 0x130u},
               {0x132u, 0x132u}, {0x134u, 0x134u}, {0x136u, 0x136u},
               {0x139u, 0x139u}, {0x13Bu, 0x13Bu}, {0x13Du, 0x13Du},
               {0x13Fu, 0x13Fu}, {0x141u, 0x141u}, {0x143u, 0x143u},
               {0x145u, 0x145u}, {0x147u, 0x147u}, {0x14Au, 0x14Au},
               {0x14Cu, 0x14Cu}, {0x14Eu, 0x14Eu}, {0x150u, 0x150u},
               {0x152u, 0x152u}, {0x154u, 0x154u}, {0x156u, 0x156u},
               {0x158u, 0x158u}, {0x15Au, 0x15Au}, {0x15Cu, 0x15Cu},
               {0x15Eu, 0x15Eu}, {0x160u, 0x160u}, {0x162u, 0x162u},
               {0x164u, 0x164u}, {0x166u, 0x166u}, {0x168u, 0x168u},
               {0x16Au, 0x16Au}, {0x16Cu, 0x16Cu}, {0x16Eu, 0x16Eu},
               {0x170u, 0x170u}, {0x172u, 0x172u}, {0x174u, 0x174u},
               {0x176u, 0x176u}, {0x178u, 0x179u}, {0x17Bu, 0x17Bu},
               {0x17Du, 0x17Du}, {0x181u, 0x182u}, {0x184u, 0x184u},
               {0x186u, 0x187u}, {0x189u, 0x18Bu}, {0x18Eu, 0x191u},
               {0x193u, 0x194u}, {0x196u, 0x198u}, {0x19Cu, 0x19Du},
               {0x19Fu, 0x1A0u}, {0x1A2u, 0x1A2u}, {0x1A4u, 0x1A4u},
               {0x1A7u, 0x1A7u}, {0x1ACu, 0x1ACu}, {0x1AFu, 0x1AFu},
               {0x1B1u, 0x1B3u}, {0x1B5u, 0x1B5u}, {0x1B7u, 0x1B8u},
               {0x1BCu, 0x1BCu}, {0x1C4u, 0x1C4u}, {0x1C7u, 0x1C7u},
               {0x1CAu, 0x1CAu}, {0x1CDu, 0x1CDu}, {0x1CFu, 0x1CFu},
               {0x1D1u, 0x1D1u}, {0x1D3u, 0x1D3u}, {0x1D5u, 0x1D5u},
               {0x1D7u, 0x1D7u}, {0x1D9u, 0x1D9u}, {0x1DBu, 0x1DBu},
               {0x1DEu, 0x1DEu}, {0x1E0u, 0x1E0u}, {0x1E2u, 0x1E2u},
               {0x1E4u, 0x1E4u}, {0x1E6u, 0x1E6u}, {0x1E8u, 0x1E8u},
               {0x1EAu, 0x1EAu}, {0x1ECu, 0x1ECu}, {0x1EEu, 0x1EEu},
               {0x1F1u, 0x1F1u}, {0x1F4u, 0x1F4u}, {0x1F6u, 0x1F8u},
               {0x1FAu, 0x1FAu}, {0x1FCu, 0x1FCu}, {0x1FEu, 0x1FEu},
               {0x200u, 0x200u}, {0x202u, 0x202u}, {0x204u, 0x204u},
               {0x206u, 0x206u}, {0x208u, 0x208u}, {0x20Au, 0x20Au},
               {0x20Cu, 0x20Cu}, {0x20Eu, 0x20Eu}, {0x210u, 0x210u},
               {0x212u, 0x212u}, {0x214u, 0x214u}, {0x216u, 0x216u},
               {0x218u, 0x218u}, {0x21Au, 0x21Au}, {0x21Cu, 0x21Cu},
               {0x21Eu, 0x21Eu}, {0x220u, 0x220u}, {0x222u, 0x222u},
               {0x224u, 0x224u}, {0x226u, 0x226u}, {0x228u, 0x228u},
               {0x22Au, 0x22Au}, {0x22Cu, 0x22Cu}, {0x22Eu, 0x22Eu},
               {0x230u, 0x230u}, {0x232u, 0x232u}, {0x386u, 0x386u},
               {0x388u, 0x38Au}, {0x38Cu, 0x38Cu}, {0x38Eu, 0x38Fu},
               {0x391u, 0x3A1u}, {0x3A3u, 0x3ABu}, {0x3D2u, 0x3D4u},
               {0x3DAu, 0x3DAu}, {0x3DCu, 0x3DCu}, {0x3DEu, 0x3DEu},
               {0x3E0u, 0x3E0u}, {0x3E2u, 0x3E2u}, {0x3E4u, 0x3E4u},
               {0x3E6u, 0x3E6u}, {0x3E8u, 0x3E8u}, {0x3EAu, 0x3EAu},
               {0x3ECu, 0x3ECu}, {0x3EEu, 0x3EEu}, {0x3F4u, 0x3F4u},
               {0x3F7u, 0x3F7u}, {0x400u, 0x42Fu}, {0x460u, 0x460u},
               {0x462u, 0x462u}, {0x464u, 0x464u}, {0x466u, 0x466u},
               {0x468u, 0x468u}, {0x46Au, 0x46Au}, {0x46Cu, 0x46Cu},
               {0x46Eu, 0x46Eu}, {0x470u, 0x470u}, {0x472u, 0x472u},
               {0x474u, 0x474u}, {0x476u, 0x476u}, {0x478u, 0x478u},
               {0x47Au, 0x47Au}, {0x47Cu, 0x47Cu}, {0x47Eu, 0x47Eu},
               {0x480u, 0x480u}, {0x48Au, 0x48Au}, {0x48Cu, 0x48Cu},
               {0x48Eu, 0x48Eu}, {0x490u, 0x490u}, {0x492u, 0x492u},
               {0x494u, 0x494u}, {0x496u, 0x496u}, {0x498u, 0x498u},
               {0x49Au, 0x49Au}, {0x49Cu, 0x49Cu}, {0x49Eu, 0x49Eu},
               {0x4A0u, 0x4A0u}, {0x4A2u, 0x4A2u}, {0x4A4u, 0x4A4u},
               {0x4A6u, 0x4A6u}, {0x4A8u, 0x4A8u}, {0x4AAu, 0x4AAu},
               {0x4ACu, 0x4ACu}, {0x4AEu, 0x4AEu}, {0x4B0u, 0x4B0u},
               {0x4B2u, 0x4B2u}, {0x4B4u, 0x4B4u}, {0x4B6u, 0x4B6u},
               {0x4B8u, 0x4B8u}, {0x4BAu, 0x4BAu}, {0x4BCu, 0x4BCu},
               {0x4BEu, 0x4BEu}, {0x4C0u, 0x4C1u}, {0x4C3u, 0x4C3u},
               {0x4C5u, 0x4C5u}, {0x4C7u, 0x4C7u}, {0x4C9u, 0x4C9u},
               {0x4CBu, 0x4CBu}, {0x4CDu, 0x4CDu}, {0x4D0u, 0x4D0u},
               {0x531u, 0x556u}, {0x10A0u, 0x10C5u}, {0x1E00u, 0x1E00u},
               {0x1E02u, 0x1E02u}, {0x1E04u, 0x1E04u}, {0x1E06u, 0x1E06u},
               {0x1E08u, 0x1E08u}, {0x1E0Au, 0x1E0Au}, {0x1E0Cu, 0x1E0Cu},
               {0x1E0Eu, 0x1E0Eu}, {0x1E10u, 0x1E10u}, {0x1E12u, 0x1E12u},
               {0x1E14u, 0x1E14u}, {0x1E16u, 0x1E16u}, {0x1E18u, 0x1E18u},
               {0x1E1Au, 0x1E1Au}, {0x1E1Cu, 0x1E1Cu}, {0x1E1Eu, 0x1E1Eu},
               {0x1E20u, 0x1E20u}, {0x1E22u, 0x1E22u}, {0x1E24u, 0x1E24u},
               {0x1E26u, 0x1E26u}, {0x1E28u, 0x1E28u}, {0x1E2Au, 0x1E2Au},
               {0x1E2Cu, 0x1E2Cu}, {0x1E2Eu, 0x1E2Eu}, {0x1E30u, 0x1E30u},
               {0x1E32u, 0x1E32u}, {0x1E34u, 0x1E34u}, {0x1E36u, 0x1E36u},
               {0x1E38u, 0x1E38u}, {0x1E3Au, 0x1E3Au}, {0x1E3Cu, 0x1E3Cu},
               {0x1E3Eu, 0x1E3Eu}, {0x1E40u, 0x1E40u}, {0x1E42u, 0x1E42u},
               {0x1E44u, 0x1E44u}, {0x1E46u, 0x1E46u}, {0x1E48u, 0x1E48u},
               {0x1E4Au, 0x1E4Au}, {0x1E4Cu, 0x1E4Cu}, {0x1E4Eu, 0x1E4Eu},
               {0x1E50u, 0x1E50u}, {0x1E52u, 0x1E52u}, {0x1E54u, 0x1E54u},
               {0x1E56u, 0x1E56u}, {0x1E58u, 0x1E58u}, {0x1E5Au, 0x1E5Au},
               {0x1E5Cu, 0x1E5Cu}, {0x1E5Eu, 0x1E5Eu}, {0x1E60u, 0x1E60u},
               {0x1E62u, 0x1E62u}, {0x1E64u, 0x1E64u}, {0x1E66u, 0x1E66u},
               {0x1E68u, 0x1E68u}, {0x1E6Au, 0x1E6Au}, {0x1E6Cu, 0x1E6Cu},
               {0x1E6Eu, 0x1E6Eu}, {0x1E70u, 0x1E70u}, {0x1E72u, 0x1E72u},
               {0x1E74u, 0x1E74u}, {0x1E76u, 0x1E76u}, {0x1E78u, 0x1E78u},
               {0x1E7Au, 0x1E7Au}, {0x1E7Cu, 0x1E7Cu}, {0x1E7Eu, 0x1E7Eu},
               {0x1E80u, 0x1E80u}, {0x1E82u, 0x1E82u}, {0x1E84u, 0x1E84u},
               {0x1E86u, 0x1E86u}, {0x1E88u, 0x1E88u}, {0x1E8Au, 0x1E8Au},
               {0x1E8Cu, 0x1E8Cu}, {0x1E8Eu, 0x1E8Eu}, {0x1E90u, 0x1E90u},
               {0x1E92u, 0x1E92u}, {0x1E94u, 0x1E94u}, {0x1E9Eu, 0x1E9Eu},
               {0x1EA0u, 0x1EA0u}, {0x1EA2u, 0x1EA2u}, {0x1EA4u, 0x1EA4u},
               {0x1EA6u, 0x1EA6u}, {0x1EA8u, 0x1EA8u}, {0x1EAAu, 0x1EAAu},
               {0x1EACu, 0x1EACu}, {0x1EAEu, 0x1EAEu}, {0x1EB0u, 0x1EB0u},
               {0x1EB2u, 0x1EB2u}, {0x1EB4u, 0x1EB4u}, {0x1EB6u, 0x1EB6u},
               {0x1EB8u, 0x1EB8u}, {0x1EBAu, 0x1EBAu}, {0x1EBCu, 0x1EBCu},
               {0x1EBEu, 0x1EBEu}, {0x1EC0u, 0x1EC0u}, {0x1EC2u, 0x1EC2u},
               {0x1EC4u, 0x1EC4u}, {0x1EC6u, 0x1EC6u}, {0x1EC8u, 0x1EC8u},
               {0x1ECAu, 0x1ECAu}, {0x1ECCu, 0x1ECCu}, {0x1ECEu, 0x1ECEu},
               {0x1ED0u, 0x1ED0u}, {0x1ED2u, 0x1ED2u}, {0x1ED4u, 0x1ED4u},
               {0x1ED6u, 0x1ED6u}, {0x1ED8u, 0x1ED8u}, {0x1EDAu, 0x1EDAu},
               {0x1EDCu, 0x1EDCu}, {0x1EDEu, 0x1EDEu}, {0x1EE0u, 0x1EE0u},
               {0x1EE2u, 0x1EE2u}, {0x1EE4u, 0x1EE4u}, {0x1EE6u, 0x1EE6u},
               {0x1EE8u, 0x1EE8u}, {0x1EEAu, 0x1EEAu}, {0x1EECu, 0x1EECu},
               {0x1EEEu, 0x1EEEu}, {0x1EF0u, 0x1EF0u}, {0x1EF2u, 0x1EF2u},
               {0x1EF4u, 0x1EF4u}, {0x1EF6u, 0x1EF6u}, {0x1EF8u, 0x1EF8u},
               {0x1F08u, 0x1F0Fu}, {0x1F18u, 0x1F1Du}, {0x1F28u, 0x1F2Fu},
               {0x1F38u, 0x1F3Fu}, {0x1F48u, 0x1F4Du}, {0x1F59u, 0x1F59u},
               {0x1F5Bu, 0x1F5Bu}, {0x1F5Du, 0x1F5Du}, {0x1F5Fu, 0x1F5Fu},
               {0x1F68u, 0x1F6Fu}, {0x1FB8u, 0x1FBBu}, {0x1FC8u, 0x1FCBu},
               {0x1FD8u, 0x1FDBu}, {0x1FE8u, 0x1FECu}, {0x1FF8u, 0x1FFBu},
               {0x2102u, 0x2102u}, {0x2107u, 0x2107u}, {0x210Bu, 0x210Du},
               {0x2110u, 0x2112u}, {0x2115u, 0x2115u}, {0x2119u, 0x211Du},
               {0x2124u, 0x2124u}, {0x2126u, 0x2126u}, {0x2128u, 0x2128u},
               {0x212Au, 0x212Du}, {0x2130u, 0x2133u}, {0x213Eu, 0x213Fu},
               {0x2C00u, 0x2C2Eu}, {0x2C60u, 0x2C60u}, {0x2C62u, 0x2C64u},
               {0x2C67u, 0x2C67u}, {0x2C69u, 0x2C69u}, {0xFF21u, 0xFF3Au},
               {0x10400u, 0x10427u}, {0x1D400u, 0x1D419u}, {0x1E900u, 0x1E921u}};
        return true;
    }
    if ((name == "Ll") || (name == "Lowercase_Letter"))
    {
        out = {{0x61u, 0x7Au}, {0xB5u, 0xB5u}, {0xDFu, 0xF6u}, {0xF8u, 0xFFu},
               {0x101u, 0x101u}, {0x103u, 0x103u}, {0x105u, 0x105u},
               {0x107u, 0x107u}, {0x109u, 0x109u}, {0x10Bu, 0x10Bu},
               {0x10Du, 0x10Du}, {0x10Fu, 0x10Fu}, {0x111u, 0x111u},
               {0x113u, 0x113u}, {0x115u, 0x115u}, {0x117u, 0x117u},
               {0x119u, 0x119u}, {0x11Bu, 0x11Bu}, {0x11Du, 0x11Du},
               {0x11Fu, 0x11Fu}, {0x121u, 0x121u}, {0x123u, 0x123u},
               {0x125u, 0x125u}, {0x127u, 0x127u}, {0x129u, 0x129u},
               {0x12Bu, 0x12Bu}, {0x12Du, 0x12Du}, {0x12Fu, 0x12Fu},
               {0x131u, 0x131u}, {0x133u, 0x133u}, {0x135u, 0x135u},
               {0x137u, 0x138u}, {0x13Au, 0x13Au}, {0x13Cu, 0x13Cu},
               {0x13Eu, 0x13Eu}, {0x140u, 0x140u}, {0x142u, 0x142u},
               {0x144u, 0x144u}, {0x146u, 0x146u}, {0x148u, 0x149u},
               {0x14Bu, 0x14Bu}, {0x14Du, 0x14Du}, {0x14Fu, 0x14Fu},
               {0x151u, 0x151u}, {0x153u, 0x153u}, {0x155u, 0x155u},
               {0x157u, 0x157u}, {0x159u, 0x159u}, {0x15Bu, 0x15Bu},
               {0x15Du, 0x15Du}, {0x15Fu, 0x15Fu}, {0x161u, 0x161u},
               {0x163u, 0x163u}, {0x165u, 0x165u}, {0x167u, 0x167u},
               {0x169u, 0x169u}, {0x16Bu, 0x16Bu}, {0x16Du, 0x16Du},
               {0x16Fu, 0x16Fu}, {0x171u, 0x171u}, {0x173u, 0x173u},
               {0x175u, 0x175u}, {0x177u, 0x177u}, {0x17Au, 0x17Au},
               {0x17Cu, 0x17Cu}, {0x17Eu, 0x180u}, {0x183u, 0x183u},
               {0x185u, 0x185u}, {0x188u, 0x188u}, {0x18Cu, 0x18Du},
               {0x192u, 0x192u}, {0x195u, 0x195u}, {0x199u, 0x19Bu},
               {0x19Eu, 0x19Eu}, {0x1A1u, 0x1A1u}, {0x1A3u, 0x1A3u},
               {0x1A5u, 0x1A5u}, {0x1A8u, 0x1A8u}, {0x1AAu, 0x1ABu},
               {0x1ADu, 0x1ADu}, {0x1B0u, 0x1B0u}, {0x1B4u, 0x1B4u},
               {0x1B6u, 0x1B6u}, {0x1B9u, 0x1BAu}, {0x1BDu, 0x1BFu},
               {0x1C6u, 0x1C6u}, {0x1C9u, 0x1C9u}, {0x1CCu, 0x1CCu},
               {0x1CEu, 0x1CEu}, {0x1D0u, 0x1D0u}, {0x1D2u, 0x1D2u},
               {0x1D4u, 0x1D4u}, {0x1D6u, 0x1D6u}, {0x1D8u, 0x1D8u},
               {0x1DAu, 0x1DAu}, {0x1DCu, 0x1DDu}, {0x1DFu, 0x1DFu},
               {0x1E1u, 0x1E1u}, {0x1E3u, 0x1E3u}, {0x1E5u, 0x1E5u},
               {0x1E7u, 0x1E7u}, {0x1E9u, 0x1E9u}, {0x1EBu, 0x1EBu},
               {0x1EDu, 0x1EDu}, {0x1EFu, 0x1F0u}, {0x1F3u, 0x1F3u},
               {0x1F5u, 0x1F5u}, {0x1F9u, 0x1F9u}, {0x1FBu, 0x1FBu},
               {0x1FDu, 0x1FDu}, {0x1FFu, 0x1FFu}, {0x201u, 0x201u},
               {0x203u, 0x203u}, {0x205u, 0x205u}, {0x207u, 0x207u},
               {0x209u, 0x209u}, {0x20Bu, 0x20Bu}, {0x20Du, 0x20Du},
               {0x20Fu, 0x20Fu}, {0x211u, 0x211u}, {0x213u, 0x213u},
               {0x215u, 0x215u}, {0x217u, 0x217u}, {0x219u, 0x219u},
               {0x21Bu, 0x21Bu}, {0x21Du, 0x21Du}, {0x21Fu, 0x21Fu},
               {0x221u, 0x221u}, {0x223u, 0x223u}, {0x225u, 0x225u},
               {0x227u, 0x227u}, {0x229u, 0x229u}, {0x22Bu, 0x22Bu},
               {0x22Du, 0x22Du}, {0x22Fu, 0x22Fu}, {0x231u, 0x231u},
               {0x233u, 0x239u}, {0x23Cu, 0x23Cu}, {0x256u, 0x257u},
               {0x250u, 0x2AFu}, {0x371u, 0x371u}, {0x373u, 0x373u},
               {0x377u, 0x377u}, {0x37Bu, 0x37Du}, {0x390u, 0x390u},
               {0x3ACu, 0x3CEu}, {0x3D0u, 0x3D1u}, {0x3D5u, 0x3D7u},
               {0x3D9u, 0x3D9u}, {0x3DBu, 0x3DBu}, {0x3DDu, 0x3DDu},
               {0x3DFu, 0x3DFu}, {0x3E1u, 0x3E1u}, {0x3E3u, 0x3E3u},
               {0x3E5u, 0x3E5u}, {0x3E7u, 0x3E7u}, {0x3E9u, 0x3E9u},
               {0x3EBu, 0x3EBu}, {0x3EDu, 0x3EDu}, {0x3EFu, 0x3F3u},
               {0x3F5u, 0x3F5u}, {0x3F8u, 0x3F8u}, {0x430u, 0x45Fu},
               {0x461u, 0x461u}, {0x463u, 0x463u}, {0x465u, 0x465u},
               {0x467u, 0x467u}, {0x469u, 0x469u}, {0x46Bu, 0x46Bu},
               {0x46Du, 0x46Du}, {0x46Fu, 0x46Fu}, {0x471u, 0x471u},
               {0x473u, 0x473u}, {0x475u, 0x475u}, {0x477u, 0x477u},
               {0x479u, 0x479u}, {0x47Bu, 0x47Bu}, {0x47Du, 0x47Du},
               {0x47Fu, 0x47Fu}, {0x481u, 0x481u}, {0x48Bu, 0x48Bu},
               {0x48Du, 0x48Du}, {0x48Fu, 0x48Fu}, {0x491u, 0x491u},
               {0x493u, 0x493u}, {0x495u, 0x495u}, {0x497u, 0x497u},
               {0x499u, 0x499u}, {0x49Bu, 0x49Bu}, {0x49Du, 0x49Du},
               {0x49Fu, 0x49Fu}, {0x4A1u, 0x4A1u}, {0x4A3u, 0x4A3u},
               {0x4A5u, 0x4A5u}, {0x4A7u, 0x4A7u}, {0x4A9u, 0x4A9u},
               {0x4ABu, 0x4ABu}, {0x4ADu, 0x4ADu}, {0x4AFu, 0x4AFu},
               {0x4B1u, 0x4B1u}, {0x4B3u, 0x4B3u}, {0x4B5u, 0x4B5u},
               {0x4B7u, 0x4B7u}, {0x4B9u, 0x4B9u}, {0x4BBu, 0x4BBu},
               {0x4BDu, 0x4BDu}, {0x4BFu, 0x4BFu}, {0x4C2u, 0x4C2u},
               {0x4C4u, 0x4C4u}, {0x4C6u, 0x4C6u}, {0x4C8u, 0x4C8u},
               {0x4CAu, 0x4CAu}, {0x4CCu, 0x4CCu}, {0x4CEu, 0x4CFu},
               {0x4D1u, 0x4D1u}, {0x561u, 0x587u}, {0x1E01u, 0x1E01u},
               {0x1E03u, 0x1E03u}, {0x1E05u, 0x1E05u}, {0x1E07u, 0x1E07u},
               {0x1E09u, 0x1E09u}, {0x1E0Bu, 0x1E0Bu}, {0x1E0Du, 0x1E0Du},
               {0x1E0Fu, 0x1E0Fu}, {0x1E11u, 0x1E11u}, {0x1E13u, 0x1E13u},
               {0x1E15u, 0x1E15u}, {0x1E17u, 0x1E17u}, {0x1E19u, 0x1E19u},
               {0x1E1Bu, 0x1E1Bu}, {0x1E1Du, 0x1E1Du}, {0x1E1Fu, 0x1E1Fu},
               {0x1E21u, 0x1E21u}, {0x1E23u, 0x1E23u}, {0x1E25u, 0x1E25u},
               {0x1E27u, 0x1E27u}, {0x1E29u, 0x1E29u}, {0x1E2Bu, 0x1E2Bu},
               {0x1E2Du, 0x1E2Du}, {0x1E2Fu, 0x1E2Fu}, {0x1E31u, 0x1E31u},
               {0x1E33u, 0x1E33u}, {0x1E35u, 0x1E35u}, {0x1E37u, 0x1E37u},
               {0x1E39u, 0x1E39u}, {0x1E3Bu, 0x1E3Bu}, {0x1E3Du, 0x1E3Du},
               {0x1E3Fu, 0x1E3Fu}, {0x1E41u, 0x1E41u}, {0x1E43u, 0x1E43u},
               {0x1E45u, 0x1E45u}, {0x1E47u, 0x1E47u}, {0x1E49u, 0x1E49u},
               {0x1E4Bu, 0x1E4Bu}, {0x1E4Du, 0x1E4Du}, {0x1E4Fu, 0x1E4Fu},
               {0x1E51u, 0x1E51u}, {0x1E53u, 0x1E53u}, {0x1E55u, 0x1E55u},
               {0x1E57u, 0x1E57u}, {0x1E59u, 0x1E59u}, {0x1E5Bu, 0x1E5Bu},
               {0x1E5Du, 0x1E5Du}, {0x1E5Fu, 0x1E5Fu}, {0x1E61u, 0x1E61u},
               {0x1E63u, 0x1E63u}, {0x1E65u, 0x1E65u}, {0x1E67u, 0x1E67u},
               {0x1E69u, 0x1E69u}, {0x1E6Bu, 0x1E6Bu}, {0x1E6Du, 0x1E6Du},
               {0x1E6Fu, 0x1E6Fu}, {0x1E71u, 0x1E71u}, {0x1E73u, 0x1E73u},
               {0x1E75u, 0x1E75u}, {0x1E77u, 0x1E77u}, {0x1E79u, 0x1E79u},
               {0x1E7Bu, 0x1E7Bu}, {0x1E7Du, 0x1E7Du}, {0x1E7Fu, 0x1E7Fu},
               {0x1E81u, 0x1E81u}, {0x1E83u, 0x1E83u}, {0x1E85u, 0x1E85u},
               {0x1E87u, 0x1E87u}, {0x1E89u, 0x1E89u}, {0x1E8Bu, 0x1E8Bu},
               {0x1E8Du, 0x1E8Du}, {0x1E8Fu, 0x1E8Fu}, {0x1E91u, 0x1E91u},
               {0x1E93u, 0x1E93u}, {0x1E95u, 0x1E9Du}, {0x1E9Fu, 0x1E9Fu},
               {0x1EA1u, 0x1EA1u}, {0x1EA3u, 0x1EA3u}, {0x1EA5u, 0x1EA5u},
               {0x1EA7u, 0x1EA7u}, {0x1EA9u, 0x1EA9u}, {0x1EABu, 0x1EABu},
               {0x1EADu, 0x1EADu}, {0x1EAFu, 0x1EAFu}, {0x1EB1u, 0x1EB1u},
               {0x1EB3u, 0x1EB3u}, {0x1EB5u, 0x1EB5u}, {0x1EB7u, 0x1EB7u},
               {0x1EB9u, 0x1EB9u}, {0x1EBBu, 0x1EBBu}, {0x1EBDu, 0x1EBDu},
               {0x1EBFu, 0x1EBFu}, {0x1EC1u, 0x1EC1u}, {0x1EC3u, 0x1EC3u},
               {0x1EC5u, 0x1EC5u}, {0x1EC7u, 0x1EC7u}, {0x1EC9u, 0x1EC9u},
               {0x1ECBu, 0x1ECBu}, {0x1ECDu, 0x1ECDu}, {0x1ECFu, 0x1ECFu},
               {0x1ED1u, 0x1ED1u}, {0x1ED3u, 0x1ED3u}, {0x1ED5u, 0x1ED5u},
               {0x1ED7u, 0x1ED7u}, {0x1ED9u, 0x1ED9u}, {0x1EDBu, 0x1EDBu},
               {0x1EDDu, 0x1EDDu}, {0x1EDFu, 0x1EDFu}, {0x1EE1u, 0x1EE1u},
               {0x1EE3u, 0x1EE3u}, {0x1EE5u, 0x1EE5u}, {0x1EE7u, 0x1EE7u},
               {0x1EE9u, 0x1EE9u}, {0x1EEBu, 0x1EEBu}, {0x1EEDu, 0x1EEDu},
               {0x1EEFu, 0x1EEFu}, {0x1EF1u, 0x1EF1u}, {0x1EF3u, 0x1EF3u},
               {0x1EF5u, 0x1EF5u}, {0x1EF7u, 0x1EF7u}, {0x1EF9u, 0x1EF9u},
               {0x1F00u, 0x1F07u}, {0x1F10u, 0x1F15u}, {0x1F20u, 0x1F27u},
               {0x1F30u, 0x1F37u}, {0x1F40u, 0x1F45u}, {0x1F50u, 0x1F57u},
               {0x1F60u, 0x1F67u}, {0x1F70u, 0x1F7Du}, {0x1F80u, 0x1F87u},
               {0x1F90u, 0x1F97u}, {0x1FA0u, 0x1FA7u}, {0x1FB0u, 0x1FB4u},
               {0x1FB6u, 0x1FB7u}, {0x1FBEu, 0x1FBEu}, {0x1FC2u, 0x1FC4u},
               {0x1FC6u, 0x1FC7u}, {0x1FD0u, 0x1FD3u}, {0x1FD6u, 0x1FD7u},
               {0x1FE0u, 0x1FE7u}, {0x1FF2u, 0x1FF4u}, {0x1FF6u, 0x1FF7u},
               {0x210Au, 0x210Au}, {0x210Eu, 0x210Fu}, {0x2113u, 0x2113u},
               {0x212Fu, 0x212Fu}, {0x2134u, 0x2134u}, {0x2C30u, 0x2C5Eu},
               {0x2C61u, 0x2C61u}, {0x2C65u, 0x2C66u}, {0x2C68u, 0x2C68u},
               {0x2C6Au, 0x2C6Au}, {0xFF41u, 0xFF5Au}, {0x10428u, 0x1044Fu},
               {0x1D41Au, 0x1D433u}, {0x1E922u, 0x1E943u}};
        return true;
    }
    if ((name == "N") || (name == "Number"))
    {
        out = {{0x30u, 0x39u}, {0xB2u, 0xB3u}, {0xB9u, 0xB9u}, {0xBCu, 0xBEu},
               {0x660u, 0x669u}, {0x6F0u, 0x6F9u}, {0x966u, 0x96Fu},
               {0x9E6u, 0x9EFu}, {0xA66u, 0xA6Fu}, {0xAE6u, 0xAEFu},
               {0xB66u, 0xB6Fu}, {0xBE6u, 0xBEFu}, {0xC66u, 0xC6Fu},
               {0xCE6u, 0xCEFu}, {0xD66u, 0xD6Fu}, {0xE50u, 0xE59u},
               {0xED0u, 0xED9u}, {0xF20u, 0xF33u}, {0x1040u, 0x1049u},
               {0x1090u, 0x1099u}, {0x1369u, 0x137Cu}, {0x16EEu, 0x16F0u},
               {0x17E0u, 0x17E9u}, {0x1810u, 0x1819u}, {0x1946u, 0x194Fu},
               {0x19D0u, 0x19D9u}, {0x2070u, 0x2070u}, {0x2074u, 0x2079u},
               {0x2080u, 0x2089u}, {0x2150u, 0x2182u}, {0x2460u, 0x249Bu},
               {0x24EAu, 0x24FFu}, {0x2776u, 0x2793u}, {0x3007u, 0x3007u},
               {0x3021u, 0x3029u}, {0x3038u, 0x303Au}, {0x3192u, 0x3195u},
               {0x3220u, 0x3229u}, {0x3251u, 0x325Fu}, {0x3280u, 0x3289u},
               {0x4E00u, 0x9FCCu}, {0xA620u, 0xA629u}, {0xF144u, 0xF144u},
               {0xFF10u, 0xFF19u}, {0x1D7CEu, 0x1D7FFu}, {0x1F100u, 0x1F10Cu}};
        return true;
    }
    if ((name == "Nd") || (name == "Decimal_Number") || (name == "Digit"))
    {
        out = {{0x30u, 0x39u}, {0x660u, 0x669u}, {0x6F0u, 0x6F9u},
               {0x966u, 0x96Fu}, {0x9E6u, 0x9EFu}, {0xA66u, 0xA6Fu},
               {0xAE6u, 0xAEFu}, {0xB66u, 0xB6Fu}, {0xBE6u, 0xBEFu},
               {0xC66u, 0xC6Fu}, {0xCE6u, 0xCEFu}, {0xD66u, 0xD6Fu},
               {0xE50u, 0xE59u}, {0xED0u, 0xED9u}, {0xF20u, 0xF29u},
               {0x1040u, 0x1049u}, {0x1090u, 0x1099u}, {0x17E0u, 0x17E9u},
               {0x1810u, 0x1819u}, {0x1946u, 0x194Fu}, {0x19D0u, 0x19D9u},
               {0xA620u, 0xA629u}, {0xFF10u, 0xFF19u}, {0x1D7CEu, 0x1D7FFu}};
        return true;
    }
    if ((name == "Z") || (name == "Separator"))
    {
        out = {{0x20u, 0x20u}, {0xA0u, 0xA0u}, {0x1680u, 0x1680u},
               {0x2000u, 0x200Au}, {0x2028u, 0x2029u}, {0x202Fu, 0x202Fu},
               {0x205Fu, 0x205Fu}, {0x3000u, 0x3000u}};
        return true;
    }
    if ((name == "Zs") || (name == "Space_Separator"))
    {
        out = {{0x20u, 0x20u}, {0xA0u, 0xA0u}, {0x1680u, 0x1680u},
               {0x2000u, 0x200Au}, {0x202Fu, 0x202Fu}, {0x205Fu, 0x205Fu},
               {0x3000u, 0x3000u}};
        return true;
    }
    if ((name == "P") || (name == "Punctuation"))
    {
        out = {{0x21u, 0x23u}, {0x25u, 0x2Au}, {0x2Cu, 0x2Fu}, {0x3Au, 0x3Bu},
               {0x3Fu, 0x40u}, {0x5Bu, 0x5Du}, {0x5Fu, 0x5Fu}, {0x7Bu, 0x7Bu},
               {0x7Du, 0x7Du}, {0xA1u, 0xA1u}, {0xA7u, 0xA7u}, {0xABu, 0xABu},
               {0xB6u, 0xB7u}, {0xBBu, 0xBBu}, {0xBFu, 0xBFu},
               {0x2010u, 0x2027u}, {0x2030u, 0x2043u}, {0x2045u, 0x2051u},
               {0x2053u, 0x205Eu}, {0x3001u, 0x3003u}, {0x3008u, 0x3011u},
               {0x3014u, 0x301Fu}, {0xFF01u, 0xFF03u}, {0xFF05u, 0xFF0Au},
               {0xFF0Cu, 0xFF0Fu}, {0xFF1Au, 0xFF1Bu}, {0xFF1Fu, 0xFF20u},
               {0xFF3Bu, 0xFF3Du}, {0xFF5Fu, 0xFF5Fu}, {0xFF5Bu, 0xFF5Bu},
               {0xFF5Du, 0xFF5Du}, {0xFF5Fu, 0xFF5Fu}};
        return true;
    }
    if ((name == "S") || (name == "Symbol"))
    {
        out = {{0x24u, 0x24u}, {0x2Bu, 0x2Bu}, {0x3Cu, 0x3Eu}, {0x5Eu, 0x5Eu},
               {0x60u, 0x60u}, {0x7Cu, 0x7Cu}, {0x7Eu, 0x7Eu}, {0xA2u, 0xA6u},
               {0xA8u, 0xA9u}, {0xACu, 0xACu}, {0xAEu, 0xB1u}, {0xB4u, 0xB4u},
               {0xB8u, 0xB8u}, {0xD7u, 0xD7u}, {0xF7u, 0xF7u},
               {0x2044u, 0x2044u}, {0x20A0u, 0x20BFu}, {0x2100u, 0x2101u},
               {0x2118u, 0x2118u}, {0x2190u, 0x2195u}, {0x2200u, 0x22FFu},
               {0x25A0u, 0x25FFu}, {0x2600u, 0x26FFu}, {0x1F000u, 0x1F9FFu}};
        return true;
    }
    if ((name == "C") || (name == "Other"))
    {
        out = {{0x0u, 0x1Fu}, {0x7Fu, 0x9Fu}};
        return true;
    }
    return false;
}

/**
 * @brief `[:alpha:]` 这类 POSIX 类名（正则库里的名字，不是 Unicode 属性名）。
 * @param name 类名（如 `alpha`）。
 * @param negate 是否取反（`[:^alpha:]`）。
 * @param out 输出区间表。
 * @return 识别成功返回 true。
 */
bool posixClassRanges(const std::string &name, bool negate,
                      std::vector<std::pair<char32_t, char32_t>> &out)
{
    if (name == "alpha")
    {
        out = {{0x41u, 0x5Au}, {0x61u, 0x7Au}};
    }
    else if (name == "digit")
    {
        out = {{0x30u, 0x39u}};
    }
    else if (name == "alnum")
    {
        out = {{0x30u, 0x39u}, {0x41u, 0x5Au}, {0x61u, 0x7Au}};
    }
    else if (name == "upper")
    {
        out = {{0x41u, 0x5Au}};
    }
    else if (name == "lower")
    {
        out = {{0x61u, 0x7Au}};
    }
    else if (name == "space")
    {
        out = {{0x9u, 0xDu}, {0x20u, 0x20u}};
    }
    else if (name == "blank")
    {
        out = {{0x9u, 0x9u}, {0x20u, 0x20u}};
    }
    else if (name == "xdigit")
    {
        out = {{0x30u, 0x39u}, {0x41u, 0x46u}, {0x61u, 0x66u}};
    }
    else if (name == "punct")
    {
        out = {{0x21u, 0x2Fu}, {0x3Au, 0x40u}, {0x5Bu, 0x60u}, {0x7Bu, 0x7Eu}};
    }
    else if (name == "cntrl")
    {
        out = {{0x0u, 0x1Fu}, {0x7Fu, 0x7Fu}};
    }
    else if (name == "print")
    {
        out = {{0x20u, 0x7Eu}};
    }
    else if (name == "graph")
    {
        out = {{0x21u, 0x7Eu}};
    }
    else if (name == "word")
    {
        out = {{0x30u, 0x39u}, {0x41u, 0x5Au}, {0x5Fu, 0x5Fu}, {0x61u, 0x7Au}};
    }
    else if (name == "ascii")
    {
        out = {{0x0u, 0x7Fu}};
    }
    else if (name == "wordminus")
    {
        out = {{0x30u, 0x39u}, {0x41u, 0x5Au}, {0x5Fu, 0x5Fu}, {0x61u, 0x7Au}};
    }
    else if (name == "unicode")
    {
        out = {{0x0u, k_maxCodePoint}};
    }
    else
    {
        return false;
    }
    if (negate)
    {
        out = complementRanges(out);
    }
    return true;
}

/**
 * @brief `\d` / `\w` / `\s` / `\h` 一类预定义逃逸返回区间表。
 * @param ch 反斜杠后的字符（大小写已归一）。
 * @param negate 是否取反（大写形式）。
 * @param out 输出区间表。
 * @return 是预定义逃逸返回 true。
 */
bool shorthandRanges(char ch, bool negate,
                     std::vector<std::pair<char32_t, char32_t>> &out)
{
    if (ch == 'd')
    {
        posixClassRanges("digit", false, out);
    }
    else if (ch == 'w')
    {
        posixClassRanges("word", false, out);
    }
    else if (ch == 's')
    {
        out = {{0x9u, 0xDu}, {0x20u, 0x20u}};
    }
    else if (ch == 'h')
    {
        out = {{0x30u, 0x39u}, {0x41u, 0x46u}, {0x61u, 0x66u}};
    }
    else
    {
        return false;
    }
    if (negate)
    {
        out = complementRanges(out);
    }
    return true;
}

/**
 * @brief 把 UTF-8 文本解码成码点序列（无效字节映射成 U+DC00+值，便于报错）。
 * @param text UTF-8 文本。
 * @return 码点序列。
 */
std::vector<char32_t> decodeText(const std::string &text)
{
    std::vector<char32_t> points;
    points.reserve(text.size());
    std::size_t index = 0;
    while (index < text.size())
    {
        const unsigned char lead = static_cast<unsigned char>(text[index]);
        if (lead < 0x80u)
        {
            points.push_back(lead);
            ++index;
            continue;
        }
        int extra = 0;
        char32_t value = 0;
        char32_t minimum = 0;
        if ((lead & 0xE0u) == 0xC0u)
        {
            extra = 1;
            value = lead & 0x1Fu;
            minimum = 0x80u;
        }
        else if ((lead & 0xF0u) == 0xE0u)
        {
            extra = 2;
            value = lead & 0x0Fu;
            minimum = 0x800u;
        }
        else if ((lead & 0xF8u) == 0xF0u)
        {
            extra = 3;
            value = lead & 0x07u;
            minimum = 0x10000u;
        }
        else
        {
            points.push_back(0xDC00u + lead);
            ++index;
            continue;
        }
        if (index + static_cast<std::size_t>(extra) >= text.size())
        {
            // 截断的多字节序列：首字节按非法处理，逐个字节往后退
            points.push_back(0xDC00u + lead);
            ++index;
            continue;
        }
        bool valid = true;
        for (int offset = 1; offset <= extra; ++offset)
        {
            const unsigned char follow =
                static_cast<unsigned char>(text[index + static_cast<std::size_t>(offset)]);
            if ((follow & 0xC0u) != 0x80u)
            {
                valid = false;
                break;
            }
            value = (value << 6) | (follow & 0x3Fu);
        }
        if ((!valid) || (value < minimum) || (value > k_maxCodePoint) ||
            ((value >= 0xD800u) && (value <= 0xDFFFu)))
        {
            points.push_back(0xDC00u + lead);
            ++index;
            continue;
        }
        points.push_back(value);
        index += static_cast<std::size_t>(extra + 1);
    }
    return points;
}

/**
 * @brief 码点渲染成错误信息里的一段文字。
 * @param point 码点。
 * @return 可读文本。
 */
std::string renderPoint(char32_t point)
{
    if (point < 0x80u && std::isprint(static_cast<int>(point)) != 0)
    {
        return std::string(1, static_cast<char>(point));
    }
    std::string text = "\\u{";
    const char *const digits = "0123456789ABCDEF";
    std::string hex;
    char32_t value = point;
    do
    {
        hex.insert(hex.begin(), digits[value & 0xFu]);
        value >>= 4;
    } while (value != 0u);
    text += hex;
    text += "}";
    return text;
}

/**
 * @brief 正则递归下降解析器（字符域是码点）。
 */
class RegexParserImpl
{
public:
    /**
     * @brief 绑定待解析的正则文本。
     * @param pattern 正则文本。
     */
    explicit RegexParserImpl(const std::string &pattern)
        : m_points(decodeText(pattern))
    {
    }

    /**
     * @brief 解析整条正则。
     * @return AST 根节点；语法有误返回 nullptr 并置 error。
     */
    std::shared_ptr<RegexNode> parse()
    {
        if (m_points.empty())
        {
            fail("正则表达式不能为空");
            return {};
        }
        while ((peek() == '(') && (peek(1) == '?') &&
               ((peek(2) == 'i') || (peek(2) == 'u')))
        {
            const std::size_t begin = m_pos;
            m_pos += 2u;
            bool ignoreCase = m_ignoreCase, unicode = m_unicode;
            while ((peek() == 'i') || (peek() == 'u'))
            {
                if (next() == 'i') ignoreCase = true;
                else unicode = true;
            }
            if (peek() != ')') { m_pos = begin; break; }
            next();
            m_ignoreCase = ignoreCase;
            m_unicode = unicode;
        }
        std::shared_ptr<RegexNode> node = parseAlt();
        if (m_maxReference > m_captureCount)
        {
            fail("反向引用编号超过捕获组数量");
        }
        if (failed())
        {
            return {};
        }
        if (m_pos != m_points.size())
        {
            fail("位置 " + std::to_string(m_pos) + " 处有多余字符 '" +
                 peekText() + "'");
            return {};
        }
        node->unicode = m_unicode;
        return node;
    }

    /**
     * @brief 是否已经出错。
     * @return 出过错返回 true。
     */
    bool failed() const
    {
        return !m_error.empty();
    }

    /**
     * @brief 取第一个错误。
     * @return 错误值。
     */
    const Error &error() const
    {
        return m_error;
    }

    /**
     * @brief 记录第一个错误（错误槽已满时不覆盖）。
     * @param message 错误说明。
     */
    void fail(const std::string &message)
    {
        ParseEngine::fail(m_error, message);
    }

private:
    /// 未匹配的哨兵码点（越界时返回）。
    static constexpr char32_t k_none = 0xFFFFFFFFu;

    /**
     * @brief 查看当前位置偏移处的码点。
     * @param offset 相对当前位置的偏移。
     * @return 码点；越界返回 k_none。
     */
    char32_t peek(std::size_t offset = 0) const
    {
        const std::size_t index = m_pos + offset;
        if (index < m_points.size())
        {
            return m_points[index];
        }
        return k_none;
    }

    /**
     * @brief 把当前码点渲染成插入错误信息用的文本。
     * @return 单字符文本；越界返回 "结尾"。
     */
    std::string peekText() const
    {
        const char32_t point = peek();
        if (point == k_none)
        {
            return "结尾";
        }
        return renderPoint(point);
    }

    /**
     * @brief 取出当前码点并前移。
     * @return 码点；越界返回 k_none。
     */
    char32_t next()
    {
        const char32_t point = peek();
        if (point != k_none)
        {
            ++m_pos;
        }
        return point;
    }

    /**
     * @brief 是否是 ASCII 数字。
     * @param point 码点。
     * @return 是数字返回 true。
     */
    static bool isDigit(char32_t point)
    {
        return (point >= '0') && (point <= '9');
    }

    /**
     * @brief alt := concat ('|' concat)*
     * @return 解析出的节点。
     */
    std::shared_ptr<RegexNode> parseAlt()
    {
        std::shared_ptr<RegexNode> node = parseConcat();
        while (!failed() && (peek() == '|'))
        {
            next();
            const std::shared_ptr<RegexNode> right = parseConcat();
            if (failed())
            {
                return {};
            }
            node = makeBinary(RegexNode::Kind::Alt, node, right);
        }
        return node;
    }

    /**
     * @brief concat := factor*
     * @return 解析出的节点；空串返回 Empty 节点。
     */
    std::shared_ptr<RegexNode> parseConcat()
    {
        std::vector<std::shared_ptr<RegexNode>> nodes;
        while (!failed() && (peek() != k_none) && (peek() != '|') &&
               (peek() != ')'))
        {
            const std::shared_ptr<RegexNode> factor = parseFactor();
            if (failed())
            {
                return {};
            }
            nodes.push_back(factor);
        }
        if (nodes.empty())
        {
            return makeNode(RegexNode::Kind::Empty);
        }
        std::shared_ptr<RegexNode> node = nodes[0];
        for (std::size_t index = 1; index < nodes.size(); ++index)
        {
            node = makeBinary(RegexNode::Kind::Concat, node, nodes[index]);
        }
        return node;
    }

    /**
     * @brief factor := base ('*' | '+' | '?' | '{m,n}') '?'?*
     * @return 解析出的节点。
     */
    std::shared_ptr<RegexNode> parseFactor()
    {
        std::shared_ptr<RegexNode> node = parseAtom();
        while (!failed())
        {
            const char32_t ch = peek();
            if ((ch == '*') || (ch == '+') || (ch == '?'))
            {
                next();
                const RegexNode::Kind kind = (ch == '*')
                                                 ? RegexNode::Kind::Star
                                                 : ((ch == '+') ? RegexNode::Kind::Plus
                                                                : RegexNode::Kind::Opt);
                node = makeUnary(kind, node);
                if (peek() == '?')
                {
                    // 非贪婪：DFA 本来就取最长接受前缀，这里只记下来（AST 语义完整）
                    next();
                    node->lazy = true;
                }
                continue;
            }
            if (ch == '{')
            {
                const std::shared_ptr<RegexNode> repeated = parseRepeat(node);
                if (failed())
                {
                    return {};
                }
                node = repeated;
                continue;
            }
            break;
        }
        if (failed())
        {
            return {};
        }
        return node;
    }

    /**
     * @brief 读取 `{m}` / `{m,}` / `{m,n}` 里的十进制数字。
     * @param allowEmpty 是否允许空（逗号后面可以什么都不写）。
     * @param ok 是否读到了合法的结构。
     * @return 数字；超过 k_maxRepeat 时返回超限标记。
     */
    std::size_t readRepeatCount(bool allowEmpty, bool &ok)
    {
        std::string digits;
        while (isDigit(peek()))
        {
            digits += static_cast<char>(next());
        }
        ok = allowEmpty || (!digits.empty());
        if (!ok)
        {
            return 0;
        }
        std::size_t value = 0;
        for (const char ch : digits)
        {
            value = value * 10u + static_cast<std::size_t>(ch - '0');
            if (value > static_cast<std::size_t>(k_maxRepeat))
            {
                fail("重复次数不能超过 " + std::to_string(k_maxRepeat));
                return 0;
            }
        }
        return value;
    }

    /**
     * @brief 解析 {m} / {m,} / {m,n}（可带非贪婪后缀）成一个 Repeat 节点。
     * @param node 重复的基节点。
     * @return 展开后的节点。
     */
    std::shared_ptr<RegexNode> parseRepeat(const std::shared_ptr<RegexNode> &node)
    {
        next(); // '{'
        bool ok = true;
        std::size_t minimum = readRepeatCount(false, ok);
        if (!ok)
        {
            fail("重复次数缺少下限数字");
            return {};
        }
        if (failed())
        {
            return {};
        }
        bool unbounded = false;
        bool hasComma = false;
        std::size_t maximum = minimum;
        if (peek() == ',')
        {
            hasComma = true;
            next();
            unbounded = !isDigit(peek());
            maximum = readRepeatCount(true, ok);
            if (failed())
            {
                return {};
            }
        }
        if (peek() != '}')
        {
            fail("重复次数缺少右花括号 }");
            return {};
        }
        next();
        if (!hasComma)
        {
            unbounded = false;
            maximum = minimum;
        }
        if (!unbounded && (maximum < minimum))
        {
            fail("重复次数上限小于下限");
            return {};
        }
        const bool lazy = (peek() == '?');
        if (lazy)
        {
            next();
        }
        std::shared_ptr<RegexNode> result = makeNode(RegexNode::Kind::Repeat);
        result->left = node;
        result->lazy = lazy;
        result->min = static_cast<int>(minimum);
        result->max = unbounded ? -1 : static_cast<int>(maximum);
        return result;
    }

    /**
     * @brief atom := '(' '?:'? alt ')' | '[' class ']' | '\' 转义 | '.' | 断言 | 普通字符
     * @return 解析出的节点。
     */
    std::shared_ptr<RegexNode> parseAtom()
    {
        const char32_t ch = peek();
        if (ch == '(')
        {
            next();
            RegexNode::Kind kind = RegexNode::Kind::Group;
            int group = 0;
            if (peek() == '?')
            {
                next();
                if (peek() == ':') kind = RegexNode::Kind::Empty;
                else if (peek() == '=') kind = RegexNode::Kind::LookAhead;
                else if (peek() == '!') kind = RegexNode::Kind::NegativeLookAhead;
                else
                {
                    fail("不支持的组写法：支持 (...)、(?:...)、(?=...)、(?!...)；(?i) / (?u) 只能放在模式开头");
                    return {};
                }
                next();
            }
            else group = ++m_captureCount;
            auto child = parseAlt();
            if (failed()) return {};
            if (peek() != ')')
            {
                fail("缺少右括号 )");
                return {};
            }
            next();
            if (kind == RegexNode::Kind::Empty) return child;
            auto node = makeUnary(kind, child);
            node->group = group;
            return node;
        }
        if (ch == '[')
        {
            return parseClass();
        }
        if (ch == '\\')
        {
            next();
            return parseEscape(false);
        }
        if (ch == '.')
        {
            next();
            std::vector<std::pair<char32_t, char32_t>> ranges = {
                {0x0u, 0x9u}, {0xBu, 0xCu}, {0xEu, k_maxCodePoint}};
            if (m_unicode) ranges = {{0u, 9u}, {11u, 12u}, {14u, 0x2027u}, {0x202Au, k_maxCodePoint}};
            return makeClass(ranges);
        }
        if (ch == '^')
        {
            next();
            std::shared_ptr<RegexNode> node = makeNode(RegexNode::Kind::Assertion);
            node->assertion = RegexNode::Assertion::LineStart;
            return node;
        }
        if (ch == '$')
        {
            next();
            std::shared_ptr<RegexNode> node = makeNode(RegexNode::Kind::Assertion);
            node->assertion = RegexNode::Assertion::LineEnd;
            return node;
        }
        if ((ch == k_none) || (ch == '|') || (ch == ')') || (ch == '*') ||
            (ch == '+') || (ch == '?') || (ch == '{') || (ch == '}'))
        {
            fail("意外的字符 '" + peekText() + "'");
            return {};
        }
        next();
        return makeClass({{ch, ch}});
    }

    /**
     * @brief 读取 `\` 之后的转义（调用前 `\` 已消费）。
     *
     * 返回普通字符、预定义类、属性类、词边界或数字反向引用节点。
     * 词边界和反向引用由有序运行时执行；类内 `\b` 表示退格。
     * @param inClass 是否在 `[...]` 里（`\b` 在类里是退格）。
     * @return 解析出的节点；出错返回 nullptr。
     */
    std::shared_ptr<RegexNode> parseEscape(bool inClass)
    {
        const char32_t ch = next();
        if (ch == k_none)
        {
            fail("反斜杠后没有字符");
            return {};
        }
        if ((ch == 'd') || (ch == 'D') || (ch == 'w') || (ch == 'W') ||
            (ch == 's') || (ch == 'S') || (ch == 'h') || (ch == 'H'))
        {
            const char lowered = static_cast<char>(
                std::tolower(static_cast<int>(ch)));
            const bool negate = std::isupper(static_cast<int>(ch)) != 0;
            std::vector<std::pair<char32_t, char32_t>> ranges;
            if (m_unicode && ((lowered == 'd') || (lowered == 'w') || (lowered == 's')))
            {
                unicodeRanges(lowered == 'd' ? "Nd" : (lowered == 'w' ? "Word" : "White_Space"), ranges);
                ranges = foldCase(ranges);
                if (negate) ranges = complementRanges(ranges);
            }
            else shorthandRanges(lowered, negate, ranges);
            if (ranges.empty())
            {
                fail(std::string("\\") + static_cast<char>(ch) + " 是空字符集");
                return {};
            }
            auto node = makeNode(RegexNode::Kind::Class);
            node->ranges = ranges;
            node->ignoreCase = m_ignoreCase;
            return node;
        }
        if ((ch == 'p') || (ch == 'P'))
        {
            const bool negate = (ch == 'P');
            if (peek() != '{')
            {
                fail(std::string("\\") + static_cast<char>(ch) +
                     " 后面应跟 {属性名}，例如 \\p{L}、\\p{Lu}、\\p{Nd}");
                return {};
            }
            next();
            std::string name;
            while ((peek() != k_none) && (peek() != '}'))
            {
                const char32_t point = next();
                if (point > 0x7Fu)
                {
                    fail("\\p{...} 的属性名只能是 ASCII");
                    return {};
                }
                name += static_cast<char>(point);
            }
            if (peek() != '}')
            {
                fail("\\p{...} 缺少右花括号 }");
                return {};
            }
            next();
            std::vector<std::pair<char32_t, char32_t>> ranges;
            if (!(m_unicode ? unicodeRanges(name, ranges) : propertyRanges(name, ranges)))
            {
                if (m_unicode)
                {
                    fail("未知的 Unicode 17.0.0 属性：" + name);
                    return {};
                }
                fail("不支持的码点属性 \\p{" + name +
                     "}（可用：L / Lu / Ll / N / Nd / Z / Zs / P / S / C，"
                     "或 POSIX 类名 alpha / digit / alnum / space / word …）");
                return {};
            }
            ranges = foldCase(ranges);
            if (negate) ranges = complementRanges(ranges);
            auto node = makeNode(RegexNode::Kind::Class);
            node->ranges = ranges;
            node->ignoreCase = m_ignoreCase;
            return node;
        }
        if (ch == 'o')
        {
            // \o{101} 八进制码点
            if (peek() != '{')
            {
                fail("\\o 后面应跟 {八进制码点}");
                return {};
            }
            next();
            char32_t value = 0;
            bool any = false;
            while ((peek() >= '0') && (peek() <= '7'))
            {
                value = value * 8u + (next() - '0');
                any = true;
                if (value > k_maxCodePoint)
                {
                    fail("八进制码点超出 Unicode 范围");
                    return {};
                }
            }
            if ((!any) || (peek() != '}'))
            {
                fail("\\o{...} 里的八进制码点不合法");
                return {};
            }
            next();
            return makeClass({{value, value}});
        }
        if (((ch == 'b') && !inClass) || (ch == 'B'))
        {
            if (inClass)
            {
                fail("字符类内不支持 \\B");
                return {};
            }
            return makeNode(ch == 'b' ? RegexNode::Kind::WordBoundary : RegexNode::Kind::NotWordBoundary);
        }
        if ((ch >= '1') && (ch <= '9'))
        {
            if (inClass)
            {
                fail("字符类内不支持反向引用");
                return {};
            }
            int group = static_cast<int>(ch - '0');
            while (isDigit(peek()))
            {
                if (group > k_maxRepeat)
                {
                    fail("反向引用编号过大");
                    return {};
                }
                group = group * 10 + static_cast<int>(next() - '0');
            }
            m_maxReference = std::max(m_maxReference, group);
            auto node = makeNode(RegexNode::Kind::Backref);
            node->group = group;
            node->ignoreCase = m_ignoreCase;
            return node;
        }
        if ((ch == 'k') || (ch == 'g'))
        {
            fail("暂不支持命名反向引用，请使用数字编号 \\1、\\2");
            return {};
        }
        const char32_t value = escapedValue(ch);
        return makeClass({{value, value}});
    }

    /**
     * @brief 把简单转义还原成码点（`\n` / `\xHH` / `\uHHHH` / `\u{H…}`）。
     * @param ch 反斜杠后的码点。
     * @return 码点。
     */
    char32_t escapedValue(char32_t ch)
    {
        switch (ch)
        {
        case 'n': return '\n';
        case 'r': return '\r';
        case 't': return '\t';
        case 'f': return '\f';
        case 'v': return '\v';
        case 'a': return 0x7;
        case 'e': return 0x1B;
        case 'b': return 0x8;
        case '0': return 0x0;
        default: break;
        }
        if (ch == 'x')
        {
            return readHex(2u, "\\xHH");
        }
        if (ch == 'u')
        {
            if (peek() == '{')
            {
                next();
                std::vector<char32_t> digits;
                while (isHexDigit(peek()))
                {
                    digits.push_back(next());
                }
                if (digits.empty() || (peek() != '}'))
                {
                    fail("\\u{...} 里的码点不合法");
                    return 0;
                }
                next();
                char32_t value = 0;
                for (const char32_t digit : digits)
                {
                    value = value * 16u + hexValue(digit);
                    if (value > k_maxCodePoint)
                    {
                        fail("\\u{...} 里的码点超出 Unicode 范围");
                        return 0;
                    }
                }
                return value;
            }
            return readHex(4u, "\\uHHHH");
        }
        if (ch == 'U')
        {
            return readHex(8u, "\\UHHHHHHHH");
        }
        return ch;
    }

    /**
     * @brief 是否是十六进制数字。
     * @param point 码点。
     * @return 是返回 true。
     */
    static bool isHexDigit(char32_t point)
    {
        return ((point >= '0') && (point <= '9')) ||
               ((point >= 'a') && (point <= 'f')) ||
               ((point >= 'A') && (point <= 'F'));
    }

    /**
     * @brief 十六进制字符转数值。
     * @param point 字符。
     * @return 数值（0..15）。
     */
    static char32_t hexValue(char32_t point)
    {
        if ((point >= '0') && (point <= '9'))
        {
            return point - '0';
        }
        if ((point >= 'a') && (point <= 'f'))
        {
            return point - 'a' + 10u;
        }
        return point - 'A' + 10u;
    }

    /**
     * @brief 读固定位数的十六进制转义。
     * @param count 位数。
     * @param what 报错用的写法名。
     * @return 码点；位数不够时返回 0 并报错。
     */
    char32_t readHex(std::size_t count, const std::string &what)
    {
        char32_t value = 0;
        for (std::size_t index = 0; index < count; ++index)
        {
            if (!isHexDigit(peek()))
            {
                fail(what + " 需要 " + std::to_string(count) + " 位十六进制数字");
                return 0;
            }
            value = value * 16u + hexValue(next());
        }
        if (value > k_maxCodePoint)
        {
            fail(what + " 超出 Unicode 范围");
            return 0;
        }
        return value;
    }

    /**
     * @brief class := '[' '^'? ('[' ':' … ':' ']' | item)* ']'
     * @return 解析出的字符类节点。
     */
    std::shared_ptr<RegexNode> parseClass()
    {
        next(); // '['
        bool negate = false;
        if (peek() == '^')
        {
            next();
            negate = true;
        }
        std::vector<std::pair<char32_t, char32_t>> ranges;
        bool first = true;
        while (!failed())
        {
            const char32_t ch = peek();
            if (ch == k_none)
            {
                fail("字符集缺少右括号 ]");
                return {};
            }
            if ((ch == ']') && !first)
            {
                next();
                break;
            }
            // [[:alpha:]] / [[:^alpha:]]
            if ((ch == '[') && (peek(1) == ':'))
            {
                next();
                next();
                bool classNegate = false;
                if (peek() == '^')
                {
                    next();
                    classNegate = true;
                }
                std::string name;
                while ((peek() != k_none) && !((peek() == ':') && (peek(1) == ']')))
                {
                    const char32_t point = next();
                    if (point > 0x7Fu)
                    {
                        fail("[[:...:]] 里的类名只能是 ASCII");
                        return {};
                    }
                    name += static_cast<char>(point);
                }
                if ((peek() == k_none))
                {
                    fail("[[:...:]] 缺少结尾的 :]]");
                    return {};
                }
                next();
                next();
                std::vector<std::pair<char32_t, char32_t>> addition;
                const bool knownClass = m_unicode ? unicodeRanges(name, addition)
                                                 : posixClassRanges(name, classNegate, addition);
                if (m_unicode)
                {
                    addition = foldCase(addition);
                    if (classNegate) addition = complementRanges(addition);
                }
                if (!knownClass)
                {
                    fail("不支持的字符类名 [:" + name + ":]");
                    return {};
                }
                unionRanges(ranges, addition);
                first = false;
                continue;
            }
            first = false;
            char32_t start = 0;
            bool isSingle = false;
            if (ch == '\\')
            {
                next();
                const bool savedCase = m_ignoreCase;
                if (std::string("dDwWsShHpP").find(static_cast<char>(peek())) == std::string::npos)
                    m_ignoreCase = false;
                const std::shared_ptr<RegexNode> node = parseEscape(true);
                m_ignoreCase = savedCase;
                if (failed())
                {
                    return {};
                }
                if ((node->ranges.size() != 1u) ||
                    (node->ranges.front().first != node->ranges.front().second))
                {
                    unionRanges(ranges, node->ranges);
                    continue;
                }
                isSingle = true;
                start = node->ranges.front().first;
            }
            else
            {
                next();
                start = ch;
                isSingle = true;
            }
            if (!isSingle)
            {
                continue;
            }
            // 区间 a-z：'-' 后面必须还有字符，且不是 ']'
            if ((peek() == '-') && (peek(1) != k_none) && (peek(1) != ']'))
            {
                next();
                char32_t end = 0;
                if (peek() == '\\')
                {
                    next();
                    const bool savedCase = m_ignoreCase;
                    m_ignoreCase = false;
                    const std::shared_ptr<RegexNode> node = parseEscape(true);
                    m_ignoreCase = savedCase;
                    if (failed())
                    {
                        return {};
                    }
                    if (node->kind != RegexNode::Kind::Class ||
                        node->ranges.size() != 1u ||
                        node->ranges.front().first != node->ranges.front().second)
                    {
                        fail("字符集区间的右端点不能是字符类 / 预定义类");
                        return {};
                    }
                    end = node->ranges.front().first;
                }
                else
                {
                    end = next();
                }
                if (start > end)
                {
                    fail("字符集区间 " + renderPoint(start) + "-" + renderPoint(end) +
                         " 顺序颠倒");
                    return {};
                }
                ranges.emplace_back(start, end);
                continue;
            }
            ranges.emplace_back(start, start);
        }
        ranges = foldCase(normalizeRanges(ranges));
        if (negate)
        {
            ranges = complementRanges(ranges);
        }
        if (ranges.empty())
        {
            fail("字符集不能为空");
            return {};
        }
        auto node = makeNode(RegexNode::Kind::Class);
        node->ranges = ranges;
        node->ignoreCase = m_ignoreCase;
        return node;
    }

    /**
     * @brief 构造一个带码点区间的 Class 节点。
     * @param ranges 码点区间（已排序、互不重叠）。
     * @return 新建节点。
     */
    std::shared_ptr<RegexNode> makeClass(
        const std::vector<std::pair<char32_t, char32_t>> &ranges)
    {
        std::shared_ptr<RegexNode> node = makeNode(RegexNode::Kind::Class);
        node->ranges = foldCase(ranges);
        node->ignoreCase = m_ignoreCase;
        return node;
    }

    /**
     * @brief 构造一个只有类别的节点。
     * @param kind 节点类别。
     * @return 新建节点。
     */
    static std::shared_ptr<RegexNode> makeNode(RegexNode::Kind kind)
    {
        std::shared_ptr<RegexNode> node = std::make_shared<RegexNode>();
        node->kind = kind;
        return node;
    }

    /**
     * @brief 构造一元节点。
     * @param kind 节点类别。
     * @param child 子节点。
     * @return 新建节点。
     */
    static std::shared_ptr<RegexNode> makeUnary(
        RegexNode::Kind kind, const std::shared_ptr<RegexNode> &child)
    {
        std::shared_ptr<RegexNode> node = makeNode(kind);
        node->left = child;
        return node;
    }

    /**
     * @brief 构造二元节点。
     * @param kind 节点类别。
     * @param left 左子节点。
     * @param right 右子节点。
     * @return 新建节点。
     */
    static std::shared_ptr<RegexNode> makeBinary(
        RegexNode::Kind kind, const std::shared_ptr<RegexNode> &left,
        const std::shared_ptr<RegexNode> &right)
    {
        std::shared_ptr<RegexNode> node = makeNode(kind);
        node->left = left;
        node->right = right;
        return node;
    }

    /** @brief 查询固定版本 Unicode 属性。 @param name 属性名。 @param out 输出区间。 @return 已知属性返回真。 */
    bool unicodeRanges(std::string name, std::vector<std::pair<char32_t, char32_t>> &out) const
    {
        if (name == "alpha") name = "L";
        else if (name == "digit") name = "Nd";
        else if (name == "lower") name = "Ll";
        else if (name == "upper") name = "Lu";
        else if (name == "word") name = "Word";
        else if (name == "space") name = "White_Space";
        else if (name == "ascii") name = "ASCII";
        else if (name == "cntrl") name = "Cc";
        else if (name == "punct") name = "P";
        else if (name == "alnum")
        {
            unicodeRanges("L", out); unicodeRanges("N", out);
            out = normalizeRanges(out); return true;
        }
        else if (name == "blank")
        {
            unicodeRanges("Zs", out); out.emplace_back(9u, 9u);
            out = normalizeRanges(out); return true;
        }
        else if (name == "graph" || name == "print")
        {
            unicodeRanges("C", out); unicodeRanges("Z", out);
            out = complementRanges(out);
            if (name == "print") unicodeRanges("Zs", out);
            out = normalizeRanges(out); return true;
        }
        else if (name == "xdigit") return posixClassRanges("xdigit", false, out);
        if (name.compare(0, 3, "gc=") == 0) name.erase(0, 3);
        if (name.compare(0, 17, "General_Category=") == 0) name.erase(0, 17);
        std::size_t count = 0;
        const auto *ranges = PeUnicodeProperty(name.c_str(), &count);
        if (!ranges) return false;
        for (std::size_t index = 0; index < count; ++index)
            out.emplace_back(ranges[2 * index], ranges[2 * index + 1]);
        return true;
    }
    /** @brief 按 ASCII 大小写关系扩展字符集合。 @param ranges 输入区间。 @return 闭包。 */
    std::vector<std::pair<char32_t, char32_t>> foldCase(
        std::vector<std::pair<char32_t, char32_t>> ranges) const
    {
        if (m_ignoreCase)
        {
            const auto original = normalizeRanges(ranges);
            if (m_unicode)
            {
                std::set<uint32_t> targets;
                for (const auto &pair : PeUnicodeCasePairs)
                    if (rangesContain(original, pair[0]) || rangesContain(original, pair[1])) targets.insert(pair[1]);
                for (const auto &pair : PeUnicodeCasePairs)
                    if (targets.count(pair[1]))
                    {
                        ranges.emplace_back(pair[0], pair[0]);
                        ranges.emplace_back(pair[1], pair[1]);
                    }
                return normalizeRanges(ranges);
            }
            for (char32_t ch = 'A'; ch <= 'Z'; ++ch)
            {
                if (rangesContain(original, ch) || rangesContain(original, ch + 32u))
                {
                    ranges.emplace_back(ch, ch);
                    ranges.emplace_back(ch + 32u, ch + 32u);
                }
            }
        }
        return normalizeRanges(ranges);
    }
    bool m_unicode = false; ///< 显式 Unicode 码点模式
    bool m_ignoreCase = false; ///< 模式开头的 (?i) 标志
    int m_captureCount = 0; ///< 已分配捕获组数
    int m_maxReference = 0; ///< 最大反向引用编号
    std::vector<char32_t> m_points; ///< 正则文本的码点序列
    std::size_t m_pos = 0;          ///< 当前解析位置
    Error m_error;                  ///< 第一个错误
};

/**
 * @brief Thompson 构造的 NFA；转移符号是字符类编号。
 */
class RegexNfa
{
public:
    /**
     * @brief 绑定共享的字符类划分。
     * @param cells 字符类划分。
     */
    explicit RegexNfa(const std::vector<RegexCell> &cells)
        : m_cells(cells)
    {
    }

    /**
     * @brief 从 AST 构造 NFA。
     * @param ast AST 根节点。
     * @return 自身引用。
     */
    RegexNfa &build(const std::shared_ptr<RegexNode> &ast)
    {
        const std::pair<int, int> ends = buildNode(ast);
        m_start = ends.first;
        m_accept = ends.second;
        return *this;
    }

    int start() const { return m_start; }       ///< 起始状态编号
    int accept() const { return m_accept; }     ///< 接受状态编号
    const std::map<int, std::map<int, std::set<int>>> &trans() const
    {
        return m_trans;
    }
    const std::map<int, std::set<int>> &eps() const { return m_eps; }

private:
    /**
     * @brief 新建一个状态。
     * @return 新状态编号。
     */
    int newState()
    {
        const int state = m_stateCount;
        ++m_stateCount;
        m_trans[state] = {};
        m_eps[state] = {};
        return state;
    }

    /**
     * @brief 把码点区间映射成字符类编号集合。
     * @param ranges 码点区间表（已规范化）。
     * @return 命中的字符类编号列表。
     */
    std::vector<int> symbolsOf(
        const std::vector<std::pair<char32_t, char32_t>> &ranges) const
    {
        std::set<int> symbols;
        for (std::size_t index = 0; index < m_cells.size(); ++index)
        {
            if (rangesContain(ranges, m_cells[index].representative))
            {
                symbols.insert(static_cast<int>(index));
            }
        }
        return std::vector<int>(symbols.begin(), symbols.end());
    }

    /**
     * @brief 把一个 Class 节点的区间接到某个起点上。
     * @param start 起点状态。
     * @param end 终点状态。
     * @param ranges 码点区间表。
     */
    void addClassEdges(int start, int end,
                       const std::vector<std::pair<char32_t, char32_t>> &ranges)
    {
        for (const int symbol : symbolsOf(ranges))
        {
            m_trans[start][symbol].insert(end);
        }
    }

    /**
     * @brief 递归构造某个 AST 子节点对应的 NFA 片段。
     * @param node AST 节点。
     * @return (起点, 终点) 二元组。
     */
    std::pair<int, int> buildNode(const std::shared_ptr<RegexNode> &node)
    {
        switch (node->kind)
        {
        case RegexNode::Kind::Class:
        {
            const int start = newState();
            const int end = newState();
            addClassEdges(start, end, node->ranges);
            return {start, end};
        }
        case RegexNode::Kind::Group:
            return buildNode(node->left);
        case RegexNode::Kind::Assertion:
        {
            // buildDfa 在入口拒绝上下文断言，此分支不会用于生成可执行 DFA。
            const int start = newState();
            const int end = newState();
            m_eps[start].insert(end);
            return {start, end};
        }
        case RegexNode::Kind::Empty:
        {
            const int start = newState();
            const int end = newState();
            m_eps[start].insert(end);
            return {start, end};
        }
        case RegexNode::Kind::Concat:
        {
            const auto left = buildNode(node->left);
            const auto right = buildNode(node->right);
            m_eps[left.second].insert(right.first);
            return {left.first, right.second};
        }
        case RegexNode::Kind::Alt:
        {
            const auto left = buildNode(node->left);
            const auto right = buildNode(node->right);
            const int start = newState();
            const int end = newState();
            m_eps[start].insert(left.first);
            m_eps[start].insert(right.first);
            m_eps[left.second].insert(end);
            m_eps[right.second].insert(end);
            return {start, end};
        }
        case RegexNode::Kind::Star:
        {
            const auto sub = buildNode(node->left);
            const int start = newState();
            const int end = newState();
            m_eps[start].insert(sub.first);
            m_eps[start].insert(end);
            m_eps[sub.second].insert(sub.first);
            m_eps[sub.second].insert(end);
            return {start, end};
        }
        case RegexNode::Kind::Plus:
        {
            const auto sub = buildNode(node->left);
            const int start = newState();
            const int end = newState();
            m_eps[start].insert(sub.first);
            m_eps[sub.second].insert(sub.first);
            m_eps[sub.second].insert(end);
            return {start, end};
        }
        case RegexNode::Kind::Opt:
        {
            const auto sub = buildNode(node->left);
            const int start = newState();
            const int end = newState();
            m_eps[start].insert(sub.first);
            m_eps[start].insert(end);
            m_eps[sub.second].insert(end);
            return {start, end};
        }
        case RegexNode::Kind::Repeat:
        default:
        {
            // {m,n}：串 m 份子片段，再接 n-m 份「可选」（子片段只构造一次就共享
            // 状态会破坏 Thompson 的「一个片段一个入口」约定，所以逐份重建）。
            const int start = newState();
            int tail = start;
            for (int index = 0; index < node->min; ++index)
            {
                const auto piece = buildNode(node->left);
                m_eps[tail].insert(piece.first);
                tail = piece.second;
            }
            const int end = newState();
            if (node->max < 0)
            {
                const auto piece = buildNode(node->left);
                m_eps[tail].insert(piece.first);
                m_eps[piece.second].insert(piece.first);
                m_eps[tail].insert(end);
                m_eps[piece.second].insert(end);
                return {start, end};
            }
            for (int index = node->min; index < node->max; ++index)
            {
                const auto piece = buildNode(node->left);
                m_eps[tail].insert(piece.first);
                m_eps[tail].insert(end);
                tail = piece.second;
            }
            m_eps[tail].insert(end);
            return {start, end};
        }
        }
    }

    std::vector<RegexCell> m_cells;                    ///< 字符类划分
    std::map<int, std::map<int, std::set<int>>> m_trans; ///< 非 ε 转移
    std::map<int, std::set<int>> m_eps;                ///< ε 转移
    int m_start = 0;                                   ///< 起始状态
    int m_accept = 0;                                  ///< 接受状态
    int m_stateCount = 0;                              ///< 状态总数
};

/**
 * @brief ε 闭包。
 * @param nfa 目标 NFA。
 * @param states 起始状态集合。
 * @return 闭包集合。
 */
std::set<int> epsilonClosure(const RegexNfa &nfa, const std::set<int> &states)
{
    std::vector<int> stack(states.begin(), states.end());
    std::set<int> closure = states;
    while (!stack.empty())
    {
        const int state = stack.back();
        stack.pop_back();
        const auto found = nfa.eps().find(state);
        if (found == nfa.eps().end())
        {
            continue;
        }
        for (const int target : found->second)
        {
            if (!closure.count(target))
            {
                closure.insert(target);
                stack.push_back(target);
            }
        }
    }
    return closure;
}

/**
 * @brief 子集构造：NFA -> DFA（补齐陷阱状态，转移全覆盖）。
 * @param nfa 目标 NFA。
 * @param alphabet 字符类编号列表。
 * @param outTransitions 输出转移表。
 * @param outStart 输出起始状态。
 * @param outAccept 输出接受状态集合。
 * @param outTrap 输出陷阱状态。
 * @return 状态总数（含陷阱状态）。
 */
int subsetConstruction(const RegexNfa &nfa,
                       const std::vector<int> &alphabet,
                       std::map<int, std::map<int, int>> &outTransitions,
                       int &outStart, std::set<int> &outAccept, int &outTrap)
{
    const std::set<int> startClosure = epsilonClosure(nfa, {nfa.start()});
    std::map<std::set<int>, int> stateMap;
    stateMap[startClosure] = 0;
    std::map<int, std::map<int, int>> transitions;
    std::deque<std::set<int>> queue;
    queue.push_back(startClosure);
    int nextId = 1;
    while (!queue.empty())
    {
        const std::set<int> current = queue.front();
        queue.pop_front();
        const int currentId = stateMap[current];
        if (current.count(nfa.accept()))
        {
            outAccept.insert(currentId);
        }
        std::map<int, int> row;
        for (const int symbol : alphabet)
        {
            std::set<int> following;
            for (const int state : current)
            {
                const auto found = nfa.trans().find(state);
                if (found == nfa.trans().end())
                {
                    continue;
                }
                const auto symbolFound = found->second.find(symbol);
                if (symbolFound != found->second.end())
                {
                    following.insert(symbolFound->second.begin(),
                                     symbolFound->second.end());
                }
            }
            if (following.empty())
            {
                continue;
            }
            const std::set<int> closure = epsilonClosure(nfa, following);
            if (!stateMap.count(closure))
            {
                stateMap[closure] = nextId;
                ++nextId;
                queue.push_back(closure);
            }
            row[symbol] = stateMap[closure];
        }
        transitions[currentId] = row;
    }
    const int trap = nextId;
    for (const int symbol : alphabet)
    {
        transitions[trap][symbol] = trap;
    }
    for (int state = 0; state < nextId; ++state)
    {
        for (const int symbol : alphabet)
        {
            if (!transitions[state].count(symbol))
            {
                transitions[state][symbol] = trap;
            }
        }
    }
    outTransitions = transitions;
    outStart = 0;
    outTrap = trap;
    return nextId;
}

/**
 * @brief 划分法最小化（保留陷阱状态）。
 * @param transitions DFA 转移表。
 * @param start 起始状态。
 * @param accept 接受状态集合。
 * @param trap 陷阱状态。
 * @param alphabet 字符类编号列表。
 * @param outTransitions 输出新转移表。
 * @param outStart 输出新起始状态。
 * @param outAccept 输出新接受状态集合。
 * @param outTrap 输出新陷阱状态。
 * @param outStateCount 输出新状态总数。
 */
void minimize(const std::map<int, std::map<int, int>> &transitions, int start,
              const std::set<int> &accept, int trap,
              const std::vector<int> &alphabet,
              std::map<int, std::map<int, int>> &outTransitions, int &outStart,
              std::set<int> &outAccept, int &outTrap, int &outStateCount)
{
    // 可达性分析：从起始状态出发按字母表遍历
    std::set<int> reachable;
    std::vector<int> stack{start};
    while (!stack.empty())
    {
        const int state = stack.back();
        stack.pop_back();
        if (!reachable.insert(state).second)
        {
            continue;
        }
        for (const int symbol : alphabet)
        {
            stack.push_back(transitions.at(state).at(symbol));
        }
    }
    // 初始划分：接受 / 非接受
    std::vector<std::vector<int>> partitions;
    std::vector<int> accepting;
    std::vector<int> others;
    for (const int state : reachable)
    {
        if (accept.count(state))
        {
            accepting.push_back(state);
        }
        else
        {
            others.push_back(state);
        }
    }
    if (!others.empty())
    {
        partitions.push_back(others);
    }
    if (!accepting.empty())
    {
        partitions.push_back(accepting);
    }
    std::map<int, int> stateToGroup;
    for (std::size_t index = 0; index < partitions.size(); ++index)
    {
        for (const int state : partitions[index])
        {
            stateToGroup[state] = static_cast<int>(index);
        }
    }
    // Hopcroft 风格的反复细化：按「各符号的目标组」签名拆组
    while (true)
    {
        std::vector<std::vector<int>> refined;
        for (const std::vector<int> &group : partitions)
        {
            std::vector<std::pair<std::vector<int>, std::vector<int>>> buckets;
            for (const int state : group)
            {
                std::vector<int> signature;
                signature.reserve(alphabet.size());
                for (const int symbol : alphabet)
                {
                    signature.push_back(
                        stateToGroup[transitions.at(state).at(symbol)]);
                }
                auto found = std::find_if(
                    buckets.begin(), buckets.end(),
                    [&signature](const auto &bucket)
                    { return bucket.first == signature; });
                if (found == buckets.end())
                {
                    buckets.emplace_back(signature, std::vector<int>{state});
                }
                else
                {
                    found->second.push_back(state);
                }
            }
            for (auto &bucket : buckets)
            {
                refined.push_back(std::move(bucket.second));
            }
        }
        if (refined.size() == partitions.size())
        {
            break;
        }
        partitions = std::move(refined);
        stateToGroup.clear();
        for (std::size_t index = 0; index < partitions.size(); ++index)
        {
            for (const int state : partitions[index])
            {
                stateToGroup[state] = static_cast<int>(index);
            }
        }
    }
    std::map<int, std::map<int, int>> newTransitions;
    for (std::size_t index = 0; index < partitions.size(); ++index)
    {
        const int representative = partitions[index].front();
        for (const int symbol : alphabet)
        {
            newTransitions[static_cast<int>(index)][symbol] =
                stateToGroup[transitions.at(representative).at(symbol)];
        }
    }
    outAccept.clear();
    for (const int state : accept)
    {
        const auto found = stateToGroup.find(state);
        if (found != stateToGroup.end())
        {
            outAccept.insert(found->second);
        }
    }
    outTransitions = newTransitions;
    outStart = stateToGroup[start];
    outTrap = stateToGroup[trap];
    outStateCount = static_cast<int>(partitions.size());
}

} // namespace

std::vector<char32_t> RegexDfa::decodeUtf8(const std::string &text)
{
    return decodeText(text);
}

bool RegexDfa::parse(const std::string &pattern,
                     std::shared_ptr<RegexNode> &out, Error &error)
{
    RegexParserImpl parser(pattern);
    const std::shared_ptr<RegexNode> node = parser.parse();
    if (parser.failed())
    {
        return fail(error, parser.error());
    }
    out = node;
    return true;
}

void RegexDfa::collectClassSets(const std::shared_ptr<RegexNode> &node,
                                std::vector<RangeList> &found)
{
    switch (node->kind)
    {
    case RegexNode::Kind::Class:
        found.push_back(node->ranges);
        break;
    case RegexNode::Kind::Concat:
    case RegexNode::Kind::Alt:
        collectClassSets(node->left, found);
        collectClassSets(node->right, found);
        break;
    case RegexNode::Kind::Group:
    case RegexNode::Kind::LookAhead:
    case RegexNode::Kind::NegativeLookAhead:
    case RegexNode::Kind::Star:
    case RegexNode::Kind::Plus:
    case RegexNode::Kind::Opt:
    case RegexNode::Kind::Repeat:
        collectClassSets(node->left, found);
        break;
    case RegexNode::Kind::Assertion:
    case RegexNode::Kind::Empty:
    default:
        break;
    }
}

std::pair<std::vector<RegexCell>, std::vector<int>> RegexDfa::charClasses(
    const std::vector<RangeList> &classSets)
{
    // 端点扫描：每个码点只要知道「是不是某个集合的区间起点 / 终点」，就能把
    // 0..U+10FFFF 切成一列等价的段。段的等价性由「落在哪些集合里」决定。
    std::set<char32_t> starts{0u};
    std::vector<RangeList> rangeLists;
    rangeLists.reserve(classSets.size());
    for (const RangeList &list : classSets)
    {
        const auto ranges = normalizeRanges(list);
        rangeLists.push_back(ranges);
        for (const auto &range : ranges)
        {
            starts.insert(range.first);
            if (range.second < k_maxCodePoint)
            {
                starts.insert(range.second + 1u);
            }
        }
    }
    const std::vector<char32_t> bounds(starts.begin(), starts.end());
    struct Segment
    {
        std::vector<bool> signature;
        char32_t first = 0;
        char32_t last = 0;
    };
    std::vector<Segment> segments;
    for (std::size_t index = 0; index < bounds.size(); ++index)
    {
        const char32_t first = bounds[index];
        const char32_t last = (index + 1u < bounds.size())
                                  ? bounds[index + 1u] - 1u
                                  : k_maxCodePoint;
        Segment segment;
        segment.first = first;
        segment.last = last;
        segment.signature.reserve(rangeLists.size());
        for (const auto &ranges : rangeLists)
        {
            segment.signature.push_back(rangesContain(ranges, first));
        }
        if ((!segments.empty()) && (segments.back().signature == segment.signature))
        {
            segments.back().last = last;
            continue;
        }
        segments.push_back(std::move(segment));
    }
    // 相邻且签名相同的段已在上面合并，所以每一段就是一个字符类
    std::vector<RegexCell> cells;
    cells.reserve(segments.size());
    for (const Segment &segment : segments)
    {
        RegexCell cell;
        cell.representative = segment.first;
        cell.ranges.emplace_back(segment.first, segment.last);
        cells.push_back(std::move(cell));
    }
    // 字节映射：给定首字节（以及续字节个数的上下文），能命中的最低类编号。
    // 只用于运行时早退与「整段按字节判它不可能属于某类」的快速排除；真正判等
    // 仍然看整码点落在哪个类里，所以这里给保守值（同一首字节的类取最小编号）。
    std::vector<int> byteMap(256, -1);
    for (std::size_t index = 0; index < cells.size(); ++index)
    {
        for (const auto &range : cells[index].ranges)
        {
            for (int code = 0; code < 256; ++code)
            {
                if (code < 0x80)
                {
                    if ((range.first > static_cast<char32_t>(code)) ||
                        (range.second < static_cast<char32_t>(code)))
                    {
                        continue;
                    }
                    if (byteMap[static_cast<std::size_t>(code)] < 0)
                    {
                        byteMap[static_cast<std::size_t>(code)] =
                            static_cast<int>(index);
                    }
                    continue;
                }
                if (code < 0xC2)
                {
                    continue; // 非法的首字节
                }
                char32_t low = 0;
                char32_t high = 0;
                if (code < 0xE0)
                {
                    low = (static_cast<char32_t>(code) & 0x1Fu) << 6;
                    high = low + 0x3F;
                }
                else if (code < 0xF0)
                {
                    low = (static_cast<char32_t>(code) & 0x0Fu) << 12;
                    high = low + 0xFFF;
                }
                else
                {
                    low = (static_cast<char32_t>(code) & 0x07u) << 18;
                    high = low + 0x3FFFF;
                }
                if ((high < range.first) || (low > range.second))
                {
                    continue;
                }
                if (byteMap[static_cast<std::size_t>(code)] < 0)
                {
                    byteMap[static_cast<std::size_t>(code)] =
                        static_cast<int>(index);
                }
            }
        }
    }
    for (int code = 0; code < 256; ++code)
    {
        if (byteMap[static_cast<std::size_t>(code)] < 0)
        {
            byteMap[static_cast<std::size_t>(code)] = 0;
        }
    }
    return {cells, byteMap};
}

bool RegexDfa::buildDfa(const std::string &pattern,
                        const std::vector<RegexCell> &cells, DfaProgram &out,
                        Error &error)
{
    std::shared_ptr<RegexNode> ast;
    if (!parse(pattern, ast, error))
    {
        return false;
    }
    if (RegexVm::contextRequired(ast))
    {
        return fail(error, "该正则包含上下文断言或反向引用，需要有序正则运行时");
    }
    RegexNfa nfa(cells);
    nfa.build(ast);
    std::vector<int> alphabet;
    for (std::size_t index = 0; index < cells.size(); ++index)
    {
        alphabet.push_back(static_cast<int>(index));
    }
    std::map<int, std::map<int, int>> transitions;
    int start = 0;
    int trap = 0;
    std::set<int> accept;
    subsetConstruction(nfa, alphabet, transitions, start, accept, trap);
    std::map<int, std::map<int, int>> minimizedTransitions;
    std::set<int> minimizedAccept;
    int minimizedStart = 0;
    int minimizedTrap = 0;
    int minimizedCount = 0;
    minimize(transitions, start, accept, trap, alphabet, minimizedTransitions,
             minimizedStart, minimizedAccept, minimizedTrap, minimizedCount);
    out.pattern = pattern;
    out.stateCount = minimizedCount;
    out.start = minimizedStart;
    out.trap = minimizedTrap;
    out.accept = minimizedAccept;
    out.transitions = minimizedTransitions;
    return true;
}

bool RegexDfa::compileAll(
    const std::vector<std::pair<std::string, std::string>> &regexDefs,
    RegexCompileResult &out, Error &error)
{
    std::vector<std::shared_ptr<RegexNode>> asts;
    std::vector<RangeList> classSets;
    std::vector<std::pair<std::string, std::string>> defs;
    for (const auto &entry : regexDefs)
    {
        std::shared_ptr<RegexNode> ast;
        if (!parse(entry.second, ast, error))
        {
            return false;
        }
        asts.push_back(ast);
        defs.push_back(entry);
        collectClassSets(ast, classSets);
    }
    const auto classes = charClasses(classSets);
    out.cells = classes.first;
    out.byteMap = classes.second;
    for (std::size_t index = 0; index < defs.size(); ++index)
    {
        CompiledRegex compiled;
        compiled.name = defs[index].first;
        compiled.pattern = defs[index].second;
        if (!buildDfa(defs[index].second, out.cells, compiled.dfa, error))
        {
            return false;
        }
        out.compiled.push_back(compiled);
    }
    return true;
}

} // namespace ParseEngine
